import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The OSA calls CarbonLib added to AppleScriptLib's: dialects, contexts,
    /// terminology, source styles, the generic component and storage types.
    @Suite struct CarbonOSA {
        typealias AS = BlueBox.AppleScript

        func textDesc(_ s: String) -> UInt32 {
            let d = svcPtr(8), b = svcPtr(max(1, s.utf8.count))
            for (i, c) in s.utf8.enumerated() { wr8(b + UInt32(i), c) }
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("TEXT")), b, Int32(s.utf8.count), d) == 0)
            return d
        }

        func listTexts(_ list: UInt32) -> [String] {
            let n = svcPtr(4)
            #expect(tb_AECountItems(list, n) == 0)
            return (0..<Int(rd32(n))).map { i in
                let d = svcPtr(8)
                #expect(tb_AEGetNthDesc(list, Int32(i + 1), Int32(bitPattern: fourCC("TEXT")), 0, d) == 0)
                let h = rd32(d + 4)
                return String(decoding: svcBytes(rd32(h), Int(tb_GetHandleSize(h))), as: UTF8.self)
            }
        }

        func shown(_ ci: UInt32, _ id: UInt32) -> String {
            let out = svcPtr(8)
            #expect(svcCall("OSADisplay", ci, id, fourCC("TEXT"), 0, out) == 0)
            let h = rd32(out + 4)
            return String(decoding: svcBytes(rd32(h), Int(tb_GetHandleSize(h))), as: UTF8.self)
        }

        func loaded(_ ci: UInt32, _ data: [UInt8]) -> UInt32 {
            let desc = svcPtr(8), idp = svcPtr(4)
            let buf = svcPtr(data.count)
            for (i, b) in data.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("scpt")), buf, Int32(data.count), desc) == 0)
            #expect(svcCall("OSALoad", ci, desc, 0, idp) == 0)
            return rd32(idp)
        }

        @Test func contextsAndHandlers() {
            launchTestApp()
            let q = QTTune()
            q.install()
            let ci = svcCall("OpenDefaultComponent", fourCC("osa "), fourCC("ascr"))
            #expect(ci != 0)
            let a = AS.Asm()
            a.op(0x6A, AS.ret)
            let foo = AS.handler(.ident("foo"), locals: [], literals: [], code: a.done())
            let data = AS.script([(.ident("x"), .long(5)), (.ident("foo"), foo)])
            let script = loaded(ci, data)
            let list = svcPtr(8), vid = svcPtr(4)
            #expect(svcCall("OSAGetPropertyNames", ci, 0, script, list) == 0)
            #expect(listTexts(list) == ["x"])
            #expect(svcCall("OSAGetHandlerNames", ci, 0, script, list) == 0)
            #expect(listTexts(list) == ["foo"])
            // A property by name, any case
            #expect(svcCall("OSAGetProperty", ci, 0, script, textDesc("X"), vid) == 0)
            #expect(shown(ci, rd32(vid)) == "5")
            #expect(svcStatus(svcCall("OSAGetProperty", ci, 0, script, textDesc("nothere"), vid)) == -1728)
            // Setting it, defining another, and refusing to define with kOSAModeDontDefine
            let newValue = svcPtr(4)
            #expect(svcCall("OSACoerceFromDesc", ci, textDesc("hey"), 0, newValue) == 0)
            #expect(svcCall("OSASetProperty", ci, 0, script, textDesc("x"), rd32(newValue)) == 0)
            #expect(svcCall("OSAGetProperty", ci, 0, script, textDesc("x"), vid) == 0)
            #expect(shown(ci, rd32(vid)) == "\"hey\"")
            #expect(svcStatus(svcCall("OSASetProperty", ci, 1, script, textDesc("z"), rd32(newValue))) == -1728)
            #expect(svcCall("OSASetProperty", ci, 0, script, textDesc("z"), rd32(newValue)) == 0)
            #expect(svcCall("OSAGetPropertyNames", ci, 0, script, list) == 0)
            #expect(listTexts(list) == ["x", "z"])
            // The older forms are the same without mode flags
            #expect(svcCall("ASGetProperty", ci, script, textDesc("z"), vid) == 0)
            #expect(shown(ci, rd32(vid)) == "\"hey\"")
            // Handlers
            let h = svcPtr(4)
            #expect(svcCall("OSAGetHandler", ci, 0, script, textDesc("foo"), h) == 0)
            #expect(svcStatus(svcCall("OSAGetHandler", ci, 0, script, textDesc("x"), h)) == -1728)
            #expect(svcCall("OSAGetHandler", ci, 0, script, textDesc("foo"), h) == 0)
            #expect(svcCall("OSASetHandler", ci, 0, script, textDesc("bar"), rd32(h)) == 0)
            #expect(svcCall("OSAGetHandlerNames", ci, 0, script, list) == 0)
            #expect(listTexts(list) == ["foo", "bar"])
            // A value that is not a handler can't be one
            #expect(svcStatus(svcCall("OSASetHandler", ci, 0, script, textDesc("baz"), rd32(newValue))) == -1700)
            // Not a context
            #expect(svcStatus(svcCall("OSAGetPropertyNames", ci, 0, rd32(newValue), list)) == -1751)
            _ = q
        }

        @Test func dialectsSourceStylesAndDebugger() {
            launchTestApp()
            let q = QTTune()
            q.install()
            let ci = svcCall("OpenDefaultComponent", fourCC("osa "), fourCC("ascr"))
            let code = svcPtr(2), list = svcPtr(8), out = svcPtr(8)
            #expect(svcCall("OSAGetCurrentDialect", ci, code) == 0 && rd16(code) == 0)
            #expect(svcCall("OSASetCurrentDialect", ci, 0) == 0)
            #expect(svcStatus(svcCall("OSASetCurrentDialect", ci, 3)) == -1757)
            #expect(svcCall("OSAAvailableDialectCodeList", ci, list) == 0)
            let n = svcPtr(4)
            #expect(tb_AECountItems(list, n) == 0 && rd32(n) == 1)
            #expect(svcCall("OSAAvailableDialects", ci, list) == 0)
            #expect(tb_AECountItems(list, n) == 0 && rd32(n) == 1)
            #expect(svcCall("OSAGetDialectInfo", ci, 0, fourCC("dnam"), out) == 0)
            #expect(rd32(out) == fourCC("TEXT"))
            #expect(svcCall("OSAGetDialectInfo", ci, 0, fourCC("difo"), out) == 0 && rd32(out) == fourCC("difo"))
            #expect(svcStatus(svcCall("OSAGetDialectInfo", ci, 4, fourCC("dnam"), out)) == -1757)
            #expect(svcStatus(svcCall("OSAGetDialectInfo", ci, 0, fourCC("zzzz"), out)) == -1754)
            // Source styles
            #expect(svcCall("ASGetSourceStyleNames", ci, 0, list) == 0)
            #expect(listTexts(list).count == 8 && listTexts(list)[2] == "Language Keyword")
            let styles = svcPtr(4)
            #expect(svcCall("ASGetSourceStyles", ci, styles) == 0 && rd32(styles) != 0)
            #expect(svcCall("ASSetSourceStyles", ci, rd32(styles)) == 0)
            #expect(svcCall("ASInit", ci, 0, 0, 0, 0, 0, 0, 0) == 0)
            // The debugger isn't there: the selector isn't
            let session = svcPtr(4)
            #expect(svcCall("OSADebuggerCreateSession", ci, 1, 1, session) == 0x8000_8002)
            #expect(svcCall("OSADebuggerDisposeSession", ci, 1) == 0x8000_8002)
            _ = q
        }

        @Test func genericComponentAndStorageTypes() {
            launchTestApp()
            let q = QTTune()
            q.install()
            let generic = svcCall("OpenDefaultComponent", fourCC("osa "), fourCC("scpt"))
            #expect(generic != 0)
            let sub = svcPtr(4), inst = svcPtr(4)
            #expect(svcCall("OSAGetDefaultScriptingComponent", generic, sub) == 0 && rd32(sub) == fourCC("ascr"))
            #expect(svcCall("OSAGetScriptingComponent", generic, fourCC("ascr"), inst) == 0 && rd32(inst) != 0)
            #expect(svcStatus(svcCall("OSAGetScriptingComponent", generic, fourCC("jscr"), inst)) == -1761)
            let id = svcPtr(4), comp = svcPtr(4)
            wr32(id, 7)
            #expect(svcCall("OSAGenericToRealID", generic, id, comp) == 0)
            #expect(svcCall("OSARealToGenericID", generic, id, rd32(inst)) == 0 && rd32(id) == 7)
            #expect(svcCall("OSAGetScriptingComponentFromStored", generic, textDesc("x"), sub) == 0)
            // Storage types: see OS9RestScriptTests (the trailer is os9rest's)
            let h = tb_NewHandle(3, false, true)
            #expect(svcCall("OSAGetStorageType", h, sub) == 0 && rd32(sub) == fourCC("scpt"))
            #expect(svcCall("OSAAddStorageType", h, fourCC("jsrc")) == 0)
            #expect(svcCall("OSAGetStorageType", h, sub) == 0 && rd32(sub) == fourCC("jsrc"))
            #expect(svcCall("OSARemoveStorageType", h) == 0)
            _ = q
        }

        @Test func applicationTerminology() {
            let aete: [UInt8] = [1, 1, 0, 0, 0, 0, 0, 0, 0, 0]
            let dir = launchTestApp([TestResource(type: "aete", id: 0, data: aete)])
            _ = dir
            let q = QTTune()
            q.install()
            let ci = svcCall("OpenDefaultComponent", fourCC("osa "), fourCC("ascr"))
            let spec = svcPtr(70), launched = svcPtr(2), list = svcPtr(8)
            #expect(tb_FSMakeFSSpec(0, 0, svcPStr("Test App"), spec) == 0)
            wr8(launched, 9)
            #expect(svcCall("OSAGetAppTerminology", ci, 0, spec, 0, launched, list) == 0 && rd8(launched) == 0)
            let n = svcPtr(4), item = svcPtr(8)
            #expect(tb_AECountItems(list, n) == 0 && rd32(n) == 1)
            #expect(tb_AEGetNthDesc(list, 1, Int32(bitPattern: fourCC("aete")), 0, item) == 0)
            #expect(svcBytes(rd32(rd32(item + 4)), 10) == aete)
            #expect(svcCall("ASGetAppTerminology", ci, spec, 0, launched, list) == 0)
            // No such resource id; no terminology for the system without the AppleScript file
            #expect(svcStatus(svcCall("OSAGetAppTerminology", ci, 0, spec, 5, launched, list)) == -2709)
            #expect(svcStatus(svcCall("OSAGetSysTerminology", ci, 0, 0, list)) == -2709)
        }

        @Test func copiedScriptsAreTheirOwn() {
            launchTestApp()
            let q = QTTune()
            q.install()
            let ci = svcCall("OpenDefaultComponent", fourCC("osa "), fourCC("ascr"))
            let data = AS.script([(.ident("x"), .long(5))])
            let script = loaded(ci, data)
            let copy = svcPtr(4)
            #expect(svcCall("OSACopyScript", ci, script, copy) == 0 && rd32(copy) != script)
            let newValue = svcPtr(4), vid = svcPtr(4)
            #expect(svcCall("OSACoerceFromDesc", ci, textDesc("changed"), 0, newValue) == 0)
            #expect(svcCall("OSASetProperty", ci, 0, rd32(copy), textDesc("x"), rd32(newValue)) == 0)
            #expect(svcCall("OSAGetProperty", ci, 0, script, textDesc("x"), vid) == 0)
            #expect(shown(ci, rd32(vid)) == "5") // the original keeps its own
            #expect(svcCall("OSAGetProperty", ci, 0, rd32(copy), textDesc("x"), vid) == 0)
            #expect(shown(ci, rd32(vid)) == "\"changed\"")
            #expect(svcStatus(svcCall("OSACopyScript", ci, 999, copy)) == -1751)
            _ = q
        }
    }
}
