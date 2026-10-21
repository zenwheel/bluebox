import CCore
import CPU68K
import Testing
import Toolbox

/// Internet Config's component ('PREF' 'ICAp'), called as 68k programs
/// call it: through _ComponentDispatch, the arguments last first.
extension BlueBox {
    @Suite struct InternetConfig68K {
        @Test func preferencesThroughTheComponent() {
            launchTestApp()
            let q = QTTune()
            q.install()
            machine_set_internet_pref("Email", [3] + Array("a@b".utf8), 4)
            let ic = q.call([0x7021, 0xA82A], [fourCC("PREF"), fourCC("ICAp")]) // OpenDefaultComponent
            #expect(ic != 0)
            let key = q.guestBytes([5] + Array("EMAIL".utf8))
            let attr = tb_NewPtr(4, false, true), buf = tb_NewPtr(16, false, true), size = tb_NewPtr(4, false, true)
            wr32(size, 16)
            // ICGetPref(inst, key, &attr, buf, &size): selector 6
            #expect(q.component(ic, 6, [key, attr, buf, size]) == 0)
            #expect(rd32(size) == 4 && rd8(buf) == 3 && rd8(buf + 3) == UInt8(ascii: "b"))
            // ICSetPref(inst, key, attr, buf, size): 7; ICGetPrefHandle(inst, key, &attr, &h): 26
            let other = q.guestBytes([4] + Array("Test".utf8)), data = q.guestBytes([9, 8, 7])
            #expect(q.component(ic, 7, [other, 0, data, 3]) == 0)
            let hp = tb_NewPtr(4, false, true)
            #expect(q.component(ic, 26, [other, attr, hp]) == 0)
            #expect(tb_GetHandleSize(rd32(hp)) == 3 && rd8(rd32(rd32(hp)) + 2) == 7)
            // ICDeletePref(inst, key): 12, then it is gone
            #expect(q.component(ic, 12, [other]) == 0)
            #expect(q.component(ic, 26, [other, attr, hp]) == -666)
        }

        /// A 68k program calls a PowerPC routine through its routine
        /// descriptor, as on a Power Mac (Installer VISE's own PowerPC
        /// code): the PowerPC processor is set up when first needed.
        @Test func sixtyEightKCallsPowerPCRoutine() {
            launchTestApp()
            let q = QTTune()
            q.install()
            let code = tb_NewPtr(8, false, true), tv = tb_NewPtr(8, false, true)
            wr32(code, 0x3863_0001) // addi r3, r3, 1
            wr32(code + 4, 0x4E80_0020) // blr
            wr32(tv, code)
            wr32(tv + 4, 0)
            // NewRoutineDescriptor(tv, pascal: one long in, a long out, PowerPC)
            withUnsafeMutableBytes(of: &g_ppc.gpr) { raw in
                for (i, a) in [tv, 0xF0, 1].enumerated() { raw.storeBytes(of: UInt32(a), toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            cfm_export_fn("NewRoutineDescriptor")!(&g_ppc)
            let rd = g_ppc.gpr.3
            #expect(rd16(rd) == 0xAAFE)
            #expect(q.call([0x4EB9, UInt16(rd >> 16), UInt16(rd & 0xFFFF)], [41]) == 42) // jsr rd
            #expect(!g_cpu!.pointee.halted)
        }
    }
}
