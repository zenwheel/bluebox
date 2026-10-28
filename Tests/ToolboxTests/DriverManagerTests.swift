import CCore
import CPU68K
import ForkStore
import Foundation
import Testing
import Toolbox

/// A 68k driver of the kind a program supplies: a header and routines that
/// count their calls in a storage block at offset 0x100 (position
/// independent, so it can sit in a DRVR resource).  Storage:
///  0 opens, 2 closes, 4 last Control csCode, 6 Control calls, 8 Prime calls,
///  10 accRun calls, 12 last accEvent's event.what, 14 accMenu item,
///  16 last Prime ioTrap, 20 pending Prime pb, 22 "leave Prime pending",
///  24 Status calls, 26 completions, 30 accMenu menu ID.
private enum Drv {
    static let storage = 0x100
    static func blob(flags: Int = 0x3F00, delay: Int = 0, emask: Int = 0x0148, menu: Int = 0, name: String = ".Test") -> [UInt8] {
        var b = [UInt8](repeating: 0, count: 0x180)
        func put(_ at: Int, _ words: [Int]) { for (i, w) in words.enumerated() { b[at + 2 * i] = UInt8((w >> 8) & 0xFF); b[at + 2 * i + 1] = UInt8(w & 0xFF) } }
        put(0, [flags, delay, emask, menu, 0x20, 0x60, 0xA0, 0x40, 0x30])
        b[18] = UInt8(name.utf8.count)
        for (i, c) in name.utf8.enumerated() { b[19 + i] = c }
        func lea(_ at: Int) -> Int { storage - at } // LEA S(PC),A2's displacement
        let jio = [0x7000, 0x2678, 0x08FC, 0x4ED3]       // MOVEQ #0,D0; MOVEA.L $08FC,A3; JMP (A3)
        // Open (0x20), Close (0x30), Status (0x40)
        put(0x20, [0x45FA, lea(0x22), 0x526A, 0, 0x7000, 0x4E75])
        put(0x30, [0x45FA, lea(0x32), 0x526A, 2, 0x7000, 0x4E75])
        put(0x40, [0x45FA, lea(0x42), 0x526A, 24, 0x317C, 0x1234, 0x001C] + jio)
        // Prime (0x60)
        put(0x60, [0x45FA, lea(0x62), 0x526A, 8, 0x3028, 6, 0x3540, 16, 0x4A6A, 22, 0x6708,
                   0x2548, 20, 0x7001, 0x4E75, 0x2168, 36, 40] + jio)
        // Control (0xA0)
        put(0xA0, [0x45FA, lea(0xA2), 0x3028, 26, 0x3540, 4, 0x526A, 6, 0x0C40, 65, 0x660E,
                   0x526A, 10, 0x4AAA, 20, 0x6704, 0x42AA, 20, 0x0C40, 64, 0x6608, 0x2668, 28, 0x3553, 12,
                   0x0C40, 67, 0x660C, 0x3568, 30, 14, 0x3568, 28, 30] + jio)
        // The completion routine (0xF0)
        put(0xF0, [0x45FA, lea(0xF2), 0x526A, 26, 0x4E75])
        return b
    }
}

private func pstr(_ s: [UInt8]) -> UInt32 {
    let p = tb_NewPtr(Int32(s.count + 1), false, true)
    wr8(p, UInt8(s.count))
    for (i, c) in s.enumerated() { wr8(p + 1 + UInt32(i), c) }
    return p
}
private func pstr(_ s: String) -> UInt32 { pstr(Array(s.utf8)) }

private func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
    let p = tb_NewPtr(8, false, true)
    for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(2 * i), UInt16(truncatingIfNeeded: v)) }
    return p
}

private func installTestDriver(unit: Int = 60, name: String = ".Test", flags: Int = 0x3F00, delay: Int = 0, menu: Int = 0) -> (hdr: UInt32, refnum: Int16) {
    let hdr = tb_NewPtr(0x180, false, true)
    for (i, b) in Drv.blob(flags: flags, delay: delay, menu: menu, name: name).enumerated() { wr8(hdr + UInt32(i), b) }
    let refnum = Int16(-1 - unit)
    #expect(tb_DriverInstall(hdr, refnum) == 0)
    return (hdr, refnum)
}

private func iopb(_ name: UInt32 = 0, refnum: Int16 = 0) -> UInt32 {
    let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
    wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
    wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: refnum))
    return pb
}

private func ioResult(_ pb: UInt32) -> Int16 { Int16(bitPattern: rd16(pb + UInt32(IOParam_ioResult_off))) }
private func field(_ hdr: UInt32, _ off: Int) -> UInt16 { rd16(hdr + UInt32(Drv.storage + off)) }
private func dce(_ refnum: Int16) -> UInt32 { rd32(rd32(rd32(UInt32(LM_UTableBase)) + 4 * UInt32(-1 - Int(refnum)))) }

extension BlueBox {
    /// Drivers a program installs itself: the Device Manager calls their routines.
    @Suite struct ProgramDrivers {
        init() { bootMachine() }

        @Test func openedByNameRunsOpenAndClose() {
            let (hdr, rn) = installTestDriver()
            let pb = iopb(pstr(".Test"))
            #expect(tb_PBOpen(pb, false) == 0)
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off))) == rn)
            #expect(field(hdr, 0) == 1)
            #expect(rd16(dce(rn) + UInt32(DCtlEntry_dCtlFlags_off)) & 0x20 != 0)
            // Already open: the Open routine is not called again.
            #expect(tb_PBOpen(iopb(pstr(".test")), false) == 0)
            #expect(field(hdr, 0) == 1)
            // DrvrRemove refuses an open driver; Close runs goodbye and Close.
            #expect(tb_DrvrRemove(rn) == -25)
            #expect(tb_PBClose(iopb(refnum: rn), false) == 0)
            #expect(field(hdr, 2) == 1)
            #expect(field(hdr, 4) == 0xFFFF) // goodbye
            #expect(rd16(dce(rn) + UInt32(DCtlEntry_dCtlFlags_off)) & 0x20 == 0)
            #expect(tb_DrvrRemove(rn) == 0)
        }

        @Test func aDceWrittenIntoTheUnitTableIsFoundByName() {
            let hdr = tb_NewPtr(0x180, false, true)
            for (i, b) in Drv.blob(name: ".Mine").enumerated() { wr8(hdr + UInt32(i), b) }
            let h = tb_NewHandle(Int32(DCtlEntry_SIZEOF), true, true)
            wr32(rd32(h) + UInt32(DCtlEntry_dCtlDriver_off), hdr)
            wr16(rd32(h) + UInt32(DCtlEntry_dCtlFlags_off), 0x0F00)
            wr32(rd32(UInt32(LM_UTableBase)) + 4 * 58, h)
            let pb = iopb(pstr(".Mine"))
            #expect(tb_PBOpen(pb, false) == 0)
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off))) == -59)
            #expect(field(hdr, 0) == 1)
            #expect(tb_PBOpen(iopb(pstr(".Nobody")), false) == -43)
        }

        @Test func readWriteControlAndStatusReachTheRoutines() {
            let (hdr, rn) = installTestDriver()
            #expect(tb_PBOpen(iopb(pstr(".Test")), false) == 0)
            let pb = iopb(refnum: rn)
            wr32(pb + UInt32(IOParam_ioReqCount_off), 77)
            #expect(tb_PBWrite(pb, false) == 0)
            #expect(rd32(pb + UInt32(IOParam_ioActCount_off)) == 77)
            #expect(field(hdr, 16) & 0xFF == 3) // _Write's trap in ioTrap
            #expect(tb_PBRead(pb, false) == 0)
            #expect(field(hdr, 16) & 0xFF == 2)
            #expect(field(hdr, 8) == 2)
            wr16(pb + UInt32(CntrlParam_csCode_off), 9)
            #expect(tb_PBControl(pb, false) == 0)
            #expect(field(hdr, 4) == 9)
            #expect(tb_PBStatus(pb, false) == 0)
            #expect(rd16(pb + UInt32(CntrlParam_csParam_off)) == 0x1234)
            #expect(field(hdr, 24) == 1)
        }

        @Test func aDriverWithoutAnEnabledCallAnswersTheErrorForIt() {
            let (_, rn) = installTestDriver(unit: 61, flags: 0x0100) // read only
            #expect(tb_PBOpen(iopb(pstr(".Test")), false) == 0)
            let pb = iopb(refnum: rn)
            #expect(tb_PBWrite(pb, false) == -20)
            #expect(tb_PBControl(pb, false) == -17)
            #expect(tb_PBStatus(pb, false) == -18)
            #expect(tb_PBRead(pb, false) == 0)
            // Calls to a driver that is not open.
            let (_, rn2) = installTestDriver(unit: 62, name: ".Other")
            #expect(tb_PBRead(iopb(refnum: rn2), false) == -28)
        }

        @Test func anAsynchronousCallStaysInProgressUntilTheDriverFinishesIt() {
            let (hdr, rn) = installTestDriver(delay: 0)
            #expect(tb_PBOpen(iopb(pstr(".Test")), false) == 0)
            wr16(hdr + UInt32(Drv.storage + 22), 1) // leave Prime pending
            let pb = iopb(refnum: rn)
            wr32(pb + UInt32(IOParam_ioCompletion_off), hdr + 0xF0)
            wr32(pb + UInt32(IOParam_ioReqCount_off), 5)
            #expect(tb_PBWrite(pb, true) == 1) // ioInProgress
            #expect(ioResult(pb) == 1)
            #expect(rd32(hdr + UInt32(Drv.storage + 20)) == pb)
            #expect(field(hdr, 26) == 0)
            // dNeedTime: SystemTask runs the driver's accRun, which ends the request.
            wr16(hdr + UInt32(Drv.storage + 22), 0)
            tb_SystemTask()
            #expect(field(hdr, 10) >= 1)
            #expect(ioResult(pb) == 0)
            #expect(field(hdr, 26) == 1) // the completion ran
        }

        @Test func killIOAbortsWhatIsQueued() {
            let (hdr, rn) = installTestDriver(delay: 0x7FFF) // accRun far off
            #expect(tb_PBOpen(iopb(pstr(".Test")), false) == 0)
            wr16(hdr + UInt32(Drv.storage + 22), 1)
            let a = iopb(refnum: rn), b = iopb(refnum: rn)
            wr32(a + UInt32(IOParam_ioCompletion_off), hdr + 0xF0)
            #expect(tb_PBRead(a, true) == 1)
            #expect(tb_PBRead(b, true) == 1) // queued behind it
            #expect(field(hdr, 8) == 1)        // the driver has seen one
            #expect(tb_PBKillIO(iopb(refnum: rn), false) == 0)
            #expect(field(hdr, 4) == 1)        // Control killIO
            #expect(ioResult(a) == -27 && ioResult(b) == -27)
            #expect(field(hdr, 26) == 1)
        }

        @Test func aQueuedCallStartsWhenTheOneBeforeItEnds() {
            let (hdr, rn) = installTestDriver()
            #expect(tb_PBOpen(iopb(pstr(".Test")), false) == 0)
            wr16(hdr + UInt32(Drv.storage + 22), 1)
            let a = iopb(refnum: rn), b = iopb(refnum: rn)
            #expect(tb_PBRead(a, true) == 1)
            #expect(tb_PBWrite(b, true) == 1)
            wr16(hdr + UInt32(Drv.storage + 22), 0)
            tb_SystemTask() // ends a; b starts and ends at once
            #expect(ioResult(a) == 0 && ioResult(b) == 0)
            #expect(field(hdr, 8) == 2)
        }
    }

    /// Desk accessories: DRVR resources opened by name, and the system events
    /// that reach their windows.
    @Suite struct DeskAccessories {
        func launch(menu: Int = 0) -> (UInt32, Int16) {
            var blob = Drv.blob(flags: 0x3F00, delay: 0, emask: 0x0148, menu: menu, name: "\u{0}Calc")
            blob[19] = 0
            launchTestApp([TestResource(type: "DRVR", id: 20, name: "\u{0}Calc", data: blob)])
            _ = initToolbox()
            let rn = tb_OpenDeskAcc(pstr([0, 0x43, 0x61, 0x6C, 0x63]))
            return (rd32(rd32(dce(rn) + UInt32(DCtlEntry_dCtlDriver_off))), rn)
        }

        @Test func openDeskAccLoadsTheDrvrResource() {
            let (hdr, rn) = launch()
            #expect(rn == -21) // unit 20, as the resource's ID says
            #expect(field(hdr, 0) == 1)
            #expect(rd16(dce(rn) + UInt32(DCtlEntry_dCtlFlags_off)) & 0x60 == 0x60) // dOpened, dRAMBased
            // A second OpenDeskAcc of it opens nothing new.
            #expect(tb_OpenDeskAcc(pstr([0, 0x43, 0x61, 0x6C, 0x63])) == rn)
            #expect(field(hdr, 0) == 1)
            tb_CloseDeskAcc(rn)
            #expect(field(hdr, 2) == 1)
        }

        @Test func systemEventGivesTheFrontDeskAccessoryItsEvents() {
            let (hdr, rn) = launch(menu: 20)
            let w = tb_NewWindow(0, rect(60, 40, 260, 340), pstr("Calc"), 1, 0, 0xFFFF_FFFF, 0, 0)
            wr16(w + 108, UInt16(bitPattern: rn)) // windowKind: the driver's reference number
            wr32(dce(rn) + UInt32(DCtlEntry_dCtlWindow_off), w)
            let ev = tb_NewPtr(16, false, true)
            wr16(ev, 3); wr32(ev + 2, 0x41) // keyDown
            #expect(tb_SystemEvent(ev) == 1)
            #expect(field(hdr, 12) == 3)
            wr16(ev + 14, 0x0100) // with the command key: for the program's menus
            #expect(tb_SystemEvent(ev) == 0)
            wr16(ev + 14, 0)
            wr16(ev, 6); wr32(ev + 2, w) // an update for its window
            #expect(tb_SystemEvent(ev) == 1)
            #expect(field(hdr, 12) == 6)
            wr16(ev, 15) // not one it is told of
            #expect(tb_SystemEvent(ev) == 0)
            // SEvtEnb off stops GetNextEvent's own call, not a program's.
            wr8(UInt32(LM_SEvtEnb), 0)
            wr16(ev, 3)
            #expect(tb_SystemEvent(ev) == 1)
            wr8(UInt32(LM_SEvtEnb), 0xFF)
            // Edit commands and the menu.
            #expect(tb_SystemEdit(3) == 1 && field(hdr, 4) == 71)
            #expect(tb_SystemEdit(0) == 1 && field(hdr, 4) == 68)
            #expect(tb_SystemEdit(1) == 0)
            tb_SystemMenu(Int32(20 << 16 | 2))
            #expect(field(hdr, 14) == 2 && field(hdr, 30) == 20 && field(hdr, 4) == 67)
            // A click in its content: SystemClick hands it the mouseDown.
            wr16(ev, 1); wr32(ev + 10, UInt32(120) << 16 | 100)
            tb_SystemClick(ev, w)
            #expect(field(hdr, 12) == 1)
            // Its window is not the front one: other windows' events stay the program's.
            let w2 = tb_NewWindow(0, rect(80, 60, 200, 300), pstr("Doc"), 1, 0, 0xFFFF_FFFF, 0, 0)
            wr16(ev, 3)
            #expect(tb_SystemEvent(ev) == 0)
            tb_DisposeWindow(w2)
        }

        @Test func systemTaskRunsADriverThatNeedsTime() {
            let (hdr, _) = launch()
            tb_SystemTask()
            tb_SystemTask()
            #expect(field(hdr, 10) == 2)
        }

        @Test func aDeskAccessoryFileInTheAppleMenuItemsFolder() throws {
            bootMachine()
            let dir = URL(fileURLWithPath: testSystemFolder).appendingPathComponent("Apple Menu Items")
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            var blob = Drv.blob(name: "\u{0}Clock")
            blob[19] = 0
            var fork = buildFork([TestResource(type: "DRVR", id: 22, name: "\u{0}Clock", data: blob)])
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            withUnsafeMutableBytes(of: &f.finder_info) { b in for (i, c) in "dfilDMOV".utf8.enumerated() { b[i] = c } }
            f.has_finder_info = true
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(dir.appendingPathComponent("Clock").path, &f, &err, err.count))
            _ = initToolbox()
            let got = tb_OpenDeskAcc(pstr("Clock"))
            #expect(got == -23)
            let e = dce(-23)
            #expect(e != 0 && rd16(e + UInt32(DCtlEntry_dCtlFlags_off)) & 0x20 != 0)
            let hdr = rd32(rd32(e + UInt32(DCtlEntry_dCtlDriver_off)))
            #expect(field(hdr, 0) == 1)
        }
    }
}
