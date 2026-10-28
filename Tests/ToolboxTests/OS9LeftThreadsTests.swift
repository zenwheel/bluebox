import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// ThreadsLib's remaining names on the Thread Manager.
    @Suite(.serialized) struct OS9LeftThreads {
        init() { Rest.boot() }

        let notFound: Int32 = -618, protocolErr: Int32 = -619

        func make(suspended: Bool = true) -> UInt32 {
            let made = Rest.ptr(4)
            let entry = Rest.routine([0x3860_0000, 0x4E80_0020])
            #expect(tb_NewThread(2, entry, 0x77, 0x8000, suspended ? 1 : 0, 0, made) == 0)
            return rd32(made)
        }

        func state(_ id: UInt32) -> UInt16 {
            let p = Rest.ptr(2)
            _ = tb_GetThreadState(Int32(bitPattern: id), p)
            return rd16(p)
        }

        @Test func suspendAndResume() {
            let t = make(suspended: false)
            #expect(state(t) == 0) // ready
            #expect(Rest.signed("SuspendThread", [t]) == 0)
            #expect(state(t) == 1)
            #expect(Rest.signed("ResumeThread", [t]) == 0)
            #expect(state(t) == 0)
            #expect(Rest.signed("ResumeThread", [t]) == protocolErr) // not stopped
            #expect(Rest.signed("ResumeThread", [999]) == notFound)
            #expect(Rest.signed("SuspendThread", [999]) == notFound)
            #expect(Rest.signed("TerminateThread", [t]) == 0)
            #expect(Rest.signed("TerminateThread", [t]) == notFound)
            #expect(Rest.signed("TerminateThread", [2]) == protocolErr) // the application's
        }

        @Test func information() {
            let t = make()
            let info = Rest.ptr(32)
            #expect(Rest.signed("GetThreadInformation", [t, info]) == 0)
            #expect(rd32(info) == 32 && rd32(info + 4) == t && rd32(info + 8) == 1 && rd32(info + 12) == 2)
            #expect(rd32(info + 16) >= 0x8000 && rd32(info + 20) != 0 && rd32(info + 28) == 0x77)
            #expect(Rest.signed("GetThreadInformation", [999, info]) == notFound)
            #expect(Rest.signed("GetThreadInformation", [t, 0]) == -50)
        }

        @Test func threadsInContext() {
            let a = make(), b = make()
            let count = Rest.ptr(4), ids = Rest.ptr(64)
            #expect(Rest.signed("GetThreadsInContext", [1, 0, count, 0]) == 0 && rd32(count) == 3)
            #expect(Rest.signed("GetThreadsInContext", [1, 16, count, ids]) == 0 && rd32(count) == 3)
            #expect(Set((0..<3).map { rd32(ids + UInt32($0 * 4)) }) == [2, a, b])
            #expect(Rest.signed("GetThreadsInContext", [1, 2, count, ids]) == 0 && rd32(count) == 2)
            #expect(Rest.signed("GetThreadsInContext", [9, 16, count, ids]) == -50)
            let ref = Rest.ptr(4)
            #expect(Rest.signed("GetThreadTaskRefGivenProcID", [0, ref]) == 0 && rd32(ref) == 1)
        }

        @Test func registers() {
            let t = make()
            let regs = Rest.ptr(37 * 4), back = Rest.ptr(37 * 4)
            #expect(Rest.signed("ReadWriteThreadRegisters", [t, 0, regs]) == 0)
            #expect(rd32(regs + 4) != 0) // r1: its stack
            wr32(regs + 12, 0x1234_5678) // r3
            #expect(Rest.signed("ReadWriteThreadRegisters", [t, 1, regs]) == 0)
            #expect(Rest.signed("ReadWriteThreadRegisters", [t, 0, back]) == 0)
            #expect(rd32(back + 12) == 0x1234_5678 && rd32(back + 4) == rd32(regs + 4))
            // the running thread's own are the live ones
            #expect(Rest.signed("ReadWriteThreadRegisters", [2, 0, back]) == 0)
            #expect(Rest.signed("ReadWriteThreadRegisters", [999, 0, back]) == notFound)
        }

        @Test func schedulerAndDebuggerProcs() {
            let was = g_ppc_mode
            g_ppc_mode = true // the procedures are PowerPC routines
            defer { g_ppc_mode = was }
            let seen = Rest.ptr(8)
            let out = Rest.ptr(4)
            #expect(Rest.signed("GetThreadScheduler", [out]) == 0 && rd32(out) == 0)
            let sched = Rest.routine([0x3860_0000, 0x4E80_0020])
            #expect(tb_SetThreadScheduler(sched) == 0)
            #expect(Rest.signed("GetThreadScheduler", [out]) == 0 && rd32(out) == sched)
            // the debugger is told of a thread made: stw r3,0(seen)
            let newProc = Rest.routine(Rest.stash(seen) + [0x906B_0000, 0x4E80_0020])
            #expect(Rest.signed("SetDebugProcsGivenTaskRef", [7, newProc, 0, 0]) == -50)
            #expect(Rest.signed("SetDebugProcsGivenTaskRef", [1, newProc, 0, 0]) == 0)
            let t = make()
            #expect(rd32(seen) == t)
        }

        @Test func poolAndCleanup() {
            let free = Rest.ptr(4)
            #expect(Rest.signed("CreateThreadPoolExtended", [2, 3, 0x4000, 0]) == 0)
            #expect(tb_GetFreeThreadCount(2, free) == 0 && rd16(free) == 3)
            #expect(Rest.signed("CreateThreadPoolExtended", [9, 3, 0x4000, 0]) == -50)
            let a = make(), b = make()
            let count = Rest.ptr(4)
            #expect(Rest.signed("CleanupThreadsPowerPC") == 0)
            #expect(Rest.signed("GetThreadsInContext", [1, 0, count, 0]) == 0 && rd32(count) == 1)
            #expect(Rest.signed("SuspendThread", [a]) == notFound && Rest.signed("SuspendThread", [b]) == notFound)
            #expect(tb_GetFreeThreadCount(2, free) == 0 && rd16(free) == 0)
        }
    }
}
