import Testing
import Toolbox

/// The Image Compression Manager's decompression, on a 2x2 'raw ' image:
/// DecompressImage, a sequence, and a compressed picture ($8200).
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
            var body: [UInt8] = be32(0x10000) + be32(0) + be32(0) + be32(0) + be32(0x10000) + be32(0)
                + be32(2 << 16) + be32(2 << 16) + be32(0x4000_0000)            // matrix: translate 2,2
            body += be32(0) + [UInt8](repeating: 0, count: 8) + [0, 0]          // no matte; mode
            body += [0, 0, 0, 0, 0, 2, 0, 2] + be32(0) + be32(0)                 // srcRect; accuracy; no mask
            let d = tb_NewPtr(86, false, true)
            describe(d)
            body += (0..<86).map { rd8(d + UInt32($0)) } + pixels.flatMap(be32)
            bytes += be32(UInt32(body.count)) + body
            if bytes.count % 2 == 1 { bytes.append(0) }
            bytes += [0x00, 0xFF]
            let pic = tb_NewHandle(Int32(bytes.count), false, true)
            for (i, b) in bytes.enumerated() { wr8(rd32(pic) + UInt32(i), b) }
            tb_DrawPicture(pic, rect(0, 0, 20, 20))
            #expect(pixel(2, 2) == (0xFFFF, 0, 0))
            #expect(pixel(3, 3) == (0xFFFF, 0xFFFF, 0xFFFF))
            tb_DisposeGWorld(port)
        }
    }
}
