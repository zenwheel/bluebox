// The Blue Box shell (bluebox shell): one window that is the home of the
// environment.  A sidebar lists the default settings and every application
// in the library and on the classic Mac's disk; an application's page
// launches it (its own `bluebox run` process, as from the command line)
// and holds its settings.  Archives dropped on the window are imported.
//
// Settings live in the library's Settings.json (Settings.swift), which
// `bluebox run` reads; each control here edits one field of AppSettings,
// so a new option is a field there and a control in SettingsForm.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import SwiftUI

/// An application the shell can launch.
struct ShellApp: Identifiable, Hashable {
    let path: String
    let place: String        /* "Library" or "Macintosh HD" */
    let folder: String       /* the folder it is in, relative to its place */
    let architecture: String
    let creator: UInt32
    var id: String { path }
    var name: String { (path as NSString).lastPathComponent }
}

/// Something an installer put in the classic System Folder.
struct SystemItem: Identifiable, Hashable {
    let path: String
    let folder: String /* Control Panels, Extensions ... */
    let type: String
    let creator: String
    let architecture: String
    var id: String { path }
    var name: String { (path as NSString).lastPathComponent }
    var isApplication: Bool { type == "APPL" }
    /// What it is, in words.
    var kind: String {
        switch type {
        case "APPL": return "application"
        case "cdev": return "control panel"
        case "INIT": return "extension"
        case "appe": return "background application"
        case "FFIL", "tfil": return "font suitcase"
        case "ffil": return "font"
        case "PRER", "PRES": return "printer driver"
        case "RDEV": return "Chooser extension"
        case "thng": return "component"
        case "shlb": return "shared library"
        case "dfil": return "desk accessory"
        default: return "\(type) file"
        }
    }
}

/// The System Folder's folders the shell shows and watches.
let watchedSystemFolders = ["Apple Menu Items", "Control Panels", "Extensions", "Startup Items", "Fonts"]

/// A sidebar entry: the defaults, the System Folder, or one application.
/// The sidebar's tree: a folder, or an application.
struct ShellNode: Identifiable, Hashable {
    let id: String
    let name: String
    var app: ShellApp?
    var children: [ShellNode]? /* nil for an application */
    /// A folder's place and its path there (the outer folder of a joined
    /// label such as "Journeyman.toast_.zip/Journeyman").
    var place = ""
    var folder = ""
    /// A folder's applications, at any depth (for its menu, without
    /// searching the whole library).
    var paths: [String] = []
}

/// The applications a sidebar search finds: those whose folder and name
/// hold every word of the search, ignoring case and diacritics.  An empty
/// search finds them all.
func shellSearch(_ apps: [ShellApp], _ text: String) -> [ShellApp] {
    let words = text.split(whereSeparator: \.isWhitespace)
    guard !words.isEmpty else { return apps }
    return apps.filter { app in
        let whole = app.folder + "/" + app.name
        return words.allSatisfy { whole.range(of: $0, options: [.caseInsensitive, .diacriticInsensitive]) != nil }
    }
}

/// The applications of one place as their folders hold them.  A folder that
/// holds only one application is that application; a folder that holds
/// only one folder joins it ("Journeyman.toast_.zip/Journeyman").
func shellTree(_ apps: [ShellApp], prefix: String = "") -> [ShellNode] {
    var folders: [String: [ShellApp]] = [:]
    var nodes: [ShellNode] = []
    for app in apps {
        let rest = prefix.isEmpty ? app.folder : String(app.folder.dropFirst(prefix.count + 1))
        if rest.isEmpty {
            nodes.append(ShellNode(id: app.path, name: app.name, app: app, children: nil))
        } else {
            let first = String(rest.split(separator: "/", maxSplits: 1)[0])
            folders[first, default: []].append(app)
        }
    }
    var folderNodes: [ShellNode] = []
    for (name, inside) in folders {
        let path = prefix.isEmpty ? name : prefix + "/" + name
        var label = name
        var children = shellTree(inside, prefix: path)
        while children.count == 1, let only = children.first, only.app == nil, let deeper = only.children {
            label += "/" + only.name
            children = deeper
        }
        if children.count == 1, let only = children.first, only.app != nil {
            folderNodes.append(only)
        } else {
            let place = inside.first?.place ?? ""
            folderNodes.append(ShellNode(id: "folder:" + place + "/" + path, name: label, app: nil,
                                         children: children, place: place, folder: path, paths: inside.map(\.path)))
        }
    }
    let byName: (ShellNode, ShellNode) -> Bool = { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    return folderNodes.filter { $0.app == nil }.sorted(by: byName)
        + (nodes + folderNodes.filter { $0.app != nil }).sorted(by: byName)
}

/// The tree's rows: folders that open and close (remembered; all open
/// while searching) and can be moved to the Trash, and applications tagged
/// for the sidebar's selection.
struct ShellNodeRows: View {
    @ObservedObject var model: ShellModel
    let nodes: [ShellNode]
    var searching = false
    /// The folder whose Move to Trash is being confirmed.
    @Binding var trashing: ShellNode?

    var body: some View {
        ForEach(nodes) { node in
            if let app = node.app {
                HStack {
                    Image(nsImage: model.icon(app)).resizable().interpolation(.none)
                        .frame(width: 16, height: 16)
                    Text(app.name)
                    if model.running[app.path] != nil {
                        Circle().fill(Color.green).frame(width: 6, height: 6).help("Running")
                    }
                }
                .help(app.folder.isEmpty ? app.name : app.folder + "/" + app.name)
                .tag(ShellItem.app(app))
            } else {
                // While searching, folders start open and close for this
                // search only: the outline must always be told what it shows
                // (refusing a collapse left it collapsing rows SwiftUI still
                // had open, and the shell crashed).
                DisclosureGroup(isExpanded: Binding(
                    get: { searching ? !model.searchCollapsed.contains(node.id) : model.expanded.contains(node.id) },
                    set: { open in
                        if searching {
                            if open { model.searchCollapsed.remove(node.id) } else { model.searchCollapsed.insert(node.id) }
                        } else if open {
                            model.expanded.insert(node.id)
                        } else {
                            model.expanded.remove(node.id)
                        }
                    })) {
                    ShellNodeRows(model: model, nodes: node.children ?? [], searching: searching, trashing: $trashing)
                } label: {
                    Label(node.name, systemImage: "folder").lineLimit(1)
                        .contextMenu {
                            let running = node.paths.contains { model.running[$0] != nil }
                            Button("Move to Trash…") { trashing = node }
                                .disabled(running || !model.canTrash(node))
                                .help(running ? "Quit its applications first" : "Move the whole folder to the Trash")
                            Button("Show in Finder") {
                                if let path = model.folderPath(node) {
                                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
                                }
                            }
                        }
                }
            }
        }
    }
}

enum ShellItem: Hashable {
    case defaults
    case systemFolder
    case app(ShellApp)
}

@MainActor final class ShellModel: ObservableObject {
    @Published var apps: [ShellApp] = [] {
        didSet { sidebarCache = nil }
    }
    /// Folders closed during the current search.
    @Published var searchCollapsed: Set<String> = []
    /// The sidebar's open folders, kept between runs.
    @Published var expanded: Set<String> = Set(UserDefaults.standard.stringArray(forKey: "expanded") ?? []) {
        didSet { UserDefaults.standard.set(Array(expanded), forKey: "expanded") }
    }
    @Published var themes: [String] = []
    @Published var importing: [String] = []
    @Published var messages: [String] = []
    @Published var settings: [String: AppSettings] = [:]
    @Published var systemItems: [SystemItem] = []
    @Published var running: [String: Process] = [:] /* by application path */
    /// Recently launched applications (paths saved before the rename from
    /// Classic follow the moved data folder).
    @Published var recent: [String] = (UserDefaults.standard.stringArray(forKey: "recent") ?? [])
        .map { $0.replacingOccurrences(of: "/Application Support/Classic/", with: "/Application Support/Blue Box/") }
        .filter { FileManager.default.fileExists(atPath: $0) }
    private var watchers: [DispatchSourceFileSystemObject] = []
    private var iconCache: [String: NSImage] = [:]
    private var iconsLoading: Set<String> = []
    private var iconsPublishPending = false
    /// Bumped when icons have arrived, so the rows showing them redraw.
    @Published private(set) var iconsVersion = 0
    private var sidebarCache: (query: String, places: [(place: String, nodes: [ShellNode])])?
    let library = defaultLibrary()
    private static let genericIcon = NSImage(systemSymbolName: "app", accessibilityDescription: nil) ?? NSImage()

    /// An application's own Finder icon (from its bundle), or a generic one
    /// while it is read: off the main thread, as reading a few hundred
    /// resource forks when a search opens every folder stalled typing.
    func icon(_ app: ShellApp) -> NSImage {
        if let cached = iconCache[app.path] { return cached }
        if iconsLoading.insert(app.path).inserted {
            let path = app.path
            DispatchQueue.global(qos: .userInitiated).async {
                let image = dockImage(finderBundle(applicationAt: path).appIcons)
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        self.iconCache[path] = image ?? Self.genericIcon
                        self.iconsLoading.remove(path)
                        // One redraw for a batch of icons, not one each.
                        if !self.iconsPublishPending {
                            self.iconsPublishPending = true
                            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                                MainActor.assumeIsolated {
                                    self.iconsPublishPending = false
                                    self.iconsVersion += 1
                                }
                            }
                        }
                    }
                }
            }
        }
        return Self.genericIcon
    }

    /// The sidebar's places and their trees for a search, built once per
    /// search and library rather than on every redraw.
    func sidebar(_ query: String) -> [(place: String, nodes: [ShellNode])] {
        if let cache = sidebarCache, cache.query == query { return cache.places }
        let found = shellSearch(apps, query)
        let places = ["Library", "Macintosh HD"].compactMap { place -> (place: String, nodes: [ShellNode])? in
            let here = found.filter { $0.place == place }
            return here.isEmpty ? nil : (place, shellTree(here))
        }
        sidebarCache = (query, places)
        return places
    }

    init() {
        refresh()
        watchSystemFolder()
    }

    var systemFolder: String { defaultDisk() + "/System Folder" }

    /// Refresh when an installer (or anything else) changes the System
    /// Folder's folders.
    private func watchSystemFolder() {
        for name in watchedSystemFolders {
            let fd = open(systemFolder + "/" + name, O_EVTONLY)
            guard fd >= 0 else { continue }
            let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete],
                                                                   queue: .main)
            source.setEventHandler { [weak self] in
                MainActor.assumeIsolated { self?.refresh() }
            }
            source.setCancelHandler { close(fd) }
            source.resume()
            watchers.append(source)
        }
    }

    /// Scan the library and the disk again (after an import or an install).
    func refresh() {
        var found: [ShellApp] = []
        // Theme archives (Kaleidoscope schemes) often carry an application of
        // their own; their entries are themes, not applications to list.
        let themePaths = classicThemes(in: library)
        let themeEntries = Set(themePaths.map { String($0.dropFirst(library.count + 1)).split(separator: "/").first.map(String.init) ?? "" })
        for (root, place) in [(library, "Library"), (defaultDisk(), "Macintosh HD")] {
            guard FileManager.default.fileExists(atPath: root) else { continue }
            for a in scan(root).filter({ $0.isApplication }) {
                let rel = String(a.path.dropFirst(root.count + 1))
                if rel.hasPrefix("System Folder/") && !rel.hasPrefix("System Folder/Apple Menu Items/") { continue }
                if place == "Library", let entry = rel.split(separator: "/").first, themeEntries.contains(String(entry)) {
                    continue
                }
                found.append(ShellApp(path: a.path, place: place, folder: (rel as NSString).deletingLastPathComponent,
                                      architecture: a.architecture, creator: a.creator))
            }
        }
        apps = found.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        themes = themePaths.map { ($0 as NSString).lastPathComponent }.sorted()
        var items: [SystemItem] = []
        for folder in watchedSystemFolders {
            let root = systemFolder + "/" + folder
            guard FileManager.default.fileExists(atPath: root) else { continue }
            for f in scan(root) {
                items.append(SystemItem(path: f.path, folder: folder, type: fourCCString(f.type),
                                        creator: fourCCString(f.creator), architecture: f.architecture))
            }
        }
        systemItems = items.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        settings = loadSettings(library)
    }

    func settingsBinding(_ key: String) -> Binding<AppSettings> {
        Binding(get: { self.settings[key] ?? AppSettings() },
                set: { new in
                    self.settings[key] = new.isEmpty ? nil : new
                    saveSettings(self.settings, library: self.library)
                })
    }

    /// Run an application as `bluebox run` does from a terminal: a process of
    /// its own, with its own Dock icon and rootless windows.
    func launch(_ app: ShellApp) { launch(path: app.path) }

    func launch(path: String) {
        let name = (path as NSString).lastPathComponent
        if let p = running[path], p.isRunning {
            messages.append("\(name) is already running")
            return
        }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Bundle.main.executablePath ?? CommandLine.arguments[0])
        p.arguments = ["run", path]
        p.terminationHandler = { [weak self] _ in
            Task { @MainActor in self?.running[path] = nil }
        }
        do {
            try p.run()
            running[path] = p
            recent.removeAll { $0 == path }
            recent.insert(path, at: 0)
            recent = Array(recent.prefix(8))
            UserDefaults.standard.set(recent, forKey: "recent")
        } catch {
            messages.append("Could not launch \(name): \(error.localizedDescription)")
        }
    }

    /// Move an application (its data file and its AppleDouble sidecar) to
    /// the macOS Trash, from where it can be put back, and forget its
    /// settings.
    func delete(_ app: ShellApp) {
        let fm = FileManager.default
        var trashed = false
        for path in [app.path, sidecarPath(app.path)] where fm.fileExists(atPath: path) {
            do {
                try fm.trashItem(at: URL(fileURLWithPath: path), resultingItemURL: nil)
                trashed = true
            } catch {
                messages.append("Could not move \((path as NSString).lastPathComponent) to the Trash: \(error.localizedDescription)")
            }
        }
        if trashed {
            settings[settingsKey(app.path)] = nil
            saveSettings(settings, library: library)
            recent.removeAll { $0 == app.path }
            UserDefaults.standard.set(recent, forKey: "recent")
            messages.append("Moved \(app.name) to the Trash")
        }
        refresh()
    }

    /// A sidebar folder on disk.
    func folderPath(_ node: ShellNode) -> String? {
        guard node.app == nil, !node.folder.isEmpty else { return nil }
        return (node.place == "Library" ? library : defaultDisk()) + "/" + node.folder
    }

    /// The applications in a sidebar folder, at any depth.
    func apps(in node: ShellNode) -> [ShellApp] {
        apps.filter { $0.place == node.place && ($0.folder == node.folder || $0.folder.hasPrefix(node.folder + "/")) }
    }

    /// Whether a folder may go to the Trash: one inside the library or on
    /// the classic disk, never a place itself nor the disk's System Folder.
    func canTrash(_ node: ShellNode) -> Bool {
        let parts = node.folder.split(separator: "/")
        guard folderPath(node) != nil, ["Library", "Macintosh HD"].contains(node.place), !parts.isEmpty,
              !parts.contains(where: { $0 == "." || $0 == ".." }) else { return false }
        return !(node.place == "Macintosh HD" && parts[0] == "System Folder")
    }

    /// Move a sidebar folder (with any AppleDouble sidecar of its own; those
    /// of what it holds go inside it) to the macOS Trash, and forget the
    /// settings of the applications in it.
    func delete(folder node: ShellNode) {
        guard canTrash(node), let path = folderPath(node) else { return }
        let inside = apps(in: node)
        let name = (node.folder as NSString).lastPathComponent
        if inside.contains(where: { running[$0.path] != nil }) {
            messages.append("Quit the applications in \(name) before moving it to the Trash")
            return
        }
        let fm = FileManager.default
        var trashed = false
        for p in [path, sidecarPath(path)] where fm.fileExists(atPath: p) {
            do {
                try fm.trashItem(at: URL(fileURLWithPath: p), resultingItemURL: nil)
                trashed = true
            } catch {
                messages.append("Could not move \((p as NSString).lastPathComponent) to the Trash: \(error.localizedDescription)")
            }
        }
        refresh()
        if trashed {
            // Settings go by name, so those of a namesake still here stay.
            let left = Set(apps.map { settingsKey($0.path) })
            for app in inside where !left.contains(settingsKey(app.path)) {
                settings[settingsKey(app.path)] = nil
            }
            saveSettings(settings, library: library)
            recent.removeAll { path in inside.contains { $0.path == path } }
            UserDefaults.standard.set(recent, forKey: "recent")
            expanded = expanded.filter { $0 != node.id && !$0.hasPrefix(node.id + "/") }
            messages.append("Moved \(name) to the Trash")
        }
    }

    /// Import dropped or chosen files into the library, off the main thread.
    /// Ask for files to import (the File menu, the sidebar, the status item).
    func chooseImport() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        panel.prompt = "Import"
        panel.message = "Choose archives (.sit, .sea, .cpt, .hqx, .bin) or disk images (.img, .dsk, .toast, .iso) to import"
        NSApp.activate(ignoringOtherApps: true)
        if panel.runModal() == .OK { importFiles(panel.urls) }
    }

    /// File > Import Fonts from Disk Image…: the fonts of a System disk image
    /// (the user's own) into Blue Box's Fonts folder, for every run after.
    func chooseFontImport() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.prompt = "Import Fonts"
        panel.message = "Choose a System disk image (.img, .dsk, .dmg, or gzipped) to take its fonts from"
        NSApp.activate(ignoringOtherApps: true)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let path = url.path
        let name = (path as NSString).lastPathComponent
        importing.append(name)
        Task.detached {
            let result: String
            do {
                let added = try importFonts(fromImage: path)
                result = "Fonts from \(name): \(added.joined(separator: ", ")) (used from the next launch)"
            } catch {
                result = "Could not import fonts from \(name): \(error)"
            }
            await MainActor.run {
                self.importing.removeAll { $0 == name }
                self.messages.append(result)
            }
        }
    }

    func importFiles(_ urls: [URL]) {
        let lib = library
        for url in urls {
            let path = url.path
            importing.append((path as NSString).lastPathComponent)
            Task.detached {
                let result: String
                do {
                    let (entry, notes) = try ingest(path, library: lib)
                    result = "Imported \((path as NSString).lastPathComponent) as \((entry as NSString).lastPathComponent)"
                        + (notes.isEmpty ? "" : " (\(notes.joined(separator: "; ")))")
                } catch {
                    result = "Could not import \((path as NSString).lastPathComponent): \(error)"
                }
                await MainActor.run {
                    self.importing.removeAll { $0 == (path as NSString).lastPathComponent }
                    self.messages.append(result)
                    self.refresh()
                }
            }
        }
    }
}

/// Kaleidoscope schemes in a library (Colr/Acid files).
func classicThemes(in library: String) -> [String] {
    guard FileManager.default.fileExists(atPath: library) else { return [] }
    return scan(library).filter { fourCCString($0.type) == "Colr" && fourCCString($0.creator) == "Acid" }.map(\.path)
}

// MARK: - Views

struct ShellView: View {
    @ObservedObject var model: ShellModel
    /// The page shown first (BLUEBOX_SHELL_PAGE=system, or app for the first
    /// application's, for snapshots).
    @State private var selection: ShellItem? = {
        switch ProcessInfo.processInfo.environment["BLUEBOX_SHELL_PAGE"] {
        case "system": return .systemFolder
        default: return .defaults
        }
    }()
    @State private var dropTargeted = false
    @State private var search = ""
    /// The search the sidebar shows: the typed one, once typing pauses.
    @State private var query = ""
    @State private var trashing: ShellNode?

    var body: some View {
        NavigationSplitView {
            List(selection: $selection) {
                Section("Settings") {
                    Label("Defaults", systemImage: "slider.horizontal.3").tag(ShellItem.defaults)
                    Label("System Folder", systemImage: "folder.badge.gearshape").tag(ShellItem.systemFolder)
                }
                let places = model.sidebar(query)
                ForEach(places, id: \.place) { entry in
                    Section(entry.place) {
                        ShellNodeRows(model: model, nodes: entry.nodes, searching: !query.isEmpty, trashing: $trashing)
                    }
                }
                if places.isEmpty && !query.isEmpty {
                    Text("No applications match").foregroundStyle(.secondary)
                }
            }
            // Searching is a new tree: the outline starts afresh rather than
            // reconciling rows whose shape changed under it.
            .id(query.isEmpty ? "all" : "search")
            // The search finds applications by name or folder; the folders
            // leading to them are shown open.
            .searchable(text: $search, placement: .sidebar, prompt: "Search applications")
            .task(id: search) {
                // The search applies when typing pauses; clearing it, at once.
                if !search.isEmpty {
                    try? await Task.sleep(nanoseconds: 200_000_000)
                    if Task.isCancelled { return }
                }
                model.searchCollapsed = []
                query = search
            }
            // Double-clicking an application launches it; its context menu
            // launches it or shows it in the Finder.
            .contextMenu(forSelectionType: ShellItem.self) { items in
                if case .app(let app)? = items.first {
                    Button("Launch") { model.launch(app) }
                    Button("Show in Finder") {
                        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: app.path)])
                    }
                }
            } primaryAction: { items in
                if case .app(let app)? = items.first { model.launch(app) }
            }
            .safeAreaInset(edge: .bottom) {
                // The window has no toolbar, so importing is at the foot of the list.
                HStack {
                    Button { model.chooseImport() } label: { Label("Import…", systemImage: "square.and.arrow.down") }
                        .help("Import archives, disk images or MacBinary files into the library")
                    Spacer()
                    Button { model.refresh() } label: { Image(systemName: "arrow.clockwise") }
                        .help("Look for changes in the library")
                }
                .buttonStyle(.borderless)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
            }
            .navigationSplitViewColumnWidth(min: 220, ideal: 260)
        } detail: {
            VStack(spacing: 0) {
                switch selection {
                case .app(let app): AppPage(model: model, app: app)
                case .systemFolder: SystemFolderPage(model: model)
                default: DefaultsPage(model: model)
                }
                StatusBar(model: model)
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
            for provider in providers {
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    guard let url else { return }
                    Task { @MainActor in model.importFiles([url]) }
                }
            }
            return true
        }
        .confirmationDialog("Move \(trashing?.name ?? "") to the Trash?", isPresented: Binding(
            get: { trashing != nil }, set: { if !$0 { trashing = nil } }), titleVisibility: .visible,
            presenting: trashing) { node in
            Button("Move to Trash", role: .destructive) { model.delete(folder: node) }
            Button("Cancel", role: .cancel) {}
        } message: { node in
            let count = model.apps(in: node).count
            Text("\(node.place) › \(node.folder), with \(count) application\(count == 1 ? "" : "s") and everything "
                 + "else in it. Their settings are forgotten; you can put it back from the Trash.")
        }
        .overlay(dropTargeted ? RoundedRectangle(cornerRadius: 8).stroke(Color.accentColor, lineWidth: 3) : nil)
        .onAppear {
            if ProcessInfo.processInfo.environment["BLUEBOX_SHELL_PAGE"] == "app", let first = model.apps.first {
                selection = .app(first)
            }
        }
        .onChange(of: model.apps) { apps in
            // A deleted application's page goes with it.
            if case .app(let app) = selection, !apps.contains(app) { selection = .defaults }
        }
        .frame(minWidth: 720, minHeight: 440)
    }

}

struct StatusBar: View {
    @ObservedObject var model: ShellModel
    var body: some View {
        if !model.importing.isEmpty || !model.messages.isEmpty {
            Divider()
            HStack {
                if !model.importing.isEmpty {
                    ProgressView().controlSize(.small)
                    Text("Importing \(model.importing.joined(separator: ", "))…")
                } else if let last = model.messages.last {
                    Text(last).lineLimit(2)
                }
                Spacer()
                if model.importing.isEmpty && !model.messages.isEmpty {
                    Button("Clear") { model.messages.removeAll() }.buttonStyle(.borderless)
                }
            }
            .font(.callout)
            .padding(8)
        }
    }
}

struct DefaultsPage: View {
    @ObservedObject var model: ShellModel
    var body: some View {
        Form {
            Section {
                Text("Every classic application gets these unless its own settings say otherwise.")
                    .foregroundStyle(.secondary)
            }
            SettingsForm(settings: model.settingsBinding(defaultSettingsKey), themes: model.themes, isDefault: true,
                         fallback: AppSettings())
        }
        .formStyle(.grouped)
        .navigationTitle("Defaults")
    }
}

struct SystemFolderPage: View {
    @ObservedObject var model: ShellModel
    var body: some View {
        Form {
            Section {
                Text("What installers have put in the classic Mac's System Folder. Applications in Apple Menu Items "
                     + "can be launched here; control panels and extensions are listed, and run once the "
                     + "environment supports them.")
                    .foregroundStyle(.secondary)
            }
            ForEach(watchedSystemFolders, id: \.self) { folder in
                let items = model.systemItems.filter { $0.folder == folder }
                Section(folder) {
                    if items.isEmpty {
                        Text("Empty").foregroundStyle(.secondary)
                    }
                    ForEach(items) { item in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(item.name)
                                Text("\(item.kind) · \(item.type)/\(item.creator)").font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            if item.isApplication {
                                Button("Launch") { model.launch(path: item.path) }
                            }
                        }
                    }
                }
            }
        }
        .formStyle(.grouped)
        .navigationTitle("System Folder")
    }
}

struct AppPage: View {
    @ObservedObject var model: ShellModel
    let app: ShellApp
    @State private var confirmDelete = false

    private var neededDepth: Int? {
        requiredDepth(creator: app.creator, over: model.settings[defaultSettingsKey]?.depth)
    }
    /// What unset settings come to: the defaults, with the depth a program
    /// known to refuse the default one is given instead.
    private var fallback: AppSettings {
        var f = model.settings[defaultSettingsKey] ?? AppSettings()
        if let d = neededDepth { f.depth = d }
        return f
    }
    var body: some View {
        Form {
            Section {
                HStack {
                    Image(nsImage: model.icon(app)).resizable().interpolation(.none).frame(width: 32, height: 32)
                    VStack(alignment: .leading) {
                        Text(app.name).font(.title2)
                        Text("\(app.place)\(app.folder.isEmpty ? "" : " › " + app.folder) · \(app.architecture)")
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    if model.running[app.path] != nil {
                        Text("Running").foregroundStyle(.secondary)
                    }
                    Button("Delete…", role: .destructive) { confirmDelete = true }
                        .disabled(model.running[app.path] != nil)
                        .help(model.running[app.path] != nil ? "Quit it first" : "Move it to the Trash")
                    Button("Launch") { model.launch(app) }.keyboardShortcut(.defaultAction)
                }
            }
            SettingsForm(settings: model.settingsBinding(settingsKey(app.path)), themes: model.themes, isDefault: false,
                         fallback: fallback, depthNote: neededDepth == nil ? nil : "which it needs")
        }
        .formStyle(.grouped)
        .navigationTitle(app.name)
        .confirmationDialog("Move \(app.name) to the Trash?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Move to Trash", role: .destructive) { model.delete(app) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("\(app.place)\(app.folder.isEmpty ? "" : " › " + app.folder). Its settings are forgotten; "
                 + "you can put it back from the Trash.")
        }
    }
}

/// The settings, as pickers: "Default" leaves one unset (taken from the
/// defaults, or for the defaults themselves from the built-in value).
struct SettingsForm: View {
    @Binding var settings: AppSettings
    let themes: [String]
    let isDefault: Bool
    /// What an unset setting comes to: the defaults' values (for an
    /// application), or nothing (the built-in values, for the defaults).
    let fallback: AppSettings
    /// Why the inherited depth is what it is, when not simply the default's.
    var depthNote: String? = nil

    private var inherit: String { isDefault ? "Built-in" : "Default" }

    var body: some View {
        Section("Display") {
            Picker("Scale", selection: $settings.scale) {
                Text("\(inherit) (\(scaleName(fallback.scale ?? 1)))").tag(Double?.none)
                ForEach([1.0, 1.5, 2.0], id: \.self) { v in Text(scaleName(v)).tag(Double?.some(v)) }
            }
            Picker("Screen depth", selection: $settings.depth) {
                Text("\(inherit) (\(depthName(fallback.depth ?? 32).lowercased())\(depthNote.map { ", " + $0 } ?? ""))")
                    .tag(Int?.none)
                ForEach([1, 2, 4, 8, 16, 32], id: \.self) { d in
                    Text(depthName(d)).tag(Int?.some(d))
                }
            }
            Picker("Appearance", selection: $settings.appearance) {
                Text("\(inherit) (\(appearanceName(fallback.appearance ?? "bw").lowercased()))").tag(String?.none)
                Text("Black and white").tag(String?.some("bw"))
                Text("macOS window frames").tag(String?.some("native"))
                ForEach(themes, id: \.self) { t in Text("Theme: \(t)").tag(String?.some(t)) }
            }
            Picker("Display", selection: $settings.display) {
                Text("\(inherit) (\(displayName(fallback.display).lowercased()))").tag(String?.none)
                Text(displayName("rootless")).tag(String?.some("rootless"))
                Text(displayName("screen")).tag(String?.some("screen"))
            }
            .help("Rootless: each classic window is a window of its own. The whole screen: the Mac's screen in "
                  + "one window, for programs that draw on the screen outside their windows (games, mostly). By "
                  + "default a program is rootless until it shows it wants the whole screen, which then becomes "
                  + "its setting.")
            HighlightPicker(highlight: $settings.highlight, inherit: inherit,
                            inheritedName: highlightName(fallback.highlight ?? "host"))
        }
        Section("Machine") {
            Picker("Processor", selection: $settings.cpu) {
                Text("\(inherit) (\(fallback.cpu.map(cpuName) ?? "PowerPC for fat applications, else 68040"))")
                    .tag(Int?.none)
                Text(cpuName(68040)).tag(Int?.some(68040))
                Text(cpuName(68020)).tag(Int?.some(68020))
                Text(cpuName(powerPCSetting)).tag(Int?.some(powerPCSetting))
            }
        }
        Section("Network and ports") {
            Picker("AppleTalk", selection: $settings.appletalk) {
                Text("\(inherit) (\(appletalkName(fallback.appletalk ?? "local").lowercased()))").tag(String?.none)
                ForEach(["local", "lan", "off"], id: \.self) { a in Text(appletalkName(a)).tag(String?.some(a)) }
                ForEach(NetworkInterface.all(), id: \.name) { i in
                    Text("Local network through \(i.label)").tag(String?.some("lan:" + i.name))
                }
                ForEach(NetworkInterface.all().filter { !isWireless($0.name) }, id: \.name) { i in
                    Text("EtherTalk bridged onto \(i.name) (asks for a password)").tag(String?.some("bridge:" + i.name))
                }
                // An interface from the settings that isn't up now.
                if let a = settings.appletalk, a.hasPrefix("lan:") || a.hasPrefix("bridge:"),
                   !NetworkInterface.all().contains(where: { a.hasSuffix(":" + $0.name) }) {
                    Text("\(a) (not connected)").tag(String?.some(a))
                } else if let a = settings.appletalk, a.hasPrefix("bridge:"), isWireless(String(a.dropFirst(7))) {
                    Text("\(a) (Wi-Fi, which drops EtherTalk: not used)").tag(String?.some(a))
                }
            }
            .help(Self.appleTalkHelp)
            PortPicker(label: "Modem port", spec: $settings.modemPort, inherit: inherit,
                       inherited: fallback.modemPort ?? "modem")
            PortPicker(label: "Printer port", spec: $settings.printerPort, inherit: inherit,
                       inherited: fallback.printerPort ?? "none")
            TextField("AppleShare volume", text: Binding(get: { settings.appleShare ?? "" }, set: {
                let v = $0.trimmingCharacters(in: .whitespaces)
                settings.appleShare = v.isEmpty ? nil : v
            }), prompt: Text(fallback.appleShare.map { $0.isEmpty ? "none" : $0 } ?? "tcp:host/Volume or atalk:Server:AFPServer@*/Volume"))
            .help("A file server's volume, mounted as a disk while the program runs: tcp:host/Volume reaches it over "
                  + "TCP/IP, atalk:Server:AFPServer@*/Volume over AppleTalk (with AppleTalk on). Add user:password@ "
                  + "before the server to log in; without it you are a guest.")
        }
        Section("Printing") {
            PrintFolderPicker(folder: $settings.printFolder, inherit: inherit, inherited: fallback.printFolder)
        }
        Section {
            internetField("Real name", \.realName)
            internetField("Email address", \.email, example: "you@example.com")
            internetField("Organization", \.organization)
            internetField("Mail account", \.mailAccount, example: "user@pop.example.com")
            internetField("Outgoing mail (SMTP)", \.smtpHost, example: "smtp.example.com")
            internetField("News server (NNTP)", \.newsHost, example: "news.example.com")
            internetField("Home page", \.homePage, example: "http://…")
            internetField("Search page", \.searchPage, example: "http://…")
        } header: {
            Text("Internet")
        } footer: {
            Text("Internet Config's preferences: programs that use Internet Config (Internet Explorer, Netscape 4, "
                 + "Fetch, mail and news readers) read them instead of asking. Programs that keep their own settings "
                 + "still use those.")
        }
        Section("Proxies") {
            internetField("Web proxy (HTTP)", \.httpProxy, example: "host:port", proxy: true)
            internetField("FTP proxy", \.ftpProxy, example: "host:port", proxy: true)
            internetField("Gopher proxy", \.gopherProxy, example: "host:port", proxy: true)
            internetField("SOCKS server", \.socks, example: "host:1080", proxy: true)
            internetField("No proxy for", \.noProxy, example: "local.lan, example.com")
            Picker("Passive FTP", selection: Binding(get: { settings.internet?.passiveFTP }, set: { v in
                var i = settings.internet ?? InternetSettings()
                i.passiveFTP = v
                settings.internet = i.isEmpty ? nil : i
            })) {
                Text("\(inherit) (\(fallback.internet?.passiveFTP.map { $0 ? "on" : "off" } ?? "the program's choice"))")
                    .tag(Bool?.none)
                Text("On").tag(Bool?.some(true))
                Text("Off").tag(Bool?.some(false))
            }
        }
    }

    /// One Internet Config preference as a text field: empty takes the
    /// default's value, shown as the prompt.  A proxy can be "none", which
    /// turns it off over a default that has one.
    private func internetField(_ label: String, _ path: WritableKeyPath<InternetSettings, String?>,
                               example: String? = nil, proxy: Bool = false) -> some View {
        let inherited = fallback.internet?[keyPath: path]
        let value = settings.internet?[keyPath: path]
        let valid = !proxy || value == nil || value == "none" || validHostPort(value!)
        return TextField(label, text: Binding(get: { settings.internet?[keyPath: path] ?? "" }, set: {
            let v = $0.trimmingCharacters(in: .whitespaces)
            var i = settings.internet ?? InternetSettings()
            i[keyPath: path] = v.isEmpty ? nil : v
            settings.internet = i.isEmpty ? nil : i
        }), prompt: Text(inherited.map { "\(inherit): \($0)" } ?? (proxy ? "none (\(example ?? "host:port"))" : example ?? "")))
        .foregroundStyle(valid ? Color.primary : Color.red)
        .help(proxy ? "host:port, or none to turn it off" + (isDefault ? "" : " for this application") + "."
              : "Leave empty for " + (isDefault ? "none." : "the default's."))
    }

    private static let appleTalkHelp: String =
        "LocalTalk over UDP (multicast): copies on this Mac find each other, or, on the local network "
        + "through the interface chosen, other Macs, emulators and AppleTalk routers that speak LToUDP. "
        + "EtherTalk bridged onto a wired interface reaches real Macs, file servers and routers (Wi-Fi "
        + "interfaces aren't offered: access points drop its frames); it "
        + "asks for an administrator password each time AppleTalk opens, for a moment's access to "
        + "the interface's raw frames."

    private func appletalkName(_ a: String) -> String {
        switch a {
        case "lan": return "On the local network (automatic interface)"
        case let b where b.hasPrefix("bridge:"): return "EtherTalk bridged onto \(b.dropFirst(7))"
        case let l where l.hasPrefix("lan:"): return "Local network through \(l.dropFirst(4))"
        case "off": return "Off"
        default: return "This Mac only"
        }
    }

    private func displayName(_ d: String?) -> String {
        switch d {
        case "rootless": return "Rootless windows"
        case "screen": return "The whole screen in a window"
        default: return "Rootless, or the whole screen when wanted"
        }
    }

    private func scaleName(_ v: Double) -> String { v == 1.5 ? "1.5×" : "\(Int(v))×" }
    private func cpuName(_ m: Int) -> String {
        m == powerPCSetting ? "PowerPC (fat applications)" : m == 68020 ? "68020 without FPU (68k code)"
            : "68040 with FPU (68k code)"
    }
    private func appearanceName(_ a: String) -> String {
        switch a {
        case "bw", "classic": return "Black and white"
        case "native": return "macOS window frames"
        default: return "Theme: \(a)"
        }
    }
    private func highlightName(_ h: String) -> String { h == "host" ? "macOS's selection colour" : h }

    private func depthName(_ d: Int) -> String {
        switch d {
        case 1: return "Black and white"
        case 2: return "4 colours"
        case 4: return "16 colours"
        case 8: return "256 colours"
        case 16: return "Thousands of colours"
        default: return "Millions of colours"
        }
    }
}

/// The highlight colour: the host's selection colour, or one of your own.
struct HighlightPicker: View {
    @Binding var highlight: String?
    let inherit: String
    let inheritedName: String

    private enum Kind: Hashable { case inherit, host, custom }

    var body: some View {
        let kind: Kind = highlight == nil ? .inherit : highlight == "host" ? .host : .custom
        Picker("Highlight colour", selection: Binding(get: { kind }, set: { k in
            switch k {
            case .inherit: highlight = nil
            case .host: highlight = "host"
            case .custom: if kind != .custom { highlight = "#CCCCFF" }
            }
        })) {
            Text("\(inherit) (\(inheritedName))").tag(Kind.inherit)
            Text("macOS's selection colour").tag(Kind.host)
            Text("Custom").tag(Kind.custom)
        }
        if kind == .custom {
            ColorPicker("Custom colour", selection: Binding(get: { color(highlight) }, set: { highlight = hex($0) }),
                        supportsOpacity: false)
        }
    }

    private func color(_ s: String?) -> Color {
        guard let s, let rgb = parseHighlight(s) else { return .blue }
        return Color(red: Double(rgb.0) / 65535, green: Double(rgb.1) / 65535, blue: Double(rgb.2) / 65535)
    }

    private func hex(_ c: Color) -> String {
        let ns = NSColor(c).usingColorSpace(.sRGB) ?? .systemBlue
        func b(_ v: CGFloat) -> Int { Int((v * 255).rounded()) }
        return String(format: "#%02X%02X%02X", b(ns.redComponent), b(ns.greenComponent), b(ns.blueComponent))
    }
}

// MARK: - The application

@MainActor final class ShellDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    var window: NSWindow?
    var statusItem: NSStatusItem?
    let model = ShellModel()

    func applicationDidFinishLaunching(_ notification: Notification) {
        showWindow()
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.image = NSImage(systemSymbolName: "macwindow", accessibilityDescription: "Blue Box")
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        statusItem = item
    }

    /// The status item's menu, built as it opens: running and recent
    /// applications, the window, quitting.
    private func header(_ title: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let running = model.running.keys.sorted()
        if !running.isEmpty {
            menu.addItem(header("Running"))
            for path in running {
                let item = NSMenuItem(title: (path as NSString).lastPathComponent, action: nil, keyEquivalent: "")
                item.isEnabled = false
                menu.addItem(item)
            }
            menu.addItem(.separator())
        }
        if !model.recent.isEmpty {
            menu.addItem(header("Recent"))
            for path in model.recent {
                let item = NSMenuItem(title: (path as NSString).lastPathComponent, action: #selector(launchRecent(_:)),
                                      keyEquivalent: "")
                item.target = self
                item.representedObject = path
                menu.addItem(item)
            }
            menu.addItem(.separator())
        }
        let open = NSMenuItem(title: "Open Blue Box", action: #selector(openWindow(_:)), keyEquivalent: "")
        open.target = self
        menu.addItem(open)
        let importItem = NSMenuItem(title: "Import…", action: #selector(importFiles(_:)), keyEquivalent: "")
        importItem.target = self
        menu.addItem(importItem)
        menu.addItem(NSMenuItem(title: "Quit Blue Box", action: #selector(NSApplication.terminate(_:)), keyEquivalent: ""))
    }

    @objc func launchRecent(_ sender: NSMenuItem) {
        if let path = sender.representedObject as? String { model.launch(path: path) }
    }

    @objc func openWindow(_ sender: Any?) { showWindow() }
    @objc func importFiles(_ sender: Any?) { model.chooseImport() }
    @objc func importFonts(_ sender: Any?) { model.chooseFontImport() }
    @objc func refreshLibrary(_ sender: Any?) { model.refresh() }

    /// Clicking the Dock icon brings the window back.
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { showWindow() }
        return true
    }

    private func showWindow() {
        if let window {
            window.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            return
        }
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 560),
                         styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                         backing: .buffered, defer: false)
        w.title = "Blue Box"
        w.isReleasedWhenClosed = false
        w.contentView = NSHostingView(rootView: ShellView(model: model))
        w.center()
        w.setFrameAutosaveName("BlueBoxShell")
        w.makeKeyAndOrderFront(nil)
        window = w
        NSApp.activate(ignoringOtherApps: true)
        // For tests and documentation: draw the window to a PNG and quit.
        if let out = ProcessInfo.processInfo.environment["BLUEBOX_SHELL_SNAPSHOT"] {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                if let view = w.contentView, let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                    view.cacheDisplay(in: view.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: out))
                }
                NSApp.terminate(nil)
            }
        }
    }

    /// The status item keeps the shell in the background, like the Finder.
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    /// Files dropped on the Dock icon are imported too.
    func application(_ application: NSApplication, open urls: [URL]) { model.importFiles(urls) }
}

func shellCommand(_ args: [String]) -> Never {
    MainActor.assumeIsolated {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        app.applicationIconImage = blueBoxIconImage()
        let delegate = ShellDelegate()
        app.delegate = delegate
        app.mainMenu = shellMenu()
        withExtendedLifetime(delegate) { app.run() }
    }
    exit(0)
}

@MainActor private func shellMenu() -> NSMenu {
    let main = NSMenu()
    let appItem = NSMenuItem()
    main.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "Quit Blue Box", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    let fileItem = NSMenuItem()
    main.addItem(fileItem)
    let file = NSMenu(title: "File")
    // Sent up the responder chain to the application's delegate.
    file.addItem(withTitle: "Import…", action: #selector(ShellDelegate.importFiles(_:)), keyEquivalent: "o")
    file.addItem(withTitle: "Import Fonts from Disk Image…", action: #selector(ShellDelegate.importFonts(_:)),
                 keyEquivalent: "")
    file.addItem(withTitle: "Refresh Library", action: #selector(ShellDelegate.refreshLibrary(_:)), keyEquivalent: "r")
    file.addItem(.separator())
    file.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
    fileItem.submenu = file
    let editItem = NSMenuItem()
    main.addItem(editItem)
    let edit = NSMenu(title: "Edit")
    edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
    editItem.submenu = edit
    return main
}
#endif

/// What a serial port connects to: a kind, and for some kinds a value
/// (a port number, host:port, or a device), kept as the spec that
/// `bluebox run --modem-port` takes.
struct PortPicker: View {
    let label: String
    @Binding var spec: String?
    let inherit: String
    let inherited: String
    @State private var devices: [String] = []

    enum Kind: String, CaseIterable { case modem, answer, tcp, listen, pty, device, none }

    private var kind: Kind? {
        guard let s = spec else { return nil }
        if s == "modem" { return .modem }
        if s.hasPrefix("modem:") { return .answer }
        if s.hasPrefix("tcp:") { return .tcp }
        if s.hasPrefix("listen:") { return .listen }
        if s == "pty" { return .pty }
        if s.hasPrefix("/") { return .device }
        return Kind.none /* "nothing", not unset */
    }

    private var value: String {
        guard let s = spec, let k = kind else { return "" }
        switch k {
        case .answer: return String(s.dropFirst(6))
        case .tcp: return String(s.dropFirst(4))
        case .listen: return String(s.dropFirst(7))
        case .device: return s
        default: return ""
        }
    }

    private func set(_ k: Kind?, _ v: String) {
        guard let k else { spec = nil; return }
        switch k {
        case .modem: spec = "modem"
        case .answer: spec = "modem:" + (v.isEmpty ? "2323" : v)
        case .tcp: spec = "tcp:" + (v.isEmpty ? "localhost:23" : v)
        case .listen: spec = "listen:" + (v.isEmpty ? "2323" : v)
        case .pty: spec = "pty"
        case .device: spec = v.hasPrefix("/") ? v : (devices.first ?? "/dev/cu.usbserial")
        case .none: spec = "none"
        }
    }

    private func name(_ k: Kind) -> String {
        switch k {
        case .modem: return "Modem (dials over the internet)"
        case .answer: return "Modem that answers calls on a port"
        case .tcp: return "TCP connection to a host"
        case .listen: return "TCP connection from a host"
        case .pty: return "Pseudo-terminal"
        case .device: return "Serial device"
        case .none: return "Nothing"
        }
    }

    private func describe(_ s: String) -> String {
        if s == "modem" { return "modem" }
        if s == "none" { return "nothing" }
        if s == "pty" { return "pseudo-terminal" }
        return s
    }

    var body: some View {
        Picker(label, selection: Binding(get: { kind }, set: { set($0, value) })) {
            Text("\(inherit) (\(describe(inherited)))").tag(Kind?.none)
            ForEach(Kind.allCases, id: \.self) { k in Text(name(k)).tag(Kind?.some(k)) }
        }
        .onAppear { devices = Self.serialDevices() }
        switch kind {
        case .answer?, .listen?:
            TextField("Port", text: Binding(get: { value }, set: { set(kind, $0) }))
        case .tcp?:
            TextField("Host:port", text: Binding(get: { value }, set: { set(kind, $0) }))
        case .device?:
            Picker("Device", selection: Binding(get: { value }, set: { set(.device, $0) })) {
                ForEach(Array(Set(devices + [value])).sorted(), id: \.self) { d in Text(d).tag(d) }
            }
        default:
            EmptyView()
        }
    }

    /// The host's serial devices (/dev/cu.*).
    static func serialDevices() -> [String] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        return names.filter { $0.hasPrefix("cu.") }.map { "/dev/" + $0 }.sorted()
    }
}

/// The host's network interfaces with an IPv4 address (what LocalTalk over
/// UDP's multicast can use), loopback left out.
struct NetworkInterface {
    let name: String
    let address: String
    var label: String { "\(name) (\(address))" }

    static func all() -> [NetworkInterface] {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return [] }
        defer { freeifaddrs(list) }
        var out = [NetworkInterface]()
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let i = p.pointee
            guard let sa = i.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET),
                  i.ifa_flags & UInt32(IFF_LOOPBACK) == 0, i.ifa_flags & UInt32(IFF_UP) != 0 else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
            let name = String(cString: i.ifa_name)
            if !out.contains(where: { $0.name == name }) {
                let bytes = host.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }
                out.append(NetworkInterface(name: name, address: String(decoding: bytes, as: UTF8.self)))
            }
        }
        return out
    }
}

/// Printing: the print panel, or every job saved as a PDF in a folder.
struct PrintFolderPicker: View {
    @Binding var folder: String?
    let inherit: String
    let inherited: String?

    private enum Kind: Hashable { case inherit, panel, folder }
    private static let panel = ""

    var body: some View {
        // An empty string stands for "the print panel" overriding an inherited folder.
        let kind: Kind = folder == nil ? .inherit : folder == Self.panel ? .panel : .folder
        Picker("Printed pages", selection: Binding(get: { kind }, set: { k in
            switch k {
            case .inherit: folder = nil
            case .panel: folder = Self.panel
            case .folder: if kind != .folder { choose() }
            }
        })) {
            Text("\(inherit) (\(inherited.map { "saved in " + name($0) } ?? "the print panel"))").tag(Kind.inherit)
            Text("The print panel").tag(Kind.panel)
            Text(kind == .folder ? "Saved as PDF in \(name(folder!))" : "Saved as PDF in a folder…").tag(Kind.folder)
        }
        .help("Saving to a folder skips the print panel: each printed document becomes a PDF there, named "
              + "after its window and the time.")
        if kind == .folder {
            Button("Choose Folder…") { choose() }
        }
    }

    private func name(_ path: String) -> String { (path as NSString).abbreviatingWithTildeInPath }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = true
        panel.prompt = "Choose"
        if panel.runModal() == .OK, let url = panel.url { folder = url.path }
    }
}
