import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Speech Manager, without a host: utterances end at the next safe point.
    @Suite struct Speech {
        nonisolated(unsafe) static var done: [UInt32] = []
        nonisolated(unsafe) static var phonemes: [UInt16] = []

        /// SpeechDone(chan, refCon).
        static let doneProc: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let refcon = cpu_pop32(c)
            _ = cpu_pop32(c)
            Speech.done.append(refcon)
            cpu_push32(c, ret)
        }

        /// PhonemeCallBack(chan, refCon, phonemeOpcode).
        static let phonemeProc: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let op = cpu_pop16(c)
            _ = cpu_pop32(c)
            _ = cpu_pop32(c)
            Speech.phonemes.append(op)
            cpu_push32(c, ret)
        }

        @Test func voicesAreTheClassicOnes() {
            launchTestApp()
            _ = initToolbox()
            let n = tb_NewPtr(2, false, true)
            #expect(tb_CountVoices(n) == 0 && rd16(n) >= 20)
            let spec = tb_NewPtr(8, false, true)
            var names: [String] = []
            let info = tb_NewPtr(362, false, true)
            for i in 1...Int(rd16(n)) {
                #expect(tb_GetIndVoice(Int16(i), spec) == 0)
                #expect(tb_GetVoiceDescription(spec, info, 362) == 0)
                names.append(pstring(at: info + 16))
            }
            #expect(names.contains("Fred") && names.contains("Kathy") && names.contains("Zarvox"))
            var g: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: 0x7474_7363), &g) == 0 && g & 1 == 1) // 'ttsc'
        }

        @Test func aChannelSpeaksAndSaysWhenItIsDone() {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            Self.done = []
            let chp = tb_NewPtr(4, false, true)
            #expect(tb_NewSpeechChannel(0, chp) == 0)
            let ch = rd32(chp)
            let proc = native_register(Self.doneProc, "speech done", 0)
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x7265_6663), 1234) == 0) // 'refc'
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x646E_6362), proc) == 0) // 'dncb'
            let rate = tb_NewPtr(4, false, true)
            #expect(tb_SetSpeechRate(ch, 200 << 16) == 0)
            #expect(tb_GetSpeechRate(ch, rate) == 0 && rd32(rate) == 200 << 16)
            let text = tb_NewPtr(32, false, true)
            for (i, b) in "Hello, world.".utf8.enumerated() { wr8(text + UInt32(i), b) }
            #expect(tb_SpeakText(ch, text, 13) == 0)
            let status = tb_NewPtr(8, false, true)
            #expect(tb_GetSpeechInfo(ch, Int32(bitPattern: 0x7374_6174), status) == 0 && rd8(status) == 1) // busy
            #expect(tb_SpeechBusy() == 0)                                  // over at the safe point
            #expect(Self.done == [1234])
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// Programs look their voice up by name (Mouth wants "Victoria, high
        /// quality") or by VoiceSpec; every voice is there, and any VoiceSpec
        /// opens a channel, since they all speak with the host's voice.
        @Test func everyVoiceIsThere() {
            launchTestApp()
            _ = initToolbox()
            let n = tb_NewPtr(2, false, true)
            #expect(tb_CountVoices(n) == 0)
            let spec = tb_NewPtr(8, false, true)
            let info = tb_NewPtr(362, false, true)
            var names: [String] = []
            for i in 1...Int(rd16(n)) {
                #expect(tb_GetIndVoice(Int16(i), spec) == 0)
                #expect(tb_GetVoiceDescription(spec, info, 362) == 0)
                names.append(pstring(at: info + 16))
            }
            for name in ["Victoria, high quality", "Bruce, high quality", "Agnes, high quality", "Victoria"] {
                #expect(names.contains(name))
            }
            #expect(tb_GetIndVoice(Int16(rd16(n)) + 1, spec) == -244) // voiceNotFound past the end
            #expect(tb_MakeVoiceSpec(Int32(bitPattern: 0x7878_7878), 7, spec) == 0) // 'xxxx', a voice we don't list
            #expect(tb_GetVoiceDescription(spec, info, 362) == 0)
            let chp = tb_NewPtr(4, false, true)
            #expect(tb_NewSpeechChannel(spec, chp) == 0)
            #expect(tb_SetSpeechInfo(rd32(chp), Int32(bitPattern: 0x6376_6F78), spec) == 0) // 'cvox'
            #expect(tb_DisposeSpeechChannel(rd32(chp)) == 0)
        }

        /// The host reports words but no phonemes: a channel's phoneme
        /// callbacks are made up from each word, ending with a silence.
        @Test func phonemesComeFromTheWords() {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let log = tempDir().appendingPathComponent("speech.txt").path
            #expect(machine_set_speech_record(log))
            defer { machine_set_speech_handler(nil, nil) }
            Self.phonemes = []
            let chp = tb_NewPtr(4, false, true)
            #expect(tb_NewSpeechChannel(0, chp) == 0)
            let ch = rd32(chp)
            let proc = native_register(Self.phonemeProc, "speech phoneme", 0)
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x7068_6362), proc) == 0) // 'phcb'
            let text = tb_NewPtr(32, false, true)
            for (i, b) in "Mom, why?".utf8.enumerated() { wr8(text + UInt32(i), b) }
            #expect(tb_SpeakText(ch, text, 9) == 0)
            #expect(tb_SpeechBusy() == 0)
            // m AA m, w IY, then silence (MacinTalk opcodes).
            #expect(Self.phonemes == [28, 11, 28, 38, 6, 0])
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// MacinTalk 1, as WordSpell's SuperCard externals use it: the glue
        /// opens "MacinTalk" (found in the System Folder by the poor man's
        /// search path), gives DRVR ".SPEECH" a free unit's ID and _Opens it
        /// with the SpeechRecord as the parameter block; the Reader's handle
        /// comes back at $34.  The Reader spells English in MacinTalk's code,
        /// MacinTalk() (a _Write of the phonemes) says the English it came
        /// from, or a program's own phonemes in the host's code; Control 2
        /// is the rate; Close takes the entry out of the unit table.
        @Test func macinTalkOneSpeaksThroughTheSpeechManager() {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let log = tempDir().appendingPathComponent("speech.txt").path
            #expect(machine_set_speech_record(log))
            defer { machine_set_speech_handler(nil, nil) }
            func code(_ words: [UInt16]) -> UInt32 {
                let p = tb_NewPtr(Int32(words.count * 2), false, false)
                for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
                return p
            }
            func long(_ v: UInt32) -> [UInt16] { [UInt16(v >> 16), UInt16(v & 0xFFFF)] }
            func pstr(_ s: String) -> UInt32 {
                let p = tb_NewPtr(256, false, true)
                guestPString(s, at: p)
                return p
            }
            // A device call through the trap, A0 the block.
            func io(_ trap: UInt16, _ pb: UInt32) { guest_call(code([0x41F9] + long(pb) + [trap, 0x4E75])) }

            #expect(tb_OpenResFile(pstr("Macintalk")) != -1) // not in the program's folder
            let drvr = tb_GetNamedResource(Int32(bitPattern: 0x4452_5652), pstr(".SPEECH")) // 'DRVR'
            #expect(drvr != 0)
            tb_SetResInfo(drvr, 25, 0)
            let rec = tb_NewPtr(0x64, false, true)
            wr32(rec + 0x12, pstr(".SPEECH"))
            wr32(rec + 0x3C, pstr(""))
            io(0xA000, rec) // _Open
            #expect(rd16(rec + 0x10) == 0 && Int16(bitPattern: rd16(rec + 0x18)) == -26)
            let reader = rd32(rec + 0x34)
            #expect(reader != 0 && rd32(reader) != 0)

            // Reader(englishInput, inputLength, phoneticOutput), entered as the glue jumps in.
            let text = tb_NewPtr(16, false, true)
            for (i, b) in "Cat".utf8.enumerated() { wr8(text + UInt32(i), b) }
            let phonemes = tb_NewHandle(0, false, true)
            let result = tb_NewPtr(2, false, true)
            guest_call(code([0x4267, 0x2F3C] + long(text) + [0x2F3C] + long(3) + [0x2F3C] + long(phonemes) +
                            [0x207C] + long(reader) + [0x2050, 0x4E90, 0x33DF] + long(result) + [0x4E75]))
            #expect(rd16(result) == 0)
            let n = Int(tb_GetHandleSize(phonemes))
            #expect(String(decoding: (0..<n).map { rd8(rd32(phonemes) + UInt32($0)) }, as: UTF8.self) == "KAE4T")

            // MacinTalk(theSpeech, phonemes): an immediate _Write; then a program's own phonemes, faster.
            wr32(rec + 0x20, phonemes)
            wr32(rec + 0x24, UInt32(n))
            io(0xA203, rec)
            #expect(rd16(rec + 0x10) == 0 && rd32(rec + 0x28) == UInt32(n))
            wr16(rec + 0x1A, 2)   // csCode: the rate
            wr16(rec + 0x1C, 200)
            io(0xA204, rec)
            let own = tb_NewHandle(8, false, true)
            for (i, b) in "/HEH4LOW".utf8.enumerated() { wr8(rd32(own) + UInt32(i), b) }
            wr32(rec + 0x20, own)
            wr32(rec + 0x24, 8)
            io(0xA203, rec)
            let said = (try? String(contentsOfFile: log, encoding: .utf8)) ?? ""
            #expect(said.contains("rate 150.0 pitch 45.0 modulation 35.0 volume 1.00: Cat\n"))
            #expect(said.contains("rate 200.0 pitch 45.0 modulation 35.0 volume 1.00: [[inpt PHON]]h1EHlOW[[inpt TEXT]]\n"))

            io(0xA001, rec) // _Close (SpeechOff)
            #expect(rd16(rec + 0x10) == 0 && rd32(rd32(UInt32(LM_UTableBase)) + 4 * 25) == 0)
        }
    }
}
