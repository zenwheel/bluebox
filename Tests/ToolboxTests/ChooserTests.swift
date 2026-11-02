import CCore
import ForkStore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Chooser: our desk accessory in the System file, its devices,
    /// and what programs see of its choices.
    @Suite struct Chooser {
        nonisolated(unsafe) static var changes: [(String, String)] = []
        nonisolated(unsafe) static var printers: [String] = []
        nonisolated(unsafe) static var printed: (destination: Int32, printer: String, folder: String)?
        nonisolated(unsafe) static var counts = (pages: 0, texts: 0, images: 0)

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// A machine with the Chooser's settings, a program running, and a
        /// host that has `printers` and records what the Chooser changes.
        func boot(printer: String = "panel", user: String = "Ada", folder: String = "", appletalk: String = "off",
                  printers: [String] = ["Test_Printer", "Other_One"]) {
            Chooser.changes = []
            Chooser.printers = printers
            Chooser.printed = nil
            machine_set_user_name(user)
            #expect(machine_set_printer(printer))
            machine_set_print_folder(folder)
            #expect(machine_set_appletalk(appletalk))
            machine_set_chooser_host({ names, max, _ in
                guard let names else { return 0 }
                for (i, n) in Chooser.printers.prefix(Int(max)).enumerated() {
                    let bytes = Array(n.utf8) + [0]
                    withUnsafeMutableBytes(of: &names[i]) { $0.copyBytes(from: bytes) }
                }
                return Int32(min(Chooser.printers.count, Int(max)))
            }, { key, value, _ in
                Chooser.changes.append((String(cString: key!), String(cString: value!)))
            }, nil)
            launchTestApp()
            _ = initToolbox()
            guestStack()
        }

        /// The settings back as other tests expect them.
        func restore() {
            machine_set_printer("panel")
            machine_set_user_name("")
            machine_set_print_folder("")
            machine_set_appletalk("local")
            machine_set_chooser_host(nil, nil, nil)
        }

        /// The Chooser's window: its refnum, and where its content is.
        func open() -> (Int16, UInt32, Int, Int) {
            let rn = Int16(machine_open_chooser())
            let w = tb_FrontWindow()
            // activated, as the program's event loop would pass it on
            let ev = tb_NewPtr(16, false, true)
            wr16(ev, 8)
            wr32(ev + 2, w)
            wr16(ev + 14, 1)
            _ = tb_SystemEvent(ev)
            // The content's top left, in global coordinates.
            let pt = tb_NewPtr(4, false, true)
            wr32(pt, 0)
            tb_SetPort(w)
            tb_LocalToGlobal(pt)
            let v = Int(Int16(bitPattern: rd16(pt))), h = Int(Int16(bitPattern: rd16(pt + 2)))
            return (rn, w, h, v)
        }

        /// A click at a point in the window (local coordinates).
        func click(_ w: UInt32, _ origin: (Int, Int), _ x: Int, _ y: Int) {
            let ev = tb_NewPtr(16, false, true)
            let gx = origin.0 + x, gy = origin.1 + y
            machine_post_mouse(Int32(gx), Int32(gy), 1)
            machine_post_mouse(Int32(gx), Int32(gy), 0)
            wr16(ev, 1)
            wr32(ev + 2, 0)
            wr32(ev + 10, UInt32(gy) << 16 | UInt32(gx))
            wr16(ev + 14, 0)
            tb_SystemClick(ev, w)
            tb_FlushEvents(-1, 0)
        }

        func key(_ c: Character) {
            let ev = tb_NewPtr(16, false, true)
            wr16(ev, 3)
            wr32(ev + 2, UInt32(c.asciiValue!))
            wr16(ev + 14, 0)
            _ = tb_SystemEvent(ev)
        }

        /// The update a program would pass on.
        func update(_ w: UInt32) {
            let ev = tb_NewPtr(16, false, true)
            wr16(ev, 6)
            wr32(ev + 2, w)
            _ = tb_SystemEvent(ev)
        }

        func string(_ id: Int16) -> String {
            let h = tb_GetString(id)
            return h != 0 && rd32(h) != 0 ? pstring(at: rd32(h)) : ""
        }

        /// Print one page through the Printing Manager; what the host got.
        func printPage() {
            machine_set_print_handlers({ job, _ in
                guard let job else { return false }
                let printer = withUnsafeBytes(of: job.pointee.printer) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
                let folder = withUnsafeBytes(of: job.pointee.folder) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
                Chooser.printed = (job.pointee.destination, printer, folder)
                return true
            }, nil, nil)
            defer { machine_set_print_handlers(nil, nil, nil) }
            tb_PrOpen()
            let h = tb_NewHandle(120, false, true)
            tb_PrintDefault(h)
            let port = tb_PrOpenDoc(h, 0, 0)
            tb_PrOpenPage(port, 0)
            let r = tb_NewPtr(8, false, true)
            wr16(r, 10); wr16(r + 2, 10); wr16(r + 4, 100); wr16(r + 6, 200)
            tb_PaintRect(r)
            tb_PrClosePage(port)
            tb_PrCloseDoc(port)
            tb_PrClose()
        }

        /// Every program's Apple menu has it: a DRVR named "\0Chooser" in
        /// the System file, which AppendResMenu lists.
        @Test func appleMenuListsTheChooser() {
            boot()
            defer { restore() }
            let m = tb_NewMenu(200, pstr("\u{14}"))
            tb_AppendResMenu(m, Int32(bitPattern: fourCC("DRVR")))
            var items: [String] = []
            let s = tb_NewPtr(256, false, true)
            for i in 1...Int(tb_CountMItems(m)) {
                tb_GetMenuItemText(m, Int16(i), s)
                items.append(pstring(at: s))
            }
            #expect(items.contains("Chooser"))
        }

        /// The user name is 'STR ' -16096 and the printer's driver name
        /// 'STR ' -8192, as programs read them.
        @Test func systemStringsSayTheChoices() {
            boot(printer: "laserwriter:Office@*", user: "Ada Lovelace")
            defer { restore() }
            #expect(string(-16096) == "Ada Lovelace")
            #expect(string(-8192) == "LaserWriter")
        }

        /// It opens as a desk accessory: its window's kind is its refnum,
        /// the driver's entry points to the window.
        @Test func opensAsADeskAccessory() {
            boot()
            defer { restore() }
            let (rn, w, _, _) = open()
            #expect(rn < 0)
            #expect(Int16(bitPattern: rd16(w + 108)) == rn)
            let title = tb_NewPtr(256, false, true)
            tb_GetWTitle(w, title)
            #expect(pstring(at: title) == "Chooser")
            // A second open brings the same window forward.
            #expect(Int16(machine_open_chooser()) == rn)
            tb_CloseDeskAcc(rn)
            #expect(tb_FrontWindow() != w)
        }

        /// The Printer device lists the host's printers; choosing one
        /// sends the Printing Manager's pages to it, without a panel.
        @Test func choosingAHostPrinter() {
            boot()
            defer { restore() }
            let (rn, w, h, v) = open()
            update(w)
            click(w, (h, v), 9 + 35, 9 + 53 + 20) // Printer: the second row's first icon
            click(w, (h, v), 191, 27 + 8)         // the first printer, "Other One"
            #expect(Chooser.changes.contains { $0 == ("printer", "host:Other_One") })
            #expect(string(-8192) == "Printer")
            if let path = ProcessInfo.processInfo.environment["BLUEBOX_TEST_SHOT"] {
                update(w)
                _ = machine_screenshot("\(path)-chooser-printer.png")
            }
            tb_CloseDeskAcc(rn)
            printPage()
            #expect(Chooser.printed?.destination == Int32(MACHINE_PRINT_TO_HOST))
            #expect(Chooser.printed?.printer == "Other_One")
        }

        /// Save as PDF: the pages go to the PDF folder the settings name.
        @Test func savingAsPDF() {
            let folder = tempDir().path
            boot(printer: "pdf", folder: folder)
            defer { restore() }
            #expect(string(-8192) == "Save as PDF")
            printPage()
            #expect(Chooser.printed?.destination == Int32(MACHINE_PRINT_TO_PDF))
            #expect(Chooser.printed?.folder == folder)
        }

        /// No choice: the host's print panel, as before.
        @Test func noChoiceIsThePanel() {
            boot()
            defer { restore() }
            printPage()
            #expect(Chooser.printed?.destination == Int32(MACHINE_PRINT_TO_PANEL))
        }

        /// AppleTalk Inactive and Active: the setting kept, Gestalt's
        /// 'atlk' and SPConfig following.
        @Test func appleTalkActiveAndInactive() {
            boot(appletalk: "local")
            defer { restore() }
            let (rn, w, h, v) = open()
            var resp: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("atlk")), &resp) == 0 && resp != 0)
            #expect(rd8(UInt32(LM_SPConfig)) & 0x0F == 1)
            click(w, (h, v), 335 + 8, 210 + 8) // Inactive
            #expect(Chooser.changes.contains { $0 == ("appletalk", "off") })
            _ = tb_Gestalt(Int32(bitPattern: fourCC("atlk")), &resp)
            #expect(resp == 0)
            #expect(rd8(UInt32(LM_SPConfig)) & 0x0F == 0)
            click(w, (h, v), 272 + 8, 210 + 8) // Active
            #expect(Chooser.changes.contains { $0 == ("appletalk", "local") })
            _ = tb_Gestalt(Int32(bitPattern: fourCC("atlk")), &resp)
            #expect(resp == 58)
            tb_CloseDeskAcc(rn)
        }

        /// The user name typed is kept when the window closes.
        @Test func userNameIsKept() {
            boot(user: "Ada")
            defer { restore() }
            let (rn, w, _, _) = open()
            update(w)
            for c in "Grace" { key(c) } // the name is selected: typing replaces it
            tb_CloseDeskAcc(rn)
            #expect(Chooser.changes.contains { $0 == ("user-name", "Grace") })
            #expect(string(-16096) == "Grace")
        }

        /// The window drawn: the device icons' frame and a label.
        @Test func drawsTheClassicWindow() {
            boot()
            defer { restore() }
            let (rn, w, h, v) = open()
            update(w)
            display_sync()
            var rgba = [UInt8](repeating: 0, count: 4)
            // the left edge of the icons' frame, in black
            machine_screen_rgba(Int32(h + 8), Int32(v + 60), 1, 1, &rgba)
            #expect(rgba[0] < 64)
            if let path = ProcessInfo.processInfo.environment["BLUEBOX_TEST_SHOT"] {
                _ = machine_screenshot("\(path)-chooser.png")
            }
            tb_CloseDeskAcc(rn)
        }

        /// A Chooser extension in the Extensions folder: its 'PACK' -4096
        /// (68k, logging each message and filling the list on fillListMsg)
        /// gets the messages Inside Macintosh: Devices lists, in order.
        @Test func callsAChooserExtension() throws {
            boot()
            defer { restore() }
            let log = tb_NewPtr(64, true, true)
            // flags: left button, chooserInitMsg, fillListMsg, getSelMsg,
            // selectMsg, deselectMsg, terminateMsg
            let flags: UInt32 = 0x0800_0000 | 0x0002_0000 | 0x0000_8000 | 0x0000_4000 | 0x0000_2000 | 0x0000_1000 | 0x0000_0800
            var code: [UInt16] = [0x600E, 0x0000, 0x5041, 0x434B, 0xF000, 0x0001, UInt16(flags >> 16), UInt16(flags & 0xFFFF)]
            code += [0x4E56, 0x0000,                                  // LINK A6,#0
                     0x207C, UInt16(log >> 16), UInt16(log & 0xFFFF), // MOVEA.L #log,A0
                     0x3010, 0x5250, 0xD040,                          // MOVE.W (A0),D0; ADDQ.W #1,(A0); ADD.W D0,D0
                     0x31AE, 0x001A, 0x0002,                          // MOVE.W 26(A6),2(A0,D0.W): the message
                     0x0C6E, 0x000D, 0x001A]                          // CMPI.W #fillListMsg,26(A6)
            let bne = code.count
            code += [0x6600]                                          // BNE.S done
            code += [0x4267, 0x3F3C, 0x0001, 0x4267, 0x2F2E, 0x000C, 0x3F3C, 0x0008, 0xA9E7, 0x544F] // LAddRow(1, 0, list)
            let pea = code.count
            code += [0x487A, 0x0000,                                  // PEA name(PC)
                     0x3F3C, 0x0004, 0x42A7, 0x2F2E, 0x000C, 0x3F3C, 0x0058, 0xA9E7] // LSetCell(name, 4, (0,0), list)
            let done = code.count
            code[bne] = 0x6600 | UInt16((done - bne - 1) * 2)
            code += [0x426E, 0x001C, 0x4E5E, 0x205F, 0x4FEF, 0x0014, 0x4ED0] // result noErr; UNLK; pop 20 bytes; return
            let name = code.count
            code[pea + 1] = UInt16((name - pea - 1) * 2)
            code += [0x4465, 0x7631]                                  // "Dev1"
            func str(_ s: String) -> [UInt8] { [UInt8(s.utf8.count)] + Array(s.utf8) }
            var icon = [UInt8](repeating: 0xFF, count: 256)
            for i in 0..<128 where i % 4 == 0 { icon[i] = 0x80 }
            var fork = buildFork([
                TestResource(type: "PACK", id: -4096, data: code68k(code)),
                TestResource(type: "STR ", id: -4091, data: str("Select a gadget:")),
                TestResource(type: "STR ", id: -4093, data: str("Go")),
                TestResource(type: "ICN#", id: -4096, data: icon),
            ])
            let dir = URL(fileURLWithPath: testSystemFolder).appendingPathComponent("Extensions")
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            let path = dir.appendingPathComponent("Gadget").path
            defer { try? FileManager.default.removeItem(atPath: path) }
            var f = mac_file()
            var empty: [UInt8] = []
            var err = [CChar](repeating: 0, count: 256)
            let ok = empty.withUnsafeMutableBufferPointer { e in
                fork.withUnsafeMutableBufferPointer { r -> Bool in
                    f.data = e.baseAddress; f.data_len = 0
                    f.rsrc = r.baseAddress; f.rsrc_len = r.count
                    let info: [UInt8] = Array("RDEVgadg".utf8) + [UInt8](repeating: 0, count: 24)
                    withUnsafeMutableBytes(of: &f.finder_info) { $0.copyBytes(from: info) }
                    f.has_finder_info = true
                    return appledouble_write(path, &f, &err, err.count)
                }
            }
            #expect(ok)
            let (rn, w, h, v) = open()
            update(w)
            func messages() -> [Int16] { (0..<Int(rd16(log))).map { Int16(bitPattern: rd16(log + 2 + UInt32($0 * 2))) } }
            click(w, (h, v), 9 + 35, 9 + 104 + 20) // the fifth device, the third row's first: Gadget
            #expect(messages() == [11, 13, 14]) // chooserInitMsg, fillListMsg, getSelMsg
            click(w, (h, v), 191, 27 + 8)       // its "Dev1"
            #expect(messages().last == 15)      // selectMsg
            click(w, (h, v), 206 + 20, 112 + 8) // the left button, "Go"
            #expect(messages().last == 19)      // buttonMsg
            if let shot = ProcessInfo.processInfo.environment["BLUEBOX_TEST_SHOT"] {
                update(w)
                _ = machine_screenshot("\(shot)-chooser-extension.png")
            }
            click(w, (h, v), 9 + 35 + 70, 9 + 52 + 20) // Save as PDF
            #expect(messages().last == 17)      // terminateMsg
            tb_CloseDeskAcc(rn)
        }

        /// AppleShare by address: Server IP Address..., the server's address,
        /// "Connect to the file server" as a registered user (the user name
        /// filled in, the password typed as bullets), its volumes checked:
        /// the volume is mounted while the program runs.
        @Test func connectsToAnAppleShareServer() {
            boot(user: "joe")
            defer { restore() }
            let afp = AFPStub(), dsi = DSIStub(afp)
            defer { dsi.thread.finish() }
            defer { machine_close_appleshares() }
            let (rn, w, h, v) = open()
            update(w)
            click(w, (h, v), 9 + 35, 9 + 20) // AppleShare
            // What will be typed into the dialogs: the address, the password, OK.
            for c in "127.0.0.1:\(dsi.port)\r" + "secret\r" + "\r" {
                machine_post_key(Int32(c.asciiValue!), 0, 0, 1, 0)
                machine_post_key(Int32(c.asciiValue!), 0, 0, 0, 0)
            }
            let before = machine_volume_count()
            // the right button: Server IP Address...
            let ev = tb_NewPtr(16, false, true)
            machine_post_mouse(Int32(h + 300), Int32(v + 120), 1)
            machine_post_mouse(Int32(h + 300), Int32(v + 120), 0)
            wr16(ev, 1)
            wr32(ev + 10, UInt32(v + 120) << 16 | UInt32(h + 300))
            tb_SystemClick(ev, w)
            #expect(machine_volume_count() == before + 1)
            var name = [CChar](repeating: 0, count: 64), root = [CChar](repeating: 0, count: 1024)
            #expect(machine_volume_info(before, &name, 64, &root, 1024) && String(cString: name) == "StubVol")
            #expect(afp.commands.contains(18)) // FPLogin
            tb_CloseDeskAcc(rn)
        }

        /// The Printing Manager's low-level calls: the .Print driver is in
        /// the unit table (PrDrvrDCE), text sent with iPrIOCtl and a
        /// bitmap with iPrBitsCtl are printed when the document closes.
        @Test func lowLevelPrinting() {
            boot()
            defer { restore() }
            Chooser.counts = (0, 0, 0)
            machine_set_print_handlers({ job, _ in
                guard let job else { return false }
                Chooser.counts.pages = Int(job.pointee.npages)
                for p in 0..<Int(job.pointee.npages) {
                    let page = job.pointee.pages[p]
                    for i in 0..<Int(page.nops) {
                        if Int(page.ops[i].kind) == Int(MACHINE_PRINT_TEXT) { Chooser.counts.texts += 1 }
                        if Int(page.ops[i].kind) == Int(MACHINE_PRINT_IMAGE) { Chooser.counts.images += 1 }
                    }
                }
                return true
            }, nil, nil)
            defer { machine_set_print_handlers(nil, nil, nil) }
            tb_PrOpen()
            tb_PrDrvrOpen()
            let dce = tb_PrDrvrDCE()
            #expect(dce != 0 && Int16(bitPattern: rd16(dce + 24)) == -3) // dCtlRefNum: unit 2
            tb_PrCtlCall(7, 0x0001_0000, 0, 0) // iPrDevCtl, lPrDocOpen
            let text = tb_NewPtr(64, false, true)
            for (i, b) in Array("Low\rlevel".utf8).enumerated() { wr8(text + UInt32(i), b) }
            tb_PrCtlCall(5, Int32(text), 9, 0) // iPrIOCtl
            tb_PrCtlCall(7, 0x0003_FFFF, 0, 0) // a standard line feed
            // a 16 by 16 bitmap
            let bits = tb_NewPtr(32, false, true)
            for i in 0..<32 { wr8(bits + UInt32(i), i & 1 == 0 ? 0xAA : 0x55) }
            let bm = tb_NewPtr(14, false, true), r = tb_NewPtr(8, false, true)
            wr32(bm, bits); wr16(bm + 4, 2); wr16(bm + 6, 0); wr16(bm + 8, 0); wr16(bm + 10, 16); wr16(bm + 12, 16)
            wr16(r, 0); wr16(r + 2, 0); wr16(r + 4, 16); wr16(r + 6, 16)
            tb_PrCtlCall(4, Int32(bm), Int32(r), 0) // iPrBitsCtl
            tb_PrCtlCall(7, 0x0005_0000, 0, 0) // lPrDocClose
            #expect(Chooser.counts.pages == 1)
            #expect(Chooser.counts.texts >= 8) // a character at a time
            #expect(Chooser.counts.images == 1)
            tb_PrDrvrClose()
            tb_PrClose()
        }
    }
}
