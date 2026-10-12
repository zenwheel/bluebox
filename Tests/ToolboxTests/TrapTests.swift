import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

extension Classic {
    /// Hand-assembled 68k programs exercising the trap dispatcher.
    @Suite struct TrapDispatch {
        init() { bootMachine() }

        var cpu: UnsafeMutablePointer<cpu68k> { g_cpu! }

        /// Load code into the heap with a fresh stack and run it until it halts.
        func run(_ words: [UInt16], max: UInt64 = 10_000) -> String {
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let stack = tb_NewPtr(65536, false, true)
            cpu.pointee.a.7 = stack + 65536 - 16
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, max)
            return String(cString: machine_halt_reason())
        }

        func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
        func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }

        @Test func toolboxTrapWithPascalArguments() {
            var fork = buildFork([TestResource(type: "TEST", id: 128, data: [1, 2, 3])])
            #expect(rm_open_host_file(nil, &fork, UInt32(fork.count), true) > 0)
            let t = fourCC("TEST")
            let reason = run([
                0x42A7,                         // CLR.L -(SP)        result space
                0x2F3C, hi(t), lo(t),           // MOVE.L #'TEST',-(SP)
                0x3F3C, 0x0080,                 // MOVE.W #128,-(SP)
                0xA9A0,                         // _GetResource
                0x2E1F,                         // MOVE.L (SP)+,D7
                0xA9F4,                         // _ExitToShell
            ])
            #expect(reason == "ExitToShell")
            let h = cpu.pointee.d.7
            #expect(h != 0)
            #expect(tb_GetHandleSize(h) == 3)
        }

        @Test func osTrapPreservesRegistersAndReturnsInA0() {
            let reason = run([
                0x227C, 0x1234, 0x5678,         // MOVEA.L #$12345678,A1
                0x7405,                         // MOVEQ #5,D2
                0x203C, 0x0000, 0x0064,         // MOVE.L #100,D0
                0xA122,                         // _NewHandle
                0x2E08,                         // MOVE.L A0,D7
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(cpu.pointee.d.0 == 0)               // noErr in D0
            #expect(cpu.pointee.a.1 == 0x1234_5678)     // A1 preserved
            #expect(cpu.pointee.d.2 == 5)               // D2 preserved
            #expect(tb_GetHandleSize(cpu.pointee.d.7) == 100)
        }

        @Test func osTrapWithoutA0ResultPreservesA0() {
            let h = tb_NewHandle(77, false, false)
            let reason = run([
                0x207C, hi(h), lo(h),           // MOVEA.L #h,A0
                0xA025,                         // _GetHandleSize
                0x2E00,                         // MOVE.L D0,D7
                0x2C08,                         // MOVE.L A0,D6
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(cpu.pointee.d.7 == 77)
            #expect(cpu.pointee.d.6 == h)
        }

        @Test func patchedToolboxTrapRunsGuestCodeThenTheOriginal() {
            let original = tb_GetTrapAddress(0xA975, true, true) // TickCount
            let patch = tb_NewPtr(16, false, false)
            let words: [UInt16] = [0x5286, 0x4EF9, hi(original), lo(original)] // ADDQ.L #1,D6; JMP orig
            for (i, w) in words.enumerated() { wr16(patch + UInt32(i * 2), w) }
            tb_SetTrapAddress(patch, 0xA975, true, true)
            wr32(UInt32(LM_Ticks), 1234)
            let reason = run([
                0x7C00,                         // MOVEQ #0,D6
                0x42A7,                         // CLR.L -(SP)
                0xA975,                         // _TickCount
                0x2E1F,                         // MOVE.L (SP)+,D7
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(cpu.pointee.d.6 == 1)
            #expect(cpu.pointee.d.7 == 1234)
        }

        @Test func patchedOSTrapGoesThroughTheDispatcherFrame() {
            let original = tb_GetTrapAddress(0xA025, true, false) // GetHandleSize
            let patch = tb_NewPtr(16, false, false)
            let words: [UInt16] = [0x5286, 0x4EF9, hi(original), lo(original)]
            for (i, w) in words.enumerated() { wr16(patch + UInt32(i * 2), w) }
            tb_SetTrapAddress(patch, 0xA025, true, false)
            let h = tb_NewHandle(9, false, false)
            let reason = run([
                0x7C00,                         // MOVEQ #0,D6
                0x7205,                         // MOVEQ #5,D1
                0x207C, hi(h), lo(h),           // MOVEA.L #h,A0
                0xA025,                         // _GetHandleSize (patched)
                0x2E00,                         // MOVE.L D0,D7
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(cpu.pointee.d.6 == 1)
            #expect(cpu.pointee.d.7 == 9)
            #expect(cpu.pointee.d.1 == 5) // restored by the dispatcher epilogue
        }

        @Test func unimplementedTrapsShareTheUnimplementedAddress() {
            let unimplemented = tb_GetTrapAddress(0xA89F, true, true)
            #expect(tb_GetTrapAddress(0xA86E, true, true) == unimplemented) // InitGraf, for now
            #expect(tb_GetTrapAddress(0xA9A0, true, true) != unimplemented) // GetResource
            // Old-style numbering: 0x46 is the OS trap GetTrapAddress itself.
            #expect(tb_GetTrapAddress(0x46, false, false) != unimplemented)
        }

        @Test func unimplementedTrapHaltsWithItsName() {
            let reason = run([0xA86E]) // _InitGraf
            #expect(reason.contains("InitGraf"))
        }
    }

    @Suite struct Launch {
        @Test func synthesizedApplicationRunsThroughTheJumpTable() throws {
            bootMachine()
            let jt = cat(
                be16(0), be16(0x3F3C), be16(1), be16(0xA9F0),     // entry 0: seg 1, offset 0
                be16(8), be16(0x3F3C), be16(1), be16(0xA9F0))     // entry 1: seg 1, offset 8
            let code0 = cat(be32(48), be32(256), be32(16), be32(32), jt)
            let code1 = cat(
                be16(0), be16(2),                                 // first entry 0, 2 entries
                be16(0x702A),                                     // MOVEQ #42,D0
                be16(0x4EAD), be16(42),                           // JSR 42(A5): entry 1
                be16(0xA9F4),                                     // _ExitToShell
                be16(0x7207), be16(0x4E75))                       // MOVEQ #7,D1; RTS
            var fork = buildFork([
                TestResource(type: "CODE", id: 0, data: code0),
                TestResource(type: "CODE", id: 1, data: code1),
            ])
            let dir = tempDir()
            defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("Tiny").path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))

            #expect(machine_launch(path, &err, err.count))
            #expect(machine_run(10_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
            #expect(g_cpu!.pointee.d.0 == 42)
            #expect(g_cpu!.pointee.d.1 == 7)
            let a5 = rd32(UInt32(LM_CurrentA5))
            #expect(rd16(a5 + 32 + 2) == 0x4EF9) // entry 0 is now a JMP
            #expect(rd16(a5 + 40 + 2) == 0x4EF9) // and so is entry 1
        }
    }
}
