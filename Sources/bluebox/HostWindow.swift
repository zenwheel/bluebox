// macOS host: the classic screen in a window (screen-in-a-window mode).
//
// The machine runs on a background thread; the window redraws the 1-bit
// framebuffer at 60 Hz and forwards mouse and keyboard input.  macOS virtual
// key codes are the classic ADB key codes, so they pass straight through.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Toolbox

/// Host points per Mac pixel: 1, 1.5 or 2 (--scale, or the app's setting).
nonisolated(unsafe) var displayScale: CGFloat = 1

final class ScreenView: NSView {
    private var timer: Timer?
    private var rgba = [UInt32]()

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func resetCursorRects() { addCursorRect(bounds, cursor: CursorSync.shared.cursor) }

    override init(frame: NSRect) {
        super.init(frame: frame)
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.refreshIfChanged()
                CursorSync.shared.tick()
                if machine_halted() {
                    relaunchAtWantedDepth()
                    NSApp.terminate(nil)
                }
            }
        }
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private var lastChecksum: Int = -1

    /// Ask for a redraw only when the framebuffer changed (cheap hash check).
    func refreshIfChanged() {
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        if rgba.count != Int(w) * Int(h) { rgba = [UInt32](repeating: 0, count: Int(w) * Int(h)) }
        rgba.withUnsafeMutableBytes { raw in
            machine_screen_rgba(0, 0, w, h, raw.baseAddress!.assumingMemoryBound(to: UInt8.self))
        }
        var hasher = Hasher()
        rgba.withUnsafeBytes { hasher.combine(bytes: $0) }
        let sum = hasher.finalize()
        if sum != lastChecksum {
            lastChecksum = sum
            needsDisplay = true
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        let width = Int(w), height = Int(h)
        guard rgba.count == width * height else { return }
        let space = CGColorSpaceCreateDeviceRGB()
        rgba.withUnsafeMutableBytes { buf in
            guard let ctx = CGContext(data: buf.baseAddress, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: width * 4, space: space,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
                  let image = ctx.makeImage(),
                  let gc = NSGraphicsContext.current?.cgContext else { return }
            gc.interpolationQuality = .none
            gc.saveGState()
            gc.translateBy(x: 0, y: bounds.height)
            gc.scaleBy(x: 1, y: -1)
            gc.draw(image, in: CGRect(x: 0, y: 0, width: bounds.width, height: bounds.height))
            gc.restoreGState()
        }
    }

    // MARK: Mouse

    private func post(_ event: NSEvent, button: Int32) {
        let p = convert(event.locationInWindow, from: nil)
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        let x = Int32(p.x * CGFloat(w) / bounds.width), y = Int32(p.y * CGFloat(h) / bounds.height)
        machine_post_mouse(x, y, button)
    }

    override func mouseDown(with event: NSEvent) { post(event, button: 1) }
    override func mouseUp(with event: NSEvent) { post(event, button: 0) }
    override func mouseDragged(with event: NSEvent) { post(event, button: -1) }
    override func mouseMoved(with event: NSEvent) { post(event, button: -1) }

    // MARK: Keyboard

    static func modifiers(_ flags: NSEvent.ModifierFlags) -> UInt32 {
        var m: UInt32 = 0
        if flags.contains(.command) { m |= 0x100 }
        if flags.contains(.shift) { m |= 0x200 }
        if flags.contains(.capsLock) { m |= 0x400 }
        if flags.contains(.option) { m |= 0x800 }
        if flags.contains(.control) { m |= 0x1000 }
        return m
    }

    /// The classic character code for a key event (MacRoman).
    static func charCode(_ event: NSEvent) -> Int32 {
        switch event.keyCode {
        case 123: return 28  // left
        case 124: return 29  // right
        case 125: return 31  // down
        case 126: return 30  // up
        case 51: return 8    // delete (backspace)
        case 117: return 127 // forward delete
        case 36: return 13   // return
        case 76: return 3    // enter
        case 48: return 9    // tab
        case 53: return 27   // escape
        default: break
        }
        var chars = event.modifierFlags.contains(.command) ? event.charactersIgnoringModifiers : event.characters
        if event.type == .keyDown && !event.modifierFlags.contains(.command) {
            // Through the keyboard layout, so dead keys (Option-E, E) compose.
            guard let composed = keyTranslator.translate(event) else { return -1 }
            chars = composed
        }
        guard let s = chars, let data = s.data(using: .macOSRoman, allowLossyConversion: true), let b = data.first else {
            return -1
        }
        return Int32(b)
    }

    override func keyDown(with event: NSEvent) {
        machine_post_key(Self.charCode(event), Int32(event.keyCode), Self.modifiers(event.modifierFlags), 1,
                         event.isARepeat ? 1 : 0)
    }

    override func keyUp(with event: NSEvent) {
        machine_post_key(Self.charCode(event), Int32(event.keyCode), Self.modifiers(event.modifierFlags), 0, 0)
    }

    override func flagsChanged(with event: NSEvent) { Self.postModifier(event) }

    /// A modifier key pressed or released (flagsChanged says only that its
    /// state changed): down while its flag is set.  The right-hand keys are
    /// the left-hand ones in the KeyMap, as programs test those (Shift 56).
    static func postModifier(_ event: NSEvent) {
        let keys: [UInt16: (Int32, NSEvent.ModifierFlags)] = [
            56: (56, .shift), 60: (56, .shift), 58: (58, .option), 61: (58, .option),
            59: (59, .control), 62: (59, .control), 55: (55, .command), 54: (55, .command), 57: (57, .capsLock),
        ]
        guard let (code, flag) = keys[event.keyCode] else { return }
        let down: Int32 = event.modifierFlags.contains(flag) ? 1 : 0
        machine_post_key(-1, code, modifiers(event.modifierFlags), down, 0)
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        // Command keys belong to the classic app, not the host menu.
        if event.modifierFlags.contains(.command) && event.charactersIgnoringModifiers != "q" {
            keyDown(with: event)
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}

/// Documents opened from the Finder or Dock become Open Documents events.
/// The first event is Open Application only if nothing was opened at launch,
/// as with the System 7 Finder.
struct LaunchEvents {
    private var launched = false
    private var openedAtLaunch = false

    mutating func open(_ urls: [URL]) {
        let paths = urls.filter(\.isFileURL).map(\.path)
        guard !paths.isEmpty else { return }
        if !launched { openedAtLaunch = true }
        var cpaths = paths.map { strdup($0) }
        cpaths.withUnsafeMutableBufferPointer { buf in
            buf.baseAddress!.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: buf.count) {
                machine_open_documents($0, Int32(buf.count), false)
            }
        }
        cpaths.forEach { free($0) }
    }

    mutating func finishLaunching() {
        if !launched && !openedAtLaunch && !launchDocumentsSent { machine_send_open_application() }
        launched = true
    }
}

@MainActor let keyTranslator = KeyTranslator()

/// Name the host process after the classic application, so the macOS
/// application menu, Dock and app switcher say "TeachText", not "classic".
/// Must run before NSApplication is created.
func nameProcess(_ name: String) {
    ProcessInfo.processInfo.processName = name
    if let info = Bundle.main.infoDictionary as NSDictionary? as? NSMutableDictionary {
        info["CFBundleName"] = name
        info["CFBundleDisplayName"] = name
    } else if let info = Bundle.main.infoDictionary as NSDictionary? {
        info.setValue(name, forKey: "CFBundleName")
    }
}

/// The classic cursor on the host: hidden while the application hides it
/// (only over its own windows, so other applications stay reachable), its
/// own image when it sets one, and ObscureCursor's hide-until-moved.
@MainActor final class CursorSync {
    static let shared = CursorSync()
    private(set) var cursor = NSCursor.arrow
    private var version: UInt32 = .max
    private var obscures: UInt32 = 0
    private var hiddenOnHost = false
    private var imageKey: [Int] = []
    private var state = machine_cursor()

    func tick() {
        let v = machine_cursor_state(&state)
        if v != version {
            version = v
            if state.obscures != obscures {
                obscures = state.obscures
                NSCursor.setHiddenUntilMouseMoves(true)
            }
            let key = [Int(state.kind), Int(state.hot_h), Int(state.hot_v)]
                + withUnsafeBytes(of: state.data) { $0.map(Int.init) } + withUnsafeBytes(of: state.mask) { $0.map(Int.init) }
            if key != imageKey {
                imageKey = key
                setImage()
            }
        }
        let overUs = NSApp.windows.contains { $0.isVisible && $0.frame.contains(NSEvent.mouseLocation) }
        let hide = state.hidden && overUs && NSApp.isActive
        if hide != hiddenOnHost {
            hiddenOnHost = hide
            if hide { NSCursor.hide() } else { NSCursor.unhide() }
        }
    }

    private func setImage() {
        let next: NSCursor
        // The System's own cursors as the host's (the plus and the watch
        // have no public macOS equivalent and keep their classic images).
        switch Int(state.kind) {
        case Int(MACHINE_CURSOR_ARROW): next = .arrow
        case Int(MACHINE_CURSOR_IBEAM): next = .iBeam
        case Int(MACHINE_CURSOR_CROSS): next = .crosshair
        default: next = classicImage()
        }
        cursor = next
        for w in NSApp.windows { w.invalidateCursorRects(for: w.contentView ?? NSView()) }
        if NSApp.windows.contains(where: { $0.isVisible && $0.frame.contains(NSEvent.mouseLocation) }) { next.set() }
    }

    /// The cursor's own 16x16 image: black, white or clear by data and mask
    /// (data outside the mask, which inverts, shown black).
    /// At the display scale, in whole device pixels (nearest neighbour), so it
    /// matches the windows it points at.
    private func classicImage() -> NSCursor {
        let points = 16 * displayScale
        let n = Int((points * 2).rounded()) // device pixels on a Retina display
        let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: n, pixelsHigh: n, bitsPerSample: 8,
                                   samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                   colorSpaceName: .deviceRGB, bytesPerRow: n * 4, bitsPerPixel: 32)!
        let px = rep.bitmapData!
        let data = withUnsafeBytes(of: state.data) { Array($0) }
        let mask = withUnsafeBytes(of: state.mask) { Array($0) }
        for py in 0..<n {
            for pxl in 0..<n {
                let x = pxl * 16 / n, y = py * 16 / n
                let bit = UInt8(0x80 >> (x % 8)), i = y * 2 + x / 8
                let d = data[i] & bit != 0, m = mask[i] & bit != 0
                let o = (py * n + pxl) * 4
                let v: UInt8 = d ? 0 : 255
                px[o] = v; px[o + 1] = v; px[o + 2] = v
                px[o + 3] = d || m ? 255 : 0
            }
        }
        rep.size = NSSize(width: points, height: points)
        let image = NSImage(size: rep.size)
        image.addRepresentation(rep)
        return NSCursor(image: image, hotSpot: NSPoint(x: CGFloat(state.hot_h) * displayScale,
                                                       y: CGFloat(state.hot_v) * displayScale))
    }
}

/// The classic highlight colour (HiliteRGB) follows the host's selection
/// colour, so selected text and rows look as the host's do.
@MainActor func useHostSelectionColor() {
    guard let c = NSColor.selectedTextBackgroundColor.usingColorSpace(.sRGB) else { return }
    func word(_ v: CGFloat) -> UInt16 { UInt16((max(0, min(1, v)) * 65535).rounded()) }
    machine_set_hilite_color(word(c.redComponent), word(c.greenComponent), word(c.blueComponent))
}

/// The classic application's Finder icon, for the Dock and alerts.
nonisolated(unsafe) var appIcon: NSImage?

/// Set when the command line already sent the launch event.
nonisolated(unsafe) var launchDocumentsSent = false

final class HostDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let title: String
    var window: NSWindow!
    var launchEvents = LaunchEvents()

    init(title: String) { self.title = title }

    func application(_ application: NSApplication, open urls: [URL]) { launchEvents.open(urls) }
    func applicationDidBecomeActive(_ notification: Notification) { ClipboardSync.shared.didBecomeActive() }
    func applicationWillResignActive(_ notification: Notification) { ClipboardSync.shared.willResignActive() }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // After launch: AppKit sets the icon from the main bundle while launching.
        if let appIcon { NSApp.applicationIconImage = appIcon }
        launchEvents.finishLaunching()
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        let size = NSSize(width: CGFloat(w) * displayScale, height: CGFloat(h) * displayScale)
        window = NSWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled, .closable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = title
        window.delegate = self
        let view = ScreenView(frame: NSRect(origin: .zero, size: size))
        window.contentView = view
        window.acceptsMouseMovedEvents = true
        window.center()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
        NSApp.activate(ignoringOtherApps: true)

        // Run the machine off the main thread.
        Thread.detachNewThread {
            while !machine_halted() {
                _ = machine_run(1_000_000)
            }
        }
    }

    @objc func quitApp(_ sender: NSMenuItem) { machine_request_quit() }

    func windowWillClose(_ notification: Notification) { NSApp.terminate(nil) }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// Show the classic screen in a window until the app quits.  Never returns.
@MainActor func runHostWindow(title: String, icon: NSImage?) -> Never {
    nameProcess(title)
    machine_attach_host(true)
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    appIcon = icon
    installAlertHandler()
    installFileHandler()
    installLaunchHandler()
    installColorPicker()
    installPrintHandlers()
    ClipboardSync.shared.install()
    ClassicAudio.shared.install()
    NativeControls.install()
    NotificationRelay.shared.install()
    let delegate = HostDelegate(title: title)
    app.delegate = delegate
    // Minimal app menu: Quit asks the app to quit; Option shows Force Quit.
    let menu = NSMenu()
    let appItem = NSMenuItem()
    menu.addItem(appItem)
    let appMenu = NSMenu()
    let quit = NSMenuItem(title: "Quit \(title)", action: #selector(HostDelegate.quitApp(_:)), keyEquivalent: "q")
    quit.target = delegate
    appMenu.addItem(quit)
    let force = NSMenuItem(title: "Force Quit \(title)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    force.keyEquivalentModifierMask = [.command, .option]
    force.isAlternate = true
    appMenu.addItem(force)
    appItem.submenu = appMenu
    app.mainMenu = menu
    app.run()
    exit(0)
}
#endif

#if canImport(AppKit)
/// Run on the main thread and wait.  Already there (an app's dialog hook
/// running while a panel is up), run directly rather than deadlock.
func onMain(_ body: () -> Void) {
    if Thread.isMainThread { body() } else { DispatchQueue.main.sync(execute: body) }
}

/// Text from a MacRoman C array (CR line breaks become LF).
func macRomanString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in
        let bytes = raw.prefix { $0 != 0 }
        let s = String(data: Data(bytes), encoding: .macOSRoman) ?? ""
        return s.replacingOccurrences(of: "\r", with: "\n")
    }
}

/// Classic alerts become native alerts, run on the main thread while the
/// emulator waits for the answer.
func installAlertHandler() {
    machine_set_alert_handler({ alertPtr, _ in
        guard let a = alertPtr?.pointee else { return 0 }
        let message = macRomanString(a.message)
        var buttons: [(item: Int16, title: String, isDefault: Bool)] = []
        withUnsafeBytes(of: a.buttons) { raw in
            let list = raw.bindMemory(to: machine_alert_button.self)
            for i in 0..<Int(a.nbuttons) {
                buttons.append((list[i].item, macRomanString(list[i].title), list[i].is_default))
            }
        }
        // The default button comes first: it is the one Return presses.
        buttons.sort { $0.isDefault && !$1.isDefault }
        let kind = a.kind
        var chosen: Int32 = 0
        onMain {
            MainActor.assumeIsolated {
                let alert = NSAlert()
                if let appIcon { alert.icon = appIcon }
                let paragraphs = message.components(separatedBy: "\n\n")
                alert.messageText = paragraphs.first ?? ""
                alert.informativeText = paragraphs.dropFirst().joined(separator: "\n\n")
                switch Int(kind) {
                case Int(MACHINE_ALERT_STOP): alert.alertStyle = .critical
                case Int(MACHINE_ALERT_CAUTION): alert.alertStyle = .warning
                default: alert.alertStyle = .informational
                }
                for b in buttons { alert.addButton(withTitle: b.title) }
                if buttons.isEmpty { alert.addButton(withTitle: "OK") }
                let response = alert.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
                if response >= 0 && response < buttons.count { chosen = Int32(buttons[response].item) }
                if ProcessInfo.processInfo.environment["BLUEBOX_DEBUG_HOST"] != nil {
                    eprint("alert \"\(message)\" -> response \(response), item \(chosen)\n")
                }
            }
        }
        return chosen
    }, nil)
}
#endif

#if canImport(AppKit)
/// Shows only files of the requested classic types (and folders).
final class ClassicTypeFilter: NSObject, NSOpenSavePanelDelegate {
    let types: Set<UInt32>
    init(types: Set<UInt32>) { self.types = types }

    func panel(_ sender: Any, shouldEnable url: URL) -> Bool {
        if types.isEmpty { return true }
        var isDir: ObjCBool = false
        if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue,
           url.pathExtension != "app" { return true }
        return types.contains(machine_file_type(url.path))
    }
}

/// LaunchApplication starts the other classic application as its own run of
/// this program, with the same appearance.
func installLaunchHandler() {
    machine_set_launch_handler({ pathPtr, docsPtr, ndocs, _ in
        guard let pathPtr else { return false }
        let path = String(cString: pathPtr)
        let docs = (0..<Int(ndocs)).compactMap { i in docsPtr?[i].map { String(cString: $0) } }
        // The program itself, not the bundle this run was named by.
        let me = URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL
        let exe = (Bundle.main.executableURL ?? me).resolvingSymlinksInPath()
        var args = ["run", path] + docs
        if let i = CommandLine.arguments.firstIndex(of: "--appearance"), i + 1 < CommandLine.arguments.count {
            args += ["--appearance", CommandLine.arguments[i + 1]]
        }
        let p = Process()
        p.executableURL = exe
        p.arguments = args
        var env = ProcessInfo.processInfo.environment
        env["__CFBundleIdentifier"] = nil // its own name, not its launcher's
        p.environment = env
        do { try p.run() } catch { return false }
        return true
    }, nil)
}

/// The Color Picker's GetColor: an alert with a colour well (which opens the
/// system colour panel), run on the main thread while the emulator waits.
func installColorPicker() {
    machine_set_color_picker({ promptPtr, inPtr, outPtr, _ in
        guard let inPtr, let outPtr else { return false }
        let prompt = promptPtr.map { String(cString: $0) } ?? ""
        let start = NSColor(srgbRed: CGFloat(inPtr[0]) / 65535, green: CGFloat(inPtr[1]) / 65535,
                            blue: CGFloat(inPtr[2]) / 65535, alpha: 1)
        var chosen: NSColor?
        let ask = {
            MainActor.assumeIsolated {
                let alert = NSAlert()
                alert.messageText = prompt.isEmpty ? "Choose a color:" : prompt
                alert.addButton(withTitle: "OK")
                alert.addButton(withTitle: "Cancel")
                let well = NSColorWell(frame: NSRect(x: 0, y: 0, width: 160, height: 44))
                well.color = start
                alert.accessoryView = well
                if alert.runModal() == .alertFirstButtonReturn { chosen = well.color }
                NSColorPanel.shared.orderOut(nil)
            }
        }
        if Thread.isMainThread { ask() } else { DispatchQueue.main.sync(execute: ask) }
        guard let c = chosen?.usingColorSpace(.sRGB) else { return false }
        outPtr[0] = UInt16(max(0, min(1, c.redComponent)) * 65535)
        outPtr[1] = UInt16(max(0, min(1, c.greenComponent)) * 65535)
        outPtr[2] = UInt16(max(0, min(1, c.blueComponent)) * 65535)
        return true
    }, nil)
}

/// Standard File becomes the host's open and save panels, run on the main
/// thread while the emulator waits.
func installFileHandler() {
    machine_set_file_handler({ rqPtr, pathOut, pathSize, _ in
        guard let rq = rqPtr?.pointee, let pathOut else { return false }
        let save = rq.save
        let prompt = withUnsafeBytes(of: rq.prompt) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        let name = withUnsafeBytes(of: rq.default_name) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        let dir = withUnsafeBytes(of: rq.directory) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        let types = withUnsafeBytes(of: rq.types) { raw in
            Set(raw.bindMemory(to: UInt32.self).prefix(Int(rq.ntypes)))
        }
        let items = withUnsafeBytes(of: rq.items) { raw in
            Array(raw.bindMemory(to: machine_file_item.self).prefix(Int(rq.nitems)))
        }
        var chosen: String?
        onMain {
            MainActor.assumeIsolated {
                let panel: NSSavePanel
                let filter = ClassicTypeFilter(types: types)
                if save {
                    panel = NSSavePanel()
                    panel.nameFieldStringValue = name
                    // Classic names are taken as typed: no extension added or hidden.
                    panel.allowsOtherFileTypes = true
                    panel.isExtensionHidden = false
                    panel.canSelectHiddenExtension = false
                    panel.canCreateDirectories = true
                } else {
                    let open = NSOpenPanel()
                    open.canChooseFiles = true
                    open.canChooseDirectories = false
                    open.allowsMultipleSelection = false
                    open.delegate = filter
                    panel = open
                }
                if !prompt.isEmpty { panel.message = prompt }
                if !dir.isEmpty { panel.directoryURL = URL(fileURLWithPath: dir, isDirectory: true) }
                let accessory = ClassicItemsView(items: items, panel: panel)
                if !items.isEmpty {
                    panel.accessoryView = accessory
                    if let open = panel as? NSOpenPanel { open.isAccessoryViewDisclosed = true }
                }
                if panel.runModal() == .OK, let url = panel.url { chosen = url.path }
                _ = filter
            }
        }
        guard let chosen else { return false }
        let bytes = Array(chosen.utf8)
        guard bytes.count < pathSize else { return false }
        for (i, b) in bytes.enumerated() { pathOut[i] = CChar(bitPattern: b) }
        pathOut[bytes.count] = 0
        return true
    }, nil)
}
#endif

#if canImport(AppKit)
import PDFKit

/// Print settings for the session (the page layout panel edits them).
@MainActor let sessionPrintInfo: NSPrintInfo = {
    let info = NSPrintInfo.shared.copy() as! NSPrintInfo
    info.horizontalPagination = .clip
    info.verticalPagination = .clip
    return info
}()

@MainActor func pageSetup(from info: NSPrintInfo) -> machine_page_setup {
    let paper = info.paperSize, ib = info.imageablePageBounds
    return machine_page_setup(paper_width: paper.width, paper_height: paper.height,
                              left: ib.minX, top: paper.height - ib.maxY, right: ib.maxX, bottom: paper.height - ib.minY,
                              landscape: info.orientation == .landscape)
}

/// Printing goes to the host: Page Setup is the page layout panel, and the
/// recorded pages are printed as PDF through the print panel (with preview).
/// Not main-actor isolated: the handlers run on the emulator thread and hop
/// to the main thread themselves.
func installPrintHandlers() {
    var defaults = MainActor.assumeIsolated { pageSetup(from: sessionPrintInfo) }
    machine_set_page_defaults(&defaults)
    machine_set_print_handlers({ job, _ in
        guard let job else { return false }
        let pdf = ClassicPrint.pdf(job)
        var ok = false
        onMain {
            MainActor.assumeIsolated {
                guard let doc = PDFDocument(data: pdf) else { return }
                let info = sessionPrintInfo.copy() as! NSPrintInfo
                info.topMargin = 0
                info.bottomMargin = 0
                info.leftMargin = 0
                info.rightMargin = 0
                guard let op = doc.printOperation(for: info, scalingMode: .pageScaleNone, autoRotate: false) else { return }
                op.jobTitle = NSApp.windows.first(where: { $0.isKeyWindow })?.title ?? ProcessInfo.processInfo.processName
                op.showsPrintPanel = true
                op.showsProgressPanel = true
                ok = op.run()
            }
        }
        return ok
    }, { psPtr, _ in
        guard let psPtr else { return false }
        var chosen: machine_page_setup?
        onMain {
            MainActor.assumeIsolated {
                let info = sessionPrintInfo
                if NSPageLayout().runModal(with: info) == NSApplication.ModalResponse.OK.rawValue {
                    chosen = pageSetup(from: info)
                }
            }
        }
        guard let chosen else { return false }
        psPtr.pointee = chosen
        return true
    }, nil)
}

/// Where headless runs write printed pages (--print-to).
nonisolated(unsafe) var printToPath: String?

/// Headless printing: the job is written as a PDF file, no panels.
func installPrintToFile(_ path: String) {
    printToPath = path
    machine_set_print_handlers({ job, _ in
        guard let job, let path = printToPath else { return false }
        do {
            try ClassicPrint.pdf(job).write(to: URL(fileURLWithPath: path))
            print("printed \(job.pointee.npages) page(s) to \(path)")
            return true
        } catch {
            eprint("could not write \(path): \(error)\n")
            return false
        }
    }, nil, nil)
}
#endif

#if canImport(AppKit)
/// The items an app added to its Standard File dialog, laid out as in the
/// app's own dialog, below the host panel.  Clicks go to the app's dialog
/// hook, which decides the items' new states.
final class ClassicItemsView: NSView {
    weak var panel: NSSavePanel?
    var controls: [Int16: NSView] = [:]
    var origin = CGPoint.zero
    override var isFlipped: Bool { true }

    init(items: [machine_file_item], panel: NSSavePanel) {
        self.panel = panel
        let shown = items.filter { $0.visible }
        let rects = shown.map { CGRect(x: CGFloat($0.rect.left), y: CGFloat($0.rect.top),
                                       width: CGFloat($0.rect.right - $0.rect.left),
                                       height: CGFloat($0.rect.bottom - $0.rect.top)) }
        let box = rects.reduce(CGRect.null) { $0.union($1) }
        let margin: CGFloat = 8
        super.init(frame: CGRect(x: 0, y: 0, width: (box.isNull ? 0 : box.width) + 2 * margin,
                                 height: (box.isNull ? 0 : box.height) + 2 * margin))
        origin = box.isNull ? .zero : CGPoint(x: box.minX - margin, y: box.minY - margin)
        // Icons first, so buttons drawn over them still get the clicks.
        for it in shown.sorted(by: { ($0.kind == Int32(MACHINE_ITEM_ICON) ? 0 : 1) < ($1.kind == Int32(MACHINE_ITEM_ICON) ? 0 : 1) }) {
            let frame = CGRect(x: CGFloat(it.rect.left) - origin.x, y: CGFloat(it.rect.top) - origin.y,
                               width: CGFloat(it.rect.right - it.rect.left), height: CGFloat(it.rect.bottom - it.rect.top))
            let view = makeView(it, frame: frame)
            addSubview(view)
            controls[it.item] = view
        }
        refresh(items)
    }

    required init?(coder: NSCoder) { fatalError() }

    static func title(_ it: machine_file_item) -> String {
        withUnsafeBytes(of: it.title) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }

    static func icon(_ it: machine_file_item) -> NSImage {
        // A plain ICON has no mask: its inside is white, found by filling
        // the clear pixels reachable from the edge (those stay transparent).
        var on = [Bool](repeating: false, count: 32 * 32)
        withUnsafeBytes(of: it.icon) { raw in
            for y in 0..<32 { for x in 0..<32 { on[y * 32 + x] = raw[y * 4 + x / 8] & (0x80 >> UInt8(x % 8)) != 0 } }
        }
        var outside = [Bool](repeating: false, count: 32 * 32)
        var stack = (0..<32).flatMap { i in [(i, 0), (i, 31), (0, i), (31, i)] }
        while let (x, y) = stack.popLast() {
            guard x >= 0, y >= 0, x < 32, y < 32, !on[y * 32 + x], !outside[y * 32 + x] else { continue }
            outside[y * 32 + x] = true
            stack += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
        }
        var rgba = [UInt8](repeating: 0, count: 32 * 32 * 4)
        for i in 0..<(32 * 32) where !outside[i] {
            let v: UInt8 = on[i] ? 0 : 255
            rgba[i * 4] = v; rgba[i * 4 + 1] = v; rgba[i * 4 + 2] = v; rgba[i * 4 + 3] = 255
        }
        let img = IconImage(size: 32, rgba: rgba)
        return dockImage([img]).map { i in i.size = NSSize(width: 32, height: 32); return i } ?? NSImage()
    }

    func makeView(_ it: machine_file_item, frame: CGRect) -> NSView {
        let title = Self.title(it)
        let view: NSView
        switch Int(it.kind) {
        case Int(MACHINE_ITEM_CHECKBOX):
            view = NSButton(checkboxWithTitle: title, target: self, action: #selector(clicked(_:)))
        case Int(MACHINE_ITEM_RADIO):
            view = NSButton(radioButtonWithTitle: title, target: self, action: #selector(clicked(_:)))
        case Int(MACHINE_ITEM_BUTTON):
            let b = NSButton(title: title, target: self, action: #selector(clicked(_:)))
            b.bezelStyle = .push
            view = b
        case Int(MACHINE_ITEM_EDIT):
            let f = NSTextField(string: title)
            f.target = self
            f.action = #selector(edited(_:))
            view = f
        case Int(MACHINE_ITEM_ICON):
            let iv = NSImageView(image: Self.icon(it))
            iv.imageScaling = .scaleNone
            view = iv
        default:
            view = NSTextField(wrappingLabelWithString: title)
        }
        view.frame = frame
        (view as? NSControl)?.tag = Int(it.item)
        return view
    }

    /// Show the states the app's hook left the items in.
    func refresh(_ items: [machine_file_item]) {
        for it in items {
            guard let v = controls[it.item] else { continue }
            v.isHidden = !it.visible
            if let b = v as? NSButton {
                b.state = it.value != 0 ? .on : .off
                b.isEnabled = it.enabled
                if b.title != Self.title(it) { b.title = Self.title(it) }
            } else if let f = v as? NSTextField, f.isEditable == false {
                f.stringValue = Self.title(it)
            }
        }
    }

    func hit(_ item: Int16, text: String?) {
        let result = text.map { t in t.withCString { machine_file_item_hit(item, $0) } } ?? machine_file_item_hit(item, nil)
        var states = [machine_file_item](repeating: machine_file_item(), count: 16)
        let n = Int(machine_file_items(&states, 16))
        refresh(Array(states.prefix(n)))
        if result == 1 { panel?.ok(nil) }
        if result == 2 { panel?.cancel(nil) }
    }

    @objc func clicked(_ sender: NSButton) { hit(Int16(sender.tag), text: nil) }
    @objc func edited(_ sender: NSTextField) { hit(Int16(sender.tag), text: sender.stringValue) }
}
#endif

#if canImport(AppKit)
/// The classic clipboard and the host's, kept in step at the moments
/// System 7 apps expect: suspend (export) and resume (import).
@MainActor final class ClipboardSync {
    static let shared = ClipboardSync()
    private var seenChange = NSPasteboard.general.changeCount
    private var launched = false

    func install() {
        // Start with whatever text the host clipboard holds.
        if let text = NSPasteboard.general.string(forType: .string) {
            text.withCString { machine_set_clipboard_text($0) }
        }
        machine_set_clipboard_handler({ utf8, _ in
            guard let utf8 else { return }
            let text = String(cString: utf8)
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    let pb = NSPasteboard.general
                    pb.clearContents()
                    pb.setString(text, forType: .string)
                    ClipboardSync.shared.seenChange = pb.changeCount
                }
            }
        }, nil)
    }

    func didBecomeActive() {
        guard launched else { launched = true; return }
        let pb = NSPasteboard.general
        var changed = false
        if pb.changeCount != seenChange {
            seenChange = pb.changeCount
            if let text = pb.string(forType: .string) {
                text.withCString { machine_set_clipboard_text($0) }
                changed = true
            }
        }
        machine_post_suspend_resume(true, changed)
    }

    func willResignActive() {
        machine_release_keys()
        machine_post_suspend_resume(false, false)
    }
}
#endif

#if canImport(AppKit)
import UserNotifications

/// Notification Manager requests on the host: the message as a macOS
/// notification (or an alert, when there is no bundle to post as), the
/// mark as a Dock badge, and a bounce of the Dock icon.
@MainActor final class NotificationRelay {
    static let shared = NotificationRelay()
    private var attention: [Int: Int] = [:]

    func install() {
        if Bundle.main.bundleIdentifier != nil {
            UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { _, _ in }
        }
        machine_set_notification_handler({ id, text, flags, _ in
            let message = text.map { String(cString: $0) } ?? ""
            DispatchQueue.main.async {
                MainActor.assumeIsolated { NotificationRelay.shared.post(id: Int(id), message: message, flags: Int(flags)) }
            }
        }, nil)
    }

    private func post(id: Int, message: String, flags: Int) {
        if id < 0 {
            if let req = attention.removeValue(forKey: -id) { NSApp.cancelUserAttentionRequest(req) }
            if attention.isEmpty { NSApp.dockTile.badgeLabel = nil }
            UNUserNotificationCenter.current().removeDeliveredNotifications(withIdentifiers: ["bluebox-\(-id)"])
            return
        }
        if flags & Int(MACHINE_NOTIFY_MARK) != 0 { NSApp.dockTile.badgeLabel = "◆" }
        if !NSApp.isActive { attention[id] = NSApp.requestUserAttention(.criticalRequest) }
        guard !message.isEmpty else { return }
        if Bundle.main.bundleIdentifier != nil {
            let content = UNMutableNotificationContent()
            content.title = ProcessInfo.processInfo.processName
            content.body = message
            UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: "bluebox-\(id)", content: content,
                                                                         trigger: nil))
        } else {
            let alert = NSAlert()
            alert.messageText = message
            alert.alertStyle = .informational
            if let appIcon { alert.icon = appIcon }
            alert.runModal()
        }
    }
}
#endif
