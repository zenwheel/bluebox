import CCore
import CPU68K
import Testing
import Toolbox

extension BlueBox {
    /// Trap-dispatcher selectors the full archive walk found programs using
    /// that Blue Box stopped at.
    @Suite struct DispatchSelectors {
        init() { bootMachine() }

        /// _LayerDispatch selector 2 is IsLayer (HyperCard's window walk, so
        /// every program standalone from HyperCard): true for a layer,
        /// false for a window.
        @Test func isLayerFrom68k() {
            let app = tb_GetCurLayer()
            let r = tb_NewPtr(8, false, true)
            wr16(r + 4, 100)
            wr16(r + 6, 100)
            let pname = tb_NewPtr(8, false, true)
            wr8(pname, 1)
            wr8(pname + 1, 0x57)
            let w = tb_NewWindow(0, r, pname, 1, 0, 0xFFFF_FFFF, 0, 0)
            func isLayer(_ p: UInt32) -> UInt32 {
                #expect(run68k([
                    0x554F, 0x2F3C, hiWord(p), loWord(p), // SUBQ #2,SP; MOVE.L #p,-(SP)
                    0x7002, 0xA829, 0x1E1F,               // MOVEQ #2,D0; _LayerDispatch; MOVE.B (SP)+,D7
                    0xA9F4,
                ]) == "ExitToShell")
                return g_cpu!.pointee.d.7 & 0xFF
            }
            #expect(isLayer(app) == 1)
            #expect(isLayer(w) == 0)
        }

        /// Selector -1 (MoviePlay, which swaps in the layer it gives): the
        /// program's layer, even while another layer is current.
        @Test func appLayerFrom68k() {
            let app = tb_GetCurLayer()
            let out = tb_NewPtr(4, false, true)
            #expect(tb_NewLayer(out, 1, 0, 0xFFFF_FFFF, 0) == 0)
            #expect(tb_SetCurLayer(rd32(out)) == 0)
            #expect(run68k([
                0x598F, 0x70FF, 0xA829, 0x2E1F, // SUBQ.L #4,SP; MOVEQ #-1,D0; _LayerDispatch; MOVE.L (SP)+,D7
                0xA9F4,
            ]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 == app)
            #expect(tb_SetCurLayer(app) == 0)
        }

        func pstring(_ str: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            let b = Array(str.utf8)
            wr8(p, UInt8(b.count))
            for (i, c) in b.enumerated() { wr8(p + UInt32(1 + i), c) }
            return p
        }

        /// The Control Strip's preferences (CalcStrip's and G3Strip's
        /// registration programs, OT:PPP's CSM helper: selector 4) are
        /// missing until saved, then come back as a copy of what was saved.
        @Test func controlStripPreferences() {
            let name = pstring("Test Module")
            let out = tb_NewPtr(4, false, true)
            wr32(out, 0xDEAD)
            #expect(run68k([
                0x554F, 0x2F3C, hiWord(name), loWord(name), // SUBQ #2,SP; MOVE.L #name,-(SP)
                0x2F3C, hiWord(out), loWord(out),          // MOVE.L #out,-(SP)
                0x303C, 0x0404, 0xAAF2, 0x3E1F,            // MOVE.W #$0404,D0; _ControlStripDispatch; MOVE.W (SP)+,D7
                0xA9F4,
            ]) == "ExitToShell")
            #expect(Int16(truncatingIfNeeded: g_cpu!.pointee.d.7) == -192 && rd32(out) == 0)
            let h = tb_NewHandle(4, false, false)
            wr32(rd32(h), 0x0102_0304)
            #expect(tb_SBSavePreferences(name, h) == 0)
            wr32(rd32(h), 0) // the program changes its handle afterwards
            #expect(tb_SBLoadPreferences(name, out) == 0)
            let got = rd32(out)
            #expect(got != 0 && got != h && tb_GetHandleSize(got) == 4 && rd32(rd32(got)) == 0x0102_0304)
            #expect(tb_SBLoadPreferences(pstring("Other"), out) == -192 && rd32(out) == 0)
        }

        /// Detached strings come from a STR# list's data.
        @Test func controlStripDetachedStrings() {
            let list = tb_NewHandle(9, false, false)
            let d = rd32(list)
            wr16(d, 2)
            wr8(d + 2, 2); wr8(d + 3, 0x61); wr8(d + 4, 0x62)
            wr8(d + 5, 3); wr8(d + 6, 0x63); wr8(d + 7, 0x64); wr8(d + 8, 0x65)
            let out = tb_NewPtr(256, false, true)
            tb_SBGetDetachedIndString(out, list, 2)
            #expect(rd8(out) == 3 && rd8(out + 1) == 0x63 && rd8(out + 3) == 0x65)
            tb_SBGetDetachedIndString(out, list, 3)
            #expect(rd8(out) == 0)
        }
    }
}
