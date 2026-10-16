import Foundation

/// Per-application settings kept in the library (Settings.json), keyed by
/// the application's file name.  `classic run` applies them unless the
/// command line says otherwise; installed bundles run through it too.
struct AppSettings: Codable {
    var depth: Int?
    var cpu: Int?
    var scale: Double? /* host points per Mac pixel */
    var appearance: String? /* bw, native, or a theme's name */
    var highlight: String? /* host (the host's selection colour) or #RRGGBB */
    var isEmpty: Bool { depth == nil && cpu == nil && scale == nil && appearance == nil && highlight == nil }
    var summary: String {
        "depth \(depth.map(String.init) ?? "default (32)"), cpu \(cpu.map(String.init) ?? "default (68040)"), "
            + "scale \(scale.map { $0 == 1.5 ? "1.5" : String(Int($0)) } ?? "default"), "
            + "appearance \(appearance ?? "default (bw)"), highlight \(highlight ?? "default (host)")"
    }

    /// These settings, with any left unset taken from `fallback`.
    func over(_ fallback: AppSettings?) -> AppSettings {
        guard let f = fallback else { return self }
        return AppSettings(depth: depth ?? f.depth, cpu: cpu ?? f.cpu, scale: scale ?? f.scale,
                           appearance: appearance ?? f.appearance, highlight: highlight ?? f.highlight)
    }
}

/// An RGB highlight colour from "#RRGGBB" as 16-bit components.
func parseHighlight(_ s: String) -> (UInt16, UInt16, UInt16)? {
    guard s.hasPrefix("#"), s.count == 7, let v = UInt32(s.dropFirst(), radix: 16) else { return nil }
    func c(_ shift: UInt32) -> UInt16 { UInt16((v >> shift) & 0xFF) * 0x101 }
    return (c(16), c(8), c(0))
}

/// The settings every application gets unless its own say otherwise, kept
/// under this key (classic set --default ...).
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

/// classic set <app>|--default [depth 1|2|4|8|16|32|default] [cpu 68020|68040|default]
///     [scale 1|1.5|2|default] [appearance bw|native|<theme>|default] [highlight host|#RRGGBB|default]
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
            } else if let m = Int(value), [68020, 68040].contains(m) {
                s.cpu = m
            } else {
                fail("cpu expects 68020, 68040 or default")
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
        default:
            fail("unknown setting \(name) (settings: depth, cpu, scale, appearance, highlight)")
        }
    }
    if !pairs.isEmpty { usage() }
    all[key] = s.isEmpty ? nil : s
    saveSettings(all, library: library)
    print("\(key): \(s.summary)")
}
