import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    @Suite struct QuickDrawExtras {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        /// A 32-bit GWorld to draw into, made current.
        func offscreen() -> UInt32 {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 40, 40), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            return rd32(gw)
        }

        func black(_ h: Int, _ v: Int) -> Bool {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return rd16(c) == 0 && rd16(c + 2) == 0
        }

        @Test func polygonsPaintTheirInside() {
            _ = offscreen()
            let poly = tb_OpenPoly()
            tb_MoveTo(5, 5)
            tb_LineTo(35, 5)
            tb_LineTo(5, 35)
            tb_LineTo(5, 5)
            tb_ClosePoly()
            #expect(rd16(rd32(poly)) == 10 + 4 * 4)
            #expect(rd16(rd32(poly) + 2) == 5 && rd16(rd32(poly) + 8) == 35) // bbox top, right
            tb_PaintPoly(poly)
            #expect(black(8, 8))          // inside the triangle
            #expect(!black(30, 30))       // beyond the diagonal
            tb_OffsetPoly(poly, 2, 3)
            #expect(rd16(rd32(poly) + 2) == 8)
            tb_KillPoly(poly)
        }

        @Test func rectProcBottleneckSeesCalls() {
            _ = offscreen()
            let port = tb_NewPtr(4, false, true)
            tb_GetPort(port)
            // A rectProc that counts its calls, then draws through StdRect.
            let count = tb_NewPtr(4, false, true)
            let stdRect = tb_GetTrapAddress(0xA8A0, true, true)
            let words: [UInt16] = [
                0x52B9, UInt16(count >> 16), UInt16(count & 0xFFFF), // ADDQ.L #1,count
                0x4EF9, UInt16(stdRect >> 16), UInt16(stdRect & 0xFFFF), // JMP StdRect
            ]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let procs = tb_NewPtr(Int32(CQDProcs_SIZEOF), false, true)
            tb_SetStdCProcs(procs)
            wr32(procs + UInt32(QDProcs_rectProc_off), code)
            GrafPort_set_grafProcs(rd32(port), procs)
            tb_PaintRect(rect(10, 10, 20, 20))
            #expect(rd32(count) == 1)
            #expect(black(15, 15))
            GrafPort_set_grafProcs(rd32(port), 0)
        }

        @Test func seedFillAndColourMask() {
            let gw = offscreen()
            tb_FrameRect(rect(5, 5, 15, 15))
            let src = gw + 2 // the port's bits, as CopyBits takes them
            // A 1-bit 32x20 mask bitmap.
            let bits = tb_NewPtr(4 * 20, false, true), bm = tb_NewPtr(14, false, true)
            wr32(bm, bits); wr16(bm + 4, 4)
            for (i, v) in [0, 0, 20, 32].enumerated() { wr16(bm + 6 + UInt32(i * 2), UInt16(v)) }
            func bit(_ x: Int, _ y: Int) -> Bool { rd8(bits + UInt32(y * 4 + x / 8)) & UInt8(0x80 >> (x % 8)) != 0 }
            let r = rect(0, 0, 20, 20)
            tb_SeedCFill(src, bm, r, r, 10, 10, 0, 0)
            #expect(bit(10, 10) && !bit(2, 2) && !bit(5, 5))
            for i in 0..<80 { wr8(bits + UInt32(i), 0) }
            let black = tb_NewPtr(6, false, true)
            tb_CalcCMask(src, bm, r, r, black, 0, 0)
            #expect(bit(10, 10) && bit(5, 5) && !bit(2, 2))
        }

        @Test func bitMapToRegion() {
            launchTestApp()
            _ = initToolbox()
            // 16x4 bitmap: rows 1-2 have bits 4..7 set.
            let bits = tb_NewPtr(8, false, true)
            wr8(bits + 2, 0x0F); wr8(bits + 4, 0x0F)
            let bm = tb_NewPtr(14, false, true)
            wr32(bm, bits); wr16(bm + 4, 2)
            for (i, v) in [0, 0, 4, 16].enumerated() { wr16(bm + 6 + UInt32(i * 2), UInt16(v)) }
            let rgn = tb_NewRgn()
            #expect(tb_BitMapToRegion(rgn, bm) == 0)
            let box = rd32(rgn) + 2
            #expect(rd16(box) == 1 && rd16(box + 2) == 4 && rd16(box + 4) == 3 && rd16(box + 6) == 8)
        }
    }

    @Suite struct International {
        @Test func datesRoundTripAndFormat() {
            launchTestApp()
            let d = tb_NewPtr(14, false, true)
            // 2 January 1996, 13:05:07, a Tuesday.
            for (i, v) in [1996, 1, 2, 13, 5, 7, 0].enumerated() { wr16(d + UInt32(i * 2), UInt16(v)) }
            var secs: UInt32 = 0
            tb_DateToSeconds(d, &secs)
            let back = tb_NewPtr(14, false, true)
            tb_SecondsToDate(secs, back)
            #expect(rd16(back) == 1996 && rd16(back + 4) == 2 && rd16(back + 12) == 3)
            let s = tb_NewPtr(256, false, true)
            tb_IUDateString(Int32(bitPattern: secs), 0, s)
            #expect(pstring(at: s) == "1/2/96")
            tb_IUDateString(Int32(bitPattern: secs), 1, s)
            #expect(pstring(at: s) == "Tuesday, January 2, 1996")
            tb_IUDateString(Int32(bitPattern: secs), 2, s)
            #expect(pstring(at: s) == "Tue, Jan 2, 1996")
            tb_IUTimeString(Int32(bitPattern: secs), 1, s)
            #expect(pstring(at: s) == "1:05:07 PM")
            #expect(tb_GetIntlResource(1) != 0)
        }

        @Test func ordering() {
            launchTestApp()
            func cmp(_ a: String, _ b: String) -> Int16 {
                let pa = tb_NewPtr(64, false, true), pb = tb_NewPtr(64, false, true)
                for (i, c) in a.utf8.enumerated() { wr8(pa + UInt32(i), c) }
                for (i, c) in b.utf8.enumerated() { wr8(pb + UInt32(i), c) }
                return tb_IUMagString(pa, pb, Int16(a.utf8.count), Int16(b.utf8.count))
            }
            #expect(cmp("apple", "Banana") == -1)
            #expect(cmp("Zoo", "apple") == 1)
            #expect(cmp("same", "same") == 0)
            #expect(tb_KeyTranslate(0, 0x0200 | 0x00, 0) == UInt32(UInt8(ascii: "A")))
            #expect(tb_KeyTranslate(0, 0x0012, 0) == UInt32(UInt8(ascii: "1")))
        }
    
    @Suite struct ColorPicker {
        @Test func colourModelsRoundTrip() {
            launchTestApp()
            let rgb = tb_NewPtr(6, false, true), hsv = tb_NewPtr(6, false, true), back = tb_NewPtr(6, false, true)
            wr16(rgb, 0xFFFF); wr16(rgb + 2, 0x8000); wr16(rgb + 4, 0)
            tb_RGB2HSV(rgb, hsv)
            #expect(rd16(hsv + 4) == 0xFFFF) // value: the brightest component
            tb_HSV2RGB(hsv, back)
            #expect(abs(Int(rd16(back + 2)) - 0x8000) < 4)
            tb_RGB2HSL(rgb, hsv)
            tb_HSL2RGB(hsv, back)
            #expect(abs(Int(rd16(back + 2)) - 0x8000) < 4 && rd16(back) > 0xFFF0)
            tb_RGB2CMY(rgb, back)
            #expect(rd16(back) == 0 && rd16(back + 4) == 0xFFFF)
            // Headless: GetColor is cancelled and gives the colour back unchanged.
            #expect(tb_GetColor(0, 0, rgb, back) == 0 && rd16(back + 2) == 0x8000)
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (k, v) in [50, 50, 150, 250].enumerated() { wr16(r + UInt32(k * 2), UInt16(v)) }
            let win = tb_NewWindow(0, r, 0, 0, 4, 0xFFFF_FFFF, 0, 0)
            #expect(tb_GetWVariant(win) == 4)
        }
    }
}
}
