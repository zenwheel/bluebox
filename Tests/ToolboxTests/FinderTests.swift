import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

/// Native routines for the Finder tests' callbacks: each pops its
/// arguments (sizes in bytes) and leaves a long result.
nonisolated(unsafe) var finderLongBodies: [(sizes: [Int], body: ([UInt32]) -> UInt32)] = []
let finderLongTrampoline: native_fn = { c, trapword in
    guard let c else { return }
    let entry = finderLongBodies[Int(trapword)]
    let ret = cpu_pop32(c)
    var args = [UInt32]()
    for size in entry.sizes.reversed() {
        args.insert(size == 4 ? cpu_pop32(c) : UInt32(cpu_pop16(c)), at: 0)
    }
    wr32(c.pointee.a.7, entry.body(args))
    cpu_push32(c, ret)
}

func longFunction(_ sizes: [Int], _ body: @escaping ([UInt32]) -> UInt32) -> UInt32 {
    finderLongBodies.append((sizes, body))
    return native_register(finderLongTrampoline, "FinderTestProc", UInt16(finderLongBodies.count - 1))
}

extension BlueBox {
    /// What System 7.1's Finder needs from the system (M21): the Process
    /// Manager's Finder process and private calls, the desktop window, the
    /// Icon Utilities' caches, methods and labels, the disk driver's icons,
    /// zones' grow-zone functions and the Command-period checks.
    @Suite struct Finder {
        func psn(_ hi: UInt32, _ lo: UInt32) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            wr32(p, hi)
            wr32(p + 4, lo)
            return p
        }

        @Test func theRunIsTheFinderProcess() {
            launchTestApp()
            g_is_finder = true
            defer { g_is_finder = false }
            let p = psn(0, 0)
            #expect(tb_GetCurrentProcess(p) == 0 && rd32(p) == 0 && rd32(p + 4) == 0x1001)
            let info = tb_NewPtr(Int32(ProcessInfoRec_SIZEOF), false, true)
            wr32(info + UInt32(ProcessInfoRec_processInfoLength_off), UInt32(ProcessInfoRec_SIZEOF))
            #expect(tb_GetProcessInformation(p, info) == 0)
            #expect(rd32(info + UInt32(ProcessInfoRec_processType_off)) == fourCC("FNDR"))
            // First in the list, and no second Finder after it.
            let walk = psn(0, 0)
            #expect(tb_GetNextProcess(walk) == 0 && rd32(walk + 4) == 0x1001)
            let next = tb_GetNextProcess(walk)
            #expect(next != 0 || rd32(walk) == 1) // only other runs follow
            #expect(process_is_us(0, 0x1001) && process_is_us(0, 2) && !process_is_us(0, 0x1000))
        }

        @Test func volumeNotificationAndTheSystemHeap() {
            launchTestApp()
            #expect(tb_RequestVolumeNotification(0x1234, 7) == 0)
            #expect(tb_DeclineVolumeNotification(0x1234, 8) == -50) // not that refCon
            #expect(tb_DeclineVolumeNotification(0x1234, 7) == 0)
            let grow = tb_NewPtr(4, false, true), free = tb_NewPtr(4, false, true)
            #expect(tb_SystemHeapRoom(grow, free) == 0)
            #expect(rd32(free) == UInt32(tb_FreeMem(true)) && rd32(grow) == 0)
            #expect(tb_SystemHeapRoom(0, free) == 0)
            // ExpandMem, with the system heap's reserve the Finder reads.
            let em = rd32(0x2B6)
            #expect(em != 0 && rd32(em + 0x1AE) != 0)
            #expect(tb_ClearAppleMenuItems(0) == 0)
            #expect(tb_AddAppleMenuItem(0, 0, 0, 0, 0, 0, 0) == 0)
            #expect(finder_apple_menu_items() == 1)
        }

        /// The desktop window: not a window FindWindow finds, but the
        /// uncovered desktop is its visible region and its update events
        /// come behind the windows'.
        @Test func theDesktopWindow() {
            launchTestApp()
            _ = initToolbox()
            let out = tb_NewPtr(4, false, true)
            #expect(tb_NewDesktopWindow(out) == 0)
            let desk = rd32(out)
            #expect(desk != 0 && wm_desk_window() == desk)
            #expect(rd32(UInt32(LM_WindowList)) != desk)
            let wp = tb_NewPtr(4, false, true)
            #expect(tb_FindWindow(UInt32(200 << 16 | 300), wp) == 0 && rd32(wp) == 0) // inDesk
            #expect(tb_EmptyRgn(rd32(desk + UInt32(WindowRecord_updateRgn_off))) == 0)
            let ev = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            #expect(tb_CheckUpdate(ev) != 0 && rd16(ev) == 6 && rd32(ev + 2) == desk)
            tb_SetPort(desk)
            tb_BeginUpdate(desk)
            tb_EndUpdate(desk)
            #expect(tb_CheckUpdate(ev) == 0)
            // A window on it takes its place out of the visible region.
            let r = tb_NewPtr(8, false, true)
            wr16(r, 100); wr16(r + 2, 100); wr16(r + 4, 200); wr16(r + 6, 300)
            let w = tb_NewWindow(0, r, 0, 1, 0, 0xFFFF_FFFF, 0, 0)
            let vis = rd32(desk + UInt32(GrafPort_visRgn_off))
            #expect(tb_PtInRgn(UInt32(150 << 16 | 150), vis) == 0 && tb_PtInRgn(UInt32(400 << 16 | 500), vis) != 0)
            // and what it uncovers on closing is the desktop's to draw.
            tb_DisposeWindow(w)
            #expect(tb_PtInRgn(UInt32(150 << 16 | 150), rd32(desk + UInt32(WindowRecord_updateRgn_off))) != 0)
        }

        /// Labels: System 7's, changed by SetLabel.
        @Test func labels() {
            launchTestApp()
            let rgb = tb_NewPtr(6, false, true), name = tb_NewPtr(256, false, true)
            #expect(tb_GetLabel(2, rgb, name) == 0 && pstring(at: name) == "Hot" && rd16(rgb) > 0xC000)
            guestPString("Urgent", at: name)
            wr16(rgb, 0x1111); wr16(rgb + 2, 0x2222); wr16(rgb + 4, 0x3333)
            #expect(tb_SetLabel(2, rgb, name) == 0)
            gmemset(rgb, 0, 6)
            #expect(tb_GetLabel(2, rgb, name) == 0 && pstring(at: name) == "Urgent" && rd16(rgb + 2) == 0x2222)
            #expect(tb_GetLabel(8, rgb, name) == -50 && tb_SetLabel(0, rgb, name) == -50)
        }

        /// An ICN# whose mask is the top half.
        func topHalfIcon() -> UInt32 {
            let h = tb_NewHandle(256, false, true)
            for i in 0..<64 { wr8(rd32(h) + 128 + UInt32(i), 0xFF) }
            return h
        }

        /// An icon cache asks its getter for what it lacks, and keeps it;
        /// regions and hit tests follow the mask.
        @Test func iconCachesAndMasks() {
            launchTestApp()
            guestStack()
            let icon = topHalfIcon()
            var asked: [UInt32] = []
            let getter = longFunction([4, 4]) { args in
                asked.append(args[0])
                return args[0] == fourCC("ICN#") && args[1] == 0x55 ? icon : 0
            }
            let cp = tb_NewPtr(4, false, true)
            #expect(tb_MakeIconCache(cp, getter, 0x55) == 0)
            let cache = rd32(cp)
            let r = tb_NewPtr(8, false, true)
            wr16(r, 0); wr16(r + 2, 0); wr16(r + 4, 32); wr16(r + 6, 32)
            #expect(tb_LoadIconCache(r, 0, 0, cache) == 0)
            #expect(asked.contains(fourCC("ICN#")))
            let got = tb_NewPtr(4, false, true)
            #expect(tb_GetIconFromSuite(got, cache, Int32(bitPattern: fourCC("ICN#"))) == 0 && rd32(got) == icon)
            let data = tb_NewPtr(4, false, true)
            #expect(tb_GetIconCacheData(cache, data) == 0 && rd32(data) == 0x55)
            #expect(tb_SetIconCacheData(cache, 0x66) == 0 && tb_GetIconCacheData(cache, data) == 0 && rd32(data) == 0x66)
            #expect(tb_GetIconCacheProc(cache, data) == 0 && rd32(data) == getter)
            // The mask, not the square: the top half is in, the bottom out.
            #expect(tb_PtInIconSuite(UInt32(5 << 16 | 10), r, 0, cache) != 0)
            #expect(tb_PtInIconSuite(UInt32(25 << 16 | 10), r, 0, cache) == 0)
            let rgn = tb_NewRgn()
            #expect(tb_IconSuiteToRgn(rgn, r, 0, cache) == 0)
            let box = rd32(rgn) + 2
            #expect(rd16(box) == 0 && rd16(box + 4) == 16 && rd16(box + 6) == 32)
            let test = tb_NewPtr(8, false, true)
            wr16(test, 20); wr16(test + 2, 0); wr16(test + 4, 30); wr16(test + 6, 32)
            #expect(tb_RectInIconSuite(test, r, 0, cache) == 0)
            // The same through a method (a cache for the call).
            #expect(tb_PtInIconMethod(UInt32(5 << 16 | 10), r, 0, getter, 0x55) != 0)
            #expect(tb_RectInIconMethod(test, r, 0, getter, 0x55) == 0)
            #expect(tb_IconMethodToRgn(rgn, r, 0, getter, 0x55) == 0 && rd16(rd32(rgn) + 6) == 16)
            #expect(tb_DisposeIconSuite(cache, 0) == 0)
        }

        @Test func forEachIconDoVisitsTheMembers() {
            launchTestApp()
            guestStack()
            let sp = tb_NewPtr(4, false, true)
            #expect(tb_NewIconSuite(sp) == 0)
            let suite = rd32(sp)
            let icon = topHalfIcon()
            #expect(tb_AddIconToSuite(icon, suite, Int32(bitPattern: fourCC("ICN#"))) == 0)
            var seen: [UInt32: UInt32] = [:]
            let action = pascalProc([4, 4, 4]) { args in
                seen[args[0]] = rd32(args[1])
                if args[0] == fourCC("ics#") { wr32(args[1], 0x4242) } // fill a member in
                return 0
            }
            #expect(tb_ForEachIconDo(suite, 0x101, action, 0) == 0) // ICN# and ics#
            #expect(seen[fourCC("ICN#")] == icon && seen[fourCC("ics#")] == 0 && seen.count == 2)
            let got = tb_NewPtr(4, false, true)
            #expect(tb_GetIconFromSuite(got, suite, Int32(bitPattern: fourCC("ics#"))) == 0 && rd32(got) == 0x4242)
        }

        /// The main volume's driver gives the Finder its drive icon and the
        /// drive's kind.
        @Test func diskDriverIcons() {
            launchTestApp()
            let pb = tb_NewPtr(Int32(CntrlParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -5))
            wr16(pb + UInt32(IOParam_ioVRefNum_off), 1)
            wr16(pb + UInt32(CntrlParam_csCode_off), 21)
            #expect(tb_PBControl(pb, false) == 0)
            let icon = rd32(pb + UInt32(CntrlParam_csParam_off))
            #expect(icon != 0 && (0..<256).contains { rd8(icon + UInt32($0)) != 0 })
            wr16(pb + UInt32(CntrlParam_csCode_off), 23)
            #expect(tb_PBControl(pb, false) == 0 && rd32(pb + UInt32(CntrlParam_csParam_off)) & 0xFF == 1)
            wr16(pb + UInt32(CntrlParam_csCode_off), 99)
            #expect(tb_PBControl(pb, false) == -17)
            wr16(pb + UInt32(CntrlParam_csCode_off), 8)
            #expect(tb_PBStatus(pb, false) == 0)
        }

        /// A zone's grow-zone function is called when it is full, and the
        /// allocation tried again with what it freed (the Finder's code zone).
        @Test func growZoneFunctionsAreCalled() {
            launchTestApp()
            guestStack()
            let size: UInt32 = 4096
            let sub = tb_NewPtr(Int32(size), false, true)
            let args = tb_NewPtr(14, false, true)
            wr32(args, sub)
            wr32(args + 4, sub + size)
            wr16(args + 8, 4)
            var c = cpu68k()
            c.a.0 = args
            raw_InitZone(&c, 0xA019)
            let filler = tb_NewHandle(2048, false, false)
            #expect(filler != 0)
            var calls = 0
            let gz = longFunction([4]) { _ in
                calls += 1
                if calls > 1 { return 0 }
                tb_DisposeHandle(filler)
                return 2048
            }
            tb_SetGrowZone(gz)
            let h = tb_NewHandle(2600, false, false)
            #expect(h != 0 && calls >= 1)
            #expect(mm_check_zone(sub))
            tb_SetZone(rd32(UInt32(LM_ApplZone)))
        }

        @Test func commandPeriodChecks() {
            launchTestApp()
            let ev = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            wr16(ev, 3) // keyDown
            wr32(ev + 2, 0x2F2E) // '.'
            wr16(ev + 14, 0x0100) // cmdKey
            #expect(tb_IsUserCancelEvent(ev) != 0)
            wr16(ev + 14, 0)
            #expect(tb_IsUserCancelEvent(ev) == 0)
            wr32(ev + 2, 0x351B) // Escape
            #expect(tb_IsUserCancelEvent(ev) != 0)
            wr16(ev, 1)
            #expect(tb_IsUserCancelEvent(ev) == 0)
            #expect(tb_CheckEventQueueForUserCancel(0) == 0)
            machine_post_key(0x1B, 0x35, 0, 1, 0)
            #expect(tb_CheckEventQueueForUserCancel(0) != 0)
            #expect(tb_CheckEventQueueForUserCancel(0) == 0)
        }
    }
}
