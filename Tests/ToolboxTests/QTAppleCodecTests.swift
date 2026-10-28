import Testing
import Toolbox

/// Apple's Animation, Graphics, Video and None codecs, on small hand-made
/// samples.
@Suite struct QTAppleCodec {
    private static let rle: UInt32 = 0x726C_6520, smc: UInt32 = 0x736D_6320
    private static let rpza: UInt32 = 0x7270_7A61, raw: UInt32 = 0x7261_7720

    /// White, red, green, blue, then black.
    private static let palette: [UInt32] = [0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF]
        + [UInt32](repeating: 0, count: 252)

    private let none: [UInt8] = [0, 0, 0], white: [UInt8] = [255, 255, 255]
    private let red: [UInt8] = [255, 0, 0], green: [UInt8] = [0, 255, 0], blue: [UInt8] = [0, 0, 255]

    private func open(_ codec: UInt32, _ width: Int, _ height: Int, _ depth: Int) throws -> OpaquePointer {
        #expect(qt_vdec_supported(codec, Int32(depth)))
        return try Self.palette.withUnsafeBufferPointer { p in
            var format = qt_video_format(format: codec, width: Int32(width), height: Int32(height),
                                         depth: Int32(depth), palette: p.baseAddress, desc: nil, desc_len: 0)
            return try #require(qt_vdec_new(&format))
        }
    }

    private func decode(_ d: OpaquePointer, _ sample: [UInt8], into pixels: inout [UInt8]) -> Bool {
        sample.withUnsafeBufferPointer { s in
            pixels.withUnsafeMutableBufferPointer { qt_vdec_decode(d, s.baseAddress, s.count, $0.baseAddress) }
        }
    }

    /// A pixel's colour, or none (black with alpha 0) if never drawn.
    private func rgb(_ pixels: [UInt8], _ width: Int, _ x: Int, _ y: Int) -> [UInt8] {
        let i = (y * width + x) * 4
        return pixels[i + 3] == 255 ? Array(pixels[i ..< i + 3]) : none
    }

    /// 8x4 at 8 bits: line 0 a run of two red units, line 1 a skip of one
    /// unit then a literal green unit, lines 2 and 3 untouched.
    @Test func animation() throws {
        let d = try open(Self.rle, 8, 4, 8)
        defer { qt_vdec_free(d) }
        let body: [UInt8] = [0x00, 0x00,
                             0x01, 0xFE, 1, 1, 1, 1, 0xFF,
                             0x02, 0x01, 2, 2, 2, 2, 0xFF,
                             0x01, 0xFF,
                             0x01, 0xFF]
        let sample = [0, 0, 0, UInt8(body.count + 4)] + body
        var pixels = [UInt8](repeating: 0, count: 8 * 4 * 4)
        #expect(decode(d, sample, into: &pixels))
        for x in 0 ..< 8 {
            #expect(rgb(pixels, 8, x, 0) == red)
            #expect(rgb(pixels, 8, x, 1) == (x < 4 ? none : green))
            #expect(rgb(pixels, 8, x, 2) == none)
        }

        // A header with a start line: only line 3 changes, a blue literal unit.
        let part: [UInt8] = [0x00, 0x08, 0, 3, 0, 0, 0, 1, 0, 0, 0x01, 0x01, 3, 3, 3, 3, 0xFF]
        #expect(decode(d, [0, 0, 0, UInt8(part.count + 4)] + part, into: &pixels))
        #expect(rgb(pixels, 8, 0, 0) == red)
        #expect(rgb(pixels, 8, 3, 3) == blue)
        #expect(rgb(pixels, 8, 4, 3) == none)

        // Eight bytes or fewer: no change.
        #expect(decode(d, [0, 0, 0, 6, 0, 0], into: &pixels))
        // Truncated mid-line: refused.
        #expect(!decode(d, Array(sample.prefix(12)), into: &pixels))
    }

    /// 16-bit RGB555 runs, one pixel per unit.
    @Test func animationThousands() throws {
        let d = try open(Self.rle, 3, 1, 16)
        defer { qt_vdec_free(d) }
        let body: [UInt8] = [0x00, 0x00, 0x01, 0xFE, 0x7C, 0x00, 0x01, 0x00, 0x1F, 0xFF]
        var pixels = [UInt8](repeating: 0, count: 3 * 4)
        #expect(decode(d, [0, 0, 0, UInt8(body.count + 4)] + body, into: &pixels))
        #expect(rgb(pixels, 3, 0, 0) == red)
        #expect(rgb(pixels, 3, 1, 0) == red)
        #expect(rgb(pixels, 3, 2, 0) == blue)
    }

    /// One 4x4 block of one colour, then a frame that skips it: the block
    /// stays, even if the caller's frame was cleared.
    @Test func graphics() throws {
        let d = try open(Self.smc, 4, 4, 8)
        defer { qt_vdec_free(d) }
        var pixels = [UInt8](repeating: 0, count: 4 * 4 * 4)
        #expect(decode(d, [0x80, 0, 0, 6, 0x60, 1], into: &pixels))
        for y in 0 ..< 4 {
            for x in 0 ..< 4 {
                #expect(rgb(pixels, 4, x, y) == red)
            }
        }
        pixels = [UInt8](repeating: 0, count: 4 * 4 * 4)
        #expect(decode(d, [0x80, 0, 0, 5, 0x00], into: &pixels))
        #expect(rgb(pixels, 4, 2, 2) == red)
    }

    /// 6x4 (two blocks, the second clipped): a two-colour block with its top
    /// row green, then the block repeated.
    @Test func graphicsTwoColoursAndRepeat() throws {
        let d = try open(Self.smc, 6, 4, 8)
        defer { qt_vdec_free(d) }
        var pixels = [UInt8](repeating: 0, count: 6 * 4 * 4)
        #expect(decode(d, [0x80, 0, 0, 10, 0x80, 1, 2, 0xF0, 0x00, 0x20], into: &pixels))
        for x in 0 ..< 6 {
            #expect(rgb(pixels, 6, x, 0) == green)
            #expect(rgb(pixels, 6, x, 3) == red)
        }
        // A repeat with nothing before it is refused.
        #expect(!decode(d, [0x80, 0, 0, 5, 0x20], into: &pixels))
    }

    /// 5x5 at 16 bits (2x2 blocks, clipped): one colour over four blocks,
    /// then a frame that skips three and fills the last with blue.
    @Test func video() throws {
        let d = try open(Self.rpza, 5, 5, 16)
        defer { qt_vdec_free(d) }
        var pixels = [UInt8](repeating: 0, count: 5 * 5 * 4)
        #expect(decode(d, [0xE1, 0, 0, 7, 0xA3, 0x7C, 0x00], into: &pixels))
        for y in 0 ..< 5 {
            for x in 0 ..< 5 {
                #expect(rgb(pixels, 5, x, y) == red)
            }
        }
        #expect(decode(d, [0xE1, 0, 0, 8, 0x82, 0xA0, 0x00, 0x1F], into: &pixels))
        #expect(rgb(pixels, 5, 3, 3) == red)
        #expect(rgb(pixels, 5, 4, 4) == blue)
    }

    /// Two-colour blocks: indices 0 and 3 are the colours, 1 and 2 blends.
    @Test func videoFourColours() throws {
        let d = try open(Self.rpza, 4, 4, 16)
        defer { qt_vdec_free(d) }
        var pixels = [UInt8](repeating: 0, count: 4 * 4 * 4)
        // colour A white, B black; each row indices 0, 1, 2, 3.
        #expect(decode(d, [0xE1, 0, 0, 13, 0xC0, 0x7F, 0xFF, 0x00, 0x00, 0x1B, 0x1B, 0x1B, 0x1B],
                       into: &pixels))
        #expect(rgb(pixels, 4, 0, 0) == none)
        #expect(rgb(pixels, 4, 3, 0) == white)
        #expect(rgb(pixels, 4, 1, 2) == [82, 82, 82])   // 31 * 11 / 32 = 10
        #expect(rgb(pixels, 4, 2, 2) == [165, 165, 165]) // 31 * 21 / 32 = 20
    }

    /// 16-bit rows of 3 pixels padded to 8 bytes.
    @Test func none16() throws {
        let d = try open(Self.raw, 3, 2, 16)
        defer { qt_vdec_free(d) }
        let sample: [UInt8] = [0x7C, 0x00, 0x03, 0xE0, 0x00, 0x1F, 0xAA, 0xAA,
                               0x7F, 0xFF, 0x00, 0x00, 0x7C, 0x00, 0xAA, 0xAA]
        var pixels = [UInt8](repeating: 0, count: 3 * 2 * 4)
        #expect(decode(d, sample, into: &pixels))
        #expect(rgb(pixels, 3, 0, 0) == red)
        #expect(rgb(pixels, 3, 1, 0) == green)
        #expect(rgb(pixels, 3, 2, 0) == blue)
        #expect(rgb(pixels, 3, 0, 1) == white)
        #expect(rgb(pixels, 3, 1, 1) == [0, 0, 0])
        #expect(rgb(pixels, 3, 2, 1) == red)
        #expect(!decode(d, Array(sample.prefix(10)), into: &pixels))
    }

    /// 8-bit indices through the palette.
    @Test func none8() throws {
        let d = try open(Self.raw, 2, 2, 8)
        defer { qt_vdec_free(d) }
        var pixels = [UInt8](repeating: 0, count: 2 * 2 * 4)
        #expect(decode(d, [0, 1, 2, 3], into: &pixels))
        #expect(rgb(pixels, 2, 0, 0) == white)
        #expect(rgb(pixels, 2, 1, 0) == red)
        #expect(rgb(pixels, 2, 0, 1) == green)
        #expect(rgb(pixels, 2, 1, 1) == blue)
    }

    @Test func depths() {
        for depth in [1, 2, 4, 8, 16, 24, 32, 33, 34, 36, 40] {
            #expect(qt_vdec_supported(Self.rle, Int32(depth)))
            #expect(qt_vdec_supported(Self.raw, Int32(depth)))
        }
        #expect(qt_vdec_supported(Self.smc, 8) && !qt_vdec_supported(Self.smc, 16))
        #expect(qt_vdec_supported(Self.rpza, 16) && !qt_vdec_supported(Self.rpza, 8))
    }
}
