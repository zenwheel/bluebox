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

// run68k is in LegacyManagersTests.swift.

func rd64(_ p: UInt32) -> UInt64 { UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4)) }

extension BlueBox {
    /// BitShift as QuickDraw's: a negative count shifts right by its
    /// magnitude (Extension Overload takes a name's length with -24), and
    /// LSL/LSR's count modulo 64 shifts 32 to 63 bits out altogether.
    @Suite struct BitShiftCalls {
        @Test func negativeCountsShiftRight() {
            #expect(tb_BitShift(0x0644_1100, -24) == 0x06)
            #expect(tb_BitShift(0x0644_1100, 8) == 0x4411_0000)
            #expect(tb_BitShift(Int32(bitPattern: 0x8000_0000), -31) == 1) // logical
            #expect(tb_BitShift(0x1234, 32) == 0)
            #expect(tb_BitShift(0x1234, -40) == 0)
            #expect(tb_BitShift(0x1234, 64) == 0x1234)
            #expect(tb_BitShift(0x1234, 0) == 0x1234)
        }
    }

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
            #expect(info.map { rd32($0) } == [1, 40, 1, 25, UInt32(tb_GetCPUSpeed())])

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
            #expect(ilCall("GetCPUSpeed", []) == UInt32(tb_GetCPUSpeed()))
            let reason = run68k([
                0x70FF, 0xA485,   // MOVEQ #-1,D0; _IdleUpdateDispatch  GetCPUSpeed
                0x2E00,           // MOVE.L D0,D7
                0xA285,           // _IdleUpdate
                0x2C00,           // MOVE.L D0,D6
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 == UInt32(tb_GetCPUSpeed()))
            #expect(g_cpu!.pointee.d.6 == rd32(UInt32(LM_Ticks)))
        }
    }

    /// LowMem.h's accessors for the globals that had none: on the globals.
    @Suite struct LowMemoryAccessors {
        init() { bootMachine() }

        @Test func scalarsRoundTrip() {
            let longs: [(String, UInt32)] = [("ABusGlobals", 0x2D8), ("ATalkHk2", 0xB18),
                                              ("DiskFormatingHFSDefaults", 0x39E), ("HighHeapMark", 0xBAE),
                                              ("KeyTime", 0x186), ("MBTicks", 0x16E), ("ROMMapHandle", 0xB06),
                                              ("StackLowPoint", 0x110)]
            for (name, addr) in longs {
                ilCall("LMSet\(name)", [0x1234_5678])
                #expect(rd32(addr) == 0x1234_5678, "\(name)")
                wr32(addr, 0x0BAD_F00D)
                #expect(ilCall("LMGet\(name)", []) == 0x0BAD_F00D, "\(name)")
            }
            let bytes: [(String, UInt32)] = [("CursorNew", 0x8CE), ("MouseButtonState", 0x172), ("PortAInfo", 0x290),
                                              ("ROMMapInsert", 0xB9E), ("WordRedraw", 0xBA5)]
            for (name, addr) in bytes {
                let next = rd8(addr + 1)
                ilCall("LMSet\(name)", [0x1FF])
                #expect(rd8(addr) == 0xFF && rd8(addr + 1) == next, "\(name)")
                wr8(addr, 0x42)
                #expect(ilCall("LMGet\(name)", []) == 0x42, "\(name)")
            }
            ilCall("LMSetUnitTableEntryCount", [UInt32(bitPattern: -2)])
            #expect(rd16(0x1D2) == 0xFFFE)
            #expect(Int32(bitPattern: ilCall("LMGetUnitTableEntryCount", [])) == -2)
        }

        @Test func pointsAndTheCursor() {
            ilCall("LMSetMouseTemp", [0x0010_0020])
            #expect(rd32(0x828) == 0x0010_0020)
            wr32(0x82C, 0x0030_0040)
            #expect(ilCall("LMGetRawMouseLocation", [0]) == 0x0030_0040)

            // SetCursor keeps TheCrsr, which LMGetTheCursor copies out.
            let cursor = tb_NewPtr(68, false, true)
            for i in 0..<16 { wr16(cursor + UInt32(i * 2), UInt16(0x0101 * i)) }
            wr32(cursor + 64, 0x0003_0004)
            tb_SetCursor(cursor)
            let out = tb_NewPtr(68, false, true)
            ilCall("LMGetTheCursor", [out])
            #expect((0..<68).allSatisfy { rd8(out + UInt32($0)) == rd8(cursor + UInt32($0)) })
            tb_InitCursor()
            ilCall("LMGetTheCursor", [out])
            #expect(rd16(out + 2) == 0x4000 && rd32(out + 64) == 0x0001_0001) // the arrow
            ilCall("LMSetTheCursor", [cursor])
            #expect(rd32(0x844 + 64) == 0x0003_0004)
            #expect(ilCall("GetEventQueue", []) == UInt32(LM_EventQueue))
        }
    }

    /// The Component Manager's calls of components from PowerPC code, and
    /// public resources.
    @Suite struct ComponentCalls {
        init() { bootMachine() }

        /// A 68k routine descriptor for a native stub.
        func descriptor68k(_ proc: UInt32, _ procInfo: UInt32) -> UInt32 {
            let rd = tb_NewPtr(32, false, true)
            wr16(rd, 0xAAFE)
            wr8(rd + 2, 7)
            wr32(rd + 12, procInfo)
            wr32(rd + 20, proc)
            return rd
        }

        @Test func callsOfAComponent() {
            guestStack()
            testTarget = 0
            let entry = native_register(testComponent, "TestComponent", 0)
            let cd = tb_NewPtr(20, false, true)
            wr32(cd, fourCC("tEst"))
            let comp = ilCall("RegisterComponent", [cd, entry, 0, 0, 0, 0])
            #expect(comp != 0)
            let ci = ilCall("OpenComponent", [comp])
            #expect(ci != 0)

            #expect(ilCall("CallComponentVersion", [ci]) == 0x0002_0003)
            #expect(ilCall("CallComponentCanDo", [ci, UInt32(bitPattern: -10)]) == 1)
            #expect(ilCall("ComponentFunctionImplemented", [ci, 7]) == 0)
            #expect(ilCall("CallComponentTarget", [ci, 0x1234]) == 0 && testTarget == 0x1234)
            #expect(ilCall("ComponentSetTarget", [ci, 0x5678]) == 0 && testTarget == 0x5678)
            #expect(ilCall("CallComponentRegister", [ci]) == 0)
            #expect(ilCall("CallComponentUnregister", [ci]) == 0)
            let h = tb_NewPtr(4, false, true)
            #expect(ilCall("CallComponentGetPublicResource", [ci, fourCC("STR#"), 128, h]) == 0 && rd32(h) != 0)
            #expect(ilCall("CallComponentGetMPWorkFunction", [ci, h, h]) == UInt32(bitPattern: -2009)) // not one of its calls

            // Public resources: asked of the component.
            wr32(h, 0)
            #expect(ilCall16("GetComponentPublicResource", [comp, fourCC("STR#"), 128, h]) == 0)
            #expect(rd32(h) != 0 && tb_GetHandleSize(rd32(h)) > 2)
            #expect(ilCall16("GetComponentPublicResource", [comp, fourCC("PICT"), 128, h]) == -192 && rd32(h) == 0)
            let str = tb_NewPtr(256, false, true)
            #expect(ilCall16("GetComponentPublicIndString", [comp, str, 128, 2]) == 0 && pstring(at: str) == "two")
            #expect(ilCall16("GetComponentPublicIndString", [comp, str, 128, 3]) == -192 && rd8(str) == 0)
            #expect(ilCall16("GetComponentPublicResourceList", [fourCC("STR#"), 128, 0, cd, 0, 0, h]) == 0)
            #expect(tb_GetHandleSize(rd32(h)) == 32 && rd32(rd32(rd32(h)) + 16) == fourCC("sean"))

            // CallComponentFunctionWithStorage: the storage, then the call's
            // arguments (a long, then a short) from its parameters.
            ilCall("SetComponentInstanceStorage", [ci, 1000])
            let params = tb_NewPtr(16, false, true)
            wr8(params + 1, 6)
            wr16(params + 2, 1)
            wr32(params + 6, 7)  // the first argument, highest
            wr16(params + 4, 3)  // the second
            wr32(params + 10, ci)
            let fn = native_register(storageFunction, "StorageFunction", 0)
            let upp = descriptor68k(fn, 0xBF0) // pascal long (long, long, short)
            #expect(ilCall("CallComponentFunctionWithStorage", [1000, params, upp]) == 1073)
            // A PowerPC function by its procInfo.
            _ = cfm_ppc_cpu()
            let tv = cfm_native_tvector({ c in
                guard let c else { return }
                withUnsafeMutableBytes(of: &c.pointee.gpr) { g in
                    let a = g.load(fromByteOffset: 12, as: UInt32.self), b = g.load(fromByteOffset: 16, as: UInt32.self)
                    let d = g.load(fromByteOffset: 20, as: UInt32.self)
                    g.storeBytes(of: a + b * 10 + d, toByteOffset: 12, as: UInt32.self)
                }
                c.pointee.pc = c.pointee.lr
            }, "PPCFunction")
            #expect(ilCall("CallComponentFunctionWithStorageProcInfo", [2000, params, tv, 0xBF0]) == 2073)
            // Without storage: the arguments alone.
            let upp2 = descriptor68k(native_register(plainFunction, "PlainFunction", 0), 0x2F0)
            #expect(ilCall("CallComponentFunction", [params, upp2]) == 73)

            #expect(ilCall("CallComponentClose", [ci, ci]) == 0)
            #expect(ilCall16("CloseComponent", [ci]) == 0)
        }
    }
}

nonisolated(unsafe) var testTarget: UInt32 = 0

/// A component: pascal long (ComponentParameters *params, Handle storage).
let testComponent: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    _ = cpu_pop32(c)
    let params = cpu_pop32(c)
    let what = Int16(bitPattern: rd16(params + 2))
    var r: UInt32 = 0
    switch what {
    case -3: r = [-1, -2, -3, -4, -5, -6, -7, -10].contains(Int16(bitPattern: rd16(params + 4))) ? 1 : 0
    case -4: r = 0x0002_0003
    case -6: testTarget = rd32(params + 4)
    case -10:
        if rd32(params + 10) == fourCC("STR#") && rd16(params + 8) == 128 {
            let h = tb_NewHandle(10, false, true)
            let p = rd32(h)
            wr16(p, 2)
            wr8(p + 2, 3); wr8(p + 3, 0x6F); wr8(p + 4, 0x6E); wr8(p + 5, 0x65) // "one"
            wr8(p + 6, 3); wr8(p + 7, 0x74); wr8(p + 8, 0x77); wr8(p + 9, 0x6F) // "two"
            wr32(rd32(params + 4), h)
        } else {
            r = UInt32(bitPattern: -192)
        }
    case -1, -2, -5, -7: r = 0
    default: r = UInt32(bitPattern: -2009) // badComponentSelector's low word, say
    }
    wr32(c.pointee.a.7, r)
    cpu_push32(c, ret)
}

/// pascal long (Handle storage, long a, short b): storage + a * 10 + b.
let storageFunction: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    let b = UInt32(cpu_pop16(c)), a = cpu_pop32(c), storage = cpu_pop32(c)
    wr32(c.pointee.a.7, storage + a * 10 + b)
    cpu_push32(c, ret)
}

/// pascal long (long a, short b): a * 10 + b.
let plainFunction: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    let b = UInt32(cpu_pop16(c)), a = cpu_pop32(c)
    wr32(c.pointee.a.7, a * 10 + b)
    cpu_push32(c, ret)
}

extension BlueBox {
    /// The Dictionary Manager: keyed records in a file of ours.
    @Suite struct DictionaryManager {
        init() { bootMachine() }

        func handle(_ bytes: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            return h
        }
        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }
        func bytes(_ h: UInt32) -> [UInt8] { (0..<UInt32(tb_GetHandleSize(h))).map { rd8(rd32(h) + $0) } }

        @Test func recordsKeptInAFile() {
            let dir = launchTestApp()
            let spec = tb_NewPtr(70, false, true)
            fm_spec_for_path(dir.appendingPathComponent("Words").path, spec)
            #expect(tb_InitializeDictionary(spec, 0, 0, 0) == -416)    // btKeyLenErr
            #expect(tb_InitializeDictionary(spec, 32, 0x40, 0) == -417) // btKeyAttrErr
            #expect(tb_InitializeDictionary(spec, 32, 0, 0) == 0)      // case and marks ignored
            let ref = tb_NewPtr(4, false, true)
            #expect(ilCall16("OpenDictionary", [spec, 3, ref]) == 0)
            let d = Int32(bitPattern: rd32(ref))

            #expect(tb_InsertRecordToDictionary(d, pstr("pear"), handle([1, 2, 3]), 0) == 0)
            #expect(tb_InsertRecordToDictionary(d, pstr("Apple"), handle([4]), 0) == 0)
            #expect(tb_InsertRecordToDictionary(d, pstr("APPLE"), handle([5]), 0) == -414) // btDupRecErr
            #expect(tb_InsertRecordToDictionary(d, pstr("fig"), handle([6]), 1) == -415)   // kReplace: none
            #expect(tb_InsertRecordToDictionary(d, pstr("apple"), handle([7, 7]), 2) == 0) // kInsertOrReplace
            #expect(tb_InsertRecordToDictionary(d, pstr("x"), handle([]), 3) == -20000)
            #expect(tb_InsertRecordToDictionary(d, pstr(String(repeating: "k", count: 33)), handle([]), 0) == -416)
            #expect(tb_InsertRecordToDictionary(d, pstr("big"), handle([UInt8](repeating: 0, count: 1025)), 0) == -20001)
            #expect(tb_InsertRecordToDictionary(d, pstr("fig"), handle([8]), 0) == 0)

            let out = tb_NewHandle(0, false, false)
            #expect(tb_FindRecordInDictionary(d, pstr("PEAR"), 0, out) == 0 && bytes(out) == [1, 2, 3])
            #expect(tb_FindRecordInDictionary(d, pstr("plum"), 0, out) == -415)
            // In key order, from 0.
            let key = tb_NewPtr(256, false, true)
            #expect(tb_FindRecordByIndexInDictionary(d, 0, 0, key, out) == 0 && pstring(at: key) == "Apple" && bytes(out) == [7, 7])
            #expect(tb_FindRecordByIndexInDictionary(d, 2, 0, key, out) == 0 && pstring(at: key) == "pear")
            #expect(tb_FindRecordByIndexInDictionary(d, 3, 0, key, out) == -415)

            #expect(tb_DeleteRecordFromDictionary(d, pstr("FIG")) == 0)
            #expect(tb_DeleteRecordFromDictionary(d, pstr("fig")) == -415)
            let info = tb_NewPtr(84, false, true)
            #expect(tb_GetDictionaryInformation(d, info) == 0)
            #expect(rd32(info + 70) == 2 && rd16(info + 80) == 32 && rd8(info + 82) == 0)
            #expect(rd32(info + 2) == rd32(spec + 2)) // the file's parent
            #expect(tb_CompactDictionary(d) == 0)
            #expect(ilCall16("CloseDictionary", [rd32(ref)]) == 0)
            #expect(tb_CloseDictionary(d) == -50)

            // Opened again, read only: the records came back from the file.
            #expect(tb_OpenDictionary(spec, 1, ref) == 0)
            let d2 = Int32(bitPattern: rd32(ref))
            #expect(tb_FindRecordInDictionary(d2, pstr("apple"), 0, out) == 0 && bytes(out) == [7, 7])
            #expect(tb_InsertRecordToDictionary(d2, pstr("kiwi"), handle([9]), 0) == -61) // wrPermErr
            #expect(tb_CloseDictionary(d2) == 0)

            // Not one of ours.
            let other = tb_NewPtr(70, false, true)
            let path = dir.appendingPathComponent("Other").path
            try? Data([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17]).write(to: URL(fileURLWithPath: path))
            fm_spec_for_path(path, other)
            #expect(tb_OpenDictionary(other, 1, ref) == -410) // notBTree
        }
    }

    /// AVL trees, ordered by the program's compare procedure.
    @Suite struct AVLTrees {
        init() { bootMachine() }

        @Test func insertFindRemoveWalk() {
            guestStack()
            avlDisposed = []
            avlWalked = []
            let compare = native_register(avlCompare, "AVLCompare", 0)
            let size = native_register(avlSize, "AVLSize", 0)
            let dispose = native_register(avlDispose, "AVLDispose", 0)
            let walkProc = native_register(avlWalk, "AVLWalk", 0)
            let treeVar = tb_NewPtr(4, false, true)
            #expect(tb_AVLInit(0, compare, size, dispose, 0x1234, treeVar) == 0)
            let tree = rd32(treeVar)
            #expect(tree != 0)
            let item = tb_NewPtr(8, false, true)
            for k: UInt32 in [50, 20, 70, 10, 30, 60, 80, 40] {
                wr32(item, k)
                wr32(item + 4, k * 2)
                #expect(tb_AVLInsert(tree, item) == 0)
            }
            wr32(item, 30)
            #expect(tb_AVLInsert(tree, item) == -960) // errItemAlreadyInTree
            let count = tb_NewPtr(4, false, true), out = tb_NewPtr(8, false, true), sz = tb_NewPtr(4, false, true)
            #expect(tb_AVLCount(tree, count) == 0 && rd32(count) == 8)
            // Found by key alone.
            wr32(item, 60)
            wr32(item + 4, 0)
            #expect(tb_AVLFind(tree, item, out, sz) == 0 && rd32(out + 4) == 120 && rd32(sz) == 8)
            wr32(item, 65)
            #expect(tb_AVLFind(tree, item, out, sz) == -962)
            #expect(tb_AVLGetIndItem(tree, 3, out, sz) == 0 && rd32(out) == 30)
            #expect(tb_AVLGetIndItem(tree, 9, out, sz) == -962)
            // Removing calls the dispose procedure.
            wr32(item, 20)
            #expect(tb_AVLRemove(tree, item, out, sz) == 0 && rd32(out + 4) == 40 && avlDisposed == [20])
            #expect(tb_AVLCount(tree, count) == 0 && rd32(count) == 7)

            // The walk: in order left to right, then right to left.
            #expect(tb_AVLWalk(tree, walkProc, 0, 0) == 0)
            #expect(avlWalked.map(\.key) == [10, 30, 40, 50, 60, 70, 80])
            #expect(avlWalked.map(\.level).max()! <= 4 && avlWalked.map(\.level).min()! == 1)
            avlWalked = []
            #expect(tb_AVLWalk(tree, walkProc, 1, 0) == 0)
            #expect(avlWalked.map(\.key) == [80, 70, 60, 50, 40, 30, 10])
            // A walk stops at the first nonzero result.
            avlWalked = []
            #expect(tb_AVLWalk(tree, walkProc, 0, 3) == -1)
            #expect(avlWalked.count == 3)

            // Through the 68k trap: AVLGetRefcon, then AVLCount.
            let refcon = tb_NewPtr(4, false, true)
            let reason = run68k([
                0x4267,                                   // CLR.W -(SP)
                0x2F3C, hiWord(tree), loWord(tree),       // MOVE.L #tree,-(SP)
                0x2F3C, hiWord(refcon), loWord(refcon),   // MOVE.L #refcon,-(SP)
                0x303C, 0x0409, 0xAA80,                   // AVLGetRefcon
                0x3E1F,                                   // MOVE.W (SP)+,D7
                0xA9F4,
            ])
            #expect(reason == "ExitToShell" && g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd32(refcon) == 0x1234)
            #expect(ilCall16("AVLCount", [tree, count]) == 0 && rd32(count) == 7)

            avlDisposed = []
            #expect(tb_AVLDispose(treeVar, 0) == 0 && rd32(treeVar) == 0)
            #expect(avlDisposed.count == 7)
            #expect(tb_AVLCount(tree, count) == -961) // errNotValidTree
        }
    }
}

extension BlueBox {
    /// The Text Services Manager on a Mac with no input methods.
    @Suite struct TextServices {
        init() { bootMachine() }

        @Test func documentsAndServices() {
            guestStack()
            let types = tb_NewPtr(4, false, true), docVar = tb_NewPtr(4, false, true)
            wr32(types, fourCC("tmTE"))
            #expect(ilCall16("NewTSMDocument", [1, types, docVar, 42]) == 0)
            let doc = rd32(docVar)
            #expect(doc != 0 && rd32(doc + 4) == 42)
            #expect(ilCall16("DeactivateTSMDocument", [doc]) == -2507) // not active
            #expect(ilCall16("FixTSMDocument", [doc]) == -2507)
            #expect(ilCall16("ActivateTSMDocument", [doc]) == 0)
            #expect(ilCall16("FixTSMDocument", [doc]) == 0)
            #expect(ilCall("TSMEvent", [tb_NewPtr(16, false, true)]) == 0)
            #expect(ilCall("TSMMenuSelect", [0x0080_0001]) == 0)
            #expect(ilCall16("UseInputWindow", [doc, 1]) == 0)

            // No services offered, no default input method.
            let list = tb_NewPtr(4, false, true), seed = tb_NewPtr(4, false, true)
            #expect(ilCall16("GetServiceList", [1, types, list, seed]) == 0)
            #expect(rd16(rd32(rd32(list))) == 0)
            let sl = tb_NewPtr(4, false, true), comp = tb_NewPtr(4, false, true)
            #expect(ilCall16("GetDefaultInputMethod", [comp, sl]) == -2517 && rd32(comp) == 0)
            wr32(sl, 0x0000_000B) // Roman, langDutch
            #expect(ilCall16("SetTextServiceLanguage", [sl]) == 0)
            wr32(sl, 0)
            #expect(ilCall16("GetTextServiceLanguage", [sl]) == 0 && rd32(sl) == 0x0000_000B)

            // A component of the program's own can be opened as a service.
            let entry = native_register(testComponent, "TestComponent", 0)
            let cd = tb_NewPtr(20, false, true)
            wr32(cd, fourCC("tsvc"))
            let c = ilCall("RegisterComponent", [cd, entry, 0, 0, 0, 0])
            let ciVar = tb_NewPtr(4, false, true)
            #expect(ilCall16("OpenTextService", [doc, c, ciVar]) == 0 && rd32(ciVar) != 0)
            let ci = rd32(ciVar)
            #expect(ilCall("ActivateTextService", [ci]) == UInt32(bitPattern: -2009)) // the component's answer
            #expect(ilCall("TextServiceEvent", [ci, 1, tb_NewPtr(16, false, true)]) == UInt32(bitPattern: -2009))
            #expect(ilCall16("CloseTextService", [doc, ci]) == 0)
            #expect(ilCall16("CloseTextService", [doc, ci]) == -2514)
            #expect(ilCall16("OpenTextService", [doc, 0, ciVar]) == -2509)

            #expect(ilCall16("DeactivateTSMDocument", [doc]) == 0)
            #expect(ilCall16("DeleteTSMDocument", [doc]) == 0)
            #expect(ilCall16("DeleteTSMDocument", [doc]) == -2505)

            // Through the trap: TSMEvent answers false.
            let reason = run68k([
                0x4267,                                 // CLR.W -(SP)
                0x2F3C, 0x0000, 0x0000,                 // MOVE.L #0,-(SP)
                0x7004, 0xAA54,                         // MOVEQ #4,D0; _TSMDispatch
                0x3E1F,                                 // MOVE.W (SP)+,D7
                0x7014, 0xAA54,                         // InitTSMAwareApplication (D0 the result)
                0x3C00,                                 // MOVE.W D0,D6
                0xA9F4,
            ])
            #expect(reason == "ExitToShell" && g_cpu!.pointee.d.7 & 0xFFFF == 0)
        }

        @Test func serviceWindowsFloatInFront() {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            wr16(r, 50); wr16(r + 2, 50); wr16(r + 4, 150); wr16(r + 6, 200)
            let other = tb_NewWindow(0, r, 0, 1, 0, 0xFFFF_FFFF, 0, 0)
            let wVar = tb_NewPtr(4, false, true)
            wr16(r, 60); wr16(r + 2, 60); wr16(r + 4, 120); wr16(r + 6, 160)
            #expect(tb_NewCServiceWindow(0, r, 0, 1, 0, 0, 0, 0x7356_6373, wVar) == 0)
            let sw = rd32(wVar)
            #expect(sw != 0 && sw != other)
            let front = tb_NewPtr(4, false, true)
            #expect(tb_GetFrontServiceWindow(front) == 0 && rd32(front) == sw)
            #expect(tb_FindServiceWindow(0x0064_0064, front) != 0 && rd32(front) == sw) // (100, 100)
            #expect(tb_FindServiceWindow(0x008C_00B4, front) == 0 && rd32(front) == 0)  // the other window's
            #expect(tb_CloseServiceWindow(sw) == 0)
            #expect(tb_CloseServiceWindow(sw) == -50)
            #expect(tb_GetFrontServiceWindow(front) == 0 && rd32(front) == 0)
        }
    }
}

extension BlueBox {
    /// The Display Manager for the one screen, from PowerPC and 68k code.
    @Suite struct DisplayManagerCalls {
        init() { bootMachine() }

        @Test func theOneDisplay() {
            launchTestApp()
            _ = initToolbox()
            let main = rd32(UInt32(LM_MainDevice))
            #expect(ilCall("DMGetFirstScreenDevice", [1]) == rd32(UInt32(LM_DeviceList)))
            let id = tb_NewPtr(4, false, true), gd = tb_NewPtr(4, false, true)
            #expect(ilCall16("DMGetDisplayIDByGDevice", [main, id, 0]) == 0 && rd32(id) == 1)
            #expect(ilCall16("DMGetDisplayIDByGDevice", [0x1234, id, 0]) == -6229)
            #expect(ilCall16("DMGetGDeviceByDisplayID", [1, gd, 0]) == 0 && rd32(gd) == main)
            #expect(ilCall16("DMGetGDeviceByDisplayID", [7, gd, 1]) == 0 && rd32(gd) == main) // failToMain

            // No mirroring, with nothing to mirror to.
            let flag = tb_NewPtr(2, false, true)
            #expect(ilCall16("DMQDIsMirroringCapable", [flag]) == 0 && rd8(flag) == 1)
            #expect(ilCall16("DMCanMirrorNow", [flag]) == 0 && rd8(flag) == 0)
            #expect(ilCall16("DMIsMirroringOn", [flag]) == 0 && rd8(flag) == 0)
            #expect(ilCall16("DMMirrorDevices", [main, main, 0]) == -6222)
            #expect(ilCall16("DMBlockMirroring", []) == 0)
            #expect(ilCall16("DMMirrorDevices", [main, main, 0]) == -6223)
            #expect(ilCall16("DMUnblockMirroring", []) == 0)
            #expect(ilCall16("DMUnmirrorDevice", [main, 0]) == -6225)
            #expect(ilCall16("DMGetNextMirroredDevice", [main, gd]) == 0 && rd32(gd) == 0)

            // A configuration that changes nothing.
            let state = tb_NewPtr(4, false, true)
            #expect(ilCall16("DMBeginConfigureDisplays", [state]) == 0 && rd32(state) != 0)
            #expect(ilCall16("DMMoveDisplay", [main, 0, 0, rd32(state)]) == 0)
            #expect(ilCall16("DMMoveDisplay", [main, 10, 0, rd32(state)]) == -6231)
            #expect(ilCall16("DMDisableDisplay", [main, rd32(state)]) == -6231)
            #expect(ilCall16("DMRemoveDisplay", [main, rd32(state)]) == -6231)
            #expect(ilCall16("DMDisposeDisplay", [0x1234, rd32(state)]) == -6229)
            #expect(ilCall16("DMEnableDisplay", [main, rd32(state)]) == 0)
            #expect(ilCall16("DMSetMainDisplay", [main, rd32(state)]) == 0)
            #expect(ilCall16("DMAddDisplay", [main, 0, 0, 0, 0, 0, rd32(state)]) == -6230)
            #expect(ilCall16("DMNewDisplay", [gd, UInt32(bitPattern: -50), 0, 0, 2, 0, rd32(state)]) == -6228 && rd32(gd) == 0)
            #expect(ilCall16("DMEndConfigureDisplays", [rd32(state)]) == 0)

            #expect(ilCall16("DMSetDisplayComponent", [main, 0xC10003]) == 0)
            #expect(ilCall16("DMGetDisplayComponent", [main, id]) == 0 && rd32(id) == 0xC10003)
            #expect(ilCall16("DMResolveDisplayComponents", []) == 0)
            #expect(ilCall16("DMDisposePortComponent", [0xC10003]) == -6229)
            #expect(ilCall16("DMRegisterExtendedNotifyProc", [0x5000, 7, 0, 0]) == 0)
            #expect(ilCall16("DMRemoveExtendedNotifyProc", [0x5000, 7, 0, 0]) == 0)
            #expect(ilCall16("DMRemoveExtendedNotifyProc", [0x5000, 7, 0, 0]) == -6229)
            #expect(ilCall16("DMSendDependentNotification", [fourCC("dpnd"), fourCC("disp"), 1, 0]) == 0)
            let a5 = tb_NewPtr(4, false, true)
            #expect(ilCall16("DMGetDisplayMgrA5World", [a5]) == 0 && rd32(a5) == rd32(UInt32(LM_CurrentA5)))

            // The desktop drawn where a rectangle meets it.
            let r = tb_NewPtr(8, false, true)
            wr16(r, 100); wr16(r + 2, 100); wr16(r + 4, 120); wr16(r + 6, 120)
            ilCall("DMDrawDesktopRect", [r])
            let rgn = tb_NewRgn()
            tb_RectRgn(rgn, r)
            ilCall("DMDrawDesktopRegion", [rgn])

            // The mode it is in, which is all that can be set.
            let sw = tb_NewPtr(12, false, true)
            #expect(ilCall16("DMGetDisplayMode", [main, sw]) == 0 && rd32(sw + 2) == 0x80)
            let depth = tb_NewPtr(4, false, true)
            wr32(depth, UInt32(rd16(sw)))
            #expect(ilCall16("DMSetDisplayMode", [main, 0x80, depth, 0, 0]) == 0)
            #expect(ilCall16("DMSetDisplayMode", [main, 0x81, depth, 0, 0]) == -50)

            // The display list: the one display; no AV components.
            let count = tb_NewPtr(4, false, true), list = tb_NewPtr(4, false, true)
            #expect(tb_DMNewDisplayList(0, 0, 0, count, list) == 0 && rd32(count) == 1)
            #expect(tb_DMGetIndexedDisplayFromList(rd32(list), 1, 0, 0, 0) == -6229)
            #expect(tb_DMDisposeList(rd32(list)) == 0)
            #expect(tb_DMNewAVDeviceList(0, 0, 0, count, list) == 0 && rd32(count) == 0)
            #expect(tb_DMGetIndexedComponentFromList(rd32(list), 0, 0, 0, 0) == -6229)
            let name = tb_NewPtr(256, false, true)
            #expect(tb_DMGetNameByAVID(1, 0, name) == 0 && pstring(at: name) == "Display")
            #expect(tb_DMGetEnableByAVID(1, flag, flag + 1) == 0 && rd8(flag) == 1 && rd8(flag + 1) == 0)
            #expect(tb_DMSetEnableByAVID(1, 0, 0) == -6231)
            #expect(tb_DMGetGammaByAVID(1, 0, name) == -6228)
            #expect(tb_DMDisplayGestalt(1, 0, name) == -5551)
            #expect(tb_DMConfirmConfiguration(0, 0, 0, 0) == 0)

            // From 68k code: DMGetFirstScreenDevice and DMGetDeskRegion.
            let desk = tb_NewPtr(4, false, true)
            let reason = run68k([
                0x42A7,                               // CLR.L -(SP)
                0x1F3C, 0x0001,                       // MOVE.B #1,-(SP)  activeOnly
                0x7000, 0xABEB,                       // MOVEQ #0,D0; _DisplayDispatch
                0x2E1F,                               // MOVE.L (SP)+,D7
                0x4267,                               // CLR.W -(SP)
                0x2F3C, hiWord(desk), loWord(desk),   // MOVE.L #desk,-(SP)
                0x303C, 0x0213, 0xABEB,               // DMGetDeskRegion
                0x3C1F,                               // MOVE.W (SP)+,D6
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 == rd32(UInt32(LM_DeviceList)))
            #expect(g_cpu!.pointee.d.6 & 0xFFFF == 0 && rd32(desk) == rd32(UInt32(LM_GrayRgn)))
        }
    }
}

extension BlueBox {
    /// The Color Picker: PickColor through the host's colour panel.
    @Suite struct ColorPickerCalls {
        init() { bootMachine() }

        @Test func pickColor() {
            let info = tb_NewPtr(312, false, true)
            wr16(info + 4, 0x1111); wr16(info + 6, 0x2222); wr16(info + 8, 0x3333)
            guestPString("Pick one", at: info + 42)
            machine_set_color_picker({ prompt, inRGB, out, _ in
                guard let prompt, let inRGB, let out else { return false }
                if String(cString: prompt) != "Pick one" || inRGB[0] != 0x1111 { return false }
                out[0] = 0xFFFF; out[1] = 0x8000; out[2] = 0
                return true
            }, nil)
            #expect(ilCall16("PickColor", [info]) == 0)
            #expect(rd16(info + 4) == 0xFFFF && rd16(info + 6) == 0x8000 && rd16(info + 8) == 0 && rd8(info + 310) == 1)
            // Cancelled: the colour stays, none chosen, no error.
            machine_set_color_picker({ _, _, _, _ in false }, nil)
            #expect(ilCall16("NPickColor", [info]) == 0)
            #expect(rd16(info + 4) == 0xFFFF && rd8(info + 310) == 0)
            machine_set_color_picker(nil, nil)
            // 68k: Pack12 selector $0213.
            let reason = run68k([
                0x4267,                                // CLR.W -(SP)
                0x2F3C, hiWord(info), loWord(info),    // MOVE.L #info,-(SP)
                0x3F3C, 0x0213, 0xA82E,                // _Pack12 PickColor
                0x3E1F, 0xA9F4,                        // MOVE.W (SP)+,D7
            ])
            #expect(reason == "ExitToShell" && g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd8(info + 310) == 0)
            let p = tb_NewPtr(4, false, true)
            #expect(ilCall16("CreateColorDialog", [info, p]) == -4004)
            #expect(ilCall16("GetPickerColor", [0, 0, p]) == -50)
        }
    }
}

extension BlueBox {
    /// MPW's C forms of Toolbox calls: Points by address, C strings.
    @Suite struct CForms {
        init() { bootMachine() }

        func cstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            for (i, b) in Array(s.utf8).enumerated() { wr8(p + UInt32(i), b) }
            return p
        }
        func cstring(_ p: UInt32) -> String {
            var b = [UInt8]()
            var i: UInt32 = 0
            while rd8(p + i) != 0 { b.append(rd8(p + i)); i += 1 }
            return String(decoding: b, as: UTF8.self)
        }

        @Test func everyPascalFormIsThere() {
            var buf = [CChar](repeating: 0, count: 1024)
            let ok = cglue_check(&buf, 1024)
            #expect(ok, "\(String(cString: buf))")
        }

        @Test func pointsAndStrings() {
            launchTestApp()
            _ = initToolbox()
            let a = tb_NewPtr(4, false, true), b = tb_NewPtr(4, false, true)
            wr32(a, 0x0001_0002); wr32(b, 0x0010_0020)
            ilCall("addpt", [a, b])
            #expect(rd32(b) == 0x0011_0022)
            ilCall("subpt", [a, b])
            #expect(rd32(b) == 0x0010_0020)
            #expect(ilCall("equalpt", [b, b]) & 0xFF == 1 && ilCall("equalpt", [a, b]) & 0xFF == 0)
            let r = tb_NewPtr(8, false, true)
            ilCall("pt2rect", [a, b, r])
            #expect(rd16(r) == 1 && rd16(r + 2) == 2 && rd16(r + 4) == 0x10 && rd16(r + 6) == 0x20)
            wr32(a, 0x0005_0005)
            #expect(ilCall("ptinrect", [a, r]) & 0xFF == 1)
            // A C string to Pascal and back.
            let h = ilCall("newstring", [cstr("hello")])
            #expect(h != 0 && pstring(at: rd32(h)) == "hello")
            ilCall("setstring", [h, cstr("bye")])
            #expect(pstring(at: rd32(h)) == "bye")
            #expect(Int16(truncatingIfNeeded: ilCall("iucompstring", [cstr("abc"), cstr("abd")])) == -1)
            #expect(ilCall("iuequalstring", [cstr("abc"), cstr("abc")]) & 0xFFFF == 0)
            let out = tb_NewPtr(256, false, true)
            ilCall("iudatestring", [0, 0, out])
            #expect(cstring(out) == "1/1/04")
            let vol = tb_NewPtr(256, false, true), vref = tb_NewPtr(2, false, true)
            #expect(ilCall16("getvol", [vol, vref]) == 0 && !cstring(vol).isEmpty)
        }
    }
}

extension BlueBox {
    /// The Script Manager's tokenizer and its other remaining calls.
    @Suite struct ScriptUtilities {
        init() { bootMachine() }

        func text(_ s: String) -> (UInt32, Int) {
            let b = Array(s.utf8)
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return (p, b.count)
        }

        @Test func intlTokenize() {
            let (src, n) = text("x1 := 3.25 + foo; \"hi\" (*note*)\r")
            let tb = tb_NewPtr(100, false, true), tokens = tb_NewPtr(14 * 32, false, true)
            let strings = tb_NewPtr(256, false, true)
            wr32(tb, src); wr32(tb + 4, UInt32(n))
            wr32(tb + 8, tokens); wr32(tb + 12, 32)
            wr32(tb + 20, strings); wr32(tb + 24, 256)
            wr8(tb + 32, 1)                  // doString
            wr8(tb + 34, 1)                  // doAlphanumeric
            wr16(tb + 36, 51); wr16(tb + 38, 0xFFFF) // "..." literals
            wr16(tb + 40, 51); wr16(tb + 42, 0xFFFF)
            wr16(tb + 44, 16); wr16(tb + 46, 26)    // (* ... *)
            wr16(tb + 48, 0xFFFF); wr16(tb + 50, 0xFFFF)
            wr16(tb + 52, 26); wr16(tb + 54, 17)
            wr16(tb + 56, 0xFFFF); wr16(tb + 58, 0xFFFF)
            wr16(tb + 62, 46)                // the decimal point
            #expect(tb_IntlTokenize(tb) == 0)
            let count = Int(rd32(tb + 16))
            let kinds = (0..<count).map { Int16(bitPattern: rd16(tokens + UInt32($0 * 14))) }
            // x1, white, :=, white, 3.25, white, +, white, foo, ;, white, "  hi  ", white, (* *), new line
            #expect(kinds == [4, 1, 39, 1, 12, 1, 24, 1, 4, 53, 1, 2, 9, 3, 1, 7, 8, 6])
            #expect(pstring(at: rd32(tokens + 10)) == "x1")
            #expect(rd32(tokens + 4 * 14 + 6) == 4 && rd32(tokens + 4 * 14 + 2) == src + 6) // "3.25"
            // Too few tokens allowed.
            wr32(tb + 12, 3)
            #expect(tb_IntlTokenize(tb) == 1 && rd32(tb + 16) == 3) // tokenOverflow
        }

        @Test func otherScriptCalls() {
            launchTestApp()
            _ = initToolbox()
            // A format back to its text, with its parts.
            let fmt = tb_NewPtr(256, false, true), parts = tb_NewPtr(256, false, true)
            guestPString("#,##0.00;(#,##0.00);0", at: parts) // (used as the input string)
            let canonical = tb_NewPtr(256, false, true)
            #expect(tb_StringToFormatRec(parts, 0, canonical) == 0)
            let positions = tb_NewPtr(12, false, true)
            #expect(tb_FormatRecToString(canonical, 0, fmt, positions) == 0)
            #expect(pstring(at: fmt) == "#,##0.00;(#,##0.00);0")
            #expect(rd16(positions) == 1 && rd16(positions + 2) == 8 && rd16(positions + 4) == 10 && rd16(positions + 10) == 1)

            #expect(tb_GetScriptUtilityAddress(10, 1, 0) == 0)
            tb_SetScriptUtilityAddress(10, 1, 0x1234, 0)
            #expect(tb_GetScriptUtilityAddress(10, 1, 0) == 0x1234 && tb_GetScriptUtilityAddress(10, 0, 0) == 0)
            tb_SetScriptQDPatchAddress(Int16(bitPattern: 0xA884), 0, 1, 0x5678, 1)
            #expect(tb_GetScriptQDPatchAddress(Int16(bitPattern: 0xA884), 0, 1, 1) == 0x5678)

            #expect(Int16(truncatingIfNeeded: ilCall("ScriptOrder", [0, 1])) == -1)
            let a = tb_NewPtr(16, false, true), b = tb_NewPtr(16, false, true)
            guestPString("apple", at: a); guestPString("Apple", at: b)
            #expect(Int16(truncatingIfNeeded: ilCall("IUStringOrder", [a, b, 0, 0, 0, 0])) == tb_IUMagString(a + 1, b + 1, 5, 5))
            #expect(Int16(truncatingIfNeeded: ilCall("IUStringOrder", [a, b, 1, 0, 0, 0])) == 1)
            #expect(Int16(truncatingIfNeeded: ilCall("IUStringOrder", [a, a, 0, 0, 3, 1])) == 1)

            let (t, n) = text("one two")
            let offsets = tb_NewPtr(12, false, true)
            tb_NFindWord(t, Int16(n), 5, 1, 0, offsets)
            #expect(rd16(offsets) == 4 && rd16(offsets + 2) == 7)
            #expect(tb_PortionText(t, Int32(n)) > 0)

            // Fonts: FetchFontInfo leaves the port's font alone.
            let port = qd_port()
            let info = tb_NewPtr(8, false, true), info2 = tb_NewPtr(8, false, true)
            let before = rd16(port + UInt32(GrafPort_txSize_off))
            #expect(tb_FetchFontInfo(0, 24, 0, info) == 0)
            #expect(rd16(port + UInt32(GrafPort_txSize_off)) == before)
            tb_TextSize(24)
            tb_GetFontInfo(info2)
            #expect(rd32(info) == rd32(info2) && rd32(info + 4) == rd32(info2 + 4))
            let r = tb_NewPtr(8, false, true)
            tb_QDTextBounds(Int16(n), t, r)
            #expect(Int16(bitPattern: rd16(r)) == -Int16(bitPattern: rd16(info2)) && rd16(r + 6) == UInt16(tb_TextWidth(t, 0, Int16(n))))

            // StripText and the rest.
            let (u, un) = text("ete")
            wr8(u, 0x8E); wr8(u + 2, 0x8E) // é t é in Mac Roman
            ilCall("StripUpperText", [u, UInt32(un)])
            #expect(rd8(u) == 0x45 && rd8(u + 1) == 0x54)
            ilCall("TempInsertROMMap", [1])
            #expect(rd16(0xB9E) == 0xFFFF)
            #expect(ilCall("GetTrapVector", [0xA9A0]) == UInt32(TB_TRAP_TABLE) + 0x1A0 * 4)
            #expect(ilCall("GetTrapVector", [0xA02E]) == UInt32(OS_TRAP_TABLE) + 0x2E * 4)
        }

        @Test func quickDrawMenusAndCursorComponents() {
            launchTestApp()
            _ = initToolbox()
            let gd = rd32(UInt32(LM_MainDevice))
            let pm = rd32(rd32(gd) + UInt32(GDevice_gdPMap_off))
            #expect(ilCall("GetPixRowBytes", [pm]) == UInt32(rd16(rd32(pm) + 4) & 0x3FFF))
            let port = qd_port(), out = tb_NewPtr(12, false, true)
            wr32(out, 7)
            #expect(ilCall16("GetPortCustomXFerProc", [port, out, out + 4, out + 8]) == 0 && rd32(out) == 0)
            #expect(ilCall16("SetPortCustomXFerProc", [port, 0, 0, 0]) == 0)
            #expect(ilCall16("SetPortCustomXFerProc", [port, 0x1000, 0, 0]) == -4)

            let entry = native_register(testComponent, "TestComponent", 0)
            let cd = tb_NewPtr(20, false, true)
            wr32(cd, fourCC("curs"))
            let comp = ilCall("RegisterComponent", [cd, entry, 0, 0, 0, 0])
            #expect(ilCall16("OpenCursorComponent", [comp, out]) == 0 && rd32(out) != 0)
            let ci = rd32(out)
            #expect(ilCall16("SetCursorComponent", [0]) == 0)
            #expect(ilCall16("SetCursorComponent", [ci]) == -4)
            #expect(ilCall16("CursorComponentChanged", [ci]) == 0)
            #expect(ilCall16("CursorComponentSetData", [ci, 5]) == 0)
            #expect(ilCall16("CloseCursorComponent", [ci]) == 0)
            #expect(ilCall16("OpenCursorComponent", [0, out]) == -3000)

            // A font menu: the fonts there are, after item 0.
            let menu = tb_NewPtr(256, false, true)
            guestPString("Font", at: menu)
            let mh = tb_NewMenu(300, menu)
            ilCall("InsertFontResMenu", [mh, 0, 0xFFFF])
            let fonts = tb_CountMItems(mh)
            let mh2 = tb_NewMenu(301, menu)
            tb_AppendResMenu(mh2, Int32(bitPattern: fourCC("FONT")))
            #expect(fonts == tb_CountMItems(mh2))
        }

        @Test func startupParametersAndManagers() {
            // GetOSDefault through its trap: Mac OS.
            let rec = tb_NewPtr(4, false, true)
            let reason = run68k([
                0x207C, hiWord(rec), loWord(rec), // MOVEA.L #rec,A0
                0xA084,                           // _GetOSDefault
                0xA9F4,
            ])
            #expect(reason == "ExitToShell" && rd8(rec + 1) == 1)
            wr32(rec, 0x0102_0304)
            tb_SetDefaultStartup(rec)
            wr32(rec, 0)
            tb_GetDefaultStartup(rec)
            #expect(rd32(rec) == 0x0102_0304)
            tb_SetTimeout(7)
            tb_GetTimeout(rec)
            #expect(rd16(rec) == 7)

            // The Extension Manager after startup: an empty table.
            let h = tb_NewPtr(4, false, true)
            #expect(ilCall16("InstallExtensionTableHandlerProc", [0x4000, h]) == 0)
            #expect(rd32(rd32(rd32(h))) == 20 && rd32(rd32(rd32(h)) + 4) == 0x100 && rd32(rd32(rd32(h)) + 16) == 0)
            #expect(ilCall16("RemoveExtensionTableHandlerProc", [0x4000]) == 0)
            #expect(ilCall16("InstallExtensionNotificationProc", [0x4004]) == 0)
            #expect(ilCall16("RemoveExtensionNotificationProc", [0x4004]) == 0)
            #expect(ilCall16("RemoveExtensionNotificationProc", [0x4004]) == -50)

            #expect(ilCall16("PrLoadDriver", []) == 0)
            ilCall("PrNoPurge", [])
            ilCall("PrPurge", [])
            #expect(ilCall16("LaunchControlPanel", [rec]) == -4)

            // SetTime sets the clock, which then goes on from there.
            let date = tb_NewPtr(14, false, true)
            wr16(date, 2001); wr16(date + 2, 2); wr16(date + 4, 3)
            wr16(date + 6, 4); wr16(date + 8, 5); wr16(date + 10, 6)
            ilCall("SetTime", [date])
            var secs: UInt32 = 0
            tb_DateToSeconds(date, &secs)
            #expect(rd32(UInt32(LM_Time)) == secs)
            let now = tb_NewPtr(4, false, true)
            tb_GetDateTime(now)
            #expect(rd32(now) >= secs && rd32(now) < secs + 5)
        }
    }
}

nonisolated(unsafe) var avlDisposed: [UInt32] = []
nonisolated(unsafe) var avlWalked: [(key: UInt32, level: UInt32)] = []

/// pascal long (tree, i1, i2, nodeType): the items' keys compared.
let avlCompare: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    _ = cpu_pop16(c)
    let i2 = cpu_pop32(c), i1 = cpu_pop32(c)
    _ = cpu_pop32(c)
    let a = Int64(rd32(i1)), b = Int64(rd32(i2))
    wr32(c.pointee.a.7, UInt32(bitPattern: Int32(a < b ? -1 : a > b ? 1 : 0)))
    cpu_push32(c, ret)
}

/// pascal long (tree, item): 8.
let avlSize: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    c.pointee.a.7 += 8
    wr32(c.pointee.a.7, 8)
    cpu_push32(c, ret)
}

/// pascal void (tree, item)
let avlDispose: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    let item = cpu_pop32(c)
    _ = cpu_pop32(c)
    avlDisposed.append(rd32(item))
    cpu_push32(c, ret)
}

/// pascal OSErr (tree, item, stage, node, level, balance, refCon): the
/// in-order visits noted; stops (-1) after refCon of them when it is not 0.
let avlWalk: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    let refCon = cpu_pop32(c)
    _ = cpu_pop32(c)
    let level = cpu_pop32(c)
    _ = cpu_pop16(c)
    let stage = cpu_pop16(c)
    let item = cpu_pop32(c)
    _ = cpu_pop32(c)
    var r: Int16 = 0
    if stage == 1 {
        avlWalked.append((rd32(item), level))
        if refCon != 0 && avlWalked.count == Int(refCon) { r = -1 }
    }
    wr16(c.pointee.a.7, UInt16(bitPattern: r))
    cpu_push32(c, ret)
}
