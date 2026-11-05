import CPU68K
import CPUTestSupport
import Foundation
import Testing

extension BlueBox {
    /// Runs the SingleStepTests 68000 vectors, if Tests/fetch-cpu-tests.sh has fetched them.
    @Suite struct CPUSingleStepTests {
        static let cacheDir: URL = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent(".cache/sst")

        static var files: [String] {
            let names = (try? FileManager.default.contentsOfDirectory(atPath: cacheDir.path)) ?? []
            return names.filter { $0.hasSuffix(".json") }.sorted()
        }

        @Test("68000 single-step vectors", arguments: files)
        func vectors(file: String) throws {
            var result = sst_result()
            let path = Self.cacheDir.appendingPathComponent(file).path
            #expect(sst_run_file(path, &result), "could not parse \(file)")
            let failure = withUnsafeBytes(of: &result.first_failure) { raw in
                String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
            }
            #expect(result.failed == 0, "\(file): \(result.failed)/\(result.total) failed; first: \(failure)")
        }

        /// The same vectors through the 68k JIT, each instruction a block of
        /// its own (instructions it doesn't translate go to the interpreter).
        @Test("68000 single-step vectors through the JIT", arguments: files)
        func vectorsJIT(file: String) throws {
            guard cpu_jit_available() else { return }
            sst_jit = true
            defer { sst_jit = false }
            var result = sst_result()
            let path = Self.cacheDir.appendingPathComponent(file).path
            #expect(sst_run_file(path, &result), "could not parse \(file)")
            let failure = withUnsafeBytes(of: &result.first_failure) { raw in
                String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
            }
            #expect(result.failed == 0, "\(file): \(result.failed)/\(result.total) failed; first: \(failure)")
        }
    }
}

extension BlueBox {
    /// The 68k JIT against the interpreter on random blocks of the
    /// instructions it translates: the same registers, SR and memory after.
    @Suite struct M68KJIT {
        /// A store into the block being run (an immediate operand ahead,
        /// patched) takes effect, as under the interpreter.
        @Test func jitSeesCodeItStoresAhead() {
            guard cpu_jit_available() else { return }
            bootMachine()
            let at: UInt32 = 0x5000_0000
            // MOVE.W #5,(at+14).L; NOP; NOP; ADDI.W #0,D1; NOP; BRA *
            let code: [UInt16] = [0x33FC, 0x0005, 0x5000, 0x000E, 0x4E71, 0x4E71, 0x0641, 0x0000, 0x4E71, 0x60FE]
            for jit in [false, true] {
                for (i, w) in code.enumerated() { wr16(at + UInt32(i * 2), w) }
                var c = cpu68k()
                cpu_init(&c)
                c.model = 68040
                c.pc = at
                cpu_jit_enabled = jit
                _ = cpu_run(&c, 6)
                cpu_jit_enabled = false
                #expect(c.d.1 == 5 && c.icount == 6, "jit \(jit): d1 \(c.d.1), \(c.icount) instructions")
            }
        }

        @Test func jitMatchesInterpreterOnRandomBlocks() {
            guard cpu_jit_available() else { return }
            bootMachine()
            var before = cpu_jit_stats()
            cpu_jit_get_stats(&before)
            var why = [CChar](repeating: 0, count: 1024)
            let bad = cpu_jit_selftest(0x2468_ACE1, 2000, 0x5000_0000, 0x6000_0000, &why, Int32(why.count))
            #expect(bad == 0, "\(bad) mismatches; first: \(String(cString: why))")
            var after = cpu_jit_stats()
            cpu_jit_get_stats(&after)
            #expect(after.translated > before.translated && after.jitted > before.jitted)
        }
    }
}

extension BlueBox {
    /// A 68000 counts its clock cycles as the User's Manual's tables give
    /// them (PLAN M22's timing): what a program timing a loop by the tick saw.
    @Suite struct M68000Timing {
        @Test func cyclesFollowTheManualsTables() {
            bootMachine()
            let at: UInt32 = 0x5000_0000
            // MOVEQ #9,D0; loop: CLR.L (A0)+; DBRA D0,loop; ADD.L D1,D0; MULU.W D1,D0; BRA *
            let code: [UInt16] = [0x7009, 0x4298, 0x51C8, 0xFFFC, 0xD081, 0xC0C1, 0x60FE]
            for (i, w) in code.enumerated() { wr16(at + UInt32(i * 2), w) }
            var c = cpu68k()
            cpu_init(&c)
            cpu_set_model(&c, 68000, 0)
            c.pc = at
            c.a.0 = at + 0x100
            var spent: [UInt64] = []
            func run(_ n: UInt64) {
                let before = c.cycles
                _ = cpu_run(&c, n)
                spent.append(c.cycles - before)
            }
            run(21) // MOVEQ 4; ten CLR.L (A0)+ at 20; nine DBRA taken at 10, the last 14
            run(1)  // ADD.L Dn,Dn: 8
            run(1)  // MULU.W: 70 (38 + 2n; Blue Box takes n as 16)
            run(1)  // BRA.B taken: 10
            #expect(spent == [308, 8, 70, 10], "cycles \(spent)")
            #expect(c.timed == c.icount)
        }
    }
}
