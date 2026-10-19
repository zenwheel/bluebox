// QuickTime music tracks on the host: Apple's General MIDI synthesizer (the
// DLS synth with its built-in GS bank), one per movie.  The machine sends
// MIDI a little ahead of its clock with each message's delay; messages wait
// on a queue of their own, and silencing a synthesizer drops any still
// waiting (a generation count), so a paused movie stops at once.

#if canImport(AVFoundation)
@preconcurrency import AVFoundation
import AudioToolbox
import Toolbox

public final class ClassicMusic: @unchecked Sendable {
    public static let shared = ClassicMusic()

    private let engine = AVAudioEngine()
    private let queue = DispatchQueue(label: "bluebox.music", qos: .userInteractive)
    /* Touched only on `queue`. */
    private var synths: [Int32: AVAudioUnitMIDIInstrument] = [:]
    private var generation: [Int32: Int] = [:]

    public func install() {
        machine_set_midi_handler({ id, events, n, _ in
            let list = (0..<Int(n)).compactMap { events?[$0] }
            ClassicMusic.shared.queue.async { ClassicMusic.shared.handle(id: id, events: list) }
        }, nil)
    }

    /// On `queue`.
    private func handle(id: Int32, events: [machine_midi_event]) {
        guard let synth = synth(for: id) else { return }
        if events.isEmpty { // silence: all notes off, and nothing still waiting plays
            generation[id, default: 0] += 1
            for ch in 0..<16 {
                synth.sendController(123, withValue: 0, onChannel: UInt8(ch))
                synth.sendController(120, withValue: 0, onChannel: UInt8(ch))
            }
            return
        }
        let gen = generation[id, default: 0]
        let start = DispatchTime.now()
        for e in events {
            let send: @Sendable () -> Void = { [weak self] in
                guard let self, self.generation[id, default: 0] == gen else { return }
                synth.sendMIDIEvent(e.status, data1: e.data1, data2: e.data2)
            }
            if e.delay_ms <= 0 {
                send()
            } else {
                queue.asyncAfter(deadline: start + .milliseconds(Int(e.delay_ms)), execute: send)
            }
        }
    }

    /// On `queue`: the movie's synthesizer, attached and the engine running.
    private func synth(for id: Int32) -> AVAudioUnitMIDIInstrument? {
        if let s = synths[id] { return s }
        let desc = AudioComponentDescription(componentType: kAudioUnitType_MusicDevice,
                                             componentSubType: kAudioUnitSubType_DLSSynth,
                                             componentManufacturer: kAudioUnitManufacturer_Apple,
                                             componentFlags: 0, componentFlagsMask: 0)
        let synth = AVAudioUnitMIDIInstrument(audioComponentDescription: desc)
        engine.attach(synth)
        engine.connect(synth, to: engine.mainMixerNode, format: nil)
        if !engine.isRunning {
            do { try engine.start() } catch { return nil }
        }
        synths[id] = synth
        return synth
    }
}
#endif
