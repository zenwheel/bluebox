import CCore
import Foundation
import Testing
import Toolbox

/// Text and sprite tracks drawn by qt_native: hand-made movies with a
/// styled caption, and a sprite that moves between a key frame and an
/// override sample.
extension BlueBox {
    @Suite struct QTTextSprite {
        private func b16(_ v: Int) -> [UInt8] { [UInt8(v >> 8 & 0xFF), UInt8(v & 0xFF)] }
        private func b32(_ v: Int) -> [UInt8] { (0..<4).map { UInt8(v >> (24 - 8 * $0) & 0xFF) } }
        private func atom(_ type: String, _ body: [UInt8]) -> [UInt8] { b32(body.count + 8) + Array(type.utf8) + body }
        private func full(_ type: String, _ body: [UInt8]) -> [UInt8] { atom(type, [0, 0, 0, 0] + body) }
        private func matrix(tx: Int = 0, ty: Int = 0) -> [UInt8] {
            b32(0x10000) + b32(0) + b32(0) + b32(0) + b32(0x10000) + b32(0) + b32(tx << 16) + b32(ty << 16) + b32(0x4000_0000)
        }

        /// A track whose samples are at the given file offsets, one a chunk.
        private func trak(handler: String, width: Int, height: Int, desc: [UInt8], sizes: [Int], offsets: [Int],
                          duration: Int, keys: [Int]? = nil) -> [UInt8] {
            let tkhd = full("tkhd", [UInt8](repeating: 0, count: 28) + b16(0) + b16(0) + b16(0) + b16(0) + matrix()
                + b32(width << 16) + b32(height << 16))
            let mdhd = full("mdhd", b32(0) + b32(0) + b32(600) + b32(duration * sizes.count) + b32(0))
            let hdlr = full("hdlr", Array("mhlr".utf8) + Array(handler.utf8) + [UInt8](repeating: 0, count: 13))
            var stbl = full("stsd", b32(1) + desc) + full("stts", b32(1) + b32(sizes.count) + b32(duration))
            if let keys { stbl += full("stss", b32(keys.count) + keys.flatMap { b32($0) }) }
            stbl += full("stsc", b32(1) + b32(1) + b32(1) + b32(1))
                + full("stsz", b32(0) + b32(sizes.count) + sizes.flatMap { b32($0) })
                + full("stco", b32(offsets.count) + offsets.flatMap { b32($0) })
            var t = atom("trak", tkhd + atom("mdia", mdhd + hdlr + atom("minf", atom("stbl", stbl))))
            t[8 + 11] = 1 // tkhd flags: enabled
            return t
        }

        /// Open a movie from a 'moov' and its samples (the data file).
        private func open(_ moov: [UInt8], data: [UInt8]) throws -> (OpaquePointer, URL) {
            let url = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-qttext-\(UUID().uuidString).mov")
            try Data(data).write(to: url)
            let p = try #require(moov.withUnsafeBufferPointer {
                qt_native_open($0.baseAddress, UInt32($0.count), url.path, 600, nil, 0)
            })
            return (p, url)
        }

        private func pixel(_ rgba: UnsafeMutablePointer<UInt8>, _ w: Int32, _ x: Int, _ y: Int) -> [UInt8] {
            let i = (y * Int(w) + x) * 4
            return [rgba[i], rgba[i + 1], rgba[i + 2]]
        }

        @Test func styledCaption() throws {
            launchTestApp()
            _ = initToolbox()
            // TextDescription: no flags, left, black background, the box the
            // track, default style Geneva 12 white.
            let style = b32(0) + b16(16) + b16(12) + b16(3) + [0, 0] + b16(12) + b16(0xFFFF) + b16(0xFFFF) + b16(0xFFFF)
            var body = Array("text".utf8) + [UInt8](repeating: 0, count: 6) + b16(1) + b32(0) + b32(0)
            body += b16(0) + b16(0) + b16(0) + b16(0) + b16(0) + b16(30) + b16(100) + style + [6] + Array("Geneva".utf8)
            let desc = b32(body.count + 4) + body
            // "WWW www": the first three red and bold, the rest white.
            let text = Array("WWW www".utf8)
            var styl = b16(2)
            styl += b32(0) + b16(16) + b16(12) + b16(3) + [1, 0] + b16(12) + b16(0xFFFF) + b16(0) + b16(0)
            styl += b32(3) + b16(16) + b16(12) + b16(3) + [0, 0] + b16(12) + b16(0xFFFF) + b16(0xFFFF) + b16(0xFFFF)
            let sample = b16(text.count) + text + atom("styl", styl)
            let moov = atom("moov", trak(handler: "text", width: 100, height: 30, desc: desc, sizes: [sample.count],
                                         offsets: [0], duration: 600))
            let (p, url) = try open(moov, data: sample)
            defer { qt_native_close(p); try? FileManager.default.removeItem(at: url) }
            #expect(qt_native_has_video(p))

            var serial: UInt32 = 0, w: Int32 = 0, h: Int32 = 0
            var rgba: UnsafeMutablePointer<UInt8>?
            defer { free(rgba) }
            #expect(qt_native_frame(p, 100, &serial, &rgba, &w, &h))
            #expect(w == 100 && h == 30)
            var red = 0, white = 0, other = 0
            for y in 0..<Int(h) {
                for x in 0..<Int(w) {
                    let c = pixel(rgba!, w, x, y)
                    if c == [255, 0, 0] { red += 1; #expect(x < 50) }        // the red run comes first
                    else if c == [255, 255, 255] { white += 1; #expect(x > 25) }
                    else if c != [0, 0, 0] { other += 1 }
                }
            }
            #expect(red > 20 && white > 20 && other == 0)
            #expect(pixel(rgba!, w, 99, 29) == [0, 0, 0])                    // the background
            #expect(!qt_native_frame(p, 300, &serial, &rgba, &w, &h))        // the same sample
        }

        @Test func spriteMovesBetweenSamples() throws {
            // A 4x4 green 'raw ' 32-bit image.
            let image = [UInt8](repeating: 0, count: 0) + (0..<16).flatMap { _ in [255, 0, 255, 0] as [UInt8] }
            let idesc = b32(86) + Array("raw ".utf8) + b32(0) + b16(0) + b16(0) + b16(0) + b16(0) + Array("appl".utf8)
                + b32(0) + b32(0) + b16(4) + b16(4) + b32(72 << 16) + b32(72 << 16) + b32(image.count) + b16(1)
                + [UInt8](repeating: 0, count: 32) + b16(32) + b16(0xFFFF)
            func qtAtom(_ type: [UInt8], _ id: Int, _ children: [[UInt8]]? = nil, data: [UInt8] = []) -> [UInt8] {
                let body = children?.flatMap { $0 } ?? data
                return b32(20 + body.count) + type + b32(id) + b16(0) + b16(children?.count ?? 0) + b32(0) + body
            }
            func container(_ children: [[UInt8]]) -> [UInt8] {
                [UInt8](repeating: 0, count: 12) + qtAtom(Array("sean".utf8), 1, children)
            }
            let sprt = Array("sprt".utf8)
            let key = container([
                qtAtom(sprt, 1, [qtAtom(b32(1), 1, data: matrix(tx: 2, ty: 3)), qtAtom(b32(4), 1, data: b16(1)),
                                 qtAtom(b32(100), 1, data: b16(1))]),
                qtAtom(Array("dflt".utf8), 1, [qtAtom(Array("imct".utf8), 1, [
                    qtAtom(Array("imag".utf8), 1, [qtAtom(Array("imda".utf8), 1, data: idesc + image)]),
                ])]),
            ])
            let override = container([qtAtom(sprt, 1, [qtAtom(b32(1), 1, data: matrix(tx: 30, ty: 3))])])
            let desc = b32(16) + Array("sprt".utf8) + [UInt8](repeating: 0, count: 6) + b16(1)
            let moov = atom("moov", trak(handler: "sprt", width: 40, height: 10, desc: desc,
                                         sizes: [key.count, override.count], offsets: [0, key.count],
                                         duration: 60, keys: [1]))
            let (p, url) = try open(moov, data: key + override)
            defer { qt_native_close(p); try? FileManager.default.removeItem(at: url) }

            var serial: UInt32 = 0, w: Int32 = 0, h: Int32 = 0
            var rgba: UnsafeMutablePointer<UInt8>?
            defer { free(rgba) }
            #expect(qt_native_frame(p, 50, &serial, &rgba, &w, &h))         // the key frame
            #expect(w == 40 && h == 10)
            #expect(pixel(rgba!, w, 3, 4) == [0, 255, 0])
            #expect(pixel(rgba!, w, 31, 4) == [0, 0, 0])                     // the background: black
            #expect(pixel(rgba!, w, 1, 4) == [0, 0, 0] && pixel(rgba!, w, 6, 4) == [0, 0, 0])
            #expect(qt_native_frame(p, 150, &serial, &rgba, &w, &h))        // the override
            #expect(pixel(rgba!, w, 3, 4) == [0, 0, 0])
            #expect(pixel(rgba!, w, 31, 4) == [0, 255, 0] && pixel(rgba!, w, 33, 6) == [0, 255, 0])
            #expect(qt_native_frame(p, 20, &serial, &rgba, &w, &h))         // back to the key frame alone
            #expect(pixel(rgba!, w, 3, 4) == [0, 255, 0] && pixel(rgba!, w, 31, 4) == [0, 0, 0])
        }
    }
}
