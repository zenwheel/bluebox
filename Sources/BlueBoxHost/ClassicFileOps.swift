// The Blue Box Browser's file operations, keeping classic files whole: a
// file's data fork and its AppleDouble "._" sidecar (resource fork, Finder
// information, comment) go together; a folder's Finder information and
// comment, which are the catalog's (Toolbox/catalog.c), follow it; and so
// do the directory and file IDs the catalog keeps for Macintosh HD, as the
// File Manager's own renames and moves keep them (filemgr.c rename_host).
//
// A file with forks of its own (macOS extended attributes: one copied from
// a Mac volume or unpacked with them) gets a sidecar when it is copied or
// moved in, so Blue Box's files keep one form.  Going out to the host
// (exportForHost: a drag to the macOS Finder) a file's sidecar becomes its
// own forks again, the form the host copies, zips and sends whole.

import CCore
import Foundation
import ForkStore
import Toolbox

public struct ClassicFileError: Error, CustomStringConvertible, Equatable {
    public enum Kind: Sendable { case exists, missing, locked, inside, notAFile, failed }
    public let kind: Kind
    public let description: String
    public let path: String
    init(_ kind: Kind, _ path: String, _ description: String) {
        self.kind = kind
        self.path = path
        self.description = description
    }
}

/// A file or folder as Get Info shows it.
public struct ClassicItemInfo: Sendable {
    public var isFolder: Bool
    /// A file's: its sidecar's, or its own forks'; a folder's: the
    /// catalog's Finder information (the first 16 bytes) and comment.
    public var meta: ClassicMeta
    /// The file has a classic side (a sidecar or forks of its own).
    public var isClassic: Bool
    public var hasSidecar: Bool
    public var dataLength: Int
    public var locked: Bool
}

public struct ClassicFileOps {
    /// Macintosh HD's folder, whose files' IDs the catalog keeps.
    public let disk: String

    /// Opens the catalog beside the disk (Catalog.sqlite, as the File
    /// Manager does), shared with the runs.
    public init(disk: String, catalog: String? = nil) {
        self.disk = (disk as NSString).standardizingPath
        let path = catalog ?? ((self.disk as NSString).deletingLastPathComponent as NSString)
            .appendingPathComponent("Catalog.sqlite")
        catalog_set_path(path)
    }

    private var fm: FileManager { .default }

    /// Whether anything is at path: its data fork, or a sidecar alone (a
    /// file whose data fork is empty, written by tools that leave it out).
    public func exists(_ path: String) -> Bool {
        var st = stat()
        return lstat(path, &st) == 0 || lstat(classicSidecarPath(path), &st) == 0
    }

    public func isFolder(_ path: String) -> Bool {
        var dir: ObjCBool = false
        return fm.fileExists(atPath: path, isDirectory: &dir) && dir.boolValue
    }

    // MARK: Get Info

    public func info(_ path: String) -> ClassicItemInfo {
        var st = stat()
        let there = stat(path, &st) == 0
        let folder = there && (st.st_mode & S_IFMT) == S_IFDIR
        #if canImport(Darwin)
        let locked = there && st.st_flags & UInt32(UF_IMMUTABLE) != 0
        #else
        let locked = false
        #endif
        if folder {
            var meta = ClassicMeta()
            var fi = [UInt8](repeating: 0, count: 16), fx = [UInt8](repeating: 0, count: 16)
            if catalog_get_dirinfo(path, &fi, &fx) {
                meta.finderInfo = fi + fx
                meta.hasFinderInfo = true
            }
            var text = [UInt8](repeating: 0, count: 256)
            let n = Int(catalog_get_comment(path, &text, 200))
            meta.comment = Array(text.prefix(max(0, n)))
            return ClassicItemInfo(isFolder: true, meta: meta, isClassic: false, hasSidecar: false, dataLength: 0,
                                   locked: locked)
        }
        let sidecar = fm.fileExists(atPath: classicSidecarPath(path))
        let meta = ClassicMeta.read(path) ?? ClassicMeta()
        return ClassicItemInfo(isFolder: false, meta: meta, isClassic: sidecar || appledouble_has_native(path),
                               hasSidecar: sidecar, dataLength: there ? Int(st.st_size) : 0, locked: locked)
    }

    /// Change what Get Info changes: a file's sidecar (made if it had
    /// none), a folder's catalog entry.
    public func update(_ path: String, _ change: (inout ClassicMeta) -> Void) throws {
        var meta = info(path).meta
        let before = meta
        change(&meta)
        guard meta != before else { return }
        if isFolder(path) {
            let fi = Array(meta.finderInfo[0..<16]), fx = Array(meta.finderInfo[16..<32])
            guard catalog_set_dirinfo(path, fi, fx) else { throw ClassicFileError(.failed, path, "the catalog can't be written") }
            if meta.comment != before.comment {
                guard catalog_set_comment(path, meta.comment, Int32(meta.comment.count)) else {
                    throw ClassicFileError(.failed, path, "the catalog can't be written")
                }
            }
            return
        }
        guard !info(path).locked else { throw ClassicFileError(.locked, path, "\(name(path)) is locked") }
        try meta.write(sidecarOf: path)
    }

    /// The host's lock (the macOS Finder's Locked: the user immutable flag).
    public func setLocked(_ path: String, _ locked: Bool) throws {
        #if canImport(Darwin)
        var values = URLResourceValues()
        values.isUserImmutable = locked
        var url = URL(fileURLWithPath: path)
        try url.setResourceValues(values)
        #endif
    }

    // MARK: Copying, moving, renaming

    private func name(_ path: String) -> String { (path as NSString).lastPathComponent }

    /// A name not taken in dir: "Name", else "Name copy", "Name copy 2" ...
    /// (before any extension), as the Finder names duplicates.
    public func freeName(_ name: String, in dir: String, copy: Bool) -> String {
        let ext = (name as NSString).pathExtension
        let base = ext.isEmpty || ext.count > 5 ? name : (name as NSString).deletingPathExtension
        let suffix = ext.isEmpty || ext.count > 5 ? "" : "." + ext
        var n = 1
        while true {
            let candidate = n == 1 ? (copy ? "\(base) copy\(suffix)" : name) : copy ? "\(base) copy \(n)\(suffix)"
                : "\(base) \(n)\(suffix)"
            if !exists(dir + "/" + candidate) { return candidate }
            n += 1
        }
    }

    /// The disk-relative path the catalog keeps a node's ID by, or nil off
    /// the disk.
    private func rel(_ path: String) -> String? {
        let p = (path as NSString).standardizingPath
        guard p.hasPrefix(disk + "/") else { return nil }
        return String(p.dropFirst(disk.count + 1))
    }

    private func checkTarget(_ src: String, _ dest: String) throws {
        guard exists(src) else { throw ClassicFileError(.missing, src, "\(name(src)) is not there any more") }
        if exists(dest) { throw ClassicFileError(.exists, dest, "an item named \(name(dest)) is already there") }
        let s = (src as NSString).standardizingPath, d = (dest as NSString).standardizingPath
        if d == s || d.hasPrefix(s + "/") {
            throw ClassicFileError(.inside, dest, "\(name(src)) can't go inside itself")
        }
    }

    /// Copy a file or folder into dir (under name, or its own); returns
    /// the copy's path.  Forks of its own become a sidecar.
    @discardableResult
    public func copy(_ src: String, into dir: String, name newName: String? = nil) throws -> String {
        let dest = dir + "/" + (newName ?? name(src))
        try checkTarget(src, dest)
        do {
            if isFolder(src) {
                try fm.copyItem(atPath: src, toPath: dest)
                copyFolderCatalog(from: src, to: dest)
                normalizeTree(dest)
            } else {
                try copyFile(src, to: dest)
            }
        } catch let e as ClassicFileError {
            throw e
        } catch {
            throw ClassicFileError(.failed, src, "\(name(src)) could not be copied: \(error.localizedDescription)")
        }
        return dest
    }

    private func copyFile(_ src: String, to dest: String) throws {
        let side = classicSidecarPath(src)
        let hasData = fm.fileExists(atPath: src)
        if hasData { try fm.copyItem(atPath: src, toPath: dest) }
        if fm.fileExists(atPath: side) {
            try fm.copyItem(atPath: side, toPath: classicSidecarPath(dest))
            if hasData { removeNativeForks(dest) }
        } else if hasData {
            normalizeFile(dest)
        }
        // A copy is never locked, as the Finder's weren't.
        if hasData, info(dest).locked { try? setLocked(dest, false) }
    }

    /// A file with forks of its own given a sidecar instead.
    func normalizeFile(_ path: String) {
        guard appledouble_has_native(path), !fm.fileExists(atPath: classicSidecarPath(path)),
              let meta = ClassicMeta.read(path) else { return }
        try? meta.write(sidecarOf: path)
    }

    /// Every file in a tree with forks of its own given a sidecar; a
    /// folder's own Finder information (its label) into the catalog.
    func normalizeTree(_ root: String) {
        normalizeFolder(root)
        guard let e = fm.enumerator(atPath: root) else { return }
        while let rel = e.nextObject() as? String {
            let path = root + "/" + rel
            if (rel as NSString).lastPathComponent.hasPrefix("._") { continue }
            if isFolder(path) { normalizeFolder(path) } else { normalizeFile(path) }
        }
    }

    private func normalizeFolder(_ path: String) {
        var fi = [UInt8](repeating: 0, count: 32)
        guard appledouble_native_finder_info(path, &fi), fi.contains(where: { $0 != 0 }) else { return }
        var cfi = [UInt8](repeating: 0, count: 16), cfx = [UInt8](repeating: 0, count: 16)
        if !catalog_get_dirinfo(path, &cfi, &cfx) {
            catalog_set_dirinfo(path, Array(fi[0..<16]), Array(fi[16..<32]))
        }
        removeNativeForks(path)
    }

    /// The catalog's entries for a folder and the folders in it, copied.
    private func copyFolderCatalog(from src: String, to dest: String) {
        var dirs = [""]
        if let e = fm.enumerator(atPath: src) {
            while let rel = e.nextObject() as? String {
                if isFolder(src + "/" + rel) { dirs.append("/" + rel) }
            }
        }
        for d in dirs {
            var fi = [UInt8](repeating: 0, count: 16), fx = [UInt8](repeating: 0, count: 16)
            if catalog_get_dirinfo(src + d, &fi, &fx) { catalog_set_dirinfo(dest + d, fi, fx) }
            var text = [UInt8](repeating: 0, count: 256)
            let n = catalog_get_comment(src + d, &text, 200)
            if n > 0 { catalog_set_comment(dest + d, text, n) }
        }
    }

    /// Move (or rename) a file or folder into dir; returns its new path.
    @discardableResult
    public func move(_ src: String, into dir: String, name newName: String? = nil) throws -> String {
        let dest = dir + "/" + (newName ?? name(src))
        // A change of case alone is a rename of the item to itself.
        let sameItem = src.lowercased() == dest.lowercased() && src != dest
        if !sameItem { try checkTarget(src, dest) }
        if info(src).locked { throw ClassicFileError(.locked, src, "\(name(src)) is locked") }
        let side = classicSidecarPath(src)
        do {
            if fm.fileExists(atPath: src) || (try? fm.destinationOfSymbolicLink(atPath: src)) != nil {
                try fm.moveItem(atPath: src, toPath: dest)
            }
            if fm.fileExists(atPath: side) { try fm.moveItem(atPath: side, toPath: classicSidecarPath(dest)) }
        } catch {
            throw ClassicFileError(.failed, src, "\(name(src)) could not be moved: \(error.localizedDescription)")
        }
        catalog_moved(src, dest)
        switch (rel(src), rel(dest)) {
        case let (f?, t?): catalog_nodes_moved(f, t)
        case let (f?, nil): catalog_nodes_moved(f, nil)
        default: break
        }
        if isFolder(dest) { normalizeTree(dest) } else { normalizeFile(dest) }
        return dest
    }

    @discardableResult
    public func rename(_ src: String, to newName: String) throws -> String {
        guard !newName.isEmpty, !newName.contains("/"), !newName.hasPrefix("._"), newName != ".", newName != ".." else {
            throw ClassicFileError(.failed, src, "“\(newName)” can't be used as a name")
        }
        return try move(src, into: (src as NSString).deletingLastPathComponent, name: newName)
    }

    /// "Name copy" beside the original.
    @discardableResult
    public func duplicate(_ src: String) throws -> String {
        let dir = (src as NSString).deletingLastPathComponent
        return try copy(src, into: dir, name: freeName(name(src), in: dir, copy: true))
    }

    // MARK: Aliases

    /// The Mac path a run gives a host path on Macintosh HD (others by
    /// their own folders' names, which only the host path then finds).
    func macPath(_ path: String) -> [UInt8] {
        let parts: [String]
        if let r = rel(path) {
            parts = ["Macintosh HD"] + r.split(separator: "/").map(String.init)
        } else {
            parts = (path as NSString).pathComponents.filter { $0 != "/" }
        }
        var out: [UInt8] = []
        for (i, p) in parts.enumerated() {
            if i > 0 { out.append(0x3A) }
            out += macRomanBytes(p.replacingOccurrences(of: ":", with: "/"), max: i == 0 ? 27 : 31)
        }
        return out
    }

    /// "Name alias" beside the original (or in dir): a classic alias,
    /// which Blue Box's Alias Manager resolves, and the browser opens.
    @discardableResult
    public func makeAlias(_ target: String, in dir: String? = nil) throws -> String {
        guard exists(target) else { throw ClassicFileError(.missing, target, "\(name(target)) is not there any more") }
        let folder = dir ?? (target as NSString).deletingLastPathComponent
        let base = name(target) + " alias"
        var aliasName = base, n = 2
        while exists(folder + "/" + aliasName) {
            aliasName = "\(base) \(n)"
            n += 1
        }
        let path = folder + "/" + aliasName
        let isDir = isFolder(target)
        var meta = ClassicAlias.finderInfo(target: isDir ? nil : ClassicMeta.read(target), isFolder: isDir)
        let rec = ClassicAlias.record(hostPath: (target as NSString).standardizingPath, macPath: macPath(target),
                                      isFolder: isDir)
        meta.resourceFork = ClassicAlias.resourceFork(record: rec)
        do {
            try Data().write(to: URL(fileURLWithPath: path))
            try meta.write(sidecarOf: path)
        } catch {
            throw ClassicFileError(.failed, path, "the alias could not be made: \(error.localizedDescription)")
        }
        return path
    }

    /// Where a classic alias leads, if it is one.
    public func aliasTarget(_ path: String) -> String? {
        guard let meta = ClassicMeta.read(path), meta.isAlias, let rec = ClassicAlias.record(inFork: meta.resourceFork)
        else { return nil }
        return ClassicAlias.hostPath(of: rec)
    }

    // MARK: The Trash

    /// Move to the Trash: trash puts one host item there (the system's,
    /// FileManager.trashItem, or a test's) and says where.  The sidecar
    /// goes too, named to match the file however the Trash renamed it.
    @discardableResult
    public func moveToTrash(_ src: String, trash: (URL) throws -> URL) throws -> String {
        guard exists(src) else { throw ClassicFileError(.missing, src, "\(name(src)) is not there any more") }
        if info(src).locked { throw ClassicFileError(.locked, src, "\(name(src)) is locked") }
        let side = classicSidecarPath(src)
        let hasSide = fm.fileExists(atPath: side)
        var trashed: String
        do {
            if fm.fileExists(atPath: src) || (try? fm.destinationOfSymbolicLink(atPath: src)) != nil {
                trashed = try trash(URL(fileURLWithPath: src)).path
                if hasSide {
                    let sideTrashed = try trash(URL(fileURLWithPath: side)).path
                    let want = classicSidecarPath(trashed)
                    if sideTrashed != want, !fm.fileExists(atPath: want) { try? fm.moveItem(atPath: sideTrashed, toPath: want) }
                }
            } else {
                // A sidecar alone is the whole file.
                let sideTrashed = try trash(URL(fileURLWithPath: side)).path
                let n = (sideTrashed as NSString).lastPathComponent
                trashed = (sideTrashed as NSString).deletingLastPathComponent + "/" + String(n.dropFirst(2))
            }
        } catch {
            throw ClassicFileError(.failed, src, "\(name(src)) could not be moved to the Trash: \(error.localizedDescription)")
        }
        catalog_moved(src, trashed)
        if let r = rel(src) { catalog_nodes_moved(r, nil) }
        return trashed
    }

    /// Delete for good what the Trash folder holds (or the items given),
    /// their sidecars and their catalog entries with them.  Returns how
    /// many items went; throws if the folder can't be read.
    @discardableResult
    public func emptyTrash(_ trashFolder: String) throws -> Int {
        _ = try fm.contentsOfDirectory(atPath: trashFolder) // readable at all?
        var removed = 0
        let names = directoryEntries(trashFolder).map(\.name)
        let present = Set(names)
        for n in names where n != ".DS_Store" {
            // A sidecar goes with its file; one alone is a whole file.
            if n.hasPrefix("._"), present.contains(String(n.dropFirst(2))) { continue }
            let path = trashFolder + "/" + (n.hasPrefix("._") ? String(n.dropFirst(2)) : n)
            // A locked item can't be deleted until it is unlocked.
            if info(path).locked { try? setLocked(path, false) }
            if isFolder(path) { unlockTree(path) }
            let side = classicSidecarPath(path)
            var st = stat()
            if lstat(side, &st) == 0 { unlink(side) }
            if lstat(path, &st) == 0 {
                do { try fm.removeItem(atPath: path) } catch { continue }
            }
            removed += 1
            catalog_moved(path, nil)
        }
        return removed
    }

    private func unlockTree(_ root: String) {
        guard let e = fm.enumerator(atPath: root) else { return }
        while let rel = e.nextObject() as? String {
            if info(root + "/" + rel).locked { try? setLocked(root + "/" + rel, false) }
        }
    }

    // MARK: To and from the host

    /// A copy for the host at dest: the data fork with the classic side as
    /// the file's own forks, no sidecar (a folder: each file in it so).
    public func exportForHost(_ src: String, to dest: String) throws {
        if exists(dest) { throw ClassicFileError(.exists, dest, "an item named \(name(dest)) is already there") }
        do {
            if isFolder(src) {
                try fm.copyItem(atPath: src, toPath: dest)
                // Sidecars into the files beside them.
                let sidecars = walkFiles(dest).filter { ($0 as NSString).lastPathComponent.hasPrefix("._") }
                    .map { dest + "/" + $0 }
                for side in sidecars {
                    let dir = (side as NSString).deletingLastPathComponent
                    let file = dir + "/" + String((side as NSString).lastPathComponent.dropFirst(2))
                    if !fm.fileExists(atPath: file) { fm.createFile(atPath: file, contents: nil) }
                    if let meta = ClassicMeta.read(file) { writeNativeForks(meta, to: file) }
                    try? fm.removeItem(atPath: side)
                }
            } else {
                if fm.fileExists(atPath: src) {
                    try fm.copyItem(atPath: src, toPath: dest)
                } else {
                    fm.createFile(atPath: dest, contents: nil)
                }
                if fm.fileExists(atPath: classicSidecarPath(src)), let meta = ClassicMeta.read(src) {
                    writeNativeForks(meta, to: dest)
                }
                if info(dest).locked { try? setLocked(dest, false) }
            }
        } catch {
            throw ClassicFileError(.failed, src, "\(name(src)) could not be copied: \(error.localizedDescription)")
        }
    }

    /// The file as MacBinary III in dir ("Name.bin"); returns its path.
    @discardableResult
    public func exportMacBinary(_ src: String, into dir: String) throws -> String {
        guard !isFolder(src) else { throw ClassicFileError(.notAFile, src, "a folder can't be made MacBinary") }
        guard let meta = ClassicMeta.read(src) else { throw ClassicFileError(.failed, src, "\(name(src)) can't be read") }
        let data = fm.fileExists(atPath: src) ? Array((try? Data(contentsOf: URL(fileURLWithPath: src))) ?? Data()) : []
        let macName = meta.name.isEmpty ? macRomanBytes(name(src), max: 63) : meta.name
        let out = dir + "/" + freeName(name(src) + ".bin", in: dir, copy: false)
        do {
            try Data(MacBinary.encode(name: macName, data: data, meta: meta)).write(to: URL(fileURLWithPath: out))
        } catch {
            throw ClassicFileError(.failed, out, "\(name(out)) could not be written: \(error.localizedDescription)")
        }
        return out
    }
}
