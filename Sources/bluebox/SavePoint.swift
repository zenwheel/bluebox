// Save points: a snapshot made while the program goes on, and going back to
// the latest without the program (or its windows) ever quitting, as a
// game's quick save and quick load.
//
//   Make Save Point (Control-Option-Command-S, beside Hibernate in the
//   application menu and the status item's menu; savepoint in --keys)
//       stops the machine at a safe point as Hibernate does (drain.c), but
//       without suspending it: no overlay, the sound already given to the
//       host plays on, the network's connections stay open.  The machine's
//       clock stands still for the moment it takes to copy the snapshot
//       (machine_savepoint_take), then the program goes on while the copy
//       is compressed and written (a .bbhib marked as a save point: its
//       record's kind, its registry entry), with a thumbnail, entered so
//       that opening the program offers it beside its hibernated snapshots.
//       Its newest few are kept apart from those (keep-save-points).  The
//       mouse button held (a drag, a menu) is waited out first: the drain
//       would otherwise let go of it for the program.
//   Restore Save Point (Control-Option-Command-R; restore in --keys)
//       stops the machine at a safe point again and puts the latest save
//       point back over it in this process (machine_savepoint_restore): the
//       host's sound and speech for the timeline left behind stop, its
//       sockets close (a connection is over, and the program hears so, as
//       after resuming), and windows, menus and cursor are taken afresh.
//       No question is asked, as a game's quick load asks none: the save
//       point is there to go back to, and the keys are hard to press by
//       chance.  The latest is this run's own (kept in memory too), or the
//       newest save point of the program's that was made by a run like
//       this one (the same arguments, screen and depth), read from its
//       file.
//
// Both say how it went in a small panel over the program's windows
// ("Saved", "Restored", or why not) for a moment.

#if canImport(AppKit)
import AppKit
#endif
import Foundation
import BlueBoxHost
import Toolbox

/// A save point this run made or went back to: its file, its record, and
/// the snapshot itself, so going back to it needs no reading.
final class HeldSavePoint: @unchecked Sendable {
    let path: String
    let record: HibernateRecord
    let buffer: UnsafeMutablePointer<UInt8>
    let length: Int

    init(path: String, record: HibernateRecord, buffer: UnsafeMutablePointer<UInt8>, length: Int) {
        self.path = path
        self.record = record
        self.buffer = buffer
        self.length = length
    }

    deinit { machine_savepoint_free(buffer) }
}

/// The latest save point of this run's (made, or gone back to).
nonisolated(unsafe) var heldSavePoint: HeldSavePoint?

/// The machine stopped at a safe point: a save point taken, for a file at
/// a new path in the Hibernated folder.
func takeSavePoint(app: String, runArgs: [String]) -> Result<HeldSavePoint, SnapshotProblem> {
    guard machine_hibernatable() else { return .failure(SnapshotProblem(String(cString: machine_drain_reason()))) }
    let record = snapshotRecord(app: app, runArgs: runArgs, kind: HibernateRecord.savePointKind)
    var buf: UnsafeMutablePointer<UInt8>?
    var len = 0
    var err = [CChar](repeating: 0, count: 512)
    guard machine_savepoint_take(&buf, &len, &err, err.count), let buf else { return .failure(SnapshotProblem(str(err))) }
    return .success(HeldSavePoint(path: newSnapshotPath(title: record.title), record: record, buffer: buf, length: len))
}

/// A save point taken, written to its file (from any thread, the machine
/// running or not).  Returns its size.
func writeSavePoint(_ held: HeldSavePoint) -> Result<UInt64, SnapshotProblem> {
    try? FileManager.default.createDirectory(atPath: (held.path as NSString).deletingLastPathComponent,
                                             withIntermediateDirectories: true)
    var err = [CChar](repeating: 0, count: 512)
    var bytes: UInt64 = 0
    guard machine_savepoint_write(held.path, held.buffer, held.length, held.record.json, &bytes, &err, err.count) else {
        return .failure(SnapshotProblem(str(err)))
    }
    return .success(bytes)
}

/// A save point's entry in the registry, beside the running program's own
/// (which goes on): hibernated, marked a save point, kept when the process
/// ends.  The program's oldest save points beyond those it keeps then go.
func enterSavePoint(app: String, title: String, snapshot: String) {
    let registry = hibernationRegistry()
    let live = Registration.current?.entry
    let executable = live?.executable ?? Bundle.main.executableURL?.resolvingSymlinksInPath().path ?? CommandLine.arguments[0]
    let program = live?.program ?? RunningProgram.canonical(app)
    let entry = RunningProgram(program: program, executable: executable, title: live?.title ?? title,
                               launchedBy: live?.launchedBy ?? .commandLine, bundle: live?.bundle)
    registry.exclusively {
        guard let r = try? registry.register(entry) else { return }
        r.hibernated(snapshot: snapshot, savePoint: true)
        r.end(keeping: true)
    }
    let keep = (settings(for: app) ?? AppSettings()).over(defaultSettings()).keepSavePoints
        ?? HibernatedSnapshots.defaultKeepSavePoints
    for gone in HibernatedSnapshots.tidy(program: program, keep: keep, folders: hibernatedFolders(), registry: registry,
                                         savePoints: true) {
        if heldSavePoint?.path == gone { heldSavePoint = nil }
        print("save point deleted (\(title) keeps its newest \(keep)): \(gone)")
    }
}

/// The save point Restore goes back to: this run's own, or a newer one of
/// the program's made by a run booted as this one (its arguments, screen
/// and depth: the same machine), which is read from its file.
enum SavePointChoice {
    case held(HeldSavePoint)
    case file(String, HibernateRecord)
}

func latestSavePoint() -> SavePointChoice? {
    guard let ctx = hibernateContext else { return heldSavePoint.map { .held($0) } }
    let program = Registration.current?.entry.program ?? RunningProgram.canonical(ctx.app)
    let args = [ctx.app] + runArgsToKeep(ctx.args)
    let newest = HibernatedSnapshots.snapshots(of: program, registry: hibernationRegistry()).first { s in
        guard s.isSavePoint, s.unresumable == nil, let r = s.record else { return false }
        return r.args == args && r.screen == ctx.screen && r.depth == ctx.depth
    }
    if let held = heldSavePoint {
        guard let newest, let record = newest.record, newest.path != held.path, record.saved > held.record.saved else {
            return .held(held)
        }
        return .file(newest.path, record)
    }
    guard let newest, let record = newest.record else { return nil }
    return .file(newest.path, record)
}

/// The machine stopped at a safe point (machine_drain_at_safe_point): the
/// chosen save point put back over it, which is then the one held.
/// `check`: the differences between the save point and the machine put
/// back are counted and listed (--savepoint-check).
func restoreSavePoint(_ choice: SavePointChoice, check: Bool = false) -> Result<HeldSavePoint, SnapshotProblem> {
    var err = [CChar](repeating: 0, count: 512)
    let held: HeldSavePoint
    switch choice {
    case .held(let h):
        held = h
    case .file(let path, let record):
        var buf: UnsafeMutablePointer<UInt8>?
        var len = 0
        guard hb_file_load(path, &buf, &len, &err, err.count), let buf else { return .failure(SnapshotProblem(str(err))) }
        held = HeldSavePoint(path: path, record: record, buffer: buf, length: len)
    }
    var differences: Int32 = 0
    let ok = withUnsafeMutablePointer(to: &differences) { d in
        machine_savepoint_restore(held.buffer, held.length, check ? d : nil, &err, err.count)
    }
    guard ok else { return .failure(SnapshotProblem(str(err))) }
    if check { print("restore check: \(differences) difference\(differences == 1 ? "" : "s")") }
    heldSavePoint = held
    return .success(held)
}

// MARK: - Headless

/// A scripted step's save point, done when the machine stops at its safe
/// point (the run's loop, runCommand): MACHINE_SAVEPOINT_*.
nonisolated(unsafe) var pendingSavePoint: Int32?

/// Headless (the machine on this thread): a step asks for a safe point,
/// where machine_run returns.
func installHeadlessSavePoints() {
    machine_set_savepoint_handler({ action, _ in
        pendingSavePoint = action
        machine_drain_begin_now()
    }, nil)
}

/// The run stopped at the safe point a step asked for: the save point made
/// (and written) or put back.  The run goes on either way.
func headlessSavePoint(_ action: Int32, app: String, runArgs: [String], check: Bool) {
    if action == Int32(MACHINE_SAVEPOINT_SAVE) {
        let t0 = Date()
        let taken = takeSavePoint(app: app, runArgs: runArgs)
        let t1 = Date()
        switch taken.flatMap({ h in writeSavePoint(h).map { (h, $0) } }) {
        case .success(let (held, bytes)):
            heldSavePoint = held
            let heldMs = Int(t1.timeIntervalSince(t0) * 1000), allMs = Int(Date().timeIntervalSince(t0) * 1000)
            print("save point: \(held.path) (\(bytes) bytes, the machine held \(heldMs) ms, written in \(allMs) ms; "
                  + "stopped at \(held.record.drainedAt) after \(held.record.instructions) instructions)")
            enterSavePoint(app: app, title: held.record.title, snapshot: held.path)
        case .failure(let p):
            print("save point refused: \(p.message)")
        }
        return
    }
    guard let choice = latestSavePoint() else { return print("restore refused: no save point") }
    let from = machine_instruction_count()
    let t0 = Date()
    switch restoreSavePoint(choice, check: check) {
    case .success(let held):
        print("restored: \(held.path) at instruction \(machine_instruction_count()) (from \(from); "
              + "\(Int(Date().timeIntervalSince(t0) * 1000)) ms)")
    case .failure(let p):
        print("restore refused: \(p.message)")
    }
}

// MARK: - Interactive

#if canImport(AppKit)
@MainActor final class SavePointer {
    static let shared = SavePointer()

    enum Work { case saving, restoring }
    /// From the key or item until the program goes on again.
    private(set) var busy: Work?
    private var timer: Timer?
    /// The key whose key-down was a save point's: its key-up is the host's too.
    var keyCode: UInt16?

    /// Whether Make Save Point can be chosen now.
    var saving: HibernateAvailability {
        if hibernateContext == nil { return .unavailable("This run cannot make save points.") }
        if let busy { return .unavailable(busy == .saving ? "Saving…" : "Going back…") }
        if Suspender.shared.hibernating { return .unavailable("Hibernating…") }
        let s = Suspender.shared
        if s.suspended && machine_pause_parked() && !machine_hibernatable() {
            let why = String(cString: machine_drain_reason())
            return .unavailable("It cannot be saved now: \(why.isEmpty ? "it stopped where it cannot be saved" : why).")
        }
        return .available
    }

    /// Whether Restore Save Point can be chosen now.
    var restoring: HibernateAvailability {
        if hibernateContext == nil { return .unavailable("This run cannot make save points.") }
        if let busy { return .unavailable(busy == .saving ? "Saving…" : "Going back…") }
        if Suspender.shared.hibernating { return .unavailable("Hibernating…") }
        if heldSavePoint == nil && latestSavePoint() == nil {
            return .unavailable("No save point yet: Make Save Point (Control-Option-Command-S) makes one.")
        }
        return .available
    }

    var items: HostQuitItems.SavePointItems {
        HostQuitItems.SavePointItems(save: #selector(Suspender.makeSavePoint(_:)),
                                     restore: #selector(Suspender.restoreSavePoint(_:)), saving: saving,
                                     restoring: restoring)
    }

    /// The menus' items as things are now.
    func refresh() { Suspender.shared.refreshHibernate() }

    /// The scripted steps of an interactive run (--keys savepoint, restore)
    /// come here, on the main thread.
    static func installScripted() {
        machine_set_savepoint_handler({ action, _ in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    let me = SavePointer.shared
                    if action == Int32(MACHINE_SAVEPOINT_SAVE) { me.save() } else { me.restore() }
                }
            }
        }, nil)
    }

    // ---- Make Save Point

    func save() {
        guard busy == nil, !Suspender.shared.hibernating, !machine_halted() else { return }
        guard let ctx = hibernateContext else { return SavePointToast.show("This run cannot make save points") }
        // The program waiting in an alert or panel of the host's: the drain
        // would close it (it is shown again after), which a save point
        // must not do to a program that goes on.
        if NSApp.modalWindow != nil { return SavePointToast.show("No save point: close the dialog first", failed: true) }
        busy = .saving
        refresh()
        // A drag or a menu held: the drain would let go of the button for
        // the program, so the save point waits for the hand to.
        whenMouseUp {
            let wasSuspended = Suspender.shared.suspended
            if !wasSuspended { machine_pause() } // the clock stands still from here
            self.whenParked { parked in
                guard parked && machine_hibernatable() else {
                    var why = String(cString: machine_drain_reason())
                    if why.isEmpty {
                        why = parked ? "it stopped where it cannot be saved" : "it did not stop: it is waiting for the host"
                    }
                    self.goOn(wasSuspended)
                    return self.finished("No save point: \(why)", failed: true)
                }
                DispatchQueue.global(qos: .userInitiated).async {
                    let taken = takeSavePoint(app: ctx.app, runArgs: ctx.args)
                    DispatchQueue.main.async {
                        MainActor.assumeIsolated {
                            // Taken: the program goes on while it is written.
                            self.goOn(wasSuspended)
                            switch taken {
                            case .failure(let p): self.finished("No save point: \(p.message)", failed: true)
                            case .success(let held): self.write(held, title: ctx.title, app: ctx.app)
                            }
                        }
                    }
                }
            }
        }
    }

    private func write(_ held: HeldSavePoint, title: String, app: String) {
        DispatchQueue.global(qos: .utility).async {
            let written = writeSavePoint(held)
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    switch written {
                    case .success(let bytes):
                        heldSavePoint = held
                        print("save point: \(held.path) (\(bytes) bytes; stopped at \(held.record.drainedAt))")
                        enterSavePoint(app: app, title: title, snapshot: held.path)
                        self.finished("Saved")
                    case .failure(let p):
                        self.finished("No save point: \(p.message)", failed: true)
                    }
                }
            }
        }
    }

    // ---- Restore Save Point

    func restore() {
        guard busy == nil, !Suspender.shared.hibernating, !machine_halted(), hibernateContext != nil else { return }
        guard let choice = latestSavePoint() else { return SavePointToast.show("No save point to go back to", failed: true) }
        busy = .restoring
        refresh()
        // Suspended: it goes on from the save point.
        if Suspender.shared.suspended { Suspender.shared.resume() }
        machine_pause()
        // An alert or panel of the host's the program waits in is closed:
        // what it was asking is the timeline left behind.
        Suspender.shared.closeProgramModal()
        whenParked { parked in
            guard parked && machine_drain_at_safe_point() else {
                let why = String(cString: machine_drain_reason())
                self.goOn(false)
                return self.finished("Could not go back: " + (why.isEmpty ? "it did not stop where it can be" : why),
                                     failed: true)
            }
            // The host's own players first: what they hold is the timeline
            // left behind (the save point's queued sound is given again).
            ClassicAudio.shared.stopAll()
            // On this thread: the host reads nothing of the machine while
            // it is replaced.
            let result = restoreSavePoint(choice)
            self.goOn(false)
            switch result {
            case .success(let held):
                print("restored: \(held.path) at instruction \(machine_instruction_count())")
                self.finished("Restored")
            case .failure(let p):
                self.finished("Could not go back: \(p.message)", failed: true)
            }
        }
    }

    // ---- Helpers

    /// The program goes on (unless the user suspended it meanwhile, or it
    /// was suspended before).
    private func goOn(_ wasSuspended: Bool) {
        if !wasSuspended && !Suspender.shared.suspended { machine_unpause() }
    }

    private func finished(_ message: String, failed: Bool = false) {
        busy = nil
        refresh()
        if failed { eprint("bluebox: \(message)\n") }
        SavePointToast.show(message, failed: failed)
    }

    private func whenMouseUp(_ then: @escaping @MainActor () -> Void) {
        guard NSEvent.pressedMouseButtons != 0 else { return then() }
        waiting = { _ in then() }
        startTimer(0.03) { NSEvent.pressedMouseButtons == 0 ? true : nil }
    }

    /// Once the machine has stopped (at a safe point or not), or six
    /// seconds on (the drain gives up after two, and the machine then stops
    /// where it is; one in a wait of the host's never stops).
    private func whenParked(_ then: @escaping @MainActor (Bool) -> Void) {
        if machine_pause_parked() { return then(true) }
        let started = Date()
        waiting = then
        startTimer(0.01) {
            let parked = machine_pause_parked()
            return parked || Date().timeIntervalSince(started) > 6 ? parked : nil
        }
    }

    /// What a timer waits to call, with what `poll` answered once it
    /// answered at all.
    private var waiting: (@MainActor (Bool) -> Void)?

    private func startTimer(_ interval: TimeInterval, _ poll: @escaping @MainActor () -> Bool?) {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { _ in
            MainActor.assumeIsolated {
                let me = SavePointer.shared
                guard let answer = poll() else { return }
                me.timer?.invalidate()
                me.timer = nil
                let then = me.waiting
                me.waiting = nil
                then?(answer)
            }
        }
    }
}

/// "Saved", "Restored" or why not, for a moment over the program's
/// windows: a small panel that takes no clicks and fades.
@MainActor enum SavePointToast {
    private static var panel: NSPanel?
    private static var hide: DispatchWorkItem?

    static func show(_ text: String, failed: Bool = false) {
        hide?.cancel()
        panel?.orderOut(nil)
        let label = NSTextField(labelWithString: text)
        label.font = .systemFont(ofSize: failed ? 13 : 15, weight: .semibold)
        label.textColor = failed ? .systemOrange : .labelColor
        label.lineBreakMode = .byWordWrapping
        label.preferredMaxLayoutWidth = 420
        label.maximumNumberOfLines = 3
        let effect = NSVisualEffectView()
        effect.material = .hudWindow
        effect.state = .active
        effect.wantsLayer = true
        effect.layer?.cornerRadius = 10
        label.translatesAutoresizingMaskIntoConstraints = false
        effect.addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: effect.leadingAnchor, constant: 18),
            label.trailingAnchor.constraint(equalTo: effect.trailingAnchor, constant: -18),
            label.topAnchor.constraint(equalTo: effect.topAnchor, constant: 10),
            label.bottomAnchor.constraint(equalTo: effect.bottomAnchor, constant: -10),
        ])
        let size = effect.fittingSize
        let p = NSPanel(contentRect: NSRect(origin: .zero, size: size), styleMask: [.borderless, .nonactivatingPanel],
                        backing: .buffered, defer: false)
        p.isReleasedWhenClosed = false
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = true
        p.ignoresMouseEvents = true
        p.level = .statusBar // over the program's windows, a floating one's too
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        p.contentView = effect
        // Near the top of the program's window (or the screen it is on).
        let over = NSApp.keyWindow ?? NSApp.mainWindow
        let area = over.map { $0.frame } ?? NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 800, height: 600)
        p.setFrameOrigin(NSPoint(x: area.midX - size.width / 2, y: area.maxY - size.height - 48))
        p.alphaValue = 1
        p.orderFrontRegardless()
        panel = p
        let work = DispatchWorkItem {
            MainActor.assumeIsolated {
                NSAnimationContext.runAnimationGroup({ ctx in
                    ctx.duration = 0.3
                    p.animator().alphaValue = 0
                }, completionHandler: {
                    MainActor.assumeIsolated {
                        p.orderOut(nil)
                        if SavePointToast.panel === p { SavePointToast.panel = nil }
                    }
                })
            }
        }
        hide = work
        DispatchQueue.main.asyncAfter(deadline: .now() + (failed ? 3 : 1.2), execute: work)
    }
}
#endif
