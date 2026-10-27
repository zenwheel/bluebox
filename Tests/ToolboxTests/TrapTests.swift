import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
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

        /// Jumping to ROMBase+$0A restarted a Mac before the Shutdown
        /// Manager (World Builder's games): the run ends there.
        @Test func jumpToTheROMsStartRestarts() {
            let reason = run([
                0x2078, 0x02AE,                 // MOVEA.L ROMBase,A0
                0xD0FC, 0x000A,                 // ADDA.W #$A,A0
                0x4ED0,                         // JMP (A0)
            ])
            #expect(reason.hasPrefix("restart"))
            // Running into the ROM's header from below is no restart.
            let wild = run([
                0x41F9, 0xEFFF, 0xFFF0,         // LEA $EFFFFFF0,A0
                0x4ED0,                         // JMP (A0): zeros up to the ROM
            ])
            #expect(!wild.hasPrefix("restart"))
        }

        /// A program's own A-line handler that passes traps on to the vector
        /// it found there (Lunar Commando's) has them dispatched.
        @Test func aLineHandlerPassesTrapsOn() {
            let reason = run([
                0x7A00,                         // MOVEQ #0,D5
                0x2C38, 0x0028,                 // MOVE.L $28.W,D6     the system's vector
                0x41FA, 0x0012,                 // LEA handler(PC),A0
                0x21C8, 0x0028,                 // MOVE.L A0,$28.W
                0x7064,                         // MOVEQ #100,D0
                0xA11E,                         // _NewPtr
                0x2E08,                         // MOVE.L A0,D7
                0x21C6, 0x0028,                 // MOVE.L D6,$28.W
                0xA9F4,                         // _ExitToShell
                0x5285,                         // handler: ADDQ.L #1,D5
                0x2F06,                         // MOVE.L D6,-(SP)
                0x4E75,                         // RTS: on to the system's
            ])
            #expect(reason == "ExitToShell")
            #expect(cpu.pointee.d.5 == 1)
            #expect(cpu.pointee.d.7 != 0 && tb_GetPtrSize(cpu.pointee.d.7) == 100)
        }

        /// In a PowerPC program, 68k code asking Gestalt for Open Transport
        /// is told it is missing (its client glue needs the Shared Library
        /// Manager); the PowerPC side and native callers still see it.
        @Test func openTransportHiddenFrom68kCode() {
            gestalt_powerpc()
            let otan = fourCC("otan")
            let reason = run([
                0x203C, hi(otan), lo(otan),     // MOVE.L #'otan',D0
                0xA1AD,                         // _Gestalt
                0x2E00,                         // MOVE.L D0,D7
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(Int16(truncatingIfNeeded: cpu.pointee.d.7) != 0)
            var answer: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: otan), &answer) == 0 && answer & 1 != 0)
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

        /// InsTime and InsXTime share a slot, told apart by bit $0400: an
        /// ordinary task stays ordinary (MacMan's frame timer, which went
        /// on from its last wake-up as an extended one would).
        @Test func insTimeAndInsXTimeShareASlot() {
            let tm = tb_NewPtr(Int32(TMTask_SIZEOF), false, true)
            let xtm = tb_NewPtr(Int32(TMTask_SIZEOF), false, true)
            let reason = run([
                0x207C, hi(tm), lo(tm),         // MOVEA.L #tm,A0
                0xA058,                         // _InsTime
                0x207C, hi(xtm), lo(xtm),       // MOVEA.L #xtm,A0
                0xA458,                         // _InsXTime
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(rd16(tm + 4) & 0x4000 == 0)
            #expect(rd16(xtm + 4) & 0x4000 != 0)
            tb_RmvTime(tm)
            tb_RmvTime(xtm)
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
            #expect(tb_GetTrapAddress(0xA9F3, true, true) == unimplemented) // _Chain: one app per process
            #expect(tb_GetTrapAddress(0xA9A0, true, true) != unimplemented) // GetResource
            // Old-style numbering: 0x46 is the OS trap GetTrapAddress itself.
            #expect(tb_GetTrapAddress(0x46, false, false) != unimplemented)
        }

        /// A package with no native code runs from the program's PACK
        /// resource (ResEdit's PACK 1), auto-pop or not; the System's
        /// PACK 0 is there for programs that load it, and runs the List
        /// Manager.
        @Test func packagesComeFromPACKResources() {
            var fork = buildFork([TestResource(type: "PACK", id: 1, data: [
                0x30, 0x3C, 0x00, 0x2A, // MOVE.W #42,D0
                0x4E, 0x75,             // RTS
            ])])
            #expect(rm_open_host_file(nil, &fork, UInt32(fork.count), true) > 0)
            let reason = run([
                0x7000,         // MOVEQ #0,D0
                0xA9E8,         // _Pack1
                0x3E00,         // MOVE.W D0,D7
                0x4EBA, 0x0004, // JSR glue
                0xA9F4,         // _ExitToShell
                0xADE8,         // glue: _Pack1 with auto-pop, returning to the JSR's caller
            ])
            #expect(reason == "ExitToShell")
            #expect(cpu.pointee.d.7 & 0xFFFF == 42 && cpu.pointee.d.0 & 0xFFFF == 42)
            let pack0 = tb_GetResource(Int32(bitPattern: fourCC("PACK")), 0)
            #expect(pack0 != 0 && rd16(rd32(pack0)) == 0x2F38) // MOVE.L _Pack0's address,-(SP)
        }

        /// MultiFinder's OSDispatch calls (Microsoft Word 5.0 asks for the
        /// switch information at startup) and the PPC Toolbox, which starts
        /// but opens no ports.
        @Test func multiFinderCallsAndThePPCToolbox() {
            let info = tb_NewPtr(20, false, true), pb = tb_NewPtr(64, false, true)
            let reason = run([
                0x2F3C, hi(info), lo(info),     // MOVE.L #info,-(SP)
                0x3F3C, 0x0002,                 // MOVE.W #2,-(SP)    GetSwitchInfo
                0xA88F,                         // _OSDispatch
                0x4267,                         // CLR.W -(SP)        result space
                0x2F3C, hi(info + 8), lo(info + 8),
                0x3F3C, 0x0001,                 // MOVE.W #1,-(SP)    one slot
                0x3F3C, 0x0001,                 // MOVE.W #1,-(SP)    GetActiveTaskInfo
                0xA88F,
                0x3E1F,                         // MOVE.W (SP)+,D7
                0x7000,                         // MOVEQ #0,D0        PPCInit
                0xA0DD,                         // _PPC
                0x3C00,                         // MOVE.W D0,D6
                0x7001,                         // MOVEQ #1,D0        PPCOpen
                0x207C, hi(pb), lo(pb),         // MOVEA.L #pb,A0
                0xA0DD,
                0x3A00,                         // MOVE.W D0,D5
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(rd32(info) == fourCC("TWIT"))
            #expect(cpu.pointee.d.7 & 0xFFFF == 1 && rd16(info + 8) == 1)
            #expect(cpu.pointee.d.6 & 0xFFFF == 0)
            #expect(Int16(bitPattern: UInt16(cpu.pointee.d.5 & 0xFFFF)) == -919) // badPortNameErr: no name
            #expect(Int16(bitPattern: rd16(pb + 16)) == -919)

            // Our high-level event port's name, as the Process Manager gives it.
            let psn = tb_NewPtr(8, false, true), port = tb_NewPtr(Int32(PPCPortRec_SIZEOF), false, true)
            wr32(psn + 4, 2) // kCurrentProcess
            #expect(tb_GetPortNameFromProcessSerialNumber(port, psn) == 0)
            #expect(rd16(port + UInt32(PPCPortRec_portKindsSelector_off)) == 1) // ppcByCreatorAndType
            #expect(rd32(port + UInt32(PPCPortRec_u_port_type_off)) == fourCC("ep01"))
        }

        @Test func unimplementedTrapHaltsWithItsName() {
            let reason = run([0xA9F3]) // _Chain
            #expect(reason.contains("A9F3"))
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

        /// LoadSeg gets each segment through GetResource, so a patch on it
        /// (self-expanding applications unpack their segments there) is called.
        @Test func loadSegCallsAPatchedGetResource() throws {
            bootMachine()
            let jt = cat(
                be16(0), be16(0x3F3C), be16(1), be16(0xA9F0),     // entry 0: seg 1, offset 0
                be16(0), be16(0x3F3C), be16(2), be16(0xA9F0))     // entry 1: seg 2, offset 0
            let code0 = cat(be32(48), be32(256), be32(16), be32(32), jt)
            let code1 = cat(
                be16(0), be16(1),
                be16(0x2B78), be16(0x1480), be16(0xFFF8),         // MOVE.L GetResource,-8(A5)
                be16(0x41FA), be16(16),                           // LEA patch(PC),A0
                be16(0x21C8), be16(0x1480),                       // MOVE.L A0,GetResource
                be16(0x4EAD), be16(42),                           // JSR 42(A5): entry 1, CODE 2
                be16(0x242D), be16(0xFFFC),                       // MOVE.L -4(A5),D2
                be16(0xA9F4),                                     // _ExitToShell
                be16(0x52AD), be16(0xFFFC),                       // patch: ADDQ.L #1,-4(A5)
                be16(0x2F2D), be16(0xFFF8), be16(0x4E75))         // MOVE.L -8(A5),-(SP); RTS
            let code2 = cat(be16(8), be16(1), be16(0x7209), be16(0x4E75)) // MOVEQ #9,D1; RTS
            var fork = buildFork([
                TestResource(type: "CODE", id: 0, data: code0),
                TestResource(type: "CODE", id: 1, data: code1),
                TestResource(type: "CODE", id: 2, data: code2),
            ])
            let dir = tempDir()
            defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("Patcher").path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))

            #expect(machine_launch(path, &err, err.count))
            #expect(machine_run(10_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
            #expect(g_cpu!.pointee.d.1 == 9)  // CODE 2 ran
            #expect(g_cpu!.pointee.d.2 == 1)  // through the patch, once
        }

        /// A program in a 24-bit partition that sets a segment's master
        /// pointer flags itself (Dungeon of Doom locks it so in its LoadSeg
        /// patch) still has its jump table point at the code.
        @Test func loadSegIgnoresMasterPointerFlagsIn24BitMode() throws {
            bootMachine()
            let jt = cat(
                be16(0), be16(0x3F3C), be16(1), be16(0xA9F0),     // entry 0: seg 1, offset 0
                be16(0), be16(0x3F3C), be16(2), be16(0xA9F0))     // entry 1: seg 2, offset 0
            let code0 = cat(be32(48), be32(256), be32(16), be32(32), jt)
            let code1 = cat(
                be16(0), be16(1),
                be16(0x594F),                                     // SUBQ.W #4,SP
                be16(0x2F3C), be16(0x434F), be16(0x4445),         // MOVE.L #'CODE',-(SP)
                be16(0x3F3C), be16(2),                            // MOVE.W #2,-(SP)
                be16(0xA9A0),                                     // _GetResource
                be16(0x205F),                                     // MOVEA.L (SP)+,A0
                be16(0x08D0), be16(7),                            // BSET #7,(A0): locked
                be16(0x08D0), be16(6),                            // BSET #6,(A0): purgeable
                be16(0x4EAD), be16(42),                           // JSR 42(A5): entry 1, CODE 2
                be16(0xA9F4))                                     // _ExitToShell
            let code2 = cat(be16(8), be16(1), be16(0x7209), be16(0x4E75)) // MOVEQ #9,D1; RTS
            var fork = buildFork([
                TestResource(type: "CODE", id: 0, data: code0),
                TestResource(type: "CODE", id: 1, data: code1),
                TestResource(type: "CODE", id: 2, data: code2),
            ])
            let dir = tempDir()
            defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("Flags").path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))

            #expect(machine_launch(path, &err, err.count))
            #expect(rd32(UInt32(LM_CurrentA5)) < 0x0100_0000)  // a 24-bit partition
            #expect(machine_run(10_000) == RUN_HALTED)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
            #expect(g_cpu!.pointee.d.1 == 9)  // CODE 2 ran
        }

        /// A program not marked 32-bit compatible gets its stack, A5 world
        /// and heap below 16 MB, as in 24-bit mode (Cribbage 2.5 clears the
        /// top byte of a saved frame pointer); a 32-bit clean one doesn't.
        @Test func partitionBelow16MBUnless32BitCompatible() throws {
            launchTestApp(sizeFlags: 0)
            let a5 = rd32(UInt32(LM_CurrentA5))
            let stackBase = rd32(UInt32(LM_CurStackBase))
            let limit = rd32(UInt32(LM_ApplLimit))
            #expect(a5 < 0x0100_0000)
            #expect(g_cpu!.pointee.a.7 < 0x0100_0000)
            #expect(rd32(UInt32(LM_BufPtr)) <= 0x0100_0000)
            #expect(limit < stackBase && stackBase - limit >= 0x10_0000)
            #expect(limit & 3 == 0) // whole longs, or the zone grows by an odd amount
            tb_MaxApplZone()
            #expect(rd32(UInt32(LM_HeapEnd)) <= limit)
            #expect(mm_check_zone(rd32(UInt32(LM_ApplZone))))
            #expect(tb_StackSpace() > 0xF_0000)

            launchTestApp(sizeFlags: 0x0080) // is32BitCompatible
            #expect(rd32(UInt32(LM_CurrentA5)) >= 0x0100_0000)
            #expect(g_cpu!.pointee.a.7 >= 0x0100_0000)
        }
    }
}
