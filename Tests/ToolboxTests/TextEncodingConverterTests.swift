import CCore
import Testing
import Toolbox

extension BlueBox {
    /// The Text Encoding Converter (TextCommon, UnicodeConverter,
    /// TextEncodingConverter): the exports called as a PowerPC caller
    /// would, arguments in r3 on and past the eighth in the parameter area.
    @Suite struct TextEncodingConverter {
        let macRoman: UInt32 = 0, macJapanese: UInt32 = 1, utf16: UInt32 = 0x100, utf8: UInt32 = 0x0800_0100
        let shiftJIS: UInt32 = 0x0A01, latin1: UInt32 = 0x0201

        func status(_ v: Int32) -> UInt32 { UInt32(bitPattern: v) }

        @discardableResult
        func call(_ name: String, _ args: UInt32...) -> UInt32 {
            let sp = tb_NewPtr(256, false, true) + 64
            withUnsafeMutableBytes(of: &g_ppc.gpr) { raw in
                for (i, a) in args.prefix(8).enumerated() { raw.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
                raw.storeBytes(of: sp, toByteOffset: 4, as: UInt32.self)
            }
            for (i, a) in args.enumerated() where i >= 8 { wr32(sp + 24 + UInt32(4 * i), a) }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("\(name) is not exported")
                return 0xFFFF_FFFF
            }
            fn(&g_ppc)
            return g_ppc.gpr.3
        }

        func buffer(_ b: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        func bytes(_ p: UInt32, _ n: UInt32) -> [UInt8] { (0..<n).map { rd8(p + $0) } }
        func word() -> UInt32 { tb_NewPtr(4, false, true) }
        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func converter(_ from: UInt32, _ to: UInt32) -> UInt32 {
            let ref = word()
            #expect(call("TECCreateConverter", ref, from, to) == 0)
            return rd32(ref)
        }

        /// TECConvertText: (status, input taken, output).
        func convert(_ tec: UInt32, _ input: [UInt8], cap: UInt32 = 256) -> (UInt32, UInt32, [UInt8]) {
            let inLen = word(), outLen = word(), out = tb_NewPtr(Int32(cap), false, true)
            let st = call("TECConvertText", tec, buffer(input), UInt32(input.count), inLen, out, cap, outLen)
            return (st, rd32(inLen), bytes(out, rd32(outLen)))
        }

        @Test func textEncodingsAndScripts() {
            launchTestApp()
            let e = call("CreateTextEncoding", 0x100, 3, 2)
            #expect(e == 0x0803_0100)
            #expect(call("GetTextEncodingBase", e) == 0x100)
            #expect(call("GetTextEncodingVariant", e) == 3)
            #expect(call("GetTextEncodingFormat", e) == 2)
            #expect(call("ResolveDefaultTextEncoding", utf16) == 0x103)

            let out = word()
            // smRoman, kTextLanguageDontCare, kTextRegionDontCare: Mac OS Roman.
            #expect(call("UpgradeScriptInfoToTextEncoding", 0, 0xFFFF_FF80, 0xFFFF_FF80, 0, out) == 0)
            #expect(rd32(out) == macRoman)
            #expect(call("UpgradeScriptInfoToTextEncoding", 1, 0xFFFF_FF80, 0xFFFF_FF80, 0, out) == 0)
            #expect(rd32(out) == macJapanese)
            #expect(call("UpgradeScriptInfoToTextEncoding", 0xFFFF_FF80, 0xFFFF_FF80, 0xFFFF_FF80, pname("Symbol"), out) == 0)
            #expect(rd32(out) == 33)
            // smRoman with langTurkish: Mac OS Turkish, and back.
            #expect(call("UpgradeScriptInfoToTextEncoding", 0, 17, 0xFFFF_FF80, 0, out) == 0)
            #expect(rd32(out) == 35)
            let script = word(), lang = word()
            #expect(call("RevertTextEncodingToScriptInfo", 35, script, lang, 0) == 0)
            #expect(rd16(script) == 0 && rd16(lang) == 17)
            #expect(call("RevertTextEncodingToScriptInfo", latin1, script, lang, 0) == status(-8738))
            let best = word(), alt = word()
            #expect(call("NearestMacTextEncodings", shiftJIS, best, alt) == 0)
            #expect(rd32(best) == macJapanese && rd32(alt) == macRoman)

            let info = word()
            #expect(call("TECGetInfo", info) == 0)
            #expect(rd16(rd32(rd32(info)) + 2) == 0x0131)
        }

        @Test func macRomanToUTF8AndBack() {
            launchTestApp()
            // "Café über™" in Mac OS Roman.
            let roman: [UInt8] = [0x43, 0x61, 0x66, 0x8E, 0x20, 0x9F, 0x62, 0x65, 0x72, 0xAA]
            let utf: [UInt8] = Array("Café über™".utf8)
            let there = converter(macRoman, utf8)
            let (st, used, out) = convert(there, roman)
            #expect(st == 0 && used == UInt32(roman.count))
            #expect(out == utf)
            let back = converter(utf8, macRoman)
            let (st2, used2, out2) = convert(back, utf)
            #expect(st2 == 0 && used2 == UInt32(utf.count))
            #expect(out2 == roman)
            #expect(call("TECDisposeConverter", there) == 0)
            #expect(call("TECDisposeConverter", there) == status(-8748)) // kTECCorruptConverterErr
            #expect(call("TECCreateConverter", word(), macRoman, 0x7777) == status(-8749)) // kTECNoConversionPathErr
        }

        @Test func unicodeConverterWithAndWithoutFallbacks() {
            launchTestApp()
            let t2u = word()
            #expect(call("CreateTextToUnicodeInfoByEncoding", macRoman, t2u) == 0)
            let read = word(), len = word(), uni = tb_NewPtr(64, false, true)
            #expect(call("ConvertFromTextToUnicode", rd32(t2u), 2, buffer([0x8E, 0xAA]), 0, 0, 0, 0, 0,
                         64, read, len, uni) == 0)
            #expect(rd32(read) == 2 && rd32(len) == 4)
            #expect(bytes(uni, 4) == [0x00, 0xE9, 0x21, 0x22])

            // "A一B" to Mac OS Roman: the ideograph isn't there.
            let u2t = word()
            #expect(call("CreateUnicodeToTextInfoByEncoding", macRoman, u2t) == 0)
            let src = buffer([0x00, 0x41, 0x4E, 0x00, 0x00, 0x42]), out = tb_NewPtr(16, false, true)
            #expect(call("ConvertFromUnicodeToText", rd32(u2t), 6, src, 0, 0, 0, 0, 0, 16, read, len, out)
                    == status(-8754)) // kTECUnmappableElementErr
            #expect(rd32(read) == 2 && rd32(len) == 1 && rd8(out) == 0x41)
            // kUnicodeUseFallbacksMask: '?' in its place.
            #expect(call("ConvertFromUnicodeToText", rd32(u2t), 6, src, 1, 0, 0, 0, 0, 16, read, len, out)
                    == status(-8783)) // kTECUsedFallbacksStatus
            #expect(rd32(read) == 6 && bytes(out, rd32(len)) == [0x41, 0x3F, 0x42])
            #expect(call("DisposeUnicodeToTextInfo", u2t) == 0 && rd32(u2t) == 0)

            // Into Unicode, a byte Shift-JIS hasn't got: U+FFFD with fallbacks.
            #expect(call("CreateTextToUnicodeInfoByEncoding", shiftJIS, t2u) == 0)
            #expect(call("ConvertFromTextToUnicode", rd32(t2u), 4, buffer([0x41, 0xA0, 0x82, 0xA0]), 1, 0, 0, 0, 0,
                         64, read, len, uni) == status(-8783))
            #expect(bytes(uni, rd32(len)) == [0x00, 0x41, 0xFF, 0xFD, 0x30, 0x42])
            // A character cut short at the end: kTECPartialCharErr, the rest read.
            #expect(call("ConvertFromTextToUnicode", rd32(t2u), 2, buffer([0x41, 0x82]), 0, 0, 0, 0, 0,
                         64, read, len, uni) == status(-8753))
            #expect(rd32(read) == 1 && rd32(len) == 2)
        }

        @Test func runsOfEncodings() {
            launchTestApp()
            let run = word()
            let encs = buffer(be32(macRoman) + be32(macJapanese))
            #expect(call("CreateUnicodeToTextRunInfoByEncoding", 2, encs, run) == 0)
            // "Aあ"
            let read = word(), len = word(), out = tb_NewPtr(16, false, true), nruns = word()
            let runs = tb_NewPtr(64, false, true)
            #expect(call("ConvertFromUnicodeToTextRun", rd32(run), 4, buffer([0x00, 0x41, 0x30, 0x42]), 0, 0, 0, 0,
                         0, 16, read, len, out, 8, nruns, runs) == 0)
            #expect(bytes(out, rd32(len)) == [0x41, 0x82, 0xA0])
            #expect(rd32(nruns) == 2)
            #expect(rd32(runs) == 0 && rd32(runs + 4) == macRoman && rd32(runs + 8) == 1 && rd32(runs + 12) == macJapanese)
        }

        @Test func internetNames() {
            launchTestApp()
            let e = word()
            #expect(call("TECGetTextEncodingFromInternetName", e, pname("iso-8859-1")) == 0)
            #expect(rd32(e) == latin1)
            #expect(call("TECGetTextEncodingFromInternetName", e, pname("Shift_JIS")) == 0)
            #expect(rd32(e) == shiftJIS)
            #expect(call("TECGetTextEncodingFromInternetName", e, pname("x-no-such")) == status(-8738))
            let name = tb_NewPtr(256, false, true)
            #expect(call("TECGetTextEncodingInternetName", utf8, name) == 0)
            #expect(bytes(name + 1, UInt32(rd8(name))) == Array("UTF-8".utf8))
            #expect(call("TECGetTextEncodingInternetName", macRoman, name) == 0)
            #expect(bytes(name + 1, UInt32(rd8(name))) == Array("macintosh".utf8))
        }

        @Test func streamingKeepsACharacterCutShort() {
            launchTestApp()
            // é in UTF-8 split between two calls.
            let tec = converter(utf8, macRoman)
            let (st1, in1, out1) = convert(tec, [0x61, 0xC3])
            #expect(st1 == 0 && in1 == 2 && out1 == [0x61])
            let (st2, in2, out2) = convert(tec, [0xA9, 0x62])
            #expect(st2 == 0 && in2 == 2 && out2 == [0x8E, 0x62])
            // Shift-JIS (the host's) split the same way: あ.
            let sj = converter(shiftJIS, utf8)
            let (_, a, o1) = convert(sj, [0x41, 0x82])
            #expect(a == 2 && o1 == [0x41])
            let (_, b, o2) = convert(sj, [0xA0])
            #expect(b == 1 && o2 == [0xE3, 0x81, 0x82])
            // Flushed while cut short: a fallback.
            _ = convert(sj, [0x82])
            let out = tb_NewPtr(16, false, true), len = word()
            #expect(call("TECFlushText", sj, out, 16, len) == status(-8783))
            #expect(bytes(out, rd32(len)) == [0xEF, 0xBF, 0xBD])
        }

        @Test func outputBufferFull() {
            launchTestApp()
            let tec = converter(macRoman, utf8)
            let (st, used, out) = convert(tec, [0x8E, 0x8E, 0x8E], cap: 3)
            #expect(st == status(-8785)) // kTECOutputBufferFullStatus
            #expect(used == 1 && out == [0xC3, 0xA9])
        }

        @Test func sniffer() {
            launchTestApp()
            let ref = word()
            let list: [UInt32] = [macRoman, shiftJIS, utf8]
            #expect(call("TECCreateSniffer", ref, buffer(list.flatMap(be32)), 3) == 0)
            let text = Array("café résumé".utf8)
            let encs = buffer(list.flatMap(be32)), errs = tb_NewPtr(12, false, true), feats = tb_NewPtr(12, false, true)
            #expect(call("TECSniffTextEncoding", rd32(ref), buffer(text), UInt32(text.count), encs, 3, errs, 100,
                         feats, 100) == 0)
            #expect(rd32(encs) == utf8 && rd32(errs) == 0 && rd32(feats) == 3)
            #expect(call("TECDisposeSniffer", rd32(ref)) == 0)
        }
    }
}
