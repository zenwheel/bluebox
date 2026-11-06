// The After Dark modules the screen saver can show: found in Blue Box's
// library and folders the user adds, with the controls each describes in
// its resources (afterdark.c has the resource formats).

#if canImport(ScreenSaver)
import CCore
import Foundation
import ForkStore
import Toolbox

/// The user's own home folder: the screen saver runs sandboxed, where
/// NSHomeDirectory() is its container, but may read the whole disk.
func realHome() -> String {
    if let pw = getpwuid(getuid()), let dir = pw.pointee.pw_dir { return String(cString: dir) }
    return NSHomeDirectory()
}

/// Blue Box's library and its Mac volume, where imported modules are.
func defaultModuleFolders() -> [String] {
    let base = realHome() + "/Library/Application Support/Blue Box"
    return [base + "/Library", base + "/Macintosh HD"]
}

/// A control in After Dark's panel for the module: 'sVal' slider, 'mVal'
/// pop-up menu, 'xVal' check box, 'tVal' text, 'bVal' button.
struct ModuleControl {
    enum Kind { case slider, menu, checkbox, text, button }
    var kind: Kind
    var title: String
    var value: Int
    var type = ""                     /* the resource holding it ('sVal' ...) and its ID */
    var id = 0
    var units: [(Int, String)] = []   /* sUnt: from this value up, this label */
    var items: [String] = []          /* the MENU's items, or the STR# for text */

    /// The label a slider's units give its value.
    func unitLabel(_ v: Int) -> String {
        var label = "\(v)"
        for (from, s) in units where v >= from { label = s }
        return label
    }
}

struct ModuleInfo {
    let path: String
    let name: String
    var credit = ""
    var controls: [ModuleControl?] = [nil, nil, nil, nil]
    var isPowerPC = false
}

private func macRoman(_ p: UnsafePointer<UInt8>, _ n: Int) -> String {
    String(data: Data(bytes: p, count: max(0, n)), encoding: .macOSRoman) ?? ""
}

private func be16(_ d: UnsafePointer<UInt8>, _ o: Int) -> Int { Int(Int16(bitPattern: UInt16(d[o]) << 8 | UInt16(d[o + 1]))) }

private func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

/// A file's type from its AppleDouble sidecar (or its own Finder info):
/// read without its resource fork.
func fileType(_ path: String) -> UInt32? {
    var info = [UInt8](repeating: 0, count: 32)
    if appledouble_native_finder_info(path, &info) {
        return UInt32(info[0]) << 24 | UInt32(info[1]) << 16 | UInt32(info[2]) << 8 | UInt32(info[3])
    }
    let dir = (path as NSString).deletingLastPathComponent, base = (path as NSString).lastPathComponent
    guard let h = FileHandle(forReadingAtPath: dir + "/._" + base) else { return nil }
    defer { try? h.close() }
    guard let head = try? h.read(upToCount: 4096), head.count >= 26 else { return nil }
    let b = [UInt8](head)
    let n = Int(b[24]) << 8 | Int(b[25])
    for i in 0..<n {
        let e = 26 + i * 12
        guard e + 12 <= b.count else { break }
        let id = UInt32(b[e]) << 24 | UInt32(b[e + 1]) << 16 | UInt32(b[e + 2]) << 8 | UInt32(b[e + 3])
        let off = Int(b[e + 4]) << 24 | Int(b[e + 5]) << 16 | Int(b[e + 6]) << 8 | Int(b[e + 7])
        if id == 9, off + 4 <= b.count {
            return UInt32(b[off]) << 24 | UInt32(b[off + 1]) << 16 | UInt32(b[off + 2]) << 8 | UInt32(b[off + 3])
        }
    }
    return nil
}

/// A file there, or only its AppleDouble sidecar (an empty data fork).
func moduleExists(_ path: String) -> Bool {
    let fm = FileManager.default
    let dir = (path as NSString).deletingLastPathComponent, base = (path as NSString).lastPathComponent
    return fm.fileExists(atPath: path) || fm.fileExists(atPath: dir + "/._" + base)
}

enum ModuleCatalog {
    static let moduleTypes: Set<UInt32> = [fourCC("ADgm"), fourCC("A4gm")]

    /// A resource type's code ('sVal' ...).
    static func typeCode(_ s: String) -> UInt32 { fourCC(s) }

    /// Every module in the folders (and below them), by name.
    static func scan(_ folders: [String], limit: Int = 4000) -> [String] {
        var found: [String] = []
        let fm = FileManager.default
        for folder in folders {
            guard let e = fm.enumerator(atPath: folder) else { continue }
            var seen = 0
            while let rel = e.nextObject() as? String {
                seen += 1
                if seen > 200_000 || found.count >= limit { break }
                var name = (rel as NSString).lastPathComponent
                var path = folder + "/" + rel
                // A sidecar whose data file isn't there (an empty data
                // fork, as the archive keeps them) stands for the file.
                if name.hasPrefix("._") {
                    name = String(name.dropFirst(2))
                    path = (path as NSString).deletingLastPathComponent + "/" + name
                    if fm.fileExists(atPath: path) { continue }
                } else if name.hasPrefix(".") {
                    continue
                }
                if let t = fileType(path), moduleTypes.contains(t) { found.append(path) }
            }
        }
        return found.sorted { ($0 as NSString).lastPathComponent.localizedStandardCompare(($1 as NSString).lastPathComponent) == .orderedAscending }
    }

    /// The module's name, credit line and controls, from its resources.
    /// `config` is the systemConfig the controls' 'µVal' are checked
    /// against (afterdark.c's, for the screen it will run on).
    static func info(_ path: String, config: Int) -> ModuleInfo {
        var info = ModuleInfo(path: path, name: (path as NSString).lastPathComponent)
        var err = [CChar](repeating: 0, count: 256)
        // Its resources with the settings kept for it over them.
        var forkLen = 0
        guard let fork = machine_screensaver_module_fork(path, &forkLen), forkLen > 0 else { return info }
        defer { bb_free(fork) }
        var idx = rsrc_index()
        guard rsrc_parse(fork, forkLen, &idx, &err, err.count) else { return info }
        defer { rsrc_index_free(&idx) }
        func find(_ type: String, _ id: Int) -> (UnsafePointer<UInt8>, Int, String)? {
            let t = fourCC(type)
            for i in 0..<idx.count {
                var e = idx.entries[i]
                if e.type == t && Int(e.id) == id, let d = rsrc_data(fork, &idx, &e) {
                    let name = withUnsafeBytes(of: &e.name) { raw -> String in
                        let bytes = raw.prefix { $0 != 0 }
                        return String(data: Data(bytes), encoding: .macOSRoman) ?? ""
                    }
                    return (d, Int(e.data_len), name)
                }
            }
            return nil
        }
        if let (d, n, _) = find("STR ", 128), n > 0 { info.credit = macRoman(d + 1, min(Int(d[0]), n - 1)) }
        if let (d, n, _) = find("ADgm", 0), n >= 2 { info.isPowerPC = d[0] == 0xAA && d[1] == 0xFE }
        let muType: UInt32 = 0xB500_0000 | fourCC("Val") // 'µVal': µ is $B5 in MacRoman
        for slot in 0..<4 {
            var chosen: ModuleControl?
            for alt in 0..<9 {
                let id = 1000 + slot + alt * 1000
                var control: ModuleControl?
                for (type, kind) in [("sVal", ModuleControl.Kind.slider), ("mVal", .menu), ("xVal", .checkbox),
                                     ("tVal", .text), ("bVal", .button)] {
                    guard let (d, n, name) = find(type, id), n >= 2 else { continue }
                    var c = ModuleControl(kind: kind, title: name, value: be16(d, 0), type: type, id: id)
                    if kind == .slider, let (u, un, _) = find("sUnt", id), un >= 2 {
                        var o = 2
                        for _ in 0..<max(0, be16(u, 0)) where o + 3 <= un {
                            let from = be16(u, o), len = Int(u[o + 2])
                            guard o + 3 + len <= un else { break }
                            c.units.append((from, macRoman(u + o + 3, len)))
                            o += 3 + len
                        }
                    }
                    if kind == .menu, let (m, mn, _) = find("MENU", id), mn > 14 {
                        var o = 14 + Int(m[14]) + 1 // after the header and the title
                        while o < mn, m[o] != 0 {
                            let len = Int(m[o])
                            guard o + 1 + len + 4 <= mn else { break }
                            c.items.append(macRoman(m + o + 1, len))
                            o += 1 + len + 4
                        }
                    }
                    if kind == .text, let (s, sn, _) = find("STR#", id), sn >= 2 {
                        var o = 2
                        for _ in 0..<max(0, be16(s, 0)) where o < sn {
                            let len = Int(s[o])
                            guard o + 1 + len <= sn else { break }
                            c.items.append(macRoman(s + o + 1, len))
                            o += 1 + len
                        }
                    }
                    control = c
                    break
                }
                guard let control else { continue }
                chosen = control
                // Its 'µVal': the configuration bits it needs, else the next alternative.
                var need: Int?
                for i in 0..<idx.count {
                    var e = idx.entries[i]
                    if e.type == muType && Int(e.id) == id, e.data_len >= 2, let d = rsrc_data(fork, &idx, &e) {
                        need = Int(UInt16(d[0]) << 8 | UInt16(d[1]))
                    }
                }
                if let need, need & config != need { continue }
                break
            }
            info.controls[slot] = chosen
        }
        return info
    }

    /// systemConfig as afterdark.c gives it for a screen of this depth
    /// (Color QuickDraw, dimming, the extensions; sound if on).
    static func config(depth: Int, sound: Bool) -> Int {
        var c = 1 | 1 << 7 | 1 << 8 | 1 << 14
        if depth > 1 { c |= 1 << 1 | 1 << 2 | 1 << 3 | 1 << 4 }
        if depth > 1 && depth <= 8 { c |= 1 << 5 | 1 << 6 }
        if sound { c |= 1 << 15 }
        return c
    }
}
#endif
