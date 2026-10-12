// classic: command-line entry point.
//
//   classic run <app> [--census] [--max N] [--log traps,unimpl,seg,rsrc,mem,cpu]
//                     [--gestalt sel=value]...
//   classic import <macbinary-file> <dest-dir>
//   classic rsrc <file>

#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif
import CCore
import Foundation
import ForkStore
import ClassicHost
import Toolbox

/// Write to standard error (fd 2; glibc's `stderr` global is not concurrency-safe).
func eprint(_ s: String) {
    var bytes = Array(s.utf8)
    _ = bytes.withUnsafeMutableBytes { write(2, $0.baseAddress, $0.count) }
}

func fail(_ message: String) -> Never {
    eprint("classic: \(message)\n")
    exit(1)
}

func usage() -> Never {
    eprint("""
    usage:
      classic run <app-or-archive> [document...] [--app name] [--census] [--max N] [--log list] [--gestalt sel=value]...
          Run a classic application.  The argument may be an AppleDouble application
          (data file + ._ sidecar) or a .bin/.sit/.sea/.cpt/.hqx file, which is first
          added to the library; --app picks one when the archive holds several.
          --headless run without a window (the default on Linux for now)
          --screen-window  show the whole classic screen in one window instead of
                     rootless windows (implied by --screen)
          --census   keep going past unimplemented traps and report every trap used
          --max N    stop after N instructions (default 50000000)
          --log      comma list of: traps, unimpl, seg, rsrc, mem, cpu
          --gestalt  override a Gestalt selector, e.g. sysv=0x0755
          --fonts F  also open resource file F (AppleDouble) for fonts; repeatable
          --screen WxH  screen size (default 1024x768)
      --depth N  screen depth: 1, 8, 16 or 32 bits (default 32, or the app's setting; an
                     application can still switch depth itself with SetDepth)
          --cpu M    processor: 68040 (default, with FPU) or 68020 (no FPU)
          --screenshot P  write the screen to PNG file P when the run ends
          --print-to F  print to PDF file F instead of the print panel (headless runs)
          --appearance A  bw (default); native (macOS window frames); or a theme: a
                     Kaleidoscope scheme file, an archive of schemes (.sit etc.,
                     "archive.sit:Name" picks one), or the name of an imported
                     scheme (classic themes lists them); themes use a 32-bit screen
          --keys S   scripted input, comma separated: wait:N (instructions), cmd-x,
                     text:abc, return, tab, menu:ID/item (e.g. menu:128/1)
      classic import <file>... [--library dir] [--force]
          Add MacBinary (.bin), StuffIt (.sit/.sea), Compact Pro (.cpt) or BinHex (.hqx)
          files to the library as AppleDouble.  Archives need unar (The Unarchiver).
      classic install <app-or-archive> [--app name] [--to folder] [--force]
          Make a macOS application bundle for a classic application (default folder
          ~/Applications/Classic) with its Finder icon; documents dropped on it open
          in the application.
      classic set <app> [depth 1|8|16|32|default] [cpu 68020|68040|default] [--library dir]
          Remember settings for an application (by file name), used by every run,
          including installed bundles: e.g. classic set "Star Chaos" depth 8 for a
          game that wants 256 colours, or cpu 68020 for one that misbehaves on a
          68040.  With no setting given, show them.
      classic apps [--library dir]
          List the applications in the library.
      classic themes [--library dir]
          List the themes (Kaleidoscope schemes) in the library.
      classic rsrc <file>
          List the resources in a file (AppleDouble or MacBinary).

      The library defaults to \(defaultLibrary())
      (override with CLASSIC_LIBRARY or --library).

    """)
    exit(2)
}

/// A String from a NUL-terminated C buffer.
func str(_ buf: [CChar]) -> String {
    let bytes = buf.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
    return String(decoding: bytes, as: UTF8.self)
}

func cString(_ tuple: UnsafeRawPointer, _ length: Int) -> String {
    let bytes = tuple.assumingMemoryBound(to: UInt8.self)
    var out = [UInt8]()
    for i in 0..<length where bytes[i] != 0 { out.append(bytes[i]) }
    return String(decoding: out, as: UTF8.self)
}

func fourCC(_ code: UInt32) -> String {
    var s = ""
    for shift in stride(from: 24, through: 0, by: -8) {
        let b = UInt8((code >> UInt32(shift)) & 0xFF)
        s.append(b >= 0x20 && b < 0x7F ? Character(UnicodeScalar(b)) : "?")
    }
    return s
}

func parseFourCC(_ s: String) -> UInt32? {
    let bytes = Array(s.utf8)
    guard bytes.count == 4 else { return nil }
    return bytes.reduce(0) { ($0 << 8) | UInt32($1) }
}

func parseNumber(_ s: String) -> UInt64? {
    if s.hasPrefix("0x") || s.hasPrefix("0X") { return UInt64(s.dropFirst(2), radix: 16) }
    return UInt64(s)
}

/// Read any supported file into a mac_file (MacBinary or AppleDouble).
func loadMacFile(_ path: String) -> mac_file {
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    var buf: UnsafeMutablePointer<UInt8>?
    var len = 0
    if read_file(path, &buf, &len), let buf, macbinary_detect(buf, len) {
        defer { free(buf) }
        if !macbinary_parse(buf, len, &mf, &err, err.count) {
            fail(str(err))
        }
        return mf
    }
    free(buf)
    if !appledouble_read(path, &mf, &err, err.count) {
        fail(str(err))
    }
    return mf
}

/// A "menu:ID/item" script step.
func menuStep(_ step: String) -> (Int16, Int16)? {
    guard step.hasPrefix("menu:") else { return nil }
    let parts = step.dropFirst(5).split(separator: "/").compactMap { Int16($0) }
    return parts.count == 2 ? (parts[0], parts[1]) : nil
}

/// Scripted input: wait:N (instructions), cmd-x, text:..., return, tab, menu:ID/item.
func runScript(_ script: [String], budget: inout UInt64) -> run_status {
    var status = RUN_LIMIT
    for step in script {
        if step.hasPrefix("wait:"), let n = UInt64(step.dropFirst(5)) {
            let slice = min(n, budget)
            status = machine_run(slice)
            budget -= slice
            if status == RUN_HALTED { break }
            continue
        }
        if let (menu, item) = menuStep(step) {
            machine_choose_menu(menu, item)
            status = machine_run(min(200_000, budget))
            budget -= min(200_000, budget)
            continue
        }
        var mods: UInt32 = 0
        var chars: [UInt8] = []
        if step.hasPrefix("cmd-") {
            mods = 0x100
            chars = Array(step.dropFirst(4).utf8)
        } else if step.hasPrefix("text:") {
            chars = Array(step.dropFirst(5).utf8)
        } else if step == "return" {
            chars = [13]
        } else if step == "tab" {
            chars = [9]
        }
        for ch in chars {
            machine_post_key(Int32(ch), 0, mods, 1, 0)
            machine_post_key(Int32(ch), 0, mods, 0, 0)
            status = machine_run(min(50_000, budget))
            budget -= min(50_000, budget)
        }
    }
    return status
}

// MARK: - Commands

func runCommand(_ args: [String]) {
    var app: String?
    var documents: [String] = []
    var appName: String?
    var census = false
    var maxInstructions: UInt64 = 50_000_000
    var logMask: UInt32 = UInt32(LOG_UNIMPL)
    var gestalts: [(UInt32, UInt32)] = []
    var fontFiles: [String] = []
    var keyScript: [String] = []
    var headless = false
    var screenWindow = false
    var screenshot: String?
    var screenDepth: Int32?
    var cpuModel: Int32?
    var printTo: String?
    var appearance = Int32(MACHINE_APPEARANCE_BW)
    var themePath: String?
    var i = 0
    while i < args.count {
        let a = args[i]
        switch a {
        case "--census": census = true
        case "--headless": headless = true
        case "--screen-window": screenWindow = true
        case "--fonts":
            i += 1
            guard i < args.count else { usage() }
            fontFiles.append(args[i])
        case "--keys":
            i += 1
            guard i < args.count else { usage() }
            keyScript = args[i].split(separator: ",").map(String.init)
        case "--appearance":
            i += 1
            guard i < args.count else { usage() }
            switch args[i] {
            case "bw", "classic": appearance = Int32(MACHINE_APPEARANCE_BW)
            case "native": appearance = Int32(MACHINE_APPEARANCE_NATIVE)
            default:
                guard let path = findTheme(args[i]) else {
                    fail("no theme \"\(args[i])\": give a scheme file, or import one and use its name (classic themes)")
                }
                themePath = path
                appearance = Int32(MACHINE_APPEARANCE_THEME)
            }
        case "--print-to":
            i += 1
            guard i < args.count else { usage() }
            printTo = args[i]
        case "--depth":
            i += 1
            guard i < args.count, let d = Int32(args[i]), [1, 8, 16, 32].contains(d) else {
                fail("--depth expects 1, 8, 16 or 32")
            }
            screenDepth = d
        case "--cpu":
            i += 1
            guard i < args.count, let m = Int32(args[i]), [68020, 68040].contains(m) else {
                fail("--cpu expects 68020 or 68040")
            }
            cpuModel = m
        case "--screenshot":
            i += 1
            guard i < args.count else { usage() }
            screenshot = args[i]
        case "--screen":
            i += 1
            let parts = i < args.count ? args[i].split(separator: "x").compactMap { Int32($0) } : []
            guard parts.count == 2 else { fail("--screen expects WxH, e.g. 1024x768") }
            machine_set_screen(parts[0], parts[1])
            screenWindow = true
        case "--app":
            i += 1
            guard i < args.count else { usage() }
            appName = args[i]
        case "--max":
            i += 1
            guard i < args.count, let n = parseNumber(args[i]) else { usage() }
            maxInstructions = n
        case "--log":
            i += 1
            guard i < args.count else { usage() }
            logMask = 0
            for item in args[i].split(separator: ",") {
                switch item {
                case "traps": logMask |= UInt32(LOG_TRAPS)
                case "unimpl": logMask |= UInt32(LOG_UNIMPL)
                case "seg": logMask |= UInt32(LOG_SEG)
                case "rsrc": logMask |= UInt32(LOG_RSRC)
                case "mem": logMask |= UInt32(LOG_MEM)
                case "cpu": logMask |= UInt32(LOG_CPU)
                default: fail("unknown log category \(item)")
                }
            }
        case "--gestalt":
            i += 1
            guard i < args.count else { usage() }
            let parts = args[i].split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2, let sel = parseFourCC(parts[0]), let v = parseNumber(parts[1]) else {
                fail("--gestalt expects sel=value, e.g. sysv=0x0755")
            }
            gestalts.append((sel, UInt32(truncatingIfNeeded: v)))
        default:
            if a.hasPrefix("-psn_") { break } // LaunchServices process serial number
            if a.hasPrefix("-") { usage() }
            if app == nil { app = a } else { documents.append(a) }
        }
        i += 1
    }
    guard var app else { usage() }
    app = resolveApplication(app, named: appName)

    #if canImport(AppKit)
    let interactive = !headless && !census && screenshot == nil && printTo == nil
    if interactive && !screenWindow {
        // Rootless: the Mac screen covers the host display's visible area.
        MainActor.assumeIsolated {
            let vf = rootlessScreenFrame()
            machine_set_screen(Int32(vf.width), Int32(vf.height + classicMenuBarHeight))
        }
    }
    #endif
    machine_set_appearance(appearance)
    if let themePath {
        var terr = [CChar](repeating: 0, count: 256)
        guard machine_load_theme(themePath, &terr, terr.count) else { fail(str(terr)) }
    }
    if let screenDepth {
        machine_set_screen_depth(screenDepth)
    } else if let d = settings(for: app)?.depth {
        machine_set_screen_depth(Int32(d))
    }
    if let m = cpuModel ?? settings(for: app)?.cpu.map(Int32.init) { machine_set_cpu(m) }
    guard machine_boot(census) else { fail("could not set up guest memory") }
    machine_set_log(logMask)
    for (sel, v) in gestalts { gestalt_set(sel, v) }

    var err = [CChar](repeating: 0, count: 256)
    for f in fontFiles {
        guard machine_add_resource_file(f, &err, err.count) else { fail(str(err)) }
    }
    var launchDocs = documents.map { strdup($0) }
    launchDocs.withUnsafeMutableBufferPointer { buf in
        buf.baseAddress.map {
            $0.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: buf.count) {
                machine_set_launch_documents($0, Int32(buf.count))
            }
        }
    }
    launchDocs.forEach { free($0) }
    guard machine_launch(app, &err, err.count) else { fail(str(err)) }
    // As the Finder does: open the documents, or just open the application.
    // An interactive host waits, since the Finder may still hand it documents.
    if !documents.isEmpty {
        var cpaths = documents.map { strdup($0) }
        cpaths.withUnsafeMutableBufferPointer { buf in
            buf.baseAddress!.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: buf.count) {
                machine_open_documents($0, Int32(buf.count), false)
            }
        }
        cpaths.forEach { free($0) }
    }
    #if canImport(AppKit)
    if let printTo { installPrintToFile(printTo) }
    launchDocumentsSent = !documents.isEmpty
    if !interactive && documents.isEmpty { machine_send_open_application() }
    #else
    if documents.isEmpty { machine_send_open_application() }
    #endif

    #if canImport(AppKit)
    if interactive {
        let title = (app as NSString).lastPathComponent
        let script = keyScript
        let icons = finderBundle(applicationAt: app).appIcons
        if screenWindow {
            MainActor.assumeIsolated { runHostWindow(title: title, icon: dockImage(icons)) }
        } else {
            MainActor.assumeIsolated { runRootless(title: title, icon: dockImage(icons), script: script) }
        }
    }
    #endif

    var budget = maxInstructions
    machine_set_instruction_limit(machine_instruction_count() + maxInstructions)
    var status = runScript(keyScript, budget: &budget)
    if status != RUN_HALTED && budget > 0 {
        status = machine_run(budget)
    }

    let count = machine_instruction_count()
    if status == RUN_HALTED {
        print("stopped: \(String(cString: machine_halt_reason())) after \(count) instructions")
    } else {
        let waiting = machine_waiting_for_input() ? " waiting for input" : ""
        print("instruction limit reached\(waiting) after \(count) instructions (pc \(String(machine_pc(), radix: 16, uppercase: true)))")
    }

    if let screenshot {
        if machine_screenshot(screenshot) { print("screenshot: \(screenshot)") } else { eprint("could not write \(screenshot)\n") }
    }

    var regs = [CChar](repeating: 0, count: 512)
    machine_describe_cpu(&regs, regs.count)
    print(str(regs))

    var rows = [census_row](repeating: census_row(), count: 4096)
    let n = Int(machine_census(&rows, UInt32(rows.count)))
    if n > 0 {
        let seen = rows[0..<min(n, rows.count)]
        let done = seen.filter { $0.implemented }.count
        print("\ntraps used: \(seen.count) (\(done) implemented, \(seen.count - done) missing)")
        for r in seen.sorted(by: { ($0.implemented ? 1 : 0, $0.trap, $0.selector) < ($1.implemented ? 1 : 0, $1.trap, $1.selector) }) {
            let mark = r.implemented ? " " : "!"
            let sel = r.selector >= 0 ? " sel \(r.selector)" : ""
            let trap = String(r.trap, radix: 16, uppercase: true)
            print("  \(mark) \(trap)\(sel)  \(String(cString: r.name))  ×\(r.calls)")
        }
    }
    exit(status == RUN_HALTED && String(cString: machine_halt_reason()).hasPrefix("ExitToShell") ? 0 : 1)
}

/// Turn a run argument into an AppleDouble application path, ingesting
/// MacBinary and archives into the library first.
func resolveApplication(_ path: String, named: String?) -> String {
    switch classify(path) {
    case .appleDouble:
        return path
    case .plain:
        fail("\(path) has no resource fork: expected an application, a disk image, or a .bin/.sit/.sea/.cpt/.hqx file")
    case .macBinary, .archive, .diskImage:
        break
    }
    let entry: String
    do {
        let result = try ingest(path, library: defaultLibrary())
        entry = result.entry
        for n in result.notes { eprint("note: \(n)\n") }
    } catch {
        fail("\(error)")
    }
    let apps = scan(entry).filter { $0.isApplication }
    var candidates = apps.filter { $0.hasCODE }
    if let named {
        candidates = candidates.filter {
            ($0.path as NSString).lastPathComponent.range(of: named, options: .caseInsensitive) != nil
        }
    }
    if candidates.count == 1 { return candidates[0].path }
    if candidates.isEmpty {
        let ppc = apps.filter { $0.hasCfrg && !$0.hasCODE }
        if !ppc.isEmpty {
            fail("only PowerPC applications in \(entry) (68k only for now): " +
                 ppc.map { ($0.path as NSString).lastPathComponent }.joined(separator: ", "))
        }
        fail(named == nil ? "no 68k application found in \(entry)" : "no application matching \"\(named!)\" in \(entry)")
    }
    eprint("several applications in \(entry); choose one with --app:\n")
    for a in candidates { eprint("  \((a.path as NSString).lastPathComponent)\n") }
    exit(2)
}

func printEntry(_ entry: String, filesOnly: Bool = false) {
    for f in scan(entry) {
        let rel = String(f.path.dropFirst(entry.count + 1))
        let kind = f.isApplication ? "app \(f.architecture)" : ""
        print("  \(fourCCString(f.type))/\(fourCCString(f.creator))  \(kind.padding(12)) \(rel)")
    }
}

func importCommand(_ args: [String]) {
    var files = [String]()
    var library = defaultLibrary()
    var force = false
    var i = 0
    while i < args.count {
        switch args[i] {
        case "--library":
            i += 1
            guard i < args.count else { usage() }
            library = args[i]
        case "--force": force = true
        default:
            if args[i].hasPrefix("-") { usage() }
            files.append(args[i])
        }
        i += 1
    }
    guard !files.isEmpty else { usage() }
    var failed = false
    for file in files {
        do {
            let (entry, notes) = try ingest(file, library: library, force: force)
            print(entry)
            for n in notes { print("  note: \(n)") }
            printEntry(entry)
        } catch {
            eprint("classic: \(file): \(error)\n")
            failed = true
        }
    }
    exit(failed ? 1 : 0)
}

func appsCommand(_ args: [String]) {
    var library = defaultLibrary()
    if args.count == 2 && args[0] == "--library" {
        library = args[1]
    } else if !args.isEmpty {
        usage()
    }
    let apps = scan(library).filter { $0.isApplication }
    if apps.isEmpty {
        print("no applications in \(library)")
        return
    }
    for a in apps {
        let rel = String(a.path.dropFirst(library.count + 1))
        print("\(a.architecture.padding(8)) \(fourCCString(a.creator))  \(rel)")
    }
}

/// Kaleidoscope schemes ('Colr'/'Acid' files) in a library.
func themes(in library: String) -> [String] {
    scan(library).filter { fourCCString($0.type) == "Colr" && fourCCString($0.creator) == "Acid" }.map(\.path)
}

/// A theme by path (a scheme file, or an archive or disk image holding
/// schemes, which is imported first; "archive.sit:Name" picks one of
/// several), or by name among the library's schemes.
func findTheme(_ spec: String) -> String? {
    let fm = FileManager.default
    var name = spec, pick: String?
    if !fm.fileExists(atPath: name), let colon = spec.lastIndex(of: ":"),
       fm.fileExists(atPath: String(spec[..<colon])) {
        name = String(spec[..<colon])
        pick = String(spec[spec.index(after: colon)...])
    }
    if fm.fileExists(atPath: name) || fm.fileExists(atPath: sidecarPath(name)) {
        switch classify(name) {
        case .archive, .macBinary, .diskImage:
            let entry: String
            do { entry = try ingest(name, library: defaultLibrary()).entry } catch { fail("\(error)") }
            let found = themes(in: entry)
            if let pick {
                return found.first { ($0 as NSString).lastPathComponent.caseInsensitiveCompare(pick) == .orderedSame }
            }
            if found.count == 1 { return found[0] }
            if found.isEmpty { fail("no Kaleidoscope schemes in \(name)") }
            eprint("several schemes in \(name); choose one with --appearance \"\(name):Name\" or by name:\n")
            for t in found { eprint("  \((t as NSString).lastPathComponent)\n") }
            exit(2)
        default:
            return name
        }
    }
    let all = themes(in: defaultLibrary())
    return all.first { ($0 as NSString).lastPathComponent.caseInsensitiveCompare(name) == .orderedSame }
        ?? all.first { ($0 as NSString).lastPathComponent.range(of: name, options: .caseInsensitive) != nil }
}

func themesCommand(_ args: [String]) {
    var library = defaultLibrary()
    if args.count == 2 && args[0] == "--library" { library = args[1] } else if !args.isEmpty { usage() }
    let found = themes(in: library)
    if found.isEmpty { print("no themes in \(library); import a Kaleidoscope scheme archive with classic import") }
    for t in found { print("\((t as NSString).lastPathComponent)  (\(String(t.dropFirst(library.count + 1))))") }
}

func rsrcCommand(_ args: [String]) {
    guard args.count == 1 else { usage() }
    var mf = loadMacFile(args[0])
    defer { mac_file_free(&mf) }
    var idx = rsrc_index()
    var err = [CChar](repeating: 0, count: 256)
    guard rsrc_parse(mf.rsrc, mf.rsrc_len, &idx, &err, err.count) else { fail(str(err)) }
    defer { rsrc_index_free(&idx) }
    print("\(fourCC(mac_file_type(&mf)))/\(fourCC(mac_file_creator(&mf)))  data \(mf.data_len)  rsrc \(mf.rsrc_len)  \(idx.count) resources")
    for i in 0..<idx.count {
        var e = idx.entries[i]
        let data = rsrc_data(mf.rsrc, &idx, &e)
        let compressed = rsrc_is_compressed(data, Int(e.data_len))
        let size = compressed ? "\(e.data_len) → \(rsrc_decompressed_size(data, Int(e.data_len)))" : "\(e.data_len)"
        let name = cString(&e.name, 256)
        let attrs = String(e.attrs, radix: 16)
        print("  \(fourCC(e.type)) \(String(e.id).padding(6))  \(size.padding(14)) attrs \(attrs)\(name.isEmpty ? "" : "  \"\(name)\"")")
    }
}

extension String {
    func padding(_ width: Int) -> String {
        count >= width ? self : self + String(repeating: " ", count: width - count)
    }
}

// MARK: - Main

let argv = Array(CommandLine.arguments.dropFirst())
guard let command = argv.first else { usage() }
let rest = Array(argv.dropFirst())
switch command {
case "run": runCommand(rest)
case "import": importCommand(rest)
case "apps": appsCommand(rest)
case "set": setCommand(rest)
case "rsrc": rsrcCommand(rest)
case "themes": themesCommand(rest)
#if canImport(AppKit)
case "install": installCommand(rest)
#endif
default: usage()
}
