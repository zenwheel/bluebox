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

        /// Programs that load D0 with only the Display Manager's routine
        /// number (King of Parking, Bugs Bannis, Rain'Net: MOVEQ #$36,D0)
        /// get DMNewDisplayModeList.
        @Test func displayManagerSelectorWithoutSize() {
            let count = tb_NewPtr(4, false, true), list = tb_NewPtr(4, false, true)
            func push(_ v: UInt32) -> [UInt16] { [0x2F3C, hiWord(v), loWord(v)] }
            #expect(run68k([0x554F] + push(1) + push(0) + push(0) + push(count) + push(list) + [
                0x7036, 0xABEB, 0x3E1F,         // MOVEQ #$36,D0; _DisplayDispatch; MOVE.W (SP)+,D7
                0xA9F4,
            ]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd32(count) > 0 && rd32(list) != 0)
        }

        /// GetFormatOrder from 68k code (Eudora; ScriptUtil selector
        /// $8012FFFC): a run the direction procedure says goes right to
        /// left is reversed within its stretch on a left-to-right line.
        @Test func getFormatOrderFrom68k() {
            // pascal Boolean proc(short run, void *param): true for runs 2 and 3
            let words: [UInt16] = [0x205F, 0x588F, 0x301F, 0x5540, 0x0C40, 0x0002, 0x55C0, 0x1E80, 0x4ED0]
            let proc = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(proc + UInt32(i * 2), w) }
            let order = tb_NewPtr(16, false, true)
            func push(_ v: UInt32) -> [UInt16] { [0x2F3C, hiWord(v), loWord(v)] }
            #expect(run68k(push(order) + [0x3F3C, 1, 0x3F3C, 4, 0x1F3C, 0] + push(proc) + push(0) + [
                0x2F3C, 0x8012, 0xFFFC, 0xA8B5,
                0xA9F4,
            ]) == "ExitToShell")
            #expect((0..<4).map { rd16(order + UInt32(2 * $0)) } == [1, 3, 2, 4])
        }

        /// The CallComponent routine 68k code calls through (Eudora) is a
        /// component call whatever its caller left in D0 (Eudora's mask,
        /// $FFFFFFEF): it reaches the Component Manager as one, and an
        /// instance that isn't one is an error, not a halt.
        @Test func callComponentIgnoresD0() {
            let params = tb_NewPtr(8, false, true) // flags, size 0, what 1, no parameters, instance 0
            wr16(params + 2, 1)
            let upp = mm_call_component_upp()
            #expect(upp != 0)
            #expect(run68k([
                0x70EF, 0x598F, 0x4879, hiWord(params), loWord(params), // MOVEQ #-17,D0; SUBQ.L #4,SP; PEA params
                0x2079, hiWord(upp), loWord(upp), 0x4E90, 0x2E1F,        // MOVEA.L upp,A0; JSR (A0); MOVE.L (SP)+,D7
                0xA9F4,
            ]) == "ExitToShell")
        }

        /// OSDispatch selectors no header names (PrintMonitor $28, Virtual
        /// and DragStrip $2B, NotePad Deluxe $55, MACAST and MacAMP $5A):
        /// each takes the arguments its callers push and leaves the stack as
        /// it found it, a result aside.
        @Test func unnamedOSDispatchSelectors() {
            launchTestApp()
            _ = initToolbox()
            func push(_ v: UInt32) -> [UInt16] { [0x2F3C, hiWord(v), loWord(v)] }
            let marks: [UInt16] = [0x2C0F]                  // MOVE.L A7,D6
            let check: [UInt16] = [0x2A0F, 0xA9F4]          // MOVE.L A7,D5; _ExitToShell
            func run(_ body: [UInt16]) {
                #expect(run68k(marks + body + check) == "ExitToShell")
                #expect(g_cpu!.pointee.d.5 == g_cpu!.pointee.d.6)
            }
            // $28: three arguments, no result (PrintMonitor)
            run(push(0) + push(0) + push(0x0028_0000) + [0xA88F]) // the word argument, then the selector
            // $55: Boolean (long), false for nil (NotePad Deluxe)
            run([0x554F] + push(0) + [0x3F3C, 0x0055, 0xA88F, 0x3E1F])
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            // $5A: NewWindow without the storage (NotePad Deluxe, MACAST, MacAMP);
            // $64: FindWindow
            let bounds = tb_NewPtr(8, false, true)
            wr16(bounds, 40); wr16(bounds + 2, 30); wr16(bounds + 4, 140); wr16(bounds + 6, 230)
            let title = tb_NewPtr(8, false, true)
            wr8(title, 1); wr8(title + 1, 0x4E)
            run([0x598F] + push(bounds) + push(title) + [0x1F3C, 0x0001, 0x3F3C, 0] + push(0xFFFF_FFFF) + [0x1F3C, 0x0001]
                + push(0) + push(0) + push(0) + [0x3F3C, 0x005A, 0xA88F, 0x2E1F])
            let made = g_cpu!.pointee.d.7
            #expect(made != 0 && rd32(UInt32(LM_WindowList)) == made && rd8(made + UInt32(WindowRecord_visible_off)) != 0)
            let out = tb_NewPtr(4, false, true)
            let inside: UInt32 = 90 << 16 | 100 // v 90, h 100: inside the content
            run([0x554F] + push(inside) + push(out) + [0x3F3C, 0x0064, 0xA88F, 0x3E1F])
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 3 && rd32(out) == made) // inContent
            // $2B: the program's windows one after another from nil (Virtual)
            let r = tb_NewPtr(8, false, true)
            wr16(r + 4, 100)
            wr16(r + 6, 100)
            let name = tb_NewPtr(4, false, true)
            wr8(name, 1)
            wr8(name + 1, 0x57)
            let first = tb_NewWindow(0, r, name, 1, 0, 0xFFFF_FFFF, 0, 0)
            let second = tb_NewWindow(0, r, name, 1, 0, 0xFFFF_FFFF, 0, 0)
            run([0x598F] + push(0) + [0x1F3C, 0, 0x3F3C, 0x002B, 0xA88F, 0x2E1F]) // from nil
            #expect(g_cpu!.pointee.d.7 == second) // the front window: the second one made
            run([0x598F] + push(second) + [0x1F3C, 0, 0x3F3C, 0x002B, 0xA88F, 0x2E1F])
            #expect(g_cpu!.pointee.d.7 == first)
            run([0x598F] + push(first) + [0x1F3C, 0, 0x3F3C, 0x002B, 0xA88F, 0x2E1F])
            #expect(g_cpu!.pointee.d.7 == made) // the one made before, and then the list ends
        }
    }
}
