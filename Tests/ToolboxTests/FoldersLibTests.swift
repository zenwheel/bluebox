import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// FoldersLib: the Folder Manager's descriptors and routings, which
    /// Aladdin's Transporter samples add and remove through CarbonLib.
    @Suite struct FoldersLib {
        func code(_ s: String) -> Int32 { Int32(bitPattern: fourCC(s)) }

        @Test func descriptorsDescribeAndPlaceFolders() {
            launchTestApp()
            let name = tb_NewPtr(64, false, true)
            guestPString("Transporter Items", at: name)
            #expect(tb_AddFolderDescriptor(code("tpTI"), 0, code("relf"), code("pref"), 0, 0, name, 0) == 0)
            #expect(tb_AddFolderDescriptor(code("tpTI"), 0, code("relf"), code("pref"), 0, 0, name, 0) == -4271)
            #expect(tb_AddFolderDescriptor(code("tpTJ"), 0, code("xxxx"), code("pref"), 0, 0, name, 0) == -4270)

            // FindFolder makes it inside the Preferences folder.
            let vref = tb_NewPtr(2, false, true), dir = tb_NewPtr(4, false, true)
            #expect(tb_FindFolder(-1, code("tpTI"), 0, vref, dir) == -43)
            #expect(tb_FindFolder(-1, code("tpTI"), 1, vref, dir) == 0)
            let made = URL(fileURLWithPath: testSystemFolder).appendingPathComponent("Preferences/Transporter Items")
            var isDir: ObjCBool = false
            #expect(FileManager.default.fileExists(atPath: made.path, isDirectory: &isDir) && isDir.boolValue)
            let type = tb_NewPtr(4, false, true)
            #expect(tb_IdentifyFolder(Int16(bitPattern: rd16(vref)), Int32(bitPattern: rd32(dir)), type) == 0)
            #expect(rd32(type) == fourCC("tpTI"))

            // Its descriptor, and the standard ones'.
            let desc = tb_NewPtr(96, false, true)
            #expect(tb_GetFolderDescriptor(code("tpTI"), 96, desc) == 0)
            #expect(rd32(desc) == 96 && rd32(desc + 4) == fourCC("tpTI") && rd32(desc + 12) == fourCC("relf"))
            #expect(rd32(desc + 16) == fourCC("pref") && pstring(at: desc + 32) == "Transporter Items")
            #expect(tb_GetFolderDescriptor(code("pref"), 96, desc) == 0)
            #expect(rd32(desc + 16) == fourCC("macs") && pstring(at: desc + 32) == "Preferences")
            let vname = tb_NewPtr(64, false, true), found = tb_NewPtr(2, false, true)
            #expect(tb_GetFolderName(-1, code("macs"), found, vname) == 0 && pstring(at: vname) == "System Folder")

            let total = tb_NewPtr(4, false, true), types = tb_NewPtr(400, false, true)
            #expect(tb_GetFolderTypes(100, total, types) == 0)
            let n = Int(rd32(total))
            #expect(n > 10)
            let listed = (0..<min(n, 100)).map { rd32(types + UInt32($0 * 4)) }
            #expect(listed.contains(fourCC("tpTI")) && listed.contains(fourCC("pref")))

            #expect(tb_RemoveFolderDescriptor(code("tpTI")) == 0)
            #expect(tb_GetFolderDescriptor(code("tpTI"), 96, desc) == -4273)
            #expect(tb_FindFolder(-1, code("tpTI"), 1, vref, dir) == -43)
            #expect(tb_RemoveFolderDescriptor(code("tpTI")) == -4273)
        }

        @Test func routingsAreKept() {
            launchTestApp()
            #expect(tb_AddFolderRouting(code("INIT"), code("macs"), code("extn"), 0, 0) == 0)
            #expect(tb_AddFolderRouting(code("INIT"), code("macs"), code("ctrl"), 0, 0) == -4274)
            #expect(tb_AddFolderRouting(code("INIT"), code("macs"), code("ctrl"), 0, 1) == 0)
            let to = tb_NewPtr(4, false, true), flags = tb_NewPtr(4, false, true)
            #expect(tb_FindFolderRouting(code("INIT"), code("macs"), to, flags) == 0 && rd32(to) == fourCC("ctrl"))
            let total = tb_NewPtr(4, false, true), list = tb_NewPtr(40, false, true)
            #expect(tb_GetFolderRoutings(2, total, 10, list) == -4276)
            #expect(tb_GetFolderRoutings(2, total, 20, list) == 0 && rd32(total) == 1)
            #expect(rd32(list + 4) == fourCC("INIT") && rd32(list + 12) == fourCC("ctrl"))
            #expect(tb_RemoveFolderRouting(code("INIT"), code("macs")) == 0)
            #expect(tb_FindFolderRouting(code("INIT"), code("macs"), to, flags) == -4275)
        }
    }
}
