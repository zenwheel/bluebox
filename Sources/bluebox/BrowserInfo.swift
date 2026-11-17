// The Blue Box Browser's Get Info window, as the Finder's: an item's icon,
// name, kind, where it is, its dates and the size of both forks, and what
// can be changed: a classic file's type and creator, the label, the
// comment (the sidecar's, or the catalog's for a folder) and Locked (the
// host's lock, the macOS Finder's own).

#if canImport(AppKit)
import AppKit
import BlueBoxHost

@MainActor final class BrowserInfoWindow: NSWindowController, NSWindowDelegate, NSTextFieldDelegate, NSTextViewDelegate {
    private static var open: [String: BrowserInfoWindow] = [:]
    private var item: BrowserItem
    private weak var browser: BrowserWindowController?
    private let type = NSTextField(), creator = NSTextField()
    private let label = NSPopUpButton()
    private let comment = NSTextView()
    private let locked = NSButton(checkboxWithTitle: "Locked", target: nil, action: nil)
    private var commentChanged = false

    static func show(_ item: BrowserItem, browser: BrowserWindowController) {
        if let w = open[item.path] {
            w.showWindow(nil)
            return
        }
        let w = BrowserInfoWindow(item, browser: browser)
        open[item.path] = w
        w.showWindow(nil)
    }

    private init(_ item: BrowserItem, browser: BrowserWindowController) {
        self.item = item
        self.browser = browser
        let w = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 320, height: 520), styleMask: [.titled, .closable, .utilityWindow],
                        backing: .buffered, defer: false)
        w.title = "\(item.name) Info"
        w.isFloatingPanel = false
        w.hidesOnDeactivate = false
        super.init(window: w)
        w.delegate = self
        build()
        w.center()
    }

    @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

    private func build() {
        let ops = BrowserCore.shared.ops
        let info = ops.info(item.path)
        let icon = NSImageView()
        icon.image = BrowserCore.shared.icon(item) { [weak icon] image in icon?.image = image }
        icon.imageScaling = .scaleProportionallyUpOrDown
        icon.widthAnchor.constraint(equalToConstant: 48).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 48).isActive = true
        let name = NSTextField(labelWithString: item.name)
        name.font = .boldSystemFont(ofSize: 13)
        name.lineBreakMode = .byTruncatingMiddle
        let size = NSTextField(labelWithString: item.isFolder ? "Folder" : sizeText(item))
        size.textColor = .secondaryLabelColor
        let names = NSStackView(views: [name, size])
        names.orientation = .vertical
        names.alignment = .leading
        names.spacing = 2
        let header = NSStackView(views: [icon, names])
        header.spacing = 10

        func value(_ s: String) -> NSTextField {
            let t = NSTextField(wrappingLabelWithString: s)
            t.isSelectable = true
            t.preferredMaxLayoutWidth = 200
            return t
        }
        var rows: [[NSView]] = [[NSTextField(labelWithString: "Kind:"), value(item.kind)]]
        if !item.isFolder {
            let data = item.size - item.resourceSize
            let forks = item.isClassic ? "\(byteCount(data)) data, \(byteCount(item.resourceSize)) resource"
                : byteCount(max(0, item.size))
            rows.append([NSTextField(labelWithString: "Size:"), value(forks)])
        }
        rows.append([NSTextField(labelWithString: "Where:"), value((item.parent as NSString).abbreviatingWithTildeInPath)])
        rows.append([NSTextField(labelWithString: "Created:"), value(DateFormatter.localizedString(from: item.created, dateStyle: .long, timeStyle: .short))])
        rows.append([NSTextField(labelWithString: "Modified:"), value(DateFormatter.localizedString(from: item.modified, dateStyle: .long, timeStyle: .short))])
        if item.isAlias, let target = ops.aliasTarget(item.path) {
            rows.append([NSTextField(labelWithString: "Original:"), value((target as NSString).abbreviatingWithTildeInPath)])
        }
        if !item.isFolder {
            for (field, code) in [(type, info.meta.type), (creator, info.meta.creator)] {
                field.stringValue = code == 0 ? "" : fourCCString(code)
                field.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
                field.placeholderString = "none"
                field.delegate = self
                field.widthAnchor.constraint(equalToConstant: 64).isActive = true
            }
            let codes = NSStackView(views: [type, NSTextField(labelWithString: "Creator:"), creator])
            codes.spacing = 6
            rows.append([NSTextField(labelWithString: "Type:"), codes])
        }
        for l in ClassicLabel.all {
            label.addItem(withTitle: l.name)
            label.lastItem?.tag = l.index
            if l.index > 0 { label.lastItem?.image = BrowserWindowController.labelDot(l) }
        }
        label.selectItem(withTag: info.meta.label)
        label.target = self
        label.action = #selector(labelChanged(_:))
        rows.append([NSTextField(labelWithString: "Label:"), label])
        locked.state = info.locked ? .on : .off
        locked.target = self
        locked.action = #selector(lockChanged(_:))
        rows.append([NSView(), locked])

        let grid = NSGridView(views: rows)
        grid.column(at: 0).xPlacement = .trailing
        grid.rowAlignment = .firstBaseline
        grid.columnSpacing = 8
        grid.rowSpacing = 6
        for row in rows {
            (row[0] as? NSTextField)?.textColor = .secondaryLabelColor
        }

        let commentTitle = NSTextField(labelWithString: "Comments:")
        commentTitle.textColor = .secondaryLabelColor
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.borderType = .bezelBorder
        comment.string = info.meta.commentText
        comment.isRichText = false
        comment.font = .systemFont(ofSize: 12)
        comment.delegate = self
        comment.autoresizingMask = [.width]
        scroll.documentView = comment
        scroll.heightAnchor.constraint(equalToConstant: 80).isActive = true
        let note = NSTextField(wrappingLabelWithString: info.isClassic || item.isFolder
            ? "Classic programs see the label and comment; Locked is the Mac's own."
            : "Setting a type and creator makes this a classic file for Blue Box.")
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        note.textColor = .tertiaryLabelColor

        let stack = NSStackView(views: [header, grid, commentTitle, scroll, note])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 12
        stack.edgeInsets = NSEdgeInsets(top: 16, left: 16, bottom: 16, right: 16)
        for v in [scroll, note] { v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -32).isActive = true }
        window?.contentView = stack
        window?.setContentSize(stack.fittingSize)
    }

    private func change(_ what: String, _ f: @escaping @Sendable (inout ClassicMeta) -> Void) {
        let path = item.path
        browser?.perform("change", reload: [], select: false) { ops in
            try ops.update(path, f)
            return []
        }
    }

    @objc private func labelChanged(_ sender: NSPopUpButton) {
        let l = sender.selectedTag()
        change("label") { $0.label = l }
    }

    @objc private func lockChanged(_ sender: NSButton) {
        do {
            try BrowserCore.shared.ops.setLocked(item.path, sender.state == .on)
        } catch {
            sender.state = sender.state == .on ? .off : .on
            browser?.alert("“\(item.name)” could not be \(sender.state == .on ? "unlocked" : "locked").", info: error.localizedDescription)
        }
    }

    /// Four characters, as MacRoman; shorter ones padded with spaces.
    private static func code(_ s: String) -> UInt32? {
        let bytes = macRomanBytes(s, max: 4)
        guard !s.isEmpty else { return 0 }
        guard bytes.count == s.count, bytes.count <= 4 else { return nil }
        return (bytes + [UInt8](repeating: 0x20, count: 4 - bytes.count)).reduce(0) { $0 << 8 | UInt32($1) }
    }

    func controlTextDidEndEditing(_ note: Notification) {
        guard let field = note.object as? NSTextField, let value = Self.code(field.stringValue) else {
            NSSound.beep()
            return
        }
        let isType = field === type
        change("type") { m in if isType { m.type = value } else { m.creator = value } }
    }

    func textDidChange(_ notification: Notification) { commentChanged = true }

    func textDidEndEditing(_ notification: Notification) { saveComment() }

    private func saveComment() {
        guard commentChanged else { return }
        commentChanged = false
        let text = comment.string
        change("comment") { $0.commentText = text }
    }

    func windowWillClose(_ notification: Notification) {
        saveComment()
        Self.open[item.path] = nil
    }
}
#endif
