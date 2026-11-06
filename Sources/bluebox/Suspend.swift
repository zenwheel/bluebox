// macOS host: suspending the program and resuming it.
//
// Suspend (in the application menu, in a background program's status item
// menu, or asked by the shell) holds the machine still: no instruction runs, and its
// clock, sound and voices stand still until Resume, so the program finds
// no jump when it goes on.  Meanwhile its windows are greyed and inert,
// with Resume and Hide over each.  The shell hears of each change.

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
        ClassicAudio.shared.setSuspended(true)
        ClassicMusic.shared.setSuspended(true)
        ClassicSpeech.shared.setSuspended(true)
        changed?(true)
        SuspendNotice.tell(suspended: true)
    }

    func resume() {
        guard suspended else { return }
        suspended = false
        changed?(false)
        ClassicSpeech.shared.setSuspended(false)
        ClassicMusic.shared.setSuspended(false)
        ClassicAudio.shared.setSuspended(false)
        machine_unpause()
        SuspendNotice.tell(suspended: false)
    }

    func toggle() { if suspended { resume() } else { suspend() } }

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
    @objc func quitApp(_ sender: Any?) { quit() }
    @objc func forceQuit(_ sender: Any?) { NSApp.terminate(nil) }

    /// The application menu's Hide items, then Suspend, Quit and their
    /// alternates.
    func applicationMenuItems(title: String) -> [NSMenuItem] {
        HostHideItems.items(title: title) + menuItems(title: title)
    }

    /// Suspend, Quit and their alternates.
    func menuItems(title: String, quitKeys: Bool = true) -> [NSMenuItem] {
        HostQuitItems.items(title: title, suspended: suspended, target: self, quit: #selector(quitApp(_:)),
                            suspend: #selector(toggleSuspend(_:)), forceQuit: #selector(forceQuit(_:)),
                            quitKeys: quitKeys)
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
    }

    /// An overlay for a window's content view (its whole area).
    func overlay(for view: NSView) -> SuspendedOverlay {
        SuspendedOverlay(frame: view.bounds,
                         resume: { MainActor.assumeIsolated { Suspender.shared.resume() } },
                         hide: { NSApp.hide(nil) })
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
#endif
