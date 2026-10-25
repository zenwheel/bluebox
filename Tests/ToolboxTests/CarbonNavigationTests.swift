import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Navigation Services' Carbon dialogs on the host's panels, and the
    /// universal procedure pointers CarbonLib makes.
    @Suite struct CarbonNavigation {
        nonisolated(unsafe) static var answer = ""
        nonisolated(unsafe) static var messages: [(Int32, UInt32)] = []
        nonisolated(unsafe) static var replyName = ""

        func answering(_ path: String, _ body: () -> Void) {
            Self.answer = path
            machine_set_file_handler({ _, out, size, _ in
                guard let out, !CarbonNavigation.answer.isEmpty else { return false }
                let b = Array(CarbonNavigation.answer.utf8.prefix(size - 1)) + [0]
                for (i, c) in b.enumerated() { out[i] = CChar(bitPattern: c) }
                return true
            }, nil)
            defer { machine_set_file_handler(nil, nil) }
            body()
        }

        /// pascal void (NavEventCallbackMessage, NavCBRecPtr, void *): notes
        /// each message, and on the user's action gets the reply as programs do.
        static let events: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)
            let cb = cpu_pop32(c)
            let message = Int32(bitPattern: cpu_pop32(c))
            let action = rd32(cb + 32)
            CarbonNavigation.messages.append((message, action))
            if message == 12 && (action == 2 || action == 3) { // kNavCBUserAction: open, save
                let reply = svcPtr(256)
                if svcCall("NavDialogGetReply", rd32(cb + 2), reply) == 0 {
                    let spec = svcPtr(80), key = svcPtr(4), type = svcPtr(4), size = svcPtr(4)
                    _ = tb_AEGetNthPtr(reply + 6, 1, Int32(bitPattern: fourCC("fss ")), key, type, spec, 70, size)
                    CarbonNavigation.replyName = pstring(at: spec + 6)
                    svcCall("NavDisposeReply", reply)
                }
            }
            cpu_push32(c, ret)
        }

        @Test func getAndPutFileDialogs() throws {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("Letter").path
            try Data("x".utf8).write(to: URL(fileURLWithPath: file))
            let options = svcPtr(66), out = svcPtr(4)
            #expect(svcCall("NavGetDefaultDialogCreationOptions", options) == 0)
            #expect(rd32(options + 2) == 0xE4 && rd32(options + 42) == 2) // flags, app-modal
            #expect(svcString(rd32(options + 10)) == "Test App")
            wr32(options + 30, svcCFString("Open a letter")) // message

            // The event procedure, called through its UPP.
            let events = svcUPP68k(Self.events, "NavEventsTest", 0xFC0)
            #expect(svcCall("NavCreateGetFileDialog", options, 0, events, 0, 0, 0, out) == 0)
            let dialog = rd32(out)
            Self.messages = []
            Self.replyName = ""
            answering(file) { #expect(svcCall("NavDialogRun", dialog) == 0) }
            #expect(Self.messages.map(\.0) == [2, 12, 3]) // start, user action, terminate
            #expect(Self.messages[1].1 == 2) // kNavUserActionOpen
            #expect(Self.replyName == "Letter")
            #expect(svcCall("NavDialogGetUserAction", dialog) == 2)
            #expect(svcCall("NavDialogGetWindow", dialog) == 0)
            #expect(svcStatus(svcCall("NavDialogRun", dialog)) == -5694) // run once
            svcCall("NavDialogDispose", dialog)

            // Cancelled: no reply.
            #expect(svcCall("NavCreateChooseFileDialog", options, 0, 0, 0, 0, 0, out) == 0)
            answering("") { #expect(svcCall("NavDialogRun", rd32(out)) == 0) }
            #expect(svcCall("NavDialogGetUserAction", rd32(out)) == 1)
            #expect(svcStatus(svcCall("NavDialogGetReply", rd32(out), svcPtr(256))) == -5694)
            svcCall("NavDialogDispose", rd32(out))

            // Save: the name given, then the one chosen.
            wr32(options + 26, svcCFString("Untitled"))
            #expect(svcCall("NavCreatePutFileDialog", options, fourCC("TEXT"), fourCC("ttxt"), events, 0, out) == 0)
            let put = rd32(out)
            #expect(svcString(svcCall("NavDialogGetSaveFileName", put)) == "Untitled")
            #expect(svcCall("NavDialogSetSaveFileName", put, svcCFString("Memo")) == 0)
            Self.messages = []
            answering(dir.appendingPathComponent("Memo 2").path) { #expect(svcCall("NavDialogRun", put) == 0) }
            #expect(Self.messages[1].1 == 3 && Self.replyName == "Memo 2") // kNavUserActionSaveAs
            #expect(svcString(svcCall("NavDialogGetSaveFileName", put)) == "Memo 2")
            let reply = svcPtr(256)
            #expect(svcCall("NavDialogGetReply", put, reply) == 0)
            #expect(rd16(reply) == 1 && svcString(rd32(reply + 24)) == "Memo 2")
            #expect(svcCall("NavDisposeReply", reply) == 0 && rd32(reply + 24) == 0)
            svcCall("NavDialogDispose", put)

            // Asking to save changes: the alert's default, Save.
            #expect(svcCall("NavCreateAskSaveChangesDialog", options, 1, 0, 0, out) == 0)
            #expect(svcCall("NavDialogRun", rd32(out)) == 0)
            #expect(svcCall("NavDialogGetUserAction", rd32(out)) == 6) // kNavUserActionSaveChanges
            #expect(svcStatus(svcCall("NavDialogGetReply", rd32(out), reply)) == -5695)
            svcCall("NavDialogDispose", rd32(out))
            svcCall("DisposeNavEventUPP", events)
        }

        /// pascal Boolean (AEDesc *, void *, void *, NavFilterModes): notes its
        /// arguments and says yes.
        nonisolated(unsafe) static var seen: [UInt32] = []
        static let filter68k: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let mode = UInt32(cpu_pop16(c))
            let ud = cpu_pop32(c), info = cpu_pop32(c), item = cpu_pop32(c)
            CarbonNavigation.seen = [item, info, ud, mode]
            wr16(c.pointee.a.7, 0x0100)
            cpu_push32(c, ret)
        }

        static let ppcFilter: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
            c?.pointee.pc = c?.pointee.lr ?? 0
        }

        @Test func uppsRoundTrip() {
            launchTestApp()
            // New: a PowerPC routine descriptor with the kind's procInfo.
            let tv = cfm_native_tvector(Self.ppcFilter, "NavFilterPPC")
            let upp = svcCall("NewNavObjectFilterUPP", tv)
            #expect(upp != 0 && rd16(upp) == 0xAAFE && rd32(upp + 12) == 0x2FD0 && rd8(upp + 17) == 1 && rd32(upp + 20) == tv)
            svcCall("DisposeNavObjectFilterUPP", upp)
            #expect(svcCall("NewPMItemUPP", 0) == 0)
            // Invoke: the arguments, then the UPP (here one for 68k code).
            let filter = svcUPP68k(Self.filter68k, "NavFilter68k", 0x2FD0)
            Self.seen = []
            #expect(svcCall("InvokeNavObjectFilterUPP", 0x100, 0x200, 0x300, 4, filter) & 0xFF == 1)
            #expect(Self.seen == [0x100, 0x200, 0x300, 4])
        }
    }
}
