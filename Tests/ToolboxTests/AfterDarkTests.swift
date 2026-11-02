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

        /// After Dark 3 and 4 as the PowerPC modules of After Dark 4 find
        /// them: Gestalt 'aYmm' gives the record with the version, and the
        /// 'ADdl' extension (the linker, version $0300 and up) opens a
        /// resource database by name from a "Module Resources" folder,
        /// makes it current and puts the module's file back.
        @Test func theLinkersResourceDatabases() {
            bootMachine()
            let dir = tempDir()
            let res = dir.appendingPathComponent("Module Resources")
            try? FileManager.default.createDirectory(at: res, withIntermediateDirectories: true)
            _ = writeResourceFile([TestResource(type: "PICT", id: 128, data: [0, 1, 2, 3])], name: "Art", in: res)
            let out = tb_NewPtr(32, true, true), name = tb_NewPtr(8, true, true)
            guestPString("Art", at: name)
            func hi(_ a: UInt32) -> UInt16 { UInt16(a >> 16) }
            func lo(_ a: UInt32) -> UInt16 { UInt16(a & 0xFFFF) }
            let body: [UInt16] = [
                0x206F, 0x0004,                   // MOVEA.L 4(SP),A0      params
                0x2068, 0x0028,                   // MOVEA.L 40(A0),A0     the extensions
                0x2068, 0x0016,                   // MOVEA.L 22(A0),A0     the third's routines
                0x23C8, hi(out), lo(out),         // MOVE.L A0,out
                0x4267,                           // CLR.W -(SP)           OpenDatabase(params, "Art", &ref)
                0x2F2F, 0x0006,                   // MOVE.L 6(SP),-(SP)
                0x2F3C, hi(name), lo(name),
                0x2F3C, hi(out + 4), lo(out + 4),
                0x2079, hi(out), lo(out), 0x2068, 0x001A, 0x4E90,
                0x33DF, hi(out + 8), lo(out + 8), // MOVE.W (SP)+,out+8
                0x2F39, hi(out + 4), lo(out + 4), // UseDatabase(ref)
                0x2079, hi(out), lo(out), 0x2068, 0x0022, 0x4E90,
                0x554F, 0xA994,                   // CurResFile
                0x33DF, hi(out + 10), lo(out + 10),
                0x42A7,                           // RestoreDatabase()
                0x2079, hi(out), lo(out), 0x2068, 0x0026, 0x4E90,
                0x23DF, hi(out + 12), lo(out + 12),
                0x554F, 0xA994,
                0x33DF, hi(out + 16), lo(out + 16),
            ]
            let code = code68k([0x4A6F, 0x0008, 0x6600, UInt16(body.count * 2 + 2)] + body + [
                0x426F, 0x0012, 0x225F, 0x4FEF, 0x000E, 0x4ED1, // noErr; return
            ])
            let path = moduleFile(code, in: dir)
            machine_screensaver_set_seconds(0.1)
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 20_000_000)
            _ = machine_run(30_000_000)
            machine_set_instruction_limit(0)
            #expect(String(cString: machine_halt_reason()).contains("ran its time"))
            let info = gestalt_value(fourCC("aYmm"), 0)
            #expect(info != 0 && rd16(info + 12) >= 0x0300)
            let dl = rd32(out)
            #expect(dl != 0 && rd16(dl) >= 0x0300)
            #expect(rd16(out + 8) == 0 && rd32(out + 4) != 0)         // opened
            let db = Int16(bitPattern: rd16(out + 10))
            #expect(db > 0 && db != Int16(bitPattern: rd16(UInt32(LM_CurApRefNum)))) // the database was current
            #expect(Int32(bitPattern: rd32(out + 12)) == Int32(db))  // Restore answers the one it leaves
            #expect(rd16(out + 16) == rd16(UInt32(LM_CurApRefNum)))  // and the module's file is back
        }

        /// Settings are kept, as After Dark kept them in the module's file:
        /// a control given once and a resource the module changed itself
        /// are there the next time, from a file of their own (the module
        /// file isn't written), which the Options sheet can also set.
        @Test func settingsAreKept() {
            let dir = tempDir(), settings = dir.appendingPathComponent("Settings").path
            func run(_ path: String, control: Int32? = nil) {
                bootMachine()
                machine_screensaver_reset()
                machine_screensaver_set_settings_dir(settings)
                if let control { #expect(machine_screensaver_set_control(0, control)) }
                machine_screensaver_set_seconds(0.1)
                var err = [CChar](repeating: 0, count: 256)
                #expect(machine_launch(path, &err, err.count))
                machine_set_instruction_limit(g_cpu!.pointee.icount + 20_000_000)
                _ = machine_run(30_000_000)
                machine_set_instruction_limit(0)
            }
            // A module that adds one to its 'xDat' 128 at each Initialize.
            let code = code68k([
                0x4A6F, 0x0008, 0x6600, 0x001A, // TST.W 8(SP); BNE.W done
                0x598F,                         // SUBQ.L #4,SP
                0x2F3C, 0x7844, 0x6174,         // MOVE.L #'xDat',-(SP)
                0x3F3C, 0x0080, 0xA81F,         // MOVE.W #128,-(SP); _Get1Resource
                0x205F, 0x2F08, 0x2050,         // MOVEA.L (SP)+,A0; MOVE.L A0,-(SP); MOVEA.L (A0),A0
                0x5250, 0xA9AA,                 // ADDQ.W #1,(A0); _ChangedResource
                0x426F, 0x0012, 0x225F, 0x4FEF, 0x000E, 0x4ED1, // done: noErr; return
            ])
            let counter = writeResourceFile([
                TestResource(type: "ADgm", id: 0, data: code),
                TestResource(type: "xDat", id: 128, data: be16(5)),
                TestResource(type: "sVal", id: 1000, data: be16(10)),
            ], name: "Counter", in: dir)
            run(counter)
            run(counter, control: 66)
            var len = 0
            let fork = machine_screensaver_module_fork(counter, &len)
            defer { free(fork) }
            var idx = rsrc_index()
            var err = [CChar](repeating: 0, count: 256)
            #expect(fork != nil && rsrc_parse(fork, len, &idx, &err, err.count))
            defer { rsrc_index_free(&idx) }
            func word(_ type: String, _ id: Int16) -> Int? {
                for i in 0..<idx.count {
                    var e = idx.entries[i]
                    if e.type == fourCC(type) && e.id == id, let d = rsrc_data(fork, &idx, &e) {
                        return Int(UInt16(d[0]) << 8 | UInt16(d[1]))
                    }
                }
                return nil
            }
            #expect(word("xDat", 128) == 7)  // two runs, each one more
            #expect(word("sVal", 1000) == 66) // the control given
            // The module file is as it was.
            var mf = mac_file()
            #expect(appledouble_read(counter, &mf, &err, err.count))
            #expect(machine_screensaver_store_resource(counter, fourCC("sVal"), 1000, be16(10), 2)) // back to its own
            var path = [CChar](repeating: 0, count: 1400)
            #expect(machine_screensaver_settings_path(counter, &path, path.count))
            let kept = FileManager.default.contents(atPath: String(cString: path))
            var keptIdx = rsrc_index()
            if let kept {
                let ok = [UInt8](kept).withUnsafeBufferPointer { rsrc_parse($0.baseAddress, kept.count, &keptIdx, &err, err.count) }
                #expect(ok && keptIdx.count == 1)                 // only the xDat now
                if ok { rsrc_index_free(&keptIdx) }
            } else {
                Issue.record("no settings file")
            }
            var len2 = 0
            let fork2 = machine_screensaver_module_fork(counter, &len2)
            #expect(fork2 != nil && len2 > 0)
            free(fork2)
            mac_file_free(&mf)
            // A module that logs its parameter block sees the kept control,
            // and a copy of it elsewhere has the same settings.
            func boot() -> (UInt32, UInt32, UInt32) {
                bootMachine()
                machine_screensaver_reset()
                machine_screensaver_set_settings_dir(settings)
                machine_screensaver_set_seconds(0.1)
                return (tb_NewPtr(4096, false, true), tb_NewPtr(64, false, true), tb_NewPtr(16, false, true))
            }
            func go(_ path: String) {
                var e = [CChar](repeating: 0, count: 256)
                #expect(machine_launch(path, &e, e.count))
                machine_set_instruction_limit(g_cpu!.pointee.icount + 20_000_000)
                _ = machine_run(30_000_000)
                machine_set_instruction_limit(0)
            }
            let (log, psave, evt) = boot()
            let logger = moduleFile(moduleCode(log: log, psave: psave, evt: evt), extra: [
                TestResource(type: "sVal", id: 1000, data: be16(42)),
            ], in: tempDir())
            #expect(machine_screensaver_set_control(0, 12))
            go(logger)
            #expect(rd16(psave) == 12)
            let elsewhere = tempDir()
            for f in ["Test Module", "._Test Module"] {
                let from = (logger as NSString).deletingLastPathComponent + "/" + f
                if FileManager.default.fileExists(atPath: from) {
                    try? FileManager.default.copyItem(atPath: from, toPath: elsewhere.appendingPathComponent(f).path)
                }
            }
            let (log2, psave2, evt2) = boot()
            #expect(log2 == log && psave2 == psave && evt2 == evt) // the module's code writes there
            go(elsewhere.appendingPathComponent("Test Module").path)
            #expect(rd16(psave2) == 12) // the same module, elsewhere: its settings follow it
        }

        /// Modules in turn, as After Dark's Randomize ran them: the first
        /// is closed when its time is up and the next begun from its file,
        /// then the first again.
        @Test func modulesTakeTurns() {
            bootMachine()
            let logA = tb_NewPtr(4096, false, true), logB = tb_NewPtr(4096, false, true)
            let psave = tb_NewPtr(64, false, true), evt = tb_NewPtr(16, false, true)
            let a = moduleFile(moduleCode(log: logA, psave: psave, evt: evt), in: tempDir())
            let b = moduleFile(moduleCode(log: logB, psave: psave, evt: evt), in: tempDir())
            #expect(machine_screensaver_add_module(b))
            machine_screensaver_set_rotation(0.2, false)
            machine_screensaver_set_seconds(0.5)
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(a, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 60_000_000)
            _ = machine_run(80_000_000)
            machine_set_instruction_limit(0)
            #expect(String(cString: machine_halt_reason()).contains("ran its time"))
            let seenA = messages(logA), seenB = messages(logB)
            #expect(seenA.filter { $0 == 0 }.count == 2 && seenA.filter { $0 == 1 }.count == 2) // A, B, A
            #expect(seenB.filter { $0 == 0 }.count == 1 && seenB.filter { $0 == 1 }.count == 1)
            #expect(Array(seenB.prefix(3)) == [0, 2, 3] && seenB.last == 1)
            #expect(machine_screensaver_calls(0) == 3)
        }

        /// A module begins (and starts over, RestartMe) in memory as fresh
        /// as the heap After Dark gave it: a block it allocates without
        /// clearing holds zeros, whatever was there before.
        @Test func aModuleGetsFreshMemory() {
            bootMachine()
            let zone = rd32(UInt32(LM_TheZone))
            let p = tb_NewPtr(256, false, false)
            for i in 0..<256 { wr8(p + UInt32(i), 0xAA) }
            tb_DisposePtr(p)
            mm_clear_free(zone)
            let q = tb_NewPtr(256, false, false)
            #expect((0..<256).allSatisfy { rd8(q + UInt32($0)) == 0 })
            #expect(mm_heap_valid())
        }

        /// Several displays: the module gets each as one of After Dark's
        /// monitors, main first, and each starts from the desktop picture;
        /// what lies outside them is no display's.
        @Test func eachDisplayIsAMonitor() {
            bootMachine()
            let out = tb_NewPtr(64, false, true)
            func hi(_ a: UInt32) -> UInt16 { UInt16(a >> 16) }
            func lo(_ a: UInt32) -> UInt16 { UInt16(a & 0xFFFF) }
            let body: [UInt16] = [
                0x206F, 0x0004, 0x2068, 0x0008,   // MOVEA.L 4(SP),A0; MOVEA.L 8(A0),A0   the monitors
                0x43F9, hi(out), lo(out),         // LEA out,A1
                0x700A, 0x32D8, 0x51C8, 0xFFFC,   // MOVEQ #10,D0; copy 11 words
            ]
            let code = code68k([0x4A6F, 0x0008, 0x6600, UInt16(body.count * 2 + 2)] + body + [
                0x426F, 0x0012, 0x225F, 0x4FEF, 0x000E, 0x4ED1,
            ])
            let path = moduleFile(code, in: tempDir())
            #expect(machine_screensaver_add_monitor(320, 0, 640, 480)) // main: the right half
            #expect(machine_screensaver_add_monitor(0, 0, 320, 240))
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
            #expect(rd16(out) == 2)
            #expect((0..<4).map { rd16(out + 2 + UInt32($0 * 2)) } == [0, 320, 480, 640])   // top, left, bottom, right
            #expect((0..<4).map { rd16(out + 12 + UInt32($0 * 2)) } == [0, 0, 240, 320])
            func px(_ x: Int32, _ y: Int32) -> [UInt8] {
                var p = [UInt8](repeating: 0, count: 4)
                machine_screen_rgba(x, y, 1, 1, &p)
                return Array(p.prefix(3))
            }
            #expect(px(500, 400) == [255, 0, 0] && px(100, 100) == [255, 0, 0])
            #expect(px(100, 400) != [255, 0, 0])
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

        #if canImport(ScreenSaver)
        /// The Options sheet reads a module's controls with its kept
        /// settings over them, stores a control's value there by its
        /// resource, and offers its buttons (their 'bVal' IDs run its setup).
        @Test func theOptionsSheetUsesTheSettings() {
            let dir = tempDir()
            machine_screensaver_reset()
            machine_screensaver_set_settings_dir(dir.appendingPathComponent("Settings").path)
            defer { machine_screensaver_set_settings_dir(nil) }
            let path = writeResourceFile([
                TestResource(type: "ADgm", id: 0, data: code68k([0x4E75])),
                TestResource(type: "sVal", id: 1000, name: "Speed:", data: be16(60)),
                TestResource(type: "bVal", id: 1002, name: "Choose File", data: be16(9)),
            ], name: "Settings Module", in: dir)
            var info = ModuleCatalog.info(path, config: ModuleCatalog.config(depth: 8, sound: false))
            #expect(info.controls[0]?.value == 60 && info.controls[0]?.type == "sVal" && info.controls[0]?.id == 1000)
            #expect(info.controls[2]?.kind == .button && info.controls[2]?.id == 1002 && info.controls[2]?.title == "Choose File")
            #expect(machine_screensaver_store_resource(path, ModuleCatalog.typeCode("sVal"), 1000, be16(25), 2))
            info = ModuleCatalog.info(path, config: ModuleCatalog.config(depth: 8, sound: false))
            #expect(info.controls[0]?.value == 25)
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

        /// 68k code on a Power Mac loads PowerPC code with the Code
        /// Fragment Manager's trap, $AA5A (the "PPC" modules' stubs do):
        /// GetMemFragment gives a connection and main's transition vector,
        /// FindSymbol finds main by name.
        @Test func codeFragmentDispatchFrom68k() {
            bootMachine()
            let unimplemented = tb_GetTrapAddress(0xA89F, true, true)
            #expect(tb_GetTrapAddress(0xAA5A, false, false) == unimplemented) // a Quadra has no such trap
            gestalt_powerpc()                                                 // a Power Mac does
            #expect(tb_GetTrapAddress(0xAA5A, false, false) != unimplemented)
            cfm_reset()
            _ = cfm_ppc_cpu()
            func b32(_ v: UInt32) -> [UInt8] { be32(v) }
            func b16(_ v: UInt16) -> [UInt8] { be16(Int(v)) }
            // A PEF: code (li r3,7; blr), a data section holding main's
            // vector (RelocTVector8), main exported as "main".
            let strings = Array("main".utf8) + [0, 0, 0, 0]
            var loader = b32(1) + b32(0) + b32(UInt32(bitPattern: -1)) + b32(0) + b32(UInt32(bitPattern: -1)) + b32(0)
            loader += b32(0) + b32(0) + b32(1) + b32(68) + b32(72) + b32(80) + b32(0) + b32(1)
            loader += b16(1) + b16(0) + b32(1) + b32(0)                    // relocations for section 1
            loader += b16(0x4600) + b16(0)                                 // RelocTVector8, once
            loader += strings                                              // at 72
            loader += b32(1 << 18 | 0)                                     // hash table: one slot, one export
            loader += b32(4 << 16 | 0)                                     // key: length 4 (hash ignored)
            loader += b32(UInt32(2) << 24 | 0) + b32(0) + b16(1)           // "main": a TVector, at 0 in section 1
            while loader.count % 4 != 0 { loader.append(0) }
            let codeOff: UInt32 = 40 + 3 * 28, dataOff = codeOff + 8, loaderOff = dataOff + 8
            var pef = Array("Joy!peffpwpc".utf8) + b32(1) + b32(0) + b32(0) + b32(0) + b32(0) + b16(3) + b16(2) + b32(0)
            pef += b32(UInt32(bitPattern: -1)) + b32(0) + b32(8) + b32(8) + b32(8) + b32(codeOff) + [0, 1, 2, 0]
            pef += b32(UInt32(bitPattern: -1)) + b32(0) + b32(8) + b32(8) + b32(8) + b32(dataOff) + [1, 1, 2, 0]
            pef += b32(UInt32(bitPattern: -1)) + b32(0) + b32(0) + b32(0) + b32(UInt32(loader.count)) + b32(loaderOff) + [4, 1, 0, 0]
            pef += b32(0x3860_0007) + b32(0x4E80_0020) + b32(0) + b32(0) + loader
            let mem = tb_NewPtr(Int32(pef.count), false, false)
            for (i, b) in pef.enumerated() { wr8(mem + UInt32(i), b) }
            let out = tb_NewPtr(64, false, true) // connID, main, err (Str255) at 8, symAddr at 4+..., symClass
            let name = tb_NewPtr(8, false, true)
            guestPString("main", at: name)
            func hi(_ a: UInt32) -> UInt16 { UInt16(a >> 16) }
            func lo(_ a: UInt32) -> UInt16 { UInt16(a & 0xFFFF) }
            let reason = run68k([
                0x4267,                                          // CLR.W -(SP)   the OSErr
                0x2F3C, hi(mem), lo(mem),                        // memAddr
                0x2F3C, hi(UInt32(pef.count)), lo(UInt32(pef.count)), // length
                0x42A7,                                          // fragName nil
                0x2F3C, 0x0000, 0x0001,                          // loadFlags kReferenceCFrag
                0x2F3C, hi(out), lo(out),                        // *connID
                0x2F3C, hi(out + 4), lo(out + 4),                // *mainAddr
                0x2F3C, hi(out + 8), lo(out + 8),                // errMessage
                0x3F3C, 0x0003, 0xAA5A,                          // GetMemFragment
                0x33DF, hi(out + 40), lo(out + 40),              // MOVE.W (SP)+,out+40
                0x4267,                                          // FindSymbol(connID, "main", *symAddr, *symClass)
                0x2F39, hi(out), lo(out),
                0x2F3C, hi(name), lo(name),
                0x2F3C, hi(out + 44), lo(out + 44),
                0x2F3C, hi(out + 48), lo(out + 48),
                0x3F3C, 0x0005, 0xAA5A,
                0x33DF, hi(out + 42), lo(out + 42),              // MOVE.W (SP)+,out+42
                0xA9F4,                                          // ExitToShell
            ])
            #expect(reason.hasPrefix("ExitToShell"))
            #expect(rd16(out + 40) == 0 && rd32(out) != 0)
            let tv = rd32(out + 4)
            #expect(tv != 0 && rd32(rd32(tv)) == 0x3860_0007)  // main: li r3,7
            #expect(rd16(out + 42) == 0 && rd32(out + 44) == tv)
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
