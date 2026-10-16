import CCore
import CPUPPC
import Foundation
import Testing

extension BlueBox {
    /// The PowerPC interpreter against hand-checked cases (Tests/PPC/gen_tests.py).
    @Suite struct PowerPC {
        static let codeAt: UInt32 = 0x5000_0000
        static let returnAt: UInt32 = 0x5000_F000

        func reg(_ c: ppc_cpu, _ name: String) -> UInt32? {
            switch name {
            case "cr": return c.cr
            case "xer": return c.xer
            case "ctr": return c.ctr
            case "lr": return c.lr
            default:
                if name.hasPrefix("m"), let a = UInt32(name.dropFirst(), radix: 16) { return rd32(a) }
                guard name.hasPrefix("r"), let n = Int(name.dropFirst()), n < 32 else { return nil }
                return withUnsafeBytes(of: c.gpr) { $0.load(fromByteOffset: n * 4, as: UInt32.self) }
            }
        }

        @Test(arguments: ppcCases.map(\.name))
        func instructionCase(_ name: String) {
            bootMachine()
            let t = ppcCases.first { $0.name == name }!
            for i in 0..<0x40 { wr32(0x6000_0000 + UInt32(i * 4), 0) }
            for (i, w) in t.code.enumerated() { wr32(Self.codeAt + UInt32(i * 4), w) }
            wr32(Self.returnAt, PPC_NATIVE_OP)
            var c = ppc_cpu()
            ppc_init(&c)
            c.native = { c, _ in
                guard let c else { return }
                c.pointee.halted = true
                withUnsafeMutableBytes(of: &c.pointee.halt_reason) { $0[0] = 0x64; $0[1] = 0x6F; $0[2] = 0x6E; $0[3] = 0x65; $0[4] = 0 }
            }
            c.pc = Self.codeAt
            c.lr = Self.returnAt
            for (k, v) in t.start {
                switch k {
                case "cr": c.cr = v
                case "xer": c.xer = v
                default:
                    if k.hasPrefix("m"), let a = UInt32(k.dropFirst(), radix: 16) {
                        wr32(a, v)
                    } else if let n = Int(k.dropFirst()) {
                        withUnsafeMutableBytes(of: &c.gpr) { $0.storeBytes(of: v, toByteOffset: n * 4, as: UInt32.self) }
                    }
                }
            }
            _ = ppc_run(&c, 10000)
            let reason = withUnsafeBytes(of: c.halt_reason) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            #expect(reason == "done", "\(name): \(reason)")
            for (k, v) in t.expect {
                let got = reg(c, k)
                #expect(got == v, "\(name) \(k): \(got.map { String($0, radix: 16) } ?? "?") != \(String(v, radix: 16))")
            }
        }
    }
}
