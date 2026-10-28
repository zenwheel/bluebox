import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

/// Native routines for the Finder tests' callbacks: each pops its
/// arguments (sizes in bytes) and leaves a long result.
nonisolated(unsafe) var finderLaunched: [String] = []
nonisolated(unsafe) var finderStages: [UInt32] = []
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

        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// A file in the test application's folder, with some data.
        func makeFile(_ folder: URL, _ name: String, _ text: String) -> UInt32 {
            try? text.write(to: folder.appendingPathComponent(name), atomically: true, encoding: .utf8)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname(name), spec) == 0)
            return spec
        }

        /// System 7's volume folders are at the volume's root, and volumes
        /// say they have a desktop database, file IDs, CatSearch, deny modes,
        /// CopyFile and MoveRename.
        @Test func volumeFoldersAndParameters() {
            launchTestApp()
            let vref = tb_NewPtr(2, false, true), dir = tb_NewPtr(4, false, true)
            #expect(tb_FindFolder(-32768, Int32(bitPattern: fourCC("trsh")), 1, vref, dir) == 0)
            let pb = tb_NewPtr(Int32(CInfoPBRec_SIZEOF), false, true)
            let name = tb_NewPtr(256, false, true)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            wr16(pb + UInt32(IOParam_ioVRefNum_off), rd16(vref))
            wr16(pb + UInt32(FileParam_ioFDirIndex_off), UInt16(bitPattern: -1))
            wr32(pb + UInt32(HFileInfo_ioDirID_off), rd32(dir))
            #expect(tb_PBGetCatInfo(pb, false) == 0 && pstring(at: name) == "Trash")
            #expect(rd32(pb + UInt32(DirInfo_ioDrParID_off)) == 2) // at the root
            #expect(tb_FindFolder(-32768, Int32(bitPattern: fourCC("desk")), 1, vref, dir) == 0)
            wr32(pb + UInt32(HFileInfo_ioDirID_off), rd32(dir))
            #expect(tb_PBGetCatInfo(pb, false) == 0 && pstring(at: name) == "Desktop Folder")
            #expect(rd32(pb + UInt32(DirInfo_ioDrParID_off)) == 2)

            let vp = tb_NewPtr(Int32(HParamBlockRec_SIZEOF), false, true)
            let buf = tb_NewPtr(20, false, true)
            wr16(vp + UInt32(IOParam_ioVRefNum_off), rd16(vref))
            wr32(vp + UInt32(HIoParam_ioBuffer_off), buf)
            wr32(vp + UInt32(HIoParam_ioReqCount_off), 20)
            #expect(tb_PBHGetVolParms(vp, false) == 0)
            let attrib = rd32(buf + 2)
            for bit in [6, 7, 12, 13, 14, 15] { #expect(attrib & (1 << UInt32(bit)) != 0) }
        }

        /// PBHCopyFile, PBHMoveRename and PBExchangeFiles.
        @Test func copyMoveAndExchange() {
            let folder = launchTestApp()
            let a = makeFile(folder, "Alpha", "first")
            _ = makeFile(folder, "Beta", "second, longer")
            let sub = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Sub"), sub)
            let subID = tb_NewPtr(4, false, true)
            #expect(tb_FSpDirCreate(sub, 0, subID) == 0)
            let vref = Int16(bitPattern: rd16(a + UInt32(FSSpec_vRefNum_off)))
            let parent = rd32(a + UInt32(FSSpec_parID_off))

            let pb = tb_NewPtr(Int32(HParamBlockRec_SIZEOF), false, true)
            func setup(_ src: String, into dirID: UInt32, as copyName: String?) {
                gmemset(pb, 0, UInt32(HParamBlockRec_SIZEOF))
                wr32(pb + UInt32(IOParam_ioNamePtr_off), pname(src))
                wr16(pb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: vref))
                wr32(pb + 48, parent)
                wr16(pb + 24, UInt16(bitPattern: vref))
                wr32(pb + 36, dirID)
                wr32(pb + 32, copyName.map { pname($0) } ?? 0)
            }
            setup("Alpha", into: rd32(subID), as: nil)
            #expect(tb_PBHCopyFile(pb, false) == 0)
            let copied = folder.appendingPathComponent("Sub/Alpha").path
            #expect((try? String(contentsOfFile: copied, encoding: .utf8)) == "first")
            #expect(FileManager.default.fileExists(atPath: folder.appendingPathComponent("Alpha").path))
            #expect(tb_PBHCopyFile(pb, false) == -48) // dupFNErr
            setup("Alpha", into: rd32(subID), as: "Gamma")
            #expect(tb_PBHMoveRename(pb, false) == 0)
            #expect(!FileManager.default.fileExists(atPath: folder.appendingPathComponent("Alpha").path))
            #expect((try? String(contentsOfFile: folder.appendingPathComponent("Sub/Gamma").path, encoding: .utf8)) == "first")

            gmemset(pb, 0, UInt32(HParamBlockRec_SIZEOF))
            wr32(pb + UInt32(IOParam_ioNamePtr_off), pname("Beta"))
            wr16(pb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: vref))
            wr32(pb + 48, parent)
            wr32(pb + 28, pname("Gamma"))
            wr32(pb + 36, rd32(subID))
            #expect(tb_PBExchangeFiles(pb, false) == 0)
            #expect((try? String(contentsOfFile: folder.appendingPathComponent("Beta").path, encoding: .utf8)) == "first")
            #expect((try? String(contentsOfFile: folder.appendingPathComponent("Sub/Gamma").path, encoding: .utf8)) == "second, longer")
        }

        /// The test application started again, in a fresh machine.
        func relaunch(_ folder: URL) {
            catalog_close()
            bootMachine()
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(folder.appendingPathComponent("Test App").path, &err, err.count))
        }

        func catInfo(_ vref: Int16, _ dir: UInt32, _ name: String?) -> UInt32 {
            let pb = tb_NewPtr(Int32(CInfoPBRec_SIZEOF), false, true)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name.map { pname($0) } ?? tb_NewPtr(256, false, true))
            wr16(pb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: vref))
            wr16(pb + UInt32(FileParam_ioFDirIndex_off), name == nil ? UInt16(bitPattern: -1) : 0)
            wr32(pb + UInt32(HFileInfo_ioDirID_off), dir)
            #expect(tb_PBGetCatInfo(pb, false) == 0)
            return pb
        }

        /// A folder's Finder information and comment are kept in the
        /// catalog: from one run to the next, and with the folder when it
        /// is renamed.
        @Test func folderInformationIsKept() {
            let folder = launchTestApp()
            try? FileManager.default.createDirectory(at: folder.appendingPathComponent("Kept"), withIntermediateDirectories: true)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname("Kept"), spec) == 0)
            let vref = Int16(bitPattern: rd16(spec))
            let parent = rd32(spec + 2)
            var pb = catInfo(vref, parent, "Kept")
            // The window's rectangle, the icon's place, a label and the view.
            let di = (0..<16).map { UInt8($0 + 1) }, dx = (0..<16).map { UInt8(0xA0 + $0) }
            for (i, b) in di.enumerated() { wr8(pb + UInt32(DirInfo_ioDrUsrWds_off) + UInt32(i), b) }
            for (i, b) in dx.enumerated() { wr8(pb + UInt32(DirInfo_ioDrFndrInfo_off) + UInt32(i), b) }
            wr32(pb + UInt32(HFileInfo_ioDirID_off), parent)
            #expect(tb_PBSetCatInfo(pb, false) == 0)
            pb = catInfo(vref, parent, "Kept")
            #expect((0..<16).map { rd8(pb + UInt32(DirInfo_ioDrUsrWds_off) + UInt32($0)) } == di)
            #expect((0..<16).map { rd8(pb + UInt32(DirInfo_ioDrFndrInfo_off) + UInt32($0)) } == dx)

            // A comment, through the desktop database.
            let dtp = tb_NewPtr(Int32(DTPBRec_SIZEOF), false, true)
            wr16(dtp + UInt32(DTPBRec_ioVRefNum_off), UInt16(bitPattern: vref))
            #expect(tb_PBDTGetPath(dtp) == 0)
            wr32(dtp + UInt32(DTPBRec_ioNamePtr_off), pname("Kept"))
            wr32(dtp + UInt32(DTPBRec_ioDirID_off), parent)
            let text = Array("A folder to keep".utf8)
            let buf = tb_NewPtr(256, false, true)
            for (i, b) in text.enumerated() { wr8(buf + UInt32(i), b) }
            wr32(dtp + UInt32(DTPBRec_ioDTBuffer_off), buf)
            wr32(dtp + UInt32(DTPBRec_ioDTReqCount_off), UInt32(text.count))
            #expect(tb_PBDTSetComment(dtp, false) == 0)

            // Renamed, then the machine started again: all still there.
            #expect(tb_FSpRename(spec, pname("Moved")) == 0)
            relaunch(folder)
            let spec2 = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname("Moved"), spec2) == 0)
            pb = catInfo(Int16(bitPattern: rd16(spec2)), rd32(spec2 + 2), "Moved")
            #expect((0..<16).map { rd8(pb + UInt32(DirInfo_ioDrUsrWds_off) + UInt32($0)) } == di)
            let dtp2 = tb_NewPtr(Int32(DTPBRec_SIZEOF), false, true)
            wr16(dtp2 + UInt32(DTPBRec_ioVRefNum_off), rd16(spec2))
            #expect(tb_PBDTGetPath(dtp2) == 0)
            wr32(dtp2 + UInt32(DTPBRec_ioNamePtr_off), pname("Moved"))
            wr32(dtp2 + UInt32(DTPBRec_ioDirID_off), rd32(spec2 + 2))
            let out = tb_NewPtr(256, false, true)
            wr32(dtp2 + UInt32(DTPBRec_ioDTBuffer_off), out)
            wr32(dtp2 + UInt32(DTPBRec_ioDTReqCount_off), 256)
            #expect(tb_PBDTGetComment(dtp2, false) == 0)
            #expect((0..<text.count).map { rd8(out + UInt32($0)) } == text)

            // The Trash at the root is invisible until the Finder says otherwise.
            let vr = tb_NewPtr(2, false, true), dir = tb_NewPtr(4, false, true)
            #expect(tb_FindFolder(-32768, Int32(bitPattern: fourCC("trsh")), 1, vr, dir) == 0)
            pb = catInfo(Int16(bitPattern: rd16(vr)), rd32(dir), nil)
            #expect(rd16(pb + UInt32(DirInfo_ioDrUsrWds_off) + 8) & 0x4000 != 0)
        }

        /// The desktop database in the catalog: new the first time it is
        /// opened, then the applications and icons programs add, kept from
        /// run to run until it is reset.
        @Test func desktopDatabaseIsKept() {
            let folder = launchTestApp()
            let dtp = tb_NewPtr(Int32(DTPBRec_SIZEOF), false, true)
            wr16(dtp + UInt32(DTPBRec_ioVRefNum_off), UInt16(bitPattern: -1))
            #expect(tb_PBDTOpenInform(dtp) == 0)
            let first = rd32(dtp + UInt32(DTPBRec_ioTagInfo_off))
            #expect(tb_PBDTOpenInform(dtp) == 0 && rd32(dtp + UInt32(DTPBRec_ioTagInfo_off)) == 0)
            _ = first // new unless an earlier test opened it

            try? "x".write(to: folder.appendingPathComponent("Tool"), atomically: true, encoding: .utf8)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname("Tool"), spec) == 0)
            wr32(dtp + UInt32(DTPBRec_ioNamePtr_off), pname("Tool"))
            wr32(dtp + UInt32(DTPBRec_ioDirID_off), rd32(spec + 2))
            wr32(dtp + UInt32(DTPBRec_ioFileCreator_off), fourCC("TOOL"))
            wr32(dtp + UInt32(DTPBRec_ioTagInfo_off), 0)
            #expect(tb_PBDTAddAPPL(dtp, false) == 0)
            let icon = (0..<256).map { UInt8(truncatingIfNeeded: $0 * 7) }
            let buf = tb_NewPtr(1024, false, true)
            for (i, b) in icon.enumerated() { wr8(buf + UInt32(i), b) }
            wr32(dtp + UInt32(DTPBRec_ioDTBuffer_off), buf)
            wr32(dtp + UInt32(DTPBRec_ioDTReqCount_off), 256)
            wr32(dtp + UInt32(DTPBRec_ioFileType_off), fourCC("TDOC"))
            wr8(dtp + UInt32(DTPBRec_ioIconType_off), 1)
            #expect(tb_PBDTAddIcon(dtp, false) == 0)
            wr32(dtp + UInt32(DTPBRec_ioDTLgLen_off), 0)
            #expect(tb_PBDTGetInfo(dtp, false) == 0 && rd32(dtp + UInt32(DTPBRec_ioDTLgLen_off)) >= 256)

            relaunch(folder)
            let d2 = tb_NewPtr(Int32(DTPBRec_SIZEOF), false, true)
            wr16(d2 + UInt32(DTPBRec_ioVRefNum_off), UInt16(bitPattern: -1))
            #expect(tb_PBDTGetPath(d2) == 0)
            let name = tb_NewPtr(64, false, true)
            wr32(d2 + UInt32(DTPBRec_ioNamePtr_off), name)
            wr32(d2 + UInt32(DTPBRec_ioFileCreator_off), fourCC("TOOL"))
            #expect(tb_PBDTGetAPPL(d2, false) == 0 && pstring(at: name) == "Tool")
            let out = tb_NewPtr(1024, false, true)
            wr32(d2 + UInt32(DTPBRec_ioDTBuffer_off), out)
            wr32(d2 + UInt32(DTPBRec_ioDTReqCount_off), 1024)
            wr32(d2 + UInt32(DTPBRec_ioFileType_off), fourCC("TDOC"))
            wr8(d2 + UInt32(DTPBRec_ioIconType_off), 1)
            #expect(tb_PBDTGetIcon(d2, false) == 0)
            #expect((0..<256).map { rd8(out + UInt32($0)) } == icon)

            // Reset: the icons and applications go; deleted: new again.
            #expect(tb_PBDTReset(d2, false) == 0)
            #expect(tb_PBDTGetIcon(d2, false) == -5018)
            let spec2 = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname("Tool"), spec2) == 0)
            wr32(d2 + UInt32(DTPBRec_ioNamePtr_off), pname("Tool"))
            wr32(d2 + UInt32(DTPBRec_ioDirID_off), rd32(spec2 + 2))
            #expect(tb_PBDTRemoveAPPL(d2, false) == -5018)
            #expect(tb_PBDTDelete(d2, false) == 0)
            #expect(tb_PBDTOpenInform(d2) == 0 && rd32(d2 + UInt32(DTPBRec_ioTagInfo_off)) == 1)
        }

        /// The Finder goes on after it launches a program, whatever the
        /// flags: the program is its own run.
        @Test func launchingFromTheFinderGoesOn() {
            let folder = launchTestApp()
            g_is_finder = true
            defer { g_is_finder = false }
            finderLaunched = []
            machine_set_launch_handler({ path, _, _, _ in
                finderLaunched.append(String(cString: path!))
                return true
            }, nil)
            defer { machine_set_launch_handler(nil, nil) }
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(folder.appendingPathComponent("Test App").path, spec)
            let pb = tb_NewPtr(Int32(LaunchParamBlockRec_SIZEOF), false, true)
            wr16(pb + UInt32(LaunchParamBlockRec_launchBlockID_off), 0x4C43)
            wr16(pb + UInt32(LaunchParamBlockRec_launchControlFlags_off), 0) // no launchContinue
            wr32(pb + UInt32(LaunchParamBlockRec_launchAppSpec_off), spec)
            #expect(tb_LaunchApplication(pb) == 0)
            #expect(finderLaunched.count == 1 && !g_cpu!.pointee.halted)
            #expect(tb_MenuDispatchFB(pname("Test App"), 0, 1) == 0)
        }

        /// WakeUpProcess ends this process's sleep; asleep, a headless
        /// machine's clock moves on by the ticks asked for and no more.
        @Test func sleepingAndWaking() {
            launchTestApp()
            let ev = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            tb_FlushEvents(-1, 0)
            var before = g_cpu!.pointee.icount
            _ = tb_WaitNextEvent(0, ev, 6, 0)
            let slept = g_cpu!.pointee.icount - before
            #expect(slept >= 6 * 166_000 && slept < 7 * 166_667 + 10_000)
            let me = psn(0, 2)
            #expect(tb_WakeUpProcess(me) == 0)
            before = g_cpu!.pointee.icount
            _ = tb_WaitNextEvent(0, ev, 60, 0)
            #expect(g_cpu!.pointee.icount - before < 200_000) // a tick at most
        }

        /// Quit All Applications to the system process quits this one.
        @Test func quitAllToTheSystemProcess() {
            launchTestApp(sizeFlags: 0x0040) // takes high-level events
            g_is_finder = true
            defer { g_is_finder = false }
            let evt = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            wr16(evt, 23)
            wr32(evt + UInt32(EventRecord_message_off), fourCC("aevt"))
            wr32(evt + UInt32(EventRecord_where_off), fourCC("quia"))
            #expect(tb_PostHighLevelEvent(evt, psn(0, 1), 0, 0, 0, 0x8000) == 0) // receiverIDisPSN
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            #expect(tb_GetNextEvent(0x0400, e) != 0 && rd16(e) == 23)
            #expect(rd32(e + UInt32(EventRecord_message_off)) == fourCC("aevt"))
            #expect(rd32(e + UInt32(EventRecord_where_off)) == fourCC("quit"))
            // An event to the Finder's own serial number is its own.
            wr32(evt + UInt32(EventRecord_where_off), fourCC("abcd"))
            #expect(tb_PostHighLevelEvent(evt, psn(0, 0x1001), 0, 0, 0, 0x8000) == 0)
            #expect(tb_GetNextEvent(0x0400, e) != 0 && rd32(e + UInt32(EventRecord_where_off)) == fourCC("abcd"))
        }

        /// The Shutdown Manager: procedures for each stage, then the run ends.
        @Test func shutdownManager() {
            launchTestApp()
            guestStack()
            finderStages = []
            let proc = native_register({ c, _ in finderStages.append(c!.pointee.d.0 & 0xFFFF) }, "SDProc", 0)
            let other = native_register({ _, _ in finderStages.append(99) }, "SDOther", 0)
            tb_ShutDwnInstall(proc, 3 | 8)  // power off or restart, and drivers
            tb_ShutDwnInstall(other, 3)
            tb_ShutDwnRemove(other)
            tb_ShutDwnStart()
            #expect(finderStages == [8, 2])
            #expect(g_cpu!.pointee.halted)
            #expect(String(cString: machine_halt_reason()).contains("restart"))
        }
    }
}
