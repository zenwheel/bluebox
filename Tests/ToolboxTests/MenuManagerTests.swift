import CCore
import ForkStore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    @Suite struct MenuManager {
        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func itemText(_ m: UInt32, _ i: Int16) -> String {
            let t = tb_NewPtr(256, false, true)
            tb_GetMenuItemText(m, i, t)
            return pstring(at: t)
        }

        nonisolated(unsafe) static var launched: [String] = []

        /// The Apple menu lists the Apple Menu Items folder, and choosing an
        /// application there launches it.
        @Test func appleMenuItems() {
            let folder = (testSystemFolder as NSString).appendingPathComponent("Apple Menu Items")
            try? FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
            let path = (folder as NSString).appendingPathComponent("Puzzle")
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            withUnsafeMutableBytes(of: &f.finder_info) { raw in for (i, b) in "APPL".utf8.enumerated() { raw[i] = b } }
            f.has_finder_info = true
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            defer { try? FileManager.default.removeItem(atPath: folder) }
            launchTestApp()
            _ = initToolbox()
            let m = tb_NewMenu(1, pstr("\u{14}"))
            tb_AppendResMenu(m, 0x4452_5652) // 'DRVR'
            let names = (1...max(1, Int(tb_CountMItems(m)))).map { itemText(m, Int16($0)) }
            #expect(names.contains("Puzzle"), "\(names)")
            Self.launched = []
            machine_set_launch_handler({ p, _, _, _ in
                MenuManager.launched.append(String(cString: p!))
                return true
            }, nil)
            defer { machine_set_launch_handler(nil, nil) }
            _ = tb_OpenDeskAcc(pstr("Puzzle"))
            #expect(Self.launched.count == 1 && Self.launched[0].hasSuffix("Apple Menu Items/Puzzle"))
        }

        @Test func insertDeleteAndStyle() {
            launchTestApp()
            _ = initToolbox()
            let m = tb_NewMenu(200, pstr("Test"))
            tb_AppendMenu(m, pstr("One;Two;(Three"))
            tb_InsertMenuItem(m, pstr("Zero"), 0)
            #expect(tb_CountMItems(m) == 4 && itemText(m, 1) == "Zero" && itemText(m, 4) == "Three")
            tb_DeleteMenuItem(m, 2)
            #expect(tb_CountMItems(m) == 3 && itemText(m, 2) == "Two")
            // "Three" was disabled and keeps that after moving up.
            #expect(rd32(rd32(m) + UInt32(MenuInfo_enableFlags_off)) & (1 << 3) == 0)
            // GetItemStyle writes a word with the style in the low byte.
            tb_SetItemStyle(m, 2, 1)
            let s = tb_NewPtr(2, false, false)
            wr16(s, 0xFFFF)
            tb_GetItemStyle(m, 2, s)
            #expect(rd16(s) == 1)
        }

        @Test func menuColorTable() {
            launchTestApp()
            _ = initToolbox()
            #expect(tb_GetMCEntry(200, 1) == 0)
            let e = tb_NewPtr(Int32(MCEntry_SIZEOF), false, true)
            wr16(e, 200); wr16(e + 2, 1); wr16(e + 4, 0xFFFF)
            tb_SetMCEntries(1, e)
            let found = tb_GetMCEntry(200, 1)
            #expect(found != 0 && rd16(found + 4) == 0xFFFF)
            tb_DeleteMCEntries(200, -98)
            #expect(tb_GetMCEntry(200, 1) == 0)
        }
    }
}

extension BlueBox {
    @Suite struct FontMenu {
        @Test func fontMenuShowsClassicNames() {
            launchTestApp()
            _ = initToolbox()
            let p = tb_NewPtr(256, false, true)
            guestPString("Font", at: p)
            let m = tb_NewMenu(300, p)
            tb_AppendResMenu(m, Int32(bitPattern: fourCC("FONT")))
            var names: [String] = []
            for i in 1...tb_CountMItems(m) {
                let t = tb_NewPtr(256, false, true)
                tb_GetMenuItemText(m, Int16(i), t)
                names.append(pstring(at: t))
            }
            #expect(names.contains("Geneva") && names.contains("Times") && names.contains("Monaco"))
            #expect(!names.contains("Classic Sans"))
            let num = tb_NewPtr(2, false, true)
            guestPString("Helvetica", at: p)
            tb_GetFNum(p, num)
            #expect(rd16(num) == 21)
        }
    }
}
