// Telling archives from other Mac files: what bluebox import expands.

import Foundation

private let stuffItSignatures: Set<String> = ["SIT!", "ST46", "ST50", "ST60", "ST65", "STin", "STi2", "STi3", "STi4"]
private let archiveTypes: Set<String> = ["SIT!", "SITD", "SIT5", "SITX", "PACT", "BnHq"]
private let archiveCreators: Set<String> = ["SIT!", "SITx", "CPCT", "aust", "BnHq", "ArcF"]
public let archiveExtensions: Set<String> = ["sit", "sea", "cpt", "hqx", "zip"]

private func isZip(_ b: [UInt8]) -> Bool { b.starts(with: [0x50, 0x4B, 0x03, 0x04]) }

/// Archive formats recognisable from the data fork alone.
public func looksLikeArchiveData(_ b: [UInt8]) -> Bool {
    if b.count >= 14 {
        let sig = String(decoding: b[0..<4], as: UTF8.self)
        if stuffItSignatures.contains(sig) && String(decoding: b[10..<14], as: UTF8.self) == "rLau" { return true }
    }
    if b.starts(with: Array("StuffIt (c)1997".utf8)) { return true }
    if isZip(b) { return true }
    let text = String(decoding: b, as: UTF8.self)
    return text.contains("(This file must be converted with BinHex")
}

/// Whether a Mac file (its type, creator, name and the start of its data
/// fork) is an archive to expand.
public func isMacArchive(type: UInt32, creator: UInt32, name: String, data: [UInt8]) -> Bool {
    let t = fourCCText(type), c = fourCCText(creator)
    // A Java application (JBindery's, 'jtUA' and the like) keeps its classes
    // as a zip in its data fork: an application to run, not to expand.
    if t == "APPL" && isZip(data) && !archiveCreators.contains(c) { return false }
    if looksLikeArchiveData(data) { return true }
    // Types alone are not enough: Word settings files, for one, are 'SITD'.
    if archiveTypes.contains(t) && archiveCreators.contains(c) { return true }
    if t == "APPL" && (c == "aust" || c == "EXTR") { return true } // self-extracting archives
    let ext = (name as NSString).pathExtension.lowercased()
    return archiveExtensions.contains(ext)
}
