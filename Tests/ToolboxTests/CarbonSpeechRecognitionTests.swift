import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Speech Recognition Manager: language objects, with no recognizer to listen with.
    @Suite struct CarbonSpeechRecognition {
        func bytes(_ s: String) -> UInt32 {
            let p = svcPtr(max(1, s.utf8.count))
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func system() -> UInt32 {
            let out = svcPtr(4)
            #expect(svcCall("SROpenRecognitionSystem", out, 0) == 0 && rd32(out) != 0)
            return rd32(out)
        }

        func spelling(_ obj: UInt32) -> String {
            let buf = svcPtr(64), len = svcPtr(4)
            wr32(len, 64)
            #expect(svcCall("SRGetProperty", obj, fourCC("spel"), buf, len) == 0)
            return String(decoding: svcBytes(buf, Int(rd32(len))), as: UTF8.self)
        }

        func newWord(_ sys: UInt32, _ text: String) -> UInt32 {
            let w = svcPtr(4)
            #expect(svcCall("SRNewWord", sys, w, bytes(text), UInt32(text.utf8.count)) == 0)
            return rd32(w)
        }

        @Test func systemsAndRecognizers() {
            bootMachine()
            let sys = system()
            let out = svcPtr(4)
            #expect(svcStatus(svcCall("SROpenRecognitionSystem", out, 77)) == -5102 && rd32(out) == 0)
            // No recognizer can be made; what needs one finds none
            #expect(svcStatus(svcCall("SRNewRecognizer", sys, out, 0)) == -5100 && rd32(out) == 0)
            #expect(svcStatus(svcCall("SRNewRecognizer", 999, out, 0)) == -5104)
            #expect(svcStatus(svcCall("SRStartListening", sys)) == -5105)
            #expect(svcStatus(svcCall("SRContinueRecognition", 999)) == -5104)
            #expect(svcCall("SRSpeechBusy", sys) == 0 && svcCall("SRIdle") == 0)
            // The system answers its feedback modes
            let modes = svcPtr(4), len = svcPtr(4)
            wr32(len, 4)
            #expect(svcCall("SRGetProperty", sys, fourCC("fbwn"), modes, len) == 0 && rd32(modes) == 2)
            #expect(svcStatus(svcCall("SRGetProperty", sys, fourCC("zzzz"), modes, len)) == -5112)
            #expect(svcCall("SRCloseRecognitionSystem", sys) == 0)
            #expect(svcStatus(svcCall("SRCloseRecognitionSystem", sys)) == -5125) // already released
            #expect(svcStatus(svcCall("SRNewWord", sys, out, bytes("x"), 1)) == -5125)
        }

        @Test func modelsPathsPhrasesAndWords() {
            bootMachine()
            let sys = system()
            let model = svcPtr(4), path = svcPtr(4), phrase = svcPtr(4), count = svcPtr(4), item = svcPtr(4)
            #expect(svcCall("SRNewLanguageModel", sys, model, bytes("Commands"), 8) == 0)
            #expect(svcCall("SRNewPath", sys, path) == 0)
            #expect(svcCall("SRNewPhrase", sys, phrase, bytes("open the door"), 13) == 0)
            #expect(spelling(rd32(phrase)) == "open the door")
            let word = newWord(sys, "please")
            // A word holds nothing; the others take what fits
            #expect(svcStatus(svcCall("SRCountItems", word, count)) == -5122)
            #expect(svcStatus(svcCall("SRAddLanguageObject", word, rd32(phrase))) == -5131)
            #expect(svcStatus(svcCall("SRAddLanguageObject", rd32(phrase), rd32(path))) == -5131) // a phrase takes words
            #expect(svcCall("SRAddLanguageObject", rd32(path), rd32(phrase)) == 0)
            #expect(svcCall("SRAddLanguageObject", rd32(path), word) == 0)
            #expect(svcCall("SRAddLanguageObject", rd32(model), rd32(path)) == 0)
            #expect(svcStatus(svcCall("SRAddLanguageObject", rd32(path), rd32(model))) == -5131) // not into itself, round
            #expect(svcCall("SRCountItems", rd32(path), count) == 0 && rd32(count) == 2)
            #expect(svcCall("SRGetIndexedItem", rd32(path), item, 1) == 0 && spelling(rd32(item)) == "please")
            #expect(svcStatus(svcCall("SRGetIndexedItem", rd32(path), item, 2)) == -5123)
            // Text added makes a word or a phrase, with the refCon
            #expect(svcCall("SRAddText", rd32(model), bytes("stop"), 4, 77) == 0)
            #expect(svcCall("SRAddText", rd32(model), bytes("go away"), 7, 78) == 0)
            #expect(svcCall("SRCountItems", rd32(model), count) == 0 && rd32(count) == 3)
            #expect(svcCall("SRGetIndexedItem", rd32(model), item, 1) == 0)
            let lmtp = svcPtr(4), len = svcPtr(4)
            wr32(len, 4)
            #expect(svcCall("SRGetProperty", rd32(item), fourCC("lmtp"), lmtp, len) == 0 && rd32(lmtp) == fourCC("word"))
            let refcon = svcPtr(4)
            wr32(len, 4)
            #expect(svcCall("SRGetProperty", rd32(item), fourCC("refc"), refcon, len) == 0 && rd32(refcon) == 77)
            #expect(svcCall("SRGetIndexedItem", rd32(model), item, 2) == 0)
            wr32(len, 4)
            #expect(svcCall("SRGetProperty", rd32(item), fourCC("lmtp"), lmtp, len) == 0 && rd32(lmtp) == fourCC("phra"))
            // Replace, remove
            #expect(svcCall("SRSetIndexedItem", rd32(model), word, 1) == 0)
            #expect(svcCall("SRRemoveIndexedItem", rd32(model), 2) == 0)
            #expect(svcCall("SRCountItems", rd32(model), count) == 0 && rd32(count) == 2)
            #expect(svcCall("SRRemoveLanguageObject", rd32(model), word) == 0)
            #expect(svcStatus(svcCall("SRRemoveLanguageObject", rd32(model), word)) == -5123)
            #expect(svcCall("SRCountItems", rd32(model), count) == 0 && rd32(count) == 1)
            #expect(svcCall("SREmptyLanguageObject", rd32(model)) == 0)
            #expect(svcCall("SRCountItems", rd32(model), count) == 0 && rd32(count) == 0)
            // Changing words
            #expect(svcCall("SRChangeLanguageObject", word, bytes("thanks"), 6) == 0 && spelling(word) == "thanks")
            #expect(svcStatus(svcCall("SRChangeLanguageObject", rd32(path), bytes("x"), 1)) == -5111)
        }

        @Test func propertiesAndReferences() {
            bootMachine()
            let sys = system()
            let word = newWord(sys, "stop")
            let b = svcPtr(4), len = svcPtr(4)
            // Defaults, then set
            wr32(len, 1)
            #expect(svcCall("SRGetProperty", word, fourCC("enbl"), b, len) == 0 && rd8(b) == 1)
            wr8(b, 1)
            #expect(svcCall("SRSetProperty", word, fourCC("optl"), b, 1) == 0)
            wr32(len, 1)
            #expect(svcCall("SRGetProperty", word, fourCC("optl"), b, len) == 0 && rd8(b) == 1)
            wr16(b, 80)
            #expect(svcCall("SRSetProperty", word, fourCC("rjct"), b, 2) == 0)
            wr32(len, 2)
            #expect(svcCall("SRGetProperty", word, fourCC("rjct"), b, len) == 0 && rd16(b) == 80)
            #expect(svcStatus(svcCall("SRSetProperty", word, fourCC("rjct"), b, 3)) == -5111) // wrong size
            wr32(len, 1)
            #expect(svcStatus(svcCall("SRGetProperty", word, fourCC("spel"), b, len)) == -5108 && rd32(len) == 4)
            // References are each their own; the object lasts until the last is released
            let again = svcPtr(4)
            #expect(svcCall("SRGetReference", word, again) == 0 && rd32(again) != word)
            #expect(svcCall("SRReleaseObject", word) == 0)
            #expect(spelling(rd32(again)) == "stop")
            #expect(svcStatus(svcCall("SRReleaseObject", word)) == -5125)
            #expect(svcCall("SRReleaseObject", rd32(again)) == 0)
            #expect(svcStatus(svcCall("SRReleaseObject", rd32(again))) == -5125)
            #expect(svcStatus(svcCall("SRReleaseObject", 12)) == -5104)
        }

        @Test func languageObjectsSaveAndLoad() {
            launchTestApp()
            let sys = system()
            let model = svcPtr(4), path = svcPtr(4), count = svcPtr(4)
            #expect(svcCall("SRNewLanguageModel", sys, model, bytes("Colors"), 6) == 0)
            #expect(svcCall("SRNewPath", sys, path) == 0)
            for (i, w) in ["red", "green", "blue"].enumerated() {
                #expect(svcCall("SRAddText", rd32(path), bytes(w), UInt32(w.utf8.count), UInt32(100 + i)) == 0)
            }
            #expect(svcCall("SRAddLanguageObject", rd32(model), rd32(path)) == 0)
            let flag = svcPtr(1)
            wr8(flag, 1)
            #expect(svcCall("SRSetProperty", rd32(path), fourCC("rptb"), flag, 1) == 0)
            // To a handle and back
            let h = tb_NewHandle(0, false, true)
            #expect(svcCall("SRPutLanguageObjectIntoHandle", rd32(model), h) == 0 && tb_GetHandleSize(h) > 20)
            let copy = svcPtr(4)
            #expect(svcCall("SRNewLanguageObjectFromHandle", sys, copy, h) == 0 && spelling(rd32(copy)) == "Colors")
            #expect(svcCall("SRCountItems", rd32(copy), count) == 0 && rd32(count) == 1)
            let p2 = svcPtr(4), w2 = svcPtr(4), b = svcPtr(4), len = svcPtr(4)
            #expect(svcCall("SRGetIndexedItem", rd32(copy), p2, 0) == 0 && svcCall("SRCountItems", rd32(p2), count) == 0 && rd32(count) == 3)
            wr32(len, 1)
            #expect(svcCall("SRGetProperty", rd32(p2), fourCC("rptb"), b, len) == 0 && rd8(b) == 1)
            #expect(svcCall("SRGetIndexedItem", rd32(p2), w2, 2) == 0 && spelling(rd32(w2)) == "blue")
            wr32(len, 4)
            #expect(svcCall("SRGetProperty", rd32(w2), fourCC("refc"), b, len) == 0 && rd32(b) == 102)
            // Damaged data
            let junk = tb_NewHandle(6, false, true)
            #expect(svcStatus(svcCall("SRNewLanguageObjectFromHandle", sys, copy, junk)) == -5133 && rd32(copy) == 0)
            tb_SetHandleSize(h, 30)
            #expect(svcStatus(svcCall("SRNewLanguageObjectFromHandle", sys, copy, h)) == -5133)
            // To a data file and back
            let spec = svcPtr(70), ref = svcPtr(2)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr("Colors.lm"), spec)
            #expect(tb_FSpCreate(spec, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("lmfm")), 0) == 0)
            #expect(tb_FSpOpenDF(spec, 3, ref) == 0)
            let file = Int16(bitPattern: rd16(ref))
            #expect(svcCall("SRPutLanguageObjectIntoDataFile", rd32(model), UInt32(UInt16(bitPattern: file))) == 0)
            let pb = svcPtr(Int(IOParam_SIZEOF))
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: file))
            wr16(pb + UInt32(IOParam_ioPosMode_off), 1)
            wr32(pb + UInt32(IOParam_ioPosOffset_off), 0)
            #expect(tb_PBSetFPos(pb, false) == 0)
            #expect(svcCall("SRNewLanguageObjectFromDataFile", sys, copy, UInt32(UInt16(bitPattern: file))) == 0)
            #expect(spelling(rd32(copy)) == "Colors")
            #expect(tb_PBClose(pb, false) == 0)
        }
    }
}
