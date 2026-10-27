import CCore
import CPU68K
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// Programs the walk of the archive found going round for ever: calls
    /// that answered so that a program's loop never ended.
    @Suite struct WalkHangs {
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

        /// Open Transport lists no ports: OTGetIndexedPort says false
        /// (IPNetMonitor lists them until it does), as does OTFindPort.
        @Test func openTransportListsNoPorts() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let record = tb_NewPtr(400, false, true)
            #expect(call("OTGetIndexedPort", [record, 0]) & 0xFF == 0)
            let name = tb_NewPtr(8, false, true)
            #expect(call("OTFindPort", [record, name]) & 0xFF == 0)
        }

        /// OTUtilityLib's string and memory helpers do what they say
        /// (IPNetRouter compares names with OTStrEqual and OTMemcmp, which
        /// said "not supported", taken as true).
        @Test func openTransportStringHelpers() {
            bootMachine()
            _ = cfm_ppc_cpu()
            func cstr(_ s: String) -> UInt32 {
                let p = tb_NewPtr(64, false, true)
                for (i, b) in Array(s.utf8).enumerated() { wr8(p + UInt32(i), b) }
                return p
            }
            func read(_ p: UInt32) -> String {
                var bytes: [UInt8] = []
                var i: UInt32 = 0
                while rd8(p + i) != 0 { bytes.append(rd8(p + i)); i += 1 }
                return String(decoding: bytes, as: UTF8.self)
            }
            let a = cstr("enet"), b = cstr("enet"), d = cstr("")
            #expect(call("OTStrEqual", [a, b]) & 0xFF != 0)
            #expect(call("OTStrEqual", [a, cstr("ene")]) & 0xFF == 0)
            #expect(call("OTMemcmp", [a, b, 4]) & 0xFF != 0)
            #expect(call("OTMemcmp", [a, cstr("enXt"), 4]) & 0xFF == 0)
            call("OTStrCopy", [d, a])
            call("OTStrCat", [d, cstr("0")])
            #expect(read(d) == "enet0")
        }
    }
}
