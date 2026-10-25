import CCore
import Testing
import Toolbox

extension BlueBox {
    @Suite struct MemoryManager {
        let appZone: UInt32
        let sysZone: UInt32

        init() {
            bootMachine()
            appZone = rd32(UInt32(LM_ApplZone))
            sysZone = rd32(UInt32(LM_SysZone))
        }

        @Test func handlesAllocateResizeAndKeepContents() {
            let h = tb_NewHandle(100, false, false)
            #expect(h != 0)
            #expect(memErr() == 0)
            #expect(tb_GetHandleSize(h) == 100)
            let p = rd32(h)
            for i in 0..<100 { wr8(p + UInt32(i), UInt8(i)) }
            tb_SetHandleSize(h, 5000)
            #expect(memErr() == 0)
            #expect(tb_GetHandleSize(h) == 5000)
            let q = rd32(h)
            #expect((0..<100).allSatisfy { rd8(q + UInt32($0)) == UInt8($0) })
            tb_SetHandleSize(h, 10)
            #expect(tb_GetHandleSize(h) == 10)
            #expect(mm_check_zone(appZone))
            tb_DisposeHandle(h)
            #expect(memErr() == 0)
        }

        /// A locked handle's master pointer stepped into its own block is
        /// the program's business (Bloodsuckers steps it past a length
        /// word); an unlocked one's, or one pointing elsewhere, is damage.
        @Test func zoneCheckLetsALockedHandleBeStepped() {
            let h = tb_NewHandle(64, false, false)
            let p = rd32(h)
            tb_HLock(h)
            wr32(h, p + 4)
            #expect(mm_check_zone(appZone))
            wr32(h, p + 0x10000)
            #expect(!mm_check_zone(appZone))
            wr32(h, p)
            tb_HUnlock(h)
            wr32(h, p + 4)
            #expect(!mm_check_zone(appZone))
            wr32(h, p)
            #expect(mm_check_zone(appZone))
            tb_DisposeHandle(h)
        }

        @Test func clearedAllocationsAreZero() {
            let h = tb_NewHandle(64, false, true)
            #expect((0..<64).allSatisfy { rd8(rd32(h) + UInt32($0)) == 0 })
            let p = tb_NewPtr(64, false, true)
            #expect((0..<64).allSatisfy { rd8(p + UInt32($0)) == 0 })
        }

        @Test func compactionMovesUnlockedBlocksAndUpdatesMasterPointers() {
            // Fill the system zone (fixed size) so a large request needs compaction.
            var handles = [UInt32]()
            while true {
                let h = tb_NewHandle(64 * 1024, true, false)
                if h == 0 { break }
                wr32(rd32(h), h) // tag each block with its own handle
                handles.append(h)
            }
            #expect(memErr() == -108)
            #expect(handles.count > 10)
            // Free every other block: lots of free space, none of it contiguous.
            for (i, h) in handles.enumerated() where i % 2 == 0 { tb_DisposeHandle(h) }
            let survivors = handles.enumerated().filter { $0.offset % 2 == 1 }.map(\.element)
            let locked = survivors[0]
            let lockedAddr = rd32(locked)
            tb_HLock(locked)
            let big = tb_NewHandle(200 * 1024, true, false)
            #expect(big != 0)
            #expect(mm_check_zone(sysZone))
            #expect(rd32(locked) == lockedAddr) // locked blocks do not move
            for h in survivors { #expect(rd32(rd32(h)) == h) } // contents moved with them
        }

        @Test func purgeableBlocksArePurgedUnderPressure() {
            let victim = tb_NewHandle(1024 * 1024, true, false)
            tb_HPurge(victim)
            var others = [UInt32]()
            while true {
                let h = tb_NewHandle(256 * 1024, true, false)
                if h == 0 { break }
                others.append(h)
            }
            #expect(rd32(victim) == 0) // purged: an empty handle
            #expect(tb_GetHandleSize(victim) == -109) // nilHandleErr, as on System 7.1
            tb_ReallocateHandle(victim, 16)
            #expect(memErr() == -108 || rd32(victim) != 0)
        }

        @Test func pointersResizeInPlaceOnly() {
            let p = tb_NewPtr(32, false, false)
            #expect(tb_GetPtrSize(p) == 32)
            tb_SetPtrSize(p, 16)
            #expect(memErr() == 0)
            #expect(tb_GetPtrSize(p) == 16)
            let blocker = tb_NewPtr(16, false, false) // sits right after p
            tb_SetPtrSize(p, 4096)
            if memErr() == 0 {
                #expect(tb_GetPtrSize(p) == 4096)
            } else {
                #expect(memErr() == -108)
                #expect(tb_GetPtrSize(p) == 16)
            }
            tb_DisposePtr(blocker)
            tb_DisposePtr(p)
            #expect(mm_check_zone(appZone))
        }

        @Test func lockedHandleGrowsByMovingTheHandlesAfterIt() {
            // Sirtet Info: its TextEdit text, locked, emptied and filled again.
            let a = tb_NewHandle(16, false, false)
            let follower = tb_NewHandle(64, false, false) // most likely right after a
            for i in 0..<64 { wr8(rd32(follower) + UInt32(i), UInt8(i)) }
            tb_HLock(a)
            let at = rd32(a)
            tb_SetHandleSize(a, 0)
            tb_SetHandleSize(a, 2000)
            #expect(memErr() == 0)
            #expect(tb_GetHandleSize(a) == 2000)
            #expect(rd32(a) == at) // locked: grown where it is
            #expect((0..<64).allSatisfy { rd8(rd32(follower) + UInt32($0)) == UInt8($0) })
            #expect(mm_check_zone(appZone))
            tb_HUnlock(a)
            tb_DisposeHandle(a)
            tb_DisposeHandle(follower)
        }

        @Test func handleStateAndRecovery() {
            let h = tb_NewHandle(8, false, false)
            #expect(tb_HGetState(h) == 0)
            tb_HLock(h)
            tb_HPurge(h)
            #expect(UInt8(bitPattern: tb_HGetState(h)) == 0xC0)
            tb_HSetState(h, 0x20)
            #expect(tb_HGetState(h) == 0x20)
            #expect(tb_RecoverHandle(rd32(h), false) == h)
            #expect(tb_HandleZone(h) == appZone)
            #expect(tb_PtrZone(tb_NewPtr(4, true, false)) == sysZone)
        }

        @Test func handleUtilities() {
            let p = tb_NewPtr(4, false, false)
            for i in 0..<4 { wr8(p + UInt32(i), UInt8(0x41 + i)) }
            var h: UInt32 = 0
            #expect(tb_PtrToHand(p, &h, 4) == 0)
            #expect(tb_PtrAndHand(p, h, 2) == 0)
            #expect(tb_GetHandleSize(h) == 6)
            var copy = h
            #expect(tb_HandToHand(&copy) == 0)
            #expect(copy != h)
            #expect(tb_HandAndHand(h, copy) == 0)
            #expect(tb_GetHandleSize(copy) == 12)
            let bytes = (0..<12).map { rd8(rd32(copy) + UInt32($0)) }
            #expect(bytes == Array("ABCDABABCDAB".utf8))
        }

        @Test func applicationZoneGrowsOnDemand() {
            let before = rd32(UInt32(LM_HeapEnd))
            let h = tb_NewHandle(64 * 1024 * 1024, false, false)
            #expect(h != 0)
            #expect(rd32(UInt32(LM_HeapEnd)) > before)
            #expect(tb_FreeMem(false) <= Int32(g_mem_report_cap))
            #expect(tb_MaxBlock(false) > 0)
            #expect(mm_check_zone(appZone))
        }

        /// A zone made with InitZone inside a block of the application zone
        /// (NCSA Mosaic's text fields): its handles belong to it, and resizing
        /// one leaves the application zone alone.
        @Test func zonesInsideBlocksOwnTheirHandles() {
            let sub = tb_NewPtr(356, false, true)
            let args = tb_NewPtr(14, false, true)
            wr32(args, sub)
            wr32(args + 4, sub + 356)
            wr16(args + 8, 4)
            var c = cpu68k()
            c.a.0 = args
            raw_InitZone(&c, 0xA019)
            #expect(rd32(UInt32(LM_TheZone)) == sub)
            let h = tb_NewHandle(16, false, false)
            #expect(h > sub && h < sub + 356)
            tb_SetZone(appZone)
            #expect(tb_HandleZone(h) == sub)
            let rover = rd32(appZone + UInt32(Zone_allocPtr_off))
            tb_SetHandleSize(h, 20)
            #expect(memErr() == 0 && tb_GetHandleSize(h) == 20)
            #expect(rd32(appZone + UInt32(Zone_allocPtr_off)) == rover)
            #expect(mm_check_zone(sub))
            #expect(mm_check_zone(appZone))
            tb_DisposePtr(sub)
            #expect(mm_check_zone(appZone))
        }

        /// The heap check names the call after which a master pointer stopped
        /// pointing at its block (a Boolean written through a stray pointer).
        @Test func heapCheckFindsABrokenMasterPointer() {
            let h = tb_NewHandle(32, false, true)
            machine_set_heap_check(1)
            defer { machine_set_heap_check(0) }
            #expect(mm_debug_after_call("Fine"))
            wr8(h + 1, rd8(h + 1) ^ 0x01) // as ISpConfigure's "accepted" once did
            #expect(!mm_debug_after_call("Stray"))
            wr8(h + 1, rd8(h + 1) ^ 0x01)
            #expect(mm_debug_after_call("Fine"))
            tb_DisposeHandle(h)
        }

        /// The code check names the call after which loaded code changed
        /// (Greg's Browser's thread stack ran down over its code), once.
        @Test func codeCheckFindsChangedCode() {
            let code = tb_NewPtr(64, false, true)
            machine_set_code_check(1)
            defer { machine_set_code_check(0) }
            code_check_add(code, 64, "test code")
            defer { code_check_remove(code) }
            #expect(code_check_after_call("Fine"))
            wr32(code + 8, 0x26)
            #expect(!code_check_after_call("Stray"))
            #expect(code_check_after_call("Fine"))
            tb_DisposePtr(code)
        }

        @Test func badHandlesSetErrors() {
            tb_HLock(0)
            #expect(memErr() == -109) // nilHandleErr
            let p = tb_NewPtr(16, false, false)
            #expect(tb_GetPtrSize(p) == 16)
            #expect(tb_GetHandleSize(p) < 0 || memErr() != 0)
        }

        /// MemoryDispatch: holding and locking memory succeed with nothing
        /// to do (Prince of Persia 2 locks its buffers); others are refused.
        @Test func memoryDispatchHoldsAndLocks() {
            var c = cpu68k()
            for sel: UInt32 in 0...4 {
                c.d.0 = sel
                raw_MemoryDispatch(&c, 0xA05C)
                #expect(c.d.0 == 0)
            }
            c.d.0 = 9
            raw_MemoryDispatch(&c, 0xA05C)
            #expect(Int32(bitPattern: c.d.0) == -50)
        }

        /// A program of the 64K ROM's days grows its zone by hand, with
        /// 24-bit block headers (Sword of Ages): the zone takes it in.
        @Test func zoneGrownByHandWith24BitHeaders() {
            let area = tb_NewPtr(64 * 1024, false, true)
            let z = (area + 15) & ~15
            mm_init_zone(z, z + 16 * 1024, 0)
            let old = rd32(z) // bkLim: the trailer
            let lim = (z + 60 * 1024) & ~3
            wr32(old, lim - old) // a free block reaching the new trailer, tag clear
            wr32(z, lim)
            wr32(lim, 12) // the new trailer, 24-bit style
            #expect(mm_check_zone(z))
            #expect(rd32(old + 4) == lim - old)
        }

        /// DrvrInstall leaves the new entry's handle in A0, where programs
        /// installing drivers of their own take it to fill the entry in.
        @Test func drvrInstallReturnsTheEntryInA0() {
            var c = cpu68k()
            let refnum: Int16 = -40
            c.a.0 = 0
            c.d.0 = UInt32(bitPattern: Int32(refnum))
            raw_DrvrInstall(&c, 0xA13D)
            #expect(c.d.0 == 0)
            let entry = rd32(rd32(UInt32(LM_UTableBase)) + 4 * 39)
            #expect(entry != 0 && c.a.0 == entry)
            #expect(Int16(bitPattern: rd16(rd32(entry) + 24)) == refnum) // dCtlRefNum
            #expect(mm_check_zone(sysZone))
        }
    }
}
