// Ingesting classic software: MacBinary (.bin), StuffIt (.sit, .sea), Compact Pro
// (.cpt) and BinHex (.hqx) become AppleDouble files in the library.
//
// MacBinary is decoded natively.  Archives are extracted with The Unarchiver's
// `unar`, which writes resource forks as AppleDouble "._" sidecars, the format
// the library uses.  Archives nested inside archives are extracted too.

import CCore
import Foundation
import ForkStore

struct IngestedFile {
    var path: String
    var type: UInt32
    var creator: UInt32
    var dataSize: Int
    var rsrcSize: Int
    var hasCODE: Bool
    var hasCfrg: Bool

    var isApplication: Bool { type == fourCCValue("APPL") }
    var architecture: String {
        switch (hasCODE, hasCfrg) {
        case (true, true): return "68k+PPC"
        case (false, true): return "PPC"
        case (true, false): return "68k"
        default: return "?"
        }
    }
}

enum IngestError: Error, CustomStringConvertible {
    case unreadable(String)
    case unarMissing
    case unarFailed(String, String)
    case notAnArchive(String)
    case diskImage(String, String)

    var description: String {
        switch self {
        case .unreadable(let p): return "cannot read \(p)"
        case .unarMissing:
            return "extracting archives needs unar (The Unarchiver): brew install unar, or apt install unar"
        case .unarFailed(let p, let msg): return "unar could not extract \(p): \(msg)"
        case .notAnArchive(let p): return "\(p) is not MacBinary, an archive, a disk image, or an AppleDouble file"
        case .diskImage(let p, let msg): return "\((p as NSString).lastPathComponent): \(msg)"
        }
    }
}

func fourCCValue(_ s: String) -> UInt32 {
    Array(s.utf8).reduce(0) { ($0 << 8) | UInt32($1) }
}

/// Where ingested software goes unless the caller says otherwise.
/// Blue Box was called Classic: its folder (Application Support/Classic,
/// or ~/.local/share/classic) moves to the new name the first time, with
/// everything in it, if the new one is not there yet.
func migrateDataFolder() {
    let env = ProcessInfo.processInfo.environment
    let home = env["HOME"] ?? FileManager.default.homeDirectoryForCurrentUser.path
    #if os(macOS)
    let old = home + "/Library/Application Support/Classic", new = home + "/Library/Application Support/Blue Box"
    #else
    let data = env["XDG_DATA_HOME"].flatMap { $0.isEmpty ? nil : $0 } ?? home + "/.local/share"
    let old = data + "/classic", new = data + "/bluebox"
    #endif
    let fm = FileManager.default
    if fm.fileExists(atPath: old) && !fm.fileExists(atPath: new) {
        try? fm.moveItem(atPath: old, toPath: new)
    }
}

func defaultLibrary() -> String {
    let env = ProcessInfo.processInfo.environment
    if let lib = env["BLUEBOX_LIBRARY"], !lib.isEmpty { return lib }
    migrateDataFolder()
    let home = env["HOME"] ?? FileManager.default.homeDirectoryForCurrentUser.path
    #if os(macOS)
    return home + "/Library/Application Support/Blue Box/Library"
    #else
    let data = env["XDG_DATA_HOME"].flatMap { $0.isEmpty ? nil : $0 } ?? home + "/.local/share"
    return data + "/bluebox/library"
    #endif
}

/// The folder that is the classic Mac's disk (Macintosh HD), where
/// installers put what they install (see filemgr.c).
func defaultDisk() -> String {
    let env = ProcessInfo.processInfo.environment
    let home = env["HOME"] ?? FileManager.default.homeDirectoryForCurrentUser.path
    #if os(macOS)
    return home + "/Library/Application Support/Blue Box/Macintosh HD"
    #else
    let data = env["XDG_DATA_HOME"].flatMap { $0.isEmpty ? nil : $0 } ?? home + "/.local/share"
    return data + "/bluebox/Macintosh HD"
    #endif
}

// MARK: - Recognising inputs

enum InputKind {
    case macBinary
    case archive
    case diskImage    // an HFS disk image (DiskCopy 4.2, Disk Copy 6 NDIF, raw, CD-ROM)
    case appleDouble  // a data file with a "._" sidecar
    case plain
}

private func readPrefix(_ path: String, _ n: Int = 2048) -> [UInt8] {
    guard let h = FileHandle(forReadingAtPath: path) else { return [] }
    defer { try? h.close() }
    return Array((try? h.read(upToCount: n)) ?? Data())
}

private let stuffItSignatures: Set<String> = ["SIT!", "ST46", "ST50", "ST60", "ST65", "STin", "STi2", "STi3", "STi4"]
private let archiveTypes: Set<String> = ["SIT!", "SITD", "SIT5", "SITX", "PACT", "BnHq"]
private let archiveCreators: Set<String> = ["SIT!", "SITx", "CPCT", "aust", "BnHq", "ArcF"]
private let archiveExtensions: Set<String> = ["sit", "sea", "cpt", "hqx", "zip"]
private let diskTypes: Set<String> = ["dImg", "rohd", "dCpy", "ddsk", "hdsk"]
private let diskExtensions: Set<String> = ["img", "image", "dsk", "dc42", "iso", "toast", "hfs", "cdr", "disk"]

/// An HFS signature where a raw volume or a DiskCopy 4.2 image would have one.
private func looksLikeDiskData(_ b: [UInt8]) -> Bool {
    if b.count >= 1026 && b[1024] == 0x42 && b[1025] == 0x44 { return true }
    if b.count >= 1026 && b[0] == 0x45 && b[1] == 0x52 && b[512] == 0x50 && b[513] == 0x4D { return true }
    return b.count >= 84 + 1026 && b[0] <= 63 && b[82] == 0x01 && b[83] == 0x00 &&
        b[84 + 1024] == 0x42 && b[84 + 1025] == 0x44
}

/// Archive formats recognisable from the data fork alone.
private func looksLikeArchiveData(_ b: [UInt8]) -> Bool {
    if b.count >= 14 {
        let sig = String(decoding: b[0..<4], as: UTF8.self)
        if stuffItSignatures.contains(sig) && String(decoding: b[10..<14], as: UTF8.self) == "rLau" { return true }
    }
    if b.starts(with: Array("StuffIt (c)1997".utf8)) { return true }
    if b.starts(with: [0x50, 0x4B, 0x03, 0x04]) { return true } // zip
    let text = String(decoding: b, as: UTF8.self)
    return text.contains("(This file must be converted with BinHex")
}

private func isArchive(type: UInt32, creator: UInt32, name: String, data: [UInt8]) -> Bool {
    let t = fourCCString(type), c = fourCCString(creator)
    if looksLikeArchiveData(data) { return true }
    // Types alone are not enough: Word settings files, for one, are 'SITD'.
    if archiveTypes.contains(t) && archiveCreators.contains(c) { return true }
    if t == "APPL" && (c == "aust" || c == "EXTR") { return true } // self-extracting archives
    let ext = (name as NSString).pathExtension.lowercased()
    return archiveExtensions.contains(ext) || looksLikeArchiveData(data)
}

func fourCCString(_ code: UInt32) -> String {
    var s = ""
    for shift in stride(from: 24, through: 0, by: -8) {
        let b = UInt8((code >> UInt32(shift)) & 0xFF)
        s.append(b >= 0x20 && b < 0x7F ? Character(UnicodeScalar(b)) : "?")
    }
    return s
}

func sidecarPath(_ path: String) -> String {
    let url = URL(fileURLWithPath: path)
    return url.deletingLastPathComponent().appendingPathComponent("._" + url.lastPathComponent).path
}

func classify(_ path: String) -> InputKind {
    var prefix = readPrefix(path)
    let hasSidecar = FileManager.default.fileExists(atPath: sidecarPath(path))
    let ext = (path as NSString).pathExtension.lowercased()
    // The MacBinary I check is weak; a file that already has Mac forks is only
    // MacBinary if it says so in its name.
    if macbinary_detect(&prefix, prefix.count) && (!hasSidecar || ext == "bin" || ext == "macbin") {
        return .macBinary
    }
    if hasSidecar {
        // An AppleDouble file may itself be an archive (e.g. a .sit with its forks split out).
        var f = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        if appledouble_read(path, &f, &err, err.count) {
            defer { mac_file_free(&f) }
            let name = (path as NSString).lastPathComponent
            let t = fourCCString(mac_file_type(&f)), c = fourCCString(mac_file_creator(&f))
            if diskTypes.contains(t) || diskTypes.contains(c) || looksLikeDiskData(prefix) { return .diskImage }
            if isArchive(type: mac_file_type(&f), creator: mac_file_creator(&f), name: name, data: prefix) {
                return .archive
            }
        }
        return .appleDouble
    }
    if archiveExtensions.contains(ext) || looksLikeArchiveData(prefix) { return .archive }
    if isUDIF(path) { return .diskImage }
    if diskExtensions.contains(ext) && looksLikeDiskData(prefix) { return .diskImage }
    if looksLikeDiskData(prefix) { return .diskImage }
    return .plain
}

// MARK: - Extraction

private func findUnar() -> String? {
    let paths = (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":").map(String.init)
    for dir in paths + ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"] {
        let p = dir + "/unar"
        if FileManager.default.isExecutableFile(atPath: p) { return p }
    }
    return nil
}

/// Extract an archive into dest with resource forks as AppleDouble sidecars.
private func unar(_ archive: String, into dest: String) throws {
    guard let tool = findUnar() else { throw IngestError.unarMissing }
    let p = Process()
    p.executableURL = URL(fileURLWithPath: tool)
    // -k hidden: forks as "._" AppleDouble; -nq: no quarantine attributes;
    // -D: no extra containing folder (dest is already one); -f: overwrite.
    p.arguments = ["-q", "-nq", "-k", "hidden", "-D", "-f", "-o", dest, archive]
    var env = ProcessInfo.processInfo.environment
    env["COPYFILE_DISABLE"] = "1"
    p.environment = env
    let errPipe = Pipe()
    p.standardError = errPipe
    p.standardOutput = FileHandle.nullDevice
    try p.run()
    p.waitUntilExit()
    let msg = String(decoding: errPipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
    if p.terminationStatus != 0 {
        throw IngestError.unarFailed(archive, msg.trimmingCharacters(in: .whitespacesAndNewlines))
    }
}

/// Mac names may contain '/', host names may not; Mac names cannot contain ':'.
private func hostName(_ macName: String) -> String {
    macName.replacingOccurrences(of: "/", with: ":")
}

private func writeAppleDouble(_ f: inout mac_file, to path: String) throws {
    var err = [CChar](repeating: 0, count: 256)
    if !appledouble_write(path, &f, &err, err.count) {
        throw IngestError.unreadable(String(decoding: err.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self))
    }
}

/// Walk an extracted tree: give sidecar-only files an empty data file, decode
/// MacBinary files in place, and extract nested archives (bounded depth).
private func normalise(_ dir: String, depth: Int, notes: inout [String]) throws {
    let fm = FileManager.default
    guard let names = try? fm.contentsOfDirectory(atPath: dir) else { return }
    for name in names.sorted() {
        let path = dir + "/" + name
        var isDir: ObjCBool = false
        fm.fileExists(atPath: path, isDirectory: &isDir)
        if isDir.boolValue {
            try normalise(path, depth: depth, notes: &notes)
            continue
        }
        if name.hasPrefix("._") {
            let data = dir + "/" + String(name.dropFirst(2))
            if !fm.fileExists(atPath: data) { fm.createFile(atPath: data, contents: Data()) }
        }
    }
    guard let names2 = try? fm.contentsOfDirectory(atPath: dir) else { return }
    for name in names2.sorted() where !name.hasPrefix("._") && name != ".classic-source" {
        let path = dir + "/" + name
        var isDir: ObjCBool = false
        fm.fileExists(atPath: path, isDirectory: &isDir)
        if isDir.boolValue { continue }
        switch classify(path) {
        case .macBinary:
            do {
                try decodeMacBinary(path, into: dir, depth: depth, notes: &notes)
                try? fm.removeItem(atPath: path) // only an encoding wrapper
            } catch {
                notes.append("\(name): not decoded (\(error))")
            }
        case .diskImage where depth < 4:
            do {
                let vol = try extractDiskImage(path, into: dir, notes: &notes)
                notes.append("\(name): extracted volume \"\((vol as NSString).lastPathComponent)\"")
                try normalise(vol, depth: depth + 1, notes: &notes)
            } catch {
                notes.append("\(name): not extracted (\(error))")
            }
        case .archive where depth < 4:
            let sub = dir + "/" + name + " ƒ"
            if !fm.fileExists(atPath: sub) {
                try fm.createDirectory(atPath: sub, withIntermediateDirectories: true)
                do {
                    try unar(path, into: sub)
                    try normalise(sub, depth: depth + 1, notes: &notes)
                } catch {
                    try? fm.removeItem(atPath: sub)
                    notes.append("\(name): not extracted (\(error))")
                }
            }
        default:
            break
        }
    }
}

private func decodeMacBinary(_ path: String, into dir: String, depth: Int, notes: inout [String]) throws {
    guard let data = FileManager.default.contents(atPath: path) else { throw IngestError.unreadable(path) }
    var bytes = Array(data)
    var f = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    guard macbinary_parse(&bytes, bytes.count, &f, &err, err.count) else { throw IngestError.unreadable(path) }
    defer { mac_file_free(&f) }
    var name = withUnsafeBytes(of: &f.name) { raw in String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self) }
    if name.isEmpty { name = ((path as NSString).lastPathComponent as NSString).deletingPathExtension }
    let dataPrefix = Array(UnsafeBufferPointer(start: f.data, count: min(f.data_len, 1024)))
    if depth < 4 && isArchive(type: mac_file_type(&f), creator: mac_file_creator(&f), name: name, data: dataPrefix) {
        // unar understands MacBinary-wrapped archives directly.
        let sub = dir + "/" + hostName(name) + " ƒ"
        try FileManager.default.createDirectory(atPath: sub, withIntermediateDirectories: true)
        try unar(path, into: sub)
        try normalise(sub, depth: depth + 1, notes: &notes)
        return
    }
    try writeAppleDouble(&f, to: dir + "/" + hostName(name))
}

// MARK: - Library entries

/// The library folder for a source file: its name without wrapper extensions.
private func entryName(_ source: String) -> String {
    var name = (source as NSString).lastPathComponent
    let wrappers: Set<String> = ["bin", "sit", "sea", "cpt", "hqx", "macbin", "img", "image", "dsk", "dc42",
                                 "iso", "toast", "hfs", "cdr"]
    while wrappers.contains((name as NSString).pathExtension.lowercased()) {
        name = (name as NSString).deletingPathExtension
    }
    return name.isEmpty ? "Untitled" : name
}

private func sourceStamp(_ source: String) -> String {
    let attrs = (try? FileManager.default.attributesOfItem(atPath: source)) ?? [:]
    let size = (attrs[.size] as? NSNumber)?.int64Value ?? -1
    let mtime = (attrs[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
    return "\(URL(fileURLWithPath: source).standardizedFileURL.path)\n\(size)\n\(Int64(mtime))\n"
}

/// Ingest a file into the library.  Re-ingesting an unchanged source reuses
/// the existing entry.  Returns the entry folder and any notes.
func ingest(_ source: String, library: String, force: Bool = false) throws -> (entry: String, notes: [String]) {
    let fm = FileManager.default
    guard fm.isReadableFile(atPath: source) else { throw IngestError.unreadable(source) }
    let entry = library + "/" + entryName(source)
    let stampPath = entry + "/.classic-source"
    let stamp = sourceStamp(source)
    if !force, let old = fm.contents(atPath: stampPath), String(decoding: old, as: UTF8.self) == stamp {
        return (entry, ["already in the library (unchanged)"])
    }
    if fm.fileExists(atPath: entry) { try fm.removeItem(atPath: entry) }
    try fm.createDirectory(atPath: entry, withIntermediateDirectories: true)
    var notes = [String]()
    do {
        switch classify(source) {
        case .macBinary:
            try decodeMacBinary(source, into: entry, depth: 0, notes: &notes)
        case .archive:
            try unar(source, into: entry)
            try normalise(entry, depth: 1, notes: &notes)
        case .appleDouble, .diskImage:
            let name = (source as NSString).lastPathComponent
            try fm.copyItem(atPath: source, toPath: entry + "/" + name)
            if fm.fileExists(atPath: sidecarPath(source)) {
                try fm.copyItem(atPath: sidecarPath(source), toPath: entry + "/._" + name)
            }
            try normalise(entry, depth: 1, notes: &notes)
        case .plain:
            throw IngestError.notAnArchive(source)
        }
    } catch {
        try? fm.removeItem(atPath: entry)
        throw error
    }
    fm.createFile(atPath: stampPath, contents: Data(stamp.utf8))
    return (entry, notes)
}

/// Every Mac file (data file + optional sidecar) under a folder.
/// Every file under root, relative paths, including "._" sidecars (which
/// Foundation's directory listings hide).
func walkFiles(_ root: String) -> [String] {
    var out: [String] = []
    func walk(_ rel: String) {
        let dirPath = rel.isEmpty ? root : root + "/" + rel
        guard let d = opendir(dirPath) else { return }
        defer { closedir(d) }
        while let e = readdir(d) {
            let name = withUnsafeBytes(of: e.pointee.d_name) { raw in
                String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
            }
            if name == "." || name == ".." { continue }
            let child = rel.isEmpty ? name : rel + "/" + name
            if e.pointee.d_type == UInt8(DT_DIR) { walk(child) } else { out.append(child) }
        }
    }
    walk("")
    return out
}

func scan(_ root: String) -> [IngestedFile] {
    let fm = FileManager.default
    var out = [IngestedFile]()
    for rel in walkFiles(root) {
        var name = (rel as NSString).lastPathComponent
        var path = root + "/" + rel
        if name.hasPrefix("._") {
            // A sidecar whose file has no data fork stands for that file.
            let bare = (path as NSString).deletingLastPathComponent + "/" + String(name.dropFirst(2))
            if fm.fileExists(atPath: bare) { continue }
            path = bare
            name = String(name.dropFirst(2))
        } else if name == ".classic-source" || name.hasPrefix(".") {
            continue
        }
        var isDir: ObjCBool = false
        fm.fileExists(atPath: path, isDirectory: &isDir)
        if isDir.boolValue { continue }
        var f = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        guard appledouble_read(path, &f, &err, err.count) else { continue }
        defer { mac_file_free(&f) }
        var hasCODE = false, hasCfrg = false
        var idx = rsrc_index()
        if f.rsrc_len > 0, rsrc_parse(f.rsrc, f.rsrc_len, &idx, &err, err.count) {
            for i in 0..<idx.count {
                if idx.entries[i].type == fourCCValue("CODE") { hasCODE = true }
                if idx.entries[i].type == fourCCValue("cfrg") { hasCfrg = true }
            }
            rsrc_index_free(&idx)
        }
        out.append(IngestedFile(path: path, type: mac_file_type(&f), creator: mac_file_creator(&f),
                                dataSize: f.data_len, rsrcSize: f.rsrc_len, hasCODE: hasCODE, hasCfrg: hasCfrg))
    }
    return out.sorted { $0.path < $1.path }
}

// MARK: - Disk images

/// MacRoman bytes as a String.
func macRoman(_ bytes: [UInt8]) -> String {
    String(bytes: bytes, encoding: .macOSRoman) ?? String(decoding: bytes, as: UTF8.self)
}

private func pascal<T>(_ tuple: inout T) -> [UInt8] {
    withUnsafeBytes(of: &tuple) { raw in
        let n = Int(raw[0])
        return Array(raw[1..<min(1 + n, raw.count)])
    }
}

private func uniquePath(_ base: String) -> String {
    var path = base
    var n = 2
    while FileManager.default.fileExists(atPath: path) {
        path = "\(base) \(n)"
        n += 1
    }
    return path
}

private func cError(_ err: [CChar]) -> String {
    String(decoding: err.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
}

/// Extract every file of the image's HFS volume into a folder named after the
/// volume, next to the image.  Returns that folder.
/// A modern Mac disk image (UDIF, .dmg): its trailer starts with 'koly'.
func isUDIF(_ path: String) -> Bool {
    guard let h = FileHandle(forReadingAtPath: path) else { return false }
    defer { try? h.close() }
    guard let end = try? h.seekToEnd(), end >= 512 else { return false }
    try? h.seek(toOffset: end - 512)
    return h.readData(ofLength: 4) == Data("koly".utf8)
}

/// Convert a UDIF image to a raw one (hdiutil on macOS, dmg2img elsewhere).
private func rawFromUDIF(_ path: String) throws -> String {
    let out = NSTemporaryDirectory() + "bluebox-udif-\(UUID().uuidString).img"
    let p = Process()
    #if os(macOS)
    p.executableURL = URL(fileURLWithPath: "/usr/bin/hdiutil")
    p.arguments = ["convert", path, "-format", "UDTO", "-quiet", "-o", out]
    #else
    p.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    p.arguments = ["dmg2img", "-s", path, out]
    #endif
    p.standardOutput = FileHandle.nullDevice
    p.standardError = FileHandle.nullDevice
    try p.run()
    p.waitUntilExit()
    let produced = FileManager.default.fileExists(atPath: out) ? out : out + ".cdr"
    guard p.terminationStatus == 0, FileManager.default.fileExists(atPath: produced) else {
        throw IngestError.diskImage(path, "could not convert the .dmg (needs hdiutil, or dmg2img on Linux)")
    }
    return produced
}

func extractDiskImage(_ path: String, into dir: String, notes: inout [String]) throws -> String {
    if isUDIF(path) {
        let raw = try rawFromUDIF(path)
        defer { try? FileManager.default.removeItem(atPath: raw) }
        return try extractDiskImage(raw, into: dir, notes: &notes)
    }
    #if os(macOS)
    if isHFSPlus(path) { return try extractMounted(path, into: dir) }
    #endif
    var f = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    guard appledouble_read(path, &f, &err, err.count) else { throw IngestError.unreadable(path) }
    defer { mac_file_free(&f) }
    var vol: UnsafeMutablePointer<UInt8>?
    var vlen = 0
    guard diskimage_decode(&f, &vol, &vlen, &err, err.count) else {
        throw IngestError.diskImage(path, cError(err))
    }
    defer { free(vol) }
    guard let v = hfs_open(vol, vlen, &err, err.count) else { throw IngestError.diskImage(path, cError(err)) }
    defer { hfs_close(v) }
    var entries: UnsafeMutablePointer<hfs_entry>?
    var count = 0
    guard hfs_list(v, &entries, &count, &err, err.count), let entries else {
        throw IngestError.diskImage(path, cError(err))
    }
    defer { free(entries) }

    let volName = macRoman(Array(UnsafeBufferPointer(start: hfs_volume_name(v) + 1,
                                                     count: Int(hfs_volume_name(v)[0]))))
    let root = uniquePath(dir + "/" + hostName(volName.isEmpty ? "Untitled" : volName))
    let fm = FileManager.default
    try fm.createDirectory(atPath: root, withIntermediateDirectories: true)

    // Folder paths by directory ID; the root folder (ID 2) is the volume itself.
    var dirs: [UInt32: String] = [2: root]
    var byID: [UInt32: hfs_entry] = [:]
    for i in 0..<count where entries[i].is_dir { byID[entries[i].cnid] = entries[i] }
    func folder(_ id: UInt32, depth: Int = 0) -> String? {
        if let p = dirs[id] { return p }
        guard depth < 100, var e = byID[id], let parent = folder(e.parent, depth: depth + 1) else { return nil }
        let p = parent + "/" + hostName(macRoman(pascal(&e.name)))
        dirs[id] = p
        return p
    }
    for id in byID.keys {
        if let p = folder(id) { try fm.createDirectory(atPath: p, withIntermediateDirectories: true) }
    }
    for i in 0..<count where !entries[i].is_dir {
        var e = entries[i]
        guard let parent = folder(e.parent) else { continue }
        let nameBytes = pascal(&e.name)
        var mf = mac_file()
        var data: UnsafeMutablePointer<UInt8>?, rsrc: UnsafeMutablePointer<UInt8>?
        var dlen = 0, rlen = 0
        guard hfs_read_fork(v, &e, false, &data, &dlen, &err, err.count),
              hfs_read_fork(v, &e, true, &rsrc, &rlen, &err, err.count) else {
            free(data)
            notes.append("\(macRoman(nameBytes)): skipped (\(cError(err)); image truncated or damaged?)")
            continue
        }
        mf.data = data; mf.data_len = dlen
        mf.rsrc = rsrc; mf.rsrc_len = rlen
        withUnsafeMutableBytes(of: &mf.finder_info) { dst in
            withUnsafeBytes(of: &e.finder_info) { src in dst.copyMemory(from: src) }
        }
        mf.has_finder_info = true
        withUnsafeMutableBytes(of: &mf.name) { dst in
            for (k, b) in nameBytes.prefix(63).enumerated() { dst[k] = b }
        }
        mf.create_date = e.create_date
        mf.modify_date = e.modify_date
        defer { mac_file_free(&mf) }
        try writeAppleDouble(&mf, to: parent + "/" + hostName(macRoman(nameBytes)))
    }
    return root
}

// MARK: - HFS+ (macOS: through the system's own driver)

/// An HFS+ or HFSX volume, bare or as the first partition after a driver map.
func isHFSPlus(_ path: String) -> Bool {
    guard let h = FileHandle(forReadingAtPath: path) else { return false }
    defer { try? h.close() }
    let head = h.readData(ofLength: 1026)
    guard head.count == 1026 else { return false }
    let sig = head[1024..<1026]
    return sig == Data("H+".utf8) || sig == Data("HX".utf8)
}

#if os(macOS)
/// Mount the image read-only and copy its files out as AppleDouble.
private func extractMounted(_ path: String, into dir: String) throws -> String {
    let mount = NSTemporaryDirectory() + "bluebox-mount-\(UUID().uuidString)"
    try FileManager.default.createDirectory(atPath: mount, withIntermediateDirectories: true)
    func hdiutil(_ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/hdiutil")
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }
    guard hdiutil(["attach", "-readonly", "-nobrowse", "-noverify", "-noautoopen", "-mountpoint", mount, "-imagekey",
                   "diskimage-class=CRawDiskImage", path]) == 0 else {
        throw IngestError.diskImage(path, "could not mount the HFS+ volume")
    }
    defer {
        _ = hdiutil(["detach", mount, "-quiet", "-force"])
        try? FileManager.default.removeItem(atPath: mount)
    }
    let fm = FileManager.default
    let volName = fm.displayName(atPath: mount)
    let root = uniquePath(dir + "/" + (volName.isEmpty ? "Untitled" : volName))
    try fm.createDirectory(atPath: root, withIntermediateDirectories: true)
    guard let walker = fm.enumerator(atPath: mount) else { return root }
    for case let rel as String in walker {
        let src = mount + "/" + rel, dst = root + "/" + rel
        if rel.split(separator: "/").contains(where: { $0.hasPrefix(".") }) { continue }
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: src, isDirectory: &isDir) else { continue }
        if isDir.boolValue {
            try fm.createDirectory(atPath: dst, withIntermediateDirectories: true)
            continue
        }
        var data = (try? Data(contentsOf: URL(fileURLWithPath: src))) ?? Data()
        var rsrc = (try? Data(contentsOf: URL(fileURLWithPath: src + "/..namedfork/rsrc"))) ?? Data()
        var info = [UInt8](repeating: 0, count: 32)
        let n = getxattr(src, "com.apple.FinderInfo", &info, 32, 0, 0)
        var mf = mac_file()
        try data.withUnsafeMutableBytes { d in
            try rsrc.withUnsafeMutableBytes { r in
                mf.data = d.baseAddress?.assumingMemoryBound(to: UInt8.self)
                mf.data_len = d.count
                mf.rsrc = r.baseAddress?.assumingMemoryBound(to: UInt8.self)
                mf.rsrc_len = r.count
                if n > 0 {
                    withUnsafeMutableBytes(of: &mf.finder_info) { $0.copyBytes(from: info) }
                    mf.has_finder_info = true
                }
                var err = [CChar](repeating: 0, count: 256)
                if !appledouble_write(dst, &mf, &err, err.count) {
                    throw IngestError.diskImage(path, str(err))
                }
            }
        }
    }
    return root
}
#endif
