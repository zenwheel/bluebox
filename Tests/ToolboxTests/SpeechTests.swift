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
            // m AA m, w AY (the dictionary's), then silence (MacinTalk opcodes).
            #expect(Self.phonemes == [28, 11, 28, 38, 9, 0])
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// A host that speaks nothing and reports nothing: the test posts
        /// its words, and a clock of its own times them.
        nonisolated(unsafe) static var clock = 0.0
        nonisolated(unsafe) static var timed: [(Double, UInt16)] = []
        nonisolated(unsafe) static var a5s: [UInt32] = []
        static let timedProc: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let op = cpu_pop16(c)
            _ = cpu_pop32(c)
            _ = cpu_pop32(c)
            Speech.timed.append((Speech.clock, op))
            Speech.a5s.append(c.pointee.a.5)
            cpu_push32(c, ret)
        }

        /// A channel speaking `text` to a silent host, its phonemes timed.
        func timedChannel(_ text: String) -> (UInt32, UInt32) {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            machine_set_speech_handler({ _, _ in }, nil)
            machine_set_speech_clock { Speech.clock }
            Self.clock = 100
            Self.timed = []
            Self.a5s = []
            let chp = tb_NewPtr(4, false, true)
            #expect(tb_NewSpeechChannel(0, chp) == 0)
            let ch = rd32(chp)
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x7068_6362), native_register(Self.timedProc, "timed phoneme", 0)) == 0)
            let buf = tb_NewPtr(256, false, true)
            for (i, b) in text.utf8.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(tb_SpeakText(ch, buf, UInt32(text.utf8.count)) == 0)
            return (ch, buf)
        }

        /// Time passes in 5 ms steps, the Time Manager's pump running.
        func run(until t: Double) {
            while Self.clock < t - 1e-9 {
                Self.clock += 0.005
                time_pump()
            }
        }

        /// macOS reports words but no phonemes.  A word's made-up phonemes
        /// come at interrupt time (the program needn't call the Event
        /// Manager), spread over the word: vowels longer than stops, and
        /// the mouth closes at a pause, before the next word.  How long the
        /// words took sets the pace of the later ones.
        @Test func madeUpPhonemesKeepTimeWithTheWords() {
            let (ch, _) = timedChannel("lalala top, top top")
            defer { machine_set_speech_handler(nil, nil); machine_set_speech_clock(nil) }
            Self.clock = 100.5
            machine_speech_event(0, 1, 0, 6) // "lalala", heard a little before macOS says
            run(until: 100.51)
            #expect(Self.timed.map(\.1) == [27, 2]) // l AE, at once
            run(until: 100.83)
            #expect(Self.timed.map(\.1) == [27, 2, 27, 2, 27, 2]) // l AE l AE l AE
            let at = Self.timed.map(\.0)
            #expect(at[2] - at[1] > at[1] - at[0]) // AE outlasts l
            #expect(at[5] - at[0] > 0.25 && at[5] - at[0] < 0.5) // over the word, not at once
            // No word yet: the voice is still on this one, and they go round again.
            run(until: 101)
            #expect(Self.timed.count > 6 && Self.timed[6].1 == 27)
            // The next word comes half a second after the first: the pace is learnt.
            Self.timed = []
            machine_speech_event(0, 1, 7, 4) // "top,"
            run(until: 101.9)
            #expect(Self.timed.map(\.1) == [35, 11, 31, 0]) // t AA p, then the pause closes the mouth
            let top1 = Self.timed[3].0 - Self.timed[0].0
            Self.timed = []
            Self.clock = 102
            machine_speech_event(0, 1, 12, 3) // "top"
            run(until: 102.15)
            machine_speech_event(0, 1, 16, 3) // "top", sooner than expected
            run(until: 102.4)
            let ops = Self.timed.map(\.1)
            #expect(ops.count >= 6 && Array(ops.prefix(6)) == [35, 11, 31, 35, 11, 31]) // nothing lost
            #expect(Self.timed[5].0 - Self.timed[3].0 < top1) // faster now
            machine_speech_event(0, 0, 0, 0) // done
            run(until: 102.6)
            #expect(Self.timed.last?.1 == 0) // closed at the end
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// A word the voice spells out for far longer than its phonemes
        /// were guessed to take (a URL lasting 30 s): they go round again
        /// at the pace until the next word, which starts without a burst
        /// of what was left, and the outlier teaches the pace nothing.
        @Test func aLongWordKeepsTheMouthMoving() {
            let (ch, _) = timedChannel("go https://www.zenwheel.com/software/static/mouth.sit.bin go go")
            defer { machine_set_speech_handler(nil, nil); machine_set_speech_clock(nil) }
            machine_speech_event(0, 1, 0, 2) // "go": 99.93 (heard before macOS says) to 100.07, as guessed
            run(until: 100.07)
            machine_speech_event(0, 1, 3, 54) // the URL
            run(until: 130.07)
            let url = Self.timed.filter { $0.0 > 100.07 }.map(\.0)
            #expect(url.count > 300)
            #expect(zip(url, url.dropFirst()).map { $1 - $0 }.max()! < 0.25) // never still
            Self.timed = []
            machine_speech_event(0, 1, 58, 2) // "go"
            run(until: 130.075)
            #expect(Self.timed.map(\.1) == [23]) // g, and nothing of the URL's left over
            run(until: 130.3)
            #expect(Self.timed.count >= 2 && Self.timed[1].1 == 11)
            #expect(abs(Self.timed[1].0 - Self.timed[0].0 - 0.8 * 12.6 / 180) < 0.006) // the pace as it was
            machine_speech_event(0, 0, 0, 0)
            run(until: 130.4)
            #expect(Self.timed.last?.1 == 0)
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// The host's odd ranges: an address split in two ("bob@example",
        /// "org": the dot between said with the second), the word after it
        /// cut short ("tod" for "today."), and a word given twice.
        @Test func oddRangesFromTheHost() {
            let (ch, _) = timedChannel("mail bob@example.org today. 42 kB, ok")
            defer { machine_set_speech_handler(nil, nil); machine_set_speech_clock(nil) }
            machine_speech_event(0, 1, 5, 11) // "bob@example"
            run(until: 101)
            Self.timed = []
            machine_speech_event(0, 1, 17, 3) // "org"
            run(until: 101.5)
            #expect(Array(Self.timed.map(\.1).prefix(6)) == [20, 11, 35, 11, 32, 23]) // d AA t, AA r g
            Self.timed = []
            machine_speech_event(0, 1, 21, 3) // "tod"
            run(until: 102.8)
            #expect(Self.timed.map(\.1) == [35, 11, 20, 3, 0]) // t AA d EY, closed at the period
            Self.timed = []
            machine_speech_event(0, 1, 28, 5) // "42 kB"
            run(until: 102.9)
            machine_speech_event(0, 1, 28, 6) // "42 kB,": the same word, which goes on
            run(until: 104.6)
            #expect(Self.timed.map(\.1) == (spoken("42 kB,") + [0]).map(UInt16.init))
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// How the host's voice says a word: URLs, addresses, numbers,
        /// acronyms and symbols.
        func spoken(_ s: String, after prev: String? = nil, max: Int = 2048) -> [UInt8] {
            let t = Array(s.utf8), p = Array((prev ?? "").utf8)
            var out = [UInt8](repeating: 0xEE, count: max + 8)
            let n = t.withUnsafeBufferPointer { tb in
                p.withUnsafeBufferPointer { pb in
                    out.withUnsafeMutableBufferPointer { ob in
                        speech_spoken_phonemes(tb.baseAddress, Int32(t.count), prev == nil ? nil : pb.baseAddress,
                                               Int32(p.count), ob.baseAddress, Int32(max))
                    }
                }
            }
            #expect(out[max...].allSatisfy { $0 == 0xEE }) // nothing past the end
            return Array(out.prefix(Int(n)))
        }
        /// Phonemes in MacinTalk's symbols, and by the letter rules.
        func sym(_ s: String) -> [UInt8] {
            let t = Array(s.utf8)
            var out = [UInt8](repeating: 0, count: 1024)
            let n = speech_symbol_phonemes(t, Int32(t.count), &out, 1024)
            return Array(out.prefix(Int(n)))
        }
        func rules(_ s: String) -> [UInt8] {
            let t = Array(s.utf8)
            var out = [UInt8](repeating: 0, count: 1024)
            let n = speech_word_phonemes(t, Int32(t.count), &out, 1024)
            return Array(out.prefix(Int(n)))
        }

        @Test func wordsAreSaidAsTheVoiceSaysThem() {
            let fwd = sym("fAOrwUXrdslAES"), dot = sym("dAAt"), at = sym("AEt")
            // Numbers: cardinals, years (by the word before), money, decimals, ordinals.
            #expect(spoken("1984") == sym("nAYntIYnEYtIYfAOr"))
            #expect(spoken("2026") == sym("tUWTAWzAXndtwEHntIYsIHks"))
            #expect(spoken("2026", after: "In") == sym("twEHntIYtwEHntIYsIHks"))
            #expect(spoken("1,234,") == sym("wUXnTAWzAXndtUWhUXndrAXdTUXrtIYfAOr"))
            #expect(spoken("$19.99") == sym("nAYntIYndAAlUXrzAEndnAYntIYnAYnsEHnts"))
            #expect(spoken("3.14") == sym("TrIYpOYntwUXnfAOr"))
            #expect(spoken("-5") == sym("mAYnAXsfAYv"))
            #expect(spoken("50%") == sym("fIHftIYpUXrsEHnt"))
            #expect(spoken("21st") == sym("twEHntIYfUXrst"))
            #expect(spoken("40th") == sym("fAOrtIYIXT"))
            #expect(spoken("3:45") == sym("TrIYfAOrtIYfAYv"))
            #expect(spoken("007") == sym("OWOWsEHvAXn"))
            // Acronyms: spelled unless they can be said; symbols by name.
            #expect(spoken("FBI") == sym("EHfbIYAY"))
            #expect(spoken("NASA,") == rules("nasa"))
            #expect(spoken("JPEG") == sym("JEY") + rules("peg"))
            #expect(spoken("AT&T") == sym("EYtIYAEndtIY"))
            #expect(spoken("C++") == sym("sIYplUXsplUXs"))
            #expect(spoken("...") == [] && spoken("--") == [])
            // Addresses and URLs, part by part, unsayable parts spelled.
            #expect(spoken("bob@example.org.") == rules("bob") + [0] + at + rules("example") + [0] + dot + rules("org"))
            #expect(spoken("https://www.zenwheel.com/x") ==
                    sym("EYCtIYtIYpIYEHskOWlAXn") + fwd + fwd + sym("dUXbAXlyUWdUXbAXlyUWdUXbAXlyUW") + dot +
                    rules("zenwheel") + dot + rules("com") + fwd + sym("EHks"))
            #expect(spoken("file.txt") == rules("file") + dot + sym("tIYEHkstIY"))
            // Any length: a long word fills what there is room for, and no more.
            let long = "https://" + String(repeating: "ab.", count: 1000) + "com"
            #expect(spoken(long, max: 1024).count == 1024)
            #expect(spoken(String(repeating: "la", count: 500)).count > 900)
            #expect(spoken(long, max: 10).count == 10)
        }

        /// soCurrentA5: the callbacks run with the program's A5.  soStatus
        /// tells the phoneme being said; soPhonemeSymbols lists them.
        @Test func callbacksGetTheirA5AndTheStatusHasThePhoneme() {
            let (ch, _) = timedChannel("mom")
            defer { machine_set_speech_handler(nil, nil); machine_set_speech_clock(nil) }
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x6D79_4135), 0x0012_3450) == 0) // 'myA5'
            machine_speech_event(0, 1, 0, 3)
            run(until: 100.01)
            #expect(Self.a5s.first == 0x0012_3450)
            let status = tb_NewPtr(8, false, true)
            #expect(tb_GetSpeechInfo(ch, Int32(bitPattern: 0x7374_6174), status) == 0 && rd16(status + 6) == 28) // m
            let hp = tb_NewPtr(4, false, true)
            #expect(tb_GetSpeechInfo(ch, Int32(bitPattern: 0x7068_7379), hp) == 0) // 'phsy'
            let d = rd32(rd32(hp))
            #expect(rd16(d) == 42)
            let ae = d + 2 + 2 * 54 // opcode 2: AE, "bat" with the a lit
            #expect(rd16(ae) == 2 && pstring(at: ae + 2) == "AE" && pstring(at: ae + 18) == "bat")
            #expect(rd16(ae + 50) == 1 && rd16(ae + 52) == 2)
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// Phonemes given as input ([[inpt PHON]]) move the mouth as written:
        /// the host says words made of them, and its first ("good") is
        /// placed back on the phonemes it came from.
        @Test func phonemeInputMovesTheMouthAsWritten() {
            let text = "[[inpt PHON]]_gUXd\\>>>AY_m//////<<<AYt.[[inpt TEXT]]"
            let (ch, _) = timedChannel(text)
            defer { machine_set_speech_handler(nil, nil); machine_set_speech_clock(nil) }
            machine_speech_event(0, 1, 0, 4) // "good", the host's first word
            run(until: 101)
            #expect(Self.timed.map(\.1) == [23, 14, 20, 0]) // g UX d, closed when no word comes
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// What a channel gave the record backend to say.
        func said(_ text: String, inputMode: UInt32? = nil) -> String {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let log = tempDir().appendingPathComponent("said.txt").path
            #expect(machine_set_speech_record(log))
            defer { machine_set_speech_handler(nil, nil) }
            let chp = tb_NewPtr(4, false, true)
            #expect(tb_NewSpeechChannel(0, chp) == 0)
            if let mode = inputMode {
                let m = tb_NewPtr(4, false, true)
                wr32(m, mode)
                #expect(tb_SetSpeechInfo(rd32(chp), Int32(bitPattern: 0x696E_7074), m) == 0) // 'inpt'
            }
            let buf = tb_NewPtr(Int32(text.utf8.count + 1), false, true)
            for (i, b) in text.utf8.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(tb_SpeakText(rd32(chp), buf, UInt32(text.utf8.count)) == 0)
            _ = tb_SpeechBusy()
            #expect(tb_DisposeSpeechChannel(rd32(chp)) == 0)
            let lines = ((try? String(contentsOfFile: log, encoding: .utf8)) ?? "").split(separator: "\n")
            guard let line = lines.first(where: { $0.contains(" speak ") }), let colon = line.range(of: ": ") else { return "" }
            return String(line[colon.upperBound...])
        }

        /// The host's voices read phonemes out as letters, so phonemes given
        /// as input are said as the words they make: several words written
        /// together, stress, pitch and length marks, doubled sounds, near
        /// misses (a schwa for a full vowel, a dropped r) and the embedded
        /// commands around them; what no word matches is spelt by its sounds.
        @Test func phonemeInputIsSaidAsWords() {
            #expect(said("[[inpt PHON]]_hEHlOW _w1UXrld.[[inpt TEXT]] Bye now.") == "hello world. Bye now.")
            #expect(said("[[rate 200; inpt PHON]]1AYm=2sOW _sl>>>IYIYIYp1IY[[inpt TEXT]]") == "[[rate 200]]I'm so sleepy")
            #expect(said("[[inpt PHON]]mEYkmIY AX sAEndwIHC, %%plIYz?") == "make me a sandwich, please?")
            #expect(said("[[inpt PHON]]_D1AX _k1AEts _s1AEt _AAn _D1AX _mAEt") == "the cats sat on the mat")
            #expect(said("[[inpt PHON]]AY tAYrd AXv wEYtIHN") == "I tired of waiting") // tired, waiting: nearly
            #expect(said("[[inpt PHON]]+gOW") == "[[emph +]] go")
            #expect(said("[[inpt PHON]]zgrOWfk") == "zgrohfk") // no such word: spelt by its sounds
            #expect(said("hEHlOW wUXrld", inputMode: 0x5048_4F4E) == "hello world") // the 'PHON' input mode
            #expect(said("Plain text, [[rate 150]]untouched.") == "Plain text, [[rate 150]]untouched.")
        }

        /// Word callbacks give the program's offsets: a word the host says
        /// is the phonemes it came from in the program's text.
        nonisolated(unsafe) static var words: [(UInt32, UInt16)] = []
        static let wordProc: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let len = cpu_pop16(c)
            let pos = cpu_pop32(c)
            _ = cpu_pop32(c)
            _ = cpu_pop32(c)
            Speech.words.append((pos, len))
            cpu_push32(c, ret)
        }

        @Test func wordsOfPhonemesAreThePrograms() {
            let text = "[[inpt PHON]]mEYkmIY hAEpIY"
            let (ch, _) = timedChannel(text)
            defer { machine_set_speech_handler(nil, nil); machine_set_speech_clock(nil) }
            Self.words = []
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x7764_6362), native_register(Self.wordProc, "speech word", 0)) == 0)
            // The host says "make me happy": its words, as it counts them.
            machine_speech_event(0, 1, 0, 4)
            machine_speech_event(0, 1, 5, 2)
            machine_speech_event(0, 1, 8, 5)
            run(until: 100.2)
            #expect(Self.words.map(\.0) == [13, 17, 21] && Self.words.map(\.1) == [4, 3, 6]) // mEYk, mIY, hAEpIY
            #expect(Self.timed.first?.1 == 28) // m: the mouth moves to the phonemes written
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }

        /// macOS no longer turns text into phonemes: TextToPhonemes has ours,
        /// the dictionary's or else by the letters.  Given back as input,
        /// they are said as the text they came from.
        @Test func textToPhonemesOfOurOwn() {
            launchTestApp()
            _ = initToolbox()
            let chp = tb_NewPtr(4, false, true)
            #expect(tb_NewSpeechChannel(0, chp) == 0)
            let text = tb_NewPtr(16, false, true)
            for (i, b) in "Mom, why?".utf8.enumerated() { wr8(text + UInt32(i), b) }
            let h = tb_NewHandle(0, false, true)
            let len = tb_NewPtr(4, false, true)
            #expect(tb_TextToPhonemes(rd32(chp), text, 9, h, len) == 0)
            let n = Int(rd32(len))
            #expect(String(decoding: (0..<n).map { rd8(rd32(h) + UInt32($0)) }, as: UTF8.self) == "mAAm, wAY?")
            for (i, b) in "Blorft".utf8.enumerated() { wr8(text + UInt32(i), b) }
            #expect(tb_TextToPhonemes(rd32(chp), text, 6, h, len) == 0)
            let blorft = String(decoding: (0..<Int(rd32(len))).map { rd8(rd32(h) + UInt32($0)) }, as: UTF8.self)
            #expect(blorft == "blAArft") // not a word: by the letters
            let log = tempDir().appendingPathComponent("t2p.txt").path
            #expect(machine_set_speech_record(log))
            defer { machine_set_speech_handler(nil, nil) }
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let input = "[[inpt PHON]]" + blorft + "[[inpt TEXT]]"
            let buf = tb_NewPtr(64, false, true)
            for (i, b) in input.utf8.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(tb_SpeakText(rd32(chp), buf, UInt32(input.utf8.count)) == 0)
            _ = tb_SpeechBusy()
            #expect(((try? String(contentsOfFile: log, encoding: .utf8)) ?? "").contains(": Blorft\n"))
            #expect(tb_DisposeSpeechChannel(rd32(chp)) == 0)
        }

        /// MacinTalk 1, as WordSpell's SuperCard externals use it: the glue
        /// opens "MacinTalk" (found in the System Folder by the poor man's
        /// search path), gives DRVR ".SPEECH" a free unit's ID and _Opens it
        /// with the SpeechRecord as the parameter block; the Reader's handle
        /// comes back at $34.  The Reader spells English in MacinTalk's code,
        /// MacinTalk() (a _Write of the phonemes) says the English it came
        /// from, or a program's own phonemes as the words they make; Control 2
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
            #expect(said.contains("rate 200.0 pitch 45.0 modulation 35.0 volume 1.00: hello\n"))
            // A phrase of the program's own, in MacinTalk's code (several words
            // run together, doubled sounds, stress digits, its end mark).
            let phrase = "WAH5TS FOHR DIH4NER, QXQX MAA4M? AY5M SOW SLIY6IY5IY4PIY.#"
            let h2 = tb_NewHandle(Int32(phrase.utf8.count), false, true)
            for (i, b) in phrase.utf8.enumerated() { wr8(rd32(h2) + UInt32(i), b) }
            wr32(rec + 0x20, h2)
            wr32(rec + 0x24, UInt32(phrase.utf8.count))
            io(0xA203, rec)
            let said2 = (try? String(contentsOfFile: log, encoding: .utf8)) ?? ""
            #expect(said2.contains(": what's for dinner, mom? I'm so sleepy.\n"))

            io(0xA001, rec) // _Close (SpeechOff)
            #expect(rd16(rec + 0x10) == 0 && rd32(rd32(UInt32(LM_UTableBase)) + 4 * 25) == 0)
        }
    }
}
