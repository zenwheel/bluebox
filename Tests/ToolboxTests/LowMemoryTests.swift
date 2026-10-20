import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Low-memory globals that programs read or call directly.
    @Suite struct LowMemory {
        init() { bootMachine() }

        func setStack() {
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
        }

        /// EventQueue holds the pending events as event records.
        @Test func eventQueueMirrorsTheEvents() {
            tb_FlushEvents(-1, 0)
            #expect(rd32(UInt32(LM_EventQueue) + 2) == 0)
            #expect(tb_PostEvent(3, 0x0061) == 0) // keyDown 'a'
            #expect(tb_PostEvent(5, 0x0062) == 0) // autoKey 'b'
            let head = rd32(UInt32(LM_EventQueue) + 2), tail = rd32(UInt32(LM_EventQueue) + 6)
            #expect(head != 0 && tail != 0 && head != tail)
            #expect(rd16(head + 4) == 4 && rd16(head + 6) == 3 && rd32(head + 8) == 0x61) // evType, keyDown, 'a'
            #expect(rd32(head) == tail && rd16(tail + 6) == 5 && rd32(tail) == 0)
            tb_FlushEvents(-1, 0)
            #expect(rd32(UInt32(LM_EventQueue) + 2) == 0)
        }

        /// StillDown is false while a mouse event waits: the second press of
        /// a double click ends the first one's tracking.
        @Test func stillDownEndsAtAQueuedClick() {
            tb_FlushEvents(-1, 0)
            machine_post_mouse(10, 10, 1)
            tb_FlushEvents(-1, 0) // the program has taken the first press
            #expect(tb_StillDown() != 0)
            machine_post_mouse(10, 10, 0)
            machine_post_mouse(10, 10, 1)
            #expect(tb_StillDown() == 0) // down again, but a release and a press wait
            tb_FlushEvents(-1, 0)
            #expect(tb_StillDown() != 0)
            machine_post_mouse(10, 10, 0)
            tb_FlushEvents(-1, 0)
        }

        /// Button is the button itself: a program polling it never takes the
        /// mouse-down from the queue, and still sees the press (The Colony).
        @Test func buttonIgnoresTheQueue() {
            tb_FlushEvents(-1, 0)
            machine_post_mouse(10, 10, 1)
            #expect(tb_Button() != 0 && tb_StillDown() == 0)
            machine_post_mouse(10, 10, 0)
            #expect(tb_Button() == 0)
            tb_FlushEvents(-1, 0)
        }

        /// PortList lists the open ports.
        @Test func portListFollowsOpenAndClose() {
            let port = initToolbox()
            func listed(_ p: UInt32) -> Bool {
                let h = rd32(UInt32(LM_PortList))
                let n = Int(rd16(rd32(h)))
                return (0..<n).contains { rd32(rd32(h) + 2 + UInt32($0 * 4)) == p }
            }
            #expect(listed(port))
            tb_ClosePort(port)
            #expect(!listed(port))
        }

        /// AuxWinHead ends with the default record; SetWinColor gives a window
        /// its own, which goes with the window.
        @Test func auxiliaryWindowRecords() {
            launchTestApp()
            _ = initToolbox()
            let hp = tb_NewPtr(4, false, true)
            #expect(tb_GetAuxWin(0, hp) == 0)
            let def = rd32(hp)
            #expect(def != 0 && rd32(rd32(def) + 8) != 0) // the default colour table
            let r = tb_NewPtr(8, false, true)
            wr16(r, 50); wr16(r + 2, 50); wr16(r + 4, 150); wr16(r + 6, 200)
            let w = tb_NewCWindow(0, r, 0, 0, 0, 0xFFFF_FFFF, 0, 0)
            #expect(tb_GetAuxWin(w, hp) == 0 && rd32(hp) == def)
            let ct = tb_NewHandle(16, false, true)
            tb_SetWinColor(w, ct)
            #expect(tb_GetAuxWin(w, hp) != 0 && rd32(rd32(rd32(hp)) + 8) == ct)
            tb_DisposeWindow(w)
            #expect(rd32(UInt32(LM_AuxWinHead)) == def)
        }

        /// Measuring text leaves the font's width table in WidthTabHandle.
        @Test func widthTableFollowsTheFont() {
            _ = initToolbox()
            tb_TextFont(4) // Monaco
            tb_TextSize(9)
            let s = tb_NewPtr(2, false, true)
            wr8(s, 1); wr8(s + 1, 0x57) // "W"
            let w = tb_StringWidth(s)
            let t = rd32(UInt32(LM_WidthPtr))
            #expect(t != 0 && t == rd32(rd32(UInt32(LM_WidthTabHandle))))
            #expect(Int16(rd32(t + 0x57 * 4) >> 16) == w)
            #expect(rd16(t + UInt32(WidthTable_fID_off)) == 4 && rd16(t + UInt32(WidthTable_fSize_off)) == 9)
            #expect(rd16(t + UInt32(WidthTable_tabSize_off)) == UInt16(WidthTable_SIZEOF))
        }

        /// A program moving the cursor itself (RawMouse and MTemp, then
        /// CrsrNew, as games recentring it do) moves the mouse: the host
        /// hears of it, and captured motion is added to where it put it.
        @Test func programsMoveTheCursor() {
            machine_post_mouse(100, 100, -1)
            events_sync_lowmem()
            var x: Int32 = 0, y: Int32 = 0, relative = false
            let before = machine_mouse_state(&x, &y, &relative)
            #expect(x == 100 && y == 100)
            let center: UInt32 = 384 << 16 | 512
            wr32(UInt32(LM_MouseLocation), center)
            wr32(UInt32(LM_MTemp), center)
            events_sync_lowmem() // between its writes and CrsrNew: waits
            wr8(0x8CE, 0xFF)
            events_sync_lowmem()
            #expect(machine_mouse_state(&x, &y, &relative) == before + 1)
            #expect(x == 512 && y == 384 && relative)
            #expect(rd8(0x8CE) == 0)
            machine_post_mouse_delta(3.5, -2)
            machine_post_mouse_delta(0.5, 0)
            events_sync_lowmem()
            _ = machine_mouse_state(&x, &y, &relative)
            #expect(x == 516 && y == 382)
            #expect(rd32(UInt32(LM_MTemp)) == UInt32(382) << 16 | 516)
            machine_post_mouse_delta(-5000, 5000) // kept on the screen
            _ = machine_mouse_state(&x, &y, &relative)
            #expect(x == 0 && y > 382)
        }

        /// An ADB mouse service routine (Apeiron's) hears the mouse's own
        /// motion, not the program's moves of the cursor.
        @Test func adbPacketsCarryMotionNotCursorMoves() {
            setStack()
            let out = tb_NewPtr(8, false, true), code = tb_NewPtr(32, false, true)
            let words: [UInt16] = [0x13E8, 1, UInt16(out >> 16), UInt16(out & 0xFFFF),           // MOVE.B 1(A0),out
                                   0x13E8, 2, UInt16((out + 1) >> 16), UInt16((out + 1) & 0xFFFF), // MOVE.B 2(A0),out+1
                                   0x5279, UInt16((out + 2) >> 16), UInt16((out + 2) & 0xFFFF),     // ADDQ.W #1,out+2
                                   0x4E75]
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let info = tb_NewPtr(8, false, true)
            wr32(info, code)
            machine_post_mouse(100, 100, -1)
            events_sync_lowmem()
            #expect(tb_SetADBInfo(info, 3) == 0)
            // The program recentres the cursor: no packet.
            let center: UInt32 = 384 << 16 | 512
            wr32(UInt32(LM_MouseLocation), center)
            wr32(UInt32(LM_MTemp), center)
            wr8(0x8CE, 0xFF)
            events_sync_lowmem()
            adb_pump()
            #expect(rd16(out + 2) == 0)
            // The mouse moves 5 right: one packet saying so.
            machine_post_mouse_delta(5, 0)
            events_sync_lowmem()
            adb_pump()
            #expect(rd16(out + 2) == 1)
            #expect(rd8(out) == 0x80 && rd8(out + 1) == 0x85)
        }

        /// Key1Trans translates a key code (D2) with modifiers (D1) to a character.
        @Test func keyTransVectors() {
            setStack()
            let c = g_cpu!
            c.pointee.d.1 = 0
            c.pointee.d.2 = 0 // the A key
            guest_call_regs(rd32(UInt32(LM_Key1Trans)))
            #expect(c.pointee.d.0 & 0xFF == 0x61)
            c.pointee.d.1 = 0x02 // shift
            c.pointee.d.2 = 0
            guest_call_regs(rd32(UInt32(LM_Key2Trans)))
            #expect(c.pointee.d.0 & 0xFF == 0x41)
        }

        /// ErrorSound sets the alert sound procedure; InitDialogs keeps ResumeProc.
        @Test func dialogGlobals() {
            #expect(rd32(UInt32(LM_DABeeper)) != 0) // the standard one
            tb_ErrorSound(0x1234)
            #expect(rd32(UInt32(LM_DABeeper)) == 0x1234)
            tb_InitDialogs(0x5678)
            #expect(rd32(UInt32(LM_ResumeProc)) == 0x5678)
        }

        /// A desk hook paints the desktop (D0 = 0) instead of DeskPattern, and
        /// FindWindow gives the desktop to it as inSysWindow.
        @Test func deskHookPaintsTheDesktop() {
            launchTestApp()
            _ = initToolbox()
            let out = tb_NewPtr(8, false, true)
            let words: [UInt16] = [0x52B9, UInt16(out >> 16), UInt16(out & 0xFFFF),               // ADDQ.L #1,out
                                   0x23C0, UInt16((out + 4) >> 16), UInt16((out + 4) & 0xFFFF), 0x4E75] // MOVE.L D0,out+4; RTS
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            wr32(UInt32(LM_DeskHook), code)
            wr32(out + 4, 0xFFFF_FFFF)
            let r = tb_NewPtr(8, false, true)
            wr16(r, 50); wr16(r + 2, 50); wr16(r + 4, 150); wr16(r + 6, 200)
            let title = tb_NewPtr(8, false, true)
            guestPString("Test", at: title)
            let w = tb_NewWindow(0, r, title, 1, 0, 0xFFFF_FFFF, 0, 0)
            #expect(w != 0)
            tb_DisposeWindow(w) // uncovers desktop
            #expect(rd32(out) >= 1 && rd32(out + 4) == 0)
            let wp = tb_NewPtr(4, false, true)
            #expect(tb_FindWindow(0x0200_0200, wp) == 2 && rd32(wp) == 0) // inSysWindow, no window
            wr32(UInt32(LM_DeskHook), 0)
        }

        /// A deferred task runs once at the next interrupt, with A1 = dtParam.
        @Test func deferredTaskRuns() {
            setStack()
            let out = tb_NewPtr(8, false, true)
            let words: [UInt16] = [0x23C9, UInt16(out >> 16), UInt16(out & 0xFFFF),               // MOVE.L A1,out
                                   0x52B9, UInt16((out + 4) >> 16), UInt16((out + 4) & 0xFFFF), 0x4E75] // ADDQ.L #1,out+4; RTS
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let task = tb_NewPtr(20, false, true)
            wr16(task + 4, 7) // dtQType
            wr32(task + 8, code)
            wr32(task + 12, 0xCAFE)
            #expect(tb_DTInstall(task) == 0)
            g_cpu!.pointee.sr = 0x2000
            time_pump()
            time_pump()
            #expect(rd32(out) == 0xCAFE && rd32(out + 4) == 1)
            #expect(rd32(UInt32(LM_JDTInstall)) != 0)
        }
    }
}

extension BlueBox {
    /// The environment profile: the host's input timing and --lowmem
    /// settings at boot, only real settings; Blue Box's own Gestalt marker.
    @Suite struct Profile {
        @Test func settingsAtBoot() {
            machine_set_input_timing(20, 0, 15, 2)
            #expect(machine_set_lowmem("SdVolume", 3))
            #expect(machine_set_lowmem("CaretTime", 40))
            #expect(!machine_set_lowmem("ApplZone", 0)) // the machine's own state
            #expect(!machine_set_lowmem("NoSuchThing", 0))
            launchTestApp()
            defer {
                _ = machine_set_lowmem(nil, 0)
                machine_set_input_timing(30, 32, 16, 4)
            }
            #expect(rd32(UInt32(LM_DoubleTime)) == 20 && rd16(UInt32(LM_KeyThresh)) == 15)
            #expect(rd16(UInt32(LM_KeyRepThresh)) == 2)
            #expect(rd32(UInt32(LM_CaretTime)) == 40 && rd8(UInt32(LM_SdVolume)) == 3)
            var v: Int32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("blbx")), &v) == 0 && v >> 16 == 0x0100)
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("bbox")), &v) != 0) // not Classic's
        }
    }

    /// A folder mounted as a volume of its own, as an application's CD.
    @Suite struct MountedVolumes {
        @Test func theApplicationsDiskIsAVolume() {
            let dir = tempDir()
            FileManager.default.createFile(atPath: dir.appendingPathComponent("Readme").path, contents: Data("hi".utf8))
            machine_mount_folder(dir.path, "FREAKSHOW")
            launchTestApp()
            _ = initToolbox()
            // Index 2 is the mounted volume, with its own refnum and name.
            let pb = tb_NewPtr(Int32(HVolumeParam_SIZEOF), false, true)
            let name = tb_NewPtr(256, false, true)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            wr16(pb + UInt32(VolumeParam_ioVolIndex_off), 2)
            #expect(tb_PBHGetVInfo(pb, false) == 0)
            let vref = Int16(bitPattern: rd16(pb + UInt32(IOParam_ioVRefNum_off)))
            #expect(vref == -2)
            #expect(pstring(at: name) == "FREAKSHOW")
            // Its root is directory 2; a file there has parent 2 on that volume.
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            guestPString("Readme", at: name)
            #expect(tb_FSMakeFSSpec(vref, 2, name, spec) == 0)
            #expect(Int16(bitPattern: rd16(spec)) == -2 && rd32(spec + 2) == 2)
            // A full path by the volume's name.
            guestPString("FREAKSHOW:Readme", at: name)
            #expect(tb_FSMakeFSSpec(0, 0, name, spec) == 0)
            #expect(Int16(bitPattern: rd16(spec)) == -2)
            // A full path ignores the directory given with it, even one that doesn't exist.
            #expect(tb_FSMakeFSSpec(-2, 65546, name, spec) == 0)
        }

        /// The mounted volume's drive is in the drive queue, on the CD-ROM
        /// driver (Prince of Persia 2 looks for it there and checks the
        /// driver's name), and the driver reports the drive's status.
        @Test func theVolumesDriveIsOnTheCDDriver() {
            machine_mount_folder(tempDir().path, "PoP")
            launchTestApp()
            _ = initToolbox()
            let pb = tb_NewPtr(Int32(HVolumeParam_SIZEOF), false, true)
            wr16(pb + UInt32(VolumeParam_ioVolIndex_off), 2)
            #expect(tb_PBHGetVInfo(pb, false) == 0)
            let drive = rd16(pb + UInt32(HVolumeParam_ioVDrvInfo_off))
            let refnum = Int16(bitPattern: rd16(pb + UInt32(HVolumeParam_ioVDRefNum_off)))
            var el = rd32(UInt32(LM_DrvQHdr) + 2), found: UInt32 = 0
            while el != 0 {
                if rd16(el + UInt32(DrvQEl_dQDrive_off)) == drive { found = el }
                el = rd32(el)
            }
            #expect(found != 0)
            #expect(Int16(bitPattern: rd16(found + UInt32(DrvQEl_dQRefNum_off))) == refnum)
            #expect(rd8(found - 4) == 0x80) // write-protected
            // The unit table's entry for the driver names it.
            let dce = rd32(rd32(rd32(UInt32(LM_UTableBase)) + UInt32(-1 - Int32(refnum)) * 4))
            #expect(pstring(at: rd32(dce + UInt32(DCtlEntry_dCtlDriver_off)) + 18) == ".AppleCD")
            // Status 8: the drive's status.
            let st = tb_NewPtr(Int32(CntrlParam_SIZEOF), false, true)
            wr16(st + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: refnum))
            wr16(st + UInt32(IOParam_ioVRefNum_off), drive)
            wr16(st + UInt32(CntrlParam_csCode_off), 8)
            #expect(tb_PBStatus(st, false) == 0)
            let sts = st + UInt32(CntrlParam_csParam_off)
            #expect(rd8(sts + UInt32(DrvSts_writeProt_off)) == 0x80 && rd8(sts + UInt32(DrvSts_installed_off)) == 1)
            #expect(rd16(sts + UInt32(DrvSts_dQDrive_off)) == drive)
        }
    }
}
