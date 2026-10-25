import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// MLTE on styled TextEdit.
    @Suite struct CarbonMLTE {
        func window() -> UInt32 {
            let w = tb_NewCWindow(0, svcRect(40, 40, 240, 340), svcPStr("Text"), 1, 0, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            return w
        }

        func object(_ w: UInt32, options: UInt32 = 0x8) -> UInt32 {
            let v = svcPtr(4), frame = svcPtr(4)
            #expect(svcStatus(svcCall("TXNNewObject", 0, w, svcRect(0, 0, 200, 300), options, 1, fourCC("TEXT"), 1,
                                      v, frame, 0x1234)) == 0)
            return rd32(v)
        }

        func text(_ o: UInt32, _ type: String = "TEXT") -> String {
            let h = svcPtr(4)
            #expect(svcCall("TXNGetDataEncoded", o, 0, 0x7FFF_FFFF, h, fourCC(type)) == 0)
            if type == "utxt" {
                let n = Int(tb_GetHandleSize(rd32(h))) / 2
                return String(decoding: (0..<n).map { rd16(rd32(rd32(h)) + UInt32(2 * $0)) }, as: UTF16.self)
            }
            return String(decoding: svcBytes(rd32(rd32(h)), Int(tb_GetHandleSize(rd32(h)))), as: UTF8.self)
        }

        func key(_ o: UInt32, _ ch: UInt8, mods: UInt16 = 0) {
            let ev = svcPtr(16)
            wr16(ev, 3) // keyDown
            wr32(ev + 2, UInt32(ch))
            wr16(ev + 14, mods)
            svcCall("TXNKeyDown", o, ev)
        }

        @Test func editingAndUndo() {
            launchTestApp()
            _ = initToolbox()
            #expect(svcCall("TXNInitTextension", 0, 0, 0) == 0)
            #expect(svcStatus(svcCall("TXNInitTextension", 0, 0, 0)) == -22012)
            let w = window()
            let o = object(w)
            let (u, n) = svcUTF16("Hello world")
            #expect(svcCall("TXNSetData", o, fourCC("utxt"), u, 2 * n, 0, 0x7FFF_FFFF) == 0)
            #expect(text(o) == "Hello world")
            #expect(text(o, "utxt") == "Hello world")
            #expect(svcCall("TXNDataSize", o) == 22)

            // Typing at the end, then undone as one.
            #expect(svcCall("TXNSetSelection", o, 11, 11) == 0)
            #expect(svcCall("TXNGetChangeCount", o) == 1)
            key(o, UInt8(ascii: "!"))
            key(o, UInt8(ascii: "!"))
            #expect(text(o) == "Hello world!!")
            #expect(svcCall("TXNCanUndo", o, svcPtr(4)) & 0xFF == 1)
            svcCall("TXNUndo", o)
            #expect(text(o) == "Hello world")
            #expect(svcCall("TXNCanRedo", o, svcPtr(4)) & 0xFF == 1)
            svcCall("TXNRedo", o)
            #expect(text(o) == "Hello world!!")

            // Selection, deleting, forward delete.
            svcCall("TXNSetSelection", o, 0, 6)
            let a = svcPtr(4), b = svcPtr(4)
            svcCall("TXNGetSelection", o, a, b)
            #expect(rd32(a) == 0 && rd32(b) == 6 && svcCall("TXNIsSelectionEmpty", o) & 0xFF == 0)
            key(o, 0x08)
            #expect(text(o) == "world!!")
            svcCall("TXNSetSelection", o, 0, 0)
            key(o, 0x7F)
            #expect(text(o) == "orld!!")

            // Cut and paste, through the desk scrap.
            svcCall("TXNSelectAll", o)
            #expect(svcCall("TXNCut", o) == 0 && text(o) == "")
            #expect(svcCall("TXNIsScrapPastable") & 0xFF == 1)
            #expect(svcCall("TXNPaste", o) == 0 && text(o) == "orld!!")

            // Finding.
            let match = svcPtr(12), pat = svcPStr("LD")
            wr32(match, pat + 1); wr32(match + 4, 2); wr32(match + 8, 0)
            #expect(svcStatus(svcCall("TXNFind", o, match, fourCC("TEXT"), 0, 0, 0x7FFF_FFFF, 0, 0, a, b)) == -22008)
            #expect(svcCall("TXNFind", o, match, fourCC("TEXT"), 1, 0, 0x7FFF_FFFF, 0, 0, a, b) == 0)
            #expect(rd32(a) == 2 && rd32(b) == 4)

            // Read only: typing does nothing.
            let tags = svcPtr(4), data = svcPtr(4)
            wr32(tags, fourCC("iopv")); wr32(data, 1)
            #expect(svcCall("TXNSetTXNObjectControls", o, 0, 1, tags, data) == 0)
            key(o, UInt8(ascii: "x"))
            #expect(text(o) == "orld!!")
            wr32(data, 0)
            #expect(svcCall("TXNGetTXNObjectControls", o, 1, tags, data) == 0 && rd32(data) == 1)
            svcCall("TXNDeleteObject", o)
        }

        @Test func stylesRunsAndLines() {
            launchTestApp()
            _ = initToolbox()
            let w = window()
            let o = object(w)
            let t = svcPStr("first line\rsecond line")
            #expect(svcCall("TXNSetData", o, fourCC("TEXT"), t + 1, UInt32(rd8(t)), 0, 0x7FFF_FFFF) == 0)
            // Bigger and bold, the first word.
            let attrs = svcPtr(24)
            wr32(attrs, fourCC("size")); wr32(attrs + 4, 4); wr32(attrs + 8, 24 << 16)
            wr32(attrs + 12, fourCC("face")); wr32(attrs + 16, 1); wr32(attrs + 20, 1)
            #expect(svcCall("TXNSetTypeAttributes", o, 2, attrs, 0, 5) == 0)
            let count = svcPtr(4)
            #expect(svcCall("TXNCountRunsInRange", o, 0, 0x7FFF_FFFF, count) == 0 && rd32(count) == 2)
            let rs = svcPtr(4), re = svcPtr(4), type = svcPtr(4), q = svcPtr(12)
            wr32(q, fourCC("size")); wr32(q + 4, 4)
            #expect(svcCall("TXNGetIndexedRunInfoFromRange", o, 0, 0, 0x7FFF_FFFF, rs, re, type, 1, q) == 0)
            #expect(rd32(rs) == 0 && rd32(re) == 5 && rd32(type) == fourCC("TEXT") && rd32(q + 8) == 24 << 16)
            #expect(svcStatus(svcCall("TXNGetIndexedRunInfoFromRange", o, 5, 0, 0x7FFF_FFFF, rs, re, type, 0, 0)) == -22007)
            svcCall("TXNSetSelection", o, 0, 5)
            let flags = svcPtr(4)
            wr32(q + 8, 0)
            #expect(svcCall("TXNGetContinuousTypeAttributes", o, flags, 1, q) == 0)
            #expect(rd32(flags) & 2 != 0 && rd32(q + 8) == 24 << 16)

            // Lines and their metrics.
            #expect(svcCall("TXNGetLineCount", o, count) == 0 && rd32(count) == 2)
            let width = svcPtr(4), height = svcPtr(4)
            #expect(svcCall("TXNGetLineMetrics", o, 0, width, height) == 0 && rd32(height) >> 16 > 20)
            #expect(svcStatus(svcCall("TXNGetLineMetrics", o, 5, width, height)) == -22017)
            let pt = svcPtr(4), off = svcPtr(4)
            #expect(svcCall("TXNOffsetToPoint", o, 13, pt) == 0)
            #expect(svcCall("TXNPointToOffset", o, rd32(pt), off) == 0 && rd32(off) == 13)

            // Drawn into the window, with its scroll bar.
            svcCall("TXNFocus", o, 1)
            svcCall("TXNDraw", o, 0)
            svcCall("TXNDeleteObject", o)
        }

        @Test func savedAndReverted() throws {
            let dir = launchTestApp()
            _ = initToolbox()
            let w = window()
            let o = object(w)
            let t = svcPStr("saved text")
            svcCall("TXNSetData", o, fourCC("TEXT"), t + 1, UInt32(rd8(t)), 0, 0x7FFF_FFFF)
            let path = dir.appendingPathComponent("Doc").path
            try Data().write(to: URL(fileURLWithPath: path))
            let spec = svcPtr(80), ref = svcPtr(2)
            fm_spec_for_path(path, spec)
            #expect(tb_FSpOpenDF(spec, 3, ref) == 0)
            #expect(svcCall("TXNSave", o, fourCC("TEXT"), fourCC("MPSR"), 1, spec, UInt32(rd16(ref)), 0) == 0)
            let pb = svcPtr(80)
            wr16(pb + 24, rd16(ref))
            _ = tb_PBClose(pb, false)
            #expect(try String(contentsOfFile: path, encoding: .macOSRoman) == "saved text")
            #expect(svcCall("TXNGetChangeCount", o) == 0)
            let more = svcPStr(" and more")
            svcCall("TXNSetData", o, fourCC("TEXT"), more + 1, UInt32(rd8(more)), 10, 10)
            #expect(text(o) == "saved text and more")
            #expect(svcCall("TXNRevert", o) == 0 && text(o) == "saved text")
            svcCall("TXNDeleteObject", o)
        }

        @Test func echoModeAndTextBoxes() {
            launchTestApp()
            _ = initToolbox()
            let w = window()
            let o = object(w, options: 0)
            #expect(svcCall("TXNEchoMode", o, UInt32(UInt8(ascii: "*")), 0, 1) == 0)
            key(o, UInt8(ascii: "p"))
            key(o, UInt8(ascii: "w"))
            #expect(text(o) == "pw")
            #expect(svcStatus(svcCall("TXNCopy", o)) == -22014)
            #expect(svcCall("TXNEchoMode", o, 0, 0, 0) == 0 && text(o) == "pw")
            svcCall("TXNDeleteObject", o)

            // A Unicode text box: measured, the box's right edge moved in.
            let (u, n) = svcUTF16("Boxed text")
            let box = svcRect(10, 10, 100, 300), opts = svcPtr(20)
            #expect(svcCall("TXNDrawUnicodeTextBox", u, n, box, 0, opts) == 0)
            let right = Int16(bitPattern: rd16(box + 6)), bottom = Int16(bitPattern: rd16(box + 4))
            #expect(right > 30 && right < 300 && bottom > 10 && bottom < 40)
        }
    }
}
