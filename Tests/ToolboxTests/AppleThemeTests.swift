import CCore
import ForkStore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Apple's own theme files ('tthm', Apple platinum): only scenes and
    /// colour variations, the look drawn by thme_platinum.c.
    @Suite(.serialized) struct AppleThemeFiles {
        typealias T = ThemeFiles

        /// A theme with its owner resource, a scene naming the variation
        /// "Rust" and that 'clut', a ramp of eight.
        static func fork() -> [UInt8] {
            let pstr = { (t: String) in [UInt8(t.utf8.count)] + Array(t.utf8) }
            let scene = T.collection([("name", pstr("Default")), ("varn", pstr("Rust"))])
            var clut = T.be32(0) + T.be16(0) + T.be16(7)
            for i in 0..<8 {
                clut += T.be16(i) + T.be16(0x1111 * (8 - i)) + T.be16(0x1010 * i) + T.be16(0x0808)
            }
            let res: [(String, Int, String?, [UInt8])] = [("tthm", 0, nil, [0]), ("scen", 2000, "Default", scene),
                                                           ("clut", 200, "Azul", T.be32(0) + T.be16(0) + T.be16(0)),
                                                           ("clut", 201, "Rust", clut)]
            var datas: [UnsafeMutablePointer<UInt8>] = []
            var names: [UnsafeMutablePointer<CChar>] = []
            var specs: [rsrc_spec] = []
            for (type, id, name, bytes) in res {
                let p = UnsafeMutablePointer<UInt8>.allocate(capacity: bytes.count)
                p.initialize(from: bytes, count: bytes.count)
                datas.append(p)
                let n = name.map { strdup($0)! }
                if let n { names.append(n) }
                specs.append(rsrc_spec(type: fourCC(type), id: Int16(id), name: n.map { UnsafePointer($0) }, attrs: 0,
                                       data: p, len: UInt32(bytes.count), name_len: 0))
            }
            var out: UnsafeMutablePointer<UInt8>?
            var len = 0
            #expect(rsrc_build(specs, specs.count, &out, &len))
            let fork = Array(UnsafeBufferPointer(start: out, count: len))
            free(out)
            datas.forEach { $0.deallocate() }
            names.forEach { free($0) }
            return fork
        }

        func load() {
            var f = Self.fork()
            var err = [CChar](repeating: 0, count: 256)
            #expect(thme_fork_is_theme(&f, f.count))
            #expect(thme_load("Apple platinum", &f, f.count, &err, err.count))
        }

        let content = qrect(top: 40, left: 20, bottom: 100, right: 120)

        func render(_ id: Int32, state: UInt32, title: Int = 0) -> [UInt8] {
            var m = [Int32](repeating: 0, count: Int(THME_MEASURES))
            m[1] = Int32(title)
            m[2] = Int32(title / 2)
            // a white background, for the shadow to darken
            var buf = [UInt8](repeating: 0xFF, count: 200 * 150 * 4)
            thme_render_part(&buf, 200, 150, id, content, state, &m, 0)
            return buf
        }

        func px(_ b: [UInt8], _ x: Int, _ y: Int) -> [UInt8] { Array(b[(y * 200 + x) * 4 ..< (y * 200 + x) * 4 + 3]) }

        @Test func platinumWindowsAreDrawn() {
            load()
            defer { thme_unload() }
            #expect(thme_has_part(128) && thme_has_part(129) && thme_has_part(257))
            // close box, zoom and collapse, titled
            let b = render(128, state: 0x20 | 0x18 | 0x40 | 0x80, title: 30)
            // the outer line 22 above and 6 left of the content, then the slab's light ring
            #expect(px(b, 20 - 6, 40 - 22) == [0x10, 0x10, 0x10])
            #expect(px(b, 40, 40 - 22) == [0x40, 0x40, 0x40])
            #expect(px(b, 40, 40 - 21) == [0xF2, 0xF2, 0xF2])
            // the title bar's grey, the line round the content
            #expect(px(b, 40, 40 - 5) == [0xCC, 0xCC, 0xCC])
            #expect(px(b, 40, 40 - 1) == [0x40, 0x40, 0x40])
            #expect(px(b, 20 - 1, 60) == [0x40, 0x40, 0x40])
            // the content is left alone, the shadow darkens what was there
            #expect(px(b, 60, 60) == [0xFF, 0xFF, 0xFF])
            let shade = px(b, 120 + 6, 60)
            #expect(shade[0] > 0x20 && shade[0] < 0x60)
            var m = [Int32](repeating: 0, count: Int(THME_MEASURES))
            var r = qrect()
            #expect(thme_part_rect(128, content, 0x20 | 0x80, &m, UInt32(THME_CLOSE), &r))
            #expect(r.left == 17 && r.top == 21 && r.right - r.left == 14)
            #expect(thme_part_hit(128, content, 0x20, &m, 0xFF, 20, 25) & UInt32(THME_CLOSE) != 0)
            // an inactive window has no boxes
            #expect(!thme_part_rect(129, content, 0x80, &m, UInt32(THME_CLOSE), &r))
        }

        @Test func theSceneNamesTheVariation() {
            load()
            defer { thme_unload() }
            var rgb = [UInt16](repeating: 0, count: 3)
            #expect(thme_variation_rgb(3, &rgb))
            #expect(rgb == [0x5555, 0x3030, 0x0808])
            var h: Int32 = 0
            #expect(thme_metric("msht", &h) && h == 6)
            #expect(thme_metric("mbht", &h) && h == 20)
        }
    }
}
