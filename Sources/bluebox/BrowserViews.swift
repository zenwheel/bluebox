// The Blue Box Browser's three views of a folder, as the Finder's: icons
// (a collection view), a list (an outline view whose folders open in
// place, sorted by any column) and columns (a browser).  Each shows its
// items' icons as they are read, opens them on a double-click, has the
// window's menu for them, drags them out and takes drops.

#if canImport(AppKit)
import AppKit
import BlueBoxHost

private let dropTypes: [NSPasteboard.PasteboardType] =
    [.fileURL] + NSFilePromiseReceiver.readableDraggedTypes.map { NSPasteboard.PasteboardType($0) }

private let dateFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateStyle = .medium
    f.timeStyle = .short
    f.doesRelativeDateFormatting = true
    return f
}()

func sizeText(_ item: BrowserItem) -> String {
    item.size < 0 ? "--" : ByteCountFormatter.string(fromByteCount: Int64(item.size), countStyle: .file)
}

// MARK: - Icons

@MainActor final class BrowserIconItem: NSCollectionViewItem {
    static let id = NSUserInterfaceItemIdentifier("BrowserIconItem")
    private let icon = NSImageView()
    private let name = NSTextField(wrappingLabelWithString: "")
    private let plate = NSView()
    var item: BrowserItem?

    override func loadView() {
        view = NSView()
        plate.wantsLayer = true
        plate.layer?.cornerRadius = 6
        name.alignment = .center
        name.maximumNumberOfLines = 2
        name.lineBreakMode = .byTruncatingMiddle
        name.font = .systemFont(ofSize: 12)
        name.wantsLayer = true
        name.layer?.cornerRadius = 4
        icon.imageScaling = .scaleProportionallyUpOrDown
        for v in [plate, icon, name] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            view.addSubview(v)
        }
        NSLayoutConstraint.activate([
            plate.centerXAnchor.constraint(equalTo: view.centerXAnchor), plate.topAnchor.constraint(equalTo: view.topAnchor, constant: 2),
            plate.widthAnchor.constraint(equalToConstant: 72), plate.heightAnchor.constraint(equalToConstant: 72),
            icon.centerXAnchor.constraint(equalTo: plate.centerXAnchor), icon.centerYAnchor.constraint(equalTo: plate.centerYAnchor),
            icon.widthAnchor.constraint(equalToConstant: 64), icon.heightAnchor.constraint(equalToConstant: 64),
            name.topAnchor.constraint(equalTo: plate.bottomAnchor, constant: 3),
            name.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            name.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, constant: -4),
        ])
        imageView = icon
        textField = name
    }

    func configure(_ item: BrowserItem, image: NSImage) {
        self.item = item
        icon.image = image
        icon.alphaValue = item.flags & FinderFlags.isInvisible != 0 ? 0.5 : 1
        let font = item.isAlias ? NSFontManager.shared.convert(.systemFont(ofSize: 12), toHaveTrait: .italicFontMask)
            : NSFont.systemFont(ofSize: 12)
        name.font = font
        name.stringValue = item.name
        update()
    }

    override var isSelected: Bool { didSet { update() } }
    override var highlightState: NSCollectionViewItem.HighlightState { didSet { update() } }

    private func update() {
        let on = isSelected || highlightState == .forSelection
        plate.layer?.backgroundColor = on ? NSColor.quaternaryLabelColor.cgColor : nil
        if on {
            name.layer?.backgroundColor = NSColor.selectedContentBackgroundColor.cgColor
            name.textColor = .alternateSelectedControlTextColor
        } else if let item, item.label > 0 {
            name.layer?.backgroundColor = BrowserWindowController.labelColor(ClassicLabel.all[item.label]).withAlphaComponent(0.35).cgColor
            name.textColor = .labelColor
        } else {
            name.layer?.backgroundColor = nil
            name.textColor = .labelColor
        }
    }
}

/// A collection view that opens on a double-click and asks for its menu.
@MainActor final class BrowserCollectionView: NSCollectionView {
    var onOpen: (() -> Void)?
    var onMenu: ((NSEvent) -> NSMenu?)?
    var onRename: (() -> Void)?
    /// Letters typed: the name so far (type-to-select).
    var onType: ((String) -> Void)?
    /// An arrow key with nothing selected: true for the first item (Down,
    /// Right), false for the last (Up, Left), as the Finder picks one.
    var onArrowFromNothing: ((Bool) -> Void)?
    private var typeSelect = TypeSelect()

    override func mouseDown(with event: NSEvent) {
        typeSelect.reset()
        super.mouseDown(with: event)
        if event.clickCount == 2 { onOpen?() }
    }

    override func menu(for event: NSEvent) -> NSMenu? { onMenu?(event) }

    override func keyDown(with event: NSEvent) {
        if event.keyCode == 36 || event.keyCode == 76 { onRename?(); return } // Return: rename, as in the Finder
        if event.modifierFlags.contains(.command), event.keyCode == 125 { onOpen?(); return } // Command-Down
        if let first = arrowFromNothing(event, selected: !selectionIndexPaths.isEmpty) {
            typeSelect.reset()
            onArrowFromNothing?(first)
            return
        }
        if event.modifierFlags.intersection([.command, .control, .option]).isEmpty, let chars = event.characters,
           let typed = typeSelect.feed(chars, at: event.timestamp) {
            onType?(typed)
            return
        }
        typeSelect.reset()
        super.keyDown(with: event)
    }
}

@MainActor final class BrowserIconView: NSObject, BrowserContentView, NSCollectionViewDataSource, NSCollectionViewDelegate {
    private weak var delegate: BrowserViewDelegate?
    private let scroll = NSScrollView()
    private let collection = BrowserCollectionView()
    private var items: [BrowserItem] = []
    private var location = ""
    var view: NSView { scroll }

    init(delegate: BrowserViewDelegate) {
        self.delegate = delegate
        super.init()
        let layout = NSCollectionViewFlowLayout()
        layout.itemSize = NSSize(width: 104, height: 112)
        layout.minimumInteritemSpacing = 8
        layout.minimumLineSpacing = 8
        layout.sectionInset = NSEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)
        collection.collectionViewLayout = layout
        collection.isSelectable = true
        collection.allowsMultipleSelection = true
        collection.backgroundColors = [.textBackgroundColor]
        collection.register(BrowserIconItem.self, forItemWithIdentifier: BrowserIconItem.id)
        collection.dataSource = self
        collection.delegate = self
        collection.registerForDraggedTypes(dropTypes)
        collection.setDraggingSourceOperationMask(.every, forLocal: true)
        collection.setDraggingSourceOperationMask([.copy, .move, .generic], forLocal: false)
        collection.onOpen = { [weak self] in self?.openSelection() }
        collection.onArrowFromNothing = { [weak self] first in
            guard let self, let item = first ? self.items.first : self.items.last else { return }
            self.select([item.path])
        }
        collection.onType = { [weak self] typed in
            guard let self, let i = TypeSelect.match(typed, in: self.items.map(\.name)) else { return }
            self.select([self.items[i].path])
        }
        collection.onRename = { [weak self] in
            guard let self, self.selectedItems.count == 1 else { return }
            self.delegate?.browserRename(self.selectedItems[0])
        }
        collection.onMenu = { [weak self] event in
            guard let self else { return nil }
            let at = self.collection.convert(event.locationInWindow, from: nil)
            if let ip = self.collection.indexPathForItem(at: at), !self.collection.selectionIndexPaths.contains(ip) {
                self.collection.selectionIndexPaths = [ip]
            } else if self.collection.indexPathForItem(at: at) == nil {
                self.collection.deselectAll(nil)
            }
            return self.delegate?.browserMenu(for: self.selectedItems)
        }
        scroll.documentView = collection
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = true
        scroll.backgroundColor = .textBackgroundColor
    }

    private func openSelection() {
        let s = selectedItems
        if !s.isEmpty { delegate?.browserOpen(s) }
    }

    var selectedItems: [BrowserItem] {
        collection.selectionIndexPaths.sorted().compactMap { items.indices.contains($0.item) ? items[$0.item] : nil }
    }

    func show(_ items: [BrowserItem], location: String) {
        self.items = items
        self.location = location
        collection.reloadData()
    }

    func select(_ paths: Set<String>) {
        let ips = Set(items.indices.filter { paths.contains(items[$0].path) }.map { IndexPath(item: $0, section: 0) })
        collection.selectionIndexPaths = ips
        if let first = ips.min() { collection.scrollToItems(at: [first], scrollPosition: .nearestHorizontalEdge) }
        delegate?.browserSelectionChanged()
    }

    func selectAll() { collection.selectAll(nil) }
    func focus() { collection.window?.makeFirstResponder(collection) }
    func refreshIcons() { collection.reloadData() }

    func collectionView(_ cv: NSCollectionView, numberOfItemsInSection section: Int) -> Int { items.count }

    func collectionView(_ cv: NSCollectionView, itemForRepresentedObjectAt ip: IndexPath) -> NSCollectionViewItem {
        let cell = cv.makeItem(withIdentifier: BrowserIconItem.id, for: ip)
        guard let c = cell as? BrowserIconItem, items.indices.contains(ip.item) else { return cell }
        let item = items[ip.item]
        let image = delegate?.browserIcon(item) { [weak c] image in
            if c?.item?.path == item.path { c?.imageView?.image = image }
        } ?? NSImage()
        c.configure(item, image: image)
        return c
    }

    func collectionView(_ cv: NSCollectionView, didSelectItemsAt ips: Set<IndexPath>) { delegate?.browserSelectionChanged() }
    func collectionView(_ cv: NSCollectionView, didDeselectItemsAt ips: Set<IndexPath>) { delegate?.browserSelectionChanged() }

    func collectionView(_ cv: NSCollectionView, canDragItemsAt ips: Set<IndexPath>, with event: NSEvent) -> Bool { true }

    func collectionView(_ cv: NSCollectionView, pasteboardWriterForItemAt ip: IndexPath) -> NSPasteboardWriting? {
        items.indices.contains(ip.item) ? delegate?.browserDragWriter(items[ip.item]) : nil
    }

    func collectionView(_ cv: NSCollectionView, draggingSession session: NSDraggingSession, willBeginAt point: NSPoint,
                        forItemsAt ips: Set<IndexPath>) {
        delegate?.browserDragBegan(ips.sorted().compactMap { items.indices.contains($0.item) ? items[$0.item] : nil })
    }

    /// Dropped on a folder: into it; elsewhere, into the folder shown.
    private func target(_ ip: IndexPath, _ op: NSCollectionView.DropOperation) -> String {
        if op == .on, items.indices.contains(ip.item), items[ip.item].isFolder { return items[ip.item].path }
        return location
    }

    func collectionView(_ cv: NSCollectionView, validateDrop info: NSDraggingInfo,
                        proposedIndexPath ip: AutoreleasingUnsafeMutablePointer<NSIndexPath>,
                        dropOperation op: UnsafeMutablePointer<NSCollectionView.DropOperation>) -> NSDragOperation {
        let path = target(ip.pointee as IndexPath, op.pointee)
        if path == location { op.pointee = .before; ip.pointee = NSIndexPath(forItem: items.count, inSection: 0) }
        return delegate?.browserValidateDrop(info, into: path) ?? []
    }

    func collectionView(_ cv: NSCollectionView, acceptDrop info: NSDraggingInfo, indexPath ip: IndexPath,
                        dropOperation op: NSCollectionView.DropOperation) -> Bool {
        delegate?.browserAcceptDrop(info, into: target(ip, op)) ?? false
    }
}

// MARK: - List

@MainActor final class BrowserOutlineView: NSOutlineView {
    var onMenu: ((NSEvent) -> NSMenu?)?
    var onRename: (() -> Void)?
    var onOpen: (() -> Void)?
    override func menu(for event: NSEvent) -> NSMenu? { onMenu?(event) }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 36 || event.keyCode == 76 { onRename?(); return }
        if event.modifierFlags.contains(.command), event.keyCode == 125 { onOpen?(); return }
        if let first = arrowFromNothing(event, selected: selectedRow >= 0), numberOfRows > 0 {
            let row = first ? 0 : numberOfRows - 1
            selectRowIndexes([row], byExtendingSelection: false)
            scrollRowToVisible(row)
            return
        }
        super.keyDown(with: event)
    }
}

/// An arrow key pressed with nothing selected: whether it picks the first
/// item (Down, Right) or the last (Up, Left); nil for any other key, or
/// when something is selected (the view moves the selection itself).
@MainActor func arrowFromNothing(_ event: NSEvent, selected: Bool) -> Bool? {
    guard !selected, event.modifierFlags.intersection([.command, .control, .option]).isEmpty else { return nil }
    switch event.keyCode {
    case 124, 125: return true  // Right, Down
    case 123, 126: return false // Left, Up
    default: return nil
    }
}

/// A list item: an item, and its folder's items once it is opened.
@MainActor final class BrowserNode: NSObject {
    let item: BrowserItem
    var children: [BrowserNode]?
    init(_ item: BrowserItem) { self.item = item }
}

@MainActor final class BrowserListView: NSObject, BrowserContentView, NSOutlineViewDataSource, NSOutlineViewDelegate {
    private weak var delegate: BrowserViewDelegate?
    private let scroll = NSScrollView()
    private let outline = BrowserOutlineView()
    private var roots: [BrowserNode] = []
    private var location = ""
    private var expanded: Set<String> = []
    var view: NSView { scroll }

    private enum Col {
        static let name = NSUserInterfaceItemIdentifier("name"), date = NSUserInterfaceItemIdentifier("date")
        static let size = NSUserInterfaceItemIdentifier("size"), kind = NSUserInterfaceItemIdentifier("kind")
    }

    init(delegate: BrowserViewDelegate) {
        self.delegate = delegate
        super.init()
        for (id, title, width) in [(Col.name, "Name", 300.0), (Col.date, "Date Modified", 170.0), (Col.size, "Size", 80.0),
                                   (Col.kind, "Kind", 160.0)] {
            let c = NSTableColumn(identifier: id)
            c.title = title
            c.width = width
            c.minWidth = 60
            c.sortDescriptorPrototype = NSSortDescriptor(key: id.rawValue, ascending: id != Col.date)
            if id == Col.size { c.headerCell.alignment = .right }
            outline.addTableColumn(c)
            if id == Col.name { outline.outlineTableColumn = c }
        }
        outline.style = .inset
        outline.usesAlternatingRowBackgroundColors = true
        outline.allowsMultipleSelection = true
        if !BrowserWindowController.ephemeral {
            outline.autosaveTableColumns = true
            outline.autosaveName = "BlueBoxBrowserList"
        }
        outline.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
        outline.sortDescriptors = [NSSortDescriptor(key: "name", ascending: true)]
        outline.dataSource = self
        outline.delegate = self
        outline.target = self
        outline.doubleAction = #selector(doubleClicked(_:))
        outline.registerForDraggedTypes(dropTypes)
        outline.setDraggingSourceOperationMask(.every, forLocal: true)
        outline.setDraggingSourceOperationMask([.copy, .move, .generic], forLocal: false)
        outline.onOpen = { [weak self] in self?.openSelection() }
        outline.onRename = { [weak self] in
            guard let self, self.selectedItems.count == 1 else { return }
            self.delegate?.browserRename(self.selectedItems[0])
        }
        outline.onMenu = { [weak self] event in
            guard let self else { return nil }
            let row = self.outline.row(at: self.outline.convert(event.locationInWindow, from: nil))
            if row >= 0, !self.outline.selectedRowIndexes.contains(row) {
                self.outline.selectRowIndexes([row], byExtendingSelection: false)
            } else if row < 0 {
                self.outline.deselectAll(nil)
            }
            return self.delegate?.browserMenu(for: self.selectedItems)
        }
        scroll.documentView = outline
        scroll.hasVerticalScroller = true
    }

    @objc private func doubleClicked(_ sender: Any?) {
        if outline.clickedRow >= 0 { openSelection() }
    }

    private func openSelection() {
        let s = selectedItems
        if !s.isEmpty { delegate?.browserOpen(s) }
    }

    var selectedItems: [BrowserItem] {
        outline.selectedRowIndexes.compactMap { (outline.item(atRow: $0) as? BrowserNode)?.item }
    }

    private func sort(_ list: [BrowserItem]) -> [BrowserItem] {
        guard let d = outline.sortDescriptors.first, let key = d.key else { return list }
        let up = d.ascending
        func byName(_ a: BrowserItem, _ b: BrowserItem) -> Bool { a.name.localizedStandardCompare(b.name) == .orderedAscending }
        return list.sorted { a, b in
            let less: Bool
            switch key {
            case "date": less = a.modified == b.modified ? byName(a, b) : a.modified < b.modified
            case "size": less = a.size == b.size ? byName(a, b) : a.size < b.size
            case "kind": less = a.kind == b.kind ? byName(a, b) : a.kind.localizedStandardCompare(b.kind) == .orderedAscending
            default: less = byName(a, b)
            }
            return up ? less : !less
        }
    }

    func show(_ items: [BrowserItem], location: String) {
        if location != self.location { expanded = [] }
        self.location = location
        roots = sort(items).map(BrowserNode.init)
        outline.reloadData()
        // Folders open before stay open, read again.
        for row in 0..<outline.numberOfRows {
            if let node = outline.item(atRow: row) as? BrowserNode, expanded.contains(node.item.path) {
                outline.expandItem(node)
            }
        }
    }

    func select(_ paths: Set<String>) {
        var rows = IndexSet()
        for row in 0..<outline.numberOfRows {
            if let n = outline.item(atRow: row) as? BrowserNode, paths.contains(n.item.path) { rows.insert(row) }
        }
        outline.selectRowIndexes(rows, byExtendingSelection: false)
        if let first = rows.first { outline.scrollRowToVisible(first) }
        delegate?.browserSelectionChanged()
    }

    func selectAll() { outline.selectAll(nil) }
    func focus() { outline.window?.makeFirstResponder(outline) }
    func refreshIcons() { outline.reloadData() }

    func outlineView(_ o: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        guard let node = item as? BrowserNode else { return roots.count }
        if node.children == nil { node.children = sort(delegate?.browserChildren(of: node.item.path) ?? []).map(BrowserNode.init) }
        return node.children?.count ?? 0
    }

    func outlineView(_ o: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        guard let node = item as? BrowserNode else { return roots[index] }
        return node.children?[index] ?? roots[0]
    }

    func outlineView(_ o: NSOutlineView, isItemExpandable item: Any) -> Bool { (item as? BrowserNode)?.item.isFolder ?? false }

    /// Type-to-select matches the names (the outline view does the rest).
    func outlineView(_ o: NSOutlineView, typeSelectStringFor col: NSTableColumn?, item: Any) -> String? {
        guard col == nil || col?.identifier == Col.name else { return nil }
        return (item as? BrowserNode)?.item.name
    }

    func outlineViewItemDidExpand(_ note: Notification) {
        if let n = note.userInfo?["NSObject"] as? BrowserNode { expanded.insert(n.item.path) }
    }

    func outlineViewItemDidCollapse(_ note: Notification) {
        if let n = note.userInfo?["NSObject"] as? BrowserNode {
            expanded.remove(n.item.path)
            n.children = nil
        }
    }

    func outlineView(_ o: NSOutlineView, sortDescriptorsDidChange old: [NSSortDescriptor]) {
        let selected = Set(selectedItems.map(\.path))
        show(roots.map(\.item), location: location)
        select(selected)
    }

    func outlineView(_ o: NSOutlineView, viewFor col: NSTableColumn?, item: Any) -> NSView? {
        guard let node = item as? BrowserNode, let col else { return nil }
        let i = node.item
        let id = col.identifier
        let cell = o.makeView(withIdentifier: id, owner: nil) as? NSTableCellView ?? makeCell(id)
        switch id {
        case Col.name:
            cell.textField?.stringValue = i.name
            cell.textField?.font = i.isAlias ? NSFontManager.shared.convert(.systemFont(ofSize: 13), toHaveTrait: .italicFontMask)
                : .systemFont(ofSize: 13)
            cell.imageView?.image = delegate?.browserIcon(i) { [weak cell] image in
                if (cell?.objectValue as? String) == i.path { cell?.imageView?.image = image }
            }
            cell.objectValue = i.path
            if let dot = cell.viewWithTag(7) as? NSImageView {
                dot.image = i.label > 0 ? BrowserWindowController.labelDot(ClassicLabel.all[i.label], size: 10) : nil
            }
        case Col.date: cell.textField?.stringValue = dateFormatter.string(from: i.modified)
        case Col.size: cell.textField?.stringValue = sizeText(i)
        default: cell.textField?.stringValue = i.kind
        }
        return cell
    }

    private func makeCell(_ id: NSUserInterfaceItemIdentifier) -> NSTableCellView {
        let c = NSTableCellView()
        c.identifier = id
        let text = NSTextField(labelWithString: "")
        text.lineBreakMode = .byTruncatingMiddle
        text.translatesAutoresizingMaskIntoConstraints = false
        c.addSubview(text)
        c.textField = text
        if id == Col.name {
            let image = NSImageView(), dot = NSImageView()
            dot.tag = 7
            for v in [image, dot] {
                v.translatesAutoresizingMaskIntoConstraints = false
                c.addSubview(v)
            }
            c.imageView = image
            NSLayoutConstraint.activate([
                image.leadingAnchor.constraint(equalTo: c.leadingAnchor, constant: 2),
                image.centerYAnchor.constraint(equalTo: c.centerYAnchor),
                image.widthAnchor.constraint(equalToConstant: 16), image.heightAnchor.constraint(equalToConstant: 16),
                text.leadingAnchor.constraint(equalTo: image.trailingAnchor, constant: 6),
                dot.leadingAnchor.constraint(equalTo: text.trailingAnchor, constant: 4),
                dot.trailingAnchor.constraint(lessThanOrEqualTo: c.trailingAnchor, constant: -2),
                dot.centerYAnchor.constraint(equalTo: c.centerYAnchor),
                dot.widthAnchor.constraint(equalToConstant: 10),
            ])
            text.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        } else {
            text.textColor = .secondaryLabelColor
            if id == Col.size { text.alignment = .right }
            NSLayoutConstraint.activate([
                text.leadingAnchor.constraint(equalTo: c.leadingAnchor, constant: 2),
                text.trailingAnchor.constraint(equalTo: c.trailingAnchor, constant: -2),
            ])
        }
        text.centerYAnchor.constraint(equalTo: c.centerYAnchor).isActive = true
        return c
    }

    func outlineViewSelectionDidChange(_ note: Notification) { delegate?.browserSelectionChanged() }

    func outlineView(_ o: NSOutlineView, pasteboardWriterForItem item: Any) -> NSPasteboardWriting? {
        (item as? BrowserNode).flatMap { delegate?.browserDragWriter($0.item) }
    }

    func outlineView(_ o: NSOutlineView, draggingSession session: NSDraggingSession, willBeginAt point: NSPoint,
                     forItems items: [Any]) {
        delegate?.browserDragBegan(items.compactMap { ($0 as? BrowserNode)?.item })
    }

    func outlineView(_ o: NSOutlineView, validateDrop info: NSDraggingInfo, proposedItem item: Any?,
                     proposedChildIndex index: Int) -> NSDragOperation {
        // On a folder: into it; anywhere else, into the folder shown.
        if let node = item as? BrowserNode, node.item.isFolder, index == NSOutlineViewDropOnItemIndex {
            return delegate?.browserValidateDrop(info, into: node.item.path) ?? []
        }
        o.setDropItem(nil, dropChildIndex: NSOutlineViewDropOnItemIndex)
        return delegate?.browserValidateDrop(info, into: location) ?? []
    }

    func outlineView(_ o: NSOutlineView, acceptDrop info: NSDraggingInfo, item: Any?, childIndex index: Int) -> Bool {
        let folder = (item as? BrowserNode).map { $0.item.isFolder ? $0.item.path : $0.item.parent } ?? location
        return delegate?.browserAcceptDrop(info, into: folder) ?? false
    }
}

// MARK: - Columns

@MainActor final class BrowserColumnNode: NSObject {
    let path: String
    let item: BrowserItem?
    var children: [BrowserColumnNode]?
    init(path: String, item: BrowserItem?) {
        self.path = path
        self.item = item
    }
}

@MainActor final class BrowserColumns: NSBrowser {
    var onMenu: ((NSEvent) -> NSMenu?)?
    var onRename: (() -> Void)?
    override func menu(for event: NSEvent) -> NSMenu? { onMenu?(event) }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 36 || event.keyCode == 76 { onRename?(); return }
        super.keyDown(with: event)
    }
}

@MainActor final class BrowserColumnView: NSObject, BrowserContentView, NSBrowserDelegate {
    private weak var delegate: BrowserViewDelegate?
    private let browser = BrowserColumns()
    private var root = BrowserColumnNode(path: "/", item: nil)
    private var location = ""
    private var settingPath = false
    var view: NSView { browser }

    init(delegate: BrowserViewDelegate) {
        self.delegate = delegate
        super.init()
        browser.delegate = self
        browser.separatesColumns = true
        browser.hasHorizontalScroller = true
        browser.allowsMultipleSelection = true
        browser.allowsEmptySelection = true
        browser.takesTitleFromPreviousColumn = false
        browser.isTitled = false
        browser.columnResizingType = .userColumnResizing
        browser.minColumnWidth = 180
        browser.setDefaultColumnWidth(220)
        browser.autohidesScroller = true
        browser.target = self
        browser.action = #selector(clicked(_:))
        browser.doubleAction = #selector(doubleClicked(_:))
        browser.registerForDraggedTypes(dropTypes)
        browser.setDraggingSourceOperationMask(.every, forLocal: true)
        browser.setDraggingSourceOperationMask([.copy, .move, .generic], forLocal: false)
        browser.onRename = { [weak self] in
            guard let self, self.selectedItems.count == 1 else { return }
            self.delegate?.browserRename(self.selectedItems[0])
        }
        browser.onMenu = { [weak self] event in
            guard let self else { return nil }
            return self.delegate?.browserMenu(for: self.selectedItems)
        }
    }

    var selectedItems: [BrowserItem] {
        let c = browser.selectedColumn
        guard c >= 0 else { return [] }
        return browser.selectedRowIndexes(inColumn: c)?.compactMap {
            (browser.item(atRow: $0, inColumn: c) as? BrowserColumnNode)?.item
        } ?? []
    }

    /// The columns start at the place the folder is in (or the disk's top).
    func show(_ items: [BrowserItem], location: String) {
        let selected = Set(selectedItems.map(\.path))
        let top = rootPath(for: location)
        if top != root.path || location == self.location {
            root = BrowserColumnNode(path: top, item: nil)
        } else {
            invalidate(root)
        }
        self.location = location
        browser.loadColumnZero()
        settingPath = true
        defer { settingPath = false }
        // Down the folders to the one shown.
        var node = root
        var column = 0
        let rel = location == top ? "" : String(location.dropFirst(top == "/" ? 1 : top.count + 1))
        for part in rel.split(separator: "/") {
            _ = children(node)
            guard let row = node.children?.firstIndex(where: { ($0.path as NSString).lastPathComponent == part }) else { break }
            browser.selectRow(row, inColumn: column)
            node = node.children![row]
            column += 1
        }
        select(selected)
    }

    private func invalidate(_ node: BrowserColumnNode) {
        node.children = nil
    }

    private func rootPath(for location: String) -> String {
        let places = [BrowserPlaces.library, BrowserPlaces.disk, BrowserPlaces.home]
        return places.filter { location == $0 || location.hasPrefix($0 + "/") }.max { $0.count < $1.count } ?? "/"
    }

    private func children(_ node: BrowserColumnNode) -> [BrowserColumnNode] {
        if let c = node.children { return c }
        let c = (delegate?.browserChildren(of: node.path) ?? []).map { BrowserColumnNode(path: $0.path, item: $0) }
        node.children = c
        return c
    }

    func select(_ paths: Set<String>) {
        guard !paths.isEmpty else { return }
        let c = max(0, browser.lastColumn)
        var rows = IndexSet()
        for (i, n) in (children(browser.parentForItems(inColumn: c) as? BrowserColumnNode ?? root)).enumerated()
            where paths.contains(n.path) { rows.insert(i) }
        if !rows.isEmpty { browser.selectRowIndexes(rows, inColumn: c) }
        delegate?.browserSelectionChanged()
    }

    func selectAll() { browser.selectAll(nil) }
    func focus() { browser.window?.makeFirstResponder(browser) }
    func refreshIcons() { browser.loadColumnZero() }

    @objc private func clicked(_ sender: Any?) {
        guard !settingPath else { return }
        let items = selectedItems
        // A folder chosen: it is the one shown (its column opens beside it).
        if items.count == 1, items[0].isFolder {
            delegate?.browserNavigate(to: items[0].path)
        } else if let parent = browser.parentForItems(inColumn: browser.selectedColumn) as? BrowserColumnNode {
            delegate?.browserNavigate(to: parent.path)
        }
        delegate?.browserSelectionChanged()
    }

    @objc private func doubleClicked(_ sender: Any?) {
        let items = selectedItems.filter { !$0.isFolder }
        if !items.isEmpty { delegate?.browserOpen(items) }
    }

    func rootItem(for browser: NSBrowser) -> Any? { root }

    func browser(_ b: NSBrowser, numberOfChildrenOfItem item: Any?) -> Int {
        children((item as? BrowserColumnNode) ?? root).count
    }

    func browser(_ b: NSBrowser, child index: Int, ofItem item: Any?) -> Any {
        children((item as? BrowserColumnNode) ?? root)[index]
    }

    func browser(_ b: NSBrowser, isLeafItem item: Any?) -> Bool { !((item as? BrowserColumnNode)?.item?.isFolder ?? true) }

    func browser(_ b: NSBrowser, objectValueForItem item: Any?) -> Any? {
        (item as? BrowserColumnNode)?.item?.name ?? ""
    }

    /// Type-to-select matches the names, not the icon drawn before them.
    func browser(_ b: NSBrowser, typeSelectStringForRow row: Int, inColumn column: Int) -> String? {
        (b.item(atRow: row, inColumn: column) as? BrowserColumnNode)?.item?.name ?? ""
    }

    /// Each row drawn: its icon (as it is read) before its name, an
    /// attachment in the browser's text cell.
    func browser(_ b: NSBrowser, willDisplayCell cell: Any, atRow row: Int, column: Int) {
        // While a column loads, its items come from its parent, not from it.
        let list = children(b.parentForItems(inColumn: column) as? BrowserColumnNode ?? root)
        guard let cell = cell as? NSCell, list.indices.contains(row), let item = list[row].item else { return }
        let image = delegate?.browserIcon(item) { [weak b] _ in
            // Read now: drawn again, this time from the cache.
            @MainActor func redraw(_ v: NSView) {
                v.needsDisplay = true
                v.subviews.forEach(redraw)
            }
            if let b { redraw(b) }
        } ?? NSImage()
        let icon = image.copy() as? NSImage ?? image
        icon.size = NSSize(width: 16, height: 16)
        let attachment = NSTextAttachment()
        attachment.image = icon
        attachment.bounds = NSRect(x: 0, y: -3, width: 16, height: 16)
        let text = NSMutableAttributedString(attachment: attachment)
        let font = item.isAlias ? NSFontManager.shared.convert(.systemFont(ofSize: 13), toHaveTrait: .italicFontMask)
            : NSFont.systemFont(ofSize: 13)
        text.append(NSAttributedString(string: "  " + item.name, attributes: [.font: font]))
        cell.attributedStringValue = text
    }

    func browser(_ b: NSBrowser, canDragRowsWith rows: IndexSet, inColumn column: Int, with event: NSEvent) -> Bool { true }

    func browser(_ b: NSBrowser, writeRowsWith rows: IndexSet, inColumn column: Int, to pasteboard: NSPasteboard) -> Bool {
        let items = rows.compactMap { (b.item(atRow: $0, inColumn: column) as? BrowserColumnNode)?.item }
        delegate?.browserDragBegan(items)
        let writers = items.compactMap { delegate?.browserDragWriter($0) }
        pasteboard.clearContents()
        return !writers.isEmpty && pasteboard.writeObjects(writers)
    }

    func browser(_ b: NSBrowser, validateDrop info: NSDraggingInfo, proposedRow row: UnsafeMutablePointer<Int>,
                 column: UnsafeMutablePointer<Int>, dropOperation: UnsafeMutablePointer<NSBrowser.DropOperation>) -> NSDragOperation {
        delegate?.browserValidateDrop(info, into: dropFolder(b, row.pointee, column.pointee, dropOperation.pointee)) ?? []
    }

    func browser(_ b: NSBrowser, acceptDrop info: NSDraggingInfo, atRow row: Int, column: Int,
                 dropOperation: NSBrowser.DropOperation) -> Bool {
        delegate?.browserAcceptDrop(info, into: dropFolder(b, row, column, dropOperation)) ?? false
    }

    private func dropFolder(_ b: NSBrowser, _ row: Int, _ column: Int, _ op: NSBrowser.DropOperation) -> String {
        if op == .on, row >= 0, let n = b.item(atRow: row, inColumn: column) as? BrowserColumnNode, n.item?.isFolder == true {
            return n.path
        }
        return (b.parentForItems(inColumn: max(0, column)) as? BrowserColumnNode)?.path ?? location
    }
}
#endif
