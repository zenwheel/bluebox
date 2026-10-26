import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

nonisolated(unsafe) var dsHandled: [(p1: UInt32, p2: UInt32, level: UInt32)] = []

/// A secondary interrupt or software interrupt handler: logs its
/// parameters and the execution level, and answers 7.
let dsHandler: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    dsHandled.append((ceArg(c, 0), ceArg(c, 1), ceCallFrom(c, "CurrentExecutionLevel", [])))
    ceReturn(c, 7)
}

extension BlueBox {
    /// DriverServicesLib: strings, memory, queues, interrupts and timers.
    @Suite(.serialized) struct OS9DriverServices {
        init() {
            bootMachine()
            cfm_reset()
            _ = cfm_ppc_cpu()
            dsHandled = []
        }

        func cstr(_ s: String, room: Int = 64) -> UInt32 {
            let p = tb_NewPtr(Int32(room), false, true)
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func read(_ p: UInt32) -> String {
            var bytes: [UInt8] = []
            var i: UInt32 = 0
            while rd8(p + i) != 0 { bytes.append(rd8(p + i)); i += 1 }
            return String(decoding: bytes, as: UTF8.self)
        }

        func pas(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        @Test func cStrings() {
            let a = cstr("hello"), b = cstr(" world")
            #expect(ilCall("CStrLen", [a]) == 5)
            #expect(ilCall("CStrCat", [a, b]) == a && read(a) == "hello world")
            let d = cstr("", room: 32)
            #expect(ilCall("CStrCopy", [d, a]) == d && read(d) == "hello world")
            #expect(ilCall("CStrNCopy", [cstr("xxxxxxxxxx"), a, 5]) != 0)
            let e = cstr("", room: 32)
            _ = ilCall("CStrNCat", [e, a, 3])
            #expect(read(e) == "hel")
            #expect(Int16(truncatingIfNeeded: ilCall("CStrCmp", [cstr("abc"), cstr("abd")])) == -1)
            #expect(ilCall("CStrCmp", [cstr("abc"), cstr("abc")]) == 0)
            #expect(Int16(truncatingIfNeeded: ilCall("CStrCmp", [cstr("abcd"), cstr("abc")])) == 1)
            #expect(ilCall("CStrNCmp", [cstr("abcx"), cstr("abcy"), 3]) == 0)
            #expect(Int16(truncatingIfNeeded: ilCall("CStrNCmp", [cstr("abcx"), cstr("abcy"), 4])) == -1)
        }

        @Test func pascalStrings() {
            let a = pas("hello"), b = pas(" world")
            #expect(ilCall("PStrLen", [a]) == 5)
            _ = ilCall("PStrCat", [a, b])
            #expect(pstring(at: a) == "hello world")
            let d = pas("")
            _ = ilCall("PStrNCopy", [d, a, 4])
            #expect(pstring(at: d) == "hell")
            _ = ilCall("PStrNCat", [d, b, 2])
            #expect(pstring(at: d) == "hell w")
            _ = ilCall("PStrCopy", [d, b])
            #expect(pstring(at: d) == " world")
            let c = cstr("", room: 32)
            ilCall("PStrToCStr", [c, a])
            #expect(read(c) == "hello world")
            let p = tb_NewPtr(256, false, true)
            ilCall("CStrToPStr", [p, c])
            #expect(pstring(at: p) == "hello world")
            #expect(Int16(truncatingIfNeeded: ilCall("PStrCmp", [pas("a"), pas("b")])) == -1)
            #expect(ilCall("PStrCmp", [pas("ab"), pas("ab")]) == 0)
            #expect(Int16(truncatingIfNeeded: ilCall("PStrNCmp", [pas("abz"), pas("aby"), 2])) == 0)
        }

        @Test func memoryAndCaches() {
            let m = ilCall("PoolAllocateResident", [100, 1])
            #expect(m != 0 && (0..<100).allSatisfy { rd8(m + UInt32($0)) == 0 })
            #expect(ilCall("PoolDeallocate", [m]) == 0)
            let p = ilCall("MemAllocatePhysicallyContiguous", [5000, 0])
            #expect(p != 0 && ilCall("MemDeallocatePhysicallyContiguous", [p]) == 0)
            #expect(ilCall("GetLogicalPageSize", []) == 4096 && ilCall("GetDataCacheLineSize", []) == 32)
            let src = tb_NewPtr(16, false, true), dst = tb_NewPtr(16, false, true)
            for i in 0..<8 { wr8(src + UInt32(i), UInt8(i + 1)) }
            ilCall("BlockCopy", [src, dst, 8])
            #expect((0..<8).allSatisfy { rd8(dst + UInt32($0)) == UInt8($0 + 1) })
            #expect(ilCall("DeviceProbe", [src, dst + 8, 2]) == 0 && rd16(dst + 8) == 0x0102)
            #expect(Int32(bitPattern: ilCall("DeviceProbe", [0xF000_0000, dst, 4])) == -200)
            #expect(Int32(bitPattern: ilCall("DeviceProbe", [src, dst, 3])) == -50)
        }

        @Test func preparedMemoryAndPages() {
            let area = tb_NewPtr(3 * 4096 + 64, false, true)
            let t = tb_NewPtr(64, false, true), phys = tb_NewPtr(64, false, true)
            wr32(t, 0)                  // one range
            wr32(t + 28, 16)            // room for 16 entries
            wr32(t + 36, phys)
            wr32(t + 40, area + 100)
            wr32(t + 44, 5000)
            #expect(ilCall("PrepareMemoryForIO", [t]) == 0)
            #expect(rd32(t + 24) == 5000 && rd32(t + 4) == 1) // all of it, done
            let pages = rd32(t + 28)
            #expect(pages == 2 || pages == 3)
            #expect(rd32(phys) == (area + 100) & ~4095)
            #expect(ilCall("CheckpointIO", [rd32(t + 8), 0]) == 0)
            #expect(Int32(bitPattern: ilCall("CheckpointIO", [rd32(t + 8), 0])) == -50) // finished already
            // Room for one page only: partly prepared.
            wr32(t + 28, 1)
            #expect(ilCall("PrepareMemoryForIO", [t]) == 0 && rd32(t + 4) == 0 && rd32(t + 24) < 5000)

            let info = tb_NewPtr(64, false, true)
            wr32(info + 4, 4)
            #expect(ilCall("GetPageInformation", [UInt32(bitPattern: -1), area, 8192, 1, info]) == 0)
            #expect(rd32(info + 4) == 2 || rd32(info + 4) == 3)
            #expect(rd32(info + 8) & 0x20 != 0)
            #expect(Int32(bitPattern: ilCall("GetPageInformation", [UInt32(bitPattern: -1), area, 8192, 9, info])) == -50)
        }

        @Test func queues() {
            let qp = tb_NewPtr(4, false, true)
            #expect(ilCall("PBQueueCreate", [qp]) == 0)
            let q = rd32(qp)
            let e1 = tb_NewPtr(16, false, true), e2 = tb_NewPtr(16, false, true), e3 = tb_NewPtr(16, false, true)
            ilCall("PBEnqueue", [e1, q])
            #expect(ilCall("PBEnqueueLast", [e2, q]) == 0 && ilCall("PBEnqueueLast", [e3, q]) == 0)
            let out = tb_NewPtr(4, false, true)
            #expect(ilCall("PBDequeueLast", [q, out]) == 0 && rd32(out) == e3)
            #expect(ilCall("PBDequeueFirst", [q, out]) == 0 && rd32(out) == e1)
            #expect(ilCall("PBDequeue", [e2, q]) == 0)
            #expect(Int32(bitPattern: ilCall("PBDequeueFirst", [q, out])) == -1 && rd32(out) == 0)
            #expect(ilCall("PBQueueInit", [q]) == 0 && ilCall("PBQueueDelete", [q]) == 0)
        }

        @Test func softwareInterruptsAndTimers() {
            launchTestApp()
            let h = cfm_native_tvector(dsHandler, "dsHandler")
            let id = tb_NewPtr(4, false, true)
            #expect(ilCall("CurrentExecutionLevel", []) == 0)
            #expect(ilCall("CreateSoftwareInterrupt", [h, 0, 0x11, 1, id]) == 0)
            #expect(ilCall("SendSoftwareInterrupt", [rd32(id), 0x22]) == 0)
            #expect(ilCall("SendSoftwareInterrupt", [rd32(id), 0x33]) == 0) // persistent
            #expect(dsHandled.map { $0.p1 } == [0x11, 0x11] && dsHandled.map { $0.p2 } == [0x22, 0x33])
            #expect(ilCall("DeleteSoftwareInterrupt", [rd32(id)]) == 0)
            #expect(ilCall("SendSoftwareInterrupt", [rd32(id), 1]) != 0)
            dsHandled = []
            #expect(ilCall("CallSecondaryInterruptHandler2", [h, 0, 5, 6]) == 7)
            #expect(dsHandled.count == 1 && dsHandled[0].level == 5)
            #expect(ilCall("CurrentExecutionLevel", []) == 0)

            // Queued: at the next pump.  A timer already due runs then; one for later waits.
            dsHandled = []
            #expect(ilCall("QueueSecondaryInterruptHandler", [h, 0, 8, 9]) == 0)
            let now = tb_NewPtr(8, false, true)
            let t0 = ilCall("UpTime", [now])
            _ = t0
            let past = tb_NewPtr(8, false, true), future = tb_NewPtr(8, false, true)
            wr32(past, 0); wr32(past + 4, 1)
            wr32(future, 0x7FFF_FFFF); wr32(future + 4, 0)
            let t1 = tb_NewPtr(4, false, true), t2 = tb_NewPtr(4, false, true)
            #expect(ilCall("SetInterruptTimer", [past, h, 0x51, t1]) == 0)
            #expect(ilCall("SetInterruptTimer", [future, h, 0x52, t2]) == 0)
            #expect(dsHandled.isEmpty)
            time_pump()
            #expect(dsHandled.map { $0.p1 } == [8, 0x51])
            let left = tb_NewPtr(8, false, true)
            #expect(ilCall("CancelTimer", [rd32(t2), left]) == 0 && rd32(left) > 0)
            #expect(ilCall("CancelTimer", [rd32(t1), left]) != 0) // already gone
            dsHandled = []
            time_pump()
            #expect(dsHandled.isEmpty)
        }

        @Test func interruptSets() {
            let setp = tb_NewPtr(4, false, true), child = tb_NewPtr(4, false, true)
            #expect(ilCall("CreateInterruptSet", [0, 0, 4, setp, 0x10]) == 0)
            let set = rd32(setp)
            #expect(ilCall("CreateInterruptSet", [set, 2, 8, child, 0]) == 0)
            #expect(Int32(bitPattern: ilCall("CreateInterruptSet", [set, 5, 8, child, 0])) == -50) // no member 5
            #expect(ilCall("InstallInterruptFunctions", [set, 2, 0xAA, 0xBB, 0xCC, 0xDD]) == 0)
            let out = tb_NewPtr(16, false, true)
            #expect(ilCall("GetInterruptFunctions", [set, 2, out, out + 4, out + 8, out + 12]) == 0)
            #expect(rd32(out) == 0xAA && rd32(out + 4) == 0xBB && rd32(out + 8) == 0xCC && rd32(out + 12) == 0xDD)
            #expect(ilCall("RemoveInterruptFunctions", [set, 2]) == 0)
            #expect(ilCall("GetInterruptFunctions", [set, 2, out, out + 4, out + 8, out + 12]) == 0 && rd32(out + 4) == 0)
            #expect(ilCall("GetInterruptSetOptions", [set, out]) == 0 && rd32(out) == 0x10)
            #expect(ilCall("ChangeInterruptSetOptions", [set, 0x20]) == 0)
            #expect(ilCall("GetInterruptSetOptions", [set, out]) == 0 && rd32(out) == 0x20)
            #expect(ilCall("DeleteInterruptSet", [set]) == 0)
            #expect(Int32(bitPattern: ilCall("GetInterruptSetOptions", [set, out])) == -50)
        }

        @Test func delays() {
            launchTestApp()
            let t0 = machine_time_ns()
            #expect(ilCall("DelayFor", [UInt32(bitPattern: -5000)]) == 0) // 5 ms
            #expect(machine_time_ns() - t0 >= 4_000_000)
            #expect(ilCall("DelayFor", [0]) == 0)
            #expect(ilCall("SynchronizeIO", []) == ilCall("SynchronizeIO", []))
        }
    }
}
