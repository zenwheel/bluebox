import CCore
import CPU68K
import Testing
import Toolbox

extension BlueBox {
    /// When a program wants the whole screen shown instead of rootless
    /// windows: it writes to the screen outside its windows itself, or
    /// hides the menu bar.
    @Suite struct ScreenMode {
        var cpu: UnsafeMutablePointer<cpu68k> { g_cpu! }

        /// A window at (100,100)-(200,300) on a fresh machine, the display synced.
        func setUp() -> UInt32 {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 200, 300].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let title = tb_NewPtr(2, false, true)
            let w = tb_NewWindow(0, r, title, 1, 0, 0xFFFF_FFFF, 0, 0)
            display_sync()
            #expect(machine_screen_mode_wanted() == nil)
            return w
        }

        /// The program's own code stores a long at screen pixel (x, y).
        func store(x: Int, y: Int) {
            let at = rd32(UInt32(LM_ScrnBase)) + UInt32(y) * qd_screen_row_bytes() + UInt32(x * Int(qd_screen_depth()) / 8)
            let words: [UInt16] = [0x23FC, 0xFFFF, 0xFFFF, UInt16(at >> 16), UInt16(at & 0xFFFF), // MOVE.L #-1,at
                                   0xA9F4] // _ExitToShell
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let stack = tb_NewPtr(4096, false, true)
            cpu.pointee.a.7 = stack + 4096 - 16
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, 100)
            display_sync()
        }

        @Test func writesInsideAWindowStayRootless() {
            _ = setUp()
            store(x: 150, y: 150)
            #expect(machine_screen_mode_wanted() == nil)
        }

        @Test func writesOutsideTheWindowsWantTheScreen() {
            _ = setUp()
            store(x: 500, y: 500)
            #expect(machine_screen_mode_wanted().map { String(cString: $0) } == "it draws on the screen outside its windows")
        }

        @Test func toolboxDrawingIsNotWatched() {
            _ = setUp()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [400, 400, 420, 420].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_SetPort(rd32(UInt32(LM_WMgrPort)))
            tb_PaintRect(r) // QuickDraw on the desktop: the Toolbox's own writes
            display_sync()
            #expect(machine_screen_mode_wanted() == nil)
        }

        @Test func hidingTheMenuBarWantsTheScreen() {
            _ = setUp()
            wr16(UInt32(LM_MBarHeight), 0)
            display_sync()
            #expect(machine_screen_mode_wanted().map { String(cString: $0) } == "it hides the menu bar")
        }
    }
}
