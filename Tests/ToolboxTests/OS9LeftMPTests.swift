import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// MPLibrary's calls beyond Multiprocessing.h: areas, pages, pools, OID
    /// tables, the queries and the handlers.
    @Suite(.serialized) struct OS9LeftMP {
        init() { Rest.boot() }

        let invalid: Int32 = -29299, insufficient: Int32 = -29298, end: Int32 = -29275

        func area(_ size: UInt32, attrs: UInt32 = 1) -> (id: UInt32, base: UInt32) {
            let base = Rest.ptr(4), id = Rest.ptr(4)
            wr32(base, 0)
            #expect(Rest.signed("MPCreateArea", [0, base, size, attrs, 3, 12, id]) == 0)
            return (rd32(id), rd32(base))
        }

        @Test func areasAreRealMemory() {
            let a = area(5000)
            #expect(a.base % 4096 == 0)
            for i in 0..<5000 { #expect(rd8(a.base + UInt32(i)) == 0) } // cleared
            wr32(a.base, 0xCAFE_BABE)
            let info = Rest.ptr(24)
            #expect(Rest.signed("MPGetAreaInfo", [a.id, info]) == 0)
            #expect(rd32(info) == 1 && rd32(info + 4) == a.base && rd32(info + 8) == 8192 && rd32(info + 16) == 3)
            let found = Rest.ptr(4)
            #expect(Rest.signed("MPGetAreaFromAddress", [a.base + 7000, found]) == 0 && rd32(found) == a.id)
            #expect(Rest.signed("MPGetAreaFromAddress", [4, found]) == invalid)
            #expect(Rest.signed("MPSetAreaAccess", [a.id, 1]) == 0)
            #expect(Rest.signed("MPGetAreaInfo", [a.id, info]) == 0 && rd32(info + 16) == 1)
            // growing keeps the bytes
            #expect(Rest.signed("MPSetAreaSize", [a.id, 20000]) == 0)
            #expect(Rest.signed("MPGetAreaInfo", [a.id, info]) == 0)
            #expect(rd32(info + 8) == 20480 && rd32(rd32(info + 4)) == 0xCAFE_BABE)
            #expect(Rest.signed("MPDeleteArea", [a.id]) == 0)
            #expect(Rest.signed("MPDeleteArea", [a.id]) == invalid)
            // a base can't be asked for
            let base = Rest.ptr(4), id = Rest.ptr(4)
            wr32(base, 0x0010_0000)
            #expect(Rest.signed("MPCreateArea", [0, base, 4096, 0, 3, 12, id]) == insufficient)
        }

        @Test func aliasesShareBytes() {
            let a = area(8192)
            let al = Rest.ptr(4)
            #expect(Rest.signed("MPCreateAliasArea", [0, a.id, 4096, 4096, 1, al]) == 0)
            let info = Rest.ptr(24)
            #expect(Rest.signed("MPGetAreaInfo", [rd32(al), info]) == 0)
            #expect(rd32(info + 4) == a.base + 4096)
            wr32(a.base + 4096, 1234)
            #expect(rd32(rd32(info + 4)) == 1234)
            #expect(Rest.signed("MPDeleteArea", [a.id]) == insufficient) // an alias lives
            #expect(Rest.signed("MPSetAreaSize", [a.id, 4096]) == insufficient)
            #expect(Rest.signed("MPCreateAliasArea", [0, a.id, 4096, 8192, 1, al]) == -50)
            #expect(Rest.signed("MPDeleteArea", [rd32(al)]) == 0)
            #expect(Rest.signed("MPDeleteArea", [a.id]) == 0)
        }

        @Test func iteratingAreas() {
            let a = area(4096), b = area(4096)
            let p = Rest.ptr(4)
            wr32(p, 0)
            #expect(Rest.signed("MPGetNextAreaID", [0, p]) == 0 && rd32(p) == a.id)
            #expect(Rest.signed("MPGetNextAreaID", [0, p]) == 0)
            wr32(p, a.id)
            #expect(Rest.signed("MPGetNextAreaID", [0, p]) == 0 && rd32(p) == b.id)
            #expect(Rest.signed("MPGetNextAreaID", [0, p]) == end)
            Rest.call("MPDeleteArea", [a.id]); Rest.call("MPDeleteArea", [b.id])
            wr32(p, 0)
            #expect(Rest.signed("MPGetNextAreaID", [0, p]) == end)
        }

        @Test func backingProviderFillsPages() {
            let a = area(8192)
            // a provider that stores its offset argument at the page: stw r4,0(r5); li r3,0; blr
            let provider = Rest.routine([0x9085_0000, 0x3860_0000, 0x4E80_0020])
            #expect(Rest.signed("MPSetAreaBackingProvider", [a.id, provider, 7]) == 0)
            #expect(rd32(a.base) == 0 && rd32(a.base + 4096) == 4096)
            #expect(Rest.signed("MPSetAreaBackingProvider", [0x1234, provider, 7]) == invalid)
        }

        @Test func addressSpaceAndProcessAreTheOneThereIs() {
            let id = Rest.ptr(4)
            #expect(Rest.signed("MPCreateAddressSpace", [id, 0]) == insufficient)
            #expect(Rest.signed("MPCreateProcess", [id, 0]) == insufficient)
            #expect(Rest.signed("MPCreateProcessor", [id, 0]) == insufficient)
            wr32(id, 0)
            #expect(Rest.signed("MPGetNextID", [8, id]) == 0)
            let space = rd32(id)
            #expect(Rest.signed("MPDeleteAddressSpace", [space]) == insufficient)
            #expect(Rest.signed("MPDeleteAddressSpace", [0x77]) == invalid)
            wr32(id, 0)
            #expect(Rest.signed("MPGetNextID", [1, id]) == 0)
            #expect(Rest.signed("MPDeleteProcess", [rd32(id)]) == insufficient)
            wr32(id, 0)
            #expect(Rest.signed("MPGetNextID", [7, id]) == 0)
            #expect(Rest.signed("MPDeleteProcessor", [rd32(id)]) == insufficient)
            // an area made in the address space there is
            let base = Rest.ptr(4), area = Rest.ptr(4)
            #expect(Rest.signed("MPCreateArea", [space, base, 4096, 0, 3, 12, area]) == 0)
            #expect(Rest.signed("MPCreateArea", [0x77, base, 4096, 0, 3, 12, area]) == -50)
        }

        @Test func pagesHeldLockedAndAged() {
            let a = area(3 * 4096)
            let at = Rest.ptr(4)
            #expect(Rest.signed("MPHoldPages", [a.base, 2]) == 0)
            #expect(Rest.signed("MPGetPageAttributes", [a.base, at]) == 0 && rd32(at) & 3 == 1)
            #expect(Rest.signed("MPLockPages", [a.base, 1]) == 0)
            #expect(Rest.signed("MPGetPageAttributes", [a.base, at]) == 0 && rd32(at) & 3 == 3)
            #expect(Rest.signed("MPGetPageAttributes", [a.base + 8192, at]) == 0 && rd32(at) == 0)
            #expect(Rest.signed("MPUnlockPages", [a.base, 1]) == 0)
            #expect(Rest.signed("MPUnlockPages", [a.base, 1]) == -50)
            #expect(Rest.signed("MPUnholdPages", [a.base, 2]) == 0)
            #expect(Rest.signed("MPUnholdPages", [a.base, 2]) == -50)
            #expect(Rest.signed("MPHoldPages", [a.base + 1, 1]) == -50) // not a page
            #expect(Rest.signed("MPSetPageAttributes", [a.base, 1, 0x5 << 16]) == 0)
            #expect(Rest.signed("MPGetPageAttributes", [a.base, at]) == 0 && rd32(at) >> 16 == 5)
        }

        @Test func victimIsTheOldestUnheldPage() {
            let a = area(3 * 4096)
            let v = Rest.ptr(4)
            Rest.call("MPSetPageAge", [a.base, 10])
            Rest.call("MPSetPageAge", [a.base + 4096, 50])
            Rest.call("MPSetPageAge", [a.base + 8192, 30])
            Rest.call("MPHoldPages", [a.base + 4096, 1])
            #expect(Rest.signed("MPFindVictimPage", [v]) == 0 && rd32(v) == a.base + 8192)
            Rest.call("MPUnholdPages", [a.base + 4096, 1])
            #expect(Rest.signed("MPFindVictimPage", [v]) == 0 && rd32(v) == a.base + 4096)
            let age = Rest.ptr(4)
            #expect(Rest.signed("MPGetPageAge", [a.base, age]) == 0 && rd32(age) == 10)
            // the ager is asked of each page: age := page-relative value 7
            let ager = Rest.routine([0x38E0_0007, 0x90E5_0000, 0x3860_0000, 0x4E80_0020]) // li r7,7; stw r7,0(r5)...
            #expect(Rest.signed("MPRegisterAger", [ager, 1]) == 0)
        }

        @Test func freePagesAndPhysicalAddresses() {
            let p = Rest.ptr(4), n0 = Rest.call("MPGetFreePageCount")
            #expect(Rest.signed("MPGetFreePage", [p]) == 0)
            let page = rd32(p)
            #expect(page % 4096 == 0)
            #expect(Rest.signed("MPPutFreePage", [page]) == 0)
            #expect(Rest.call("MPGetFreePageCount") == n0 + 1)
            #expect(Rest.signed("MPPutFreePage", [page + 8]) == -50)
            let phys = Rest.ptr(4), extent = Rest.ptr(4)
            #expect(Rest.signed("MPGetPhysicalAddress", [page, phys]) == 0 && rd32(phys) == page)
            #expect(Rest.signed("MPGetPhysicalExtent", [page, 9000, phys, extent]) == 0 && rd32(extent) == 9000)
            #expect(Rest.signed("MPMapPage", [0, page, page]) == 0)
            let at = Rest.ptr(4)
            Rest.call("MPGetPageAttributes", [page, at])
            #expect(rd32(at) & 4 != 0)
            #expect(Rest.signed("MPMapPage", [0, page, page + 4096]) == insufficient)
            #expect(Rest.signed("MPUnmapPages", [page, 1]) == 0)
            Rest.call("MPGetPageAttributes", [page, at])
            #expect(rd32(at) & 4 == 0)
        }

        @Test func pools() {
            let pool = Rest.ptr(4), stats = Rest.ptr(16)
            #expect(Rest.signed("MPCreatePool", [0, 65536, pool]) == 0)
            let p = rd32(pool)
            let b1 = Rest.call("MPAllocateFromPool", [p, 100, 1])
            let b2 = Rest.call("MPAllocateFromPool", [p, 300, 0])
            #expect(b1 != 0 && b2 != 0 && b1 != b2)
            #expect(Rest.signed("MPGetPoolStatistics", [p, stats]) == 0)
            #expect(rd32(stats + 4) == 2 && rd32(stats + 8) == 400 && rd32(stats + 12) == 400)
            #expect(Rest.signed("MPValidatePool", [p]) == 0)
            #expect(Rest.signed("MPCoalescePool", [p]) == 0)
            Rest.call("MPFree", [b1])
            #expect(Rest.signed("MPGetPoolStatistics", [p, stats]) == 0)
            #expect(rd32(stats + 4) == 1 && rd32(stats + 8) == 300 && rd32(stats + 12) == 400)
            #expect(Rest.signed("MPDeletePool", [p]) == 0)
            #expect(Rest.signed("MPDeletePool", [p]) == invalid)
            #expect(Rest.call("MPAllocateFromPool", [p, 4, 0]) == 0)
            let std = Rest.call("MPGetStandardPoolRef")
            #expect(std != 0 && std == Rest.call("MPGetStandardPoolRef"))
            #expect(Rest.signed("MPDeletePool", [std]) == insufficient)
        }

        @Test func oidTables() {
            let t = Rest.ptr(4), id = Rest.ptr(4), v = Rest.ptr(4), cls = Rest.ptr(4), gen = Rest.ptr(4), idx = Rest.ptr(4)
            #expect(Rest.signed("OIDCreateTable", [5, 0, t]) == 0)
            let tab = rd32(t)
            #expect(Rest.signed("OIDAssignID", [tab, 111, id]) == 0)
            let first = rd32(id)
            #expect(first >> 28 == 5)
            #expect(Rest.signed("OIDAssignID", [tab, 222, id]) == 0)
            let second = rd32(id)
            #expect(first != second)
            #expect(Rest.signed("OIDTranslateID", [tab, first, v]) == 0 && rd32(v) == 111)
            #expect(Rest.signed("OIDSetIDValues", [tab, first, 999]) == 0)
            #expect(Rest.signed("OIDTranslateID", [tab, first, v]) == 0 && rd32(v) == 999)
            #expect(Rest.signed("OIDExtractID", [tab, second, idx]) == 0 && rd32(idx) == 1)
            #expect(Rest.signed("OIDDecomposeID", [second, cls, gen, idx]) == 0)
            #expect(rd32(cls) == 5 && rd32(gen) == 0 && rd32(idx) == 1)
            // released, the ID is dead and a reuse of the slot is a different ID
            #expect(Rest.signed("OIDReleaseID", [tab, first]) == 0)
            #expect(Rest.signed("OIDTranslateID", [tab, first, v]) == invalid)
            #expect(Rest.signed("OIDReleaseID", [tab, first]) == invalid)
            #expect(Rest.signed("OIDAssignID", [tab, 333, id]) == 0)
            #expect(rd32(id) != first && rd32(id) & 0xFFFF == first & 0xFFFF)
            let slots = Rest.ptr(4), used = Rest.ptr(4)
            #expect(Rest.signed("OIDGetArrayInfo", [tab, slots, used]) == 0 && rd32(slots) >= 2 && rd32(used) == 2)
            #expect(Rest.signed("OIDLockTable", [tab]) == 0)
            #expect(Rest.signed("OIDDeleteTable", [tab]) == insufficient)
            #expect(Rest.signed("OIDUnlockTable", [tab]) == 0)
            #expect(Rest.signed("OIDUnlockTable", [tab]) == -50)
            #expect(Rest.signed("OIDDeleteTable", [tab]) == 0)
            // a table with room for two
            #expect(Rest.signed("OIDCreateTable", [1, 2, t]) == 0)
            #expect(Rest.signed("OIDAssignID", [rd32(t), 1, id]) == 0)
            #expect(Rest.signed("OIDAssignID", [rd32(t), 2, id]) == 0)
            #expect(Rest.signed("OIDAssignID", [rd32(t), 3, id]) == insufficient)
        }

        @Test func queries() {
            let id = Rest.ptr(4), info = Rest.ptr(20)
            #expect(Rest.signed("MPCreateQueue", [id]) == 0)
            let q = rd32(id)
            #expect(Rest.signed("MPNotifyQueue", [q, 1, 2, 3]) == 0)
            #expect(Rest.signed("MPNotifyQueue", [q, 4, 5, 6]) == 0)
            #expect(Rest.signed("MPQueryQueue", [q, info]) == 0 && rd32(info) == 1 && rd32(info + 4) == 2)
            #expect(Rest.signed("MPQuerySemaphore", [q, info]) == invalid) // not a semaphore
            #expect(Rest.signed("MPCreateSemaphore", [5, 3, id]) == 0)
            #expect(Rest.signed("MPQuerySemaphore", [rd32(id), info]) == 0 && rd32(info + 4) == 3 && rd32(info + 8) == 5)
            #expect(Rest.signed("MPCreateCriticalRegion", [id]) == 0)
            let region = rd32(id)
            #expect(Rest.signed("MPEnterCriticalRegion", [region, 0]) == 0)
            #expect(Rest.signed("MPQueryCriticalRegion", [region, info]) == 0)
            #expect(rd32(info + 4) == Rest.call("MPCurrentTaskID") && rd32(info + 8) == 1)
            #expect(Rest.signed("MPCreateEvent", [id]) == 0)
            Rest.call("MPSetEvent", [rd32(id), 0x14])
            #expect(Rest.signed("MPQueryEvent", [rd32(id), info]) == 0 && rd32(info + 4) == 0x14)
            #expect(Rest.signed("MPQueryEvent", [rd32(id), 0]) == -50)
            // the main task isn't blocked on anything
            #expect(Rest.signed("MPQueryBlocking", [Rest.call("MPCurrentTaskID"), info]) == 0 && rd32(info + 8) == 0)
            #expect(Rest.signed("MPQueryBlocking", [q, info]) == invalid)
        }

        @Test func handlersAreCalled() {
            let seen = Rest.ptr(16)
            // stw r3,0(seen); stw r4,4(seen); stw r5,8(seen); li r3,0; blr
            let h = Rest.routine(Rest.stash(seen) + [0x906B_0000, 0x908B_0004, 0x90AB_0008, 0x3860_0000, 0x4E80_0020])
            #expect(Rest.signed("MPSetSWIEvent", [1, 5]) == 0) // none registered
            #expect(Rest.signed("MPRegisterPMFHandler", [h, 0x77]) == 0)
            #expect(Rest.signed("MPSetSWIEvent", [1, 5]) == 0)
            #expect(rd32(seen) == 0x77 && rd32(seen + 4) == 1 && rd32(seen + 8) == 5)
            #expect(Rest.signed("MPRegisterThermalHandler", [h, 0x88]) == 0)
            #expect(Rest.signed("MPSetSWIEvent", [2, 9]) == 0)
            #expect(rd32(seen) == 0x88 && rd32(seen + 4) == 2 && rd32(seen + 8) == 9)
            #expect(Rest.signed("MPSetSWIEvent", [3, 9]) == -50)
            let plug = Rest.routine(Rest.stash(seen) + [0x906B_000C, 0x3860_0000, 0x4E80_0020])
            #expect(Rest.signed("MPCpuPlugin", [1, 2, 3]) == 0)
            #expect(Rest.signed("MPRegisterCpuPlugin", [plug, 0x99]) == 0)
            #expect(Rest.signed("MPCpuPlugin", [1, 2, 3]) == 0)
            #expect(rd32(seen + 12) == 0x99)
        }

        @Test func remoteCallsAndSchedulingAndKernelState() {
            let rpc = Rest.call("MPGetSystemRPCQueueID")
            #expect(rpc != 0 && rpc == Rest.call("MPGetSystemRPCQueueID"))
            let reply = Rest.call("MPGetLocalReplyQueue")
            #expect(reply != 0 && reply != rpc)
            // a remote procedure: r3 := r3 + 1
            let proc = Rest.routine([0x3863_0001, 0x4E80_0020])
            #expect(Rest.signed("MPNotifyQueue", [rpc, proc, 41, reply]) == 0)
            #expect(Rest.signed("MPDoPendingRemoteCalls") == 0)
            let m = Rest.ptr(12)
            #expect(Rest.signed("MPWaitOnQueue", [reply, m, m + 4, m + 8, 0]) == 0)
            #expect(rd32(m) == 42)
            let size = Rest.ptr(4), state = Rest.ptr(20)
            #expect(Rest.signed("MPGetKernelStateSize", [size]) == 0 && rd32(size) == 20)
            #expect(Rest.signed("MPGetKernelState", [state, 20]) == 0 && rd32(state + 4) >= 1 && rd32(state + 16) == 1)
            #expect(Rest.signed("MPStopScheduling") == 0)
            #expect(Rest.signed("MPGetKernelState", [state, 20]) == 0 && rd32(state + 16) == 0)
            #expect(Rest.signed("MPStartScheduling") == 0)
            #expect(Rest.signed("MPGetKernelState", [state, 4]) == -50)
        }

        @Test func terminateProcess() {
            let id = Rest.ptr(4)
            wr32(id, 0)
            Rest.call("MPGetNextID", [1, id])
            #expect(Rest.signed("MPTerminateProcess", [rd32(id), 0]) == 0)
            #expect(Rest.signed("MPTerminateProcess", [0x55, 0]) == invalid)
        }
    }
}
