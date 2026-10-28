import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Locale Manager and the Unicode Utilities.
    @Suite struct CarbonUnicode {
        func cstr(_ s: String) -> UInt32 {
            let p = svcPtr(s.utf8.count + 2)
            for (i, b) in Array(s.utf8).enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func units(_ s: String) -> (UInt32, UInt32) { svcUTF16(s) }

        func readUnits(_ p: UInt32, _ n: Int) -> String {
            String(decoding: (0..<n).map { rd16(p + UInt32(2 * $0)) }, as: UTF16.self)
        }

        func locale(_ s: String) -> UInt32 {
            let out = svcPtr(4)
            #expect(svcCall("LocaleRefFromLocaleString", cstr(s), out) == 0)
            return rd32(out)
        }

        func part(_ l: UInt32, _ mask: UInt32) -> String {
            let out = svcPtr(32)
            #expect(svcCall("LocaleRefGetPartString", l, mask, 32, out) == 0)
            var n = 0
            while rd8(out + UInt32(n)) != 0 { n += 1 }
            return String(decoding: svcBytes(out, n), as: UTF8.self)
        }

        @Test func localesFromCodesAndStrings() {
            bootMachine()
            let out = svcPtr(4)
            #expect(svcCall("LocaleRefFromLangOrRegionCode", 0, 0, out) == 0)
            #expect(part(rd32(out), 0x3F) == "en_US")
            #expect(svcCall("LocaleRefFromLangOrRegionCode", 0xFF80, 14, out) == 0)
            #expect(part(rd32(out), 0x3F) == "ja_JP" && part(rd32(out), 1) == "ja" && part(rd32(out), 16) == "JP")
            #expect(svcCall("LocaleRefFromLangOrRegionCode", 1, 0, out) == 0 && part(rd32(out), 0x3F) == "en_US") // the region wins
            #expect(svcStatus(svcCall("LocaleRefFromLangOrRegionCode", 999, 0xFF80, out)) == -50)
            // The same string is the same ref
            #expect(locale("en_US") != locale("fr_CA"))
            #expect(svcStatus(svcCall("LocaleRefGetPartString", locale("en_US"), 0x3F, 3, svcPtr(8))) == -30001)
            // Back to codes
            let lang = svcPtr(2), region = svcPtr(2)
            #expect(svcCall("LocaleStringToLangAndRegionCodes", cstr("fr_CA"), lang, region) == 0)
            #expect(rd16(lang) == 1 && rd16(region) == 11)
            #expect(svcCall("LocaleStringToLangAndRegionCodes", cstr("de_CH"), lang, region) == 0 && rd16(lang) == 2 && rd16(region) == 19)
            #expect(svcCall("LocaleStringToLangAndRegionCodes", cstr("zh_Hant_TW"), lang, region) == 0 && rd16(lang) == 19 && rd16(region) == 53)
            #expect(svcCall("LocaleStringToLangAndRegionCodes", cstr("xx_ZZ"), lang, region) == 0 && rd16(lang) == 0xFF80 && rd16(region) == 0xFF80) // both don't-care (-128)
        }

        @Test func localeNamesAndOperations() {
            bootMachine()
            let name = svcPtr(128), n = svcPtr(4)
            let us = locale("en_US"), fr = locale("fr_FR")
            #expect(svcCall("LocaleGetName", us, 0, 1, us, 64, n, name) == 0)
            #expect(readUnits(name, Int(rd32(n))) == "English (United States)")
            // In another display language: the English, with the status that says so
            #expect(svcStatus(svcCall("LocaleGetName", us, 0, 1, fr, 64, n, name)) == -30029)
            #expect(readUnits(name, Int(rd32(n))) == "English (United States)")
            #expect(svcStatus(svcCall("LocaleGetName", us, 0, 1, us, 4, n, name)) == -30001 && rd32(n) == 23)
            let count = svcPtr(4), shown = svcPtr(4)
            #expect(svcCall("LocaleCountNames", us, 0, 1, count) == 0 && rd32(count) == 1)
            #expect(svcCall("LocaleGetIndName", us, 0, 1, 1, 64, n, name, shown) == 0 && rd32(shown) == locale("en_US"))
            #expect(svcStatus(svcCall("LocaleGetIndName", us, 0, 1, 2, 64, n, name, shown)) == -50)
            let pname = svcPtr(256)
            #expect(svcCall("LocaleGetRegionLanguageName", 1, pname) == 0)
            #expect(String(decoding: svcBytes(pname + 1, Int(rd8(pname))), as: UTF8.self) == "French")
            // Collation and text break serve many locales; others none
            let k = svcPtr(4)
            #expect(svcCall("LocaleOperationCountLocales", fourCC("ucol"), k) == 0 && rd32(k) > 20)
            #expect(svcCall("LocaleOperationCountLocales", fourCC("zzzz"), k) == 0 && rd32(k) == 0)
            let list = svcPtr(8 * 4), actual = svcPtr(4)
            #expect(svcCall("LocaleOperationGetLocales", fourCC("ubrk"), 4, actual, list) == 0 && rd32(actual) == 4)
            #expect(rd32(list) >> 16 == 0x4C43 && rd32(list + 4) == 0)
            #expect(svcCall("LocaleOperationGetName", fourCC("ucol"), 0, 64, n, name) == 0 && readUnits(name, Int(rd32(n))) == "Collation")
            #expect(svcCall("LocaleOperationCountNames", fourCC("ucol"), k) == 0 && rd32(k) == 1)
            #expect(svcCall("LocaleOperationGetIndName", fourCC("ubrk"), 1, 64, n, name, shown) == 0 && readUnits(name, Int(rd32(n))) == "Text Breaking")
        }

        @Test func characterProperties() {
            bootMachine()
            let v = svcPtr(4)
            func prop(_ s: String, _ type: UInt32) -> UInt32 {
                let (p, n) = units(s)
                #expect(svcCall("UCGetCharProperty", p, n, type, v) == 0)
                return rd32(v)
            }
            #expect(prop("A", 1) == 14 && prop("a", 1) == 15 && prop("5", 1) == 8 && prop(" ", 1) == 11 && prop("!", 1) == 26)
            #expect(prop("\u{0301}", 1) == 5 && prop("\u{0301}", 2) == 230 && prop("a", 2) == 0)
            #expect(prop("\u{05D0}", 3) == 2 && prop("a", 3) == 1 && prop("5", 3) == 3 && prop("\u{0627}", 3) == 12)
            #expect(prop("\u{1F600}", 1) == 31) // a surrogate pair
            let (p, _) = units("a")
            #expect(svcStatus(svcCall("UCGetCharProperty", p, 1, 9, v)) == -50)
            let hi = svcPtr(2)
            wr16(hi, 0xD83D)
            #expect(svcStatus(svcCall("UCGetCharProperty", hi, 1, 1, v)) == -8739)
        }

        @Test func comparingAndCollationKeys() {
            bootMachine()
            func compare(_ a: String, _ b: String, _ options: UInt32) -> Int32 {
                let (pa, na) = units(a), (pb, nb) = units(b)
                let eq = svcPtr(1), order = svcPtr(4)
                #expect(svcCall("UCCompareTextNoLocale", options, pa, na, pb, nb, eq, order) == 0)
                return Int32(bitPattern: rd32(order))
            }
            #expect(compare("apple", "banana", 0) == -1 && compare("b", "a", 0) == 1 && compare("a", "a", 0) == 0)
            #expect(compare("a", "A", 0) != 0 && compare("a", "A", 1 << 3) == 0) // case insensitive
            #expect(compare("é", "e", 0) != 0 && compare("é", "e", 1 << 4) == 0) // diacritic insensitive
            #expect(compare("a10", "a9", 0) == -1 && compare("a10", "a9", 1 << 17) == 1) // digits as numbers
            // The user's locale and a collator
            let (pa, na) = units("Zebra"), (pb, nb) = units("apple")
            let eq = svcPtr(1), order = svcPtr(4)
            #expect(svcCall("UCCompareTextDefault", 1 << 3, pa, na, pb, nb, eq, order) == 0 && Int32(bitPattern: rd32(order)) == 1)
            let ref = svcPtr(4)
            #expect(svcCall("UCCreateCollator", locale("en_US"), 0, 1 << 3, ref) == 0)
            #expect(svcCall("UCCompareText", rd32(ref), pa, na, pb, nb, eq, order) == 0 && Int32(bitPattern: rd32(order)) == 1)
            // Keys order as the texts do
            func key(_ s: String) -> [UInt32] {
                let (p, n) = units(s)
                let keybuf = svcPtr(256), size = svcPtr(4)
                #expect(svcCall("UCGetCollationKey", rd32(ref), p, n, 64, size, keybuf) == 0)
                return (0..<Int(rd32(size))).map { rd32(keybuf + UInt32(4 * $0)) }
            }
            let keys = ["banana", "Apple", "apple", "apples"].map { s -> (String, [UInt32]) in (s, key(s)) }
            _ = keys
            func cmpKeys(_ a: String, _ b: String) -> Int32 {
                let ka = key(a), kb = key(b)
                let pa = svcPtr(ka.count * 4), pb = svcPtr(kb.count * 4)
                for (i, x) in ka.enumerated() { wr32(pa + UInt32(4 * i), x) }
                for (i, x) in kb.enumerated() { wr32(pb + UInt32(4 * i), x) }
                #expect(svcCall("UCCompareCollationKeys", pa, UInt32(ka.count), pb, UInt32(kb.count), eq, order) == 0)
                return Int32(bitPattern: rd32(order))
            }
            #expect(cmpKeys("apple", "banana") == -1 && cmpKeys("banana", "apple") == 1)
            #expect(cmpKeys("apple", "apples") == -1)
            #expect(cmpKeys("Apple", "apple") == 0) // this collator ignores case
            let sizeOnly = svcPtr(4)
            let (pp, np) = units("apple")
            #expect(svcStatus(svcCall("UCGetCollationKey", rd32(ref), pp, np, 1, sizeOnly, svcPtr(8))) == -25340 && rd32(sizeOnly) > 1)
            #expect(svcCall("UCDisposeCollator", ref) == 0 && rd32(ref) == 0)
            #expect(svcStatus(svcCall("UCDisposeCollator", ref)) == -50)
        }

        @Test func textBreaks() {
            bootMachine()
            let ref = svcPtr(4), at = svcPtr(4)
            #expect(svcCall("UCCreateTextBreakLocator", 0, 0, 1 | 4 | 16, ref) == 0)
            let (p, n) = units("Hello big world")
            // words: Hello|space|big|space|world: 0 5 6 9 10 15
            var offsets: [UInt32] = [0]
            while true {
                #expect(svcCall("UCFindTextBreak", rd32(ref), 16, 1 | 4, p, n, offsets.last!, at) == 0) // leading edge, iterate
                offsets.append(rd32(at))
                if rd32(at) >= n { break }
            }
            #expect(offsets == [0, 5, 6, 9, 10, 15])
            #expect(svcCall("UCFindTextBreak", rd32(ref), 16, 2 | 4, p, n, 15, at) == 0 && rd32(at) == 10) // backwards
            // Clusters keep a base and its mark together
            let (q, m) = units("e\u{0301}x")
            #expect(svcCall("UCFindTextBreak", rd32(ref), 4, 1 | 4, q, m, 0, at) == 0 && rd32(at) == 2)
            // A kind the locator wasn't made for
            #expect(svcStatus(svcCall("UCFindTextBreak", rd32(ref), 64, 0, p, n, 0, at)) == -25341)
            #expect(svcCall("UCDisposeTextBreakLocator", ref) == 0 && rd32(ref) == 0)
        }

        /// A keyboard layout ('uchr') built here: key 0 is "a" (which combines with the dead acute),
        /// 1 "s", 2 the sequence "ae", 3 the dead acute; shift makes key 0 "A".
        func keyboardLayout() -> UInt32 {
            var d = [UInt8](repeating: 0, count: 160)
            func w16(_ at: Int, _ v: Int) { d[at] = UInt8(v >> 8 & 0xFF); d[at + 1] = UInt8(v & 0xFF) }
            func w32(_ at: Int, _ v: Int) { w16(at, v >> 16 & 0xFFFF); w16(at + 2, v & 0xFFFF) }
            w16(0, 0x1002); w32(8, 1) // header format, one keyboard type
            w32(12, 0); w32(16, 0xFFFF)
            w32(20, 40); w32(24, 52); w32(28, 100); w32(32, 132); w32(36, 140) // the offsets of its tables
            w16(40, 0x3001); w16(42, 0); w32(44, 2); d[48] = 0; d[49] = 1 // shift is table 1
            w16(52, 0x4001); w16(54, 8); w32(56, 2); w32(60, 68); w32(64, 84)
            for (i, v) in [0x4001, 0x73, 0x8000, 0x4000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF].enumerated() { w16(68 + 2 * i, v) }
            for (i, v) in [0x41, 0x53, 0x8000, 0x4000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF].enumerated() { w16(84 + 2 * i, v) }
            w16(100, 0x5001); w16(102, 2); w32(104, 112); w32(108, 120) // state records: the dead acute, then "a"
            w16(112, 0xB4); w16(114, 1); w16(116, 0); w16(118, 1)
            w16(120, 0x61); w16(122, 0); w16(124, 1); w16(126, 1); w16(128, 1); w16(130, 0xE1) // state 1 + a = á
            w16(132, 0x6001); w16(134, 1); w16(136, 0xB4) // the terminator of state 1: the acute
            w16(140, 0x7001); w16(142, 1); w16(144, 8); w16(146, 12); w16(148, 0x61); w16(150, 0x65) // "ae"
            let p = svcPtr(d.count)
            for (i, b) in d.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        @Test func keyTranslation() {
            bootMachine()
            let layout = keyboardLayout()
            let dead = svcPtr(4), n = svcPtr(4), out = svcPtr(32)
            func press(_ key: UInt32, action: UInt32 = 0, mods: UInt32 = 0, options: UInt32 = 0, max: UInt32 = 8) -> (Int32, String, UInt32) {
                let e = svcStatus(svcCall("UCKeyTranslate", layout, key, action, mods, 0, options, dead, max, n, out))
                return (e, readUnits(out, Int(rd32(n))), rd32(dead))
            }
            wr32(dead, 0)
            #expect(press(1) == (0, "s", 0))
            #expect(press(0) == (0, "a", 0))
            #expect(press(0, mods: 1) == (0, "A", 0)) // table 1
            #expect(press(2) == (0, "ae", 0))
            // The dead acute says nothing and waits; a then gives á
            #expect(press(3) == (0, "", 1))
            #expect(press(0) == (0, "á", 0))
            // Another key after it: the acute itself, then the key
            #expect(press(3) == (0, "", 1))
            #expect(press(1) == (0, "´s", 0))
            // The acute twice: one acute out and again waiting
            #expect(press(3) == (0, "", 1))
            #expect(press(3) == (0, "´", 1))
            wr32(dead, 0)
            // Shown on a keycap it is the acute and waits for nothing; with dead keys off it is typed
            #expect(press(3, action: 3) == (0, "´", 0))
            #expect(press(3, options: 1) == (0, "´", 0))
            // Key up says nothing; a key the layout lacks, and too small a buffer
            #expect(press(0, action: 1) == (0, "", 0))
            #expect(press(7) == (0, "", 0))
            #expect(press(40).0 == -50)
            #expect(press(2, max: 1).0 == -25340 && rd32(n) == 2)
            #expect(svcStatus(svcCall("UCKeyTranslate", 0, 0, 0, 0, 0, 0, dead, 8, n, out)) == -50)
        }
    }
}
