import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Hibernating with sound (hibernate milestone M4): a snapshot taken
    /// while channels play, a tune plays, a voice speaks or a recording
    /// runs, put back as a resumed process would.  The channels' queues,
    /// callbacks and double buffers go on by the machine's clock; the host,
    /// new, is given what its old self had queued and not played, and its
    /// synthesizers what they had been told; an utterance is over; a
    /// recording stops.
    @Suite(.serialized) struct HibernateSound {
        nonisolated(unsafe) static var played: [(id: Int32, frames: Int32)] = []
        nonisolated(unsafe) static var stops: [Int32] = []

        func installSound() {
            Self.played = []
            Self.stops = []
            machine_set_sound_handler({ id, pcm, frames, _, _, _, _ in
                if pcm != nil { HibernateSound.played.append((id, frames)) } else { HibernateSound.stops.append(id) }
            }, nil)
        }

        /// The program's event loop (_SystemTask; BRA.S), with a stack.
        func startLoop() {
            let code = tb_NewPtr(4, false, false)
            wr16(code, 0xA9B4)
            wr16(code + 2, 0x60FC)
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000
            cpu.pointee.pc = code
            cpu.pointee.halted = false
        }

        /// Stopped at the loop's SystemTask and saved; the machine then goes on.
        func hibernate(_ dir: URL, _ name: String) -> String {
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            let snap = dir.appendingPathComponent(name).path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            return snap
        }

        func resume(_ snap: String) {
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
        }

        func sound(_ samples: Int) -> UInt32 {
            let data = SoundManager.snd(samples: samples)
            let h = tb_NewHandle(Int32(data.count), false, false)
            for (i, b) in data.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            return h
        }

        /// A channel mid-queue: a second's sound playing, a callBackCmd and
        /// another sound waiting.  Resumed, the host is given the rest of the
        /// first (what its old self had not played), and the callback and
        /// the second sound come when the first ends, as they did in the
        /// process that hibernated.
        @Test func aChannelMidQueueGoesOn() {
            let dir = launchTestApp()
            startLoop()
            installSound()
            defer { machine_set_sound_handler(nil, nil) }
            SoundManager.callbacks = 0
            let proc = native_register(SoundManager.callback, "hibernate sound callback", 0)
            let chanp = tb_NewPtr(4, false, true)
            #expect(tb_SndNewChannel(chanp, 5, 0, proc) == 0)
            let chan = rd32(chanp)
            #expect(tb_SndPlay(chan, sound(22254), 1) == 0)          // a second
            let cmd = tb_NewPtr(8, false, true)
            wr16(cmd, 13)                                            // callBackCmd
            #expect(tb_SndDoCommand(chan, cmd, 0) == 0)
            #expect(tb_SndPlay(chan, sound(4450), 1) == 0)           // a fifth of a second, queued
            #expect(Self.played.count == 1 && Self.played[0].frames == 22254)
            let id = Self.played[0].id
            machine_skip_time(400_000_000)
            _ = tb_TickCount()
            #expect(SoundManager.callbacks == 0)

            let snap = hibernate(dir, "queue.bbhib")
            // The process that hibernated goes on.
            machine_skip_time(700_000_000)
            _ = tb_TickCount()
            #expect(SoundManager.callbacks == 1)
            #expect(Self.played.count == 2 && Self.played[1].frames == 4450)

            resume(snap)
            Self.played = []
            SoundManager.callbacks = 0
            sound_pump()                                             // the first safe point
            #expect(Self.played.count == 1)
            if let first = Self.played.first {
                #expect(first.id == id)
                #expect(abs(Int(first.frames) - 22254 * 6 / 10) < 400) // the 0.6 s not yet played
            }
            #expect(SoundManager.callbacks == 0)
            machine_skip_time(700_000_000)
            _ = tb_TickCount()
            #expect(SoundManager.callbacks == 1)
            #expect(Self.played.count == 2 && Self.played.last?.frames == 4450)
            #expect(tb_SndDisposeChannel(chan, 1) == 0)
        }

        /// doubleBack(chan, buffer): counts its calls in guest memory and
        /// makes the buffer ready again with a tenth of a second.
        nonisolated(unsafe) static var backCount: UInt32 = 0
        static let refill: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let buf = cpu_pop32(c)
            _ = cpu_pop32(c)
            wr32(buf, 2205)                                          // dbNumFrames
            wr32(buf + 4, 1)                                         // dbBufferReady
            wr32(HibernateSound.backCount, rd32(HibernateSound.backCount) + 1)
            cpu_push32(c, ret)
        }

        /// Double buffering goes on after resuming: the host is given what
        /// was queued, then the buffers play and doubleBack refills them in
        /// turn, its count (the program's memory) going on from the
        /// snapshot's.
        @Test func doubleBufferingGoesOn() {
            let dir = launchTestApp()
            startLoop()
            installSound()
            defer { machine_set_sound_handler(nil, nil) }
            Self.backCount = tb_NewPtr(4, false, true)
            let chanp = tb_NewPtr(4, false, true)
            #expect(tb_SndNewChannel(chanp, 5, 0, 0) == 0)
            let chan = rd32(chanp)
            let header = tb_NewPtr(24, false, true)
            wr16(header, 1); wr16(header + 2, 8)
            wr32(header + 8, 0x5622_0000)                            // 22050 Hz
            for i in 0..<2 {
                let buf = tb_NewPtr(16 + 2205, false, true)
                wr32(buf, 2205); wr32(buf + 4, 1)
                wr32(header + 12 + UInt32(i * 4), buf)
            }
            wr32(header + 20, native_register(Self.refill, "hibernate doubleBack", 0))
            #expect(tb_SndPlayDoubleBuffer(chan, header) == 0)
            for _ in 0..<5 {
                machine_skip_time(50_000_000)
                sound_pump()
            }
            let calls = rd32(Self.backCount)
            #expect(calls >= 3)

            let snap = hibernate(dir, "double.bbhib")
            resume(snap)
            Self.played = []
            #expect(rd32(Self.backCount) == calls)
            sound_pump()
            // What the old host had queued: no more than the lead the
            // machine keeps (a buffer, and the one playing).
            let replayed = Self.played.reduce(0) { $0 + Int($1.frames) }
            #expect(replayed > 0 && replayed <= 2 * 2205 + 10)
            for _ in 0..<10 {
                machine_skip_time(50_000_000)
                sound_pump()
            }
            #expect(rd32(Self.backCount) >= calls + 4)                // half a second: about five more
            #expect(Self.played.dropFirst().contains { $0.frames == 2205 })
            #expect(tb_SndDisposeChannel(chan, 1) == 0)
        }

        /// A tune playing: the resumed process's synthesizer is told the
        /// part's instrument and the note held, and the tune goes on from
        /// where it was, its next note and its callback on time.
        @Test func aTuneGoesOnWithItsInstrumentAndHeldNote() {
            let dir = launchTestApp()
            startLoop()
            let tune = QTTune()
            QTTune.midi = []
            QTTune.callbacks = []
            machine_set_midi_handler({ id, events, n, _ in
                QTTune.midi.append((id, (0..<Int(n)).compactMap { events?[$0] }))
            }, nil)
            defer { machine_set_midi_handler(nil, nil) }
            let tp = tune.openDefault("tune")
            #expect(tp != 0)
            #expect(tune.component(tp, 4, [tune.header(gm: 41)]) == 0)            // a violin: program 40
            // Part 1: pitch 60 for two seconds, a second's rest, pitch 64, a second's rest, the end.
            func note(_ pitch: UInt32, _ duration: UInt32) -> [UInt8] {
                be32(0x2000_0000 | 1 << 24 | (pitch - 32) << 18 | 100 << 11 | duration)
            }
            let seq = tune.guestBytes(cat(note(60, 1200), be32(600), note(64, 300), be32(600), be32(0x6000_0000)))
            let cb = native_register(QTTune.callback, "hibernate tune callback", 0)
            #expect(tune.component(tp, 10, [seq, 0x10000, 0, 0x7FFF_FFFF, 0, cb, 9]) == 0)
            let id: Int32 = 0x5455_0000
            let first = tune.sent(id)
            guard let on = first.first(where: { $0.status & 0xF0 == 0x90 && $0.data1 == 60 }) else {
                Issue.record("no note on")
                return
            }
            let ch = on.status & 0x0F
            #expect(first.contains { $0.status == 0xC0 | ch && $0.data1 == 40 })
            machine_skip_time(500_000_000)
            sound_pump()
            #expect(!tune.sent(id).contains { $0.data1 == 64 })

            let snap = hibernate(dir, "tune.bbhib")
            resume(snap)
            QTTune.midi = []
            sound_pump()
            let told = tune.sent(id)
            let program = told.firstIndex { $0.status == 0xC0 | ch && $0.data1 == 40 }
            let held = told.firstIndex { $0.status == 0x90 | ch && $0.data1 == 60 && $0.data2 == 100 }
            #expect(program != nil && held != nil)
            if let program, let held { #expect(program < held) }
            #expect(!told.contains { $0.data1 == 64 })                            // not yet: at a second
            #expect(QTTune.callbacks.isEmpty)
            machine_skip_time(800_000_000)
            sound_pump()
            #expect(tune.sent(id).contains { $0.status == 0x90 | ch && $0.data1 == 64 })
            machine_skip_time(1_000_000_000)
            sound_pump()
            #expect(tune.sent(id).contains { $0.status == 0x80 | ch && $0.data1 == 60 })
            #expect(QTTune.callbacks == [9])
        }

        /// The note allocator's channels: their instruments are told again.
        @Test func noteChannelsKeepTheirInstruments() {
            let dir = launchTestApp()
            startLoop()
            let tune = QTTune()
            QTTune.midi = []
            machine_set_midi_handler({ id, events, n, _ in
                QTTune.midi.append((id, (0..<Int(n)).compactMap { events?[$0] }))
            }, nil)
            defer { machine_set_midi_handler(nil, nil) }
            let na = tune.openDefault("nota")
            #expect(na != 0)
            let request = tune.guestBytes(tune.noteRequest(gm: 57))                // a trumpet: program 56
            let ncp = tb_NewPtr(4, false, true)
            #expect(tune.component(na, 4, [request, ncp]) == 0)                   // NANewNoteChannel
            let nc = rd32(ncp)
            #expect(tune.component(na, 13, [nc, 67, 90]) == 0)                    // NAPlayNote, held
            let id: Int32 = 0x4E41_0000
            let ch = tune.sent(id).first { $0.status & 0xF0 == 0x90 }.map { $0.status & 0x0F } ?? 0xFF

            let snap = hibernate(dir, "nota.bbhib")
            resume(snap)
            QTTune.midi = []
            sound_pump()
            let told = tune.sent(id)
            #expect(told.contains { $0.status == 0xC0 | ch && $0.data1 == 56 })
            #expect(told.contains { $0.status == 0x90 | ch && $0.data1 == 67 && $0.data2 == 90 })
        }

        /// The MIDI Manager: the host synthesizer its host port plays is told
        /// the program and the held note again, and a packet time-stamped
        /// for later reaches it when due, after resuming.
        @Test func midiManagerPacketsGoOn() {
            let dir = launchTestApp()
            let mm = MIDIManager()
            mm.install()
            defer { machine_set_midi_handler(nil, nil) }
            startLoop()
            mm.signIn("play", "Player")
            let out = mm.addPort("play", "out ", type: 2)
            #expect(tb_MIDIWritePacket(out, mm.packet([0xC0, 41])) == 0)
            #expect(tb_MIDIWritePacket(out, mm.packet([0x90, 60, 100])) == 0)
            let now = UInt32(machine_time_ns() / 1_000_000)
            #expect(tb_MIDIWritePacket(out, mm.packet([0x90, 64, 90], stamp: now + 500, now: false)) == 0)
            #expect(!MIDIManager.synth.contains { $0.data1 == 64 })

            let snap = hibernate(dir, "midi.bbhib")
            resume(snap)
            MIDIManager.synth = []
            sound_pump()
            let told = MIDIManager.synth.map { [$0.status, $0.data1, $0.data2] }
            #expect(told == [[0xC0, 41, 0], [0x90, 60, 100]])
            machine_skip_time(600_000_000)
            time_pump()
            #expect(MIDIManager.synth.last.map { [$0.status, $0.data1, $0.data2] } == [0x90, 64, 90])
        }

        /// A voice speaking: hibernating is not refused; resumed, the
        /// utterance is over at the next safe point and the program's
        /// SpeechDone comes.  One the program had paused stays paused, and
        /// ends when it is continued.
        @Test func anUtteranceEndsWithItsCallback() {
            let dir = launchTestApp()
            _ = initToolbox()
            startLoop()
            machine_set_speech_handler({ _, _ in }, nil)   // a host that never says it is done
            defer { machine_set_speech_handler(nil, nil) }
            Speech.done = []
            let proc = native_register(Speech.doneProc, "hibernate speech done", 0)
            var chans: [UInt32] = []
            for refcon: UInt32 in [11, 22] {
                let chp = tb_NewPtr(4, false, true)
                #expect(tb_NewSpeechChannel(0, chp) == 0)
                let ch = rd32(chp)
                #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x7265_6663), refcon) == 0) // 'refc'
                #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x646E_6362), proc) == 0)   // 'dncb'
                let text = tb_NewPtr(64, false, true)
                for (i, b) in "A long sentence the voice is saying.".utf8.enumerated() { wr8(text + UInt32(i), b) }
                #expect(tb_SpeakText(ch, text, 36) == 0)
                chans.append(ch)
            }
            #expect(tb_PauseSpeechAt(chans[1], 0) == 0)
            #expect(tb_SpeechBusy() == 2)

            let snap = hibernate(dir, "speech.bbhib")
            #expect(tb_SpeechBusy() == 2)                                          // the old host still speaks
            resume(snap)
            Speech.done = []
            #expect(tb_SpeechBusy() == 1)                                          // the paused one is left
            #expect(Speech.done == [11])
            #expect(tb_ContinueSpeech(chans[1]) == 0)
            #expect(tb_SpeechBusy() == 0)
            #expect(Speech.done == [11, 22])
            for ch in chans { #expect(tb_DisposeSpeechChannel(ch) == 0) }
        }

        /// The program's code from `words`, run from the start with a stack.
        func program(_ words: [UInt16]) -> UInt32 {
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            cpu.pointee.d.7 = 0x1234
            return code
        }

        /// A synchronous SndPlay waiting for its sound gives its call up for
        /// the drain; made again (going on, or resumed) it only waits, and
        /// the sound plays once.
        @Test func aSynchronousPlayGivesUpForTheDrain() {
            let dir = launchTestApp()
            installSound()
            defer { machine_set_sound_handler(nil, nil) }
            let h = sound(4450)
            // CLR.W -(SP); CLR.L -(SP); MOVE.L #h,-(SP); CLR.B -(SP); _SndPlay; MOVE.W (SP)+,D7; loop: _SystemTask; BRA.S loop
            let code = program([0x4267, 0x42A7, 0x2F3C, UInt16(h >> 16), UInt16(h & 0xFFFF), 0x4227, 0xA805, 0x3E1F,
                                0xA9B4, 0x60FC])
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_pc() == code + 12)                                     // on _SndPlay
            #expect(String(cString: machine_drain_point()).hasSuffix("(abandoned)"))
            #expect(Self.played.count == 1)
            let snap = dir.appendingPathComponent("sync.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            _ = machine_run(100_000)                                             // going on
            #expect(Self.played.count == 1)
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)

            resume(snap)
            Self.played = []
            #expect(g_cpu!.pointee.d.7 == 0x1234)
            _ = machine_run(100_000)
            #expect(Self.played.count == 1 && Self.played.first?.frames == 4450, Comment(rawValue: "\(Self.played)"))  // the old host's, given again
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
        }

        /// A synchronous recording gives its call up for the drain too; made
        /// again going on it records to the end, and resumed it has stopped:
        /// abortErr, without asking the host for the microphone again.
        @Test func aSynchronousRecordingGivesUpForTheDrain() {
            let dir = launchTestApp()
            Self.captures = []
            machine_set_sound_input_handler({ on, _ in HibernateSound.captures.append(on); return true }, nil)
            defer { machine_set_sound_input_handler(nil, nil) }
            let r = tb_NewPtr(4, false, true)
            #expect(tb_SPBOpenDevice(0, 1, r) == 0)
            let pb = tb_NewPtr(38, false, true), buffer = tb_NewPtr(4450, false, true)
            wr32(pb, rd32(r))
            wr32(pb + 4, 4450)
            wr32(pb + 12, 4450)
            wr32(pb + 16, buffer)
            // CLR.W -(SP); MOVE.L #pb,-(SP); CLR.B -(SP); MOVE.L #$03200014,D0; _SoundDispatch (SPBRecord);
            // MOVE.W (SP)+,D7; loop: _SystemTask; BRA.S loop
            _ = program([0x4267, 0x2F3C, UInt16(pb >> 16), UInt16(pb & 0xFFFF), 0x4227, 0x203C, 0x0320, 0x0014,
                         0xA800, 0x3E1F, 0xA9B4, 0x60FC])
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(String(cString: machine_drain_point()).hasSuffix("(abandoned)"))
            let snap = dir.appendingPathComponent("syncrec.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            _ = machine_run(100_000)                                             // going on: to the end
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd32(pb + 4) == 4450)

            resume(snap)
            Self.captures = []
            _ = machine_run(100_000)
            #expect(Int16(bitPattern: UInt16(g_cpu!.pointee.d.7 & 0xFFFF)) == -27) // abortErr
            #expect(rd32(pb + 4) < 4450)
            #expect(Self.captures.isEmpty)
            #expect(tb_SPBCloseDevice(Int32(bitPattern: rd32(r))) == 0)
        }

        nonisolated(unsafe) static var captures: [Bool] = []
        nonisolated(unsafe) static var completions = 0
        static let recorded: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)                                                       // the SPB
            HibernateSound.completions += 1
            cpu_push32(c, ret)
        }

        /// A recording going on: hibernating is not refused; resumed, the
        /// recording stops as SPBStopRecording stops one (abortErr, the
        /// bytes recorded before), the microphone is not opened again, and
        /// it is when the program records again.
        @Test func aRecordingStops() {
            let dir = launchTestApp()
            startLoop()
            Self.captures = []
            Self.completions = 0
            machine_set_sound_input_handler({ on, _ in HibernateSound.captures.append(on); return true }, nil)
            defer { machine_set_sound_input_handler(nil, nil) }
            let r = tb_NewPtr(4, false, true)
            #expect(tb_SPBOpenDevice(0, 1, r) == 0)
            let refnum = rd32(r)
            let pb = tb_NewPtr(38, false, true), buffer = tb_NewPtr(44508, false, true)
            wr32(pb, refnum)
            wr32(pb + 4, 44508)                                                    // two seconds
            wr32(pb + 12, 44508)
            wr32(pb + 16, buffer)
            wr32(pb + 20, native_register(Self.recorded, "hibernate recorded", 0))
            #expect(tb_SPBRecord(pb, 1) == 0)
            machine_skip_time(1_000_000_000)   // the host gives nothing: silence for what is half a second late
            sound_pump()
            #expect(rd16(pb + 32) == 1)                                            // in progress

            let snap = hibernate(dir, "record.bbhib")
            resume(snap)
            Self.captures = []
            sound_pump()
            #expect(Self.completions == 1)
            #expect(Int16(bitPattern: rd16(pb + 32)) == -27)                      // abortErr
            let count = rd32(pb + 4)
            #expect(count > 5000 && count < 44508)                                // half a second's, about
            #expect(Self.captures.isEmpty)
            wr32(pb + 4, 1000)
            wr32(pb + 20, 0)
            #expect(tb_SPBRecord(pb, 1) == 0)
            #expect(Self.captures == [true])                                       // asked again for the new one
            #expect(tb_SPBCloseDevice(Int32(bitPattern: refnum)) == 0)
        }
    }
}
