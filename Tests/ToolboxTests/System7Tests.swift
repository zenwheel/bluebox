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

extension Classic {
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
