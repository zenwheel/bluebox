// The Blue Box Browser's names edited in place, its column view's preview
// column, and Quick Look.
//
// A name is edited in a field laid over it, as the Finder's is: Return
// (or a click elsewhere) renames the item through ClassicFileOps, so its
// sidecar and catalog entries follow; Escape leaves it as it was.  The
// preview column shows a file selected alone in the column view: its
// icon, name, kind, size, dates, type and creator.  Quick Look (the space
// bar, File › Quick Look) shows the selection in the system's panel; a
// classic file macOS can't show is given Blue Box's own preview page (the
// one its Quick Look extension shows in the Finder), made here.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Quartz

/// Where a view shows an item's name, for the field that edits it: a
/// rectangle in the view's own coordinates (BrowserContentView.view).
struct BrowserNameSpot {
    var rect: NSRect
    var font: NSFont
    var centered: Bool
    var wraps: Bool
}

@MainActor final class BrowserNameEditor: NSObject, NSTextFieldDelegate {
    let item: BrowserItem
    let field = NSTextField()
    private var done: ((String?) -> Void)?
    private let spot: BrowserNameSpot

    /// done: the new name, or nil when the edit was cancelled.
    init(item: BrowserItem, spot: BrowserNameSpot, done: @escaping (String?) -> Void) {
        self.item = item
        self.spot = spot
        self.done = done
        super.init()
    }

    func begin(in view: NSView) {
        field.stringValue = item.name
        field.font = spot.font
        field.alignment = spot.centered ? .center : .natural
        field.isEditable = true
        field.isSelectable = true
        field.isBordered = true
        field.isBezeled = false
        field.drawsBackground = true
        field.backgroundColor = .textBackgroundColor
        field.focusRingType = .exterior
        field.usesSingleLineMode = !spot.wraps
        field.cell?.wraps = spot.wraps
        field.cell?.isScrollable = !spot.wraps
        field.lineBreakMode = spot.wraps ? .byCharWrapping : .byClipping
        field.delegate = self
        field.frame = spot.rect
        fit()
        view.addSubview(field)
        view.window?.makeFirstResponder(field)
        if let editor = field.currentEditor() {
            editor.selectedRange = BrowserNames.editableRange(item.name, isFolder: item.isFolder)
        }
    }

    /// As wide as the name (within the spot), as the Finder's field grows.
    private func fit() {
        guard !spot.wraps else { return }
        let width = (field.stringValue as NSString).size(withAttributes: [.font: spot.font]).width + 16
        var r = spot.rect
        r.size.width = min(spot.rect.width, max(60, width))
        field.frame = r
    }

    func controlTextDidChange(_ note: Notification) { fit() }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        switch selector {
        case #selector(NSResponder.cancelOperation(_:)):
            finish(nil)
            return true
        case #selector(NSResponder.insertNewline(_:)), #selector(NSResponder.insertTab(_:)),
             #selector(NSResponder.insertBacktab(_:)):
            finish(field.stringValue)
            return true
        default:
            return false
        }
    }

    /// A click elsewhere ends it with the name as typed.
    func controlTextDidEndEditing(_ note: Notification) { finish(field.stringValue) }

    /// Ended (the window or the view going away): as typed.
    func end() { finish(field.stringValue) }

    func cancel() { finish(nil) }

    private func finish(_ name: String?) {
        guard let done else { return }
        self.done = nil
        field.delegate = nil
        field.removeFromSuperview()
        done(name)
    }
}

// MARK: - The preview column

@MainActor final class BrowserPreviewController: NSViewController {
    weak var delegate: BrowserViewDelegate?
    private let icon = NSImageView()
    private let name = NSTextField(wrappingLabelWithString: "")
    private var grid = NSGridView()
    private static let dates: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        return f
    }()

    override func loadView() {
        // As tall as the column it is put in.
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 220, height: 420))
        v.autoresizingMask = [.width, .height]
        icon.imageScaling = .scaleProportionallyUpOrDown
        name.alignment = .center
        name.font = .boldSystemFont(ofSize: 13)
        name.maximumNumberOfLines = 3
        for item in [icon, name] as [NSView] {
            item.translatesAutoresizingMaskIntoConstraints = false
            v.addSubview(item)
        }
        NSLayoutConstraint.activate([
            icon.topAnchor.constraint(equalTo: v.topAnchor, constant: 24),
            icon.centerXAnchor.constraint(equalTo: v.centerXAnchor),
            icon.widthAnchor.constraint(equalToConstant: 128), icon.heightAnchor.constraint(equalToConstant: 128),
            name.topAnchor.constraint(equalTo: icon.bottomAnchor, constant: 12),
            name.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 16),
            name.trailingAnchor.constraint(equalTo: v.trailingAnchor, constant: -16),
        ])
        view = v
    }

    /// The browser sets the leaf it shows here.
    override var representedObject: Any? {
        didSet { show((representedObject as? BrowserColumnNode)?.item) }
    }

    private func show(_ item: BrowserItem?) {
        _ = view
        // The facts laid out afresh for each item.
        grid.removeFromSuperview()
        grid = NSGridView()
        grid.rowSpacing = 4
        grid.columnSpacing = 6
        grid.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(grid)
        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: name.bottomAnchor, constant: 14),
            grid.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            grid.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 10),
            grid.trailingAnchor.constraint(lessThanOrEqualTo: view.trailingAnchor, constant: -10),
        ])
        guard let item else {
            icon.image = nil
            name.stringValue = ""
            return
        }
        name.stringValue = item.name
        icon.image = delegate?.browserIcon(item) { [weak self] image in
            if (self?.representedObject as? BrowserColumnNode)?.item?.path == item.path { self?.icon.image = image }
        }
        let rows = ClassicItemFacts.rows(kind: item.kind, dataSize: max(0, item.size - item.resourceSize),
                                         resourceSize: item.resourceSize, created: item.created, modified: item.modified,
                                         type: item.type, creator: item.creator, isClassic: item.isClassic, dates: Self.dates)
        for row in rows {
            let label = NSTextField(labelWithString: row.label)
            label.textColor = .secondaryLabelColor
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            label.alignment = .right
            let value = NSTextField(wrappingLabelWithString: row.value)
            value.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            value.preferredMaxLayoutWidth = 130
            value.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            grid.addRow(with: [label, value])
        }
        if grid.numberOfColumns == 2 { grid.column(at: 0).xPlacement = .trailing }
    }
}

// MARK: - Quick Look

private func escaped(_ s: String) -> String {
    s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
        .replacingOccurrences(of: ">", with: "&gt;")
}

/// An item for the Quick Look panel: the file itself, or Blue Box's page
/// for it, named for the file.
final class BrowserPreviewItem: NSObject, QLPreviewItem, @unchecked Sendable {
    let path: String
    let title: String
    var url: URL
    init(path: String, title: String, url: URL) {
        self.path = path
        self.title = title
        self.url = url
    }
    var previewItemURL: URL! { url }
    var previewItemTitle: String! { title }
}

/// Blue Box's own previews of classic files, made off the main thread
/// into a temporary folder of their own (cleared when the panel closes).
@MainActor final class BrowserPreviews {
    static let shared = BrowserPreviews()
    private let queue = DispatchQueue(label: "Blue Box Browser previews")
    private var folder: URL?
    private var made: [String: URL] = [:]

    private func place() -> URL {
        if let folder { return folder }
        let f = FileManager.default.temporaryDirectory.appendingPathComponent("Blue Box Previews-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: f, withIntermediateDirectories: true)
        folder = f
        return f
    }

    /// The item for one selected: the file, or a page ("Reading…" at
    /// first) and ready(item) when the page is made.
    func item(for item: BrowserItem, ready: @escaping @MainActor @Sendable (BrowserPreviewItem) -> Void) -> BrowserPreviewItem {
        guard ClassicPreviewPage.wanted(name: item.name, isClassic: item.isClassic, isFolder: item.isFolder) else {
            return BrowserPreviewItem(path: item.path, title: item.name, url: item.url)
        }
        let key = "\(item.path)|\(item.modified.timeIntervalSince1970)"
        if let url = made[key] { return BrowserPreviewItem(path: item.path, title: item.name, url: url) }
        let dir = place().appendingPathComponent(UUID().uuidString)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let waiting = dir.appendingPathComponent("waiting.html")
        try? Data("<!doctype html><html><body style=\"font: 13px -apple-system; color: gray; text-align: center; padding-top: 40%\">Reading “\(escaped(item.name))”…</body></html>".utf8)
            .write(to: waiting)
        let shown = BrowserPreviewItem(path: item.path, title: item.name, url: waiting)
        let lookup = BrowserCore.shared.lookup, path = item.path
        let page = dir.appendingPathComponent("preview.html")
        queue.async {
            let html = ClassicPreviewPage(path: path, lookup: lookup)?.selfContainedHTML
            let ok = html.map { (try? Data($0.utf8).write(to: page)) != nil } ?? false
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    // No page (not classic after all): the file itself.
                    shown.url = ok ? page : URL(fileURLWithPath: path)
                    if ok { self.made[key] = page }
                    ready(shown)
                }
            }
        }
        return shown
    }

    /// The pages made, gone.
    func clear() {
        if let folder { try? FileManager.default.removeItem(at: folder) }
        folder = nil
        made = [:]
    }
}
#endif
