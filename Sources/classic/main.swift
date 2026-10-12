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
      classic run <app-or-archive> [--app name] [--census] [--max N] [--log list] [--gestalt sel=value]...
          Run a classic application.  The argument may be an AppleDouble application
          (data file + ._ sidecar) or a .bin/.sit/.sea/.cpt/.hqx file, which is first
          added to the library; --app picks one when the archive holds several.
          --census   keep going past unimplemented traps and report every trap used
          --max N    stop after N instructions (default 50000000)
          --log      comma list of: traps, unimpl, seg, rsrc, mem, cpu
          --gestalt  override a Gestalt selector, e.g. sysv=0x0755
      classic import <file>... [--library dir] [--force]
          Add MacBinary (.bin), StuffIt (.sit/.sea), Compact Pro (.cpt) or BinHex (.hqx)
          files to the library as AppleDouble.  Archives need unar (The Unarchiver).
      classic apps [--library dir]
          List the applications in the library.
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

// MARK: - Commands

func runCommand(_ args: [String]) {
    var app: String?
    var appName: String?
    var census = false
    var maxInstructions: UInt64 = 50_000_000
    var logMask: UInt32 = UInt32(LOG_UNIMPL)
    var gestalts: [(UInt32, UInt32)] = []
    var i = 0
    while i < args.count {
        let a = args[i]
        switch a {
        case "--census": census = true
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
            if a.hasPrefix("-") || app != nil { usage() }
            app = a
        }
        i += 1
    }
    guard var app else { usage() }
    app = resolveApplication(app, named: appName)

    guard machine_boot(census) else { fail("could not set up guest memory") }
    machine_set_log(logMask)
    for (sel, v) in gestalts { gestalt_set(sel, v) }

    var err = [CChar](repeating: 0, count: 256)
    guard machine_launch(app, &err, err.count) else { fail(str(err)) }

    let status = machine_run(maxInstructions)
    let count = machine_instruction_count()
    if status == RUN_HALTED {
        print("stopped: \(String(cString: machine_halt_reason())) after \(count) instructions")
    } else {
        print("instruction limit reached after \(count) instructions (pc \(String(machine_pc(), radix: 16, uppercase: true)))")
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
        fail("\(path) has no resource fork: expected an application, .bin, .sit, .sea, .cpt or .hqx")
    case .macBinary, .archive:
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
case "rsrc": rsrcCommand(rest)
default: usage()
}
