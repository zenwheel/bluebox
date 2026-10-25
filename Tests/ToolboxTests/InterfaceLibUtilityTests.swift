import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// Call an InterfaceLib export with its arguments in r3 on; r3 after.
@discardableResult
func ilCall(_ name: String, _ args: [UInt32]) -> UInt32 {
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

func ilCall16(_ name: String, _ args: [UInt32]) -> Int16 { Int16(truncatingIfNeeded: ilCall(name, args)) }

/// Run 68k code with a fresh stack until it halts (end it with _ExitToShell).
func run68k(_ words: [UInt16], max: UInt64 = 100_000) -> String {
    let cpu = g_cpu!
    let code = tb_NewPtr(Int32(words.count * 2), false, false)
    for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
    let stack = tb_NewPtr(65536, false, true)
    cpu.pointee.a.7 = stack + 65536 - 16
    cpu.pointee.pc = code
    cpu.pointee.halted = false
    _ = cpu_run(cpu, max)
    return String(cString: machine_halt_reason())
}

func rd64(_ p: UInt32) -> UInt64 { UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4)) }
func hiWord(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
func loWord(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }

extension BlueBox {
    /// Driver Services' time base, atomic operations and memory calls, and
    /// the debugger utility trap, as InterfaceLib exports them.
    @Suite struct DriverServicesCalls {
        init() { bootMachine() }

        /// UpTime follows the machine's clock in 40 ns units (a 25 MHz time
        /// base), and the conversions agree with GetTimeBaseInfo.
        @Test func upTimeAndConversions() {
            let t0 = tb_NewPtr(8, false, true), t1 = tb_NewPtr(8, false, true), out = tb_NewPtr(8, false, true)
            ilCall("UpTime", [t0])
            machine_skip_time(1_000_000) // 1 ms
            ilCall("UpTime", [t1])
            let delta = rd64(t1) - rd64(t0)
            #expect(delta >= 25_000 && delta < 25_100)

            let info = (0..<5).map { _ in tb_NewPtr(4, false, true) }
            ilCall("GetTimeBaseInfo", info)
            #expect(info.map { rd32($0) } == [1, 40, 1, 1, 12])

            // 25000 units: 1 ms, which as a Duration is -1000 (microseconds).
            ilCall("AbsoluteToNanoseconds", [out, 0, 25_000])
            #expect(rd64(out) == 1_000_000)
            #expect(Int32(bitPattern: ilCall("AbsoluteToDuration", [0, 25_000])) == -1000)
            // Later time on the left; the other way round is nothing.
            #expect(Int32(bitPattern: ilCall("AbsoluteDeltaToDuration", [rd32(t1), rd32(t1 + 4), rd32(t0), rd32(t0 + 4)])) == -1000 ||
                    Int32(bitPattern: ilCall("AbsoluteDeltaToDuration", [rd32(t1), rd32(t1 + 4), rd32(t0), rd32(t0 + 4)])) == -1001)
            #expect(ilCall("AbsoluteDeltaToDuration", [rd32(t0), rd32(t0 + 4), rd32(t1), rd32(t1 + 4)]) == 0)
            ilCall("AbsoluteDeltaToNanoseconds", [out, rd32(t0), rd32(t0 + 4), rd32(t1), rd32(t1 + 4)])
            #expect(rd64(out) == 0)

            // Durations: positive milliseconds, negative microseconds.
            ilCall("DurationToAbsolute", [out, 5])
            #expect(rd64(out) == 125_000)
            ilCall("DurationToAbsolute", [out, UInt32(bitPattern: -80)])
            #expect(rd64(out) == 2_000)
            ilCall("DurationToNanoseconds", [out, 2])
            #expect(rd64(out) == 2_000_000)
            // An hour is too many microseconds for a Duration: milliseconds.
            #expect(ilCall("NanosecondsToDuration", [0x346, 0x30B8_A000]) == 3_600_000)
            #expect(Int32(bitPattern: ilCall("NanosecondsToDuration", [0, 1_500_000])) == -1500)
            ilCall("NanosecondsToAbsolute", [out, 0, 400])
            #expect(rd64(out) == 10)

            ilCall("AddDurationToAbsolute", [out, 1, 0, 10])
            #expect(rd64(out) == 25_010)
            ilCall("SubDurationFromAbsolute", [out, 1, 0, 25_010])
            #expect(rd64(out) == 10)
            ilCall("AddNanosecondsToAbsolute", [out, 0, 80, 1, 0])
            #expect(rd64(out) == 0x1_0000_0002)
            ilCall("SubNanosecondsFromAbsolute", [out, 0, 80, 1, 0])
            #expect(rd64(out) == 0xFFFF_FFFE)
            ilCall("AddAbsoluteToAbsolute", [out, 0, 0xFFFF_FFFF, 0, 1])
            #expect(rd64(out) == 0x1_0000_0000)
            ilCall("SubAbsoluteFromAbsolute", [out, 1, 0, 0, 1])
            #expect(rd64(out) == 0xFFFF_FFFF)
        }

        /// The Time Manager's PowerPC names are _InsTime, _PrimeTime and
        /// _RmvTime.
        @Test func timeTasksByTheirPowerPCNames() {
            let tm = tb_NewPtr(Int32(TMTask_SIZEOF), false, true)
            #expect(ilCall16("InstallXTimeTask", [tm]) == 0)
            #expect(ilCall16("PrimeTimeTask", [tm, 10_000]) == 0)
            #expect(rd16(tm + 4) & 0x8000 != 0) // active
            #expect(ilCall16("RemoveTimeTask", [tm]) == 0)
            #expect(rd16(tm + 4) & 0x8000 == 0)
            #expect(Int32(bitPattern: rd32(tm + UInt32(TMTask_tmCount_off))) < 0) // what was left
            #expect(ilCall16("InstallTimeTask", [0]) == -50)
        }

        /// The atomic operations answer the value from before.
        @Test func atomicOperations() {
            let p = tb_NewPtr(8, false, true)
            wr32(p, 10)
            #expect(ilCall("AddAtomic", [5, p]) == 10 && rd32(p) == 15)
            #expect(ilCall("IncrementAtomic", [p]) == 15 && rd32(p) == 16)
            #expect(ilCall("DecrementAtomic", [p]) == 16 && rd32(p) == 15)
            #expect(ilCall("BitAndAtomic", [0x0C, p]) == 15 && rd32(p) == 0x0C)
            #expect(ilCall("BitOrAtomic", [0x30, p]) == 0x0C && rd32(p) == 0x3C)
            #expect(ilCall("BitXorAtomic", [0xFF, p]) == 0x3C && rd32(p) == 0xC3)
            wr16(p, 0x7FFF)
            #expect(ilCall("IncrementAtomic16", [p]) == 0x7FFF && rd16(p) == 0x8000)
            #expect(Int32(bitPattern: ilCall("AddAtomic16", [1, p])) == -32768) // signed
            #expect(ilCall("BitAndAtomic16", [0xFF00, p]) == 0x8001 && rd16(p) == 0x8000) // unsigned
            wr8(p, 0xFF)
            #expect(Int32(bitPattern: ilCall("DecrementAtomic8", [p])) == -1 && rd8(p) == 0xFE)
            #expect(ilCall("BitOrAtomic8", [1, p]) == 0xFE && rd8(p) == 0xFF)
            #expect(ilCall("BitXorAtomic8", [0x0F, p]) == 0xFF && rd8(p) == 0xF0)
            #expect(ilCall("IncrementAtomic8", [p]) == 0xFFFF_FFF0 && rd8(p) == 0xF1)
            #expect(ilCall("AddAtomic8", [0x10, p]) == 0xFFFF_FFF1 && rd8(p) == 0x01)
            #expect(ilCall("BitAndAtomic8", [0, p]) == 1 && rd8(p) == 0)
            #expect(ilCall("DecrementAtomic16", [p]) == 0 && rd16(p) == 0xFFFF)
            #expect(ilCall("BitOrAtomic16", [0, p]) == 0xFFFF)
            #expect(ilCall("BitXorAtomic16", [0xFFFF, p]) == 0xFFFF && rd16(p) == 0)

            wr32(p, 7)
            #expect(ilCall("CompareAndSwap", [6, 9, p]) == 0 && rd32(p) == 7)
            #expect(ilCall("CompareAndSwap", [7, 9, p]) == 1 && rd32(p) == 9)

            // Bit 0 is the first byte's high bit, as BitTst numbers them.
            wr16(p, 0)
            #expect(ilCall("TestAndSet", [0, p]) == 0 && rd8(p) == 0x80)
            #expect(ilCall("TestAndSet", [0, p]) == 1)
            #expect(ilCall("TestAndSet", [9, p]) == 0 && rd8(p + 1) == 0x40)
            #expect(ilCall("TestAndClear", [9, p]) == 1 && rd8(p + 1) == 0)
            #expect(ilCall("TestAndClear", [9, p]) == 0)
        }

        /// Memory: zeroing and moving, purge space, and virtual memory's
        /// calls answering as a Mac with it off.
        @Test func memoryCalls() {
            let a = tb_NewPtr(16, false, false), b = tb_NewPtr(16, false, true)
            for i in 0..<16 { wr8(a + UInt32(i), UInt8(i + 1)) }
            ilCall("BlockMoveUncached", [a, b, 8])
            #expect(rd8(b) == 1 && rd8(b + 7) == 8 && rd8(b + 8) == 0)
            ilCall("BlockMoveDataUncached", [a + 8, b + 8, 8])
            #expect(rd8(b + 15) == 16)
            ilCall("BlockZero", [b, 4])
            #expect(rd32(b) == 0 && rd8(b + 4) == 5)
            ilCall("BlockZeroUncached", [b + 4, 12])
            #expect(rd32(b + 12) == 0)

            let total = ilCall("PurgeSpaceTotal", []), contig = ilCall("PurgeSpaceContiguous", [])
            // (The contiguous figure counts what the zone could grow by.)
            var t: UInt32 = 0, cg: UInt32 = 0
            tb_PurgeSpace(&t, &cg, false)
            #expect(total == t && contig == cg && total > 0)
            let sysTotal = ilCall("PurgeSpaceSysTotal", []), sysContig = ilCall("PurgeSpaceSysContiguous", [])
            tb_PurgeSpace(&t, &cg, true)
            #expect(sysTotal == t && sysContig == cg && sysTotal > 0)

            let h = tb_NewHandle(10, false, false)
            tb_EmptyHandle(h)
            ilCall("ReallocateHandleSys", [h, 40])
            #expect(memErr() == 0 && tb_GetHandleSize(h) == 40)

            for name in ["LockMemoryForOutput", "MakeMemoryResident", "ReleaseMemoryData", "MakeMemoryNonResident",
                         "FlushMemory"] {
                #expect(ilCall16(name, [a, 16]) == 0)
            }
            // GetPhysical: the logical range is its own physical one.
            let table = tb_NewPtr(72, false, true), count = tb_NewPtr(4, false, true)
            wr32(table, a)
            wr32(table + 4, 16)
            wr32(count, 8)
            #expect(ilCall16("GetPhysical", [table, count]) == 0)
            #expect(rd32(count) == 1 && rd32(table + 8) == a && rd32(table + 12) == 16)
            #expect(rd32(table + 4) == 0) // nothing left to translate
            // No volume backs virtual memory.
            let vm = tb_NewPtr(10, false, true)
            wr16(vm, 1)
            wr8(vm + 4, 0xFF)
            #expect(ilCall16("GetVolumeVirtualMemoryInfo", [vm]) == 0 && rd8(vm + 4) == 0)
            wr16(vm, 0)
            #expect(ilCall16("GetVolumeVirtualMemoryInfo", [vm]) == -50)
            #expect(ilCall("TopMem", []) == rd32(UInt32(LM_MemTop)))
            #expect(ilCall("GetSysPPtr", []) == UInt32(LM_SPValid))
        }

        /// The 68k forms: _MemoryDispatch's GetPhysical and the debugger
        /// utility trap.
        @Test func memoryDispatchAndDebugUtilFrom68k() {
            let a = tb_NewPtr(16, false, true)
            let table = tb_NewPtr(72, false, true)
            wr32(table, a)
            wr32(table + 4, 16)
            let reason = run68k([
                0x207C, hiWord(table), loWord(table), // MOVEA.L #table,A0
                0x7208,                               // MOVEQ #8,D1
                0x2241,                               // MOVEA.L D1,A1  (entries the table holds)
                0x7005,                               // MOVEQ #5,D0    GetPhysical
                0xA15C,                               // _MemoryDispatch
                0x2C00,                               // MOVE.L D0,D6
                0x2A08,                               // MOVE.L A0,D5   entries used
                0x7000, 0xA08D,                       // MOVEQ #0,D0; _DebugUtil  DebuggerGetMax
                0x2800,                               // MOVE.L D0,D4
                0x207C, hiWord(a), loWord(a),         // MOVEA.L #a,A0
                0x7004, 0xA08D,                       // GetPageState
                0x2600,                               // MOVE.L D0,D3
                0x7005, 0xA08D,                       // PageFaultFatal
                0x2E00,                               // MOVE.L D0,D7
                0x7008, 0xA08D,                       // EnterSupervisorMode
                0x3400,                               // MOVE.W D0,D2
                0xA9F4,                               // _ExitToShell
            ])
            #expect(reason == "ExitToShell")
            let c = g_cpu!.pointee
            #expect(c.d.6 == 0 && c.d.5 == 1 && rd32(table + 8) == a && rd32(table + 12) == 16)
            #expect(c.d.4 == 8)               // the highest selector
            #expect(Int16(truncatingIfNeeded: c.d.3) == 2) // kNotPaged
            #expect(c.d.7 & 0xFF == 0)
            #expect(c.d.2 & 0x2000 != 0)      // supervisor
            #expect(tb_DebuggerGetMax() == 8)
        }

        /// GetCPUSpeed: from PowerPC, and through _IdleUpdateDispatch with
        /// D0 = -1; _IdleUpdate answers Ticks.
        @Test func cpuSpeed() {
            #expect(ilCall("GetCPUSpeed", []) == 300)
            let reason = run68k([
                0x70FF, 0xA485,   // MOVEQ #-1,D0; _IdleUpdateDispatch  GetCPUSpeed
                0x2E00,           // MOVE.L D0,D7
                0xA285,           // _IdleUpdate
                0x2C00,           // MOVE.L D0,D6
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 == 300)
            #expect(g_cpu!.pointee.d.6 == rd32(UInt32(LM_Ticks)))
        }
    }
}
