import Foundation
import ForkStore
#if canImport(SystemConfiguration)
import SystemConfiguration
#endif

/// Whether a host interface is Wi-Fi, which bridged EtherTalk can't use:
/// access points drop its frames.
func isWireless(_ name: String) -> Bool {
    #if canImport(SystemConfiguration)
    let all = SCNetworkInterfaceCopyAll() as? [SCNetworkInterface] ?? []
    return all.contains {
        SCNetworkInterfaceGetBSDName($0) as String? == name
            && SCNetworkInterfaceGetInterfaceType($0) as String? == kSCNetworkInterfaceTypeIEEE80211 as String
    }
    #else
    return false
    #endif
}

/// Per-application settings kept in the library (Settings.json), keyed by
/// the application's file name.  `bluebox run` applies them unless the
/// command line says otherwise; installed bundles run through it too.
/// The cpu setting that means a fat application runs its PowerPC code (as it
/// does without one; 68020 or 68040 runs its 68k code).
let powerPCSetting = 750

struct AppSettings: Codable {
    var depth: Int?
    var cpu: Int?
    var scale: Double? /* host points per Mac pixel */
    var appearance: String? /* bw, native, or a theme's name */
    var highlight: String? /* host (the host's selection colour) or #RRGGBB */
    var appletalk: String? /* local, lan, lan:IF (an interface, e.g. en0), bridge:IF (EtherTalk) or off */
    var modemPort: String? /* what the modem port connects to (machine_set_serial's specs) */
    var printerPort: String?
    var printFolder: String? /* printed pages saved there as PDF files, without the print panel ("": the panel) */
    var appleShare: String? /* an AppleShare volume to mount (machine_mount_appleshare's spec; "": none) */
    var display: String? /* rootless, or screen (the whole Mac screen in one window); chosen by itself when unset */
    var isEmpty: Bool {
        depth == nil && cpu == nil && scale == nil && appearance == nil && highlight == nil && appletalk == nil
            && modemPort == nil && printerPort == nil && printFolder == nil && appleShare == nil && display == nil
    }
    var summary: String {
        "depth \(depth.map(String.init) ?? "default (32)"), "
            + "cpu \(cpu.map { $0 == powerPCSetting ? "ppc" : String($0) } ?? "default (ppc if fat, else 68040)"), "
            + "scale \(scale.map { $0 == 1.5 ? "1.5" : String(Int($0)) } ?? "default"), "
            + "appearance \(appearance ?? "default (bw)"), highlight \(highlight ?? "default (host)"), "
            + "appletalk \(appletalk ?? "default (local)"), modem-port \(modemPort ?? "default (modem)"), "
            + "printer-port \(printerPort ?? "default (none)"), "
            + "print-folder \(printFolder.map { $0.isEmpty ? "panel" : $0 } ?? "default (the print panel)"), "
            + "appleshare \(appleShare.map { $0.isEmpty ? "none" : $0 } ?? "default (none)"), "
            + "display \(display ?? "default (rootless, the whole screen when the program wants it)")"
    }

    /// These settings, with any left unset taken from `fallback`.
    func over(_ fallback: AppSettings?) -> AppSettings {
        guard let f = fallback else { return self }
        return AppSettings(depth: depth ?? f.depth, cpu: cpu ?? f.cpu, scale: scale ?? f.scale,
                           appearance: appearance ?? f.appearance, highlight: highlight ?? f.highlight,
                           appletalk: appletalk ?? f.appletalk, modemPort: modemPort ?? f.modemPort,
                           printerPort: printerPort ?? f.printerPort, printFolder: printFolder ?? f.printFolder,
                           appleShare: appleShare ?? f.appleShare, display: display ?? f.display)
    }
}

/// Depths a program will run at, and the one to give it when the chosen
/// depth is not among them.
struct DepthRequirement {
    let allowed: [Int]
    let preferred: Int
}

/// Programs, by creator, that refuse to run at depths they were not written
/// for ("Please set your monitor to 256 colors") and quit: a real Mac's
/// user switched the Monitors control panel first.  Applied only when the
/// application has no depth setting of its own and the default is not one
/// it accepts.
let knownDepthRequirements: [UInt32: DepthRequirement] = [
    0x506F_C450: DepthRequirement(allowed: [1, 2, 4, 8], preferred: 8), /* PoƒP Prince of Persia */
    0x7344_3039: DepthRequirement(allowed: [4], preferred: 4),          /* sD09 Glypha II */
    0x474C_6564: DepthRequirement(allowed: [1, 4], preferred: 4),       /* GLed Glider 4 Room Editor */
    0x5370_6532: DepthRequirement(allowed: [1, 4, 8], preferred: 8),    /* Spe2 Spectre 1.1 */
    0x5370_6563: DepthRequirement(allowed: [1, 4, 8], preferred: 8),    /* Spec Spectre 1.0 */
    0x534D_554E: DepthRequirement(allowed: [1, 2, 4, 8], preferred: 8), /* SMUN Super Munchers */
    0xA7BF_C2A2: DepthRequirement(allowed: [8], preferred: 8),          /* ßø¬¢ Solarian II */
    0x4449_4132: DepthRequirement(allowed: [8], preferred: 8),          /* DIA2 Diamonds */
    0x264D_696C: DepthRequirement(allowed: [8], preferred: 8),          /* &Mil Mille Bornes */
    0x4B49_5249: DepthRequirement(allowed: [8], preferred: 8),          /* KIRI S.C. Out */
    0x5375_B972: DepthRequirement(allowed: [8], preferred: 8),          /* Suπr SS II */
    0x5257_B93D: DepthRequirement(allowed: [8], preferred: 8),          /* RWπ= Slider */
    0xC66E_4CBA: DepthRequirement(allowed: [8], preferred: 8),          /* ∆nL∫ Space Gypsy */
    0x6368_5453: DepthRequirement(allowed: [8], preferred: 8),          /* chTS Star Flick */
    0x4D4B_444E: DepthRequirement(allowed: [8], preferred: 8),          /* MKDN SuperColumns */
    /* SAIL Sail Away: depth × 3600 in 16-bit arithmetic overflows (TRAPV) above 8 bits */
    0x5341_494C: DepthRequirement(allowed: [1, 2, 4, 8], preferred: 8),
    /* ScJs Space Joust: depth × screen width in 16-bit arithmetic wraps at 32 bits on a
     * 1024-wide screen, and its offscreen bitmap then points at low memory */
    0x5363_4A73: DepthRequirement(allowed: [1, 2, 4, 8, 16], preferred: 8),
]

/// The depth a program with this creator needs when `depth` (nil: the
/// built-in 32) is not one it runs at, or nil.
func requiredDepth(creator: UInt32, over depth: Int?) -> Int? {
    guard let r = knownDepthRequirements[creator], !r.allowed.contains(depth ?? 32) else { return nil }
    return r.preferred
}

func requiredDepth(application path: String, over depth: Int?) -> Int? {
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    guard appledouble_read(path, &mf, &err, err.count) else { return nil }
    let creator = mac_file_creator(&mf)
    mac_file_free(&mf)
    return requiredDepth(creator: creator, over: depth)
}

/// An RGB highlight colour from "#RRGGBB" as 16-bit components.
func parseHighlight(_ s: String) -> (UInt16, UInt16, UInt16)? {
    guard s.hasPrefix("#"), s.count == 7, let v = UInt32(s.dropFirst(), radix: 16) else { return nil }
    func c(_ shift: UInt32) -> UInt16 { UInt16((v >> shift) & 0xFF) * 0x101 }
    return (c(16), c(8), c(0))
}

/// The settings every application gets unless its own say otherwise, kept
/// under this key (bluebox set --default ...).
let defaultSettingsKey = "*"

func defaultSettings() -> AppSettings? { loadSettings(defaultLibrary())[defaultSettingsKey] }

func settingsPath(_ library: String) -> String {
    (library as NSString).appendingPathComponent("Settings.json")
}

func loadSettings(_ library: String) -> [String: AppSettings] {
    guard let data = FileManager.default.contents(atPath: settingsPath(library)),
          let all = try? JSONDecoder().decode([String: AppSettings].self, from: data) else { return [:] }
    return all
}

func saveSettings(_ all: [String: AppSettings], library: String) {
    let enc = JSONEncoder()
    enc.outputFormatting = [.prettyPrinted, .sortedKeys]
    guard let data = try? enc.encode(all) else { fail("could not encode settings") }
    try? FileManager.default.createDirectory(atPath: library, withIntermediateDirectories: true)
    guard FileManager.default.createFile(atPath: settingsPath(library), contents: data) else {
        fail("could not write \(settingsPath(library))")
    }
}

/// The settings key for an application given by path or by name.
func settingsKey(_ app: String) -> String { (app as NSString).lastPathComponent }

func settings(for app: String) -> AppSettings? { loadSettings(defaultLibrary())[settingsKey(app)] }

/// bluebox set <app>|--default [depth 1|2|4|8|16|32|default] [cpu 68020|68040|ppc|default]
///     [scale 1|1.5|2|default] [appearance bw|native|<theme>|default] [highlight host|#RRGGBB|default]
///     [appletalk local|lan|lan:IF|bridge:IF|off|default] [modem-port <spec>|default] [printer-port <spec>|default]
///     [print-folder <folder>|panel|default] [appleshare <spec>|none|default] [display rootless|screen|default]
func setCommand(_ args: [String]) {
    var rest = args
    var library = defaultLibrary()
    if let i = rest.firstIndex(of: "--library"), i + 1 < rest.count {
        library = rest[i + 1]
        rest.removeSubrange(i...i + 1)
    }
    guard let app = rest.first else { usage() }
    let key = app == "--default" ? defaultSettingsKey : settingsKey(app)
    var all = loadSettings(library)
    var s = all[key] ?? AppSettings()
    var pairs = Array(rest.dropFirst())
    if pairs.isEmpty {
        print("\(key): \(s.summary)")
        return
    }
    while pairs.count >= 2 {
        let (name, value) = (pairs[0], pairs[1])
        pairs.removeFirst(2)
        switch name {
        case "depth":
            if value == "default" {
                s.depth = nil
            } else if let d = Int(value), [1, 2, 4, 8, 16, 32].contains(d) {
                s.depth = d
            } else {
                fail("depth expects 1, 2, 4, 8, 16, 32 or default")
            }
        case "cpu":
            if value == "default" {
                s.cpu = nil
            } else if value == "ppc" || value == "powerpc" {
                s.cpu = powerPCSetting /* a fat application runs its PowerPC code */
            } else if let m = Int(value), [68020, 68040].contains(m) {
                s.cpu = m
            } else {
                fail("cpu expects 68020, 68040, ppc or default")
            }
        case "scale":
            if value == "default" {
                s.scale = nil
            } else if let v = Double(value), [1, 1.5, 2].contains(v) {
                s.scale = v
            } else {
                fail("scale expects 1, 1.5, 2 or default")
            }
        case "appearance":
            s.appearance = value == "default" ? nil : value
        case "highlight":
            if value == "default" {
                s.highlight = nil
            } else if value == "host" || parseHighlight(value) != nil {
                s.highlight = value
            } else {
                fail("highlight expects host, #RRGGBB or default")
            }
        case "appletalk":
            if value == "default" {
                s.appletalk = nil
            } else if value.hasPrefix("bridge:") && isWireless(String(value.dropFirst(7))) {
                fail("appletalk: \(value.dropFirst(7)) is Wi-Fi, which drops EtherTalk; bridge onto a wired interface")
            } else if ["local", "lan", "off"].contains(value) || (value.hasPrefix("lan:") && value.count > 4)
                        || (value.hasPrefix("bridge:") && value.count > 7) {
                s.appletalk = value
            } else {
                fail("appletalk expects local, lan, lan:IF (e.g. lan:en0), bridge:IF, off or default")
            }
        case "modem-port", "printer-port":
            let spec: String? = value == "default" ? nil : value
            if let spec, !validSerialSpec(spec) {
                fail("\(name) expects modem, modem:PORT, tcp:HOST:PORT, listen:PORT, pty, a device, none or default")
            }
            if name == "modem-port" { s.modemPort = spec } else { s.printerPort = spec }
        case "print-folder":
            if value == "default" {
                s.printFolder = nil
            } else if value == "panel" {
                s.printFolder = ""
            } else {
                var isDir: ObjCBool = false
                let path = (value as NSString).expandingTildeInPath
                guard FileManager.default.fileExists(atPath: path, isDirectory: &isDir), isDir.boolValue else {
                    fail("print-folder expects an existing folder, panel or default")
                }
                s.printFolder = (path as NSString).standardizingPath
            }
        case "appleshare":
            if value == "default" {
                s.appleShare = nil
            } else if value == "none" {
                s.appleShare = ""
            } else if value.hasPrefix("tcp:") || value.hasPrefix("atalk:"), value.contains("/") {
                s.appleShare = value
            } else {
                fail("appleshare expects tcp:[user[:pass]@]host/Volume, atalk:[user[:pass]@]Server:AFPServer[@zone]/Volume, none or default")
            }
        case "display":
            guard ["rootless", "screen", "default"].contains(value) else { fail("display expects rootless, screen or default") }
            s.display = value == "default" ? nil : value
        default:
            fail("unknown setting \(name) (settings: depth, cpu, scale, appearance, highlight, appletalk, "
                 + "modem-port, printer-port, print-folder, appleshare, display)")
        }
    }
    if !pairs.isEmpty { usage() }
    all[key] = s.isEmpty ? nil : s
    saveSettings(all, library: library)
    print("\(key): \(s.summary)")
}

/// Whether a serial port spec is one machine_set_serial takes.
func validSerialSpec(_ s: String) -> Bool {
    let num = { (t: Substring) in Int(t).map { $0 > 0 && $0 < 65536 } ?? false }
    if ["modem", "pty", "none"].contains(s) || s.hasPrefix("/") { return true }
    if s.hasPrefix("modem:") { return num(s.dropFirst(6)) }
    if s.hasPrefix("listen:") { return num(s.dropFirst(7)) }
    if s.hasPrefix("tcp:") {
        let rest = s.dropFirst(4)
        guard let c = rest.lastIndex(of: ":") else { return false }
        return !rest[..<c].isEmpty && num(rest[rest.index(after: c)...])
    }
    return false
}
