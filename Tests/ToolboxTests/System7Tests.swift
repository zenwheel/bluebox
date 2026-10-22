import CCore
import Foundation
import ForkStore
import Testing
import Toolbox

// Behaviour TeachText (System 7.0/7.1 era) depends on.

/// A file in a fresh temporary folder.
private func hostFile(_ name: String, _ text: String, in dir: URL? = nil) -> URL {
    let folder = dir ?? tempDir()
    let url = folder.appendingPathComponent(name)
    try? Data(text.utf8).write(to: url)
    return url
}

private func guestString(_ s: String) -> UInt32 {
    let p = tb_NewPtr(256, false, true)
    guestPString(s, at: p)
    return p
}

extension BlueBox {
    @Suite struct Fonts {
        @Test func applicationFontIsGeneva() {
            launchTestApp()
            #expect(rd16(UInt32(LM_ApFontID)) == 3)
        }

        /// MacRoman characters beyond ASCII have glyphs in the built-in fonts.
        @Test(arguments: [(0, 12), (0, 9), (3, 9), (3, 12), (2, 12), (4, 9)])
        func macRomanGlyphs(family: Int, size: Int) {
            launchTestApp()
            let h = tb_GetResource(Int32(bitPattern: fourCC("NFNT")), Int16(family * 128 + size))
            #expect(h != 0)
            let f = rd32(h)
            let first = Int(rd16(f + 2)), last = Int(rd16(f + 4))
            let nDescent = Int(Int16(bitPattern: rd16(f + 10)))  // NFNT: high word of owTLoc when positive
            let owtloc = Int(rd16(f + 16)) | (nDescent > 0 ? nDescent << 16 : 0)
            let ow = f + 16 + UInt32(owtloc * 2)
            func hasGlyph(_ c: Int) -> Bool { c >= first && c <= last && rd16(ow + UInt32((c - first) * 2)) != 0xFFFF }
            // é, ï, —, “, ’, …, •, ©
            for c in [0x8E, 0x95, 0xD1, 0xD2, 0xD5, 0xC9, 0xA5, 0xA9] {
                #expect(hasGlyph(c), "family \(family) size \(size) char \(String(c, radix: 16))")
            }
        }
    }

    @Suite struct AliasManager {
        @Test func aliasRoundTripsLongHostNames() {
            let dir = launchTestApp()
            let long = dir.appendingPathComponent(String(repeating: "folder-name-", count: 4))
            try? FileManager.default.createDirectory(at: long, withIntermediateDirectories: true)
            let file = hostFile("note.txt", "hello", in: long)
            let ah = tb_NewPtr(4, false, true)
            #expect(alias_new_for_host(file.path, ah) == 0)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            let changed = tb_NewPtr(2, false, true)
            #expect(tb_ResolveAlias(0, rd32(ah), spec, changed) == 0)
            #expect(pstring(at: spec + UInt32(FSSpec_name_off)) == "note.txt")
            let name = tb_NewPtr(256, false, true)
            #expect(tb_GetAliasInfo(rd32(ah), 0, name) == 0)
            #expect(pstring(at: name) == "note.txt")
        }

        @Test func missingTargetIsFileNotFound() {
            let dir = launchTestApp()
            let file = hostFile("gone.txt", "x", in: dir)
            let ah = tb_NewPtr(4, false, true)
            #expect(alias_new_for_host(file.path, ah) == 0)
            try? FileManager.default.removeItem(at: file)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            #expect(tb_ResolveAlias(0, rd32(ah), spec, 0) == -43)
        }
    }

    @Suite struct HighLevelEvents {
        func nextEvent() -> (what: UInt16, message: UInt32, whereField: UInt32)? {
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            defer { tb_DisposePtr(e) }
            guard tb_GetNextEvent(-1, e) != 0 else { return nil }
            return (rd16(e), rd32(e + UInt32(EventRecord_message_off)), rd32(e + UInt32(EventRecord_where_off)))
        }

        /// TeachText accepts the raw message and walks the System 7 wire format.
        @Test func openDocumentsReachHighLevelEventAwareApps() {
            let dir = launchTestApp(sizeFlags: 0x0040)
            let file = hostFile("letter.txt", "Dear Sir", in: dir)
            withCStrings([file.path]) { machine_open_documents($0, 1, false) }
            let ev = nextEvent()
            #expect(ev?.what == 23)
            #expect(ev?.message == fourCC("aevt"))
            #expect(ev?.whereField == fourCC("odoc"))

            // First call with no buffer: bufferIsSmall and the length needed.
            let len = tb_NewPtr(4, false, true)
            let refcon = tb_NewPtr(4, false, true)
            let sender = tb_NewPtr(Int32(TargetID_SIZEOF), false, true)
            #expect(tb_AcceptHighLevelEvent(sender, refcon, 0, len) == -607)
            let n = rd32(len)
            #expect(n > 16)
            let buf = tb_NewPtr(Int32(n), false, true)
            #expect(tb_AcceptHighLevelEvent(sender, refcon, buf, len) == 0)

            // As TeachText parses it: header, attributes up to ';;;;', then '----'.
            #expect(rd32(buf) == fourCC("aevt"))
            var p = buf + 8
            while rd32(p) != 0x3B3B_3B3B { p += 12 + rd32(p + 8) }
            p += 4
            #expect(rd32(p) == fourCC("----"))
            #expect(rd32(p + 4) == fourCC("list"))
            let list = p + 4
            #expect(rd32(list + 8) == 1)                      // one item
            let item = list + 16
            #expect(rd32(item) == fourCC("alis"))
            let size = rd32(item + 4)
            let alias = tb_NewHandle(Int32(size), false, false)
            gmemcpy(rd32(alias), item + 8, size)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            #expect(tb_ResolveAlias(0, alias, spec, 0) == 0)
            #expect(pstring(at: spec + UInt32(FSSpec_name_off)) == "letter.txt")
        }

        @Test func appsThatTakeNoHighLevelEventsGetDocumentsAtLaunch() {
            let dir = tempDir()
            let file = hostFile("memo.txt", "hi", in: dir)
            launchTestApp(documents: [file.path])
            withCStrings([file.path]) { machine_open_documents($0, 1, false) }
            #expect(nextEvent()?.what != 23)
            let parms = rd32(rd32(UInt32(LM_AppParmHandle)))
            #expect(rd16(parms + 2) == 1)                     // one file in the Finder info
            #expect(pstring(at: parms + 4 + 8) == "memo.txt")
        }

        @Test func awareAppsGetNoLaunchDocumentsInFinderInfo() {
            let file = hostFile("memo.txt", "hi")
            launchTestApp(sizeFlags: 0x0040, documents: [file.path])
            #expect(rd16(rd32(rd32(UInt32(LM_AppParmHandle))) + 2) == 0)
        }
    }

    @Suite struct TextEdit {
        @Test func calTextTakesTheLengthFromTheTextHandle() {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 100, 200].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let te = tb_TENew(r, r)
            // Replace the text the way TeachText reads a file: resize hText, fill it.
            let hText = rd32(rd32(te) + 62)
            tb_SetHandleSize(hText, 5)
            for (i, b) in Array("Hello".utf8).enumerated() { wr8(rd32(hText) + UInt32(i), b) }
            tb_TECalText(te)
            #expect(rd16(rd32(te) + UInt32(TERec_teLength_off)) == 5)
        }

        /// caretTime is when the caret next blinks: apps sleep until then.
        @Test func caretTimeIsTheNextBlink() {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 100, 200].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let te = tb_TENew(r, r)
            tb_TEActivate(te)
            tb_TEIdle(te)
            let now = rd32(UInt32(LM_Ticks))
            let next = UInt32(bitPattern: TERec_caretTime(rd32(te)))
            #expect(next > now)
            #expect(next - now <= rd32(UInt32(LM_CaretTime)))
        }
    }

    /// The Drag Manager's items and flavors, and TextEdit's highlight region.
    @Suite struct DragManager {
        @Test func itemsCarryFlavorData() {
            launchTestApp()
            _ = initToolbox()
            let ref = tb_NewPtr(4, false, true)
            #expect(tb_NewDrag(ref) == 0)
            let drag = rd32(ref)
            let text = tb_NewPtr(5, false, true)
            for (i, b) in "hello".utf8.enumerated() { wr8(text + UInt32(i), b) }
            let TEXT = Int32(bitPattern: 0x5445_5854), styl = Int32(bitPattern: 0x7374_796C)
            #expect(tb_AddDragItemFlavor(drag, 1, TEXT, text, 5, 0) == 0)
            #expect(tb_AddDragItemFlavor(drag, 1, TEXT, text, 5, 0) == -1853)  // duplicateFlavorErr
            #expect(tb_AddDragItemFlavor(drag, 1, styl, 0, 0, 0) == 0)          // promised, no sender
            let n = tb_NewPtr(4, false, true)
            #expect(tb_CountDragItems(drag, n) == 0 && rd16(n) == 1)
            #expect(tb_CountDragItemFlavors(drag, 1, n) == 0 && rd16(n) == 2)
            #expect(tb_GetFlavorType(drag, 1, 2, n) == 0 && rd32(n) == UInt32(bitPattern: styl))
            let buf = tb_NewPtr(16, false, true), size = tb_NewPtr(4, false, true)
            wr32(size, 3)
            #expect(tb_GetFlavorData(drag, 1, TEXT, buf, size, 1) == 0)        // from offset 1, 3 bytes
            #expect(rd32(size) == 3 && rd8(buf) == UInt8(ascii: "e") && rd8(buf + 2) == UInt8(ascii: "l"))
            #expect(tb_GetFlavorDataSize(drag, 1, styl, size) == -1854)        // cantGetFlavorErr
            #expect(tb_GetFlavorDataSize(drag, 2, TEXT, size) == -1851)        // badDragItemErr
            #expect(tb_DisposeDrag(drag) == 0)
            #expect(tb_CountDragItems(drag, n) == -1850)                       // badDragRefErr
        }

        /// ZoomRects and ZoomRegion draw their outlines a tick at a time and
        /// leave the screen as it was.
        @Test func zoomsLeaveTheScreenAsItWas() {
            launchTestApp()
            _ = initToolbox()
            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            let base = rd32(UInt32(LM_ScrnBase)), n = Int(qd_screen_row_bytes()) * Int(h)
            func screen() -> [UInt8] {
                var b = [UInt8](repeating: 0, count: n)
                b.withUnsafeMutableBytes { gget($0.baseAddress, base, UInt32(n)) }
                return b
            }
            let before = screen()
            let from = tb_NewPtr(8, false, true), to = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 132, 132].enumerated() { wr16(from + UInt32(i * 2), UInt16(v)) }
            for (i, v) in [40, 200, 240, 500].enumerated() { wr16(to + UInt32(i * 2), UInt16(v)) }
            let ticks = rd32(UInt32(LM_Ticks))
            #expect(tb_ZoomRects(from, to, 12, 2) == 0)                        // kZoomDecelerate
            #expect(rd32(UInt32(LM_Ticks)) - ticks >= 10)                     // about a tick a step
            #expect(screen() == before)
            let rgn = tb_NewRgn()
            tb_RectRgn(rgn, from)
            #expect(tb_ZoomRegion(rgn, 0x0050_0080, 8, 1) == 0)                // 80 down, 128 across
            #expect(screen() == before)
            #expect(tb_ZoomRects(from, to, 0, 0) == 0)                         // no steps, nothing drawn
            tb_DisposeRgn(rgn)
        }

        /// A drag from the host: its files as 'hfs ' flavors and its text as
        /// 'TEXT', tracked and dropped through the program's handlers; files
        /// a window takes no drags for become an Open Documents event.
        @Test func dragsFromTheHost() {
            let dir = launchTestApp(sizeFlags: 0x0040)
            _ = initToolbox()
            let file = hostFile("note.txt", "hi", in: dir)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 300, 400].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            _ = tb_NewWindow(0, r, tb_NewPtr(2, false, true), 1, 0, 0xFFFF_FFFF, 0, 0)

            // Over the window: the drag exists with both items.
            withCStrings([file.path]) { machine_host_drag(Int32(MACHINE_DRAG_ENTER), 200, 200, $0, 1, "café") }
            drag_host_pump()
            let drag: UInt32 = 0x4452_0001, n = tb_NewPtr(2, false, true), size = tb_NewPtr(4, false, true)
            #expect(tb_CountDragItems(drag, n) == 0 && rd16(n) == 2)
            #expect(tb_GetFlavorDataSize(drag, 1, Int32(bitPattern: fourCC("hfs ")), size) == 0 && rd32(size) == 80)
            let text = tb_NewPtr(16, false, true)
            wr32(size, 16)
            #expect(tb_GetFlavorData(drag, 2, Int32(bitPattern: fourCC("TEXT")), text, size, 0) == 0)
            #expect(rd32(size) == 4 && rd8(text + 3) == 0x8E) // é in MacRoman
            machine_host_drag(Int32(MACHINE_DRAG_LEAVE), 0, 0, nil, 0, nil)
            drag_host_pump()
            #expect(tb_CountDragItems(drag, n) == -1850) // gone

            // Dropped with no receive handler: the file opens.
            withCStrings([file.path]) { machine_host_drag(Int32(MACHINE_DRAG_ENTER), 200, 200, $0, 1, nil) }
            machine_host_drag(Int32(MACHINE_DRAG_DROP), 200, 200, nil, 0, nil)
            drag_host_pump()
            let e = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            func opened() -> Bool { // past the window's activate and update events
                for _ in 0..<8 where tb_GetNextEvent(-1, e) != 0 {
                    if rd16(e) == 23 && rd32(e + 10) == fourCC("odoc") { return true }
                    if rd16(e) == 6 { tb_BeginUpdate(rd32(e + 2)); tb_EndUpdate(rd32(e + 2)) }
                }
                return false
            }
            #expect(opened())

            // With one (68k: keep the drag reference, take the drop), nothing opens.
            let got = tb_NewPtr(4, false, true)
            let words: [UInt16] = [0x23EF, 0x0004, UInt16(got >> 16), UInt16(got & 0xFFFF), // MOVE.L 4(SP),got
                                   0x426F, 0x0010,                                        // CLR.W 16(SP): noErr
                                   0x205F, 0x4FEF, 0x000C, 0x4ED0]                         // pop and return
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            #expect(tb_InstallReceiveHandler(code, 0, 0) == 0)
            withCStrings([file.path]) { machine_host_drag(Int32(MACHINE_DRAG_ENTER), 200, 200, $0, 1, nil) }
            machine_host_drag(Int32(MACHINE_DRAG_DROP), 210, 210, nil, 0, nil)
            drag_host_pump()
            #expect(rd32(got) != 0)
            #expect(!opened())
        }

        /// What the host was given, as it would copy it.
        struct Exported {
            var reps: [[(kind: Int32, type: UInt32, data: Data?)]] = []
            var at = (h: Int16(0), v: Int16(0))
        }
        nonisolated(unsafe) static var exported = Exported()
        nonisolated(unsafe) static var fetched: [String] = []
        nonisolated(unsafe) static var hostSide: (() -> Void)?

        /// The host's side of a drag out: copy what it was given, then (from
        /// another thread, as the host's main thread would) ask for data and
        /// end the drag.
        static let dragOutHandler: machine_drag_out_fn = { x, _ in
            guard let x = x?.pointee else { return false }
            var e = Exported()
            e.at = (x.h, x.v)
            for i in 0..<Int(x.nitems) {
                let item = x.items[i]
                e.reps.append((0..<Int(item.nreps)).map { k in
                    let r = item.reps[k]
                    return (r.kind, r.type, r.data.map { Data(bytes: $0, count: r.size) })
                })
            }
            exported = e
            if let side = hostSide { Thread.detachNewThread(side) }
            return true
        }

        static func fetch(_ item: Int32, _ kind: Int32, folder: String? = nil) -> String? {
            var out = machine_drag_data()
            guard machine_drag_out_data(item, kind, folder, &out) else { return nil }
            defer { free(out.data) }
            return String(decoding: Data(bytes: out.data, count: out.size), as: UTF8.self)
        }

        /// A drag that leaves the windows goes to the host with its flavors
        /// converted, promised data fetched through the send procedure on
        /// request, and the drop location the host gives.
        @Test func dragsOutToTheHost() {
            let dir = launchTestApp()
            _ = initToolbox()
            let file = hostFile("note.txt", "hi", in: dir)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 300, 400].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            _ = tb_NewWindow(0, r, tb_NewPtr(2, false, true), 1, 0, 0xFFFF_FFFF, 0, 0)
            let refp = tb_NewPtr(4, false, true)
            #expect(tb_NewDrag(refp) == 0)
            let drag = rd32(refp)
            func flavor(_ item: UInt32, _ type: String, _ bytes: [UInt8]?) {
                var p: UInt32 = 0
                if let bytes {
                    p = tb_NewPtr(Int32(max(bytes.count, 1)), false, false)
                    for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
                }
                #expect(tb_AddDragItemFlavor(drag, item, Int32(bitPattern: fourCC(type)), p,
                                             Int32(bytes?.count ?? 0), 0) == 0)
            }
            // 1: styled text ("caf" plain, "é!" bold in Geneva 12, red)
            flavor(1, "TEXT", [0x63, 0x61, 0x66, 0x8E, 0x21, 0x0D, 0x7B])
            var styl: [UInt8] = [0, 2]
            for (start, face, red) in [(0, 0, 0), (3, 1, 0xFFFF)] {
                styl += [0, 0, 0, UInt8(start), 0, 16, 0, 12, 0, 3, UInt8(face), 0, 0, 12,
                         UInt8(red >> 8), UInt8(red & 0xFF), 0, 0, 0, 0]
            }
            flavor(1, "styl", styl)
            // 2: a file
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(file.path, spec)
            var hfs = [UInt8](repeating: 0, count: 10 + Int(FSSpec_SIZEOF))
            for i in 0..<Int(FSSpec_SIZEOF) { hfs[10 + i] = rd8(spec + UInt32(i)) }
            flavor(2, "hfs ", hfs)
            // 3: promised text, from the send procedure
            flavor(3, "TEXT", nil)
            let late = tb_NewPtr(4, false, false)
            for (i, b) in "late".utf8.enumerated() { wr8(late + UInt32(i), b) }
            let words: [UInt16] = [0x4267,                         // CLR.W -(SP): result
                                   0x2F2F, 0x0006,                 // theDrag
                                   0x2F2F, 0x000E,                 // theItemRef
                                   0x2F2F, 0x001A,                 // theType
                                   0x4879, UInt16(late >> 16), UInt16(late & 0xFFFF),
                                   0x2F3C, 0x0000, 0x0004,         // dataSize 4
                                   0x42A7,                         // dataOffset 0
                                   0x303C, 0x0009, 0xABED,         // SetDragItemFlavorData
                                   0x548F,                         // ADDQ.L #2,SP
                                   0x426F, 0x0014,                 // CLR.W 20(SP): noErr
                                   0x205F, 0x4FEF, 0x0010, 0x4ED0] // pop and return
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            #expect(tb_SetDragSendProc(drag, code, 0) == 0)
            // 4: a picture (version 1, a 12 by 10 frame, a painted rectangle)
            flavor(4, "PICT", [0, 22, 0, 0, 0, 0, 0, 10, 0, 12, 0x11, 0x01,
                               0x31, 0, 0, 0, 0, 0, 5, 0, 12, 0xFF])

            Self.fetched = []
            Self.hostSide = {
                Self.fetched.append(Self.fetch(2, Int32(MACHINE_FLAVOR_TEXT)) ?? "-")
                var image = machine_drag_data()
                if machine_drag_out_data(3, Int32(MACHINE_FLAVOR_IMAGE), nil, &image) {
                    Self.fetched.append("\(image.width)x\(image.height) \(image.data[0]) \(image.data[9 * 48])")
                    free(image.data)
                }
                machine_drag_out_end(Int32(MACHINE_DRAG_OUT_DROPPED), 0, 0, dir.path)
            }
            machine_set_drag_out_handler(Self.dragOutHandler, nil)
            defer { machine_set_drag_out_handler(nil, nil) }
            machine_post_mouse(150, 150, 1)
            #expect(!machine_drag_out_tracking())
            machine_drag_out_pointer(true)
            let event = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            wr32(event + 10, 150 << 16 | 150)
            #expect(tb_TrackDrag(drag, event, 0) == 0)
            machine_post_mouse(150, 150, 0)

            let e = Self.exported
            #expect(e.at == (150, 150))
            #expect(e.reps.count == 4)
            let rtf = e.reps[0].first { $0.kind == Int32(MACHINE_FLAVOR_RTF) }?.data
                .map { String(decoding: $0, as: UTF8.self) } ?? ""
            #expect(rtf.hasPrefix("{\\rtf1\\mac"))
            #expect(rtf.contains("Geneva;"))
            #expect(rtf.contains("\\red255\\green0\\blue0;"))
            #expect(rtf.contains("{\\f0\\fs24\\cf1 caf}"))
            #expect(rtf.contains("{\\f0\\fs24\\cf2\\b \\'8e!\\par\n\\{}"))
            let text = e.reps[0].first { $0.kind == Int32(MACHINE_FLAVOR_TEXT) }?.data
            #expect(text == Data("café!\n{".utf8))
            #expect(e.reps[1].map(\.kind) == [Int32(MACHINE_FLAVOR_FILE)])
            let path = e.reps[1][0].data.map { String(decoding: $0, as: UTF8.self) } ?? ""
            #expect(URL(fileURLWithPath: path).resolvingSymlinksInPath() == file.resolvingSymlinksInPath())
            #expect(e.reps[2].count == 1 && e.reps[2][0].data == nil) // promised: on request
            #expect(e.reps[3].map(\.kind) == [Int32(MACHINE_FLAVOR_IMAGE)])
            #expect(Self.fetched == ["late", "12x10 0 255"]) // black above, white below
            // Dropped in a host folder: an alias to it.
            let loc = tb_NewPtr(8, false, true)
            #expect(tb_GetDropLocation(drag, loc) == 0 && rd32(loc) == fourCC("alis") && rd32(loc + 4) != 0)
            #expect(Self.fetch(0, Int32(MACHINE_FLAVOR_TEXT)) == nil) // over: no more requests
            #expect(tb_DisposeDrag(drag) == 0)
        }

        /// Brought back over a window and dropped there, the drag goes to
        /// the window's receive handlers; cancelled, TrackDrag says so.
        @Test func dragsOutAndBack() {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [100, 100, 300, 400].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            _ = tb_NewWindow(0, r, tb_NewPtr(2, false, true), 1, 0, 0xFFFF_FFFF, 0, 0)
            let got = tb_NewPtr(4, false, true)
            let words: [UInt16] = [0x23EF, 0x0004, UInt16(got >> 16), UInt16(got & 0xFFFF), // MOVE.L 4(SP),got
                                   0x426F, 0x0010,                                        // CLR.W 16(SP): noErr
                                   0x205F, 0x4FEF, 0x000C, 0x4ED0]                         // pop and return
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            #expect(tb_InstallReceiveHandler(code, 0, 0) == 0)
            let refp = tb_NewPtr(4, false, true)
            #expect(tb_NewDrag(refp) == 0)
            let drag = rd32(refp)
            let p = tb_NewPtr(2, false, false)
            wr16(p, 0x6869)
            #expect(tb_AddDragItemFlavor(drag, 1, Int32(bitPattern: fourCC("TEXT")), p, 2, 0) == 0)
            machine_set_drag_out_handler(Self.dragOutHandler, nil)
            defer { machine_set_drag_out_handler(nil, nil) }
            let event = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            wr32(event + 10, 150 << 16 | 150)

            Self.hostSide = {
                machine_drag_out_over(true, 200, 200)
                machine_drag_out_end(Int32(MACHINE_DRAG_OUT_INSIDE), 210, 210, nil)
            }
            machine_post_mouse(150, 150, 1)
            machine_drag_out_pointer(true)
            #expect(tb_TrackDrag(drag, event, 0) == 0)
            #expect(rd32(got) == drag)
            let loc = tb_NewPtr(8, false, true)
            #expect(tb_GetDropLocation(drag, loc) == 0 && rd32(loc) == fourCC("null"))

            Self.hostSide = { machine_drag_out_end(Int32(MACHINE_DRAG_OUT_CANCELLED), 0, 0, nil) }
            machine_drag_out_pointer(true)
            #expect(tb_TrackDrag(drag, event, 0) == -128) // userCanceledErr
            machine_post_mouse(150, 150, 0)
            #expect(tb_DisposeDrag(drag) == 0)
        }

        @Test func hiliteRegionCoversTheSelection() {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 300, 400].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let te = tb_TENew(r, r)
            let p = tb_NewPtr(11, false, false)
            for (i, b) in "hello world".utf8.enumerated() { wr8(p + UInt32(i), b) }
            tb_TEInsert(p, 11, te)
            let rgn = tb_NewRgn()
            tb_TESetSelect(3, 3, te)
            #expect(tb_TEGetHiliteRgn(rgn, te) == 0)
            #expect(tb_EmptyRgn(rgn) != 0)                                     // a caret: nothing
            tb_TESetSelect(0, 5, te)
            #expect(tb_TEGetHiliteRgn(rgn, te) == 0)
            let box = rd32(rgn) + 2
            #expect(rd16(box + 2) == 0 && rd16(box + 6) > 20 && rd16(box + 6) < 60) // "hello"
        }
    }

    /// Styled TextEdit: runs, the style table, the null style, line
    /// heights and style scraps, in Apple's structures.
    @Suite struct StyledTextEdit {
        func newStyled() -> UInt32 {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 300, 400].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            return tb_TEStyleNew(r, r)
        }
        func insert(_ s: String, _ te: UInt32) {
            let p = tb_NewPtr(Int32(s.utf8.count), false, false)
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            tb_TEInsert(p, Int32(s.utf8.count), te)
        }
        func style(face: UInt8 = 0, size: Int16 = 0) -> UInt32 {
            let ts = tb_NewPtr(Int32(TextStyle_SIZEOF), false, true)
            wr8(ts + 2, face)
            wr16(ts + 4, UInt16(bitPattern: size))
            return ts
        }
        func faceAt(_ off: Int16, _ te: UInt32) -> UInt8 {
            let ts = tb_NewPtr(Int32(TextStyle_SIZEOF), false, true)
            tb_TEGetStyle(off, ts, 0, 0, te)
            return rd8(ts + 2)
        }
        func runs(_ te: UInt32) -> Int { Int(rd16(rd32(tb_TEGetStyleHandle(te)))) }

        @Test func runsFollowStylesAndEdits() {
            let te = newStyled()
            #expect(Int16(bitPattern: rd16(rd32(te) + UInt32(TERec_txSize_off))) == -1)
            insert("Hello world", te)
            tb_TESetSelect(0, 5, te)
            tb_TESetStyle(2 | 32, style(face: 1), 0, te) // doFace + doToggle: bold
            #expect(runs(te) == 2)
            #expect(faceAt(0, te) == 1 && faceAt(4, te) == 1 && faceAt(5, te) == 0)
            #expect(tb_TENumStyles(0, 11, te) == 2)
            tb_TESetStyle(2 | 32, style(face: 1), 0, te) // toggled off again
            #expect(runs(te) == 1 && faceAt(0, te) == 0)
            tb_TESetStyle(2 | 32, style(face: 1), 0, te)

            // Typing after the bold word takes its style.
            tb_TESetSelect(5, 5, te)
            tb_TEKey(0x58, te) // X
            #expect(runs(te) == 2 && faceAt(5, te) == 1 && faceAt(6, te) == 0)

            // The null style: set with nothing selected, typed text has it.
            let len = Int16(bitPattern: rd16(rd32(te) + UInt32(TERec_teLength_off)))
            tb_TESetSelect(Int32(len), Int32(len), te)
            let plainHeight = tb_TEGetHeight(1, 1, te)
            tb_TESetStyle(4, style(size: 24), 0, te) // doSize
            tb_TEKey(0x21, te) // !
            #expect(runs(te) == 3)
            let ts = tb_NewPtr(Int32(TextStyle_SIZEOF), false, true)
            tb_TEGetStyle(len, ts, 0, 0, te)
            #expect(rd16(ts + 4) == 24)
            #expect(plainHeight > 0 && tb_TEGetHeight(1, 1, te) >= plainHeight)

            // Deleting across runs.
            tb_TESetSelect(3, 8, te)
            tb_TEDelete(te)
            let sr = rd32(tb_TEGetStyleHandle(te))
            let n = Int(rd16(sr)), newLen = rd16(rd32(te) + UInt32(TERec_teLength_off))
            #expect(rd16(sr + 20 + UInt32(n) * 4) == newLen + 1) // the end marker
            #expect(faceAt(0, te) == 1 && faceAt(3, te) == 0)

            // A style scrap of everything, into another record.
            tb_TESetSelect(0, Int32(newLen), te)
            let scrap = tb_TEGetStyleScrapHandle(te)
            #expect(Int(rd16(rd32(scrap))) == runs(te))
            let r2 = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 300, 400].enumerated() { wr16(r2 + UInt32(i * 2), UInt16(v)) }
            let te2 = tb_TEStyleNew(r2, r2)
            let text = tb_TEGetText(te)
            tb_HLock(text)
            tb_TEStyleInsert(rd32(text), Int32(newLen), scrap, te2)
            #expect(runs(te2) == runs(te))
            #expect(faceAt(0, te2) == 1 && faceAt(3, te2) == 0)
        }
    }

    @Suite struct Alerts {
        /// ALRT 128 with DITL 128: OK (1), Cancel (2), and text with ^0.
        static func alertResources(stages: UInt16 = 0x5555) -> [TestResource] {
            func item(_ type: UInt8, _ text: String) -> [UInt8] {
                var d = cat(be32(0), be16(0), be16(0), be16(20), be16(60), [type, UInt8(text.utf8.count)], Array(text.utf8))
                if text.utf8.count % 2 == 1 { d.append(0) }
                return d
            }
            let ditl = cat(be16(2), item(4, "OK"), item(4, "Cancel"), item(8, "Cannot open “^0”."))
            let alrt = cat(be16(40), be16(40), be16(140), be16(340), be16(128), be16(Int(stages)))
            return [TestResource(type: "ALRT", id: 128, data: alrt), TestResource(type: "DITL", id: 128, data: ditl)]
        }

        nonisolated(unsafe) static var seen: (message: String, buttons: [String], defaultItem: Int16)?
        nonisolated(unsafe) static var answer: Int32 = 0

        func install() {
            Self.seen = nil
            machine_set_alert_handler({ a, _ in
                guard let a = a?.pointee else { return 0 }
                let msg = withUnsafeBytes(of: a.message) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
                var titles: [String] = []
                withUnsafeBytes(of: a.buttons) { raw in
                    let b = raw.bindMemory(to: machine_alert_button.self)
                    for i in 0..<Int(a.nbuttons) {
                        titles.append(withUnsafeBytes(of: b[i].title) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) })
                    }
                }
                Alerts.seen = (msg, titles, a.default_item)
                return Alerts.answer
            }, nil)
        }

        @Test func alertTextAndButtonsGoToTheHost() {
            launchTestApp(Self.alertResources())
            install()
            defer { machine_set_alert_handler(nil, nil) }
            tb_ParamText(guestString("Report"), 0, 0, 0)
            Self.answer = 2
            #expect(tb_StopAlert(128, 0) == 2)
            #expect(Self.seen?.buttons == ["OK", "Cancel"])
            #expect(Self.seen?.defaultItem == 1)
            #expect(Self.seen?.message.contains("Report") == true)
            #expect(Self.seen?.message.contains("^0") == false)
        }

        /// StandardAlert: the error and explanation, and the buttons its
        /// parameter block asks for (-1 for a standard title).
        @Test func standardAlertGoesToTheHost() {
            launchTestApp()
            install()
            defer { machine_set_alert_handler(nil, nil) }
            let param = tb_NewPtr(24, false, true)
            wr32(param + 6, 0xFFFF_FFFF)                 // defaultText: "OK"
            wr32(param + 10, 0xFFFF_FFFF)                // cancelText: "Cancel"
            wr16(param + 18, 1)                          // defaultButton
            let hit = tb_NewPtr(2, false, true)
            Self.answer = 2
            #expect(dialog_standard_alert(2, guestString("Disk full"), guestString("Try again."), param, hit) == 0)
            #expect(rd16(hit) == 2)
            #expect(Self.seen?.buttons == ["OK", "Cancel"])
            #expect(Self.seen?.message.contains("Disk full") == true)
            #expect(Self.seen?.message.contains("Try again.") == true)
            Self.answer = 0
            #expect(dialog_standard_alert(0, guestString("Oops"), 0, 0, hit) == 0)
            #expect(Self.seen?.buttons == ["OK"] && rd16(hit) == 1)
        }

        @Test func withoutAHostTheDefaultButtonIsChosen() {
            // Stage bits 0x8 make item 2 the default.
            launchTestApp(Self.alertResources(stages: 0xDDDD))
            machine_set_alert_handler(nil, nil)
            #expect(tb_Alert(128, 0) == 2)
        }

        @Test func stagesWithoutABoxReturnMinusOne() {
            launchTestApp(Self.alertResources(stages: 0x0000))
            install()
            defer { machine_set_alert_handler(nil, nil) }
            #expect(tb_NoteAlert(128, 0) == -1)
            #expect(Self.seen == nil)
        }

        @Test func numberStrings() {
            launchTestApp()
            let s = tb_NewPtr(256, false, true)
            tb_NumToString(-1234, s)
            #expect(pstring(at: s) == "-1234")
            var v: UInt32 = 0
            guestPString("4096", at: s)
            tb_StringToNum(s, &v)
            #expect(Int32(bitPattern: v) == 4096)
        }
    }

    @Suite struct StandardFile {
        nonisolated(unsafe) static var path = ""
        nonisolated(unsafe) static var request: (save: Bool, types: [UInt32], name: String)?

        func install() {
            Self.request = nil
            machine_set_file_handler({ rq, out, size, _ in
                guard let rq = rq?.pointee, let out else { return false }
                let types = withUnsafeBytes(of: rq.types) { Array($0.bindMemory(to: UInt32.self).prefix(Int(rq.ntypes))) }
                let name = withUnsafeBytes(of: rq.default_name) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
                StandardFile.request = (rq.save, types, name)
                if StandardFile.path.isEmpty { return false }
                _ = StandardFile.path.withCString { strlcpy(out, $0, size) }
                return true
            }, nil)
        }

        @Test func openReplyNamesTheChosenFile() {
            let dir = launchTestApp()
            install()
            defer { machine_set_file_handler(nil, nil) }
            Self.path = hostFile("chapter.txt", "text", in: dir).path
            let types = tb_NewPtr(8, false, true)
            wr32(types, fourCC("TEXT"))
            wr32(types + 4, fourCC("ttro"))
            let reply = tb_NewPtr(Int32(StandardFileReply_SIZEOF), false, true)
            tb_CustomGetFile(0, 2, types, reply, 400, 0xFFFF_FFFF, 0, 0, 0, 0, 0)
            #expect(Self.request?.save == false)
            #expect(Self.request?.types == [fourCC("TEXT"), fourCC("ttro")])
            #expect(rd8(reply + UInt32(StandardFileReply_sfGood_off)) == 1)
            #expect(rd32(reply + UInt32(StandardFileReply_sfType_off)) == fourCC("TEXT"))
            #expect(pstring(at: reply + UInt32(StandardFileReply_sfFile_off + FSSpec_name_off)) == "chapter.txt")
        }

        @Test func saveReplySaysWhetherItReplaces() {
            let dir = launchTestApp()
            install()
            defer { machine_set_file_handler(nil, nil) }
            Self.path = dir.appendingPathComponent("new.txt").path
            let reply = tb_NewPtr(Int32(StandardFileReply_SIZEOF), false, true)
            tb_CustomPutFile(guestString("Save as:"), guestString("Untitled"), reply, 399, 0xFFFF_FFFF, 0, 0, 0, 0, 0)
            #expect(Self.request?.save == true)
            #expect(Self.request?.name == "Untitled")
            #expect(rd8(reply + UInt32(StandardFileReply_sfGood_off)) == 1)
            #expect(rd8(reply + UInt32(StandardFileReply_sfReplacing_off)) == 0)
            Self.path = hostFile("old.txt", "x", in: dir).path
            tb_StandardPutFile(0, guestString("old.txt"), reply)
            #expect(rd8(reply + UInt32(StandardFileReply_sfReplacing_off)) == 1)
        }

        @Test func oldStyleReplyUsesAWorkingDirectory() {
            let dir = launchTestApp()
            install()
            defer { machine_set_file_handler(nil, nil) }
            Self.path = hostFile("old style.txt", "x", in: dir).path
            let reply = tb_NewPtr(Int32(SFReply_SIZEOF), false, true)
            tb_SFGetFile(0x0050_0050, 0, 0, -1, 0, 0, reply)
            #expect(rd8(reply + UInt32(SFReply_good_off)) == 1)
            #expect(Int16(bitPattern: rd16(reply + UInt32(SFReply_vRefNum_off))) <= -32000)
            #expect(pstring(at: reply + UInt32(SFReply_fName_off)) == "old style.txt")
        }

        @Test func cancelAndNoHostLeaveGoodFalse() {
            launchTestApp()
            let reply = tb_NewPtr(Int32(StandardFileReply_SIZEOF), false, true)
            machine_set_file_handler(nil, nil)
            tb_StandardGetFile(0, -1, 0, reply)
            #expect(rd8(reply) == 0)
            install()
            defer { machine_set_file_handler(nil, nil) }
            Self.path = ""
            tb_StandardGetFile(0, -1, 0, reply)
            #expect(rd8(reply) == 0)
        }
    }

    @Suite struct HostTextFiles {
        /// A TEXT file saved under a text extension stays a plain host file:
        /// UTF-8 with LF line ends, and no AppleDouble sidecar.
        @Test func savedTextFilesStayPlain() throws {
            let dir = launchTestApp()
            let path = dir.appendingPathComponent("saved.txt").path
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            #expect(tb_FSpCreate(spec, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("TEXT")), 0) == 0)
            let finfo = tb_NewPtr(16, false, true)
            #expect(tb_FSpGetFInfo(spec, finfo) == 0)
            #expect(tb_FSpSetFInfo(spec, finfo) == 0)          // as apps do after creating
            let rn = tb_NewPtr(2, false, true)
            #expect(tb_FSpOpenDF(spec, 3, rn) == 0)
            let text: [UInt8] = Array("caf".utf8) + [0x8E] + [0x0D] + Array("line two".utf8) // café CR line two
            let buf = tb_NewPtr(Int32(text.count), false, true)
            for (i, b) in text.enumerated() { wr8(buf + UInt32(i), b) }
            let pb = tb_NewPtr(Int32(ParamBlockRec_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), rd16(rn))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), UInt32(text.count))
            #expect(tb_PBWrite(pb, false) == 0)
            #expect(tb_PBClose(pb, false) == 0)
            #expect(try String(contentsOfFile: path, encoding: .utf8) == "café\nline two")
            #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("._saved.txt").path))
            #expect(rd32(finfo) == fourCC("TEXT"))
        }

        /// A plain host file's type comes from its extension, so Open
        /// dialogs that list only their own types (a .mod player) show it.
        @Test func extensionsGiveTypes() {
            let dir = launchTestApp()
            #expect(fm_type_of_path(hostFile("song.MOD", "M.K.", in: dir).path) == fourCC("STrk"))
            #expect(fm_type_of_path(hostFile("tune.mid", "MThd", in: dir).path) == fourCC("Midi"))
            #expect(fm_type_of_path(hostFile("readme.txt", "x", in: dir).path) == fourCC("TEXT"))
        }

        @Test func otherTypesKeepTheirSidecar() {
            let dir = launchTestApp()
            let path = dir.appendingPathComponent("stationery.txt").path
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            #expect(tb_FSpCreate(spec, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("ttro")), 0) == 0)
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("._stationery.txt").path))
        }
    }

    /// The host's native menu bar: opening a menu lets the app adjust its
    /// menus, and MenuSelect waits until the host menu closes.
    @Suite struct HostMenus {
        func menuChoice(_ menu: Int32, _ item: Int32) -> Int32? { menu << 16 | item }

        func setUpMenus() {
            _ = initToolbox()
            for (id, title, items) in [(2, "File", "New;Open…;(-;Close"), (3, "Edit", "Undo;(-;Cut;Copy;Paste")] {
                let m = tb_NewMenu(Int16(id), guestString(title))
                tb_AppendMenu(m, guestString(items))
                tb_InsertMenu(m, 0)
            }
        }

        /// Runs MenuSelect on its own thread, as the emulator thread would.
        func menuSelectInBackground() -> (wait: (TimeInterval) -> Int32?, done: () -> Void) {
            let sem = DispatchSemaphore(value: 0)
            nonisolated(unsafe) var result: Int32 = -1
            let t = Thread {
                result = tb_MenuSelect(0x0001_0001)
                sem.signal()
            }
            t.start()
            return ({ timeout in sem.wait(timeout: .now() + timeout) == .success ? result : nil }, {})
        }

        @Test func choiceMadeInAnOpenHostMenuIsReturned() {
            launchTestApp()
            setUpMenus()
            machine_menu_tracking_begin()                // the app has not called MenuSelect yet
            let select = menuSelectInBackground()
            usleep(50_000)
            machine_choose_menu(2, 1)
            machine_menu_tracking_end()
            #expect(select.wait(2) == menuChoice(2, 1))
        }

        /// Sliding from one menu to the next opens the second before the first
        /// closes; one choice must still come through promptly.
        @Test func movingBetweenMenusKeepsOneSession() {
            launchTestApp()
            setUpMenus()
            let select = menuSelectInBackground()
            machine_menu_tracking_begin()                // File opens
            usleep(50_000)
            machine_menu_tracking_begin()                // Edit opens before File's close is handled
            machine_choose_menu(3, 4)                    // Copy
            machine_menu_tracking_end()
            #expect(select.wait(2) == menuChoice(3, 4))
        }

        @Test func closingWithoutAChoiceReturnsZero() {
            launchTestApp()
            setUpMenus()
            let select = menuSelectInBackground()
            machine_menu_tracking_begin()
            usleep(50_000)
            machine_menu_tracking_begin()
            machine_menu_tracking_end()
            #expect(select.wait(2) == 0)
        }

        @Test func disabledItemsAreNotChosen() {
            launchTestApp()
            setUpMenus()
            tb_DisableItem(tb_GetMenuHandle(2), 1)
            let select = menuSelectInBackground()
            machine_menu_tracking_begin()
            machine_choose_menu(2, 1)
            machine_menu_tracking_end()
            #expect(select.wait(2) == 0)
        }
    }
}

extension BlueBox {
    @Suite struct AppleEventBuilding {
        @Test func listsRecordsAndSendingToOurselves() {
            launchTestApp()
            let list = tb_NewPtr(8, false, true)
            #expect(tb_AECreateList(0, 0, 0, list) == 0)
            let v = tb_NewPtr(4, false, true)
            for i in 1...3 {
                wr32(v, UInt32(i * 10))
                #expect(tb_AEPutPtr(list, 0, Int32(bitPattern: fourCC("long")), v, 4) == 0)
            }
            wr32(v, 99)
            #expect(tb_AEPutPtr(list, 2, Int32(bitPattern: fourCC("long")), v, 4) == 0) // replace item 2
            let count = tb_NewPtr(4, false, true)
            _ = tb_AECountItems(list, count)
            #expect(rd32(count) == 3)
            let out = tb_NewPtr(4, false, true), type = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true)
            _ = tb_AEGetNthPtr(list, 2, Int32(bitPattern: fourCC("long")), 0, type, out, 4, size)
            #expect(rd32(out) == 99)
            #expect(tb_AEDeleteItem(list, 1) == 0)
            _ = tb_AEGetNthPtr(list, 1, Int32(bitPattern: fourCC("long")), 0, type, out, 4, size)
            #expect(rd32(out) == 99)
            _ = tb_AEDisposeDesc(list)

            // An event to ourselves reaches our handler, which counts calls.
            let calls = tb_NewPtr(4, false, true)
            // FUNCTION handler(event, reply: AppleEvent; refcon): OSErr
            let words: [UInt16] = [0x205F, 0x4FEF, 0x000C, 0x52B9, UInt16(calls >> 16), UInt16(calls & 0xFFFF),
                                   0x4257, 0x4ED0] // MOVEA.L (SP)+,A0; LEA 12(SP),SP; ADDQ.L #1,calls; CLR.W (SP); JMP (A0)
            let handler = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(handler + UInt32(i * 2), w) }
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            _ = tb_AEInstallEventHandler(Int32(bitPattern: fourCC("test")), Int32(bitPattern: fourCC("ping")), handler, 0, 0)
            let target = tb_NewPtr(8, false, true), psn = tb_NewPtr(8, false, true)
            wr32(psn + 4, 2) // kCurrentProcess
            _ = tb_AECreateDesc(Int32(bitPattern: fourCC("psn ")), psn, 8, target)
            let evt = tb_NewPtr(8, false, true), reply = tb_NewPtr(8, false, true)
            #expect(tb_AECreateAppleEvent(Int32(bitPattern: fourCC("test")), Int32(bitPattern: fourCC("ping")),
                                          target, -1, 0, evt) == 0)
            #expect(tb_AESend(evt, reply, 3, 0, -1, 0, 0) == 0)
            #expect(rd32(calls) == 1)
        }

        /// Events to the Finder that open items: an application is launched,
        /// a document opened with the application of its creator.
        @Test func finderOpensItems() throws {
            let dir = launchTestApp()
            _ = initToolbox()
            func create(_ name: String, _ type: String) -> UInt32 {
                let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
                fm_spec_for_path(dir.appendingPathComponent(name).path, spec)
                _ = tb_FSpCreate(spec, Int32(bitPattern: fourCC("TeSt")), Int32(bitPattern: fourCC(type)), 0)
                return spec
            }
            let app = create("Tester", "APPL"), doc = create("Tester Doc", "TDoc")
            _ = tb_PBDTAddAPPL(tb_NewPtr(128, false, true), false) // scan again
            launched = []
            machine_set_launch_handler({ path, docs, n, _ in
                launched.append(([String(cString: path!)] + (0..<Int(n)).map { String(cString: docs![$0]!) })
                    .map { ($0 as NSString).lastPathComponent }.joined(separator: " "))
                return true
            }, nil)
            defer { machine_set_launch_handler(nil, nil) }

            let target = tb_NewPtr(8, false, true), sig = tb_NewPtr(4, false, true)
            wr32(sig, fourCC("MACS"))
            _ = tb_AECreateDesc(Int32(bitPattern: fourCC("sign")), sig, 4, target)
            let evt = tb_NewPtr(8, false, true), reply = tb_NewPtr(8, false, true)
            #expect(tb_AECreateAppleEvent(Int32(bitPattern: fourCC("aevt")), Int32(bitPattern: fourCC("odoc")),
                                          target, -1, 0, evt) == 0)
            let list = tb_NewPtr(8, false, true)
            _ = tb_AECreateList(0, 0, 0, list)
            _ = tb_AEPutPtr(list, 0, Int32(bitPattern: fourCC("fss ")), app, Int32(FSSpec_SIZEOF))
            _ = tb_AEPutPtr(list, 0, Int32(bitPattern: fourCC("fss ")), doc, Int32(FSSpec_SIZEOF))
            _ = tb_AEPutParamDesc(evt, Int32(bitPattern: fourCC("----")), list)
            #expect(tb_AESend(evt, reply, 1, 0, -1, 0, 0) == 0)
            #expect(launched == ["Tester", "Tester Tester Doc"])
        }
    }
}

private nonisolated(unsafe) var launched: [String] = []
