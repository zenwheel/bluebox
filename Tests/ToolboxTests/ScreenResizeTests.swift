import BlueBoxHost
import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// A running program's screen made another size (machine_resize_screen),
    /// as a resumed program's is for the display and scale it now runs on:
    /// a smaller display, a larger one, or another scale all come to that.
    @Suite(.serialized) struct ScreenResize {
        func rect(_ at: UInt32) -> [Int] { (0..<4).map { Int(Int16(bitPattern: rd16(at + UInt32($0 * 2)))) } }
        func bbox(_ rgn: UInt32) -> [Int] { rect(rd32(rgn) + 2) }

        /// A 1024x768 machine with a window whose content is at
        /// (top, left)-(bottom, right).
        func setUp(_ content: [Int], color: Bool = true) -> UInt32 {
            machine_set_screen(1024, 768)
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in content.enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let title = tb_NewPtr(2, false, true)
            let w = color ? tb_NewCWindow(0, r, title, 1, 0, 0xFFFF_FFFF, 1, 0) : tb_NewWindow(0, r, title, 1, 0, 0xFFFF_FFFF, 1, 0)
            if content != [0, 0, 768, 1024] { #expect(machine_screen_fixed() == nil) }
            return w
        }

        func size() -> (Int, Int) {
            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            return (Int(w), Int(h))
        }

        /// The screen and everything that describes it agree on its size.
        func expectDescribes(_ width: Int, _ height: Int, window w: UInt32) {
            #expect(size() == (width, height))
            let screen = [0, 0, height, width]
            let gd = rd32(rd32(UInt32(LM_MainDevice)))
            #expect(rect(gd + UInt32(GDevice_gdRect_off)) == screen)
            let pm = rd32(rd32(gd + UInt32(GDevice_gdPMap_off)))
            #expect(rect(pm + UInt32(PixMap_bounds_off)) == screen)
            #expect(Int(rd16(UInt32(LM_ScreenRow))) == Int(qd_screen_row_bytes()))
            #expect(Int(rd16(pm + UInt32(PixMap_rowBytes_off)) & 0x3FFF) == Int(qd_screen_row_bytes()))
            let sb = qd_screen_bits()
            #expect(rect(sb + UInt32(BitMap_bounds_off)) == screen)
            #expect(rect(UInt32(LM_CrsrPin)) == screen)
            #expect(bbox(rd32(UInt32(LM_GrayRgn))) == [Int(rd16(UInt32(LM_MBarHeight))), 0, height, width])
            let wmgr = rd32(UInt32(LM_WMgrPort))
            #expect(rect(wmgr + UInt32(GrafPort_portRect_off)) == screen)
            // The window's pixel map (or a black-and-white window's bit
            // map): the screen's rows, its bounds the new screen's size from
            // where the window is.
            let bits = qd_port_is_color(w) ? rd32(rd32(w + UInt32(CGrafPort_portPixMap_off))) : w + UInt32(GrafPort_portBits_off)
            #expect(Int(rd16(bits + 4) & 0x3FFF) == Int(qd_screen_row_bytes()))
            let b = rect(bits + 6)
            #expect(b[2] - b[0] == height && b[3] - b[1] == width)
        }

        /// What QuickDraw draws in the window lands where the window is on
        /// the screen as it now is.
        func expectDrawsAt(window w: UInt32) {
            let cont = bbox(rd32(w + UInt32(WindowRecord_contRgn_off)))
            tb_SetPort(w)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 10, 10].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_PaintRect(r)
            var px = [UInt8](repeating: 0, count: 4)
            machine_screen_rgba(Int32(cont[1] + 5), Int32(cont[0] + 5), 1, 1, &px)
            #expect(px[0] < 0x40 && px[1] < 0x40 && px[2] < 0x40)
            machine_screen_rgba(Int32(cont[1] + 15), Int32(cont[0] + 15), 1, 1, &px)
            #expect(px[0] > 0xC0)
        }

        /// A smaller display (or a larger scale): the screen shrinks, and a
        /// window over its new edge is moved back onto it.
        @Test func smallerScreenBringsWindowsBack() {
            let w = setUp([500, 600, 700, 900])
            defer { machine_set_screen(1024, 768) }
            #expect(machine_resize_screen(832, 624))
            expectDescribes(832, 624, window: w)
            let s = bbox(rd32(w + UInt32(WindowRecord_strucRgn_off)))
            #expect(s[2] <= 624 && s[3] <= 832 && s[0] >= 20 && s[1] >= 0)
            #expect(s[2] == 624 && s[3] == 832) // moved by as much as it was over
            expectDrawsAt(window: w)
        }

        /// A black-and-white window's port is brought up to date the same way.
        @Test func blackAndWhiteWindow() {
            let w = setUp([500, 600, 700, 900], color: false)
            defer { machine_set_screen(1024, 768) }
            #expect(machine_resize_screen(832, 624))
            expectDescribes(832, 624, window: w)
            expectDrawsAt(window: w)
        }

        /// A larger display (or a smaller scale): the screen grows and the
        /// windows stay where they were.
        @Test func largerScreenKeepsWindows() {
            let w = setUp([100, 100, 300, 400])
            defer { machine_set_screen(1024, 768) }
            let before = bbox(rd32(w + UInt32(WindowRecord_contRgn_off)))
            #expect(machine_resize_screen(1600, 1000))
            expectDescribes(1600, 1000, window: w)
            #expect(bbox(rd32(w + UInt32(WindowRecord_contRgn_off))) == before)
            expectDrawsAt(window: w)
        }

        /// A size a Mac could not have is made one it could (rows of 64).
        @Test func sizeIsAMacsSize() {
            _ = setUp([100, 100, 300, 400])
            defer { machine_set_screen(1024, 768) }
            #expect(machine_resize_screen(1300, 801))
            #expect(size() == (1280, 800))
        }

        /// A window too big for the new screen stays put while its top left
        /// is on it.
        @Test func oversizedWindowStays() {
            let w = setUp([40, 10, 760, 1010])
            defer { machine_set_screen(1024, 768) }
            let before = bbox(rd32(w + UInt32(WindowRecord_strucRgn_off)))
            #expect(machine_resize_screen(640, 480))
            #expect(bbox(rd32(w + UInt32(WindowRecord_strucRgn_off))) == before)
        }

        /// A program with the whole screen keeps it: one that hid the menu
        /// bar, or covers the screen with a window.
        @Test func wholeScreenProgramsKeepTheirScreen() {
            _ = setUp([100, 100, 300, 400])
            defer { machine_set_screen(1024, 768) }
            wr16(UInt32(LM_MBarHeight), 0)
            #expect(machine_screen_fixed() != nil)
            #expect(!machine_resize_screen(832, 624))
            #expect(size() == (1024, 768))

            _ = setUp([0, 0, 768, 1024])
            #expect(machine_screen_fixed().map { String(cString: $0) } == "it has a window covering the screen")
            #expect(!machine_resize_screen(832, 624))
        }

        /// The snapshot's arguments a resumed run repeats leave out what the
        /// settings now decide (a program launched by another at 1x carried
        /// --scale 1) and the screen size the record has.
        @Test func resumeDoesNotRepeatDisplaySettings() {
            let record = HibernateRecord(args: ["/Apps/Game", "--scale", "1", "--appearance", "bw", "--screen", "1728x1100",
                                                "--depth", "8", "--cpu", "68030"],
                                         program: "Game", title: "Game", saved: Date(), instructions: 0, drainedAt: "",
                                         screen: [1728, 1100])
            #expect(record.replayedArgs == ["/Apps/Game", "--depth", "8", "--cpu", "68030"])
            var old = record
            old.screen = nil // an older record: its --screen is all it has
            #expect(old.replayedArgs == ["/Apps/Game", "--screen", "1728x1100", "--depth", "8", "--cpu", "68030"])
        }
    }
}
