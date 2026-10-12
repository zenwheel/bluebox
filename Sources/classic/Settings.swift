import Foundation

/// Per-application settings kept in the library (Settings.json), keyed by
/// the application's file name.  `classic run` applies them unless the
/// command line says otherwise; installed bundles run through it too.
struct AppSettings: Codable {
    var depth: Int?
    var cpu: Int?
    var isEmpty: Bool { depth == nil && cpu == nil }
    var summary: String {
        "depth \(depth.map(String.init) ?? "default (32)"), cpu \(cpu.map(String.init) ?? "default (68040)")"
    }
}

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

/// classic set <app> [depth 1|8|16|32|default] [cpu 68020|68040|default]
func setCommand(_ args: [String]) {
    var rest = args
    var library = defaultLibrary()
    if let i = rest.firstIndex(of: "--library"), i + 1 < rest.count {
        library = rest[i + 1]
        rest.removeSubrange(i...i + 1)
    }
    guard let app = rest.first else { usage() }
    let key = settingsKey(app)
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
            } else if let d = Int(value), [1, 8, 16, 32].contains(d) {
                s.depth = d
            } else {
                fail("depth expects 1, 8, 16, 32 or default")
            }
        case "cpu":
            if value == "default" {
                s.cpu = nil
            } else if let m = Int(value), [68020, 68040].contains(m) {
                s.cpu = m
            } else {
                fail("cpu expects 68020, 68040 or default")
            }
        default:
            fail("unknown setting \(name) (settings: depth, cpu)")
        }
    }
    if !pairs.isEmpty { usage() }
    all[key] = s.isEmpty ? nil : s
    saveSettings(all, library: library)
    print("\(key): \(s.summary)")
}
