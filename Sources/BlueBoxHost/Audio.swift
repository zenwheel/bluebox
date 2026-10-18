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
    /* Touched only on `queue`.  scheduled: seconds of sound given to the
     * player since it last started. */
    private var players: [Int32: (node: AVAudioPlayerNode, format: AVAudioFormat, scheduled: Double)] = [:]
    /// More waiting to play than this when the next sound comes is sound
    /// that fell behind: the machine paces its channels by its clock and
    /// keeps at most about a tenth of a second ahead.
    private let maxBacklog = 0.5
    private var observing = false

    /// Route the machine's sound to the host.
    public func install() {
        // The engine running before the first sound: sound that came while
        // it started (which can take a while) would otherwise all wait for
        // it, and the program's sound would stay that far behind.
        queue.async {
            _ = self.engine.mainMixerNode
            if !self.engine.isRunning { try? self.engine.start() }
        }
        machine_set_sound_handler({ channel, pcm, frames, channels, rate, volume, _ in
            ClassicAudio.shared.play(channel: channel, pcm: pcm, frames: Int(frames), channels: Int(channels),
                                     rate: rate, volume: volume)
        }, nil)
    }

    private func play(channel: Int32, pcm: UnsafePointer<Int16>?, frames: Int, channels: Int, rate: Double,
                      volume: Float) {
        guard let pcm else {
            queue.async {
                self.players[channel]?.node.stop()
                self.players[channel]?.scheduled = 0
            }
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
                guard let self else { return }
                self.queue.async { [weak self] in self?.restartPlayers() }
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
            entry = (node, format, 0)
            players[channel] = entry
        }
        if !engine.isRunning {
            do { try engine.start() } catch { return }
        }
        let node = entry!.node
        // Sound that piled up while the player could not keep up (the
        // output device starting, which can take seconds) would otherwise
        // stay ahead of everything after it: a game's sound ten seconds
        // late.  Drop it and go on from this buffer.
        if backlog(node, scheduled: entry!.scheduled) > maxBacklog {
            node.stop()
            entry!.scheduled = 0
        }
        node.volume = volume
        node.scheduleBuffer(buffer, completionHandler: nil)
        entry!.scheduled += Double(buffer.frameLength) / format.sampleRate
        players[channel] = entry
        if !node.isPlaying { node.play() }
    }

    /// Seconds given to the player that it has not played yet.
    private func backlog(_ node: AVAudioPlayerNode, scheduled: Double) -> Double {
        guard node.isPlaying, let now = node.lastRenderTime, let played = node.playerTime(forNodeTime: now),
              played.sampleRate > 0 else { return scheduled }
        return scheduled - Double(played.sampleTime) / played.sampleRate
    }

    private func restartPlayers() {
        if !engine.isRunning { try? engine.start() }
    }
}
#endif
