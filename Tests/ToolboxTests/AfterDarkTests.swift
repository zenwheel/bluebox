import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox
#if canImport(ScreenSaver)
@testable import BlueBoxSaver
#endif

/// An After Dark graphics module of our own: main logs each message it gets
/// (a count, then the messages), keeps a copy of the parameter block, asks
/// whether After Dark is there (AfterDarkExists: an event record with
/// 'aYmm' and GetOSEvent with no mask), and answers noErr, or drawResult to
/// DrawFrame.
private func moduleCode(log: UInt32, psave: UInt32, evt: UInt32, drawResult: UInt16 = 0) -> [UInt8] {
    func hi(_ a: UInt32) -> UInt16 { UInt16(a >> 16) }
    func lo(_ a: UInt32) -> UInt16 { UInt16(a & 0xFFFF) }
    return code68k([
        // pascal OSErr main(Handle *storage, RgnHandle blankRgn, short message, GMParamBlockPtr params)
        0x302F, 0x0008,                   // MOVE.W 8(SP),D0       the message
        0x207C, hi(log), lo(log),         // MOVEA.L #log,A0
        0x3210,                           // MOVE.W (A0),D1
        0x43E8, 0x0002,                   // LEA 2(A0),A1
        0xD2C1, 0xD2C1,                   // ADDA.W D1,A1 twice
        0x3280,                           // MOVE.W D0,(A1)
        0x5250,                           // ADDQ.W #1,(A0)
        0x206F, 0x0004,                   // MOVEA.L 4(SP),A0      the parameter block
        0x227C, hi(psave), lo(psave),     // MOVEA.L #psave,A1
        0x720A,                           // MOVEQ #10,D1
        0x22D8, 0x51C9, 0xFFFC,           // MOVE.L (A0)+,(A1)+; DBRA D1 (its 44 bytes)
        0x41F9, hi(evt), lo(evt),         // LEA evt,A0
        0x217C, 0x6159, 0x6D6D, 0x0002,   // MOVE.L #'aYmm',2(A0)
        0x7000, 0xA031,                   // MOVEQ #0,D0; _GetOSEvent
        0x426F, 0x0012,                   // CLR.W 18(SP)          noErr
        0x0C6F, 0x0003, 0x0008,           // CMPI.W #DrawFrame,8(SP)
        0x6606,                           // BNE.S +6
        0x3F7C, drawResult, 0x0012,       // MOVE.W #drawResult,18(SP)
        0x225F, 0x4FEF, 0x000E, 0x4ED1,   // MOVEA.L (SP)+,A1; LEA 14(SP),SP; JMP (A1)
    ])
}

private func moduleFile(_ code: [UInt8], extra: [TestResource] = [], in dir: URL) -> String {
    writeResourceFile([TestResource(type: "ADgm", id: 0, data: code)] + extra, name: "Test Module", in: dir)
}

/// The messages the module logged.
private func messages(_ log: UInt32) -> [Int16] {
    (0..<Int(rd16(log))).map { Int16(bitPattern: rd16(log + 2 + UInt32($0 * 2))) }
}

extension BlueBox {
    /// After Dark graphics modules, run as the screen saver (afterdark.c).
    @Suite struct AfterDark {
        init() { machine_screensaver_reset() }

        /// Initialize, Blank, then DrawFrame each tick, and Close when the
        /// time is up; the parameter block has the controls, the version,
        /// the configuration and the extensions, and After Dark answers
        /// the module asking whether it is there.
        @Test func aModuleRunsItsMessages() {
            bootMachine()
            let log = tb_NewPtr(4096, false, true), psave = tb_NewPtr(64, false, true), evt = tb_NewPtr(16, false, true)
            let path = moduleFile(moduleCode(log: log, psave: psave, evt: evt), extra: [
                TestResource(type: "sVal", id: 1000, data: be16(42)),
                TestResource(type: "xVal", id: 1001, data: be16(1)),
                TestResource(type: "mVal", id: 1003, data: be16(2)),
            ], in: tempDir())
            machine_screensaver_set_seconds(0.5)
            #expect(machine_screensaver_set_control(1002, 77))
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 40_000_000)
            _ = machine_run(50_000_000)
            machine_set_instruction_limit(0)
            #expect(String(cString: machine_halt_reason()).contains("ran its time"))
            let seen = messages(log)
            #expect(Array(seen.prefix(3)) == [0, 2, 3]) // Initialize, Blank, DrawFrame
            #expect(seen.last == 1)                     // Close
            #expect(seen.dropFirst(2).dropLast().allSatisfy { $0 == 3 })
            #expect(seen.count > 30 + 3)                // a frame a tick at least, for half a second
            #expect(machine_screensaver_frames() == UInt32(seen.count - 3))
            // controlValues: the resources' and the one given
            #expect((0..<4).map { rd16(psave + UInt32($0 * 2)) } == [42, 1, 77, 2])
            #expect(rd16(psave + 38) == 0x0300)            // adVersion
            #expect(rd16(psave + 14) & 0x4001 == 0x4001)    // extensions there, Color QuickDraw
            #expect(rd16(psave + 14) & 0x8000 == 0)         // no sound unless asked
            #expect(rd16(psave + 20) == 255)                // brightness
            #expect(rd32(psave + 8) != 0 && rd32(psave + 16) != 0 && rd32(psave + 30) != 0 && rd32(psave + 40) != 0)
            // AfterDarkExists: 'ADrk' and the record with the version at 12
            #expect(rd32(evt + 2) == fourCC("ADrk"))
            #expect(rd16(rd32(evt + 10) + 12) == 0x0300)
        }

        /// The extension tables the developer kit's glue looks up: sound
        /// ('ADSd', seven routines) and memory ('ADmf', four), each after
        /// a count; the glue calls them as Pascal routines.
        @Test func theExtensionTablesAreThere() {
            bootMachine()
            let log = tb_NewPtr(4096, false, true), psave = tb_NewPtr(64, false, true), evt = tb_NewPtr(16, false, true)
            // A module that, on Initialize, calls FindMemory(params, 1000) and
            // UseMemory, keeping the handle and the zone UseMemory set.
            let out = tb_NewPtr(16, false, true)
            func hi(_ a: UInt32) -> UInt16 { UInt16(a >> 16) }
            func lo(_ a: UInt32) -> UInt16 { UInt16(a & 0xFFFF) }
            let code = code68k([
                0x4A6F, 0x0008,                 // TST.W 8(SP)            Initialize?
                0x6600, 0x0040,                 // BNE.W done
                0x206F, 0x0004,                 // MOVEA.L 4(SP),A0       params
                0x2068, 0x0028,                 // MOVEA.L 40(A0),A0      the extension table
                0x2068, 0x000E,                 // MOVEA.L 14(A0),A0      the second's routines ('ADmf')
                0x2F08,                         // MOVE.L A0,-(SP)        kept
                0x42A7,                         // CLR.L -(SP)            the result
                0x2F2F, 0x000C,                 // MOVE.L 12(SP),-(SP)    params (4(SP) before the pushes)
                0x2F3C, 0x0000, 0x03E8,         // MOVE.L #1000,-(SP)
                0x2068, 0x0002,                 // MOVEA.L 2(A0),A0       FindMemory
                0x4E90,                         // JSR (A0)
                0x23DF, hi(out), lo(out),       // MOVE.L (SP)+,out       the handle
                0x205F,                         // MOVEA.L (SP)+,A0       the table
                0x2F39, hi(out), lo(out),       // MOVE.L out,-(SP)
                0x2068, 0x000A,                 // MOVEA.L 10(A0),A0      UseMemory
                0x4E90,                         // JSR (A0)
                0x23F8, 0x0118, hi(out + 4), lo(out + 4), // MOVE.L TheZone,out+4
                0x4E71,                         // NOP
                // done:
                0x426F, 0x0012,                 // CLR.W 18(SP)
                0x225F, 0x4FEF, 0x000E, 0x4ED1, // return
            ])
            _ = (log, psave, evt)
            let path = moduleFile(code, in: tempDir())
            machine_screensaver_set_seconds(0.2)
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 20_000_000)
            _ = machine_run(30_000_000)
            machine_set_instruction_limit(0)
            #expect(String(cString: machine_halt_reason()).contains("ran its time"))
            #expect(rd32(out) != 0)                                   // a handle
            #expect(rd32(out + 4) == rd32(UInt32(LM_ApplZone)))       // the zone given
        }

        /// ImDone: no more DrawFrame calls; Close still comes.  A module
        /// whose 'Cals' leaves out Blank gets the screen blanked for it.
        @Test func imDoneStopsTheFrames() {
            bootMachine()
            let log = tb_NewPtr(4096, false, true), psave = tb_NewPtr(64, false, true), evt = tb_NewPtr(16, false, true)
            let path = moduleFile(moduleCode(log: log, psave: psave, evt: evt, drawResult: 2), extra: [
                TestResource(type: "Cals", id: 1, data: [0x0B]), // Initialize, Close, DrawFrame
            ], in: tempDir())
            machine_screensaver_set_seconds(0.3)
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 30_000_000)
            _ = machine_run(40_000_000)
            machine_set_instruction_limit(0)
            #expect(messages(log) == [0, 3])  // Initialize, one DrawFrame; Close not after ImDone
            var px = [UInt8](repeating: 9, count: 4)
            machine_screen_rgba(300, 300, 1, 1, &px)
            #expect(Array(px.prefix(3)) == [0, 0, 0])
        }

        /// The screen saver's desktop picture is what the screen shows when
        /// the module starts (one that blanks nothing leaves it there).
        @Test func theBackdropIsTheScreenItStartsFrom() {
            bootMachine()
            let log = tb_NewPtr(4096, false, true), psave = tb_NewPtr(64, false, true), evt = tb_NewPtr(16, false, true)
            let path = moduleFile(moduleCode(log: log, psave: psave, evt: evt), in: tempDir())
            var red = [UInt8](repeating: 0, count: 4 * 4 * 4)
            for i in stride(from: 0, to: red.count, by: 4) { red[i] = 255; red[i + 3] = 255 }
            machine_screensaver_set_backdrop(red, 4, 4)
            defer { machine_screensaver_set_backdrop(nil, 0, 0) }
            machine_screensaver_set_seconds(0.1)
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 20_000_000)
            _ = machine_run(30_000_000)
            machine_set_instruction_limit(0)
            var px = [UInt8](repeating: 0, count: 4)
            machine_screen_rgba(500, 400, 1, 1, &px)
            #expect(Array(px.prefix(3)) == [255, 0, 0])
        }

        #if canImport(ScreenSaver)
        /// The screen saver finds modules by their file type and reads the
        /// controls After Dark's panel showed: a slider with its units, a
        /// pop-up menu with the MENU's items, a check box, and a control
        /// whose 'µVal' the screen doesn't meet giving way to the next one.
        @Test func theScreenSaverReadsAModulesControls() {
            let dir = tempDir()
            let units = be16(2) + be16(0) + [4] + Array("slow".utf8) + be16(50) + [4] + Array("fast".utf8)
            let menu = cat(be16(1003), be16(0), be16(0), be16(0), be16(0), be32(0xFFFF_FFFF), [5] + Array("Light".utf8),
                           [3] + Array("Day".utf8) + [0, 0, 0, 0], [5] + Array("Night".utf8) + [0, 0, 0, 0], [0])
            var fork = buildFork([
                TestResource(type: "ADgm", id: 0, data: code68k([0x4E75])),
                TestResource(type: "STR ", id: 128, data: [6] + Array("Credit".utf8)),
                TestResource(type: "sVal", id: 1000, name: "Speed:", data: be16(60)),
                TestResource(type: "sUnt", id: 1000, data: units),
                TestResource(type: "xVal", id: 1001, name: "Colour", data: be16(1)),
                // 1002: a check box only where every screen is a CLUT device, else the slider at 2002
                TestResource(type: "xVal", id: 1002, name: "Colour table", data: be16(0)),
                TestResource(type: "@Val", id: 1002, data: be16(0x0040)),
                TestResource(type: "sVal", id: 2002, name: "Depth", data: be16(5)),
                TestResource(type: "mVal", id: 1003, name: "Light", data: be16(2)),
                TestResource(type: "MENU", id: 1003, data: menu),
            ])
            // 'µVal' is $B5 'V' 'a' 'l' in MacRoman.
            if let i = (0..<(fork.count - 4)).last(where: { Array(fork[$0..<($0 + 4)]) == Array("@Val".utf8) }) { fork[i] = 0xB5 }
            var f = mac_file()
            var empty: [UInt8] = []
            var errb = [CChar](repeating: 0, count: 256)
            let path = dir.appendingPathComponent("My Module").path
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            withUnsafeMutableBytes(of: &f.finder_info) { $0.copyBytes(from: Array("ADgmADrk".utf8)) }
            f.has_finder_info = true
            #expect(appledouble_write(path, &f, &errb, errb.count))
            #expect(ModuleCatalog.scan([dir.path]) == [path])
            let info = ModuleCatalog.info(path, config: ModuleCatalog.config(depth: 32, sound: false))
            #expect(info.credit == "Credit")
            let c = info.controls
            #expect(c[0]?.kind == .slider && c[0]?.title == "Speed:" && c[0]?.value == 60)
            #expect(c[0]?.unitLabel(10) == "slow" && c[0]?.unitLabel(60) == "fast")
            #expect(c[1]?.kind == .checkbox && c[1]?.value == 1)
            #expect(c[2]?.kind == .slider && c[2]?.title == "Depth") // the 'µVal' isn't met at 32 bits
            #expect(ModuleCatalog.info(path, config: ModuleCatalog.config(depth: 8, sound: false)).controls[2]?.kind == .checkbox)
            #expect(c[3]?.kind == .menu && c[3]?.items == ["Day", "Night"] && c[3]?.value == 2)
        }
        #endif

        /// A module whose main is a C function (its arguments left on the
        /// stack, the answer in D0) runs too, and the VBL task sets each
        /// monitor's synchFlag to $FF, which a module waits for in DrawFrame.
        @Test func aCModuleAndTheSynchFlag() {
            bootMachine()
            let log = tb_NewPtr(4096, false, true)
            func hi(_ a: UInt32) -> UInt16 { UInt16(a >> 16) }
            func lo(_ a: UInt32) -> UInt16 { UInt16(a & 0xFFFF) }
            let code = code68k([
                0x302F, 0x0008,                   // MOVE.W 8(SP),D0       the message
                0x207C, hi(log), lo(log),         // MOVEA.L #log,A0
                0x3210, 0x43E8, 0x0002, 0xD2C1, 0xD2C1, 0x3280, 0x5250, // log it
                0x0C40, 0x0003, 0x6612,           // CMPI.W #DrawFrame,D0; BNE.S +18
                0x206F, 0x0004,                   // MOVEA.L 4(SP),A0      params
                0x2068, 0x0008,                   // MOVEA.L 8(A0),A0      monitors
                0x4228, 0x000A,                   // CLR.B 10(A0)          the first one's synchFlag
                0x4A28, 0x000A, 0x6CFA,           // wait: TST.B 10(A0); BGE.S wait
                0x7000, 0x4E75,                   // MOVEQ #0,D0; RTS
            ])
            let path = moduleFile(code, in: tempDir())
            machine_screensaver_set_seconds(0.3)
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 30_000_000)
            _ = machine_run(40_000_000)
            machine_set_instruction_limit(0)
            #expect(String(cString: machine_halt_reason()).contains("ran its time"))
            let seen = messages(log)
            #expect(Array(seen.prefix(3)) == [0, 2, 3] && seen.last == 1)
            // a frame a VBL: about 18 in 0.3 seconds
            #expect(seen.count - 3 >= 10 && seen.count - 3 <= 24)
        }

        /// A button pressed first, as in the control panel: ModuleSelected
        /// (when 'Cals' takes it), then the 'bVal' button's value.
        @Test func buttonsGoFirst() {
            bootMachine()
            let log = tb_NewPtr(4096, false, true), psave = tb_NewPtr(64, false, true), evt = tb_NewPtr(16, false, true)
            let path = moduleFile(moduleCode(log: log, psave: psave, evt: evt), extra: [
                TestResource(type: "Cals", id: 1, data: [0x1F]),
                TestResource(type: "bVal", id: 1002, data: be16(9)),
            ], in: tempDir())
            machine_screensaver_set_seconds(0.1)
            #expect(machine_screensaver_press_button(1002))
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 20_000_000)
            _ = machine_run(30_000_000)
            machine_set_instruction_limit(0)
            #expect(Array(messages(log).prefix(4)) == [4, 9, 0, 2])
        }

        /// In a program's run: after the minutes without input the module
        /// blanks the screen, until a key wakes it; the program's screen
        /// comes back as it was.
        @Test func idleBlankingInARun() {
            bootMachine()
            let log = tb_NewPtr(4096, true, true), psave = tb_NewPtr(64, true, true), evt = tb_NewPtr(16, true, true)
            let dir = tempDir()
            let module = moduleFile(moduleCode(log: log, psave: psave, evt: evt), in: dir)
            // A program waiting for events: WaitNextEvent(everyEvent, ..., 1, nil) in a loop.
            let ebuf = tb_NewPtr(16, true, true)
            let loop: [UInt16] = [0x5547, 0x3F3C, 0xFFFF, 0x4879, UInt16(ebuf >> 16), UInt16(ebuf & 0xFFFF),
                                  0x2F3C, 0x0000, 0x0001, 0x42A7, 0xA860, 0x544F, 0x60E6]
            let jt = cat(be16(0), be16(0x3F3C), be16(1), be16(0xA9F0))
            let app = writeResourceFile([
                TestResource(type: "CODE", id: 0, data: cat(be32(40), be32(256), be32(8), be32(32), jt)),
                TestResource(type: "CODE", id: 1, data: cat(be16(0), be16(1), code68k(loop))),
                TestResource(type: "SIZE", id: -1, data: cat(be16(0x0080), be32(0x80000), be32(0x80000))),
            ], name: "Waiting App", in: dir)
            machine_screensaver_set_idle(module, 0.01) // 36 ticks
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(app, &err, err.count))
            let start = g_cpu!.pointee.icount
            _ = machine_run(2_000_000)
            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            var before = [UInt8](repeating: 0, count: Int(w * h * 4))
            machine_screen_rgba(0, 0, w, h, &before)
            machine_script_add(start + 30_000_000, 0, 0x61, 0, 0) // "a"
            // woken at 30 million; not yet blanked again (36 ticks later)
            machine_set_instruction_limit(start + 33_000_000)
            _ = machine_run(40_000_000)
            machine_set_instruction_limit(0)
            machine_screensaver_set_idle(nil, 0)
            let seen = messages(log)
            #expect(Array(seen.prefix(3)) == [0, 2, 3])
            #expect(seen.last == 1)
            #expect(seen.count > 20)
            var after = [UInt8](repeating: 0, count: Int(w * h * 4))
            machine_screen_rgba(0, 0, w, h, &after)
            #expect(before == after)
        }
    }
}
