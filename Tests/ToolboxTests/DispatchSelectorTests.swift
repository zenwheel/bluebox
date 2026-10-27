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
    }
}
