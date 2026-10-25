import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// ATSUI on QuickDraw text: styles' attributes, layouts measured and
    /// drawn, line breaks, carets and fonts.
    @Suite struct CarbonATSUI {
        func gworld() -> UInt32 {
            let gw = svcPtr(4)
            #expect(tb_NewGWorld(gw, 32, svcRect(0, 0, 60, 200), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            _ = tb_LockPixels(tb_GetGWorldPixMap(rd32(gw)))
            tb_EraseRect(svcRect(0, 0, 60, 200))
            return rd32(gw)
        }

        /// A style of Geneva at a size.
        func style(size: Int) -> UInt32 {
            let v = svcPtr(16)
            #expect(svcCall("ATSUCreateStyle", v) == 0)
            let style = rd32(v)
            let font = svcPtr(4), sz = svcPtr(4)
            #expect(svcCall("ATSUFONDtoFontID", 3, 0, font) == 0)
            wr32(sz, UInt32(size) << 16)
            let tags = svcPtr(8), sizes = svcPtr(8), values = svcPtr(8)
            wr32(tags, 261); wr32(tags + 4, 262) // kATSUFontTag, kATSUSizeTag
            wr32(sizes, 4); wr32(sizes + 4, 4)
            wr32(values, font); wr32(values + 4, sz)
            #expect(svcCall("ATSUSetAttributes", style, 2, tags, sizes, values) == 0)
            return style
        }

        @Test func stylesHoldAttributes() {
            launchTestApp()
            _ = initToolbox()
            let s = style(size: 18)
            let out = svcPtr(8), size = svcPtr(4)
            #expect(svcCall("ATSUGetAttribute", s, 262, 4, out, size) == 0 && rd32(out) == 18 << 16)
            // Unset: the default, and kATSUNotSetErr.
            #expect(svcStatus(svcCall("ATSUGetAttribute", s, 263, 6, out, size)) == -8801 && rd32(size) == 6)
            #expect(svcStatus(svcCall("ATSUGetAttribute", s, 999, 4, out, size)) == -8801)
            let tags = svcPtr(4), sizes = svcPtr(4), values = svcPtr(4)
            wr32(tags, 262); wr32(sizes, 2); wr32(values, out)
            #expect(svcStatus(svcCall("ATSUSetAttributes", s, 1, tags, sizes, values)) == -8798) // wrong size
            wr32(tags, 300); wr32(sizes, 4)
            #expect(svcStatus(svcCall("ATSUSetAttributes", s, 1, tags, sizes, values)) == -8799) // not a tag
            let info = svcPtr(64), count = svcPtr(4)
            #expect(svcCall("ATSUGetAllAttributes", s, info, 8, count) == 0 && rd32(count) == 2)

            // Copies, comparisons, clearing.
            let v = svcPtr(4)
            #expect(svcCall("ATSUCreateAndCopyStyle", s, v) == 0)
            let copy = rd32(v)
            #expect(svcCall("ATSUCompareStyles", s, copy, v) == 0 && rd16(v) == 2) // equal
            wr32(tags, 256); wr32(sizes, 1); wr8(out, 1); wr32(values, out) // bold
            #expect(svcCall("ATSUSetAttributes", copy, 1, tags, sizes, values) == 0)
            #expect(svcCall("ATSUCompareStyles", copy, s, v) == 0 && rd16(v) == 1) // contains
            let types = svcPtr(4), sels = svcPtr(4)
            wr16(types, 1); wr16(sels, 2)
            #expect(svcCall("ATSUSetFontFeatures", copy, 1, types, sels) == 0)
            #expect(svcCall("ATSUGetFontFeature", copy, 0, types, sels) == 0 && rd16(sels) == 2)
            #expect(svcCall("ATSUClearStyle", copy) == 0)
            #expect(svcCall("ATSUStyleIsEmpty", copy, v) == 0 && rd8(v) == 1)
            #expect(svcCall("ATSUDisposeStyle", copy) == 0)
            #expect(svcStatus(svcCall("ATSUDisposeStyle", copy)) == -8791)
        }

        @Test func layoutsMeasureAndDraw() {
            launchTestApp()
            _ = initToolbox()
            _ = gworld()
            let s = style(size: 24)
            let (text, n) = svcUTF16("Hello")
            let lens = svcPtr(4), styles = svcPtr(4), v = svcPtr(16)
            wr32(lens, n); wr32(styles, s)
            #expect(svcCall("ATSUCreateTextLayoutWithTextPtr", text, 0, n, n, 1, lens, styles, v) == 0)
            let layout = rd32(v)

            // Measured as QuickDraw measures the same text.
            tb_TextFont(3)
            tb_TextSize(24)
            let expected = Int(tb_TextWidth(svcPStr("Hello") + 1, 0, 5))
            tb_TextSize(12)
            let before = svcPtr(4), after = svcPtr(4), ascent = svcPtr(4), descent = svcPtr(4)
            #expect(svcCall("ATSUMeasureText", layout, 0, n, before, after, ascent, descent) == 0)
            #expect(Int(rd32(after) >> 16) == expected && rd32(ascent) >> 16 > 10)
            #expect(rd16(qd_port() + UInt32(GrafPort_txSize_off)) == 12) // the port's size kept

            // Drawn: dark pixels above the baseline.
            #expect(svcCall("ATSUDrawText", layout, 0, n, 10 << 16, 40 << 16) == 0)
            let c = svcPtr(6)
            var dark = 0
            for x in 10..<(10 + expected) {
                for y in 20..<40 {
                    tb_GetCPixel(Int16(x), Int16(y), c)
                    if rd16(c) < 0x8000 { dark += 1 }
                }
            }
            #expect(dark > 20)
            let image = svcPtr(8)
            #expect(svcCall("ATSUMeasureTextImage", layout, 0, n, 10 << 16, 40 << 16, image) == 0)
            #expect(Int16(bitPattern: rd16(image + 2)) == 10 && Int16(bitPattern: rd16(image + 6)) == Int16(10 + expected))

            // Carets and offsets.
            let caret = svcPtr(16)
            #expect(svcCall("ATSUOffsetToPosition", layout, 2, 1, caret, caret, v) == 0)
            let x2 = Int32(bitPattern: rd32(caret)) >> 16
            #expect(x2 > 0 && x2 < Int32(expected))
            wr32(v, 0)
            #expect(svcCall("ATSUPositionToOffset", layout, UInt32(x2) << 16, 0, v, svcPtr(4), svcPtr(4)) == 0 && rd32(v) == 2)
            #expect(svcCall("ATSUNextCursorPosition", layout, 4, 0, v) == 0 && rd32(v) == 5)
            #expect(svcCall("ATSUPreviousCursorPosition", layout, 0, 0, v) == 0 && rd32(v) == 0)

            // Runs.
            let other = style(size: 12)
            #expect(svcCall("ATSUSetRunStyle", layout, other, 1, 2) == 0)
            let start = svcPtr(4), len = svcPtr(4)
            #expect(svcCall("ATSUGetRunStyle", layout, 2, v, start, len) == 0)
            #expect(rd32(v) == other && rd32(start) == 1 && rd32(len) == 2)
            #expect(svcCall("ATSUGetRunStyle", layout, 4, v, start, len) == 0 && rd32(v) == s && rd32(start) == 3)
            #expect(svcStatus(svcCall("ATSUSetRunStyle", layout, other, 4, 9)) == -8792)
            #expect(svcCall("ATSUDisposeTextLayout", layout) == 0)
        }

        @Test func linesBreakAtSpaces() {
            launchTestApp()
            _ = initToolbox()
            let s = style(size: 12)
            let (text, n) = svcUTF16("one two three four five six")
            let lens = svcPtr(4), styles = svcPtr(4), v = svcPtr(16)
            wr32(lens, n); wr32(styles, s)
            svcCall("ATSUCreateTextLayoutWithTextPtr", text, 0, n, n, 1, lens, styles, v)
            let layout = rd32(v)
            tb_TextFont(3)
            tb_TextSize(12)
            let width = Int(tb_TextWidth(svcPStr("one two thr") + 1, 0, 11))
            #expect(svcCall("ATSUBreakLine", layout, 0, UInt32(width) << 16, 1, v) == 0 && rd32(v) == 8) // after "two "
            let breaks = svcPtr(16), count = svcPtr(4)
            #expect(svcCall("ATSUGetSoftLineBreaks", layout, 0, n, 4, breaks, count) == 0)
            #expect(rd32(count) == 1 && rd32(breaks) == 8)
            // A word wider than the line breaks inside it.
            #expect(svcStatus(svcCall("ATSUBreakLine", layout, 8, 8 << 16, 0, v)) == -8808 && rd32(v) > 8)
            #expect(svcCall("ATSUClearSoftLineBreaks", layout, 0, n) == 0)
            #expect(svcCall("ATSUGetSoftLineBreaks", layout, 0, n, 4, breaks, count) == 0 && rd32(count) == 0)
            // Edits move the runs.
            #expect(svcCall("ATSUTextInserted", layout, 3, 2) == 0)
            #expect(svcCall("ATSUGetTextLocation", layout, v, svcPtr(4), svcPtr(4), count, svcPtr(4)) == 0 && rd32(count) == n + 2)
            #expect(svcCall("ATSUTextDeleted", layout, 0, 5) == 0)
            #expect(svcCall("ATSUGetRunStyle", layout, 0, v, breaks, count) == 0 && rd32(count) == n - 3)
        }

        @Test func fontsByNameAndNumber() {
            launchTestApp()
            _ = initToolbox()
            let v = svcPtr(256), len = svcPtr(4)
            #expect(svcCall("ATSUFontCount", v) == 0 && rd32(v) > 2)
            let name = svcPStr("Geneva")
            #expect(svcCall("ATSUFindFontFromName", name + 1, 6, 1, 0xFFFF_FFFF, 0xFFFF_FFFF, 0xFFFF_FFFF, v) == 0)
            let id = rd32(v)
            let fond = svcPtr(2), style = svcPtr(2)
            #expect(svcCall("ATSUFontIDtoFOND", id, fond, style) == 0 && rd16(fond) == 3 && rd8(style) == 0)
            #expect(svcCall("ATSUFindFontName", id, 1, 0xFFFF_FFFF, 0xFFFF_FFFF, 0xFFFF_FFFF, 64, v, len, svcPtr(4)) == 0)
            #expect(String(decoding: svcBytes(v, Int(rd32(len))), as: UTF8.self) == "Geneva")
            #expect(svcCall("ATSUFONDtoFontID", 3, 1, v) == 0) // bold
            let bold = rd32(v)
            #expect(svcCall("ATSUFindFontName", bold, 4, 0xFFFF_FFFF, 0xFFFF_FFFF, 0xFFFF_FFFF, 64, v, len, svcPtr(4)) == 0)
            #expect(String(decoding: svcBytes(v, Int(rd32(len))), as: UTF8.self) == "Geneva Bold")
            #expect(svcCall("ATSUCountFontFeatureTypes", id, v) == 0 && rd32(v) == 0)
            #expect(svcStatus(svcCall("ATSUFontIDtoFOND", 12345, fond, style)) == -8796)
        }
    }
}
