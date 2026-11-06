// macOS host, rootless mode: drawing the system's own code does over the
// desktop, and running a program in the background.
//
// An INIT's or desk accessory's resident code that draws straight on the
// Mac screen outside the windows (the Talking Moose, which puts itself in
// the screen's top left corner while it speaks) is shown floating over the
// host's desktop, in a panel that never takes the focus, for as long as
// it is there (machine_overlay).  Clicks on it go to the program.
//
// In the background (bluebox run --background, and bundles made with
// bluebox install --background) the program has no Dock icon and no menu
// bar: a status item's menu has its menus' items, its desk accessories,
// Open at Login, Suspend (Resume) and Quit.  The user's typing and clicking in other host
// applications counts as activity, so code waiting for the Mac to be left
// alone waits on.

#if canImport(AppKit)
import AppKit
import ServiceManagement
import Toolbox

/// The pixels the system's code drew, clear where it drew nothing.
final class OverlayView: NSView {
    private var rgba = [UInt32]()
    private var lastHash = 0

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var wantsUpdateLayer: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.magnificationFilter = .nearest
        layer?.contentsGravity = .resize
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    func show(left: Int, top: Int, width w: Int, height h: Int) {
        if rgba.count != w * h { rgba = [UInt32](repeating: 0, count: w * h) }
        rgba.withUnsafeMutableBytes { raw in
            machine_overlay_rgba(Int32(left), Int32(top), Int32(w), Int32(h), raw.baseAddress!.assumingMemoryBound(to: UInt8.self))
        }
        // Premultiplied: a clear pixel is all zeros.
        for k in 0..<rgba.count where rgba[k] & 0xFF00_0000 == 0 { rgba[k] = 0 }
        var hasher = Hasher()
        hasher.combine(w)
        rgba.withUnsafeBytes { hasher.combine(bytes: $0) }
        let hash = hasher.finalize()
        guard hash != lastHash else { return }
        lastHash = hash
        layer?.contents = rgbaImage(rgba, width: w, height: h)
    }

    override func mouseDown(with event: NSEvent) { postHostMouse(button: 1) }
    override func mouseUp(with event: NSEvent) { postHostMouse(button: 0) }
    override func mouseDragged(with event: NSEvent) { postHostMouse(button: -1) }
}

extension RootlessHost {
    // MARK: Drawing over the desktop

    func syncOverlay() {
        var r = [Int16](repeating: 0, count: 4)
        var on = false
        _ = r.withUnsafeMutableBufferPointer { machine_overlay($0.baseAddress!, &on) }
        let left = Int(r[1]), top = Int(r[0]), w = Int(r[3]) - Int(r[1]), h = Int(r[2]) - Int(r[0])
        guard on, screenWindow == nil, w > 0, h > 0 else {
            overlayWindow?.orderOut(nil)
            overlayWindow = nil
            return
        }
        let origin = macToHost(CGFloat(left), CGFloat(r[2]))
        let frame = NSRect(x: origin.x, y: origin.y, width: CGFloat(w) * displayScale, height: CGFloat(h) * displayScale)
        let panel: NSPanel
        if let p = overlayWindow {
            panel = p
            if panel.frame != frame { panel.setFrame(frame, display: false) }
        } else {
            panel = NSPanel(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
            panel.isOpaque = false
            panel.backgroundColor = .clear
            panel.hasShadow = false
            panel.level = .floating
            panel.hidesOnDeactivate = false
            panel.isReleasedWhenClosed = false
            panel.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle, .fullScreenAuxiliary]
            panel.contentView = OverlayView(frame: NSRect(origin: .zero, size: frame.size))
            overlayWindow = panel
        }
        panel.contentView?.frame = NSRect(origin: .zero, size: frame.size)
        (panel.contentView as? OverlayView)?.show(left: left, top: top, width: w, height: h)
        if !panel.isVisible { panel.orderFrontRegardless() }
    }

    // MARK: The status item

    func installStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        let image = NSImage(systemSymbolName: "text.bubble", accessibilityDescription: title)
        image?.isTemplate = true
        item.button?.image = image
        item.button?.toolTip = title
        let menu = NSMenu(title: title)
        menu.autoenablesItems = false
        menu.delegate = self
        item.menu = menu
        statusItem = item
        var menus = [machine_menu](repeating: machine_menu(), count: 32)
        syncStatusMenu(menus, Int(machine_menus(&menus, 32)))
    }

    /// The program's own menus' items (not the Apple, File and Edit menus,
    /// which the host's items stand for), its desk accessories, then
    /// Open at Login, Suspend (Resume while suspended) and Quit.
    func syncStatusMenu(_ snapshot: [machine_menu], _ n: Int) {
        guard let menu = statusItem?.menu else { return }
        var menus = snapshot
        menu.removeAllItems()
        var deskAccessories: [NSMenuItem] = []
        for i in 0..<n {
            var m = menus[i]
            let title = withUnsafeBytes(of: &m.title) { Self.string($0.baseAddress!, 64) }
            if i == 0 && (title.isEmpty || title.unicodeScalars.first?.value == 0x14) {
                deskAccessories = hostItems(&m, keys: false)
                continue
            }
            if title == "File" || title == "Edit" { continue }
            if menu.numberOfItems > 0 { menu.addItem(.separator()) }
            for item in hostItems(&m, keys: false) { menu.addItem(item) }
        }
        if !deskAccessories.isEmpty {
            if menu.numberOfItems > 0 { menu.addItem(.separator()) }
            let item = NSMenuItem(title: "Desk Accessories", action: nil, keyEquivalent: "")
            let sub = NSMenu(title: "Desk Accessories")
            sub.autoenablesItems = false
            sub.delegate = self
            deskAccessories.forEach { sub.addItem($0) }
            item.submenu = sub
            menu.addItem(item)
        }
        if menu.numberOfItems > 0 { menu.addItem(.separator()) }
        if Self.canOpenAtLogin {
            let login = NSMenuItem(title: "Open at Login", action: #selector(toggleOpenAtLogin(_:)), keyEquivalent: "")
            login.target = self
            login.state = SMAppService.mainApp.status == .enabled ? .on : .off
            menu.addItem(login)
        }
        Suspender.shared.menuItems(title: title, quitKeys: false).forEach { menu.addItem($0) }
    }

    /// Only an application bundle can be a login item (a run from the
    /// command line has no bundle to open).
    static var canOpenAtLogin: Bool { Bundle.main.bundlePath.hasSuffix(".app") && Bundle.main.bundleIdentifier != nil }

    /// Open at Login: only when the user turns it on.
    @objc func toggleOpenAtLogin(_ sender: NSMenuItem) {
        let service = SMAppService.mainApp
        do {
            if service.status == .enabled {
                try service.unregister()
            } else {
                try service.register()
            }
        } catch {
            let alert = NSAlert()
            alert.messageText = "\(title) could not be set to open at login."
            alert.informativeText = error.localizedDescription
            NSApp.activate(ignoringOtherApps: true)
            alert.runModal()
        }
        sender.state = service.status == .enabled ? .on : .off
        if service.status == .requiresApproval { SMAppService.openSystemSettingsLoginItems() }
    }

    /// The user at work in other applications: input the program would not
    /// see counts as activity, at most once a second.  So does nobody being
    /// at the host (see watchAway): the Talking Moose keeps its remarks
    /// for when someone is there to hear them.
    func noteActivityElsewhere() {
        guard !NSApp.isActive, Date().timeIntervalSince(lastActivityNote) >= 1 else { return }
        let anyInput = CGEventType(rawValue: ~0)!
        guard !awayReasons.isEmpty ||
            CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: anyInput) < 1.5 else { return }
        lastActivityNote = Date()
        machine_note_user_activity()
    }

    /// The host's displays sleeping and waking, its screen locked and
    /// unlocked, its screen saver starting and stopping, and the host
    /// sleeping and waking.  On the way back, activity at once: the wait
    /// for the Mac to be left alone starts over.
    func watchAway() {
        let workspace = NSWorkspace.shared.notificationCenter
        let distributed = DistributedNotificationCenter.default()
        let pairs: [(NotificationCenter, String, String, String)] = [
            (workspace, NSWorkspace.screensDidSleepNotification.rawValue, NSWorkspace.screensDidWakeNotification.rawValue, "displays"),
            (workspace, NSWorkspace.willSleepNotification.rawValue, NSWorkspace.didWakeNotification.rawValue, "host"),
            (distributed, "com.apple.screenIsLocked", "com.apple.screenIsUnlocked", "locked"),
            (distributed, "com.apple.screensaver.didstart", "com.apple.screensaver.didstop", "screen saver"),
        ]
        for (center, gone, back, reason) in pairs {
            center.addObserver(forName: Notification.Name(gone), object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { _ = self?.awayReasons.insert(reason) }
            }
            center.addObserver(forName: Notification.Name(back), object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.awayReasons.remove(reason)
                    self.lastActivityNote = Date()
                    machine_note_user_activity()
                }
            }
        }
    }
}
#endif
