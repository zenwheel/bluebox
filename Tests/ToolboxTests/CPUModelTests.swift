import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

/// Boot as `model` (with `fpu`, -1 for its own), run `body`, then boot the
/// default machine again.
func withModel(_ model: Int32, fpu: Int32 = -1, _ body: () -> Void) {
    machine_set_cpu(model)
    machine_set_fpu(fpu)
    bootMachine()
    body()
    machine_set_cpu(68040)
    machine_set_fpu(-1)
    bootMachine()
}

/// Run code built for the address it is put at (ending in _ExitToShell),
/// in supervisor mode with a fresh stack; returns the halt reason.
@discardableResult
func runAt(_ build: (UInt32) -> [UInt16], max: UInt64 = 100_000) -> String {
    let code = tb_NewPtr(1024, false, true)
    let words = build(code)
    for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
    let stack = tb_NewPtr(65536, false, true)
    let cpu = g_cpu!
    cpu.pointee.a.7 = stack + 65536 - 16
    cpu.pointee.pc = code
    cpu.pointee.halted = false
    cpu.pointee.stopped = false
    _ = cpu_run(cpu, max)
    return String(cString: machine_halt_reason())
}

/// A vector pointing at a STOP, so the frame stays to be read.
func catchVector(_ n: UInt32) {
    let stop = tb_NewPtr(4, true, false)
    wr16(stop, 0x4E72)
    wr16(stop + 2, 0x2700)
    wr32(n * 4, stop)
}

/// The end of code whose stack is to be read after (a trap would use it).
let stopWords: [UInt16] = [0x4E72, 0x2700]

func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }

extension BlueBox {
    /// M22's processor models: the 68000's own behaviour, the 68020's and
    /// 68030's instruction caches, the FPU choices, and what the machine
    /// reports for each.
    @Suite struct CPUModels {
        @Test func the68000TakesAddressErrors() {
            withModel(68000) {
                #expect(g_mem_24bit && g_cpu!.pointee.model == 68000)
                catchVector(3)
                var at: UInt32 = 0
                let reason = runAt { code in
                    at = code + 6
                    return [0x207C, 0x0012, 0x3457,  // MOVEA.L #$123457,A0
                            0x3010,                  // MOVE.W (A0),D0: odd
                            0xA9F4]
                }
                #expect(g_cpu!.pointee.stopped, "\(reason)")
                let sp = g_cpu!.pointee.a.7
                // Status: the instruction word's upper bits, a read, not an
                // instruction fetch, supervisor data.
                #expect(rd16(sp) == (0x3010 & 0xFFE0) | 0x10 | 5)
                #expect(rd32(sp + 2) == 0x0012_3457)  // the access
                #expect(rd16(sp + 6) == 0x3010)       // the instruction
                #expect(rd16(sp + 8) == 0x2000)       // the SR
                #expect(rd32(sp + 10) == at)          // the last word fetched
                #expect(g_cpu!.pointee.last_vector == 3)
            }
        }

        @Test func the68000FaultsJumpingToAnOddAddress() {
            withModel(68000) {
                catchVector(3)
                let reason = runAt { _ in [0x4EF9, 0x0012, 0x3457] }  // JMP $123457
                #expect(g_cpu!.pointee.stopped, "\(reason)")
                let sp = g_cpu!.pointee.a.7
                #expect(rd16(sp) == (0x4EF9 & 0xFFE0) | 0x10 | 0x08 | 6)  // a fetch, supervisor program
                #expect(rd32(sp + 2) == 0x0012_3457 && rd32(sp + 10) == 0x0012_3453)
            }
            // A 68000 frame's vector is the CPU's to say, and Blue Box's own
            // handler names it.
            withModel(68000) {
                #expect(runAt { _ in [0x207C, 0x0000, 0x0101, 0x2010, 0xA9F4] }.hasPrefix("address error (vector 3)"))
            }
        }

        @Test func the68000LacksLaterInstructions() {
            // Each is illegal on the 68000 and runs on the 68020.
            let later: [[UInt16]] = [
                [0x49C0],                  // EXTB.L D0
                [0x4C00, 0x0800],          // MULS.L D0,D0
                [0xE9C0, 0x0008],          // BFEXTU D0{0:8},D0
                [0x4808, 0x0000, 0x0000],  // LINK.L A0,#0
                [0x51FC],                  // TRAPF
                [0x4E7A, 0x0801],          // MOVEC VBR,D0
                [0x4100],                  // CHK.L D0,D0
                [0x4A48],                  // TST.W A0
                [0x8141, 0x0000],          // PACK D1,D0,#0
                [0x0AD0, 0x0000],          // CAS.B D0,D0,(A0)
                [0x42C0],                  // MOVE CCR,D0
            ]
            for code in later {
                withModel(68000) {
                    #expect(runAt { _ in [0x41EF, 0xFFF0] + code + [0xA9F4] }.hasPrefix("illegal instruction (vector 4)"),
                            "\(code) on a 68000")
                }
                withModel(68020) {
                    #expect(runAt { _ in [0x41EF, 0xFFF0, 0x7001] + code + [0xA9F4] } == "ExitToShell", "\(code) on a 68020")
                }
            }
        }

        @Test func moveFromSRAndExceptionFrames() {
            // MOVE SR,D0 in user mode: the 68000's own; privileged from the 68010 on.
            let user: [UInt16] = [0x46FC, 0x0000, 0x40C0, 0xA9F4]  // MOVE #0,SR; MOVE SR,D0
            withModel(68000) { #expect(runAt { _ in user } == "ExitToShell") }
            withModel(68020) { #expect(runAt { _ in user }.hasPrefix("privilege violation (vector 8)")) }
            // Zero divide: the 68000's three words, the 68020's format 2.
            withModel(68000) {
                catchVector(5)
                var code0: UInt32 = 0
                runAt { code in code0 = code; return [0x7000, 0x80C0, 0xA9F4] }  // DIVU D0,D0
                #expect(g_cpu!.pointee.stopped)
                let sp = g_cpu!.pointee.a.7
                #expect(rd16(sp) & 0x2000 != 0 && rd32(sp + 2) == code0 + 4)  // SR and PC, no format word
            }
            withModel(68020) {
                catchVector(5)
                var code0: UInt32 = 0
                runAt { code in code0 = code; return [0x7000, 0x80C0, 0xA9F4] }
                #expect(g_cpu!.pointee.stopped)
                let sp = g_cpu!.pointee.a.7
                #expect(rd16(sp + 6) == 0x2014)       // format 2, vector 5
                #expect(rd32(sp + 8) == code0 + 2)    // the instruction's address
                #expect(rd32(sp + 2) == code0 + 4)    // and where it goes on
            }
            // RTE takes a format 2 frame back.
            withModel(68020) {
                let reason = runAt { code in
                    [0x2F3C, hi(code + 16), lo(code + 16),    // MOVE.L #code+16,-(SP): instruction address
                     0x3F3C, 0x2014,                         // MOVE.W #$2014,-(SP)
                     0x2F3C, hi(code + 24), lo(code + 24),   // MOVE.L #code+24,-(SP): the return
                     0x3F3C, 0x2700,                         // MOVE.W #$2700,-(SP)
                     0x4E73, 0x4E71,                         // RTE; NOP
                     0xA9F4]
                }
                #expect(reason == "ExitToShell")
            }
        }

        /// The 68020's and 68030's instruction caches: code a program
        /// changes itself runs as it was until the cache is flushed; the
        /// Toolbox's own stores (BlockMove) flush what they cover; the
        /// 68000 and the 68040 here see the change at once.
        @Test func instructionCaches() {
            func selfModifying(_ code: UInt32) -> [UInt16] {
                let target = code + 60
                var w: [UInt16] = [
                    0x6100, UInt16(target - (code + 2)),            // BSR target (cached)
                    0x33FC, 0x7E02, hi(target), lo(target),         // MOVE.W #$7E02,target: MOVEQ #2,D7
                    0x6100, UInt16(target - (code + 14)),           // BSR target
                    0x2C07,                                         // MOVE.L D7,D6
                    0x7001, 0xA198,                                 // FlushInstructionCache
                    0x6100, UInt16(target - (code + 24)),           // BSR target
                    0x2A07,                                         // MOVE.L D7,D5
                    // BlockMove MOVEQ #3,D7 over it: the Toolbox's store.
                    0x41FA, UInt16(58 - 30),                        // LEA src(PC),A0 (code+58)
                    0x227C, hi(target), lo(target),                 // MOVEA.L #target,A1
                    0x7002, 0xA02E,                                 // MOVEQ #2,D0; _BlockMove
                    0x6100, UInt16(target - (code + 44)),           // BSR target
                    0x2807,                                         // MOVE.L D7,D4
                    0xA9F4,
                ]
                while w.count < 29 { w.append(0x4E71) }
                w.append(0x7E03)                                    // src (code+58): MOVEQ #3,D7
                w += [0x7E01, 0x4E75]                               // target (code+60): MOVEQ #1,D7; RTS
                return w
            }
            for (model, stale) in [(Int32(68020), true), (68030, true), (68000, false), (68040, false)] {
                withModel(model) {
                    #expect(runAt(selfModifying) == "ExitToShell")
                    let c = g_cpu!.pointee
                    #expect(c.d.6 == (stale ? 1 : 2), "\(model): the changed code before a flush")
                    #expect(c.d.5 == 2 && c.d.4 == 3, "\(model): after FlushInstructionCache and BlockMove")
                }
            }
            // With the cache off (CACR), the 68020 sees its change at once.
            withModel(68020) {
                g_cpu!.pointee.cacr = 0
                #expect(runAt(selfModifying) == "ExitToShell" && g_cpu!.pointee.d.6 == 2)
            }
        }

        @Test func cacheControlRegisters() {
            let probe: [UInt16] = [0x70FF, 0x4E7B, 0x0002, 0x4E7A, 0x1002, 0xA9F4]  // MOVEQ #-1,D0; MOVEC D0,CACR; MOVEC CACR,D1
            for (model, kept) in [(Int32(68020), UInt32(0x3)), (68030, 0x3313), (68040, 0x8000_8000)] {
                withModel(model) {
                    #expect(runAt { _ in probe } == "ExitToShell")
                    #expect(g_cpu!.pointee.d.1 == kept, "\(model)")
                }
            }
            // CAAR is the 68020's and 68030's.
            withModel(68030) { #expect(runAt { _ in [0x4E7A, 0x0802, 0xA9F4] } == "ExitToShell") }
            withModel(68040) { #expect(runAt { _ in [0x4E7A, 0x0802, 0xA9F4] }.hasPrefix("illegal instruction")) }
            // At boot the caches are on, as the ROM left them.
            withModel(68030) { #expect(g_cpu!.pointee.cacr == 0x3111) }
            withModel(68040) { #expect(g_cpu!.pointee.cacr == 0x8000_8000) }
        }

        @Test func pmmuRegistersOnThe68030() {
            withModel(68030) {
                let buf = tb_NewPtr(16, false, true)
                wr32(buf, 0x8100_C060)
                let reason = runAt { _ in
                    [0x41F9, hi(buf), lo(buf),           // LEA buf,A0
                     0xF010, 0x4000,                     // PMOVE (A0),TC
                     0xF028, 0x4200, 0x0004,             // PMOVE TC,4(A0)
                     0xF010, 0x2400,                     // PFLUSHA
                     0xA9F4]
                }
                #expect(reason == "ExitToShell")
                #expect(rd32(buf + 4) == 0x8100_C060)
            }
            withModel(68020) {
                #expect(runAt { _ in [0xF010, 0x4000, 0xA9F4] }.hasPrefix("F-line"))  // no PMMU on a Mac II
            }
        }

        @Test func floatingPointUnits() {
            let fadd: [UInt16] = [0xF200, 0x0022, 0xA9F4]  // FADD.X FP0,FP0
            withModel(68020) { #expect(runAt { _ in fadd }.hasPrefix("F-line (no FPU) (vector 11)")) }
            withModel(68000) { #expect(runAt { _ in fadd }.hasPrefix("F-line")) }
            withModel(68040, fpu: 0) { #expect(runAt { _ in fadd }.hasPrefix("F-line")) }  // a 68LC040
            withModel(68020, fpu: 68881) { #expect(runAt { _ in fadd } == "ExitToShell") }
            // FSAVE's idle frame for each: its size in the format word, so
            // FRESTORE takes the same back.
            let save: [UInt16] = [0x2C4F, 0xF327, 0xF35F, 0x2A4F] + stopWords  // MOVEA.L SP,A6; FSAVE -(SP); FRESTORE (SP)+; MOVEA.L SP,A5
            for (model, fpu, word) in [(Int32(68020), Int32(68881), UInt32(0x1F18_0000)), (68030, 68882, 0x1F38_0000),
                                       (68040, 68040, 0x4100_0000)] {
                withModel(model, fpu: fpu) {
                    runAt { _ in save }
                    let c = g_cpu!.pointee
                    #expect(c.stopped)
                    let frame = c.a.6 - 4 - ((word >> 16) & 0xFF)
                    #expect(rd32(frame) == word, "\(fpu)")
                    #expect(c.a.5 == c.a.6, "\(fpu): FRESTORE took the frame back")
                }
            }
            // The single- and double-rounding forms are the 68040's.
            let fsadd: [UInt16] = [0xF200, 0x0062, 0xA9F4]  // FSADD.X FP0,FP0
            withModel(68030) { #expect(runAt { _ in fsadd }.hasPrefix("F-line")) }
            withModel(68040) { #expect(runAt { _ in fsadd } == "ExitToShell") }
        }

        /// Gestalt, SysEnvirons and the low-memory flags agree on the
        /// machine each model stands for.
        @Test func machineReportsTheModel() {
            func gestalt(_ s: String) -> UInt32 {
                var r: UInt32 = 0
                #expect(tb_Gestalt(Int32(bitPattern: fourCC(s)), &r) == 0)
                return r
            }
            let table: [(Int32, Int32, UInt32, UInt32, UInt32, UInt32, UInt8, UInt16)] = [
                // model, fpu, proc, fpu, mmu, mach, CPUFlag, HWCfgFlags
                (68000, -1, 1, 0, 0, 5, 0, 0xE400),
                (68020, -1, 3, 0, 1, 6, 2, 0xEC00),
                (68020, 68881, 3, 1, 1, 6, 2, 0xFC00),
                (68030, -1, 4, 2, 3, 11, 3, 0xFC00),
                (68040, -1, 5, 3, 4, 35, 4, 0xFC00),
                (68040, 0, 5, 0, 4, 35, 4, 0xEC00),
            ]
            for (model, fpu, proc, gfpu, mmu, mach, cpuflag, hwcfg) in table {
                withModel(model, fpu: fpu) {
                    #expect(gestalt("proc") == proc && gestalt("fpu ") == gfpu && gestalt("mmu ") == mmu, "\(model)/\(fpu)")
                    #expect(gestalt("mach") == mach, "\(model)")
                    #expect(rd8(UInt32(LM_CPUFlag)) == cpuflag && rd16(UInt32(LM_SCSIFlags)) == hwcfg, "\(model)/\(fpu)")
                    #expect(rd8(UInt32(LM_MMUType)) == UInt8(mmu))
                    let env = tb_NewPtr(16, false, true)
                    #expect(tb_SysEnvirons(2, env) == 0)
                    #expect(rd16(env + 2) == UInt16(mach - 2) && rd16(env + 6) == UInt16(proc))  // machineType, processor
                    #expect((rd8(env + 8) != 0) == (gfpu != 0))  // hasFPU
                }
            }
        }
    }
}
