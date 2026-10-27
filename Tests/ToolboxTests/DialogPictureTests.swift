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

extension BlueBox {
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

        /// As the Dialog Manager made them: a plain GrafPort, whose
        /// portBits.bounds programs read (Word 5.0 centres its dialogs by
        /// it), unless the dialog has a 'dctb' or comes from NewColorDialog.
        @Test func dialogsAreColourOnlyWithAColourTable() {
            let d = open()
            func isColour(_ w: UInt32) -> Bool { rd16(w + 6) & 0xC000 == 0xC000 }
            #expect(!isColour(d))
            #expect(Int16(bitPattern: rd16(d + 8)) == -40) // portBits.bounds.top: the window's top, negated
            let dctb = cat(be32(0), be16(0), be16(0xFFFF)) // ctSize -1: the default colours
            launchTestApp([TestResource(type: "DLOG", id: 200, data: dlog(items: 200)),
                           TestResource(type: "DITL", id: 200, data: Self.items),
                           TestResource(type: "dctb", id: 200, data: dctb)])
            _ = initToolbox()
            let c = tb_GetNewDialog(200, 0, 0xFFFF_FFFF)
            #expect(isColour(c) && WindowRecord_visible(c) != 0)
            let none = tb_NewHandle(2, false, true)
            wr16(rd32(none), 0xFFFF) // no items
            let n = tb_NewColorDialog(0, tb_NewPtr(8, false, true), 0, 0, 1, 0xFFFF_FFFF, 0, 0, none)
            #expect(isColour(n))
        }

        /// An 'ictb' (with a 'dctb') gives text items their font, face and
        /// size: PopChar Pro's installer sets its static texts in Geneva 9.
        @Test func itemColourTableSetsTheTextStyle() {
            func textRight(_ ictb: [UInt8]?) -> Int {
                var res = [TestResource(type: "DLOG", id: 200, data: dlog(items: 200)),
                           TestResource(type: "DITL", id: 200, data: Self.items),
                           TestResource(type: "dctb", id: 200, data: cat(be32(0), be16(0), be16(0xFFFF)))]
                if let ictb { res.append(TestResource(type: "ictb", id: 200, data: ictb)) }
                launchTestApp(res)
                _ = initToolbox()
                let d = tb_GetNewDialog(200, 0, 0xFFFF_FFFF)
                tb_SetPort(d)
                tb_DrawDialog(d)
                let c = tb_NewPtr(6, false, true)
                var right = 0
                for v in 50..<66 { for h in 20..<280 {
                    tb_GetCPixel(Int16(h), Int16(v), c)
                    if rd16(c) < 0x8000 { right = max(right, h) }
                } }
                tb_DisposeDialog(d)
                return right
            }
            // Items 1, 2 none; item 3 the size (bit 2) from the style at 20.
            let style = cat(be16(0), be16(0), be16(9), [UInt8](repeating: 0, count: 14))
            let ictb = cat(be32(0), be32(0), be16(0x0004), be16(20), be32(0), be32(0), style)
            let plain = textRight(nil), small = textRight(ictb)
            #expect(plain > 0 && small > 0)
            #expect(small < plain)
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

        /// Snood 2.1's alert filter "presses" its Cancel item on Command-period
        /// with HiliteControl, which in its one-button alert is the static
        /// text: the text's handle is no control record, nothing to change
        /// in it or defproc to call.
        @Test func hilitingAnItemThatIsNoControlLeavesItAlone() {
            let d = open()
            tb_SetPort(d)
            let text = item(d, 3).handle
            let s = tb_NewPtr(256, false, true)
            guestPString("Codes are case sensitive. Please try again.", at: s)
            tb_SetDialogItemText(text, s)
            let before = (0..<tb_GetHandleSize(text)).map { rd8(rd32(text) + UInt32($0)) }
            tb_HiliteControl(text, 1)
            let after = (0..<tb_GetHandleSize(text)).map { rd8(rd32(text) + UInt32($0)) }
            #expect(after == before)
            #expect(!g_cpu!.pointee.halted)
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

        /// A dialog with a windowKind of the program's own (ResEdit's
        /// editors) still takes DialogSelect's keys.
        @Test func dialogSelectTakesAnyKindOfDialog() {
            let d = open()
            tb_SelectWindow(d)
            wr16(d + UInt32(WindowRecord_windowKind_off), 135)
            let evt = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true), dp = tb_NewPtr(4, false, true)
            let hit = tb_NewPtr(2, false, true)
            wr16(evt, 3) // keyDown
            wr32(evt + UInt32(EventRecord_message_off), UInt32(UInt8(ascii: "x")))
            #expect(tb_IsDialogEvent(evt) == 0) // by windowKind, as before
            #expect(tb_DialogSelect(evt, dp, hit) != 0 && rd32(dp) == d && rd16(hit) == 4)
            let text = tb_NewPtr(256, false, true)
            tb_GetDialogItemText(item(d, 4).handle, text)
            #expect(pstring(at: text) == "x")
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

        /// An extended version 2 picture (header version -2) draws in the
        /// coordinates of its native-resolution frame.
        @Test func extendedVersionTwoUsesItsNativeFrame() {
            launchTestApp()
            let bits = offscreen()
            // frame 0,0,32,32 at 72 dpi; header -2, 144 dpi, native frame 0,0,64,64;
            // paintRect 32,32,64,64 (the lower right quarter); end.
            let pic = picture(cat(be16(0), be16(0), be16(0), be16(32), be16(32), be16(0x0011), be16(0x02FF),
                                  be16(0x0C00), be16(0xFFFE), be16(0), be32(0x0090_0000), be32(0x0090_0000),
                                  be16(0), be16(0), be16(64), be16(64), be32(0),
                                  be16(0x0031), be16(32), be16(32), be16(64), be16(64), be16(0x00FF)))
            draw(pic, [0, 0, 32, 32])
            #expect(pixel(bits, 20, 20))
            #expect(!pixel(bits, 10, 10))
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

        /// A colour PixMap in a picture keeps its colours in a colour port.
        @Test func colourPixMapsKeepTheirColours() {
            launchTestApp()
            _ = initToolbox()
            let gw = tb_NewPtr(4, false, true), bounds = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 2, 2].enumerated() { wr16(bounds + UInt32(i * 2), UInt16(v)) }
            #expect(tb_NewGWorld(gw, 32, bounds, 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            // PackBitsRect: a 2x2 8-bit PixMap (rowBytes 2: rows unpacked), index 0 red, 1 blue.
            let pixmap = cat(be16(0x8002), be16(0), be16(0), be16(2), be16(2), be16(0), be16(0), be32(0),
                             be32(0x0048_0000), be32(0x0048_0000), be16(0), be16(8), be16(1), be16(8),
                             be32(0), be32(0), be32(0))
            let ctab = cat(be32(0), be16(0), be16(1), be16(0), be16(0xFFFF), be16(0), be16(0),
                           be16(1), be16(0), be16(0), be16(0xFFFF))
            let pic = picture(cat(be16(0), be16(0), be16(0), be16(2), be16(2), be16(0x0011), be16(0x02FF),
                                  be16(0x0C00), [UInt8](repeating: 0xFF, count: 4), [UInt8](repeating: 0, count: 20),
                                  be16(0x0098), pixmap, ctab,
                                  be16(0), be16(0), be16(2), be16(2), be16(0), be16(0), be16(2), be16(2), be16(0),
                                  [0, 1, 1, 0], be16(0x00FF)))
            draw(pic, [0, 0, 2, 2])
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(0, 0, c)
            #expect(rd16(c) == 0xFFFF && rd16(c + 4) == 0) // red, not dithered to black and white
            tb_GetCPixel(1, 0, c)
            #expect(rd16(c) == 0 && rd16(c + 4) == 0xFFFF) // blue
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

        func rect(_ v: [Int]) -> UInt32 {
            let r = tb_NewPtr(8, false, true)
            for (i, x) in v.enumerated() { wr16(r + UInt32(i * 2), UInt16(bitPattern: Int16(x))) }
            return r
        }

        func currentPort() -> UInt32 {
            let p = tb_NewPtr(4, false, true)
            tb_GetPort(p)
            return rd32(p)
        }

        func clear(_ bits: UInt32) { for i in 0..<(8 * 64) { wr8(bits + UInt32(i), 0) } }

        /// Drawing between OpenPicture and ClosePicture is recorded, not shown,
        /// and the picture draws it again, scaled to where it is drawn.
        @Test func openPictureRecordsTheDrawing() {
            launchTestApp()
            let bits = offscreen()
            let pic = tb_OpenPicture(rect([0, 0, 32, 32]))
            tb_PaintRect(rect([2, 2, 10, 10]))
            tb_PenSize(2, 2)
            tb_MoveTo(0, 30)
            tb_LineTo(31, 30)
            tb_PenNormal()
            tb_FrameOval(rect([12, 12, 30, 30]))
            tb_ClosePicture()
            #expect(!pixel(bits, 5, 5))                   // nothing drawn while recording
            #expect(Int16(bitPattern: rd16(currentPort() + UInt32(GrafPort_pnVis_off))) == 0) // the pen shown again
            draw(pic, [0, 0, 64, 64])
            #expect(pixel(bits, 10, 10))                  // the painted rectangle, doubled
            #expect(!pixel(bits, 22, 22))
            #expect(pixel(bits, 40, 61))                  // the 2-pixel line, doubled
            #expect(pixel(bits, 42, 24))                  // the oval's frame...
            #expect(!pixel(bits, 42, 42))                 // ...not its inside
            tb_KillPicture(pic)
        }

        /// CopyBits into a recording port keeps the source's pixels.
        @Test func recordedCopyBitsKeepsItsPixels() {
            launchTestApp()
            let bits = offscreen()
            tb_PaintRect(rect([0, 0, 16, 16]))
            tb_FrameRect(rect([20, 20, 30, 30]))
            let src = tb_NewPtr(14, false, true)
            wr32(src, bits)
            wr16(src + 4, 8)
            for (i, v) in [0, 0, 64, 64].enumerated() { wr16(src + 6 + UInt32(i * 2), UInt16(v)) }
            // Keep the source: copy it to its own buffer first.
            let copy = tb_NewPtr(8 * 64, false, true)
            for i in 0..<(8 * 64) { wr8(copy + UInt32(i), rd8(bits + UInt32(i))) }
            wr32(src, copy)
            clear(bits)
            let pic = tb_OpenPicture(rect([0, 0, 32, 32]))
            tb_CopyBits(src, currentPort() + UInt32(GrafPort_portBits_off), rect([0, 0, 32, 32]),
                        rect([0, 0, 32, 32]), 0, 0)
            tb_ClosePicture()
            #expect(!pixel(bits, 4, 4))
            draw(pic, [0, 0, 32, 32])
            #expect(pixel(bits, 4, 4))
            #expect(pixel(bits, 20, 25))
            #expect(!pixel(bits, 25, 25))
            tb_KillPicture(pic)
        }

        /// Text is recorded with its font settings.
        @Test func recordedTextDrawsAgain() {
            launchTestApp()
            let bits = offscreen()
            let pic = tb_OpenPicture(rect([0, 0, 64, 64]))
            tb_TextFont(4)
            tb_TextSize(9)
            tb_MoveTo(2, 20)
            let s = tb_NewPtr(4, false, true)
            for (i, c) in "WWW".utf8.enumerated() { wr8(s + UInt32(i), c) }
            tb_DrawText(s, 0, 3)
            tb_ClosePicture()
            var before = 0
            for y in 0..<64 { for x in 0..<64 where pixel(bits, x, y) { before += 1 } }
            #expect(before == 0)
            tb_TextFont(0)
            tb_TextSize(0)
            draw(pic, [0, 0, 64, 64])
            var after = 0
            for y in 10..<22 { for x in 0..<30 where pixel(bits, x, y) { after += 1 } }
            #expect(after > 10)
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

        nonisolated(unsafe) static var filterEvents: [Int16] = []

        /// The filter: ModalFilterYD(dialog, event, itemHit, data): BOOLEAN.
        /// Records the event's kind and answers item 10.
        static let filter: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)                               // data
            let item = cpu_pop32(c)
            let event = cpu_pop32(c)
            _ = cpu_pop32(c)                               // dialog
            StandardFileExtras.filterEvents.append(Int16(bitPattern: rd16(event)))
            wr16(item, 10)
            wr16(c.pointee.a.7, 0x0100)                    // true
            cpu_push32(c, ret)
        }

        /// CustomGetFile's dialog filter gets null events while the panel is
        /// up, and an item it answers reaches the hook.
        @Test func dialogFiltersGetNullEvents() {
            var items = (1...9).map { _ in ditlItem(0, (0, 0, 10, 10), []) }
            items.append(ditlItem(5, (200, 20, 216, 180), pstr("Show all")))
            launchTestApp([TestResource(type: "DLOG", id: 301, data: dlog(items: 301, visible: false)),
                           TestResource(type: "DITL", id: 301, data: ditl(items))])
            _ = initToolbox()
            Self.hookItems = []
            Self.filterEvents = []
            let hook = native_register(Self.hook, "test hook", 0)
            let filter = native_register(Self.filter, "test filter", 0)
            machine_set_file_handler({ _, _, _, _ in
                machine_file_idle()
                return false
            }, nil)
            defer { machine_set_file_handler(nil, nil) }
            let reply = tb_NewPtr(Int32(StandardFileReply_SIZEOF), false, true)
            tb_CustomGetFile(0, -1, 0, reply, 301, 0xFFFF_FFFF, hook, filter, 0, 0, 0)
            #expect(Self.filterEvents == [0])                 // a null event
            #expect(Self.hookItems == [-1, 10, -2])           // first call, the filter's item, last call
        }

        nonisolated(unsafe) static var listProc: UInt32 = 0

        /// A hook that keeps the file list's user item procedure on its first call.
        static let listHook: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)                               // data
            let dialog = cpu_pop32(c)
            let item = Int16(bitPattern: cpu_pop16(c))
            if item == -1 {
                let t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
                tb_GetDialogItem(dialog, 7, t, h, r)
                StandardFileExtras.listProc = rd32(h)
            }
            wr16(c.pointee.a.7, UInt16(bitPattern: item))
            cpu_push32(c, ret)
        }

        /// The package's own user items (the file list, item 7) have a
        /// drawing procedure that programs wrapping it call: PrettyC's.
        @Test func standardUserItemsHaveProcedures() {
            var items = (1...9).map { _ in ditlItem(0, (0, 0, 10, 10), []) }
            items.append(ditlItem(5, (200, 20, 216, 180), pstr("Show all")))
            launchTestApp([TestResource(type: "DLOG", id: 302, data: dlog(items: 302, visible: false)),
                           TestResource(type: "DITL", id: 302, data: ditl(items))])
            _ = initToolbox()
            guestStack()
            Self.listProc = 0
            let hook = native_register(Self.listHook, "test hook", 0)
            let reply = tb_NewPtr(Int32(StandardFileReply_SIZEOF), false, true)
            tb_CustomGetFile(0, -1, 0, reply, 302, 0xFFFF_FFFF, hook, 0, 0, 0, 0)
            let index = native_index_for(Self.listProc)
            #expect(index >= 0)
            // Called as userItem procedures are, it removes its arguments.
            guard index >= 0, let c = g_cpu, let fn = native_fn_at(index) else { return }
            let sp = c.pointee.a.7
            cpu_push32(c, 0x1234)                          // the dialog
            cpu_push16(c, 7)
            cpu_push32(c, 0x5678)                          // return address
            fn(c, 0)
            #expect(cpu_pop32(c) == 0x5678)
            #expect(c.pointee.a.7 == sp)
        }

        func writeFile(_ dir: URL, _ name: String, type: String) -> String {
            let path = dir.appendingPathComponent(name).path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            var info = [UInt8](repeating: 0, count: 32)
            for (i, b) in type.utf8.enumerated() { info[i] = b }
            withUnsafeMutableBytes(of: &f.finder_info) { raw in for i in 0..<32 { raw[i] = info[i] } }
            f.has_finder_info = true
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            return path
        }

        /// The application's file filter decides what an open panel offers:
        /// here it hides TEXT files.
        @Test func fileFiltersHideFiles() {
            let dir = launchTestApp()
            _ = initToolbox()
            let text = writeFile(dir, "Notes", type: "TEXT"), pict = writeFile(dir, "Drawing", type: "PICT")
            // FileFilter(pb): BOOLEAN, true for ioFlFndrInfo.fdType 'TEXT'.
            let words: [UInt16] = [
                0x225F,                                  // MOVEA.L (SP)+,A1   return address
                0x205F,                                  // MOVEA.L (SP)+,A0   pb
                0x0CA8, 0x5445, 0x5854, 0x0020,          // CMPI.L #'TEXT',32(A0)
                0x57C0,                                  // SEQ D0
                0x1E80,                                  // MOVE.B D0,(SP)
                0x4ED1,                                  // JMP (A1)
            ]
            let code = tb_NewPtr(Int32(words.count * 2), false, true)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            defer { machine_set_file_handler(nil, nil) }
            var paths = [text, pict]
            withUnsafeMutablePointer(to: &paths) { ptr in
                machine_set_file_handler({ _, _, _, ctx in
                    let paths = ctx!.assumingMemoryBound(to: [String].self).pointee
                    FilterResults.values = paths.map { machine_file_filtered($0) }
                    return false
                }, ptr)
                let reply = tb_NewPtr(Int32(StandardFileReply_SIZEOF), false, true)
                tb_StandardGetFile(code, -1, 0, reply)
            }
            #expect(FilterResults.values == [true, false])
            #expect(!machine_file_filtered(text))           // only while the panel is up
        }
    }
}

enum FilterResults { nonisolated(unsafe) static var values: [Bool] = [] }

extension BlueBox {
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

        /// Colours written into the screen's table (for an inverse table to
        /// draw with) are not shown until SetEntries puts them on the screen.
        @Test func theScreenShowsColoursSetEntriesGaveIt() {
            machine_set_screen_depth(8)
            defer { machine_set_screen_depth(1) }
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 120, 140].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_PaintRect(r)
            let ct = rd32(PixMap_pmTable(rd32(GDevice_gdPMap(rd32(tb_GetMainDevice())))))
            let black = ct + 8 + 255 * 8
            wr16(black + 2, 0xFFFF)
            tb_MakeITable(0, 0, 0)
            wr32(ct, UInt32(bitPattern: tb_GetCTSeed()))
            #expect(rgba(110, 110) == [0, 0, 0])
            let spec = tb_NewPtr(8, false, true)
            wr16(spec + 2, 0xFFFF)
            tb_SetEntries(255, 0, spec)
            #expect(rgba(110, 110) == [255, 0, 0])
        }

        /// Moving or sizing a hidden window leaves the windows under it as
        /// they were (Solitaire Till Dawn sizes its game window while its
        /// splash dialog shows, and never redraws the dialog).
        @Test func hiddenWindowsChangeNothingOnScreen() {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [60, 60, 260, 360].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let shown = tb_NewWindow(0, r, 0, 1, 2, 0xFFFF_FFFF, 0, 0) // a plain box
            tb_SetPort(shown)
            tb_PaintRect(shown + UInt32(GrafPort_portRect_off))
            #expect(rgba(150, 150) == [0, 0, 0])
            let hidden = tb_NewWindow(0, r, 0, 0, 2, 0xFFFF_FFFF, 0, 0)
            tb_SizeWindow(hidden, 400, 300, 1)
            tb_SendBehind(hidden, 0)
            #expect(rgba(150, 150) == [0, 0, 0])
        }

        @Test func blackAndWhiteStaysOneBit() {
            launchTestApp()
            #expect(PixMap_pixelSize(rd32(GDevice_gdPMap(rd32(tb_GetMainDevice())))) == 1)
        }
    }
}

extension BlueBox {
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

extension BlueBox {
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
