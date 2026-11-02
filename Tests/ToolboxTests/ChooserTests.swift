import CCore
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
    }
}
