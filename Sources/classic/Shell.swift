// The Classic shell (classic shell): one window that is the home of the
// environment.  A sidebar lists the default settings and every application
// in the library and on the classic Mac's disk; an application's page
// launches it (its own `classic run` process, as from the command line)
// and holds its settings.  Archives dropped on the window are imported.
//
// Settings live in the library's Settings.json (Settings.swift), which
// `classic run` reads; each control here edits one field of AppSettings,
// so a new option is a field there and a control in SettingsForm.

#if canImport(AppKit)
import AppKit
import ClassicHost
import SwiftUI

/// An application the shell can launch.
struct ShellApp: Identifiable, Hashable {
    let path: String
    let place: String        /* "Library" or "Macintosh HD" */
    let folder: String       /* the folder it is in, relative to its place */
    let architecture: String
    var id: String { path }
    var name: String { (path as NSString).lastPathComponent }
}

/// A sidebar entry: the defaults, or one application.
enum ShellItem: Hashable {
    case defaults
    case app(ShellApp)
}

@MainActor final class ShellModel: ObservableObject {
    @Published var apps: [ShellApp] = []
    @Published var themes: [String] = []
    @Published var importing: [String] = []
    @Published var messages: [String] = []
    @Published var settings: [String: AppSettings] = [:]
    private var iconCache: [String: NSImage] = [:]
    let library = defaultLibrary()

    /// An application's own Finder icon (from its bundle), or a generic one.
    func icon(_ app: ShellApp) -> NSImage {
        if let cached = iconCache[app.path] { return cached }
        let image = dockImage(finderBundle(applicationAt: app.path).appIcons)
            ?? NSImage(systemSymbolName: "app", accessibilityDescription: nil) ?? NSImage()
        iconCache[app.path] = image
        return image
    }

    init() { refresh() }

    /// Scan the library and the disk again (after an import or an install).
    func refresh() {
        var found: [ShellApp] = []
        for (root, place) in [(library, "Library"), (defaultDisk(), "Macintosh HD")] {
            guard FileManager.default.fileExists(atPath: root) else { continue }
            for a in scan(root).filter({ $0.isApplication }) {
                let rel = String(a.path.dropFirst(root.count + 1))
                if rel.hasPrefix("System Folder/") && !rel.hasPrefix("System Folder/Apple Menu Items/") { continue }
                found.append(ShellApp(path: a.path, place: place, folder: (rel as NSString).deletingLastPathComponent,
                                      architecture: a.architecture))
            }
        }
        apps = found.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        themes = classicThemes(in: library).map { ($0 as NSString).lastPathComponent }.sorted()
        settings = loadSettings(library)
    }

    func settingsBinding(_ key: String) -> Binding<AppSettings> {
        Binding(get: { self.settings[key] ?? AppSettings() },
                set: { new in
                    self.settings[key] = new.isEmpty ? nil : new
                    saveSettings(self.settings, library: self.library)
                })
    }

    /// Run an application as `classic run` does from a terminal: a process of
    /// its own, with its own Dock icon and rootless windows.
    func launch(_ app: ShellApp) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Bundle.main.executablePath ?? CommandLine.arguments[0])
        p.arguments = ["run", app.path]
        do { try p.run() } catch { messages.append("Could not launch \(app.name): \(error.localizedDescription)") }
    }

    /// Import dropped or chosen files into the library, off the main thread.
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
    @State private var selection: ShellItem? = .defaults
    @State private var dropTargeted = false

    var body: some View {
        NavigationSplitView {
            List(selection: $selection) {
                Section("Settings") {
                    Label("Defaults", systemImage: "slider.horizontal.3").tag(ShellItem.defaults)
                }
                ForEach(["Library", "Macintosh HD"], id: \.self) { place in
                    let here = model.apps.filter { $0.place == place }
                    if !here.isEmpty {
                        Section(place) {
                            ForEach(here) { app in
                                HStack {
                                    Image(nsImage: model.icon(app)).resizable().interpolation(.none)
                                        .frame(width: 16, height: 16)
                                    VStack(alignment: .leading, spacing: 0) {
                                        Text(app.name)
                                        if !app.folder.isEmpty {
                                            Text(app.folder).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                                        }
                                    }
                                }
                                .tag(ShellItem.app(app))
                            }
                        }
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 220, ideal: 260)
        } detail: {
            VStack(spacing: 0) {
                switch selection {
                case .app(let app): AppPage(model: model, app: app)
                default: DefaultsPage(model: model)
                }
                StatusBar(model: model)
            }
        }
        .toolbar {
            ToolbarItem {
                Button { chooseImport() } label: { Label("Import…", systemImage: "square.and.arrow.down") }
                    .help("Import archives, disk images or MacBinary files into the library")
            }
            ToolbarItem {
                Button { model.refresh() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
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
        .overlay(dropTargeted ? RoundedRectangle(cornerRadius: 8).stroke(Color.accentColor, lineWidth: 3) : nil)
        .frame(minWidth: 720, minHeight: 440)
    }

    private func chooseImport() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = false
        panel.message = "Choose archives (.sit, .sea, .cpt, .hqx, .bin) or disk images to import"
        if panel.runModal() == .OK { model.importFiles(panel.urls) }
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
            SettingsForm(settings: model.settingsBinding(defaultSettingsKey), themes: model.themes, isDefault: true)
        }
        .formStyle(.grouped)
        .navigationTitle("Defaults")
    }
}

struct AppPage: View {
    @ObservedObject var model: ShellModel
    let app: ShellApp
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
                    Button("Launch") { model.launch(app) }.keyboardShortcut(.defaultAction)
                }
            }
            SettingsForm(settings: model.settingsBinding(settingsKey(app.path)), themes: model.themes, isDefault: false)
        }
        .formStyle(.grouped)
        .navigationTitle(app.name)
    }
}

/// The settings, as pickers: "Default" leaves one unset (taken from the
/// defaults, or for the defaults themselves from the built-in value).
struct SettingsForm: View {
    @Binding var settings: AppSettings
    let themes: [String]
    let isDefault: Bool

    private var inherit: String { isDefault ? "Built-in" : "Default" }

    var body: some View {
        Section("Display") {
            Picker("Scale", selection: $settings.scale) {
                Text("\(inherit) (1×)").tag(Double?.none)
                Text("1× (one point per Mac pixel)").tag(Double?.some(1))
                Text("1.5×").tag(Double?.some(1.5))
                Text("2×").tag(Double?.some(2))
            }
            Picker("Screen depth", selection: $settings.depth) {
                Text("\(inherit) (millions of colours)").tag(Int?.none)
                ForEach([1, 2, 4, 8, 16, 32], id: \.self) { d in
                    Text(depthName(d)).tag(Int?.some(d))
                }
            }
            Picker("Appearance", selection: $settings.appearance) {
                Text("\(inherit) (black and white)").tag(String?.none)
                Text("Black and white").tag(String?.some("bw"))
                Text("macOS window frames").tag(String?.some("native"))
                ForEach(themes, id: \.self) { t in Text("Theme: \(t)").tag(String?.some(t)) }
            }
            HighlightPicker(highlight: $settings.highlight, inherit: inherit)
        }
        Section("Machine") {
            Picker("Processor", selection: $settings.cpu) {
                Text("\(inherit) (68040 with FPU)").tag(Int?.none)
                Text("68040 with FPU").tag(Int?.some(68040))
                Text("68020 without FPU").tag(Int?.some(68020))
            }
        }
    }

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
            Text("\(inherit) (macOS's selection colour)").tag(Kind.inherit)
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

@MainActor final class ShellDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow?
    let model = ShellModel()

    func applicationDidFinishLaunching(_ notification: Notification) {
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 560),
                         styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                         backing: .buffered, defer: false)
        w.title = "Classic"
        w.contentView = NSHostingView(rootView: ShellView(model: model))
        w.center()
        w.setFrameAutosaveName("ClassicShell")
        w.makeKeyAndOrderFront(nil)
        window = w
        NSApp.activate(ignoringOtherApps: true)
        // For tests and documentation: draw the window to a PNG and quit.
        if let out = ProcessInfo.processInfo.environment["CLASSIC_SHELL_SNAPSHOT"] {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                if let view = w.contentView, let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                    view.cacheDisplay(in: view.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: out))
                }
                NSApp.terminate(nil)
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// Files dropped on the Dock icon are imported too.
    func application(_ application: NSApplication, open urls: [URL]) { model.importFiles(urls) }
}

func shellCommand(_ args: [String]) -> Never {
    MainActor.assumeIsolated {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
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
    appMenu.addItem(withTitle: "Quit Classic", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    let editItem = NSMenuItem()
    main.addItem(editItem)
    let edit = NSMenu(title: "Edit")
    edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
    editItem.submenu = edit
    return main
}
#endif
