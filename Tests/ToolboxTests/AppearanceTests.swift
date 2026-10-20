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

extension BlueBox {
    /// Control Manager 2.0: embedding, activation and visibility following
    /// the embedder, data access, keyboard focus, and the Appearance CDEFs.
    @Suite struct ControlManager2 {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func window() -> UInt32 {
            let w = tb_NewCWindow(0, rect(40, 40, 240, 340), pstr("Test"), 1, 0, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            return w
        }

        func pixel(_ h: Int, _ v: Int) -> (UInt16, UInt16, UInt16) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return (rd16(c), rd16(c + 2), rd16(c + 4))
        }

        @Test func embeddingFollowsTheRoot() {
            launchTestApp()
            _ = initToolbox()
            let w = window()
            let out = tb_NewPtr(4, false, true)
            #expect(tb_GetRootControl(w, out) == -30586) // errNoRootControl
            #expect(tb_CreateRootControl(w, out) == 0)
            let root = rd32(out)
            #expect(root != 0)
            #expect(tb_CreateRootControl(w, out) == -30587) // errRootAlreadyExists
            // A group box, then a push button inside it: each goes in the deepest embedder holding it.
            let group = tb_NewControl(w, rect(10, 10, 150, 250), pstr("Group"), 1, 0, 0, 0, 160, 0)
            let button = tb_NewControl(w, rect(40, 30, 60, 110), pstr("OK"), 1, 0, 0, 1, 368, 0)
            let outside = tb_NewControl(w, rect(160, 10, 180, 100), pstr("Out"), 1, 0, 0, 1, 369, 0)
            let count = tb_NewPtr(2, false, true)
            #expect(tb_CountSubControls(root, count) == 0 && rd16(count) == 2)
            #expect(tb_CountSubControls(group, count) == 0 && rd16(count) == 1)
            #expect(tb_GetSuperControl(button, out) == 0 && rd32(out) == group)
            #expect(tb_GetSuperControl(outside, out) == 0 && rd32(out) == root)
            #expect(tb_GetIndexedSubControl(root, 1, out) == 0 && rd32(out) == group)
            #expect(tb_GetIndexedSubControl(root, 2, out) == 0 && rd32(out) == outside)
            // Deactivating the group deactivates the button; it comes back with it.
            #expect(tb_DeactivateControl(group) == 0)
            #expect(tb_IsControlActive(group) == 0 && tb_IsControlActive(button) == 0)
            #expect(tb_IsControlActive(outside) != 0)
            #expect(tb_ActivateControl(group) == 0)
            #expect(tb_IsControlActive(button) != 0)
            // One deactivated on its own stays so.
            _ = tb_DeactivateControl(button)
            _ = tb_DeactivateControl(group)
            _ = tb_ActivateControl(group)
            #expect(tb_IsControlActive(button) == 0)
            _ = tb_ActivateControl(button)
            // Hidden with its embedder, shown with it.
            tb_HideControl(group)
            #expect(tb_IsControlVisible(button) == 0)
            tb_ShowControl(group)
            #expect(tb_IsControlVisible(button) != 0)
            #expect(tb_SetControlVisibility(button, 0, 0) == 0 && tb_IsControlVisible(button) == 0)
            _ = tb_SetControlVisibility(button, 1, 1)
            // FindControl finds the button, not the group behind it.
            let hit = tb_NewPtr(4, false, true)
            #expect(tb_FindControl(0x0032_0040, w, hit) == 10 && rd32(hit) == button) // (v 50, h 64)
            // Disposing the group disposes the button.
            tb_DisposeControl(group)
            #expect(tb_CountSubControls(root, count) == 0 && rd16(count) == 1)
            tb_DisposeWindow(w)
        }

        @Test func dataAccessAndFocus() {
            launchTestApp()
            _ = initToolbox()
            let w = window()
            let out = tb_NewPtr(4, false, true)
            _ = tb_CreateRootControl(w, out)
            let label = tb_NewControl(w, rect(10, 10, 26, 200), 0, 1, 0, 0, 0, 288, 0)
            let text = Array("Hello, world".utf8)
            let buf = tb_NewPtr(64, false, true)
            for (i, b) in text.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(tb_SetControlData(label, 0, Int32(bitPattern: fourCC("text")), Int32(text.count), buf) == 0)
            let size = tb_NewPtr(4, false, true)
            #expect(tb_GetControlDataSize(label, 0, Int32(bitPattern: fourCC("text")), size) == 0)
            #expect(rd32(size) == UInt32(text.count))
            #expect(tb_GetControlData(label, 0, Int32(bitPattern: fourCC("bogs")), 4, buf, size) == -30581)
            let features = tb_NewPtr(4, false, true)
            #expect(tb_GetControlFeatures(label, features) == 0 && rd32(features) & (1 << 6) != 0) // data access
            // Two edit text fields: focus, and Tab's advance.
            let e1 = tb_NewControl(w, rect(40, 12, 56, 150), 0, 1, 0, 0, 0, 272, 0)
            let e2 = tb_NewControl(w, rect(70, 12, 86, 150), 0, 1, 0, 0, 0, 272, 0)
            #expect(tb_SetKeyboardFocus(w, e1, -1) == 0)
            #expect(tb_GetKeyboardFocus(w, out) == 0 && rd32(out) == e1)
            #expect(tb_HandleControlKey(e1, 0, 0x41, 0) == 5) // 'A' typed: kControlEditTextPart
            #expect(tb_GetControlData(e1, 0, Int32(bitPattern: fourCC("text")), 64, buf, size) == 0)
            #expect(rd32(size) == 1 && rd8(buf) == 0x41)
            #expect(tb_AdvanceKeyboardFocus(w) == 0)
            #expect(tb_GetKeyboardFocus(w, out) == 0 && rd32(out) == e2)
            #expect(tb_SetKeyboardFocus(w, label, -1) == -30582) // errControlDoesntSupportFocus
            #expect(tb_ClearKeyboardFocus(w) == 0)
            #expect(tb_GetKeyboardFocus(w, out) == 0 && rd32(out) == 0)
            tb_DisposeWindow(w)
        }

        /// Bevel buttons take their content and behaviour; the auto-toggle
        /// check box and tabs change their value when clicked.
        @Test func appearanceControlsBehave() {
            launchTestApp()
            _ = initToolbox()
            let w = window()
            // Toggling behaviour (0x0100) with no content.
            let bevel = tb_NewControl(w, rect(10, 10, 40, 60), pstr("B"), 1, 0, 0x0100, 0, 33, 0)
            #expect(tb_GetControlValue(bevel) == 0 && tb_GetControlMaximum(bevel) == 1)
            let info = tb_NewPtr(6, false, true)
            wr16(info, 3); wr16(info + 2, 999) // kControlContentPictRes, a picture that isn't there
            #expect(tb_SetBevelButtonContentInfo(bevel, info) == 0)
            let back = tb_NewPtr(6, false, true)
            #expect(tb_GetBevelButtonContentInfo(bevel, back) == 0 && rd16(back) == 3)
            ap_after_track(bevel, 10)
            #expect(tb_GetControlValue(bevel) == 1)
            ap_after_track(bevel, 10)
            #expect(tb_GetControlValue(bevel) == 0)
            let check = tb_NewControl(w, rect(50, 10, 66, 120), pstr("Auto"), 1, 0, 0, 1, 371, 0)
            ap_after_track(check, 11)
            #expect(tb_GetControlValue(check) == 1)
            // A progress bar's indeterminate flag.
            let progress = tb_NewControl(w, rect(80, 10, 94, 200), 0, 1, 30, 0, 100, 80, 0)
            var yes: UInt8 = 1
            let flag = tb_NewPtr(1, false, true)
            wr8(flag, yes)
            #expect(tb_SetControlData(progress, 0, Int32(bitPattern: fourCC("inde")), 1, flag) == 0)
            wr8(flag, 0)
            let got = tb_NewPtr(4, false, true)
            #expect(tb_GetControlData(progress, 0, Int32(bitPattern: fourCC("inde")), 1, flag, got) == 0)
            yes = rd8(flag)
            #expect(yes == 1)
            tb_DisposeWindow(w)
        }

        /// Appearance clients' push buttons are Platinum: grey faces, black frames.
        @Test func platinumPushButton() {
            machine_set_screen_depth(32)
            defer { machine_set_screen_depth(1) }
            launchTestApp()
            _ = initToolbox()
            #expect(tb_RegisterAppearanceClient() == 0)
            let w = window()
            let b = tb_NewControl(w, rect(20, 20, 40, 100), pstr("OK"), 1, 0, 0, 1, 368, 0)
            tb_Draw1Control(b)
            #expect(pixel(60, 20) == (0, 0, 0))                 // the frame along the top
            #expect(pixel(30, 34) == (0xDDDD, 0xDDDD, 0xDDDD))  // the face
            tb_HiliteControl(b, 10)
            #expect(pixel(30, 34) == (0x7777, 0x7777, 0x7777))  // pressed
            tb_HiliteControl(b, 255)
            #expect(pixel(60, 20) == (0x8888, 0x8888, 0x8888))  // inactive frame
            // CDEF 0's buttons too, for a client.
            let old = tb_NewControl(w, rect(60, 20, 80, 100), pstr("Old"), 1, 0, 0, 1, 0, 0)
            tb_Draw1Control(old)
            #expect(pixel(30, 74) == (0xDDDD, 0xDDDD, 0xDDDD))
            tb_DisposeWindow(w)
        }
    }
}

extension BlueBox {
    /// Mac OS 8's Dialog Manager: 'dlgx' dialogs with a control hierarchy
    /// and the theme's background, items as controls, AutoSizeDialog.
    @Suite struct AppearanceDialogs {
        static func text(_ s: String) -> [UInt8] { Array(s.utf8) }
        static let items = ditl([
            ditlItem(4, (160, 220, 180, 290), text("OK")),             // 1 button
            ditlItem(5, (20, 20, 36, 200), text("Remember me")),      // 2 check box
            ditlItem(8, (50, 20, 66, 280), text("Hello")),            // 3 static text
            ditlItem(16, (80, 20, 96, 200), text("abc")),             // 4 edit text
            ditlItem(0, (110, 20, 130, 60), []),                      // 5 user item
            ditlItem(8, (140, 20, 156, 120), text("A longer text that wraps onto more lines in its box")), // 6
            ditlItem(4, (190, 220, 210, 290), text("Below")),          // 7 button below it
        ])

        func open(flags: UInt32) -> UInt32 {
            launchTestApp([TestResource(type: "DLOG", id: 210, data: dlog(items: 210)),
                           TestResource(type: "DITL", id: 210, data: Self.items),
                           TestResource(type: "dlgx", id: 210, data: cat(be16(0), be32(flags)))])
            _ = initToolbox()
            let d = tb_GetNewDialog(210, 0, 0xFFFF_FFFF)
            #expect(d != 0)
            return d
        }

        func item(_ d: UInt32, _ n: Int16) -> (handle: UInt32, rect: [Int16]) {
            let t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
            tb_GetDialogItem(d, n, t, h, r)
            return (rd32(h), (0..<4).map { Int16(bitPattern: rd16(r + UInt32($0 * 2))) })
        }

        func controlText(_ ch: UInt32) -> String {
            let buf = tb_NewPtr(256, false, true), size = tb_NewPtr(4, false, true)
            #expect(tb_GetControlData(ch, 0, Int32(bitPattern: fourCC("text")), 256, buf, size) == 0)
            return String(decoding: (0..<Int(rd32(size))).map { rd8(buf + UInt32($0)) }, as: UTF8.self)
        }

        @Test func itemsAsControls() {
            let d = open(flags: 0x0B) // theme background, control hierarchy, theme controls
            let out = tb_NewPtr(4, false, true)
            #expect(tb_GetRootControl(d, out) == 0)
            let root = rd32(out)
            let count = tb_NewPtr(2, false, true)
            #expect(tb_CountSubControls(root, count) == 0 && rd16(count) >= 3) // the buttons and the check box
            #expect(tb_GetDialogItemAsControl(d, 1, out) == 0 && rd32(out) == item(d, 1).handle)
            #expect(tb_GetSuperControl(item(d, 1).handle, out) == 0 && rd32(out) == root)
            // Static text: the control's text is the item's.
            #expect(tb_GetDialogItemAsControl(d, 3, out) == 0)
            let label = rd32(out)
            #expect(controlText(label) == "Hello")
            let bye = Array("Bye".utf8), buf = tb_NewPtr(8, false, true)
            for (i, b) in bye.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(tb_SetControlData(label, 0, Int32(bitPattern: fourCC("text")), 3, buf) == 0)
            let s = tb_NewPtr(256, false, true)
            tb_GetDialogItemText(item(d, 3).handle, s)
            #expect(pstring(at: s) == "Bye")
            #expect(tb_GetDialogItemAsControl(d, 3, out) == 0 && rd32(out) == label) // the same one again
            // Edit text: its text, and it has the keyboard focus.
            #expect(tb_GetDialogItemAsControl(d, 4, out) == 0)
            let field = rd32(out)
            #expect(controlText(field) == "abc")
            #expect(tb_GetKeyboardFocus(d, out) == 0 && rd32(out) == field)
            // A user item is a user pane.
            #expect(tb_GetDialogItemAsControl(d, 5, out) == 0 && rd32(out) != 0)
            let features = tb_NewPtr(4, false, true)
            #expect(tb_GetControlFeatures(rd32(out), features) == 0 && rd32(features) & (1 << 1) != 0) // embedding
            // The theme's background.
            tb_SetPort(d)
            let c = tb_NewPtr(6, false, true)
            tb_GetBackColor(c)
            #expect(rd16(c) == 0xDDDD)
            tb_DisposeDialog(d)
        }

        @Test func autoSizeDialogGrowsTextItems() {
            let d = open(flags: 0)
            let before = item(d, 6).rect, below = item(d, 7).rect
            #expect(tb_AutoSizeDialog(d) == 0)
            let after = item(d, 6).rect
            #expect(after[2] > before[2])                       // taller
            let grown = after[2] - before[2]
            #expect(item(d, 7).rect[0] == below[0] + grown)     // the button below moved down
            #expect(item(d, 1).rect[0] == 160 + grown)
            #expect(item(d, 2).rect[0] == 20)                    // the check box above did not
            #expect(tb_MoveDialogItem(d, 2, 30, 25) == 0 && item(d, 2).rect == [25, 30, 41, 210])
            #expect(tb_SizeDialogItem(d, 2, 100, 20) == 0 && item(d, 2).rect == [25, 30, 45, 130])
            tb_DisposeDialog(d)
        }

        @Test func newFeaturesDialogMakesARoot() {
            launchTestApp([TestResource(type: "DITL", id: 211, data: Self.items)])
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            for (i, v) in [50, 50, 300, 400].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            let items = tb_GetResource(Int32(bitPattern: fourCC("DITL")), 211)
            _ = tb_DetachResource(items)
            let d = tb_NewFeaturesDialog(0, r, 0, 0, 1, 0xFFFF_FFFF, 0, 0, items, 2)
            let out = tb_NewPtr(4, false, true)
            #expect(tb_GetRootControl(d, out) == 0 && rd32(out) != 0)
            tb_DisposeDialog(d)
        }
    }
}
