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
    }
}
