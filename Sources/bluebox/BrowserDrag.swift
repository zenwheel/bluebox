// Dragging in and out of the Blue Box Browser.
//
// Out (the decision, PLAN.md M23): a classic file leaves as a copy whose
// resource fork and Finder information are the file's own (macOS extended
// attributes), not a "._" sidecar beside it.  That is the host's own form
// of a classic file: the macOS Finder keeps it through copies, Archive
// Utility's zips and AirDrop carry it, other emulators read it, and on a
// volume without extended attributes (FAT, SMB) macOS writes the "._"
// AppleDouble file itself.  A sidecar beside it would be hidden in the
// Finder and dropped by most of what the user then does with the file.
// For somewhere neither survives (mail, the web) the browser's Save as
// MacBinary… writes a .bin.  The drag is a file promise, written when the
// drop lands (ClassicFileOps.exportForHost); folders in Blue Box's own
// places go the same way, so the classic files in them do.  Host files and
// folders elsewhere are dragged as themselves (file URLs: the Finder may
// move them).
//
// In: files from the Finder are copied, or moved on the same volume, by
// ClassicFileOps, which turns forks of their own into sidecars (and a
// folder's Finder information into the catalog).  File promises (Mail's
// attachments, a classic program's drag out of a Blue Box run) are taken
// into the folder and adopted the same way.  Between browser windows, items
// move or copy with their sidecars, as the Finder's do.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import UniformTypeIdentifiers

/// A classic file promised to a drop: written with its own forks.
final class BrowserFilePromise: NSFilePromiseProvider {
    override func writableTypes(for pasteboard: NSPasteboard) -> [NSPasteboard.PasteboardType] {
        super.writableTypes(for: pasteboard) + [BrowserDrag.pathsType]
    }

    override func writingOptions(forType type: NSPasteboard.PasteboardType, pasteboard: NSPasteboard)
        -> NSPasteboard.WritingOptions {
        type == BrowserDrag.pathsType ? [] : super.writingOptions(forType: type, pasteboard: pasteboard)
    }

    override func pasteboardPropertyList(forType type: NSPasteboard.PasteboardType) -> Any? {
        if type == BrowserDrag.pathsType { return userInfo as? String }
        return super.pasteboardPropertyList(forType: type)
    }
}

@MainActor final class BrowserDrag: NSObject, NSFilePromiseProviderDelegate {
    static let shared = BrowserDrag()
    /// The paths of the items a browser drags (also on the pasteboard by
    /// Copy), so a drop in a browser moves the originals.
    nonisolated static let pathsType = NSPasteboard.PasteboardType("com.bluebox.browser-paths")

    private var dragging: [String] = []
    private let promiseQueue: OperationQueue = {
        let q = OperationQueue()
        q.qualityOfService = .userInitiated
        return q
    }()

    func began(_ items: [BrowserItem]) { dragging = items.map(\.path) }

    /// Whether an item leaves as a promise: a classic file, or a folder in
    /// Blue Box's places.
    private func promised(_ item: BrowserItem) -> Bool {
        if item.isFolder {
            let places = [BrowserPlaces.library, BrowserPlaces.disk]
            return places.contains { item.path.hasPrefix($0 + "/") }
        }
        return item.isClassic
    }

    func writer(for item: BrowserItem) -> NSPasteboardWriting? {
        guard promised(item) else { return item.url as NSURL }
        let type = item.isFolder ? UTType.folder : UTType.data
        let p = BrowserFilePromise(fileType: type.identifier, delegate: self)
        p.userInfo = item.path
        return p
    }

    // MARK: The promise

    nonisolated func filePromiseProvider(_ provider: NSFilePromiseProvider, fileNameForType fileType: String) -> String {
        ((provider.userInfo as? String).map { ($0 as NSString).lastPathComponent }) ?? "Untitled"
    }

    nonisolated func operationQueue(for provider: NSFilePromiseProvider) -> OperationQueue {
        MainActor.assumeIsolated { promiseQueue }
    }

    nonisolated func filePromiseProvider(_ provider: NSFilePromiseProvider, writePromiseTo url: URL,
                                         completionHandler: @escaping (Error?) -> Void) {
        guard let src = provider.userInfo as? String else {
            completionHandler(CocoaError(.fileWriteUnknown))
            return
        }
        let ops = BrowserCore.ops
        var dest = url.path
        if ops.exists(dest) {
            let dir = url.deletingLastPathComponent().path
            dest = dir + "/" + ops.freeName(url.lastPathComponent, in: dir, copy: false)
        }
        do {
            try ops.exportForHost(src, to: dest)
            completionHandler(nil)
        } catch {
            completionHandler(error)
        }
    }

    // MARK: Drops

    /// The browsers' own drag, or one from elsewhere.
    private func ours(_ info: NSDraggingInfo) -> [String]? {
        guard let view = info.draggingSource as? NSView, view.window?.windowController is BrowserWindowController else {
            return nil
        }
        return dragging
    }

    private func fileURLs(_ info: NSDraggingInfo) -> [URL] {
        info.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
    }

    private func sameVolume(_ a: String, _ b: String) -> Bool {
        var sa = stat(), sb = stat()
        return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev
    }

    func validate(_ info: NSDraggingInfo, into folder: String) -> NSDragOperation {
        guard access(folder, W_OK) == 0 || folder == BrowserPlaces.trash else { return [] }
        let sources = ours(info) ?? fileURLs(info).map(\.path)
        if sources.isEmpty {
            let promises = info.draggingPasteboard.canReadObject(forClasses: [NSFilePromiseReceiver.self], options: nil)
            return promises && folder != BrowserPlaces.trash ? .copy : []
        }
        for s in sources where folder == s || folder.hasPrefix(s + "/") { return [] }
        if folder == BrowserPlaces.trash { return .delete.union(.move) }
        let mask = info.draggingSourceOperationMask
        if !mask.contains(.move) || NSEvent.modifierFlags.contains(.option) { return .copy }
        return sources.allSatisfy { sameVolume($0, folder) } ? .move : .copy
    }

    func accept(_ info: NSDraggingInfo, into folder: String, browser: BrowserWindowController) -> Bool {
        let op = validate(info, into: folder)
        guard !op.isEmpty else { return false }
        if let sources = ours(info) ?? Optional(fileURLs(info).map(\.path)), !sources.isEmpty {
            if folder == BrowserPlaces.trash {
                browser.perform("move to the Trash", reload: [], select: false) { ops in
                    try sources.map { src in
                        try ops.moveToTrash(src) { url in
                            var out: NSURL?
                            try FileManager.default.trashItem(at: url, resultingItemURL: &out)
                            return (out as URL?) ?? url
                        }
                    }
                }
            } else {
                browser.transfer(sources, into: folder, move: op == .move)
            }
            return true
        }
        // Promised files: written into the folder, then made Blue Box's own form.
        guard let receivers = info.draggingPasteboard.readObjects(forClasses: [NSFilePromiseReceiver.self], options: nil)
            as? [NSFilePromiseReceiver], !receivers.isEmpty else { return false }
        let ops = BrowserCore.shared.ops
        for r in receivers {
            r.receivePromisedFiles(atDestination: URL(fileURLWithPath: folder), options: [:], operationQueue: promiseQueue) { url, error in
                guard error == nil else { return }
                ops.adopt(url.path)
            }
        }
        return true
    }
}
#endif
