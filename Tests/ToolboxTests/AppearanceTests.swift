import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Appearance Manager: Gestalt, theme brushes in the port, and the
    /// Platinum primitives' pixels.
    @Suite struct AppearanceManager {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        /// A 32-bit GWorld, current, filled white.
        func gworld(_ w: Int = 100, _ h: Int = 60) -> UInt32 {
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, h, w), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            _ = tb_LockPixels(tb_GetGWorldPixMap(rd32(gw)))
            tb_EraseRect(rect(0, 0, h, w))
            return rd32(gw)
        }

        func pixel(_ h: Int, _ v: Int) -> (UInt16, UInt16, UInt16) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return (rd16(c), rd16(c + 2), rd16(c + 4))
        }

        func grey(_ v: UInt16) -> (UInt16, UInt16, UInt16) { (v, v, v) }

        /// 'appr' says the Appearance Manager is there, and 'apvr' which.
        @Test func gestaltReportsAppearance() {
            launchTestApp()
            var resp: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("appr")), &resp) == 0 && resp & 1 == 1)
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("apvr")), &resp) == 0 && resp == 0x0110)
        }

        @Test func registeringAsAClient() {
            launchTestApp()
            #expect(tb_IsAppearanceClient(0) == 0)
            #expect(tb_RegisterAppearanceClient() == 0)
            #expect(tb_RegisterAppearanceClient() == -30561) // themeProcessRegisteredErr
            #expect(tb_IsAppearanceClient(0) != 0)
            #expect(tb_UnregisterAppearanceClient() == 0)
            #expect(tb_IsAppearanceClient(0) == 0)
        }

        /// SetThemeBackground and SetThemeTextColor set the port's colours;
        /// the highlight brush is HiliteRGB.
        @Test func brushesGoToThePort() {
            launchTestApp()
            _ = initToolbox()
            let gw = gworld()
            #expect(tb_SetThemeBackground(1, 32, 1) == 0) // kThemeBrushDialogBackgroundActive
            let c = tb_NewPtr(6, false, true)
            tb_GetBackColor(c)
            #expect(rd16(c) == 0xDDDD && rd16(c + 2) == 0xDDDD && rd16(c + 4) == 0xDDDD)
            tb_EraseRect(rect(0, 0, 10, 10))
            #expect(pixel(5, 5) == grey(0xDDDD))
            #expect(tb_SetThemeTextColor(2, 32, 1) == 0) // kThemeTextColorDialogInactive
            tb_GetForeColor(c)
            #expect(rd16(c) == 0x7777)
            wr16(UInt32(LM_HiliteRGB), 0x1111); wr16(UInt32(LM_HiliteRGB) + 2, 0x2222); wr16(UInt32(LM_HiliteRGB) + 4, 0x3333)
            #expect(tb_GetThemeBrushAsColor(-3, 32, 1, c) == 0) // kThemeBrushPrimaryHighlightColor
            #expect(rd16(c) == 0x1111 && rd16(c + 2) == 0x2222 && rd16(c + 4) == 0x3333)
            #expect(tb_SetThemePen(99, 32, 1) == -30560) // themeInvalidBrushErr
            // In black and white the dialog grey is white.
            #expect(tb_GetThemeBrushAsColor(1, 1, 0, c) == 0 && rd16(c) == 0xFFFF)
            tb_DisposeGWorld(gw)
        }

        /// The host's selection colour reaches HiliteRGB at the next event call.
        @Test func highlightFollowsTheHost() {
            launchTestApp()
            _ = initToolbox()
            machine_set_hilite_color(0x0101, 0x0202, 0x0303)
            let evt = tb_NewPtr(16, false, true)
            _ = tb_GetNextEvent(0, evt)
            #expect(rd16(UInt32(LM_HiliteRGB)) == 0x0101 && rd16(UInt32(LM_HiliteRGB) + 4) == 0x0303)
            machine_set_hilite_color(0xCCCC, 0xCCCC, 0xFFFF)
            _ = tb_GetNextEvent(0, evt)
        }

        /// Placards are framed grey panels; edit text frames are drawn outside
        /// their rectangle; group boxes are etched.
        @Test func primitivesDrawPlatinum() {
            launchTestApp()
            _ = initToolbox()
            let gw = gworld()
            #expect(tb_DrawThemePlacard(rect(0, 0, 20, 40), 1) == 0)
            #expect(pixel(0, 0) == grey(0))          // the frame
            #expect(pixel(1, 1) == grey(0xFFFF))     // the light edge
            #expect(pixel(20, 10) == grey(0xDDDD))   // the face
            #expect(pixel(38, 18) == grey(0xAAAA))   // the dark edge
            #expect(tb_DrawThemeEditTextFrame(rect(30, 10, 40, 60), 1) == 0)
            #expect(pixel(30, 35) == grey(0xFFFF))   // inside: untouched
            #expect(pixel(30, 29) == grey(0))        // the dark ring, a pixel out
            #expect(pixel(30, 28) == grey(0x8888))   // the shadow, two out
            tb_EraseRect(rect(0, 0, 60, 100))
            #expect(tb_DrawThemePrimaryGroup(rect(5, 5, 50, 90), 1) == 0)
            #expect(pixel(5, 20) == grey(0x8888) && pixel(20, 6) == grey(0xFFFF))
            tb_DisposeGWorld(gw)
        }

        /// DrawThemeButton: a checked box has its mark; a pressed push button is dark.
        @Test func themeButtons() {
            launchTestApp()
            _ = initToolbox()
            let gw = gworld()
            let info = tb_NewPtr(8, false, true)
            wr32(info, 1); wr16(info + 4, 1) // active, on
            #expect(tb_DrawThemeButton(rect(0, 0, 12, 60), 1, info, 0, 0, 0, 0) == 0) // kThemeCheckBox
            #expect(pixel(0, 0) == grey(0))                 // the box's frame
            let marked = (0..<12).contains { x in (0..<12).contains { y in pixel(x, y) == grey(0) && x > 2 && y > 2 && x < 10 } }
            #expect(marked)
            wr32(info, 2) // pressed
            #expect(tb_DrawThemeButton(rect(20, 10, 40, 80), 0, info, 0, 0, 0, 0) == 0) // kThemePushButton
            #expect(pixel(40, 30) == grey(0x7777))
            #expect(pixel(40, 20) == grey(0))               // the frame along the top
            tb_DisposeGWorld(gw)
        }

        /// A progress bar fills its value's share with the accent colour.
        @Test func progressTrack() {
            launchTestApp()
            _ = initToolbox()
            let gw = gworld()
            let info = tb_NewPtr(36, false, true)
            wr16(info, 3) // kThemeProgressBar
            for (i, v) in [0, 0, 14, 100].enumerated() { wr16(info + 2 + UInt32(i * 2), UInt16(v)) }
            wr32(info + 10, 0); wr32(info + 14, 100); wr32(info + 18, 50)
            wr16(info + 26, 1) // horizontal
            #expect(tb_DrawThemeTrack(info, 0, 0, 0) == 0)
            let filled = pixel(20, 8), empty = pixel(80, 8)
            #expect(filled != empty)
            #expect(empty == grey(0xEEEE))
            #expect(filled.2 > filled.0) // the accent: bluer than it is red
            let bounds = tb_NewPtr(8, false, true)
            #expect(tb_GetThemeTrackBounds(info, bounds) == 0)
            tb_DisposeGWorld(gw)
        }

        /// The 68k trap: SetThemeBackground through _AppearanceDispatch.
        @Test func appearanceDispatchFrom68k() {
            launchTestApp()
            _ = initToolbox()
            let gw = gworld()
            let words: [UInt16] = [
                0x42A7,                 // CLR.L -(SP)        result space
                0x3F3C, 0x0001,         // MOVE.W #kThemeBrushDialogBackgroundActive,-(SP)
                0x3F3C, 0x0020,         // MOVE.W #32,-(SP)
                0x1F3C, 0x0001,         // MOVE.B #1,-(SP)
                0x303C, 0x0002,         // MOVE.W #2,D0
                0xAA74,                 // _AppearanceDispatch
                0x2E1F,                 // MOVE.L (SP)+,D7
                0xA9F4,                 // _ExitToShell
            ]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let cpu = g_cpu!
            let stack = tb_NewPtr(65536, false, true)
            cpu.pointee.a.7 = stack + 65536 - 16
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, 10_000)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
            #expect(cpu.pointee.d.7 == 0)
            let c = tb_NewPtr(6, false, true)
            tb_GetBackColor(c)
            #expect(rd16(c) == 0xDDDD)
            tb_DisposeGWorld(gw)
        }

        @Test func metricsAndFonts() {
            launchTestApp()
            let v = tb_NewPtr(4, false, true)
            #expect(tb_GetThemeMetric(0, v) == 0 && rd32(v) == 16) // kThemeMetricScrollBarWidth
            let name = tb_NewPtr(256, false, true), size = tb_NewPtr(2, false, true), style = tb_NewPtr(2, false, true)
            #expect(tb_GetThemeFont(1, 0, name, size, style) == 0) // kThemeSmallSystemFont
            #expect(pstring(at: name) == "Geneva" && rd16(size) == 10)
            let ctab = tb_NewPtr(4, false, true)
            #expect(tb_GetThemeAccentColors(ctab) == 0 && rd32(ctab) != 0)
            #expect(rd16(rd32(rd32(ctab)) + 6) == 7) // eight colours
        }
    }
}
