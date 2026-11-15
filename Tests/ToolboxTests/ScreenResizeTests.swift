import BlueBoxHost
import CCore
import CPU68K
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

        /// A program that has the whole screen but draws with QuickDraw
        /// (a hidden menu bar, a window covering the screen) may have a
        /// larger screen, not a smaller one: it laid out what it shows for
        /// the screen it had, which a smaller one would cut off.
        @Test func wholeScreenProgramsGrowOnly() {
            _ = setUp([100, 100, 300, 400])
            defer { machine_set_screen(1024, 768) }
            wr16(UInt32(LM_MBarHeight), 0)
            #expect(machine_screen_fixed() == nil)
            #expect(machine_screen_fixed_at(832, 624).map { String(cString: $0) }
                    == "it has the whole screen, laid out for a larger one")
            #expect(!machine_resize_screen(832, 624))
            #expect(size() == (1024, 768))
            #expect(machine_resize_screen(1280, 960))
            #expect(size() == (1280, 960))
        }

        /// A window covering the screen (a game's) covers the larger one,
        /// and what is new beyond the old screen is made from its edge, not
        /// erased: a game paints its backdrop once.
        @Test func coveringWindowGrowsWithTheScreen() {
            let w = setUp([0, 0, 768, 1024])
            defer { machine_set_screen(1024, 768) }
            #expect(machine_screen_whole().map { String(cString: $0) } == "it has a window covering the screen")
            tb_SetPort(w)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 768, 1024].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_PaintRect(r) // a black backdrop
            #expect(!machine_resize_screen(832, 624))
            #expect(machine_resize_screen(1280, 960))
            #expect(bbox(rd32(w + UInt32(WindowRecord_contRgn_off))) == [0, 0, 960, 1280])
            var px = [UInt8](repeating: 0, count: 4)
            for (x, y) in [(1200, 400), (500, 900), (1270, 950), (10, 30)] {
                machine_screen_rgba(Int32(x), Int32(y), 1, 1, &px)
                #expect(px[0] < 0x40 && px[1] < 0x40 && px[2] < 0x40, Comment(rawValue: "\(x),\(y): \(px)"))
            }
            // and no update for it, which would erase it white again
            #expect(bbox(rd32(w + UInt32(WindowRecord_updateRgn_off)))[3] <= 1024)
        }

        /// What the screen shows stays where it was.
        @Test func contentsStay() {
            let w = setUp([100, 100, 300, 400])
            defer { machine_set_screen(1024, 768) }
            tb_SetPort(w)
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [20, 20, 40, 40].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            tb_PaintRect(r)
            #expect(machine_resize_screen(1280, 960))
            var px = [UInt8](repeating: 0, count: 4)
            machine_screen_rgba(130, 130, 1, 1, &px)
            #expect(px[0] < 0x40)
        }

        /// A program whose own code stores into the screen keeps it: it
        /// may hold the screen's address and row length itself.
        @Test func programThatDrawsOnTheScreenKeepsIt() {
            _ = setUp([100, 100, 300, 400])
            defer { machine_set_screen(1024, 768) }
            display_sync()
            let at = rd32(UInt32(LM_ScrnBase)) + 150 * qd_screen_row_bytes() + 600 * UInt32(qd_screen_depth()) / 8
            let words: [UInt16] = [0x23FC, 0xFFFF, 0xFFFF, UInt16(at >> 16), UInt16(at & 0xFFFF), 0xA9F4]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(4096, false, true) + 4096 - 16
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, 100)
            display_sync()
            #expect(machine_screen_fixed().map { String(cString: $0) } == "it draws on the screen itself")
            #expect(!machine_resize_screen(1280, 960))
            #expect(size() == (1024, 768))
        }

        /// The host's request (its window or display resized) is taken at
        /// the program's next event call.
        @Test func requestTakenAtTheNextEventCall() {
            _ = setUp([100, 100, 300, 400])
            defer { machine_set_screen(1024, 768) }
            machine_request_screen_size(1152, 870)
            #expect(size() == (1024, 768))
            machine_screen_request_poll()
            #expect(size() == (1152, 870))
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
