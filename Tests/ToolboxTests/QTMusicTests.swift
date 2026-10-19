import Foundation
import Testing
import Toolbox

/// QuickTime music events to MIDI, on a hand-made tune: a header naming
/// two parts' instruments (piano, and a drum kit), then a rest, notes, a
/// controller and the end marker.
@Suite struct QTMusic {
    private func be32(_ v: UInt32) -> [UInt8] { (0..<4).map { UInt8(v >> (24 - 8 * UInt32($0)) & 0xFF) } }

    /// A general NoteRequest event for a part: head, NoteRequest (84 bytes), tail.
    private func noteRequest(part: UInt32, gm: UInt32) -> [UInt8] {
        var nr = [UInt8](repeating: 0, count: 84)
        nr.replaceSubrange(80..<84, with: be32(gm))
        return be32(0xF000_0000 | part << 16 | 23) + nr + be32(0xC000_0000 | 1 << 16 | 23)
    }

    @Test func notesRestsAndControllers() {
        let header = noteRequest(part: 1, gm: 1) + noteRequest(part: 2, gm: 0x4001) + be32(0x6000_0000)
        let note = { (part: UInt32, pitch: UInt32, vel: UInt32, dur: UInt32) -> [UInt8] in
            self.be32(0x2000_0000 | part << 24 | (pitch - 32) << 18 | vel << 11 | dur)
        }
        let tune = be32(600)                                      // rest: a second at 600 a second
            + note(1, 60, 100, 300)                               // middle C, half a second
            + be32(0x4000_0000 | 1 << 24 | 7 << 16 | 0x5000)      // part 1 volume 80
            + note(2, 36, 90, 60)                                 // bass drum
            + be32(300)
            + note(1, 64, 0, 100)                                 // velocity 0: silent, no event
            + be32(0x6000_0000)                                   // the end
            + note(1, 67, 100, 100)                               // after the end: ignored
        var count = 0
        let events = header.withUnsafeBufferPointer { h in
            tune.withUnsafeBufferPointer { t in
                var sample = qt_music_sample(data: t.baseAddress, len: t.count, time: 0)
                return qt_music_events(h.baseAddress, h.count, &sample, 1, &count)
            }
        }
        defer { free(events) }
        let list = (0..<count).map { events![$0] }
        let piano = list.first { $0.status & 0xF0 == 0xC0 }
        #expect(piano?.data1 == 0 && piano?.time == 0)            // GM 1, program 0, at the start
        #expect(!list.contains { $0.status == 0xC9 })             // drums: channel 10, no program
        let on = list.filter { $0.status & 0xF0 == 0x90 }
        #expect(on.count == 2)
        #expect(on[0].data1 == 60 && on[0].data2 == 100 && on[0].time == 600)
        #expect(on[1].status == 0x99 && on[1].data1 == 36)         // the drum on channel 10
        let off = list.first { $0.status & 0xF0 == 0x80 && $0.data1 == 60 }
        #expect(off?.time == 900)
        #expect(list.contains { $0.status & 0xF0 == 0xB0 && $0.data1 == 7 && $0.data2 == 80 && $0.time == 600 })
        #expect(zip(list, list.dropFirst()).allSatisfy { $0.time <= $1.time }) // in time order
    }
}
