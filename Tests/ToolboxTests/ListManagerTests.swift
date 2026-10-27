import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    @Suite struct ListManager {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func pt(_ v: Int, _ h: Int) -> UInt32 { UInt32(UInt16(v)) << 16 | UInt32(UInt16(h)) }

        func text(_ s: String) -> (UInt32, Int16) {
            let p = tb_NewPtr(Int32(max(s.utf8.count, 1)), false, true)
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            return (p, Int16(s.utf8.count))
        }

        func cell(_ list: UInt32, _ v: Int, _ h: Int = 0) -> String {
            let buf = tb_NewPtr(64, false, true), len = tb_NewPtr(2, false, true)
            wr16(len, 64)
            tb_LGetCell(buf, len, pt(v, h), list)
            return String(decoding: (0..<Int(rd16(len))).map { rd8(buf + UInt32($0)) }, as: UTF8.self)
        }

        func makeList() -> (window: UInt32, list: UInt32) {
            launchTestApp()
            _ = initToolbox()
            let w = tb_NewWindow(0, rect(50, 50, 300, 300), 0, 1, 0, 0xFFFF_FFFF, 1, 0)
            tb_SetPort(w)
            let list = tb_LNew(rect(10, 10, 110, 150), rect(0, 0, 0, 1), 0, 0, w, 1, 0, 0, 1)
            #expect(list != 0)
            return (w, list)
        }

        @Test func rowsCellsAndData() {
            let (_, list) = makeList()
            #expect(tb_LAddRow(3, 0, list) == 0)
            for (i, s) in ["apple", "banana", "cherry"].enumerated() {
                let (p, n) = text(s)
                tb_LSetCell(p, n, pt(i, 0), list)
            }
            #expect(cell(list, 0) == "apple" && cell(list, 1) == "banana" && cell(list, 2) == "cherry")
            let (more, m) = text("!")
            tb_LAddToCell(more, m, pt(1, 0), list)
            #expect(cell(list, 1) == "banana!" && cell(list, 2) == "cherry")
            // Insert a row in the middle; data moves down with its rows.
            #expect(tb_LAddRow(1, 1, list) == 1)
            #expect(cell(list, 1) == "" && cell(list, 2) == "banana!" && cell(list, 3) == "cherry")
            tb_LDelRow(1, 1, list)
            #expect(cell(list, 1) == "banana!")
            // Search.
            let c = tb_NewPtr(4, false, true)
            let (key, k) = text("CHERRY")
            #expect(tb_LSearch(key, k, 0, c, list) != 0 && rd32(c) == pt(2, 0))
            // Geometry: cells stack down from rView's top-left.
            let r = tb_NewPtr(8, false, true)
            tb_LRect(r, pt(1, 0), list)
            let h = Int(rd16(r + 4)) - Int(rd16(r))
            #expect(rd16(r) == UInt16(10 + h) && rd16(r + 2) == 10)
            tb_LDispose(list)
        }

        @Test func selectionAndClicks() {
            let (_, list) = makeList()
            _ = tb_LAddRow(4, 0, list)
            let c = tb_NewPtr(4, false, true)
            wr32(c, pt(0, 0))
            #expect(tb_LGetSelect(1, c, list) == 0)
            tb_LSetSelect(1, pt(2, 0), list)
            wr32(c, pt(0, 0))
            #expect(tb_LGetSelect(1, c, list) != 0 && rd32(c) == pt(2, 0))
            // A click on row 1 selects it alone.
            let r = tb_NewPtr(8, false, true)
            tb_LRect(r, pt(1, 0), list)
            let inside = pt(Int(rd16(r)) + 2, Int(rd16(r + 2)) + 2)
            #expect(tb_LClick(inside, 0, list) == 0)
            wr32(c, pt(0, 0))
            #expect(tb_LGetSelect(1, c, list) != 0 && rd32(c) == pt(1, 0))
            wr32(c, pt(2, 0))
            #expect(tb_LGetSelect(1, c, list) == 0)
            #expect(tb_LLastClick(list) == Int32(bitPattern: pt(1, 0)))
            // A second click in the same cell straight away is a double click.
            #expect(tb_LClick(inside, 0, list) != 0)
            // LNextCell walks down the column and stops at the end.
            wr32(c, pt(2, 0))
            #expect(tb_LNextCell(0, 1, c, list) != 0 && rd32(c) == pt(3, 0))
            #expect(tb_LNextCell(0, 1, c, list) == 0)
            tb_LDispose(list)
        }

        /// TimeSlice counts the selected cells with LGetSelect from each
        /// LNextCell: the cell moves past the end, where LGetSelect finds
        /// no more, and past a row's end LGetSelect goes on from the next.
        @Test func countingSelectedCellsEnds() {
            let (_, list) = makeList()
            _ = tb_LAddRow(3, 0, list)
            tb_LSetSelect(1, pt(0, 0), list)
            tb_LSetSelect(1, pt(2, 0), list)
            let c = tb_NewPtr(4, false, true)
            wr32(c, pt(0, 0))
            var count = 0
            while tb_LGetSelect(1, c, list) != 0 && count < 10 {
                count += 1
                _ = tb_LNextCell(0, 1, c, list)
            }
            #expect(count == 2)
            #expect(rd32(c) == pt(3, 0))
            wr32(c, pt(0, 1))               // past row 0's only column
            #expect(tb_LGetSelect(1, c, list) != 0 && rd32(c) == pt(2, 0))
            tb_LDispose(list)
        }

        /// An inactive list shows no selection, as System 7.1 draws it:
        /// LActivate draws the selected cells plain or highlighted, cells
        /// selected meanwhile aren't drawn, and drawing an inactive list
        /// leaves them plain.  Greg's Browser sets lActive itself and
        /// outlines the selection of an inactive list in hilite mode, which
        /// over a highlighted cell left the highlight inset.
        @Test func inactiveListShowsNoSelection() {
            let (_, list) = makeList()
            _ = tb_LAddRow(4, 0, list)
            let r = tb_NewPtr(8, false, true)
            // A pixel in the blank right part of a row (LDEF 0 inverts it).
            func shown(_ v: Int) -> Bool {
                tb_LRect(r, pt(v, 0), list)
                return tb_GetPixel(Int16(rd16(r + 6)) - 3, Int16(rd16(r)) + 3) != 0
            }
            tb_LSetSelect(1, pt(1, 0), list)
            #expect(shown(1))
            tb_LActivate(0, list)
            #expect(!shown(1))
            tb_LSetSelect(1, pt(2, 0), list)
            #expect(!shown(2))
            tb_LUpdate(0, list)
            tb_LDraw(pt(1, 0), list)
            #expect(!shown(1) && !shown(2))
            tb_LActivate(1, list)
            #expect(shown(1) && shown(2) && !shown(0))
            // Set by hand, as Greg's Browser does, the field alone decides.
            wr8(rd32(list) + UInt32(ListRec_lActive_off), 0)
            tb_LUpdate(0, list)
            #expect(!shown(1) && !shown(2))
            tb_LDispose(list)
        }
    }
}
