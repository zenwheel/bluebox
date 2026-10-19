// Speech: the Speech Manager's channels as NSSpeechSynthesizers.
//
// NSSpeechSynthesizer speaks through the same engine the classic Speech
// Manager grew into: it understands the classic embedded commands
// ([[rate 200]], [[pbas 40]], [[slnc 500]] ...) and macOS still has the
// MacinTalk voices by name (Fred, Ralph, Kathy, Junior, Zarvox, Whisper,
// Bad News ...).  Requests come on the emulator thread and are done on the
// main thread without waiting; words, sync messages, phonemes and the end
// of each utterance go back as machine_speech_event.

#if canImport(AppKit)
import AppKit
import AVFoundation
import Toolbox

public final class ClassicSpeech: NSObject, NSSpeechSynthesizerDelegate, @unchecked Sendable {
    public static let shared = ClassicSpeech()

    /// Touched only on the main thread.
    private var synths: [Int32: NSSpeechSynthesizer] = [:]
    private var channelOf: [ObjectIdentifier: Int32] = [:]

    /// AVSpeechSynthesizer instead (--speech avspeech, or no NSSpeechSynthesizer voices).
    private var useAV = false
    private let av = AVSpeech()

    /// Route the machine's speech to the host.
    public func install(avspeech: Bool = false) {
        useAV = avspeech || NSSpeechSynthesizer.availableVoices.isEmpty
        machine_set_speech_handler({ rqp, _ in
            guard let rq = rqp?.pointee else { return }
            let op = Int(rq.op)
            let channel = rq.channel
            if op == Int(MACHINE_SPEECH_PHONEMES) {
                // Answered at once (the emulator waits for the result).
                let text = rq.text.map { String(cString: $0) } ?? ""
                let voice = rq.voice.map { String(cString: $0) }
                var phonemes = ""
                let work = { phonemes = MainActor.assumeIsolated { ClassicSpeech.shared.phonemes(text, voice: voice) } }
                if Thread.isMainThread { work() } else { DispatchQueue.main.sync(execute: work) }
                if let out = rq.result, rq.result_size > 0 {
                    let bytes = Array(phonemes.utf8.prefix(Int(rq.result_size) - 1))
                    for (i, b) in bytes.enumerated() { out[i] = CChar(bitPattern: b) }
                    out[bytes.count] = 0
                }
                return
            }
            let text = rq.text.map { String(cString: $0) }
            let voice = rq.voice.map { String(cString: $0) }
            let (rate, pitch, modulation, volume) = (rq.rate, rq.pitch, rq.modulation, rq.volume)
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    if ClassicSpeech.shared.useAV {
                        ClassicSpeech.shared.av.perform(op: op, channel: channel, text: text, voice: voice, rate: rate,
                                                        pitch: pitch, volume: volume)
                    } else {
                        ClassicSpeech.shared.perform(op: op, channel: channel, text: text, voice: voice, rate: rate,
                                                     pitch: pitch, modulation: modulation, volume: volume)
                    }
                }
            }
        }, nil)
    }

    /// The macOS voice standing for a classic one: the same name, else the default.
    private func voiceIdentifier(_ name: String?) -> NSSpeechSynthesizer.VoiceName? {
        guard let name else { return nil }
        let wanted = name.lowercased().replacingOccurrences(of: " ", with: "")
        for v in NSSpeechSynthesizer.availableVoices {
            let attrs = NSSpeechSynthesizer.attributes(forVoice: v)
            let vname = (attrs[.name] as? String ?? "").lowercased().replacingOccurrences(of: " ", with: "")
            if vname == wanted { return v }
        }
        return nil
    }

    @MainActor private func synth(_ channel: Int32) -> NSSpeechSynthesizer {
        if let s = synths[channel] { return s }
        let s = NSSpeechSynthesizer()
        s.delegate = self
        synths[channel] = s
        channelOf[ObjectIdentifier(s)] = channel
        return s
    }

    @MainActor private func perform(op: Int, channel: Int32, text: String?, voice: String?, rate: Double, pitch: Double,
                                    modulation: Double, volume: Double) {
        switch op {
        case Int(MACHINE_SPEECH_SPEAK):
            let s = synth(channel)
            if s.isSpeaking { s.stopSpeaking() }
            if let v = voiceIdentifier(voice) { _ = s.setVoice(v) }
            if rate > 0 { s.rate = Float(rate) }
            s.volume = Float(max(0, min(1, volume)))
            if pitch > 0 { try? s.setObject(NSNumber(value: pitch), forProperty: .pitchBase) }
            if modulation >= 0 { try? s.setObject(NSNumber(value: modulation), forProperty: .pitchMod) }
            if !s.startSpeaking(text ?? "") { machine_speech_event(channel, 0, 0, 0) }
        case Int(MACHINE_SPEECH_STOP):
            synths[channel]?.stopSpeaking()
        case Int(MACHINE_SPEECH_PAUSE):
            synths[channel]?.pauseSpeaking(at: .immediateBoundary)
        case Int(MACHINE_SPEECH_CONTINUE):
            synths[channel]?.continueSpeaking()
        case Int(MACHINE_SPEECH_DISPOSE):
            if let s = synths.removeValue(forKey: channel) {
                s.stopSpeaking()
                channelOf[ObjectIdentifier(s)] = nil
            }
        default:
            break
        }
    }

    @MainActor private func phonemes(_ text: String, voice: String?) -> String {
        let s = NSSpeechSynthesizer()
        if let v = voiceIdentifier(voice) { _ = s.setVoice(v) }
        return s.phonemes(from: text)
    }

    private func channel(_ s: NSSpeechSynthesizer) -> Int32? { channelOf[ObjectIdentifier(s)] }

    public func speechSynthesizer(_ sender: NSSpeechSynthesizer, didFinishSpeaking finishedSpeaking: Bool) {
        if let c = channel(sender) { machine_speech_event(c, 0, 0, 0) }
    }

    public func speechSynthesizer(_ sender: NSSpeechSynthesizer, willSpeakWord characterRange: NSRange,
                                  of string: String) {
        // MacRoman text: one UTF-16 unit per byte, so the range is the text's.
        if let c = channel(sender) {
            machine_speech_event(c, 1, Int32(characterRange.location), Int32(characterRange.length))
        }
    }

    public func speechSynthesizer(_ sender: NSSpeechSynthesizer, didEncounterSyncMessage message: String) {
        // Sync messages are four characters ([[sync 0x61626364]] or 'abcd').
        var v: UInt32 = 0
        if message.hasPrefix("0x"), let n = UInt32(message.dropFirst(2), radix: 16) {
            v = n
        } else {
            for b in message.utf8.prefix(4) { v = v << 8 | UInt32(b) }
        }
        if let c = channel(sender) { machine_speech_event(c, 2, Int32(bitPattern: v), 0) }
    }

    public func speechSynthesizer(_ sender: NSSpeechSynthesizer, willSpeakPhoneme phonemeOpcode: Int16) {
        if let c = channel(sender) { machine_speech_event(c, 3, Int32(phonemeOpcode), 0) }
    }
}

/// The AVSpeechSynthesizer backend.  Its utterances have no embedded
/// commands, so an utterance is cut at each: rate, pitch base and volume
/// become the next piece's settings, silence its pause, and a sync is
/// reported when the speech reaches it.  Words come from willSpeakRange,
/// placed back in the original text; phonemes aren't reported.
final class AVSpeech: NSObject, AVSpeechSynthesizerDelegate, @unchecked Sendable {
    private struct Piece {
        var text: String
        var offset: Int /* in the original text */
        var rate: Double, pitch: Double, volume: Double
        var silenceAfter: Double
        var syncsBefore: [Int32]
    }
    private var synths: [Int32: AVSpeechSynthesizer] = [:]
    private var current: [ObjectIdentifier: (channel: Int32, offsets: [ObjectIdentifier: Int], syncs: [ObjectIdentifier: [Int32]], last: ObjectIdentifier?)] = [:]

    private func voice(_ name: String?) -> AVSpeechSynthesisVoice? {
        guard let name else { return nil }
        let wanted = name.lowercased().replacingOccurrences(of: " ", with: "")
        return AVSpeechSynthesisVoice.speechVoices().first {
            $0.name.lowercased().replacingOccurrences(of: " ", with: "") == wanted
        }
    }

    /// The text cut at its embedded commands.
    private func split(_ text: String, rate: Double, pitch: Double, volume: Double) -> [Piece] {
        let u = Array(text.utf16)
        var out = [Piece]()
        var cur = Piece(text: "", offset: 0, rate: rate, pitch: pitch, volume: volume, silenceAfter: 0, syncsBefore: [])
        var i = 0, start = 0
        func flush(_ end: Int) {
            cur.text = String(utf16CodeUnits: Array(u[start..<end]), count: end - start)
            cur.offset = start
            if !cur.text.trimmingCharacters(in: .whitespaces).isEmpty || !cur.syncsBefore.isEmpty || cur.silenceAfter > 0 {
                out.append(cur)
            }
            cur.syncsBefore = []
            cur.silenceAfter = 0
        }
        while i < u.count {
            if i + 1 < u.count, u[i] == 0x5B, u[i + 1] == 0x5B,
               let close = (i + 2..<u.count).first(where: { $0 + 1 < u.count && u[$0] == 0x5D && u[$0 + 1] == 0x5D }) {
                flush(i)
                let cmd = String(utf16CodeUnits: Array(u[(i + 2)..<close]), count: close - i - 2)
                    .trimmingCharacters(in: .whitespaces)
                let parts = cmd.split(separator: " ", maxSplits: 1).map(String.init)
                let arg = parts.count > 1 ? parts[1].trimmingCharacters(in: .whitespaces) : ""
                let value = Double(arg.replacingOccurrences(of: "+", with: "")) ?? 0
                switch parts.first?.lowercased() ?? "" {
                case "rate": cur.rate = arg.hasPrefix("+") || arg.hasPrefix("-") ? cur.rate + value : value
                case "pbas": cur.pitch = arg.hasPrefix("+") || arg.hasPrefix("-") ? cur.pitch + value : value
                case "volm": cur.volume = value
                case "slnc": if var last = out.popLast() { last.silenceAfter += value / 1000; out.append(last) }
                case "sync":
                    var v: UInt32 = 0
                    if arg.hasPrefix("0x"), let n = UInt32(arg.dropFirst(2), radix: 16) {
                        v = n
                    } else if let n = UInt32(arg) {
                        v = n
                    } else {
                        for b in arg.trimmingCharacters(in: CharacterSet(charactersIn: "'\"")).utf8.prefix(4) { v = v << 8 | UInt32(b) }
                    }
                    cur.syncsBefore.append(Int32(bitPattern: v))
                default: break
                }
                i = close + 2
                start = i
                continue
            }
            i += 1
        }
        flush(u.count)
        return out
    }

    @MainActor func perform(op: Int, channel: Int32, text: String?, voice name: String?, rate: Double, pitch: Double,
                            volume: Double) {
        switch op {
        case Int(MACHINE_SPEECH_SPEAK):
            let s = synths[channel] ?? {
                let s = AVSpeechSynthesizer()
                s.delegate = self
                synths[channel] = s
                return s
            }()
            if s.isSpeaking { s.stopSpeaking(at: .immediate) }
            let parts = split(text ?? "", rate: rate > 0 ? rate : 180, pitch: pitch > 0 ? pitch : 46,
                              volume: max(0, min(1, volume)))
            var state = (channel: channel, offsets: [ObjectIdentifier: Int](), syncs: [ObjectIdentifier: [Int32]](),
                         last: ObjectIdentifier?.none)
            let v = self.voice(name)
            for p in parts {
                let ut = AVSpeechUtterance(string: p.text)
                if let v { ut.voice = v }
                // About 180 words a minute is AV's default rate.
                ut.rate = Float(max(Double(AVSpeechUtteranceMinimumSpeechRate),
                                    min(Double(AVSpeechUtteranceMaximumSpeechRate),
                                        Double(AVSpeechUtteranceDefaultSpeechRate) * p.rate / 180)))
                ut.pitchMultiplier = Float(max(0.5, min(2, pow(2, (p.pitch - 46) / 12))))
                ut.volume = Float(p.volume)
                ut.postUtteranceDelay = p.silenceAfter
                state.offsets[ObjectIdentifier(ut)] = p.offset
                state.syncs[ObjectIdentifier(ut)] = p.syncsBefore
                state.last = ObjectIdentifier(ut)
                s.speak(ut)
            }
            current[ObjectIdentifier(s)] = state
            if parts.isEmpty { machine_speech_event(channel, 0, 0, 0) }
        case Int(MACHINE_SPEECH_STOP):
            synths[channel]?.stopSpeaking(at: .immediate)
        case Int(MACHINE_SPEECH_PAUSE):
            synths[channel]?.pauseSpeaking(at: .immediate)
        case Int(MACHINE_SPEECH_CONTINUE):
            synths[channel]?.continueSpeaking()
        case Int(MACHINE_SPEECH_DISPOSE):
            if let s = synths.removeValue(forKey: channel) {
                s.stopSpeaking(at: .immediate)
                current[ObjectIdentifier(s)] = nil
            }
        default:
            break
        }
    }

    func speechSynthesizer(_ s: AVSpeechSynthesizer, didStart utterance: AVSpeechUtterance) {
        guard let st = current[ObjectIdentifier(s)] else { return }
        for sync in st.syncs[ObjectIdentifier(utterance)] ?? [] { machine_speech_event(st.channel, 2, sync, 0) }
    }

    func speechSynthesizer(_ s: AVSpeechSynthesizer, willSpeakRangeOfSpeechString range: NSRange,
                           utterance: AVSpeechUtterance) {
        guard let st = current[ObjectIdentifier(s)] else { return }
        let base = st.offsets[ObjectIdentifier(utterance)] ?? 0
        machine_speech_event(st.channel, 1, Int32(base + range.location), Int32(range.length))
    }

    func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        guard let st = current[ObjectIdentifier(s)], st.last == ObjectIdentifier(utterance) else { return }
        machine_speech_event(st.channel, 0, 0, 0)
    }

    func speechSynthesizer(_ s: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        guard let st = current[ObjectIdentifier(s)], st.last == ObjectIdentifier(utterance) else { return }
        machine_speech_event(st.channel, 0, 0, 0)
    }
}
#endif
