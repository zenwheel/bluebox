// Drags out of the classic program to the host: a TrackDrag that leaves
// the machine's windows goes on as an AppKit drag session, its flavors
// converted (machine_drag_out_*): text as text and RTF, files as file URLs,
// pictures as PNG and TIFF, promised files as file promises.  Text dropped
// on the Finder becomes a text file, as a clipping did.  Brought back over
// a classic window, the drag is the program's own again: its tracking
// handlers follow it and a drop there goes to its receive handlers.

#if canImport(AppKit)
import AppKit
import UniformTypeIdentifiers
import Toolbox

/// A view showing the Mac screen, or part of it, in host coordinates.
@MainActor protocol MacScreenView: NSView {
    func macPoint(screen: NSPoint) -> (Int32, Int32)
}

/// What the machine handed over, copied off the emulator thread.
struct ExportedRep: Sendable {
    let kind: Int32
    let data: Data?
}

/// A file promise that also carries the item's text (and RTF) and our own
/// marker type, so text editors take the text and the Finder the file.
final class ClassicFilePromise: NSFilePromiseProvider {
    var extra: [NSPasteboard.PasteboardType: Data] = [:]

    override func writableTypes(for pasteboard: NSPasteboard) -> [NSPasteboard.PasteboardType] {
        super.writableTypes(for: pasteboard) + Array(extra.keys) + [DragOut.marker]
    }

    override func writingOptions(forType type: NSPasteboard.PasteboardType, pasteboard: NSPasteboard)
        -> NSPasteboard.WritingOptions {
        extra[type] != nil || type == DragOut.marker ? [] : super.writingOptions(forType: type, pasteboard: pasteboard)
    }

    override func pasteboardPropertyList(forType type: NSPasteboard.PasteboardType) -> Any? {
        if type == DragOut.marker { return Data() }
        return extra[type] ?? super.pasteboardPropertyList(forType: type)
    }
}

/// What a file promise is for: a clipping of the text, or the sender's file.
struct PromiseInfo {
    let item: Int32
    let clipping: String?
}

@MainActor final class DragOut: NSObject, NSDraggingSource, NSPasteboardItemDataProvider, NSFilePromiseProviderDelegate {
    static let shared = DragOut()
    /// On every item we put out: our own drags coming back are told apart.
    nonisolated static let marker = NSPasteboard.PasteboardType("com.bluebox.classic-drag")

    private var lastEvent: NSEvent?
    private weak var lastView: MacScreenView?
    private var pending: [[ExportedRep]]?
    private var session: NSDraggingSession?
    private var finished = false
    private var providedItems: [ObjectIdentifier: Int32] = [:]
    /// Redraws the machine's windows while AppKit tracks the drag.
    var refresh: (() -> Void)?

    /// The promises' bookkeeping, shared with their writing queue.
    nonisolated private static let promiseLock = NSLock()
    nonisolated(unsafe) private static var promisesStarted = 0
    nonisolated(unsafe) private static var promisesDone = 0
    nonisolated(unsafe) private static var droppedFolder: String?
    private let promiseQueue: OperationQueue = {
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 1
        return q
    }()

    func install() {
        machine_set_drag_out_handler({ x, _ in
            guard let x = x?.pointee else { return false }
            var items: [[ExportedRep]] = []
            for i in 0..<Int(x.nitems) {
                let item = x.items[i]
                items.append((0..<Int(item.nreps)).map { k in
                    let r = item.reps[k]
                    return ExportedRep(kind: r.kind, data: r.data.map { Data(bytes: $0, count: r.size) })
                })
            }
            DispatchQueue.main.async {
                MainActor.assumeIsolated { DragOut.shared.offered(items) }
            }
            return true
        }, nil)
    }

    // MARK: Before the session

    /// A mouse-down or drag in one of the machine's views.
    func note(_ event: NSEvent, in view: MacScreenView) {
        lastEvent = event
        lastView = view
        tick()
    }

    /// From the host's timer and its mouse events: while a TrackDrag
    /// follows the mouse, whether the pointer is over our windows.
    func tick() {
        if machine_drag_out_tracking() { machine_drag_out_pointer(!pointerOverOurWindows()) }
        startIfPending()
    }

    private func pointerOverOurWindows() -> Bool {
        let n = NSWindow.windowNumber(at: NSEvent.mouseLocation, belowWindowWithWindowNumber: 0)
        return NSApp.window(withWindowNumber: n) != nil
    }

    private func offered(_ items: [[ExportedRep]]) {
        pending = items
        startIfPending()
    }

    private func startIfPending() {
        guard let items = pending else { return }
        pending = nil
        guard NSEvent.pressedMouseButtons & 1 != 0, let view = lastView, let event = lastEvent, let window = view.window
        else {
            // Let go before the session could start: nowhere to drop it.
            machine_drag_out_end(Int32(MACHINE_DRAG_OUT_CANCELLED), 0, 0, nil)
            return
        }
        finished = false
        providedItems = [:]
        Self.promiseLock.withLock {
            Self.promisesStarted = 0
            Self.promisesDone = 0
            Self.droppedFolder = nil
        }
        let at = view.convert(window.convertPoint(fromScreen: NSEvent.mouseLocation), from: nil)
        var dragging: [NSDraggingItem] = []
        for (i, reps) in items.enumerated() {
            guard let writer = writer(Int32(i), reps) else { continue }
            let item = NSDraggingItem(pasteboardWriter: writer)
            let image = Self.image(for: reps)
            let size = image.size
            item.setDraggingFrame(NSRect(x: at.x - size.width / 2, y: at.y - size.height / 2 + CGFloat(i) * 8,
                                         width: size.width, height: size.height), contents: image)
            dragging.append(item)
        }
        guard !dragging.isEmpty else {
            machine_drag_out_end(Int32(MACHINE_DRAG_OUT_CANCELLED), 0, 0, nil)
            return
        }
        let s = view.beginDraggingSession(with: dragging, event: event, source: self)
        s.animatesToStartingPositionsOnCancelOrFail = false
        session = s
    }

    /// The pasteboard writer for one item: a file promise for a promised
    /// file or a text clipping, otherwise an item of the converted data.
    private func writer(_ index: Int32, _ reps: [ExportedRep]) -> NSPasteboardWriting? {
        func rep(_ kind: Int32) -> ExportedRep? { reps.first { $0.kind == kind } }
        let text = rep(Int32(MACHINE_FLAVOR_TEXT)), rtf = rep(Int32(MACHINE_FLAVOR_RTF))
        let file = rep(Int32(MACHINE_FLAVOR_FILE)), image = rep(Int32(MACHINE_FLAVOR_IMAGE))
        if rep(Int32(MACHINE_FLAVOR_PROMISED_FILE)) != nil {
            let p = ClassicFilePromise(fileType: UTType.data.identifier, delegate: self)
            p.userInfo = PromiseInfo(item: index, clipping: nil)
            return p
        }
        if file == nil, let data = text?.data, let string = String(data: data, encoding: .utf8) {
            let p = ClassicFilePromise(fileType: UTType.utf8PlainText.identifier, delegate: self)
            p.userInfo = PromiseInfo(item: index, clipping: string)
            p.extra[.string] = data
            if let rtf = rtf?.data { p.extra[.rtf] = rtf }
            return p
        }
        let item = NSPasteboardItem()
        var lazy: [NSPasteboard.PasteboardType] = []
        item.setData(Data(), forType: Self.marker)
        if let file {
            if let data = file.data, let path = String(data: data, encoding: .utf8) {
                item.setString(URL(fileURLWithPath: path).absoluteString, forType: .fileURL)
            } else {
                lazy.append(.fileURL)
            }
        }
        if let rtf {
            if let data = rtf.data { item.setData(data, forType: .rtf) } else { lazy.append(.rtf) }
        }
        if let text {
            if let data = text.data { item.setData(data, forType: .string) } else { lazy.append(.string) }
        }
        if image != nil { lazy += [.png, .tiff] }
        if !lazy.isEmpty {
            item.setDataProvider(self, forTypes: lazy)
            providedItems[ObjectIdentifier(item)] = index
        }
        return item
    }

    private static func image(for reps: [ExportedRep]) -> NSImage {
        if let file = reps.first(where: { $0.kind == Int32(MACHINE_FLAVOR_FILE) })?.data,
           let path = String(data: file, encoding: .utf8) {
            let icon = NSWorkspace.shared.icon(forFile: path)
            icon.size = NSSize(width: 32, height: 32)
            return icon
        }
        if let data = reps.first(where: { $0.kind == Int32(MACHINE_FLAVOR_TEXT) })?.data,
           let text = String(data: data, encoding: .utf8) {
            let line = String(text.split(separator: "\n").first ?? "").prefix(40)
            let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 12),
                                                        .foregroundColor: NSColor.labelColor]
            let s = NSAttributedString(string: String(line), attributes: attrs)
            let size = NSSize(width: max(16, ceil(s.size().width) + 8), height: ceil(s.size().height) + 4)
            return NSImage(size: size, flipped: false) { r in
                NSColor.textBackgroundColor.withAlphaComponent(0.8).setFill()
                NSBezierPath(roundedRect: r, xRadius: 3, yRadius: 3).fill()
                s.draw(at: NSPoint(x: 4, y: 2))
                return true
            }
        }
        let icon = NSWorkspace.shared.icon(for: .data)
        icon.size = NSSize(width: 32, height: 32)
        return icon
    }

    // MARK: Data on request

    nonisolated static func fetch(_ item: Int32, _ kind: Int32, folder: String? = nil) -> machine_drag_data? {
        var out = machine_drag_data()
        return machine_drag_out_data(item, kind, folder, &out) ? out : nil
    }

    nonisolated func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem,
                                provideDataForType type: NSPasteboard.PasteboardType) {
        let id = ObjectIdentifier(item)
        let index = MainActor.assumeIsolated { providedItems[id] }
        guard let index else { return }
        switch type {
        case .png, .tiff:
            guard let d = Self.fetch(index, Int32(MACHINE_FLAVOR_IMAGE)), let base = d.data else { return }
            defer { free(base) }
            let w = Int(d.width), h = Int(d.height)
            guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8,
                                             samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                             colorSpaceName: .deviceRGB, bytesPerRow: w * 4, bitsPerPixel: 32),
                  let dest = rep.bitmapData else { return }
            memcpy(dest, base, w * h * 4)
            let data = type == .png ? rep.representation(using: .png, properties: [:]) : rep.tiffRepresentation
            if let data { item.setData(data, forType: type) }
        default:
            let kind = type == .fileURL ? MACHINE_FLAVOR_FILE : type == .rtf ? MACHINE_FLAVOR_RTF : MACHINE_FLAVOR_TEXT
            guard let d = Self.fetch(index, Int32(kind)), let base = d.data else { return }
            defer { free(base) }
            let data = Data(bytes: base, count: d.size)
            if type == .fileURL, let path = String(data: data, encoding: .utf8) {
                item.setString(URL(fileURLWithPath: path).absoluteString, forType: .fileURL)
            } else {
                item.setData(data, forType: type)
            }
        }
    }

    // MARK: File promises

    nonisolated func filePromiseProvider(_ filePromiseProvider: NSFilePromiseProvider, fileNameForType fileType: String)
        -> String {
        guard let info = filePromiseProvider.userInfo as? PromiseInfo, let text = info.clipping else { return "Untitled" }
        return Self.clippingName(text)
    }

    /// A clipping's name: its first words, as the Finder named them.
    nonisolated static func clippingName(_ text: String) -> String {
        let line = text.split(whereSeparator: \.isNewline).first.map(String.init) ?? ""
        var name = line.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "/", with: "-")
            .replacingOccurrences(of: ":", with: "-")
        if name.count > 31 { name = String(name.prefix(31)).trimmingCharacters(in: .whitespaces) }
        return (name.isEmpty ? "text clipping" : name) + ".txt"
    }

    nonisolated func operationQueue(for filePromiseProvider: NSFilePromiseProvider) -> OperationQueue {
        MainActor.assumeIsolated { promiseQueue }
    }

    nonisolated func filePromiseProvider(_ filePromiseProvider: NSFilePromiseProvider, writePromiseTo url: URL,
                                         completionHandler: @escaping (Error?) -> Void) {
        Self.promiseLock.withLock { Self.promisesStarted += 1 }
        defer { Self.promiseLock.withLock { Self.promisesDone += 1 } }
        guard let info = filePromiseProvider.userInfo as? PromiseInfo else {
            completionHandler(CocoaError(.fileWriteUnknown))
            return
        }
        let folder = url.deletingLastPathComponent()
        if let text = info.clipping {
            var target = url, n = 2
            let base = url.deletingPathExtension().lastPathComponent
            while FileManager.default.fileExists(atPath: target.path) {
                target = folder.appendingPathComponent("\(base) \(n).txt")
                n += 1
            }
            do {
                try Data(text.utf8).write(to: target)
                Self.promiseLock.withLock { Self.droppedFolder = folder.path }
                completionHandler(nil)
            } catch {
                completionHandler(error)
            }
            return
        }
        // The sender writes its file in the folder, under a name of its own.
        guard let d = Self.fetch(info.item, Int32(MACHINE_FLAVOR_PROMISED_FILE), folder: folder.path),
              let base = d.data else {
            completionHandler(CocoaError(.fileWriteUnknown))
            return
        }
        free(base)
        Self.promiseLock.withLock { Self.droppedFolder = folder.path }
        completionHandler(nil)
    }

    // MARK: The session

    nonisolated func draggingSession(_ session: NSDraggingSession,
                                     sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
        .copy // a file dragged out is copied, never moved
    }

    nonisolated func draggingSession(_ session: NSDraggingSession, movedTo screenPoint: NSPoint) {
        MainActor.assumeIsolated { refresh?() }
    }

    nonisolated func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint,
                                     operation: NSDragOperation) {
        MainActor.assumeIsolated { ended(at: screenPoint, operation: operation) }
    }

    private func ended(at screenPoint: NSPoint, operation: NSDragOperation) {
        session = nil
        // The button came up in the session, not in our views.
        let (x, y) = lastView?.macPoint(screen: screenPoint) ?? (0, 0)
        machine_post_mouse(x, y, 0)
        guard !finished else { return }
        finished = true
        if operation.isEmpty {
            machine_drag_out_end(Int32(MACHINE_DRAG_OUT_CANCELLED), x, y, nil)
            return
        }
        // A receiver taking a file promise writes it after the session
        // ends: TrackDrag goes on until it is written (the sender may be
        // asked for it), or until it is clear none was taken.
        DispatchQueue.global().async {
            let start = Date()
            while true {
                let (started, done) = Self.promiseLock.withLock { (Self.promisesStarted, Self.promisesDone) }
                let waited = Date().timeIntervalSince(start)
                if started > 0 ? done >= started || waited > 60 : waited > 0.5 { break }
                usleep(20_000)
            }
            let folder = Self.promiseLock.withLock { Self.droppedFolder }
            machine_drag_out_end(Int32(MACHINE_DRAG_OUT_DROPPED), x, y, folder)
        }
    }

    // MARK: Back over our windows

    static func isOurs(_ info: NSDraggingInfo) -> Bool { info.draggingSource is DragOut }

    /// Our drag over one of the machine's views (kind as machine_host_drag's).
    func over(_ kind: Int32, at: (Int32, Int32)) -> NSDragOperation {
        switch kind {
        case Int32(MACHINE_DRAG_LEAVE):
            machine_drag_out_over(false, 0, 0)
        case Int32(MACHINE_DRAG_DROP):
            finished = true
            machine_drag_out_end(Int32(MACHINE_DRAG_OUT_INSIDE), at.0, at.1, nil)
        default:
            machine_drag_out_over(true, at.0, at.1)
        }
        refresh?()
        return .copy
    }
}
#endif
