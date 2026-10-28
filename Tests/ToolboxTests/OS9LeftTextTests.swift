import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// TextCommon's Unicode properties and the private calls around them,
    /// UnicodeConverter's and TextEncodingConverter's remaining names, and
    /// ATSUnicodeLib's memory settings.
    @Suite struct OS9LeftText {
        init() { Rest.boot() }

        func utf16(_ s: String) -> UInt32 {
            let u = Array(s.utf16)
            let p = Rest.ptr(max(u.count * 2, 2))
            for (i, x) in u.enumerated() { wr16(p + UInt32(i * 2), x) }
            return p
        }

        func prop(_ s: String, _ type: UInt32) -> UInt32 {
            let out = Rest.ptr(4)
            #expect(Rest.signed("UCGetCharProperty", [utf16(s), UInt32(s.utf16.count), type, out]) == 0)
            return rd32(out)
        }

        @Test func generalCategory() {
            #expect(prop("A", 1) == 14)       // Lu
            #expect(prop("a", 1) == 15)       // Ll
            #expect(prop("5", 1) == 8)        // Nd
            #expect(prop(" ", 1) == 11)       // Zs
            #expect(prop("\u{20AC}", 1) == 29) // Sc
            #expect(prop("(", 1) == 22)       // Ps
            #expect(prop("\u{0301}", 1) == 5) // Mn
            #expect(prop("\u{0378}", 1) == 0) // unassigned
            #expect(prop("\u{1F600}", 1) == 31) // a pair of surrogates is one character
        }

        @Test func combiningAndBidi() {
            #expect(prop("\u{0301}", 2) == 230)
            #expect(prop("a", 2) == 0)
            #expect(prop("a", 3) == 1)         // L
            #expect(prop("\u{05D0}", 3) == 2)  // R
            #expect(prop("\u{0627}", 3) == 12) // AL
            #expect(prop("1", 3) == 3)         // EN
            #expect(prop(" ", 3) == 10)        // WS
        }

        @Test func badArguments() {
            let out = Rest.ptr(4)
            #expect(Rest.signed("UCGetCharProperty", [0, 1, 1, out]) == -50)
            #expect(Rest.signed("UCGetCharProperty", [utf16("a"), 0, 1, out]) == -50)
            #expect(Rest.signed("UCGetCharProperty", [utf16("a"), 1, 9, out]) == -50)
            #expect(Rest.signed("UCGetCharProperty", [utf16("a"), 1, 1, 0]) == -50)
        }

        @Test func allThreeAtOnce() {
            let out = Rest.ptr(12)
            #expect(Rest.signed("GetCharacterProperties", [utf16("\u{0301}"), 1, out]) == 0)
            #expect(rd32(out) == 5 && rd32(out + 4) == 230 && rd32(out + 8) == 18)
        }

        @Test func tableAgreesWithTheCalls() {
            let tp = Rest.ptr(4)
            #expect(Rest.signed("UCLoadCharPropsTablePriv", [tp]) == 0)
            let t = rd32(tp)
            #expect(t != 0)
            var checked = 0
            for u in stride(from: 0, to: 0x3000, by: 7) {
                let s = String(UnicodeScalar(u) ?? " ")
                for type in UInt32(1)...3 where UnicodeScalar(u) != nil {
                    #expect(Rest.call("UCUseCharPropsTablePriv", [t, UInt32(u), type]) == prop(s, type), "\(u) \(type)")
                    checked += 1
                }
            }
            #expect(checked > 1000)
            // beyond the BMP the host answers
            #expect(Rest.call("UCUseCharPropsTablePriv", [t, 0x1F600, 1]) == 31)
            let out = Rest.ptr(4)
            #expect(Rest.signed("UCGetCharPropsPriv", [t, utf16("a"), 1, 1, out]) == 0 && rd32(out) == 15)
            #expect(Rest.signed("UCGetCharPropsPriv", [0, utf16("a"), 1, 1, out]) == -50)
            #expect(Rest.signed("UCUnloadCharPropsTablePriv", [t]) == 0)
            #expect(Rest.signed("UCUnloadCharPropsTablePriv", [t]) == -50)
        }

        @Test func handleChecks() {
            let h = tb_NewHandle(16, false, true)
            #expect(Rest.call("LockHandleCheck", [0]) == 0)
            let state = Rest.call("LockHandleCheck", [h])
            #expect(UInt8(bitPattern: tb_HGetState(h)) & 0x80 != 0)
            #expect(Rest.signed("UnlockHandleCheck", [h, state]) == 0)
            #expect(UInt8(bitPattern: tb_HGetState(h)) & 0x80 == 0)
            #expect(Rest.signed("UnlockHandleCheck", [0, 0]) == -109)
        }

        @Test func missingTable() {
            let out = Rest.ptr(4)
            #expect(Rest.signed("LoadTextCommonTable", [0x7A7A_7A7A, 12345, out]) != 0)
            #expect(rd32(out) == 0)
        }

        @Test func versionedGlobals() {
            #expect(Rest.call("TECGetGlobalsForVersion", [0x0100]) != 0)
            #expect(Rest.call("TECGetGlobalsForVersion", [0x0500]) == 0)
            #expect(Rest.call("TECGetGlobalsEntryForVersion", [0x0150]) == Rest.call("TECGetGlobalsForVersion", [0x0150]))
            let scripts = Rest.call("GetInstalledScriptsPtr")
            #expect(scripts != 0 && rd32(scripts) >= 1)
        }

        @Test func validConversionLength() {
            // Shift-JIS: the two-byte character 0x82A0 cut after its first byte
            let info = Rest.ptr(4)
            #expect(Rest.signed("CreateTextToUnicodeInfoByEncoding", [1, info]) == -8738 ||
                    Rest.signed("CreateTextToUnicodeInfoByEncoding", [0x0001, info]) == 0)
            let i = rd32(info)
            guard i != 0 else { return }
            let text = Rest.ptr(4)
            wr8(text, 0x41); wr8(text + 1, 0x82); wr8(text + 2, 0xA0); wr8(text + 3, 0x82)
            let valid = Rest.ptr(4)
            #expect(Rest.signed("UnicodeValidConversionLength", [i, text, 4, valid]) == 0)
            #expect(rd32(valid) == 4) // the text ends there
            #expect(Rest.signed("UnicodeValidConversionLength", [i, text, 4, 0]) == -50)
            #expect(Rest.call("LockTextToUnicodeInfo", [i]) == i)
            #expect(Rest.call("LockUnicodeToTextInfo", [i]) == 0)
        }

        @Test func converterFromPathAndExamine() {
            let conv = Rest.ptr(4)
            let path = Rest.ptr(12)
            wr32(path, 0x0000); wr32(path + 4, 0x0100); wr32(path + 8, 0x0000)
            #expect(Rest.signed("TECCreateOneToManyConverterFromPath", [conv, path, 3]) == 0)
            #expect(rd32(conv) != 0)
            #expect(Rest.signed("TECCreateOneToManyConverterFromPath", [conv, path, 1]) == -50)
            let src = Rest.cstr("caf\u{e9}")
            let dst = Rest.ptr(32)
            let used = Rest.ptr(4), made = Rest.ptr(4)
            #expect(Rest.signed("TECConvertTextBuffer", [rd32(conv), src, 5, dst, 32, used, made]) == 0)
            #expect(rd32(used) == 5 && rd32(made) > 0)
            let info = Rest.ptr(4)
            #expect(Rest.signed("TECExamineTextEncoding", [0x0000, info]) == 0 && rd32(info) != 0)
            #expect(Rest.signed("TECExamineTextEncoding", [0x7777, info]) == -8738)
        }

        @Test func memorySettings() {
            let cb = Rest.ptr(16)
            let s = Rest.ptr(4), s2 = Rest.ptr(4)
            #expect(Rest.call("ATSUGetCurrentMemorySetting") == 0)
            #expect(Rest.signed("ATSUCreateMemorySetting", [cb, s]) == 0)
            #expect(Rest.signed("ATSUCreateMemorySetting", [cb, s2]) == 0)
            #expect(rd32(s) != rd32(s2))
            #expect(Rest.signed("ATSUSetCurrentMemorySetting", [rd32(s)]) == 0)
            #expect(Rest.call("ATSUGetCurrentMemorySetting") == rd32(s))
            #expect(Rest.signed("ATSUDisposeMemorySetting", [rd32(s)]) != 0) // in use
            #expect(Rest.signed("ATSUDisposeMemorySetting", [rd32(s2)]) == 0)
            #expect(Rest.signed("ATSUSetCurrentMemorySetting", [0x1234]) == -50)
        }
    }
}
