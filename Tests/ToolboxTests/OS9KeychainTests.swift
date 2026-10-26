import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// KeychainLib on a Mac with no keychain: items in memory, and the
    /// errors that say there is nowhere to keep or find them.
    @Suite struct OS9Keychain {
        init() { bootMachine() }

        func err(_ name: String, _ args: [UInt32]) -> Int32 { Int32(bitPattern: ilCall(name, args)) }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        let noDefault: Int32 = -25307

        @Test func managerAndSettings() {
            let v = tb_NewPtr(4, false, true)
            #expect(err("KCGetKeychainManagerVersion", [v]) == 0 && rd32(v) >> 24 == 2)
            #expect(ilCall("KCIsInteractionAllowed", []) & 0xFF == 1)
            #expect(err("KCSetInteractionAllowed", [0]) == 0 && ilCall("KCIsInteractionAllowed", []) & 0xFF == 0)
            #expect(err("KCSetInteractionAllowed", [1]) == 0)
            // A callback can be registered once.
            #expect(err("KCAddCallback", [0x1000, 0xFFFF, 0]) == 0)
            #expect(err("KCAddCallback", [0x1000, 0xFFFF, 0]) == -25297)
            #expect(err("KCRemoveCallback", [0x1000]) == 0)
            #expect(err("KCRemoveCallback", [0x1000]) == -25298)
        }

        @Test func thereIsNoKeychain() {
            let kc = tb_NewPtr(4, false, true), n = tb_NewPtr(4, false, true)
            wr32(kc, 0x55)
            #expect(ilCall("KCCountKeychains", []) & 0xFFFF == 0)
            #expect(err("KCGetDefaultKeychain", [kc]) == noDefault && rd32(kc) == 0)
            #expect(err("KCGetIndKeychain", [1, kc]) == -25294)
            #expect(err("KCGetStatus", [0, n]) == noDefault)
            #expect(err("KCGetStatus", [0x77, n]) == -25295)
            #expect(err("KCUnlock", [0, pstr("secret")]) == noDefault)
            #expect(err("KCLock", [0x77]) == -25295)
            #expect(err("KCCreateKeychain", [pstr("secret"), kc]) == -25312)
            #expect(err("KCSetDefaultKeychain", [0x77]) == -25295)
            #expect(err("KCMakeKCRefFromFSSpec", [tb_NewPtr(70, false, true), kc]) == -25294)
            #expect(err("KCAddNewAsymmetricKeyPair", [0, 0, 0, 0]) == -25312)
        }

        @Test func passwordsHaveNowhereToGo() {
            let item = tb_NewPtr(4, false, true), len = tb_NewPtr(4, false, true), data = tb_NewPtr(16, false, true)
            wr32(item, 0x66); wr32(len, 9)
            #expect(err("KCAddGenericPassword", [pstr("svc"), pstr("scott"), 6, data, item]) == noDefault && rd32(item) == 0)
            #expect(err("KCFindGenericPassword", [pstr("svc"), pstr("scott"), 16, data, len, item]) == noDefault && rd32(len) == 0)
            // Internet passwords: item is argument 8, the path version's 9 (the rest on the stack)
            let sp = tb_NewPtr(128, false, true)
            func stackArgs(_ n: Int, _ values: [UInt32]) {
                // r1 points at a frame whose parameter area starts at 24
                for (i, v) in values.enumerated() { wr32(sp + 24 + UInt32(4 * (n + i)), v) }
            }
            wr32(item, 0x66)
            stackArgs(8, [item])
            withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: sp, toByteOffset: 4, as: UInt32.self) }
            let fn = cfm_export_fn("KCAddInternetPassword")!
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                let regs: [UInt32] = [pstr("host"), pstr("dom"), pstr("acct"), 80, fourCC("http"), fourCC("dflt"), 4, data]
                for (i, r) in regs.enumerated() { gpr.storeBytes(of: r, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            fn(&g_ppc)
            #expect(Int32(bitPattern: g_ppc.gpr.3) == noDefault && rd32(item) == 0)
            #expect(err("KCFindFirstItem", [0, 0, len, item]) == noDefault)
            #expect(err("KCFindNextItem", [0x99, item]) == -25305)
            #expect(err("KCReleaseSearch", [len]) == -25305)
        }

        @Test func itemsInMemory() {
            let item = tb_NewPtr(4, false, true), data = tb_NewPtr(32, false, true), out = tb_NewPtr(32, false, true)
            for (i, b) in Array("hunter2".utf8).enumerated() { wr8(data + UInt32(i), b) }
            #expect(err("KCNewItem", [fourCC("zzzz"), fourCC("Test"), 7, data, item]) == -25306)
            #expect(err("KCNewItem", [fourCC("genp"), fourCC("Test"), 7, data, item]) == 0 && rd32(item) != 0)
            let it = rd32(item)
            let actual = tb_NewPtr(4, false, true)
            #expect(err("KCGetData", [it, 32, out, actual]) == 0 && rd32(actual) == 7 && rd8(out) == 0x68)
            #expect(err("KCGetData", [it, 3, out, actual]) == -25301 && rd32(actual) == 7)
            // The class and creator are attributes already; the dates can't be set
            let attr = tb_NewPtr(12, false, true), value = tb_NewPtr(16, false, true)
            wr32(attr, fourCC("crtr")); wr32(attr + 4, 4); wr32(attr + 8, value)
            #expect(err("KCGetAttribute", [it, attr, actual]) == 0 && rd32(actual) == 4 && rd32(value) == fourCC("Test"))
            wr32(attr, fourCC("clas"))
            #expect(err("KCSetAttribute", [it, attr]) == -25309)
            for (i, b) in Array("scott".utf8).enumerated() { wr8(value + UInt32(i), b) }
            wr32(attr, fourCC("acct")); wr32(attr + 4, 5)
            #expect(err("KCSetAttribute", [it, attr]) == 0)
            wr32(attr + 4, 16)
            #expect(err("KCGetAttribute", [it, attr, actual]) == 0 && rd32(actual) == 5 && rd8(value) == 0x73)
            wr32(attr, fourCC("nope"))
            #expect(err("KCGetAttribute", [it, attr, actual]) == -25303)
            wr32(attr, fourCC("svce"))
            #expect(err("KCGetAttribute", [it, attr, actual]) == -25303) // not set yet
            // New data
            wr8(data, 0x41)
            #expect(err("KCSetData", [it, 1, data]) == 0)
            #expect(err("KCGetData", [it, 32, out, actual]) == 0 && rd32(actual) == 1 && rd8(out) == 0x41)
            #expect(err("KCSetData", [it, 0x20000, data]) == -25302)
            // There is nowhere to put it.
            #expect(err("KCAddItem", [it]) == noDefault)
            #expect(err("KCUpdateItem", [it]) == noDefault)
            #expect(err("KCDeleteItem", [it]) == -25300)
            let kc = tb_NewPtr(4, false, true)
            #expect(err("KCGetKeychain", [it, kc]) == -25294)
            #expect(err("KCCopyItem", [it, 0, kc]) == noDefault)
            #expect(err("KCReleaseItem", [item]) == 0 && rd32(item) == 0)
            #expect(err("KCReleaseItem", [item]) == -25304)
            #expect(err("KCAddItem", [it]) == -25304) // released
        }

        @Test func certificatesAndPolicies() {
            #expect(err("KCFindX509Certificates", [0, 0, 0, 0, 0]) == -25313)
            #expect(err("KCChooseCertificate", [0, 0, 0, 0]) == -25313)
            #expect(err("KCVerifyX509CertificateGroup", [0, 0, 0]) == -25314)
        }
    }
}
