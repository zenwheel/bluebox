// A classic file's icon as the Finder found it: its custom icon, else the
// bundle of the file itself (an application, control panel or extension
// with a BNDL), else the bundle of the application that made it (by its
// creator: the FREF for its type).  Applications are found by creator in
// Blue Box's library and on its Macintosh HD, as the desktop database
// found them on a Mac's volumes.

import CCore
import Foundation
import ForkStore

/// The applications in some folders, by creator, read once and kept (the
/// Finder's desktop database, for the host).  Thread-safe.
public final class ClassicApplicationIndex: @unchecked Sendable {
    public let roots: [String]
    private let lock = NSLock()
    private var byCreator: [UInt32: [String]]?
    private var readAt = Date.distantPast
    /// How long a reading stays good.
    public var lifetime: TimeInterval = 300

    public init(roots: [String]) { self.roots = roots }

    /// Blue Box's library and Macintosh HD under a home folder (the real
    /// one: a sandboxed extension's HOME is its container).
    public static func standardRoots(home: String) -> [String] {
        let support = home + "/Library/Application Support/Blue Box"
        return [support + "/Library", support + "/Macintosh HD"]
    }

    /// The applications with this creator, nearest the top of the roots first.
    public func applications(creator: UInt32) -> [String] {
        lock.lock()
        defer { lock.unlock() }
        if byCreator == nil || Date().timeIntervalSince(readAt) > lifetime {
            byCreator = Self.scan(roots)
            readAt = Date()
        }
        return byCreator?[creator] ?? []
    }

    /// Read again at the next question.
    public func invalidate() {
        lock.lock()
        byCreator = nil
        lock.unlock()
    }

    /// Every file of type APPL (or the Finder's FNDR) under the roots, by
    /// creator: read from the sidecars' Finder information alone.
    static func scan(_ roots: [String]) -> [UInt32: [String]] {
        var out: [UInt32: [String]] = [:]
        for root in roots {
            var found: [(depth: Int, path: String, creator: UInt32)] = []
            for rel in walkFiles(root) {
                let name = (rel as NSString).lastPathComponent
                guard name.hasPrefix("._"), name.count > 2 else { continue }
                let dir = (rel as NSString).deletingLastPathComponent
                let path = root + "/" + (dir.isEmpty ? "" : dir + "/") + String(name.dropFirst(2))
                var s = mac_file_summary()
                guard appledouble_read_summary(path, true, &s) else { continue }
                let fi = withUnsafeBytes(of: s.finder_info) { Array($0) }
                mac_file_summary_free(&s)
                let type = fi[0..<4].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
                let creator = fi[4..<8].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
                guard type == fourCharCode("APPL") || type == fourCharCode("FNDR") || type == fourCharCode("appe") else { continue }
                found.append((rel.split(separator: "/").count, path, creator))
            }
            for f in found.sorted(by: { ($0.depth, $0.path) < ($1.depth, $1.path) }) {
                out[f.creator, default: []].append(f.path)
            }
        }
        return out
    }
}

/// Finds and keeps classic files' icons.  Thread-safe.
public final class ClassicIconLookup: @unchecked Sendable {
    public typealias Fallback = @Sendable (_ path: String, _ meta: ClassicMeta) -> [IconImage]
    public let applications: ClassicApplicationIndex
    /// Icons for files nothing above names one for (BlueBoxHost's: the
    /// System's generic ones for desk accessories and suitcases).
    private let fallback: Fallback?
    private let lock = NSLock()
    /// Applications' document icons, by creator and type.
    private var documentCache: [UInt64: [IconImage]] = [:]

    public init(applications: ClassicApplicationIndex, fallback: Fallback? = nil) {
        self.applications = applications
        self.fallback = fallback
    }

    /// The icons of the classic file at path (32 and 16 pixels), or none
    /// for a file with no classic side or no icon to be found.
    public func icons(forFileAt path: String, meta given: ClassicMeta? = nil) -> [IconImage] {
        guard let meta = given ?? ClassicMeta.read(path), meta.isClassic else { return [] }
        let own = meta.resourceFork.withUnsafeBufferPointer { ResourceFork(fork: $0.baseAddress, length: $0.count) }
        if let own, meta.flags & FinderFlags.hasCustomIcon != 0 {
            let custom = ClassicFiles.icons(fork: own, id: -16455) // kCustomIconResource
            if !custom.isEmpty { return custom }
        }
        // The file's own bundle (an application's, a control panel's).
        if let own {
            let mine = ClassicFiles.documentIcons(fork: own, creator: meta.creator, type: meta.type)
            if !mine.isEmpty { return mine }
            if meta.type == fourCharCode("APPL") {
                let app = finderBundle(fork: own, creator: meta.creator).appIcons
                if !app.isEmpty { return app }
            }
        }
        // A document: its application's bundle.
        if meta.type != fourCharCode("APPL"), meta.creator != 0 {
            let docs = documentIcons(creator: meta.creator, type: meta.type)
            if !docs.isEmpty { return docs }
        }
        return fallback?(path, meta) ?? []
    }

    /// The icons the application with this creator gives its documents
    /// of this type.
    public func documentIcons(creator: UInt32, type: UInt32) -> [IconImage] {
        let key = UInt64(creator) << 32 | UInt64(type)
        lock.lock()
        if let cached = documentCache[key] {
            lock.unlock()
            return cached
        }
        lock.unlock()
        var found: [IconImage] = []
        for app in applications.applications(creator: creator).prefix(4) {
            guard let meta = ClassicMeta.read(app), !meta.resourceFork.isEmpty,
                  let fork = meta.resourceFork.withUnsafeBufferPointer({ ResourceFork(fork: $0.baseAddress, length: $0.count) })
            else { continue }
            found = ClassicFiles.documentIcons(fork: fork, creator: creator, type: type)
            if !found.isEmpty { break }
        }
        lock.lock()
        documentCache[key] = found
        lock.unlock()
        return found
    }

    /// Forget the icons kept (applications came or went).
    public func invalidate() {
        lock.lock()
        documentCache = [:]
        lock.unlock()
        applications.invalidate()
    }
}
