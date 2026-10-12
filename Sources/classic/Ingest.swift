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

    var description: String {
        switch self {
        case .unreadable(let p): return "cannot read \(p)"
        case .unarMissing:
            return "extracting archives needs unar (The Unarchiver): brew install unar, or apt install unar"
        case .unarFailed(let p, let msg): return "unar could not extract \(p): \(msg)"
        case .notAnArchive(let p): return "\(p) is not MacBinary, an archive, or an AppleDouble file"
        }
    }
}

func fourCCValue(_ s: String) -> UInt32 {
    Array(s.utf8).reduce(0) { ($0 << 8) | UInt32($1) }
}

/// Where ingested software goes unless the caller says otherwise.
func defaultLibrary() -> String {
    let env = ProcessInfo.processInfo.environment
    if let lib = env["CLASSIC_LIBRARY"], !lib.isEmpty { return lib }
    let home = env["HOME"] ?? FileManager.default.homeDirectoryForCurrentUser.path
    #if os(macOS)
    return home + "/Library/Application Support/Classic/Library"
    #else
    let data = env["XDG_DATA_HOME"].flatMap { $0.isEmpty ? nil : $0 } ?? home + "/.local/share"
    return data + "/classic/library"
    #endif
}

// MARK: - Recognising inputs

enum InputKind {
    case macBinary
    case archive
    case appleDouble  // a data file with a "._" sidecar
    case plain
}

private func readPrefix(_ path: String, _ n: Int = 1024) -> [UInt8] {
    guard let h = FileHandle(forReadingAtPath: path) else { return [] }
    defer { try? h.close() }
    return Array((try? h.read(upToCount: n)) ?? Data())
}

private let stuffItSignatures: Set<String> = ["SIT!", "ST46", "ST50", "ST60", "ST65", "STin", "STi2", "STi3", "STi4"]
private let archiveTypes: Set<String> = ["SIT!", "SITD", "SIT5", "SITX", "PACT", "BnHq"]
private let archiveCreators: Set<String> = ["SIT!", "SITx", "CPCT", "aust", "BnHq"]
private let archiveExtensions: Set<String> = ["sit", "sea", "cpt", "hqx"]

/// Archive formats recognisable from the data fork alone.
private func looksLikeArchiveData(_ b: [UInt8]) -> Bool {
    if b.count >= 14 {
        let sig = String(decoding: b[0..<4], as: UTF8.self)
        if stuffItSignatures.contains(sig) && String(decoding: b[10..<14], as: UTF8.self) == "rLau" { return true }
    }
    if b.starts(with: Array("StuffIt (c)1997".utf8)) { return true }
    let text = String(decoding: b, as: UTF8.self)
    return text.contains("(This file must be converted with BinHex")
}

private func isArchive(type: UInt32, creator: UInt32, name: String, data: [UInt8]) -> Bool {
    let t = fourCCString(type), c = fourCCString(creator)
    if archiveTypes.contains(t) || (archiveCreators.contains(c) && t != "APPL") { return true }
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
    if macbinary_detect(&prefix, prefix.count) { return .macBinary }
    if FileManager.default.fileExists(atPath: sidecarPath(path)) {
        // An AppleDouble file may itself be an archive (e.g. a .sit with its forks split out).
        var f = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        if appledouble_read(path, &f, &err, err.count) {
            defer { mac_file_free(&f) }
            let name = (path as NSString).lastPathComponent
            if isArchive(type: mac_file_type(&f), creator: mac_file_creator(&f), name: name, data: prefix) {
                return .archive
            }
        }
        return .appleDouble
    }
    let ext = (path as NSString).pathExtension.lowercased()
    if archiveExtensions.contains(ext) || looksLikeArchiveData(prefix) { return .archive }
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
            try decodeMacBinary(path, into: dir, depth: depth, notes: &notes)
            try? fm.removeItem(atPath: path) // only an encoding wrapper
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
    let wrappers: Set<String> = ["bin", "sit", "sea", "cpt", "hqx", "macbin"]
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
        case .appleDouble:
            let name = (source as NSString).lastPathComponent
            try fm.copyItem(atPath: source, toPath: entry + "/" + name)
            try fm.copyItem(atPath: sidecarPath(source), toPath: entry + "/._" + name)
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
func scan(_ root: String) -> [IngestedFile] {
    let fm = FileManager.default
    var out = [IngestedFile]()
    guard let walker = fm.enumerator(atPath: root) else { return out }
    for case let rel as String in walker {
        let name = (rel as NSString).lastPathComponent
        if name.hasPrefix("._") || name == ".classic-source" || name.hasPrefix(".") { continue }
        let path = root + "/" + rel
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
