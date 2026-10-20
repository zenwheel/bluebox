// The MIDI Manager's host client on the host's MIDI: what programs send it
// goes to every CoreMIDI destination (none, and the machine plays it on the
// General MIDI synthesizer instead), and every CoreMIDI source plays into
// it.  CoreMIDI speaks Universal MIDI Packets; the machine, MIDI 1.0 bytes.

#if canImport(CoreMIDI)
import CoreMIDI
import Toolbox

public final class ClassicMIDI: @unchecked Sendable {
    public static let shared = ClassicMIDI()

    private var client = MIDIClientRef()
    private var input = MIDIPortRef()
    private var output = MIDIPortRef()
    private let lock = NSLock()
    /* Under `lock`. */
    private var connected: Set<MIDIEndpointRef> = []

    public func install() {
        guard client == 0 else { return }
        guard MIDIClientCreateWithBlock("Blue Box" as CFString, &client, { _ in
            ClassicMIDI.shared.connectSources()          // sources come and go
        }) == noErr else { return }
        MIDIOutputPortCreate(client, "Blue Box out" as CFString, &output)
        MIDIInputPortCreateWithProtocol(client, "Blue Box in" as CFString, ._1_0, &input) { list, _ in
            var bytes: [UInt8] = []
            for packet in list.unsafeSequence() {
                ClassicMIDI.bytes(of: packet, into: &bytes)
            }
            if !bytes.isEmpty { machine_midi_in(bytes, Int32(bytes.count)) }
        }
        connectSources()
        machine_set_midi_out_handler({ bytes, n, _ in
            guard let bytes else { return false }
            return ClassicMIDI.shared.send(Array(UnsafeBufferPointer(start: bytes, count: Int(n))))
        }, nil)
    }

    private func connectSources() {
        lock.lock()
        defer { lock.unlock() }
        let sources = Set((0..<MIDIGetNumberOfSources()).map { MIDIGetSource($0) }.filter { $0 != 0 })
        for s in sources.subtracting(connected) where MIDIPortConnectSource(input, s, nil) == noErr {
            connected.insert(s)
        }
        connected.formIntersection(sources)
    }

    /// On the emulator thread: one whole message to every destination; false if there are none.
    private func send(_ message: [UInt8]) -> Bool {
        let n = MIDIGetNumberOfDestinations()
        guard n > 0, !message.isEmpty else { return false }
        let packets = ClassicMIDI.packets(of: message)
        let size = 1024
        let raw = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { raw.deallocate() }
        let list = raw.bindMemory(to: MIDIEventList.self, capacity: 1)
        var cur: UnsafeMutablePointer<MIDIEventPacket>? = MIDIEventListInit(list, ._1_0)
        for words in packets {
            guard let c = cur else { break }
            cur = MIDIEventListAdd(list, size, c, 0, words.count, words)
        }
        for i in 0..<n {
            let dest = MIDIGetDestination(i)
            if dest != 0 { MIDISendEventList(output, dest, list) }
        }
        return true
    }

    /// A MIDI 1.0 message as Universal MIDI Packets (group 0): System Exclusive six bytes a packet.
    public static func packets(of message: [UInt8]) -> [[UInt32]] {
        guard let status = message.first else { return [] }
        guard status == 0xF0 else {
            let d1 = message.count > 1 ? UInt32(message[1]) : 0, d2 = message.count > 2 ? UInt32(message[2]) : 0
            return [[(status < 0xF0 ? 0x2000_0000 : 0x1000_0000) | UInt32(status) << 16 | d1 << 8 | d2]]
        }
        let body = Array(message.dropFirst().prefix { $0 != 0xF7 })
        let chunks = stride(from: 0, to: max(body.count, 1), by: 6).map { Array(body[$0..<min($0 + 6, body.count)]) }
        return chunks.enumerated().map { i, chunk in
            let kind: UInt32 = chunks.count == 1 ? 0 : i == 0 ? 1 : i == chunks.count - 1 ? 3 : 2
            let b = (chunk + [UInt8](repeating: 0, count: 6 - chunk.count)).map { UInt32($0 & 0x7F) }
            return [0x3000_0000 | kind << 20 | UInt32(chunk.count) << 16 | b[0] << 8 | b[1],
                    b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5]]
        }
    }

    /// MIDI 1.0 bytes of a packet's system, channel voice and System Exclusive messages.
    private static func bytes(of packet: UnsafePointer<MIDIEventPacket>, into out: inout [UInt8]) {
        let count = Int(packet.pointee.wordCount)
        withUnsafeBytes(of: packet.pointee.words) { raw in
            bytes(of: Array(raw.bindMemory(to: UInt32.self).prefix(count)), into: &out)
        }
    }

    /// The same from the packets' words.
    public static func bytes(of words: [UInt32], into out: inout [UInt8]) {
        var i = 0
        while i < words.count {
            let w = words[i]
            let type = w >> 28
            switch type {
            case 1, 2:
                let status = UInt8(truncatingIfNeeded: w >> 16)
                out.append(status)
                out.append(contentsOf: [UInt8(truncatingIfNeeded: w >> 8), UInt8(truncatingIfNeeded: w)]
                            .prefix(dataBytes(status)))
            case 3 where i + 1 < words.count:
                let kind = (w >> 20) & 0xF, n = Int((w >> 16) & 0xF)
                let w1 = words[i + 1]
                let body = [w >> 8, w, w1 >> 24, w1 >> 16, w1 >> 8, w1].map { UInt8(truncatingIfNeeded: $0) }
                if kind == 0 || kind == 1 { out.append(0xF0) }
                out.append(contentsOf: body.prefix(min(n, 6)))
                if kind == 0 || kind == 3 { out.append(0xF7) }
            default:
                break
            }
            i += [1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4][Int(type)]  // words by message type
        }
    }

    private static func dataBytes(_ status: UInt8) -> Int {
        switch status & 0xF0 {
        case 0xC0, 0xD0: return 1
        case 0xF0: return status == 0xF2 ? 2 : status == 0xF1 || status == 0xF3 ? 1 : 0
        default: return 2
        }
    }
}
#endif
