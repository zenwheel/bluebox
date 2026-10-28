import BlueBoxHost
import CPU68K
import Testing
import Toolbox

/// The Image Compression Manager's decompression, on a 2x2 'raw ' image:
/// DecompressImage, a sequence, and a compressed picture ($8200); and its
/// compression: images, sequences and pictures compressed and drawn back,
/// and the Standard Image Compression component.
extension BlueBox {
    @Suite struct ImageCompression {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(v)) }
            return p
        }

        /// An ImageDescription for a 2x2 32-bit 'raw ' image, at p.
        func describe(_ p: UInt32) {
            for i in 0..<86 { wr8(p + UInt32(i), 0) }
            wr32(p, 86)
            wr32(p + 4, 0x7261_7720)
            wr16(p + 32, 2)
            wr16(p + 34, 2)
            wr32(p + 44, 16)
            wr16(p + 82, 32)
            wr16(p + 84, 0xFFFF)
        }

        /// Red, green / blue, white, as ARGB.
        let pixels: [UInt32] = [0xFFFF_0000, 0xFF00_FF00, 0xFF00_00FF, 0xFFFF_FFFF]

        func pixel(_ h: Int, _ v: Int) -> (UInt16, UInt16, UInt16) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return (rd16(c), rd16(c + 2), rd16(c + 4))
        }

        func gworld() -> UInt32 {
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 20, 20), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            return rd32(gw)
        }

        @Test func decompressImageScalesIntoThePort() {
            launchTestApp()
            _ = initToolbox()
            let port = gworld()
            let desc = tb_NewHandle(86, false, true)
            describe(rd32(desc))
            let data = tb_NewPtr(16, false, true)
            for (i, v) in pixels.enumerated() { wr32(data + UInt32(i * 4), v) }
            // Twice the size, at 4,4.
            #expect(tb_DecompressImage(Int32(bitPattern: data), Int32(bitPattern: desc), 0, 0, rect(4, 4, 8, 8), 0, 0) == 0)
            #expect(pixel(4, 4) == (0xFFFF, 0, 0))
            #expect(pixel(7, 5) == (0, 0xFFFF, 0))
            #expect(pixel(5, 7) == (0, 0, 0xFFFF))
            let ct = tb_NewPtr(4, false, true)
            #expect(tb_GetImageDescriptionCTable(Int32(bitPattern: desc), ct) == 0 && rd32(ct) == 0) // direct: none
            tb_DisposeGWorld(port)
        }

        @Test func sequencesKeepTheirPlace() {
            launchTestApp()
            _ = initToolbox()
            let port = gworld()
            let desc = tb_NewHandle(86, false, true)
            describe(rd32(desc))
            let seq = tb_NewPtr(4, false, true)
            // Translated to 10,10 by the matrix.
            let matrix = tb_NewPtr(36, false, true)
            wr32(matrix, 0x10000); wr32(matrix + 16, 0x10000); wr32(matrix + 24, 10 << 16); wr32(matrix + 28, 10 << 16)
            wr32(matrix + 32, 0x4000_0000)
            #expect(tb_DecompressSequenceBegin(seq, Int32(bitPattern: desc), Int32(bitPattern: port), 0, 0,
                                               Int32(bitPattern: matrix), 0, 0, 0, 0, 0) == 0)
            let data = tb_NewPtr(16, false, true)
            for (i, v) in pixels.enumerated() { wr32(data + UInt32(i * 4), v) }
            #expect(tb_DecompressSequenceFrameS(Int32(bitPattern: rd32(seq)), Int32(bitPattern: data), 16, 0, 0, 0) == 0)
            #expect(pixel(10, 10) == (0xFFFF, 0, 0))
            #expect(pixel(11, 11) == (0xFFFF, 0xFFFF, 0xFFFF))
            #expect(tb_CDSequenceEnd(Int32(bitPattern: rd32(seq))) == 0)
            tb_DisposeGWorld(port)
        }

        @Test func compressedPicturesDraw() {
            launchTestApp()
            _ = initToolbox()
            let port = gworld()
            // A version 2 picture: header, then $8200 with the image at 2,2.
            var bytes: [UInt8] = [0, 0] + [0, 0, 0, 0, 0, 20, 0, 20] + [0x00, 0x11, 0x02, 0xFF]
                + [0x0C, 0x00] + [UInt8](repeating: 0, count: 24) + [0x82, 0x00]
            func be32(_ v: UInt32) -> [UInt8] { (0..<4).map { UInt8(v >> (24 - 8 * UInt32($0)) & 0xFF) } }
            var body: [UInt8] = [0, 0] + be32(0x10000) + be32(0) + be32(0) + be32(0) + be32(0x10000) + be32(0)
                + be32(2 << 16) + be32(2 << 16) + be32(0x4000_0000)            // matrix: translate 2,2
            body += be32(0) + [UInt8](repeating: 0, count: 8) + [0, 0]          // no matte; mode
            body += [0, 0, 0, 0, 0, 2, 0, 2] + be32(0) + be32(0)                 // srcRect; accuracy; no mask
            let d = tb_NewPtr(86, false, true)
            describe(d)
            body += (0..<86).map { rd8(d + UInt32($0)) } + pixels.flatMap(be32)
            bytes += be32(UInt32(body.count)) + body
            if bytes.count % 2 == 1 { bytes.append(0) }
            // the warning for Macs without QuickTime, which isn't drawn
            bytes += [0x00, 0x03, 0x00, 0x03, 0x00, 0x0D, 0x00, 0x0C, 0x00, 0x28, 0x00, 0x10, 0x00, 0x08, 0x02]
                + Array("QT".utf8) + [0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x14]
            bytes += [0x00, 0xFF]
            let pic = tb_NewHandle(Int32(bytes.count), false, true)
            for (i, b) in bytes.enumerated() { wr8(rd32(pic) + UInt32(i), b) }
            tb_DrawPicture(pic, rect(0, 0, 20, 20))
            #expect(pixel(2, 2) == (0xFFFF, 0, 0))
            #expect(pixel(3, 3) == (0xFFFF, 0xFFFF, 0xFFFF))
            var inked = 0
            for y in 6..<18 { for x in 6..<18 where pixel(x, y) != (0xFFFF, 0xFFFF, 0xFFFF) { inked += 1 } }
            #expect(inked == 0)
            #expect(pixel(0, 10) == (0, 0, 0)) // the frame after the warning is drawn
            tb_DisposeGWorld(port)
        }

        // ---- Compression ----

        let raw: UInt32 = 0x7261_7720, rle: UInt32 = 0x726C_6520, jpeg: UInt32 = 0x6A70_6567

        func gworld(depth: Int16, _ w: Int = 20, _ h: Int = 20) -> UInt32 {
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, depth, rect(0, 0, h, w), 0, 0, 0) == 0)
            return rd32(gw)
        }

        /// Paints a picture with runs and detail: solid bands, then a
        /// pattern of the standard colours (exact at every depth).
        func paint(_ gw: UInt32, _ w: Int = 20, _ h: Int = 20, smooth: Bool = false) {
            tb_SetGWorld(gw, 0)
            let c = tb_NewPtr(6, false, true)
            let colours: [(UInt16, UInt16, UInt16)] = [(0, 0, 0), (0xFFFF, 0xFFFF, 0xFFFF), (0xFFFF, 0, 0),
                                                       (0, 0xFFFF, 0), (0, 0, 0xFFFF)]
            for y in 0..<h {
                for x in 0..<w {
                    var rgb = y < 5 ? colours[2] : colours[(x * 7 + y * 3) % 5]
                    if smooth { rgb = (UInt16(x * 3000), UInt16(y * 3000), 0x8000) }
                    wr16(c, rgb.0); wr16(c + 2, rgb.1); wr16(c + 4, rgb.2)
                    tb_SetCPixel(Int16(x), Int16(y), c)
                }
            }
        }

        func colours(_ gw: UInt32, _ w: Int = 20, _ h: Int = 20) -> [(UInt16, UInt16, UInt16)] {
            tb_SetGWorld(gw, 0)
            var out: [(UInt16, UInt16, UInt16)] = []
            for y in 0..<h { for x in 0..<w { out.append(pixel(x, y)) } }
            return out
        }

        func same(_ a: [(UInt16, UInt16, UInt16)], _ b: [(UInt16, UInt16, UInt16)], within: Int = 0) -> Bool {
            a.count == b.count && zip(a, b).allSatisfy {
                abs(Int($0.0) - Int($1.0)) >> 8 <= within && abs(Int($0.1) - Int($1.1)) >> 8 <= within
                    && abs(Int($0.2) - Int($1.2)) >> 8 <= within
            }
        }

        /// Compresses a GWorld's pixels and decompresses them into another.
        func roundTrip(_ type: UInt32, depth: Int16, compressDepth: Int16 = 0, quality: Int32 = 0x200,
                       smooth: Bool = false) -> (sent: [(UInt16, UInt16, UInt16)], back: [(UInt16, UInt16, UInt16)], desc: UInt32) {
            let src = gworld(depth: depth), dst = gworld(depth: depth)
            paint(src, smooth: smooth)
            let pm = tb_GetGWorldPixMap(src)
            let size = tb_NewPtr(4, false, true)
            #expect(tb_GetMaxCompressionSize(Int32(bitPattern: pm), 0, compressDepth, quality, Int32(bitPattern: type), 0, size) == 0)
            let data = tb_NewPtr(Int32(rd32(size)), false, true)
            let desc = tb_NewHandle(4, false, true)
            #expect(tb_FCompressImage(Int32(bitPattern: pm), 0, compressDepth, quality, Int32(bitPattern: type), 0, 0, 0, 0,
                                      0, 0, Int32(bitPattern: desc), Int32(bitPattern: data)) == 0)
            #expect(rd32(rd32(desc) + 4) == type)
            #expect(rd32(rd32(desc) + 44) > 0 && rd32(rd32(desc) + 44) <= rd32(size))
            tb_SetGWorld(dst, 0)
            #expect(tb_DecompressImage(Int32(bitPattern: data), Int32(bitPattern: desc), 0, 0, 0, 0, 0) == 0)
            let result = (colours(src), colours(dst), rd32(desc))
            tb_DisposeGWorld(src)
            tb_DisposeGWorld(dst)
            return result
        }

        @Test func noneAndAnimationRoundTripExactly() {
            launchTestApp()
            _ = initToolbox()
            for depth: Int16 in [8, 16, 32] {
                let r = roundTrip(raw, depth: depth)
                #expect(same(r.sent, r.back), "'raw ' at \(depth)")
                #expect(rd16(r.desc + 82) == UInt16(depth))
                let a = roundTrip(rle, depth: depth)
                #expect(same(a.sent, a.back), "'rle ' at \(depth)")
                #expect(rd16(a.desc + 82) == UInt16(depth))
            }
            for depth: Int16 in [1, 2, 4] {
                let a = roundTrip(rle, depth: depth)
                #expect(same(a.sent, a.back), "'rle ' at \(depth)")
                let b = roundTrip(raw, depth: depth)
                #expect(same(a.back, b.back), "'rle ' and 'raw ' agree at \(depth)")
            }
            let m = roundTrip(rle, depth: 32, compressDepth: 24) // Millions of Colors, from a 32-bit GWorld
            #expect(same(m.sent, m.back))
            #expect(rd16(m.desc + 82) == 24)
            let i = roundTrip(rle, depth: 8)
            #expect(rd16(i.desc + 84) == 0 && rd32(i.desc) == 86 + 8 + 256 * 8) // the colour table travels along
        }

        @Test func photoJPEGComesBackClose() {
            launchTestApp()
            _ = initToolbox()
            ClassicImages.install()
            let r = roundTrip(jpeg, depth: 32, quality: 0x300, smooth: true)
            #expect(same(r.sent, r.back, within: 24))
            #expect(rd16(r.desc + 82) == 24)
            let info = tb_NewPtr(80, false, true)
            #expect(tb_GetCodecInfo(info, Int32(bitPattern: jpeg), 0) == 0 && rd32(info + 44) != 0) // a compressor
            let comp = tb_NewPtr(8, false, true)
            #expect(tb_FindCodec(Int32(bitPattern: rle), 0, comp, comp + 4) == 0 && rd32(comp) == rle)
            let list = tb_NewPtr(4, false, true)
            #expect(tb_GetCodecNameList(list, 0) == 0 && rd16(rd32(list)) == 3)
            #expect(tb_DisposeCodecNameList(Int32(bitPattern: rd32(list))) == 0)
        }

        @Test(arguments: [1, 2, 8, 16, 32] as [Int16])
        func sequencesCompressDifferenceFrames(depth: Int16) {
            launchTestApp()
            _ = initToolbox()
            let src = gworld(depth: depth, 40, 20), dst = gworld(depth: depth, 40, 20)
            paint(src, 40, 20)
            let pm = Int32(bitPattern: tb_GetGWorldPixMap(src))
            let seq = tb_NewPtr(4, false, true), desc = tb_NewHandle(4, false, true)
            #expect(tb_CompressSequenceBegin(seq, pm, 0, 0, 0, 0, Int32(bitPattern: rle), 0, 0x200, 0x200, 10, 0, 4,
                                             Int32(bitPattern: desc)) == 0)
            let id = Int32(bitPattern: rd32(seq))
            let max = tb_NewPtr(4, false, true)
            #expect(tb_GetCSequenceMaxCompressionSize(id, pm, max) == 0)
            let key = tb_NewPtr(Int32(rd32(max)), false, true), diff = tb_NewPtr(Int32(rd32(max)), false, true)
            let size = tb_NewPtr(8, false, true), sim = tb_NewPtr(2, false, true)
            #expect(tb_CompressSequenceFrame(id, pm, 0, 0, Int32(bitPattern: key), size, sim, 0) == 0)
            let keySize = rd32(size)
            #expect(rd8(sim) == 0) // a key frame
            tb_SetGWorld(src, 0)
            tb_RGBForeColor({ let c = tb_NewPtr(6, false, true); wr16(c + 2, 0xFFFF); return c }())
            tb_PaintRect(rect(8, 8, 11, 30))
            #expect(tb_CompressSequenceFrame(id, pm, 0, 0, Int32(bitPattern: diff), size, sim, 0) == 0)
            let diffSize = rd32(size)
            #expect(rd8(sim) != 0)
            #expect(diffSize < keySize / 2)
            let rate = tb_NewPtr(4, false, true)
            #expect(tb_GetCSequenceKeyFrameRate(id, rate) == 0 && rd32(rate) == 10)
            #expect(tb_CDSequenceEnd(id) == 0)
            #expect(tb_CDSequenceEnd(id) != 0)
            // Both frames through a decompression sequence.
            let dseq = tb_NewPtr(4, false, true)
            #expect(tb_DecompressSequenceBegin(dseq, Int32(bitPattern: desc), Int32(bitPattern: dst), 0, 0, 0, 0, 0, 0, 0, 0) == 0)
            let did = Int32(bitPattern: rd32(dseq))
            #expect(tb_DecompressSequenceFrameS(did, Int32(bitPattern: key), Int32(keySize), 0, 0, 0) == 0)
            #expect(tb_DecompressSequenceFrameS(did, Int32(bitPattern: diff), Int32(diffSize), 0, 0, 0) == 0)
            #expect(tb_CDSequenceEnd(did) == 0)
            #expect(same(colours(src, 40, 20), colours(dst, 40, 20)))
            tb_SetGWorld(dst, 0)
            #expect(depth < 16 || pixel(9, 9) == (0, 0xFFFF, 0))
            tb_DisposeGWorld(src)
            tb_DisposeGWorld(dst)
        }

        @Test func compressedPicturesDrawBack() {
            launchTestApp()
            _ = initToolbox()
            let a = gworld(depth: 32), b = gworld(depth: 32)
            tb_SetGWorld(a, 0)
            let pic = tb_OpenPicture(rect(0, 0, 20, 20))
            tb_PaintRect(rect(2, 2, 10, 15))
            tb_PaintOval(rect(12, 5, 18, 18))
            tb_ClosePicture()
            tb_EraseRect(rect(0, 0, 20, 20))
            tb_DrawPicture(pic, rect(0, 0, 20, 20))
            for type in [rle, raw] {
                let dst = tb_NewHandle(0, false, true)
                #expect(tb_CompressPicture(Int32(bitPattern: pic), Int32(bitPattern: dst), 0x200, Int32(bitPattern: type)) == 0)
                #expect(rd16(rd32(dst) + 40) == 0x8200)
                tb_SetGWorld(b, 0)
                tb_EraseRect(rect(0, 0, 20, 20))
                tb_DrawPicture(dst, rect(0, 0, 20, 20))
                #expect(same(colours(a), colours(b)))
                #expect(pixel(3, 3) == (0, 0, 0) && pixel(1, 1) == (0xFFFF, 0xFFFF, 0xFFFF))
                tb_DisposeHandle(dst)
            }
            tb_DisposeGWorld(a)
            tb_DisposeGWorld(b)
        }

        // The Standard Image Compression component, called as a 68k program would.

        func code(_ words: [UInt16]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
            return p
        }

        func push(_ v: UInt32) -> [UInt16] { [0x2F3C, UInt16(v >> 16), UInt16(v & 0xFFFF)] }

        /// A component call's result: the arguments (4 bytes each) after the instance.
        func call(_ ci: UInt32, _ what: UInt16, _ args: [UInt32]) -> UInt32 {
            let out = tb_NewPtr(4, false, true)
            var w: [UInt16] = [0x42A7] + push(ci)
            for a in args { w += push(a) }
            w += push(UInt32(args.count * 4) << 16 | UInt32(what)) + [0x7000, 0xA82A]
            w += [0x23DF, UInt16(out >> 16), UInt16(out & 0xFFFF), 0x4E75]
            guest_call(code(w))
            return rd32(out)
        }

        @Test func standardCompressionComponentCompresses() {
            launchTestApp()
            _ = initToolbox()
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let out = tb_NewPtr(4, false, true)
            // OpenDefaultComponent('scdi', 'imag')
            guest_call(code([0x42A7] + push(0x7363_6469) + push(0x696D_6167) + [0x7021, 0xA82A,
                             0x23DF, UInt16(out >> 16), UInt16(out & 0xFFFF), 0x4E75]))
            let ci = rd32(out)
            #expect(ci != 0)
            let src = gworld(depth: 8)
            paint(src)
            let pm = tb_GetGWorldPixMap(src)
            #expect(call(ci, 10, []) == 0) // SCRequestImageSettings: OK
            let sp = tb_NewPtr(14, false, true)
            wr32(sp, rle); wr16(sp + 8, 8); wr32(sp + 10, 0x200)
            #expect(call(ci, 22, [0x7370_746C, sp]) == 0) // SCSetInfo('sptl')
            let back = tb_NewPtr(14, false, true)
            let gi = call(ci, 21, [0x7370_746C, back])
            #expect(gi == 0 && rd32(back) == rle && rd16(back + 8) == 8)
            let descp = tb_NewPtr(8, false, true)
            #expect(call(ci, 11, [pm, 0, descp, descp + 4]) == 0) // SCCompressImage
            let desc = rd32(descp), data = rd32(descp + 4)
            #expect(rd32(rd32(desc) + 4) == rle && UInt32(tb_GetHandleSize(data)) == rd32(rd32(desc) + 44))
            let dst = gworld(depth: 8)
            tb_SetGWorld(dst, 0)
            tb_HLock(data)
            #expect(tb_DecompressImage(Int32(bitPattern: rd32(data)), Int32(bitPattern: desc), 0, 0, 0, 0, 0) == 0)
            #expect(same(colours(src), colours(dst)))
            tb_DisposeGWorld(src)
            tb_DisposeGWorld(dst)
        }
    }
}
