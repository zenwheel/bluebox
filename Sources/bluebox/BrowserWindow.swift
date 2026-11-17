// The Blue Box Browser: a window of Blue Box.app that looks and works like
// the macOS Finder's, for Blue Box's library, its Macintosh HD and the
// user's own files, which keeps classic files whole (ClassicFileOps):
// a sidebar of places, a toolbar (back and forward, the views, search),
// icon, list and column views, a path bar and a status bar.  Classic
// programs and documents open in Blue Box runs, as the shell launches
// them; host files and applications with macOS, by the user's choice
// here, never a guest's.  Windows come from File > New Browser Window,
// the Dock menu, the shell, `bluebox browser [folder]` and the "Open in
// Blue Box" service, and take tabs.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Quartz
import UniformTypeIdentifiers

/// What a browser view tells its window.
@MainActor protocol BrowserViewDelegate: AnyObject {
    var location: String { get }
    func browserOpen(_ items: [BrowserItem])
    func browserMenu(for items: [BrowserItem]) -> NSMenu
    func browserSelectionChanged()
    func browserDragWriter(_ item: BrowserItem) -> NSPasteboardWriting?
    func browserDragBegan(_ items: [BrowserItem])
    func browserValidateDrop(_ info: NSDraggingInfo, into folder: String) -> NSDragOperation
    func browserAcceptDrop(_ info: NSDraggingInfo, into folder: String) -> Bool
    func browserIcon(_ item: BrowserItem, ready: @escaping (NSImage) -> Void) -> NSImage
    func browserChildren(of folder: String) -> [BrowserItem]
    func browserNavigate(to folder: String)
    func browserRename(_ item: BrowserItem)
    /// A click on the name of the item alone selected: edited after the
    /// double-click time, unless another click comes first.
    func browserClickedName(_ item: BrowserItem)
    func browserCancelRename()
    func browserQuickLook()
}

/// A view of a folder's items: icons, a list or columns.
@MainActor protocol BrowserContentView: AnyObject {
    var view: NSView { get }
    var selectedItems: [BrowserItem] { get }
    func show(_ items: [BrowserItem], location: String)
    func select(_ paths: Set<String>)
    func selectAll()
    func focus()
    func refreshIcons()
    /// Where the item's name is drawn (scrolled into sight), for editing.
    func nameSpot(for path: String) -> BrowserNameSpot?
    /// The item's icon on the screen (Quick Look zooms from it).
    func screenFrame(for path: String) -> NSRect?
}

enum BrowserMode: Int { case icons, list, columns }

@MainActor final class BrowserWindowController: NSWindowController, NSWindowDelegate, NSToolbarDelegate,
    NSMenuItemValidation, NSSearchFieldDelegate, BrowserViewDelegate, @preconcurrency QLPreviewPanelDataSource, @preconcurrency QLPreviewPanelDelegate {
    static var windows: [BrowserWindowController] = []
    /// A window drawn for a test (BLUEBOX_BROWSER_SNAPSHOT): nothing of it
    /// is saved in the user's defaults, and BLUEBOX_BROWSER_MODE (icons,
    /// list, columns) picks its view.
    static let ephemeral = ProcessInfo.processInfo.environment["BLUEBOX_BROWSER_SNAPSHOT"] != nil
    /// The shell's model, which launches programs.
    static weak var model: ShellModel?

    private(set) var location: String
    private var back: [String] = [], forward: [String] = []
    private var mode: BrowserMode
    private var showHidden = false
    private var items: [BrowserItem] = []
    private var search = ""
    private var searchGeneration = 0
    private var watcher: DispatchSourceFileSystemObject?
    private var places: [BrowserPlace] = []

    private let split = NSSplitViewController()
    private let sidebar = BrowserSidebar()
    private let content = NSViewController()
    private let host = NSView()
    private let pathControl = NSPathControl()
    private let status = NSTextField(labelWithString: "")
    private var views: [BrowserMode: BrowserContentView] = [:]
    private var current: BrowserContentView { view(for: mode) }
    /// The name being edited in place, and a click's edit to come.
    private var editor: BrowserNameEditor?
    private var pendingRename: DispatchWorkItem?
    private var reloadAfterEdit = false
    private var previewItems: [BrowserPreviewItem] = []
    private var modeControl: NSSegmentedControl?
    private var historyControl: NSSegmentedControl?
    private weak var searchField: NSSearchField?

    // MARK: Opening windows

    /// A new browser window at path (the library by default), with the
    /// paths given selected, or a new tab of window.
    @discardableResult
    static func open(at path: String? = nil, selecting: [String] = [], tabOf window: NSWindow? = nil) -> BrowserWindowController {
        let c = BrowserWindowController(location: path ?? BrowserPlaces.library)
        windows.append(c)
        if ephemeral {
            // Never on the screen.
        } else if let window, let w = c.window {
            window.addTabbedWindow(w, ordered: .above)
            w.makeKeyAndOrderFront(nil)
        } else {
            c.showWindow(nil)
        }
        if !selecting.isEmpty { c.current.select(Set(selecting)) }
        if !ephemeral { NSApp.activate(ignoringOtherApps: true) }
        return c
    }

    /// The item at path shown in a browser window: its folder's, selected.
    static func reveal(_ path: String) {
        open(at: (path as NSString).deletingLastPathComponent, selecting: [path])
    }

    init(location: String) {
        self.location = (location as NSString).standardizingPath
        if Self.ephemeral {
            let wanted = ProcessInfo.processInfo.environment["BLUEBOX_BROWSER_MODE"] ?? ""
            mode = ["icons": .icons, "list": .list, "columns": .columns][wanted] ?? .icons
        } else {
            mode = BrowserMode(rawValue: UserDefaults.standard.integer(forKey: "BrowserMode")) ?? .icons
        }
        let w = BrowserNSWindow(contentRect: NSRect(x: 0, y: 0, width: 920, height: 560),
                         styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                         backing: .buffered, defer: false)
        w.tabbingIdentifier = "BlueBoxBrowser"
        w.tabbingMode = .automatic
        w.isReleasedWhenClosed = false
        super.init(window: w)
        _ = BrowserCore.ops // the catalog opened, before the first listing reads it
        w.delegate = self
        w.keyFilter = { [weak self] event in
            (self?.views[.columns] as? BrowserColumnView)?.handleKey(event) ?? false
        }
        w.clickFilter = { [weak self] event in
            guard let self, self.mode == .columns else { return nil }
            return (self.views[.columns] as? BrowserColumnView)?.handleClick(event)
        }
        build()
        if !Self.ephemeral { w.setFrameAutosaveName("BlueBoxBrowser") }
        if BrowserWindowController.windows.contains(where: { $0.window?.isVisible == true }) {
            w.setFrameTopLeftPoint(NSPoint(x: w.frame.minX + 24, y: w.frame.maxY - 24))
        } else if Self.ephemeral || !w.setFrameUsingName("BlueBoxBrowser") {
            w.center()
        }
        go(to: self.location, remember: false)
    }

    @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

    // MARK: Building the window

    private func build() {
        guard let w = window else { return }
        sidebar.onChoose = { [weak self] place in self?.go(to: place.path) }
        sidebar.onDrop = { [weak self] info, path, accept in
            guard let self else { return [] }
            if accept { return self.browserAcceptDrop(info, into: path) ? .copy : [] }
            return self.browserValidateDrop(info, into: path)
        }
        let sideItem = NSSplitViewItem(sidebarWithViewController: sidebar)
        sideItem.minimumThickness = 160
        sideItem.maximumThickness = 320
        split.addSplitViewItem(sideItem)

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.spacing = 0
        stack.alignment = .leading
        host.translatesAutoresizingMaskIntoConstraints = false
        pathControl.pathStyle = .standard
        pathControl.controlSize = .small
        pathControl.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        pathControl.target = self
        pathControl.action = #selector(pathClicked(_:))
        pathControl.focusRingType = .none
        pathControl.backgroundColor = .clear
        pathControl.translatesAutoresizingMaskIntoConstraints = false
        status.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        status.textColor = .secondaryLabelColor
        status.alignment = .center
        status.lineBreakMode = .byTruncatingMiddle
        status.translatesAutoresizingMaskIntoConstraints = false
        let pathBar = bar(pathControl, inset: 6), statusBar = bar(status, inset: 0)
        stack.addArrangedSubview(host)
        stack.addArrangedSubview(divider())
        stack.addArrangedSubview(pathBar)
        stack.addArrangedSubview(divider())
        stack.addArrangedSubview(statusBar)
        for v in stack.arrangedSubviews { v.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true }
        // Below the toolbar (the window's content runs under it, for the
        // sidebar), on the window's own background.
        let pane = BrowserPane()
        stack.translatesAutoresizingMaskIntoConstraints = false
        pane.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: pane.safeAreaLayoutGuide.topAnchor),
            stack.bottomAnchor.constraint(equalTo: pane.bottomAnchor),
            stack.leadingAnchor.constraint(equalTo: pane.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: pane.trailingAnchor),
        ])
        content.view = pane
        let contentItem = NSSplitViewItem(viewController: content)
        contentItem.minimumThickness = 320
        split.addSplitViewItem(contentItem)
        if !Self.ephemeral { split.splitView.autosaveName = "BlueBoxBrowserSplit" }
        w.contentViewController = split
        w.setContentSize(NSSize(width: 920, height: 560))

        let toolbar = NSToolbar(identifier: "BlueBoxBrowser")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        toolbar.allowsUserCustomization = true
        toolbar.autosavesConfiguration = !Self.ephemeral
        w.toolbar = toolbar
        w.toolbarStyle = .unified
        w.titlebarSeparatorStyle = .automatic
        reloadPlaces()
        attach(mode)
    }

    private func bar(_ v: NSView, inset: CGFloat) -> NSView {
        let b = NSView()
        b.translatesAutoresizingMaskIntoConstraints = false
        b.addSubview(v)
        NSLayoutConstraint.activate([
            b.heightAnchor.constraint(equalToConstant: 24),
            v.leadingAnchor.constraint(equalTo: b.leadingAnchor, constant: 8 + inset),
            v.trailingAnchor.constraint(equalTo: b.trailingAnchor, constant: -8),
            v.centerYAnchor.constraint(equalTo: b.centerYAnchor),
        ])
        return b
    }

    private func divider() -> NSView {
        let d = NSBox()
        d.boxType = .separator
        d.translatesAutoresizingMaskIntoConstraints = false
        return d
    }

    private func view(for mode: BrowserMode) -> BrowserContentView {
        if let v = views[mode] { return v }
        let v: BrowserContentView
        switch mode {
        case .icons: v = BrowserIconView(delegate: self)
        case .list: v = BrowserListView(delegate: self)
        case .columns: v = BrowserColumnView(delegate: self)
        }
        views[mode] = v
        return v
    }

    private func attach(_ m: BrowserMode) {
        editor?.end()
        let selected = Set(views[mode]?.selectedItems.map(\.path) ?? [])
        host.subviews.forEach { $0.removeFromSuperview() }
        mode = m
        let v = current.view
        v.translatesAutoresizingMaskIntoConstraints = false
        host.addSubview(v)
        NSLayoutConstraint.activate([
            v.leadingAnchor.constraint(equalTo: host.leadingAnchor), v.trailingAnchor.constraint(equalTo: host.trailingAnchor),
            v.topAnchor.constraint(equalTo: host.topAnchor), v.bottomAnchor.constraint(equalTo: host.bottomAnchor),
        ])
        current.show(items, location: location)
        current.select(selected)
        modeControl?.selectedSegment = m.rawValue
        if !Self.ephemeral { UserDefaults.standard.set(m.rawValue, forKey: "BrowserMode") }
        current.focus()
    }

    func reloadPlaces() {
        let volumes = BrowserPlaces.volumes()
        let classic = volumes.filter { if case .volume(_, let p) = $0 { return p.hasPrefix(BrowserPlaces.library + "/") }; return false }
        let host = volumes.filter { !classic.contains($0) }
        sidebar.sections = [("Blue Box", [.library, .disk] + classic), ("Places", [.home, .trash]), ("Locations", host)]
        places = [.library, .disk, .home, .trash] + volumes
        sidebar.select(BrowserPlaces.place(of: location, among: places))
    }

    // MARK: Going places

    /// Show a folder (remember: on the Back list).
    func go(to path: String, remember: Bool = true) {
        editor?.end()
        let target = (path as NSString).standardizingPath
        if remember && target != location {
            back.append(location)
            forward.removeAll()
        }
        location = target
        if !search.isEmpty {
            search = ""
            searchField?.stringValue = ""
        }
        reload()
        watch()
        let title = location == "/" ? "/" : FileManager.default.displayName(atPath: location)
        window?.title = title
        window?.representedURL = URL(fileURLWithPath: location)
        window?.tab.title = title
        pathControl.url = URL(fileURLWithPath: location)
        sidebar.select(BrowserPlaces.place(of: location, among: places))
        historyControl?.setEnabled(!back.isEmpty, forSegment: 0)
        historyControl?.setEnabled(!forward.isEmpty, forSegment: 1)
    }

    func browserNavigate(to folder: String) {
        guard folder != location else { return }
        editor?.end()
        // The column view's own path: no new listing in it.
        back.append(location)
        forward.removeAll()
        location = folder
        items = sorted(BrowserListing.items(in: location, showHidden: showHidden))
        watch()
        window?.title = FileManager.default.displayName(atPath: location)
        pathControl.url = URL(fileURLWithPath: location)
        sidebar.select(BrowserPlaces.place(of: location, among: places))
        historyControl?.setEnabled(true, forSegment: 0)
        historyControl?.setEnabled(false, forSegment: 1)
        updateStatus()
    }

    /// Read the folder again and show it, keeping the selection.
    func reload() {
        // Not under a name being edited: after it.
        if editor != nil { reloadAfterEdit = true; return }
        if !search.isEmpty { runSearch(); return }
        let selected = Set(current.selectedItems.map(\.path))
        items = sorted(BrowserListing.items(in: location, showHidden: showHidden))
        current.show(items, location: location)
        current.select(selected)
        updateStatus()
    }

    private func sorted(_ list: [BrowserItem]) -> [BrowserItem] {
        list.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    /// The folder watched: its listing changes come in as they happen.
    private func watch() {
        watcher?.cancel()
        let fd = Darwin.open(location, O_EVTONLY)
        guard fd >= 0 else { return }
        let s = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete], queue: .main)
        s.setEventHandler { [weak self] in
            MainActor.assumeIsolated {
                guard let self else { return }
                // Settle a burst of changes (a copy writing many files).
                NSObject.cancelPreviousPerformRequests(withTarget: self, selector: #selector(self.changed), object: nil)
                self.perform(#selector(self.changed), with: nil, afterDelay: 0.15)
            }
        }
        s.setCancelHandler { Darwin.close(fd) }
        s.resume()
        watcher = s
    }

    @objc private func changed() {
        if !FileManager.default.fileExists(atPath: location) {
            go(to: (location as NSString).deletingLastPathComponent, remember: false)
            return
        }
        reload()
    }

    private func updateStatus() {
        let n = items.count
        let selected = current.selectedItems.count
        var text = selected > 0 ? "\(selected) of \(n) selected" : "\(n) item\(n == 1 ? "" : "s")"
        if !search.isEmpty { text = "\(n) found" + (selected > 0 ? ", \(selected) selected" : "") }
        if n == 0 && search.isEmpty && !BrowserListing.readable(location) {
            status.stringValue = location == BrowserPlaces.trash
                ? "macOS keeps the Trash private: give Blue Box Full Disk Access to see it (Empty Trash still works)"
                : "Blue Box can't read this folder"
            return
        }
        if let v = try? URL(fileURLWithPath: location).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
           let free = v.volumeAvailableCapacityForImportantUsage {
            text += ", " + ByteCountFormatter.string(fromByteCount: free, countStyle: .file) + " available"
        }
        status.stringValue = text
    }

    @objc private func pathClicked(_ sender: NSPathControl) {
        if let url = sender.clickedPathItem?.url { go(to: url.path) }
    }

    // MARK: Search

    func controlTextDidChange(_ note: Notification) {
        guard let field = note.object as? NSSearchField else { return }
        search = field.stringValue.trimmingCharacters(in: .whitespaces)
        if search.isEmpty { reload() } else { runSearch() }
    }

    /// Everything under the folder whose name holds the search (ignoring
    /// case and diacritics), read off the main thread.
    private func runSearch() {
        searchGeneration += 1
        let generation = searchGeneration, root = location, text = search, hidden = showHidden
        status.stringValue = "Searching\u{2026}"
        DispatchQueue.global(qos: .userInitiated).async {
            var found: [BrowserItem] = []
            var folders = [root]
            while let folder = folders.popLast(), found.count < 2000 {
                for item in BrowserListing.items(in: folder, showHidden: hidden) {
                    if item.name.range(of: text, options: [.caseInsensitive, .diacriticInsensitive]) != nil { found.append(item) }
                    if item.isFolder { folders.append(item.path) }
                }
            }
            let results = found
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard generation == self.searchGeneration, !self.search.isEmpty else { return }
                    self.items = self.sorted(results)
                    self.current.show(self.items, location: self.location)
                    self.updateStatus()
                }
            }
        }
    }

    // MARK: The views' questions

    func browserSelectionChanged() {
        updateStatus()
        if previewing { refreshPreview() }
    }

    func browserIcon(_ item: BrowserItem, ready: @escaping (NSImage) -> Void) -> NSImage {
        BrowserCore.shared.icon(item, ready: ready)
    }

    func browserChildren(of folder: String) -> [BrowserItem] {
        sorted(BrowserListing.items(in: folder, showHidden: showHidden))
    }

    func browserMenu(for items: [BrowserItem]) -> NSMenu {
        let menu = NSMenu()
        func add(_ title: String, _ action: Selector, _ key: String = "", _ mods: NSEvent.ModifierFlags = .command) {
            let item = menu.addItem(withTitle: title, action: action, keyEquivalent: key)
            item.keyEquivalentModifierMask = mods
            item.target = self
        }
        if items.isEmpty {
            add("New Folder", #selector(newFolder(_:)), "n", [.command, .shift])
            add("Get Info", #selector(getInfo(_:)), "i")
            menu.addItem(.separator())
            add("Paste", #selector(paste(_:)), "v")
            add(showHidden ? "Hide Hidden Files" : "Show Hidden Files", #selector(toggleHidden(_:)), ".", [.command, .shift])
            add("Show in Finder", #selector(showInFinder(_:)))
            if location == BrowserPlaces.trash { add("Empty Trash", #selector(emptyTrash(_:))) }
            return menu
        }
        add("Open", #selector(openSelection(_:)), "o")
        if canShowOriginal(items) { add("Show Original", #selector(showOriginal(_:)), "r") }
        add(items.count == 1 ? "Quick Look “\(items[0].name)”" : "Quick Look \(items.count) Items", #selector(quickLook(_:)), "y")
        menu.addItem(.separator())
        add("Get Info", #selector(getInfo(_:)), "i")
        if items.count == 1 { add("Rename", #selector(rename(_:))) }
        add("Duplicate", #selector(duplicate(_:)), "d")
        add("Make Alias", #selector(makeAlias(_:)), "a", [.command, .control])
        if canPutBack(items) {
            add("Put Back", #selector(putBack(_:)), "\u{8}")
        } else {
            add("Move to Trash", #selector(moveToTrash(_:)), "\u{8}")
        }
        menu.addItem(.separator())
        add("Copy", #selector(copy(_:)), "c")
        if items.contains(where: { !$0.isFolder && $0.isClassic }) {
            add("Save as MacBinary\u{2026}", #selector(saveMacBinary(_:)))
        }
        if items.contains(where: { isImportable($0) }) { add("Import into Library", #selector(importIntoLibrary(_:))) }
        add("Show in Finder", #selector(showInFinder(_:)))
        menu.addItem(.separator())
        let labels = NSMenuItem(title: "Label", action: nil, keyEquivalent: "")
        labels.submenu = labelMenu(current: items.count == 1 ? items[0].label : -1)
        menu.addItem(labels)
        return menu
    }

    /// The labels, each with its colour, the current one checked.
    func labelMenu(current: Int) -> NSMenu {
        let menu = NSMenu()
        for label in ClassicLabel.all {
            let item = menu.addItem(withTitle: label.name, action: #selector(setLabel(_:)), keyEquivalent: "")
            item.tag = label.index
            item.target = self
            item.state = label.index == current ? .on : .off
            if label.index > 0 { item.image = Self.labelDot(label) }
        }
        return menu
    }

    static func labelColor(_ label: ClassicLabel) -> NSColor {
        NSColor(srgbRed: CGFloat(label.red) / 65535, green: CGFloat(label.green) / 65535, blue: CGFloat(label.blue) / 65535,
                alpha: 1)
    }

    static func labelDot(_ label: ClassicLabel, size: CGFloat = 12) -> NSImage {
        NSImage(size: NSSize(width: size, height: size), flipped: false) { r in
            labelColor(label).setFill()
            NSBezierPath(ovalIn: r.insetBy(dx: 1, dy: 1)).fill()
            return true
        }
    }

    private func isImportable(_ item: BrowserItem) -> Bool {
        let ext = (item.path as NSString).pathExtension.lowercased()
        return !item.isFolder && (archiveExtensions.contains(ext)
            || ["bin", "macbin", "img", "image", "dsk", "dc42", "toast", "iso", "hfs", "cdr", "dmg"].contains(ext)
            || isMacArchive(type: item.type, creator: item.creator, name: item.name, data: [])
            || isMacDiskImage(type: item.type, creator: item.creator))
    }

    // MARK: Opening

    func browserOpen(_ items: [BrowserItem]) {
        var documents: [String: [String]] = [:]
        var orphans: [BrowserItem] = []
        for item in items {
            var item = item
            if item.isAlias {
                guard let target = BrowserCore.shared.ops.aliasTarget(item.path), let t = BrowserListing.item(target) else {
                    brokenAlias(item)
                    continue
                }
                item = t
            }
            if item.isFolder {
                if items.count == 1 { go(to: item.path) } else { Self.open(at: item.path) }
            } else if item.isApplication {
                Self.model?.openProgram(path: item.path)
            } else if item.isClassic && item.creator != 0 && item.type != 0 {
                if let app = BrowserCore.shared.applications.applications(creator: item.creator).first {
                    documents[app, default: []].append(item.path)
                } else {
                    orphans.append(item)
                }
            } else {
                // A host file or application: macOS opens it, as the user asked here.
                NSWorkspace.shared.open(item.url)
            }
        }
        for (app, docs) in documents { Self.model?.openDocuments(docs, application: app) }
        for item in orphans { chooseApplication(for: item) }
    }

    /// No application in Blue Box has the document's creator: one chosen
    /// in the library opens it.
    private func chooseApplication(for item: BrowserItem) {
        let a = NSAlert()
        a.messageText = "There is no application to open “\(item.name)”."
        a.informativeText = "Its creator is ‘\(fourCCString(item.creator))’ and its type ‘\(fourCCString(item.type))’. "
            + "Import the application that made it into the library, or choose one to open it with."
        a.addButton(withTitle: "Choose Application\u{2026}")
        a.addButton(withTitle: "Cancel")
        guard let w = window else { return }
        a.beginSheetModal(for: w) { response in
            guard response == .alertFirstButtonReturn else { return }
            MainActor.assumeIsolated {
                let panel = NSOpenPanel()
                panel.directoryURL = URL(fileURLWithPath: BrowserPlaces.library)
                panel.prompt = "Open"
                panel.message = "Choose a classic application to open “\(item.name)”"
                panel.beginSheetModal(for: w) { r in
                    guard r == .OK, let url = panel.url else { return }
                    MainActor.assumeIsolated {
                        guard let app = BrowserListing.item(url.path), app.isApplication else {
                            self.alert("“\(url.lastPathComponent)” is not a classic application.")
                            return
                        }
                        Self.model?.openDocuments([item.path], application: app.path)
                    }
                }
            }
        }
    }

    // MARK: Actions

    private var selection: [BrowserItem] { current.selectedItems }

    @objc func openSelection(_ sender: Any?) { browserOpen(selection) }

    /// An item's original: a classic alias's, a symbolic link's or a macOS
    /// alias's; nil for an item that is none of them.  found is false when
    /// the original isn't there.
    static func original(of item: BrowserItem) -> (path: String, found: Bool)? {
        if item.isAlias {
            guard let target = BrowserCore.shared.ops.aliasTarget(item.path) else { return ("", false) }
            return (target, BrowserCore.shared.ops.exists(target))
        }
        let values = try? item.url.resourceValues(forKeys: [.isSymbolicLinkKey, .isAliasFileKey])
        guard values?.isSymbolicLink == true || values?.isAliasFile == true else { return nil }
        if let url = try? URL(resolvingAliasFileAt: item.url, options: [.withoutUI, .withoutMounting]),
           FileManager.default.fileExists(atPath: url.path) {
            return (url.path, true)
        }
        return ("", false)
    }

    func canShowOriginal(_ items: [BrowserItem]) -> Bool {
        !items.isEmpty && items.allSatisfy { $0.isAlias || Self.original(of: $0) != nil }
    }

    /// Command-R, Show Original, as the Finder has it: each alias's
    /// original selected in its folder (this window for the first folder,
    /// a window each for others); an alias whose original is gone asks
    /// what to do about it.
    @objc func showOriginal(_ sender: Any?) {
        var folders: [String: [String]] = [:], order: [String] = []
        for item in selection {
            guard let o = Self.original(of: item) else { continue }
            guard o.found else { brokenAlias(item); return }
            let folder = (o.path as NSString).deletingLastPathComponent
            if folders[folder] == nil { order.append(folder) }
            folders[folder, default: []].append(o.path)
        }
        for (i, folder) in order.enumerated() {
            let originals = folders[folder] ?? []
            if i == 0 {
                go(to: folder)
                current.select(Set(originals))
            } else {
                Self.open(at: folder, selecting: originals)
            }
        }
    }

    /// An alias whose original can't be found: the Finder's choices, to
    /// delete it, fix it (choose another original) or leave it.
    func brokenAlias(_ item: BrowserItem) {
        let a = NSAlert()
        a.messageText = "The alias “\(item.name)” can't be opened because the original item can't be found."
        a.informativeText = "It may have been moved, renamed or deleted."
        a.addButton(withTitle: "OK")
        a.addButton(withTitle: "Delete Alias")
        let fixable = item.isAlias
        if fixable { a.addButton(withTitle: "Fix Alias\u{2026}") }
        guard let w = window else { return }
        a.beginSheetModal(for: w) { response in
            MainActor.assumeIsolated {
                switch response {
                case .alertSecondButtonReturn:
                    self.perform("move to the Trash", reload: [], select: false) { ops in
                        [try ops.moveToTrash(item.path) { url in
                            var out: NSURL?
                            try FileManager.default.trashItem(at: url, resultingItemURL: &out)
                            return (out as URL?) ?? url
                        }]
                    }
                case .alertThirdButtonReturn where fixable:
                    let panel = NSOpenPanel()
                    panel.canChooseDirectories = true
                    panel.prompt = "Choose"
                    panel.message = "Choose a new original for the alias “\(item.name)”"
                    panel.beginSheetModal(for: w) { r in
                        guard r == .OK, let url = panel.url else { return }
                        MainActor.assumeIsolated {
                            self.perform("fix", reload: [], select: false) { ops in
                                try ops.retargetAlias(item.path, to: url.path)
                                return []
                            }
                        }
                    }
                default: break
                }
            }
        }
    }


    @objc func getInfo(_ sender: Any?) {
        let items = selection.isEmpty ? [BrowserListing.item(location)].compactMap { $0 } : selection
        for item in items { BrowserInfoWindow.show(item, browser: self) }
    }

    @objc func rename(_ sender: Any?) {
        guard selection.count == 1 else { return }
        browserRename(selection[0])
    }

    func browserRename(_ item: BrowserItem) {
        cancelPendingRename()
        guard editor == nil else { return }
        guard let spot = current.nameSpot(for: item.path) else { return renameInSheet(item) }
        let e = BrowserNameEditor(item: item, spot: spot) { [weak self] name in
            guard let self else { return }
            self.editor = nil
            if self.window?.firstResponder == nil || self.window?.firstResponder === self.window { self.current.focus() }
            if let name, name != item.name {
                if let problem = BrowserNames.problem(name) {
                    self.alert("The name “\(name)” can't be used.", info: problem)
                } else {
                    self.perform("rename", reload: [item.parent]) { ops in [try ops.rename(item.path, to: name)] }
                }
            }
            if self.reloadAfterEdit {
                self.reloadAfterEdit = false
                self.reload()
            }
        }
        editor = e
        e.begin(in: current.view)
    }

    func browserClickedName(_ item: BrowserItem) {
        cancelPendingRename()
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.selection.map(\.path) == [item.path], NSEvent.pressedMouseButtons == 0 else { return }
            self.browserRename(item)
        }
        pendingRename = work
        DispatchQueue.main.asyncAfter(deadline: .now() + NSEvent.doubleClickInterval + 0.05, execute: work)
    }

    func browserCancelRename() { cancelPendingRename() }

    private func cancelPendingRename() {
        pendingRename?.cancel()
        pendingRename = nil
    }

    /// The rename asked for in a sheet, where the name isn't in sight.
    private func renameInSheet(_ item: BrowserItem) {
        let a = NSAlert()
        a.messageText = "Rename “\(item.name)”"
        a.addButton(withTitle: "Rename")
        a.addButton(withTitle: "Cancel")
        let field = NSTextField(string: item.name)
        field.frame = NSRect(x: 0, y: 0, width: 280, height: 22)
        a.accessoryView = field
        a.window.initialFirstResponder = field
        guard let w = window else { return }
        a.beginSheetModal(for: w) { response in
            MainActor.assumeIsolated {
                let newName = field.stringValue
                guard response == .alertFirstButtonReturn, newName != item.name else { return }
                self.perform("rename", reload: [item.parent]) { ops in [try ops.rename(item.path, to: newName)] }
            }
        }
    }

    @objc func duplicate(_ sender: Any?) {
        let paths = selection.map(\.path)
        perform("duplicate", reload: []) { ops in try paths.map { try ops.duplicate($0) } }
    }

    @objc func makeAlias(_ sender: Any?) {
        let paths = selection.map(\.path)
        perform("make an alias of", reload: []) { ops in try paths.map { try ops.makeAlias($0) } }
    }

    @objc func moveToTrash(_ sender: Any?) {
        let items = selection
        guard !items.isEmpty else { return }
        let running = items.filter { Self.model?.states.isRunning($0.path) == true }
        if !running.isEmpty {
            alert("“\(running[0].name)” is running.", info: "Quit it before moving it to the Trash.")
            return
        }
        perform("move to the Trash", reload: [], select: false) { ops in
            try items.map { item in
                try ops.moveToTrash(item.path) { url in
                    var out: NSURL?
                    try FileManager.default.trashItem(at: url, resultingItemURL: &out)
                    return (out as URL?) ?? url
                }
            }
        }
    }

    /// Whether each item is in the Trash with a place to go back to.
    func canPutBack(_ items: [BrowserItem]) -> Bool {
        !items.isEmpty && items.allSatisfy { BrowserCore.shared.ops.putBackLocation($0.path) != nil }
    }

    /// File › Move to Trash, which is Put Back for items in the Trash, as
    /// in the Finder (one key, Command-Delete).
    @objc func moveToTrashOrPutBack(_ sender: Any?) {
        if canPutBack(selection) { putBack(sender) } else { moveToTrash(sender) }
    }

    /// Put Back: the items go back where they were moved to the Trash
    /// from; a name taken there asks, as a copy does.
    @objc func putBack(_ sender: Any?) {
        let ops = BrowserCore.shared.ops
        var plan: [(src: String, name: String?, replace: String?)] = []
        for item in selection {
            guard let original = ops.putBackLocation(item.path) else { continue }
            let name = (original as NSString).lastPathComponent, dir = (original as NSString).deletingLastPathComponent
            if ops.exists(original) {
                let a = NSAlert()
                a.messageText = "An item named “\(name)” already exists in the location it came from. "
                    + "Do you want to replace it with the one you're putting back?"
                a.addButton(withTitle: "Keep Both")
                a.addButton(withTitle: "Stop")
                a.addButton(withTitle: "Replace")
                switch a.runModal() {
                case .alertFirstButtonReturn: plan.append((item.path, ops.freeName(name, in: dir, copy: false), nil))
                case .alertThirdButtonReturn: plan.append((item.path, nil, original))
                default: return
                }
            } else {
                plan.append((item.path, nil, nil))
            }
        }
        guard !plan.isEmpty else { return }
        let steps = plan
        perform("put back", reload: [], select: false) { ops in
            var back: [String] = []
            for step in steps {
                if let old = step.replace {
                    _ = try ops.moveToTrash(old) { url in
                        var out: NSURL?
                        try FileManager.default.trashItem(at: url, resultingItemURL: &out)
                        return (out as URL?) ?? url
                    }
                }
                back.append(try ops.putBack(step.src, name: step.name))
            }
            return back
        }
    }

    @objc func emptyTrash(_ sender: Any?) {
        let a = NSAlert()
        a.messageText = "Are you sure you want to permanently erase the items in the Trash?"
        a.informativeText = "You can't undo this action."
        a.addButton(withTitle: "Empty Trash").hasDestructiveAction = true
        a.addButton(withTitle: "Cancel")
        guard let w = window else { return }
        a.beginSheetModal(for: w) { response in
            guard response == .alertFirstButtonReturn else { return }
            MainActor.assumeIsolated {
                let ops = BrowserCore.shared.ops, trash = BrowserPlaces.trash
                BrowserCore.shared.queue.async {
                    var failed: Error?
                    do { try ops.emptyTrash(trash) } catch { failed = error }
                    let unreadable = failed
                    DispatchQueue.main.async {
                        MainActor.assumeIsolated {
                            // Blue Box may not read the Trash (macOS keeps it private
                            // without Full Disk Access): the Finder empties it then.
                            if unreadable != nil { Self.emptyTrashWithFinder(in: self) }
                            self.reload()
                        }
                    }
                }
            }
        }
    }

    /// The Finder asked to empty the Trash (an Apple event, which macOS
    /// asks the user to allow once).
    static func emptyTrashWithFinder(in browser: BrowserWindowController) {
        var error: NSDictionary?
        NSAppleScript(source: "tell application \"Finder\" to empty trash")?.executeAndReturnError(&error)
        if let error {
            browser.alert("The Trash could not be emptied.",
                          info: (error[NSAppleScript.errorMessage] as? String)
                            ?? "Allow Blue Box to control the Finder in System Settings › Privacy & Security › Automation.")
        }
    }

    @objc func newFolder(_ sender: Any?) {
        let dir = location
        perform("make a new folder in", reload: [dir]) { ops in
            let path = dir + "/" + ops.freeName("untitled folder", in: dir, copy: false)
            try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: false)
            return [path]
        }
    }

    @objc func copy(_ sender: Any?) {
        let items = selection
        guard !items.isEmpty else { return }
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.writeObjects(items.map { $0.url as NSURL })
        pb.setString(items.map(\.path).joined(separator: "\n"), forType: BrowserDrag.pathsType)
    }

    @objc func paste(_ sender: Any?) {
        let pb = NSPasteboard.general
        let urls = pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        guard !urls.isEmpty else { return }
        transfer(urls.map(\.path), into: location, move: false)
    }

    override func selectAll(_ sender: Any?) { current.selectAll() }

    @objc func setLabel(_ sender: NSMenuItem) {
        let paths = selection.map(\.path), label = sender.tag
        perform("label", reload: [], select: false) { ops in
            for p in paths { try ops.update(p) { $0.label = label } }
            return []
        }
    }

    @objc func saveMacBinary(_ sender: Any?) {
        let files = selection.filter { !$0.isFolder && $0.isClassic }
        guard !files.isEmpty, let w = window else { return }
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.prompt = "Save"
        panel.message = "Choose where to save \(files.count == 1 ? "“\(files[0].name).bin”" : "the MacBinary files")"
        panel.directoryURL = URL(fileURLWithPath: BrowserPlaces.home + "/Desktop")
        panel.beginSheetModal(for: w) { r in
            guard r == .OK, let dir = panel.url?.path else { return }
            MainActor.assumeIsolated {
                self.perform("save as MacBinary", reload: [dir], select: false) { ops in
                    try files.map { try ops.exportMacBinary($0.path, into: dir) }
                }
            }
        }
    }

    @objc func importIntoLibrary(_ sender: Any?) {
        Self.model?.importFiles(selection.filter(isImportable).map(\.url))
        BrowserCore.shared.lookup.invalidate()
    }

    @objc func showInFinder(_ sender: Any?) {
        let urls = selection.map(\.url)
        if urls.isEmpty {
            NSWorkspace.shared.open(URL(fileURLWithPath: location))
        } else {
            NSWorkspace.shared.activateFileViewerSelecting(urls)
        }
    }

    @objc func toggleHidden(_ sender: Any?) {
        showHidden.toggle()
        reload()
    }

    @objc func goBack(_ sender: Any?) {
        guard let prev = back.popLast() else { return }
        forward.append(location)
        go(to: prev, remember: false)
    }

    @objc func goForward(_ sender: Any?) {
        guard let next = forward.popLast() else { return }
        back.append(location)
        go(to: next, remember: false)
    }

    @objc func goUp(_ sender: Any?) {
        guard location != "/" else { return }
        let from = location
        go(to: (location as NSString).deletingLastPathComponent)
        current.select([from])
    }

    @objc func goLibrary(_ sender: Any?) { go(to: BrowserPlaces.library) }
    @objc func goDisk(_ sender: Any?) { go(to: BrowserPlaces.disk) }
    @objc func goHome(_ sender: Any?) { go(to: BrowserPlaces.home) }
    @objc func goTrash(_ sender: Any?) { go(to: BrowserPlaces.trash) }

    @objc func viewAsIcons(_ sender: Any?) { attach(.icons) }
    @objc func viewAsList(_ sender: Any?) { attach(.list) }
    @objc func viewAsColumns(_ sender: Any?) { attach(.columns) }
    @objc private func modeChanged(_ sender: NSSegmentedControl) {
        attach(BrowserMode(rawValue: sender.selectedSegment) ?? .icons)
    }
    @objc private func historyClicked(_ sender: NSSegmentedControl) {
        if sender.selectedSegment == 0 { goBack(sender) } else { goForward(sender) }
    }

    @objc func focusSearch(_ sender: Any?) { window?.makeFirstResponder(searchField) }

    override func newWindowForTab(_ sender: Any?) {
        Self.open(at: location, tabOf: window)
    }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        let none = selection.isEmpty
        switch item.action {
        case #selector(openSelection(_:)), #selector(duplicate(_:)), #selector(makeAlias(_:)), #selector(copy(_:)):
            return !none
        case #selector(moveToTrash(_:)):
            return !none && location != BrowserPlaces.trash
        case #selector(putBack(_:)): return canPutBack(selection)
        case #selector(moveToTrashOrPutBack(_:)):
            let back = canPutBack(selection)
            item.title = back ? "Put Back" : "Move to Trash"
            return back || (!none && location != BrowserPlaces.trash)
        case #selector(quickLook(_:)):
            item.title = selection.count == 1 ? "Quick Look “\(selection[0].name)”" : "Quick Look"
            return !none
        case #selector(rename(_:)): return selection.count == 1
        case #selector(showOriginal(_:)): return canShowOriginal(selection)
        case #selector(saveMacBinary(_:)): return selection.contains { !$0.isFolder && $0.isClassic }
        case #selector(importIntoLibrary(_:)): return selection.contains(where: isImportable)
        case #selector(paste(_:)):
            return NSPasteboard.general.canReadObject(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true])
        case #selector(goBack(_:)): return !back.isEmpty
        case #selector(goForward(_:)): return !forward.isEmpty
        case #selector(goUp(_:)): return location != "/"
        case #selector(viewAsIcons(_:)): item.state = mode == .icons ? .on : .off
        case #selector(viewAsList(_:)): item.state = mode == .list ? .on : .off
        case #selector(viewAsColumns(_:)): item.state = mode == .columns ? .on : .off
        case #selector(toggleHidden(_:)): item.title = showHidden ? "Hide Hidden Files" : "Show Hidden Files"
        case #selector(setLabel(_:)):
            if !none { item.state = selection.allSatisfy { $0.label == item.tag } ? .on : .off }
            return !none
        default: break
        }
        return true
    }

    // MARK: Quick Look

    private var previewing = false

    /// The space bar, File › Quick Look: the panel shows the selection, or
    /// goes.
    @objc func quickLook(_ sender: Any?) { browserQuickLook() }

    func browserQuickLook() {
        guard let panel = QLPreviewPanel.shared() else { return }
        if previewing && panel.isVisible {
            panel.orderOut(nil)
        } else if !selection.isEmpty {
            panel.makeKeyAndOrderFront(nil)
        }
    }

    override func acceptsPreviewPanelControl(_ panel: QLPreviewPanel!) -> Bool { true }

    // QuickLookUI's informal protocol, asked on the main thread.
    override func beginPreviewPanelControl(_ panel: QLPreviewPanel!) {
        MainActor.assumeIsolated {
            previewing = true
            panel.dataSource = self
            panel.delegate = self
            refreshPreview()
        }
    }

    override func endPreviewPanelControl(_ panel: QLPreviewPanel!) {
        MainActor.assumeIsolated {
            previewing = false
            panel.dataSource = nil
            panel.delegate = nil
            previewItems = []
            BrowserPreviews.shared.clear()
        }
    }

    /// The panel shows the selection as it is now.
    private func refreshPreview() {
        let items = selection
        previewItems = items.map { item in
            BrowserPreviews.shared.item(for: item) { [weak self] ready in
                guard let self, self.previewing, let panel = QLPreviewPanel.shared() else { return }
                if self.previewItems.indices.contains(panel.currentPreviewItemIndex),
                   self.previewItems[panel.currentPreviewItemIndex] === ready {
                    panel.refreshCurrentPreviewItem()
                }
            }
        }
        QLPreviewPanel.shared()?.reloadData()
    }

    func numberOfPreviewItems(in panel: QLPreviewPanel!) -> Int { previewItems.count }

    func previewPanel(_ panel: QLPreviewPanel!, previewItemAt index: Int) -> QLPreviewItem! {
        previewItems.indices.contains(index) ? previewItems[index] : nil
    }

    /// The arrow keys move the selection, as in the Finder, and the panel
    /// follows it.
    func previewPanel(_ panel: QLPreviewPanel!, handle event: NSEvent!) -> Bool {
        guard let event, event.type == .keyDown, [123, 124, 125, 126].contains(event.keyCode),
              let responder = window?.firstResponder else { return false }
        responder.keyDown(with: event)
        return true
    }

    func previewPanel(_ panel: QLPreviewPanel!, sourceFrameOnScreenFor item: QLPreviewItem!) -> NSRect {
        guard let item = item as? BrowserPreviewItem else { return .zero }
        return current.screenFrame(for: item.path) ?? .zero
    }

    // MARK: Doing it, off the main thread

    /// Run file operations on the operations queue; errors are shown, the
    /// folders reloaded and what was made selected.
    func perform(_ what: String, reload folders: [String], select: Bool = true,
                 _ work: @escaping @Sendable (ClassicFileOps) throws -> [String]) {
        let ops = BrowserCore.shared.ops
        BrowserCore.shared.queue.async {
            var made: [String] = []
            var failure: String?
            do { made = try work(ops) } catch { failure = "\(error)" }
            let result = made, message = failure
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    if let message { self.alert("Blue Box couldn't \(what) the item.", info: message) }
                    BrowserCore.shared.lookup.invalidate()
                    for w in Self.windows { w.reload() }
                    if select && !result.isEmpty { self.current.select(Set(result)) }
                }
            }
        }
    }

    func alert(_ message: String, info: String = "") {
        let a = NSAlert()
        a.messageText = message
        a.informativeText = info
        if let w = window { a.beginSheetModal(for: w) } else { a.runModal() }
    }

    // MARK: Dragging

    func browserDragBegan(_ items: [BrowserItem]) {
        cancelPendingRename()
        BrowserDrag.shared.began(items)
    }

    func browserDragWriter(_ item: BrowserItem) -> NSPasteboardWriting? { BrowserDrag.shared.writer(for: item) }

    func browserValidateDrop(_ info: NSDraggingInfo, into folder: String) -> NSDragOperation {
        BrowserDrag.shared.validate(info, into: folder)
    }

    func browserAcceptDrop(_ info: NSDraggingInfo, into folder: String) -> Bool {
        BrowserDrag.shared.accept(info, into: folder, browser: self)
    }

    /// Copy or move paths into a folder, asking what to do about names
    /// already there.
    func transfer(_ paths: [String], into folder: String, move: Bool) {
        let ops = BrowserCore.shared.ops
        var plan: [(src: String, name: String, replace: Bool)] = []
        for src in paths {
            let name = (src as NSString).lastPathComponent
            let dest = folder + "/" + name
            if move && (src as NSString).deletingLastPathComponent == folder { continue }
            if ops.exists(dest) && dest != src {
                let a = NSAlert()
                a.messageText = "An item named “\(name)” already exists in this location. Do you want to replace it with the one you're "
                    + (move ? "moving?" : "copying?")
                a.addButton(withTitle: "Keep Both")
                a.addButton(withTitle: "Stop")
                a.addButton(withTitle: "Replace")
                switch a.runModal() {
                case .alertFirstButtonReturn: plan.append((src, ops.freeName(name, in: folder, copy: !move), false))
                case .alertThirdButtonReturn: plan.append((src, name, true))
                default: return
                }
            } else {
                plan.append((src, (src == dest && !move) ? ops.freeName(name, in: folder, copy: true) : name, false))
            }
        }
        guard !plan.isEmpty else { return }
        let steps = plan
        perform(move ? "move" : "copy", reload: [folder]) { ops in
            var made: [String] = []
            for step in steps {
                if step.replace {
                    _ = try ops.moveToTrash(folder + "/" + step.name) { url in
                        var out: NSURL?
                        try FileManager.default.trashItem(at: url, resultingItemURL: &out)
                        return (out as URL?) ?? url
                    }
                }
                made.append(move ? try ops.move(step.src, into: folder, name: step.name)
                                 : try ops.copy(step.src, into: folder, name: step.name))
            }
            return made
        }
    }

    // MARK: The window

    func windowWillClose(_ notification: Notification) {
        editor?.end()
        cancelPendingRename()
        watcher?.cancel()
        Self.windows.removeAll { $0 === self }
    }

    // MARK: The toolbar

    private enum Item {
        static let history = NSToolbarItem.Identifier("history")
        static let mode = NSToolbarItem.Identifier("mode")
        static let search = NSToolbarItem.Identifier("search")
        static let action = NSToolbarItem.Identifier("action")
        static let trash = NSToolbarItem.Identifier("trash")
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator, Item.history, .flexibleSpace, Item.mode, Item.action, Item.search]
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        toolbarDefaultItemIdentifiers(toolbar) + [Item.trash, .space]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        switch id {
        case Item.history:
            let images = [NSImage(systemSymbolName: "chevron.left", accessibilityDescription: "Back")!,
                          NSImage(systemSymbolName: "chevron.right", accessibilityDescription: "Forward")!]
            let group = NSToolbarItemGroup(itemIdentifier: id, images: images, selectionMode: .momentary,
                                           labels: ["Back", "Forward"], target: self, action: #selector(historyClicked(_:)))
            group.label = "Back/Forward"
            group.isNavigational = true
            if let control = group.view as? NSSegmentedControl {
                control.segmentStyle = .separated
                control.setEnabled(!back.isEmpty, forSegment: 0)
                control.setEnabled(!forward.isEmpty, forSegment: 1)
                historyControl = control
            }
            return group
        case Item.mode:
            let images = [NSImage(systemSymbolName: "square.grid.2x2", accessibilityDescription: "Icons")!,
                          NSImage(systemSymbolName: "list.bullet", accessibilityDescription: "List")!,
                          NSImage(systemSymbolName: "rectangle.split.3x1", accessibilityDescription: "Columns")!]
            let group = NSToolbarItemGroup(itemIdentifier: id, images: images, selectionMode: .selectOne,
                                           labels: ["Icons", "List", "Columns"], target: self, action: #selector(modeChanged(_:)))
            group.label = "View"
            group.selectedIndex = mode.rawValue
            modeControl = group.view as? NSSegmentedControl
            return group
        case Item.action:
            let item = NSMenuToolbarItem(itemIdentifier: id)
            item.image = NSImage(systemSymbolName: "ellipsis.circle", accessibilityDescription: "Action")
            item.label = "Action"
            item.showsIndicator = true
            let menu = NSMenu()
            menu.delegate = actionMenuDelegate
            item.menu = menu
            return item
        case Item.trash:
            let item = NSToolbarItem(itemIdentifier: id)
            item.image = NSImage(systemSymbolName: "trash", accessibilityDescription: "Move to Trash")
            item.label = "Move to Trash"
            item.target = self
            item.action = #selector(moveToTrash(_:))
            return item
        case Item.search:
            let item = NSSearchToolbarItem(itemIdentifier: id)
            item.searchField.delegate = self
            item.searchField.placeholderString = "Search"
            searchField = item.searchField
            return item
        default:
            return nil
        }
    }

    /// The Action menu's items, made when it opens, for the selection.
    private lazy var actionMenuDelegate = BrowserActionMenu(browser: self)

    // MARK: For tests and documentation

    /// Select an item by name (BLUEBOX_BROWSER_SELECT), for a snapshot.
    func selectForSnapshot(_ name: String) {
        current.select([location + "/" + name])
    }

    /// Keys sent to the window (BLUEBOX_BROWSER_KEYS, comma-separated:
    /// down, up, left, right, return, esc, space, tab, text:LETTERS,
    /// dump), a step a second apart, for tests of the keyboard; dump
    /// writes what is selected and edited to standard error.
    func runKeysForTest(_ steps: [String], then done: @escaping @MainActor () -> Void) {
        guard let step = steps.first, let w = window else { return done() }
        let codes: [String: (UInt16, String)] = ["down": (125, "\u{F701}"), "up": (126, "\u{F700}"),
                                                 "left": (123, "\u{F702}"), "right": (124, "\u{F703}"),
                                                 "return": (36, "\r"), "esc": (53, "\u{1B}"), "space": (49, " "),
                                                 "tab": (48, "\t")]
        func send(_ code: UInt16, _ chars: String) {
            let arrow = (123...126).contains(code)
            guard let e = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: arrow ? [.numericPad, .function] : [],
                                           timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: w.windowNumber,
                                           context: nil, characters: chars, charactersIgnoringModifiers: chars,
                                           isARepeat: false, keyCode: code) else { return }
            w.sendEvent(e)
        }
        if step == "dump" {
            var chain: [String] = []
            var r = w.firstResponder
            while let x = r, chain.count < 6 { chain.append(String(describing: type(of: x))); r = x.nextResponder }
            let names = BrowserListing.items(in: location, showHidden: false).map(\.name).sorted()
            FileHandle.standardError.write(Data(("dump: responder \(chain.joined(separator: ">")) selected "
                + "\(selection.map(\.name)) editing \(editor.map { "“" + $0.field.stringValue + "”" } ?? "no") "
                + "location \((location as NSString).lastPathComponent) files \(names) "
                + ((current as? BrowserColumnView)?.debugState ?? "") + "\n").utf8))
        } else if step.hasPrefix("text:") {
            if let editor, let text = editor.field.currentEditor() as? NSTextView {
                text.insertText(String(step.dropFirst(5)), replacementRange: text.selectedRange())
            } else {
                for c in step.dropFirst(5) { send(0, String(c)) }
            }
        } else if let (code, chars) = codes[step] {
            send(code, chars)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
            MainActor.assumeIsolated { self.runKeysForTest(Array(steps.dropFirst()), then: done) }
        }
    }

    /// The window drawn into a PNG, without showing it on the screen.
    func snapshot(to path: String) {
        guard let w = window, let view = w.contentView else { return }
        w.layoutIfNeeded()
        view.layoutSubtreeIfNeeded()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    }
}

/// The browser's right-hand pane, on the window's background.
final class BrowserPane: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSColor.windowBackgroundColor.setFill()
        dirtyRect.fill()
    }
}

/// The toolbar's Action menu: the selection's context menu.
@MainActor final class BrowserActionMenu: NSObject, NSMenuDelegate {
    weak var browser: BrowserWindowController?
    init(browser: BrowserWindowController) { self.browser = browser }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        guard let browser else { return }
        let built = browser.browserMenu(for: browser.selectedForMenu)
        for item in built.items {
            built.removeItem(item)
            menu.addItem(item)
        }
    }
}

extension BrowserWindowController {
    var selectedForMenu: [BrowserItem] { views[mode]?.selectedItems ?? [] }
}

// MARK: - The sidebar

/// The sidebar's places, a source list in sections, each a drop target.
@MainActor final class BrowserSidebar: NSViewController, NSOutlineViewDataSource, NSOutlineViewDelegate {
    var sections: [(title: String, places: [BrowserPlace])] = [] {
        didSet {
            let shown = sections.filter { !$0.places.isEmpty }
            sectionObjects = shown.map { Section($0.title) }
            placeObjects = shown.map { $0.places.map(Place.init) }
            if isViewLoaded {
                outline.reloadData()
                outline.expandItem(nil, expandChildren: true)
            }
        }
    }
    var onChoose: ((BrowserPlace) -> Void)?
    var onDrop: ((NSDraggingInfo, String, Bool) -> NSDragOperation)?
    private let outline = NSOutlineView()
    private var choosing = false

    final class Section: NSObject {
        let title: String
        init(_ title: String) { self.title = title }
    }
    final class Place: NSObject {
        let place: BrowserPlace
        init(_ place: BrowserPlace) { self.place = place }
    }
    private var sectionObjects: [Section] = []
    private var placeObjects: [[Place]] = []

    override func loadView() {
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        outline.style = .sourceList
        outline.headerView = nil
        outline.floatsGroupRows = false
        outline.rowSizeStyle = .default
        let column = NSTableColumn(identifier: .init("place"))
        outline.addTableColumn(column)
        outline.outlineTableColumn = column
        outline.dataSource = self
        outline.delegate = self
        outline.registerForDraggedTypes([.fileURL] + NSFilePromiseReceiver.readableDraggedTypes.map { .init($0) })
        scroll.documentView = outline
        view = scroll
        outline.reloadData()
        outline.expandItem(nil, expandChildren: true)
    }

    func select(_ place: BrowserPlace?) {
        _ = view
        choosing = true
        defer { choosing = false }
        guard let place else { outline.deselectAll(nil); return }
        for row in 0..<outline.numberOfRows {
            if let p = outline.item(atRow: row) as? Place, p.place == place {
                outline.selectRowIndexes([row], byExtendingSelection: false)
                return
            }
        }
        outline.deselectAll(nil)
    }

    func outlineView(_ o: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        if item == nil { return sectionObjects.count }
        if let s = item as? Section, let i = sectionObjects.firstIndex(of: s) { return placeObjects[i].count }
        return 0
    }

    func outlineView(_ o: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        if item == nil { return sectionObjects[index] }
        guard let s = item as? Section, let i = sectionObjects.firstIndex(of: s) else { return NSNull() }
        return placeObjects[i][index]
    }

    func outlineView(_ o: NSOutlineView, isItemExpandable item: Any) -> Bool { item is Section }
    func outlineView(_ o: NSOutlineView, isGroupItem item: Any) -> Bool { item is Section }
    func outlineView(_ o: NSOutlineView, shouldSelectItem item: Any) -> Bool { item is Place }
    func outlineView(_ o: NSOutlineView, shouldShowOutlineCellForItem item: Any) -> Bool { false }

    func outlineView(_ o: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any) -> NSView? {
        if let s = item as? Section {
            let cell = o.makeView(withIdentifier: .init("header"), owner: nil) as? NSTableCellView ?? {
                let c = NSTableCellView()
                c.identifier = .init("header")
                let t = NSTextField(labelWithString: "")
                t.translatesAutoresizingMaskIntoConstraints = false
                c.addSubview(t)
                c.textField = t
                NSLayoutConstraint.activate([t.leadingAnchor.constraint(equalTo: c.leadingAnchor, constant: 2),
                                             t.centerYAnchor.constraint(equalTo: c.centerYAnchor)])
                return c
            }()
            cell.textField?.stringValue = s.title
            return cell
        }
        guard let p = item as? Place else { return nil }
        let cell = o.makeView(withIdentifier: .init("place"), owner: nil) as? NSTableCellView ?? {
            let c = NSTableCellView()
            c.identifier = .init("place")
            let image = NSImageView()
            let text = NSTextField(labelWithString: "")
            text.lineBreakMode = .byTruncatingTail
            for v in [image, text] as [NSView] {
                v.translatesAutoresizingMaskIntoConstraints = false
                c.addSubview(v)
            }
            c.imageView = image
            c.textField = text
            NSLayoutConstraint.activate([
                image.leadingAnchor.constraint(equalTo: c.leadingAnchor, constant: 2),
                image.centerYAnchor.constraint(equalTo: c.centerYAnchor),
                image.widthAnchor.constraint(equalToConstant: 18),
                text.leadingAnchor.constraint(equalTo: image.trailingAnchor, constant: 6),
                text.trailingAnchor.constraint(equalTo: c.trailingAnchor, constant: -2),
                text.centerYAnchor.constraint(equalTo: c.centerYAnchor),
            ])
            return c
        }()
        cell.textField?.stringValue = p.place.title
        cell.imageView?.image = NSImage(systemSymbolName: p.place.symbol, accessibilityDescription: nil)
        cell.imageView?.contentTintColor = .controlAccentColor
        return cell
    }

    func outlineViewSelectionDidChange(_ note: Notification) {
        guard !choosing, let p = outline.item(atRow: outline.selectedRow) as? Place else { return }
        onChoose?(p.place)
    }

    func outlineView(_ o: NSOutlineView, validateDrop info: NSDraggingInfo, proposedItem item: Any?,
                     proposedChildIndex index: Int) -> NSDragOperation {
        guard let p = item as? Place, index == NSOutlineViewDropOnItemIndex else { return [] }
        return onDrop?(info, p.place.path, false) ?? []
    }

    func outlineView(_ o: NSOutlineView, acceptDrop info: NSDraggingInfo, item: Any?, childIndex index: Int) -> Bool {
        guard let p = item as? Place else { return false }
        return onDrop?(info, p.place.path, true) != []
    }
}
#endif
