// The Blue Box Browser's model: what a folder holds, as the Finder showed
// it (a classic file with its sidecar is one item, a sidecar alone is a
// file whose data fork is empty, invisible files are left out), the
// places in its sidebar, and the icons, read off the main thread: a
// classic file's from its bundle or its creator's (ClassicIconLookup), a
// host file's from macOS.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import ForkStore
import Toolbox
import UniformTypeIdentifiers

/// One item of a folder.
struct BrowserItem: Hashable, @unchecked Sendable {
    let path: String
    let name: String
    let isFolder: Bool
    /// A classic file: a sidecar, or forks of its own.
    let isClassic: Bool
    let type: UInt32
    let creator: UInt32
    let flags: UInt16
    /// Data and resource forks together (folders: -1).
    let size: Int
    let resourceSize: Int
    let modified: Date
    let created: Date
    let kind: String

    var url: URL { URL(fileURLWithPath: path) }
    var label: Int { Int((flags & FinderFlags.labelMask) >> 1) }
    var isAlias: Bool { isClassic && flags & FinderFlags.isAlias != 0 }
    var isApplication: Bool { isClassic && (type == fourCharCode("APPL") || type == fourCharCode("FNDR")) }
    var isPackage: Bool { !isFolder && (path as NSString).pathExtension == "app" }
    var parent: String { (path as NSString).deletingLastPathComponent }

    static func == (a: BrowserItem, b: BrowserItem) -> Bool { a.path == b.path && a.modified == b.modified && a.flags == b.flags && a.size == b.size }
    func hash(into h: inout Hasher) { h.combine(path) }
}

enum BrowserListing {
    /// Whether the folder can be read at all (macOS keeps the Trash and
    /// some folders private to apps without Full Disk Access).
    static func readable(_ folder: String) -> Bool {
        guard let d = opendir(folder) else { return false }
        closedir(d)
        return true
    }

    /// A folder's items (not sorted).  hidden: dot files and the Finder's
    /// invisible ones too.
    static func items(in folder: String, showHidden: Bool) -> [BrowserItem] {
        let entries = directoryEntries(folder)
        let names = Set(entries.map(\.name))
        var out: [BrowserItem] = []
        for e in entries {
            var name = e.name
            if name.hasPrefix("._") {
                let bare = String(name.dropFirst(2))
                // A sidecar beside its file is part of it; alone, it is the file.
                if bare.isEmpty || names.contains(bare) { continue }
                name = bare
            }
            if !showHidden && (name.hasPrefix(".") || name == "Icon\r") { continue }
            let path = folder == "/" ? "/" + name : folder + "/" + name
            guard let item = item(path, folder: e.isFolder, sidecar: names.contains("._" + name)) else { continue }
            if !showHidden && item.flags & FinderFlags.isInvisible != 0 { continue }
            out.append(item)
        }
        return out
    }

    /// One item; sidecar says whether "._" + its name is beside it (nil:
    /// look).
    static func item(_ path: String, folder: Bool? = nil, sidecar: Bool? = nil) -> BrowserItem? {
        var st = stat(), side = stat()
        let there = stat(path, &st) == 0
        let hasSidecar = sidecar ?? FileManager.default.fileExists(atPath: classicSidecarPath(path))
        guard there || hasSidecar else { return nil }
        let isDir = folder ?? (there && (st.st_mode & S_IFMT) == S_IFDIR)
        let name = (path as NSString).lastPathComponent
        // A file that is only a sidecar has the sidecar's dates.
        let dated = there ? st : (stat(classicSidecarPath(path), &side) == 0 ? side : st)
        let modified = Date(timeIntervalSince1970: TimeInterval(dated.st_mtimespec.tv_sec))
        let created = Date(timeIntervalSince1970: TimeInterval(dated.st_birthtimespec.tv_sec))
        let package = isDir && NSWorkspace.shared.isFilePackage(atPath: path)
        if isDir && !package {
            let flags = ClassicFileOps.folderFlags(path)
            return BrowserItem(path: path, name: name, isFolder: true, isClassic: false, type: 0, creator: 0, flags: flags,
                               size: -1, resourceSize: 0, modified: modified, created: created, kind: "Folder")
        }
        let dataSize = there ? Int(st.st_size) : 0
        if !package && (hasSidecar || appledouble_has_native(path)) {
            var s = mac_file_summary()
            if appledouble_read_summary(path, hasSidecar, &s) {
                defer { mac_file_summary_free(&s) }
                let fi = withUnsafeBytes(of: s.finder_info) { Array($0) }
                let type = fi[0..<4].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
                let creator = fi[4..<8].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
                let flags = UInt16(fi[8]) << 8 | UInt16(fi[9])
                if type != 0 || creator != 0 || s.rsrc_len > 0 || flags != 0 {
                    return BrowserItem(path: path, name: name, isFolder: false, isClassic: true, type: type, creator: creator,
                                       flags: flags, size: dataSize + s.rsrc_len, resourceSize: s.rsrc_len,
                                       modified: modified, created: created, kind: classicKind(type: type, flags: flags))
                }
            }
        }
        let kind = (try? URL(fileURLWithPath: path).resourceValues(forKeys: [.localizedTypeDescriptionKey]))?
            .localizedTypeDescription ?? "Document"
        return BrowserItem(path: path, name: name, isFolder: false, isClassic: false, type: 0, creator: 0, flags: 0,
                           size: package ? -1 : dataSize, resourceSize: 0, modified: modified, created: created, kind: kind)
    }

    /// A classic file's kind, in the Finder's words.
    static func classicKind(type: UInt32, flags: UInt16) -> String {
        if flags & FinderFlags.isAlias != 0 { return "Classic alias" }
        switch fourCCString(type) {
        case "APPL": return "Classic application"
        case "FNDR": return "Classic Finder"
        case "appe": return "Classic background application"
        case "cdev": return "Classic control panel"
        case "INIT": return "Classic extension"
        case "dfil": return "Desk accessory"
        case "FFIL", "ffil", "tfil", "suit": return "Font"
        case "shlb": return "Classic shared library"
        case "TEXT", "ttro": return "Classic text document"
        case "PICT": return "Classic picture"
        case "thme": return "Classic theme"
        default: return "Classic document"
        }
    }
}

/// The browser's sidebar places.
enum BrowserPlace: Hashable {
    case library, disk, home, volume(name: String, path: String), trash

    var title: String {
        switch self {
        case .library: return "Library"
        case .disk: return "Macintosh HD"
        case .home: return (BrowserPlaces.home as NSString).lastPathComponent
        case .volume(let name, _): return name
        case .trash: return "Trash"
        }
    }
    var path: String {
        switch self {
        case .library: return BrowserPlaces.library
        case .disk: return BrowserPlaces.disk
        case .home: return BrowserPlaces.home
        case .volume(_, let path): return path
        case .trash: return BrowserPlaces.trash
        }
    }
    var symbol: String {
        switch self {
        case .library: return "books.vertical"
        case .disk: return "internaldrive"
        case .home: return "house"
        case .volume: return "externaldrive"
        case .trash: return "trash"
        }
    }
}

enum BrowserPlaces {
    static var library: String { defaultLibrary() }
    static var disk: String { defaultDisk() }
    static var home: String {
        ProcessInfo.processInfo.environment["HOME"] ?? FileManager.default.homeDirectoryForCurrentUser.path
    }
    static var trash: String { home + "/.Trash" }

    /// Volumes Blue Box mounts in its runs: disk images extracted into the
    /// library (folders marked .classic-volume, named by the mark), and the
    /// host's own volumes.
    static func volumes() -> [BrowserPlace] {
        var out: [BrowserPlace] = []
        func look(_ dir: String, depth: Int) {
            for e in directoryEntries(dir) where e.isFolder && !e.name.hasPrefix(".") {
                let path = dir + "/" + e.name
                if let mark = try? String(contentsOfFile: path + "/.classic-volume", encoding: .utf8) {
                    let name = mark.trimmingCharacters(in: .whitespacesAndNewlines)
                    out.append(.volume(name: name.isEmpty ? e.name : name, path: path))
                } else if depth > 0 {
                    look(path, depth: depth - 1)
                }
            }
        }
        look(library, depth: 2)
        let keys: [URLResourceKey] = [.volumeLocalizedNameKey, .volumeIsBrowsableKey]
        for url in FileManager.default.mountedVolumeURLs(includingResourceValuesForKeys: keys, options: [.skipHiddenVolumes]) ?? [] {
            let v = try? url.resourceValues(forKeys: Set(keys))
            guard v?.volumeIsBrowsable != false else { continue }
            out.append(.volume(name: v?.volumeLocalizedName ?? url.lastPathComponent, path: url.path))
        }
        return out.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }

    /// The place a path is in (the deepest), for the sidebar's selection.
    static func place(of path: String, among places: [BrowserPlace]) -> BrowserPlace? {
        places.filter { path == $0.path || path.hasPrefix($0.path == "/" ? "/" : $0.path + "/") }
            .max { $0.path.count < $1.path.count }
    }
}

/// Icons and the file operations, shared by the browser's windows.
@MainActor final class BrowserCore {
    static let shared = BrowserCore()

    /// The operations (which open the catalog beside Macintosh HD), for
    /// any thread.
    nonisolated static let ops = ClassicFileOps(disk: defaultDisk())
    nonisolated var ops: ClassicFileOps { Self.ops }
    nonisolated let applications = ClassicApplicationIndex(roots: [defaultLibrary(), defaultDisk()])
    lazy var lookup = ClassicIconLookup(applications: applications) { path, meta in
        // Desk accessories, suitcases and other bundle-less files: the
        // System's icons, as the Finder had them.
        meta.resourceFork.withUnsafeBufferPointer {
            fileIcons(fork: ResourceFork(fork: $0.baseAddress, length: $0.count), rsrc: $0.baseAddress, length: $0.count,
                      type: meta.type, flags: meta.flags)
        }
    }
    /// Where operations run, one at a time, off the main thread.
    nonisolated let queue = DispatchQueue(label: "Blue Box Browser operations")
    private var iconCache: [String: NSImage] = [:]
    private var loading: Set<String> = []
    private var waiting: [String: [(NSImage) -> Void]] = [:]

    private init() {}

    private func key(_ item: BrowserItem) -> String { "\(item.path)|\(item.modified.timeIntervalSince1970)|\(item.flags)" }

    /// An item's icon now (a generic one while its own is read), and its
    /// own when it has been read, if that is another.
    func icon(_ item: BrowserItem, ready: @escaping (NSImage) -> Void) -> NSImage {
        if !item.isClassic { return NSWorkspace.shared.icon(forFile: item.path) }
        let k = key(item)
        if let cached = iconCache[k] { return cached }
        waiting[k, default: []].append(ready)
        if loading.insert(k).inserted {
            let lookup = lookup, path = item.path
            let alias = item.isAlias ? ops.aliasTarget(path) : nil
            DispatchQueue.global(qos: .userInitiated).async {
                var icons = lookup.icons(forFileAt: path)
                // An alias shows its original's icon.
                if icons.isEmpty, let alias { icons = lookup.icons(forFileAt: alias) }
                let images = icons
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        let image = images.isEmpty ? (alias.map { NSWorkspace.shared.icon(forFile: $0) } ?? Self.generic(item))
                            : Self.image(images)
                        self.iconCache[k] = image
                        self.loading.remove(k)
                        for f in self.waiting.removeValue(forKey: k) ?? [] { f(image) }
                    }
                }
            }
        }
        return Self.generic(item)
    }

    /// The icon to show before a classic file's own is read, or when it
    /// has none: an application's or a document's.
    static func generic(_ item: BrowserItem) -> NSImage {
        if item.isApplication { return NSWorkspace.shared.icon(for: .application) }
        return NSWorkspace.shared.icon(for: .data)
    }

    /// Classic icons as an image: blown up by whole pixels, so they stay
    /// sharp at the browser's sizes (the 16-pixel one where it is drawn
    /// that small).
    static func image(_ icons: [IconImage]) -> NSImage {
        guard let large = icons.max(by: { $0.size < $1.size }) else { return NSImage() }
        let image = NSImage(size: NSSize(width: large.size, height: large.size))
        for scale in [1, 2, 4, 8, 16] {
            let px = scaled(large, to: large.size * scale)
            guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px.size, pixelsHigh: px.size,
                                             bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                             colorSpaceName: .deviceRGB, bytesPerRow: px.size * 4, bitsPerPixel: 32),
                  let dst = rep.bitmapData else { continue }
            px.rgba.withUnsafeBufferPointer { dst.update(from: $0.baseAddress!, count: px.rgba.count) }
            rep.size = image.size
            image.addRepresentation(rep)
        }
        return image
    }

    /// Icons read again (a folder changed under them).
    func forget(_ paths: [String]) {
        let set = Set(paths)
        iconCache = iconCache.filter { !set.contains(String($0.key.split(separator: "|").first ?? "")) }
    }
}
#endif
