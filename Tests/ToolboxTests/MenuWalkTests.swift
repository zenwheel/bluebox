import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// The menu walker (--walk-menus) with synthesized programs that have
    /// no menu bar of their own.
    @Suite(.serialized) struct MenuWalk {
        /// A one-segment application of the given code, launched.
        func launch(_ code: [UInt8]) throws -> URL {
            bootMachine()
            let jt = cat(be16(0), be16(0x3F3C), be16(1), be16(0xA9F0)) // entry 0: seg 1, offset 0
            let code0 = cat(be32(40), be32(256), be32(8), be32(32), jt)
            var fork = buildFork([
                TestResource(type: "CODE", id: 0, data: code0),
                TestResource(type: "CODE", id: 1, data: cat(be16(0), be16(1), code)),
            ])
            let dir = tempDir()
            let path = dir.appendingPathComponent("NoMenus").path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            #expect(machine_launch(path, &err, err.count))
            return dir
        }

        /// A title that waits for a click by polling Button (ANSI Vision's)
        /// sees the walk's click and goes on.
        @Test func aTitlePollingButtonSeesTheClick() throws {
            let dir = try launch(cat(
                be16(0x554F),                 // loop: SUBQ #2,SP
                be16(0xA974),                 // _Button
                be16(0x4A1F),                 // TST.B (SP)+
                be16(0x67FA),                 // BEQ loop
                be16(0xA9F4)))                // _ExitToShell
            defer { try? FileManager.default.removeItem(at: dir) }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// Asleep in WaitNextEvent, a program wakes at the walk's next step
        /// (input, as a user's would be) rather than at the end of a long
        /// sleep, so the walk's steps come as often whatever it asks.
        @Test func aLongSleepEndsAtTheWalksNextStep() throws {
            let dir = try launch(cat(be16(0xA9F4))) // _ExitToShell
            defer { try? FileManager.default.removeItem(at: dir) }
            let ev = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            tb_FlushEvents(-1, 0)
            machine_set_instruction_limit(0) // no limit left by an earlier run to stop the clock
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            for _ in 0..<3 {
                let before = g_cpu!.pointee.icount
                _ = tb_WaitNextEvent(0, ev, 300, 0) // five seconds
                let slept = g_cpu!.pointee.icount - before
                #expect(slept > 2_900_000 && slept < 3_100_000) // a step: 0.3 s at 100 ns an instruction
            }
        }

        /// The wait for a headless alert's answer (its default button) is
        /// no walk step: the window behind it (a dialog the program is
        /// between ModalDialog calls in, Moose Frazer's) is not the
        /// program's to take input from then.
        @Test func anAlertsAnswerTakesNoWalkStep() throws {
            let dir = try launch(cat(be16(0xA9F4))) // _ExitToShell
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            tb_InitDialogs(0)
            _ = dialog([50, 50, 150, 250])
            tb_FlushEvents(-1, 0)
            machine_set_instruction_limit(0)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            let ev = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            machine_idle_answered()
            #expect(tb_EventAvail(8, ev) == 0) // no key for the dialog behind
            machine_idle()
            #expect(tb_EventAvail(8, ev) != 0) // the walk's Return, its first step
        }

        /// A scripted snapshot is no input: a program waiting for events
        /// (SmallTalk's one poll before it loads its Prelude) does not
        /// have the clock moved on to it, so the snapshot shows what the
        /// program drew by then; a key after it still does.
        @Test func aSnapshotDoesNotPullTheClock() throws {
            let dir = try launch(cat(be16(0xA9F4))) // _ExitToShell
            defer { try? FileManager.default.removeItem(at: dir) }
            machine_set_instruction_limit(0)
            let png = dir.appendingPathComponent("snap.png").path
            let now = g_cpu!.pointee.icount
            while events_script_next_at() != 0 { events_script_pump(events_script_next_at()) } // none left over
            machine_script_add_snap(now + 5_000_000, png)
            #expect(events_script_next_at() == 0)
            machine_idle()
            #expect(g_cpu!.pointee.icount < now + 5_000_000)
            #expect(!FileManager.default.fileExists(atPath: png))
            machine_script_add(now + 9_000_000, 0, 0x61, 0, 0) // "a"
            #expect(events_script_next_at() == now + 9_000_000)
            machine_idle()
            #expect(g_cpu!.pointee.icount >= now + 9_000_000)
            #expect(FileManager.default.fileExists(atPath: png)) // taken on the way
            tb_FlushEvents(-1, 0)
        }

        /// A program that only puts up alert after alert is still walked:
        /// after a hundred answers in a row the walk's steps come again
        /// (MacTools Deluxe's Secure).
        @Test func alertAfterAlertIsStillWalked() throws {
            let dir = try launch(cat(be16(0xA9F4))) // _ExitToShell
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            tb_InitDialogs(0)
            _ = dialog([50, 50, 150, 250])
            tb_FlushEvents(-1, 0)
            machine_set_instruction_limit(0)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            for _ in 0..<50 { machine_idle_answered() }
            #expect(walker_next_in() == 0) // a step due, not taken
            for _ in 0..<100 { machine_idle_answered() }
            #expect(walker_next_in() > 0) // taken
        }

        /// A rect in guest memory.
        func rect(_ v: [Int]) -> UInt32 {
            let r = tb_NewPtr(8, false, true)
            for (i, x) in v.enumerated() { wr16(r + UInt32(i * 2), UInt16(x)) }
            return r
        }

        /// A dialog in front with no items.
        func dialog(_ r: [Int]) -> UInt32 {
            let items = tb_NewHandle(2, false, true)
            wr16(rd32(items), 0xFFFF) // no items
            return tb_NewDialog(0, rect(r), tb_NewPtr(2, false, true), 1, 0, 0xFFFF_FFFF, 0, 0, items)
        }

        /// A program waiting in GetNextEvent for ever.
        let waiting = cat(
            be16(0x4267),                 // loop: CLR.W -(SP)
            be16(0x3F3C), be16(0xFFFF),   // MOVE.W #everyEvent,-(SP)
            be16(0x486D), be16(0xFFE0),   // PEA event(A5)
            be16(0xA970),                 // _GetNextEvent
            be16(0x544F),                 // ADDQ #2,SP
            be16(0x60F0))                 // BRA loop

        /// A modeless dialog with no items at the start, after another
        /// walked past (a splash), is walked past too, as a program's
        /// palettes or its licence are (Fusion's, Terrain Builder's): only
        /// a modal one that stays ends the walk "stuck in a dialog".
        @Test func aModelessDialogAfterASplashIsWalkedPast() throws {
            let dir = try launch(waiting)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            tb_InitDialogs(0)
            _ = dialog([50, 50, 150, 250])
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(25_000_000) != RUN_HALTED) // the splash walked past
            _ = dialog([60, 60, 200, 300])
            #expect(machine_run(1_000_000_000) == RUN_HALTED) // its screen walked too
            #expect(String(cString: machine_halt_reason()).hasPrefix("menu walk found no menu bar"))
        }

        /// A program that quits at a click on a button of its window
        /// (window(A5)), an active one.
        let untilAButton = cat(
            be16(0x4267),                 // loop: CLR.W -(SP)
            be16(0x3F3C), be16(2),        // MOVE.W #mDownMask,-(SP)
            be16(0x486D), be16(0xFFE0),   // PEA event(A5)
            be16(0xA970),                 // _GetNextEvent
            be16(0x4A1F),                 // TST.B (SP)+
            be16(0x67F0),                 // BEQ loop
            be16(0x486D), be16(0xFFEA),   // PEA event.where(A5)
            be16(0xA871),                 // _GlobalToLocal
            be16(0x554F),                 // SUBQ #2,SP
            be16(0x2F2D), be16(0xFFEA),   // MOVE.L event.where(A5),-(SP)
            be16(0x2F2D), be16(0xFFF8),   // MOVE.L window(A5),-(SP)
            be16(0x486D), be16(0xFFFC),   // PEA control(A5)
            be16(0xA96C),                 // _FindControl
            be16(0x301F),                 // MOVE.W (SP)+,D0
            be16(0x67D6),                 // BEQ loop: a click beside the button
            be16(0xA9F4))                 // _ExitToShell

        /// A Pascal string in guest memory.
        func title(_ t: String) -> UInt32 {
            let p = tb_NewPtr(32, false, true)
            wr8(p, UInt8(t.utf8.count))
            for (i, b) in t.utf8.enumerated() { wr8(p + 1 + UInt32(i), b) }
            return p
        }

        /// A program that is one window of buttons, with no menu bar
        /// (Disinfectant 1.6): the walk presses its button, which here
        /// ends the program.
        @Test func aWindowOfButtonsHasThemPressed() throws {
            let dir = try launch(untilAButton)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            let title = tb_NewPtr(8, false, true)
            for (i, b) in [3, 0x47, 0x6F, 0x21].enumerated() { wr8(title + UInt32(i), UInt8(b)) } // "Go!"
            let w = tb_NewWindow(0, rect([50, 50, 150, 250]), title, 1, 1, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            _ = tb_NewControl(w, rect([10, 10, 30, 80]), title, 1, 0, 0, 1, 0, 0)
            wr32(rd32(UInt32(LM_CurrentA5)) - 8, w)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// A program that quits at a click inside the rect at rect(A5) of
        /// its window, the current port.
        let untilAPicture = cat(
            be16(0x4267),                 // loop: CLR.W -(SP)
            be16(0x3F3C), be16(2),        // MOVE.W #mDownMask,-(SP)
            be16(0x486D), be16(0xFFE0),   // PEA event(A5)
            be16(0xA970),                 // _GetNextEvent
            be16(0x4A1F),                 // TST.B (SP)+
            be16(0x67F0),                 // BEQ loop
            be16(0x486D), be16(0xFFEA),   // PEA event.where(A5)
            be16(0xA871),                 // _GlobalToLocal
            be16(0x554F),                 // SUBQ #2,SP
            be16(0x2F2D), be16(0xFFEA),   // MOVE.L event.where(A5),-(SP)
            be16(0x486D), be16(0xFFD0),   // PEA rect(A5)
            be16(0xA8AD),                 // _PtInRect
            be16(0x4A1F),                 // TST.B (SP)+
            be16(0x67DA),                 // BEQ loop: a click beside it
            be16(0xA9F4))                 // _ExitToShell

        /// A window with no menu bar whose buttons are pictures it draws
        /// (copied from off screen, AutoEye's "Manual" and "Automatic"):
        /// the walk clicks them, not its background.
        @Test func aWindowOfPicturesHasThemClicked() throws {
            let dir = try launch(untilAPicture)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            let w = tb_NewWindow(0, rect([50, 50, 250, 450]), 0, 1, 1, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            let bits = tb_NewPtr(14, false, true)
            wr32(bits, tb_NewPtr(64 * 200, false, true))
            wr16(bits + 4, 64)
            gmemcpy(bits + 6, rect([0, 0, 200, 400]), 8)
            // The background, then the picture button.
            tb_CopyBits(bits, w + 2, rect([0, 0, 200, 400]), rect([0, 0, 200, 400]), 0, 0)
            let button = rect([150, 300, 180, 380])
            tb_CopyBits(bits, w + 2, rect([0, 0, 30, 80]), button, 0, 0)
            gmemcpy(rd32(UInt32(LM_CurrentA5)) - 48, button, 8)
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// At the start a window's Cancel is passed over, as it may quit,
        /// but not when the window has a Quit button of its own, dimmed or
        /// not (HDBackup's while it asks for a disk): Cancel is pressed.
        @Test func cancelBesideAQuitIsPressed() throws {
            let dir = try launch(untilAButton)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            let w = tb_NewWindow(0, rect([50, 50, 150, 250]), title("Backup"), 1, 1, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            _ = tb_NewControl(w, rect([10, 10, 30, 80]), title("Cancel"), 1, 0, 0, 1, 0, 0)
            let quit = tb_NewControl(w, rect([40, 10, 60, 80]), title("Quit"), 1, 0, 0, 1, 0, 0)
            tb_HiliteControl(quit, 255) // dimmed
            wr32(rd32(UInt32(LM_CurrentA5)) - 8, w)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// A program with nothing on the screen yet is given half a
        /// minute from its start (MegaSeg's launch takes more than its
        /// nudging) before the walk ends "no menu bar or window".
        @Test func aSlowStartIsWaitedFor() throws {
            let dir = try launch(waiting)
            defer { try? FileManager.default.removeItem(at: dir) }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(200_000_000) != RUN_HALTED) // 20 s
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()).hasPrefix("menu walk found no menu bar or window"))
        }

        /// While a program is given time to start it is nudged again: one
        /// that wants more keys than a round of nudging gives (a game's
        /// title screens one after another, Nanosaur 1.1.7's) gets them.
        @Test func moreKeysComeWhileWaiting() throws {
            let dir = try launch(cat(
                be16(0x7E14),                 // MOVEQ #20,D7
                be16(0x4267),                 // loop: CLR.W -(SP)
                be16(0x3F3C), be16(8),        // MOVE.W #keyDownMask,-(SP)
                be16(0x486D), be16(0xFFE0),   // PEA event(A5)
                be16(0xA970),                 // _GetNextEvent
                be16(0x4A1F),                 // TST.B (SP)+
                be16(0x67F0),                 // BEQ loop
                be16(0x5347),                 // SUBQ.W #1,D7
                be16(0x66EC),                 // BNE loop
                be16(0xA9F4)))                // _ExitToShell after 20 keys
            defer { try? FileManager.default.removeItem(at: dir) }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// A modal dialog the program stays in is not a game drawing on,
        /// whatever changes on the screen (MediaPresenter's, put up again
        /// and again as it asks for a name).
        @Test func aModalDialogIsNotAGameDrawingOn() throws {
            let dir = try launch(cat(
                be16(0x487A), be16(0x000A),   // loop: PEA filter(PC)
                be16(0x486D), be16(0xFFDE),   // PEA itemHit(A5)
                be16(0xA991),                 // _ModalDialog
                be16(0x60F4),                 // BRA loop
                be16(0x0838), be16(5), be16(0x016D), // filter: BTST #5,Ticks+3
                be16(0x6708),                 // BEQ erase
                be16(0x486D), be16(0xFFD0),   // PEA box(A5)
                be16(0xA8A2),                 // _PaintRect
                be16(0x6006),                 // BRA done
                be16(0x486D), be16(0xFFD0),   // erase: PEA box(A5)
                be16(0xA8A3),                 // _EraseRect
                be16(0x205F),                 // done: MOVE.L (SP)+,A0
                be16(0x4FEF), be16(12),       // LEA 12(SP),SP
                be16(0x4257),                 // CLR.W (SP): not handled
                be16(0x4ED0)))                // JMP (A0)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            tb_InitDialogs(0)
            _ = dialog([50, 50, 250, 350])
            let box = rd32(UInt32(LM_CurrentA5)) - 0x30
            for (i, v) in [0, 0, 200, 300].enumerated() { wr16(box + UInt32(i * 2), UInt16(v)) }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()).hasPrefix("menu walk found no menu bar"))
        }

        /// A program drawing at the walk's keys and clicks is walked as a
        /// game even when its screen comes back to where it was between
        /// rounds of nudging (Age of Empires' main menu): its screen is
        /// looked at each step.
        @Test func aScreenAnsweringInputIsAGame() throws {
            let dir = try launch(cat(
                be16(0x4267),                 // loop: CLR.W -(SP)
                be16(0x3F3C), be16(0x000A),   // MOVE.W #mDownMask+keyDownMask,-(SP)
                be16(0x486D), be16(0xFFE0),   // PEA event(A5)
                be16(0xA970),                 // _GetNextEvent
                be16(0x4A1F),                 // TST.B (SP)+
                be16(0x67F0),                 // BEQ loop
                be16(0x486D), be16(0xFFD0),   // PEA box(A5)
                be16(0x0C6D), be16(1), be16(0xFFE0), // CMPI.W #mouseDown,event.what(A5)
                be16(0x6704),                 // BEQ erase
                be16(0xA8A2),                 // _PaintRect: a key paints the box
                be16(0x60E0),                 // BRA loop
                be16(0xA8A3),                 // erase: _EraseRect: a click erases it
                be16(0x60DC)))                // BRA loop
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            let w = tb_NewWindow(0, rect([50, 50, 250, 350]), title("Game"), 1, 1, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            let box = rd32(UInt32(LM_CurrentA5)) - 0x30
            for (i, v) in [0, 0, 100, 200].enumerated() { wr16(box + UInt32(i * 2), UInt16(v)) }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "menu walk finished")
        }

        /// A program whose SIZE says it runs only in the background (a
        /// server: Syslogd) has nothing on the screen by design: its walk
        /// ends finished, not "no menu bar or window".
        @Test func aBackgroundOnlyProgramIsWalked() throws {
            let dir = try launch(waiting)
            defer { try? FileManager.default.removeItem(at: dir) }
            g_app_size_flags = 0x1480 // canBackground, onlyBackground, 32-bit clean
            defer { g_app_size_flags = 0 }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "menu walk finished")
        }

        /// A window's only button dimmed for now (a licence's Agree while
        /// it counts down, Terrain Builder's) is waited for and pressed
        /// once it is active.
        @Test func aDimmedButtonIsWaitedFor() throws {
            let dir = try launch(untilAButton)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = initToolbox()
            let w = tb_NewWindow(0, rect([50, 50, 150, 250]), title("Licence"), 1, 1, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            let agree = tb_NewControl(w, rect([10, 10, 30, 80]), title("Agree"), 1, 0, 0, 1, 0, 0)
            tb_HiliteControl(agree, 255) // dimmed
            wr32(rd32(UInt32(LM_CurrentA5)) - 8, w)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(350_000_000) != RUN_HALTED) // past the half minute
            tb_HiliteControl(agree, 0)
            #expect(machine_run(400_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }
    }
}
