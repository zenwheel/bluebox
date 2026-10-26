import CCore
import CPU68K
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// Mac OS 8.5 to 9's Toolbox libraries' own calls (os9_toolbox.c):
    /// WindowsLib, ControlsLib, MenusLib and AppearanceLib's private
    /// exports, which programs built for 8.5 and later can import.
    @Suite struct MacOS9Toolbox {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func rectAt(_ p: UInt32) -> [Int16] { (0..<4).map { Int16(bitPattern: rd16(p + UInt32($0 * 2))) } }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            wr8(p, UInt8(s.utf8.count))
            for (i, b) in s.utf8.enumerated() { wr8(p + 1 + UInt32(i), b) }
            return p
        }

        func newWindow(_ cls: UInt32, _ attrs: UInt32, _ r: UInt32) -> UInt32 {
            let out = tb_NewPtr(4, false, true)
            #expect(tb_CreateNewWindow(cls, attrs, r, out) == 0)
            let w = rd32(out)
            tb_ShowWindow(w)
            return w
        }

        /// A screen pixel, read through the Window Manager's colour port.
        func screenPixel(_ h: Int, _ v: Int) -> UInt16 {
            let saved = tb_NewPtr(4, false, true)
            tb_GetPort(saved)
            tb_SetPort(rd32(UInt32(LM_WMgrCPort)))
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            tb_SetPort(rd32(saved))
            return rd16(c)
        }

        /// Paint a screen rectangle black or white.
        func paintScreen(_ r: UInt32, black: Bool) {
            let saved = tb_NewPtr(4, false, true)
            tb_GetPort(saved)
            let wp = rd32(UInt32(LM_WMgrCPort))
            tb_SetPort(wp)
            tb_SetRectRgn(rd32(wp + 28), -32767, -32767, 32767, 32767) // clipRgn
            tb_ForeColor(33) // blackColor
            tb_BackColor(30) // whiteColor
            if black { tb_PaintRect(r) } else { tb_EraseRect(r) }
            tb_SetPort(rd32(saved))
        }

        /// Every one of the four libraries' exports that Mac OS 9 has and
        /// Blue Box lacked answers now; the Control Manager's "__" entry
        /// points are its calls.
        @Test func librariesExportTheirPrivateCalls() {
            launchTestApp()
            _ = initToolbox()
            for name in ["GetWindowSystemProperty", "WMCollapseWindow", "ActivateFloatingWindows", "SaveScreenBits",
                         "GetMonitorDesktopAlignment", "__GetControlValue", "__ScrollDelay", "SetControlSystemProperty",
                         "StandardMBDF", "StandardMDEF", "EnableMenuBarClock", "CacheMenuPixels", "GetSystemFont",
                         "SetThemeScrollBarArrowStyle", "GetThemeVariantList", "GetMenuItemBounds",
                         "InstallControlManagerDragHandlers", "SetThemePlaylist"] {
                #expect(cfm_export_fn(name) != nil, "\(name)")
            }
            #expect(cfm_export_fn("__NotAControlCall") == nil)
            let w = newWindow(6, 0x1F, rect(100, 100, 200, 300))
            let ctl = svcCall("__NewControl", w, rect(20, 20, 40, 120), pstr("OK"), 1, 0, 0, 1, 0, 0)
            #expect(ctl != 0 && tb_GetControlValue(ctl) == 0)
            svcCall("__SetControlValue", ctl, 1)
            #expect(svcCall("__GetControlValue", ctl) == 1 && svcCall("GetControlValue", ctl) == 1)
            #expect(svcCall("__GetControlMaximum", ctl) == 1)
            // the scroll arrows' pause, cut short with the button up
            let now = tb_TickCount()
            #expect(svcCall("__ScrollDelay", now, now) == 0)
        }

        /// The System's properties on windows, controls and menu items
        /// are kept under its creator 'macs', apart from a program's.
        @Test func systemProperties() {
            launchTestApp()
            _ = initToolbox()
            let macs = fourCC("macs"), tag = fourCC("tst1")
            let data = tb_NewPtr(8, false, true), back = tb_NewPtr(8, false, true), size = tb_NewPtr(4, false, true)
            wr32(data, 0x1234_5678)

            let w = newWindow(6, 0x1F, rect(100, 100, 200, 300))
            #expect(svcCall("SetWindowSystemProperty", w, tag, 4, data) == 0)
            #expect(tb_GetWindowProperty(w, macs, tag, 4, size, back) == 0 && rd32(back) == 0x1234_5678)
            #expect(tb_GetWindowProperty(w, fourCC("Test"), tag, 4, size, back) == -5604)
            #expect(svcCall("GetWindowSystemPropertySize", w, tag, size) == 0 && rd32(size) == 4)
            wr32(back, 0)
            #expect(svcCall("GetWindowSystemProperty", w, tag, 8, size, back) == 0 && rd32(back) == 0x1234_5678)
            #expect(svcCall("RemoveWindowSystemProperty", w, tag) == 0)
            #expect(svcStatus(svcCall("GetWindowSystemPropertySize", w, tag, size)) == -5604)

            let root = tb_NewPtr(4, false, true)
            _ = tb_CreateRootControl(w, root)
            let ctl = tb_NewControl(w, rect(20, 20, 40, 120), pstr("OK"), 1, 0, 0, 1, 368, 0)
            #expect(svcCall("SetControlSystemProperty", ctl, tag, 4, data) == 0)
            #expect(tb_GetControlProperty(ctl, Int32(bitPattern: macs), Int32(bitPattern: tag), 4, size, back) == 0)
            wr32(back, 0)
            #expect(svcCall("GetControlSystemProperty", ctl, tag, 4, size, back) == 0 && rd32(back) == 0x1234_5678)
            #expect(svcCall("GetControlSystemPropertySize", ctl, tag, size) == 0 && rd32(size) == 4)
            #expect(svcCall("RemoveControlSystemProperty", ctl, tag) == 0)
            #expect(svcStatus(svcCall("GetControlSystemProperty", ctl, tag, 4, size, back)) == -5604)

            let menu = tb_NewMenu(200, pstr("Test"))
            tb_AppendMenu(menu, pstr("One;Two"))
            #expect(svcCall("SetMenuItemSystemProperty", menu, 2, tag, 4, data) == 0)
            wr32(back, 0)
            #expect(svcCall("GetMenuItemProperty", menu, 2, macs, tag, 4, size, back) == 0 && rd32(back) == 0x1234_5678)
            wr32(back, 0)
            #expect(svcCall("GetMenuItemSystemProperty", menu, 2, tag, 4, size, back) == 0 && rd32(back) == 0x1234_5678)
            #expect(svcCall("GetMenuItemSystemPropertySize", menu, 2, tag, size) == 0 && rd32(size) == 4)
            #expect(svcCall("RemoveMenuItemSystemProperty", menu, 2, tag) == 0)
            #expect(svcCall("GetMenuItemSystemProperty", menu, 2, tag, 4, size, back) != 0)
        }

        /// WindowsLib: floating windows deactivated and activated, the
        /// features the System sets, the window's process and title, and
        /// double-clicking a title bar to collapse.
        @Test func windowsLibWindows() {
            launchTestApp()
            _ = initToolbox()
            #expect(tb_InitFloatingWindows() == 0)
            let doc = newWindow(6, 0x1F, rect(100, 100, 200, 300))
            let palette = newWindow(5, 0x01, rect(80, 400, 160, 480))
            #expect(svcCall("AreFloatingWindowsActive") == 1)
            #expect(WindowRecord_hilited(palette) != 0)
            #expect(svcCall("DeactivateFloatingWindows") == 0 && svcCall("AreFloatingWindowsActive") == 0)
            #expect(WindowRecord_hilited(palette) == 0 && WindowRecord_hilited(doc) != 0)
            #expect(svcCall("ActivateFloatingWindows") == 0 && WindowRecord_hilited(palette) != 0)

            let v = tb_NewPtr(8, false, true)
            #expect(svcCall("SetWindowFeatures", doc, 0x41) == 0)
            #expect(tb_GetWindowFeatures(doc, v) == 0 && rd32(v) == 0x41)
            #expect(svcCall("WMGetWindowFeatures", doc, v) == 0 && rd32(v) == 0x41)
            #expect(svcStatus(svcCall("SetWindowFeatures", 0x1234, 0)) == -5600)

            #expect(svcCall("GetWindowProcess", doc, v) == 0)
            let psn = tb_NewPtr(8, false, true)
            _ = tb_GetCurrentProcess(psn)
            #expect(rd32(v) == rd32(psn) && rd32(v + 4) == rd32(psn + 4))
            tb_SetWTitle(doc, pstr("Seven!!"))
            #expect(svcCall("GetWindowTitleLength", doc, v) == 0 && rd32(v) == 7)
            #expect(svcCall("GetWindowVisualClass", palette, v) == 0 && rd32(v) == 5)
            #expect(svcCall("WindowHasUnicodeTitle", doc) == 0 && svcStatus(svcCall("GetWindowUnicodeTitle", doc)) == -50)

            #expect(svcCall("SetWindowGadgetDropThreshold", doc, 6) == 0)
            #expect(svcCall("GetWindowGadgetDropThreshold", doc, v) == 0 && rd32(v) == 6)

            // a title bar double-clicked: collapsed, or nothing when that's off
            #expect(svcCall("SetWindowFeatures", doc, 0x5F) == 0)
            #expect(svcCall("GetWindowDoubleClickAction") == 1)
            #expect(svcCall("DoubleClickWindowTitle", doc, 0) == 1 && tb_IsWindowCollapsed(doc) != 0)
            #expect(svcCall("WMIsWindowCollapsed", doc) != 0)
            #expect(svcCall("DoubleClickWindowTitle", doc, 0) == 1 && tb_IsWindowCollapsed(doc) == 0)
            #expect(svcCall("SetWindowDoubleClickAction", 0) == 0)
            #expect(svcCall("DoubleClickWindowTitle", doc, 0) == 0 && tb_IsWindowCollapsed(doc) == 0)
            #expect(svcStatus(svcCall("SetWindowDoubleClickAction", 4)) == -50)

            // FindWindow over every program's windows
            #expect(svcCall("GlobalFindWindow", 0x0096_00C8, v) == 3 && rd32(v) == doc) // (150, 200): content

            // activate events posted as the Window Manager queues them
            svcCall("PostWindowActivateEvent", palette, 0)
            #expect(rd32(UInt32(LM_CurDeactive)) == palette)
        }

        /// The screen under a rectangle saved and put back, also moved;
        /// and the desk's pattern set and read.
        @Test func savedScreenBitsAndDesktop() {
            launchTestApp()
            _ = initToolbox()
            let r = rect(100, 100, 120, 140)
            paintScreen(r, black: true)
            #expect(screenPixel(110, 110) == 0)
            #expect(svcCall("IsScreenBitSavingEnabled") == 1)
            let out = tb_NewPtr(4, false, true)
            #expect(svcCall("SaveScreenBits", r, 0, out) == 0 && rd32(out) != 0)
            let bits = rd32(out)
            #expect(svcCall("GetSavedScreenBitsRepresentativePixMap", bits) != 0)
            paintScreen(r, black: false)
            #expect(screenPixel(110, 110) == 0xFFFF)
            #expect(svcCall("RestoreScreenBits", bits) == 0)
            #expect(screenPixel(110, 110) == 0)
            // moved to 200,300: put back there
            paintScreen(rect(300, 200, 320, 240), black: false)
            svcCall("MoveSavedScreenBits", bits, 200, 300)
            #expect(svcCall("RestoreScreenBits", bits) == 0)
            #expect(screenPixel(210, 310) == 0)
            #expect(svcCall("DisposeSavedScreenBits", bits) == 0)
            // saving turned off: nothing kept
            #expect(svcCall("SetScreenBitSavingEnabled", 0) == 0)
            #expect(svcCall("SaveScreenBits", r, 0, out) == 0 && rd32(out) == 0)

            let pp = tb_NewPtr(4, false, true), flag = tb_NewPtr(2, false, true)
            #expect(svcCall("GetDesktopPattern", pp, flag) == 0 && rd32(pp) != 0)
            let pat = rd32(pp)
            for i in 0..<8 { wr8(rd32(pat) + 20 + UInt32(i), 0x0F) } // pat1Data
            #expect(svcCall("SetDesktopPattern", pat, flag) == 0)
            #expect(rd8(UInt32(LM_DeskPattern)) == 0x0F)
            #expect(svcStatus(svcCall("SetDesktopPattern", 0, flag)) == -50)
            // the desktop pictures' lists: not known, so not pretended
            #expect(svcStatus(svcCall("CountMonitorDesktopRecordsInList", 0xFFFF_FFFF, out)) == -4)
        }

        /// MenusLib: the standard menu bar definition's messages, the bar's
        /// height, submenus' parents, cached menu pixels and the clock.
        @Test func menusLib() {
            launchTestApp()
            _ = initToolbox()
            let file = tb_NewMenu(128, pstr("File"))
            tb_AppendMenu(file, pstr("New;Open;Quit"))
            tb_InsertMenu(file, 0)
            let edit = tb_NewMenu(129, pstr("Edit"))
            tb_AppendMenu(edit, pstr("Cut;Copy"))
            tb_InsertMenu(edit, 0)
            tb_DrawMenuBar()
            #expect(svcCall("StandardMBDF", 0, 6, 0, 0) == 20) // kMenuBarHeightMsg
            #expect(svcCall("CallMenuBar", 6, 0, 0) == 20)
            #expect(svcCall("StandardMBDF", 0, 1, 0, 0x0005_0010) == 6) // hit (5, 16): File, the first menu
            #expect(svcStatus(svcCall("StandardMBDF", 0, 1, 0, 0x0064_0010)) == -1) // (100, 16): not in the bar
            let mr = svcCall("StandardMBDF", 0, 9, 12, 0) // kMenuBarRectMsg: Edit's rectangle
            #expect(mr != 0 && rectAt(mr)[0] == 20)
            svcCall("StandardMBDF", 0, 5, 0, 0x0001_0006) // hilite File
            #expect(rd16(UInt32(LM_TheMenu)) == 128)
            svcCall("StandardMBDF", 0, 5, 0, 0x0000_0006)
            #expect(rd16(UInt32(LM_TheMenu)) == 0)

            // the standard MDEF, and a menu's own called: sized
            MenuInfo_set_menuWidth(rd32(edit), 0)
            let which = tb_NewPtr(2, false, true)
            svcCall("StandardMDEF", 2, edit, rect(0, 0, 0, 0), 0, which) // mSizeMsg
            #expect(MenuInfo_menuWidth(rd32(edit)) > 0)
            MenuInfo_set_menuWidth(rd32(edit), 0)
            svcCall("InvalidateAllMenuSizes")
            #expect(MenuInfo_menuWidth(rd32(edit)) > 0)

            svcCall("SetMenuBarHeight", 24)
            #expect(rd16(UInt32(LM_MBarHeight)) == 24)
            svcCall("SetMenuBarHeight", 20)

            let b = tb_NewPtr(2, false, true)
            #expect(svcCall("SetSubmenuParentChoosable", file, 2, 1) == 0)
            #expect(svcCall("GetSubmenuParentChoosable", file, 2, b) == 0 && rd8(b) == 1)
            #expect(svcCall("GetSubmenuParentChoosable", file, 1, b) == 0 && rd8(b) == 0)

            // GetMenuItemBounds: the item's rectangle in its menu, from
            // PowerPC code and through _MenuDispatch
            let br = tb_NewPtr(8, false, true)
            #expect(svcCall("GetMenuItemBounds", file, 2, br) == 0 && rectAt(br) == [16, 0, 32, MenuInfo_menuWidth(rd32(file))])
            #expect(svcCall("SetMenuItemPencilGlyph", file, 3, 1) == 0)
            #expect(svcCall("GetMenuItemPencilGlyph", file, 3, b) == 0 && rd8(b) == 1)
            gmemset(br, 0, 8)
            let words: [UInt16] = [
                0x4267,                         // CLR.W -(SP)        result
                0x2F3C, UInt16(file >> 16), UInt16(file & 0xFFFF), // MOVE.L #menu,-(SP)
                0x3F3C, 0x0003,                 // MOVE.W #3,-(SP)
                0x2F3C, UInt16(br >> 16), UInt16(br & 0xFFFF), // MOVE.L #rect,-(SP)
                0x303C, 0x0515,                 // MOVE.W #$0515,D0
                0xA825,                         // _MenuDispatch
                0x3E1F,                         // MOVE.W (SP)+,D7
                0xA9F4,                         // _ExitToShell
            ]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let cpu = g_cpu!
            let stack = tb_NewPtr(65536, false, true)
            cpu.pointee.a.7 = stack + 65536 - 16
            cpu.pointee.d.7 = 0xFFFF
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, 10_000)
            #expect(String(cString: machine_halt_reason()) == "ExitToShell")
            #expect(cpu.pointee.d.7 & 0xFFFF == 0)
            #expect(rectAt(br)[0] == 32 && rectAt(br)[2] == 48)
        }

        @Test func menuCacheAndClock() {
            launchTestApp()
            _ = initToolbox()
            let menu = tb_NewMenu(130, pstr("View"))
            tb_AppendMenu(menu, pstr("Big;Small"))
            let r = rect(100, 100, 132, 160)
            paintScreen(r, black: true)
            #expect(svcCall("RedrawCachedMenuPixels", menu, r) == 0) // nothing cached
            #expect(svcCall("CacheMenuPixels", menu, r) == 0)
            paintScreen(r, black: false)
            #expect(svcCall("RedrawCachedMenuPixels", menu, r) == 1 && screenPixel(120, 110) == 0)
            #expect(svcCall("RedrawCachedMenuPixels", menu, rect(100, 100, 140, 160)) == 0) // another size
            svcCall("InvalidateCachedMenuPixels", menu)
            #expect(svcCall("RedrawCachedMenuPixels", menu, r) == 0)

            #expect(svcCall("IsMenuBarClockEnabled") == 0)
            #expect(svcStatus(svcCall("DisableMenuBarClock")) == -5620)
            #expect(svcStatus(svcCall("TickleMenuBarClock", 0)) == -5620)
            #expect(svcCall("EnableMenuBarClock") == 0 && svcCall("IsMenuBarClockEnabled") == 1)
            #expect(svcStatus(svcCall("EnableMenuBarClock")) == -5621)
            #expect(svcCall("TickleMenuBarClock", 0) == 0)
            // the time drawn at the bar's right
            var dark = 0
            for h in (Int(g_screen_width) - 80)..<(Int(g_screen_width) - 10) where screenPixel(h, 10) == 0 { dark += 1 }
            #expect(dark > 0)
            let cfg = tb_NewPtr(18, false, true)
            wr8(cfg + 2, 0x42)
            #expect(svcCall("SetMenuBarClockConfig", cfg) == 0)
            wr8(cfg + 2, 0)
            #expect(svcCall("GetMenuBarClockConfig", cfg) == 0 && rd8(cfg + 2) == 0x42)
            wr16(cfg, 1)
            #expect(svcStatus(svcCall("GetMenuBarClockConfig", cfg)) == -50) // version 0 only
            #expect(svcCall("DisableMenuBarClock") == 0 && svcCall("IsMenuBarClockEnabled") == 0)
        }

        /// AppearanceLib: the control panel's fonts and styles, which the
        /// Appearance Manager's getters and GetTheme answer; the one
        /// appearance and its variant; theme sounds with no sound track.
        @Test func appearanceLibSettings() {
            launchTestApp()
            _ = initToolbox()
            let name = tb_NewPtr(256, false, true), size = tb_NewPtr(2, false, true), style = tb_NewPtr(2, false, true)
            #expect(svcCall("GetSystemFont", 0, name) == 0 && pstring(at: name) == "Charcoal")
            #expect(svcCall("GetViewsFont", 0, name, size) == 0 && pstring(at: name) == "Geneva" && rd16(size) == 10)
            #expect(svcCall("SetViewsFont", pstr("Monaco"), 12, 0, 0) == 0)
            #expect(svcCall("GetViewsFont", 0, name, size) == 0 && pstring(at: name) == "Monaco" && rd16(size) == 12)
            #expect(tb_GetThemeFont(3, 0, name, size, style) == 0 && pstring(at: name) == "Monaco" && rd16(size) == 12)
            #expect(svcStatus(svcCall("SetSmallSystemFont", pstr("No Such Font"), 0, 0)) == -30574)
            #expect(svcStatus(svcCall("SetViewsFont", pstr("Monaco"), 8, 0, 0)) == -50)

            #expect(svcCall("SetThemeScrollBarArrowStyle", 1, 0) == 0)
            #expect(tb_GetThemeScrollBarArrowStyle(size) == 0 && rd16(size) == 1)
            #expect(svcStatus(svcCall("SetThemeScrollBarThumbStyle", 2, 0)) == -50)
            #expect(svcCall("SetThemeCheckBoxStyle", 0, 0) == 0)
            #expect(tb_GetThemeCheckBoxStyle(size) == 0 && rd16(size) == 0)

            #expect(svcCall("SetThemeFontSmoothing", 1, 14, 0) == 0)
            let on = tb_NewPtr(2, false, true)
            #expect(svcCall("GetThemeFontSmoothing", on, size) == 0 && rd8(on) == 1 && rd16(size) == 14)
            #expect(svcStatus(svcCall("SetThemeFontSmoothing", 1, 30, 0)) == -50)

            // GetTheme reports them; SetTheme (and SetScenario) takes them
            let coll = tb_NewCollection()
            #expect(svcCall("GetScenario", coll) == 0)
            let item = tb_NewPtr(256, false, true), isz = tb_NewPtr(4, false, true)
            wr32(isz, 2)
            #expect(tb_GetCollectionItem(coll, Int32(bitPattern: fourCC("sbar")), 0, isz, item) == 0 && rd16(item) == 1)
            wr16(item, 2)
            _ = tb_AddCollectionItem(coll, Int32(bitPattern: fourCC("sbar")), 0, 2, item)
            _ = tb_AddCollectionItem(coll, Int32(bitPattern: fourCC("vfnt")), 0, 7, pstr("Geneva"))
            #expect(svcCall("SetScenario", coll) == 0)
            #expect(tb_GetThemeScrollBarArrowStyle(size) == 0 && rd16(size) == 2)
            #expect(svcCall("GetViewsFont", 0, name, size) == 0 && pstring(at: name) == "Geneva")

            let variant = tb_NewPtr(256, false, true)
            #expect(svcCall("GetAppearance", name, variant) == 0)
            #expect(pstring(at: name) == "Apple platinum" && pstring(at: variant) == "Platinum")
            #expect(svcCall("SetAppearance", pstr("Apple platinum"), 0, 0) == 0)
            #expect(svcStatus(svcCall("SetCurrentTheme", pstr("Hi-Tech"), 0, 0)) == -43)
            let list = tb_NewPtr(4, false, true), count = tb_NewPtr(2, false, true)
            #expect(svcCall("GetThemeVariantList", pstr("Apple platinum"), list) == 0 && rd32(list) != 0)
            #expect(svcCall("CountThemeVariants", rd32(list), count) == 0 && rd16(count) == 1)
            #expect(svcCall("GetIndThemeVariant", rd32(list), 1, name) == 0 && pstring(at: name) == "Platinum")
            #expect(svcStatus(svcCall("GetIndThemeVariant", rd32(list), 2, name)) == -50)
            #expect(svcCall("GetThemeVariantByName", rd32(list), pstr("platinum"), count) == 0 && rd16(count) == 1)

            #expect(svcCall("SetThemeSoundsEnabled", 1) == 0 && svcCall("AreThemeSoundsEnabled") == 1)
            #expect(svcCall("SetThemeSoundMask", 0x0F) == 0)
            let mask = tb_NewPtr(4, false, true)
            #expect(svcCall("GetThemeSoundMask", mask) == 0 && rd32(mask) == 0x0F)
            #expect(svcCall("AreThemeSoundsPlaying", 0) == 0)
            #expect(svcStatus(svcCall("GetThemePlaylist", name)) == -30569) // no sound track chosen
            #expect(svcStatus(svcCall("SetThemePlaylist", pstr("Platinum Sounds"))) == -43)
            #expect(svcCall("IsValidThemeFileType", fourCC("thme")) == 1)
            #expect(svcStatus(svcCall("SetThemeCursorWithOptions", 18, 0, 0)) == -30565)

            // the Control Manager's drag handlers, the same as a window's
            let tracking = svcCall("GetControlManagerDragTrackingHandler")
            #expect(tracking != 0 && svcCall("GetControlManagerDragReceiveHandler") != 0)
            #expect(svcCall("GetControlManagerDragTrackingHandler") == tracking)
            #expect(svcCall("InstallControlManagerDragHandlers") == 0)
        }
    }
}
