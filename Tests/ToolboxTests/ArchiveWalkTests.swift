import CCore
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// Routines the walk of the Vintage Apple Macintosh archive found
    /// missing: Window Manager layers, Open Transport's tasks, Carbon's
    /// international string names.
    @Suite struct ArchiveWalk {
        @discardableResult
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// The program's layer holds the window list, under the root; a
        /// layer NewLayer makes is the current one's child, and swapping
        /// and setting the current layer go between them (Audion, RegEdit).
        @Test func windowLayers() {
            bootMachine()
            let app = tb_GetCurLayer()
            #expect(app != 0 && tb_IsLayer(app) != 0)
            let root = tb_GetParent(app)
            #expect(root != 0 && tb_GetParent(root) == 0 && tb_GetSubWindows(root) == app)
            let r = tb_NewPtr(8, false, true)
            wr16(r + 4, 100)
            wr16(r + 6, 100)
            let w = tb_NewWindow(0, r, pstr("W"), 1, 0, 0xFFFF_FFFF, 0, 0)
            #expect(tb_GetSubWindows(app) == w && tb_GetParent(w) == app && tb_IsLayer(w) == 0)
            let out = tb_NewPtr(4, false, true)
            #expect(tb_NewLayer(out, 1, 0, 0xFFFF_FFFF, 7) == 0)
            let mine = rd32(out)
            #expect(mine != 0 && tb_GetParent(mine) == app && rd32(mine + UInt32(WindowRecord_refCon_off)) == 7)
            #expect(tb_SwapCurLayer(mine) == app && tb_GetCurLayer() == mine)
            #expect(tb_SetCurLayer(app) == 0 && tb_GetCurLayer() == app)
            #expect(tb_SetCurLayer(w) != 0 && tb_GetCurLayer() == app) // a window isn't a layer
        }

        /// Open Transport's timer and deferred tasks run their procedure,
        /// with its argument, from the Time Manager's pump once due.
        @Test func openTransportTasks() {
            bootMachine()
            _ = cfm_ppc_cpu()
            // li r4,1; stw r4,0(r3); blr: *arg = 1
            let code = tb_NewPtr(12, false, true)
            wr32(code, 0x3880_0001)
            wr32(code + 4, 0x9083_0000)
            wr32(code + 8, 0x4E80_0020)
            let tv = tb_NewPtr(8, false, true)
            wr32(tv, code)
            let flag = tb_NewPtr(4, false, true)
            let timer = call("OTCreateTimerTask", [tv, flag])
            #expect(timer != 0)
            #expect(call("OTScheduleTimerTask", [timer, 1_000_000]) != 0)
            ot_tasks_pump()
            #expect(rd32(flag) == 0) // not yet due
            #expect(call("OTCancelTimerTask", [timer]) != 0 && call("OTCancelTimerTask", [timer]) == 0)
            #expect(call("OTScheduleTimerTask", [timer, 0]) != 0)
            ot_tasks_pump()
            #expect(rd32(flag) == 1)
            call("OTDestroyTimerTask", [timer])
            #expect(call("OTScheduleTimerTask", [timer, 0]) == 0) // gone
            wr32(flag, 0)
            let deferred = call("OTCreateDeferredTask", [tv, flag])
            #expect(call("OTScheduleDeferredTask", [deferred]) != 0)
            #expect(call("OTScheduleDeferredTask", [deferred]) == 0) // already scheduled
            ot_tasks_pump()
            #expect(rd32(flag) == 1)
            #expect(call("OTDestroyDeferredTask", [deferred]) == 0)
        }

        /// OTCompareAndSwap8 and 16, and Carbon's CompareString and
        /// TimeString (Turtle Turmoil, The Cow Catching Game, Amorphium).
        @Test func smallCalls() {
            bootMachine()
            let b = tb_NewPtr(4, false, true)
            wr8(b, 5)
            #expect(call("OTCompareAndSwap8", [4, 9, b]) == 0 && rd8(b) == 5)
            #expect(call("OTCompareAndSwap8", [5, 9, b]) == 1 && rd8(b) == 9)
            wr16(b, 0x1234)
            #expect(call("OTCompareAndSwap16", [0x1234, 0x4321, b]) == 1 && rd16(b) == 0x4321)
            #expect(Int16(truncatingIfNeeded: call("CompareString", [pstr("apple"), pstr("banana"), 0])) == -1)
            #expect(Int16(truncatingIfNeeded: call("CompareString", [pstr("pear"), pstr("pear"), 0])) == 0)
            let out = tb_NewPtr(256, false, true)
            call("TimeString", [0, 0, out, 0])
            #expect(rd8(out) > 0)
        }
    }
}
