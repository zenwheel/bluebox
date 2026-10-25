import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// CarbonLib's Control Manager and Dialog Manager calls, on the classic
    /// Control Manager's records: accessors, ControlIDs, properties, custom
    /// definitions, the Create...Control calls, the UPPs and the Data
    /// Browser.
    @Suite(.serialized) struct CarbonControls {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func rectOf(_ p: UInt32) -> [Int] { (0..<4).map { Int(Int16(bitPattern: rd16(p + UInt32($0 * 2)))) } }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func window() -> UInt32 {
            let w = tb_NewCWindow(0, rect(40, 40, 340, 440), pstr("Test"), 1, 0, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            return w
        }

        func ptr(_ n: Int = 4) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

        /// Call an export with its arguments in r3 on (the ninth on in the
        /// caller's parameter area); r3 after.
        @discardableResult
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            let sp = tb_NewPtr(8192, false, true) + 4096
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                gpr.storeBytes(of: sp, toByteOffset: 4, as: UInt32.self)
                for (i, a) in args.enumerated() where i < 8 { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            for (i, a) in args.enumerated() where i >= 8 { wr32(sp + 24 + UInt32(i * 4), a) }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        func cfString(_ s: String) -> UInt32 {
            let c = tb_NewPtr(Int32(s.utf8.count + 1), false, true)
            for (i, b) in s.utf8.enumerated() { wr8(c + UInt32(i), b) }
            return call("CFStringCreateWithCString", [0, c, 0x0800_0100]) // kCFStringEncodingUTF8
        }

        func text(_ cf: UInt32) -> String {
            let n = call("CFStringGetLength", [cf])
            let buf = tb_NewPtr(Int32(n * 2 + 2), false, true)
            call("CFStringGetCharacters", [cf, 0, n, buf])
            return String(utf16CodeUnits: (0..<Int(n)).map { rd16(buf + UInt32($0 * 2)) }, count: Int(n))
        }

        /// A machine with PowerPC code able to run (callbacks) and a window.
        func setUp() -> UInt32 {
            launch()
            _ = initToolbox()
            return window()
        }

        func launch() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            g_cpu!.pointee.halted = false // the test application never ran
        }

        @Test func accessorsReadTheRecord() {
            let w = setUp()
            let ch = tb_NewControl(w, rect(10, 10, 30, 100), pstr("OK"), 1, 0, 0, 1, 0, 0)
            let b = ptr(8)
            #expect(tb_GetControlBounds(ch, b) == b && rectOf(b) == [10, 10, 30, 100])
            tb_SetControlBounds(ch, rect(20, 30, 40, 130))
            #expect(rectOf(rd32(ch) + 8) == [20, 30, 40, 130])
            #expect(tb_GetControlOwner(ch) == w)
            #expect(tb_IsControlHilited(ch) == 0 && tb_GetControlHilite(ch) == 0)
            tb_HiliteControl(ch, 10)
            #expect(tb_IsControlHilited(ch) != 0 && tb_GetControlHilite(ch) == 10)
            tb_HiliteControl(ch, 255)
            #expect(tb_IsControlHilited(ch) == 0)
            let h = tb_NewHandle(4, false, true)
            tb_SetControlDataHandle(ch, h)
            #expect(tb_GetControlDataHandle(ch) == h)
            tb_SetControlDataHandle(ch, 0)
        }

        @Test func popupMenuAccessors() {
            let w = setUp()
            let menu = tb_NewMenu(200, pstr("Pop"))
            tb_InsertMenu(menu, -1)
            let pop = tb_NewControl(w, rect(10, 10, 30, 200), pstr("Kind:"), 1, 0, 200, 40, 1008, 0)
            #expect(tb_GetControlPopupMenuHandle(pop) == menu && tb_GetControlPopupMenuID(pop) == 200)
            let other = tb_NewMenu(201, pstr("Other"))
            tb_SetControlPopupMenuHandle(pop, other)
            tb_SetControlPopupMenuID(pop, 201)
            #expect(tb_GetControlPopupMenuHandle(pop) == other && tb_GetControlPopupMenuID(pop) == 201)
        }

        @Test func controlIDsFindControls() {
            let w = setUp()
            let a = tb_NewControl(w, rect(10, 10, 30, 100), pstr("A"), 1, 0, 0, 1, 0, 0)
            let b = tb_NewControl(w, rect(40, 10, 60, 100), pstr("B"), 1, 0, 0, 1, 0, 0)
            let id = ptr(8)
            wr32(id, fourCC("Test"))
            wr32(id + 4, 2)
            #expect(tb_SetControlID(b, id) == 0)
            wr32(id + 4, 1)
            #expect(tb_SetControlID(a, id) == 0)
            let out = ptr(), got = ptr(8)
            wr32(id + 4, 2)
            #expect(tb_GetControlByID(w, id, out) == 0 && rd32(out) == b)
            #expect(tb_GetControlID(b, got) == 0 && rd32(got) == fourCC("Test") && rd32(got + 4) == 2)
            wr32(id + 4, 3)
            #expect(tb_GetControlByID(w, id, out) == -30584 && rd32(out) == 0) // errUnknownControl
            #expect(tb_SetControlCommandID(a, fourCC("ok  ")) == 0)
            #expect(tb_GetControlCommandID(a, out) == 0 && rd32(out) == fourCC("ok  "))
            #expect(tb_GetControlCommandID(b, out) == 0 && rd32(out) == 0)
            #expect(tb_GetControlCommandID(w, out) == -30584)
        }

        @Test func propertiesKeepTheirBytes() {
            let w = setUp()
            let ch = tb_NewControl(w, rect(10, 10, 30, 100), pstr("A"), 1, 0, 0, 1, 0, 0)
            let creator = Int32(bitPattern: fourCC("Blue")), tag = Int32(bitPattern: fourCC("data"))
            let data = ptr(6)
            for i in 0..<6 { wr8(data + UInt32(i), UInt8(i + 1)) }
            #expect(tb_SetControlProperty(ch, creator, tag, 6, data) == 0)
            let size = ptr(), buf = ptr(16)
            #expect(tb_GetControlPropertySize(ch, creator, tag, size) == 0 && rd32(size) == 6)
            #expect(tb_GetControlProperty(ch, creator, tag, 4, size, buf) == -30591) // errDataSizeMismatch
            #expect(tb_GetControlProperty(ch, creator, tag, 16, size, buf) == 0)
            #expect((0..<6).map { rd8(buf + UInt32($0)) } == [1, 2, 3, 4, 5, 6])
            #expect(tb_ChangeControlPropertyAttributes(ch, creator, tag, 1, 0) == 0)
            #expect(tb_GetControlPropertyAttributes(ch, creator, tag, size) == 0 && rd32(size) == 1)
            #expect(tb_RemoveControlProperty(ch, creator, tag) == 0)
            #expect(tb_GetControlPropertySize(ch, creator, tag, size) == -5604) // controlPropertyNotFoundErr
            #expect(tb_RemoveControlProperty(ch, creator, tag) == -5604)
        }

        @Test func regionsAndTitles() {
            let w = setUp()
            let ch = tb_NewControl(w, rect(10, 20, 30, 120), pstr("Title"), 1, 0, 0, 1, 0, 0)
            let rgn = tb_NewRgn()
            #expect(tb_GetControlRegion(ch, -1, rgn) == 0)
            #expect(rectOf(rd32(rgn) + 2) == [10, 20, 30, 120])
            #expect(tb_GetControlRegion(ch, 10, rgn) == -30588) // errInvalidPartCode
            let out = ptr()
            #expect(tb_CopyControlTitleAsCFString(ch, out) == 0 && text(rd32(out)) == "Title")
            #expect(tb_SetControlTitleWithCFString(ch, cfString("Caf\u{E9}")) == 0)
            let title = rd32(ch) + UInt32(ControlRecord_contrlTitle_off)
            #expect(rd8(title) == 4 && rd8(title + 4) == 0x8E) // MacRoman e-acute
        }

        /// New...UPP makes a routine descriptor for the PowerPC routine;
        /// Invoke...UPP calls it with the arguments before the UPP.
        @Test func universalProcedurePointers() {
            launch()
            let code = ptr(8), tv = ptr(8)
            wr32(code, 0x7C63_2214) // add r3, r3, r4
            wr32(code + 4, 0x4E80_0020) // blr
            wr32(tv, code)
            let upp = call("NewControlUserPaneHitTestUPP", [tv])
            #expect(rd16(upp) == 0xAAFE && rd32(upp + 12) == 0x3E0 && rd32(upp + 20) == tv)
            #expect(call("InvokeControlUserPaneHitTestUPP", [40, 2, upp]) == 42)
            // The result is a 16-bit one: sign-extended.
            #expect(call("InvokeControlUserPaneHitTestUPP", [0xFFFF, 0, upp]) == 0xFFFF_FFFF)
            call("DisposeControlUserPaneHitTestUPP", [upp])
            for name in ["NewModalFilterUPP", "NewUserItemUPP", "NewControlActionUPP", "NewDataBrowserItemDataUPP",
                         "InvokeModalFilterYDUPP", "DisposeControlKeyFilterUPP", "InvokeDataBrowserTrackingUPP"] {
                #expect(cfm_export_fn(name) != nil, "\(name)")
            }
            // Nine arguments: the ninth from the parameter area.
            let nine = call("NewControlCNTLToCollectionUPP", [tv])
            #expect(call("InvokeControlCNTLToCollectionUPP", [1, 2, 3, 4, 5, 6, 7, 8, 9, nine]) == 3)
        }

        /// A definition procedure of the program's own, by CreateCustomControl
        /// and by NewControl through RegisterControlDefinition: the Control
        /// Manager calls it through its UPP.
        @Test func customDefinitions() {
            let w = setUp()
            // A CDEF: returns the message plus 100 (testCntl answers 101).
            let code = ptr(12), tv = ptr(8)
            wr32(code, 0x3865_0064) // addi r3, r5, 100
            wr32(code + 4, 0x4E80_0020) // blr
            wr32(tv, code)
            let def = call("NewControlDefUPP", [tv])
            let spec = ptr(8)
            wr32(spec + 4, def)
            let coll = tb_NewCollection()
            let v = ptr(4)
            wr32(v, 7)
            _ = tb_AddCollectionItem(coll, Int32(bitPattern: fourCC("valu")), 0, 4, v)
            wr32(v, 9)
            _ = tb_AddCollectionItem(coll, Int32(bitPattern: fourCC("max ")), 0, 4, v)
            let out = ptr()
            #expect(tb_CreateCustomControl(w, rect(10, 10, 50, 50), spec, coll, out) == 0)
            let ch = rd32(out)
            #expect(tb_GetControlValue(ch) == 7 && tb_GetControlMaximum(ch) == 9)
            #expect(tb_TestControl(ch, 0x0014_0014) == 101)
            #expect(tb_RegisterControlDefinition(300, spec, 0) == 0)
            let viaNew = tb_NewControl(w, rect(60, 10, 80, 50), pstr(""), 1, 0, 0, 1, 300 * 16, 0)
            #expect(tb_TestControl(viaNew, 0x0046_0014) == 101)
            #expect(tb_RegisterControlDefinition(300, 0, 0) == 0)
        }

        /// The colour procedure answers SetUpControlBackground and
        /// SetUpControlTextColor for its control.
        @Test func colorProcedure() {
            let w = setUp()
            let ch = tb_NewControl(w, rect(10, 10, 30, 100), pstr("A"), 1, 0, 0, 1, 0, 0)
            // OSStatus proc(control, message, depth, isColor): stores the message, returns 0.
            let slot = ptr()
            let code = ptr(20), tv = ptr(8)
            wr32(code, 0x3D20_0000 | (slot >> 16)) // lis r9, slot@h
            wr32(code + 4, 0x6129_0000 | (slot & 0xFFFF)) // ori r9, r9, slot@l
            wr32(code + 8, 0x9089_0000) // stw r4, 0(r9)
            wr32(code + 12, 0x3860_0000) // li r3, 0
            wr32(code + 16, 0x4E80_0020) // blr
            wr32(tv, code)
            let upp = call("NewControlColorUPP", [tv])
            #expect(tb_SetControlColorProc(ch, upp) == 0)
            #expect(tb_SetUpControlTextColor(ch, 8, 1) == 0 && rd32(slot) == 30)
            #expect(tb_SetUpControlBackground(ch, 8, 1) == 0 && rd32(slot) == 23)
        }

        /// Without a definition that handles them, the Carbon messages
        /// answer "nothing done".
        @Test func carbonMessagesDefaultToNothing() {
            let w = setUp()
            let ch = tb_NewControl(w, rect(10, 10, 30, 100), pstr("A"), 1, 0, 0, 1, 0, 0)
            let b = ptr()
            wr8(b, 1)
            #expect(tb_HandleControlSetCursor(ch, 0x0014_0014, 0, b) == 0 && rd8(b) == 0)
            wr8(b, 1)
            #expect(tb_HandleControlContextualMenuClick(ch, 0x0014_0014, b) == 0 && rd8(b) == 0)
            #expect(tb_GetControlClickActivation(ch, 0x0014_0014, 0, b) == 0 && rd32(b) == 2)
            #expect(tb_SetControlDragTrackingEnabled(ch, 1) == 0)
            #expect(tb_IsControlDragTrackingEnabled(ch, b) == 0 && rd8(b) == 1)
            #expect(tb_SetAutomaticControlDragTrackingEnabledForWindow(w, 1) == 0)
            #expect(tb_IsAutomaticControlDragTrackingEnabledForWindow(w, b) == 0 && rd8(b) == 1)
            #expect(tb_SetAutomaticControlDragTrackingEnabledForWindow(w, 0) == 0)
            #expect(tb_IsAutomaticControlDragTrackingEnabledForWindow(w, b) == 0 && rd8(b) == 0)
            #expect(tb_HandleControlDragReceive(ch, 0) == -30580) // errMessageNotSupported
        }
    }

    /// The Create...Control calls: each standard control made with its
    /// arguments, as NewControl and the data tags make it.
    @Suite(.serialized) struct CarbonCreateControls {
        let t = CarbonControls()

        func data(_ ch: UInt32, _ tag: String, part: Int16 = 0, _ n: Int = 4) -> [UInt8] {
            let buf = t.ptr(n), size = t.ptr()
            #expect(tb_GetControlData(ch, part, Int32(bitPattern: fourCC(tag)), Int32(n), buf, size) == 0, "\(tag)")
            return (0..<min(n, Int(rd32(size)))).map { rd8(buf + UInt32($0)) }
        }

        func cdefID(_ ch: UInt32) -> Int16 {
            // The definition's resource ID.
            let defh = rd32(rd32(ch) + UInt32(ControlRecord_contrlDefProc_off))
            let id = t.ptr(2), type = t.ptr(4), name = t.ptr(256)
            tb_GetResInfo(defh, id, type, name)
            return Int16(bitPattern: rd16(id))
        }

        /// Each test's window (set up once, before what the controls are made from).
        static var window: UInt32 = 0

        func start() { Self.window = t.setUp() }

        func make(_ body: (UInt32, UInt32, UInt32) -> Int32) -> UInt32 {
            if Self.window == 0 { start() }
            let w = Self.window
            let out = t.ptr()
            #expect(body(w, t.rect(10, 10, 60, 200), out) == 0)
            let ch = rd32(out)
            #expect(tb_IsValidControlHandle(ch) != 0 && tb_IsControlVisible(ch) != 0)
            return ch
        }

        @Test func buttons() {
            start()
            let push = make { w, r, out in tb_CreatePushButtonControl(w, r, t.cfString("Go"), out) }
            #expect(cdefID(push) == 23 && tb_GetControlVariant(push) == 0)
            #expect(pstring(at: rd32(push) + UInt32(ControlRecord_contrlTitle_off)) == "Go")
            let check = make { w, r, out in tb_CreateCheckBoxControl(w, r, t.cfString("On"), 1, 1, out) }
            #expect(tb_GetControlVariant(check) == 3 && tb_GetControlValue(check) == 1)
            let radio = make { w, r, out in tb_CreateRadioButtonControl(w, r, t.cfString("A"), 0, 0, out) }
            #expect(tb_GetControlVariant(radio) == 2 && tb_GetControlValue(radio) == 0)
            let group = make { w, r, out in tb_CreateRadioGroupControl(w, r, out) }
            #expect(cdefID(group) == 26)
            // A push button with an icon: the content kept for drawing.
            let info = t.ptr(6)
            wr16(info, 1) // kControlContentIconSuiteRes
            let iconic = make { w, r, out in tb_CreatePushButtonWithIconControl(w, r, t.cfString("I"), info, 7, out) }
            #expect(tb_GetControlVariant(iconic) == 7)
            let out = t.ptr()
            #expect(tb_CreatePushButtonControl(0, t.rect(0, 0, 10, 10), 0, out) == -50 && rd32(out) == 0)
        }

        @Test func scrollBarsAndSliders() {
            start()
            let action = t.ptr(8)
            let bar = make { w, r, out in tb_CreateScrollBarControl(w, r, 5, 0, 100_000, 40, 1, action, out) }
            #expect(cdefID(bar) == 24 && tb_GetControlVariant(bar) == 2)
            #expect(tb_GetControlValue(bar) == 5 && tb_GetControlMaximum(bar) == 32767)
            #expect(tb_GetControlViewSize(bar) == 40 && tb_GetControlAction(bar) == action)
            let slider = make { w, r, out in tb_CreateSliderControl(w, r, 3, 0, 10, 1, 5, 1, 0, out) }
            #expect(cdefID(slider) == 3 && tb_GetControlVariant(slider) == 1 | 2 | 4)
            let plain = make { w, r, out in tb_CreateSliderControl(w, r, 0, 0, 10, 2, 0, 0, 0, out) }
            #expect(tb_GetControlVariant(plain) == 8)
            let progress = make { w, r, out in tb_CreateProgressBarControl(w, r, 2, 0, 9, 1, out) }
            #expect(cdefID(progress) == 5 && data(progress, "inde", 1) == [1])
            #expect(data(progress, "anim", 1) == [1])
            let arrows = make { w, r, out in tb_CreateLittleArrowsControl(w, r, 4, 1, 8, 2, out) }
            #expect(cdefID(arrows) == 6 && tb_GetControlValue(arrows) == 4 && tb_GetControlMinimum(arrows) == 1)
            let chasing = make { w, r, out in tb_CreateChasingArrowsControl(w, r, out) }
            #expect(cdefID(chasing) == 7 && data(chasing, "anim", 1) == [1])
            let still = t.ptr(1)
            #expect(tb_SetControlData(chasing, 0, Int32(bitPattern: fourCC("anim")), 1, still) == 0)
            #expect(data(chasing, "anim", 1) == [0])
        }

        @Test func tabsTriangleGroups() {
            start()
            // Three tabs, the second disabled, named by CFStrings.
            let tabs = t.ptr(30)
            for (i, name) in ["One", "Two", "Three"].enumerated() {
                wr32(tabs + UInt32(i * 10) + 4, t.cfString(name))
                wr8(tabs + UInt32(i * 10) + 8, i == 1 ? 0 : 1)
            }
            let tab = make { w, r, out in tb_CreateTabsControl(w, r, 1, 0, 3, tabs, out) }
            #expect(cdefID(tab) == 8 && tb_GetControlVariant(tab) == 1)
            #expect(tb_GetControlMaximum(tab) == 3 && tb_GetControlValue(tab) == 1)
            let info = data(tab, "tabi", part: 3, 260)
            #expect(String(decoding: info[5..<(5 + Int(info[4]))], as: UTF8.self) == "Three")
            #expect(data(tab, "enab", part: 2, 1) == [0] && data(tab, "enab", part: 1, 1) == [1])
            let content = t.ptr(8)
            #expect(tb_GetTabContentRect(tab, content) == 0 && t.rectOf(content)[0] > 10)
            let tri = make { w, r, out in tb_CreateDisclosureTriangleControl(w, r, 2, t.cfString("More"), 1, 1, 1, out) }
            #expect(cdefID(tri) == 4 && tb_GetControlVariant(tri) == 3 && tb_GetControlValue(tri) == 1)
            let box = make { w, r, out in tb_CreateGroupBoxControl(w, r, t.cfString("Box"), 0, out) }
            #expect(cdefID(box) == 10 && tb_GetControlVariant(box) == 4)
            let checkBox = make { w, r, out in tb_CreateCheckGroupBoxControl(w, r, t.cfString("Use"), 1, 1, 1, out) }
            #expect(tb_GetControlVariant(checkBox) == 1 && tb_GetControlValue(checkBox) == 1)
            // A pop-up group box: its menu, the first item current.
            let menu = tb_NewMenu(300, t.pstr("Kinds"))
            tb_AppendMenu(menu, t.pstr("Alpha;Beta"))
            tb_InsertMenu(menu, -1)
            let pop = make { w, r, out in tb_CreatePopupGroupBoxControl(w, r, t.cfString("K"), 1, 300, 1, 40, 0, 0, out) }
            #expect(tb_GetControlVariant(pop) == 2 && tb_GetControlValue(pop) == 1 && tb_GetControlMaximum(pop) == 2)
            #expect(data(pop, "mhan") == be32(menu))
            #expect(cdefID(make { w, r, out in tb_CreateSeparatorControl(w, r, out) }) == 9)
            #expect(cdefID(make { w, r, out in tb_CreatePlacardControl(w, r, out) }) == 14)
            let header = make { w, r, out in tb_CreateWindowHeaderControl(w, r, 1, out) }
            #expect(cdefID(header) == 21 && tb_GetControlVariant(header) == 1)
            let arrow = make { w, r, out in tb_CreatePopupArrowControl(w, r, 3, 1, out) }
            #expect(cdefID(arrow) == 12 && tb_GetControlVariant(arrow) == 7)
        }

        @Test func imagesClocksPanes() {
            start()
            let pict = tb_NewHandle(10, false, true)
            let info = t.ptr(6)
            wr16(info, 131) // kControlContentPictHandle
            wr32(info + 2, pict)
            let well = make { w, r, out in tb_CreateImageWellControl(w, r, info, out) }
            let back = t.ptr(6)
            #expect(tb_GetImageWellContentInfo(well, back) == 0 && rd16(back) == 131 && rd32(back + 2) == pict)
            let picture = make { w, r, out in tb_CreatePictureControl(w, r, info, 1, out) }
            #expect(cdefID(picture) == 19 && tb_GetControlVariant(picture) == 1 && data(picture, "pich") == be32(pict))
            let icon = make { w, r, out in tb_CreateIconControl(w, r, info, 0, out) }
            #expect(cdefID(icon) == 20)
            let clock = make { w, r, out in tb_CreateClockControl(w, r, 2, 3, out) }
            #expect(cdefID(clock) == 15 && tb_GetControlVariant(clock) == 2 && tb_GetControlValue(clock) == 0)
            #expect(data(clock, "anim", 1) == [1]) // live
            let features = t.ptr()
            let pane = make { w, r, out in tb_CreateUserPaneControl(w, r, 0x16, out) }
            #expect(tb_GetControlFeatures(pane, features) == 0 && rd32(features) & 0x16 == 0x16)
            // A bevel button with a menu at the right.
            let menu = tb_NewMenu(301, t.pstr("B"))
            tb_InsertMenu(menu, -1)
            let bevel = make { w, r, out in tb_CreateBevelButtonControl(w, r, t.cfString("B"), 2, 0x100, 0, 301, 0x4000, 4, out) }
            #expect(cdefID(bevel) == 2 && tb_GetControlVariant(bevel) == 2 | 4)
            #expect(data(bevel, "mhnd") == be32(menu))
        }

        @Test func textAndLists() {
            start()
            let style = t.ptr(24)
            wr16(style, 0x0003) // kControlUseFontMask | kControlUseFaceMask
            wr16(style + 2, 3)
            let edit = make { w, r, out in tb_CreateEditTextControl(w, r, t.cfString("hello"), 0, 0, style, out) }
            #expect(cdefID(edit) == 17 && data(edit, "text", 64) == Array("hello".utf8))
            #expect(data(edit, "font", 24)[3] == 3)
            let pass = make { w, r, out in tb_CreateEditTextControl(w, r, t.cfString("pw"), 1, 0, 0, out) }
            #expect(tb_GetControlVariant(pass) == 2 && data(pass, "pass", 16) == Array("pw".utf8))
            let stat = make { w, r, out in tb_CreateStaticTextControl(w, r, t.cfString("Label"), 0, out) }
            #expect(cdefID(stat) == 18 && data(stat, "text", 64) == Array("Label".utf8))
            // A list box: 4 rows, 2 columns, 16 by 50 cells.
            let list = make { w, r, out in tb_CreateListBoxControl(w, r, 0, 4, 2, 0, 1, 16, 50, 0, 0, out) }
            #expect(cdefID(list) == 22)
            let lh = UInt32(bigEndian: data(list, "lhan").withUnsafeBytes { $0.load(as: UInt32.self) })
            #expect(lh != 0)
            let lr = rd32(lh)
            #expect(t.rectOf(lr + UInt32(ListRec_dataBounds_off)) == [0, 0, 4, 2])
            #expect(rd16(lr + UInt32(ListRec_cellSize_off)) == 16 && rd16(lr + UInt32(ListRec_cellSize_off) + 2) == 50)
        }

        /// A scrolling text box: its 'TEXT' in a TERec; the plain one with
        /// a scroll bar of its own, the auto-scrolling one moving at idle.
        @Test func scrollingTextBox() {
            let text = Array(String(repeating: "A line of credits\r", count: 30).utf8)
            launchTestApp([TestResource(type: "TEXT", id: 500, data: text)])
            _ = initToolbox()
            let w = t.window()
            let out = t.ptr()
            #expect(tb_CreateScrollingTextBoxControl(w, t.rect(10, 10, 110, 210), 500, 0, 0, 0, 0, out) == 0)
            let plain = rd32(out)
            #expect(cdefID(plain) == 27)
            let bar = WindowRecord_controlList(w) // the newest: the box's scroll bar
            #expect(bar != plain && tb_GetControlReference(bar) == Int32(bitPattern: plain))
            #expect(tb_GetControlMaximum(bar) > 0 && t.rectOf(rd32(bar) + 8) == [10, 194, 110, 210])
            #expect(tb_CreateScrollingTextBoxControl(w, t.rect(120, 10, 220, 210), 500, 1, 0, 2, 3, out) == 0)
            let auto = rd32(out)
            #expect(tb_GetControlVariant(auto) == 1 && data(auto, "samt", 2) == [0, 3])
            #expect(data(auto, "anim", 1) == [1])
            // Idle time after the delay: the text moves up by the amount.
            func rows() -> [UInt16] {
                let c = t.ptr(6)
                return (124..<140).flatMap { v in (12..<60).map { h in tb_GetCPixel(Int16(h), Int16(v), c); return rd16(c) } }
            }
            let before = rows()
            let ticks = UInt32(LM_Ticks)
            for _ in 1...3 {
                wr32(ticks, rd32(ticks) + 10)
                tb_IdleControls(w)
            }
            #expect(rows() != before)
            tb_DisposeControl(plain)
            #expect(tb_IsValidControlHandle(bar) == 0)
        }

        @Test func popupButtonTags() {
            let w = t.setUp()
            let menu = tb_NewMenu(302, t.pstr("P"))
            tb_AppendMenu(menu, t.pstr("One;Two;Three"))
            tb_InsertMenu(menu, -1)
            let out = t.ptr()
            #expect(tb_CreatePopupButtonControl(w, t.rect(10, 10, 30, 200), t.cfString("Pick:"), 302, 1, 50, 0, 1, out) == 0)
            let pop = rd32(out)
            #expect(cdefID(pop) == 25 && tb_GetControlVariant(pop) == 2)
            #expect(tb_GetControlPopupMenuHandle(pop) == menu && tb_GetControlMaximum(pop) == 3)
            #expect(data(pop, "mnid", 2) == [0x01, 0x2E])
            #expect(data(pop, "chck", 1) == [1])
            let no = t.ptr(1)
            #expect(tb_SetControlData(pop, 0, Int32(bitPattern: fourCC("chck")), 1, no) == 0)
            #expect(data(pop, "chck", 1) == [0])
            let extra = t.ptr(2)
            wr16(extra, 6)
            #expect(tb_SetControlData(pop, 0, Int32(bitPattern: fourCC("exht")), 2, extra) == 0)
            let best = t.ptr(8), base = t.ptr(2)
            _ = tb_GetBestControlRect(pop, best, base)
            #expect(t.rectOf(best)[2] - t.rectOf(best)[0] == 26)
            // A menu the control owns goes with it.
            let owned = tb_NewMenu(303, t.pstr("O"))
            let mp = t.ptr()
            wr32(mp, owned)
            #expect(tb_SetControlData(pop, 0, Int32(bitPattern: fourCC("omrf")), 4, mp) == 0)
            #expect(tb_GetControlPopupMenuHandle(pop) == owned)
            tb_DisposeControl(pop)
            #expect(tb_GetHandleSize(owned) == 0 || memErr() != 0)
        }
    }

    /// CarbonLib's Dialog Manager calls: the accessors, items inserted and
    /// removed, ParamText read back, timeouts and the event mask.
    @Suite(.serialized) struct CarbonDialogs {
        static func text(_ s: String) -> [UInt8] { Array(s.utf8) }
        static let items = ditl([
            ditlItem(4, (160, 220, 180, 290), text("OK")),       // 1 button
            ditlItem(8, (20, 20, 36, 200), text("Hello")),      // 2 static text
            ditlItem(16, (50, 20, 66, 200), text("abc")),       // 3 edit text
            ditlItem(4, (160, 120, 180, 190), text("Cancel")),  // 4 button
        ])

        func open() -> UInt32 {
            launchTestApp([TestResource(type: "DLOG", id: 210, data: dlog(items: 210)),
                           TestResource(type: "DITL", id: 210, data: Self.items)])
            _ = initToolbox()
            let d = tb_GetNewDialog(210, 0, 0xFFFF_FFFF)
            #expect(d != 0)
            return d
        }

        func item(_ d: UInt32, _ n: Int16) -> (type: Int16, handle: UInt32, rect: [Int16]) {
            let t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
            tb_GetDialogItem(d, n, t, h, r)
            return (Int16(bitPattern: rd16(t)), rd32(h), (0..<4).map { Int16(bitPattern: rd16(r + UInt32($0 * 2))) })
        }

        func itemText(_ h: UInt32) -> String {
            let s = tb_NewPtr(256, false, true)
            tb_GetDialogItemText(h, s)
            return pstring(at: s)
        }

        @Test func accessors() {
            let d = open()
            #expect(tb_GetDialogWindow(d) == d && tb_GetDialogPort(d) == d && tb_GetDialogFromWindow(d) == d)
            #expect(tb_GetDialogTextEditHandle(d) == rd32(d + UInt32(DialogRecord_textH_off)))
            #expect(tb_GetDialogKeyboardFocusItem(d) == 3)
            _ = tb_SetDialogDefaultItem(d, 1)
            _ = tb_SetDialogCancelItem(d, 4)
            #expect(tb_GetDialogDefaultItem(d) == 1 && tb_GetDialogCancelItem(d) == 4)
            tb_SetPort(0)
            tb_SetPortDialogPort(d)
            let port = tb_NewPtr(4, false, true)
            tb_GetPort(port)
            #expect(rd32(port) == d)
        }

        @Test func paramTextReadBack() {
            _ = open()
            let a = tb_NewPtr(256, false, true), b = tb_NewPtr(256, false, true)
            guestPString("first", at: a)
            guestPString("second", at: b)
            tb_ParamText(a, b, 0, 0)
            let o0 = tb_NewPtr(256, false, true), o1 = tb_NewPtr(256, false, true)
            tb_GetParamText(o0, o1, 0, 0)
            #expect(pstring(at: o0) == "first" && pstring(at: o1) == "second")
        }

        @Test func insertAndRemoveItems() {
            let d = open()
            let box = tb_NewPtr(8, false, true)
            for (i, v) in [90, 20, 106, 200].enumerated() { wr16(box + UInt32(i * 2), UInt16(v)) }
            // An edit text item after the static text: the active one moves to 4.
            #expect(tb_InsertDialogItem(d, 2, 16, 0, box) == 0)
            #expect(tb_CountDITL(d) == 5)
            #expect(item(d, 3).type == 16 && item(d, 3).rect == [90, 20, 106, 200] && item(d, 3).handle != 0)
            #expect(itemText(item(d, 4).handle) == "abc")
            #expect(tb_GetDialogKeyboardFocusItem(d) == 4)
            #expect(item(d, 5).type == 4) // Cancel
            // A button: a control with no title.
            #expect(tb_InsertDialogItem(d, 0, 4, 0, box) == 0)
            #expect(item(d, 1).type == 4 && tb_IsValidControlHandle(item(d, 1).handle) != 0)
            #expect(tb_CountDITL(d) == 6)
            // Remove the first two (the new button and OK), disposing them.
            let ok = item(d, 2).handle
            #expect(tb_RemoveDialogItems(d, 1, 2, 1) == 0)
            #expect(tb_CountDITL(d) == 4 && tb_IsValidControlHandle(ok) == 0)
            #expect(itemText(item(d, 1).handle) == "Hello")
            #expect(tb_GetDialogKeyboardFocusItem(d) == 3)
            #expect(tb_RemoveDialogItems(d, 9, 1, 1) == -50)
        }

        @Test func timeoutAndEventMask() {
            let d = open()
            let item = tb_NewPtr(2, false, true), secs = tb_NewPtr(4, false, true), left = tb_NewPtr(4, false, true)
            #expect(tb_GetDialogTimeout(d, item, secs, left) == -5640) // dialogNoTimeoutErr
            #expect(tb_SetDialogTimeout(d, 1, 5) == 0)
            #expect(tb_GetDialogTimeout(d, item, secs, left) == 0)
            #expect(rd16(item) == 1 && rd32(secs) == 5 && rd32(left) == 5)
            let mask = tb_NewPtr(2, false, true)
            #expect(tb_GetModalDialogEventMask(d, mask) == 0 && rd16(mask) == 0xFBFF)
            #expect(tb_SetModalDialogEventMask(d, 0x002A) == 0)
            #expect(tb_GetModalDialogEventMask(d, mask) == 0 && rd16(mask) == 0x002A)
            // ModalDialog presses the button when the time is up.
            #expect(tb_SetDialogTimeout(d, 4, 1) == 0)
            g_cpu!.pointee.halted = false
            tb_ModalDialog(0, item)
            #expect(rd16(item) == 4)
        }
    }
}
