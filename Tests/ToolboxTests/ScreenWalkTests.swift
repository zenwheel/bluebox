import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The screen walk: the menu walker's way with a program that has no
    /// menu bar and is stuck on a screen of its own (a game's title with
    /// buttons it draws and tests the mouse against itself).
    @Suite(.serialized) struct ScreenWalk {
        let walk = BlueBox.MenuWalk()

        /// A window of the program's own, the current port, with nothing
        /// in it the menu walker's older ways find (no controls, no
        /// pictures).
        func window() -> UInt32 {
            _ = initToolbox()
            let w = tb_NewWindow(0, walk.rect([50, 50, 350, 450]), walk.title("Game"), 1, 1, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            return w
        }

        /// Rectangle r (local) at offset off from A5.
        func put(_ r: [Int], at off: Int) {
            let p = rd32(UInt32(LM_CurrentA5)) &- UInt32(off)
            for (i, v) in r.enumerated() { wr16(p + UInt32(i * 2), UInt16(truncatingIfNeeded: v)) }
        }

        /// A program that quits at a click in rect A (A5-$30), tested with
        /// PtInRect, and at a click in rect B (A5-$50) paints the box at
        /// A5-$40, a screen of its own.
        let twoButtons = cat(
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
            be16(0x486D), be16(0xFFD0),   // PEA a(A5)
            be16(0xA8AD),                 // _PtInRect
            be16(0x4A1F),                 // TST.B (SP)+
            be16(0x6702),                 // BEQ notA
            be16(0xA9F4),                 // _ExitToShell
            be16(0x554F),                 // notA: SUBQ #2,SP
            be16(0x2F2D), be16(0xFFEA),   // MOVE.L event.where(A5),-(SP)
            be16(0x486D), be16(0xFFB0),   // PEA b(A5)
            be16(0xA8AD),                 // _PtInRect
            be16(0x4A1F),                 // TST.B (SP)+
            be16(0x67C8),                 // BEQ loop
            be16(0x486D), be16(0xFFC0),   // PEA box(A5)
            be16(0xA8A2),                 // _PaintRect
            be16(0x60C0))                 // BRA loop

        /// The rectangles a program tests a click against are its buttons:
        /// the walk clicks them in the order tested (here the first quits).
        @Test func aHitTestedButtonIsClicked() throws {
            let dir = try walk.launch(twoButtons)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = window()
            put([20, 20, 50, 100], at: 0x30)
            put([200, 300, 230, 380], at: 0x50)
            put([0, 0, 300, 400], at: 0x40)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(1_000_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// A step that ended the program at an earlier run is avoided
        /// (--walk-avoid): the other button is clicked, a new screen comes,
        /// and the walk ends "screen walk finished".
        @Test func anAvoidedTargetIsPassedOver() throws {
            let dir = try walk.launch(twoButtons)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = window()
            put([20, 20, 50, 100], at: 0x30)
            put([200, 300, 230, 380], at: 0x50)
            put([0, 0, 300, 400], at: 0x40)
            // Button A's centre on the screen: the window's content at 50,50.
            machine_walk_avoid("110,85")
            defer { machine_walk_avoid(nil) }
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(2_000_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "screen walk finished")
        }

        /// A program that quits at a click inside the rect at A5-$30,
        /// compared in its own code (no PtInRect); while the mouse is over
        /// it (GetMouse), it hides the cursor.
        let ownHitTest = cat(
            be16(0x4267),                 // loop: CLR.W -(SP)
            be16(0x3F3C), be16(2),        // MOVE.W #mDownMask,-(SP)
            be16(0x486D), be16(0xFFE0),   // PEA event(A5)
            be16(0xA970),                 // _GetNextEvent
            be16(0x4A1F),                 // TST.B (SP)+
            be16(0x660E),                 // BNE clicked
            be16(0x486D), be16(0xFFEA),   // PEA where(A5)
            be16(0xA972),                 // _GetMouse
            be16(0x6112),                 // BSR.S inside
            be16(0x67E6),                 // BEQ loop
            be16(0xA852),                 // _HideCursor: as the mouse passes
            be16(0x60E2),                 // BRA loop
            be16(0x486D), be16(0xFFEA),   // clicked: PEA where(A5)
            be16(0xA871),                 // _GlobalToLocal
            be16(0x6104),                 // BSR.S inside
            be16(0x67D8),                 // BEQ loop
            be16(0xA9F4),                 // _ExitToShell
            be16(0x7200),                 // inside: MOVEQ #0,D1
            be16(0x302D), be16(0xFFEA),   // MOVE.W where.v(A5),D0
            be16(0xB06D), be16(0xFFD0),   // CMP.W top(A5),D0
            be16(0x6D18),                 // BLT out
            be16(0xB06D), be16(0xFFD4),   // CMP.W bottom(A5),D0
            be16(0x6C12),                 // BGE out
            be16(0x302D), be16(0xFFEC),   // MOVE.W where.h(A5),D0
            be16(0xB06D), be16(0xFFD2),   // CMP.W left(A5),D0
            be16(0x6D08),                 // BLT out
            be16(0xB06D), be16(0xFFD6),   // CMP.W right(A5),D0
            be16(0x6C02),                 // BGE out
            be16(0x7201),                 // MOVEQ #1,D1
            be16(0x4A01),                 // out: TST.B D1
            be16(0x4E75))                 // RTS

        /// A button the program tests the mouse against in its own code
        /// is found by sweeping the mouse over the window: the code it
        /// runs as the mouse passes over (a cursor hidden, a button lit)
        /// is new, and where the mouse was is clicked.
        @Test func theMouseSweptFindsAButton() throws {
            let dir = try walk.launch(ownHitTest)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = window()
            put([200, 250, 240, 320], at: 0x30)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(1_000_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// Text drawn off the screen and copied to it (a game's screen
        /// made in a buffer of its own) is a target where it lands: the
        /// program's own test of the click (no PtInRect) finds it there.
        @Test func textCopiedToTheScreenIsClicked() throws {
            let dir = try walk.launch(cat(
                be16(0x4267),                 // loop: CLR.W -(SP)
                be16(0x3F3C), be16(2),        // MOVE.W #mDownMask,-(SP)
                be16(0x486D), be16(0xFFE0),   // PEA event(A5)
                be16(0xA970),                 // _GetNextEvent
                be16(0x4A1F),                 // TST.B (SP)+
                be16(0x67F0),                 // BEQ loop
                be16(0x486D), be16(0xFFEA),   // PEA where(A5)
                be16(0xA871),                 // _GlobalToLocal
                be16(0x302D), be16(0xFFEA),   // MOVE.W where.v(A5),D0
                be16(0xB06D), be16(0xFFD0),   // CMP.W top(A5),D0
                be16(0x6DE0),                 // BLT loop
                be16(0xB06D), be16(0xFFD4),   // CMP.W bottom(A5),D0
                be16(0x6CDA),                 // BGE loop
                be16(0x302D), be16(0xFFEC),   // MOVE.W where.h(A5),D0
                be16(0xB06D), be16(0xFFD2),   // CMP.W left(A5),D0
                be16(0x6DD0),                 // BLT loop
                be16(0xB06D), be16(0xFFD6),   // CMP.W right(A5),D0
                be16(0x6CCA),                 // BGE loop
                be16(0xA9F4)))                // _ExitToShell
            defer { try? FileManager.default.removeItem(at: dir) }
            let w = window()
            put([70, 140, 110, 220], at: 0x30)
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            // A buffer of the window's size, with "Play" drawn in it.
            let bits = tb_NewPtr(14, false, true)
            wr32(bits, tb_NewPtr(50 * 300, false, true))
            wr16(bits + 4, 50)
            gmemcpy(bits + 6, walk.rect([0, 0, 300, 400]), 8)
            let off = tb_NewPtr(Int32(GrafPort_SIZEOF), false, true)
            tb_OpenPort(off)
            tb_SetPortBits(bits)
            tb_MoveTo(150, 100)
            tb_DrawString(walk.title("Play"))
            tb_SetPort(w)
            tb_CopyBits(bits, w + 2, walk.rect([0, 0, 300, 400]), walk.rect([0, 0, 300, 400]), 0, 0)
            #expect(machine_run(1_000_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// A screen where nothing is found to click, which no key moves
        /// on, ends the walk as before: "found no menu bar".
        @Test func aStillScreenIsNotWalked() throws {
            let dir = try walk.launch(walk.waiting)
            defer { try? FileManager.default.removeItem(at: dir) }
            _ = window()
            machine_set_menu_walk(true)
            defer { machine_set_menu_walk(false) }
            #expect(machine_run(2_000_000_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "menu walk found no menu bar")
        }
    }
}
