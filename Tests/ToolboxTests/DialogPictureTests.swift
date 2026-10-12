import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

/// A DITL item: placeholder, rect, type, data (padded to even).
func ditlItem(_ type: UInt8, _ rect: (Int, Int, Int, Int), _ data: [UInt8]) -> [UInt8] {
    var d = cat(be32(0), be16(rect.0), be16(rect.1), be16(rect.2), be16(rect.3), [type, UInt8(data.count)], data)
    if data.count % 2 == 1 { d.append(0) }
    return d
}

func ditl(_ items: [[UInt8]]) -> [UInt8] { cat(be16(items.count - 1), items.flatMap { $0 }) }

/// DLOG: bounds, procID, visible, goAway, refCon, items ID, title.
func dlog(items: Int16, visible: Bool = true) -> [UInt8] {
    cat(be16(40), be16(40), be16(240), be16(340), be16(1), [visible ? 1 : 0, 0, 0, 0], be32(0),
        be16(Int(UInt16(bitPattern: items))), [0])
}

private func pstr(_ s: String) -> [UInt8] { Array(s.utf8) }

extension Classic {
    @Suite struct DialogManager {
        static let items = ditl([
            ditlItem(4, (160, 220, 180, 290), pstr("OK")),             // 1 button
            ditlItem(5, (20, 20, 36, 200), pstr("Remember me")),      // 2 checkbox
            ditlItem(8, (50, 20, 66, 280), pstr("Hello ^0")),         // 3 static text
            ditlItem(16, (80, 20, 96, 200), pstr("abc")),             // 4 edit text
            ditlItem(4 | 128, (160, 120, 180, 190), pstr("Later")),   // 5 disabled button
        ])

        func open() -> UInt32 {
            launchTestApp([TestResource(type: "DLOG", id: 200, data: dlog(items: 200)),
                           TestResource(type: "DITL", id: 200, data: Self.items)])
            _ = initToolbox()
            let d = tb_GetNewDialog(200, 0, 0xFFFF_FFFF)
            #expect(d != 0)
            return d
        }

        func item(_ d: UInt32, _ n: Int16) -> (type: UInt16, handle: UInt32, rect: [Int16]) {
            let t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
            tb_GetDialogItem(d, n, t, h, r)
            return (rd16(t), rd32(h), (0..<4).map { Int16(bitPattern: rd16(r + UInt32($0 * 2))) })
        }

        @Test func itemsAreBuiltFromTheItemList() {
            let d = open()
            #expect(WindowRecord_windowKind(d) == 2)
            let ok = item(d, 1)
            #expect(ok.type == 4 && ok.handle != 0)
            let title = tb_NewPtr(256, false, true)
            tb_GetControlTitle(ok.handle, title)
            #expect(pstring(at: title) == "OK")
            let text = tb_NewPtr(256, false, true)
            tb_GetDialogItemText(item(d, 4).handle, text)
            #expect(pstring(at: text) == "abc")
            #expect(item(d, 5).type == 4 | 128)
            #expect(tb_FindDialogItem(d, UInt32(bitPattern: Int32(25 << 16 | 30))) == 1) // item 2, zero-based
            tb_DisposeDialog(d)
        }

        @Test func setTextAndHideItems() {
            let d = open()
            tb_SetPort(d)
            let s = tb_NewPtr(256, false, true)
            guestPString("xyz", at: s)
            tb_SetDialogItemText(item(d, 3).handle, s)
            let back = tb_NewPtr(256, false, true)
            tb_GetDialogItemText(item(d, 3).handle, back)
            #expect(pstring(at: back) == "xyz")
            tb_HideDialogItem(d, 3)
            #expect(item(d, 3).rect[1] > 8192)
            tb_ShowDialogItem(d, 3)
            #expect(item(d, 3).rect[1] == 20)
            tb_DisposeDialog(d)
        }

        @Test func modalDialogReturnsTheDefaultItemOnReturn() {
            let d = open()
            tb_SelectWindow(d)
            machine_post_key(13, 36, 0, 1, 0)
            let hit = tb_NewPtr(2, false, true)
            tb_ModalDialog(0, hit)
            #expect(rd16(hit) == 1)
            tb_DisposeDialog(d)
        }

        @Test func typingGoesToTheEditTextItem() {
            let d = open()
            tb_SelectWindow(d)
            let hit = tb_NewPtr(2, false, true)
            for c in "de".utf8 { machine_post_key(Int32(c), 0, 0, 1, 0) }
            tb_ModalDialog(0, hit)                      // the first key
            #expect(rd16(hit) == 4)
            tb_ModalDialog(0, hit)
            let text = tb_NewPtr(256, false, true)
            tb_GetDialogItemText(item(d, 4).handle, text)
            #expect(pstring(at: text) == "de")          // the initial text was selected
            tb_DisposeDialog(d)
        }
    }

    @Suite struct Pictures {
        /// An offscreen 1-bit port 64x64, cleared.
        func offscreen() -> UInt32 {
            _ = initToolbox()
            let port = tb_NewPtr(Int32(GrafPort_SIZEOF), false, true)
            tb_OpenPort(port)
            let bits = tb_NewPtr(8 * 64, false, true)
            let bm = tb_NewPtr(14, false, true)
            wr32(bm, bits)
            wr16(bm + 4, 8)
            for (i, v) in [0, 0, 64, 64].enumerated() { wr16(bm + 6 + UInt32(i * 2), UInt16(v)) }
            tb_SetPortBits(bm)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 64, 64].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_ClipRect(r)
            return bits
        }

        func pixel(_ bits: UInt32, _ x: Int, _ y: Int) -> Bool {
            rd8(bits + UInt32(y * 8 + x / 8)) & (0x80 >> UInt8(x % 8)) != 0
        }

        func picture(_ bytes: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            return h
        }

        func draw(_ pic: UInt32, _ dst: [Int]) {
            let r = tb_NewPtr(8, false, true)
            for (i, v) in dst.enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_DrawPicture(pic, r)
        }

        @Test func versionOnePaintRect() {
            launchTestApp()
            let bits = offscreen()
            // frame 0,0,32,32; version 1; paintRect 8,8,24,24; end
            let pic = picture(cat(be16(0), be16(0), be16(0), be16(32), be16(32), [0x11, 0x01, 0x31],
                                  be16(8), be16(8), be16(24), be16(24), [0xFF]))
            draw(pic, [0, 0, 32, 32])
            #expect(pixel(bits, 10, 10))
            #expect(!pixel(bits, 4, 4))
            #expect(!pixel(bits, 30, 30))
        }

        @Test func versionTwoIsScaledToTheDestination() {
            launchTestApp()
            let bits = offscreen()
            // frame 0,0,32,32; v2 header; paintOval 0,0,32,32; end — drawn at 64x64.
            let pic = picture(cat(be16(0), be16(0), be16(0), be16(32), be16(32), be16(0x0011), be16(0x02FF),
                                  be16(0x0C00), [UInt8](repeating: 0xFF, count: 4), [UInt8](repeating: 0, count: 20),
                                  be16(0x0051), be16(0), be16(0), be16(32), be16(32), be16(0x00FF)))
            draw(pic, [0, 0, 64, 64])
            #expect(pixel(bits, 32, 32))   // centre of the scaled oval
            #expect(pixel(bits, 32, 60))
            #expect(!pixel(bits, 1, 1))    // corner outside the oval
        }

        @Test func packedBitmapsAreDecoded() {
            launchTestApp()
            let bits = offscreen()
            // PackBitsRect: 16x8 bitmap, rowBytes 2 (< 8, so rows are unpacked): a checkerboard row pattern.
            var data: [UInt8] = []
            for y in 0..<8 { data += y % 2 == 0 ? [0xFF, 0x00] : [0x00, 0xFF] }
            let pic = picture(cat(be16(0), be16(0), be16(0), be16(8), be16(16), be16(0x0011), be16(0x02FF),
                                  be16(0x0C00), [UInt8](repeating: 0xFF, count: 4), [UInt8](repeating: 0, count: 20),
                                  be16(0x0098), be16(2), be16(0), be16(0), be16(8), be16(16),
                                  be16(0), be16(0), be16(8), be16(16), be16(0), be16(0), be16(8), be16(16), be16(0),
                                  data, be16(0x00FF)))
            draw(pic, [0, 0, 8, 16])
            #expect(pixel(bits, 2, 0))
            #expect(!pixel(bits, 10, 0))
            #expect(!pixel(bits, 2, 1))
            #expect(pixel(bits, 10, 1))
        }

        @Test func openPictureMakesAValidEmptyPicture() {
            launchTestApp()
            _ = offscreen()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 20, 30].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let pic = tb_OpenPicture(r)
            tb_ClosePicture()
            #expect(rd16(rd32(pic) + 8) == 30)            // frame right
            #expect(rd16(rd32(pic) + 10) == 0x0011)        // version opcode
            draw(pic, [0, 0, 20, 30])                     // and it plays without trouble
            tb_KillPicture(pic)
        }
    }

    /// Items an app adds to its Standard File dialog run through its hook.
    @Suite struct StandardFileExtras {
        nonisolated(unsafe) static var hookItems: [Int16] = []
        nonisolated(unsafe) static var offeredItems: [Int16] = []

        /// The hook: DlgHookYD(item, dialog, data): INTEGER.  Toggles checkbox 13.
        static let hook: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)                               // data
            let dialog = cpu_pop32(c)
            let item = Int16(bitPattern: cpu_pop16(c))
            StandardFileExtras.hookItems.append(item)
            if item == 13 {
                let t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
                tb_GetDialogItem(dialog, 13, t, h, r)
                tb_SetControlValue(rd32(h), 1 - tb_GetControlValue(rd32(h)))
            }
            wr16(c.pointee.a.7, UInt16(bitPattern: item))  // result
            cpu_push32(c, ret)
        }

        @Test func extraItemsAreOfferedAndClicksRunTheHook() {
            // Twelve standard put items, then a checkbox.
            var items = (1...12).map { _ in ditlItem(0, (0, 0, 10, 10), []) }
            items.append(ditlItem(5, (200, 20, 216, 180), pstr("Stationery")))
            launchTestApp([TestResource(type: "DLOG", id: 300, data: dlog(items: 300, visible: false)),
                           TestResource(type: "DITL", id: 300, data: ditl(items))])
            _ = initToolbox()
            Self.hookItems = []
            Self.offeredItems = []
            let hook = native_register(Self.hook, "test hook", 0)
            machine_set_file_handler({ rq, _, _, _ in
                guard let rq = rq?.pointee else { return false }
                withUnsafeBytes(of: rq.items) { raw in
                    let items = raw.bindMemory(to: machine_file_item.self)
                    StandardFileExtras.offeredItems = (0..<Int(rq.nitems)).map { items[$0].item }
                }
                // The user clicks the checkbox twice, then cancels.
                _ = machine_file_item_hit(13, nil)
                var states = [machine_file_item](repeating: machine_file_item(), count: 4)
                _ = machine_file_items(&states, 4)
                #expect(states[0].value == 1)
                _ = machine_file_item_hit(13, nil)
                _ = machine_file_items(&states, 4)
                #expect(states[0].value == 0)
                return false
            }, nil)
            defer { machine_set_file_handler(nil, nil) }
            let reply = tb_NewPtr(Int32(StandardFileReply_SIZEOF), false, true)
            let name = tb_NewPtr(256, false, true)
            guestPString("Untitled", at: name)
            tb_CustomPutFile(0, name, reply, 300, 0xFFFF_FFFF, hook, 0, 0, 0, 0)
            #expect(Self.offeredItems == [13])
            #expect(Self.hookItems == [-1, 13, 13, -2])      // first call, two clicks, last call
            #expect(rd8(reply) == 0)
        }
    }
}

extension Classic {
    /// The 8-bit screen and the colour appearances.
    @Suite struct ColourScreen {
        func rgba(_ x: Int32, _ y: Int32) -> [UInt8] {
            var px = [UInt8](repeating: 0, count: 4)
            machine_screen_rgba(x, y, 1, 1, &px)
            return Array(px[0..<3])
        }

        @Test func oldPortsDrawAtTheScreensDepth() {
            machine_set_screen_depth(8)
            defer { machine_set_screen_depth(1) }
            launchTestApp()
            _ = initToolbox()
            #expect(PixMap_pixelSize(rd32(GDevice_gdPMap(rd32(tb_GetMainDevice())))) == 8)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 120, 140].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_PaintRect(r)
            #expect(rgba(110, 110) == [0, 0, 0])
            tb_EraseRect(r)
            #expect(rgba(110, 110) == [255, 255, 255])
            tb_ForeColor(205) // redColor
            tb_PaintRect(r)
            let red = rgba(110, 110)
            #expect(red[0] > 150 && red[1] < 80 && red[2] < 80)
        }

        @Test func blackAndWhiteStaysOneBit() {
            launchTestApp()
            #expect(PixMap_pixelSize(rd32(GDevice_gdPMap(rd32(tb_GetMainDevice())))) == 1)
        }
    }
}

extension Classic {
    @Suite struct Clipboard {
        nonisolated(unsafe) static var exported: String?

        @Test func hostTextBecomesTheDeskScrapAndBack() {
            launchTestApp()
            Self.exported = nil
            machine_set_clipboard_handler({ utf8, _ in Clipboard.exported = utf8.map { String(cString: $0) } }, nil)
            defer { machine_set_clipboard_handler(nil, nil) }
            machine_set_clipboard_text("café\nline")
            let h = tb_NewHandle(0, false, false)
            let off = tb_NewPtr(4, false, true)
            let n = tb_GetScrap(h, Int32(bitPattern: fourCC("TEXT")), off)
            #expect(n == 9)                                          // MacRoman é is one byte
            #expect(rd8(rd32(h) + 3) == 0x8E)                        // é
            #expect(rd8(rd32(h) + 4) == 0x0D)                        // CR
            #expect(Self.exported == nil)                            // importing is not exporting
            #expect(tb_TEFromScrap() == 0)
            #expect(tb_TEGetScrapLength() == 9)
            #expect(tb_TEToScrap() == 0)
            #expect(Self.exported == "café\nline")
        }
    }
}

/// A cicn (8-bit, two colours) from a pixel function: 0 white, 1 red; mask everything.
func cicnData(w: Int, h: Int, _ pixel: (Int, Int) -> UInt8) -> [UInt8] {
    let rb = w, mrb = (w + 15) / 16 * 2
    var d = cat(be32(0), be16(0x8000 | rb), be16(0), be16(0), be16(h), be16(w), be16(0), be16(0), be32(0),
                be32(0x480000), be32(0x480000), be16(0), be16(8), be16(1), be16(8), be32(0), be32(0), be32(0))
    d += cat(be32(0), be16(mrb), be16(0), be16(0), be16(h), be16(w))   // mask BitMap
    d += cat(be32(0), be16(0), be16(0), be16(0), be16(h), be16(w))     // icon BitMap (no data)
    d += be32(0)                                                         // iconData
    d += [UInt8](repeating: 0xFF, count: mrb * h)                        // mask: all opaque
    d += cat(be32(0), be16(0), be16(1),                                  // ctab: 2 entries
             be16(0), be16(0xFFFF), be16(0xFFFF), be16(0xFFFF),
             be16(1), be16(0xFFFF), be16(0), be16(0))
    for y in 0..<h { for x in 0..<w { d.append(pixel(x, y)) } }
    return d
}

extension Classic {
    @Suite struct Themes {
        /// A tiny scheme: a 40x30 sample document window, red frame, content (10,5)-(25,35).
        static func scheme() -> URL {
            let sample = cicnData(w: 40, h: 30) { x, y in (y >= 10 && y < 25 && x >= 5 && x < 35) ? 0 : 1 }
            let wnd = cat(be16(0), be16(0), be16(10), be16(5), be16(25), be16(35),   // content rect
                          be16(1), be16(0), be16(0), be16(8), be16(40),             // top: start 0, stretch to 40
                          be16(1), be16(0), be16(0), be16(8), be16(40),             // bottom
                          be16(1), be16(0), be16(10), be16(8), be16(25),            // left
                          be16(1), be16(0), be16(10), be16(8), be16(25))            // right
            var fork = buildFork([
                TestResource(type: "wnd#", id: -14336, data: wnd),
                TestResource(type: "cicn", id: -14335, data: sample),
                TestResource(type: "cicn", id: -14336, data: sample),
            ])
            let url = tempDir().appendingPathComponent("Tiny Scheme")
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(url.path, &f, &err, err.count))
            return url
        }

        @Test func schemeFramesDocumentWindows() {
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_load_theme(Self.scheme().path, &err, err.count))
            defer { machine_set_appearance(Int32(MACHINE_APPEARANCE_BW)) }
            launchTestApp()
            _ = initToolbox()
            #expect(PixMap_pixelSize(rd32(GDevice_gdPMap(rd32(tb_GetMainDevice())))) == 32)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 200, 300].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let w = tb_NewWindow(0, r, 0, 1, 0, 0xFFFF_FFFF, 1, 0)
            tb_SelectWindow(w)
            var px = [UInt8](repeating: 0, count: 4)
            machine_screen_rgba(150, 95, 1, 1, &px)                 // in the top band, 5 above the content
            #expect(Array(px[0..<3]) == [255, 0, 0])
            machine_screen_rgba(97, 150, 1, 1, &px)                 // left side band
            #expect(Array(px[0..<3]) == [255, 0, 0])
            // The structure extends by the sample's margins: 10 top, 5 sides, 5 bottom.
            let s = rd32(WindowRecord_strucRgn(w))
            #expect(Int16(bitPattern: rd16(s + 2)) == 90 && Int16(bitPattern: rd16(s + 4)) == 95)
            #expect(Int16(bitPattern: rd16(s + 6)) == 205 && Int16(bitPattern: rd16(s + 8)) == 305)
        }
    }
}
