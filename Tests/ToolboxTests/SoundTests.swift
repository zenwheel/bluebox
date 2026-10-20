import CCore
import CPU68K
import Foundation
import Testing
import MACE
import Toolbox

extension BlueBox {
    /// MACE decoding (the LGPL target): six samples per packet, a packet
    /// being two bytes of MACE 3:1 or one of MACE 6:1 per channel.
    @Suite struct MACEDecoding {
        @Test func framesPerPacket() {
            let packets = [UInt8](repeating: 0x55, count: 64)
            var out = [Int16](repeating: 0, count: 64 * 6 * 2)
            #expect(mace_decode(packets, 64, 1, true, &out) == 64 / 2 * 6)    // MACE 3, mono
            #expect(mace_decode(packets, 64, 1, false, &out) == 64 * 6)       // MACE 6, mono
            #expect(mace_decode(packets, 64, 2, false, &out) == 64 / 2 * 6)   // MACE 6, stereo
        }
    }

    @Suite struct SoundManager {
        nonisolated(unsafe) static var played: [(frames: Int32, channels: Int32, rate: Double)] = []
        nonisolated(unsafe) static var callbacks = 0

        /// A format 1 'snd ': one sampled-synth modifier, bufferCmd to a standard header.
        static func snd(samples: Int) -> [UInt8] {
            let header = cat(be32(0), be32(UInt32(samples)), be32(0x56EE_8BA3), be32(0), be32(0), [0, 60])
            let body = [UInt8](repeating: 0x80, count: samples)
            let cmdOffset = 2 + 2 + 6 + 2 + 8
            return cat(be16(1), be16(1), be16(5), be32(0), be16(1),
                       be16(0x8000 | 81), be16(0), be32(UInt32(cmdOffset)), header, body)
        }

        static let callback: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)                               // command
            _ = cpu_pop32(c)                               // channel
            SoundManager.callbacks += 1
            cpu_push32(c, ret)
        }

        func install() {
            Self.played = []
            machine_set_sound_handler({ _, pcm, frames, channels, rate, _, _ in
                if pcm != nil { SoundManager.played.append((frames, channels, rate)) }
            }, nil)
        }

        /// SndPlayDoubleBuffer through the old selector (parameter size zero),
        /// as compiled with older interfaces: both ready buffers play and the
        /// doubleBack procedure is called for each.
        @Test func doubleBufferPlaysReadyBuffersAndCallsBack() {
            launchTestApp()
            install()
            defer { machine_set_sound_handler(nil, nil) }
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let chanp = tb_NewPtr(4, false, true)
            #expect(tb_SndNewChannel(chanp, 5, 0, 0) == 0)
            let chan = rd32(chanp)
            let count = tb_NewPtr(4, false, true)
            // PROCEDURE doubleBack(chan, buffer): count calls.
            let back: [UInt16] = [0x205F, 0x508F, 0x52B9, UInt16(count >> 16), UInt16(count & 0xFFFF), 0x4ED0]
            let backCode = tb_NewPtr(Int32(back.count * 2), false, false)
            for (i, w) in back.enumerated() { wr16(backCode + UInt32(i * 2), w) }
            let header = tb_NewPtr(24, false, true)
            wr16(header, 1); wr16(header + 2, 8)
            wr32(header + 8, 0x5622_0000) // 22050 Hz
            for i in 0..<2 {
                let buf = tb_NewPtr(16 + 100, false, true)
                wr32(buf, 100); wr32(buf + 4, 1) // ready
                wr32(header + 12 + UInt32(i * 4), buf)
            }
            wr32(header + 20, backCode)
            // CLR.W -(SP); PEA chan; PEA header; MOVE.L #$00200008,D0; _SoundDispatch; MOVE.W (SP)+,D7; _ExitToShell
            let words: [UInt16] = [0x4267, 0x4879, UInt16(chan >> 16), UInt16(chan & 0xFFFF),
                                   0x4879, UInt16(header >> 16), UInt16(header & 0xFFFF),
                                   0x203C, 0x0020, 0x0008, 0xA800, 0x3E1F, 0xA9F4]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            g_cpu!.pointee.pc = code
            g_cpu!.pointee.halted = false
            _ = cpu_run(g_cpu, 100_000)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            #expect(Self.played.count == 2 && Self.played.allSatisfy { $0.frames == 100 })
            #expect(rd32(count) == 2)
            _ = tb_SndDisposeChannel(chan, 1)
        }

        nonisolated(unsafe) static var firstSamples: [Int16] = []

        /// A compressed sound header in IMA 4:1 decodes to 64 samples a packet.
        @Test func ima4Decodes() {
            launchTestApp()
            Self.played = []
            Self.firstSamples = []
            machine_set_sound_handler({ _, pcm, frames, channels, rate, _, _ in
                guard let pcm else { return }
                SoundManager.played.append((frames, channels, rate))
                SoundManager.firstSamples = (0..<3).map { pcm[$0] }
            }, nil)
            defer { machine_set_sound_handler(nil, nil) }
            let chanp = tb_NewPtr(4, false, true)
            #expect(tb_SndNewChannel(chanp, 5, 0, 0) == 0)
            let h = tb_NewPtr(64 + 34, false, true)
            wr32(h + 4, 1)                          // mono
            wr32(h + 8, 0x5622_0000)                // 22050 Hz
            wr8(h + 20, 0xFE)                       // compressed
            wr32(h + 22, 1)                         // one packet
            wr32(h + 40, fourCC("ima4"))
            wr16(h + 56, 0xFFFF)                    // compressionID: use the format
            wr16(h + 62, 16)
            for k in 0..<32 { wr8(h + 64 + 2 + UInt32(k), 0x77) } // header 0: predictor 0, index 0
            let cmd = tb_NewPtr(8, false, true)
            wr16(cmd, 81); wr32(cmd + 4, h)         // bufferCmd
            #expect(tb_SndDoImmediate(rd32(chanp), cmd) == 0)
            #expect(Self.played.count == 1 && Self.played[0].frames == 64)
            // Nibble 7 each time: +11 (step 7), +30 (step 16), +63 (step 34) …
            #expect(Self.firstSamples == [11, 41, 104])
            _ = tb_SndDisposeChannel(rd32(chanp), 1)
        }

        @Test func synchronousPlayWaitsForTheSound() {
            launchTestApp()
            install()
            defer { machine_set_sound_handler(nil, nil) }
            let data = Self.snd(samples: 2225)                   // about 0.1 s
            let h = tb_NewHandle(Int32(data.count), false, false)
            for (i, b) in data.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            let start = machine_time_ns()
            #expect(tb_SndPlay(0, h, 0) == 0)
            #expect(machine_time_ns() - start >= 90_000_000)    // the machine's clock moved past it
            #expect(Self.played.count == 1)
            #expect(Self.played.first?.frames == 2225)
            #expect(abs((Self.played.first?.rate ?? 0) - 22254.5) < 1)
        }

        /// SetupSndHeader builds a 'snd ' a program fills with samples and plays.
        @Test func setupSndHeaderMakesAPlayableSound() {
            launchTestApp()
            install()
            defer { machine_set_sound_handler(nil, nil) }
            let lenp = tb_NewPtr(2, false, true)
            let rate = Int32(bitPattern: 0x56EE_8BA3)            // 22254.5 Hz
            let h = tb_NewHandle(0, false, false)                // grows to fit
            #expect(tb_SetupSndHeader(h, 1, rate, 8, Int32(bitPattern: fourCC("NONE")), 60, 1000, lenp) == 0)
            #expect(rd16(lenp) == 42)                            // a standard header
            tb_SetHandleSize(h, 42 + 1000)
            for i in 0..<1000 { wr8(rd32(h) + 42 + UInt32(i), 0x80) }
            #expect(tb_SndPlay(0, h, 0) == 0)
            #expect(Self.played.first?.frames == 1000)
            // 16-bit stereo takes an extended header; MACE a compressed one.
            #expect(tb_SetupSndHeader(h, 2, rate, 16, Int32(bitPattern: fourCC("NONE")), 60, 4000, lenp) == 0)
            #expect(rd16(lenp) == 84 && rd8(rd32(h) + 40) == 0xFF && rd32(rd32(h) + 42) == 1000)
            #expect(tb_SetupSndHeader(h, 1, rate, 8, Int32(bitPattern: fourCC("MAC3")), 60, 600, lenp) == 0)
            #expect(rd8(rd32(h) + 40) == 0xFE && rd32(rd32(h) + 42) == 300 && rd32(rd32(h) + 60) == fourCC("MAC3"))
        }

        @Test func callbacksRunAfterTheSoundAtSafePoints() {
            launchTestApp()
            install()
            defer { machine_set_sound_handler(nil, nil) }
            Self.callbacks = 0
            let proc = native_register(Self.callback, "test sound callback", 0)
            let chanp = tb_NewPtr(4, false, true)
            #expect(tb_SndNewChannel(chanp, 5, 0, proc) == 0)
            let chan = rd32(chanp)
            let data = Self.snd(samples: 4450)                   // about 0.2 s
            let h = tb_NewHandle(Int32(data.count), false, false)
            for (i, b) in data.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            #expect(tb_SndPlay(chan, h, 1) == 0)
            let cmd = tb_NewPtr(8, false, true)
            wr16(cmd, 13)                                        // callBackCmd
            #expect(tb_SndDoCommand(chan, cmd, 0) == 0)
            _ = tb_TickCount()
            #expect(Self.callbacks == 0)                         // still playing
            machine_skip_time(250_000_000)                       // headless: the clock follows instructions
            _ = tb_TickCount()                                   // a safe point
            #expect(Self.callbacks == 1)
            #expect(tb_SndDisposeChannel(chan, 1) == 0)
        }

        /// The Sound Driver: StartSound's asynchronous free-form write stays
        /// in progress while it plays, then completes (StuntCopter).
        @Test func soundDriverPlaysFreeFormAsynchronously() {
            launchTestApp()
            let rec = tb_NewPtr(6 + 2225, false, true) // a tenth of a second
            wr16(rec, 0)                               // ffMode
            wr32(rec + 2, 0x0001_0000)                 // the Mac's rate
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -4))
            wr32(pb + UInt32(IOParam_ioBuffer_off), rec)
            wr32(pb + UInt32(IOParam_ioReqCount_off), 6 + 2225)
            #expect(driver_call(pb, true, Int32(DRIVER_WRITE)) == 1)
            sound_pump()
            #expect(rd16(pb + UInt32(IOParam_ioResult_off)) == 1)
            machine_skip_time(200_000_000)
            sound_pump()
            #expect(rd16(pb + UInt32(IOParam_ioResult_off)) == 0)
        }
    }

    /// The Sound Input Manager: one device, recording what the host
    /// captures, and silence without it.
    @Suite struct SoundInput {
        /// An SPB recording `count` bytes into a buffer of its own.
        func spb(_ refnum: UInt32, count: UInt32) -> (pb: UInt32, buffer: UInt32) {
            let pb = tb_NewPtr(38, false, true), buffer = tb_NewPtr(Int32(count), false, true)
            wr32(pb, refnum)
            wr32(pb + 4, count)
            wr32(pb + 12, count)
            wr32(pb + 16, buffer)
            return (pb, buffer)
        }

        func open() -> UInt32 {
            let r = tb_NewPtr(4, false, true)
            #expect(tb_SPBOpenDevice(0, 1, r) == 0)
            #expect(rd32(r) != 0)
            return rd32(r)
        }

        @Test func theDeviceAndItsSettings() {
            launchTestApp()
            var answer: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("snd ")), &answer) == 0 && answer & 0x20 != 0)
            let name = tb_NewPtr(256, false, true), icon = tb_NewPtr(4, false, true)
            #expect(tb_SPBGetIndexedDevice(1, name, icon) == 0 && pstring(at: name) == "Built-in" && rd32(icon) != 0)
            #expect(tb_SPBGetIndexedDevice(2, name, icon) == -220)
            let refnum = open()
            let info = tb_NewPtr(16, false, true)
            #expect(tb_SPBGetDeviceInfo(Int32(bitPattern: refnum), Int32(bitPattern: fourCC("srat")), info) == 0)
            #expect(rd32(info) == 0x56EE_8BA3)
            wr16(info, 16)
            #expect(tb_SPBSetDeviceInfo(Int32(bitPattern: refnum), Int32(bitPattern: fourCC("ssiz")), info) == 0)
            wr16(info, 12)
            #expect(tb_SPBSetDeviceInfo(Int32(bitPattern: refnum), Int32(bitPattern: fourCC("ssiz")), info) == -226)
            wr32(info, 1000)
            #expect(tb_SPBMillisecondsToBytes(Int32(bitPattern: refnum), info) == 0 && rd32(info) == 22254 * 2)
            #expect(tb_SPBGetDeviceInfo(Int32(bitPattern: refnum), Int32(bitPattern: fourCC("what")), info) == -231)
            #expect(tb_SPBCloseDevice(Int32(bitPattern: refnum)) == 0)
            #expect(tb_SPBCloseDevice(Int32(bitPattern: refnum)) == -229)
        }

        /// Without a microphone, a synchronous recording is silence, as long as asked.
        @Test func recordingWithoutAMicrophoneIsSilence() {
            launchTestApp()
            let refnum = open()
            let (pb, buffer) = spb(refnum, count: 2000)
            wr8(buffer + 1999, 0x12)
            #expect(tb_SPBRecord(pb, 0) == 0)
            #expect(rd32(pb + 4) == 2000 && rd32(pb + 8) == 90 && rd16(pb + 32) == 0)
            #expect((0..<2000).allSatisfy { rd8(buffer + UInt32($0)) == 0x80 })
            #expect(tb_SPBCloseDevice(Int32(bitPattern: refnum)) == 0)
        }

        /// What the host captures, resampled to the device's rate, fills an
        /// asynchronous recording, which then completes.
        @Test func asynchronousRecordingTakesTheHostsSamples() {
            launchTestApp()
            machine_set_sound_input_handler({ _, _ in true }, nil)
            defer { machine_set_sound_input_handler(nil, nil) }
            let refnum = open()
            let (pb, buffer) = spb(refnum, count: 1000)
            #expect(tb_SPBRecord(pb, 1) == 0)
            #expect(rd16(pb + 32) == 1) // in progress
            let half = [Float](repeating: 0.5, count: 2000) // 2000 frames at twice the rate
            machine_sound_input(half, 2000, 1, 22254.545454 * 2)
            machine_skip_time(100_000_000)
            sound_pump()
            #expect(rd16(pb + 32) == 0 && rd32(pb + 4) == 1000)
            #expect((4..<1000).allSatisfy { rd8(buffer + UInt32($0)) == 0xC0 })
            let status = tb_NewPtr(2, false, true)
            #expect(tb_SPBGetRecordingStatus(Int32(bitPattern: refnum), status, 0, 0, 0, 0, 0) == 0 && rd16(status) == 0)
            #expect(tb_SPBCloseDevice(Int32(bitPattern: refnum)) == 0)
        }

        /// SndRecord's dialog: Escape cancels it.
        @Test func sndRecordCancels() {
            launchTestApp()
            _ = initToolbox()
            machine_post_key(27, 0x35, 0, 1, 0)
            let h = tb_NewPtr(4, false, true)
            #expect(tb_SndRecord(0, 0, Int32(bitPattern: fourCC("best")), h) == -128)
            #expect(rd32(h) == 0)
        }
    }
}

extension BlueBox {
    @Suite struct StringsAndScripts {
        func compare(_ a: String, _ b: String, trap: UInt16, _ fn: (UnsafeMutablePointer<cpu68k>?, UInt16) -> Void) -> Int32 {
            let pa = tb_NewPtr(64, false, true), pb = tb_NewPtr(64, false, true)
            for (i, c) in a.utf8.enumerated() { wr8(pa + UInt32(i), c) }
            for (i, c) in b.utf8.enumerated() { wr8(pb + UInt32(i), c) }
            let c = g_cpu!
            c.pointee.a.0 = pa
            c.pointee.a.1 = pb
            c.pointee.d.0 = UInt32(a.utf8.count) << 16 | UInt32(b.utf8.count)
            fn(c, trap)
            return Int32(bitPattern: c.pointee.d.0)
        }

        @Test func cmpStringHonoursCaseFlag() {
            launchTestApp()
            #expect(compare("Hello", "hello", trap: 0xA03C, raw_CmpString) == 0)  // case-insensitive
            #expect(compare("Hello", "hello", trap: 0xA43C, raw_CmpString) == 1)  // case-sensitive
            #expect(compare("Hello", "Help", trap: 0xA03C, raw_CmpString) == 1)
        }

        @Test func relStringOrders() {
            launchTestApp()
            #expect(compare("apple", "banana", trap: 0xA050, raw_RelString) == -1)
            #expect(compare("pear", "pea", trap: 0xA050, raw_RelString) == 1)
            #expect(compare("Same", "same", trap: 0xA050, raw_RelString) == 0)
        }

        @Test func scriptManagerReportsRoman() {
            launchTestApp()
            #expect(tb_GetScriptManagerVariable(0) == 0x0710)
            #expect(tb_FontScript() == 0)
            #expect(tb_GetScriptVariable(0, 86) == (3 << 16 | 12))
            let t = tb_NewPtr(32, false, true)
            for (i, c) in "Aa1 .".utf8.enumerated() { wr8(t + UInt32(i), c) }
            #expect(tb_CharType(t, 0) == 0x4001)   // upper-case ASCII letter
            #expect(tb_CharType(t, 1) == 0x0001)
            #expect(tb_CharType(t, 2) == 0x0100)   // digit
            #expect(tb_CharType(t, 3) == 0x0300)   // blank
        }

        @Test func findWordFindsTheWordAroundAnOffset() {
            launchTestApp()
            let t = tb_NewPtr(32, false, true)
            let text = "hello brave world"
            for (i, c) in text.utf8.enumerated() { wr8(t + UInt32(i), c) }
            let offs = tb_NewPtr(12, false, true)
            tb_FindWord(t, Int16(text.utf8.count), 8, 1, 0, offs)
            #expect(rd16(offs) == 6 && rd16(offs + 2) == 11)
        }
    }
}

extension BlueBox {
    @Suite struct NotificationManager {
        nonisolated(unsafe) static var posted: [(Int32, String, Int32)] = []
        nonisolated(unsafe) static var responses = 0

        static let response: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)
            NotificationManager.responses += 1
            cpu_push32(c, ret)
        }

        func request(text: String?, resp: UInt32) -> UInt32 {
            let nm = tb_NewPtr(Int32(NMRec_SIZEOF), false, true)
            wr16(nm + UInt32(NMRec_qType_off), 8)
            wr16(nm + UInt32(NMRec_nmMark_off), 1)
            if let text {
                let s = tb_NewPtr(256, false, true)
                guestPString(text, at: s)
                wr32(nm + UInt32(NMRec_nmStr_off), s)
            }
            wr32(nm + UInt32(NMRec_nmResp_off), resp)
            return nm
        }

        @Test func requestsReachTheHostAndResponsesRun() {
            launchTestApp()
            Self.posted = []
            Self.responses = 0
            machine_set_notification_handler({ id, text, flags, _ in
                NotificationManager.posted.append((id, text.map { String(cString: $0) } ?? "", flags))
            }, nil)
            defer { machine_set_notification_handler(nil, nil) }
            let proc = native_register(Self.response, "test nm response", 0)
            let nm = request(text: "Backup finished", resp: proc)
            #expect(tb_NMInstall(nm) == 0)
            #expect(Self.posted.count == 1)
            #expect(Self.posted.first?.1 == "Backup finished")
            #expect(Self.posted.first?.2 == Int32(MACHINE_NOTIFY_MARK))
            let evt = tb_NewPtr(16, false, true)
            _ = tb_WaitNextEvent(-1, evt, 0, 0)
            #expect(Self.responses == 1)
            #expect(tb_NMRemove(nm) == 0)
            #expect(Self.posted.last?.0 == -1)                 // withdrawn
        }

        @Test func automaticRemoval() {
            launchTestApp()
            Self.posted = []
            machine_set_notification_handler({ id, _, _, _ in NotificationManager.posted.append((id, "", 0)) }, nil)
            defer { machine_set_notification_handler(nil, nil) }
            let nm = request(text: nil, resp: 0xFFFF_FFFF)
            #expect(tb_NMInstall(nm) == 0)
            let evt = tb_NewPtr(16, false, true)
            _ = tb_WaitNextEvent(-1, evt, 0, 0)
            #expect(Self.posted.map(\.0) == [1, -1])
            #expect(tb_NMRemove(nm) != 0)                      // already gone
        }
    }
}

extension BlueBox {
    @Suite struct QuickTimeStart {
        /// QuickTime is there; a movie that cannot be opened is reported, and
        /// asking about no movie says it is done, so applications carry on.
        @Test func moviesOpenOrFailCleanly() {
            launchTestApp()
            var v: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("qtim")), &v) == 0 && v >> 16 >= 0x0250)
            #expect(tb_EnterMovies() == 0)
            let mp = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromFile(mp, 0, 0, 0, 0, 0) != 0)
            #expect(rd32(mp) == 0)
            #expect(tb_IsMovieDone(0) != 0)
        }
    }
}

extension BlueBox {
    /// The old Sound Driver (.Sound, reference number -4).
    @Suite struct SoundDriver {
        /// KillIO runs the completion of a write that has finished but whose
        /// completion has not run yet, before it returns: The Colony disposes
        /// of the parameter block straight after and reuses the memory.
        @Test func killIORunsWaitingCompletions() {
            launchTestApp()
            let flag = tb_NewPtr(2, false, true)
            let words: [UInt16] = [0x33FC, 0x0001, UInt16(flag >> 16), UInt16(flag & 0xFFFF), 0x4E75] // MOVE.W #1,flag; RTS
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let rec = tb_NewPtr(6 + 64, false, true)     // free-form: mode 0, rate, 64 samples
            wr32(rec + 2, 0x0001_0000)
            for i in 0..<64 { wr8(rec + 6 + UInt32(i), 128) }
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -4))
            wr32(pb + 12, code)                          // ioCompletion
            wr32(pb + UInt32(IOParam_ioBuffer_off), rec)
            wr32(pb + UInt32(IOParam_ioReqCount_off), 70)
            _ = tb_PBWrite(pb, true)
            machine_skip_time(1_000_000_000)              // the sound plays out
            sound_pump()
            #expect(rd16(flag) == 0)                     // its completion waits for interrupt time
            let kill = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(kill + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -4))
            _ = tb_PBKillIO(kill, false)
            #expect(rd16(flag) == 1)                     // ran inside KillIO
        }
    }
}
