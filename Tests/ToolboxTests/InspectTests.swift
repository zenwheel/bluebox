import BlueBoxHost
import CCore
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// bluebox inspect: resources described, and exported as PNG, WAV and text.
    @Suite struct Inspect {
        /// A version 2 picture, 20 wide and 10 high: a red rectangle at 2,2,8,10.
        static let picture = cat(be16(0), be16(0), be16(0), be16(10), be16(20), be16(0x0011), be16(0x02FF),
                                 be16(0x0C00), [UInt8](repeating: 0xFF, count: 4), [UInt8](repeating: 0, count: 20),
                                 be16(0x001A), be16(0xFFFF), be16(0), be16(0),    // RGBFgCol red
                                 be16(0x0031), be16(2), be16(2), be16(8), be16(10), // PaintRect
                                 be16(0x00FF))

        /// An ICN#: the icon a filled square, the mask its left half.
        static let icon: [UInt8] = {
            var bits = [UInt8](), mask = [UInt8]()
            for _ in 0..<32 {
                bits += [0xFF, 0xFF, 0xFF, 0xFF]
                mask += [0xFF, 0xFF, 0x00, 0x00]
            }
            return bits + mask
        }()

        /// A format 1 'snd ': a standard header, 100 8-bit samples at 11025 Hz.
        static let sound8: [UInt8] = {
            let header = cat(be32(0), be32(100), be32(11025 << 16), be32(0), be32(0), [0, 60])
            let samples = (0..<100).map { UInt8($0 + 50) }
            return cat(be16(1), be16(1), be16(5), be32(0), be16(1), be16(0x8000 | 81), be16(0), be32(20), header, samples)
        }()

        /// A format 2 'snd ': an extended header, 16-bit stereo, 8 frames at 44100 Hz.
        static let sound16: [UInt8] = {
            let header = cat(be32(0), be32(2), be32(44100 << 16), be32(0), be32(0), [0xFF, 60], be32(8),
                             [UInt8](repeating: 0, count: 22), be16(16), [UInt8](repeating: 0, count: 14))
            let samples = (0..<16).flatMap { be16(1000 * $0 - 3000) }
            return cat(be16(2), be16(0), be16(1), be16(0x8000 | 81), be16(0), be32(14), header, samples)
        }()

        /// STR#: plain ASCII, and MacRoman (© and é) with a CR.
        static let strings = cat(be16(2), [5], Array("Hello".utf8), [7, 0xA9, 0x20, 0x43, 0x61, 0x66, 0x8E, 0x0D])

        static func resources() throws -> [InspectedResource] {
            var fork = buildFork([
                TestResource(type: "PICT", id: 128, name: "Splash/Screen", data: picture),
                TestResource(type: "ICN#", id: 128, data: icon),
                TestResource(type: "snd ", id: 1, data: sound8),
                TestResource(type: "snd ", id: 2, data: sound16),
                TestResource(type: "STR#", id: 128, data: strings),
                TestResource(type: "CODE", id: 1, data: [0x4E, 0x75]),
            ])
            return try fork.withUnsafeMutableBufferPointer {
                try inspectResources(fork: UnsafePointer($0.baseAddress), length: $0.count)
            }
        }

        static func le16(_ d: [UInt8], _ o: Int) -> Int { Int(d[o]) | Int(d[o + 1]) << 8 }
        static func le32(_ d: [UInt8], _ o: Int) -> Int { le16(d, o) | le16(d, o + 2) << 16 }

        /// A PNG of ours (stored deflate blocks): size, bytes per pixel and rows of pixels.
        static func pngPixels(_ png: [UInt8]) -> (w: Int, h: Int, bpp: Int, rows: [[UInt8]]) {
            #expect(Array(png[0..<8]) == [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
            let be = { (o: Int) in Int(png[o]) << 24 | Int(png[o + 1]) << 16 | Int(png[o + 2]) << 8 | Int(png[o + 3]) }
            let w = be(16), h = be(20), bpp = png[25] == 6 ? 4 : 3
            // The IDAT chunk follows IHDR: a zlib header, then stored blocks.
            var o = 8 + 25 + 8 + 2, raw = [UInt8]()
            while raw.count < (w * bpp + 1) * h {
                let n = le16(png, o + 1)
                raw += png[(o + 5)..<(o + 5 + n)]
                o += 5 + n
            }
            let rows = (0..<h).map { y in Array(raw[(y * (w * bpp + 1) + 1)..<((y + 1) * (w * bpp + 1))]) }
            return (w, h, bpp, rows)
        }

        func export(_ r: InspectedResource, raw: Bool = false) throws -> (name: String?, data: [UInt8]) {
            let dir = tempDir()
            let exporter = ResourceExporter(directory: dir.path, resources: try Self.resources())
            guard let name = try exporter.export(r, raw: raw) else { return (nil, []) }
            return (name, Array(try Data(contentsOf: dir.appendingPathComponent(name))))
        }

        func resource(_ type: String, _ id: Int16) throws -> InspectedResource {
            try #require(try Self.resources().first { $0.typeText == type && $0.id == id })
        }

        @Test func summaries() throws {
            let all = try Self.resources()
            #expect(all.count == 6)
            #expect(resourceSummary(try resource("PICT", 128)) == "20×10, version 2")
            #expect(resourceSummary(try resource("ICN#", 128)) == "32×32, 1-bit with mask")
            #expect(resourceSummary(try resource("snd ", 1)) == "format 1, 8-bit mono, 11025 Hz, 0.01 s")
            #expect(resourceSummary(try resource("snd ", 2)) == "format 2, 16-bit stereo, 44100 Hz, 0.00 s")
            #expect(resourceSummary(try resource("STR#", 128)) == "2 strings: \"Hello\", \"© Café \"")
            #expect(resourceSummary(try resource("CODE", 1)) == nil)
        }

        /// Pictures are drawn by QuickDraw at their frame's size.
        @Test func picturesBecomePNG() throws {
            machine_set_system_folder(testSystemFolder)
            let (name, png) = try export(try resource("PICT", 128))
            #expect(name == "PICT_128 Splash_Screen.png")
            let image = Self.pngPixels(png)
            #expect(image.w == 20 && image.h == 10 && image.bpp == 3)
            #expect(Array(image.rows[5][15..<18]) == [0xFF, 0, 0])      // red at 5,5
            #expect(Array(image.rows[1][15..<18]) == [0xFF, 0xFF, 0xFF]) // white above the rectangle
            #expect(Array(image.rows[5][45..<48]) == [0xFF, 0xFF, 0xFF]) // and beside it
        }

        /// ICN#: black pixels, and the mask as alpha.
        @Test func iconsBecomePNGWithAlpha() throws {
            let (name, png) = try export(try resource("ICN#", 128))
            #expect(name == "ICN#_128.png")
            let image = Self.pngPixels(png)
            #expect(image.w == 32 && image.h == 32 && image.bpp == 4)
            #expect(Array(image.rows[3][0..<4]) == [0, 0, 0, 255])   // inside the mask
            #expect(image.rows[3][20 * 4 + 3] == 0)                // outside it: transparent
        }

        /// 8-bit sound stays 8-bit (offset binary), at the header's rate.
        @Test func soundsBecomeWAV() throws {
            machine_set_system_folder(testSystemFolder)
            let (name, wav) = try export(try resource("snd ", 1))
            #expect(name == "snd_1.wav")
            #expect(String(decoding: wav[0..<4], as: UTF8.self) == "RIFF")
            #expect(String(decoding: wav[8..<16], as: UTF8.self) == "WAVEfmt ")
            #expect(Self.le16(wav, 20) == 1 && Self.le16(wav, 22) == 1)  // PCM, mono
            #expect(Self.le32(wav, 24) == 11025)
            #expect(Self.le16(wav, 34) == 8)
            #expect(String(decoding: wav[36..<40], as: UTF8.self) == "data" && Self.le32(wav, 40) == 100)
            #expect(wav[44] == 50 && wav[44 + 99] == 149)
            #expect(wav.count == 44 + 100)

            let (_, wav16) = try export(try resource("snd ", 2))
            #expect(Self.le16(wav16, 22) == 2 && Self.le32(wav16, 24) == 44100 && Self.le16(wav16, 34) == 16)
            #expect(Self.le32(wav16, 40) == 8 * 2 * 2)                    // 8 frames of two 16-bit samples
            #expect(Int16(truncatingIfNeeded: Self.le16(wav16, 44)) == -3000)
            #expect(Int16(truncatingIfNeeded: Self.le16(wav16, 44 + 2 * 15)) == 12000)
        }

        /// MacRoman strings as UTF-8 lines, CR as LF.
        @Test func stringsBecomeUTF8() throws {
            let (name, text) = try export(try resource("STR#", 128))
            #expect(name == "STR#_128.txt")
            #expect(String(decoding: text, as: UTF8.self) == "Hello\n© Café\n\n")
        }

        /// Kinds without a host format: raw only when asked for.
        @Test func othersOnlyRawWhenAsked() throws {
            let code = try resource("CODE", 1)
            #expect(try export(code).name == nil)
            let (name, data) = try export(code, raw: true)
            #expect(name == "CODE_1.bin" && data == [0x4E, 0x75])
        }
    }
}
