import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// MPLibrary's names beyond Multiprocessing.h's core: the ID iterators,
    /// page sizes, and the older _MP calls.
    @Suite(.serialized) struct OS9MPLibrary {
        init() {
            bootMachine()
            cfm_reset()
            _ = cfm_ppc_cpu()
        }

        @discardableResult
        func call(_ name: String, _ args: [UInt32] = []) -> UInt32 {
            ilCall(name, args)
        }

        func err(_ name: String, _ args: [UInt32] = []) -> Int32 { Int32(bitPattern: call(name, args)) }

        let iterationEnd: Int32 = -29275

        /// Every ID of a class by MPGetNextID.
        func all(_ cls: UInt32) -> [UInt32] {
            let p = tb_NewPtr(4, false, true)
            wr32(p, 0)
            var ids: [UInt32] = []
            while err("MPGetNextID", [cls, p]) == 0 && ids.count < 100 { ids.append(rd32(p)) }
            return ids
        }

        @Test func iteratingIDs() {
            let id = tb_NewPtr(4, false, true)
            #expect(err("MPCreateQueue", [id]) == 0)
            let q1 = rd32(id)
            #expect(err("MPCreateQueue", [id]) == 0)
            let q2 = rd32(id)
            #expect(err("MPCreateSemaphore", [1, 0, id]) == 0)
            let s = rd32(id)
            #expect(all(4) == [q1, q2]) // kOpaqueQueueID
            #expect(all(5) == [s])      // kOpaqueSemaphoreID
            #expect(all(6).isEmpty && all(9).isEmpty)
            #expect(err("MPDeleteQueue", [q1]) == 0)
            #expect(all(4) == [q2])
            // The one processor, process, coherence group and address space.
            #expect(all(7).count == 1 && all(1).count == 1 && all(10).count == 1 && all(8).count == 1)
            #expect(all(11).isEmpty && all(13).isEmpty) // areas, consoles
            // From a given ID on; an ID that isn't of the class.
            wr32(id, q2)
            #expect(err("MPGetNextID", [4, id]) == iterationEnd)
            wr32(id, s)
            #expect(err("MPGetNextID", [4, id]) == -29299)
            #expect(err("MPGetNextID", [4, 0]) == -50)
        }

        @Test func iteratingTasks() {
            let main = call("MPCurrentTaskID")
            let id = tb_NewPtr(4, false, true)
            wr32(id, 0)
            #expect(err("MPGetNextTaskID", [0, id]) == 0 && rd32(id) == main)
            #expect(err("MPGetNextTaskID", [0, id]) == iterationEnd)
            #expect(all(2) == [main])
            wr32(id, 0)
            #expect(err("MPGetNextCpuID", [0, id]) == 0 && rd32(id) == all(7)[0])
            #expect(err("MPGetNextAreaID", [0, id]) == iterationEnd)
        }

        @Test func pagesAndOldCalls() {
            #expect(call("MPGetPageSizeClasses") == 1)
            #expect(call("MPGetPageSize", [1]) == 4096 && call("MPGetPageSize", [2]) == 0)
            #expect(call("_MPTaskIsToolboxSafe") == 1)
            let g = tb_NewPtr(4, false, true)
            #expect(err("MPAllocateGlobalTaskStorageIndex", [g]) == 0)
            #expect(err("MPSetTaskStorageValue", [rd32(g), 77]) == 0 && call("MPGetTaskStorageValue", [rd32(g)]) == 77)
            let block = call("_MPAllocateSys", [100])
            #expect(block != 0 && call("MPGetAllocatedBlockSize", [block]) >= 100)
            call("MPFree", [block])
            call("MPYieldWithHint", [0])
            let t = tb_NewPtr(8, false, true)
            wr32(t, 0)
            wr32(t + 4, 0) // now or before: returns at once
            #expect(err("MPDelayUntilSys", [t]) == 0)
        }
    }
}
