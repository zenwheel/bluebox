import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Navigation Services on the host's panels (a handler answering here,
    /// as --file-answer does), and its alerts.
    @Suite struct NavigationServices {
        nonisolated(unsafe) static var answer = ""
        nonisolated(unsafe) static var asked: machine_file_request?
        nonisolated(unsafe) static var hidden: [String] = []
        nonisolated(unsafe) static var offered: [String] = []
        nonisolated(unsafe) static var alertText = ""

        func answering(_ path: String, _ body: () -> Void) {
            Self.answer = path
            Self.asked = nil
            machine_set_file_handler({ rq, out, size, _ in
                guard let rq, let out else { return false }
                Self.asked = rq.pointee
                // The panel asks the filter about what it would show.
                Self.hidden = Self.offered.filter { machine_file_filtered($0) }
                if Self.answer.isEmpty { return false }
                let bytes = Array(Self.answer.utf8.prefix(size - 1)) + [0]
                for (i, b) in bytes.enumerated() { out[i] = CChar(bitPattern: b) }
                return true
            }, nil)
            defer { machine_set_file_handler(nil, nil) }
            body()
        }

        /// The selection's one FSSpec: its name.
        func selectionName(_ reply: UInt32) -> String {
            let list = reply + UInt32(NavReplyRecord_selection_off)
            let count = tb_NewPtr(4, false, true)
            #expect(tb_AECountItems(list, count) == 0 && rd32(count) == 1)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true), type = tb_NewPtr(4, false, true)
            let size = tb_NewPtr(4, false, true), key = tb_NewPtr(4, false, true)
            #expect(tb_AEGetNthPtr(list, 1, Int32(bitPattern: fourCC("fss ")), key, type, spec, Int32(FSSpec_SIZEOF), size) == 0)
            return pstring(at: spec + UInt32(FSSpec_name_off))
        }

        func options(saved: String = "", message: String = "") -> UInt32 {
            let o = tb_NewPtr(Int32(NavDialogOptions_SIZEOF), false, true)
            #expect(tb_NavGetDefaultDialogOptions(o) == 0)
            #expect(rd32(o + UInt32(NavDialogOptions_dialogOptionFlags_off)) == 0xE4)
            guestPString(saved, at: o + UInt32(NavDialogOptions_savedFileName_off))
            guestPString(message, at: o + UInt32(NavDialogOptions_message_off))
            return o
        }

        /// The filter, a routine descriptor for 68k code (here native):
        /// pascal Boolean (AEDesc *, void *info, void *ud, NavFilterModes),
        /// showing only files whose name starts with "keep".
        static let filter: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop16(c) // filterMode
            _ = cpu_pop32(c) // callBackUD
            _ = cpu_pop32(c) // info
            let desc = cpu_pop32(c)
            let name = pstring(at: rd32(rd32(desc + 4)) + UInt32(FSSpec_name_off))
            wr16(c.pointee.a.7, name.hasPrefix("keep") ? 0x0100 : 0)
            cpu_push32(c, ret)
        }

        func filterUPP() -> UInt32 {
            let code = native_register(Self.filter, "NavFilterTest", 0)
            g_ppc.gpr.3 = code
            g_ppc.gpr.4 = 0x2FD0 // uppNavObjectFilterProcInfo
            g_ppc.gpr.5 = 0      // kM68kISA
            cfm_export_fn("NewRoutineDescriptor")!(&g_ppc)
            return g_ppc.gpr.3
        }

        @Test func getFileAnswered() throws {
            let dir = launchTestApp()
            let keep = dir.appendingPathComponent("keep me").path, drop = dir.appendingPathComponent("drop me").path
            try Data("x".utf8).write(to: URL(fileURLWithPath: keep))
            try Data("y".utf8).write(to: URL(fileURLWithPath: drop))
            #expect(cfm_export_fn("NavGetFile") != nil && cfm_export_fn("NavGetDefaultDialogOptions") != nil)

            let reply = tb_NewPtr(256, false, true)
            let types = tb_NewHandle(12, false, true)
            wr16(rd32(types) + 6, 1)
            wr32(rd32(types) + 8, fourCC("TEXT"))
            Self.offered = [keep, drop]
            let filter = filterUPP()
            answering(keep) {
                #expect(tb_NavGetFile(0, reply, options(message: "Pick one"), 0, 0, filter, types, 0) == 0)
            }
            #expect(Self.asked?.save == false && Self.asked?.folders == false)
            #expect(Self.asked?.ntypes == 1 && Self.asked?.types.0 == fourCC("TEXT"))
            #expect(withUnsafeBytes(of: Self.asked!.prompt) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) } == "Pick one")
            #expect(Self.hidden == [drop])
            #expect(rd8(reply + UInt32(NavReplyRecord_validRecord_off)) == 1)
            #expect(selectionName(reply) == "keep me")
            #expect(tb_NavDisposeReply(reply) == 0)
            #expect(rd8(reply + UInt32(NavReplyRecord_validRecord_off)) == 0)

            // Cancelled: no reply.
            answering("") {
                #expect(tb_NavGetFile(0, reply, 0, 0, 0, 0, 0, 0) == -128) // userCanceledErr
            }
            #expect(rd8(reply + UInt32(NavReplyRecord_validRecord_off)) == 0)

            // A folder chosen.
            let folder = dir.appendingPathComponent("Folder")
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            answering(folder.path) {
                #expect(tb_NavChooseFolder(0, reply, 0, 0, 0, 0) == 0)
            }
            #expect(Self.asked?.folders == true)
            #expect(selectionName(reply) == "Folder")
        }

        @Test func putFileAnswered() throws {
            let dir = launchTestApp()
            let reply = tb_NewPtr(256, false, true)
            let target = dir.appendingPathComponent("Report").path
            answering(target) {
                #expect(tb_NavPutFile(0, reply, options(saved: "Untitled Report"), 0, Int32(bitPattern: fourCC("TEXT")),
                                      Int32(bitPattern: fourCC("ttxt")), 0) == 0)
            }
            #expect(Self.asked?.save == true)
            #expect(withUnsafeBytes(of: Self.asked!.default_name) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) } == "Untitled Report")
            #expect(rd8(reply + UInt32(NavReplyRecord_validRecord_off)) == 1)
            #expect(rd8(reply + UInt32(NavReplyRecord_replacing_off)) == 0)
            #expect(selectionName(reply) == "Report")
            #expect(tb_NavCompleteSave(reply, 0) == 0)
            #expect(tb_NavDisposeReply(reply) == 0)

            // Over a file that is there: replacing.
            try Data("z".utf8).write(to: URL(fileURLWithPath: target))
            answering(target) {
                #expect(tb_NavPutFile(0, reply, 0, 0, 0, 0, 0) == 0)
            }
            #expect(rd8(reply + UInt32(NavReplyRecord_replacing_off)) == 1)
        }

        @Test func askSaveChangesIsAnAlert() {
            launchTestApp()
            machine_set_alert_handler({ a, _ in
                guard let a else { return 0 }
                NavigationServices.alertText = withUnsafeBytes(of: a.pointee.message) {
                    String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self)
                }
                return 3 // Don't Save
            }, nil)
            defer { machine_set_alert_handler(nil, nil) }
            let answer = tb_NewPtr(4, false, true)
            #expect(tb_NavAskSaveChanges(options(saved: "Letter"), 2, answer, 0, 0) == 0)
            #expect(rd32(answer) == 3) // kNavAskSaveChangesDontSave
            #expect(Self.alertText.contains("Letter") && Self.alertText.contains("quitting"))
        }
    }

    /// The Contextual Menu Manager: the menu pops up where asked, and the
    /// item under the mouse is the choice.
    @Suite struct ContextualMenus {
        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        @Test func selectFromPopUp() {
            launchTestApp()
            _ = initToolbox()
            #expect(tb_InitContextualMenus() == 0)
            var answer: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("cmnu")), &answer) == 0 && answer & 2 != 0)
            #expect(cfm_export_fn("ContextualMenuSelect") != nil)

            let ev = tb_NewPtr(Int32(EventRecord_SIZEOF), false, true)
            wr16(ev + UInt32(EventRecord_what_off), 1) // mouseDown
            #expect(tb_IsShowContextualMenuClick(ev) == 0)
            wr16(ev + UInt32(EventRecord_modifiers_off), 0x1000) // controlKey
            #expect(tb_IsShowContextualMenuClick(ev) != 0)

            let m = tb_NewMenu(300, pstr("Ctx"))
            tb_AppendMenu(m, pstr("Cut;Copy;Paste"))
            let type = tb_NewPtr(4, false, true), id = tb_NewPtr(2, false, true), item = tb_NewPtr(2, false, true)
            machine_post_mouse(110, 100 + 16 + 8, -1) // over the second item
            #expect(tb_ContextualMenuSelect(m, 100 << 16 | 100, 0, 0, 0, 0, type, id, item) == 0)
            #expect(rd32(type) == 1 && rd16(id) == 300 && rd16(item) == 2)

            machine_post_mouse(10, 10, -1) // outside: nothing chosen
            #expect(tb_ContextualMenuSelect(m, 100 << 16 | 100, 0, 0, 0, 0, type, id, item) == -128)
            #expect(rd32(type) == 0 && rd16(item) == 0)
        }
    }
}
