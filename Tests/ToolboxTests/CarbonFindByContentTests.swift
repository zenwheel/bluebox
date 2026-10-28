import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Find By Content: indexing documents' text and searching it.
    @Suite struct CarbonFindByContent {
        func cstr(_ s: String) -> UInt32 {
            let p = svcPtr(s.utf8.count + 2)
            for (i, b) in Array(s.utf8).enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func spec(_ name: String) -> UInt32 {
            let p = svcPtr(70)
            #expect(tb_FSMakeFSSpec(0, 0, svcPStr(name), p) == 0)
            return p
        }

        func words(_ listp: UInt32, _ count: UInt32) -> [String] {
            (0..<Int(count)).map { i in
                let w = rd32(listp + UInt32(4 * i))
                var n = 0
                while rd8(w + UInt32(n)) != 0 { n += 1 }
                return String(decoding: svcBytes(w, n), as: UTF8.self)
            }
        }

        func setup() -> (URL, UInt32, UInt32) {
            let dir = launchTestApp()
            let texts = [
                "cats.txt": "Cats and dogs are pets. Cats purr, dogs bark. Many people keep cats as pets, and cats sleep a lot.",
                "ships.txt": "Ships sail the sea. The ship crossed the sea in a storm; sailors on the sea watch the waves.",
                "mixed.txt": "A cat watched the ship leave. The sea was calm, the ship was slow.",
                "empty.txt": "",
            ]
            for (name, text) in texts { try? Data(text.utf8).write(to: dir.appendingPathComponent(name)) }
            try? Data([0, 1, 2, 3, 0, 255, 0, 0, 7, 7, 7, 0]).write(to: dir.appendingPathComponent("binary.bin"))
            let items = svcPtr(70 * 5)
            for (i, name) in ["cats.txt", "ships.txt", "mixed.txt", "empty.txt", "binary.bin"].enumerated() {
                for b in 0..<70 { wr8(items + UInt32(70 * i + b), rd8(spec(name) + UInt32(b))) }
            }
            return (dir, items, 5)
        }

        @Test func indexedVolumesAndSessions() {
            let (_, items, n) = setup()
            let vref = svcPtr(2)
            wr16(vref, 0)
            #expect(svcCall("FBCVolumeIsIndexed", 0) == 0)
            let stamp = svcPtr(4), size = svcPtr(4)
            #expect(svcStatus(svcCall("FBCVolumeIndexTimeStamp", 0, stamp)) == -30518)
            #expect(svcCall("FBCIndexItems", items, n) == 0)
            // The volume the files are on is indexed now
            let list = svcPtr(2), count = svcPtr(2), session = svcPtr(4)
            #expect(svcCall("FBCCreateSearchSession", session) == 0)
            #expect(svcCall("FBCAddAllVolumesToSession", rd32(session), 0) == 0)
            #expect(svcCall("FBCGetSessionVolumeCount", rd32(session), count) == 0 && rd16(count) >= 1)
            wr16(count, 4)
            let vols = svcPtr(8)
            #expect(svcCall("FBCGetSessionVolumes", rd32(session), vols, count) == 0)
            let mainVol = rd16(vols)
            #expect(svcCall("FBCVolumeIsIndexed", UInt32(mainVol)) == 1 && svcCall("FBCVolumeIsRemote", UInt32(mainVol)) == 0)
            #expect(svcCall("FBCVolumeIndexTimeStamp", UInt32(mainVol), stamp) == 0 && rd32(stamp) > 3_000_000_000)
            #expect(svcCall("FBCVolumeIndexPhysicalSize", UInt32(mainVol), size) == 0 && rd32(size) > 0)
            _ = list
            // A session of its own, and a clone
            let other = svcPtr(4), clone = svcPtr(4)
            #expect(svcCall("FBCCreateSearchSession", other) == 0)
            #expect(svcCall("FBCAddVolumeToSession", rd32(other), UInt32(mainVol)) == 0)
            #expect(svcCall("FBCRemoveVolumeFromSession", rd32(other), UInt32(mainVol)) == 0)
            #expect(svcStatus(svcCall("FBCRemoveVolumeFromSession", rd32(other), UInt32(mainVol))) == -30518)
            let setv = svcPtr(2)
            wr16(setv, mainVol)
            #expect(svcCall("FBCSetSessionVolumes", rd32(other), setv, 1) == 0)
            #expect(svcCall("FBCCloneSearchSession", rd32(other), clone) == 0)
            #expect(svcCall("FBCGetSessionVolumeCount", rd32(clone), count) == 0 && rd16(count) == 1)
            #expect(svcCall("FBCDestroySearchSession", rd32(clone)) == 0)
            #expect(svcStatus(svcCall("FBCDestroySearchSession", rd32(clone))) == -30519)
        }

        @Test func queryExampleAndSummaries() {
            let (dir, items, n) = setup()
            #expect(svcCall("FBCIndexItems", items, n) == 0)
            let session = svcPtr(4), count = svcPtr(4)
            #expect(svcCall("FBCCreateSearchSession", session) == 0)
            // No volume in the session yet
            #expect(svcStatus(svcCall("FBCDoQuerySearch", rd32(session), cstr("sea"), 0, 0, 10, 4)) == -30519)
            #expect(svcCall("FBCAddAllVolumesToSession", rd32(session), 0) == 0)
            #expect(svcCall("FBCDoQuerySearch", rd32(session), cstr("ships and the sea"), 0, 0, 10, 4) == 0)
            #expect(svcCall("FBCGetHitCount", rd32(session), count) == 0 && rd32(count) == 2) // ships.txt, then mixed.txt
            let hitSpec = svcPtr(70), score = svcPtr(4)
            #expect(svcCall("FBCGetHitDocument", rd32(session), 0, hitSpec) == 0)
            #expect(String(decoding: svcBytes(hitSpec + 7, Int(rd8(hitSpec + 6))), as: UTF8.self) == "ships.txt")
            #expect(svcCall("FBCGetHitScore", rd32(session), 0, score) == 0)
            #expect(Float(bitPattern: rd32(score)) == 1.0)
            #expect(svcCall("FBCGetHitScore", rd32(session), 1, score) == 0 && Float(bitPattern: rd32(score)) < 1.0)
            let wc = svcPtr(4), wl = svcPtr(4)
            #expect(svcCall("FBCGetMatchedWords", rd32(session), 0, wc, wl) == 0 && words(rd32(wl), rd32(wc)) == ["sea", "ships"])
            #expect(svcCall("FBCDestroyWordList", rd32(wl), rd32(wc)) == 0)
            #expect(svcCall("FBCGetTopicWords", rd32(session), 0, wc, wl) == 0 && words(rd32(wl), rd32(wc)).contains("sea"))
            #expect(svcStatus(svcCall("FBCGetHitScore", rd32(session), 9, score)) == -30503)
            // A search can't change its volumes after it has run
            #expect(svcStatus(svcCall("FBCAddVolumeToSession", rd32(session), 7)) == -30526)
            // An example search takes the topic of a hit; the example is not among the hits
            let examples = svcPtr(4)
            wr32(examples, 0)
            #expect(svcCall("FBCDoExampleSearch", rd32(session), examples, 1, 0, 0, 10, 4) == 0)
            #expect(svcCall("FBCGetHitCount", rd32(session), count) == 0 && rd32(count) == 1)
            #expect(svcCall("FBCGetHitDocument", rd32(session), 0, hitSpec) == 0)
            #expect(String(decoding: svcBytes(hitSpec + 7, Int(rd8(hitSpec + 6))), as: UTF8.self) == "mixed.txt")
            // Limited to a folder that holds nothing
            let sub = dir.appendingPathComponent("Sub")
            try? FileManager.default.createDirectory(at: sub, withIntermediateDirectories: true)
            let target = svcPtr(70)
            #expect(tb_FSMakeFSSpec(0, 0, svcPStr("Sub"), target) == 0)
            let s2 = svcPtr(4)
            #expect(svcCall("FBCCreateSearchSession", s2) == 0 && svcCall("FBCAddAllVolumesToSession", rd32(s2), 0) == 0)
            #expect(svcCall("FBCDoQuerySearch", rd32(s2), cstr("sea"), target, 1, 10, 4) == 0)
            #expect(svcCall("FBCGetHitCount", rd32(s2), count) == 0 && rd32(count) == 0)
            #expect(svcCall("FBCReleaseSessionHits", rd32(s2)) == 0)
            // Blind: from a document, without a session
            let blind = svcPtr(4)
            #expect(svcCall("FBCBlindExampleSearch", spec("cats.txt"), 1, 0, 0, 10, 4, 1, 0, blind) == 0)
            #expect(svcCall("FBCGetHitCount", rd32(blind), count) == 0 && rd32(count) >= 1)
            // A summary keeps the sentences with the commonest words
            let text = "Cats are pets. Cats purr and cats sleep. The weather is odd today. Cats like warm places."
            let out = svcPtr(256), outLen = svcPtr(4), sentences = svcPtr(4)
            wr32(outLen, 256)
            wr32(sentences, 2)
            #expect(svcCall("FBCSummarize", cstr(text), UInt32(text.utf8.count), out, outLen, sentences) == 0)
            let summary = String(decoding: svcBytes(out, Int(rd32(outLen))), as: UTF8.self)
            #expect(rd32(sentences) == 2 && summary.contains("Cats purr") && !summary.contains("weather"))
        }

        @Test func removingAFolderTakesItsDocuments() {
            let (dir, items, n) = setup()
            #expect(svcCall("FBCIndexItems", items, n) == 0)
            let session = svcPtr(4), count = svcPtr(4)
            #expect(svcCall("FBCCreateSearchSession", session) == 0 && svcCall("FBCAddAllVolumesToSession", rd32(session), 0) == 0)
            let folder = svcPtr(80)
            let ref = svcPtr(80)
            _ = ref
            let sp = spec("cats.txt")
            #expect(tb_FSpMakeFSRef(sp, folder) == 0)
            #expect(svcCall("FBCDeleteIndexFileForFolder", folder) == 0) // the document's own path: it leaves
            #expect(svcCall("FBCDoQuerySearch", rd32(session), cstr("cats"), 0, 0, 10, 4) == 0)
            #expect(svcCall("FBCGetHitCount", rd32(session), count) == 0 && rd32(count) == 0) // the words are not stemmed
            #expect(svcCall("FBCReleaseSessionHits", rd32(session)) == 0)
            let again = svcPtr(4)
            #expect(svcCall("FBCCreateSearchSession", again) == 0 && svcCall("FBCAddAllVolumesToSession", rd32(again), 0) == 0)
            #expect(svcCall("FBCDoQuerySearch", rd32(again), cstr("cat"), 0, 0, 10, 4) == 0)
            #expect(svcCall("FBCGetHitCount", rd32(again), count) == 0 && rd32(count) == 1) // mixed.txt alone
            _ = dir
            #expect(svcStatus(svcCall("FBCDeleteIndexFileForFolder", folder)) == -30518)
        }
    }
}
