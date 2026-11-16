// macOS host: MacsBug's console.
//
// The debugger itself is the machine's (Sources/Toolbox/macsbug.c): its
// commands are lines of text in, text out.  This is a window over it: the
// output as it comes, a command line, the registers beside them, and
// buttons for what is typed most (g, t, s, the programmer's switch).
//
// It is there, as a Debug submenu in the program's application menu and
// Control-Option-Command-D, when the developer tools are installed
// (xcode-select -p) or the program's debugger setting asks (menu; stop also
// opens it at the program's own Debugger and DebugStr calls); the setting
// off keeps it away.  While the machine is stopped in it, the machine's
// thread waits for the commands typed here, its clock held still.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Toolbox

/// Whether the developer tools are installed, as xcode-select says.
func developerToolsInstalled() -> Bool {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/xcode-select")
    p.arguments = ["-p"]
    p.standardOutput = FileHandle.nullDevice
    p.standardError = FileHandle.nullDevice
    do { try p.run() } catch { return false }
    p.waitUntilExit()
    return p.terminationStatus == 0
}

/// The machine's thread tells the console of each stop, output and going on.
private func debuggerEvent(_ ctx: UnsafeMutableRawPointer?, _ event: Int32, _ text: UnsafePointer<CChar>?) {
    let s = text.map { String(cString: $0) } ?? ""
    DispatchQueue.main.async { MainActor.assumeIsolated { DebuggerConsole.shared.event(event, s) } }
}

@MainActor final class DebuggerConsole: NSObject, NSWindowDelegate, NSTextFieldDelegate {
    static let shared = DebuggerConsole()

    /// The Debug menu and its key are there.
    private(set) var enabled = false
    private var title = ""
    private var panel: NSPanel?
    private var output: NSTextView?
    private var registers: NSTextView?
    private var input: NSTextField?
    private var status: NSTextField?
    private(set) var stopped = false
    private var history: [String] = []
    private var historyAt = 0

    /// The run's choice: the program's debugger setting (menu, stop, off or
    /// unset) and whether --debugger was given.
    func configure(title: String, setting: String?, modeGiven: Bool) {
        self.title = title
        let stops = machine_debugger_mode() == Int32(DEBUGGER_STOP)
        if setting == "stop" && !modeGiven { machine_set_debugger(Int32(DEBUGGER_STOP)) }
        guard setting == "menu" || setting == "stop" || stops || (setting != "off" && developerToolsInstalled()) else {
            return
        }
        enabled = true
        HostKey.debuggerEnabled = true
        machine_debug_attach(debuggerEvent, nil, true)
    }

    // MARK: Menu

    /// The application menu's Debug submenu (nothing when not enabled).
    func menuItems() -> [NSMenuItem] {
        guard enabled else { return [] }
        let menu = NSMenu(title: "Debug")
        menu.autoenablesItems = false
        let brk = NSMenuItem(title: "Break into MacsBug", action: #selector(breakItem(_:)), keyEquivalent: HostKey.debuggerKey)
        brk.keyEquivalentModifierMask = HostKey.hibernateModifiers
        brk.target = self
        menu.addItem(brk)
        let show = NSMenuItem(title: "Show MacsBug", action: #selector(showItem(_:)), keyEquivalent: "")
        show.target = self
        menu.addItem(show)
        menu.addItem(.separator())
        let calls = NSMenuItem(title: "Stop at the Program's Debugger Calls", action: #selector(callsItem(_:)),
                               keyEquivalent: "")
        calls.target = self
        calls.state = machine_debugger_mode() == Int32(DEBUGGER_STOP) ? .on : .off
        menu.addItem(calls)
        let item = NSMenuItem(title: "Debug", action: nil, keyEquivalent: "")
        item.submenu = menu
        return [item, .separator()]
    }

    @objc func breakItem(_ sender: Any?) { breakIn() }
    @objc func showItem(_ sender: Any?) { show() }
    @objc func callsItem(_ sender: NSMenuItem) {
        let on = machine_debugger_mode() != Int32(DEBUGGER_STOP)
        machine_set_debugger(Int32(on ? DEBUGGER_STOP : DEBUGGER_LOG))
        sender.state = on ? .on : .off
        machine_debug_attach(debuggerEvent, nil, true) // the trap hooks see the change
    }

    /// The programmer's switch: the machine stops at its next instruction.
    func breakIn() {
        guard enabled else { return }
        show()
        if !stopped { machine_debug_break() }
    }

    // MARK: Events

    func event(_ kind: Int32, _ text: String) {
        switch kind {
        case Int32(MACHINE_DEBUG_STOP):
            stopped = true
            show()
            status?.stringValue = "Stopped: \(text)"
            refreshRegisters()
            NSApp.activate(ignoringOtherApps: true)
            panel?.makeKeyAndOrderFront(nil)
            panel?.makeFirstResponder(input)
        case Int32(MACHINE_DEBUG_OUTPUT):
            append(text)
            if stopped { refreshRegisters() }
        default:
            stopped = false
            status?.stringValue = "Running"
        }
    }

    private func append(_ text: String) {
        guard let output else { return }
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular),
            .foregroundColor: NSColor.textColor,
        ]
        let storage = output.textStorage
        storage?.append(NSAttributedString(string: text, attributes: attrs))
        // Kept to the last few hundred thousand characters.
        if let s = storage, s.length > 400_000 { s.deleteCharacters(in: NSRange(location: 0, length: s.length - 300_000)) }
        output.scrollToEndOfDocument(nil)
    }

    private func refreshRegisters() {
        var buf = [CChar](repeating: 0, count: 8192)
        machine_debug_registers(&buf, buf.count)
        registers?.string = str(buf)
    }

    // MARK: Commands

    private func send(_ line: String) {
        if !line.isEmpty, history.last != line { history.append(line) }
        historyAt = history.count
        // A command typed while the machine runs stops it first.
        if !stopped { machine_debug_break() }
        machine_debug_post(line)
    }

    @objc func entered(_ sender: NSTextField) {
        // Return on an empty line repeats the last command, as in MacsBug.
        let line = sender.stringValue.isEmpty ? (history.last ?? "") : sender.stringValue
        sender.stringValue = ""
        guard !line.isEmpty else { return }
        send(line)
    }

    @objc func goButton(_ sender: Any?) { send("g") }
    @objc func stepButton(_ sender: Any?) { send("t") }
    @objc func stepIntoButton(_ sender: Any?) { send("s") }
    @objc func returnButton(_ sender: Any?) { send("mr") }
    @objc func breakButton(_ sender: Any?) { if !stopped { machine_debug_break() } }

    /// Up and down arrows walk the command history.
    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        if selector == #selector(NSResponder.moveUp(_:)), historyAt > 0 {
            historyAt -= 1
            input?.stringValue = history[historyAt]
            return true
        }
        if selector == #selector(NSResponder.moveDown(_:)), historyAt < history.count {
            historyAt += 1
            input?.stringValue = historyAt < history.count ? history[historyAt] : ""
            return true
        }
        return false
    }

    // MARK: The window

    func show() {
        if panel == nil { build() }
        panel?.makeKeyAndOrderFront(nil)
    }

    /// Closing it while stopped lets the machine go on; it is only hidden.
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if stopped { send("g") }
        sender.orderOut(nil)
        return false
    }

    private func textView(editable: Bool) -> (NSScrollView, NSTextView) {
        let scroll = NSTextView.scrollableTextView()
        let tv = scroll.documentView as! NSTextView
        tv.isEditable = editable
        tv.isRichText = false
        tv.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        tv.textContainerInset = NSSize(width: 4, height: 4)
        scroll.translatesAutoresizingMaskIntoConstraints = false
        return (scroll, tv)
    }

    private func button(_ title: String, _ action: Selector, tip: String) -> NSButton {
        let b = NSButton(title: title, target: self, action: action)
        b.bezelStyle = .rounded
        b.toolTip = tip
        return b
    }

    private func build() {
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 960, height: 560),
                        styleMask: [.titled, .closable, .resizable, .miniaturizable, .utilityWindow],
                        backing: .buffered, defer: false)
        p.title = "MacsBug — \(title)"
        p.isReleasedWhenClosed = false
        p.hidesOnDeactivate = false
        p.becomesKeyOnlyIfNeeded = false
        p.delegate = self
        p.minSize = NSSize(width: 600, height: 300)
        let (outScroll, out) = textView(editable: false)
        let (regScroll, regs) = textView(editable: false)
        let field = NSTextField(string: "")
        field.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        field.placeholderString = "MacsBug command (help lists them)"
        field.target = self
        field.action = #selector(entered(_:))
        field.delegate = self
        field.translatesAutoresizingMaskIntoConstraints = false
        let state = NSTextField(labelWithString: "Running")
        state.translatesAutoresizingMaskIntoConstraints = false
        state.lineBreakMode = .byTruncatingTail
        let buttons = NSStackView(views: [
            button("Go", #selector(goButton(_:)), tip: "g: go on"),
            button("Step", #selector(stepButton(_:)), tip: "t: step over calls and traps"),
            button("Step Into", #selector(stepIntoButton(_:)), tip: "s: step into calls"),
            button("Return", #selector(returnButton(_:)), tip: "mr: go till the routine returns"),
            button("Break", #selector(breakButton(_:)), tip: "Stop the machine (Control-Option-Command-D)"),
        ])
        buttons.orientation = .horizontal
        buttons.translatesAutoresizingMaskIntoConstraints = false
        let content = NSView()
        for v in [outScroll, regScroll, field, state, buttons] as [NSView] { content.addSubview(v) }
        NSLayoutConstraint.activate([
            buttons.topAnchor.constraint(equalTo: content.topAnchor, constant: 8),
            buttons.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 8),
            state.centerYAnchor.constraint(equalTo: buttons.centerYAnchor),
            state.leadingAnchor.constraint(equalTo: buttons.trailingAnchor, constant: 12),
            state.trailingAnchor.constraint(lessThanOrEqualTo: content.trailingAnchor, constant: -8),
            outScroll.topAnchor.constraint(equalTo: buttons.bottomAnchor, constant: 8),
            outScroll.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            outScroll.bottomAnchor.constraint(equalTo: field.topAnchor, constant: -6),
            regScroll.topAnchor.constraint(equalTo: outScroll.topAnchor),
            regScroll.leadingAnchor.constraint(equalTo: outScroll.trailingAnchor, constant: 1),
            regScroll.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            regScroll.bottomAnchor.constraint(equalTo: outScroll.bottomAnchor),
            regScroll.widthAnchor.constraint(equalToConstant: 330),
            field.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 6),
            field.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -6),
            field.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -6),
        ])
        p.contentView = content
        p.center()
        panel = p
        output = out
        registers = regs
        input = field
        status = state
        append("MacsBug for \(title).  Type help for the commands; Control-Option-Command-D breaks in.\n")
        refreshRegisters()
    }
}
#endif
