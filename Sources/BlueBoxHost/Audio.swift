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
    /// Suspended (the host's Suspend): the players hold what they were
    /// given and play it on from where they stopped.  On `queue`.
    private var suspended = false

    /// Offline (--sound offline): the engine renders to nowhere, as fast
    /// as the clock goes, with no output device (a headless run).
    private var renderer: OfflineRenderer?

    /// Route the machine's sound to the host; `offline`, rendered to
    /// nowhere without an output device.
    public func install(offline: Bool = false) {
        // The engine running before the first sound: sound that came while
        // it started (which can take a while) would otherwise all wait for
        // it, and the program's sound would stay that far behind.
        queue.async {
            if offline { self.renderer = OfflineRenderer(engine: self.engine, queue: self.queue, name: "sound") }
            _ = self.engine.mainMixerNode
            if !self.engine.isRunning { try? self.engine.start() }
            self.renderer?.begin()
        }
        machine_set_sound_handler({ channel, pcm, frames, channels, rate, volume, _ in
            ClassicAudio.shared.play(channel: channel, pcm: pcm, frames: Int(frames), channels: Int(channels),
                                     rate: rate, volume: volume)
        }, nil)
    }

    /// Hold the sound still while the program is suspended, and go on.
    public func setSuspended(_ on: Bool) {
        queue.async {
            guard self.suspended != on else { return }
            self.suspended = on
            if on {
                for entry in self.players.values where entry.node.isPlaying { entry.node.pause() }
                self.engine.pause()
            } else {
                if !self.engine.isRunning { try? self.engine.start() }
                for (channel, entry) in self.players where entry.scheduled > 0 { self.start(channel) }
            }
        }
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
        let node = entry!.node
        if suspended { // what came just before: it waits with the rest
            node.scheduleBuffer(buffer, completionHandler: nil)
            entry!.scheduled += Double(buffer.frameLength) / format.sampleRate
            players[channel] = entry
            return
        }
        if !engine.isRunning {
            do { try engine.start() } catch { return }
        }
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
        if !node.isPlaying { start(channel) }
    }

    /// On `queue`: the channel's player playing.  The engine can stop under
    /// it at any moment (the output device changing, on CoreAudio's own
    /// thread), and play on a stopped engine raises an exception Swift
    /// cannot catch: the engine is started again and the player with it,
    /// and if that fails too what the channel had waiting is dropped and the
    /// next sound starts it.
    private func start(_ channel: Int32) {
        guard let node = players[channel]?.node else { return }
        var reason = [CChar](repeating: 0, count: 256)
        if host_objc_try({ node.play() }, &reason, reason.count) { return }
        try? engine.start()
        if engine.isRunning, host_objc_try({ node.play() }, nil, 0) { return }
        node.stop()
        players[channel]?.scheduled = 0
        FileHandle.standardError.write(Data("sound: channel \(channel) not started: \(String(decoding: reason.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self))\n".utf8))
    }

    /// Seconds given to the player that it has not played yet.
    private func backlog(_ node: AVAudioPlayerNode, scheduled: Double) -> Double {
        // A render time with neither clock valid (the engine not rendering
        // yet, or rendering offline) is an exception for playerTime.
        guard node.isPlaying, let now = node.lastRenderTime, now.isSampleTimeValid || now.isHostTimeValid,
              let played = node.playerTime(forNodeTime: now), played.sampleRate > 0 else { return scheduled }
        return scheduled - Double(played.sampleTime) / played.sampleRate
    }

    private func restartPlayers() {
        if !engine.isRunning && !suspended { try? engine.start() }
    }

    /// What the offline engine rendered (seconds, and seconds not silent).
    public func offlineReport() -> String? { queue.sync { renderer?.report() } }
}

/// An engine in manual rendering mode, pulled on its own queue a slice at
/// a time as the machine's clock goes (a headless run's follows its
/// instructions, faster than the wall's) and thrown away: the players, the
/// mixer and the synthesizers run as with a device, without one.  The
/// clock is read from this queue while the machine runs: a test mode.
final class OfflineRenderer: @unchecked Sendable {
    private let engine: AVAudioEngine
    private let queue: DispatchQueue
    private let name: String
    private let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 2)!
    private let slice: AVAudioFrameCount = 1024
    private var buffer: AVAudioPCMBuffer?
    private var timer: DispatchSourceTimer?
    private var started: UInt64 = 0
    private var frames: UInt64 = 0, loud: UInt64 = 0
    private var audible = Set<Int>() // the machine's seconds with sound in them
    private var failed = false

    /// On `queue`, with the engine stopped.
    init?(engine: AVAudioEngine, queue: DispatchQueue, name: String) {
        self.engine = engine
        self.queue = queue
        self.name = name
        do {
            try engine.enableManualRenderingMode(.offline, format: format, maximumFrameCount: slice)
        } catch {
            return nil
        }
        buffer = AVAudioPCMBuffer(pcmFormat: engine.manualRenderingFormat, frameCapacity: slice)
    }

    /// On `queue`, the engine started: render as the machine's clock goes.
    func begin() {
        started = machine_time_ns()
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .milliseconds(2))
        t.setEventHandler { [weak self] in self?.render() }
        t.resume()
        timer = t
    }

    private func render() {
        guard let buffer, engine.isRunning, !failed else { return }
        let now = machine_time_ns()
        let due = now > started ? UInt64(Double(now - started) / 1e9 * format.sampleRate) : 0
        while frames + UInt64(slice) <= due {
            let status: AVAudioEngineManualRenderingStatus
            do { status = try engine.renderOffline(slice, to: buffer) } catch {
                failed = true
                return
            }
            guard status == .success else { break }
            let second = Int((Double(started) / 1e9 + Double(frames) / format.sampleRate).rounded(.down))
            frames += UInt64(buffer.frameLength)
            if let data = buffer.floatChannelData {
                var peak: Float = 0
                for ch in 0..<Int(buffer.format.channelCount) {
                    for i in 0..<Int(buffer.frameLength) { peak = max(peak, abs(data[ch][i])) }
                }
                if peak > 1.0 / 32768 {
                    loud += UInt64(buffer.frameLength)
                    audible.insert(second)
                }
            }
        }
    }

    /// On `queue`: rendered up to the machine's now, and what was heard
    /// (the machine's seconds, as ranges).
    func report() -> String {
        render()
        var ranges: [String] = []
        for sec in audible.sorted() {
            if let last = ranges.last, let dash = last.split(separator: "-").last, Int(dash) == sec - 1 {
                let from = last.split(separator: "-").first!
                ranges[ranges.count - 1] = "\(from)-\(sec)"
            } else {
                ranges.append("\(sec)")
            }
        }
        return String(format: "%@: rendered %.1f s offline from %.1f s, %.1f s of it not silent%@; heard at %@ s", name,
                      Double(frames) / format.sampleRate, Double(started) / 1e9, Double(loud) / format.sampleRate,
                      failed ? " (rendering failed)" : "", ranges.isEmpty ? "none" : ranges.joined(separator: ","))
    }
}
#endif
