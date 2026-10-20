// Sound input: the host's microphone for the Sound Input Manager, through
// an AVAudioEngine of its own whose input is tapped while a program has the
// device open.  macOS asks the user the first time; until they allow it
// (or if they don't) the machine records silence.

#if canImport(AVFoundation)
@preconcurrency import AVFoundation
import Toolbox

public final class ClassicSoundInput: @unchecked Sendable {
    public static let shared = ClassicSoundInput()

    private let queue = DispatchQueue(label: "bluebox.soundinput")
    /* Touched only on `queue`. */
    private var engine: AVAudioEngine?
    private var wanted = false

    /// Give the machine the microphone.
    public func install() {
        machine_set_sound_input_handler({ start, _ in
            ClassicSoundInput.shared.capture(start)
        }, nil)
    }

    /// Start or stop capturing; false when there is no microphone to use.
    private func capture(_ on: Bool) -> Bool {
        guard on else {
            queue.async { self.wanted = false; self.stop() }
            return true
        }
        guard AVCaptureDevice.default(for: .audio) != nil else { return false }
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .denied, .restricted:
            return false
        case .notDetermined:
            queue.async { self.wanted = true }
            AVCaptureDevice.requestAccess(for: .audio) { granted in
                guard granted else { return }
                self.queue.async { self.start() }
            }
        default:
            queue.async { self.wanted = true; self.start() }
        }
        return true
    }

    /// On `queue`: tap the input, mixing nothing, handing every buffer over.
    private func start() {
        guard wanted, engine == nil else { return }
        let engine = AVAudioEngine()
        let input = engine.inputNode
        let format = input.outputFormat(forBus: 0)
        guard format.sampleRate > 0, format.channelCount > 0 else { return }
        let channels = Int(format.channelCount), rate = format.sampleRate
        input.installTap(onBus: 0, bufferSize: 2048, format: format) { buffer, _ in
            guard let data = buffer.floatChannelData else { return }
            let frames = Int(buffer.frameLength)
            var pcm = [Float](repeating: 0, count: frames * channels)
            for ch in 0..<channels {
                let samples = data[ch]
                for i in 0..<frames { pcm[i * channels + ch] = samples[i] }
            }
            machine_sound_input(pcm, Int32(frames), Int32(channels), rate)
        }
        do {
            try engine.start()
            self.engine = engine
        } catch {
            input.removeTap(onBus: 0)
        }
    }

    private func stop() {
        guard let engine else { return }
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        self.engine = nil
    }
}
#endif
