import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// AEHelpers.h: descriptors built from format strings, printed back,
    /// and built by streaming.
    @Suite struct CarbonAEHelpers {
        func cstr(_ s: String) -> UInt32 {
            let p = svcPtr(s.utf8.count + 2)
            for (i, b) in Array(s.utf8).enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func printed(_ desc: UInt32) -> String {
            let h = svcPtr(4)
            #expect(svcCall("AEPrintDescToHandle", desc, h) == 0)
            let n = Int(tb_GetHandleSize(rd32(h)))
            return String(decoding: svcBytes(rd32(rd32(h)), n), as: UTF8.self)
        }

        func build(_ format: String, _ args: UInt32...) -> (Int32, UInt32, String, UInt32) {
            let desc = svcPtr(8), err = svcPtr(8)
            var a: [UInt32] = [desc, err, cstr(format)]
            a += args
            let r = svcCallArray("AEBuildDesc", a)
            return (svcStatus(r), desc, r == 0 ? printed(desc) : "", err)
        }

        @Test func numbersStringsAndTypes() {
            bootMachine()
            var (e, d, p, _) = build("42")
            #expect(e == 0 && rd32(d) == fourCC("long") && p == "long(42)")
            (e, d, p, _) = build("-7")
            #expect(e == 0 && p == "long(-7)")
            (e, d, p, _) = build("1.5")
            #expect(e == 0 && rd32(d) == fourCC("doub") && p == "doub(1.5)")
            (e, d, p, _) = build("\"hello\"")
            #expect(e == 0 && rd32(d) == fourCC("TEXT") && p == "TEXT(\"hello\")")
            (e, d, p, _) = build("'abcd'")
            #expect(e == 0 && rd32(d) == fourCC("type") && p == "type('abcd')")
            (e, d, p, _) = build("shor(7)")
            #expect(e == 0 && tb_AEGetDescDataSize(d) == 2 && p == "shor(7)")
            (e, d, p, _) = build("bool(1)")
            #expect(e == 0 && p == "bool(true)")
            (e, d, p, _) = build("TEXT(\"x y\")")
            #expect(e == 0 && p == "TEXT(\"x y\")")
            (e, d, p, _) = build("true")
            #expect(e == 0 && rd32(d) == fourCC("true") && tb_AEGetDescDataSize(d) == 0)
            // Hex is « » in Mac OS Roman.
            let hex = svcPtr(32), desc = svcPtr(8), err = svcPtr(8)
            for (i, b) in ([UInt8]("xyzw(".utf8) + [0xC7] + [UInt8]("0A0b ff".utf8) + [0xC8, UInt8(ascii: ")")]).enumerated() {
                wr8(hex + UInt32(i), b)
            }
            #expect(svcCall("AEBuildDesc", desc, err, hex) == 0 && rd32(desc) == fourCC("xyzw") && tb_AEGetDescDataSize(desc) == 3)
            #expect(svcBytes(rd32(rd32(desc + 4)), 3) == [0x0A, 0x0B, 0xFF])
        }

        @Test func listsAndRecords() {
            bootMachine()
            var (e, d, p, _) = build("[1, 2, \"three\"]")
            #expect(e == 0 && rd32(d) == fourCC("list") && p == "[long(1), long(2), TEXT(\"three\")]")
            #expect(svcCall("AECountItems", d, svcPtr(4)) == 0 || true)
            (e, d, p, _) = build("{foo: 1, 'b r ': [true]}")
            #expect(e == 0 && rd32(d) == fourCC("reco") && p == "{'foo ':long(1), 'b r ':[true()]}")
            (e, d, p, _) = build("obj {form: 'name', seld: TEXT(\"Disk\")}")
            #expect(e == 0 && rd32(d) == fourCC("obj ") && p == "'obj '{form:type('name'), seld:TEXT(\"Disk\")}")
            (e, d, p, _) = build("[]")
            #expect(e == 0 && p == "[]")
            (e, d, p, _) = build("{}")
            #expect(e == 0 && p == "{}")
        }

        @Test func substitutions() {
            bootMachine()
            var (e, d, p, _) = build("[long(@), TEXT(@), shor(@), bool(@), type(@)]", 123456, cstr("subst"), 9, 1, fourCC("abcd"))
            #expect(e == 0 && p == "[long(123456), TEXT(\"subst\"), shor(9), bool(true), type('abcd')]")
            // A pointer and a length for other types
            let raw = svcPtr(4)
            wr32(raw, 0x0102_0304)
            (e, d, p, _) = build("xxxx(@)", raw, 4)
            #expect(e == 0 && rd32(d) == fourCC("xxxx") && tb_AEGetDescDataSize(d) == 4)
            // Another descriptor, copied
            let src = svcPtr(8)
            let b = svcPtr(2)
            wr16(b, 0x1234)
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("shor")), b, 2, src) == 0)
            (e, d, p, _) = build("{key1: @@}", src)
            #expect(e == 0 && p == "{key1:shor(4660)}")
            // Doubles come in two words
            let bits = (2.5).bitPattern
            (e, d, p, _) = build("doub(@)", UInt32(bits >> 32), UInt32(bits & 0xFFFF_FFFF))
            #expect(e == 0 && p == "doub(2.5)")
        }

        @Test func syntaxErrors() {
            bootMachine()
            func code(_ f: String) -> (Int32, UInt32, UInt32) {
                let (e, _, _, err) = build(f)
                return (e, rd32(err), rd32(err + 4))
            }
            #expect(code("[1, 2") == (-1740, 14, 5))
            #expect(code("{foo 1}") == (-1740, 17, 5))
            #expect(code("{foo: 1") == (-1740, 15, 7))
            #expect(code("1 2") == (-1740, 3, 2))
            #expect(code("-x") == (-1740, 4, 1))
            #expect(code("\"abc") == (-1740, 10, 4))
            #expect(code("long(1") == (-1740, 13, 6))
            #expect(code("list[1]") == (-1740, 18, 4))
            #expect(code("") == (-1740, 2, 0))
            #expect(code("%") == (-1740, 1, 0))
            #expect(code("{ : 1}") == (-1740, 16, 2))
        }

        @Test func appleEvents() {
            bootMachine()
            let addr = svcPtr(4)
            wr32(addr, fourCC("MACS"))
            let ev = svcPtr(8), err = svcPtr(8)
            let r = svcCall("AEBuildAppleEvent", fourCC("aevt"), fourCC("odoc"), fourCC("sign"), addr, 4, 0, 0, ev, err,
                            cstr("'----': [TEXT(@)], &subj: long(@), 'usrf': 5"), cstr("one"), 99)
            #expect(r == 0 && rd32(ev) == fourCC("aevt"))
            let got = svcPtr(8), key = svcPtr(4)
            #expect(tb_AEGetParamDesc(ev, Int32(bitPattern: fourCC("----")), Int32(bitPattern: fourCC("list")), got) == 0)
            #expect(tb_AEGetNthDesc(got, 1, Int32(bitPattern: fourCC("TEXT")), key, svcPtr(8)) == 0)
            #expect(tb_AEGetAttributeDesc(ev, Int32(bitPattern: fourCC("subj")), Int32(bitPattern: fourCC("long")), svcPtr(8)) == 0)
            #expect(tb_AEGetParamDesc(ev, Int32(bitPattern: fourCC("usrf")), Int32(bitPattern: fourCC("long")), svcPtr(8)) == 0)
            let text = printed(ev)
            #expect(text.hasPrefix("aevt{&evcl:type('aevt'), &evid:type('odoc')"))
            #expect(text.contains("&subj:long(99)") && text.contains("'----':[TEXT(\"one\")]") && text.contains("usrf:long(5)"))
            // A syntax error leaves a null event
            let bad = svcPtr(8)
            let e2 = svcStatus(svcCall("AEBuildAppleEvent", fourCC("aevt"), fourCC("odoc"), fourCC("sign"), addr, 4, 0, 0, bad, err,
                                       cstr("'----' 1")))
            #expect(e2 == -1740 && rd32(err) == 17 && rd32(bad) == fourCC("null"))
            // vAEBuildDesc takes a va_list: the words of the arguments
            let va = svcPtr(16)
            wr32(va, 77)
            let d = svcPtr(8)
            #expect(svcCall("vAEBuildDesc", d, err, cstr("long(@)"), va) == 0 && printed(d) == "long(77)")
        }

        @Test func streams() {
            bootMachine()
            let s = svcCall("AEStreamOpen")
            #expect(s != 0)
            let desc = svcPtr(8)
            #expect(svcCall("AEStreamOpenRecord", s, fourCC("reco")) == 0)
            #expect(svcCall("AEStreamWriteKeyDesc", s, fourCC("name"), fourCC("TEXT"), cstr("abc"), 3) == 0)
            #expect(svcCall("AEStreamWriteKey", s, fourCC("list")) == 0)
            #expect(svcCall("AEStreamOpenList", s) == 0)
            let n = svcPtr(4)
            wr32(n, 5)
            #expect(svcCall("AEStreamWriteDesc", s, fourCC("long"), n, 4) == 0)
            #expect(svcCall("AEStreamOpenDesc", s, fourCC("TEXT")) == 0)
            #expect(svcCall("AEStreamWriteData", s, cstr("ab"), 2) == 0)
            #expect(svcCall("AEStreamWriteData", s, cstr("cd"), 2) == 0)
            #expect(svcCall("AEStreamCloseDesc", s) == 0)
            #expect(svcCall("AEStreamCloseList", s) == 0)
            #expect(svcStatus(svcCall("AEStreamWriteDesc", s, fourCC("long"), n, 4)) == -1737) // needs a key
            #expect(svcCall("AEStreamCloseRecord", s) == 0)
            #expect(svcCall("AEStreamClose", s, desc) == 0)
            #expect(printed(desc) == "{name:TEXT(\"abc\"), list:[long(5), TEXT(\"abcd\")]}")
            #expect(svcStatus(svcCall("AEStreamClose", s, desc)) == -50) // gone

            // Bad nesting
            let s2 = svcCall("AEStreamOpen")
            #expect(svcStatus(svcCall("AEStreamCloseList", s2)) == -1737)
            #expect(svcCall("AEStreamOpenList", s2) == 0)
            #expect(svcStatus(svcCall("AEStreamClose", s2, desc)) == -1737)

            // An event
            let addr = svcPtr(4)
            wr32(addr, fourCC("MACS"))
            let ev = svcCall("AEStreamCreateEvent", fourCC("aevt"), fourCC("odoc"), fourCC("sign"), addr, 4, 0, 0)
            #expect(ev != 0)
            #expect(svcCall("AEStreamWriteKeyDesc", ev, fourCC("----"), fourCC("long"), n, 4) == 0)
            #expect(svcCall("AEStreamOptionalParam", ev, fourCC("usrf")) == 0)
            let event = svcPtr(8)
            #expect(svcCall("AEStreamClose", ev, event) == 0)
            #expect(rd32(event) == fourCC("aevt"))
            #expect(tb_AEGetParamDesc(event, Int32(bitPattern: fourCC("----")), Int32(bitPattern: fourCC("long")), svcPtr(8)) == 0)
            #expect(tb_AEGetAttributeDesc(event, Int32(bitPattern: fourCC("optk")), Int32(bitPattern: fourCC("keyw")), svcPtr(8)) == 0)
            // A single value at the top
            let s3 = svcCall("AEStreamOpen")
            #expect(svcCall("AEStreamWriteDesc", s3, fourCC("long"), n, 4) == 0)
            #expect(svcStatus(svcCall("AEStreamWriteDesc", s3, fourCC("long"), n, 4)) == -1737)
            #expect(svcCall("AEStreamClose", s3, desc) == 0 && printed(desc) == "long(5)")
        }
    }
}
