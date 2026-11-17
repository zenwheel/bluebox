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

/// An image of RGBA pixels that owns a copy of them.  A bitmap context's
/// makeImage shares the buffer until the context draws again, and ours
/// never does: the next refresh wrote over pixels a layer was still showing,
/// so a moving sprite flashed back to where it had been (Bachman).
func rgbaImage(_ rgba: [UInt32], width: Int, height: Int) -> CGImage? {
    let data = rgba.withUnsafeBytes { Data($0) } as CFData
    guard let provider = CGDataProvider(data: data) else { return nil }
    return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                   space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                   provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
}

final class ScreenView: NSView, MacScreenView {
    private var timer: Timer?
    private var rgba = [UInt32]()

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func resetCursorRects() { addCursorRect(bounds, cursor: CursorSync.shared.cursor) }

    // A suspended program takes no drops.
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        Suspender.shared.suspended ? [] : FileDrops.send(Int32(MACHINE_DRAG_ENTER), sender, at: mac(sender))
    }
    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        Suspender.shared.suspended ? [] : FileDrops.send(Int32(MACHINE_DRAG_MOVE), sender, at: mac(sender))
    }
    override func draggingExited(_ sender: NSDraggingInfo?) {
        if let sender, !Suspender.shared.suspended { _ = FileDrops.send(Int32(MACHINE_DRAG_LEAVE), sender, at: mac(sender)) }
    }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        !Suspender.shared.suspended && FileDrops.send(Int32(MACHINE_DRAG_DROP), sender, at: mac(sender)) != []
    }

    /// A drag's point on the Mac screen.
    private func mac(_ info: NSDraggingInfo) -> (Int32, Int32) { mac(convert(info.draggingLocation, from: nil)) }

    private func mac(_ p: NSPoint) -> (Int32, Int32) {
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        if hostFullScreen { // the whole screen stretched over the display
            return (Int32(p.x * CGFloat(w) / bounds.width), Int32(p.y * CGFloat(h) / bounds.height))
        }
        let s = displayScale
        return (Int32((p.x / s + pan.x).rounded(.down)), Int32((p.y / s + pan.y).rounded(.down)))
    }

    /// The Mac pixel at the view's top left: the screen at the display
    /// scale is larger than the window can be (a program whose screen
    /// can't change size, on a smaller display), and the view shows the
    /// part of it around the mouse.
    var pan = CGPoint.zero

    /// The pan kept around the program's mouse: moved when the mouse comes
    /// within a margin of the view's edge, and kept on the screen.
    static var startAtFrontWindow = false

    func followMouse() {
        guard !hostFullScreen else { return }
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        let s = displayScale
        let vw = bounds.width / s, vh = bounds.height / s
        if Self.startAtFrontWindow {
            Self.startAtFrontWindow = false
            var list = [machine_window](repeating: machine_window(), count: 1)
            if machine_windows(&list, 1) > 0 {
                let f = list[0]
                let cx = CGFloat(Int(f.left) + Int(f.right)) / 2, cy = CGFloat(Int(f.top) + Int(f.bottom)) / 2
                pan = CGPoint(x: min(max(cx - vw / 2, 0), max(CGFloat(w) - vw, 0)).rounded(.down),
                              y: min(max(cy - vh / 2, 0), max(CGFloat(h) - vh, 0)).rounded(.down))
                needsDisplay = true
                return
            }
        }
        var x: Int32 = 0, y: Int32 = 0, relative = false
        _ = machine_mouse_state(&x, &y, &relative)
        func axis(_ at: CGFloat, _ pan: CGFloat, _ view: CGFloat, _ size: CGFloat) -> CGFloat {
            guard size > view else { return 0 }
            let margin = min(64, view / 4)
            var p = pan
            if at < p + margin { p = at - margin }
            if at > p + view - margin { p = at - view + margin }
            return min(max(p, 0), size - view).rounded(.down)
        }
        let next = CGPoint(x: axis(CGFloat(x), pan.x, vw, CGFloat(w)), y: axis(CGFloat(y), pan.y, vh, CGFloat(h)))
        guard next != pan else { return }
        pan = next
        needsDisplay = true
        // The pointer stays where it is over moved pixels: the Mac's mouse
        // is now under it somewhere else (at the edge, it goes on scrolling).
        if !captured, let window, !Suspender.shared.suspended {
            let p = convert(window.mouseLocationOutsideOfEventStream, from: nil)
            if bounds.contains(p) {
                let (mx, my) = mac(p)
                machine_post_mouse(mx, my, -1)
            }
        }
    }

    func macPoint(screen: NSPoint) -> (Int32, Int32) {
        mac(convert(window?.convertPoint(fromScreen: screen) ?? screen, from: nil))
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes(FileDrops.types)
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                if Suspender.shared.suspended { // the machine still: nothing to show, the mouse let go
                    self?.setCaptured(false)
                    return
                }
                // Unseen, nothing to show: the machine is throttled too.
                if noteHostVisibility() < 2 { self?.refreshIfChanged() }
                self?.updateCapture()
                self?.followMouse()
                CursorSync.shared.tick()
                DragOut.shared.tick()
                if machine_halted() {
                    self?.timer?.invalidate() /* one report, not one each tick */
                    relaunchAtWantedDepth()
                    reportUnexpectedStop()
                    NSApp.terminate(nil)
                }
            }
        }
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private var shownCopy = [UInt8]() // the screen as last shown

    /// Ask for a redraw only when the framebuffer changed (compared with a
    /// copy of it).
    /// Told when the Mac screen changes size (the window follows it).
    var sizeChanged: (() -> Void)?
    private var shownSize: (Int32, Int32) = (0, 0)

    func refreshIfChanged() {
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        if (w, h) != shownSize {
            let first = shownSize == (0, 0)
            shownSize = (w, h)
            if !first { sizeChanged?() }
        }
        // Converted only when it changed, or the desktop's showing through
        // changes (the mask then follows).
        let clear = !NSApp.isActive && !hostFullScreen && !machine_is_finder()
        let size = machine_screen_shadow_size(0, 0, w, h)
        if shownCopy.count != size { shownCopy = [UInt8](repeating: 0, count: size) }
        let changed = shownCopy.withUnsafeMutableBufferPointer { machine_screen_changed(0, 0, w, h, $0.baseAddress!, size) }
        if !changed && clear == clearing { return }
        if rgba.count != Int(w) * Int(h) { rgba = [UInt32](repeating: 0, count: Int(w) * Int(h)) }
        rgba.withUnsafeMutableBytes { raw in
            machine_screen_rgba(0, 0, w, h, raw.baseAddress!.assumingMemoryBound(to: UInt8.self))
        }
        // In the background, a program's desktop is left out (Maelstrom,
        // sent there, hides its play area and leaves only a small window):
        // the host's desktop shows through the window instead.  Not the
        // Finder's: the desktop is what it shows.
        if clear {
            if deskMask.count != rgba.count { deskMask = [UInt8](repeating: 1, count: rgba.count) }
            if machine_desktop_mask(0, 0, w, h, &deskMask) {
                for k in 0..<rgba.count where deskMask[k] == 0 { rgba[k] = 0 }
            }
        }
        if clear != clearing {
            clearing = clear
            window?.isOpaque = !clear
            window?.backgroundColor = clear ? .clear : .windowBackgroundColor
        }
        needsDisplay = true
    }

    private var deskMask = [UInt8]()
    private var clearing = false

    override func draw(_ dirtyRect: NSRect) {
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        let width = Int(w), height = Int(h)
        guard rgba.count == width * height else { return }
        guard let image = rgbaImage(rgba, width: width, height: height),
              let gc = NSGraphicsContext.current?.cgContext else { return }
        gc.interpolationQuality = .none
        gc.saveGState()
        gc.translateBy(x: 0, y: bounds.height)
        gc.scaleBy(x: 1, y: -1)
        if hostFullScreen {
            gc.draw(image, in: CGRect(x: 0, y: 0, width: bounds.width, height: bounds.height))
        } else { // at the display scale, from the pan
            let s = displayScale
            gc.draw(image, in: CGRect(x: -pan.x * s, y: bounds.height + pan.y * s - CGFloat(height) * s,
                                      width: CGFloat(width) * s, height: CGFloat(height) * s))
        }
        gc.restoreGState()
    }

    // MARK: Mouse
    //
    // As in rootless windows: a program reading the mouse's motion (moving
    // the cursor itself, or through InputSprocket) gets the mouse captured
    // while we are the active window, its motion given in Mac pixels, so
    // it never stops at the display's edge; the pointer catches up after.

    private var captured = false
    private var motionMonitor: Any?
    private var programWarps: UInt32 = 0

    /// Host points per Mac pixel across the view.
    private var pointsPerPixel: CGFloat {
        guard hostFullScreen else { return displayScale }
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        return w > 0 ? bounds.width / CGFloat(w) : 1
    }

    func updateCapture() {
        var x: Int32 = 0, y: Int32 = 0, relative = false
        let warps = machine_mouse_state(&x, &y, &relative)
        let active = NSApp.isActive && window?.isKeyWindow == true
        let wasCaptured = captured
        setCaptured(active && relative)
        if warps != programWarps {
            programWarps = warps
            if active && !captured { warpHostPointer(to: x, y) }
        } else if wasCaptured && !captured {
            warpHostPointer(to: x, y)
        }
    }

    private func setCaptured(_ on: Bool) {
        guard on != captured else { return }
        captured = on
        CGAssociateMouseAndMouseCursorPosition(on ? 0 : 1)
        if on && motionMonitor == nil {
            motionMonitor = NSEvent.addLocalMonitorForEvents(
                matching: [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged]
            ) { [weak self] event in
                MainActor.assumeIsolated {
                    if let self, self.captured {
                        let scale = self.pointsPerPixel
                        machine_post_mouse_delta(Double(event.deltaX / scale), Double(event.deltaY / scale))
                    }
                }
                return event
            }
        }
    }

    /// Put the host's pointer where the Mac's mouse is.
    private func warpHostPointer(to x: Int32, _ y: Int32) {
        guard let window else { return }
        let scale = pointsPerPixel
        let origin = hostFullScreen ? .zero : pan
        let inView = NSPoint(x: (CGFloat(x) - origin.x + 0.5) * scale, y: (CGFloat(y) - origin.y + 0.5) * scale)
        let onScreen = window.convertPoint(toScreen: convert(inView, to: nil))
        let top = NSScreen.screens.first?.frame.maxY ?? 0
        CGWarpMouseCursorPosition(CGPoint(x: onScreen.x, y: top - onScreen.y))
        CGAssociateMouseAndMouseCursorPosition(captured ? 0 : 1)
    }

    private func post(_ event: NSEvent, button: Int32) {
        guard !Suspender.shared.suspended else { return }
        if captured { /* the pointer is still: the button only, where the Mac's mouse is */
            guard button >= 0 else { return }
            var x: Int32 = 0, y: Int32 = 0, relative = false
            _ = machine_mouse_state(&x, &y, &relative)
            machine_post_mouse(x, y, button)
            return
        }
        let (x, y) = mac(convert(event.locationInWindow, from: nil))
        machine_post_mouse(x, y, button)
    }

    override func mouseDown(with event: NSEvent) {
        post(event, button: 1)
        DragOut.shared.note(event, in: self)
    }
    override func mouseUp(with event: NSEvent) { post(event, button: 0) }
    override func scrollWheel(with event: NSEvent) { postHostWheel(event, in: self) }
    override func mouseDragged(with event: NSEvent) {
        post(event, button: -1)
        DragOut.shared.note(event, in: self)
    }
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
        let key = HostKey.classify(event.modifierFlags, event.charactersIgnoringModifiers,
                                   suspended: Suspender.shared.suspended)
        if Suspender.shared.handle(key) { return }
        machine_post_key(Self.charCode(event), Int32(event.keyCode), Self.modifiers(event.modifierFlags), 1,
                         event.isARepeat ? 1 : 0)
    }

    override func keyUp(with event: NSEvent) {
        guard !Suspender.shared.suspended else { return }
        machine_post_key(Self.charCode(event), Int32(event.keyCode), Self.modifiers(event.modifierFlags), 0, 0)
    }

    override func flagsChanged(with event: NSEvent) {
        if !Suspender.shared.suspended { Self.postModifier(event) }
    }

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
        // Command keys belong to the classic app, not the host menu:
        // Command-Q too, the keystroke a Mac delivered, which a program's
        // MenuKey turns into its Quit item and a Director projector (Total
        // Distortion) quits on itself; the Quit Apple Event the host menu's
        // item sends went unread.  Command-Option-Q is the host's Force
        // Quit, for a program that ignores Command-Q; keyDown keeps it.
        if event.modifierFlags.contains(.command) {
            keyDown(with: event)
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}

/// An Open Documents event for these files; false if none is a file.
@discardableResult
func sendOpenDocuments(_ urls: [URL]) -> Bool {
    let paths = urls.filter(\.isFileURL).map(\.path)
    guard !paths.isEmpty else { return false }
    var cpaths = paths.map { strdup($0) }
    cpaths.withUnsafeMutableBufferPointer { buf in
        buf.baseAddress!.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: buf.count) {
            machine_open_documents($0, Int32(buf.count), false)
        }
    }
    cpaths.forEach { free($0) }
    return true
}

/// Files dropped on a classic window open in the application, as dropping
/// them on its icon in the Finder did (an Open Documents event): a playlist
/// takes songs this way, a viewer opens pictures.
/// Text and files dragged in go to the program's Drag Manager handlers for
/// the window under the pointer (an 'hfs ' flavor per file, 'TEXT' for
/// text); files a window doesn't take become an Open Documents event.
/// A drag from the Blue Box Browser brings its items' paths (its own
/// type, beside the promise it gives the Finder): they are the files
/// themselves, opened where they are, as a drop from the Finder's window
/// on a program's did.
@MainActor enum FileDrops {
    static let browserPaths = NSPasteboard.PasteboardType(BrowserPaths.pasteboardType)
    static let types: [NSPasteboard.PasteboardType] = [.fileURL, .string, DragOut.marker, browserPaths]

    static func urls(_ info: NSDraggingInfo) -> [URL] {
        let pb = info.draggingPasteboard
        let urls = (pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]) ?? []
        if !urls.isEmpty { return urls }
        let strings = (pb.pasteboardItems ?? []).compactMap { $0.string(forType: browserPaths) }
        return BrowserPaths.parse(strings).map { URL(fileURLWithPath: $0) }
    }

    static func text(_ info: NSDraggingInfo) -> String? {
        urls(info).isEmpty ? info.draggingPasteboard.string(forType: .string) : nil
    }

    /// The drag at Mac screen coordinates `at`.
    static func send(_ kind: Int32, _ info: NSDraggingInfo, at: (Int32, Int32)) -> NSDragOperation {
        if DragOut.isOurs(info) { return DragOut.shared.over(kind, at: at) }
        let paths = urls(info).map(\.path), text = self.text(info)
        guard !paths.isEmpty || text != nil else { return [] }
        if kind == Int32(MACHINE_DRAG_ENTER) {
            var cpaths = paths.map { strdup($0) }
            cpaths.withUnsafeMutableBufferPointer { buf in
                buf.baseAddress.map { base in
                    base.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: buf.count) { files in
                        machine_host_drag(kind, at.0, at.1, files, Int32(buf.count), text)
                    }
                } ?? machine_host_drag(kind, at.0, at.1, nil, 0, text)
            }
            cpaths.forEach { free($0) }
        } else {
            machine_host_drag(kind, at.0, at.1, nil, 0, nil)
        }
        return .copy
    }
}

/// Documents opened from the Finder or Dock become Open Documents events.
/// The first event is Open Application only if nothing was opened at launch,
/// as with the System 7 Finder.
struct LaunchEvents {
    private var launched = false
    private var openedAtLaunch = false

    mutating func open(_ urls: [URL]) {
        guard sendOpenDocuments(urls) else { return }
        if !launched { openedAtLaunch = true }
    }

    @MainActor mutating func finishLaunching() {
        if !launched && !openedAtLaunch && !launchDocumentsSent { machine_send_open_application() }
        launched = true
        HostAppleEvents.shared.launched = true
    }
}

/// Apple Events from macOS beyond those AppKit turns into delegate calls:
/// Quit Application asks the classic program as its own Quit would (so it
/// still asks about unsaved changes), Print Documents prints with it, and
/// Do Script ('misc'/'dosc', from osascript or the bundle's dictionary)
/// goes to it with its text, the program's reply coming back as the result.
@MainActor final class HostAppleEvents: NSObject {
    static let shared = HostAppleEvents()
    /// Set once the application has finished launching (LaunchEvents).
    var launched = false

    func install() {
        let m = NSAppleEventManager.shared()
        m.setEventHandler(self, andSelector: #selector(quit(_:reply:)), forEventClass: fourCCValue("aevt"),
                          andEventID: fourCCValue("quit"))
        m.setEventHandler(self, andSelector: #selector(printDocuments(_:reply:)), forEventClass: fourCCValue("aevt"),
                          andEventID: fourCCValue("pdoc"))
        m.setEventHandler(self, andSelector: #selector(doScript(_:reply:)), forEventClass: fourCCValue("misc"),
                          andEventID: fourCCValue("dosc"))
    }

    @objc func quit(_ event: NSAppleEventDescriptor, reply: NSAppleEventDescriptor) {
        Suspender.shared.quit() // a suspended program is resumed first, to quit
    }

    /// Launched to print, the program gets Print Documents and then Quit,
    /// as from the System 7 Finder.
    @objc func printDocuments(_ event: NSAppleEventDescriptor, reply: NSAppleEventDescriptor) {
        guard let list = event.paramDescriptor(forKeyword: keyDirectObject) else { return }
        let items = list.numberOfItems > 0 ? (1...list.numberOfItems).compactMap { list.atIndex($0) } : [list]
        let paths = items.compactMap { $0.fileURLValue?.path }
        guard !paths.isEmpty else { return }
        withCStringArray(paths) { machine_open_documents($0, Int32(paths.count), true) }
        if !launched {
            launchDocumentsSent = true
            machine_send_quit()
        }
    }

    @objc func doScript(_ event: NSAppleEventDescriptor, reply: NSAppleEventDescriptor) {
        let text = event.paramDescriptor(forKeyword: keyDirectObject)?.stringValue ?? ""
        let manager = NSAppleEventManager.shared()
        guard let suspension = manager.suspendCurrentAppleEvent() else { return }
        let ctx = Unmanaged.passRetained(Suspended(suspension)).toOpaque()
        machine_send_apple_event(fourCCValue("misc"), fourCCValue("dosc"), nil, 0, text, { ctx, data, len, err in
            var result = [CChar](repeating: 0, count: 65536)
            var errn: Int32 = 0
            let has = data.map { machine_ae_reply_result($0, len, &result, result.count, &errn) } ?? false
            let text = str(result)
            let failure = err != 0 ? Int32(err) : errn
            let suspended = Unmanaged<Suspended>.fromOpaque(ctx!).takeRetainedValue()
            DispatchQueue.main.async {
                let manager = NSAppleEventManager.shared()
                let reply = manager.replyAppleEvent(forSuspensionID: suspended.id)
                if failure != 0 {
                    reply.setParam(NSAppleEventDescriptor(int32: failure), forKeyword: keyErrorNumber)
                    if !text.isEmpty { reply.setParam(NSAppleEventDescriptor(string: text), forKeyword: keyErrorString) }
                } else if has {
                    reply.setParam(NSAppleEventDescriptor(string: text), forKeyword: keyDirectObject)
                }
                manager.resume(withSuspensionID: suspended.id)
            }
        }, ctx)
    }

    private final class Suspended {
        let id: NSAppleEventManager.SuspensionID
        init(_ id: NSAppleEventManager.SuspensionID) { self.id = id }
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
            var key = [Int(state.kind), Int(state.hot_h), Int(state.hot_v)]
                + withUnsafeBytes(of: state.data) { $0.map(Int.init) } + withUnsafeBytes(of: state.mask) { $0.map(Int.init) }
            if state.color { key += withUnsafeBytes(of: state.rgba) { $0.map(Int.init) } }
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
    /// (data outside the mask, which inverts, shown black), or a colour
    /// cursor's colours.
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
        let rgba = state.color ? withUnsafeBytes(of: state.rgba) { Array($0) } : nil
        for py in 0..<n {
            for pxl in 0..<n {
                let x = pxl * 16 / n, y = py * 16 / n
                if let rgba {
                    let o = (py * n + pxl) * 4, s = (y * 16 + x) * 4
                    for k in 0..<4 { px[o + k] = rgba[s + k] }
                    continue
                }
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
/// colour, so selected text and rows look as the host's do, and the
/// Appearance Manager's accent (scroll bar thumbs, focus rings, selected
/// menu items) follows the host's accent colour; both change when the
/// host's do.
@MainActor func useHostSelectionColor() {
    func word(_ v: CGFloat) -> UInt16 { UInt16((max(0, min(1, v)) * 65535).rounded()) }
    if let c = NSColor.selectedTextBackgroundColor.usingColorSpace(.sRGB) {
        machine_set_hilite_color(word(c.redComponent), word(c.greenComponent), word(c.blueComponent))
    }
    if let a = NSColor.controlAccentColor.usingColorSpace(.sRGB) {
        machine_set_accent_color(word(a.redComponent), word(a.greenComponent), word(a.blueComponent))
    }
    if !hostColorsObserved {
        hostColorsObserved = true
        NotificationCenter.default.addObserver(forName: NSColor.systemColorsDidChangeNotification, object: nil,
                                               queue: .main) { _ in
            MainActor.assumeIsolated { useHostSelectionColor() }
        }
    }
}

nonisolated(unsafe) private var hostColorsObserved = false

/// The classic application's Finder icon, for the Dock and alerts.
nonisolated(unsafe) var appIcon: NSImage?

/// Set when the command line already sent the launch event.
nonisolated(unsafe) var launchDocumentsSent = false

final class HostDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate {
    let title: String
    var window: NSWindow!
    var launchEvents = LaunchEvents()

    init(title: String) { self.title = title }

    func applicationWillFinishLaunching(_ notification: Notification) { HostAppleEvents.shared.install() }
    func application(_ application: NSApplication, open urls: [URL]) { launchEvents.open(urls) }
    func applicationDidBecomeActive(_ notification: Notification) { ClipboardSync.shared.didBecomeActive() }
    func applicationWillResignActive(_ notification: Notification) { ClipboardSync.shared.willResignActive() }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // After launch: AppKit sets the icon from the main bundle while launching.
        if let appIcon { NSApp.applicationIconImage = appIcon }
        launchEvents.finishLaunching()
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        var size = NSSize(width: CGFloat(w) * displayScale, height: CGFloat(h) * displayScale)
        if hostFullScreen, let screen = NSScreen.main {
            // A screen saver over the whole display: borderless, above the
            // menu bar and the Dock, the pointer hidden.
            size = screen.frame.size
            window = NSWindow(contentRect: screen.frame, styleMask: [.borderless], backing: .buffered, defer: false)
            window.level = .screenSaver
            window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            NSCursor.hide()
        } else {
            // On its display at the scale: a screen larger than that (one
            // that can't change size) is shown in part, the view panning.
            size = fitted(size, on: NSScreen.main)
            window = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
            window.contentMinSize = NSSize(width: 512 * displayScale, height: 342 * displayScale)
        }
        window.title = title
        window.delegate = self
        let view = ScreenView(frame: NSRect(origin: .zero, size: size))
        window.contentView = view
        window.acceptsMouseMovedEvents = true
        if !hostFullScreen { window.center() }
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(view)
        activateFromShell()
        PickerStandIn.closeWhenAnotherShows()
        DragOut.shared.refresh = { [weak view] in view?.refreshIfChanged() }
        if !hostFullScreen {
            view.sizeChanged = { [weak self] in self?.followScreenSize() }
            NotificationCenter.default.addObserver(forName: NSApplication.didChangeScreenParametersNotification,
                                                   object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { (NSApp.delegate as? HostDelegate)?.displayChanged() }
            }
        }
        let title = self.title
        Suspender.shared.changed = { [weak view] on in
            view?.showSuspended(on)
            Suspender.shared.retitle(NSApp.mainMenu, title: title)
        }
        Suspender.shared.listen()

        // Run the machine off the main thread.
        Thread.detachNewThread {
            while !machine_halted() {
                _ = machine_run(1_000_000)
            }
        }
    }

    // MARK: The screen's size
    //
    // The window and the Mac screen follow each other, as a virtual
    // machine's display follows its window: the window resized (on the
    // mouse-up that ends a live resize) or its display changed asks for a
    // Mac screen of its size at the display scale (a Mac's size: rows of
    // 64 pixels; machine_request_screen_size), which the program gets at
    // its next event call, and the window then takes the screen's exact
    // size.  A program whose screen can't change keeps it, and the view
    // pans over it (ScreenView.followMouse).

    /// The largest content the window can have on that display: its
    /// visible area less a title bar.
    @MainActor func contentLimit(on screen: NSScreen?) -> NSSize {
        let vf = (screen ?? NSScreen.main ?? NSScreen.screens.first)?.visibleFrame
            ?? NSRect(x: 0, y: 0, width: 1024, height: 768)
        let bar = window.map { $0.frame.height - $0.contentRect(forFrameRect: $0.frame).height } ?? 28
        return NSSize(width: vf.width, height: vf.height - bar)
    }

    @MainActor func fitted(_ size: NSSize, on screen: NSScreen?) -> NSSize {
        let limit = contentLimit(on: screen)
        return NSSize(width: min(size.width, limit.width), height: min(size.height, limit.height))
    }

    /// The Mac screen the window's content asks for.
    @MainActor func requestScreenForWindow() {
        guard let window else { return }
        let content = window.contentRect(forFrameRect: window.frame).size
        var mw = Int32((content.width / displayScale).rounded(.down)), mh = Int32((content.height / displayScale).rounded(.down))
        machine_fit_screen(&mw, &mh)
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        guard (mw, mh) != (w, h), mw >= 512, mh >= 342 else { return followScreenSize() }
        if let why = machine_screen_fixed_at(mw, mh) {
            // The window stays as made, the view panning over the screen.
            eprint("bluebox: the screen stays \(w)×\(h): \(String(cString: why))\n")
            return
        }
        machine_request_screen_size(mw, mh)
    }

    /// The window the screen's size at the scale, within its display.
    @MainActor func followScreenSize() {
        guard let window, !window.inLiveResize else { return }
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        let size = fitted(NSSize(width: CGFloat(w) * displayScale, height: CGFloat(h) * displayScale), on: window.screen)
        window.contentMaxSize = machine_screen_fixed() == nil ? contentLimit(on: window.screen)
            : NSSize(width: CGFloat(w) * displayScale, height: CGFloat(h) * displayScale)
        let frame = window.contentRect(forFrameRect: window.frame)
        guard frame.size != size else { return }
        // Its top left stays where it is.
        let content = NSRect(x: frame.minX, y: frame.maxY - size.height, width: size.width, height: size.height)
        window.setFrame(window.frameRect(forContentRect: content), display: true)
        window.contentView?.needsDisplay = true
    }

    func windowDidEndLiveResize(_ notification: Notification) {
        MainActor.assumeIsolated { requestScreenForWindow() }
    }

    /// The display changed (another resolution, a monitor connected or
    /// gone): the window kept on it, and the screen following the window.
    @MainActor func displayChanged() {
        guard let window else { return }
        let frame = window.contentRect(forFrameRect: window.frame)
        let size = fitted(frame.size, on: window.screen)
        if size != frame.size {
            let content = NSRect(x: frame.minX, y: frame.maxY - size.height, width: size.width, height: size.height)
            window.setFrame(window.frameRect(forContentRect: content), display: true)
        }
        if let vf = window.screen?.visibleFrame, !vf.contains(window.frame) {
            window.setFrameTopLeftPoint(NSPoint(x: max(vf.minX, min(window.frame.minX, vf.maxX - window.frame.width)),
                                                y: min(vf.maxY, max(window.frame.maxY, vf.minY + window.frame.height))))
        }
        requestScreenForWindow()
    }

    /// A suspended program's window stays (closing it ends the program).
    func windowShouldClose(_ sender: NSWindow) -> Bool { MainActor.assumeIsolated { !Suspender.shared.suspended } }
    func windowWillClose(_ notification: Notification) { NSApp.terminate(nil) }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// The application menu opened: Hide's keys as the program's menus now are.
    func menuNeedsUpdate(_ menu: NSMenu) {
        MainActor.assumeIsolated {
            HostHideItems.update(menu)
            Suspender.shared.menuOpening(menu)
        }
    }
}

/// The Mac screen a window of the whole screen has on the main display at
/// the display scale: its visible area less a title bar, a Mac's size.
@MainActor func screenWindowMacSize() -> (Int32, Int32) {
    let vf = (NSScreen.main ?? NSScreen.screens.first)?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1024, height: 768)
    let style: NSWindow.StyleMask = [.titled, .closable, .miniaturizable, .resizable]
    let bar = NSWindow.frameRect(forContentRect: NSRect(x: 0, y: 0, width: 100, height: 100), styleMask: style).height - 100
    var w = Int32((vf.width / displayScale).rounded(.down)), h = Int32(((vf.height - bar) / displayScale).rounded(.down))
    machine_fit_screen(&w, &h)
    return (w, h)
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
    DragOut.shared.install()
    if soundRecord == nil { ClassicAudio.shared.install() }
    ClassicSoundInput.shared.install()
    if soundRecord == nil { ClassicMusic.shared.install() }
    ClassicMIDI.shared.install()
    GameControllers.shared.install()
    NativeControls.install()
    if !speechBackend.hasPrefix("record:") { ClassicSpeech.shared.install(avspeech: speechBackend == "avspeech") }
    NotificationRelay.shared.install()
    let delegate = HostDelegate(title: title)
    app.delegate = delegate
    Suspender.shared.installHibernateKey()
    // Minimal app menu: Hide, Hide Others and Show All (Command-H hides
    // unless the program has a Command-H item); Suspend; and Quit, which
    // chosen with the mouse (Command-Q itself goes to the app as a
    // keystroke) asks the app to quit; Option shows Suspend, Control-Option
    // Force Quit.
    let menu = NSMenu()
    let appItem = NSMenuItem()
    menu.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.autoenablesItems = false
    appMenu.delegate = delegate
    Suspender.shared.applicationMenuItems(title: title).forEach { appMenu.addItem($0) }
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
    installScriptDialogHandler()
}

/// MacRoman text into a C array of chars (LF becomes CR), cut to fit.
func setMacRoman<T>(_ tuple: inout T, _ s: String) {
    let bytes = Array((s.replacingOccurrences(of: "\n", with: "\r").data(using: .macOSRoman, allowLossyConversion: true) ?? Data()))
    withUnsafeMutableBytes(of: &tuple) { raw in
        let n = min(bytes.count, raw.count - 1)
        for i in 0..<n { raw[i] = bytes[i] }
        raw[n] = 0
    }
}

/// AppleScript's display dialog and choose from list, from a script Blue
/// Box's own AppleScript runs, as native alerts: the default answer in a
/// text field, the list in a pop-up menu, giving up after its time.  Only
/// the text shown and the answer pass between them.
func installScriptDialogHandler() {
    machine_set_script_dialog_handler({ dialogPtr, _ in
        guard let d = dialogPtr else { return 0 }
        let message = macRomanString(d.pointee.message)
        var buttons: [String] = []
        withUnsafeBytes(of: d.pointee.buttons) { raw in
            for i in 0..<Int(d.pointee.nbuttons) {
                let slice = raw[(i * 64)..<(i * 64 + 64)]
                let bytes = slice.prefix { $0 != 0 }
                buttons.append(String(data: Data(bytes), encoding: .macOSRoman) ?? "")
            }
        }
        var items: [String] = []
        if let list = d.pointee.items {
            for i in 0..<Int(d.pointee.nitems) {
                if let p = list[i] { items.append(String(cString: p, encoding: .macOSRoman) ?? "") }
            }
        }
        let isList = d.pointee.kind == Int32(MACHINE_SCRIPT_LIST)
        let defaultButton = Int(d.pointee.default_button), cancelButton = Int(d.pointee.cancel_button)
        let hasAnswer = d.pointee.has_answer, answer = macRomanString(d.pointee.answer)
        let givingUp = Int(d.pointee.giving_up), icon = d.pointee.icon
        let selected = Int(d.pointee.selected)
        var chosen: Int32 = 0
        var typed = answer
        var picked = selected
        onMain {
            MainActor.assumeIsolated {
                let alert = NSAlert()
                if let appIcon { alert.icon = appIcon }
                alert.messageText = message
                switch Int(icon) {
                case Int(MACHINE_ALERT_STOP): alert.alertStyle = .critical
                case Int(MACHINE_ALERT_CAUTION): alert.alertStyle = .warning
                default: alert.alertStyle = .informational
                }
                // NSAlert's first button is the default, its buttons run right
                // to left: the script's default first, then the others from
                // the right.
                var order: [Int] = []
                if defaultButton > 0 { order.append(defaultButton) }
                for i in stride(from: buttons.count, through: 1, by: -1) where i != defaultButton { order.append(i) }
                for (k, b) in order.enumerated() {
                    let button = alert.addButton(withTitle: buttons[b - 1])
                    if k == 0 && defaultButton == 0 { button.keyEquivalent = "" }
                    if b == cancelButton { button.keyEquivalent = "\u{1b}" }
                }
                var field: NSTextField?
                var popup: NSPopUpButton?
                if isList {
                    let p = NSPopUpButton(frame: NSRect(x: 0, y: 0, width: 280, height: 26), pullsDown: false)
                    p.addItems(withTitles: items)
                    if picked >= 0 && picked < items.count { p.selectItem(at: picked) }
                    alert.accessoryView = p
                    popup = p
                } else if hasAnswer {
                    let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
                    f.stringValue = answer
                    alert.accessoryView = f
                    alert.window.initialFirstResponder = f
                    field = f
                }
                var timer: Timer?
                if givingUp > 0 {
                    timer = Timer(timeInterval: TimeInterval(givingUp), repeats: false) { _ in
                        MainActor.assumeIsolated { NSApp.abortModal() }
                    }
                    RunLoop.main.add(timer!, forMode: .modalPanel)
                }
                let response = alert.runModal()
                timer?.invalidate()
                if response == .abort {
                    chosen = 0
                } else {
                    let k = response.rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
                    chosen = k >= 0 && k < order.count ? Int32(order[k]) : Int32(defaultButton)
                }
                if let field { typed = field.stringValue }
                if let popup { picked = popup.indexOfSelectedItem }
            }
        }
        if hasAnswer { setMacRoman(&d.pointee.answer, typed) }
        d.pointee.selected = Int32(picked)
        return chosen
    }, nil)
}
#endif

#if canImport(AppKit)
/// A "Disk" pop-up for the file panels: the volumes the program sees (its
/// own disk, folders mounted as disks, AppleShare volumes), each opening
/// its folder in the panel.
@MainActor final class DiskPicker: NSObject {
    weak var panel: NSSavePanel?
    var volumes: [(name: String, root: String)] = []
    let row = NSStackView()
    var count: Int { volumes.count }

    init(panel: NSSavePanel) {
        self.panel = panel
        super.init()
        for i in 0..<Int(machine_volume_count()) {
            var name = [CChar](repeating: 0, count: 256), root = [CChar](repeating: 0, count: 1024)
            if machine_volume_info(Int32(i), &name, name.count, &root, root.count) {
                func text(_ c: [CChar]) -> String {
                    String(decoding: c.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
                }
                volumes.append((text(name), text(root)))
            }
        }
        let popup = NSPopUpButton()
        popup.addItems(withTitles: volumes.map { $0.name })
        popup.target = self
        popup.action = #selector(choose(_:))
        // The disk the panel starts on: the volume whose folder holds it most closely.
        if let start = panel.directoryURL?.path,
           let i = volumes.indices.filter({ start == volumes[$0].root || start.hasPrefix(volumes[$0].root + "/") })
                .max(by: { volumes[$0].root.count < volumes[$1].root.count }) {
            popup.selectItem(at: i)
        }
        row.orientation = .horizontal
        row.addArrangedSubview(NSTextField(labelWithString: "Disk:"))
        row.addArrangedSubview(popup)
    }

    /// The disk chosen: the panel closes, to open again there.
    var reopenAt: String?

    @objc func choose(_ sender: NSPopUpButton) {
        let i = sender.indexOfSelectedItem
        guard volumes.indices.contains(i) else { return }
        reopenAt = volumes[i].root
        panel?.cancel(nil)
    }
}

/// Shows only files of the requested classic types (and folders).
final class ClassicTypeFilter: NSObject, NSOpenSavePanelDelegate {
    let types: Set<UInt32>
    init(types: Set<UInt32>) { self.types = types }

    func panel(_ sender: Any, shouldEnable url: URL) -> Bool {
        var isDir: ObjCBool = false
        if FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue,
           url.pathExtension != "app" { return true }
        if !types.isEmpty && !types.contains(machine_file_type(url.path)) { return false }
        // The application's own file filter, if it has one.
        return !machine_file_filtered(url.path)
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
        for option in ["--appearance", "--scale", "--screen"] {
            if let i = CommandLine.arguments.firstIndex(of: option), i + 1 < CommandLine.arguments.count {
                args += [option, CommandLine.arguments[i + 1]]
            }
        }
        if CommandLine.arguments.contains("--screen-window") { args.append("--screen-window") }
        // The screen keeps a depth other than the default, as a Mac's did from
        // one program to the next (Dark Castle's loader, at 1 bit, launches
        // its game); at the default, the next program may still ask for its own.
        if machine_screen_depth() != 32 { args += ["--depth", String(machine_screen_depth())] }
        let p = Process()
        p.executableURL = exe
        p.arguments = args
        var env = ProcessInfo.processInfo.environment
        env["__CFBundleIdentifier"] = nil // its own name, not its launcher's
        p.environment = env
        do { try p.run() } catch { return false }
        // Its serial number for the launcher's events to it.
        machine_launched_process(p.processIdentifier)
        return true
    }, nil)
    // A URL a program opens (Internet Config's ICLaunchURL) goes to the
    // host's handler for its scheme: a web page to the default browser.
    machine_set_url_handler({ urlPtr, _ in
        guard let urlPtr, let url = URL(string: String(cString: urlPtr)), url.scheme != nil else { return false }
        return NSWorkspace.shared.open(url)
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
        let save = rq.save, folders = rq.folders
        let prompt = withUnsafeBytes(of: rq.prompt) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        let name = withUnsafeBytes(of: rq.default_name) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        let dir = withUnsafeBytes(of: rq.directory) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        let types = withUnsafeBytes(of: rq.types) { raw in
            Set(raw.bindMemory(to: UInt32.self).prefix(Int(rq.ntypes)))
        }
        // The items' image pointers stay valid while the emulator waits here.
        nonisolated(unsafe) let items = withUnsafeBytes(of: rq.items) { raw in
            Array(raw.bindMemory(to: machine_file_item.self).prefix(Int(rq.nitems)))
        }
        var chosen: String?
        onMain {
            MainActor.assumeIsolated {
                // Choosing another disk closes the panel and opens it again
                // there (a running panel doesn't move when told to).
                var startDir = dir, saveName = name
                var reopen: String?
                repeat {
                    reopen = nil
                    let panel: NSSavePanel
                    let filter = ClassicTypeFilter(types: types)
                    if save {
                        panel = NSSavePanel()
                        panel.nameFieldStringValue = saveName
                        // Classic names are taken as typed: no extension added or hidden.
                        panel.allowsOtherFileTypes = true
                        panel.isExtensionHidden = false
                        panel.canSelectHiddenExtension = false
                        panel.canCreateDirectories = true
                    } else {
                        let open = NSOpenPanel()
                        // Navigation Services' folder and volume choosers want a folder.
                        open.canChooseFiles = !folders
                        open.canChooseDirectories = folders
                        open.allowsMultipleSelection = false
                        open.delegate = filter
                        panel = open
                    }
                    if !prompt.isEmpty { panel.message = prompt }
                    if !startDir.isEmpty { panel.directoryURL = URL(fileURLWithPath: startDir, isDirectory: true) }
                    let accessory = ClassicItemsView(items: items, panel: panel)
                    // The program's disks, as Standard File's Desktop showed them:
                    // a mounted folder or AppleShare volume is otherwise hard to find.
                    let disks = DiskPicker(panel: panel)
                    if !items.isEmpty || disks.count > 1 {
                        let stack = NSStackView()
                        stack.orientation = .vertical
                        stack.alignment = .leading
                        if disks.count > 1 { stack.addArrangedSubview(disks.row) }
                        if !items.isEmpty { stack.addArrangedSubview(accessory) }
                        stack.layoutSubtreeIfNeeded()
                        stack.frame.size = stack.fittingSize
                        panel.accessoryView = stack
                        if let open = panel as? NSOpenPanel { open.isAccessoryViewDisclosed = true }
                    }
                    // The application's hook gets null events meanwhile.
                    let idle = Timer(timeInterval: 0.5, repeats: true) { _ in machine_file_idle() }
                    RunLoop.main.add(idle, forMode: .modalPanel)
                    // The filter and the pop-up's target (both held weakly) live as long as the panel runs.
                    withExtendedLifetime((filter, disks)) {
                        if panel.runModal() == .OK, let url = panel.url { chosen = url.path }
                    }
                    reopen = disks.reopenAt
                    saveName = panel.nameFieldStringValue // kept when it opens again
                    idle.invalidate()
                    if let r = reopen { startDir = r }
                } while reopen != nil
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
        let saveAsPDF = job.pointee.save_pdf
        let destination = Int(job.pointee.destination)
        let printerName = withUnsafeBytes(of: job.pointee.printer) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        let folder = withUnsafeBytes(of: job.pointee.folder) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        let docTitle = withUnsafeBytes(of: job.pointee.title) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        let copies = Int(job.pointee.copies)
        var ok = false
        onMain {
            MainActor.assumeIsolated {
                let title = !docTitle.isEmpty ? docTitle
                    : NSApp.windows.first(where: { $0.isKeyWindow })?.title ?? ProcessInfo.processInfo.processName
                if destination == Int(MACHINE_PRINT_TO_PDF), !saveAsPDF { // the Chooser's Save as PDF: no panel
                    let url = pdfDestination(folder, title: title)
                    ok = (try? pdf.write(to: url)) != nil
                    return
                }
                if destination == Int(MACHINE_PRINT_TO_HOST), !saveAsPDF, NSPrinter(name: printerName) == nil {
                    ok = printWithLP(pdf, printer: printerName, title: title, copies: copies) // one CUPS found
                    return
                }
                if destination == Int(MACHINE_PRINT_TO_HOST), !saveAsPDF, let printer = NSPrinter(name: printerName),
                   let doc = PDFDocument(data: pdf) { // a host printer the Chooser chose: no panel
                    let info = sessionPrintInfo.copy() as! NSPrintInfo
                    info.printer = printer
                    info.topMargin = 0
                    info.bottomMargin = 0
                    info.leftMargin = 0
                    info.rightMargin = 0
                    if copies > 1 { info.dictionary()[NSPrintInfo.AttributeKey.copies] = copies }
                    guard let op = doc.printOperation(for: info, scalingMode: .pageScaleNone, autoRotate: false) else { return }
                    op.jobTitle = title
                    op.showsPrintPanel = false
                    op.showsProgressPanel = true
                    ok = op.run()
                    return
                }
                if saveAsPDF { // the classic job dialog's Save as PDF
                    let panel = NSSavePanel()
                    panel.allowedContentTypes = [.pdf]
                    panel.nameFieldStringValue = title + ".pdf"
                    guard panel.runModal() == .OK, let url = panel.url else { return }
                    ok = (try? pdf.write(to: url)) != nil
                    return
                }
                guard let doc = PDFDocument(data: pdf) else { return }
                let info = sessionPrintInfo.copy() as! NSPrintInfo
                info.topMargin = 0
                info.bottomMargin = 0
                info.leftMargin = 0
                info.rightMargin = 0
                guard let op = doc.printOperation(for: info, scalingMode: .pageScaleNone, autoRotate: false) else { return }
                op.jobTitle = title
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

/// A PDF's destination: the path given, or a new file named after the
/// document and the time when the path is a folder.
func pdfDestination(_ path: String, title: String) -> URL {
    var isDir: ObjCBool = false
    guard FileManager.default.fileExists(atPath: path, isDirectory: &isDir), isDir.boolValue else {
        return URL(fileURLWithPath: path)
    }
    let stamp = DateFormatter()
    stamp.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
    let base = "\(title.replacingOccurrences(of: "/", with: "-")) \(stamp.string(from: Date()))"
    var url = URL(fileURLWithPath: path).appendingPathComponent(base + ".pdf")
    var n = 2
    while FileManager.default.fileExists(atPath: url.path) {
        url = URL(fileURLWithPath: path).appendingPathComponent("\(base) \(n).pdf")
        n += 1
    }
    return url
}

/// Headless printing: the job is written as a PDF file, no panels.
func installPrintToFile(_ path: String) {
    printToPath = path
    machine_set_print_handlers({ job, _ in
        guard let job, let path = printToPath else { return false }
        let url = pdfDestination(path, title: ProcessInfo.processInfo.processName)
        do {
            try ClassicPrint.pdf(job).write(to: url)
            print("printed \(job.pointee.npages) page(s) to \(url.path)")
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
        case Int(MACHINE_ITEM_IMAGE):
            let iv = NSImageView(image: Self.image(it) ?? NSImage())
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
    /// What the app draws in a user item (RGBA from the emulator).
    static func image(_ it: machine_file_item) -> NSImage? {
        guard let pixels = it.image, it.image_width > 0, it.image_height > 0,
              let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(it.image_width),
                                         pixelsHigh: Int(it.image_height), bitsPerSample: 8, samplesPerPixel: 4,
                                         hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                         bytesPerRow: Int(it.image_width) * 4, bitsPerPixel: 32),
              let dest = rep.bitmapData else { return nil }
        dest.update(from: pixels, count: Int(it.image_width) * Int(it.image_height) * 4)
        let img = NSImage(size: NSSize(width: Int(it.image_width), height: Int(it.image_height)))
        img.addRepresentation(rep)
        return img
    }

    func refresh(_ items: [machine_file_item]) {
        for it in items {
            guard let v = controls[it.item] else { continue }
            v.isHidden = !it.visible
            if it.kind == Int32(MACHINE_ITEM_IMAGE), let iv = v as? NSImageView, let img = Self.image(it) {
                iv.image = img
            }
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

    /// Nonisolated: the handler runs on the machine's thread.
    nonisolated func install() {
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
            // Permission is asked for when the program first has something
            // to say, not when every program starts.
            UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound, .badge]) { granted, _ in
                guard granted else { return }
                let content = UNMutableNotificationContent()
                content.title = ProcessInfo.processInfo.processName
                content.body = message
                UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: "bluebox-\(id)",
                                                                             content: content, trigger: nil))
            }
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

#if canImport(AppKit)
/// Tells the machine what the host shows of it (machine_set_host_visibility):
/// 0, a window of ours in front (or, rootless, the program frontmost: its
/// menu bar is up, its first window may be yet to come); 1, one shown
/// behind another application's; 2, none shown (minimized, hidden,
/// covered).  The status item's window, in the menu bar, doesn't count.
@MainActor @discardableResult func noteHostVisibility(frontIsSeen: Bool = false) -> Int32 {
    if frontIsSeen && NSApp.isActive {
        machine_set_host_visibility(0)
        return 0
    }
    let shown = !NSApp.isHidden && NSApp.windows.contains { w in
        w.isVisible && !w.isMiniaturized && w.occlusionState.contains(.visible)
            && !String(describing: type(of: w)).hasPrefix("NSStatusBar")
    }
    let level: Int32 = shown ? (NSApp.isActive ? 0 : 1) : 2
    machine_set_host_visibility(level)
    return level
}
#endif
