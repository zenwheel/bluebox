import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Internet Config's profiles, default preferences and GURL events, called as PowerPC programs call them.
    @Suite struct CarbonInternetConfig {
        func instance() -> UInt32 {
            let ic = svcCall("OpenDefaultComponent", fourCC("PREF"), fourCC("ICAp"))
            #expect(ic != 0)
            return ic
        }

        func getPref(_ ic: UInt32, _ key: String) -> (Int32, String) {
            let attr = svcPtr(4), buf = svcPtr(64), size = svcPtr(4)
            wr32(size, 64)
            let e = svcStatus(svcCall("ICGetPref", ic, svcPStr(key), attr, buf, size))
            return (e, String(decoding: svcBytes(buf, Int(rd32(size))), as: UTF8.self))
        }

        func setPref(_ ic: UInt32, _ key: String, _ value: String) {
            let buf = svcPtr(max(1, value.utf8.count))
            for (i, b) in value.utf8.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(svcCall("ICSetPref", ic, svcPStr(key), 0, buf, UInt32(value.utf8.count)) == 0)
        }

        @Test func profilesKeepTheirOwnPreferences() {
            launchTestApp()
            machine_set_internet_pref("Email", [3] + Array("a@b".utf8), 4)
            let ic = instance()
            let id = svcPtr(4), count = svcPtr(4), name = svcPtr(256)
            #expect(svcCall("ICCountProfiles", ic, count) == 0 && rd32(count) == 1)
            #expect(svcCall("ICGetCurrentProfile", ic, id) == 0)
            let first = rd32(id)
            #expect(svcCall("ICGetProfileName", ic, first, name) == 0)
            #expect(String(decoding: svcBytes(name + 1, Int(rd8(name))), as: UTF8.self) == "Default")
            setPref(ic, "Test", "one")
            // A new profile begins as a copy of one, or empty
            let copy = svcPtr(4), empty = svcPtr(4)
            #expect(svcCall("ICAddProfile", ic, first, copy) == 0)
            #expect(svcCall("ICAddProfile", ic, 0, empty) == 0)
            #expect(svcCall("ICCountProfiles", ic, count) == 0 && rd32(count) == 3)
            #expect(rd32(copy) != first && rd32(empty) != rd32(copy))
            #expect(svcCall("ICSetProfileName", ic, rd32(copy), svcPStr("Work")) == 0)
            #expect(svcCall("ICGetProfileName", ic, rd32(copy), name) == 0 && rd8(name) == 4)
            // In the copy: the same preference; changed there it leaves the first alone
            #expect(svcCall("ICSetCurrentProfile", ic, rd32(copy)) == 0)
            #expect(getPref(ic, "Test") == (0, "one"))
            setPref(ic, "Test", "two")
            #expect(svcCall("ICGetCurrentProfile", ic, id) == 0 && rd32(id) == rd32(copy))
            #expect(svcCall("ICSetCurrentProfile", ic, first) == 0)
            #expect(getPref(ic, "Test") == (0, "one"))
            #expect(svcCall("ICSetCurrentProfile", ic, rd32(empty)) == 0)
            #expect(getPref(ic, "Test").0 == -666)
            #expect(svcCall("ICSetCurrentProfile", ic, rd32(copy)) == 0)
            #expect(getPref(ic, "Test") == (0, "two"))
            // Indexed, deleted, not found
            #expect(svcCall("ICGetIndProfile", ic, 2, id) == 0 && rd32(id) == rd32(copy))
            #expect(svcStatus(svcCall("ICGetIndProfile", ic, 9, id)) == -676)
            #expect(svcStatus(svcCall("ICDeleteProfile", ic, rd32(copy))) == -50) // the one in use stays
            #expect(svcCall("ICDeleteProfile", ic, rd32(empty)) == 0)
            #expect(svcStatus(svcCall("ICDeleteProfile", ic, rd32(empty))) == -676)
            #expect(svcStatus(svcCall("ICSetCurrentProfile", ic, 99)) == -676)
            #expect(svcCall("ICCountProfiles", ic, count) == 0 && rd32(count) == 2)
        }

        @Test func defaultsPermissionAndGURLEvents() {
            launchTestApp()
            machine_set_internet_pref("Email", [3] + Array("a@b".utf8), 4)
            let ic = instance()
            setPref(ic, "Email", "changed")
            let h = tb_NewHandle(0, false, true)
            #expect(svcCall("ICGetDefaultPref", ic, svcPStr("EMAIL"), h) == 0)
            #expect(svcBytes(rd32(h), Int(tb_GetHandleSize(h))) == [3, 0x61, 0x40, 0x62])
            #expect(svcStatus(svcCall("ICGetDefaultPref", ic, svcPStr("Nothing"), h)) == -666)
            let perm = svcPtr(1)
            wr8(perm, 9)
            #expect(svcCall("ICGetPerm", ic, perm) == 0 && rd8(perm) == 0)
            // A GURL event for a helper, the URL its direct object
            let url = tb_NewHandle(18, false, true)
            for (i, b) in Array("http://example.org".utf8).enumerated() { wr8(rd32(url) + UInt32(i), b) }
            let ev = svcPtr(8)
            #expect(svcCall("ICCreateGURLEvent", ic, fourCC("MOSS"), url, ev) == 0 && rd32(ev) == fourCC("aevt"))
            let d = svcPtr(8)
            #expect(tb_AEGetParamDesc(ev, Int32(bitPattern: fourCC("----")), Int32(bitPattern: fourCC("TEXT")), d) == 0)
            #expect(tb_AEGetDescDataSize(d) == 18)
            let cls = svcPtr(8)
            #expect(tb_AEGetAttributePtr(ev, Int32(bitPattern: fourCC("evcl")), Int32(bitPattern: fourCC("type")), svcPtr(4), cls, 4, 0) == 0)
            #expect(rd32(cls) == fourCC("GURL"))
            #expect(svcStatus(svcCall("ICCreateGURLEvent", ic, fourCC("MOSS"), 0, ev)) == -50)
            // Sending it to an application that isn't there fails as AESend does, and doesn't hang
            #expect(svcCall("ICSendGURLEvent", ic, ev) != 0)
        }
    }
}
