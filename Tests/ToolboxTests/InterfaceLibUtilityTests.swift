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
