// classic: command-line entry point.
//
//   bluebox run <app> [--census] [--max N] [--log traps,unimpl,seg,rsrc,mem,cpu]
//                     [--gestalt sel=value]...
//   bluebox import <macbinary-file> <dest-dir>
//   bluebox rsrc <file>

#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif
import CCore
import Foundation
import ForkStore
import BlueBoxHost
import Toolbox
#if canImport(AppKit)
import AppKit
#endif

/// Write to standard error (fd 2; glibc's `stderr` global is not concurrency-safe).
func eprint(_ s: String) {
    var bytes = Array(s.utf8)
    _ = bytes.withUnsafeMutableBytes { write(2, $0.baseAddress, $0.count) }
}

func fail(_ message: String) -> Never {
    eprint("bluebox: \(message)\n")
    exit(1)
}

/// --appleshare: AppleShare volumes to mount.
nonisolated(unsafe) var appleShares: [String] = []
/// --file-answer's paths, taken in turn by open and save dialogs.
nonisolated(unsafe) var headlessFileAnswers: [String] = []
/// --speech: system (NSSpeechSynthesizer), avspeech (AVSpeechSynthesizer) or record:FILE.
nonisolated(unsafe) var speechBackend = "system"

func usage() -> Never {
    eprint("""
    usage:
      bluebox run <app-or-archive> [document...] [--app name] [--census] [--max N] [--log list] [--gestalt sel=value]...
          Run a classic application.  The argument may be an AppleDouble application
          (data file + ._ sidecar) or a .bin/.sit/.sea/.cpt/.hqx file, which is first
          added to the library; --app picks one when the archive holds several.
          --headless run without a window (the default on Linux for now)
          --realtime a headless run's clock follows real time, not the instructions
                     run (for networked programs talking to other copies)
          --screen-window  show the whole classic screen in one window instead of
                     rootless windows (implied by --screen)
          --census   keep going past unimplemented traps and report every trap used
          --max N    stop after N instructions (default 50000000)
          --log      comma list of: traps, unimpl, seg, rsrc, mem, cpu
          --gestalt  override a Gestalt selector, e.g. sysv=0x0755
          --lowmem N=V  set a low-memory setting: DoubleTime, CaretTime, KeyThresh,
                     KeyRepThresh (ticks), SdVolume, MenuFlash, DlgFont, SysFontFam,
                     ApFontID, ScrDmpEnb or the SP parameter RAM copies, e.g.
                     DoubleTime=20 (windowed runs take the first four from macOS)
          --fonts F  also open resource file F (AppleDouble) for fonts; repeatable
          --screen WxH  screen size (default 1024x768)
          --ppc      run a fat application's PowerPC code (PowerPC-only ones always run it)
          --68k      run a fat application's 68k code (the default unless its cpu setting is ppc)
          --depth N  screen depth: 1, 2, 4, 8, 16 or 32 bits (default 32, or the app's setting; an
                     application can still switch depth itself with SetDepth).  Without --depth,
                     a game known to need another depth gets it (unless it has a depth setting),
                     and one whose alert asks for another is started again at that depth, which
                     replaces its setting
          --cpu M    processor: 68040 (default, with FPU) or 68020 (no FPU)
          --appletalk A  AppleTalk on LocalTalk over UDP: local (the default; copies
                     on this host play each other), lan (the local network, other
                     emulators and routers that speak LToUDP), lan:IF (the local
                     network through interface IF, e.g. lan:en0), bridge:IF (EtherTalk
                     on wired interface IF, with real Macs, file servers and routers;
                     asks for an administrator password when AppleTalk opens) or off
          --modem-port S, --printer-port S  what a serial port connects to: modem (a
                     Hayes modem, the modem port's default: ATDT host:port dials over
                     TCP), modem:PORT (one that also answers calls to PORT),
                     tcp:HOST:PORT, listen:PORT, pty, a device such as
                     /dev/cu.usbserial, or none (the printer port's default)
          --scale S  host points per Mac pixel: 1, 1.5 or 2 (default 1, or the app's or the
                     default setting), so classic interfaces are bigger on modern displays
          --screenshot P  write the screen to PNG file P when the run ends
          --redraw-check P  when the run ends waiting, repaint everything, let the program
                     redraw (5 million instructions) and compare, leaving out pixels
                     that change on their own: the differences go to
                     PNG file P, with P-before.png and P-after.png (screens where
                     incremental drawing missed something)
          --print-to F  print to PDF file F instead of the print panel (headless runs)
          --speech S  speech through system (NSSpeechSynthesizer, the default), avspeech
                     (AVSpeechSynthesizer, also used when the system one is missing)
                     or record:FILE (requests logged, callbacks at once, no sound)
          --appleshare S  mount an AppleShare volume (repeatable): tcp:[user[:pass]@]host/Volume
                     (AFP over TCP) or atalk:[user[:pass]@]Server:AFPServer[@zone]/Volume
                     (over AppleTalk, which --appletalk must turn on)
          --file-answer P  answer the next open or save dialog with host path P,
                     without a panel (headless runs; repeatable, in order)
          --appearance A  bw (default); native (macOS window frames); or a theme: a
                     Kaleidoscope scheme file, an archive of schemes (.sit etc.,
                     "archive.sit:Name" picks one), or the name of an imported
                     scheme (bluebox themes lists them); themes use a 32-bit screen
          --keys S   scripted input, comma separated: wait:N (instructions), cmd-x,
                     text:abc, return, tab, menu:ID/item (e.g. menu:128/1),
                     keydown:K and keyup:K (K a character, space, return, tab,
                     escape, delete or an arrow: left, right, up, down), and
                     the mouse: click:x/y, down:x/y, move:x/y, up:x/y, drag:x1/y1/x2/y2;
                     in a window, wait:N is N/1000 ms and snap:F.png saves the screen
      bluebox import <file>... [--library dir] [--force]
          Add MacBinary (.bin), StuffIt (.sit/.sea), Compact Pro (.cpt) or BinHex (.hqx)
          files to the library as AppleDouble.  Archives need unar (The Unarchiver).
      bluebox install <app-or-archive> [--app name] [--to folder] [--force]
          Make a macOS application bundle for a classic application (default folder
          ~/Applications/Blue Box) with its Finder icon; documents dropped on it open
          in the application.
      bluebox set <app>|--default [depth 1|2|4|8|16|32|default] [cpu 68020|68040|ppc|default]
                [scale 1|1.5|2|default] [appletalk local|lan|lan:IF|bridge:IF|off|default]
                [modem-port S|default] [printer-port S|default] [print-folder F|panel|default]
                [appleshare S|none|default] [display rootless|screen|default]
                [--library dir]
          Remember settings for an application (by file name), used by every run,
          including installed bundles: e.g. bluebox set "Star Chaos" depth 8 for a
          game that wants 256 colours, or cpu 68020 for one that misbehaves on a
          68040.  --default sets them for every application without its own
          (bluebox set --default scale 2 doubles every classic window).  With no
          setting given, show them.
      bluebox shell
          Open the Blue Box shell: the library's applications to launch, with their
          settings and the defaults for every application; drop archives on its
          window (or its Dock icon) to import them.
      bluebox apps [--library dir]
          List the applications in the library and those installed on the classic
          Mac's disk (\(defaultDisk())).  run and install take
          an application as listed ("Macintosh HD/Folder/App") or by its name.
      bluebox themes [--library dir]
          List the themes (Kaleidoscope schemes) in the library.
      bluebox rsrc <file>
          List the resources in a file (AppleDouble or MacBinary).
      bluebox fonts [import <disk image>...]
          List the fonts in Blue Box's Fonts folder (\(fontsFolder())), or
          copy the fonts of a System disk image's System Folder there (raw,
          DiskCopy or .dmg, or any of these gzipped): every run opens them,
          so the real classic fonts take the place of Blue Box's look-alikes.

      The library defaults to \(defaultLibrary())
      (override with BLUEBOX_LIBRARY or --library).

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

/// US key codes for scripted keys: characters, and named keys.
let scriptKeyCodes: [String: Int32] = {
    var codes: [String: Int32] = [:]
    let rows: [(String, [Int32])] = [
        ("asdfhgzxcv", [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]),
        ("bqweryt123465=97-8", [11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28]),
        ("0]ou[ip", [29, 30, 31, 32, 33, 34, 35]),
        ("lj'k;\\,/nm.", [37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47]),
    ]
    for (chars, keys) in rows {
        for (c, k) in zip(chars, keys) { codes[String(c)] = k; codes[String(c).uppercased()] = k }
    }
    codes["`"] = 50
    for (name, k) in [("return", 36), ("tab", 48), ("space", 49), ("delete", 51), ("escape", 53),
                      ("left", 123), ("right", 124), ("down", 125), ("up", 126)] as [(String, Int32)] {
        codes[name] = k
    }
    return codes
}()

/// The character a scripted key name types.
func scriptKeyChar(_ name: String) -> Int32 {
    name.count == 1 ? Int32(name.utf8.first!) :
        ["return": 13, "tab": 9, "space": 32, "delete": 8, "escape": 27, "left": 28, "right": 29,
         "up": 30, "down": 31][name] ?? 0
}

/// Scripted input: wait:N (instructions), cmd-x, text:..., return, tab, menu:ID/item,
/// keydown:K and keyup:K (K a character or return, tab, space, delete, escape,
/// left, right, up, down: held for games that poll the keyboard), and the mouse.  Each step is queued at an instruction count (the waits
/// add up; other steps are spaced a little apart) and applied by the
/// emulator when due, even while the application waits in the Toolbox.
func queueScript(_ script: [String], start: UInt64) {
    var at = start
    func add(_ kind: Int32, _ a: Int32, _ b: Int32, _ c: Int32 = 0, after gap: UInt64) {
        machine_script_add(at, kind, a, b, c)
        at += gap
    }
    for step in script {
        if step.hasPrefix("wait:"), let n = UInt64(step.dropFirst(5)) {
            at += n
            continue
        }
        if let (menu, item) = menuStep(step) {
            add(2, Int32(menu), Int32(item), after: 200_000)
            continue
        }
        // Mouse: down:x/y, move:x/y, up:x/y, click:x/y (press and release),
        // drag:x1/y1/x2/y2 (press, move and release), in global coordinates.
        if step.hasPrefix("drag:") {
            let v = step.dropFirst(5).split(separator: "/").compactMap { Int32($0) }
            guard v.count == 4 else { fail("drag takes x1/y1/x2/y2: \(step)") }
            add(1, v[0], v[1], 1, after: 0)
            add(1, v[2], v[3], -1, after: 0)
            add(1, v[2], v[3], 0, after: 400_000)
            continue
        }
        if let colon = step.firstIndex(of: ":"),
           ["down", "move", "up", "click"].contains(String(step[..<colon])) {
            let xy = step[step.index(after: colon)...].split(separator: "/").compactMap { Int32($0) }
            guard xy.count == 2 else { fail("mouse steps take x/y: \(step)") }
            let kind = String(step[..<colon])
            let presses: [Int32] = kind == "click" ? [1, 0] : kind == "down" ? [1] : kind == "up" ? [0] : [-1]
            for b in presses { add(1, xy[0], xy[1], b, after: 0) }
            at += 200_000
            continue
        }
        if let colon = step.firstIndex(of: ":"), ["keydown", "keyup"].contains(String(step[..<colon])) {
            let name = String(step[step.index(after: colon)...])
            guard let code = scriptKeyCodes[name] else { fail("unknown key: \(step)") }
            let ch = scriptKeyChar(name)
            add(step.hasPrefix("keydown") ? 3 : 4, ch, 0, code, after: 50_000)
            continue
        }
        var mods: Int32 = 0
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
            let code = ch == 13 ? 36 : ch == 9 ? 48 : scriptKeyCodes[String(UnicodeScalar(ch))] ?? 0
            add(0, Int32(ch), mods, code, after: 50_000)
        }
    }
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
    var lowmems: [(String, UInt32)] = []
    var fontFiles: [String] = []
    var keyScript: [String] = []
    var headless = false
    var screenWindow = false
    var appletalkGiven = false
    var portsGiven = Set<Int32>()
    var screenshot: String?
    var screenDepth: Int32?
    var preferPPC: Bool? /* nil: the application's setting */
    var scaleOption: Double?
    var redrawCheck: String?
    var cpuModel: Int32?
    var printTo: String?
    var appearance = Int32(MACHINE_APPEARANCE_BW)
    var appearanceGiven = false
    var themePath: String?
    var i = 0
    while i < args.count {
        let a = args[i]
        switch a {
        case "--census": census = true
        case "--headless": headless = true
        case "--realtime": machine_set_realtime(true)
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
            appearanceGiven = true
            switch args[i] {
            case "bw", "classic": appearance = Int32(MACHINE_APPEARANCE_BW)
            case "native": appearance = Int32(MACHINE_APPEARANCE_NATIVE)
            default:
                guard let path = findTheme(args[i]) else {
                    fail("no theme \"\(args[i])\": give a scheme file, or import one and use its name (bluebox themes)")
                }
                themePath = path
                appearance = Int32(MACHINE_APPEARANCE_THEME)
            }
        case "--print-to":
            i += 1
            guard i < args.count else { usage() }
            printTo = args[i]
        case "--speech":
            i += 1
            guard i < args.count else { usage() }
            speechBackend = args[i]
            if speechBackend.hasPrefix("record:") {
                guard machine_set_speech_record(String(speechBackend.dropFirst(7))) else {
                    fail("--speech: can't write \(speechBackend.dropFirst(7))")
                }
            } else if !["system", "avspeech"].contains(speechBackend) {
                fail("--speech expects system, avspeech or record:FILE")
            }
        case "--appleshare":
            i += 1
            guard i < args.count else { usage() }
            appleShares.append(args[i])
        case "--file-answer":
            i += 1
            guard i < args.count else { usage() }
            headlessFileAnswers.append(args[i])
        case "--redraw-check":
            i += 1
            guard i < args.count else { usage() }
            redrawCheck = args[i]
        case "--scale":
            i += 1
            guard i < args.count, let v = Double(args[i]), [1, 1.5, 2].contains(v) else {
                fail("--scale expects 1, 1.5 or 2")
            }
            scaleOption = v
        case "--ppc": preferPPC = true
        case "--68k": preferPPC = false
        case "--depth":
            i += 1
            guard i < args.count, let d = Int32(args[i]), [1, 2, 4, 8, 16, 32].contains(d) else {
                fail("--depth expects 1, 2, 4, 8, 16 or 32")
            }
            screenDepth = d
        case "--cpu":
            i += 1
            guard i < args.count, let m = Int32(args[i]), [68020, 68040].contains(m) else {
                fail("--cpu expects 68020 or 68040")
            }
            cpuModel = m
        case "--appletalk":
            i += 1
            if i < args.count, args[i].hasPrefix("bridge:"), isWireless(String(args[i].dropFirst(7))) {
                fail("--appletalk: \(args[i].dropFirst(7)) is Wi-Fi, which drops EtherTalk; bridge onto a wired interface")
            }
            guard i < args.count, machine_set_appletalk(args[i]) else { fail("--appletalk expects local, lan, lan:IF (e.g. lan:en0), bridge:IF or off") }
            appletalkGiven = true
        case "--modem-port", "--printer-port":
            let port: Int32 = args[i] == "--modem-port" ? 0 : 1
            i += 1
            portsGiven.insert(port)
            guard i < args.count, machine_set_serial(port, args[i]) else {
                fail("\(args[i - 1]) expects modem, modem:PORT, tcp:HOST:PORT, listen:PORT, pty, a device or none")
            }
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
        case "--lowmem":
            i += 1
            guard i < args.count else { usage() }
            let parts = args[i].split(separator: "=", maxSplits: 1).map(String.init)
            guard parts.count == 2, let v = parseNumber(parts[1]) else { fail("--lowmem expects name=value, e.g. DoubleTime=20") }
            lowmems.append((parts[0], UInt32(truncatingIfNeeded: v)))
        default:
            if a.hasPrefix("-psn_") { break } // LaunchServices process serial number
            if a.hasPrefix("-") { usage() }
            if app == nil { app = a } else { documents.append(a) }
        }
        i += 1
    }
    guard var app else { usage() }
    app = resolveApplication(app, named: appName)

    // The application's settings over the defaults; a program known to
    // refuse the default depth gets one it accepts.
    var saved = (settings(for: app) ?? AppSettings()).over(defaultSettings())
    if screenDepth == nil {
        if settings(for: app)?.depth == nil, let d = requiredDepth(application: app, over: saved.depth) {
            saved.depth = d
        }
        // Not given on the command line: if it says it needs another depth
        // (even one set for it, which it cannot run at), it runs again at
        // that one, kept as its setting.
        depthRelaunch = (app, !census && screenshot == nil && printTo == nil)
    }
    #if canImport(AppKit)
    let interactive = !headless && !census && screenshot == nil && printTo == nil
    if interactive { reexecInNamedBundle(app: app) }
    if interactive {
        let scale = scaleOption ?? saved.scale ?? 1
        MainActor.assumeIsolated { displayScale = CGFloat(scale) }
    }
    if !appearanceGiven, let a = saved.appearance {
        switch a {
        case "bw", "classic": appearance = Int32(MACHINE_APPEARANCE_BW)
        case "native": appearance = Int32(MACHINE_APPEARANCE_NATIVE)
        default:
            if let path = findTheme(a) {
                themePath = path
                appearance = Int32(MACHINE_APPEARANCE_THEME)
            } else {
                eprint("bluebox: no theme \"\(a)\" (from the settings); using the default appearance\n")
            }
        }
    }
    if interactive && !screenWindow {
        // Rootless: the Mac screen covers the host display's visible area.
        MainActor.assumeIsolated {
            let (w, h) = rootlessMacScreenSize()
            machine_set_screen(w, h)
        }
    }
    #endif
    machine_set_appearance(appearance)
    // Network and serial ports from the settings, unless given here.
    if !appletalkGiven, let a = saved.appletalk, a.hasPrefix("bridge:"), isWireless(String(a.dropFirst(7))) {
        eprint("bluebox: appletalk \"\(a)\" (from the settings) is Wi-Fi, which drops EtherTalk; AppleTalk is on this Mac only\n")
    } else if !appletalkGiven, let a = saved.appletalk, !machine_set_appletalk(a) {
        eprint("bluebox: appletalk \"\(a)\" (from the settings) is not local, lan, lan:IF, bridge:IF or off\n")
    }
    for (port, spec) in [(Int32(0), saved.modemPort), (Int32(1), saved.printerPort)] where !portsGiven.contains(port) {
        if let spec, !machine_set_serial(port, spec) {
            eprint("bluebox: serial port \"\(spec)\" (from the settings) is not one Blue Box knows\n")
        }
    }
    #if canImport(AppKit)
    if let rgb = saved.highlight.flatMap(parseHighlight) {
        machine_set_hilite_color(rgb.0, rgb.1, rgb.2)
    } else if interactive {
        MainActor.assumeIsolated { useHostSelectionColor() }
    }
    #endif
    if let themePath {
        var terr = [CChar](repeating: 0, count: 256)
        guard machine_load_theme(themePath, &terr, terr.count) else { fail(str(terr)) }
    }
    if let screenDepth {
        machine_set_screen_depth(screenDepth)
    } else if let d = saved.depth {
        machine_set_screen_depth(Int32(d))
    }
    // The processor: a 68k model, or PowerPC (750) for a fat application's
    // PowerPC code (a 68k application then gets the default 68040).
    let processor = cpuModel ?? saved.cpu.map(Int32.init)
    if let m = processor, m != Int32(powerPCSetting) { machine_set_cpu(m) }
    machine_set_depth_alerts(depthRelaunch != nil)
    machine_set_prefer_ppc(preferPPC ?? (processor == Int32(powerPCSetting)))
    #if canImport(AppKit)
    // The host's double-click and key-repeat times, in ticks (headless runs
    // keep the defaults, so they repeat exactly).
    if interactive {
        let ticks = { (s: TimeInterval) in Int32((s * 60).rounded()) }
        machine_set_input_timing(ticks(NSEvent.doubleClickInterval), 0, ticks(NSEvent.keyRepeatDelay),
                                 ticks(NSEvent.keyRepeatInterval))
    }
    #endif
    for (name, v) in lowmems where !machine_set_lowmem(name, v) {
        fail("--lowmem: \(name) is not a low-memory setting (DoubleTime, CaretTime, KeyThresh, KeyRepThresh, "
             + "SdVolume, MenuFlash, DlgFont, SysFontFam, ApFontID, ScrDmpEnb, SPVolCtl, SPClikCaret, SPKbd, "
             + "SPMisc2, SPFont, SPAlarm)")
    }
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
    if let (root, name) = volumeRoot(containing: app) { machine_mount_folder(root, name) }
    if appleShares.isEmpty, let s = saved.appleShare, !s.isEmpty { appleShares = [s] }
    if !appleShares.isEmpty { atexit { machine_close_appleshares() } }
    for spec in appleShares {
        var aerr = [CChar](repeating: 0, count: 256)
        guard machine_mount_appleshare(spec, &aerr, aerr.count) else { fail(str(aerr)) }
    }
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
    ClassicImages.install()
    if let printTo { installPrintToFile(printTo) } else { printFolder = saved.printFolder.flatMap { $0.isEmpty ? nil : $0 } }
    if !headlessFileAnswers.isEmpty {
        // Open and save dialogs answered in turn, without a panel.
        machine_set_file_handler({ _, pathOut, pathSize, _ in
            guard let pathOut, !headlessFileAnswers.isEmpty else { return false }
            let answer = headlessFileAnswers.removeFirst()
            let bytes = Array(answer.utf8.prefix(pathSize - 1)) + [0]
            bytes.withUnsafeBufferPointer { pathOut.update(from: UnsafeRawPointer($0.baseAddress!).assumingMemoryBound(to: CChar.self), count: bytes.count) }
            eprint("bluebox: file dialog answered with \(answer)\n")
            return true
        }, nil)
    }
    launchDocumentsSent = !documents.isEmpty
    if !interactive && documents.isEmpty { machine_send_open_application() }
    #else
    if documents.isEmpty { machine_send_open_application() }
    #endif

    #if canImport(AppKit)
    // The whole screen in one window, as the setting says, or when the
    // program turns out to want it: then that becomes its setting.
    if interactive && !screenWindow && saved.display == "screen" { screenWindow = true }
    let chosenFor = app
    MainActor.assumeIsolated {
        RootlessHost.switchesToScreen = saved.display != "rootless"
        RootlessHost.screenModeChosen = { why in
            var all = loadSettings(defaultLibrary())
            var s = all[settingsKey(chosenFor)] ?? AppSettings()
            guard s.display == nil else { return }
            s.display = "screen"
            all[settingsKey(chosenFor)] = s
            saveSettings(all, library: defaultLibrary())
            eprint("bluebox: the whole screen for \(settingsKey(chosenFor)): \(why) (now its display setting)\n")
        }
    }
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

    // Headless, another application is not started: the launch is reported.
    machine_set_launch_handler({ pathPtr, docsPtr, ndocs, _ in
        guard let pathPtr else { return false }
        let docs = (0..<Int(ndocs)).compactMap { i in docsPtr?[i].map { String(cString: $0) } }
        print("launch: \(String(cString: pathPtr))" + docs.map { " \($0)" }.joined())
        return true
    }, nil)

    // Guest threads run on host threads of their own; host code they call
    // may wait for the main thread, which runs the machine here.
    #if os(macOS)
    machine_set_main_thread_pump { _ = CFRunLoopRunInMode(CFRunLoopMode.defaultMode, 0, true) }
    #endif

    let budget = maxInstructions
    machine_set_instruction_limit(machine_instruction_count() + maxInstructions)
    queueScript(keyScript, start: machine_instruction_count())
    let status = machine_run(budget)

    let count = machine_instruction_count()
    relaunchAtWantedDepth()
    if status == RUN_HALTED {
        print("stopped: \(String(cString: machine_halt_reason())) after \(count) instructions")
    } else {
        let waiting = machine_waiting_for_input() ? " waiting for input" : ""
        print("instruction limit reached\(waiting) after \(count) instructions (pc \(String(machine_pc(), radix: 16, uppercase: true)))")
    }

    if let screenshot {
        if machine_screenshot(screenshot) { print("screenshot: \(screenshot)") } else { eprint("could not write \(screenshot)\n") }
    }
    if let redrawCheck, machine_waiting_for_input() || status == RUN_LIMIT {
        redrawCheckReport(redrawCheck)
    }

    var regs = [CChar](repeating: 0, count: 1024)
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
    var missingNames: UnsafePointer<CChar>?
    let missingImports = machine_ppc_missing(&missingNames)
    if missingImports > 0, let missingNames {
        print("\nPowerPC imports missing: \(missingImports): \(String(cString: missingNames))")
    }
    exit(status == RUN_HALTED && String(cString: machine_halt_reason()).hasPrefix("ExitToShell") ? 0 : 1)
}

/// The folder that was the root of the disk an application came on, and the
/// disk's name: the import marks the folders it makes from disk images
/// (.classic-volume holds the name); a folder with a volume's Desktop
/// database is one too.  Programs from CDs look for their files at the
/// root of their own volume.
func volumeRoot(containing app: String) -> (String, String)? {
    let fm = FileManager.default
    var dir = (app as NSString).deletingLastPathComponent
    let library = defaultLibrary()
    while dir.count > 1 && dir != library {
        let marker = (dir as NSString).appendingPathComponent(".classic-volume")
        if let data = fm.contents(atPath: marker) {
            let name = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            return (dir, name.isEmpty ? (dir as NSString).lastPathComponent : name)
        }
        if ["Desktop DB", "Desktop DF"].contains(where: { fm.fileExists(atPath: (dir as NSString).appendingPathComponent($0)) }) {
            return (dir, (dir as NSString).lastPathComponent)
        }
        dir = (dir as NSString).deletingLastPathComponent
    }
    return nil
}

/// The application `run` started, when an alert asking for another depth
/// starts it again, and whether that depth is kept as its setting.
nonisolated(unsafe) var depthRelaunch: (app: String, save: Bool)?

/// After the machine stops: if the program asked for another screen depth
/// (machine_wanted_depth), keep that as its setting and start it again at
/// that depth, in this process.  Returns only if it didn't ask.
func relaunchAtWantedDepth() {
    let depth = Int(machine_wanted_depth())
    guard depth > 0, let (app, save) = depthRelaunch, let me = Bundle.main.executablePath else { return }
    let name = (app as NSString).lastPathComponent
    if save {
        var all = loadSettings(defaultLibrary())
        var s = all[settingsKey(app)] ?? AppSettings()
        s.depth = depth
        all[settingsKey(app)] = s
        saveSettings(all, library: defaultLibrary())
    }
    print("relaunch: \(name) asked for depth \(depth)\(save ? " (now its setting)" : "")")
    fflush(stdout)
    // --depth also keeps the second run from asking again.
    let args = [me] + CommandLine.arguments.dropFirst() + ["--depth", String(depth)]
    var cargs = args.map { strdup($0) } + [nil]
    execv(me, &cargs)
    cargs.forEach { free($0) }
}

/// Turn a run argument into an AppleDouble application path, ingesting
/// MacBinary and archives into the library first.
/// What `bluebox apps` lists can be given back as it was printed
/// ("Macintosh HD/Folder/App", or a path inside the library), or just as an
/// application's name, when no such file exists here.
func findListedApplication(_ spec: String) -> String? {
    let fm = FileManager.default
    if fm.fileExists(atPath: spec) { return nil }
    let disk = defaultDisk(), library = defaultLibrary()
    if spec.hasPrefix("Macintosh HD/") {
        let p = disk + "/" + spec.dropFirst("Macintosh HD/".count)
        return fm.fileExists(atPath: p) ? p : nil
    }
    let inLibrary = library + "/" + spec
    if fm.fileExists(atPath: inLibrary) { return inLibrary }
    if spec.contains("/") { return nil }
    var found: [String] = []
    for root in [library, disk] where fm.fileExists(atPath: root) {
        found += scan(root).filter { $0.isApplication && ($0.path as NSString).lastPathComponent == spec }.map(\.path)
    }
    if found.count > 1 {
        eprint("several applications named \(spec):\n")
        for f in found { eprint("  \(f)\n") }
        exit(2)
    }
    return found.first
}

func resolveApplication(_ given: String, named: String?) -> String {
    let path = findListedApplication(given) ?? given
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
    var candidates = apps.filter { $0.hasCODE || $0.hasCfrg } /* 68k, PowerPC or both */
    if let named {
        candidates = candidates.filter {
            ($0.path as NSString).lastPathComponent.range(of: named, options: .caseInsensitive) != nil
        }
    }
    if candidates.count == 1 { return candidates[0].path }
    if candidates.isEmpty {
        fail(named == nil ? "no application found in \(entry)" : "no application matching \"\(named!)\" in \(entry)")
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
            eprint("bluebox: \(file): \(error)\n")
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
    // The library, then what installers put on Macintosh HD.
    var any = false
    for (root, label) in [(library, ""), (defaultDisk(), "Macintosh HD/")] {
        guard FileManager.default.fileExists(atPath: root) else { continue }
        for a in scan(root).filter({ $0.isApplication }) {
            let rel = String(a.path.dropFirst(root.count + 1))
            if rel.hasPrefix("System Folder/") { continue }
            print("\(a.architecture.padding(8)) \(fourCCString(a.creator))  \(label)\(rel)")
            any = true
        }
    }
    if !any { print("no applications in \(library)") }
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
    if found.isEmpty { print("no themes in \(library); import a Kaleidoscope scheme archive with bluebox import") }
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

/// The redraw check: what is on the screen now, against what the program
/// draws after everything is uncovered and updated again.  Programs that
/// animate differ anyway; static screens should not.
func redrawCheckReport(_ out: String) {
    var w: Int32 = 0, h: Int32 = 0
    machine_screen_size(&w, &h)
    let n = Int(w) * Int(h)
    func grab() -> [UInt32] {
        var px = [UInt32](repeating: 0, count: n)
        px.withUnsafeMutableBytes { machine_screen_rgba(0, 0, w, h, $0.baseAddress!.assumingMemoryBound(to: UInt8.self)) }
        return px
    }
    func runOn() {
        machine_continue(5_000_000)
        _ = machine_run(5_000_000)
    }
    // Pixels that change on their own (animation, a blinking caret) over two
    // intervals left alone are not counted.
    let s0 = grab()
    runOn()
    let s1 = grab()
    runOn()
    let before = grab()
    var moving = [Bool](repeating: false, count: n)
    var nmoving = 0
    for i in 0..<n where s0[i] != s1[i] || s1[i] != before[i] {
        moving[i] = true
        nmoving += 1
    }
    machine_redraw_all()
    runOn()
    let after = grab()
    var differ = 0
    var minX = Int(w), minY = Int(h), maxX = -1, maxY = -1
    var diff = after
    for i in 0..<n {
        if before[i] != after[i] && !moving[i] {
            differ += 1
            let x = i % Int(w), y = i / Int(w)
            minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y)
            diff[i] = 0xFF0000FF // red (RGBA bytes, little-endian)
        } else {
            // Dimmed: the unchanged screen for context.
            let b = withUnsafeBytes(of: after[i]) { Array($0) }
            diff[i] = UInt32(b[0] / 3 + 170) | UInt32(b[1] / 3 + 170) << 8 | UInt32(b[2] / 3 + 170) << 16 | 0xFF00_0000
        }
    }
    let note = nmoving > 0 ? " (\(nmoving) changing on their own, not counted)" : ""
    if differ == 0 {
        print("redraw check: identical\(note)")
        return
    }
    print("redraw check: \(differ) pixels differ\(note) in \(minX),\(minY)-\(maxX + 1),\(maxY + 1); before and after in \(out)")
    diff.withUnsafeBytes { _ = machine_write_png(out, $0.baseAddress!.assumingMemoryBound(to: UInt8.self), w, h) }
    let base = (out as NSString).deletingPathExtension
    before.withUnsafeBytes { _ = machine_write_png(base + "-before.png", $0.baseAddress!.assumingMemoryBound(to: UInt8.self), w, h) }
    after.withUnsafeBytes { _ = machine_write_png(base + "-after.png", $0.baseAddress!.assumingMemoryBound(to: UInt8.self), w, h) }
}

// MARK: - Main

migrateDataFolder()
// Launch Services once added a process serial number argument.
let argv = Array(CommandLine.arguments.dropFirst()).filter { !$0.hasPrefix("-psn_") }
#if canImport(AppKit)
// Blue Box.app opened from the Finder or the Dock: the shell.
if argv.isEmpty && Bundle.main.bundleIdentifier == shellBundleIdentifier { shellCommand([]) }
#endif
guard let command = argv.first else { usage() }
let rest = Array(argv.dropFirst())
switch command {
case "run": runCommand(rest)
case "import": importCommand(rest)
case "apps": appsCommand(rest)
#if canImport(AppKit)
case "shell": shellCommand(rest)
#endif
case "set": setCommand(rest)
case "rsrc": rsrcCommand(rest)
case "themes": themesCommand(rest)
case "fonts": fontsCommand(rest)
case "bpf-helper": /* run as root by bridged AppleTalk: not for people */
    guard rest.count == 2 else { usage() }
    exit(machine_bpf_helper(rest[0], rest[1]))
#if canImport(AppKit)
case "install": installCommand(rest)
case "make-iconset": makeIconsetCommand(rest)
#endif
default: usage()
}
