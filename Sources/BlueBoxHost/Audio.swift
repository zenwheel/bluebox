// Sound output: classic Sound Manager channels as AVAudioEngine player nodes.
//
// The emulator never waits for CoreAudio: buffers are made on the calling
// thread and everything that touches the engine (attaching players,
// starting it, which can block for seconds while the output device starts,
// scheduling) happens on a serial queue of its own, in order.  A program
// whose music driver feeds the Sound Manager from a VBL task froze for as
// long as each engine start took (Prince of Persia, every half minute).

#if canImport(AVFoundation)
@preconcurrency import AVFoundation
import Toolbox

public final class ClassicAudio: @unchecked Sendable {
    public static let shared = ClassicAudio()

    private let engine = AVAudioEngine()
    private let queue = DispatchQueue(label: "bluebox.audio")
    /* Touched only on `queue`. */
    private var players: [Int32: (node: AVAudioPlayerNode, format: AVAudioFormat)] = [:]
    private var observing = false

    /// Route the machine's sound to the host.
    public func install() {
        machine_set_sound_handler({ channel, pcm, frames, channels, rate, volume, _ in
            ClassicAudio.shared.play(channel: channel, pcm: pcm, frames: Int(frames), channels: Int(channels),
                                     rate: rate, volume: volume)
        }, nil)
    }

    private func play(channel: Int32, pcm: UnsafePointer<Int16>?, frames: Int, channels: Int, rate: Double,
                      volume: Float) {
        guard let pcm else {
            queue.async { self.players[channel]?.node.stop() }
            return
        }
        guard frames > 0, channels >= 1, channels <= 2,
              let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate,
                                         channels: AVAudioChannelCount(channels), interleaved: false),
              let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames))
        else { return }
        buffer.frameLength = AVAudioFrameCount(frames)
        for ch in 0..<channels {
            guard let out = buffer.floatChannelData?[ch] else { continue }
            for i in 0..<frames { out[i] = Float(pcm[i * channels + ch]) / 32768.0 }
        }
        queue.async { self.schedule(channel: channel, buffer: buffer, format: format, volume: volume) }
    }

    /// On `queue`: the channel's player (a new one for a new format), the
    /// engine running, and the buffer scheduled.
    private func schedule(channel: Int32, buffer: AVAudioPCMBuffer, format: AVAudioFormat, volume: Float) {
        if !observing {
            observing = true
            // The engine stops when the output changes (another device, a new
            // sample rate); start it again with the next sound.
            NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine,
                                                   queue: nil) { [weak self] _ in
                self?.queue.async { self?.restartPlayers() }
            }
        }
        var entry = players[channel]
        if entry == nil || entry!.format != format {
            if let old = entry?.node {
                old.stop()
                engine.detach(old)
            }
            let node = AVAudioPlayerNode()
            engine.attach(node)
            engine.connect(node, to: engine.mainMixerNode, format: format)
            entry = (node, format)
            players[channel] = entry
        }
        if !engine.isRunning {
            do { try engine.start() } catch { return }
        }
        entry!.node.volume = volume
        entry!.node.scheduleBuffer(buffer, completionHandler: nil)
        if !entry!.node.isPlaying { entry!.node.play() }
    }

    private func restartPlayers() {
        if !engine.isRunning { try? engine.start() }
    }
}
#endif
