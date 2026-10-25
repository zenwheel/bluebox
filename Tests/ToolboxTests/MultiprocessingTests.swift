import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// The queues a test task uses: it waits on the first for a number, puts
/// twice that on the second, keeps a storage value, and ends with 99.
nonisolated(unsafe) var mpIn: UInt32 = 0
nonisolated(unsafe) var mpOut: UInt32 = 0
nonisolated(unsafe) var mpStorageIndex: UInt32 = 0
nonisolated(unsafe) var mpTaskSeen: [UInt32] = []

let mpTaskEntry: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    let param = ceArg(c, 0)
    mpTaskSeen.append(ceCallFrom(c, "MPCurrentTaskID", []))
    ceCallFrom(c, "MPSetTaskStorageValue", [mpStorageIndex, param])
    let p1 = tb_NewPtr(12, false, true)
    let err = ceCallFrom(c, "MPWaitOnQueue", [mpIn, p1, p1 + 4, p1 + 8, 0x7FFF_FFFF])
    mpTaskSeen.append(err)
    mpTaskSeen.append(ceCallFrom(c, "MPGetTaskStorageValue", [mpStorageIndex]))
    ceCallFrom(c, "MPYield", [])
    ceCallFrom(c, "MPNotifyQueue", [mpOut, rd32(p1) * 2, 0, 0])
    ceReturn(c, 99)
}

/// A task that waits on a semaphore for ever (to be terminated).
let mpWaiter: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
    guard let c else { return }
    ceReturn(c, ceCallFrom(c, "MPWaitOnSemaphore", [ceArg(c, 0), 0x7FFF_FFFF]))
}

extension BlueBox {
    /// Multiprocessing Services as CarbonLib gives them on one processor:
    /// tasks run cooperatively on Thread Manager threads; queues,
    /// semaphores, critical regions, event groups, notifications, timers
    /// and memory.
    @Suite(.serialized) struct Multiprocessing {
        init() {
            bootMachine()
            cfm_reset() // native routines' transition vectors made afresh in this machine
            _ = cfm_ppc_cpu()
            mpTaskSeen = []
        }

        @discardableResult
        func call(_ name: String, _ args: [UInt32] = []) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("\(name) is not exported")
                return 0xFFFF_FFFF
            }
            fn(&g_ppc)
            return g_ppc.gpr.3
        }

        func err(_ name: String, _ args: [UInt32] = []) -> Int32 { Int32(bitPattern: call(name, args)) }

        func out() -> UInt32 { tb_NewPtr(16, false, true) }

        let forever: UInt32 = 0x7FFF_FFFF
        let timeout: Int32 = -29296

        @Test func libraryAndProcessors() {
            #expect(call("MPProcessors") == 1 && call("MPProcessorsScheduled") == 1)
            #expect(call("_MPIsFullyInitialized") & 0xFF == 1)
            let v = out()
            call("_MPLibraryVersion", [v, v + 4, v + 8, v + 12, 0])
            #expect(rd32(v + 4) == 2 && rd32(v + 8) == 2)
            #expect(call("_MPLibraryIsCompatible", [0, 2, 0, 0, 0]) & 0xFF == 1)
            #expect(call("_MPLibraryIsCompatible", [0, 3, 0, 0, 0]) & 0xFF == 0)
            #expect(call("MPTaskIsPreemptive", [call("MPCurrentTaskID")]) & 0xFF == 0)
        }

        @Test func semaphoresQueuesRegionsAndEvents() {
            let id = out()
            #expect(err("MPCreateSemaphore", [2, 1, id]) == 0)
            let s = rd32(id)
            #expect(err("MPWaitOnSemaphore", [s, 0]) == 0)
            #expect(err("MPWaitOnSemaphore", [s, 0]) == timeout)
            #expect(err("MPSignalSemaphore", [s]) == 0 && err("MPSignalSemaphore", [s]) == 0)
            #expect(err("MPSignalSemaphore", [s]) == -29298) // at its maximum
            #expect(err("MPDeleteSemaphore", [s]) == 0 && err("MPSignalSemaphore", [s]) == -29299)
            #expect(err("MPCreateSemaphore", [1, 2, id]) == -50)

            #expect(err("MPCreateQueue", [id]) == 0)
            let q = rd32(id), m = out()
            for i: UInt32 in 1...3 { #expect(err("MPNotifyQueue", [q, i, i * 10, i * 100]) == 0) }
            for i: UInt32 in 1...3 {
                #expect(err("MPWaitOnQueue", [q, m, m + 4, m + 8, 0]) == 0)
                #expect(rd32(m) == i && rd32(m + 4) == i * 10 && rd32(m + 8) == i * 100)
            }
            #expect(err("MPWaitOnQueue", [q, m, 0, 0, 0]) == timeout)
            // A wait with a timeout lets time pass, then gives up.
            #expect(err("MPWaitOnQueue", [q, m, 0, 0, 5]) == timeout)

            #expect(err("MPCreateCriticalRegion", [id]) == 0)
            let r = rd32(id)
            #expect(err("MPEnterCriticalRegion", [r, 0]) == 0 && err("MPEnterCriticalRegion", [r, 0]) == 0)
            #expect(err("MPExitCriticalRegion", [r]) == 0 && err("MPExitCriticalRegion", [r]) == 0)
            #expect(err("MPExitCriticalRegion", [r]) != 0) // not entered

            #expect(err("MPCreateEvent", [id]) == 0)
            let e = rd32(id)
            #expect(err("MPSetEvent", [e, 5]) == 0 && err("MPSetEvent", [e, 2]) == 0)
            #expect(err("MPWaitForEvent", [e, m, 0]) == 0 && rd32(m) == 7)
            #expect(err("MPWaitForEvent", [e, m, 0]) == timeout)
        }

        /// A notification signals, notifies and sets at once; a timer's
        /// notification comes when it expires.
        @Test func notificationsAndTimers() {
            let id = out(), m = out()
            call("MPCreateQueue", [id]); let q = rd32(id)
            call("MPCreateSemaphore", [1, 0, id]); let s = rd32(id)
            call("MPCreateEvent", [id]); let e = rd32(id)
            #expect(err("MPCreateNotification", [id]) == 0)
            let n = rd32(id)
            #expect(err("MPModifyNotification", [n, q, 1, 2, 3]) == 0)
            #expect(err("MPModifyNotification", [n, s, 0, 0, 0]) == 0)
            #expect(err("MPModifyNotification", [n, e, 0x10, 0, 0]) == 0)
            #expect(err("MPModifyNotificationParameters", [n, 4, 7, 8, 9]) == 0) // the queue's
            #expect(err("MPCauseNotification", [n]) == 0)
            #expect(err("MPWaitOnQueue", [q, m, m + 4, m + 8, 0]) == 0 && rd32(m) == 7 && rd32(m + 8) == 9)
            #expect(err("MPWaitOnSemaphore", [s, 0]) == 0)
            #expect(err("MPWaitForEvent", [e, m, 0]) == 0 && rd32(m) == 0x10)

            #expect(err("MPCreateTimer", [id]) == 0)
            let t = rd32(id)
            #expect(err("MPSetTimerNotify", [t, q, 0xAB, 0, 0]) == 0)
            let when = out()
            wr32(when, 2) // a Duration of 2 ms
            #expect(err("MPArmTimer", [t, when, 4 | 1]) == 0) // kMPTimeIsDurationMask, kMPPreserveTimerIDMask
            #expect(err("MPWaitOnQueue", [q, m, 0, 0, 0]) == timeout) // not yet
            #expect(err("MPWaitOnQueue", [q, m, 0, 0, forever]) == 0 && rd32(m) == 0xAB)
            #expect(err("MPCancelTimer", [t, when]) == -29298) // not armed now
            #expect(err("MPDeleteTimer", [t]) == 0 && err("MPDeleteTimer", [t]) == -29299)
            // MPDelayUntil: an UpTime in the future.
            let up = out()
            call("UpTime", [up])
            wr32(up + 4, rd32(up + 4) &+ 25_000) // 1 ms of a 25 MHz time base
            #expect(err("MPDelayUntil", [up]) == 0)
            let now = out()
            call("UpTime", [now])
            #expect(rd64(now) >= rd64(up))
        }

        /// A task runs when the main task waits for it: it waits on a queue
        /// in turn, keeps its own storage value, and its ending notifies its
        /// termination queue with its result.
        @Test func tasksRunWhenTheMainTaskWaits() {
            g_ppc_mode = true
            defer { g_ppc_mode = false }
            let id = out(), m = out()
            call("MPCreateQueue", [id]); mpIn = rd32(id)
            call("MPCreateQueue", [id]); mpOut = rd32(id)
            call("MPCreateQueue", [id]); let term = rd32(id)
            #expect(err("MPAllocateTaskStorageIndex", [id]) == 0)
            mpStorageIndex = rd32(id)
            #expect(err("MPSetTaskStorageValue", [mpStorageIndex, 0x5555]) == 0)
            let entry = cfm_native_tvector(mpTaskEntry, "mpTaskEntry")
            #expect(err("MPCreateTask", [entry, 0x77, 0, term, 11, 22, 0, id]) == 0)
            let task = rd32(id)
            #expect(err("MPNotifyQueue", [mpIn, 21, 0, 0]) == 0)
            #expect(err("MPWaitOnQueue", [mpOut, m, 0, 0, forever]) == 0, "\(String(cString: machine_halt_reason()))")
            #expect(rd32(m) == 42)
            #expect(err("MPWaitOnQueue", [term, m, m + 4, m + 8, forever]) == 0)
            #expect(rd32(m) == 11 && rd32(m + 4) == 22 && rd32(m + 8) == 99)
            #expect(mpTaskSeen == [task, 0, 0x77])
            #expect(call("MPGetTaskStorageValue", [mpStorageIndex]) == 0x5555) // the main task's own
            #expect(err("MPTerminateTask", [task, 0]) == -29299) // ended

            // Terminated while it waits: its status goes to the queue.
            call("MPCreateSemaphore", [1, 0, id]); let s = rd32(id)
            #expect(err("MPCreateTask", [cfm_native_tvector(mpWaiter, "mpWaiter"), s, 0, term, 1, 2, 0, id]) == 0)
            let waiter = rd32(id)
            call("MPYield") // it runs to its wait
            #expect(err("MPTerminateTask", [waiter, 0x1234]) == 0)
            #expect(err("MPWaitOnQueue", [term, m, m + 4, m + 8, 0]) == 0 && rd32(m + 8) == 0x1234)
        }

        @Test func alignedMemory() {
            let p = call("MPAllocateAligned", [100, 12, 1]) // 4096-byte aligned, cleared
            #expect(p != 0 && p % 4096 == 0 && call("MPGetAllocatedBlockSize", [p]) == 100)
            #expect((0..<100).allSatisfy { rd8(p + UInt32($0)) == 0 })
            let q = call("MPAllocate", [16])
            #expect(q % 32 == 0)
            wr32(q, 0xDEAD_BEEF)
            call("MPBlockCopy", [q, p, 4])
            #expect(rd32(p) == 0xDEAD_BEEF)
            call("MPBlockClear", [p, 4])
            #expect(rd32(p) == 0)
            call("MPFree", [p])
            call("MPFree", [q])
        }
    }
}
