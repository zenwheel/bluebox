import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Dictionary Manager (Dictionary.h): dictionaries of typed fields and records, kept in files.
    @Suite struct CarbonDictionary {
        let begins = fourCC("bgwt"), contains = fourCC("cont"), ends = fourCC("ends")

        func fieldInfo(_ tag: String, type: String, maxSize: UInt32 = 0, attrs: UInt32 = 0, find: [UInt32] = [],
                       def: (String, [UInt8])? = nil) -> UInt32 {
            let rec = svcPtr(8), methods = svcPtr(max(4, find.count * 4))
            for (i, m) in find.enumerated() { wr32(methods + UInt32(i * 4), m) }
            var d: UInt32 = 0
            if let def {
                d = svcPtr(8)
                let b = svcPtr(max(1, def.1.count))
                for (i, x) in def.1.enumerated() { wr8(b + UInt32(i), x) }
                #expect(tb_AECreateDesc(Int32(bitPattern: fourCC(def.0)), b, Int32(def.1.count), d) == 0)
            }
            #expect(svcCall("DCMCreateFieldInfoRecord", fourCC(tag), fourCC(type), maxSize, attrs, d, UInt32(find.count), methods, rec) == 0)
            return rec
        }

        func fieldList(_ infos: [UInt32]) -> UInt32 {
            let list = svcPtr(8)
            #expect(tb_AECreateList(0, 0, 0, list) == 0)
            for i in infos { #expect(tb_AEPutDesc(list, 0, i) == 0) }
            return list
        }

        /// An AERecord of field data: (tag, type, bytes).
        func data(_ fields: [(String, String, [UInt8])]) -> UInt32 {
            let rec = svcPtr(8)
            #expect(tb_AECreateList(0, 0, 1, rec) == 0)
            for (tag, type, bytes) in fields {
                let b = svcPtr(max(1, bytes.count))
                for (i, x) in bytes.enumerated() { wr8(b + UInt32(i), x) }
                #expect(tb_AEPutParamPtr(rec, Int32(bitPattern: fourCC(tag)), Int32(bitPattern: fourCC(type)), b, Int32(bytes.count)) == 0)
            }
            return rec
        }

        func bytes(_ s: String) -> UInt32 {
            let p = svcPtr(max(1, s.utf8.count))
            for (i, x) in s.utf8.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        func fieldBytes(_ rec: UInt32, _ tag: String) -> [UInt8]? {
            let d = svcPtr(8)
            guard tb_AEGetParamDesc(rec, Int32(bitPattern: fourCC(tag)), Int32(bitPattern: fourCC("****")), d) == 0 else { return nil }
            let h = rd32(d + 4)
            return svcBytes(rd32(h), Int(tb_GetHandleSize(h)))
        }

        func make(_ name: String) -> (UInt32, UInt32) {
            let spec = svcPtr(70), id = svcPtr(4)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr(name), spec)
            let infos = [fieldInfo("yomi", type: "TEXT", attrs: 1 | 4, find: [0x3D20_2020, fourCC("bgwt"), fourCC("ftri")]),
                         fieldInfo("hyok", type: "TEXT", attrs: 1, find: [0x3D20_2020, fourCC("cont")]),
                         fieldInfo("hind", type: "shor", maxSize: 2, attrs: 8, def: ("shor", [0, 5]))]
            let e = svcStatus(svcCall("DCMNewDictionary", 0, spec, 0, fieldList(infos), 0, 100, id))
            #expect(e == 0)
            return (rd32(id), spec)
        }

        @Test func fieldInfoRecordsRoundTrip() {
            bootMachine()
            let rec = fieldInfo("yomi", type: "TEXT", maxSize: 40, attrs: 5, find: [begins, ends], def: ("TEXT", Array("x".utf8)))
            let tag = svcPtr(4), type = svcPtr(4), size = svcPtr(4), attrs = svcPtr(4), n = svcPtr(4), methods = svcPtr(16)
            #expect(svcCall("DCMGetFieldTagAndType", rec, tag, type) == 0 && rd32(tag) == fourCC("yomi") && rd32(type) == fourCC("TEXT"))
            #expect(svcCall("DCMGetFieldMaxRecordSize", rec, size) == 0 && rd32(size) == 40)
            #expect(svcCall("DCMGetFieldAttributes", rec, attrs) == 0 && rd32(attrs) == 5)
            #expect(svcCall("DCMGetFieldFindMethods", rec, 4, methods, n) == 0 && rd32(n) == 2 && rd32(methods + 4) == ends)
            #expect(svcStatus(svcCall("DCMGetFieldFindMethods", rec, 1, methods, n)) == -7127)
            let def = svcPtr(8)
            #expect(svcCall("DCMGetFieldDefaultData", rec, fourCC("TEXT"), def) == 0 && rd32(def) == fourCC("TEXT"))
            let bare = svcPtr(8)
            #expect(tb_AECreateList(0, 0, 1, bare) == 0)
            #expect(svcStatus(svcCall("DCMGetFieldTagAndType", bare, tag, type)) == -7111)
        }

        @Test func recordsAreAddedFoundAndIterated() {
            launchTestApp()
            let (dict, _) = make("Words")
            let ref = svcPtr(4)
            #expect(svcCall("DCMOpenDictionary", dict, 0, 0, ref) == 0)
            let r = rd32(ref)
            // Without write access nothing is added
            let word = data([("yomi", "TEXT", Array("kaki".utf8)), ("hyok", "TEXT", Array("persimmon".utf8))])
            let uid = svcPtr(4)
            #expect(svcStatus(svcCall("DCMAddRecord", r, fourCC("yomi"), 4, bytes("kaki"), 0, word, uid)) == -7103)
            #expect(svcCall("DCMGetDictionaryWriteAccess", r, 0) == 0)
            #expect(svcCall("DCMAddRecord", r, fourCC("yomi"), 4, bytes("kaki"), 0, word, uid) == 0 && rd32(uid) == 1)
            #expect(svcStatus(svcCall("DCMAddRecord", r, fourCC("yomi"), 4, bytes("kaki"), 0, word, uid)) == -7109) // duplicate
            // checkOnly says whether it could be added, and adds nothing
            #expect(svcStatus(svcCall("DCMAddRecord", r, fourCC("yomi"), 4, bytes("kaki"), 1, word, uid)) == -7109)
            #expect(svcCall("DCMAddRecord", r, fourCC("yomi"), 4, bytes("kane"), 1, data([("hyok", "TEXT", Array("metal".utf8))]), uid) == 0)
            #expect(svcCall("DCMAddRecord", r, fourCC("yomi"), 4, bytes("kama"), 0, data([("hyok", "TEXT", Array("sickle".utf8))]), uid) == 0)
            #expect(svcCall("DCMAddRecord", r, fourCC("yomi"), 3, bytes("ume"), 0, data([("hyok", "TEXT", Array("plum".utf8)), ("hind", "shor", [0, 9])]), uid) == 0)
            // A key field left out comes from the key given; defaults fill the rest
            let count = svcPtr(4)
            #expect(svcCall("DCMCountRecord", dict, count) == 0 && rd32(count) == 3)
            // Field data back, by key
            let out = svcPtr(8), tags = svcPtr(8)
            wr32(tags, fourCC("hyok"))
            wr32(tags + 4, fourCC("hind"))
            #expect(svcCall("DCMGetFieldData", r, fourCC("yomi"), 3, bytes("ume"), 0, 2, tags, out) == 0)
            #expect(fieldBytes(out, "hyok") == Array("plum".utf8) && fieldBytes(out, "hind") == [0, 9])
            #expect(svcCall("DCMGetFieldData", r, fourCC("yomi"), 4, bytes("kama"), 0, 2, tags, out) == 0)
            #expect(fieldBytes(out, "hind") == [0, 5]) // the default
            #expect(svcStatus(svcCall("DCMGetFieldData", r, fourCC("yomi"), 4, bytes("zzzz"), 0, 2, tags, out)) == -7108)
            // Order: kama < kaki? by bytes: kaki, kama, ume
            let key = svcPtr(16), ksize = svcPtr(4), id = svcPtr(4)
            #expect(svcCall("DCMGetNthRecord", r, fourCC("yomi"), 1, 16, ksize, key, id) == 0)
            #expect(String(decoding: svcBytes(key, Int(rd32(ksize))), as: UTF8.self) == "kaki")
            #expect(svcCall("DCMGetNthRecord", r, fourCC("yomi"), 3, 16, ksize, key, id) == 0)
            #expect(String(decoding: svcBytes(key, Int(rd32(ksize))), as: UTF8.self) == "ume")
            #expect(svcStatus(svcCall("DCMGetNthRecord", r, fourCC("yomi"), 4, 16, ksize, key, id)) == -7108)
            #expect(svcCall("DCMGetNextRecord", r, fourCC("yomi"), 4, bytes("kaki"), 0, 16, ksize, key, id) == 0)
            #expect(String(decoding: svcBytes(key, Int(rd32(ksize))), as: UTF8.self) == "kama")
            #expect(svcStatus(svcCall("DCMGetNextRecord", r, fourCC("yomi"), 3, bytes("ume"), 0, 16, ksize, key, id)) == -7126)
            #expect(svcCall("DCMGetPrevRecord", r, fourCC("yomi"), 4, bytes("kama"), 0, 16, ksize, key, id) == 0)
            #expect(String(decoding: svcBytes(key, Int(rd32(ksize))), as: UTF8.self) == "kaki")
            let seq = svcPtr(4)
            #expect(svcCall("DCMGetRecordSequenceNumber", r, fourCC("yomi"), 4, bytes("kama"), 0, seq) == 0 && rd32(seq) == 2)
            // Finding: beginning match, prefetching a field; a method the field lacks
            let it = svcPtr(4), fetch = svcPtr(4), list = svcPtr(8), actual = svcPtr(4), uidp = svcPtr(4)
            wr32(fetch, fourCC("hyok"))
            #expect(svcCall("DCMFindRecords", r, fourCC("yomi"), 2, bytes("ka"), begins, 1, fetch, 0, 0, it) == 0)
            #expect(svcCall("DCMCountRecordIterator", rd32(it)) == 2)
            #expect(svcCall("DCMIterateFoundRecord", rd32(it), 16, actual, key, uidp, list) == 0)
            #expect(String(decoding: svcBytes(key, Int(rd32(actual))), as: UTF8.self) == "kaki" && fieldBytes(list, "hyok") == Array("persimmon".utf8))
            #expect(svcCall("DCMIterateFoundRecord", rd32(it), 16, actual, key, uidp, list) == 0 && rd32(uidp) == 2)
            #expect(svcStatus(svcCall("DCMIterateFoundRecord", rd32(it), 16, actual, key, uidp, list)) == -7126)
            #expect(svcCall("DCMDisposeRecordIterator", rd32(it)) == 0)
            #expect(svcStatus(svcCall("DCMFindRecords", r, fourCC("yomi"), 2, bytes("ka"), contains, 0, fetch, 0, 0, it)) == -7118)
            // Skip and limit; the forward trie finds the keys that begin a word
            #expect(svcCall("DCMFindRecords", r, fourCC("yomi"), 2, bytes("ka"), begins, 0, fetch, 1, 1, it) == 0)
            #expect(svcCall("DCMCountRecordIterator", rd32(it)) == 1)
            #expect(svcCall("DCMFindRecords", r, fourCC("yomi"), 7, bytes("kakiume"), fourCC("ftri"), 0, fetch, 0, 0, it) == 0)
            #expect(svcCall("DCMCountRecordIterator", rd32(it)) == 1)
            #expect(svcCall("DCMFindRecords", r, fourCC("hyok"), 3, bytes("lum"), contains, 0, fetch, 0, 0, it) == 0)
            #expect(svcCall("DCMCountRecordIterator", rd32(it)) == 1)
            #expect(svcStatus(svcCall("DCMFindRecords", r, fourCC("nope"), 1, bytes("x"), 0x3D20_2020, 0, fetch, 0, 0, it)) == -7113)
            // Changing a field, deleting a record
            #expect(svcCall("DCMSetFieldData", r, fourCC("yomi"), 4, bytes("kama"), 0, data([("hyok", "TEXT", Array("scythe".utf8))])) == 0)
            #expect(svcCall("DCMGetFieldData", r, fourCC("yomi"), 4, bytes("kama"), 0, 1, tags, out) == 0 && fieldBytes(out, "hyok") == Array("scythe".utf8))
            #expect(svcCall("DCMDeleteRecord", r, fourCC("yomi"), 4, bytes("kaki"), 0) == 0)
            #expect(svcStatus(svcCall("DCMDeleteRecord", r, fourCC("yomi"), 4, bytes("kaki"), 0)) == -7108)
            #expect(svcCall("DCMCountRecord", dict, count) == 0 && rd32(count) == 2)
            // Giving up write access without committing undoes the changes since taking it
            #expect(svcCall("DCMReleaseDictionaryWriteAccess", r, 1) == 0)
            #expect(svcCall("DCMGetDictionaryWriteAccess", r, 0) == 0)
            #expect(svcCall("DCMDeleteRecord", r, fourCC("yomi"), 4, bytes("kama"), 0) == 0)
            #expect(svcCall("DCMReleaseDictionaryWriteAccess", r, 0) == 0)
            #expect(svcCall("DCMCountRecord", dict, count) == 0 && rd32(count) == 2)
            // A second ref can't have write access while the first holds it
            let ref2 = svcPtr(4)
            #expect(svcCall("DCMOpenDictionary", dict, 0, 0, ref2) == 0)
            #expect(svcCall("DCMGetDictionaryWriteAccess", r, 0) == 0)
            #expect(svcStatus(svcCall("DCMGetDictionaryWriteAccess", rd32(ref2), 0)) == -7105)
            #expect(svcCall("DCMCloseDictionary", rd32(ref2)) == 0)
            #expect(svcCall("DCMCloseDictionary", r) == 0)
            #expect(svcStatus(svcCall("DCMCloseDictionary", r)) == -7104)
        }

        @Test func filesPropertiesAndIterators() {
            launchTestApp()
            let (dict, spec) = make("Names")
            // The file exists, so it can't be made again; it is found by its file; reopened from disk
            let id2 = svcPtr(4)
            #expect(svcStatus(svcCall("DCMNewDictionary", 0, spec, 0, fieldList([fieldInfo("yomi", type: "TEXT")]), 0, 0, id2)) == -48)
            #expect(svcCall("DCMGetDictionaryIDFromFile", spec, id2) == 0 && rd32(id2) == dict)
            let ref = svcPtr(4)
            #expect(svcCall("DCMOpenDictionary", dict, 0, 0, ref) == 0)
            #expect(svcCall("DCMGetDictionaryIDFromRef", rd32(ref)) == dict)
            #expect(svcCall("DCMGetDictionaryWriteAccess", rd32(ref), 0) == 0)
            #expect(svcCall("DCMAddRecord", rd32(ref), fourCC("yomi"), 3, bytes("abc"), 0, data([]), svcPtr(4)) == 0)
            #expect(svcCall("DCMCloseDictionary", rd32(ref)) == 0) // commits
            #expect(svcCall("DCMUnregisterDictionary", dict) == 0)
            #expect(svcCall("DCMGetDictionaryIDFromFile", spec, id2) == 0 && rd32(id2) != 0)
            let count = svcPtr(4)
            #expect(svcCall("DCMCountRecord", rd32(id2), count) == 0 && rd32(count) == 1)
            let fileSpec = svcPtr(70)
            #expect(svcCall("DCMGetFileFromDictionaryID", rd32(id2), fileSpec) == 0)
            #expect(svcBytes(fileSpec + 6, 6) == svcBytes(spec + 6, 6))
            // Field info of a field of it
            let info = svcPtr(8), tag = svcPtr(4), ty = svcPtr(4)
            #expect(svcCall("DCMGetDictionaryFieldInfo", rd32(id2), fourCC("hind"), info) == 0)
            #expect(svcCall("DCMGetFieldTagAndType", info, tag, ty) == 0 && rd32(tag) == fourCC("hind") && rd32(ty) == fourCC("shor"))
            #expect(svcStatus(svcCall("DCMGetDictionaryFieldInfo", rd32(id2), fourCC("zzzz"), info)) == -7113)
            // Properties: the standard ones, and ours
            let value = svcPtr(16), size = svcPtr(4)
            #expect(svcCall("DCMGetDictionaryProperty", rd32(id2), fourCC("perm"), 4, size, value) == 0 && rd32(value) == 1 && rd32(size) == 4)
            #expect(svcStatus(svcCall("DCMGetDictionaryProperty", rd32(id2), fourCC("nopr"), 4, size, value)) == -7119)
            #expect(svcCall("DCMSetDictionaryProperty", rd32(id2), fourCC("info"), 4, bytes("Copy")) == 0)
            #expect(svcStatus(svcCall("DCMGetDictionaryProperty", rd32(id2), fourCC("info"), 2, size, value)) == -7127 && rd32(size) == 4)
            let list = svcPtr(32), num = svcPtr(4)
            #expect(svcCall("DCMGetDictionaryPropertyList", rd32(id2), 8, num, list) == 0 && rd32(num) == 5)
            // Read-only dictionaries refuse write access
            let rov = svcPtr(4)
            wr32(rov, 0)
            #expect(svcCall("DCMSetDictionaryProperty", rd32(id2), fourCC("perm"), 4, rov) == 0)
            let ref2 = svcPtr(4)
            #expect(svcCall("DCMOpenDictionary", rd32(id2), 0, 0, ref2) == 0)
            #expect(svcStatus(svcCall("DCMGetDictionaryWriteAccess", rd32(ref2), 0)) == -7103)
            #expect(svcCall("DCMCloseDictionary", rd32(ref2)) == 0)
            // Iterators over the dictionaries and the access methods
            let it = svcPtr(4), obj = svcPtr(4)
            #expect(svcCall("DCMCreateDictionaryIterator", it) == 0 && svcCall("DCMCountObjectIterator", rd32(it)) == 1)
            #expect(svcCall("DCMIterateObject", rd32(it), obj) == 0 && rd32(obj) == rd32(id2))
            #expect(svcStatus(svcCall("DCMIterateObject", rd32(it), obj)) == -7126)
            #expect(svcCall("DCMResetObjectIterator", rd32(it)) == 0 && svcCall("DCMIterateObject", rd32(it), obj) == 0)
            #expect(svcCall("DCMDisposeObjectIterator", rd32(it)) == 0)
            #expect(svcCall("DCMCreateAccessMethodIterator", it) == 0 && svcCall("DCMCountObjectIterator", rd32(it)) == 1)
            let method = svcPtr(4)
            #expect(svcCall("DCMIterateObject", rd32(it), method) == 0)
            #expect(svcCall("DCMGetAccessMethodIDFromName", svcPStr("Blue Box Dictionary"), obj) == 0 && rd32(obj) == rd32(method))
            #expect(svcStatus(svcCall("DCMGetAccessMethodIDFromName", svcPStr("Other"), obj)) == -7122)
            // Reorganize and compact rewrite the file; deleting removes it
            #expect(svcCall("DCMReorganizeDictionary", rd32(id2), 10, 0, 0) == 0)
            #expect(svcCall("DCMCompactDictionary", rd32(id2), 0, 0) == 0)
            #expect(svcCall("DCMDeleteDictionary", rd32(id2)) == 0)
            #expect(svcStatus(svcCall("DCMCountRecord", rd32(id2), count)) == -7100)
            #expect(svcStatus(svcCall("DCMGetDictionaryIDFromFile", spec, id2)) == -43)
            #expect(svcCall("DCMLibraryVersion") != 0)
        }

        @Test func aFileThatIsNotADictionary() {
            launchTestApp()
            let spec = svcPtr(70), id = svcPtr(4)
            #expect(tb_FSMakeFSSpec(0, 0, svcPStr("Plain"), spec) == -43)
            #expect(tb_FSpCreate(spec, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("TEXT")), 0) == 0)
            #expect(svcStatus(svcCall("DCMRegisterDictionaryFile", spec, id)) == -7101)
            #expect(svcStatus(svcCall("DCMNewDictionary", 0, spec, 0, fieldList([]), 0, 0, id)) == -48)
            let spec2 = svcPtr(70)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr("Empty"), spec2)
            #expect(svcStatus(svcCall("DCMNewDictionary", 0, spec2, 0, fieldList([]), 0, 0, id)) == -7111) // no fields
            #expect(svcStatus(svcCall("DCMNewDictionary", 7, spec2, 0, fieldList([fieldInfo("yomi", type: "TEXT")]), 0, 0, id)) == -7122)
        }
    }
}
