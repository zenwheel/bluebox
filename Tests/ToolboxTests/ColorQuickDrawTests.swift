import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    @Suite struct ColorQuickDraw {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(truncatingIfNeeded: v)) }
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

        /// BackPixPat and PenPixPat put the program's own pattern in the port:
        /// a program that reads it back and disposes of it before setting the
        /// next (Chromosomethingorother) leaves the heap whole, BackPat goes
        /// back to the port's own pattern, and CloseCPort disposes only that.
        @Test func pixPatsGoInThePortAsTheyAre() {
            launchTestApp()
            _ = initToolbox()
            let zone = rd32(UInt32(LM_ApplZone))
            let saved = tb_NewPtr(4, false, true)
            tb_GetPort(saved)
            let port = tb_NewPtr(108, true, true)
            tb_OpenCPort(port)
            let bkOff: UInt32 = 32, pnOff: UInt32 = 58
            let own = rd32(port + bkOff)
            let first = tb_NewPixPat()
            tb_BackPixPat(first)
            #expect(rd32(port + bkOff) == first)
            tb_DisposePixPat(rd32(port + bkOff))
            let second = tb_NewPixPat()
            tb_BackPixPat(second)
            #expect(rd32(port + bkOff) == second)
            #expect(mm_check_zone(zone))
            let white = tb_NewPtr(8, true, true)
            for i in 0..<8 { wr8(white + UInt32(i), 0) }
            tb_BackPat(white)
            #expect(rd32(port + bkOff) == own)
            #expect(rd16(rd32(second)) == 1) // still the program's pixel pattern
            let pen = tb_NewPixPat()
            tb_PenPixPat(pen)
            #expect(rd32(port + pnOff) == pen)
            tb_PenNormal()
            #expect(rd32(port + pnOff) != pen)
            tb_SetPort(rd32(saved))
            tb_CloseCPort(port)
            #expect(rd32(second) != 0 && rd32(pen) != 0) // not the port's to dispose
            tb_DisposePixPat(second)
            tb_DisposePixPat(pen)
            #expect(mm_check_zone(zone))
        }

        /// At depth 0 the rectangle is global, only to choose the screen: the
        /// world starts at 0,0 (Squark draws at 0,0 into a world made from
        /// its window's global rectangle).  A given depth keeps the rectangle.
        @Test func screenDepthWorldsStartAtTheOrigin() {
            launchTestApp()
            _ = initToolbox()
            let gw = tb_NewPtr(4, false, true)
            func rects(_ port: UInt32) -> [Int16] {
                let pm = rd32(tb_GetGWorldPixMap(port))
                return (0..<4).map { Int16(bitPattern: rd16(port + 16 + UInt32($0 * 2))) }
                    + (0..<4).map { Int16(bitPattern: rd16(pm + 6 + UInt32($0 * 2))) }
            }
            #expect(tb_NewGWorld(gw, 0, rect(144, 192, 624, 832), 0, 0, 2) == 0)
            #expect(rects(rd32(gw)) == [0, 0, 480, 640, 0, 0, 480, 640])
            tb_DisposeGWorld(rd32(gw))
            #expect(tb_NewGWorld(gw, 32, rect(144, 192, 624, 832), 0, 0, 0) == 0)
            #expect(rects(rd32(gw)) == [144, 192, 624, 832, 144, 192, 624, 832])
            tb_DisposeGWorld(rd32(gw))
            // A left edge off a long word of screen pixels keeps its alignment.
            #expect(tb_NewGWorld(gw, 0, rect(-50, -60, 50, 140), 0, 0, 0) == 0)
            let depth = Int(rd16(rd32(tb_GetGWorldPixMap(rd32(gw))) + 32))
            let shift = Int16(depth < 32 ? ((-60 % (32 / depth)) + 32 / depth) % (32 / depth) : 0)
            #expect(rects(rd32(gw)) == [0, 0, 100, 200, 0, -shift, 100, 200])
            tb_DisposeGWorld(rd32(gw))
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

        /// Rows have a long word of room for realigning the image, and a
        /// row more follows the last: an 8-bit world 32 pixels wide steps
        /// by 36 (Space Pebbles), and a scroller's tile run off the last
        /// row's end lands in the world's own memory (Slippy).
        @Test func gworldRowsHaveRoomToSpare() {
            launchTestApp()
            _ = initToolbox()
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 8, rect(0, 0, 32, 32), 0, 0, 0) == 0)
            let pm = rd32(tb_GetGWorldPixMap(rd32(gw)))
            #expect(rd16(pm + 4) & 0x3FFF == 36)
            #expect(tb_GetHandleSize(PixMap_baseAddr(pm)) >= 36 * 33)
            tb_DisposeGWorld(rd32(gw))
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 10, 10), 0, 0, 0) == 0)
            #expect(rd16(rd32(tb_GetGWorldPixMap(rd32(gw))) + 4) & 0x3FFF == 40)
            tb_DisposeGWorld(rd32(gw))
        }

        /// A GWorld's pixels are a handle, held in baseAddr until LockPixels
        /// puts their address there (pmVersion 2, then 1), as QuickDraw's
        /// were.  HunterDeer moves its pixels into a handle of its own by it;
        /// drawing then stays inside them.
        @Test func gworldPixelsAreAHandleUntilLocked() {
            launchTestApp()
            _ = initToolbox()
            let zone = rd32(UInt32(LM_ApplZone))
            let screen = tb_NewPtr(4, false, true), dev = tb_NewPtr(4, false, true)
            tb_GetGWorld(screen, dev)
            let gw = tb_NewPtr(4, false, true)
            let bounds = rect(0, 0, 40, 60)
            #expect(tb_NewGWorld(gw, 8, bounds, 0, 0, 0) == 0)
            let port = rd32(gw), pmh = tb_GetGWorldPixMap(port), pm = rd32(pmh)
            let h = PixMap_baseAddr(pm)
            #expect(rd16(pm + 14) == 2)
            let size = tb_GetHandleSize(h)
            #expect(size >= 40 * Int32(rd16(pm + 4) & 0x3FFF))
            #expect(tb_GetPixBaseAddr(pmh) == rd32(h))
            #expect(tb_LockPixels(pmh) != 0)
            #expect(PixMap_baseAddr(pm) == rd32(h) && rd16(pm + 14) == 1)
            #expect(tb_GetPixelsState(pmh) & 0x80 != 0)
            tb_UnlockPixels(pmh)
            #expect(PixMap_baseAddr(pm) == h && rd16(pm + 14) == 2)
            #expect(tb_GetPixelsState(pmh) & 0x80 == 0)
            // The pixels moved into a handle of the program's own.
            tb_DisposeHandle(h)
            let mine = tb_NewHandle(size, false, false)
            wr32(pm, mine)
            #expect(tb_LockPixels(pmh) != 0)
            #expect(PixMap_baseAddr(pm) == rd32(mine))
            tb_SetGWorld(port, 0)
            tb_EraseRect(bounds)
            #expect(rd8(rd32(mine)) == 0) // white, at 8 bits
            #expect(mm_check_zone(zone))
            tb_SetGWorld(rd32(screen), rd32(dev))
            tb_DisposeGWorld(port)
            #expect(mm_check_zone(zone))
        }

        /// A direct screen's device table has 256 entries all the same, for
        /// 256-colour games fading through it with SetEntries (Squish).
        @Test func directScreenTableHas256Entries() {
            launchTestApp()
            _ = initToolbox()
            let pm = rd32(GDevice_gdPMap(rd32(rd32(UInt32(LM_MainDevice)))))
            guard PixMap_pixelSize(pm) > 8 else { return }
            let ct = PixMap_pmTable(pm)
            #expect(rd16(rd32(ct) + 6) == 255)
            #expect(tb_GetHandleSize(ct) == 8 + 256 * 8)
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

        /// SaveEntries and RestoreEntries take their selection as a ReqListRec
        /// (a pointer: reqLSize, the count less one, then entry numbers).
        @Test func saveAndRestoreEntriesTakeARequestList() {
            launchTestApp()
            _ = initToolbox()
            let src = tb_GetCTable(8), dst = tb_GetCTable(8)
            let entry = { (ct: UInt32, i: Int) -> UInt32 in rd32(ct) + 8 + UInt32(i * 8) }
            wr16(entry(src, 5) + 2, 0x1234)
            wr16(entry(src, 9) + 2, 0x5678)
            let sel = tb_NewPtr(6, false, true)
            wr16(sel, 1); wr16(sel + 2, 5); wr16(sel + 4, 9)
            let saved = tb_NewHandle(0, false, true)
            tb_SaveEntries(src, saved, sel)
            #expect(tb_GetHandleSize(saved) == 8 + 2 * 8)
            #expect(rd16(rd32(saved) + 6) == 1)
            #expect(rd16(entry(saved, 0)) == 5 && rd16(entry(saved, 0) + 2) == 0x1234)
            #expect(rd16(entry(saved, 1)) == 9 && rd16(entry(saved, 1) + 2) == 0x5678)
            // Back into other entries of another table.
            wr16(sel + 2, 1); wr16(sel + 4, 2)
            tb_RestoreEntries(saved, dst, sel)
            #expect(rd16(entry(dst, 1) + 2) == 0x1234 && rd16(entry(dst, 2) + 2) == 0x5678)
            tb_DisposeHandle(saved)
            tb_DisposeCTable(src)
            tb_DisposeCTable(dst)
        }

        /// Tables 66, 68 and 72 (the standard tables with the highlight
        /// colour) exist too: the Star Trek demo writes through table 72's
        /// handle unchecked.
        @Test func highlightColorTablesExist() {
            launchTestApp()
            _ = initToolbox()
            for (id, entries) in [(66, 3), (68, 15), (72, 255)] {
                let ct = tb_GetCTable(Int16(id))
                #expect(ct != 0 && rd32(ct) != 0 && rd16(rd32(ct) + 6) == UInt16(entries))
                tb_DisposeCTable(ct)
            }
            #expect(tb_GetCTable(73) == 0)
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

        /// Pen modes 40..47 are the arithmetic modes with the pattern bit, so
        /// 44 is transparent: the pattern's 0 bits leave the pixels alone, and
        /// so do its 1 bits when the foreground is the background colour (The
        /// Journeyman Project goes over its buttons white on white).
        @Test func transparentPenMode() {
            launchTestApp()
            _ = initToolbox()
            let screen = tb_NewPtr(4, false, true), dev = tb_NewPtr(4, false, true)
            tb_GetGWorld(screen, dev)
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 16, 16), 0, 0, 0) == 0)
            let port = rd32(gw)
            tb_SetGWorld(port, 0)
            _ = tb_LockPixels(tb_GetGWorldPixMap(port))
            let all = rect(0, 0, 16, 16)
            func pixel(_ h: Int16, _ v: Int16) -> [UInt16] {
                let c = rgb(0, 0, 0)
                tb_GetCPixel(h, v, c)
                return [rd16(c), rd16(c + 2), rd16(c + 4)]
            }
            tb_RGBForeColor(rgb(0xFFFF, 0, 0))
            tb_PaintRect(all)
            let pat = tb_NewPtr(8, false, true)
            wr16(pat + 6, 0xFFFF) // the last two rows of each eight
            tb_PenPat(pat)
            tb_PenMode(44)
            tb_RGBForeColor(rgb(0xFFFF, 0xFFFF, 0xFFFF))
            tb_RGBBackColor(rgb(0xFFFF, 0xFFFF, 0xFFFF))
            tb_PaintRect(all)
            #expect(pixel(3, 2) == [0xFFFF, 0, 0])
            #expect(pixel(3, 7) == [0xFFFF, 0, 0])
            tb_RGBForeColor(rgb(0, 0, 0xFFFF))
            tb_PaintRect(all)
            #expect(pixel(3, 2) == [0xFFFF, 0, 0])
            #expect(pixel(3, 7) == [0, 0, 0xFFFF])
            tb_PenNormal()
            tb_SetGWorld(rd32(screen), rd32(dev))
            tb_DisposeGWorld(port)
        }

        @Test func auxiliaryControlRecord() {
            launchTestApp()
            _ = initToolbox()
            let h = tb_NewPtr(4, false, true)
            // No colours of its own: false, and the default record (owner nil).
            #expect(tb_GetAuxiliaryControlRecord(0x1000, h) == 0)
            #expect(rd32(h) != 0 && rd32(rd32(rd32(h)) + 4) == 0)
        }
    }
}
