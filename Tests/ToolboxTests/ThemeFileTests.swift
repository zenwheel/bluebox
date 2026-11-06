import CCore
import ForkStore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Mac OS 8.5 Appearance theme files: a synthetic theme's layout,
    /// images, fills and colours, read and drawn by thme.c.
    @Suite(.serialized) struct ThemeFiles {
        static func be16(_ v: Int) -> [UInt8] { [UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }
        static func be32(_ v: Int) -> [UInt8] { be16(v >> 16) + be16(v) }

        /// A coordinate {kind, value, ref, op, param, ref2, 0, 0}.
        static func coord(_ kind: Int, _ val: Int, _ ref: Int, op: Int = 0, param: Int = 0, ref2: Int = 0) -> [UInt8] {
            [kind, val, ref, op, param, ref2, 0, 0].flatMap { be16($0) }
        }

        /// A record: state bits needed and forbidden, flags, rectangle refs,
        /// what it draws, anchor and image index.
        static func record(need: Int = 0, forbid: Int = 0, flags: Int, _ rc: [Int], _ what: String, id: Int = 0,
                           anchor: Int = 1, index: Int = 0) -> [UInt8] {
            be32(need) + be32(forbid) + be16(0) + be16(flags) + rc.flatMap { be16($0) } + be32(0) +
                Array(what.utf8) + be16(id) + be16(anchor) + be16(index) + [UInt8](repeating: 0, count: 6)
        }

        /// A flattened collection ('tinf', 'scen'): its items by tag, each
        /// a length and the bytes.
        static func collection(_ items: [(String, [UInt8])]) -> [UInt8] {
            var entries: [UInt8] = []
            var data: [UInt8] = []
            for (tag, bytes) in items {
                entries += Array(tag.utf8) + be32(0) + be32(0) + be32(data.count)
                data += be32(bytes.count) + bytes
            }
            return be32(1) + be32(0) + be32(items.count) + entries + data
        }

        /// The theme: a document window whose frame is a green fill four
        /// pixels round the content (twenty above it), a 4x4 close box (red,
        /// blue when pressed, its top left pixel transparent) and a title
        /// text area as wide as the title, centred.
        static func fork() -> [UInt8] {
            let coords: [[UInt8]] = [
                coord(0, -20, -1),                         // c1 top - 20
                coord(0, -4, -2),                          // c2 left - 4
                coord(0, 4, -4),                           // c3 right + 4
                coord(0, 4, -3),                           // c4 bottom + 4
                coord(0, 2, -2),                           // c5 left + 2 (unused)
                coord(0, 2, 1),                            // c6 c1 + 2
                coord(0, 4, 6),                            // c7 c6 + 4
                coord(0, 4, -2),                           // c8 left + 4
                coord(2, 0, -2, ref2: -4),                 // c9 the middle
                coord(1, -2, 9, op: 1, ref2: -2),          // c10 middle - half the title, not left of left
                coord(1, 2, 9, op: 1, ref2: -4),           // c11 middle + half the title, not right of right
            ]
            let records: [[UInt8]] = [
                record(flags: 0x0120, [1, 2, 4, 3], "plut", id: -1),
                record(flags: 0x0300, [-1, -2, -3, -4], "NULL"),
                record(need: 0x20, flags: 0x0125, [6, -2, 7, 8], "pxm#", id: 200),
                record(need: 0x80, flags: 0x0123, [1, 10, -1, 11], "NULL"),
                record(flags: 0x0021, [1, 2, -1, 3], "NULL"),
            ]
            let layo = be32(0) + be32(coords.count) + coords.flatMap { $0 } + be16(records.count) + records.flatMap { $0 }
            // clut 2000: 0 red, 1 blue.
            let clut = be32(0) + be16(0) + be16(1) + be16(0) + be16(0xFFFF) + be16(0) + be16(0) +
                be16(1) + be16(0) + be16(0) + be16(0xFFFF)
            // pxm# 200: one mask for two 4x4 8-bit images.
            var pxm = be16(1) + be16(1) + be16(0) + be16(0) + be16(4) + be16(4) + be16(8) + be16(8) + be32(0) +
                be16(2000) + be16(2)
            pxm += [0x70, 0, 0xF0, 0, 0xF0, 0, 0xF0, 0]
            pxm += [UInt8](repeating: 0, count: 16) + [UInt8](repeating: 1, count: 16)
            // plut 387: -1 green, -2 grey; plut 386: brush 1; clr# 384: text colour 1.
            let fills = be32(8) + be16(42) + be16(2) + be16(0) + be16(0) + be16(0xFFFF) + be16(0) +
                be16(0) + be16(0x8080) + be16(0x8080) + be16(0x8080)
            let brushes = be32(8) + be16(42) + be16(1) + be16(0) + be16(0x1212) + be16(0x5656) + be16(0x9A9A)
            let colors = be32(1) + be16(0x1111) + be16(0x2222) + be16(0x3333)
            // frme 520: one ring a pixel outside the rectangle, green all round,
            // the inside grey.
            let frame = be16(2) + be16(0) + [0, 0xFF, 0, 0xFF] + be16(-2) + be16(1) + [0, 0xFF] + be16(-1) +
                [0xFF, 0xFF, 0xFF, 0xFF]
            // layo 257, a pulled-down menu: a pixel round it above and left,
            // three below and right (a shadow).
            let menuCoords = [coord(0, -1, -1), coord(0, -1, -2), coord(0, 3, -3), coord(0, 3, -4)]
            let menu = be32(0) + be32(menuCoords.count) + menuCoords.flatMap { $0 } + be16(1) +
                record(flags: 0x0100, [1, 2, 3, 4], "plut", id: -1)
            let tinf = collection([("vers", be16(1)), ("mbht", be16(22)), ("lvth", be16(15))])
            let pstr = { (t: String) in [UInt8(t.utf8.count)] + Array(t.utf8) }
            let scene = collection([("lgsf", pstr("Gadget")), ("smsf", pstr("Monaco")), ("vfnt", pstr("Chicago")),
                                    ("vfsz", be16(12)), ("hcol", be16(0xCCCC) + be16(0x9999) + be16(0x6666)),
                                    ("sbar", be16(2)), ("smoo", [1, 0]), ("sndt", pstr("Synthetic Sounds"))])
            // layo 510 and 511, check boxes: green, and grey right to left.
            let box = { (fill: Int) in be32(0) + be32(0) + be16(1) + record(flags: 0x0100, [-1, -2, -3, -4], "plut", id: fill) }
            // tic# 384: the trash is the theme's ICN# -3993 (a 32x32 square).
            let tic = be32(2) + Array("trsh".utf8) + be16(-3993) + Array("fldr".utf8) + be16(-3999)
            let trash = [UInt8](repeating: 0xFF, count: 256)
            let res: [(String, Int, [UInt8])] = [("layo", 128, layo), ("tinf", 128, tinf), ("scen", 128, scene),
                                                 ("tic#", 384, tic), ("ICN#", -3993, trash),
                                                 ("layo", 510, box(-1)), ("layo", 511, box(-2)), ("layo", 257, menu), ("clut", 2000, clut), ("pxm#", 200, pxm),
                                                 ("frme", 520, frame),
                                                 ("plut", 387, fills), ("plut", 386, brushes), ("clr#", 384, colors)]
            var datas: [UnsafeMutablePointer<UInt8>] = []
            var specs: [rsrc_spec] = []
            for (type, id, bytes) in res {
                let p = UnsafeMutablePointer<UInt8>.allocate(capacity: bytes.count)
                p.initialize(from: bytes, count: bytes.count)
                datas.append(p)
                specs.append(rsrc_spec(type: fourCC(type), id: Int16(id), name: nil, attrs: 0, data: p,
                                       len: UInt32(bytes.count), name_len: 0))
            }
            var out: UnsafeMutablePointer<UInt8>?
            var len = 0
            #expect(rsrc_build(specs, specs.count, &out, &len))
            let fork = Array(UnsafeBufferPointer(start: out, count: len))
            bb_free(out)
            datas.forEach { $0.deallocate() }
            return fork
        }

        func load() {
            var f = Self.fork()
            var err = [CChar](repeating: 0, count: 256)
            #expect(thme_fork_is_theme(&f, f.count))
            #expect(thme_load("Synthetic", &f, f.count, &err, err.count))
        }

        let content = qrect(top: 40, left: 20, bottom: 100, right: 120)

        func render(state: UInt32, title: Int = 20, pressed: UInt32 = 0) -> [UInt8] {
            var m = [Int32](repeating: 0, count: Int(THME_MEASURES))
            m[1] = Int32(title)
            m[2] = Int32(title / 2)
            var buf = [UInt8](repeating: 0, count: 200 * 150 * 4)
            thme_render_part(&buf, 200, 150, 128, content, state, &m, pressed)
            return buf
        }

        func px(_ b: [UInt8], _ x: Int, _ y: Int) -> [UInt8] { Array(b[(y * 200 + x) * 4 ..< (y * 200 + x) * 4 + 3]) }

        @Test func partsDrawFromTheLayout() {
            load()
            defer { thme_unload() }
            let b = render(state: 0xA0)
            #expect(px(b, 17, 30) == [0, 255, 0])       // the frame's fill
            #expect(px(b, 15, 30) == [0, 0, 0])         // outside it
            #expect(px(b, 21, 22) == [255, 0, 0])       // the close box
            #expect(px(b, 20, 22) == [0, 255, 0])       // its transparent corner
            #expect(px(render(state: 0xA0, pressed: UInt32(THME_CLOSE)), 21, 22) == [0, 0, 255])
            #expect(px(render(state: 0x80), 21, 22) == [0, 255, 0]) // no close box asked for
        }

        @Test func coordinatesTakeMeasuresAndLimits() {
            load()
            defer { thme_unload() }
            var m = [Int32](repeating: 0, count: Int(THME_MEASURES))
            var r = qrect()
            m[2] = 10
            #expect(thme_part_rect(128, content, 0xA0, &m, UInt32(THME_TITLE_TEXT), &r))
            #expect(r.top == 20 && r.left == 60 && r.bottom == 40 && r.right == 80)
            m[2] = 100 // wider than the window: held at its edges
            #expect(thme_part_rect(128, content, 0xA0, &m, UInt32(THME_TITLE_TEXT), &r))
            #expect(r.left == 20 && r.right == 120)
        }

        @Test func hitsAndRegions() {
            load()
            defer { thme_unload() }
            var m = [Int32](repeating: 0, count: Int(THME_MEASURES))
            m[2] = 10
            #expect(thme_part_hit(128, content, 0xA0, &m, 0xFF, 21, 23) & UInt32(THME_CLOSE) != 0)
            #expect(thme_part_hit(128, content, 0xA0, &m, 0xFF, 70, 30) & 0x03 == 0x03) // title bar and text
            #expect(thme_part_hit(128, content, 0xA0, &m, 0xFF, 70, 120) == 0)
            var g = hrgn()
            hrgn_init(&g)
            #expect(thme_part_region(128, content, 0xA0, &m, UInt32(THME_CONTENT), &g))
            #expect(hrgn_contains(&g, 17, 30) && hrgn_contains(&g, 60, 60))
            #expect(!hrgn_contains(&g, 10, 10) && !hrgn_contains(&g, 125, 60))
            hrgn_free(&g)
        }

        @Test func brushesAndTextColours() {
            load()
            defer { thme_unload() }
            var c: [UInt16] = [0, 0, 0]
            #expect(thme_brush_rgb(1, &c) && c == [0x1212, 0x5656, 0x9A9A])
            #expect(!thme_brush_rgb(2, &c))
            #expect(thme_text_rgb(1, &c) && c == [0x1111, 0x2222, 0x3333])
        }

        /// The theme's metrics, and the menu bar's height from them.
        @Test func metrics() {
            load()
            defer {
                thme_unload()
                machine_set_appearance(Int32(MACHINE_APPEARANCE_BW))
            }
            var v: Int32 = 0
            #expect(thme_metric("mbht", &v) && v == 22)
            #expect(thme_metric("lvth", &v) && v == 15)
            #expect(!thme_metric("msht", &v))
            #expect(machine_menu_bar_height() == 20) // not while the appearance is another
            machine_set_appearance(Int32(MACHINE_APPEARANCE_THEME))
            #expect(machine_menu_bar_height() == 22)
        }

        /// The scene: fonts (those installed), the highlight colour, settings.
        @Test func scene() {
            load()
            defer { thme_unload() }
            var name = [CChar](repeating: 0, count: 64)
            #expect(thme_scene_font("lgsf", &name, name.count) == -1) // Gadget isn't here
            #expect(thme_system_family() == 2002)                      // so Charcoal stays
            #expect(thme_scene_font("smsf", &name, name.count) == 4 && String(cString: name) == "Monaco")
            #expect(thme_scene_font("vfnt", &name, name.count) == 0 && String(cString: name) == "Chicago")
            var c: [UInt16] = [0, 0, 0]
            #expect(thme_highlight_rgb(&c) && c == [0xCCCC, 0x9999, 0x6666])
            var v: Int32 = 0
            var b = false
            #expect(thme_scene_int("sbar", &v) && v == 2)
            #expect(thme_scene_int("vfsz", &v) && v == 12)
            #expect(thme_scene_bool("smoo", &b) && b)
            #expect(machine_theme_sound_set_name(&name, name.count) && String(cString: name) == "Synthetic Sounds")
        }

        /// The theme's icons stand in for the System's ('tic#'), through
        /// Icon Services and the Icon Utilities; one it lists but hasn't
        /// is the System's.
        @Test func icons() {
            launchTestApp()
            _ = initToolbox()
            load()
            machine_set_appearance(Int32(MACHINE_APPEARANCE_THEME))
            iconservices_reset()
            defer {
                thme_unload()
                machine_set_appearance(Int32(MACHINE_APPEARANCE_BW))
                iconservices_reset()
            }
            var id: Int16 = 0
            #expect(thme_icon_id(fourCC("trsh"), &id) && id == -3993)
            #expect(!thme_icon_id(fourCC("fldr"), &id)) // listed, but no icon of its own
            #expect(thme_icon_listed(-3993) && !thme_icon_listed(-3999))
            let ref = tb_NewPtr(4, false, true), fam = tb_NewPtr(4, false, true), h = tb_NewHandle(0, false, false)
            #expect(tb_GetIconRef(0, Int32(bitPattern: fourCC("macs")), Int32(bitPattern: fourCC("trsh")), ref) == 0)
            #expect(tb_IconRefToIconFamily(rd32(ref), 0xFFFF_FFFF, fam) == 0)
            #expect(tb_GetIconFamilyData(rd32(fam), Int32(bitPattern: fourCC("ICN#")), h) == 0)
            #expect(tb_GetHandleSize(h) == 256 && rd32(rd32(h)) == 0xFFFF_FFFF)
            let h8 = tb_NewHandle(0, false, false)
            _ = tb_GetIconFamilyData(rd32(fam), Int32(bitPattern: fourCC("icl8")), h8)
            #expect(tb_GetHandleSize(h8) == 0) // not the System's
            let suite = tb_NewPtr(4, false, true)
            #expect(tb_GetIconSuite(suite, -3993, 0xFFFF_FFFF) == 0)
            let icn = tb_NewPtr(4, false, true)
            #expect(tb_GetIconFromSuite(icn, rd32(suite), Int32(bitPattern: fourCC("ICN#"))) == 0)
            #expect(rd32(rd32(rd32(icn))) == 0xFFFF_FFFF)
        }

        /// A menu's background covers what its layout draws round it.
        @Test func menuExtent() {
            load()
            defer { thme_unload() }
            var e = qrect()
            #expect(thme_menu_extent(qrect(top: 20, left: 10, bottom: 60, right: 50), 1, &e)) // pop-up: the pull-down's
            #expect(e.top == 19 && e.left == 9 && e.bottom == 63 && e.right == 53)
        }

        /// A sound set: 'snd#' maps a ThemeSoundKind to the first 'snd ' of
        /// its entry that is there, with the entry's class.
        @Test func soundSets() {
            let entry = Array("wcls".utf8) + Self.be16(1) + Self.be16(5) + Self.be16(0) + Self.be16(0) +
                Self.be16(999) + Self.be16(0) + Self.be32(0) + Self.be16(130) + Self.be16(0x9B) + Self.be32(0) +
                [UInt8](repeating: 0, count: 16)
            let table = Self.be16(1) + Self.be16(1) + entry
            let snd: [UInt8] = Self.be16(1) + [1, 2, 3, 4]
            var specs: [rsrc_spec] = []
            let datas = [table, snd].map { bytes -> UnsafeMutablePointer<UInt8> in
                let p = UnsafeMutablePointer<UInt8>.allocate(capacity: bytes.count)
                p.initialize(from: bytes, count: bytes.count)
                return p
            }
            specs.append(rsrc_spec(type: fourCC("snd#"), id: 1000, name: nil, attrs: 0, data: datas[0], len: UInt32(table.count), name_len: 0))
            specs.append(rsrc_spec(type: fourCC("snd "), id: 130, name: nil, attrs: 0, data: datas[1], len: UInt32(snd.count), name_len: 0))
            var out: UnsafeMutablePointer<UInt8>?
            var len = 0
            #expect(rsrc_build(specs, specs.count, &out, &len))
            datas.forEach { $0.deallocate() }
            var err = [CChar](repeating: 0, count: 256)
            #expect(thme_load_sounds("Synthetic Sounds", out, len, &err, err.count))
            bb_free(out)
            defer { thme_unload_sounds() }
            #expect(String(cString: thme_sound_set()) == "Synthetic Sounds")
            var cls: Int32 = 0
            var id: Int16 = 0
            var n: UInt32 = 0
            let d = thme_sound(fourCC("wcls"), &cls, &id, &n)
            #expect(d != nil && cls == 1 && id == 130 && n == UInt32(snd.count)) // 999 isn't there; 130 is
            #expect(thme_sound(fourCC("wopn"), &cls, &id, &n) == nil)
        }

        /// Drawn into a port, through its clip.
        @Test func drawsIntoThePort() {
            launchTestApp()
            _ = initToolbox()
            load()
            defer { thme_unload() }
            let gw = tb_NewPtr(4, false, true)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 150, 200].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            #expect(tb_NewGWorld(gw, 32, r, 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            _ = tb_LockPixels(tb_GetGWorldPixMap(rd32(gw)))
            tb_EraseRect(r)
            var m = [Int32](repeating: 0, count: Int(THME_MEASURES))
            #expect(thme_draw_part(128, content, 0xA0, &m, 0))
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(17, 30, c)
            #expect(rd16(c) == 0 && rd16(c + 2) == 0xFFFF && rd16(c + 4) == 0)
            tb_GetCPixel(21, 22, c)
            #expect(rd16(c) == 0xFFFF && rd16(c + 2) == 0 && rd16(c + 4) == 0)
            // A frame: its ring outside the rectangle; its fill is not drawn
            // inside (a field's, left as it is, as Mac OS 9 leaves edit text).
            #expect(thme_frame(520, qrect(top: 120, left: 130, bottom: 140, right: 170)))
            tb_GetCPixel(129, 119, c)
            #expect(rd16(c) == 0 && rd16(c + 2) == 0xFFFF && rd16(c + 4) == 0)
            tb_GetCPixel(150, 140, c)
            #expect(rd16(c) == 0 && rd16(c + 2) == 0xFFFF && rd16(c + 4) == 0)
            tb_GetCPixel(150, 130, c)
            #expect(rd16(c) == 0xFFFF && rd16(c + 2) == 0xFFFF)
            tb_GetCPixel(150, 145, c)
            #expect(rd16(c) == 0xFFFF && rd16(c + 2) == 0xFFFF) // untouched
            // A check box, and right to left (TESysJust -1) its mirrored layout.
            #expect(thme_check_box(qrect(top: 5, left: 180, bottom: 17, right: 192), false, 0, 1))
            tb_GetCPixel(185, 10, c)
            #expect(rd16(c) == 0 && rd16(c + 2) == 0xFFFF)
            wr16(0x0BAC, 0xFFFF)
            #expect(thme_check_box(qrect(top: 5, left: 180, bottom: 17, right: 192), false, 0, 1))
            wr16(0x0BAC, 0)
            tb_GetCPixel(185, 10, c)
            #expect(rd16(c) == 0x8080 && rd16(c + 2) == 0x8080)
        }
    }
}
