import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Language Analysis Manager on the host's word breaker and the Dictionary Manager's files.
    @Suite struct CarbonLanguageAnalysis {
        func pstr(_ s: String) -> UInt32 { svcPStr(s) }

        func environment() -> UInt32 {
            let count = svcPtr(4), list = svcPtr(16)
            #expect(svcCall("LAGetEnvironmentList", 4, count, list) == 0 && rd32(count) == 1)
            return rd32(list)
        }

        /// The bundle's one path as (offset, length, part of speech) per morpheme, with its homograph records.
        func morphemes(_ bundle: UInt32) -> [(UInt32, UInt32, UInt32, UInt32)] {
            let paths = svcPtr(8), path = svcPtr(8), list = svcPtr(8), n = svcPtr(4)
            #expect(tb_AEGetParamDesc(bundle, Int32(bitPattern: fourCC("lmfp")), Int32(bitPattern: fourCC("list")), paths) == 0)
            #expect(tb_AECountItems(paths, n) == 0 && rd32(n) == 1)
            #expect(tb_AEGetNthDesc(paths, 1, Int32(bitPattern: fourCC("reco")), 0, path) == 0)
            #expect(tb_AEGetParamDesc(path, Int32(bitPattern: fourCC("lmfn")), Int32(bitPattern: fourCC("list")), list) == 0)
            #expect(tb_AECountItems(list, n) == 0)
            var out: [(UInt32, UInt32, UInt32, UInt32)] = []
            for i in 0..<Int(rd32(n)) {
                let morpheme = svcPtr(8), homograph = svcPtr(8), range = svcPtr(8), pos = svcPtr(8), dic = svcPtr(8)
                #expect(tb_AEGetNthDesc(list, Int32(i + 1), Int32(bitPattern: fourCC("list")), 0, morpheme) == 0)
                #expect(tb_AEGetNthDesc(morpheme, 1, Int32(bitPattern: fourCC("reco")), 0, homograph) == 0)
                #expect(tb_AEGetParamDesc(homograph, Int32(bitPattern: fourCC("lamt")), Int32(bitPattern: fourCC("****")), range) == 0)
                #expect(tb_AEGetParamDesc(homograph, Int32(bitPattern: fourCC("lamc")), Int32(bitPattern: fourCC("****")), pos) == 0)
                let r = rd32(range + 4), p = rd32(pos + 4)
                let hasDict = tb_AEGetParamDesc(homograph, Int32(bitPattern: fourCC("lahd")), Int32(bitPattern: fourCC("****")), dic) == 0
                out.append((rd32(rd32(r)), rd32(rd32(r) + 4), rd32(rd32(p)), hasDict ? 1 : 0))
            }
            return out
        }

        @Test func environmentsAndContexts() {
            bootMachine()
            let env = environment()
            let name = svcPtr(64), ref = svcPtr(4)
            #expect(svcCall("LAGetEnvironmentName", env, name) == 0 && rd8(name) > 0)
            #expect(svcCall("LAGetEnvironmentRef", name, ref) == 0 && rd32(ref) == env)
            #expect(svcStatus(svcCall("LAGetEnvironmentRef", pstr("Klingon"), ref)) == -6986)
            // A custom environment is its own, with the dictionaries of its base
            let custom = svcPtr(4)
            #expect(svcCall("LACreateCustomEnvironment", env, pstr("Mine"), 0, custom) == 0)
            #expect(svcStatus(svcCall("LACreateCustomEnvironment", env, pstr("Mine"), 0, svcPtr(4))) == -6987)
            let count = svcPtr(4), list = svcPtr(16)
            #expect(svcCall("LAGetEnvironmentList", 4, count, list) == 0 && rd32(count) == 2)
            let ctx = svcPtr(4)
            #expect(svcCall("LAOpenAnalysisContext", rd32(custom), ctx) == 0)
            #expect(svcStatus(svcCall("LADeleteCustomEnvironment", rd32(custom))) == -6985) // in use
            #expect(svcCall("LACloseAnalysisContext", rd32(ctx)) == 0)
            #expect(svcCall("LADeleteCustomEnvironment", rd32(custom)) == 0)
            #expect(svcStatus(svcCall("LADeleteCustomEnvironment", env)) == -6985) // the built-in one stays
            #expect(svcStatus(svcCall("LAOpenAnalysisContext", 77, ctx)) == -6986)
            #expect(svcCall("LALibraryVersion") != 0)
        }

        @Test func japaneseTextIsSplitIntoMorphemes() {
            bootMachine()
            let env = environment()
            let ctx = svcPtr(4)
            #expect(svcCall("LAOpenAnalysisContext", env, ctx) == 0)
            let (text, n) = svcUTF16("私は学生です。")
            let bundle = svcPtr(8)
            #expect(svcCall("LAMorphemeAnalysis", rd32(ctx), text, n, 0, 0, 1, bundle) == 0)
            let ms = morphemes(bundle)
            #expect(ms.count >= 3)
            // They cover the text, in order, without gaps (offsets and lengths in bytes)
            var at: UInt32 = 0
            for m in ms { #expect(m.0 == at && m.1 > 0); at += m.1 }
            #expect(at == n * 2)
            #expect(ms.last!.2 == 0xA110) // 。 is a kuten
            #expect(ms.allSatisfy { $0.3 == 0 }) // none from a dictionary
            // Latin text and numbers are told apart
            let (t2, n2) = svcUTF16("ABC 123")
            #expect(svcCall("LAMorphemeAnalysis", rd32(ctx), t2, n2, 0, 0, 1, bundle) == 0)
            let m2 = morphemes(bundle)
            #expect(m2.map { $0.2 }.contains(0xA300) && m2.map { $0.2 }.contains(0xA200))
            #expect(svcStatus(svcCall("LAMorphemeAnalysis", 5, t2, n2, 0, 0, 1, bundle)) == -6988)
        }

        @Test func continuousAnalysisAndShifting() {
            bootMachine()
            let env = environment()
            let ctx = svcPtr(4), modified = svcPtr(1)
            #expect(svcCall("LAOpenAnalysisContext", env, ctx) == 0)
            let (a, na) = svcUTF16("今日は")
            #expect(svcCall("LAContinuousMorphemeAnalysis", rd32(ctx), a, na, 0, 0, 0, modified) == 0 && rd8(modified) == 1)
            let (b, nb) = svcUTF16("晴れです。")
            #expect(svcCall("LAContinuousMorphemeAnalysis", rd32(ctx), b, nb, 1, 0, 0, modified) == 0)
            let path = svcPtr(8)
            #expect(svcCall("LAGetMorphemes", rd32(ctx), path) == 0)
            let first = svcPtr(8), shifted = svcPtr(4)
            #expect(svcCall("LAShiftMorphemes", rd32(ctx), 1, first, shifted) == 0 && rd32(shifted) > 0)
            // What was shifted off is gone from the text
            #expect(svcCall("LAGetMorphemes", rd32(ctx), path) == 0)
            #expect(svcCall("LAResetAnalysis", rd32(ctx)) == 0)
            #expect(svcStatus(svcCall("LAGetMorphemes", rd32(ctx), path)) == -6989)
            #expect(svcStatus(svcCall("LAShiftMorphemes", rd32(ctx), 1, first, shifted)) == -6989)
        }

        @Test func openedDictionariesGiveWords() {
            launchTestApp()
            // A dictionary with the Japanese fields, holding a made-up compound the host would split
            func fieldInfo(_ tag: String, _ type: String, attrs: UInt32 = 0, find: [UInt32] = []) -> UInt32 {
                let rec = svcPtr(8), methods = svcPtr(max(4, find.count * 4))
                for (i, m) in find.enumerated() { wr32(methods + UInt32(i * 4), m) }
                #expect(svcCall("DCMCreateFieldInfoRecord", fourCC(tag), fourCC(type), 0, attrs, 0, UInt32(find.count), methods, rec) == 0)
                return rec
            }
            let list = svcPtr(8)
            #expect(tb_AECreateList(0, 0, 0, list) == 0)
            for i in [fieldInfo("yomi", "utxt", attrs: 1 | 4, find: [0x3D20_2020]), fieldInfo("hyok", "utxt", attrs: 1, find: [0x3D20_2020]),
                      fieldInfo("hins", "hins")] { #expect(tb_AEPutDesc(list, 0, i) == 0) }
            let spec = svcPtr(70), id = svcPtr(4)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr("JWords"), spec)
            #expect(svcCall("DCMNewDictionary", 0, spec, 0, list, 0, 10, id) == 0)
            let env = environment()
            #expect(svcStatus(svcCall("LAOpenDictionary", env, spec)) == 0)
            let count = svcPtr(4), specs = svcPtr(70 * 4), opened = svcPtr(4)
            #expect(svcCall("LAListAvailableDictionaries", env, 4, count, specs, opened) == 0 && rd32(count) == 1 && rd8(opened) == 1)
            // A word added through the manager
            func utf16Bytes(_ s: String) -> [UInt8] { Array(s.utf16).flatMap { [UInt8($0 >> 8), UInt8($0 & 0xFF)] } }
            let rec = svcPtr(8)
            #expect(tb_AECreateList(0, 0, 1, rec) == 0)
            for (tag, type, bytes) in [("yomi", "utxt", utf16Bytes("あおはこたんけん")), ("hyok", "utxt", utf16Bytes("青箱探検")),
                                       ("hins", "hins", [0, 0, 0x05, 0])] {
                let b = svcPtr(bytes.count)
                for (i, x) in bytes.enumerated() { wr8(b + UInt32(i), x) }
                #expect(tb_AEPutParamPtr(rec, Int32(bitPattern: fourCC(tag)), Int32(bitPattern: fourCC(type)), b, Int32(bytes.count)) == 0)
            }
            #expect(svcCall("LAAddNewWord", env, spec, rec) == 0)
            #expect(svcStatus(svcCall("LAAddNewWord", env, spec, rec)) == -7109) // the Dictionary Manager's duplicate
            let ctx = svcPtr(4), bundle = svcPtr(8)
            #expect(svcCall("LAOpenAnalysisContext", env, ctx) == 0)
            let (text, n) = svcUTF16("私は青箱探検が好き")
            #expect(svcCall("LAMorphemeAnalysis", rd32(ctx), text, n, 0, 0, 1, bundle) == 0)
            let ms = morphemes(bundle)
            let word = ms.first { $0.3 == 1 }
            #expect(word != nil && word!.1 == 8 && word!.2 == 0x0500 && word!.0 == 4) // 青箱探検: 4 characters from the third
            var at: UInt32 = 0
            for m in ms { #expect(m.0 == at); at += m.1 }
            #expect(at == n * 2)
            // Closed, it no longer counts
            #expect(svcCall("LACloseDictionary", env, spec) == 0)
            #expect(svcStatus(svcCall("LACloseDictionary", env, spec)) == -6992)
            #expect(svcCall("LAMorphemeAnalysis", rd32(ctx), text, n, 0, 0, 1, bundle) == 0)
            #expect(morphemes(bundle).allSatisfy { $0.3 == 0 })
            // A file that is not a dictionary of words
            let plain = svcPtr(70)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr("Plain"), plain)
            #expect(tb_FSpCreate(plain, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("TEXT")), 0) == 0)
            #expect(svcStatus(svcCall("LAOpenDictionary", env, plain)) == -6993)
        }

        @Test func textToMorphemesFillsABuffer() {
            bootMachine()
            let env = environment()
            let ctx = svcPtr(4)
            #expect(svcCall("LAOpenAnalysisContext", env, ctx) == 0)
            // Shift-JIS: 私は学生です。
            let sjis: [UInt8] = [0x8E, 0x84, 0x82, 0xCD, 0x8A, 0x77, 0x90, 0xB6, 0x82, 0xC5, 0x82, 0xB7, 0x81, 0x42]
            let src = svcPtr(sjis.count)
            for (i, b) in sjis.enumerated() { wr8(src + UInt32(i), b) }
            let buf = svcPtr(1024), accepted = svcPtr(4)
            let sjisEncoding: UInt32 = 1 // kTextEncodingMacJapanese
            #expect(svcCall("LATextToMorphemes", rd32(ctx), sjisEncoding, UInt32(sjis.count), src, 1024, 1, 0, accepted, buf) == 0)
            let count = rd32(buf)
            #expect(count >= 3 && rd32(accepted) == UInt32(sjis.count) && rd32(buf + 4) == UInt32(sjis.count))
            // The morphemes' texts are in the buffer, in the encoding asked for
            var joined: [UInt8] = []
            for i in 0..<Int(count) {
                let rec = buf + 12 + 20 * UInt32(i)
                joined += svcBytes(rd32(rec + 4), Int(rd32(rec)))
            }
            #expect(joined == sjis)
            #expect(rd32(buf + 12 + 20 * (count - 1) + 16) == 0xA110)
            // Without the end flag the last word waits for more text
            #expect(svcCall("LATextToMorphemes", rd32(ctx), sjisEncoding, UInt32(sjis.count), src, 1024, 0, 0, accepted, buf) == 0)
            #expect(rd32(buf) == count - 1 && rd32(accepted) == UInt32(sjis.count) - 2)
            // Too small a buffer for any; a structure version we don't know
            #expect(svcStatus(svcCall("LATextToMorphemes", rd32(ctx), sjisEncoding, UInt32(sjis.count), src, 20, 1, 0, accepted, buf)) == -6984)
            #expect(svcStatus(svcCall("LATextToMorphemes", rd32(ctx), sjisEncoding, UInt32(sjis.count), src, 1024, 1, 9, accepted, buf)) == -50)
        }
    }
}
