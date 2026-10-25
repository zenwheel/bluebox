import CCore
import CPU68K
import CPUPPC
import Testing
import Toolbox

/// CarbonLib's drawing, windows and menus, called as a Carbon program
/// calls them (carbon_qd.c, carbon_wm.c, carbon_menu.c, carbon_ui.c,
/// carbon_upp.c).
extension BlueBox {
    @Suite(.serialized) struct CarbonHIToolbox {
        func rect(_ top: Int16, _ left: Int16, _ bottom: Int16, _ right: Int16) -> UInt32 {
            let r = tb_NewPtr(8, false, true)
            wr16(r, UInt16(bitPattern: top)); wr16(r + 2, UInt16(bitPattern: left))
            wr16(r + 4, UInt16(bitPattern: bottom)); wr16(r + 6, UInt16(bitPattern: right))
            return r
        }

        func rectAt(_ r: UInt32) -> [Int16] { (0..<4).map { Int16(bitPattern: rd16(r + UInt32($0) * 2)) } }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func err(_ v: UInt32) -> Int32 { Int32(bitPattern: v) }

        /// A transition vector for PowerPC code.
        func ppcRoutine(_ words: [UInt32]) -> UInt32 {
            let code = tb_NewPtr(Int32(words.count * 4), false, true), tv = tb_NewPtr(8, false, true)
            for (i, w) in words.enumerated() { wr32(code + UInt32(i * 4), w) }
            wr32(tv, code)
            wr32(tv + 4, 0)
            return tv
        }

        /// The Toolbox set up, with a PowerPC stack for calls back into
        /// PowerPC code.
        func setUp() -> UInt32 {
            launchTestApp()
            let port = initToolbox()
            #expect(cfm_ppc_cpu())
            return port
        }

        /// An export with more than eight arguments: the rest in the
        /// caller's parameter area.
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            let sp = g_ppc.gpr.1
            for (i, a) in args.enumerated() where i >= 8 { wr32(sp + 24 + UInt32(4 * i), a) }
            return ilCall(name, Array(args.prefix(8)))
        }

        func newWindow(_ cls: UInt32, _ attrs: UInt32, _ r: UInt32) -> UInt32 {
            let out = tb_NewPtr(4, false, true)
            #expect(err(ilCall("CreateNewWindow", [cls, attrs, r, out])) == 0)
            return rd32(out)
        }

        @Test func portAccessors() {
            let port = setUp()
            #expect(ilCall("GetQDGlobalsThePort", []) == port)
            let bits = tb_NewPtr(14, false, true)
            #expect(ilCall("GetQDGlobalsScreenBits", [bits]) == bits)
            #expect(rectAt(bits + 6)[2] > 0 && rectAt(bits + 6)[3] > 0)

            let cport = ilCall("CreateNewPort", [])
            #expect(cport != 0 && ilCall("IsValidPort", [cport]) != 0 && ilCall("IsPortColor", [cport]) != 0)
            #expect(ilCall("IsPortColor", [port]) == 0 && ilCall("IsValidPort", [0x1234]) == 0)
            #expect(ilCall("GetQDGlobalsThePort", []) == port) // the current port kept
            let r = tb_NewPtr(8, false, true)
            #expect(ilCall("GetPortBounds", [cport, r]) == r)
            ilCall("SetPortBounds", [cport, rect(0, 0, 50, 80)])
            ilCall("GetPortBounds", [cport, r])
            #expect(rectAt(r) == [0, 0, 50, 80])
            #expect(ilCall("GetPortBitMapForCopyBits", [cport]) == cport + 2)
            let pm = ilCall("GetPortPixMap", [cport])
            #expect(pm == rd32(cport + 2))
            #expect(ilCall("GetPixDepth", [pm]) > 0)
            #expect(ilCall("GetPortPixMap", [port]) != 0) // an old port's, made for it
            ilCall("SetPortTextFont", [cport, 3]); ilCall("SetPortTextSize", [cport, 18]); ilCall("SetPortTextFace", [cport, 1])
            #expect(ilCall("GetPortTextFont", [cport]) == 3 && ilCall("GetPortTextSize", [cport]) == 18)
            #expect(ilCall("GetPortTextFace", [cport]) == 1)
            ilCall("SetPortPenSize", [cport, 0x0002_0003])
            let pt = tb_NewPtr(4, false, true)
            ilCall("GetPortPenSize", [cport, pt])
            #expect(rd32(pt) == 0x0002_0003)
            ilCall("SetPortPenMode", [cport, 10])
            #expect(ilCall("GetPortPenMode", [cport]) == 10)
            let rgb = tb_NewPtr(6, false, true)
            ilCall("GetPortBackColor", [port, rgb])
            #expect(rd16(rgb) == 0xFFFF)
            ilCall("GetPortForeColor", [port, rgb])
            #expect(rd16(rgb) == 0)
            let pp = tb_NewPixPat()
            #expect(ilCall("GetPortBackPixPat", [port, pp]) == pp)

            // swapping ports
            let old = tb_NewPtr(4, false, true)
            #expect(ilCall("QDSwapPort", [cport, old]) != 0 && rd32(old) == port)
            #expect(ilCall("QDSwapPort", [cport, old]) == 0)
            ilCall("SetPort", [port])
            #expect(ilCall("IsPortOffscreen", [port]) == 0)
            ilCall("DisposePort", [cport])
            #expect(ilCall("IsValidPort", [cport]) == 0)

            // nothing is buffered on Mac OS 9
            #expect(ilCall("QDIsPortBuffered", [port]) == 0)
            ilCall("QDFlushPortBuffer", [port, 0])
            ilCall("SetQDError", [UInt32(bitPattern: -50)])
            #expect(tb_QDError() == -50)
            ilCall("SetQDError", [0])
        }

        @Test func regions() {
            _ = setUp()
            let a = tb_NewRgn(), b = tb_NewRgn()
            tb_SetRectRgn(a, 10, 10, 50, 30)
            #expect(ilCall("IsRegionRectangular", [a]) != 0)
            tb_SetRectRgn(b, 40, 20, 80, 60)
            tb_UnionRgn(a, b, a)
            #expect(ilCall("IsRegionRectangular", [a]) == 0)
            let r = tb_NewPtr(8, false, true)
            ilCall("GetRegionBounds", [a, r])
            #expect(rectAt(r) == [10, 10, 60, 80])
            // flattened into a handle and back
            let h = tb_NewHandle(0, false, true), c = tb_NewRgn()
            ilCall("RgnToHandle", [a, h])
            ilCall("HandleToRgn", [h, c])
            #expect(tb_EqualRgn(a, c) != 0)

            // QDRegionToRects: init, a call per rectangle, terminate, counted at refCon
            // (lwz r9,0(r6); addi r9,r9,1; stw r9,0(r6); li r3,0; blr)
            let counter = tb_NewPtr(4, false, true)
            let tv = ppcRoutine([0x8126_0000, 0x3929_0001, 0x9126_0000, 0x3860_0000, 0x4E80_0020])
            let upp = ilCall("NewRegionToRectsUPP", [tv])
            #expect(rd16(upp) == 0xAAFE)
            #expect(ilCall("QDRegionToRects", [a, 5, upp, counter]) == 0)
            #expect(rd32(counter) == 2 + 3) // three bands of one rectangle each
            ilCall("DisposeRegionToRectsUPP", [upp])
        }

        @Test func universalProcedures() {
            _ = setUp()
            // ListSearch: (aPtr, bPtr, aLen, bLen) -> short; add r3,r3,r4; blr
            let tv = ppcRoutine([0x7C63_2214, 0x4E80_0020])
            let upp = ilCall("NewListSearchUPP", [tv])
            #expect(rd16(upp) == 0xAAFE && rd32(upp + 12) == 0x0000_2BE0)
            #expect(ilCall("InvokeListSearchUPP", [2, 3, 0, 0, upp]) == 5)
            ilCall("DisposeListSearchUPP", [upp])
            for name in ["NewQDTextUPP", "NewWindowPaintUPP", "NewMenuDefUPP", "NewDragTrackingHandlerUPP",
                         "NewHMWindowContentUPP", "NewThemeEraseUPP", "NewTEClickLoopUPP", "InvokeNWidthHookUPP",
                         "DisposeStyleRunDirectionUPP"] {
                #expect(cfm_export_fn(name) != nil)
            }
        }

        @Test func windowAccessorsAndBounds() {
            _ = setUp()
            let w = newWindow(6, 0x1F, rect(100, 100, 200, 300))
            #expect(ilCall("GetWindowPort", [w]) == w && ilCall("GetWindowFromPort", [w]) == w)
            #expect(ilCall("IsWindowVisible", [w]) == 0)
            tb_ShowWindow(w)
            #expect(ilCall("IsWindowVisible", [w]) != 0 && ilCall("GetWindowKind", [w]) == 8)
            #expect(ilCall("GetWindowList", []) == w && ilCall("GetNextWindow", [w]) == 0)
            let r = tb_NewPtr(8, false, true)
            #expect(ilCall("GetWindowBounds", [w, 33, r]) == 0)
            #expect(rectAt(r) == [100, 100, 200, 300])
            #expect(ilCall("GetWindowBounds", [w, 32, r]) == 0)
            #expect(rectAt(r)[0] < 100 && rectAt(r)[2] >= 200)
            let widths = tb_NewPtr(8, false, true)
            ilCall("GetWindowStructureWidths", [w, widths])
            #expect(rectAt(widths)[0] > 0)

            ilCall("SetWindowBounds", [w, 33, rect(120, 140, 260, 400)])
            ilCall("GetWindowBounds", [w, 33, r])
            #expect(rectAt(r) == [120, 140, 260, 400])
            ilCall("GetWindowPortBounds", [w, r])
            #expect(rectAt(r) == [0, 0, 140, 260])
            ilCall("MoveWindowStructure", [w, 50, 60])
            ilCall("GetWindowBounds", [w, 32, r])
            #expect(rectAt(r)[0] == 60 && rectAt(r)[1] == 50)

            // updates
            ilCall("BeginUpdate", [w]); ilCall("EndUpdate", [w])
            tb_BeginUpdate(w); tb_EndUpdate(w)
            #expect(ilCall("IsWindowUpdatePending", [w]) == 0)
            #expect(ilCall("InvalWindowRect", [w, rect(0, 0, 10, 10)]) == 0)
            #expect(ilCall("IsWindowUpdatePending", [w]) != 0)
            ilCall("ValidWindowRect", [w, rect(0, 0, 10, 10)])
            #expect(ilCall("IsWindowUpdatePending", [w]) == 0)

            // ideal zooming, and back
            let ideal = tb_NewPtr(4, false, true)
            wr32(ideal, 0x00C8_012C) // 200 high, 300 wide
            ilCall("GetWindowBounds", [w, 33, r])
            let before = rectAt(r)
            #expect(ilCall("ZoomWindowIdeal", [w, 8, ideal]) == 0)
            ilCall("GetWindowBounds", [w, 33, r])
            #expect(rectAt(r)[2] - rectAt(r)[0] == 200 && rectAt(r)[3] - rectAt(r)[1] == 300)
            #expect(ilCall("IsWindowInStandardState", [w, ideal, 0]) != 0)
            #expect(ilCall("ZoomWindowIdeal", [w, 7, ideal]) == 0)
            ilCall("GetWindowBounds", [w, 33, r])
            #expect(rectAt(r) == before)

            // repositioned: centred on the main screen, then kept on it
            #expect(ilCall("RepositionWindow", [w, 0, 1]) == 0)
            ilCall("GetWindowBounds", [w, 32, r])
            let bits = tb_NewPtr(14, false, true)
            ilCall("GetQDGlobalsScreenBits", [bits])
            let screenW = rectAt(bits + 6)[3]
            #expect(abs(Int(rectAt(r)[1]) - Int(screenW - rectAt(r)[3])) <= 1)
            ilCall("MoveWindowStructure", [w, UInt32(bitPattern: -500), 40])
            #expect(ilCall("ConstrainWindowToScreen", [w, 32, 2, 0, 0]) == 0)
            ilCall("GetWindowBounds", [w, 32, r])
            #expect(rectAt(r)[1] >= 0)

            // found by class and part
            ilCall("GetWindowBounds", [w, 33, r])
            let pt = tb_NewPtr(4, false, true), out = tb_NewPtr(8, false, true)
            wr16(pt, UInt16(bitPattern: rectAt(r)[0] + 5)); wr16(pt + 2, UInt16(bitPattern: rectAt(r)[1] + 5))
            #expect(ilCall("FindWindowOfClass", [pt, 6, out, out + 4]) == 0)
            #expect(rd32(out) == w && rd16(out + 4) == 3)
            #expect(err(ilCall("FindWindowOfClass", [pt, 5, out, out + 4])) == -5610)
            #expect(ilCall("GetFrontWindowOfClass", [6, 1]) == w)
        }

        @Test func classesModalityAndRetainCounts() {
            _ = setUp()
            let doc = newWindow(6, 0x1F, rect(100, 100, 300, 400))
            tb_ShowWindow(doc)
            let v = tb_NewPtr(8, false, true)
            // a sheet: app-modal, over its parent
            let sheet = newWindow(11, 0, rect(0, 0, 80, 200))
            #expect(tb_GetWindowClass(sheet, v) == 0 && rd32(v) == 11)
            ilCall("GetWindowModality", [sheet, v, v + 4])
            #expect(rd32(v) == 2)
            #expect(ilCall("ShowSheetWindow", [sheet, doc]) == 0)
            #expect(ilCall("IsWindowVisible", [sheet]) != 0)
            #expect(ilCall("GetSheetWindowParent", [sheet, v]) == 0 && rd32(v) == doc)
            #expect(ilCall("HideSheetWindow", [sheet]) == 0 && ilCall("IsWindowVisible", [sheet]) == 0)
            #expect(err(ilCall("ShowSheetWindow", [doc, sheet])) == -5605) // not a sheet

            ilCall("SetWindowModality", [doc, 3, sheet])
            ilCall("GetWindowModality", [doc, v, v + 4])
            #expect(rd32(v) == 3 && rd32(v + 4) == sheet)

            // attributes: the grow box taken away, then the class changed
            #expect(ilCall("ChangeWindowAttributes", [doc, 1 << 16, 0x10]) == 0)
            tb_GetWindowAttributes(doc, v)
            #expect(rd32(v) & 0x10 == 0 && rd32(v) & (1 << 16) != 0 && rd32(v) & 0x01 != 0)
            #expect(err(ilCall("ChangeWindowAttributes", [sheet, 0x10, 0])) == -5612)
            #expect(ilCall("SetWindowClass", [doc, 13]) == 0)
            #expect(tb_GetWindowClass(doc, v) == 0 && rd32(v) == 13)
            #expect(ilCall("IsValidWindowClass", [11]) != 0 && ilCall("IsValidWindowClass", [99]) == 0)

            // property attributes, on properties there are
            let data = tb_NewPtr(4, false, true)
            #expect(tb_SetWindowProperty(doc, fourCC("Test"), fourCC("prop"), 4, data) == 0)
            #expect(ilCall("ChangeWindowPropertyAttributes", [doc, fourCC("Test"), fourCC("prop"), 1, 0]) == 0)
            #expect(ilCall("GetWindowPropertyAttributes", [doc, fourCC("Test"), fourCC("prop"), v]) == 0 && rd32(v) == 1)
            #expect(err(ilCall("GetWindowPropertyAttributes", [doc, fourCC("Test"), fourCC("none"), v])) == -5604)

            // retained and released: the last release disposes of it
            #expect(ilCall("GetWindowRetainCount", [doc]) == 1)
            ilCall("RetainWindow", [doc])
            #expect(ilCall("GetWindowRetainCount", [doc]) == 2)
            ilCall("ReleaseWindow", [doc])
            #expect(tb_IsValidWindowPtr(doc) != 0)
            ilCall("ReleaseWindow", [doc])
            #expect(tb_IsValidWindowPtr(doc) == 0)
        }

        @Test func collectionsTitlesAndDefinitions() {
            _ = setUp()
            let w = newWindow(6, 0x1F, rect(80, 90, 180, 290))
            tb_SetWTitle(w, pstr("Kept"))
            let coll = tb_NewCollection()
            #expect(ilCall("StoreWindowIntoCollection", [w, coll]) == 0)
            #expect(tb_CountCollectionItems(coll) >= 5)
            let out = tb_NewPtr(4, false, true)
            #expect(ilCall("CreateWindowFromCollection", [coll, out]) == 0)
            let copy = rd32(out), r = tb_NewPtr(8, false, true), title = tb_NewPtr(256, false, true)
            ilCall("GetWindowBounds", [copy, 33, r])
            #expect(rectAt(r) == [80, 90, 180, 290])
            tb_GetWTitle(copy, title)
            #expect(pstring(at: title) == "Kept")
            #expect(err(ilCall("CreateWindowFromResource", [128, out])) == -192)

            // a window of a procID asked for (a plain box)
            let spec = tb_NewPtr(8, true, true)
            wr32(spec, 2); wr16(spec + 4, 2)
            #expect(ilCall("CreateCustomWindow", [spec, 6, 0, rect(50, 50, 100, 100), out]) == 0)
            #expect(tb_GetWVariant(rd32(out)) == 2)
            // the drag highlight, through _A829 as 68k code calls it
            tb_ShowWindow(w)
            let reason = run68k([0x42A7,                                   // CLR.L -(SP)
                                 0x2F3C, hiWord(w), loWord(w),             // MOVE.L #w,-(SP)
                                 0x1F3C, 0x0001,                           // MOVE.B #1,-(SP)
                                 0x7019, 0xA829,                           // MOVEQ #$19,D0; _A829
                                 0x2E1F, 0xA9F4])                          // MOVE.L (SP)+,D7
            #expect(reason.contains("ExitToShell"))
            #expect(g_cpu!.pointee.d.7 == 0)
            #expect(ilCall("HiliteWindowFrameForDrag", [w, 0]) == 0)
        }

        @Test func menus() {
            _ = setUp()
            let out = tb_NewPtr(4, false, true), v = tb_NewPtr(16, false, true)
            #expect(ilCall("CreateNewMenu", [128, 0, out]) == 0)
            let m = rd32(out)
            #expect(ilCall("GetMenuID", [m]) == 128 && ilCall("IsValidMenu", [m]) != 0)
            #expect(ilCall("SetMenuTitle", [m, pstr("File")]) == 0)
            let t = tb_NewPtr(256, false, true)
            ilCall("GetMenuTitle", [m, t])
            #expect(pstring(at: t) == "File")
            // text as it is: no metacharacters
            #expect(ilCall("AppendMenuItemText", [m, pstr("Open/O")]) == 0)
            #expect(ilCall("InsertMenuItemText", [m, pstr("New"), 0]) == 0)
            #expect(ilCall("CountMenuItems", [m]) == 2)
            tb_GetMenuItemText(m, 2, t)
            #expect(pstring(at: t) == "Open/O")
            ilCall("GetMenuItemCommandKey", [m, 2, 0, v])
            #expect(rd16(v) == 0)

            // command IDs
            tb_SetMenuItemCommandID(m, 2, fourCC("open"))
            #expect(ilCall("CountMenuItemsWithCommandID", [m, fourCC("open")]) == 1)
            #expect(ilCall("GetIndMenuItemWithCommandID", [m, fourCC("open"), 1, out, v]) == 0)
            #expect(rd32(out) == m && rd16(v) == 2)
            ilCall("DisableMenuCommand", [m, fourCC("open")])
            #expect(ilCall("IsMenuCommandEnabled", [m, fourCC("open")]) == 0)
            ilCall("GetMenuItemAttributes", [m, 2, v])
            #expect(rd32(v) & 1 == 1)
            ilCall("EnableMenuCommand", [m, fourCC("open")])
            #expect(ilCall("IsMenuCommandEnabled", [m, fourCC("open")]) != 0)
            ilCall("SetMenuCommandMark", [m, fourCC("open"), 0x12])
            ilCall("GetMenuCommandMark", [m, fourCC("open"), v])
            #expect(rd16(v) == 0x12)

            // properties
            let data = tb_NewPtr(4, false, true)
            wr32(data, 0xFEED_F00D)
            #expect(ilCall("SetMenuItemProperty", [m, 2, fourCC("Test"), fourCC("data"), 4, data]) == 0)
            #expect(ilCall("GetMenuItemPropertySize", [m, 2, fourCC("Test"), fourCC("data"), v]) == 0 && rd32(v) == 4)
            wr32(v, 0)
            #expect(ilCall("GetMenuItemProperty", [m, 2, fourCC("Test"), fourCC("data"), 4, v + 4, v]) == 0)
            #expect(rd32(v) == 0xFEED_F00D)
            // an item inserted before it: the property moves with its item
            ilCall("InsertMenuItemText", [m, pstr("First"), 0])
            #expect(ilCall("GetMenuItemProperty", [m, 3, fourCC("Test"), fourCC("data"), 4, v + 4, v]) == 0)
            #expect(err(ilCall("GetMenuItemProperty", [m, 2, fourCC("Test"), fourCC("data"), 4, v + 4, v])) == -5604)
            #expect(ilCall("GetMenuCommandPropertySize", [m, fourCC("open"), fourCC("Test"), fourCC("data"), v]) == 0)

            // indents, virtual keys, attributes
            ilCall("SetMenuItemIndent", [m, 1, 2])
            ilCall("GetMenuItemIndent", [m, 1, v])
            #expect(rd32(v) == 2)
            ilCall("SetMenuItemCommandKey", [m, 1, 1, 0x7A])
            ilCall("GetMenuItemCommandKey", [m, 1, 1, v])
            #expect(rd16(v) == 0x7A)
            ilCall("ChangeMenuItemAttributes", [m, 1, 1, 0])
            #expect(tb_IsMenuItemEnabled(m, 1) == 0)
            ilCall("ChangeMenuAttributes", [m, 1, 0])
            ilCall("GetMenuAttributes", [m, v])
            #expect(rd32(v) & 1 == 1 && tb_GetMenuExcludesMarkColumn(m) != 0)

            // submenus
            ilCall("CreateNewMenu", [200, 0, out])
            let sub = rd32(out)
            #expect(ilCall("SetMenuItemHierarchicalMenu", [m, 1, sub]) == 0)
            #expect(ilCall("GetMenuItemHierarchicalMenu", [m, 1, out]) == 0 && rd32(out) == sub)
            ilCall("GetMenuType", [sub, v])
            #expect(rd16(v) == 2)

            // items copied, menus duplicated
            ilCall("CreateNewMenu", [129, 0, out])
            let other = rd32(out)
            #expect(ilCall("CopyMenuItems", [m, 2, 2, other, 0]) == 0)
            #expect(ilCall("CountMenuItems", [other]) == 2)
            tb_GetMenuItemText(other, 2, t)
            #expect(pstring(at: t) == "Open/O")
            #expect(ilCall("GetMenuItemProperty", [other, 2, fourCC("Test"), fourCC("data"), 4, v + 4, v]) == 0)
            tb_GetMenuItemCommandID(other, 2, v)
            #expect(rd32(v) == fourCC("open"))
            #expect(ilCall("DuplicateMenu", [m, out]) == 0)
            #expect(ilCall("CountMenuItems", [rd32(out)]) == 3 && ilCall("GetMenuID", [rd32(out)]) == 128)

            // SetMenuItemData / CopyMenuItemData
            let rec = tb_NewPtr(80, true, true)
            wr32(rec, 1 | (1 << 10)) // text, command ID
            wr32(rec + 4, pstr("Renamed"))
            wr32(rec + 36, fourCC("renm"))
            #expect(ilCall("SetMenuItemData", [other, 1, 0, rec]) == 0)
            tb_GetMenuItemText(other, 1, t)
            #expect(pstring(at: t) == "Renamed")
            wr32(rec, 1 << 10)
            wr32(rec + 36, 0)
            #expect(ilCall("CopyMenuItemData", [other, fourCC("renm"), 1, rec]) == 0 && rd32(rec + 36) == fourCC("renm"))

            // retained
            ilCall("RetainMenu", [other])
            #expect(ilCall("GetMenuRetainCount", [other]) == 2)
            ilCall("ReleaseMenu", [other])
            #expect(ilCall("GetMenuRetainCount", [other]) == 1)

            // the bar as a root menu; hidden and shown
            tb_InsertMenu(m, 0)
            let root = ilCall("AcquireRootMenu", [])
            #expect(ilCall("CountMenuItems", [root]) == 1)
            #expect(ilCall("GetMenuItemHierarchicalMenu", [root, 1, out]) == 0 && rd32(out) == m)
            #expect(ilCall("SetRootMenu", [root]) == 0 && tb_GetMenuHandle(128) == m)
            ilCall("GetMenuType", [m, v])
            #expect(rd16(v) == 0)
            ilCall("HideMenuBar", [])
            #expect(ilCall("IsMenuBarVisible", []) == 0)
            ilCall("ShowMenuBar", [])
            #expect(ilCall("IsMenuBarVisible", []) != 0)
            #expect(err(ilCall("GetMenuTrackingData", [m, v])) == -5620)
        }

        @Test func fontMenusAndFontManager() {
            _ = setUp()
            let out = tb_NewPtr(4, false, true), v = tb_NewPtr(64, true, true)
            ilCall("CreateNewMenu", [130, 0, out])
            let m = rd32(out)
            #expect(ilCall("CreateStandardFontMenu", [m, 0, 0, 0, v]) == 0)
            let n = ilCall("CountMenuItems", [m])
            #expect(n > 0)
            #expect(ilCall("GetFontFamilyFromMenuSelection", [m, 1, v, v + 2]) == 0)

            // the families, and their fonts
            let it = tb_NewPtr(64, true, true)
            #expect(ilCall("FMCreateFontFamilyIterator", [0, 0, 0, it]) == 0)
            var families: [Int16] = []
            while ilCall("FMGetNextFontFamily", [it, v]) == 0 && families.count < 500 {
                families.append(Int16(bitPattern: rd16(v)))
            }
            #expect(err(ilCall("FMGetNextFontFamily", [it, v])) == -980)
            ilCall("FMDisposeFontFamilyIterator", [it])
            #expect(!families.isEmpty)
            let fam = families[0]
            let name = tb_NewPtr(256, true, true)
            #expect(ilCall("FMGetFontFamilyName", [UInt32(UInt16(bitPattern: fam)), name]) == 0)
            #expect(Int16(truncatingIfNeeded: ilCall("FMGetFontFamilyFromName", [name])) == fam)
            #expect(Int16(truncatingIfNeeded: ilCall("FMGetFontFamilyFromName", [pstr("No Such Font")])) == -1)
            #expect(ilCall("FMCreateFontFamilyInstanceIterator", [UInt32(UInt16(bitPattern: fam)), it]) == 0)
            #expect(ilCall("FMGetNextFontFamilyInstance", [it, v, v + 4, v + 6]) == 0)
            let font = rd32(v)
            #expect(ilCall("FMGetFontFamilyInstanceFromFont", [font, v + 8, v + 10]) == 0)
            #expect(Int16(bitPattern: rd16(v + 8)) == fam)
            #expect(ilCall("FMGetFontFromFontFamilyInstance", [UInt32(UInt16(bitPattern: fam)), 0, v + 12, v + 16]) == 0)
            #expect(ilCall("FMGetFontFormat", [font, v + 20]) == 0)
            #expect(err(ilCall("FMGetFontFormat", [0, v + 20])) == -982)
            #expect(ilCall("FMGetGeneration", []) >= 1)
        }

        @Test func listsTextEditAppearanceAndHelp() {
            _ = setUp()
            let w = newWindow(6, 0x1F, rect(60, 60, 260, 360))
            tb_ShowWindow(w)
            tb_SetPort(w)
            let out = tb_NewPtr(4, false, true), v = tb_NewPtr(16, true, true)
            let spec = tb_NewPtr(8, true, true)
            wr32(spec, 1) // kListDefStandardTextType
            #expect(call("CreateCustomList", [rect(10, 10, 110, 150), rect(0, 0, 4, 1), 0x0010_0000, spec, w, 1, 0, 0, 1, out]) == 0)
            let list = rd32(out)
            #expect(list != 0 && ilCall("GetListPort", [list]) == w)
            ilCall("GetListDataBounds", [list, v])
            #expect(rectAt(v) == [0, 0, 4, 1])
            ilCall("SetListRefCon", [list, 0x1234])
            #expect(ilCall("GetListRefCon", [list]) == 0x1234)
            ilCall("SetListSelectionFlags", [list, 0x80])
            #expect(ilCall("GetListSelectionFlags", [list]) == 0x80)
            #expect(ilCall("GetListVerticalScrollBar", [list]) != 0 && ilCall("GetListHorizontalScrollBar", [list]) == 0)
            tb_LDispose(list)

            // a list definition of the program's (PowerPC): its init message
            // leaves 100 + message in the refCon
            // (lwz r10,0(r9); addi r0,r3,100; stw r0,60(r10); blr)
            let ldef = ilCall("NewListDefUPP", [ppcRoutine([0x8149_0000, 0x3803_0064, 0x900A_003C, 0x4E80_0020])])
            wr32(spec, 0); wr32(spec + 4, ldef)
            #expect(call("CreateCustomList", [rect(10, 10, 110, 150), rect(0, 0, 4, 1), 0x0010_0000, spec, w, 0, 0, 0, 0, out]) == 0)
            #expect(ilCall("GetListRefCon", [rd32(out)]) == 100)

            // TextEdit's hooks and scrap
            ilCall("TESetDoTextHook", [0x1000])
            #expect(ilCall("TEGetDoTextHook", []) == 0x1000)
            ilCall("TESetDoTextHook", [0])
            let scrap = tb_NewHandle(4, false, true)
            ilCall("TESetScrapHandle", [scrap])
            #expect(ilCall("TEGetScrapHandle", []) == scrap)

            // theme text, through a CFString
            let cs = tb_NewPtr(32, true, true)
            for (i, b) in "Hello there".utf8.enumerated() { wr8(cs + UInt32(i), b) }
            let str = ilCall("CFStringCreateWithCString", [0, cs, 0])
            #expect(str != 0)
            wr32(v, 0)
            #expect(ilCall("GetThemeTextDimensions", [str, 0, 1, 0, v, v + 4]) == 0)
            #expect(rd16(v) > 0 && rd16(v + 2) > 20)
            let wide = rd16(v + 2)
            wr32(v, 0x0000_0020) // 32 wide, wrapped
            ilCall("GetThemeTextDimensions", [str, 0, 1, 1, v, v + 4])
            #expect(rd16(v + 2) < wide && rd16(v) > 0)
            let text = tb_NewPtr(64, true, true)
            #expect(ilCall("GetTextAndEncodingFromCFString", [str, text, 64, v, v + 4]) == 0 && rd32(v) == 11)
            #expect(ilCall("DrawThemeTextBox", [str, 0, 1, 1, rect(10, 10, 40, 200), 0, 0]) == 0)
            let coll = tb_NewCollection()
            #expect(ilCall("GetTheme", [coll]) == 0 && tb_CountCollectionItems(coll) >= 7)

            // help tags: kept, though none is shown
            let content = tb_NewPtr(534, true, true)
            wr32(content, 3); wr32(content + 14, 2) // kHMPascalStrContent
            guestPString("Tip", at: content + 18)
            #expect(ilCall("HMSetWindowHelpContent", [w, content]) == 0)
            let back = tb_NewPtr(534, true, true)
            #expect(ilCall("HMGetWindowHelpContent", [w, back]) == 0 && pstring(at: back + 18) == "Tip")
            ilCall("HMSetHelpTagsDisplayed", [0])
            #expect(ilCall("HMAreHelpTagsDisplayed", []) == 0)
            ilCall("HMSetTagDelay", [250])
            #expect(ilCall("HMGetTagDelay", [v]) == 0 && rd32(v) == 250)
            #expect(ilCall("HMGetHelpMenu", [out, v]) == 0 && rd32(out) != 0 && rd16(v) >= 1)
        }

        @Test func everyExportOfTheArea() {
            // the CarbonLib names of Quickdraw.h, QuickdrawText.h, Fonts.h,
            // MacWindows.h, Menus.h, Lists.h, TextEdit.h, Appearance.h,
            // Drag.h and MacHelp.h that Blue Box lacked
            for name in ["StdOpcode", "HiliteWindowFrameForDrag", "CheckMenuItem", "EnableMenuItem", "DisableMenuItem",
                         "SetMenuFlashCount", "StandardGlyphs", "FMActivateFonts", "TransitionWindow",
                         "CreateStandardWindowMenu", "IsMenuKeyEvent", "IsShowContextualMenuEvent",
                         "GetListClickLoop", "TESetFindWordHook", "IterateThemes", "TruncateThemeText",
                         "HMSetMenuHelpFromBalloonRsrc", "InvokeDragDrawingUPP", "InvokeHMMenuItemContentUPP"] {
                #expect(cfm_export_fn(name) != nil, "\(name)")
            }
        }
    }
}
