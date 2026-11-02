// A module's setup, from a button among its controls (Choose File…,
// Edit…, Select!): the module runs with the button pressed, as After Dark's
// panel ran it, on a classic screen in this sheet, which takes the mouse and
// the keys for the module's own dialog.  What the module changes is kept
// with its settings when Done stops it.

#if canImport(ScreenSaver)
import AppKit
import Toolbox

/// The machine's screen, with the mouse and keys going to it.
private final class SetupScreenView: NSView {
    private var timer: Timer?

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.magnificationFilter = .nearest
        layer?.contentsGravity = .resize
    }

    required init?(coder: NSCoder) { nil }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        timer?.invalidate()
        guard window != nil else { return }
        let t = Timer(timeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.layer?.contents = SaverMachine.shared.frame(for: nil) }
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        window?.acceptsMouseMovedEvents = true
        window?.makeFirstResponder(self)
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func post(_ event: NSEvent, button: Int32) {
        let p = convert(event.locationInWindow, from: nil)
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        guard bounds.width > 0, bounds.height > 0 else { return }
        machine_post_mouse(Int32(p.x * CGFloat(w) / bounds.width), Int32(p.y * CGFloat(h) / bounds.height), button)
    }

    override func mouseDown(with event: NSEvent) { post(event, button: 1) }
    override func mouseUp(with event: NSEvent) { post(event, button: 0) }
    override func mouseDragged(with event: NSEvent) { post(event, button: -1) }
    override func mouseMoved(with event: NSEvent) { post(event, button: -1) }

    /// The classic character code for a key (MacRoman).
    private static func charCode(_ event: NSEvent) -> Int32 {
        let special: [UInt16: Int32] = [123: 28, 124: 29, 125: 31, 126: 30, 51: 8, 117: 127, 36: 13, 76: 3, 48: 9, 53: 27]
        if let c = special[event.keyCode] { return c }
        guard let s = event.charactersIgnoringModifiers?.isEmpty == false ? event.characters : nil,
              let b = s.data(using: .macOSRoman, allowLossyConversion: true)?.first else { return -1 }
        return Int32(b)
    }

    private static func modifiers(_ flags: NSEvent.ModifierFlags) -> UInt32 {
        var m: UInt32 = 0
        if flags.contains(.command) { m |= 0x100 }
        if flags.contains(.shift) { m |= 0x200 }
        if flags.contains(.capsLock) { m |= 0x400 }
        if flags.contains(.option) { m |= 0x800 }
        if flags.contains(.control) { m |= 0x1000 }
        return m
    }

    override func keyDown(with event: NSEvent) {
        machine_post_key(Self.charCode(event), Int32(event.keyCode), Self.modifiers(event.modifierFlags), 1,
                         event.isARepeat ? 1 : 0)
    }

    override func keyUp(with event: NSEvent) {
        machine_post_key(Self.charCode(event), Int32(event.keyCode), Self.modifiers(event.modifierFlags), 0, 0)
    }
}

@MainActor
final class SetupSheet: NSObject {
    let window: NSWindow
    private let screen = SetupScreenView(frame: NSRect(x: 0, y: 0, width: 640, height: 480))
    private let onDone: () -> Void

    /// The module's setup for the button with this 'bVal' ID.
    init(module: String, title: String, button: Int, settings: SaverSettings, onDone: @escaping () -> Void) {
        self.onDone = onDone
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 560), styleMask: [.titled], backing: .buffered,
                          defer: true)
        super.init()
        let label = NSTextField(labelWithString: "\((module as NSString).lastPathComponent): \(title)")
        label.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        let done = NSButton(title: "Done", target: self, action: #selector(finish))
        done.keyEquivalent = "\r"
        done.keyEquivalentModifierMask = [.command]
        let note = NSTextField(labelWithString: "The module's own setup: what it changes is kept when you click Done.")
        note.textColor = .secondaryLabelColor
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        let bottom = NSStackView(views: [note, NSView(), done])
        bottom.orientation = .horizontal
        screen.translatesAutoresizingMaskIntoConstraints = false
        let stack = NSStackView(views: [label, screen, bottom])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.edgeInsets = NSEdgeInsets(top: 12, left: 20, bottom: 12, right: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false
        let content = NSView()
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            stack.topAnchor.constraint(equalTo: content.topAnchor),
            stack.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            screen.widthAnchor.constraint(equalToConstant: 640),
            screen.heightAnchor.constraint(equalToConstant: 480),
            bottom.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40),
        ])
        window.contentView = content
        SaverMachine.shared.setup(module: module, button: button, settings: settings)
    }

    @objc private func finish() {
        screen.stop()
        SaverMachine.shared.stop() // its Close: what it changed is kept
        if let parent = window.sheetParent { parent.endSheet(window) } else { window.orderOut(nil) }
        onDone()
    }
}
#endif
