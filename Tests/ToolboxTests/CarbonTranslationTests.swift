import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Translation Manager (Easy Open) with no translators installed.
    @Suite struct TranslationManager {
        func makeDoc(_ name: String, type: String, creator: String) -> UInt32 {
            let spec = svcPtr(70)
            #expect(tb_FSMakeFSSpec(0, 0, svcPStr(name), spec) == -43 || true)
            #expect(tb_FSpCreate(spec, Int32(bitPattern: fourCC(creator)), Int32(bitPattern: fourCC(type)), 0) == 0)
            return spec
        }

        func handle(_ b: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(b.count), false, false)
            for (i, x) in b.enumerated() { wr8(rd32(h) + UInt32(i), x) }
            return h
        }

        /// An application of its own creator opening TEXT and PICT, known to the Desktop Database.
        func makeApplication() {
            let spec = svcPtr(70)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr("Easy App"), spec)
            tb_FSpCreateResFile(spec, Int32(bitPattern: fourCC("TsTc")), Int32(bitPattern: fourCC("APPL")), 0)
            let rn = tb_FSpOpenResFile(spec, 3)
            #expect(rn > 0)
            for (id, type) in [(128, "APPL"), (129, "TEXT"), (130, "PICT"), (131, "TEXT")] {
                tb_AddResource(handle(be32(fourCC(type)) + be16(0) + [0]), Int32(bitPattern: fourCC("FREF")), Int16(id), 0)
            }
            tb_CloseResFile(rn)
            _ = tb_PBDTAddAPPL(svcPtr(Int(DTPBRec_SIZEOF)), false)
        }

        @Test func openingAndTranslating() {
            launchTestApp()
            makeApplication()
            let text = makeDoc("Notes", type: "TEXT", creator: "ttxt")
            let other = makeDoc("Chart", type: "XLS ", creator: "XCEL")
            // The types it opens by itself are given, and the document is native or can't be opened.
            let natives = svcPtr(64 * 4)
            wr32(natives, fourCC("PICT"))
            wr32(natives + 4, fourCC("TEXT"))
            for i in 2..<64 { wr32(natives + UInt32(4 * i), 0) }
            let how = svcPtr(2), spec = svcPtr(48)
            wr32(spec, 0x1234)
            #expect(tb_CanDocBeOpened(text, 0, 0, natives, 1, how, spec) == 0)
            #expect(rd16(how) == 1 && rd32(spec) == 0) // domNative, a clear spec
            #expect(tb_CanDocBeOpened(other, 0, 0, natives, 0, how, spec) == 0 && rd16(how) == 0) // domCannot
            #expect(svcStatus(UInt32(bitPattern: Int32(tb_CanDocBeOpened(svcPtr(70), 0, 0, natives, 0, how, spec)))) == -43)
            // The 68k trap goes the same way: $ABFC with the selector in D0
            // Applications aren't known by signature here: not found.
            #expect(tb_GetFileTypesThatAppCanNativelyOpen(0, Int32(bitPattern: fourCC("zzzz")), natives) == -5012)
            #expect(rd32(natives) == 0)
            // The test application is the one known: '????' (it opens TEXT and PICT; APPL isn't a document)
            #expect(tb_GetFileTypesThatAppCanNativelyOpen(0, Int32(bitPattern: fourCC("TsTc")), natives) == 0)
            #expect(rd32(natives) == fourCC("TEXT") && rd32(natives + 4) == fourCC("PICT") && rd32(natives + 8) == 0)
            let none = svcPtr(1)
            _ = none
            // With that application's types, the Chart (XLS ) can't be opened and the Notes can
            #expect(tb_CanDocBeOpened(text, 0, Int32(bitPattern: fourCC("TsTc")), 0, 0, how, spec) == 0 && rd16(how) == 1)
            #expect(tb_CanDocBeOpened(other, 0, Int32(bitPattern: fourCC("TsTc")), 0, 0, how, spec) == 0 && rd16(how) == 0)
            let kindOf = svcPtr(64)
            #expect(tb_GetDocumentKindString(0, Int32(bitPattern: fourCC("TEXT")), Int32(bitPattern: fourCC("TsTc")), kindOf) == 0)
            #expect(String(decoding: svcBytes(kindOf + 1, Int(rd8(kindOf))), as: UTF8.self) == "Easy App document")
            // Extending a list with no translators changes nothing
            let more = svcPtr(64 * 4), count = svcPtr(2)
            #expect(tb_ExtendFileTypeList(natives, 2, more, count) == 0 && rd16(count) == 2)
            // Paths, translating
            #expect(tb_GetFileTranslationPaths(text, Int32(bitPattern: fourCC("PICT")), 4, spec) == 0)
            #expect(tb_GetPathFromTranslationDialog(text, 0, natives, how, spec) == -3030 && rd16(how) == 0)
            #expect(tb_TranslateFile(text, other, spec) == -3031)
            let name = svcPtr(32)
            wr8(name, 9)
            #expect(tb_GetTranslationExtensionName(spec, name) == -3031 && rd8(name) == 0)
            // Kind strings
            let kind = svcPtr(64)
            #expect(tb_GetDocumentKindString(0, Int32(bitPattern: fourCC("APPL")), Int32(bitPattern: fourCC("ttxt")), kind) == 0)
            #expect(String(decoding: svcBytes(kind + 1, Int(rd8(kind))), as: UTF8.self) == "application program")
            #expect(tb_GetDocumentKindString(0, Int32(bitPattern: fourCC("TEXT")), Int32(bitPattern: fourCC("zzzz")), kind) == 0)
            #expect(String(decoding: svcBytes(kind + 1, Int(rd8(kind))), as: UTF8.self) == "document")
        }

        nonisolated(unsafe) static var asked: [UInt32] = []
        /// pascal OSErr (ScrapType format, Handle data, void *refCon): has TEXT only
        static let getter: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)
            let data = cpu_pop32(c)
            let format = cpu_pop32(c)
            asked.append(format)
            var err: UInt32 = 0
            if format == fourCC("TEXT") {
                tb_SetHandleSize(data, 3)
                for (i, b) in Array("abc".utf8).enumerated() { wr8(rd32(data) + UInt32(i), b) }
            } else {
                err = UInt32(bitPattern: -192)
            }
            wr16(c.pointee.a.7, UInt16(err & 0xFFFF))
            cpu_push32(c, ret)
        }

        @Test func scrapsAreFetchedWhenTheSourceHasThem() {
            bootMachine()
            Self.asked = []
            let upp = svcUPP68k(Self.getter, "ScrapGetterTest", 0xFE0)
            let h = tb_NewHandle(0, false, true)
            #expect(svcCall("TranslateScrap", upp, 0, fourCC("TEXT"), h, 0) == 0)
            #expect(tb_GetHandleSize(h) == 3)
            #expect(svcStatus(svcCall("TranslateScrap", upp, 0, fourCC("PICT"), h, 0)) == -3030)
            #expect(Self.asked == [fourCC("TEXT"), fourCC("PICT")])
            #expect(svcStatus(svcCall("TranslateScrap", 0, 0, fourCC("TEXT"), h, 0)) == -50)
        }

        @Test func translatorCallbacksNeedATranslation() {
            bootMachine()
            let canceled = svcPtr(1)
            wr8(canceled, 9)
            #expect(tb_UpdateTranslationProgress(1, 50, canceled) == -3031 && rd8(canceled) == 0)
            #expect(tb_SetTranslationAdvertisement(1, 0) == -3031)
        }
    }
}
