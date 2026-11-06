#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Foundation
import Testing

extension BlueBox {
    /// Suspend and Resume in a program's menus and keys, in the shell's
    /// view of its programs, and the overlay over a suspended program's
    /// windows.
    @Suite struct SuspendMenus {
        final class Target: NSObject {
            @objc func quit(_ sender: Any?) {}
            @objc func suspend(_ sender: Any?) {}
            @objc func force(_ sender: Any?) {}
            @objc func resume(_ sender: Any?) {}
        }

        /// Menu items hold their targets weakly.
        let target = Target()

        @MainActor func items(suspended: Bool, quitKeys: Bool = true) -> [NSMenuItem] {
            HostQuitItems.items(title: "TeachText", suspended: suspended, target: target,
                                quit: #selector(Target.quit(_:)), suspend: #selector(Target.suspend(_:)),
                                forceQuit: #selector(Target.force(_:)), quitKeys: quitKeys)
        }

        /// Suspend, then Quit (Command-Q) with Force Quit (Command-Option-Q)
        /// as its alternate, after it, as AppKit needs.
        @Test @MainActor func applicationMenuEnds() {
            let m = items(suspended: false)
            #expect(m.map(\.title) == ["Suspend TeachText", "", "Quit TeachText", "Force Quit TeachText"])
            #expect(m[0].keyEquivalent == "" && m[0].action == #selector(Target.suspend(_:)))
            #expect(m[1].isSeparatorItem)
            #expect(m[2].keyEquivalent == "q" && m[2].keyEquivalentModifierMask == [.command] && !m[2].isAlternate)
            #expect(m[2].action == #selector(Target.quit(_:)))
            #expect(m[3].keyEquivalent == "q" && m[3].keyEquivalentModifierMask == [.command, .option] && m[3].isAlternate)
            #expect(m[3].action == #selector(Target.force(_:)))
            #expect(m.allSatisfy { $0.isSeparatorItem || $0.target === target })
            // The masks differ, so no two items answer the same keys.
            let masks = m.filter { !$0.keyEquivalent.isEmpty }.map(\.keyEquivalentModifierMask.rawValue)
            #expect(Set(masks).count == masks.count)
        }

        @Test @MainActor func suspendedSaysResume() {
            let m = items(suspended: true)
            #expect(m[0].title == "Resume TeachText" && m[3].title == "Force Quit TeachText")
            #expect(m[2].title == "Quit TeachText")
        }

        /// Hide, Hide Others (Command-Option-H) and Show All, sent to the
        /// application, then a separator before Suspend and Quit.
        @Test @MainActor func hideItems() {
            let m = HostHideItems.items(title: "Replace")
            #expect(m.map(\.title) == ["Hide Replace", "Hide Others", "Show All", ""])
            #expect(m[0].keyEquivalent == "h" && m[0].keyEquivalentModifierMask == [.command])
            #expect(m[0].action == #selector(NSApplication.hide(_:)))
            #expect(m[1].keyEquivalent == "h" && m[1].keyEquivalentModifierMask == [.command, .option])
            #expect(m[1].action == #selector(NSApplication.hideOtherApplications(_:)))
            #expect(m[2].keyEquivalent == "" && m[2].action == #selector(NSApplication.unhideAllApplications(_:)))
            #expect(m[3].isSeparatorItem)
            #expect(m.allSatisfy { $0.target == nil && !$0.isAlternate }) // up the responder chain to NSApp
            // Typed, Command-H is the program's; suspended, the host's.
            #expect(HostKey.classify([.command], "h", suspended: false) == .classic)
            #expect(HostKey.classify([.command, .option], "h", suspended: false) == .classic)
            #expect(HostKey.classify([.command], "h", suspended: true) == .hide)
            #expect(HostKey.classify([.command, .option], "h", suspended: true) == .hideOthers)
        }

        /// A background program's status item menu: Suspend and Quit, no keys.
        @Test @MainActor func statusItemHasNoKeys() {
            let m = items(suspended: false, quitKeys: false)
            #expect(m.map(\.title) == ["Suspend TeachText", "Quit TeachText"])
            #expect(m.allSatisfy { $0.keyEquivalent.isEmpty && !$0.isAlternate })
        }

        /// Command-Option-Q force quits,
        /// other keys go to the program; suspended, only those and
        /// Command-Q (quitting it) do anything.
        @Test func hostKeys() {
            #expect(HostKey.classify([.command, .option], "q", suspended: false) == .forceQuit)
            #expect(HostKey.classify([.command, .option], "Q", suspended: true) == .forceQuit)
            #expect(HostKey.classify([.command, .option, .control], "q", suspended: false) == .classic)
            #expect(HostKey.classify([.command], "q", suspended: false) == .classic) // the program's Command-Q
            #expect(HostKey.classify([.command], "q", suspended: true) == .quit)
            #expect(HostKey.classify([.command, .shift], "q", suspended: false) == .classic)
            #expect(HostKey.classify([.command, .option, .capsLock, .numericPad], "q", suspended: false) == .forceQuit)
            #expect(HostKey.classify([], "a", suspended: false) == .classic)
            #expect(HostKey.classify([], "a", suspended: true) == .ignore)
            #expect(HostKey.classify([.command], "s", suspended: true) == .ignore)
        }

        /// The shell's programs: Launch, Suspend or Resume where Launch is;
        /// a program's word on itself; and one gone however it went.
        @Test func shellStates() {
            var s = ProgramStates()
            let a = "/lib/Games/Apeiron", b = "/lib/Bolo", c = "/lib/Apps/apple"
            #expect(s.action(for: a) == .launch)
            s.launched(a, pid: 100)
            s.launched(b, pid: 200)
            s.launched(c, pid: 300)
            #expect(s.action(for: a) == .suspend && s.action(for: a).title == "Suspend")
            s.noted(pid: 100, suspended: true)
            s.noted(pid: 300, suspended: true)
            s.noted(pid: 999, suspended: true) // not one of the shell's
            #expect(s.action(for: a) == .resume && s.action(for: a).title == "Resume")
            #expect(s.suspendedPaths == [a, c])     // by name: Apeiron, apple
            #expect(s.runningPaths == [b])
            s.ended(a)                              // crashed while suspended
            #expect(s.action(for: a) == .launch && !s.isSuspended(a))
            #expect(s.suspendedPaths == [c])
            s.noted(pid: 300, suspended: false)
            #expect(s.suspendedPaths.isEmpty && s.runningPaths == [c, b])
            s.launched(c, pid: 301)                 // launched again: running, not suspended
            s.noted(pid: 300, suspended: true)      // the old process's last word
            #expect(!s.isSuspended(c))
        }

        @Test func stateNotices() {
            let note = Notification(name: SuspendNotice.state, object: "123", userInfo: ["suspended": true])
            #expect(SuspendNotice.parse(note)?.pid == 123 && SuspendNotice.parse(note)?.suspended == true)
            #expect(SuspendNotice.parse(Notification(name: SuspendNotice.state, object: "x", userInfo: nil)) == nil)
        }

        /// A request reaches the process it names (and only it), and a
        /// program's word on itself reaches the shell.
        @Test func noticesCrossProcesses() {
            final class Heard: @unchecked Sendable { var requests: [Bool] = [], states: [Bool] = [] }
            let heard = Heard(), done = DispatchSemaphore(value: 0)
            // On a thread of its own, with its run loop, as the main
            // thread's is the test runner's.
            Thread.detachNewThread {
                let center = DistributedNotificationCenter.default()
                let a = center.addObserver(forName: SuspendNotice.request, object: String(getpid()), queue: nil) { note in
                    if let on = note.userInfo?["suspend"] as? Bool { heard.requests.append(on) }
                }
                let b = center.addObserver(forName: SuspendNotice.state, object: nil, queue: nil) { note in
                    if let (pid, on) = SuspendNotice.parse(note), pid == getpid() { heard.states.append(on) }
                }
                SuspendNotice.ask(pid: getpid() + 100_000, suspend: true) // someone else's
                SuspendNotice.ask(pid: getpid(), suspend: true)
                SuspendNotice.tell(suspended: true)
                let until = Date().addingTimeInterval(3)
                while (heard.requests.isEmpty || heard.states.isEmpty) && Date() < until {
                    RunLoop.current.run(until: Date().addingTimeInterval(0.05))
                }
                center.removeObserver(a)
                center.removeObserver(b)
                done.signal()
            }
            done.wait()
            #expect(heard.requests == [true])
            #expect(heard.states == [true])
        }

        /// The status item's section of suspended programs, each resuming it.
        @Test @MainActor func suspendedSection() {
            #expect(SuspendedMenuSection.items([], target: Target(), resume: #selector(Target.resume(_:))).isEmpty)
            let m = SuspendedMenuSection.items(["/lib/Apeiron", "/lib/Bolo"], target: Target(),
                                               resume: #selector(Target.resume(_:)))
            #expect(m.map(\.title) == ["Suspended", "Apeiron", "Bolo", ""])
            #expect(!m[0].isEnabled && m[3].isSeparatorItem)
            #expect(m[1].representedObject as? String == "/lib/Apeiron" && m[1].action == #selector(Target.resume(_:)))
        }

        /// The overlay greys the whole window, takes its clicks, and has
        /// Resume over Hide when there is room.
        @Test @MainActor func overlayButtons() {
            final class Count { var resumed = 0, hidden = 0 }
            let count = Count()
            let view = NSView(frame: NSRect(x: 0, y: 0, width: 400, height: 300))
            let overlay = SuspendedOverlay(frame: view.bounds, resume: { count.resumed += 1 }, hide: { count.hidden += 1 })
            view.addSubview(overlay)
            view.layoutSubtreeIfNeeded()
            #expect(overlay.resumeButton.title == "Resume" && overlay.hideButton.title == "Hide")
            #expect(overlay.hitTest(NSPoint(x: 5, y: 5)) === overlay)
            overlay.resumeButton.performClick(nil)
            overlay.hideButton.performClick(nil)
            #expect(count.resumed == 1 && count.hidden == 1)
            #expect(overlay.resumeButton.frame.minY != overlay.hideButton.frame.minY)
            #expect(overlay.resumeButton.superview?.superview?.isHidden == false)
            // A palette too small for them is only greyed.
            let small = SuspendedOverlay(frame: NSRect(x: 0, y: 0, width: 40, height: 30), resume: {}, hide: {})
            small.layoutSubtreeIfNeeded()
            #expect(small.resumeButton.superview?.superview?.isHidden == true)
        }
    }
}
#endif
