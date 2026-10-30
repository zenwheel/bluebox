import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Mixed Mode's calling conventions beyond the plain stack and register
    /// ones: the special cases (TextEdit's hooks, the menu bar hook,
    /// GetNextEvent's filter), dispatched routines and their selectors,
    /// condition-code results and _MixedModeDispatch; and jLg2Phys, which
    /// disk image drivers call to find a file's blocks on its disk.
    @Suite(.serialized) struct MixedMode {
        func bytes(_ b: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        /// 68k code from its words.
        func code68k(_ words: [UInt16]) -> UInt32 { bytes(words.flatMap { [UInt8($0 >> 8), UInt8($0 & 0xFF)] }) }

        /// A transition vector for PowerPC code.
        func ppc(_ words: [UInt32]) -> UInt32 {
            let code = bytes(words.flatMap { [UInt8($0 >> 24), UInt8(($0 >> 16) & 0xFF), UInt8(($0 >> 8) & 0xFF), UInt8($0 & 0xFF)] })
            let tv = bytes([UInt8](repeating: 0, count: 8))
            wr32(tv, code)
            return tv
        }

        /// A routine descriptor of records (procInfo, ISA, routine, selector, flags).
        func descriptor(_ records: [(UInt32, UInt8, UInt32, UInt32, UInt16)], indexable: Bool = false,
                        selectorInfo: UInt8 = 0) -> UInt32 {
            let rd = bytes([UInt8](repeating: 0, count: 12 + 20 * records.count))
            wr16(rd, 0xAAFE)
            wr8(rd + 2, 7)
            wr8(rd + 3, indexable ? 1 : 0)
            wr8(rd + 9, selectorInfo)
            wr16(rd + 10, UInt16(records.count - 1))
            for (i, r) in records.enumerated() {
                let rr = rd + 12 + UInt32(i * 20)
                wr32(rr, r.0)
                wr8(rr + 5, r.1)
                wr16(rr + 6, r.4)
                wr32(rr + 8, r.2)
                wr32(rr + 16, r.3)
            }
            return rd
        }

        /// A launch (a fresh Code Fragment Manager, its PowerPC processor).
        func setUp() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            g_cpu!.pointee.halted = false
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 64
        }

        /// Call a PowerPC export with its arguments in r3 on; r3 after.
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

        func mmCall(_ upp: UInt32, _ procInfo: UInt32, _ args: [UInt32]) -> UInt32 {
            args.withUnsafeBufferPointer { mm_call(upp, procInfo, $0.baseAddress, Int32(args.count)) }
        }

        var zFlag: Bool { g_cpu!.pointee.sr & 0x04 != 0 }

        // MARK: PowerPC code calling 68k code

        /// CallUniversalProc with uppMBarHookProcInfo ($CF) to 68k code (Disk
        /// Copy calls its licence agreement's code resource so): the
        /// rectangle's pointer on the stack, the short result from D0.
        @Test func menuBarHookTo68k() {
            setUp()
            // MOVEA.L 4(A7),A0; MOVE.W (A0),D0; RTS
            let hook = code68k([0x206F, 0x0004, 0x3010, 0x4E75])
            let rect = bytes([0x12, 0x34, 0, 0, 0, 0, 0, 0])
            #expect(call("CallUniversalProc", [hook, 0xCF, rect]) == 0x1234)
            wr16(rect, 0xFFFE)
            #expect(call("CallUniversalProc", [hook, 0xCF, rect]) == 0xFFFF_FFFE)
            #expect(!g_ppc.halted)
        }

        /// EOLHook: the character in D0.B, the Boolean back from the Z flag.
        @Test func endOfLineHookTo68k() {
            setUp()
            let hook = code68k([0x0C00, 0x000D, 0x4E75]) // CMPI.B #13,D0; RTS
            #expect(mmCall(hook, 0x1F, [13, 0, 0]) == 1)
            #expect(mmCall(hook, 0x1F, [0x61, 0, 0]) == 0)
        }

        /// TEFindWord: position D0.W and caller D2.W in; the word's start and
        /// end come back from D0.W and D1.W through the pointers.
        @Test func findWordTo68k() {
            setUp()
            let hook = code68k([0x3202, 0x5240, 0x4E75]) // MOVE.W D2,D1; ADDQ.W #1,D0; RTS
            let out = bytes([0, 0, 0, 0])
            _ = mmCall(hook, 0x6F, [10, 3, 0, 0, out, out + 2])
            #expect(rd16(out) == 11 && rd16(out + 2) == 3)
        }

        /// TEDoText: the port from A0 and the position from D0.W.
        @Test func doTextTo68k() {
            setUp()
            // MOVEA.L A3,A0; MOVE.W D3,D0; ADD.W D4,D0; RTS
            let hook = code68k([0x204B, 0x3003, 0xD044, 0x4E75])
            let out = bytes([UInt8](repeating: 0, count: 6))
            #expect(mmCall(hook, 0xAF, [0x0012_3456, 5, 7, 0, out, out + 4]) == 12)
            #expect(rd32(out) == 0x0012_3456 && rd16(out + 4) == 12)
        }

        /// GetNextEvent's filter: the event in A1, the result as the word on
        /// the stack, which the filter may change.
        @Test func nextEventFilterTo68k() {
            setUp()
            let hook = code68k([0x426F, 0x0004, 0x4E75]) // CLR.W 4(A7); RTS
            let result = bytes([1])
            _ = mmCall(hook, 0xBF, [0, result])
            #expect(rd8(result) == 0)
        }

        /// A register-based routine whose result is a condition code (the Z
        /// bit, kCCRegisterZBit).
        @Test func conditionCodeResult() {
            setUp()
            let tst = code68k([0x4A80, 0x4E75]) // TST.L D0; RTS
            let pi: UInt32 = 2 | (1 << 4) | (18 << 6) | (3 << 11) // Boolean in Z; D0.L
            #expect(mmCall(tst, pi, [0]) == 1)
            #expect(mmCall(tst, pi, [5]) == 0)
        }

        // MARK: 68k code calling PowerPC code

        /// EOLHook as a PowerPC routine: its Boolean result sets the Z flag.
        @Test func endOfLineHookFrom68k() {
            setUp()
            let rd = descriptor([(0x1F, 1, ppc([0x7C83_2378, 0x4E80_0020]), 0, 0)]) // mr r3,r4: pTE's low byte
            g_cpu!.pointee.a.3 = 1
            guest_call_regs(rd)
            #expect(zFlag && !g_cpu!.pointee.halted)
            g_cpu!.pointee.a.3 = 0x100
            guest_call_regs(rd)
            #expect(!zFlag)
        }

        /// TEFindWord as a PowerPC routine: what it stores through its
        /// pointers comes back in D0.W and D1.W, the rest of the registers
        /// kept.
        @Test func findWordFrom68k() {
            setUp()
            // sth r3,0(r7); sth r4,0(r8); blr
            let rd = descriptor([(0x6F, 1, ppc([0xB067_0000, 0xB088_0000, 0x4E80_0020]), 0, 0)])
            let sp = g_cpu!.pointee.a.7
            g_cpu!.pointee.d.0 = 0xAAAA_0010
            g_cpu!.pointee.d.1 = 0xBBBB_0000
            g_cpu!.pointee.d.2 = 0x0000_0020
            guest_call_regs(rd)
            #expect(g_cpu!.pointee.d.0 == 0xAAAA_0010 && g_cpu!.pointee.d.1 == 0xBBBB_0020)
            #expect(g_cpu!.pointee.a.7 == sp)
        }

        /// The menu bar hook as a PowerPC routine: the rectangle from the
        /// stack (the caller pops it), the short result in D0.
        @Test func menuBarHookFrom68k() {
            setUp()
            let rd = descriptor([(0xCF, 1, ppc([0x3860_FFFF, 0x4E80_0020]), 0, 0)]) // li r3,-1
            let sp = g_cpu!.pointee.a.7
            g_cpu!.pointee.a.7 = sp - 4
            wr32(sp - 4, 0x1000)
            guest_call_regs(rd)
            #expect(g_cpu!.pointee.d.0 == 0xFFFF_FFFF && g_cpu!.pointee.a.7 == sp - 4)
        }

        /// GetNextEvent's filter as a PowerPC routine: the Boolean it clears
        /// is GetNextEvent's result on the stack, and D0's.
        @Test func nextEventFilterFrom68k() {
            setUp()
            // li r0,0; stb r0,0(r4); blr
            let rd = descriptor([(0xBF, 1, ppc([0x3800_0000, 0x9804_0000, 0x4E80_0020]), 0, 0)])
            let sp = g_cpu!.pointee.a.7
            g_cpu!.pointee.a.7 = sp - 2
            wr16(sp - 2, 0x0100)
            g_cpu!.pointee.d.0 = 1
            guest_call_regs(rd)
            #expect(rd16(sp - 2) == 0 && g_cpu!.pointee.d.0 & 0xFFFF == 0)
        }

        /// A D0-dispatched Pascal routine: the PowerPC routine gets the
        /// selector first, then the parameters; the result goes in the
        /// caller's space and the parameters are popped.  A record marked
        /// kDontPassSelector gets the parameters alone.
        @Test func dispatchedFrom68k() {
            setUp()
            let pi: UInt32 = 8 | 0x20 | 0x80 | (3 << 8) // short f(selector.w in D0, long)
            let add = ppc([0x7C63_2214, 0x4E80_0020]) // add r3,r3,r4
            for (flags, want) in [(UInt16(0), UInt16(0x1005)), (UInt16(0x08), UInt16(0x1000))] {
                let rd = descriptor([(pi, 1, flags == 0 ? add : ppc([0x4E80_0020]), 0, flags)])
                let sp = g_cpu!.pointee.a.7
                wr16(sp - 2, 0)
                wr32(sp - 6, 0x1000)
                g_cpu!.pointee.a.7 = sp - 6
                g_cpu!.pointee.d.0 = 5
                guest_call_regs(rd)
                #expect(g_cpu!.pointee.a.7 == sp - 2 && rd16(sp - 2) == want)
                g_cpu!.pointee.a.7 = sp
            }
        }

        /// A dispatched descriptor of several routines: the selector picks
        /// the record, by its selector field or (indexable) by position.
        @Test func selectorPicksTheRoutine() {
            setUp()
            let pi: UInt32 = 8 | 0x30 | 0x80 // long f(selector.w)
            let eleven = ppc([0x3860_000B, 0x4E80_0020]), twentyTwo = ppc([0x3860_0016, 0x4E80_0020])
            let rd = descriptor([(pi, 1, eleven, 1, 0), (pi, 1, twentyTwo, 2, 0)])
            #expect(mmCall(rd, pi, [2]) == 22 && mmCall(rd, pi, [1]) == 11)
            let byIndex = descriptor([(pi, 1, eleven, 0, 0), (pi, 1, twentyTwo, 0, 0)], indexable: true)
            #expect(mmCall(byIndex, pi, [1]) == 22 && mmCall(byIndex, pi, [0]) == 11)
            // From 68k code, through selectorInfo: D0 picks.
            let sel68k = descriptor([(pi, 1, eleven, 1, 0), (pi, 1, twentyTwo, 2, 0)], selectorInfo: 8)
            let sp = g_cpu!.pointee.a.7
            g_cpu!.pointee.a.7 = sp - 4
            g_cpu!.pointee.d.0 = 2
            guest_call_regs(sel68k)
            #expect(rd32(sp - 4) == 22 && g_cpu!.pointee.a.7 == sp - 4)
        }

        /// _MixedModeDispatch: descriptors for classic 68k code, and the
        /// state the Thread Manager saves (version 1 only).
        @Test func mixedModeDispatch() {
            setUp()
            let fat = tb_NewFatRoutineDescriptor(0x1000, 0x2000, 0x2C0)
            #expect(rd16(fat) == 0xAAFE && rd16(fat + 10) == 1)
            #expect(rd32(fat + 20) == 0x1000 && rd8(fat + 17) == 0 && rd32(fat + 40) == 0x2000 && rd8(fat + 37) == 1)
            let one = tb_NewRoutineDescriptor(0x3000, 0x2C0, 1)
            #expect(rd32(one + 12) == 0x2C0 && rd32(one + 20) == 0x3000 && rd8(one + 17) == 1)
            tb_DisposeRoutineDescriptor(one)
            tb_DisposeRoutineDescriptor(fat)
            let state = bytes([UInt8](repeating: 0, count: 16))
            #expect(tb_SaveMixedModeState(state, 1) == 0 && tb_RestoreMixedModeState(state, 1) == 0)
            #expect(tb_SaveMixedModeState(state, 2) == -50 && tb_RestoreMixedModeState(state, 2) == -50)
        }

        // MARK: jLg2Phys

        /// A file's blocks: jLg2Phys gives the run where its fork lies on the
        /// drive, and the volume's driver reads and writes the file there;
        /// past the end of file it reads zeros.  Blocks no file was mapped
        /// to, and positions past the fork, are errors.
        @Test func logicalToPhysical() throws {
            let dir = launchTestApp()
            let content = (0..<3000).map { UInt8(truncatingIfNeeded: $0 * 7 + 1) }
            let path = dir.appendingPathComponent("Image.img").path
            try Data(content).write(to: URL(fileURLWithPath: path))
            let spec = tb_NewPtr(80, false, true), ref = tb_NewPtr(2, false, true)
            fm_spec_for_path(path, spec)
            #expect(tb_FSpOpenDF(spec, 3, ref) == 0)
            let rn = rd16(ref)
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 64
            g_cpu!.pointee.halted = false
            func lg2phys(_ pos: UInt32, _ want: UInt32) -> Int16 {
                g_cpu!.pointee.d.1 = UInt32(rn)
                g_cpu!.pointee.d.5 = pos
                g_cpu!.pointee.d.4 = want
                g_cpu!.pointee.a.1 = rd32(UInt32(LM_FCBSPtr))
                g_cpu!.pointee.a.2 = rd32(UInt32(LM_VCBQHdr) + 2)
                guest_call_regs(rd32(UInt32(LM_JLg2Phys)))
                return Int16(truncatingIfNeeded: g_cpu!.pointee.d.0)
            }
            #expect(lg2phys(600, 100_000) == 0)
            let first = g_cpu!.pointee.d.3 - 1
            #expect(g_cpu!.pointee.d.6 == 3072 - 600)
            #expect(lg2phys(600, 100) == 0 && g_cpu!.pointee.d.3 == first + 1 && g_cpu!.pointee.d.6 == 100)
            #expect(lg2phys(3072, 1) == -39) // eofErr

            let buf = tb_NewPtr(1024, false, true)
            func io(_ block: UInt32, _ count: UInt32, write: Bool = false) -> Int16 {
                let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
                wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -33))
                wr16(pb + UInt32(IOParam_ioVRefNum_off), 1)
                wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
                wr32(pb + UInt32(IOParam_ioReqCount_off), count)
                wr16(pb + UInt32(IOParam_ioPosMode_off), 1)
                wr32(pb + UInt32(IOParam_ioPosOffset_off), block * 512)
                return write ? tb_PBWrite(pb, false) : tb_PBRead(pb, false)
            }
            #expect(io(first, 1024) == 0)
            #expect((0..<1024).map { rd8(buf + UInt32($0)) } == Array(content[0..<1024]))
            #expect(io(first + 5, 512) == 0)
            #expect((0..<440).map { rd8(buf + UInt32($0)) } == Array(content[2560..<3000]))
            #expect((440..<512).allSatisfy { rd8(buf + UInt32($0)) == 0 })
            #expect(io(0, 512) == -36) // ioErr: no file there
            // A block written reaches the file.
            for i in 0..<512 { wr8(buf + UInt32(i), 0xEE) }
            #expect(io(first + 2, 512, write: true) == 0)
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true), data = tb_NewPtr(16, false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), rn)
            wr32(pb + UInt32(IOParam_ioBuffer_off), data)
            wr32(pb + UInt32(IOParam_ioReqCount_off), 4)
            wr16(pb + UInt32(IOParam_ioPosMode_off), 1)
            wr32(pb + UInt32(IOParam_ioPosOffset_off), 1022)
            #expect(tb_PBRead(pb, false) == 0)
            #expect((0..<4).map { rd8(data + UInt32($0)) } == [content[1022], content[1023], 0xEE, 0xEE])
            #expect(tb_PBClose(pb, false) == 0)
            let saved = try [UInt8](Data(contentsOf: URL(fileURLWithPath: path)))
            #expect(saved.count == 3000 && saved[1024] == 0xEE && saved[1536] == content[1536])
        }
    }
}
