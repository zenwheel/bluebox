import CCore
import Foundation
import Testing
import Toolbox

/// Wired sprites: a sprite track's event handlers run on clicks, frames
/// shown and idle time, with expressions, variables and conditions.
extension BlueBox {
    @Suite struct QTWired {
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

        func qtAtom(_ type: [UInt8], _ id: Int, _ children: [[UInt8]]? = nil, data: [UInt8] = []) -> [UInt8] {
            let body = children?.flatMap { $0 } ?? data
            return b32(20 + body.count) + type + b32(id) + b16(0) + b16(children?.count ?? 0) + b32(0) + body
        }
        func t(_ s: String) -> [UInt8] { Array(s.utf8) }
        func float(_ v: Float) -> [UInt8] { b32(Int(v.bitPattern)) }
        /// An action: which, then its parameters.
        func action(_ id: Int, _ which: Int, _ params: [[UInt8]], target: [UInt8]? = nil) -> [UInt8] {
            var kids = [qtAtom(t("whic"), 1, data: b32(which))]
            for (i, p) in params.enumerated() { kids.append(p.count > 4 && Array(p[4..<8]) == t("expr") ? qtAtom(t("parm"), i + 1, [p]) : qtAtom(t("parm"), i + 1, data: p)) }
            if let target { kids.append(qtAtom(t("targ"), 1, [target])) }
            return qtAtom(t("actn"), id, kids)
        }
        func constant(_ v: Float) -> [UInt8] { qtAtom(t("oprn"), 1, [qtAtom(b32(2), 1, data: float(v))]) }
        func variable(_ id: Int) -> [UInt8] { qtAtom(t("oprn"), 1, [qtAtom(b32(3079), 1, [qtAtom(t("parm"), 1, data: b32(id))])]) }
        func expr(_ op: String, _ a: [UInt8], _ b: [UInt8]) -> [UInt8] {
            var a2 = a, b2 = b
            a2.replaceSubrange(8..<12, with: b32(1)); b2.replaceSubrange(8..<12, with: b32(2))
            return qtAtom(t("expr"), 1, [qtAtom(t("oper"), Int(t(op).reduce(0) { $0 << 8 | UInt32($1) }), [a2, b2])])
        }

        @Test func clicksFramesAndExpressions() throws {
            qtMachine()
            let image = (0..<16).flatMap { _ in [255, 0, 255, 0] as [UInt8] }
            let idesc = b32(86) + t("raw ") + b32(0) + b16(0) + b16(0) + b16(0) + b16(0) + t("appl")
                + b32(0) + b32(0) + b16(4) + b16(4) + b32(72 << 16) + b32(72 << 16) + b32(image.count) + b16(1)
                + [UInt8](repeating: 0, count: 32) + b16(32) + b16(0xFFFF)
            // sprite 1 at (2, 3); a click sets variable 5 to 3 + 4, moves it by 10 and, in a case
            // on variable 5 = 7, hides it; the frame's handler sets variable 9 to 1
            let caseTest = qtAtom(t("test"), 1, [expr("=   ", variable(5), constant(7)),
                                                  qtAtom(t("list"), 1, [action(1, 3074, [b16(0)])])])
            let click = qtAtom(t("evnt"), Int(fourCC("clik")), [
                action(1, 7168, [b32(5), expr("add ", constant(3), constant(4))]),
                action(2, 3080, [b32(10 << 16), b32(0), [1]]),
                qtAtom(t("actn"), 3, [qtAtom(t("whic"), 1, data: b32(6144)), qtAtom(t("parm"), 1, [caseTest])]),
            ])
            let frame = qtAtom(t("evnt"), Int(fourCC("fram")), [action(1, 7168, [b32(9), float(1)])])
            let key = [UInt8](repeating: 0, count: 12) + qtAtom(t("sean"), 1, [
                qtAtom(t("sprt"), 1, [qtAtom(b32(1), 1, data: matrix(tx: 2, ty: 3)), qtAtom(b32(4), 1, data: b16(1)),
                                      qtAtom(b32(100), 1, data: b16(1)), click]),
                qtAtom(t("dflt"), 1, [qtAtom(t("imct"), 1, [
                    qtAtom(t("imag"), 1, [qtAtom(t("imda"), 1, data: idesc + image)]),
                ])]),
                frame,
            ])
            let desc = b32(16) + t("sprt") + [UInt8](repeating: 0, count: 6) + b16(1)
            let mvhd = full("mvhd", b32(0) + b32(0) + b32(600) + b32(600) + [UInt8](repeating: 0, count: 80))
            let moov = atom("moov", mvhd + trak(handler: "sprt", width: 40, height: 10, desc: desc, sizes: [key.count],
                                         offsets: [0], duration: 600, keys: [1]))
            let (p, url) = try open(moov, data: key)
            defer { qt_native_close(p); try? FileManager.default.removeItem(at: url) }
            var serial: UInt32 = 0, w: Int32 = 0, h: Int32 = 0
            var rgba: UnsafeMutablePointer<UInt8>?
            defer { free(rgba) }
            #expect(qt_native_frame(p, 50, &serial, &rgba, &w, &h))
            let mv = try #require(moov.withUnsafeBufferPointer { qtm_parse($0.baseAddress, UInt32($0.count)) })
            defer { qtm_free(mv) }
            let s = try #require(qt_native_sprite(p, 0))
            let box = qrect(top: 0, left: 0, bottom: 10, right: 40)
            // the frame shown: its handler
            qt_wired_task(mv, 0, p, 0, box)
            #expect(qt_sprite_var(s, 9) == 1)
            // a click beside the sprite: nothing; on it: its handler
            #expect(!qt_wired_click(mv, 0, p, box, 20, 4))
            #expect(qt_wired_click(mv, 0, p, box, 3, 4))
            #expect(qt_sprite_var(s, 5) == 7)
            var m = [UInt8](repeating: 0, count: 36)
            var n: UInt32 = 0
            #expect(qt_sprite_get(s, 1, 1, &m, &n) && Array(m[24..<28]) == b32(12 << 16))
            #expect(qt_sprite_get(s, 1, 4, &m, &n) && m[1] == 0)
        }
    }
}
