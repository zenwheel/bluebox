import CCore
import CPUPPC
import ForkStore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// AppleScriptLib: dialects, copies, the debugging calls, the generic
    /// scripting component and the storage type.
    @Suite struct OS9RestScript {
        init() {
            Rest.boot()
            launchTestApp()
            cfm_reset()
            _ = cfm_ppc_cpu()
        }

        func instance(_ sub: String = "ascr", _ type: String = "osa ") -> UInt32 {
            Rest.call("OpenDefaultComponent", [fourCC(type), fourCC(sub)])
        }

        func guestBytes(_ b: [UInt8]) -> UInt32 {
            let p = Rest.ptr(max(b.count, 1))
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        func nameDesc(_ s: String) -> UInt32 {
            let d = Rest.ptr(Int(AEDesc_SIZEOF))
            let b = Array(s.utf8)
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("TEXT")), guestBytes(b), Int32(b.count), d) == 0)
            return d
        }

        /// A loaded script with properties "start" (100) and "name" ("x") and
        /// two handlers, besides its run handler.
        func loadedScript(_ ci: UInt32) -> UInt32 {
            let a = BlueBox.AppleScript.Asm()
            a.op(BlueBox.AppleScript.pushLit | 0, BlueBox.AppleScript.ret)
            let h = BlueBox.AppleScript.handler(.ident("hello"), params: [], locals: [], literals: [.text("hi")], code: a.done())
            let h2 = BlueBox.AppleScript.handler(.ident("bye"), params: [], locals: [], literals: [.text("bye")], code: a.done())
            let data = BlueBox.AppleScript.script([(.ident("start"), .int(100)), (.ident("name"), .text("x")),
                                                   (.ident("hello"), h), (.ident("bye"), h2)])
            let desc = Rest.ptr(Int(AEDesc_SIZEOF)), id = Rest.ptr(4)
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("scpt")), guestBytes(data), Int32(data.count), desc) == 0)
            #expect(Rest.signed("OSALoad", [ci, desc, 0, id]) == 0)
            return rd32(id)
        }

        func texts(_ list: UInt32) -> [String] {
            let n = Rest.ptr(4)
            guard tb_AECountItems(list, n) == 0 else { return [] }
            return (1...max(Int(rd32(n)), 1)).compactMap { i -> String? in
                guard i <= Int(rd32(n)) else { return nil }
                let buf = Rest.ptr(64), got = Rest.ptr(4), kw = Rest.ptr(4), ty = Rest.ptr(4)
                guard tb_AEGetNthPtr(list, Int32(i), Int32(bitPattern: fourCC("TEXT")), kw, ty, buf, 64, got) == 0 else { return nil }
                return String(decoding: (0..<rd32(got)).map { rd8(buf + $0) }, as: UTF8.self)
            }
        }

        @Test func dialects() {
            let ci = instance()
            #expect(ci != 0)
            let code = Rest.ptr(2)
            wr16(code, 9)
            #expect(Rest.signed("OSAGetCurrentDialect", [ci, code]) == 0 && rd16(code) == 0)
            #expect(Rest.signed("OSASetCurrentDialect", [ci, 0]) == 0)
            #expect(Rest.signed("OSASetCurrentDialect", [ci, 3]) == -1757)
            let list = Rest.ptr(Int(AEDesc_SIZEOF)), n = Rest.ptr(4)
            #expect(Rest.signed("OSAAvailableDialectCodeList", [ci, list]) == 0)
            #expect(tb_AECountItems(list, n) == 0 && rd32(n) == 1)
            #expect(Rest.signed("OSAAvailableDialects", [ci, list]) == 0 && tb_AECountItems(list, n) == 0 && rd32(n) == 1)
            let name = Rest.ptr(Int(AEDesc_SIZEOF))
            #expect(Rest.signed("OSAGetDialectInfo", [ci, 0, fourCC("dnam"), name]) == 0)
            #expect(rd32(name) == fourCC("TEXT"))
            let h = rd32(name + 4)
            #expect(String(decoding: (0..<UInt32(tb_GetHandleSize(h))).map { rd8(rd32(h) + $0) }, as: UTF8.self) == "English")
            #expect(Rest.signed("OSAGetDialectInfo", [ci, 0, fourCC("dcod"), name]) == 0 && rd32(name) == fourCC("shor"))
            #expect(Rest.signed("OSAGetDialectInfo", [ci, 7, fourCC("dnam"), name]) == -1757)
            #expect(Rest.signed("OSAGetDialectInfo", [ci, 0, fourCC("zzzz"), name]) == -1754)
        }

        @Test func propertiesAndHandlers() {
            let ci = instance()
            let script = loadedScript(ci)
            let names = Rest.ptr(Int(AEDesc_SIZEOF))
            #expect(Rest.signed("OSAGetPropertyNames", [ci, 0, script, names]) == 0)
            #expect(texts(names) == ["start", "name"])
            #expect(Rest.signed("OSAGetHandlerNames", [ci, 0, script, names]) == 0)
            #expect(texts(names) == ["hello", "bye"])
            #expect(Rest.signed("OSAGetPropertyNames", [ci, 0, 0x7777, names]) == -1751)

            let value = Rest.ptr(4), text = Rest.ptr(Int(AEDesc_SIZEOF))
            #expect(Rest.signed("OSAGetProperty", [ci, 0, script, nameDesc("start"), value]) == 0 && rd32(value) != 0)
            #expect(Rest.signed("OSADisplay", [ci, rd32(value), fourCC("TEXT"), 0, text]) == 0)
            #expect(Rest.signed("OSAGetProperty", [ci, 0, script, nameDesc("nothing"), value]) == -1753 && rd32(value) == 0)
            // set it to another value
            let hundred = Rest.ptr(4)
            #expect(Rest.signed("OSAGetProperty", [ci, 0, script, nameDesc("name"), hundred]) == 0)
            #expect(Rest.signed("OSASetProperty", [ci, 0, script, nameDesc("start"), rd32(hundred)]) == 0)
            let shown = Rest.ptr(Int(AEDesc_SIZEOF))
            #expect(Rest.signed("OSAGetProperty", [ci, 0, script, nameDesc("start"), value]) == 0)
            #expect(Rest.signed("OSADisplay", [ci, rd32(value), fourCC("TEXT"), 0, shown]) == 0)
            let h = rd32(shown + 4)
            #expect(String(decoding: (0..<UInt32(tb_GetHandleSize(h))).map { rd8(rd32(h) + $0) }, as: UTF8.self) == "\"x\"")
            // a new one is defined, unless told not to
            #expect(Rest.signed("OSASetProperty", [ci, 1, script, nameDesc("fresh"), rd32(hundred)]) == -1753) // kOSAModeDontDefine
            #expect(Rest.signed("OSASetProperty", [ci, 0, script, nameDesc("fresh"), rd32(hundred)]) == 0)
            #expect(Rest.signed("OSAGetProperty", [ci, 0, script, nameDesc("fresh"), value]) == 0 && rd32(value) != 0)
            // the obsolete forms, without mode flags
            #expect(Rest.signed("ASGetProperty", [ci, script, nameDesc("start"), value]) == 0 && rd32(value) != 0)
            #expect(Rest.signed("ASSetProperty", [ci, script, nameDesc("start"), rd32(hundred)]) == 0)
            #expect(Rest.signed("OSAGetHandler", [ci, 0, script, nameDesc("hello"), value]) == -1754)
        }

        @Test func copyingScriptsAndStyles() {
            let ci = instance()
            let script = loadedScript(ci)
            let copy = Rest.ptr(4)
            #expect(Rest.signed("OSACopyScript", [ci, script, copy]) == 0 && rd32(copy) != 0 && rd32(copy) != script)
            #expect(Rest.signed("OSACopyScript", [ci, 0x5555, copy]) == -1751)
            let styles = Rest.ptr(4)
            #expect(Rest.signed("ASGetSourceStyles", [ci, styles]) == 0 && rd32(styles) != 0)
            #expect(Rest.signed("ASSetSourceStyles", [ci, rd32(styles)]) == 0)
            #expect(Rest.signed("ASInit", [ci, 0, 0, 0, 0, 0, 0, 0]) == 0)
            let names = Rest.ptr(Int(AEDesc_SIZEOF))
            #expect(Rest.signed("ASGetSourceStyleNames", [ci, 0, names]) == 0)
            let list = texts(names)
            #expect(list.count == 8 && list[0] == "Uncompiled text" && list[4] == "Comment")
            #expect(Rest.signed("OSAFlushTerminology", [ci, 0]) == 0)
        }

        @Test func theGenericScriptingComponent() {
            let generic = instance("scpt")
            #expect(generic != 0)
            let sub = Rest.ptr(4), real = Rest.ptr(4)
            #expect(Rest.signed("OSAGetDefaultScriptingComponent", [generic, sub]) == 0 && rd32(sub) == fourCC("ascr"))
            #expect(Rest.signed("OSAGetScriptingComponent", [generic, fourCC("ascr"), real]) == 0 && rd32(real) != 0)
            #expect(Rest.signed("OSAGetScriptingComponent", [generic, fourCC("javs"), real]) == -1761)
            let stored = Rest.ptr(Int(AEDesc_SIZEOF))
            #expect(Rest.signed("OSAGetScriptingComponentFromStored", [generic, stored, sub]) == 0 && rd32(sub) == fourCC("ascr"))
            let id = Rest.ptr(4), comp = Rest.ptr(4)
            wr32(id, 77)
            wr32(comp, rd32(real))
            #expect(Rest.signed("OSAGenericToRealID", [generic, id, comp]) == 0 && rd32(id) == 77)
            #expect(Rest.signed("OSARealToGenericID", [generic, id, rd32(real)]) == 0)
        }

        @Test func terminologyOfAFile() throws {
            let dir = launchTestApp()
            cfm_reset()
            _ = cfm_ppc_cpu()
            let ci = instance()
            let list = Rest.ptr(Int(AEDesc_SIZEOF)), launched = Rest.ptr(2)
            wr8(launched, 9)
            // the System's standard terminology isn't in a bare System here
            #expect(Rest.signed("OSAGetSysTerminology", [ci, 0, 0, list]) == -1750)
            // a file with an 'aete'
            let path = dir.appendingPathComponent("Scriptable").path
            var fork = buildFork([TestResource(type: "aete", id: 0, data: [1, 2, 3, 4, 5])])
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            let spec = Rest.ptr(Int(FSSpec_SIZEOF))
            fm_spec_for_path(path, spec)
            #expect(Rest.signed("OSAGetAppTerminology", [ci, 0, spec, 0, launched, list]) == 0 && rd8(launched) == 0)
            let n = Rest.ptr(4)
            #expect(tb_AECountItems(list, n) == 0 && rd32(n) == 1)
            let buf = Rest.ptr(16), got = Rest.ptr(4), kw = Rest.ptr(4), ty = Rest.ptr(4)
            #expect(tb_AEGetNthPtr(list, 1, Int32(bitPattern: fourCC("aete")), kw, ty, buf, 16, got) == 0 && rd32(got) == 5)
            #expect((0..<5).map { rd8(buf + UInt32($0)) } == [1, 2, 3, 4, 5])
            // an id the file lacks, the obsolete form
            #expect(Rest.signed("ASGetAppTerminology", [ci, spec, 1, launched, list]) == -1750)
            #expect(Rest.signed("ASGetAppTerminology", [ci, spec, 0, launched, list]) == 0)
        }

        @Test func storageTypes() {
            let h = tb_NewHandle(10, false, true)
            for i in 0..<10 { wr8(rd32(h) + UInt32(i), UInt8(i)) }
            let ty = Rest.ptr(4)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("scpt")) // none: the generic one
            #expect(Rest.signed("OSAAddStorageType", [h, fourCC("jsrc")]) == 0)
            #expect(tb_GetHandleSize(h) == 22)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("jsrc"))
            #expect(rd32(rd32(h) + 18) == 0xFADE_DEAD && rd16(rd32(h) + 14) == 1) // the mark and the version
            #expect((0..<10).map { rd8(rd32(h) + UInt32($0)) } == Array(0..<10)) // the data kept
            #expect(Rest.signed("OSAAddStorageType", [h, fourCC("ascr")]) == 0 && tb_GetHandleSize(h) == 22)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("ascr"))
            #expect(Rest.signed("OSARemoveStorageType", [h]) == 0 && tb_GetHandleSize(h) == 10)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("scpt"))
            #expect(Rest.signed("OSARemoveStorageType", [h]) == 0 && tb_GetHandleSize(h) == 10) // none: nothing
            // odd-sized data is padded before the trailer
            let odd = tb_NewHandle(5, false, true)
            #expect(Rest.signed("OSAAddStorageType", [odd, fourCC("jsrc")]) == 0 && tb_GetHandleSize(odd) == 18)
            #expect(Rest.signed("OSAGetStorageType", [odd, ty]) == 0 && rd32(ty) == fourCC("jsrc"))
            #expect(Rest.signed("OSAGetStorageType", [0, ty]) == -1752)
        }
    }
}
