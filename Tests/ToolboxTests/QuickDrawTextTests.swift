import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Text layout: CharExtra's widening, and outline fonts' metrics.
    @Suite struct QuickDrawText {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func string(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// A 32-bit GWorld (a colour port) to draw into, made current.
        func offscreen() {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 40, 80), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            tb_TextFont(3)
            tb_TextSize(9)
        }

        func penH() -> Int {
            let p = tb_NewPtr(4, false, true)
            tb_GetPen(p)
            return Int(Int16(bitPattern: rd16(p + 2)))
        }

        /// The black pixels after drawing, as v * 80 + h.
        func inked() -> Set<Int> {
            let c = tb_NewPtr(6, false, true)
            var on = Set<Int>()
            for v in 0..<40 {
                for h in 0..<80 {
                    tb_GetCPixel(Int16(h), Int16(v), c)
                    if rd16(c) == 0 && rd16(c + 2) == 0 { on.insert(v * 80 + h) }
                }
            }
            return on
        }

        /// CharExtra widens every character but the space, in measuring and
        /// drawing alike: the pen ends where StringWidth says it will.
        @Test func charExtraWidensNonspaceCharacters() {
            offscreen()
            let ab = string("AB"), space = string(" "), a = string("A")
            let plain = Int(tb_StringWidth(ab)), spaceWidth = Int(tb_StringWidth(space))
            tb_CharExtra(2 << 16)
            #expect(Int(tb_StringWidth(ab)) == plain + 4)
            #expect(Int(tb_StringWidth(space)) == spaceWidth)
            #expect(Int(tb_TextWidth(ab + 1, 0, 2)) == plain + 4)
            #expect(Int(tb_CharWidth(0x41)) == Int(tb_StringWidth(a)))
            tb_MoveTo(10, 20)
            tb_DrawString(ab)
            #expect(penH() == 10 + plain + 4)

            // CharToPixel and MeasureText step by the same widths.
            #expect(Int(tb_CharToPixel(ab + 1, 2, 0, 2, 1, 0, 0x00010001, 0x00010001)) == plain + 4)
            let locs = tb_NewPtr(6, false, true)
            tb_MeasureText(2, ab + 1, locs)
            #expect(Int(rd16(locs + 4)) == plain + 4)
        }

        /// Each character after the first is drawn the extra further on: the
        /// second glyph lands where drawing it alone, that far on, puts it.
        @Test func charExtraMovesTheGlyphs() {
            offscreen()
            tb_CharExtra(3 << 16)
            let aWidth = Int(tb_StringWidth(string("A")))
            tb_EraseRect(rect(0, 0, 40, 80))
            tb_MoveTo(10, 20)
            tb_DrawString(string("AB"))
            let together = inked()
            tb_EraseRect(rect(0, 0, 40, 80))
            tb_MoveTo(10, 20)
            tb_DrawString(string("A"))
            tb_MoveTo(Int16(10 + aWidth), 20)
            tb_DrawString(string("B"))
            #expect(!together.isEmpty && inked() == together)
        }

        /// The extra is fractional and kept per point of the size: half a
        /// pixel adds up over the characters, and a size set afterwards
        /// scales it.
        @Test func charExtraIsFractionalAndScales() {
            offscreen()
            let four = string("AAAA")
            let plain = Int(tb_StringWidth(four))
            tb_CharExtra(0x8000)
            #expect(Int(tb_StringWidth(four)) == plain + 2)
            tb_CharExtra(1 << 16)
            #expect(Int(tb_StringWidth(four)) == plain + 4)
            tb_TextSize(18)
            let wider = Int(tb_StringWidth(four))
            tb_CharExtra(0)
            #expect(wider == Int(tb_StringWidth(four)) + 8)
        }

        /// A basic port has no chExtra: CharExtra changes nothing there.
        @Test func charExtraIgnoresBasicPorts() {
            launchTestApp()
            _ = initToolbox()
            tb_TextFont(3)
            tb_TextSize(9)
            let ab = string("AB")
            let plain = tb_StringWidth(ab)
            tb_CharExtra(2 << 16)
            #expect(tb_StringWidth(ab) == plain)
        }

        // ---- Outline fonts ----

        /// A tiny TrueType font made here: .notdef, the space (no outline,
        /// advance 250) and "A", a triangle 50...550 by 0...700 with advance
        /// 600 and left side bearing 50, at 1000 units per em.
        func testSfnt() -> [UInt8] {
            let glyphA = cat(be16(1), be16(50), be16(0), be16(550), be16(700), // contours, bbox
                             be16(2), be16(0), [1, 1, 1],                        // end point, no instructions, flags
                             be16(50), be16(250), be16(250),                     // x deltas
                             be16(0), be16(700), be16(Int(UInt16(bitPattern: -700))), [0])
            var map = [UInt8](repeating: 0, count: 256)
            map[0x20] = 1
            map[0x41] = 2
            let tables: [(String, [UInt8])] = [
                ("cmap", cat(be16(0), be16(1), be16(1), be16(0), be32(12), be16(0), be16(262), be16(0), map)),
                ("glyf", glyphA),
                ("head", cat(be32(0x0001_0000), be32(0x0001_0000), be32(0), be32(0x5F0F_3CF5), be16(0), be16(1000),
                             [UInt8](repeating: 0, count: 16), be16(50), be16(0), be16(550), be16(700),
                             be16(0), be16(8), be16(2), be16(0), be16(0))),
                ("hhea", cat(be32(0x0001_0000), be16(800), be16(Int(UInt16(bitPattern: -200))), be16(0),
                             be16(600), [UInt8](repeating: 0, count: 22), be16(3))),
                ("hmtx", cat(be16(500), be16(0), be16(250), be16(0), be16(600), be16(50))),
                ("loca", cat(be16(0), be16(0), be16(0), be16(glyphA.count / 2))),
                ("maxp", cat(be32(0x0000_5000), be16(3))),
            ]
            var dir = cat(be32(0x0001_0000), be16(tables.count), be16(64), be16(2), be16(48))
            var body = [UInt8]()
            let start = 12 + tables.count * 16
            for (tag, data) in tables {
                dir += cat(Array(tag.utf8), be32(0), be32(UInt32(start + body.count)), be32(UInt32(data.count)))
                body += data + [UInt8](repeating: 0, count: (4 - data.count % 4) % 4)
            }
            return dir + body
        }

        /// A family (300) with the outline, and a bitmap entry at 10 points.
        func launchWithOutline() {
            let fond = cat([UInt8](repeating: 0, count: Int(FamRec_SIZEOF)),
                           be16(1), be16(0), be16(0), be16(300), be16(10), be16(0), be16(301))
            launchTestApp([TestResource(type: "FOND", id: 300, name: "Outline Test", data: fond),
                           TestResource(type: "sfnt", id: 300, data: testSfnt())])
            _ = initToolbox()
            tb_SetOutlinePreferred(0)
            tb_SetFractEnable(0)
            tb_TextFont(300)
        }

        /// IsOutline follows the Font Manager's choice: the outline unless a
        /// strike has the exact size and outlines aren't preferred; the
        /// built-in families are bitmaps only.
        @Test func isOutlineFollowsTheFontsChoice() {
            launchWithOutline()
            let one: UInt32 = 0x0001_0001
            tb_TextSize(12)
            #expect(tb_IsOutline(one, one) != 0)
            tb_TextSize(10)
            #expect(tb_IsOutline(one, one) == 0)
            #expect(tb_IsOutline(0x0002_0001, one) != 0) // scaled to 20 points vertically
            tb_SetOutlinePreferred(1)
            #expect(tb_IsOutline(one, one) != 0)
            tb_SetOutlinePreferred(0)
            tb_TextFont(3)
            #expect(tb_IsOutline(one, one) == 0)
            #expect(tb_OutlineMetrics(1, string("A") + 1, one, one, 0, 0, 0, 0, 0) == -32615) // fontNotOutlineErr
        }

        /// OutlineMetrics measures each glyph from the font's tables, scaled
        /// to the size: advances whole unless FractEnable, bearings exact,
        /// boxes about the origin with y up.
        @Test func outlineMetricsFromTheTables() {
            launchWithOutline()
            tb_TextSize(20)
            let one: UInt32 = 0x0001_0001
            let text = string("A ") + 1
            let yMax = tb_NewPtr(2, false, true), yMin = tb_NewPtr(2, false, true)
            let aw = tb_NewPtr(8, false, true), lsb = tb_NewPtr(8, false, true), bounds = tb_NewPtr(16, false, true)
            #expect(tb_OutlineMetrics(2, text, one, one, yMax, yMin, aw, lsb, bounds) == 0)
            #expect(rd32(aw) == 12 << 16 && rd32(aw + 4) == 5 << 16)
            #expect(rd32(lsb) == 1 << 16 && rd32(lsb + 4) == 0)
            #expect([rd16(bounds), rd16(bounds + 2), rd16(bounds + 4), rd16(bounds + 6)] == [14, 1, 0, 11])
            #expect(rd32(bounds + 8) == 0 && rd32(bounds + 12) == 0) // the space has no outline
            #expect(rd16(yMax) == 14 && rd16(yMin) == 0)

            // At 10 points (drawn from the outline when preferred) the space's
            // 2.5 pixels round, or stay fractional with FractEnable.
            tb_TextSize(10)
            tb_SetOutlinePreferred(1)
            #expect(tb_OutlineMetrics(2, text, one, one, 0, 0, aw, lsb, 0) == 0)
            #expect(rd32(aw + 4) == 3 << 16 && rd32(lsb) == 0x8000)
            tb_SetFractEnable(1)
            #expect(tb_OutlineMetrics(2, text, one, one, 0, 0, aw, 0, 0) == 0)
            #expect(rd32(aw) == 6 << 16 && rd32(aw + 4) == 0x0002_8000)

            // Vertical scaling stretches the boxes, horizontal the advances.
            #expect(tb_OutlineMetrics(1, text, 0x0002_0003, 0x0001_0001, yMax, 0, aw, 0, bounds) == 0)
            #expect(rd16(yMax) == 14 && rd16(bounds) == 14 && rd32(aw) == 18 << 16)
            tb_SetFractEnable(0)
            tb_SetOutlinePreferred(0)
        }
    }
}
