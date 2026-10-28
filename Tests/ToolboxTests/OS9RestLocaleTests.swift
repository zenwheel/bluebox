import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// LocalesLib.
    @Suite struct OS9RestLocale {
        init() { bootMachine() }

        func ref(_ s: String) -> UInt32 {
            let out = Rest.ptr(4)
            #expect(Rest.signed("LocaleRefFromLocaleString", [Rest.cstr(s), out]) == 0, "\(s)")
            return rd32(out)
        }

        func part(_ r: UInt32, _ mask: UInt32) -> String? {
            let buf = Rest.ptr(64)
            return Rest.signed("LocaleRefGetPartString", [r, mask, 64, buf]) == 0 ? Rest.string(buf) : nil
        }

        func unichars(_ p: UInt32, _ n: UInt32) -> String {
            String(decoding: (0..<n).map { UInt8(truncatingIfNeeded: rd16(p + 2 * $0)) }, as: UTF8.self)
        }

        @Test func stringsAndParts() {
            let us = ref("en_US")
            #expect(us != 0 && ref("en_US") == us && ref("fr_CH") != us)
            #expect(part(us, 1) == "en" && part(us, 16) == "US" && part(us, 63) == "en_US" && part(us, 1 | 16) == "en_US")
            #expect(part(us, 4) == "") // no script
            let v = ref("sr_Cyrl_YU")
            #expect(part(v, 1) == "sr" && part(v, 4) == "Cyrl" && part(v, 16) == "YU" && part(v, 63) == "sr_Cyrl_YU")
            let w = ref("de_DE_POSIX")
            #expect(part(w, 32) == "POSIX" && part(w, 16) == "DE")
            let l = ref("zh")
            #expect(part(l, 16) == "" && part(l, 63) == "zh")
            let out = Rest.ptr(4)
            for bad in ["", "E", "english", "en__US", "EN_US", "e1_US", "en_us_"] {
                #expect(Rest.signed("LocaleRefFromLocaleString", [Rest.cstr(bad), out]) == -50, "\(bad)")
            }
            #expect(Rest.signed("LocaleRefFromLocaleString", [0, out]) == -50)
            let buf = Rest.ptr(8)
            #expect(Rest.signed("LocaleRefGetPartString", [us, 63, 5, buf]) == -30001) // "en_US" needs 6
            #expect(Rest.signed("LocaleRefGetPartString", [us, 63, 6, buf]) == 0)
            #expect(Rest.signed("LocaleRefGetPartString", [0x1234, 63, 8, buf]) == -50)
        }

        @Test func codes() {
            let out = Rest.ptr(4)
            func fromCodes(_ lang: Int16, _ region: Int16) -> String? {
                guard Rest.signed("LocaleRefFromLangOrRegionCode", [UInt32(UInt16(bitPattern: lang)),
                                                                      UInt32(UInt16(bitPattern: region)), out]) == 0 else { return nil }
                return part(rd32(out), 63)
            }
            #expect(fromCodes(-128, 1) == "fr_FR")        // a region
            #expect(fromCodes(0, 14) == "ja_JP")          // the region wins
            #expect(fromCodes(11, -128) == "ja")          // a language
            #expect(fromCodes(33, -128) == "zh")
            #expect(fromCodes(-128, -128) == "en_US")     // the system's own
            #expect(fromCodes(-128, 70) == "de_DE")       // reformed German is German
            #expect(fromCodes(999, -128) == nil)
            #expect(fromCodes(-128, 38) == nil)           // unassigned
            let lang = Rest.ptr(2), region = Rest.ptr(2)
            func toCodes(_ s: String) -> (Int16, Int16)? {
                guard Rest.signed("LocaleStringToLangAndRegionCodes", [Rest.cstr(s), lang, region]) == 0 else { return nil }
                return (Int16(bitPattern: rd16(lang)), Int16(bitPattern: rd16(region)))
            }
            #expect(toCodes("fr_CA")! == (1, 11))
            #expect(toCodes("en_US")! == (0, 0))
            #expect(toCodes("zh_TW")! == (19, 53))
            #expect(toCodes("zh_CN")! == (33, 52))
            #expect(toCodes("ja")! == (11, -128))
            #expect(toCodes("en_NZ")! == (0, -128)) // a region of no code
            #expect(toCodes("xx_YY")! == (-128, -128))
            #expect(toCodes("he_IL")! == (10, 13))
            #expect(toCodes("nonsense") == nil)
        }

        @Test func names() {
            let out = Rest.ptr(128), actual = Rest.ptr(4)
            let ch = ref("fr_CH")
            #expect(Rest.signed("LocaleGetName", [ch, 0, 1, 0, 64, actual, out]) == 0)
            #expect(unichars(out, rd32(actual)) == "French (Switzerland)")
            #expect(Rest.signed("LocaleGetName", [ref("en_US"), 0, 1, ref("en_GB"), 64, actual, out]) == 0)
            #expect(unichars(out, rd32(actual)) == "English (United States)")
            // another display language: English, and the status says so
            #expect(Rest.signed("LocaleGetName", [ref("ja_JP"), 0, 1, ref("fr_FR"), 64, actual, out]) == -30029)
            #expect(unichars(out, rd32(actual)) == "Japanese (Japan)")
            // a language alone, one the table has no region name for
            #expect(Rest.signed("LocaleGetName", [ref("cy"), 0, 1, 0, 64, actual, out]) == 0)
            #expect(unichars(out, rd32(actual)) == "Welsh")
            #expect(Rest.signed("LocaleGetName", [ref("en_NZ"), 0, 1, 0, 64, actual, out]) == 0)
            #expect(unichars(out, rd32(actual)) == "English (NZ)")
            // too small a buffer: the length it needs
            #expect(Rest.signed("LocaleGetName", [ch, 0, 1, 0, 4, actual, out]) == -30001 && rd32(actual) == 20)
            #expect(Rest.signed("LocaleGetName", [ch, 0, 0, 0, 64, actual, out]) == -50) // no name asked for
            #expect(Rest.signed("LocaleGetName", [0x99, 0, 1, 0, 64, actual, out]) == -50)

            let count = Rest.ptr(4), display = Rest.ptr(4)
            #expect(Rest.signed("LocaleCountNames", [ch, 0, 1, count]) == 0 && rd32(count) == 1)
            #expect(Rest.signed("LocaleGetIndName", [ch, 0, 1, 1, 64, actual, out, display]) == 0)
            #expect(unichars(out, rd32(actual)) == "French (Switzerland)" && rd32(display) == ref("en_US"))
            #expect(Rest.signed("LocaleGetIndName", [ch, 0, 1, 2, 64, actual, out, display]) == -50)
            #expect(Rest.signed("LocaleGetIndName", [ch, 0, 1, 0, 64, actual, out, display]) == -50)

            let name = Rest.ptr(256)
            #expect(Rest.signed("LocaleGetRegionLanguageName", [1, name]) == 0 && pstring(at: name) == "French")
            #expect(Rest.signed("LocaleGetRegionLanguageName", [6, name]) == 0 && pstring(at: name) == "Flemish")
            #expect(Rest.signed("LocaleGetRegionLanguageName", [70, name]) == 0 && pstring(at: name) == "German")
            #expect(Rest.signed("LocaleGetRegionLanguageName", [38, name]) == -192 && rd8(name) == 0)
            #expect(Rest.signed("LocaleGetRegionLanguageName", [UInt32(bitPattern: -1), name]) == -50)
        }

        @Test func operationsHaveNoTables() {
            let n = Rest.ptr(4), name = Rest.ptr(64)
            let cls = UInt32(0x636F_6C6C) // 'coll'
            #expect(Rest.signed("LocaleOperationCountLocales", [cls, n]) == 0 && rd32(n) == 0)
            wr32(n, 9)
            #expect(Rest.signed("LocaleOperationGetLocales", [cls, 4, n, Rest.ptr(32)]) == 0 && rd32(n) == 0)
            #expect(Rest.signed("LocaleOperationCountNames", [cls, n]) == 0 && rd32(n) == 0)
            #expect(Rest.signed("LocaleOperationGetName", [cls, 0, 32, n, name]) == -50)
            #expect(Rest.signed("LocaleOperationGetIndName", [cls, 1, 32, n, name, Rest.ptr(4)]) == -50)
            let index = Rest.ptr(4), data = Rest.ptr(4)
            wr32(data, 7)
            #expect(Rest.signed("LocaleOperationLoadData", [0, cls, 0, index, data]) == -50)
            #expect(rd32(index) == 0xFFFF_FFFF && rd32(data) == 0)
            #expect(Rest.signed("LocaleOperationUnloadData", [0, data]) == -50)
        }
    }
}
