import Testing
import Toolbox

/// Cinepak, on a hand-made 8x4 movie of one strip: a key frame with a V1
/// and a V4 block, then an inter frame that skips the first block.
@Suite struct QTCinepak {
    private func chunk(_ id: UInt8, _ body: [UInt8]) -> [UInt8] {
        let n = body.count + 4
        return [id, UInt8(n >> 16), UInt8((n >> 8) & 0xFF), UInt8(n & 0xFF)] + body
    }

    private func be16(_ v: Int) -> [UInt8] { [UInt8(v >> 8), UInt8(v & 0xFF)] }

    private func strip(_ id: UInt8, _ chunks: [UInt8]) -> [UInt8] {
        let n = chunks.count + 12
        return [id, UInt8(n >> 16), UInt8((n >> 8) & 0xFF), UInt8(n & 0xFF)]
            + be16(0) + be16(0) + be16(4) + be16(8) + chunks
    }

    private func frame(_ strips: [UInt8]) -> [UInt8] {
        let n = strips.count + 10
        return [0, UInt8(n >> 16), UInt8((n >> 8) & 0xFF), UInt8(n & 0xFF)] + be16(8) + be16(4) + be16(1) + strips
    }

    private func decode(_ d: OpaquePointer, _ sample: [UInt8], into pixels: inout [UInt8]) -> Bool {
        sample.withUnsafeBufferPointer { s in
            pixels.withUnsafeMutableBufferPointer { qt_vdec_decode(d, s.baseAddress, s.count, $0.baseAddress) }
        }
    }

    private func rgb(_ pixels: [UInt8], _ x: Int, _ y: Int) -> [UInt8] {
        let i = (y * 8 + x) * 4
        #expect(pixels[i + 3] == 255)
        return Array(pixels[i ..< i + 3])
    }

    @Test func keyAndInterFrames() throws {
        #expect(qt_vdec_supported(0x6376_6964, 24))
        var format = qt_video_format(format: 0x6376_6964, width: 8, height: 4, depth: 24, palette: nil)
        let d = try #require(qt_vdec_new(&format))
        defer { qt_vdec_free(d) }

        // Entry 0 is grey 100; entry 1 has y 50, 60, 70, 80 with u 10, v -20,
        // so r = y - 40, g = y - 5 + 20, b = y + 20.
        let book: [UInt8] = [100, 100, 100, 100, 0, 0, 50, 60, 70, 80, 10, 0xEC]
        let e1: [[UInt8]] = [[10, 65, 70], [20, 75, 80], [30, 85, 90], [40, 95, 100]]
        let grey: [UInt8] = [100, 100, 100]

        // Intra: block 0 is V1 (flag 0) with entry 1, block 1 V4 (flag 1) with 0, 1, 1, 0.
        let key = frame(strip(0x10, chunk(0x20, book) + chunk(0x22, book)
                              + chunk(0x30, [0x40, 0, 0, 0, 1, 0, 1, 1, 0])))
        var pixels = [UInt8](repeating: 0, count: 8 * 4 * 4)
        #expect(decode(d, key, into: &pixels))
        for y in 0 ..< 4 {
            for x in 0 ..< 4 {
                #expect(rgb(pixels, x, y) == e1[(y / 2) * 2 + x / 2])
            }
        }
        for y in 0 ..< 4 {
            for x in 4 ..< 8 {
                let q = (y / 2) * 2 + (x - 4) / 2
                let expected = q == 0 || q == 3 ? grey : e1[(y % 2) * 2 + x % 2]
                #expect(rgb(pixels, x, y) == expected)
            }
        }

        // Inter: block 0 skipped (0), block 1 coded (1) as V1 (0) with entry 0,
        // using the codebooks kept from the key frame.
        let inter = frame(strip(0x11, chunk(0x31, [0x40, 0, 0, 0, 0])))
        #expect(decode(d, inter, into: &pixels))
        for y in 0 ..< 4 {
            for x in 0 ..< 4 {
                #expect(rgb(pixels, x, y) == e1[(y / 2) * 2 + x / 2])
            }
            for x in 4 ..< 8 {
                #expect(rgb(pixels, x, y) == grey)
            }
        }

        // A truncated sample is refused.
        #expect(!decode(d, Array(key.prefix(30)), into: &pixels))
    }
}
