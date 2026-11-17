// What the classic Finder knew about a file, kept on the host: the Finder
// information, resource fork, comment and dates in the AppleDouble "._"
// sidecar beside the data fork (ForkStore), or, for a file that came from
// a Mac volume with no sidecar, in its own extended attributes.  A folder
// has no sidecar: its Finder information and comment are the catalog's
// (Toolbox/catalog.c), which BlueBoxHost's ClassicFileOps reads.

import CCore
import Foundation
import ForkStore

/// The Finder flags (FInfo.fdFlags) Blue Box shows and changes.
public enum FinderFlags {
    public static let isAlias: UInt16 = 0x8000
    public static let isInvisible: UInt16 = 0x4000
    public static let hasBundle: UInt16 = 0x2000
    public static let nameLocked: UInt16 = 0x1000
    public static let isStationery: UInt16 = 0x0800
    public static let hasCustomIcon: UInt16 = 0x0400
    public static let hasBeenInited: UInt16 = 0x0100
    public static let labelMask: UInt16 = 0x000E
}

/// The Finder's seven labels and None, with the colours and names the
/// Labels control panel started with (as Toolbox/icons.c has them).
public struct ClassicLabel: Sendable, Hashable {
    public let index: Int
    public let name: String
    public let red: UInt16, green: UInt16, blue: UInt16

    public static let all: [ClassicLabel] = [
        ClassicLabel(index: 0, name: "None", red: 0, green: 0, blue: 0),
        ClassicLabel(index: 1, name: "Essential", red: 0xFFFF, green: 0x648A, blue: 0x028C),
        ClassicLabel(index: 2, name: "Hot", red: 0xDD6B, green: 0x08C2, blue: 0x06A2),
        ClassicLabel(index: 3, name: "In Progress", red: 0xF2D7, green: 0x0856, blue: 0x84EC),
        ClassicLabel(index: 4, name: "Cool", red: 0x0241, green: 0xAB54, blue: 0xEAFF),
        ClassicLabel(index: 5, name: "Personal", red: 0x0000, green: 0x0000, blue: 0xD400),
        ClassicLabel(index: 6, name: "Project 1", red: 0x0000, green: 0x64AF, blue: 0x11B0),
        ClassicLabel(index: 7, name: "Project 2", red: 0x5600, green: 0x2C9D, blue: 0x0524),
    ]
}

public func fourCharCode(_ s: String) -> UInt32 {
    var bytes = Array(s.utf8.prefix(4))
    while bytes.count < 4 { bytes.append(0x20) }
    return bytes.reduce(0) { ($0 << 8) | UInt32($1) }
}

/// MacRoman bytes as a String.
public func macRomanText<C: Collection>(_ bytes: C) -> String where C.Element == UInt8 {
    let a = Array(bytes)
    var out = [CChar](repeating: 0, count: a.count * 3 + 1)
    let n = macroman_to_utf8(a, a.count, &out, out.count)
    return String(decoding: out[0..<n].map { UInt8(bitPattern: $0) }, as: UTF8.self)
}

/// A String as MacRoman bytes (characters MacRoman lacks become "?").
public func macRomanBytes(_ s: String, max: Int = 255) -> [UInt8] {
    var out = [UInt8](repeating: 0, count: s.utf8.count + 1)
    let n = utf8_to_macroman(s, &out, out.count)
    return Array(out[0..<min(n, max)])
}

/// The sidecar's path for a data fork's.
public func classicSidecarPath(_ path: String) -> String {
    let url = URL(fileURLWithPath: path)
    return url.deletingLastPathComponent().appendingPathComponent("._" + url.lastPathComponent).path
}

/// Whether a name is a sidecar's ("._name"), never listed for itself.
public func isSidecarName(_ name: String) -> Bool { name.hasPrefix("._") }

/// A file's classic side: Finder information, resource fork, comment and
/// dates.  Its data fork is left where it is.
public struct ClassicMeta: Sendable, Equatable {
    public var finderInfo: [UInt8] = [UInt8](repeating: 0, count: 32)
    public var hasFinderInfo = false
    public var resourceFork: [UInt8] = []
    public var comment: [UInt8] = []
    public var name: [UInt8] = []
    public var createDate: UInt32 = 0
    public var modifyDate: UInt32 = 0

    public init() {}

    /// What the sidecar (or the file's own forks) holds; nil when a sidecar
    /// is there but not AppleDouble.
    public static func read(_ path: String) -> ClassicMeta? {
        var mf = mac_file()
        guard appledouble_read_meta(path, &mf) else { return nil }
        defer { mac_file_free(&mf) }
        return ClassicMeta(mf)
    }

    init(_ mf: mac_file) {
        finderInfo = withUnsafeBytes(of: mf.finder_info) { Array($0) }
        hasFinderInfo = mf.has_finder_info
        if let r = mf.rsrc, mf.rsrc_len > 0 { resourceFork = Array(UnsafeBufferPointer(start: r, count: mf.rsrc_len)) }
        comment = withUnsafeBytes(of: mf.comment) { Array($0.prefix(Int(mf.comment_len))) }
        name = withUnsafeBytes(of: mf.name) { Array($0.prefix { $0 != 0 }) }
        createDate = mf.create_date
        modifyDate = mf.modify_date
    }

    private func be32(_ at: Int) -> UInt32 { finderInfo[at..<at + 4].reduce(0) { $0 << 8 | UInt32($1) } }
    private mutating func put32(_ at: Int, _ v: UInt32) {
        for i in 0..<4 { finderInfo[at + i] = UInt8(truncatingIfNeeded: v >> UInt32(24 - i * 8)) }
        hasFinderInfo = true
    }

    public var type: UInt32 {
        get { be32(0) }
        set { put32(0, newValue) }
    }
    public var creator: UInt32 {
        get { be32(4) }
        set { put32(4, newValue) }
    }
    public var flags: UInt16 {
        get { UInt16(finderInfo[8]) << 8 | UInt16(finderInfo[9]) }
        set {
            finderInfo[8] = UInt8(newValue >> 8)
            finderInfo[9] = UInt8(newValue & 0xFF)
            hasFinderInfo = true
        }
    }
    /// The label's number, 0 (none) to 7.
    public var label: Int {
        get { Int((flags & FinderFlags.labelMask) >> 1) }
        set { flags = (flags & ~FinderFlags.labelMask) | (UInt16(max(0, min(7, newValue))) << 1) }
    }
    public var isAlias: Bool { flags & FinderFlags.isAlias != 0 }
    public var commentText: String {
        get { macRomanText(comment) }
        set { comment = macRomanBytes(newValue, max: 200) }
    }
    /// Whether anything here is a classic file's own (a sidecar holding
    /// nothing but zeros says nothing).
    public var isClassic: Bool { type != 0 || creator != 0 || !resourceFork.isEmpty || flags != 0 || !comment.isEmpty }

    /// The sidecar's bytes.
    public func encoded() -> [UInt8] {
        var mf = mac_file()
        withUnsafeMutableBytes(of: &mf.finder_info) { $0.copyBytes(from: finderInfo.prefix(32)) }
        mf.has_finder_info = true
        let n = min(comment.count, 200)
        withUnsafeMutableBytes(of: &mf.comment) { $0.copyBytes(from: comment.prefix(n)) }
        mf.comment_len = UInt8(n)
        withUnsafeMutableBytes(of: &mf.name) { $0.copyBytes(from: name.prefix(63)) }
        mf.create_date = createDate
        mf.modify_date = modifyDate
        var len = 0
        return resourceFork.withUnsafeBufferPointer { r -> [UInt8] in
            mf.rsrc = UnsafeMutablePointer(mutating: r.baseAddress)
            mf.rsrc_len = r.count
            guard let p = appledouble_encode(&mf, &len) else { return [] }
            defer { bb_free(p) }
            return Array(UnsafeBufferPointer(start: p, count: len))
        }
    }

    /// Written as the sidecar of the data fork at path (the data fork is
    /// not touched); its own forks, if it had them, are dropped so the
    /// sidecar is the one copy.
    public func write(sidecarOf path: String) throws {
        let bytes = encoded()
        guard !bytes.isEmpty else { throw CocoaError(.fileWriteUnknown) }
        try Data(bytes).write(to: URL(fileURLWithPath: classicSidecarPath(path)), options: .atomic)
        removeNativeForks(path)
    }
}

/// The macOS extended attributes that hold a file's own forks.
public enum NativeForks {
    public static let finderInfo = "com.apple.FinderInfo"
    public static let resourceFork = "com.apple.ResourceFork"
}

/// Drop a file's own Finder information and resource fork (macOS).
public func removeNativeForks(_ path: String) {
    #if canImport(Darwin)
    removexattr(path, NativeForks.finderInfo, XATTR_NOFOLLOW)
    removexattr(path, NativeForks.resourceFork, XATTR_NOFOLLOW)
    #endif
}

/// Give a file its classic side as its own forks, the form the host keeps
/// them in (macOS: extended attributes, which it writes as an AppleDouble
/// "._" file itself on volumes without them, and carries in zip archives).
/// False where the host has no forks (Linux).
@discardableResult
public func writeNativeForks(_ meta: ClassicMeta, to path: String) -> Bool {
    #if canImport(Darwin)
    var ok = true
    if meta.hasFinderInfo && meta.finderInfo.contains(where: { $0 != 0 }) {
        // The Finder's own information never carries a desktop position
        // or the "inited" flag across volumes.
        var fi = meta.finderInfo
        fi[8] &= ~UInt8(FinderFlags.hasBeenInited >> 8)
        for i in 10..<16 { fi[i] = 0 }
        ok = fi.withUnsafeBytes { setxattr(path, NativeForks.finderInfo, $0.baseAddress, 32, 0, XATTR_NOFOLLOW) } == 0
    }
    if !meta.resourceFork.isEmpty {
        ok = meta.resourceFork.withUnsafeBytes {
            setxattr(path, NativeForks.resourceFork, $0.baseAddress, $0.count, 0, XATTR_NOFOLLOW)
        } == 0 && ok
    }
    return ok
    #else
    return false
    #endif
}

/// Whether the file at path has a classic side: a sidecar, or forks of
/// its own (macOS).
public func hasClassicSide(_ path: String) -> Bool {
    FileManager.default.fileExists(atPath: classicSidecarPath(path)) || appledouble_has_native(path)
}

/// A folder's entries as they are on disk ("._" sidecars too, which
/// FileManager leaves out of its listings), without "." and "..".
public func directoryEntries(_ path: String) -> [(name: String, isFolder: Bool)] {
    guard let d = opendir(path) else { return [] }
    defer { closedir(d) }
    var out: [(name: String, isFolder: Bool)] = []
    while let e = readdir(d) {
        // In place, up to its NUL: the entry is only as long as its name.
        let name = withUnsafePointer(to: &e.pointee.d_name) {
            $0.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) }
        }
        if name == "." || name == ".." { continue }
        var folder = e.pointee.d_type == UInt8(DT_DIR)
        if e.pointee.d_type == UInt8(DT_UNKNOWN) {
            var st = stat()
            folder = stat(path + "/" + name, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR
        }
        out.append((name, folder))
    }
    return out
}

/// Every file under root (sidecars too), as paths relative to it; folders
/// whose names start with "." are left out, and descend says which other
/// folders to go into.
public func walkFiles(_ root: String, descend: (String) -> Bool = { _ in true }) -> [String] {
    var out: [String] = []
    func walk(_ rel: String) {
        for e in directoryEntries(rel.isEmpty ? root : root + "/" + rel) {
            let child = rel.isEmpty ? e.name : rel + "/" + e.name
            if e.isFolder {
                if !e.name.hasPrefix("."), descend(child) { walk(child) }
            } else {
                out.append(child)
            }
        }
    }
    walk("")
    return out
}
