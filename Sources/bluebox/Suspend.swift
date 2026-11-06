// macOS host: suspending the program and resuming it.
//
// Suspend (in the application menu, in a background program's status item
// menu, or asked by the shell) holds the machine still: no instruction runs, and its
// clock, sound and voices stand still until Resume, so the program finds
// no jump when it goes on.  Meanwhile its windows are greyed and inert,
// with Resume, Hibernate and Hide over each.  Its entry in the registry of
// running programs says so, and the shell hears of each change.
//
// Hibernate (beside Suspend in the application menu and the status item's
// menu, and on the overlay) suspends the program if it is running, waits
// for its machine to stop at a safe point (drain.c), writes the snapshot
// (Hibernate.swift), enters it in the registry as hibernated and quits the
// process; opening the program again resumes it.  An alert or a file panel
// of the host's that the program is waiting in is closed for the drain, and
// shown again when the program goes on.  A program that cannot be saved
// (one with a connection open, say) is told so in an alert: one that was
// running goes on, one that was suspended stays so.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Toolbox

@MainActor final class Suspender: NSObject {
    static let shared = Suspender()

    private(set) var suspended = false
    /// The host's part: the overlays put up or taken away, the menus
    /// retitled.
    var changed: ((Bool) -> Void)?
    private var listening = false
    /// Hibernating: from choosing it until the process ends (or it fails).
    private(set) var hibernating = false
    /// Hibernate was chosen while the program ran: it goes on if that fails.
    private var resumeIfRefused = false
    /// Polling for the machine to stop at its safe point.
    private var parkTimer: Timer?

    /// Whether Hibernate can be chosen now.
    var hibernation: HibernateAvailability {
        hibernateContext == nil ? .unavailable("This run cannot be hibernated.")
            : .current(suspended: suspended, hibernating: hibernating)
    }

    /// Menu items act while an alert or panel of the program's is up (to
    /// suspend or hibernate the program waiting in it).
    @objc var worksWhenModal: Bool { true }

    /// Hear the shell: Suspend or Resume for this process.
    func listen() {
        guard !listening else { return }
        listening = true
        let center = DistributedNotificationCenter.default()
        center.addObserver(forName: SuspendNotice.request, object: String(getpid()), queue: .main) { note in
            let on = note.userInfo?["suspend"] as? Bool
            MainActor.assumeIsolated {
                guard let on else { return }
                if on { Suspender.shared.suspend() } else { Suspender.shared.resume() }
            }
        }
    }

    func suspend() {
        guard !suspended, !machine_halted() else { return }
        machine_pause() // the clock stops now; the machine at its next pause point
        suspended = true
        closeProgramModal()
        watchForPark()
        ClassicAudio.shared.setSuspended(true)
        ClassicMusic.shared.setSuspended(true)
        ClassicSpeech.shared.setSuspended(true)
        changed?(true)
        Registration.current?.setState(.suspended) // the registry first: the shell reads it on hearing
        SuspendNotice.tell(suspended: true)
    }

    func resume() {
        guard suspended, !hibernating else { return }
        suspended = false
        parkTimer?.invalidate()
        parkTimer = nil
        changed?(false)
        ClassicSpeech.shared.setSuspended(false)
        ClassicMusic.shared.setSuspended(false)
        ClassicAudio.shared.setSuspended(false)
        machine_unpause()
        Registration.current?.setState(.running)
        SuspendNotice.tell(suspended: false)
    }

    func toggle() { if suspended { resume() } else { suspend() } }

    /// An alert or file panel of the host's that the program waits in
    /// (machine thread inside its handler) closes for the drain: the call
    /// is abandoned and made again after the program goes on (dialog.c,
    /// stdfile.c).  A call a routine of the program's own made is left: it
    /// would take the closing as its answer.
    private func closeProgramModal() {
        guard NSApp.modalWindow != nil else { return }
        // The drain starts on the machine's thread; give it a moment.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            MainActor.assumeIsolated {
                if NSApp.modalWindow != nil && machine_drain_abandonable() { NSApp.abortModal() }
            }
        }
    }

    /// Suspended: once the machine has stopped (at a safe point or not),
    /// Hibernate's state is shown again.
    private func watchForPark() {
        parkTimer?.invalidate()
        refreshHibernate()
        parkTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
            MainActor.assumeIsolated {
                let me = Suspender.shared
                guard !me.suspended || machine_pause_parked() else { return }
                me.parkTimer?.invalidate()
                me.parkTimer = nil
                me.refreshHibernate()
            }
        }
    }

    /// Hibernate's items and buttons enabled or not, with why not.
    func refreshHibernate() {
        let state = hibernation
        HostQuitItems.update(NSApp.mainMenu, hibernate: #selector(hibernateApp(_:)), state)
        for w in NSApp.windows {
            HostQuitItems.update(w.menu, hibernate: #selector(hibernateApp(_:)), state)
            w.contentView?.subviews.compactMap { $0 as? SuspendedOverlay }.forEach { $0.show(state) }
        }
        hibernateChanged?(state)
    }

    /// The host's own menus that hold Hibernate (a status item's).
    var hibernateChanged: ((HibernateAvailability) -> Void)?

    /// Hibernate: suspended first if running, then saved at the safe point
    /// and the process ended.
    func hibernate() {
        guard !hibernating, !machine_halted() else { return }
        if hibernateContext == nil { return refused("this run cannot be hibernated") }
        if suspended && machine_pause_parked() && !machine_hibernatable() {
            // Stopped already, where it cannot be saved (or holding what a
            // snapshot cannot carry).
            let why = String(cString: machine_drain_reason())
            return refused(why.isEmpty ? "it stopped where it cannot be saved; resume it and try again" : why)
        }
        resumeIfRefused = !suspended
        hibernating = true
        if !suspended { suspend() }
        refreshHibernate()
        let started = Date()
        HibernateProgress.shared.schedule(title: hibernateContext?.title ?? "")
        parkTimer?.invalidate()
        parkTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { _ in
            MainActor.assumeIsolated {
                let me = Suspender.shared
                // The drain gives up after two seconds and the machine stops
                // where it is; one in a wait of the host's never stops.
                let waited = Date().timeIntervalSince(started)
                guard machine_pause_parked() || waited > 6 else { return }
                me.parkTimer?.invalidate()
                me.parkTimer = nil
                if machine_hibernatable() {
                    me.save()
                } else {
                    let why = String(cString: machine_drain_reason())
                    me.failed(why.isEmpty ? (machine_pause_parked() ? "it stopped where it cannot be saved"
                                                                     : "it did not stop: it is waiting for the host")
                                          : why)
                }
            }
        }
    }

    /// The snapshot written off the main thread (the machine stands still
    /// meanwhile), then the registry told and the process ended.
    private func save() {
        guard let ctx = hibernateContext else { return failed("this run cannot be hibernated") }
        DispatchQueue.global(qos: .userInitiated).async {
            let result = writeSnapshot(app: ctx.app, runArgs: ctx.args, to: nil)
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    switch result {
                    case .success(let saved):
                        print("hibernated: \(saved.path) (\(saved.bytes) bytes in \(saved.ms) ms; stopped at "
                              + "\(saved.record.drainedAt) after \(saved.record.instructions) instructions)")
                        enterHibernated(app: ctx.app, title: ctx.title, snapshot: saved.path)
                        exit(0)
                    case .failure(let why):
                        Suspender.shared.failed(why.message)
                    }
                }
            }
        }
    }

    private func failed(_ why: String) {
        hibernating = false
        HibernateProgress.shared.close()
        let resumes = resumeIfRefused
        resumeIfRefused = false
        refused(why, goesOn: resumes)
        if resumes { resume() } else { refreshHibernate() }
    }

    /// Why it could not be hibernated, in an alert.
    private func refused(_ why: String, goesOn: Bool = false) {
        let title = hibernateContext?.title ?? "The program"
        let alert = NSAlert()
        if let appIcon { alert.icon = appIcon }
        alert.messageText = "\(title) could not be hibernated."
        var text = why.prefix(1).uppercased() + why.dropFirst()
        if !text.hasSuffix(".") && !text.hasSuffix("…") { text += "." }
        alert.informativeText = text + (goesOn ? " It goes on running." : suspended ? " It stays suspended." : "")
        alert.alertStyle = .warning
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    /// Quit asks the program to quit: a suspended one is resumed first, so
    /// that it can (and save what it has to).
    func quit() {
        resume()
        machine_request_quit()
    }

    /// Act on a key the host keeps; false if it is the program's.
    func handle(_ key: HostKey) -> Bool {
        switch key {
        case .classic: return false
        case .forceQuit: NSApp.terminate(nil)
        case .quit: quit()
        case .hide: NSApp.hide(nil)
        case .hideOthers: NSApp.hideOtherApplications(nil)
        case .ignore: break
        }
        return true
    }

    @objc func toggleSuspend(_ sender: Any?) { toggle() }
    @objc func hibernateApp(_ sender: Any?) { hibernate() }
    @objc func quitApp(_ sender: Any?) { quit() }
    @objc func forceQuit(_ sender: Any?) { NSApp.terminate(nil) }

    /// The application menu's Hide items, then Suspend, Quit and their
    /// alternates.
    func applicationMenuItems(title: String) -> [NSMenuItem] {
        HostHideItems.items(title: title) + menuItems(title: title)
    }

    /// Suspend, Hibernate, Quit and their alternates.
    func menuItems(title: String, quitKeys: Bool = true) -> [NSMenuItem] {
        HostQuitItems.items(title: title, suspended: suspended, target: self, quit: #selector(quitApp(_:)),
                            suspend: #selector(toggleSuspend(_:)), forceQuit: #selector(forceQuit(_:)),
                            hibernate: hibernateContext == nil ? nil : #selector(hibernateApp(_:)),
                            hibernation: hibernation, quitKeys: quitKeys)
    }

    /// Suspend items say Resume while suspended, and Suspend again after.
    func retitle(_ menu: NSMenu?, title: String) {
        guard let menu else { return }
        for item in menu.items {
            if item.action == #selector(toggleSuspend(_:)) {
                item.title = "\(suspended ? "Resume" : "Suspend") \(title)"
            }
            if let sub = item.submenu { retitle(sub, title: title) }
        }
        HostQuitItems.update(menu, hibernate: #selector(hibernateApp(_:)), hibernation)
    }

    /// A menu holding Hibernate opened: its state as it is now.
    func menuOpening(_ menu: NSMenu) {
        HostQuitItems.update(menu, hibernate: #selector(hibernateApp(_:)), hibernation)
    }

    /// An overlay for a window's content view (its whole area).
    func overlay(for view: NSView) -> SuspendedOverlay {
        let overlay = SuspendedOverlay(frame: view.bounds,
                                       resume: { MainActor.assumeIsolated { Suspender.shared.resume() } },
                                       hibernate: hibernateContext == nil ? nil : {
                                           MainActor.assumeIsolated { Suspender.shared.hibernate() }
                                       },
                                       hide: { NSApp.hide(nil) })
        overlay.show(hibernation)
        return overlay
    }
}

extension RootlessHost {
    /// Suspended: every window greyed and inert with Resume and Hide over
    /// it (the whole screen's too), the mouse let go and the menu bar and
    /// Dock back, the drawing over the desktop dimmed and taking no clicks.
    func showSuspended(_ on: Bool) {
        if on {
            setCaptured(false)
            setChromeHidden(false)
        }
        for w in Array(windows.values) + [screenWindow].compactMap({ $0 }) {
            w.contentView?.showSuspended(on)
            w.isMovable = !on
        }
        overlayWindow?.ignoresMouseEvents = on
        overlayWindow?.alphaValue = on ? 0.5 : 1
        Suspender.shared.retitle(NSApp.mainMenu, title: title)
        Suspender.shared.retitle(statusItem?.menu, title: title)
        if !on {
            windowsVersion = .max // anything that changed just before
            menusVersion = .max
        }
    }
}

extension NSView {
    /// Put a suspended program's overlay over this view, or take it away.
    @MainActor func showSuspended(_ on: Bool) {
        let existing = subviews.compactMap { $0 as? SuspendedOverlay }
        if on {
            guard existing.isEmpty else { return }
            let overlay = Suspender.shared.overlay(for: self)
            addSubview(overlay)
            window?.invalidateCursorRects(for: self)
        } else {
            existing.forEach { $0.removeFromSuperview() }
            window?.invalidateCursorRects(for: self)
        }
    }
}

/// Hibernating's progress: a small panel, shown only if it takes longer
/// than a moment.
@MainActor final class HibernateProgress {
    static let shared = HibernateProgress()
    private var panel: NSPanel?
    private var pending: DispatchWorkItem?

    func schedule(title: String) {
        close()
        let work = DispatchWorkItem { MainActor.assumeIsolated { HibernateProgress.shared.show(title: title) } }
        pending = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    private func show(title: String) {
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 300, height: 72), styleMask: [.titled, .hudWindow, .utilityWindow],
                        backing: .buffered, defer: false)
        p.title = "Hibernating"
        p.isReleasedWhenClosed = false
        p.level = .floating
        let label = NSTextField(labelWithString: "Hibernating \(title)…")
        let bar = NSProgressIndicator()
        bar.style = .bar
        bar.isIndeterminate = true
        bar.startAnimation(nil)
        let stack = NSStackView(views: [label, bar])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.edgeInsets = NSEdgeInsets(top: 12, left: 16, bottom: 12, right: 16)
        stack.translatesAutoresizingMaskIntoConstraints = false
        p.contentView = NSView()
        p.contentView?.addSubview(stack)
        if let c = p.contentView {
            NSLayoutConstraint.activate([
                stack.leadingAnchor.constraint(equalTo: c.leadingAnchor),
                stack.trailingAnchor.constraint(equalTo: c.trailingAnchor),
                stack.topAnchor.constraint(equalTo: c.topAnchor),
                stack.bottomAnchor.constraint(equalTo: c.bottomAnchor),
                bar.widthAnchor.constraint(equalToConstant: 268),
            ])
        }
        p.center()
        p.orderFrontRegardless()
        panel = p
    }

    func close() {
        pending?.cancel()
        pending = nil
        panel?.orderOut(nil)
        panel = nil
    }
}
#endif
