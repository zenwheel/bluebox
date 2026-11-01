import BlueBoxHost
import CCore
import Foundation
import CPU68K
import Testing
import Toolbox

/// A DRVR resource: the header (flags, delay, event mask, menu, the
/// routines' offsets, the name) and the code after it.  Open is `open`;
/// Prime, Control and Status answer noErr through jIODone; Close returns noErr.
func deskAccessory(name: String, flags: UInt16 = 0x0400, open: [UInt16]) -> [UInt8] {
    let nameBytes = Array(name.utf8)
    var header = 18 + 1 + nameBytes.count
    header += header & 1
    let other = header                       // MOVEQ #0,D0; MOVE.L jIODone,-(SP); RTS
    let close = other + 8                    // MOVEQ #0,D0; RTS
    let openAt = close + 4
    var d = cat(be16(Int(flags)), be16(0), be16(0), be16(0), be16(openAt), be16(other), be16(other), be16(other),
                be16(close), [UInt8(nameBytes.count)], nameBytes)
    if d.count & 1 == 1 { d.append(0) }
    d += code68k([0x7000, 0x2F38, 0x08FC, 0x4E75, 0x7000, 0x4E75] + open)
    return d
}

extension BlueBox {
    /// What a desk accessory or INIT living in the system heap counts on:
    /// jGNEFilter, drawing on the desktop in rootless mode; and the desk
    /// accessory host that runs a desk accessory file on its own.
    @Suite struct DeskAccessoryHost {
        var cpu: UnsafeMutablePointer<cpu68k> { g_cpu! }

        /// Run code put at `at` until it calls _ExitToShell.
        func run(at code: UInt32, _ words: [UInt16]) {
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let stack = tb_NewPtr(4096, false, true)
            cpu.pointee.a.7 = stack + 4096 - 16
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, 1000)
        }

        /// A file's desk accessories are its DRVRs named with a NUL first
        /// (a driver's name starts with a period); inspect shows them so.
        @Test func deskAccessoryNamesAreFound() throws {
            var fork = buildFork([
                TestResource(type: "DRVR", id: 12, name: "\u{0}Phonemes", data: [0]),
                TestResource(type: "DRVR", id: 31, name: ".SPEECH", data: [0]),
                TestResource(type: "DRVR", id: 29, name: "\u{0}Talking Moose", data: [0]),
                TestResource(type: "STR#", id: 1988, name: "Pause", data: [0, 0]),
            ])
            let names = fork.withUnsafeMutableBufferPointer {
                deskAccessoryNames(fork: UnsafePointer($0.baseAddress), length: $0.count)
            }
            #expect(names.sorted() == ["Phonemes", "Talking Moose"])
            let listed = try fork.withUnsafeMutableBufferPointer {
                try inspectResources(fork: UnsafePointer($0.baseAddress), length: $0.count)
            }
            #expect(listed.first { $0.id == 29 }?.name == "Talking Moose")
        }

        /// GetNextEvent calls the filter at jGNEFilter with A1 the event and
        /// the result at 4(SP), null events too; the filter may eat one.
        @Test func gneFilterSeesEveryEvent() {
            launchTestApp()
            guestStack()
            let log = tb_NewPtr(16, false, true)
            let filter = tb_NewPtr(64, true, false)
            let words: [UInt16] = [
                0x207C, UInt16(log >> 16), UInt16(log & 0xFFFF), // MOVEA.L #log,A0
                0x3091,                                          // MOVE.W (A1),(A0)   what
                0x5268, 0x0002,                                  // ADDQ.W #1,2(A0)    calls
                0x316F, 0x0004, 0x0004,                          // MOVE.W 4(SP),4(A0) the result it got
                0x0C51, 0x0003,                                  // CMPI.W #keyDown,(A1)
                0x6604,                                          // BNE.S +4
                0x426F, 0x0004,                                  // CLR.W 4(SP)        eaten
                0x4E75,                                          // RTS
            ]
            for (i, w) in words.enumerated() { wr16(filter + UInt32(i * 2), w) }
            wr32(0x029A, filter)
            defer { wr32(0x029A, 0) }
            tb_FlushEvents(-1, 0)
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            machine_post_key(0x61, 0, 0, 1, 0)
            #expect(tb_GetNextEvent(0x0008, e) == 0) // the filter ate it
            #expect(rd16(log) == 3 && rd16(log + 2) == 1 && rd16(log + 4) == 0x0100)
            machine_post_key(0x61, 0, 0, 0, 0)
            #expect(tb_GetNextEvent(0x0010, e) != 0) // a key-up goes through
            #expect(rd16(log) == 4 && rd16(log + 2) == 2)
            #expect(tb_GetNextEvent(0, e) == 0)      // a null event is seen too
            #expect(rd16(log) == 0 && rd16(log + 2) == 3 && rd16(log + 4) == 0)
        }

        /// The system's own code (in the system heap) drawing on the screen
        /// outside the windows is an overlay for the host, not a reason for
        /// the whole screen; it ends when what was there is put back.
        @Test func systemCodeDrawingIsAnOverlay() {
            launchTestApp()
            _ = initToolbox()
            display_sync()
            #expect(machine_screen_mode_wanted() == nil)
            let rb = qd_screen_row_bytes(), depth = UInt32(qd_screen_depth())
            let at = rd32(UInt32(LM_ScrnBase)) + 100 * rb + 80 * depth / 8 // pixel (80, 100)
            let was = rd32(at)
            let code = tb_NewPtr(64, true, false)
            var on = false
            var r = [Int16](repeating: 0, count: 4)
            let v0 = machine_overlay(&r, &on)
            #expect(!on)
            // MOVE.L #-1,at (or what was there, if that was all ones); _ExitToShell
            let drawn: UInt32 = was == 0xFFFF_FFFF ? 0 : 0xFFFF_FFFF
            run(at: code, [0x23FC, UInt16(drawn >> 16), UInt16(drawn & 0xFFFF), UInt16(at >> 16), UInt16(at & 0xFFFF), 0xA9F4])
            display_sync()
            #expect(machine_screen_mode_wanted() == nil)
            let v1 = machine_overlay(&r, &on)
            #expect(on && v1 != v0)
            #expect(r == [100, 80, 101, Int16(80 + 32 / Int(depth))])
            var rgba = [UInt8](repeating: 0, count: 4 * 4)
            machine_overlay_rgba(79, 100, 2, 1, &rgba)
            #expect(rgba[3] == 0 && rgba[7] == 255) // clear beside it, opaque where drawn
            // Put back: the overlay is over.
            run(at: code, [0x23FC, UInt16(was >> 16), UInt16(was & 0xFFFF), UInt16(at >> 16), UInt16(at & 0xFFFF), 0xA9F4])
            _ = machine_overlay(&r, &on)
            #expect(!on)
        }

        /// The same store from the program's own code still wants the screen.
        @Test func programDrawingStillWantsTheScreen() {
            launchTestApp()
            _ = initToolbox()
            display_sync()
            let at = rd32(UInt32(LM_ScrnBase)) + 100 * qd_screen_row_bytes()
            run(at: tb_NewPtr(64, false, false), [0x23FC, 0x1234, 0x5678, UInt16(at >> 16), UInt16(at & 0xFFFF), 0xA9F4])
            display_sync()
            var on = false
            var r = [Int16](repeating: 0, count: 4)
            _ = machine_overlay(&r, &on)
            #expect(!on)
            #expect(machine_screen_mode_wanted() != nil)
        }

        /// A file of desk accessories runs in the host: the first is opened;
        /// one with no window closes, and the host quits.
        @Test func deskAccessoryFileRuns() {
            bootMachine()
            let log = tb_NewPtr(16, false, true)
            let open: [UInt16] = [0x33FC, 0x1234, UInt16(log >> 16), UInt16(log & 0xFFFF), // MOVE.W #$1234,log
                                  0x7000, 0x4E75]                                         // MOVEQ #0,D0; RTS
            let path = writeResourceFile([
                TestResource(type: "DRVR", id: 12, name: "\u{0}Test DA", data: deskAccessory(name: "\u{0}Test DA", open: open)),
            ], name: "Test DA", in: tempDir())
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 3_000_000)
            _ = machine_run(4_000_000)
            machine_set_instruction_limit(0)
            #expect(rd16(log) == 0x1234)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
        }

        /// A file with the Talking Moose: it is started hidden, as its
        /// MooseUnit's StartMoose(TRUE) does, and the host stays, with a
        /// Moose menu.  Tell a Joke asks for one of the Pause phrases (STR#
        /// 1988); Be Quiet turns off its idle remarks (PauseMask).
        @Test(arguments: [1, 2]) func mooseHost(item: Int16) {
            bootMachine()
            // Settings kept from another case (Be Quiet) don't count.
            try? FileManager.default.removeItem(atPath: testSystemFolder + "/Preferences/Talking Moose Prefs")
            // The Moose's globals, a VBL task element signed 'TALK'.
            let rec = tb_NewPtr(100, true, true)
            let rts = tb_NewPtr(2, true, false)
            wr16(rts, 0x4E75)
            wr16(rec + 4, 1)                 // qType vType
            wr32(rec + 6, rts)               // vblAddr
            wr32(rec + 14, 0x5441_4C4B)      // 'TALK'
            wr16(rec + 26, 1)                // Enable
            wr16(rec + 64, 5)                // PauseMax
            wr32(rec + 92, 0xFFFF_FFFF)      // MoosFlags
            let open: [UInt16] = [
                0x0CB8, 0x4869, 0x6465, 0x0A78,                  // CMPI.L #'Hide',ApplScratch
                0x660C,                                          // BNE.S done
                0x207C, UInt16(rec >> 16), UInt16(rec & 0xFFFF), // MOVEA.L #rec,A0
                0xA033,                                          // _VInstall
                0x42B8, 0x0A78,                                  // CLR.L ApplScratch: installed
                0x7000, 0x4E75,                                  // done: MOVEQ #0,D0; RTS
            ]
            let path = writeResourceFile([
                TestResource(type: "DRVR", id: 29, name: "\u{0}Talking Moose",
                             data: deskAccessory(name: "Talk Settings", open: open)),
            ], name: "Moose", in: tempDir())
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_choose_menu(131, item) // the Moose menu: Tell a Joke, Be Quiet
            machine_set_instruction_limit(g_cpu!.pointee.icount + 3_000_000)
            _ = machine_run(4_000_000)
            machine_set_instruction_limit(0)
            #expect(String(cString: machine_halt_reason()).contains("instruction limit")) // still running
            #expect(rd32(UInt32(LM_VBLQueue) + 2) != 0)
            #expect(tb_GetMenuHandle(131) != 0)
            if item == 1 {
                #expect(rd16(rec + 44) == 1988)
                #expect((1...5).contains(rd16(rec + 46)))
                #expect(rd32(rec + 92) & 1 == 1)
            } else {
                #expect(rd16(rec + 44) == 0)
                #expect(rd32(rec + 92) & 1 == 0)
            }
        }
    }
}
