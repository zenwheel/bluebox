// macOS host: the classic screen in a window (screen-in-a-window mode).
//
// The machine runs on a background thread; the window redraws the 1-bit
// framebuffer at 60 Hz and forwards mouse and keyboard input.  macOS virtual
// key codes are the classic ADB key codes, so they pass straight through.

#if canImport(AppKit)
import AppKit
import Toolbox

final class ScreenView: NSView {
    private var timer: Timer?
    private var rgba = [UInt32]()
    private var last = [UInt8]()

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.refreshIfChanged()
                if machine_halted() { NSApp.terminate(nil) }
            }
        }
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private var lastChecksum: Int = -1

    /// Ask for a redraw only when the framebuffer changed (cheap hash check).
    func refreshIfChanged() {
        var w: Int32 = 0, h: Int32 = 0, rb: Int32 = 0
        guard let fb = machine_framebuffer(&w, &h, &rb) else { return }
        var hasher = Hasher()
        hasher.combine(bytes: UnsafeRawBufferPointer(start: fb, count: Int(rb) * Int(h)))
        let sum = hasher.finalize()
        if sum != lastChecksum {
            lastChecksum = sum
            needsDisplay = true
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        var w: Int32 = 0, h: Int32 = 0, rowBytes: Int32 = 0
        guard let fb = machine_framebuffer(&w, &h, &rowBytes) else { return }
        let width = Int(w), height = Int(h), rb = Int(rowBytes)
        if rgba.count != width * height { rgba = [UInt32](repeating: 0, count: width * height) }
        // Convert only when the framebuffer changed since the last frame.
        let bytes = UnsafeBufferPointer(start: fb, count: rb * height)
        if last.count != bytes.count || !last.elementsEqual(bytes) {
            last = Array(bytes)
            rgba.withUnsafeMutableBufferPointer { out in
                for y in 0..<height {
                    let row = y * rb
                    for xb in 0..<(width + 7) / 8 {
                        let b = last[row + xb]
                        for bit in 0..<8 {
                            let x = xb * 8 + bit
                            if x >= width { break }
                            out[y * width + x] = (b >> (7 - UInt8(bit))) & 1 != 0 ? 0xFF00_0000 : 0xFFFF_FFFF
                        }
                    }
                }
            }
        }
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
        var w: Int32 = 0, h: Int32 = 0, rb: Int32 = 0
        _ = machine_framebuffer(&w, &h, &rb)
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
        let chars = event.modifierFlags.contains(.command) ? event.charactersIgnoringModifiers : event.characters
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

    override func flagsChanged(with event: NSEvent) {
        machine_post_key(-1, Int32(event.keyCode), Self.modifiers(event.modifierFlags), 1, 0)
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

    func applicationDidFinishLaunching(_ notification: Notification) {
        // After launch: AppKit sets the icon from the main bundle while launching.
        if let appIcon { NSApp.applicationIconImage = appIcon }
        launchEvents.finishLaunching()
        var w: Int32 = 0, h: Int32 = 0, rb: Int32 = 0
        _ = machine_framebuffer(&w, &h, &rb)
        let size = NSSize(width: CGFloat(w), height: CGFloat(h))
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

    func windowWillClose(_ notification: Notification) { NSApp.terminate(nil) }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// Show the classic screen in a window until the app quits.  Never returns.
@MainActor func runHostWindow(title: String, icon: NSImage?) -> Never {
    machine_attach_host(true)
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    appIcon = icon
    installAlertHandler()
    let delegate = HostDelegate(title: title)
    app.delegate = delegate
    // Minimal app menu so Quit works.
    let menu = NSMenu()
    let appItem = NSMenuItem()
    menu.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(NSMenuItem(title: "Quit Classic", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    appItem.submenu = appMenu
    app.mainMenu = menu
    app.run()
    exit(0)
}
#endif

#if canImport(AppKit)
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
        DispatchQueue.main.sync {
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
                if ProcessInfo.processInfo.environment["CLASSIC_DEBUG_HOST"] != nil {
                    eprint("alert \"\(message)\" -> response \(response), item \(chosen)\n")
                }
            }
        }
        return chosen
    }, nil)
}
#endif
