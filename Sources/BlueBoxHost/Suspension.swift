// Suspending a running program: what the program's process, the shell and
// their menus share.
//
// A program suspended (Suspend in its application menu, the shell's
// Suspend button) has its machine held still (machine_pause)
// and its windows dimmed and inert, each with Resume and Hide buttons
// over it.  The shell and the programs it launched tell each other with
// distributed notifications, sent at once even to an application in the
// background: the shell asks a program by its process ID, and a program
// says when it is suspended or resumed.

#if canImport(AppKit)
import AppKit

public enum SuspendNotice {
    /// Shell to program: object the program's process ID, userInfo
    /// ["suspend": Bool].
    public static let request = Notification.Name("local.bluebox.suspend.request")
    /// Program to shell: object its process ID, userInfo ["suspended": Bool].
    public static let state = Notification.Name("local.bluebox.suspend.state")

    public static func ask(pid: Int32, suspend: Bool) {
        DistributedNotificationCenter.default().postNotificationName(
            request, object: String(pid), userInfo: ["suspend": suspend], deliverImmediately: true)
    }

    public static func tell(suspended: Bool) {
        DistributedNotificationCenter.default().postNotificationName(
            state, object: String(getpid()), userInfo: ["suspended": suspended], deliverImmediately: true)
    }

    /// What a state notice says: whose, and whether it is suspended.
    public static func parse(_ note: Notification) -> (pid: Int32, suspended: Bool)? {
        guard let object = note.object as? String, let pid = Int32(object),
              let suspended = note.userInfo?["suspended"] as? Bool else { return nil }
        return (pid, suspended)
    }
}

/// The shell's programs: which are running (by application path, with
/// their process IDs) and which of them are suspended.  The registry
/// (ProgramRegistry) is the truth for every program, the shell's own and
/// those started from a terminal or a bundle; the shell's own children
/// count from their launch, before they have entered themselves.
public struct ProgramStates: Equatable {
    public enum Action: Equatable {
        case launch, suspend, resume
        public var title: String {
            switch self {
            case .launch: return "Launch"
            case .suspend: return "Suspend"
            case .resume: return "Resume"
            }
        }
    }

    /// What opening a program does (double-clicking it, Launch, its context
    /// menu's Launch): start it, bring it to the front, or resume it.
    public enum Open: Equatable {
        case launch
        case activate(pid: Int32)
        case resume(pid: Int32)
    }

    /// The shell's children, by path.
    private var children: [String: Int32] = [:]
    /// The registry's programs, by path (the oldest of a path's).
    public private(set) var registered: [String: RunningProgram] = [:]
    /// What the programs have said since the registry was last read.
    private var noted: [Int32: Bool] = [:]

    public init() {}

    public var pids: [String: Int32] { children.merging(registered.mapValues(\.pid)) { _, entry in entry } }

    public func isRunning(_ path: String) -> Bool { pids[path] != nil }
    public func isSuspended(_ path: String) -> Bool {
        guard let pid = pids[path] else { return false }
        return noted[pid] ?? (registered[path]?.state == .suspended)
    }

    /// What the button where Launch is does for this application.
    public func action(for path: String) -> Action {
        !isRunning(path) ? .launch : isSuspended(path) ? .resume : .suspend
    }

    public func open(_ path: String) -> Open {
        guard let pid = pids[path] else { return .launch }
        return isSuspended(path) ? .resume(pid: pid) : .activate(pid: pid)
    }

    /// The name to list a program by: what its entry calls it.
    public func title(_ path: String) -> String { registered[path]?.title ?? (path as NSString).lastPathComponent }

    public mutating func launched(_ path: String, pid: Int32) {
        if let old = children[path] { noted[old] = nil }
        children[path] = pid
        noted[pid] = nil
    }

    /// A child gone, however it went (quit, crashed, killed while
    /// suspended): its entry too, until the registry is read again.
    public mutating func ended(_ path: String) {
        guard let pid = children.removeValue(forKey: path) else { return }
        noted[pid] = nil
        if registered[path]?.pid == pid { registered[path] = nil }
    }

    /// The registry read again: its live programs (a hibernated one is not
    /// running).
    public mutating func registry(_ entries: [RunningProgram]) {
        var byPath: [String: RunningProgram] = [:]
        for e in entries where e.state != .hibernated && byPath[e.program] == nil { byPath[e.program] = e }
        registered = byPath
        noted = noted.filter { pid, _ in !entries.contains { $0.pid == pid } }
        let known = Set(pids.values)
        noted = noted.filter { known.contains($0.key) }
    }

    /// A program's word on itself; one the shell knows nothing of is ignored.
    public mutating func noted(pid: Int32, suspended on: Bool) {
        guard pids.values.contains(pid) else { return }
        noted[pid] = on
    }

    /// Running and not suspended, and suspended: each in name order.
    public var runningPaths: [String] { Self.byName(pids.keys.filter { !isSuspended($0) }) }
    public var suspendedPaths: [String] { Self.byName(pids.keys.filter { isSuspended($0) }) }

    static func byName(_ paths: [String]) -> [String] {
        paths.sorted {
            let a = ($0 as NSString).lastPathComponent, b = ($1 as NSString).lastPathComponent
            return a == b ? $0 < $1 : a.localizedStandardCompare(b) == .orderedAscending
        }
    }
}

/// What a key does in a program's process (the key monitor's and the
/// screen window's first look at a key-down).
public enum HostKey: Equatable {
    /// To the classic program, as a Mac delivered it.
    case classic
    /// Command-Option-Q: Force Quit, for a program that ignores Command-Q.
    case forceQuit
    /// Command-Q while suspended: resume and ask the program to quit.
    case quit
    /// Command-H and Command-Option-H while suspended: Hide and Hide
    /// Others (running, the program has them, as a Mac gave them).
    case hide, hideOthers
    /// Anything else while suspended: nothing.
    case ignore

    public static func classify(_ flags: NSEvent.ModifierFlags, _ key: String?, suspended: Bool) -> HostKey {
        let mods = flags.intersection([.command, .option, .control, .shift])
        if key?.lowercased() == "q" {
            if mods == [.command, .option] { return .forceQuit }
            if mods == [.command] && suspended { return .quit }
        }
        if key?.lowercased() == "h" && suspended {
            if mods == [.command] { return .hide }
            if mods == [.command, .option] { return .hideOthers }
        }
        return suspended ? .ignore : .classic
    }
}

/// The application menu's end: Suspend (Resume when suspended), then Quit,
/// with Option Force Quit (Command-Option-Q) in its place.  Quit asks the
/// classic program to quit, resuming it first.
@MainActor public enum HostQuitItems {
    public static func items(title: String, suspended: Bool, target: AnyObject, quit: Selector, suspend: Selector,
                             forceQuit: Selector, quitKeys: Bool = true) -> [NSMenuItem] {
        let verb = suspended ? "Resume" : "Suspend"
        let toggle = NSMenuItem(title: "\(verb) \(title)", action: suspend, keyEquivalent: "")
        toggle.target = target
        let quitItem = NSMenuItem(title: "Quit \(title)", action: quit, keyEquivalent: quitKeys ? "q" : "")
        quitItem.target = target
        guard quitKeys else { return [toggle, quitItem] }
        let force = NSMenuItem(title: "Force Quit \(title)", action: forceQuit, keyEquivalent: "q")
        force.keyEquivalentModifierMask = [.command, .option]
        force.isAlternate = true
        force.target = target
        return [toggle, .separator(), quitItem, force]
    }
}

/// The application menu's Hide, Hide Others and Show All, above Suspend
/// and Quit, as a Mac application's.  Their keys show, but Command-H and
/// Command-Option-H typed go to the classic program as Command-Q does (many
/// use Command-H themselves); chosen, they hide.
@MainActor public enum HostHideItems {
    public static func items(title: String) -> [NSMenuItem] {
        let hide = NSMenuItem(title: "Hide \(title)", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let others = NSMenuItem(title: "Hide Others", action: #selector(NSApplication.hideOtherApplications(_:)),
                                keyEquivalent: "h")
        others.keyEquivalentModifierMask = [.command, .option]
        let all = NSMenuItem(title: "Show All", action: #selector(NSApplication.unhideAllApplications(_:)),
                             keyEquivalent: "")
        return [hide, others, all, .separator()]
    }
}

/// The shell's status item: its section of suspended programs, each of
/// which resumes it when chosen (representedObject: the path).
@MainActor public enum SuspendedMenuSection {
    public static func items(_ paths: [String], target: AnyObject, resume: Selector) -> [NSMenuItem] {
        guard !paths.isEmpty else { return [] }
        let header = NSMenuItem(title: "Suspended", action: nil, keyEquivalent: "")
        header.isEnabled = false
        var out = [header]
        for path in paths {
            let item = NSMenuItem(title: (path as NSString).lastPathComponent, action: resume, keyEquivalent: "")
            item.target = target
            item.representedObject = path
            item.toolTip = "Resume it"
            out.append(item)
        }
        out.append(.separator())
        return out
    }
}

/// Over each window of a suspended program: the window greyed and taking
/// no clicks, with Resume and, below it, Hide (the whole application) in
/// the middle when the window has room for them.
public final class SuspendedOverlay: NSView {
    public let resumeButton: NSButton
    public let hideButton: NSButton
    private let panel = NSVisualEffectView()

    public init(frame: NSRect, resume: @escaping () -> Void, hide: @escaping () -> Void) {
        resumeButton = ClosureButton(title: "Resume", action: resume)
        hideButton = ClosureButton(title: "Hide", action: hide)
        super.init(frame: frame)
        autoresizingMask = [.width, .height]
        wantsLayer = true
        layer?.backgroundColor = NSColor.gray.withAlphaComponent(0.55).cgColor
        panel.material = .hudWindow
        panel.blendingMode = .withinWindow
        panel.state = .active
        panel.wantsLayer = true
        panel.layer?.cornerRadius = 10
        let label = NSTextField(labelWithString: "Suspended")
        label.font = .boldSystemFont(ofSize: NSFont.systemFontSize)
        let stack = NSStackView(views: [label, resumeButton, hideButton])
        stack.orientation = .vertical
        stack.spacing = 8
        stack.edgeInsets = NSEdgeInsets(top: 12, left: 16, bottom: 12, right: 16)
        stack.translatesAutoresizingMaskIntoConstraints = false
        resumeButton.widthAnchor.constraint(equalTo: hideButton.widthAnchor).isActive = true
        resumeButton.widthAnchor.constraint(greaterThanOrEqualToConstant: 80).isActive = true
        panel.addSubview(stack)
        panel.translatesAutoresizingMaskIntoConstraints = false
        addSubview(panel)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: panel.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: panel.trailingAnchor),
            stack.topAnchor.constraint(equalTo: panel.topAnchor),
            stack.bottomAnchor.constraint(equalTo: panel.bottomAnchor),
            panel.centerXAnchor.constraint(equalTo: centerXAnchor),
            panel.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    public override func layout() {
        super.layout()
        // A window too small for the buttons (a palette) is only greyed.
        let fits = panel.fittingSize
        panel.isHidden = fits.width > bounds.width || fits.height > bounds.height
    }

    // Inert: the clicks stop here, and the window's cursor is the arrow.
    public override func mouseDown(with event: NSEvent) {}
    public override func mouseUp(with event: NSEvent) {}
    public override func mouseDragged(with event: NSEvent) {}
    public override func rightMouseDown(with event: NSEvent) {}
    public override func otherMouseDown(with event: NSEvent) {}
    public override func scrollWheel(with event: NSEvent) {}
    public override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    public override func resetCursorRects() { addCursorRect(bounds, cursor: .arrow) }
}

/// A push button that runs a closure.
final class ClosureButton: NSButton {
    private var run: () -> Void = {}

    convenience init(title: String, action: @escaping () -> Void) {
        self.init(frame: .zero)
        self.title = title
        bezelStyle = .rounded
        setButtonType(.momentaryPushIn)
        run = action
        target = self
        self.action = #selector(fire(_:))
    }

    @objc private func fire(_ sender: Any?) { run() }
}
#endif
