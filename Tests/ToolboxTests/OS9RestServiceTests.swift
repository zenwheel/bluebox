import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The small service libraries' remaining names: KeychainLib's array
    /// callbacks and start-up routines.
    @Suite struct OS9RestServices {
        init() {
            bootMachine()
            _ = cfm_ppc_cpu()
        }

        func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

        @Test func keychainItemsInAnArray() {
            let item = Rest.ptr(4), data = Rest.ptr(8)
            #expect(Rest.signed("KCNewItem", [fourCC("genp"), fourCC("Test"), 4, data, item]) == 0)
            let it = rd32(item)
            let cb = ppc_data_export("KCItemRefArrayCallBacks")
            #expect(cb != 0 && rd32(cb) == 0 && rd32(cb + 4) != 0 && rd32(cb + 8) != 0 && rd32(cb + 16) == 0)
            #expect(ppc_data_export_known("KCItemRefArrayCallBacks"))
            let array = Rest.call("CFArrayCreateMutable", [0, 0, cb])
            Rest.call("CFArrayAppendValue", [array, it])
            // the array holds the item: its maker's release leaves it be
            #expect(Rest.signed("KCReleaseItem", [item]) == 0 && rd32(item) == 0)
            let actual = Rest.ptr(4), out = Rest.ptr(8)
            #expect(Rest.signed("KCGetData", [it, 8, out, actual]) == 0 && rd32(actual) == 4)
            Rest.call("CFRelease", [array])
            #expect(Rest.signed("KCGetData", [it, 8, out, actual]) == -25304) // and now it is gone
        }

        @Test func iconServicesSwitchesAndAliases() {
            let p = Rest.ptr(2)
            #expect(Rest.signed("IconServicesInit", [0]) == 0)
            Rest.call("IconServicesTerminate")
            #expect(Rest.signed("GetDeepIconsEnabled", [p]) == 0 && rd8(p) == 1)
            #expect(Rest.signed("SetDeepIconsEnabled", [0]) == 0)
            #expect(Rest.signed("GetDeepIconsEnabled", [p]) == 0 && rd8(p) == 0)
            func same(_ a: String, _ b: String) -> Bool {
                guard let x = cfm_export_fn(a), let y = cfm_export_fn(b) else { return false }
                return unsafeBitCast(x, to: Int.self) == unsafeBitCast(y, to: Int.self)
            }
            #expect(same("CloneIconRef", "AcquireIconRef") && same("DisposeIconRef", "ReleaseIconRef"))
        }

        @Test func keychainStartAndStop() {
            #expect(Rest.signed("init", [0]) == 0)
            Rest.call("term")
            Rest.call("__som_check_ev")
            Rest.call("__som_check_new")
        }
    }
}
