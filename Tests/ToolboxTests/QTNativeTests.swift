import Foundation
import Testing
import Toolbox

/// Movies played by our own decoders: a hand-made movie with two raw
/// 32-bit frames and a second of 16-bit 'twos' sound in two chunks.
@Suite struct QTNative {
    private func be16(_ v: Int) -> [UInt8] { [UInt8(v >> 8 & 0xFF), UInt8(v & 0xFF)] }
    private func be32(_ v: Int) -> [UInt8] { (0..<4).map { UInt8(v >> (24 - 8 * $0) & 0xFF) } }
    private func atom(_ type: String, _ body: [UInt8]) -> [UInt8] { be32(body.count + 8) + Array(type.utf8) + body }
    private func full(_ type: String, _ body: [UInt8]) -> [UInt8] { atom(type, [0, 0, 0, 0] + body) }

    private func trak(handler: String, scale: Int, duration: Int, stsd: [UInt8], stts: [(Int, Int)],
                      stsc: [(Int, Int)], stsz: [UInt8], chunks: [Int]) -> [UInt8] {
        let tkhd = atom("tkhd", [0, 0, 0, 1] + [UInt8](repeating: 0, count: 80))
        let mdhd = full("mdhd", be32(0) + be32(0) + be32(scale) + be32(duration) + be32(0))
        let hdlr = full("hdlr", Array("mhlr".utf8) + Array(handler.utf8) + [UInt8](repeating: 0, count: 13))
        let stbl = atom("stbl", full("stsd", be32(1) + stsd)
            + full("stts", be32(stts.count) + stts.flatMap { be32($0.0) + be32($0.1) })
            + full("stsc", be32(stsc.count) + stsc.flatMap { be32($0.0) + be32($0.1) + be32(1) })
            + full("stsz", stsz)
            + full("stco", be32(chunks.count) + chunks.flatMap { be32($0) }))
        return atom("trak", tkhd + atom("mdia", mdhd + hdlr + atom("minf", stbl)))
    }

    @Test func framesAndSoundFollowTheClock() throws {
        // The data file: frame 1 (red), frame 2 (blue), then the sound's two chunks.
        let red: [UInt8] = [255, 255, 0, 0], blue: [UInt8] = [255, 0, 0, 255]
        var data = Array(repeating: red, count: 4).flatMap { $0 } + Array(repeating: blue, count: 4).flatMap { $0 }
        let soundAt = data.count
        for i in 0..<8000 { data += be16(i & 0x7FFF) }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-qtnative-\(getpid()).mov")
        try Data(data).write(to: url)
        defer { try? FileManager.default.removeItem(at: url) }

        // 'raw ' 2x2 at 32 bits, one frame a second; 'twos' 16-bit mono at 4000 Hz, half a second a chunk.
        let video = trak(handler: "vide", scale: 600, duration: 1200,
                         stsd: be32(86) + Array("raw ".utf8) + [0, 0, 0, 0, 0, 0] + be16(1) + [UInt8](repeating: 0, count: 16)
                             + be16(2) + be16(2) + be32(72 << 16) + be32(72 << 16) + be32(0) + be16(1)
                             + [UInt8](repeating: 0, count: 32) + be16(32) + be16(0xFFFF),
                         stts: [(2, 600)], stsc: [(1, 1)], stsz: be32(16) + be32(2), chunks: [0, 16])
        let sound = trak(handler: "soun", scale: 4000, duration: 4000,
                         stsd: be32(36) + Array("twos".utf8) + [0, 0, 0, 0, 0, 0] + be16(1) + be16(0) + be16(0) + be32(0)
                             + be16(1) + be16(16) + be16(0) + be16(0) + be32(4000 << 16),
                         stts: [(4000, 1)], stsc: [(1, 2000)], stsz: be32(1) + be32(4000),
                         chunks: [soundAt, soundAt + 4000])
        let moov = atom("moov", video + sound)

        let p = try #require(moov.withUnsafeBufferPointer {
            qt_native_open($0.baseAddress, UInt32($0.count), url.path, 600, nil, 0)
        })
        defer { qt_native_close(p) }
        #expect(qt_native_has_video(p) && qt_native_has_sound(p))

        var serial: UInt32 = 0, w: Int32 = 0, h: Int32 = 0
        var rgba: UnsafeMutablePointer<UInt8>?
        defer { free(rgba) }
        #expect(qt_native_frame(p, 500, &serial, &rgba, &w, &h))
        #expect(w == 2 && h == 2 && rgba![0] == 255 && rgba![2] == 0)            // red
        #expect(!qt_native_frame(p, 900, &serial, &rgba, &w, &h))                 // the same picture
        #expect(qt_native_frame(p, 1500, &serial, &rgba, &w, &h))
        #expect(rgba![0] == 0 && rgba![2] == 255)                                  // blue
        #expect(!qt_native_frame(p, 2000, &serial, &rgba, &w, &h))                // the end keeps the last

        // Two stretches across the chunk boundary join exactly.
        var pcm: UnsafeMutablePointer<Int16>?, channels: Int32 = 0, rate = 0.0
        let a = qt_native_sound(p, 0, 400, &pcm, &channels, &rate)
        #expect(a == 1600 && channels == 1 && rate == 4000)
        #expect(pcm![0] == 0 && pcm![1599] == 1599)
        free(pcm)
        let b = qt_native_sound(p, 400, 700, &pcm, &channels, &rate)
        #expect(b == 1200 && pcm![0] == 1600 && pcm![1199] == 2799)               // into the second chunk
        free(pcm)
    }
}
