import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

/// Helpers for the Mac OS 9 remaining-exports suites (os9rest*.c).
enum Rest {
    /// Call an export: integer args from r3 (the ninth on in the parameter
    /// area), floating args from f1; r3 after.
    @discardableResult
    static func call(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> UInt32 {
        let stack = tb_NewPtr(256, true, true)
        withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: stack, toByteOffset: 4, as: UInt32.self) }
        for (i, a) in args.enumerated() {
            if i < 8 {
                withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            } else {
                wr32(stack + 24 + UInt32(4 * i), a)
            }
        }
        for (i, f) in fp.enumerated() { ppc_set_fpr(&g_ppc, Int32(1 + i), f) }
        guard let fn = cfm_export_fn(name) else {
            Issue.record("\(name) is not exported")
            return 0xFFFF_FFFF
        }
        fn(&g_ppc)
        tb_DisposePtr(stack)
        return g_ppc.gpr.3
    }

    static func signed(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> Int32 {
        Int32(bitPattern: call(name, args, fp: fp))
    }

    static func b32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 0xFF), UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }
    static func b16(_ v: UInt16) -> [UInt8] { [UInt8(v >> 8), UInt8(v & 0xFF)] }

    static func f1() -> Double { ppc_fpr(&g_ppc, 1) }

    static func ptr(_ n: Int) -> UInt32 { tb_NewPtr(Int32(n), true, true) }

    static func cstr(_ s: String) -> UInt32 {
        let b = Array(s.utf8) + [0]
        let p = tb_NewPtr(Int32(b.count), true, true)
        for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
        return p
    }

    static func string(_ p: UInt32) -> String {
        var s = [UInt8]()
        var i: UInt32 = 0
        while rd8(p + i) != 0 { s.append(rd8(p + i)); i += 1 }
        return String(decoding: s, as: UTF8.self)
    }

    static func wrDouble(_ p: UInt32, _ d: Double) {
        let b = d.bitPattern
        wr32(p, UInt32(b >> 32))
        wr32(p + 4, UInt32(b & 0xFFFF_FFFF))
    }

    static func rdDouble(_ p: UInt32) -> Double { Double(bitPattern: UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4))) }

    static func words(_ d: Double) -> [UInt32] {
        [UInt32(d.bitPattern >> 32), UInt32(d.bitPattern & 0xFFFF_FFFF)]
    }
}
