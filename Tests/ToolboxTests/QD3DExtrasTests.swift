import CCore
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// QuickDraw 3D's controllers and trackers, Custom Elements, 1.0's
    /// names and Gestalt's answers.
    @Suite struct QD3DExtras {
        typealias Q = Q3Call

        func cstring(_ s: String) -> UInt32 {
            let p = tb_NewPtr(Int32(s.utf8.count + 1), false, true)
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func read(_ p: UInt32) -> String {
            var bytes: [UInt8] = []
            var a = p
            while rd8(a) != 0 { bytes.append(rd8(a)); a += 1 }
            return String(decoding: bytes, as: UTF8.self)
        }

        @Test func controllersAndTrackers() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let changed = tb_NewPtr(4, false, true), serial = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Controller_GetListChanged", [changed, serial]) == 1)
            let next = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Controller_Next", [0, next]) == 1 && rd32(next) == 0) // none of its own
            let ctl = Q.call("Q3Controller_New", [Q.words([cstring("Test:Knob"), 2, 0, 0, 0])])
            #expect(ctl != 0)
            #expect(Q.call("Q3Controller_GetListChanged", [changed, serial]) == 1 && rd32(changed) == 1)
            #expect(Q.call("Q3Controller_Next", [0, next]) == 1 && rd32(next) == ctl)
            let sig = tb_NewPtr(64, false, true)
            #expect(Q.call("Q3Controller_GetSignature", [ctl, sig, 64]) == 1 && read(sig) == "Test:Knob")
            // Values with serial numbers.
            #expect(Q.call("Q3Controller_SetValues", [ctl, Q.floats([0.25, 0.5]), 2]) == 1)
            let vals = tb_NewPtr(8, false, true), vserial = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Controller_GetValues", [ctl, 2, vals, changed, vserial]) == 1)
            #expect(Q.float(vals + 4) == 0.5 && rd32(changed) == 1)
            #expect(Q.call("Q3Controller_GetValues", [ctl, 2, vals, changed, vserial]) == 1 && rd32(changed) == 0)
            // A tracker following the controller's moves.
            let tracker = Q.call("Q3Tracker_New", [0])
            #expect(tracker != 0)
            #expect(Q.call("Q3Controller_SetTracker", [ctl, tracker]) == 1)
            #expect(Q.call("Q3Controller_MoveTrackerPosition", [ctl, Q.floats([1, 2, 3])]) == 1)
            let pos = tb_NewPtr(12, false, true), delta = tb_NewPtr(12, false, true), ps = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Tracker_GetPosition", [tracker, pos, delta, changed, ps]) == 1)
            #expect(Q.float(pos + 8) == 3 && rd32(changed) == 1)
            #expect(Q.call("Q3Tracker_MovePosition", [tracker, ctl, Q.floats([1, 0, 0])]) == 1)
            #expect(Q.call("Q3Tracker_GetPosition", [tracker, pos, delta, changed, ps]) == 1)
            #expect(Q.float(pos) == 2 && Q.float(delta) == 1 && rd32(changed) == 1)
            #expect(Q.call("Q3Controller_SetButtons", [ctl, 1]) == 1)
            let buttons = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Tracker_GetButtons", [tracker, buttons]) == 1 && rd32(buttons) == 1)
            // Event coordinates: the latest at or before the time.
            #expect(Q.call("Q3Tracker_SetEventCoordinates", [tracker, 100, 1, Q.floats([5, 0, 0]), 0]) == 1)
            #expect(Q.call("Q3Tracker_SetEventCoordinates", [tracker, 200, 0, Q.floats([6, 0, 0]), 0]) == 1)
            #expect(Q.call("Q3Tracker_GetEventCoordinates", [tracker, 150, buttons, pos, 0]) == 1)
            #expect(Q.float(pos) == 5 && rd32(buttons) == 1)
            // A controller state takes the tracker away and gives it back.
            let state = Q.call("Q3ControllerState_New", [ctl])
            let has = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3ControllerState_SaveAndReset", [state]) == 1)
            #expect(Q.call("Q3Controller_HasTracker", [ctl, has]) == 1 && rd32(has) == 0)
            #expect(Q.call("Q3ControllerState_Restore", [state]) == 1)
            #expect(Q.call("Q3Controller_HasTracker", [ctl, has]) == 1 && rd32(has) == 1)
            #expect(Q.call("Q3Controller_Decommission", [ctl]) == 1)
            #expect(Q.call("Q3Controller_Next", [0, next]) == 1 && rd32(next) == 0)
            Q.call("Q3Object_Dispose", [state])
            Q.call("Q3Object_Dispose", [tracker])
        }

        @Test func customElements() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let box = Q.call("Q3Box_New", [Q.floats([0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0])])
            #expect(Q.call("CENameElement_SetData", [box, cstring("Teapot")]) == 1)
            let out = tb_NewPtr(4, false, true)
            #expect(Q.call("CENameElement_GetData", [box, out]) == 1 && read(rd32(out)) == "Teapot")
            #expect(Q.call("CENameElement_EmptyData", [out]) == 1 && rd32(out) == 0)
            let url = Q.words([cstring("http://example.com/"), cstring("Home"), 1])
            #expect(Q.call("CEUrlElement_SetData", [box, url]) == 1)
            #expect(Q.call("CEUrlElement_GetData", [box, out]) == 1)
            let d = rd32(out)
            #expect(read(rd32(d)) == "http://example.com/" && read(rd32(d + 4)) == "Home" && rd32(d + 8) == 1)
            #expect(Q.call("CEUrlElement_EmptyData", [out]) == 1)
            // The name goes when set to nothing, the URL stays.
            #expect(Q.call("CENameElement_SetData", [box, 0]) == 1)
            #expect(Q.call("CENameElement_GetData", [box, out]) == 1 && rd32(out) == 0)
            #expect(Q.call("CEUrlElement_GetData", [box, out]) == 1 && rd32(out) != 0)
            // QuickDraw 3D 1.0's names still answer.
            let q = tb_NewPtr(16, false, true)
            ppc_set_fpr(&g_ppc, 1, Double.pi)
            #expect(Q.call("Q3Quaternion_SetRotateX", [q, 0], fp: [Double.pi]) == q)
            #expect(abs(Q.float(q + 4) - 1) < 1e-5)
            Q.call("Q3Object_Dispose", [box])
        }
    }
}
