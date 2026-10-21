import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

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
            runCase(name)
        }

        /// The same cases through the JIT (where the host has one).
        @Test(arguments: ppcCases.map(\.name))
        func instructionCaseJIT(_ name: String) {
            guard ppc_jit_available() else { return }
            ppc_jit_enable(true, nil, nil)
            defer { ppc_jit_enable(false, nil, nil) }
            runCase(name)
        }

        /// Random blocks of translated instructions: the interpreter and the
        /// JIT from the same state end with the same registers and memory.
        @Test func jitMatchesInterpreterOnRandomBlocks() {
            guard ppc_jit_available() else { return }
            bootMachine()
            var why = [CChar](repeating: 0, count: 1024)
            var before = ppc_jit_stats()
            ppc_jit_get_stats(&before)
            let bad = ppc_jit_selftest(0x1234_5678, 10000, Self.codeAt, 0x6000_0000, &why, Int32(why.count))
            #expect(bad == 0, "\(bad) mismatches; first: \(String(cString: why))")
            var after = ppc_jit_stats()
            ppc_jit_get_stats(&after)
            #expect(after.translated - before.translated >= 10000 && after.jitted > before.jitted)
        }

        func runCase(_ name: String) {
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

extension BlueBox {
    /// A PowerPC application built by Retro68 (Tests/Probes/PPC/build.sh): its
    /// fragment loads, its InterfaceLib calls work, and its integer and
    /// floating-point results match the host's (computed from the same
    /// source with fused multiply-add, as GCC does for PowerPC).  Skipped
    /// when the probe hasn't been built.
    @Suite struct PowerPCApplication {
        /// A NumVersion result goes through r3 when it points into the
        /// caller's stack, and comes back in r3 either way.
        @Test func smallStructResultsBothWays() {
            bootMachine()
            var c = ppc_cpu()
            ppc_init(&c)
            func setGPR(_ n: Int, _ v: UInt32) {
                withUnsafeMutableBytes(of: &c.gpr) { $0.storeBytes(of: v, toByteOffset: n * 4, as: UInt32.self) }
            }
            func gpr(_ n: Int) -> UInt32 { withUnsafeBytes(of: c.gpr) { $0.load(fromByteOffset: n * 4, as: UInt32.self) } }
            setGPR(1, 0x6000_0100)
            wr32(0x6000_0140, 0)
            ppc_struct_result(&c, 0x6000_0140, 0x0321_8000, 4)
            #expect(rd32(0x6000_0140) == 0x0321_8000 && gpr(3) == 0x0321_8000)
            let low = rd32(0)
            ppc_struct_result(&c, 1, 0x0321_8000, 4) // not a pointer: r3 only
            #expect(rd32(0) == low && gpr(3) == 0x0321_8000)
        }

        @Test func ppcHelloRunsAndComputes() throws {
            let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent()
            let built = root.appendingPathComponent(".cache/ppc-probes/PPCHello")
            let data = built.appendingPathComponent("PPCHello.ad"), sidecar = built.appendingPathComponent("%PPCHello.ad")
            guard FileManager.default.fileExists(atPath: sidecar.path) else { return }
            // A copy of the AppleDouble pair in a folder of its own.
            let dir = tempDir()
            let app = dir.appendingPathComponent("PPCHello")
            try FileManager.default.copyItem(at: data, to: app)
            try FileManager.default.copyItem(at: sidecar, to: dir.appendingPathComponent("._PPCHello"))
            bootMachine()
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(app.path, &err, err.count), "\(String(cString: err))")
            _ = machine_run(5_000_000)
            #expect(String(cString: machine_halt_reason()).hasPrefix("ExitToShell"))
            let results = try String(contentsOf: dir.appendingPathComponent("PPC Results"), encoding: .macOSRoman)
            #expect(results == "int BBEA2171 82AFE3EB 1959EDC1\rfloat 411FBE77 8195B1A6 47284222 00000000\r")
        }
    }
}
