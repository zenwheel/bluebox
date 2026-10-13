import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension Classic {
    /// Low-memory globals that programs read or call directly.
    @Suite struct LowMemory {
        init() { bootMachine() }

        func setStack() {
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
        }

        /// EventQueue holds the pending events as event records.
        @Test func eventQueueMirrorsTheEvents() {
            tb_FlushEvents(-1, 0)
            #expect(rd32(UInt32(LM_EventQueue) + 2) == 0)
            #expect(tb_PostEvent(3, 0x0061) == 0) // keyDown 'a'
            #expect(tb_PostEvent(5, 0x0062) == 0) // autoKey 'b'
            let head = rd32(UInt32(LM_EventQueue) + 2), tail = rd32(UInt32(LM_EventQueue) + 6)
            #expect(head != 0 && tail != 0 && head != tail)
            #expect(rd16(head + 4) == 4 && rd16(head + 6) == 3 && rd32(head + 8) == 0x61) // evType, keyDown, 'a'
            #expect(rd32(head) == tail && rd16(tail + 6) == 5 && rd32(tail) == 0)
            tb_FlushEvents(-1, 0)
            #expect(rd32(UInt32(LM_EventQueue) + 2) == 0)
        }

        /// PortList lists the open ports.
        @Test func portListFollowsOpenAndClose() {
            let port = initToolbox()
            func listed(_ p: UInt32) -> Bool {
                let h = rd32(UInt32(LM_PortList))
                let n = Int(rd16(rd32(h)))
                return (0..<n).contains { rd32(rd32(h) + 2 + UInt32($0 * 4)) == p }
            }
            #expect(listed(port))
            tb_ClosePort(port)
            #expect(!listed(port))
        }

        /// AuxWinHead ends with the default record; SetWinColor gives a window
        /// its own, which goes with the window.
        @Test func auxiliaryWindowRecords() {
            launchTestApp()
            _ = initToolbox()
            let hp = tb_NewPtr(4, false, true)
            #expect(tb_GetAuxWin(0, hp) == 0)
            let def = rd32(hp)
            #expect(def != 0 && rd32(rd32(def) + 8) != 0) // the default colour table
            let r = tb_NewPtr(8, false, true)
            wr16(r, 50); wr16(r + 2, 50); wr16(r + 4, 150); wr16(r + 6, 200)
            let w = tb_NewCWindow(0, r, 0, 0, 0, 0xFFFF_FFFF, 0, 0)
            #expect(tb_GetAuxWin(w, hp) == 0 && rd32(hp) == def)
            let ct = tb_NewHandle(16, false, true)
            tb_SetWinColor(w, ct)
            #expect(tb_GetAuxWin(w, hp) != 0 && rd32(rd32(rd32(hp)) + 8) == ct)
            tb_DisposeWindow(w)
            #expect(rd32(UInt32(LM_AuxWinHead)) == def)
        }

        /// Measuring text leaves the font's width table in WidthTabHandle.
        @Test func widthTableFollowsTheFont() {
            _ = initToolbox()
            tb_TextFont(4) // Monaco
            tb_TextSize(9)
            let s = tb_NewPtr(2, false, true)
            wr8(s, 1); wr8(s + 1, 0x57) // "W"
            let w = tb_StringWidth(s)
            let t = rd32(UInt32(LM_WidthPtr))
            #expect(t != 0 && t == rd32(rd32(UInt32(LM_WidthTabHandle))))
            #expect(Int16(rd32(t + 0x57 * 4) >> 16) == w)
            #expect(rd16(t + UInt32(WidthTable_fID_off)) == 4 && rd16(t + UInt32(WidthTable_fSize_off)) == 9)
            #expect(rd16(t + UInt32(WidthTable_tabSize_off)) == UInt16(WidthTable_SIZEOF))
        }

        /// Key1Trans translates a key code (D2) with modifiers (D1) to a character.
        @Test func keyTransVectors() {
            setStack()
            let c = g_cpu!
            c.pointee.d.1 = 0
            c.pointee.d.2 = 0 // the A key
            guest_call_regs(rd32(UInt32(LM_Key1Trans)))
            #expect(c.pointee.d.0 & 0xFF == 0x61)
            c.pointee.d.1 = 0x02 // shift
            c.pointee.d.2 = 0
            guest_call_regs(rd32(UInt32(LM_Key2Trans)))
            #expect(c.pointee.d.0 & 0xFF == 0x41)
        }

        /// ErrorSound sets the alert sound procedure; InitDialogs keeps ResumeProc.
        @Test func dialogGlobals() {
            #expect(rd32(UInt32(LM_DABeeper)) != 0) // the standard one
            tb_ErrorSound(0x1234)
            #expect(rd32(UInt32(LM_DABeeper)) == 0x1234)
            tb_InitDialogs(0x5678)
            #expect(rd32(UInt32(LM_ResumeProc)) == 0x5678)
        }

        /// A desk hook paints the desktop (D0 = 0) instead of DeskPattern, and
        /// FindWindow gives the desktop to it as inSysWindow.
        @Test func deskHookPaintsTheDesktop() {
            launchTestApp()
            _ = initToolbox()
            let out = tb_NewPtr(8, false, true)
            let words: [UInt16] = [0x52B9, UInt16(out >> 16), UInt16(out & 0xFFFF),               // ADDQ.L #1,out
                                   0x23C0, UInt16((out + 4) >> 16), UInt16((out + 4) & 0xFFFF), 0x4E75] // MOVE.L D0,out+4; RTS
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            wr32(UInt32(LM_DeskHook), code)
            wr32(out + 4, 0xFFFF_FFFF)
            let r = tb_NewPtr(8, false, true)
            wr16(r, 50); wr16(r + 2, 50); wr16(r + 4, 150); wr16(r + 6, 200)
            let title = tb_NewPtr(8, false, true)
            guestPString("Test", at: title)
            let w = tb_NewWindow(0, r, title, 1, 0, 0xFFFF_FFFF, 0, 0)
            #expect(w != 0)
            tb_DisposeWindow(w) // uncovers desktop
            #expect(rd32(out) >= 1 && rd32(out + 4) == 0)
            let wp = tb_NewPtr(4, false, true)
            #expect(tb_FindWindow(0x0200_0200, wp) == 2 && rd32(wp) == 0) // inSysWindow, no window
            wr32(UInt32(LM_DeskHook), 0)
        }

        /// A deferred task runs once at the next interrupt, with A1 = dtParam.
        @Test func deferredTaskRuns() {
            setStack()
            let out = tb_NewPtr(8, false, true)
            let words: [UInt16] = [0x23C9, UInt16(out >> 16), UInt16(out & 0xFFFF),               // MOVE.L A1,out
                                   0x52B9, UInt16((out + 4) >> 16), UInt16((out + 4) & 0xFFFF), 0x4E75] // ADDQ.L #1,out+4; RTS
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let task = tb_NewPtr(20, false, true)
            wr16(task + 4, 7) // dtQType
            wr32(task + 8, code)
            wr32(task + 12, 0xCAFE)
            #expect(tb_DTInstall(task) == 0)
            g_cpu!.pointee.sr = 0x2000
            time_pump()
            time_pump()
            #expect(rd32(out) == 0xCAFE && rd32(out + 4) == 1)
            #expect(rd32(UInt32(LM_JDTInstall)) != 0)
        }
    }
}
