import Testing
import Toolbox

/// The rest of the Image Compression Manager (qt_icm_more.c): description
/// extensions, similarity, converting and trimming, transcoders,
/// thumbnails, effects from data sources, pixel formats and matrices.
extension BlueBox {
    @Suite struct QTICMMore {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(v)) }
            return p
        }

        /// A w by h 32-bit 'raw ' image of one colour (ARGB), its description handle and data.
        func raw(_ w: Int, _ h: Int, _ argb: UInt32) -> (desc: UInt32, data: UInt32) {
            let hd = tb_NewHandle(86, false, true)
            let p = rd32(hd)
            for i in 0..<86 { wr8(p + UInt32(i), 0) }
            wr32(p, 86)
            wr32(p + 4, fourCC("raw "))
            wr16(p + 32, UInt16(w))
            wr16(p + 34, UInt16(h))
            wr32(p + 44, UInt32(w * h * 4))
            wr16(p + 82, 32)
            wr16(p + 84, 0xFFFF)
            let data = tb_NewPtr(Int32(w * h * 4), false, true)
            for i in 0..<(w * h) { wr32(data + UInt32(i * 4), argb) }
            return (hd, data)
        }

        func machine() {
            launchTestApp()
            _ = initToolbox()
            _ = tb_EnterMovies()
        }

        @Test func descriptionExtensions() {
            machine()
            let img = raw(2, 2, 0xFF00_0000)
            let d = Int32(bitPattern: img.desc)
            let ext = tb_NewHandle(3, false, true)
            for i in 0..<3 { wr8(rd32(ext) + UInt32(i), UInt8(i + 1)) }
            #expect(tb_AddImageDescriptionExtension(d, Int32(bitPattern: ext), Int32(bitPattern: fourCC("abcd"))) == 0)
            #expect(tb_AddImageDescriptionExtension(d, Int32(bitPattern: ext), Int32(bitPattern: fourCC("wxyz"))) == 0)
            #expect(rd32(rd32(img.desc)) == 86 + 11 * 2)
            let n = tb_NewPtr(4, false, true)
            #expect(tb_CountImageDescriptionExtensionType(d, Int32(bitPattern: fourCC("abcd")), n) == 0 && rd32(n) == 1)
            #expect(tb_CountImageDescriptionExtensionType(d, 0, n) == 0 && rd32(n) == 2)
            let t = tb_NewPtr(4, false, true)
            #expect(tb_GetNextImageDescriptionExtensionType(d, t) == 0 && rd32(t) == fourCC("abcd"))
            #expect(tb_GetNextImageDescriptionExtensionType(d, t) == 0 && rd32(t) == fourCC("wxyz"))
            let out = tb_NewPtr(4, false, true)
            #expect(tb_GetImageDescriptionExtension(d, out, Int32(bitPattern: fourCC("wxyz")), 1) == 0)
            #expect(tb_GetHandleSize(rd32(out)) == 3 && rd8(rd32(rd32(out)) + 2) == 3)
            #expect(tb_RemoveImageDescriptionExtension(d, Int32(bitPattern: fourCC("abcd")), 1) == 0)
            #expect(tb_CountImageDescriptionExtensionType(d, 0, n) == 0 && rd32(n) == 1)
            #expect(tb_GetImageDescriptionExtension(d, out, Int32(bitPattern: fourCC("abcd")), 1) == -8971)
            // a colour table in front of the extensions, which stay
            let ct = tb_NewHandle(8 + 2 * 8, false, true)
            wr16(rd32(ct) + 6, 1)
            #expect(tb_SetImageDescriptionCTable(d, Int32(bitPattern: ct)) == 0)
            wr16(rd32(img.desc) + 82, 1)
            #expect(rd16(rd32(img.desc) + 84) == 0 && rd32(rd32(img.desc)) == 86 + 24 + 11)
            #expect(tb_GetImageDescriptionExtension(d, out, Int32(bitPattern: fourCC("wxyz")), 1) == 0)
        }

        @Test func convertTrimAndCompare() {
            machine()
            let img = raw(4, 4, 0xFF00_80FF)
            // to Animation and back
            let rleDesc = tb_NewHandle(0, false, false)
            let rleData = tb_NewPtr(1024, false, true)
            #expect(tb_ConvertImage(Int32(bitPattern: img.desc), Int32(bitPattern: img.data), 32, 0, 0, 0x200,
                                    Int32(bitPattern: fourCC("rle ")), 0, Int32(bitPattern: rleDesc),
                                    Int32(bitPattern: rleData)) == 0)
            #expect(rd32(rd32(rleDesc) + 4) == fourCC("rle "))
            // the same picture: similar through and through
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 4, 4), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            let c = tb_NewPtr(6, false, true)
            wr16(c, 0); wr16(c + 2, 0x8080); wr16(c + 4, 0xFFFF)
            tb_RGBForeColor(c)
            tb_PaintRect(rect(0, 0, 4, 4))
            let sim = tb_NewPtr(4, false, true)
            #expect(tb_GetSimilarity(Int32(bitPattern: tb_GetGWorldPixMap(rd32(gw))), 0, Int32(bitPattern: rleDesc),
                                     Int32(bitPattern: rleData), sim) == 0)
            #expect(rd32(sim) > 0xFC00)
            // trimmed to the middle 2x2
            let trim = rect(1, 1, 3, 3)
            let out = tb_NewPtr(1024, false, true)
            #expect(tb_TrimImage(Int32(bitPattern: img.desc), Int32(bitPattern: img.data), 64, 0,
                                 Int32(bitPattern: out), 1024, 0, trim, 0) == 0)
            #expect(rd16(rd32(img.desc) + 32) == 2 && rd16(rd32(img.desc) + 34) == 2)
            #expect(rd32(out) & 0xFF_FFFF == 0x0080FF)
            tb_DisposeGWorld(rd32(gw))
        }

        @Test func transcodersAndThumbnails() {
            machine()
            let img = raw(8, 4, 0xFFFF_0000)
            let its = tb_NewPtr(4, false, true), dd = tb_NewPtr(4, false, true)
            #expect(tb_ImageTranscodeSequenceBegin(its, Int32(bitPattern: img.desc), Int32(bitPattern: fourCC("rle ")),
                                                   dd, 0, 0) == 0)
            let data = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true)
            #expect(tb_ImageTranscodeFrame(Int32(bitPattern: rd32(its)), img.data, 128, data, size) == 0)
            #expect(rd32(size) > 0 && rd32(rd32(rd32(dd)) + 4) == fourCC("rle "))
            #expect(tb_ImageTranscodeDisposeFrameData(Int32(bitPattern: rd32(its)), rd32(data)) == 0)
            #expect(tb_ImageTranscodeSequenceEnd(Int32(bitPattern: rd32(its))) == 0)
            // a thumbnail keeps the shape: 80 by 40
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 4, 8), 0, 0, 0) == 0)
            let thumb = tb_NewHandle(0, false, false)
            #expect(tb_MakeThumbnailFromPixMap(Int32(bitPattern: tb_GetGWorldPixMap(rd32(gw))), 0, 32,
                                               Int32(bitPattern: thumb), 0) == 0)
            let frame = rd32(thumb) + 2
            #expect(rd16(frame + 6) - rd16(frame + 2) == 80 && rd16(frame + 4) - rd16(frame) == 40)
            tb_DisposeGWorld(rd32(gw))
        }

        /// A cross fade half way between red and blue, from two data sources.
        @Test func crossFadeFromSources() {
            machine()
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 4, 4), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            let ed = tb_NewPtr(4, false, true)
            #expect(tb_MakeImageDescriptionForEffect(Int32(bitPattern: fourCC("dslv")), ed) == 0)
            let seq = tb_NewPtr(4, false, true)
            #expect(tb_DecompressSequenceBegin(seq, Int32(bitPattern: rd32(ed)), Int32(bitPattern: rd32(gw)), 0, 0,
                                               0, 0, 0, 0, 0, 0) == 0)
            let s = Int32(bitPattern: rd32(seq))
            let a = raw(4, 4, 0xFFFF_0000), b = raw(4, 4, 0xFF00_00FF)
            let src = tb_NewPtr(8, false, true)
            #expect(tb_CDSequenceNewDataSource(s, src, Int32(bitPattern: fourCC("srcA")), 1, Int32(bitPattern: a.desc), 0, 0) == 0)
            #expect(tb_CDSequenceNewDataSource(s, src + 4, Int32(bitPattern: fourCC("srcB")), 1, Int32(bitPattern: b.desc), 0, 0) == 0)
            #expect(tb_CDSequenceSetSourceData(Int32(bitPattern: rd32(src)), a.data, 64) == 0)
            #expect(tb_CDSequenceSetSourceData(Int32(bitPattern: rd32(src + 4)), b.data, 64) == 0)
            let found = tb_NewPtr(4, false, true)
            #expect(tb_CDSequenceGetDataSource(s, found, Int32(bitPattern: fourCC("srcB")), 1) == 0 && rd32(found) == rd32(src + 4))
            // ICMFrameTimeRecord: value 50 of a virtual 0..100
            let ft = tb_NewPtr(48, false, true)
            wr32(ft + 4, 50); wr32(ft + 8, 600); wr32(ft + 20, 48); wr32(ft + 44, 100)
            let params = tb_NewPtr(16, false, true)
            #expect(tb_DecompressSequenceFrameWhen(s, Int32(bitPattern: params), 16, 0, 0, 0, ft) == 0)
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(1, 1, c)
            #expect(abs(Int(rd16(c) >> 8) - 128) < 3 && rd16(c + 2) >> 8 == 0 && abs(Int(rd16(c + 4) >> 8) - 128) < 3)
            #expect(tb_CDSequenceEnd(s) == 0)
            #expect(tb_CDSequenceGetDataSource(s, found, Int32(bitPattern: fourCC("srcA")), 1) != 0) // gone with it
            // the list of effects
            let list = tb_NewPtr(4, false, true)
            #expect(tb_QTGetEffectsList(list, 2, 2, 0) == 0)
            let n = qtCall("QTCountChildrenOfType", [rd32(list), 0, fourCC("type")])
            #expect(n == 9)
            tb_DisposeGWorld(rd32(gw))
        }

        @Test func pixelFormatsAndMatrices() {
            machine()
            let info = tb_NewPtr(40, false, true)
            wr32(info, 40)
            #expect(tb_ICMGetPixelFormatInfo(32, Int32(bitPattern: info)) == 0 && rd16(info + 8) == 32)
            #expect(tb_ICMGetPixelFormatInfo(Int32(bitPattern: fourCC("2vuy")), Int32(bitPattern: info)) == 0 && rd16(info + 8) == 16)
            #expect(tb_ICMGetPixelFormatInfo(Int32(bitPattern: fourCC("what")), Int32(bitPattern: info)) != 0)
            wr16(info + 8, 12)
            #expect(tb_ICMSetPixelFormatInfo(Int32(bitPattern: fourCC("what")), Int32(bitPattern: info)) == 0)
            wr16(info + 8, 0)
            #expect(tb_ICMGetPixelFormatInfo(Int32(bitPattern: fourCC("what")), Int32(bitPattern: info)) == 0 && rd16(info + 8) == 12)
            // the unit square to a 2x scaled, translated one
            func quad(_ pts: [(Int, Int)]) -> UInt32 {
                let p = tb_NewPtr(32, false, true)
                for (i, (x, y)) in pts.enumerated() { wr32(p + UInt32(i * 8), UInt32(x << 16)); wr32(p + UInt32(i * 8 + 4), UInt32(y << 16)) }
                return p
            }
            let m = tb_NewPtr(36, false, true)
            #expect(tb_QuadToQuadMatrix(quad([(0, 0), (1, 0), (1, 1), (0, 1)]), quad([(10, 20), (12, 20), (12, 22), (10, 22)]), m) == 0)
            #expect(rd32(m) == 2 << 16 && rd32(m + 16) == 2 << 16 && rd32(m + 24) == 10 << 16 && rd32(m + 28) == 20 << 16)
            #expect(rd32(m + 32) == 0x4000_0000 && rd32(m + 8) == 0)
            // a region moved by a matrix
            let rgn = tb_NewRgn()
            tb_RectRgn(rgn, rect(0, 0, 10, 10))
            let t = tb_NewPtr(36, false, true)
            wr32(t, 0x10000); wr32(t + 16, 0x10000); wr32(t + 24, 5 << 16); wr32(t + 28, 7 << 16); wr32(t + 32, 0x4000_0000)
            #expect(tb_TransformRgn(Int32(bitPattern: t), Int32(bitPattern: rgn)) == 0)
            #expect(rd16(rd32(rgn) + 2) == 7 && rd16(rd32(rgn) + 4) == 5)
        }
    }
}
