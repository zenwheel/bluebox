import CCore
import ForkStore
import Foundation
import Testing
import Toolbox

/// A cicn (8-bit, every pixel shown) from a palette of RGB bytes and a pixel function giving the index.
func cicnColours(w: Int, h: Int, _ colours: [[UInt8]], _ pixel: (Int, Int) -> UInt8) -> [UInt8] {
    let rb = w, mrb = (w + 15) / 16 * 2
    var d = cat(be32(0), be16(0x8000 | rb), be16(0), be16(0), be16(h), be16(w), be16(0), be16(0), be32(0),
                be32(0x480000), be32(0x480000), be16(0), be16(8), be16(1), be16(8), be32(0), be32(0), be32(0))
    d += cat(be32(0), be16(mrb), be16(0), be16(0), be16(h), be16(w))   // mask BitMap
    d += cat(be32(0), be16(0), be16(0), be16(0), be16(h), be16(w))     // icon BitMap (no data)
    d += be32(0)                                                         // iconData
    d += [UInt8](repeating: 0xFF, count: mrb * h)                        // mask: all shown
    d += cat(be32(0), be16(0), be16(colours.count - 1))
    for (i, c) in colours.enumerated() {
        d += cat(be16(i), be16(Int(c[0]) * 0x101), be16(Int(c[1]) * 0x101), be16(Int(c[2]) * 0x101))
    }
    for y in 0..<h { for x in 0..<w { d.append(pixel(x, y)) } }
    return d
}

extension BlueBox {
    /// Kaleidoscope 1.x schemes: no 'wnd#', the System 7 look coloured by
    /// small icons that map its colours.
    @Suite struct Kaleidoscope1 {
        static let barFill: [UInt8] = [0x22, 0x44, 0x66], barText: [UInt8] = [0xFF, 0xEE, 0x00]
        static let barLight: [UInt8] = [0x88, 0x99, 0xAA], barShadow: [UInt8] = [0x11, 0x11, 0x11]
        static let menuFill: [UInt8] = [0x55, 0x00, 0x55], menuText: [UInt8] = [0x00, 0xFF, 0x00]
        static let menuLight: [UInt8] = [0xEE, 0xCC, 0xEE], menuDark: [UInt8] = [0x33, 0x00, 0x33]
        static let hiliteFill: [UInt8] = [0x66, 0x66, 0xCC], hiliteText: [UInt8] = [0xFF, 0xFF, 0xFF]

        /// An 8-by-4 menu map: light left column, dark right one, the text in the middle.
        static func menuMap(_ light: [UInt8], _ fill: [UInt8], _ text: [UInt8], _ dark: [UInt8]) -> [UInt8] {
            cicnColours(w: 8, h: 4, [light, fill, text, dark]) { x, y in
                x == 0 ? 0 : x == 7 ? 3 : (y == 1 || y == 2) && x >= 2 && x <= 5 ? 2 : 1
            }
        }

        static func scheme() -> URL {
            // racing stripes (which a scheme must have), the menu bar, a menu, its highlight
            let stripes = cicnColours(w: 16, h: 13, [[0xFF, 0xFF, 0xFF], [0x77, 0x77, 0x77]]) { _, y in UInt8(y & 1) }
            let bar = cicnColours(w: 20, h: 8, [barFill, barText, barLight, barShadow]) { _, y in
                y == 0 || y == 5 ? 2 : y == 2 ? 1 : y == 7 || y == 4 ? 3 : 0
            }
            var fork = buildFork([
                TestResource(type: "cicn", id: -14331, data: stripes),
                TestResource(type: "cicn", id: -12288, data: bar),
                TestResource(type: "cicn", id: -12272, data: menuMap(menuLight, menuFill, menuText, menuDark)),
                TestResource(type: "cicn", id: -12287,
                             data: menuMap([0x66, 0x66, 0xCC], [0x33, 0x33, 0x99], hiliteText, [0, 0, 0x88])),
            ])
            let url = tempDir().appendingPathComponent("Tiny K1 Scheme")
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(url.path, &f, &err, err.count))
            return url
        }

        func screen(_ x: Int32, _ y: Int32) -> [UInt8] {
            var px = [UInt8](repeating: 0, count: 4)
            machine_screen_rgba(x, y, 1, 1, &px)
            return Array(px[0..<3])
        }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// Whether any pixel of a rectangle of the screen is the colour.
        func any(_ c: [UInt8], _ x0: Int32, _ y0: Int32, _ x1: Int32, _ y1: Int32) -> Bool {
            for y in y0..<y1 { for x in x0..<x1 where screen(x, y) == c { return true } }
            return false
        }

        @Test func menusTakeTheSchemesColours() {
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_load_theme(Self.scheme().path, &err, err.count))
            defer { machine_set_appearance(Int32(MACHINE_APPEARANCE_BW)) }
            launchTestApp()
            _ = initToolbox()
            let file = tb_NewMenu(128, pstr("File"))
            tb_AppendMenu(file, pstr("New;Open;(-;Quit"))
            tb_InsertMenu(file, 0)
            tb_DrawMenuBar()
            // the bar: its fill, the highlight along the top and the shadow along the bottom
            #expect(screen(500, 10) == Self.barFill)
            #expect(screen(500, 0) == Self.barLight)
            #expect(screen(500, 19) == Self.barShadow)
            #expect(any(Self.barText, 10, 2, 60, 18))                        // the title's text
            tb_HiliteMenu(128)
            #expect(screen(12, 10) == [0x33, 0x33, 0x99])                     // selected: the highlight's fill
            #expect(any(Self.hiliteText, 10, 2, 60, 18))
            tb_HiliteMenu(0)
            #expect(screen(12, 10) == Self.barFill)

            // the menu pulled down, its first item selected
            tb_SetPort(rd32(UInt32(LM_WMgrPort)))
            let rect = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 164, 200].enumerated() { wr16(rect + UInt32(i * 2), UInt16(v)) }
            let which = tb_NewPtr(2, false, true)
            menu_standard_mdef(0, file, rect, 0, which)                       // mDrawMsg
            #expect(screen(190, 120) == Self.menuFill)
            #expect(screen(100, 120) == Self.menuLight)
            #expect(screen(199, 120) == Self.menuDark)
            #expect(any(Self.menuText, 115, 116, 160, 132))                   // "Open"
            #expect(screen(150, 139) == Self.barShadow && screen(150, 140) == Self.barLight) // the divider
            menu_standard_mdef(1, file, rect, 0x0068_0096, which)             // mChooseMsg at (104, 150): New
            #expect(rd16(which) == 1)
            #expect(screen(190, 104) == [0x33, 0x33, 0x99])
            #expect(any(Self.hiliteText, 115, 100, 160, 116))
        }
    }
}
