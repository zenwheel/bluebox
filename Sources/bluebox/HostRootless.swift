// macOS host, rootless mode: every classic window is its own borderless
// macOS window, and the classic menu bar is the native menu bar.
//
// The Mac screen is a hidden framebuffer the size of the host display's
// visible area divided by the display scale, plus the classic menu bar's 20
// pixels, which are never shown.  Mac global (x, y) maps to host screen
// (visibleFrame.minX + x * scale, visibleFrame.maxY - (y - 20) * scale).

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Toolbox

let classicMenuBarHeight: CGFloat = 20

/// The host area the Mac screen maps onto.
@MainActor func rootlessScreenFrame() -> NSRect {
    (NSScreen.main ?? NSScreen.screens.first)?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1024, height: 748)
}

/// The Mac screen's size for the host's visible area at the display scale:
/// the classic menu bar's 20 pixels on top, which are never shown.
@MainActor func rootlessMacScreenSize() -> (Int32, Int32) {
    let vf = rootlessScreenFrame()
    return (Int32((vf.width / displayScale).rounded(.down)),
            Int32((vf.height / displayScale).rounded(.down) + classicMenuBarHeight))
}

@MainActor func macToHost(_ x: CGFloat, _ y: CGFloat) -> NSPoint {
    let vf = rootlessScreenFrame()
    return NSPoint(x: vf.minX + x * displayScale, y: vf.maxY - (y - classicMenuBarHeight) * displayScale)
}

@MainActor func hostToMac(_ p: NSPoint) -> (Int32, Int32) {
    let vf = rootlessScreenFrame()
    return (Int32(((p.x - vf.minX) / displayScale).rounded(.down)),
            Int32(((vf.maxY - p.y) / displayScale + classicMenuBarHeight).rounded(.down)))
}

final class MacWindow: NSWindow {
    var macID: UInt32 = 0
    var native = false
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

final class MacWindowView: NSView {
    var macBounds = (top: Int16(0), left: Int16(0), bottom: Int16(0), right: Int16(0))
    var mask = [UInt8]()
    var lastHash = 0
    private var rgba = [UInt32]()

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var wantsUpdateLayer: Bool { true }
    override func resetCursorRects() { addCursorRect(bounds, cursor: CursorSync.shared.cursor) }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.magnificationFilter = .nearest
        layer?.contentsGravity = .resize
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    /// Rebuild the image from the screen if this window's pixels changed.
    func refresh(force: Bool) {
        let left = Int(macBounds.left), top = Int(macBounds.top)
        let w = Int(macBounds.right) - left, h = Int(macBounds.bottom) - top
        guard w > 0, h > 0, mask.count == w * h else { return }
        if rgba.count != w * h { rgba = [UInt32](repeating: 0, count: w * h) }
        rgba.withUnsafeMutableBytes { raw in
            machine_screen_rgba(Int32(left), Int32(top), Int32(w), Int32(h), raw.baseAddress!.assumingMemoryBound(to: UInt8.self))
        }
        var hasher = Hasher()
        rgba.withUnsafeBytes { hasher.combine(bytes: $0) }
        let hash = hasher.finalize()
        if !force && hash == lastHash { return }
        lastHash = hash
        // Transparent outside the window's shape.
        for k in 0..<(w * h) where mask[k] == 0 { rgba[k] = 0 }
        let space = CGColorSpaceCreateDeviceRGB()
        let image: CGImage? = rgba.withUnsafeMutableBytes { buf in
            CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)?.makeImage()
        }
        layer?.contents = image
    }

    private func post(_ event: NSEvent, button: Int32) {
        let (x, y) = hostToMac(NSEvent.mouseLocation)
        machine_post_mouse(x, y, button)
    }

    override func mouseDown(with event: NSEvent) { post(event, button: 1) }
    override func mouseUp(with event: NSEvent) { post(event, button: 0) }
    override func mouseDragged(with event: NSEvent) { post(event, button: -1) }
}

/// While a pop-up menu is up: a window over the whole screen showing the
/// menu (drawn outside the classic windows, which show only their own
/// shapes) and taking every click, so the menu sees its clicks and a click
/// elsewhere reaches it to cancel.
final class TransientView: NSView {
    let menuLayer = CALayer()
    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        menuLayer.magnificationFilter = .nearest
        layer?.addSublayer(menuLayer)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    /// Show the Mac screen's rectangle (top, left, bottom, right) at its place.
    @MainActor func show(_ r: [Int16]) {
        let top = Int(r[0]), left = Int(r[1]), w = Int(r[3]) - left, h = Int(r[2]) - top
        guard w > 0, h > 0 else { return }
        var rgba = [UInt32](repeating: 0, count: w * h)
        rgba.withUnsafeMutableBytes { raw in
            machine_screen_rgba(Int32(left), Int32(top), Int32(w), Int32(h), raw.baseAddress!.assumingMemoryBound(to: UInt8.self))
        }
        let image: CGImage? = rgba.withUnsafeMutableBytes { buf in
            CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: CGColorSpaceCreateDeviceRGB(),
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)?.makeImage()
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let s = displayScale
        menuLayer.frame = CGRect(x: CGFloat(left) * s, y: (CGFloat(top) - classicMenuBarHeight) * s,
                                 width: CGFloat(w) * s, height: CGFloat(h) * s)
        menuLayer.contents = image
        CATransaction.commit()
    }

    private func post(_ button: Int32) {
        let (x, y) = hostToMac(NSEvent.mouseLocation)
        machine_post_mouse(x, y, button)
    }

    override func mouseDown(with event: NSEvent) { post(1) }
    override func mouseUp(with event: NSEvent) { post(0) }
    override func mouseDragged(with event: NSEvent) { post(-1) }
}

@MainActor final class RootlessHost: NSObject, NSApplicationDelegate, NSMenuDelegate, NSWindowDelegate {
    let title: String
    var script: [String] = []
    var windows: [UInt32: MacWindow] = [:]
    var windowsVersion: UInt32 = .max
    var menusVersion: UInt32 = .max
    var lastMouse = NSPoint(x: -1, y: -1)
    var timer: Timer?
    var launchEvents = LaunchEvents()
    var transientVersion: UInt32 = .max
    var transientWindow: NSWindow?

    init(title: String) { self.title = title }

    func application(_ application: NSApplication, open urls: [URL]) { launchEvents.open(urls) }
    func applicationDidBecomeActive(_ notification: Notification) { ClipboardSync.shared.didBecomeActive() }
    func applicationWillResignActive(_ notification: Notification) { ClipboardSync.shared.willResignActive() }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // After launch: AppKit sets the icon from the main bundle while launching.
        if let appIcon { NSApp.applicationIconImage = appIcon }
        launchEvents.finishLaunching()
        NSApp.activate(ignoringOtherApps: true)
        installKeyMonitor()
        let script = self.script
        Thread.detachNewThread {
            while !machine_halted() { _ = machine_run(1_000_000) }
        }
        if !script.isEmpty {
            // Scripted input from its own thread, with wall-clock pauses
            // (the emulator thread sleeps while the app waits for events).
            Thread.detachNewThread { postScriptLive(script) }
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
    }

    func tick() {
        if machine_halted() {
            relaunchAtWantedDepth()
            NSApp.terminate(nil)
            return
        }
        let mouse = NSEvent.mouseLocation
        if mouse != lastMouse {
            lastMouse = mouse
            let (x, y) = hostToMac(mouse)
            machine_post_mouse(x, y, -1)
        }
        syncWindows()
        for w in windows.values { (w.contentView as? MacWindowView)?.refresh(force: false) }
        syncTransient()
        syncMenus()
        CursorSync.shared.tick()
    }

    // MARK: Pop-up menus

    func syncTransient() {
        var r = [Int16](repeating: 0, count: 4)
        var on = false
        let version = r.withUnsafeMutableBufferPointer { machine_transient($0.baseAddress!, &on) }
        if !on {
            if let w = transientWindow {
                w.orderOut(nil)
                transientWindow = nil
            }
            transientVersion = version
            return
        }
        if transientWindow == nil {
            let frame = rootlessScreenFrame()
            let w = NSWindow(contentRect: frame, styleMask: .borderless, backing: .buffered, defer: false)
            w.isOpaque = false
            // Nearly clear: a fully transparent window would let clicks through.
            w.backgroundColor = NSColor(white: 0, alpha: 0.01)
            w.level = .popUpMenu
            w.hasShadow = false
            w.isReleasedWhenClosed = false
            w.contentView = TransientView(frame: NSRect(origin: .zero, size: frame.size))
            w.orderFrontRegardless()
            transientWindow = w
        }
        // Redraw every tick while up: the menu highlights as the mouse moves.
        (transientWindow?.contentView as? TransientView)?.show(r)
        transientVersion = version
    }

    // MARK: Windows

    func syncWindows() {
        let version = machine_windows_version()
        guard version != windowsVersion else { return }
        windowsVersion = version
        var list = [machine_window](repeating: machine_window(), count: 64)
        let n = Int(machine_windows(&list, 64))
        let current = Array(list[0..<n])
        if ProcessInfo.processInfo.environment["BLUEBOX_DEBUG_HOST"] != nil {
            FileHandle.standardError.write("rootless: version \(version), \(n) windows: \(current.map { "\($0.id) \($0.left),\($0.top)-\($0.right),\($0.bottom)" })\n".data(using: .utf8)!)
        }
        let ids = Set(current.map(\.id))
        for (id, w) in windows where !ids.contains(id) {
            w.orderOut(nil)
            windows[id] = nil
        }
        for mw in current {
            let width = Int(mw.right) - Int(mw.left), height = Int(mw.bottom) - Int(mw.top)
            let origin = macToHost(CGFloat(mw.left), CGFloat(mw.bottom))
            let frame = NSRect(x: origin.x, y: origin.y, width: CGFloat(width) * displayScale,
                               height: CGFloat(height) * displayScale)
            let native = mw.flags & UInt8(MACHINE_WINDOW_NATIVE) != 0
            if let existing = windows[mw.id], existing.native != native {
                existing.orderOut(nil)
                windows[mw.id] = nil
            }
            let window: MacWindow
            if let existing = windows[mw.id] {
                window = existing
                if native {
                    let current = window.contentRect(forFrameRect: window.frame)
                    if current != frame && !window.inLiveResize {
                        applyingFrame = true
                        window.setFrame(window.frameRect(forContentRect: frame), display: false)
                        applyingFrame = false
                    }
                } else if window.frame != frame {
                    window.setFrame(frame, display: false)
                }
            } else if native {
                var style: NSWindow.StyleMask = [.titled, .miniaturizable]
                if mw.flags & UInt8(MACHINE_WINDOW_CLOSE) != 0 { style.insert(.closable) }
                if mw.flags & UInt8(MACHINE_WINDOW_GROW) != 0 { style.insert(.resizable) }
                window = MacWindow(contentRect: frame, styleMask: style, backing: .buffered, defer: false)
                window.native = true
                window.isReleasedWhenClosed = false
                window.acceptsMouseMovedEvents = true
                window.delegate = self
                window.contentView = MacWindowView(frame: NSRect(origin: .zero, size: frame.size))
                window.standardWindowButton(.zoomButton)?.isEnabled = mw.flags & UInt8(MACHINE_WINDOW_ZOOM) != 0
                applyingFrame = true
                window.setFrame(window.frameRect(forContentRect: frame), display: false)
                applyingFrame = false
                windows[mw.id] = window
            } else {
                window = MacWindow(contentRect: frame, styleMask: [.borderless], backing: .buffered, defer: false)
                window.isOpaque = false
                window.backgroundColor = .clear
                window.hasShadow = false
                window.isReleasedWhenClosed = false
                window.acceptsMouseMovedEvents = true
                window.contentView = MacWindowView(frame: NSRect(origin: .zero, size: frame.size))
                windows[mw.id] = window
            }
            window.macID = mw.id
            if native {
                var mutable = mw
                let title = withUnsafeBytes(of: &mutable.title) { Self.string($0.baseAddress!, 256) }
                if window.title != title { window.title = title }
            }
            guard let view = window.contentView as? MacWindowView else { continue }
            view.frame = NSRect(origin: .zero, size: frame.size)
            view.macBounds = (mw.top, mw.left, mw.bottom, mw.right)
            var mask = [UInt8](repeating: 0, count: width * height)
            if machine_window_mask(mw.id, &mask, Int32(width), Int32(height)) { view.mask = mask }
            view.refresh(force: true)
        }
        // Stack them in Mac order: back first, so the front ends up on top.
        for mw in current.reversed() { windows[mw.id]?.orderFront(nil) }
        if let front = current.first, let w = windows[front.id], NSApp.isActive { w.makeKey() }
    }

    // MARK: Menus

    static func string(_ tuple: UnsafeRawPointer, _ max: Int) -> String {
        let bytes = tuple.assumingMemoryBound(to: UInt8.self)
        var out = [UInt8]()
        for i in 0..<max {
            if bytes[i] == 0 { break }
            out.append(bytes[i])
        }
        return String(bytes: out, encoding: .macOSRoman) ?? ""
    }

    var openMenus = 0
    var applyingFrame = false

    // MARK: Native window frames
    //
    // The title bar's actions go to the classic app as the clicks it knows:
    // it calls TrackGoAway, TrackBox, DragWindow or GrowWindow and gets the
    // host's answer, so its own code moves, sizes and closes the window.

    private func action(_ window: NSWindow, _ kind: Int32, _ a: Int32, _ b: Int32) {
        guard let w = window as? MacWindow, w.native else { return }
        // Where the click lands matters to some apps (a grow click inside the
        // grow box); use the window's current Mac position.
        guard let view = w.contentView as? MacWindowView else { return }
        let m = view.macBounds
        let (x, y) = kind == Int32(MACHINE_ACTION_RESIZE)
            ? (Int32(m.right) - 8, Int32(m.bottom) - 8) : (Int32(m.left) + 2, Int32(m.top) + 2)
        machine_window_action(w.macID, kind, a, b, x, y)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        action(sender, Int32(MACHINE_ACTION_CLOSE), 0, 0)
        return false
    }

    func windowShouldZoom(_ window: NSWindow, toFrame newFrame: NSRect) -> Bool {
        action(window, Int32(MACHINE_ACTION_ZOOM), 0, 0)
        return false
    }

    func windowDidMove(_ notification: Notification) {
        guard !applyingFrame, let w = notification.object as? MacWindow, w.native, !w.inLiveResize else { return }
        let content = w.contentRect(forFrameRect: w.frame)
        let (left, top) = hostToMac(NSPoint(x: content.minX, y: content.maxY))
        action(w, Int32(MACHINE_ACTION_MOVE), left, top)
    }

    func windowDidResize(_ notification: Notification) {
        // Resizes outside a live drag (window tiling, scripts) as well.
        guard !applyingFrame, let w = notification.object as? MacWindow, w.native, !w.inLiveResize else { return }
        let content = w.contentRect(forFrameRect: w.frame)
        action(w, Int32(MACHINE_ACTION_RESIZE), Int32((content.width / displayScale).rounded()),
               Int32((content.height / displayScale).rounded()))
    }

    func windowDidEndLiveResize(_ notification: Notification) {
        guard let w = notification.object as? MacWindow, w.native else { return }
        let content = w.contentRect(forFrameRect: w.frame)
        action(w, Int32(MACHINE_ACTION_RESIZE), Int32((content.width / displayScale).rounded()),
               Int32((content.height / displayScale).rounded()))
    }

    func syncMenus() {
        let version = machine_menus_version()
        guard version != menusVersion, openMenus == 0 else { return }
        menusVersion = version
        var menus = [machine_menu](repeating: machine_menu(), count: 32)
        let n = Int(machine_menus(&menus, 32))
        let bar = NSMenu()
        let appItem = NSMenuItem()
        bar.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.autoenablesItems = false
        appMenu.delegate = self
        appItem.submenu = appMenu
        for i in 0..<n {
            var m = menus[i]
            let title = withUnsafeBytes(of: &m.title) { Self.string($0.baseAddress!, 64) }
            let target: NSMenu
            if i == 0 && (title == "\u{14}" || title.isEmpty || title.unicodeScalars.first?.value == 0x14) {
                target = appMenu // the classic Apple menu becomes the application menu
            } else {
                let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
                let sub = NSMenu(title: title)
                sub.autoenablesItems = false
                sub.delegate = self
                item.submenu = sub
                item.isEnabled = m.enabled
                bar.addItem(item)
                target = sub
            }
            withUnsafeBytes(of: &m.items) { raw in
                let items = raw.bindMemory(to: machine_menu_item.self)
                for k in 0..<Int(m.nitems) {
                    var it = items[k]
                    if it.separator {
                        target.addItem(.separator())
                        continue
                    }
                    let text = withUnsafeBytes(of: &it.text) { Self.string($0.baseAddress!, 64) }
                    let key = it.key != 0 ? String(UnicodeScalar(it.key)).lowercased() : ""
                    let ni = NSMenuItem(title: text, action: #selector(chooseMenu(_:)), keyEquivalent: key)
                    ni.target = self
                    ni.isEnabled = it.enabled && m.enabled
                    ni.state = it.mark == 0x12 ? .on : .off
                    ni.representedObject = [Int(m.id), k + 1]
                    if it.style & 1 != 0 { ni.attributedTitle = NSAttributedString(string: text, attributes: [.font: NSFont.boldSystemFont(ofSize: 0)]) }
                    target.addItem(ni)
                }
            }
        }
        if appMenu.numberOfItems > 0 { appMenu.addItem(.separator()) }
        // Quit asks the classic app to quit; with Option held, Force Quit
        // ends it at once.  (Command keys themselves go to the app.)
        let quit = NSMenuItem(title: "Quit \(title)", action: #selector(quitApp(_:)), keyEquivalent: "q")
        quit.target = self
        appMenu.addItem(quit)
        let force = NSMenuItem(title: "Force Quit \(title)", action: #selector(NSApplication.terminate(_:)),
                               keyEquivalent: "q")
        force.keyEquivalentModifierMask = [.command, .option]
        force.isAlternate = true
        appMenu.addItem(force)
        NSApp.mainMenu = bar
    }

    // MARK: Menu tracking
    //
    // Opening a host menu is a click in the classic menu bar: the app adjusts
    // its menus (enabling items, changing their text) and calls MenuSelect,
    // which waits until the host menu closes.

    func menuWillOpen(_ menu: NSMenu) {
        openMenus += 1
        machine_menu_tracking_begin() // the first menu starts the session; later ones just wait for it
        refreshItems(of: menu)
    }

    // Command keys go to the classic app through the key monitor.
    func menuHasKeyEquivalent(_ menu: NSMenu, for event: NSEvent, target: AutoreleasingUnsafeMutablePointer<AnyObject?>,
                              action: UnsafeMutablePointer<Selector?>) -> Bool { false }

    func menuDidClose(_ menu: NSMenu) {
        // The chosen item's action comes after this; end tracking after it.
        // Moving to another menu opens it before this close is handled, so
        // the session ends only when no host menu is open.
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                self.openMenus = max(0, self.openMenus - 1)
                if self.openMenus == 0 { machine_menu_tracking_end() }
            }
        }
    }

    /// Update a menu's items in place from the current snapshot.
    func refreshItems(of menu: NSMenu) {
        var menus = [machine_menu](repeating: machine_menu(), count: 32)
        let n = Int(machine_menus(&menus, 32))
        for item in menu.items {
            guard let pair = item.representedObject as? [Int], pair.count == 2 else { continue }
            guard var m = menus[0..<n].first(where: { Int($0.id) == pair[0] }) else { continue }
            let k = pair[1] - 1
            guard k >= 0, k < Int(m.nitems) else { continue }
            withUnsafeBytes(of: &m.items) { raw in
                var it = raw.bindMemory(to: machine_menu_item.self)[k]
                let text = withUnsafeBytes(of: &it.text) { Self.string($0.baseAddress!, 64) }
                item.isEnabled = it.enabled && m.enabled
                item.state = it.mark == 0x12 ? .on : .off
                if item.title != text { item.title = text }
            }
        }
    }

    @objc func quitApp(_ sender: NSMenuItem) { machine_request_quit() }

    @objc func chooseMenu(_ sender: NSMenuItem) {
        guard let pair = sender.representedObject as? [Int], pair.count == 2 else { return }
        machine_choose_menu(Int16(pair[0]), Int16(pair[1]))
    }

    // MARK: Keyboard

    func installKeyMonitor() {
        NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { event in
            let forClassic = MainActor.assumeIsolated { () -> Bool in
                // Native panels and alerts (Standard File, Alert) keep their keys.
                if let w = event.window ?? NSApp.keyWindow, !(w is MacWindow) { return false }
                let mods = ScreenView.modifiers(event.modifierFlags)
                switch event.type {
                case .keyDown:
                    machine_post_key(ScreenView.charCode(event), Int32(event.keyCode), mods, 1, event.isARepeat ? 1 : 0)
                case .keyUp:
                    machine_post_key(ScreenView.charCode(event), Int32(event.keyCode), mods, 0, 0)
                default:
                    ScreenView.postModifier(event)
                }
                return true
            }
            return forClassic ? nil : event // the classic app handles every key, command keys included
        }
    }
}

/// Post scripted input while the machine runs; wait:N pauses N/1000 ms (at least 200 ms).
func postScriptLive(_ script: [String]) {
    for step in script {
        if step.hasPrefix("wait:"), let n = UInt64(step.dropFirst(5)) {
            usleep(useconds_t(max(200_000, min(n, 10_000_000))))
            continue
        }
        if let (menu, item) = menuStep(step) {
            machine_choose_menu(menu, item)
            continue
        }
        if step.hasPrefix("snap:") {
            _ = machine_screenshot(String(step.dropFirst(5)))
            continue
        }
        if let colon = step.firstIndex(of: ":"), ["keydown", "keyup"].contains(String(step[..<colon])) {
            let name = String(step[step.index(after: colon)...])
            if let code = scriptKeyCodes[name] {
                machine_post_key(scriptKeyChar(name), code, 0, step.hasPrefix("keydown") ? 1 : 0, 0)
            }
            continue
        }
        if let colon = step.firstIndex(of: ":"), let button = ["click": 2, "down": 1, "up": 0, "move": -1][step[..<colon]] {
            let xy = step[step.index(after: colon)...].split(separator: "/").compactMap { Int32($0) }
            guard xy.count == 2 else { continue }
            machine_post_mouse(xy[0], xy[1], button == 2 ? 1 : Int32(button))
            if button == 2 {
                usleep(100_000)
                machine_post_mouse(xy[0], xy[1], 0)
            }
            continue
        }
        var mods: UInt32 = 0
        var chars: [UInt8] = []
        if step.hasPrefix("cmd-") {
            mods = 0x100
            chars = Array(step.dropFirst(4).utf8)
        } else if step.hasPrefix("text:") {
            chars = Array(step.dropFirst(5).utf8)
        } else if step == "return" {
            chars = [13]
        }
        for ch in chars {
            machine_post_key(Int32(ch), 0, mods, 1, 0)
            machine_post_key(Int32(ch), 0, mods, 0, 0)
            usleep(20_000)
        }
    }
}

/// Run with rootless windows and the native menu bar until the app quits.
@MainActor func runRootless(title: String, icon: NSImage?, script: [String] = []) -> Never {
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
    ClassicSpeech.shared.install()
    NotificationRelay.shared.install()
    let host = RootlessHost(title: title)
    host.script = script
    app.delegate = host
    app.run()
    exit(0)
}
#endif
