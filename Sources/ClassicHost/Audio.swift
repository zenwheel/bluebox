// Sound output: classic Sound Manager channels as AVAudioEngine player nodes.

#if canImport(AVFoundation)
import AVFoundation
import Toolbox

public final class ClassicAudio: @unchecked Sendable {
    public static let shared = ClassicAudio()

    private let engine = AVAudioEngine()
    private var players: [Int32: (node: AVAudioPlayerNode, format: AVAudioFormat)] = [:]
    private let lock = NSLock()
    private var started = false

    /// Route the machine's sound to the host.
    public func install() {
        machine_set_sound_handler({ channel, pcm, frames, channels, rate, volume, _ in
            ClassicAudio.shared.play(channel: channel, pcm: pcm, frames: Int(frames), channels: Int(channels),
                                     rate: rate, volume: volume)
        }, nil)
    }

    private func play(channel: Int32, pcm: UnsafePointer<Int16>?, frames: Int, channels: Int, rate: Double,
                      volume: Float) {
        lock.lock()
        defer { lock.unlock() }
        guard let pcm else {
            players[channel]?.node.stop()
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
        if !started {
            do { try engine.start() } catch { return }
            started = true
        }
        entry!.node.volume = volume
        entry!.node.scheduleBuffer(buffer, completionHandler: nil)
        if !entry!.node.isPlaying { entry!.node.play() }
    }
}
#endif
