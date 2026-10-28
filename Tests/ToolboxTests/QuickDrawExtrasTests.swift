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

        /// Text in srcCopy paints each character's whole cell: a number
        /// drawn over the last one leaves none of it (Maelstrom's score).
        @Test func copyModeTextPaintsTheWholeCell() {
            _ = offscreen()
            tb_TextFont(3)
            tb_TextSize(9)
            tb_TextMode(0) // srcCopy
            func digit(_ s: String) -> Set<Int> {
                let str = tb_NewPtr(4, false, true)
                guestPString(s, at: str)
                tb_MoveTo(10, 20)
                tb_DrawString(str)
                var on = Set<Int>()
                for v in 8..<24 { for h in 8..<20 where black(h, v) { on.insert(v * 40 + h) } }
                return on
            }
            tb_EraseRect(rect(0, 0, 40, 40))
            let one = digit("1")
            #expect(!one.isEmpty)
            tb_EraseRect(rect(0, 0, 40, 40))
            _ = digit("8")
            #expect(digit("1") == one)
        }

        func color(_ h: Int, _ v: Int) -> [UInt16] {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return [rd16(c), rd16(c + 2), rd16(c + 4)]
        }

        func rgb(_ r: UInt16, _ g: UInt16, _ b: UInt16) -> UInt32 {
            let c = tb_NewPtr(6, false, true)
            wr16(c, r); wr16(c + 2, g); wr16(c + 4, b)
            return c
        }

        /// Pixel patterns draw in their own colours, and an old-style pattern
        /// set afterwards draws in black and white again.
        @Test func pixelPatternsDrawInColour() {
            _ = offscreen()
            let red = tb_NewPixPat()
            tb_MakeRGBPat(red, rgb(0xFFFF, 0, 0))
            tb_FillCRect(rect(0, 0, 10, 10), red)
            #expect(color(5, 5) == [0xFFFF, 0, 0])

            // A 2x2 8-bit pattern: index 0 green, 1 blue, in a checkerboard.
            let pp = tb_NewPixPat()
            let pm = rd32(rd32(pp) + UInt32(PixPat_patMap_off))
            wr16(rd32(pm) + UInt32(PixMap_rowBytes_off), 0x8002)
            for (i, v) in [0, 0, 2, 2].enumerated() { wr16(rd32(pm) + UInt32(PixMap_bounds_off) + UInt32(i * 2), UInt16(v)) }
            wr16(rd32(pm) + UInt32(PixMap_pixelSize_off), 8)
            let ctab = tb_NewHandle(8 + 2 * 8, false, true)
            wr16(rd32(ctab) + 6, 1)
            for (i, c) in [[0, 0xFFFF, 0], [0, 0, 0xFFFF]].enumerated() {
                let e = rd32(ctab) + 8 + UInt32(i * 8)
                wr16(e, UInt16(i)); wr16(e + 2, UInt16(c[0])); wr16(e + 4, UInt16(c[1])); wr16(e + 6, UInt16(c[2]))
            }
            wr32(rd32(pm) + UInt32(PixMap_pmTable_off), ctab)
            let data = rd32(rd32(pp) + UInt32(PixPat_patData_off))
            tb_SetHandleSize(data, 4)
            for (i, v) in [0, 1, 1, 0].enumerated() { wr8(rd32(data) + UInt32(i), UInt8(v)) }
            tb_PenPixPat(pp)
            tb_PaintRect(rect(20, 20, 30, 30))
            #expect(color(20, 20) == [0, 0xFFFF, 0])
            #expect(color(21, 20) == [0, 0, 0xFFFF])
            #expect(color(21, 21) == [0, 0xFFFF, 0])

            let black = tb_NewPtr(8, false, true)
            for i in 0..<8 { wr8(black + UInt32(i), 0xFF) }
            tb_PenPat(black)
            tb_PaintRect(rect(30, 30, 35, 35))
            #expect(color(31, 31) == [0, 0, 0])
        }

        /// A 2x2 8-bit pixel pattern: index 0 green, 1 blue, in a checkerboard.
        func checkerPattern() -> UInt32 {
            let pp = tb_NewPixPat()
            let pm = rd32(rd32(pp) + UInt32(PixPat_patMap_off))
            wr16(rd32(pm) + UInt32(PixMap_rowBytes_off), 0x8002)
            for (i, v) in [0, 0, 2, 2].enumerated() { wr16(rd32(pm) + UInt32(PixMap_bounds_off) + UInt32(i * 2), UInt16(v)) }
            wr16(rd32(pm) + UInt32(PixMap_pixelSize_off), 8)
            let ctab = tb_NewHandle(8 + 2 * 8, false, true)
            wr16(rd32(ctab) + 6, 1)
            for (i, c) in [[0, 0xFFFF, 0], [0, 0, 0xFFFF]].enumerated() {
                let e = rd32(ctab) + 8 + UInt32(i * 8)
                wr16(e, UInt16(i)); wr16(e + 2, UInt16(c[0])); wr16(e + 4, UInt16(c[1])); wr16(e + 6, UInt16(c[2]))
            }
            wr32(rd32(pm) + UInt32(PixMap_pmTable_off), ctab)
            let data = rd32(rd32(pp) + UInt32(PixPat_patData_off))
            tb_SetHandleSize(data, 4)
            for (i, v) in [0, 1, 1, 0].enumerated() { wr8(rd32(data) + UInt32(i), UInt8(v)) }
            return pp
        }

        /// Colour patterns are recorded in pictures with their colours.
        @Test func picturesKeepColourPatterns() {
            _ = offscreen()
            let red = tb_NewPixPat()
            tb_MakeRGBPat(red, rgb(0xFFFF, 0, 0))
            let pic = tb_OpenPicture(rect(0, 0, 40, 40))
            tb_PenPixPat(red)
            tb_PaintRect(rect(0, 0, 10, 10))
            tb_FillCRect(rect(20, 20, 30, 30), checkerPattern())
            tb_ClosePicture()
            let black = tb_NewPtr(8, false, true)
            for i in 0..<8 { wr8(black + UInt32(i), 0xFF) }
            tb_PenPat(black)
            tb_RGBForeColor(rgb(0xFFFF, 0xFFFF, 0xFFFF))
            tb_PaintRect(rect(0, 0, 40, 40))       // white, through the pen
            tb_RGBForeColor(rgb(0, 0, 0))
            #expect(color(5, 5) == [0xFFFF, 0xFFFF, 0xFFFF])
            tb_DrawPicture(pic, rect(0, 0, 40, 40))
            #expect(color(5, 5) == [0xFFFF, 0, 0])
            #expect(color(20, 20) == [0, 0xFFFF, 0])
            #expect(color(21, 20) == [0, 0, 0xFFFF])
            tb_KillPicture(pic)
        }

        /// CopyBits' arithmetic modes combine source and destination colours.
        @Test func arithmeticTransferModes() {
            let dstWorld = offscreen()
            let dst = rd32(dstWorld + 2) // the GWorld's pixel map handle
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 4, 4), 0, 0, 0) == 0)
            let src = rd32(rd32(gw) + 2)
            func fill(_ world: UInt32, _ c: UInt32) {
                tb_SetGWorld(world, 0)
                tb_RGBForeColor(c)
                tb_PaintRect(rect(0, 0, 4, 4))
            }
            func copy(_ mode: Int16) {
                tb_SetGWorld(dstWorld, 0)
                tb_RGBForeColor(rgb(0, 0, 0))
                tb_RGBBackColor(rgb(0xFFFF, 0xFFFF, 0xFFFF))
                tb_CopyBits(rd32(src), rd32(dst), rect(0, 0, 4, 4), rect(0, 0, 4, 4), mode, 0)
            }
            func hi(_ c: [UInt16]) -> [UInt16] { c.map { $0 >> 8 } }
            fill(rd32(gw), rgb(0, 0x8000, 0))
            fill(dstWorld, rgb(0x8000, 0, 0))
            copy(34) // addOver
            #expect(hi(color(1, 1)) == [0x80, 0x80, 0])
            fill(dstWorld, rgb(0x8000, 0, 0))
            tb_SetGWorld(dstWorld, 0)
            tb_OpColor(rgb(0x8000, 0x8000, 0x8000))
            copy(32) // blend, half and half
            #expect(hi(color(1, 1)) == [0x40, 0x40, 0])
            fill(dstWorld, rgb(0x8000, 0, 0))
            copy(37) // addMax
            #expect(hi(color(1, 1)) == [0x80, 0x80, 0])
            fill(rd32(gw), rgb(0xFFFF, 0xFFFF, 0xFFFF))
            fill(dstWorld, rgb(0x8000, 0, 0))
            copy(36) // transparent: the source is all background colour
            #expect(hi(color(1, 1)) == [0x80, 0, 0])
        }

        /// A colour cursor reaches the host with its colours.
        @Test func colourCursorsKeepTheirColours() {
            _ = offscreen()
            // CCrsr (96 bytes), then a PixMap (50), 16 rows of 8-bit pixels
            // (rowBytes 16), and a two-entry colour table: offsets, as in a
            // 'crsr' resource.
            let mapOff = 96, dataOff = 96 + 50, ctOff = 96 + 50 + 256
            let h = tb_NewHandle(Int32(ctOff + 8 + 16), false, true)
            let c = rd32(h)
            wr16(c, 0x8001)
            wr32(c + 2, UInt32(mapOff))
            wr32(c + 6, UInt32(dataOff))
            for i in 0..<32 { wr8(c + 52 + UInt32(i), 0xFF) } // mask: every pixel
            wr16(c + 84, 3); wr16(c + 86, 4)                  // hot spot v, h
            let pm = c + UInt32(mapOff)
            wr16(pm + UInt32(PixMap_rowBytes_off), 0x8010)
            wr16(pm + UInt32(PixMap_bounds_off) + 4, 16); wr16(pm + UInt32(PixMap_bounds_off) + 6, 16)
            wr16(pm + UInt32(PixMap_pixelSize_off), 8)
            wr32(pm + UInt32(PixMap_pmTable_off), UInt32(ctOff))
            wr8(c + UInt32(dataOff) + 1, 1)                    // pixel (1, 0) is entry 1
            let ct = c + UInt32(ctOff)
            wr16(ct + 6, 1)
            wr16(ct + 8, 0); wr16(ct + 10, 0); wr16(ct + 12, 0xFFFF); wr16(ct + 14, 0)        // 0: green
            wr16(ct + 16, 1); wr16(ct + 18, 0xFFFF); wr16(ct + 20, 0); wr16(ct + 22, 0)       // 1: red
            tb_SetCCursor(h)
            var state = machine_cursor()
            _ = machine_cursor_state(&state)
            #expect(state.color)
            #expect(state.hot_h == 4 && state.hot_v == 3)
            let px = withUnsafeBytes(of: state.rgba) { Array($0) }
            #expect(Array(px[0..<4]) == [0, 255, 0, 255])
            #expect(Array(px[4..<8]) == [255, 0, 0, 255])
            tb_InitCursor()
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

        /// Lines drawn while a region is open bound it, as polygons do, and
        /// draw nothing: the pen is hidden until CloseRgn (Shards erases its
        /// flying pieces through regions made of their edges).
        @Test func openRegionLinesEncloseTheirInside() {
            _ = offscreen()
            tb_EraseRect(rect(0, 0, 40, 40))
            let rgn = tb_NewRgn()
            tb_OpenRgn()
            tb_MoveTo(5, 5)
            tb_LineTo(35, 5)
            tb_LineTo(5, 35)
            tb_LineTo(5, 5)
            tb_FrameRect(rect(20, 20, 30, 30)) // exclusive-or with the rest
            tb_CloseRgn(rgn)
            func inside(_ h: Int, _ v: Int) -> Bool { tb_PtInRgn(UInt32(v) << 16 | UInt32(h), rgn) != 0 }
            #expect(inside(8, 8))     // inside the triangle
            #expect(!inside(30, 12))  // beyond the diagonal
            #expect(inside(25, 25))   // inside the framed square only
            #expect(!inside(25, 18))  // ...but not above it
            #expect(!black(5, 5) && !black(20, 5)) // nothing drawn
            tb_MoveTo(0, 38)
            tb_LineTo(39, 38)
            #expect(black(10, 38))    // the pen shows again
            tb_DisposeRgn(rgn)
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

        /// A picture spooled in through the port's getPicProc: the handle
        /// holds only picSize and picFrame, the rest comes through the
        /// proc (Bad Day on the Midway draws its scenes from files so).
        @Test func drawPictureReadsThroughGetPicProc() {
            _ = offscreen()
            tb_EraseRect(rect(0, 0, 40, 40))
            let port = tb_NewPtr(4, false, true)
            tb_GetPort(port)
            // Version 1: paintRect (2,2,10,10), then the end.
            let data: [UInt8] = [0x11, 0x01, 0x31, 0, 2, 0, 2, 0, 10, 0, 10, 0xFF]
            let src = tb_NewPtr(64, false, true)
            for (i, b) in data.enumerated() { wr8(src + UInt32(i), b) }
            let cursor = tb_NewPtr(4, false, true)
            wr32(cursor, src)
            // getPicProc(dataPtr: Ptr; byteCount: INTEGER): copies from *cursor.
            let words: [UInt16] = [
                0x205F, 0x301F, 0x225F, 0x2F08,                     // ret, count, dataPtr; ret back
                0x2079, UInt16(cursor >> 16), UInt16(cursor & 0xFFFF), // MOVEA.L cursor,A0
                0x6002, 0x12D8, 0x51C8, 0xFFFC,                     // copy count bytes
                0x23C8, UInt16(cursor >> 16), UInt16(cursor & 0xFFFF), // MOVE.L A0,cursor
                0x4E75,
            ]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let procs = tb_NewPtr(Int32(CQDProcs_SIZEOF), false, true)
            tb_SetStdCProcs(procs)
            wr32(procs + UInt32(QDProcs_getPicProc_off), code)
            GrafPort_set_grafProcs(rd32(port), procs)
            let pic = tb_NewHandle(10, false, true)
            wr16(rd32(pic), 22)
            for (i, v) in [0, 0, 20, 20].enumerated() { wr16(rd32(pic) + 2 + UInt32(i * 2), UInt16(v)) }
            tb_DrawPicture(pic, rect(0, 0, 20, 20))
            GrafPort_set_grafProcs(rd32(port), 0)
            #expect(rd32(cursor) == src + UInt32(data.count)) // read to its end, no further
            #expect(black(5, 5))
            #expect(!black(15, 15))
        }

        /// A comment with data, in a handle of its own.
        func commentData(_ bytes: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(bytes.count), false, true)
            for (i, b) in bytes.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            return h
        }

        /// PicComment writes ShortComment and LongComment opcodes into the
        /// picture being recorded.
        @Test func recordedPicturesKeepComments() {
            _ = offscreen()
            let pic = tb_OpenPicture(rect(0, 0, 40, 40))
            tb_PicComment(130, 0, 0)                                  // picDwgBeg
            tb_PicComment(1000, 3, commentData([0xDE, 0xAD, 0x42]))
            tb_ClosePicture()
            let p = rd32(pic), size = Int(tb_GetHandleSize(pic))
            let bytes = (0..<size).map { rd8(p + UInt32($0)) }
            func has(_ seq: [UInt8]) -> Bool {
                (0...(bytes.count - seq.count)).contains { Array(bytes[$0..<($0 + seq.count)]) == seq }
            }
            #expect(has([0x00, 0xA0, 0x00, 130]))
            #expect(has([0x00, 0xA1, 0x03, 0xE8, 0x00, 0x03, 0xDE, 0xAD, 0x42]))
            tb_KillPicture(pic)
        }

        /// Drawing a picture gives its comments to the port's commentProc,
        /// a long comment's data in a handle.
        @Test func drawPictureCallsTheCommentProc() {
            _ = offscreen()
            let pic = tb_OpenPicture(rect(0, 0, 40, 40))
            tb_PicComment(130, 0, 0)
            tb_PicComment(1000, 4, commentData([1, 2, 3, 4]))
            tb_ClosePicture()

            let port = tb_NewPtr(4, false, true)
            tb_GetPort(port)
            // commentProc(kind, dataSize: INTEGER; dataHandle: Handle): counts
            // its calls and keeps the last kind, size and data's first long.
            let vars = tb_NewPtr(12, false, true) // count.L, kind.W, size.W, data.L
            func abs(_ a: UInt32) -> [UInt16] { [UInt16(a >> 16), UInt16(a & 0xFFFF)] }
            let words: [UInt16] = [0x205F, 0x225F, 0x301F, 0x321F]  // ret, data, size, kind
                + [0x52B9] + abs(vars)                              // ADDQ.L #1,count
                + [0x33C1] + abs(vars + 4)                          // MOVE.W D1,kind
                + [0x33C0] + abs(vars + 6)                          // MOVE.W D0,size
                + [0x2409, 0x6708, 0x2251] + [0x23D1] + abs(vars + 8) // if data: MOVE.L (*data),data
                + [0x4ED0]                                          // JMP (A0)
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let procs = tb_NewPtr(Int32(CQDProcs_SIZEOF), false, true)
            tb_SetStdCProcs(procs)
            wr32(procs + UInt32(QDProcs_commentProc_off), code)
            GrafPort_set_grafProcs(rd32(port), procs)
            tb_DrawPicture(pic, rect(0, 0, 40, 40))
            GrafPort_set_grafProcs(rd32(port), 0)
            #expect(rd32(vars) == 2)
            #expect(rd16(vars + 4) == 1000 && rd16(vars + 6) == 4)
            #expect(rd32(vars + 8) == 0x0102_0304)
            tb_KillPicture(pic)
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

            // A matchProc decides the boundary: Director's matte ink makes
            // every colour but white one, so the white around the frame is
            // left out and the frame and what it holds are kept.
            // movea.l 4(a7),a0; movea.l 8(a7),a1; moveq #0,d0;
            // cmpi.w #$FFFF,(a1); beq.s +2; moveq #1,d0; move.l d0,(a0);
            // move.b #1,12(a7); movea.l (a7)+,a0; addq.l #8,a7; jmp (a0)
            let words: [UInt16] = [0x206F, 4, 0x226F, 8, 0x7000, 0x0C51, 0xFFFF, 0x6702, 0x7001, 0x2080,
                                   0x1F7C, 1, 12, 0x205F, 0x508F, 0x4ED0]
            let proc = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(proc + UInt32(i * 2), w) }
            let white = tb_NewPtr(6, false, true)
            for i in 0..<3 { wr16(white + UInt32(i * 2), 0xFFFF) }
            let gd = rd32(rd32(LM_TheGDevice)), refCon = gd + UInt32(GDevice_gdRefCon_off)
            wr32(refCon, 1234)
            for i in 0..<80 { wr8(bits + UInt32(i), 0) }
            tb_CalcCMask(src, bm, r, r, white, proc, 99)
            #expect(bit(10, 10) && bit(5, 5) && !bit(2, 2))
            #expect(rd32(refCon) == 1234) // gdRefCon held the matchData only during the call
            // Without it, white is the boundary and nothing can be reached.
            for i in 0..<80 { wr8(bits + UInt32(i), 0) }
            tb_CalcCMask(src, bm, r, r, white, 0, 0)
            #expect(bit(2, 2) && bit(10, 10))
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

        /// A copy of a 1-bit GWorld's portBits (its PixMap handle and the
        /// 0xC000 portVersion) stands for the PixMap, as with CopyBits:
        /// Bubblomania makes its bubbles' regions so.
        @Test func bitMapToRegionFromColourPortBits() {
            launchTestApp()
            _ = initToolbox()
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 1, rect(0, 0, 4, 16), 0, 0, 0) == 0)
            let port = rd32(gw)
            let pmh = rd32(port + 2), pm = rd32(pmh)
            // GWorld pixels are a handle until locked, as QuickDraw's were
            _ = tb_LockPixels(pmh)
            let base = tb_GetPixBaseAddr(pmh), rb = UInt32(rd16(pm + 4) & 0x3FFF)
            wr8(base + rb, 0x0F); wr8(base + 2 * rb, 0x0F)
            let bm = tb_NewPtr(14, false, true)
            for i in 0..<14 { wr8(bm + UInt32(i), rd8(port + 2 + UInt32(i))) }
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

        @Test func internationalResources() {
            launchTestApp()
            func res(_ id: Int16) -> (h: UInt32, p: UInt32, size: Int32) {
                let h = tb_GetIntlResource(id)
                return (h, h == 0 ? 0 : rd32(h), h == 0 ? 0 : tb_GetHandleSize(h))
            }
            let i0 = res(0), i1 = res(1), i2 = res(2), i4 = res(4)
            #expect(i0.size == 32 && rd8(i0.p) == UInt8(ascii: ".") && rd8(i0.p + 1) == UInt8(ascii: ","))
            #expect(rd16(i0.p + 30) == 1) // intl0Vers: U.S., version 1
            // itl1: $A89F in localRtn, then the Itl1ExtRec with its abbreviated names.
            #expect(i1.size == 468 && rd16(i1.p + 330) == 0xA89F && rd16(i1.p + 334) == 1)
            let abbrevDays = rd32(i1.p + 354)
            #expect(rd16(i1.p + abbrevDays) == 7 && pstring(at: i1.p + abbrevDays + 2) == "Sun")
            // itl2: the System 7 header (the -1 flag, format 1) and its tables.
            #expect(i2.size > 64 && rd16(i2.p + 8) == 0xFFFF && rd16(i2.p + 30) == 1)
            let classes = i2.p + UInt32(rd16(i2.p + 14))
            func apply(_ slot: UInt32, _ c: UInt8) -> UInt8 {
                c &+ rd8(i2.p + UInt32(rd16(i2.p + slot * 2)) + UInt32(rd8(classes + UInt32(c))))
            }
            #expect(apply(8, 0x8E) == 0x83 && apply(9, 0x83) == 0x8E) // é and É
            #expect(apply(8, UInt8(ascii: "q")) == UInt8(ascii: "Q") && apply(8, 0xA7) == 0xA7) // ß stays
            #expect(apply(13, 0x8E) == UInt8(ascii: "e") && apply(10, 0x8E) == UInt8(ascii: "E"))
            #expect(apply(13, 0xBF) == UInt8(ascii: "o")) // ø
            // The type list: É an upper-case Euro letter, the digits numbers.
            #expect(apply(6, 0x83) &- 0x83 == 0x87 && apply(6, UInt8(ascii: "5")) &- UInt8(ascii: "5") == 0x10)
            // The tables agree with the older built-in conversions (UprString's,
            // which knew fewer letters) wherever those change a character.
            for c in 0...255 {
                let b = UInt8(c), up = UInt8(mac_char_key(b, false, false)), plain = UInt8(mac_char_key(b, true, true))
                #expect(up == b || apply(8, b) == up, "upper \(c)")
                #expect(plain == b || apply(13, b) == plain, "strip \(c)")
            }
            // The Text Utilities convert by them: á, ı and ø as well.
            let text = tb_NewPtr(8, false, true)
            for (i, ch) in [0x87, 0xF5, 0xBF, 0x61].enumerated() { wr8(text + UInt32(i), UInt8(ch)) }
            tb_TextUtilFunctions(text, 4, 0, 0x0400) // UppercaseText
            #expect((0..<4).map { rd8(text + $0) } == [0xE7, 0x49, 0xAF, 0x41])
            tb_TextUtilFunctions(text, 4, 0, 0x0800) // StripDiacritics
            #expect((0..<4).map { rd8(text + $0) } == [0x41, 0x49, 0x4F, 0x41])
            tb_TextUtilFunctions(text, 4, 0, 0) // LowercaseText
            #expect((0..<4).map { rd8(text + $0) } == [0x61, 0x69, 0x6F, 0x61])
            #expect(i4.size > 0x44 && rd32(i4.p + 2) == UInt32(fourCC("itl4")))
            #expect(tb_GetResource(Int32(bitPattern: fourCC("itlb")), 0) != 0)
            #expect(tb_GetResource(Int32(bitPattern: fourCC("itlc")), 0) != 0)

            let out = tb_NewPtr(12, false, true)
            func table(_ t: Int16) -> (h: UInt32, at: UInt32, len: UInt32) {
                tb_GetIntlResourceTable(0, t, out, out + 4, out + 8)
                return (rd32(out), rd32(out + 4), rd32(out + 8))
            }
            let word = table(0), wrap = table(1), parts = table(2), untoken = table(3), white = table(4)
            #expect(word.h == i2.h && word.len > 256 && rd8(i2.p + word.at) == 0x80) // an NBreakTable
            #expect(rd16(i2.p + word.at + 14) == word.len)
            #expect(wrap.h == i2.h && wrap.len > 256 && wrap.at != word.at)
            #expect(parts.h == i4.h && parts.len == 172 && rd16(i4.p + parts.at) == 1)
            #expect(rd16(i4.p + parts.at + 2 + 13 * 2) == UInt16(UInt8(ascii: "."))) // tokDecPoint
            #expect(rd16(i4.p + parts.at + 2 + 9 * 2) == UInt16(UInt8(ascii: ","))) // tokThousands
            #expect(untoken.h == i4.h && rd16(i4.p + untoken.at) == untoken.len)
            #expect(white.h == i4.h && rd16(i4.p + white.at + 2) == 2) // blank and tab
            #expect(table(9).h == 0 && table(9).len == 0)

            // A long date in day-month order, from an itl1 of the program's own.
            let mine = tb_NewHandle(468, false, false)
            tb_BlockMove(i1.p, rd32(mine), 468, false)
            wr8(rd32(mine) + 305, 0) // lngDateFmt: day name, day, month, year
            let d = tb_NewPtr(14, false, true)
            for (i, v) in [1996, 1, 2, 13, 5, 7, 0].enumerated() { wr16(d + UInt32(i * 2), UInt16(v)) }
            var secs: UInt32 = 0
            tb_DateToSeconds(d, &secs)
            let s = tb_NewPtr(256, false, true)
            tb_IUDatePString(Int32(bitPattern: secs), 1, s, mine)
            #expect(pstring(at: s) == "Tuesday, 2 January, 1996")
            wr8(rd32(mine) + 304, 0xFF) // suppressDay: no day name
            tb_IUDatePString(Int32(bitPattern: secs), 1, s, mine)
            #expect(pstring(at: s) == "2 January, 1996")
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

extension BlueBox {
    /// Floating windows (WDEF 124, procIDs 1985 to 1999).
    @Suite struct FloatingWindows {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func bbox(_ rgn: UInt32) -> [Int] { (0..<4).map { Int(Int16(bitPattern: rd16(rd32(rgn) + 2 + UInt32($0 * 2)))) } }

        func find(_ h: Int, _ v: Int) -> Int16 {
            let wp = tb_NewPtr(4, false, true)
            return tb_FindWindow(UInt32(v) << 16 | UInt32(h), wp)
        }

        @Test func floatingWindowsHaveANarrowTitleBar() {
            launchTestApp()
            _ = initToolbox()
            let title = tb_NewPtr(16, false, true)
            guestPString("Tools", at: title)
            // floatZoomProc (1989): title bar on top, close and zoom boxes.
            let w = tb_NewCWindow(0, rect(100, 100, 200, 160), title, 1, 1989, 0xFFFF_FFFF, 1, 0)
            #expect(bbox(WindowRecord_strucRgn(w)) == [88, 99, 202, 162])
            #expect(find(105, 94) == 6)    // inGoAway
            #expect(find(154, 94) == 7)    // inZoomIn
            #expect(find(130, 94) == 4)    // inDrag: the title bar
            #expect(find(130, 150) == 3)   // inContent
            if let path = ProcessInfo.processInfo.environment["BLUEBOX_TEST_SHOT"] { _ = machine_screenshot(path) }
            // floatSideProc (1993): the bar on the left.
            let s = tb_NewCWindow(0, rect(100, 300, 200, 360), title, 1, 1993, 0xFFFF_FFFF, 1, 0)
            #expect(bbox(WindowRecord_strucRgn(s)) == [99, 288, 202, 362])
            tb_DisposeWindow(s)
            tb_DisposeWindow(w)
        }

        /// A program's own WDEF on a window made invisible is not asked for the
        /// window's regions until the window is shown (CanOpener's WDEF calls
        /// routines the program hands it after NewCWindow returns).
        @Test func ownWDEFCalcsRegionsWhenShown() {
            // move.w 8(a7),d0; cmpi.w #2,d0; bne.s +6; lea count(pc),a1; addq.w #1,(a1);
            // movea.l (a7)+,a0; adda.w #12,a7; jmp (a0); count: dc.w 0
            let words: [UInt16] = [0x302F, 8, 0x0C40, 2, 0x6606, 0x43FA, 12, 0x5251, 0x205F, 0xDEFC, 12, 0x4ED0, 0]
            var code = [UInt8]()
            for v in words { code += [UInt8(v >> 8), UInt8(v & 0xFF)] }
            launchTestApp([TestResource(type: "WDEF", id: 128, data: code)])
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let h = tb_GetResource(Int32(bitPattern: fourCC("WDEF")), 128)
            func count() -> UInt16 { rd16(rd32(h) + 24) }
            let w = tb_NewCWindow(0, rect(100, 100, 200, 300), 0, 0, 0x800, 0xFFFF_FFFF, 0, 0)
            #expect(w != 0 && count() == 0)              // not yet asked
            tb_ShowWindow(w)
            #expect(count() == 1)
            let v = tb_NewCWindow(0, rect(100, 100, 200, 300), 0, 1, 0x800, 0xFFFF_FFFF, 0, 0)
            #expect(count() == 2)                        // a visible one at once
            tb_DisposeWindow(v)
            tb_DisposeWindow(w)
        }

        /// A WDEF that reads portBits.bounds finds the PixMap's bounds there
        /// while it works out a colour window's regions (Total Distortion's
        /// FinderHider WDEF); the grafVars handle is back afterwards.
        @Test func oldWDEFsSeeBoundsInColourWindows() {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            let w = tb_NewCWindow(0, rect(100, 100, 200, 300), 0, 0, 2, 0xFFFF_FFFF, 0, 0)
            let seen = tb_NewPtr(4, false, true)
            // cmpi.w #2,8(a7); bne.s +8; movea.l 10(a7),a0 ... as a WDEF:
            // on wCalcRgns, copy the long at 8(window) to seen; return 0.
            let words: [UInt16] = [0x0C6F, 2, 8, 0x660C, 0x206F, 10, 0x23E8, 8, UInt16(seen >> 16), UInt16(seen & 0xFFFF),
                                   0x4E71, 0x4E71, 0x205F, 0x4FEF, 12, 0x4ED0]
            let code = tb_NewHandle(Int32(words.count * 2), false, false)
            for (i, v) in words.enumerated() { wr16(rd32(code) + UInt32(i * 2), v) }
            let grafVars = rd32(w + UInt32(CGrafPort_grafVars_off))
            WindowRecord_set_windowDefProc(w, code)
            tb_MoveWindow(w, 120, 140, 0)
            // The window's top left, negated, as a BitMap port's bounds hold it.
            #expect(rd32(seen) == UInt32(UInt16(bitPattern: -140)) << 16 | UInt32(UInt16(bitPattern: -120)))
            #expect(rd32(w + UInt32(CGrafPort_grafVars_off)) == grafVars)
            WindowRecord_set_windowDefProc(w, 0)
            tb_DisposeWindow(w)
        }

        /// After InitFloatingWindows the floating windows stay in front:
        /// documents brought forward or made go behind them, the front
        /// document is the active one and the floating ones stay highlighted.
        @Test func floatingWindowsStayInFront() {
            launchTestApp()
            _ = initToolbox()
            let doc1 = tb_NewCWindow(0, rect(100, 100, 200, 300), 0, 1, 0, 0xFFFF_FFFF, 1, 0)
            let palette = tb_NewCWindow(0, rect(100, 400, 200, 460), 0, 1, 1985, 0, 1, 0) // at the back
            #expect(tb_InitFloatingWindows() == 0)
            #expect(tb_FrontWindow() == palette && tb_FrontNonFloatingWindow() == doc1)
            let doc2 = tb_NewCWindow(0, rect(150, 150, 250, 350), 0, 1, 0, 0xFFFF_FFFF, 1, 0)
            #expect(WindowRecord_nextWindow(palette) == doc2)
            tb_SelectWindow(doc1)
            #expect(tb_FrontWindow() == palette && tb_FrontNonFloatingWindow() == doc1)
            #expect(WindowRecord_hilited(doc1) != 0 && WindowRecord_hilited(doc2) == 0)
            #expect(WindowRecord_hilited(palette) != 0)
            for w in [doc2, doc1, palette] { tb_DisposeWindow(w) }
        }

        /// A zoomable window's dataHandle holds its user and standard states
        /// (Shards opens its puzzle window at another window's user state);
        /// ZoomWindow goes between them, keeping where the window was.
        @Test func zoomStatesAreInTheDataHandle() {
            launchTestApp()
            _ = initToolbox()
            let w = tb_NewCWindow(0, rect(100, 100, 200, 300), 0, 1, 8, 0xFFFF_FFFF, 1, 0) // zoomDocProc
            let state = WindowRecord_dataHandle(w)
            func content() -> [Int] { bbox(WindowRecord_contRgn(w)) }
            func get(_ off: UInt32) -> [Int] { (0..<4).map { Int(Int16(bitPattern: rd16(rd32(state) + off + UInt32($0 * 2)))) } }
            #expect(state != 0 && get(0) == [100, 100, 200, 300])
            let std = get(8)
            #expect(std[0] > 20 && std[2] > 300)
            #expect(find(280, 90) == 8)      // inZoomOut
            tb_MoveWindow(w, 120, 140, 0)
            tb_ZoomWindow(w, 8, 0)
            #expect(content() == std && get(0) == [140, 120, 240, 320])
            #expect(find(std[3] - 20, std[0] - 10) == 7) // inZoomIn
            tb_ZoomWindow(w, 7, 0)
            #expect(content() == [140, 120, 240, 320])
            tb_DisposeWindow(w)
        }

        /// Two clicks in a title bar without moving it collapse the window to
        /// its title bar (WindowShade), and two more expand it.
        @Test func doubleClickingTheTitleBarCollapses() {
            launchTestApp()
            _ = initToolbox()
            let title = tb_NewPtr(16, false, true)
            guestPString("Doc", at: title)
            let w = tb_NewCWindow(0, rect(100, 100, 200, 300), title, 1, 0, 0xFFFF_FFFF, 1, 0)
            #expect(bbox(WindowRecord_strucRgn(w)) == [81, 99, 202, 302])
            let pt = UInt32(90) << 16 | 200
            let bounds = rect(20, 0, 700, 1000)
            tb_DragWindow(w, pt, bounds)
            tb_DragWindow(w, pt, bounds)
            #expect(bbox(WindowRecord_strucRgn(w)) == [81, 99, 101, 302])  // the title bar and its shadow
            #expect(rd16(rd32(WindowRecord_contRgn(w))) == 10 && bbox(WindowRecord_contRgn(w)) == [0, 0, 0, 0])
            #expect(find(200, 150) == 0)                                     // nothing where the content was
            tb_DragWindow(w, pt, bounds)
            tb_DragWindow(w, pt, bounds)
            #expect(bbox(WindowRecord_strucRgn(w)) == [81, 99, 202, 302])
            tb_DisposeWindow(w)
        }
    }
}

extension BlueBox {
    /// A window DisposeWindow has just freed stays whole for a while (Star
    /// Chaos moves, sizes, shows and selects its CPU test's window after
    /// disposing it, and plays in it); one brought back is kept.
    @Suite struct DisposedWindows {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func bbox(_ rgn: UInt32) -> [Int] { (0..<4).map { Int(Int16(bitPattern: rd16(rd32(rgn) + 2 + UInt32($0 * 2)))) } }

        @Test func aJustDisposedWindowCanBeUsedAgain() {
            launchTestApp()
            _ = initToolbox()
            let title = tb_NewPtr(2, false, true)
            let w = tb_NewCWindow(0, rect(100, 100, 200, 300), title, 1, 0, 0xFFFF_FFFF, 0, 0)
            let regions = [WindowRecord_strucRgn(w), WindowRecord_contRgn(w), WindowRecord_updateRgn(w)]
            tb_DisposeWindow(w)
            #expect(tb_FrontWindow() == 0)
            // Its regions are not handed out again yet.
            let fresh = (0..<3).map { _ in tb_NewRgn() }
            #expect(Set(fresh).isDisjoint(with: regions))
            tb_MoveWindow(w, 0, 40, 0)
            tb_SizeWindow(w, 400, 300, 1)
            tb_ShowWindow(w)
            tb_SelectWindow(w)
            #expect(tb_FrontWindow() == w)
            #expect(bbox(WindowRecord_strucRgn(w)) == [21, -1, 342, 402])
            // Back in use, it outlives the windows disposed after it.
            for _ in 0..<6 { tb_DisposeWindow(tb_NewCWindow(0, rect(50, 50, 80, 80), title, 0, 0, 0xFFFF_FFFF, 0, 0)) }
            #expect(tb_FrontWindow() == w)
            #expect(bbox(WindowRecord_strucRgn(w)) == [21, -1, 342, 402])
        }
    }
}

extension BlueBox {
    /// Native scroll bars are hit where macOS draws their knob.
    @Suite struct NativeScrollBars {
        @Test func theKnobIsWhereTheHostDrewIt() {
            launchTestApp()
            _ = initToolbox()
            machine_set_appearance(Int32(MACHINE_APPEARANCE_NATIVE))
            defer {
                machine_set_appearance(Int32(MACHINE_APPEARANCE_BW))
                machine_set_control_renderer(nil, nil)
            }
            // A host that draws nothing visible, its knob at 40..70 of a 2..198 slot.
            machine_set_control_renderer({ rq, _, _ in
                rq!.pointee.knob_lo = 40; rq!.pointee.knob_hi = 70
                rq!.pointee.slot_lo = 2; rq!.pointee.slot_hi = 198
                return true
            }, nil)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [20, 300, 220, 316].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let w = tb_NewCWindow(0, r, 0, 1, 0, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            let cr = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 200, 16].enumerated() { wr16(cr + UInt32(i * 2), UInt16(v)) }
            let ch = tb_NewControl(w, cr, 0, 1, 0, 0, 100, 16, 0)
            tb_Draw1Control(ch)
            #expect(tb_TestControl(ch, UInt32(50) << 16 | 8) == 129)   // inThumb: in the native knob
            #expect(tb_TestControl(ch, UInt32(25) << 16 | 8) == 22)    // inPageUp, above it
            #expect(tb_TestControl(ch, UInt32(5) << 16 | 8) == 22)     // no up arrow
            #expect(tb_TestControl(ch, UInt32(195) << 16 | 8) == 23)   // no down arrow
            tb_DisposeWindow(w)
        }
    }
}
