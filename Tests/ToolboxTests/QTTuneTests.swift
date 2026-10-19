import CCore
import CPU68K
import Testing
import Toolbox

/// QuickTime's tune player and note allocator, called as a program would:
/// through _ComponentDispatch glue in guest memory, with the MIDI they send
/// the host recorded.
extension BlueBox {
    @Suite struct QTTune {
        nonisolated(unsafe) static var midi: [(id: Int32, events: [machine_midi_event])] = []
        nonisolated(unsafe) static var callbacks: [Int32] = []

        /// TuneCallBack(status, refCon), pascal: records the refCon.
        static let callback: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            QTTune.callbacks.append(Int32(bitPattern: cpu_pop32(c)))
            _ = cpu_pop32(c)                                      // status
            cpu_push32(c, ret)
        }

        func install() {
            Self.midi = []
            Self.callbacks = []
            machine_set_midi_handler({ id, events, n, _ in
                QTTune.midi.append((id, (0..<Int(n)).compactMap { events?[$0] }))
            }, nil)
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            g_cpu!.pointee.halted = false                          // the test application never ran
        }

        /// Run 68k words as a subroutine, after pushing a result and the arguments; the result.
        func call(_ words: [UInt16], _ args: [UInt32]) -> UInt32 {
            // The return address is kept aside while the glue runs, the arguments under it.
            let code = tb_NewPtr(Int32(words.count * 2 + 18), false, true)
            let save = code + UInt32(words.count * 2 + 14)
            let hi = UInt16(save >> 16), lo = UInt16(save & 0xFFFF)
            for (i, w) in ([0x23DF, hi, lo] + words + [0x2F39, hi, lo, 0x4E75]).enumerated() {
                wr16(code + UInt32(i * 2), w)
            }
            let c = g_cpu!
            cpu_push32(c, 0)
            for a in args { cpu_push32(c, a) }
            guest_call(code)
            return cpu_pop32(c)
        }

        func openDefault(_ type: String) -> UInt32 {
            call([0x7021, 0xA82A], [fourCC(type), 0])           // OpenDefaultComponent
        }

        /// A component call: the instance, then the (long) arguments.
        func component(_ ci: UInt32, _ what: UInt16, _ args: [UInt32]) -> Int32 {
            let size = UInt16(args.count * 4)
            return Int32(bitPattern: call([0x2F3C, size, what, 0x7000, 0xA82A], [ci] + args))
        }

        func guestBytes(_ bytes: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(bytes.count), false, true)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        /// The NoteRequest of a general event, or alone: info, then the tone with its GM number.
        func noteRequest(gm: UInt32) -> [UInt8] {
            var nr = [UInt8](repeating: 0, count: 84)
            nr.replaceSubrange(80..<84, with: be32(gm))
            return nr
        }

        func header(gm: UInt32) -> UInt32 {
            guestBytes(cat(be32(0xF000_0000 | 1 << 16 | 23), noteRequest(gm: gm), be32(0xC000_0000 | 1 << 16 | 23),
                           be32(0x6000_0000)))
        }

        /// Part 1's note, a second's rest at 600 a second, the end.
        func sequence(pitch: UInt32) -> UInt32 {
            guestBytes(cat(be32(0x2000_0000 | 1 << 24 | (pitch - 32) << 18 | 100 << 11 | 300), be32(600),
                           be32(0x6000_0000)))
        }

        func sent(_ id: Int32) -> [machine_midi_event] {
            Self.midi.filter { $0.id == id }.flatMap { $0.events }
        }

        @Test func tunePlaysQueuedSequenceAndCallsBack() {
            bootMachine()
            install()
            defer { machine_set_midi_handler(nil, nil) }
            let tp = openDefault("tune")
            #expect(tp != 0)
            #expect(component(tp, 4, [header(gm: 1)]) == 0)       // TuneSetHeader: a piano
            let cb = native_register(Self.callback, "tune callback", 0)
            // TuneQueue(tune, rate 1.0, start 0, stop all, flags 0, callback, refCon 7)
            #expect(component(tp, 10, [sequence(pitch: 60), 0x10000, 0, 0x7FFF_FFFF, 0, cb, 7]) == 0)
            let id: Int32 = 0x5455_0000
            var events = sent(id)
            #expect(events.contains { $0.status == 0xC0 && $0.data1 == 0 })       // program: GM 1
            #expect(events.contains { $0.status == 0x90 && $0.data1 == 60 && $0.data2 == 100 && $0.delay_ms == 0 })
            #expect(!events.contains { $0.status == 0x80 })                     // the note off is half a second on

            let status = tb_NewPtr(28, false, true)
            #expect(component(tp, 12, [status]) == 0)             // TuneGetStatus
            #expect(rd16(status + 12) == 1)                        // queueCount

            machine_skip_time(600_000_000)
            qt_tune_pump()
            events = sent(id)
            #expect(events.contains { $0.status == 0x80 && $0.data1 == 60 })
            #expect(Self.callbacks.isEmpty)
            machine_skip_time(600_000_000)                         // past the end
            qt_tune_pump()
            #expect(Self.callbacks == [7])
            #expect(component(tp, 12, [status]) == 0)
            #expect(rd16(status + 12) == 0 && rd32(status) == 0)

            // TuneStop silences what is playing.
            #expect(component(tp, 10, [sequence(pitch: 64), 0x10000, 0, 0x7FFF_FFFF, 0, 0, 0]) == 0)
            #expect(component(tp, 13, [0]) == 0)
            #expect(Self.midi.last?.id == id && Self.midi.last?.events.isEmpty == true)
            #expect(component(tp, 12, [status]) == 0)
            #expect(rd16(status + 12) == 0)
            #expect(call([0x7008, 0xA82A], [tp]) >> 16 == 0)      // CloseComponent: an OSErr
        }

        @Test func noteAllocatorPlaysNotes() {
            bootMachine()
            install()
            defer { machine_set_midi_handler(nil, nil) }
            let na = openDefault("nota")
            #expect(na != 0)
            let nr = guestBytes(noteRequest(gm: 41))               // a violin
            let ncp = tb_NewPtr(4, false, true)
            #expect(component(na, 4, [nr, ncp]) == 0)             // NANewNoteChannel
            let nc = rd32(ncp)
            #expect(nc != 0)
            #expect(component(na, 13, [nc, 60, 80]) == 0)         // NAPlayNote
            #expect(component(na, 13, [nc, 60, 0]) == 0)          // velocity 0: off
            let id: Int32 = 0x4E41_0000
            let events = sent(id)
            let ch = events.first { $0.status & 0xF0 == 0xC0 }
            #expect(ch?.data1 == 40)
            let channel = (ch?.status ?? 0) & 15
            #expect(events.contains { $0.status == 0x90 | channel && $0.data1 == 60 && $0.data2 == 80 })
            #expect(events.contains { $0.status == 0x80 | channel && $0.data1 == 60 })
            #expect(events.allSatisfy { $0.delay_ms == 0 })
            // NAStuffToneDescription names the instrument.
            let td = tb_NewPtr(76, false, true)
            #expect(component(na, 27, [41, td]) == 0)
            #expect(rd8(td + 36) == 6 && rd32(td + 72) == 41)    // "Violin"
            #expect(component(na, 5, [nc]) == 0)                  // NADisposeNoteChannel
            #expect(component(na, 13, [nc, 60, 80]) != 0)         // gone
        }
    }
}
