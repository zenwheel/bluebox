import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    @Suite struct ColorQuickDraw {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(v)) }
            return p
        }

        func rgb(_ r: UInt16, _ g: UInt16, _ b: UInt16) -> UInt32 {
            let p = tb_NewPtr(6, false, true)
            wr16(p, r); wr16(p + 2, g); wr16(p + 4, b)
            return p
        }

        @Test func gworldDrawsIntoItsOwnPixels() {
            launchTestApp()
            _ = initToolbox()
            let screen = tb_NewPtr(4, false, true), dev = tb_NewPtr(4, false, true)
            tb_GetGWorld(screen, dev)
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 20, 30), 0, 0, 0) == 0)
            let port = rd32(gw)
            #expect(port != 0)
            tb_SetGWorld(port, 0)
            #expect(tb_LockPixels(tb_GetGWorldPixMap(port)) != 0)
            #expect(tb_GetPixBaseAddr(tb_GetGWorldPixMap(port)) != 0)
            let red = rgb(0xFFFF, 0, 0)
            tb_SetCPixel(5, 5, red)
            let back = rgb(0, 0, 0)
            tb_GetCPixel(5, 5, back)
            #expect(rd16(back) == 0xFFFF && rd16(back + 2) == 0 && rd16(back + 4) == 0)
            tb_GetCPixel(6, 6, back)
            #expect(rd16(back + 2) == 0xFFFF) // still white
            tb_SetGWorld(rd32(screen), rd32(dev))
            tb_DisposeGWorld(port)
        }

        /// A picture recorded in colour plays back in colour (RGBFgCol).
        @Test func picturesKeepTheirColours() {
            launchTestApp()
            _ = initToolbox()
            let screen = tb_NewPtr(4, false, true), dev = tb_NewPtr(4, false, true)
            tb_GetGWorld(screen, dev)
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 20, 20), 0, 0, 0) == 0)
            let port = rd32(gw)
            tb_SetGWorld(port, 0)
            let pic = tb_OpenPicture(rect(0, 0, 20, 20))
            tb_RGBForeColor(rgb(0xFFFF, 0, 0))
            tb_PaintRect(rect(2, 2, 10, 10))
            tb_ClosePicture()
            tb_RGBForeColor(rgb(0, 0, 0))
            tb_EraseRect(rect(0, 0, 20, 20))
            tb_DrawPicture(pic, rect(0, 0, 20, 20))
            let back = rgb(0, 0, 0)
            tb_GetCPixel(5, 5, back)
            #expect(rd16(back) == 0xFFFF && rd16(back + 2) == 0 && rd16(back + 4) == 0)
            tb_SetGWorld(rd32(screen), rd32(dev))
            tb_DisposeGWorld(port)
        }

        /// An 8-bit GWorld brings its own device: while it is current,
        /// Color2Index gives indices into its table, not 32-bit pixels.
        @Test func gworldHasItsOwnDevice() {
            launchTestApp()
            _ = initToolbox()
            let screen = tb_NewPtr(4, false, true), dev = tb_NewPtr(4, false, true)
            tb_GetGWorld(screen, dev)
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 8, rect(0, 0, 10, 10), 0, 0, 0) == 0)
            let port = rd32(gw)
            tb_SetGWorld(port, 0)
            let gdh = tb_GetGDevice()
            #expect(gdh != rd32(dev) && gdh == tb_GetGWorldDevice(port))
            #expect(PixMap_pixelSize(rd32(GDevice_gdPMap(rd32(gdh)))) == 8)
            #expect(tb_Color2Index(rgb(0xFFFF, 0, 0)) == 35)
            #expect(rd16(port + UInt32(CGrafPort_portVersion_off)) == 0xC001) // a GWorld says so
            // Its inverse table: red cut to 4 bits a component is cell $F00.
            let itab = rd32(rd32(gdh) + UInt32(GDevice_gdITable_off))
            #expect(itab != 0 && rd16(rd32(itab) + 4) == 4)
            #expect(rd8(rd32(itab) + 6 + 0xF00) == 35)
            tb_SetGWorld(rd32(screen), rd32(dev))
            #expect(tb_GetGDevice() == rd32(dev))
            tb_DisposeGWorld(port)
        }

        @Test func colorTablesAndIndices() {
            launchTestApp()
            _ = initToolbox()
            let ct = tb_GetCTable(8)
            #expect(ct != 0 && rd16(rd32(ct) + 6) == 255)
            let black = rgb(0, 0, 0)
            let i = tb_Color2Index(black)
            let c = rgb(1, 1, 1)
            tb_Index2Color(i, c)
            #expect(rd16(c) == 0 && rd16(c + 2) == 0 && rd16(c + 4) == 0)
            tb_InvertColor(c)
            #expect(rd16(c) == 0xFFFF)
            tb_DisposeCTable(ct)
        }

        /// Black is the table's last entry even while a fade has made every
        /// entry black, so it is still black when the colours come back.
        @Test func blackIsTheLastEntryDuringAFade() {
            launchTestApp()
            _ = initToolbox()
            let ct = tb_GetCTable(8)
            for i in 0..<256 { for k in 0..<3 { wr16(rd32(ct) + 8 + UInt32(i * 8 + 2 + k * 2), 0) } }
            let screen = tb_NewPtr(4, false, true), dev = tb_NewPtr(4, false, true)
            tb_GetGWorld(screen, dev)
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 8, rect(0, 0, 4, 4), ct, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            tb_SetCPixel(1, 1, rgb(0, 0, 0))
            let base = tb_GetPixBaseAddr(tb_GetGWorldPixMap(rd32(gw)))
            let rowBytes = UInt32(rd16(rd32(tb_GetGWorldPixMap(rd32(gw))) + 4) & 0x3FFF)
            #expect(rd8(base + rowBytes + 1) == 255)
            tb_SetGWorld(rd32(screen), rd32(dev))
            tb_DisposeGWorld(rd32(gw))
        }

        /// The host sees the cursor's level, image and shield rectangle.
        @Test func cursorStateReachesTheHost() {
            launchTestApp()
            _ = initToolbox()
            var c = machine_cursor()
            tb_InitCursor()
            _ = machine_cursor_state(&c)
            #expect(!c.hidden && c.arrow)
            #expect(rd8(0x8CC) != 0) // CrsrVis
            tb_HideCursor()
            _ = machine_cursor_state(&c)
            #expect(c.hidden && Int16(bitPattern: rd16(0x8D0)) == -1) // CrsrState
            tb_ShowCursor()
            _ = machine_cursor_state(&c)
            #expect(!c.hidden)
            let crsr = tb_NewPtr(68, false, true)
            for i in 0..<16 { wr16(crsr + UInt32(i * 2), 0x0FF0); wr16(crsr + 32 + UInt32(i * 2), 0x1FF8) }
            wr16(crsr + 64, 8); wr16(crsr + 66, 8)
            tb_SetCursor(crsr)
            _ = machine_cursor_state(&c)
            #expect(!c.arrow && c.hot_h == 8 && c.data.0 == 0x0F)
            #expect(c.kind == Int32(MACHINE_CURSOR_CUSTOM))
            // The System's I-beam (CURS 1) is known, for the host's own.
            tb_SetCursor(rd32(tb_GetCursor(1)))
            _ = machine_cursor_state(&c)
            #expect(c.kind == Int32(MACHINE_CURSOR_IBEAM))
            tb_SetCursor(crsr)
            // Shielded: hidden only while the mouse is in the rectangle.
            tb_ShieldCursor(rect(100, 100, 200, 200), 0)
            machine_post_mouse(150, 150, -1)
            _ = machine_cursor_state(&c)
            #expect(c.hidden)
            machine_post_mouse(20, 20, -1)
            _ = machine_cursor_state(&c)
            #expect(!c.hidden)
            tb_ShowCursor()
            machine_post_mouse(150, 150, -1)
            _ = machine_cursor_state(&c)
            #expect(!c.hidden)
        }

        @Test func palettesKeepTheirColours() {
            launchTestApp()
            let w = initToolbox()
            let pal = tb_NewPalette(4, 0, 2, 0)
            #expect(pal != 0)
            tb_SetEntryColor(pal, 2, rgb(0x1234, 0x5678, 0x9ABC))
            let c = rgb(0, 0, 0)
            tb_GetEntryColor(pal, 2, c)
            #expect(rd16(c) == 0x1234 && rd16(c + 4) == 0x9ABC)
            tb_SetPalette(w, pal, 1)
            #expect(tb_GetPalette(w) == pal)
            tb_ResizePalette(pal, 8)
            tb_GetEntryColor(pal, 2, c)
            #expect(rd16(c + 2) == 0x5678)
            tb_DisposePalette(pal)
            #expect(tb_GetPalette(w) == 0)
        }

        /// CalcMask: the outline and what it encloses.  SeedFill: the white
        /// area around the seed.  A source that is nearly all white floods
        /// every pixel (Slabbers' sprites overran the fill's stack).
        @Test func calcMaskAndSeedFill() {
            launchTestApp()
            _ = initToolbox()
            let rows = 12, rb: Int16 = 2
            let src = tb_NewPtr(Int32(rows * 2), false, true)
            let dst = tb_NewPtr(Int32(rows * 2), false, true)
            // A ring: rows 2 and 9 from bit 2 to bit 9, columns 2 and 9 between.
            for y in 2...9 {
                let v: UInt16 = (y == 2 || y == 9) ? 0x3FC0 : 0x2040
                wr16(src + UInt32(y * 2), v)
            }
            tb_CalcMask(src, dst, rb, rb, Int16(rows), 1)
            #expect(rd16(dst) == 0)                  // outside the ring
            #expect(rd16(dst + 5 * 2) == 0x3FC0)     // the ring and its inside
            #expect(rd16(dst + 2 * 2) == 0x3FC0)
            tb_SeedFill(src, dst, rb, rb, Int16(rows), 1, 5, 5)
            #expect(rd16(dst + 5 * 2) == 0x1F80)     // inside the ring only
            #expect(rd16(dst) == 0)
            // All white: nothing in the mask, everything filled.
            gmemset(src, 0, UInt32(rows * 2))
            tb_CalcMask(src, dst, rb, rb, Int16(rows), 1)
            #expect((0..<rows).allSatisfy { rd16(dst + UInt32($0 * 2)) == 0 })
            tb_SeedFill(src, dst, rb, rb, Int16(rows), 1, 0, 0)
            #expect((0..<rows).allSatisfy { rd16(dst + UInt32($0 * 2)) == 0xFFFF })
        }

        @Test func auxiliaryControlRecord() {
            launchTestApp()
            _ = initToolbox()
            let h = tb_NewPtr(4, false, true)
            #expect(tb_GetAuxiliaryControlRecord(0x1000, h) != 0)
            #expect(rd32(h) != 0 && rd32(rd32(rd32(h)) + 4) == 0x1000)
        }
    }
}
