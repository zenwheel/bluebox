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
import Toolbox

public final class ClassicSpeech: NSObject, NSSpeechSynthesizerDelegate, @unchecked Sendable {
    public static let shared = ClassicSpeech()

    /// Touched only on the main thread.
    private var synths: [Int32: NSSpeechSynthesizer] = [:]
    private var channelOf: [ObjectIdentifier: Int32] = [:]

    /// Route the machine's speech to the host.
    public func install() {
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
                    ClassicSpeech.shared.perform(op: op, channel: channel, text: text, voice: voice, rate: rate,
                                                 pitch: pitch, modulation: modulation, volume: volume)
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
#endif
