import BlueBoxHost
import CCore
import CPU68K
import Testing
import Toolbox

/// The MIDI Manager: clients and ports, packets from an output port to the
/// read hooks of the input ports connected to it, and the host's own client
/// (MIDI out to the host, MIDI in from it).
extension BlueBox {
    @Suite struct MIDIManager {
        nonisolated(unsafe) static var packets: [(refCon: UInt32, flags: UInt8, stamp: UInt32, data: [UInt8])] = []
        nonisolated(unsafe) static var hookResult: Int16 = 1           // midiMorePacket
        nonisolated(unsafe) static var synth: [machine_midi_event] = []
        nonisolated(unsafe) static var hostOut: [[UInt8]] = []

        /// FUNCTION readHook(packet, refCon): INTEGER, pascal: records the packet.
        static let readHook: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let refCon = cpu_pop32(c), p = cpu_pop32(c)
            let len = UInt32(rd8(p + 1))
            MIDIManager.packets.append((refCon, rd8(p), rd32(p + 2), (6..<max(len, 6)).map { rd8(p + $0) }))
            wr16(c.pointee.a.7, UInt16(bitPattern: MIDIManager.hookResult))
            cpu_push32(c, ret)
        }

        func install() {
            Self.packets = []
            Self.hookResult = 1
            Self.synth = []
            Self.hostOut = []
            machine_set_midi_handler({ _, events, n, _ in
                MIDIManager.synth += (0..<Int(n)).compactMap { events?[$0] }
            }, nil)
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            g_cpu!.pointee.halted = false                          // the test application never ran
        }

        func signIn(_ id: String, _ name: String) {
            let np = tb_NewPtr(64, false, true)
            guestPString(name, at: np)
            #expect(tb_FinaleUnknown2(Int32(bitPattern: fourCC(id)), 0, 0, np) == 0)    // MIDISignIn
        }

        /// MIDIAddPort with MIDIPortParams; the port's refnum.
        func addPort(_ client: String, _ id: String, type: UInt16, hook: UInt32 = 0, refCon: UInt32 = 0,
                     offset: UInt32 = 0) -> Int16 {
            let params = tb_NewPtr(28 + 256, false, true)
            wr32(params, fourCC(id))
            wr16(params + 4, type)
            wr32(params + 8, offset)
            wr32(params + 12, hook)
            wr32(params + 16, refCon)
            guestPString(id, at: params + 28)
            let ref = tb_NewPtr(2, false, true)
            #expect(tb_DirectorUnknown4(Int32(bitPattern: fourCC(client)), 0, ref, params) == 0)
            return Int16(bitPattern: rd16(ref))
        }

        /// A MIDIPacket: flags, length, time stamp, the bytes.
        func packet(_ data: [UInt8], stamp: UInt32 = 0, now: Bool = true) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            wr8(p, now ? 0x80 : 0)                                 // midiTimeStampCurrent
            wr8(p + 1, UInt8(6 + data.count))
            wr32(p + 2, stamp)
            for (i, b) in data.enumerated() { wr8(p + 6 + UInt32(i), b) }
            return p
        }

        func ids(_ h: UInt32) -> [UInt32] {
            let p = rd32(h)
            return (0..<UInt32(rd16(p))).map { rd32(p + 2 + 4 * $0) }
        }

        /// MIDIPortInfo: type, time base, then the ports connected (client, port).
        func connections(_ client: String, _ port: String) -> [(UInt32, UInt32)] {
            let p = rd32(tb_MIDIGetPortInfo(Int32(bitPattern: fourCC(client)), Int32(bitPattern: fourCC(port))))
            return (0..<UInt32(rd16(p + 10))).map { (rd32(p + 12 + 8 * $0), rd32(p + 16 + 8 * $0)) }
        }

        /// SndDispVersion answers each SoundDispatch tool's version, 0 for
        /// one that isn't there (JUNO Librarian looks for the MIDI Manager
        /// so); a port keeps its connection procedure (Anthem's).
        @Test func dispatchVersionsAndConnectionProcs() {
            bootMachine()
            install()
            defer { machine_set_midi_handler(nil, nil) }
            #expect(tb_FinaleUnknown1(4) == tb_DirectorUnknown3())
            #expect(tb_FinaleUnknown1(8) == tb_SndSoundManagerVersion())
            #expect(tb_FinaleUnknown1(0x14) == tb_SPBVersion() && tb_FinaleUnknown1(0x18) == 0)
            signIn("anth", "Anthem")
            let port = addPort("anth", "in  ", type: 1)
            tb_MIDISetConnectionProc(port, 0x0012_3456, 77)
            let out = tb_NewPtr(8, false, true)
            tb_MIDIGetConnectionProc(port, out, out + 4)
            #expect(rd32(out) == 0x0012_3456 && rd32(out + 4) == 77)
            tb_MIDIGetConnectionProc(999, out, out + 4)
            #expect(rd32(out) == 0 && rd32(out + 4) == 0)
        }

        @Test func clientsSignInAndAddPorts() {
            bootMachine()
            install()
            defer { machine_set_midi_handler(nil, nil) }
            #expect(tb_DirectorUnknown3() == 0x0200_8000)          // MIDIVersion
            #expect(ids(tb_MIDIGetClients()) == [fourCC("host")])   // the host's, from the start
            signIn("seqr", "Sequencer")
            #expect(ids(tb_MIDIGetClients()) == [fourCC("host"), fourCC("seqr")])
            let name = tb_NewPtr(256, false, true)
            tb_MIDIGetClientName(Int32(bitPattern: fourCC("seqr")), name)
            #expect(rd8(name) == 9 && rd8(name + 1) == UInt8(ascii: "S"))

            let input = addPort("seqr", "Ain ", type: 1), output = addPort("seqr", "Aout", type: 2)
            #expect(input > 0 && output > 0 && input != output)
            #expect(ids(tb_MIDIGetPorts(Int32(bitPattern: fourCC("seqr")))) == [fourCC("Ain "), fourCC("Aout")])
            #expect(tb_MIDIGetRefCon(input) == 0)
            tb_MIDISetRefCon(input, 42)
            #expect(tb_MIDIGetRefCon(input) == 42)
            // A port ID twice: midiDupIDErr.
            let params = tb_NewPtr(28 + 256, false, true), ref = tb_NewPtr(2, false, true)
            wr32(params, fourCC("Ain "))
            #expect(tb_DirectorUnknown4(Int32(bitPattern: fourCC("seqr")), 0, ref, params) == -260)
            #expect(tb_DirectorUnknown4(Int32(bitPattern: fourCC("none")), 0, ref, params) == -250)

            // Connected to the host's ports as PatchBay would have them.
            #expect(connections("seqr", "Aout").map { $0.1 } == [fourCC("in  ")])
            #expect(connections("seqr", "Ain ").map { $0.1 } == [fourCC("out ")])
            #expect(tb_MIDIUnConnectData(Int32(bitPattern: fourCC("seqr")), Int32(bitPattern: fourCC("Aout")),
                                         Int32(bitPattern: fourCC("host")), Int32(bitPattern: fourCC("in  "))) == 0)
            #expect(connections("seqr", "Aout").isEmpty)
            // Data goes from output to input only.
            #expect(tb_MIDIConnectData(Int32(bitPattern: fourCC("seqr")), Int32(bitPattern: fourCC("Ain ")),
                                       Int32(bitPattern: fourCC("host")), Int32(bitPattern: fourCC("in  "))) == -261)

            tb_MIDISignOut(Int32(bitPattern: fourCC("seqr")))
            #expect(ids(tb_MIDIGetClients()) == [fourCC("host")])
            #expect(connections("host", "out ").isEmpty)
        }

        @Test func packetsReachConnectedReadHook() {
            bootMachine()
            install()
            defer { machine_set_midi_handler(nil, nil) }
            let hook = native_register(Self.readHook, "MIDI read hook", 0)
            signIn("play", "Player")
            signIn("recd", "Recorder")
            let out = addPort("play", "out ", type: 2)
            let time = addPort("recd", "time", type: 0)
            let input = addPort("recd", "in  ", type: 1, hook: hook, refCon: 0x1234)
            let (play, outID) = (Int32(bitPattern: fourCC("play")), Int32(bitPattern: fourCC("out ")))
            let (recd, inID) = (Int32(bitPattern: fourCC("recd")), Int32(bitPattern: fourCC("in  ")))
            #expect(tb_MIDIConnectData(play, outID, recd, inID) == 0)
            #expect(connections("recd", "in  ").map { $0.0 }.contains(fourCC("play")))

            // The time port keeps the input port's time.
            #expect(tb_MIDIConnectTime(recd, Int32(bitPattern: fourCC("time")), recd, inID) == 0)
            tb_MIDISetCurTime(time, 5000)
            #expect(tb_MIDIGetCurTime(input) == 5000)

            #expect(tb_MIDIWritePacket(out, packet([0x90, 60, 100])) == 0)
            #expect(Self.packets.count == 1)
            #expect(Self.packets.first?.refCon == 0x1234)
            #expect(Self.packets.first?.data == [0x90, 60, 100])
            #expect(Self.packets.first?.stamp == 5000 && Self.packets.first?.flags == 0)   // stamped on arrival

            // midiKeepPacket: it stays, and MIDIPoll hands it over again.
            Self.hookResult = 0
            #expect(tb_MIDIWritePacket(out, packet([0x80, 60, 0])) == 0)
            #expect(Self.packets.count == 2)
            Self.hookResult = 1
            tb_MIDIPoll(input, 0)
            #expect(Self.packets.count == 3 && Self.packets.last?.data == [0x80, 60, 0])
            tb_MIDIPoll(input, 0)
            #expect(Self.packets.count == 3)

            // A time stamp ahead in the sender's time (milliseconds since startup) waits for it.
            let now = UInt32(tb_MIDIGetCurTime(out))
            #expect(tb_MIDIWritePacket(out, packet([0xB0, 7, 90], stamp: now + 500, now: false)) == 0)
            #expect(Self.packets.count == 3)
            machine_skip_time(600_000_000)
            midi_pump()
            #expect(Self.packets.count == 4 && Self.packets.last?.data == [0xB0, 7, 90])

            #expect(tb_MIDIUnConnectData(play, outID, recd, inID) == 0)
            #expect(tb_MIDIUnConnectData(play, outID, recd, inID) == -257)    // midiNoConErr
            #expect(tb_MIDIWritePacket(out, packet([0x90, 62, 100])) == 0)
            #expect(Self.packets.count == 4)
        }

        @Test func outputPlaysOnHost() {
            bootMachine()
            install()
            defer {
                machine_set_midi_handler(nil, nil)
                machine_set_midi_out_handler(nil, nil)
            }
            signIn("play", "Player")
            let out = addPort("play", "out ", type: 2)
            // No MIDI destinations: the General MIDI synthesizer.
            #expect(tb_MIDIWritePacket(out, packet([0xC0, 41])) == 0)
            #expect(tb_MIDIWritePacket(out, packet([0x90, 60, 100, 64, 90])) == 0)    // running status
            #expect(Self.synth.map { [$0.status, $0.data1, $0.data2] } == [[0xC0, 41, 0], [0x90, 60, 100], [0x90, 64, 90]])

            // Destinations take every message whole, System Exclusive too.
            machine_set_midi_out_handler({ bytes, n, _ in
                MIDIManager.hostOut.append(Array(UnsafeBufferPointer(start: bytes, count: Int(n))))
                return true
            }, nil)
            Self.synth = []
            #expect(tb_MIDIWritePacket(out, packet([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7])) == 0)
            #expect(tb_MIDIWritePacket(out, packet([0x80, 60, 0])) == 0)
            #expect(Self.hostOut == [[0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7], [0x80, 60, 0]])
            #expect(Self.synth.isEmpty)
        }

        @Test func hostInputReachesReadHook() {
            bootMachine()
            install()
            defer { machine_set_midi_handler(nil, nil) }
            let hook = native_register(Self.readHook, "MIDI read hook", 0)
            signIn("recd", "Recorder")
            _ = addPort("recd", "in  ", type: 1, hook: hook, refCon: 7)
            let bytes: [UInt8] = [0x90, 60, 100, 60, 0, 0xF8, 0xF0, 1, 2, 0xF7]
            machine_midi_in(bytes, Int32(bytes.count))
            #expect(Self.packets.isEmpty)                          // at the next interrupt
            midi_pump()
            #expect(Self.packets.map { $0.data } == [[0x90, 60, 100], [0x90, 60, 0], [0xF8], [0xF0, 1, 2, 0xF7]])
            #expect(Self.packets.allSatisfy { $0.refCon == 7 && $0.flags & 0x80 == 0 })
        }
    }
}

#if canImport(CoreMIDI)
/// MIDI 1.0 bytes to Universal MIDI Packets for CoreMIDI and back.
@Suite struct HostMIDIPackets {
    @Test func channelAndSystemExclusiveRoundTrip() {
        let messages: [[UInt8]] = [[0x90, 60, 100], [0xC3, 41], [0xF8], [0xF2, 1, 2],
                                   [0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7], Array([0xF0] + (1...13) + [0xF7])]
        #expect(ClassicMIDI.packets(of: [0x90, 60, 100]) == [[0x2090_3C64]])
        #expect(ClassicMIDI.packets(of: messages[4]) == [[0x3004_7E7F, 0x0901_0000]])
        for m in messages {
            var out: [UInt8] = []
            ClassicMIDI.bytes(of: ClassicMIDI.packets(of: m).flatMap { $0 }, into: &out)
            #expect(out == m)
        }
    }
}
#endif
