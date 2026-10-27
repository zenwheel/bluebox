import CCore
import Testing
import Toolbox

extension BlueBox {
    /// The Window Manager's Mac OS 8 and 8.5 calls: windows by class,
    /// floating ones passed over, regions, collapsing and properties.
    @Suite struct WindowManager8 {
        func rect(_ top: Int16, _ left: Int16, _ bottom: Int16, _ right: Int16) -> UInt32 {
            let r = tb_NewPtr(8, false, true)
            wr16(r, UInt16(bitPattern: top)); wr16(r + 2, UInt16(bitPattern: left))
            wr16(r + 4, UInt16(bitPattern: bottom)); wr16(r + 6, UInt16(bitPattern: right))
            return r
        }

        func bbox(_ rgn: UInt32) -> [Int16] {
            let p = rd32(rgn)
            return (0..<4).map { Int16(bitPattern: rd16(p + 2 + UInt32($0) * 2)) }
        }

        func newWindow(_ cls: UInt32, _ attrs: UInt32, _ r: UInt32) -> UInt32 {
            let out = tb_NewPtr(4, false, true)
            #expect(tb_CreateNewWindow(cls, attrs, r, out) == 0)
            let w = rd32(out)
            tb_ShowWindow(w)
            return w
        }

        /// The rounded-corner windows' WDEF 1 is a resource programs can get
        /// and call (DeskMates); NewWindow's procID 16 draws with it.
        @Test func roundedWindowDefinitionIsAResource() {
            launchTestApp()
            _ = initToolbox()
            let wdef1 = tb_GetResource(Int32(bitPattern: fourCC("WDEF")), 1)
            #expect(wdef1 != 0 && rd32(wdef1) != 0)
            #expect(rd16(rd32(wdef1)) == 0x71FF) // our native definition
            let w = tb_NewWindow(0, rect(60, 60, 160, 260), tb_NewPtr(2, false, true), 1, 16, 0xFFFF_FFFF, 0, 0)
            #expect(rd32(WindowRecord_windowDefProc(w)) != 0)
            #expect(rd32(rd32(w + UInt32(WindowRecord_strucRgn_off))) != 0)
        }

        @Test func classesRegionsAndCollapsing() {
            launchTestApp()
            _ = initToolbox()
            #expect(cfm_export_fn("CreateNewWindow") != nil && cfm_export_fn("FrontNonFloatingWindow") != nil)
            // a document window (close box, zoom, grow), then a floating one in front
            let doc = newWindow(6, 0x1F, rect(100, 100, 200, 300))
            let palette = newWindow(5, 0x01, rect(80, 400, 160, 480))
            #expect(tb_FrontWindow() == palette)
            #expect(tb_FrontNonFloatingWindow() == doc)
            #expect(tb_IsValidWindowPtr(doc) != 0 && tb_IsValidWindowPtr(0x1234) == 0)

            let v = tb_NewPtr(4, false, true)
            #expect(tb_GetWindowClass(palette, v) == 0 && rd32(v) == 5)
            #expect(tb_GetWindowClass(doc, v) == 0 && rd32(v) == 6)
            #expect(tb_GetWindowAttributes(doc, v) == 0 && rd32(v) & 0x1F == 0x1F)
            #expect(tb_GetWindowFeatures(doc, v) == 0)
            #expect(rd32(v) & 0x57 == 0x57) // grow, zoom, collapse, regions, title bar

            let rgn = tb_NewRgn()
            #expect(tb_GetWindowRegion(doc, 33, rgn) == 0) // content
            #expect(bbox(rgn) == [100, 100, 200, 300])
            #expect(tb_GetWindowRegion(doc, 0, rgn) == 0) // title bar: above the content
            let bar = bbox(rgn)
            #expect(bar[2] == 100 && bar[0] < 100)
            #expect(tb_GetWindowRegion(doc, 2, rgn) == 0) // close box: inside the title bar, at its left
            let close = bbox(rgn)
            #expect(close[0] >= bar[0] && close[2] <= bar[2] && close[1] < 150 && close[3] > close[1])
            #expect(tb_GetWindowRegion(doc, 6, rgn) == 0) // grow box
            #expect(bbox(rgn) == [185, 285, 200, 300])

            #expect(tb_IsWindowCollapsable(doc) != 0 && tb_IsWindowCollapsed(doc) == 0)
            #expect(tb_CollapseWindow(doc, 1) == 0)
            #expect(tb_IsWindowCollapsed(doc) != 0)
            #expect(tb_GetWindowRegion(doc, 6, rgn) == 0 && bbox(rgn)[2] <= bbox(rgn)[0]) // no grow box now
            #expect(tb_CollapseAllWindows(0) == 0)
            #expect(tb_IsWindowCollapsed(doc) == 0)

            // floating windows hidden and shown together
            #expect(tb_HideFloatingWindows() == 0 && tb_AreFloatingWindowsVisible() == 0)
            #expect(tb_FrontWindow() == doc)
            #expect(tb_ShowFloatingWindows() == 0 && tb_FrontWindow() == palette)
        }

        @Test func propertiesAndContentColor() {
            launchTestApp()
            _ = initToolbox()
            let w = newWindow(6, 0x01, rect(60, 60, 160, 260))
            let data = tb_NewPtr(8, false, true), back = tb_NewPtr(8, false, true), size = tb_NewPtr(4, false, true)
            wr32(data, 0xCAFE_F00D)
            #expect(tb_SetWindowProperty(w, fourCC("Test"), fourCC("prop"), 4, data) == 0)
            #expect(tb_GetWindowPropertySize(w, fourCC("Test"), fourCC("prop"), size) == 0 && rd32(size) == 4)
            #expect(tb_GetWindowProperty(w, fourCC("Test"), fourCC("prop"), 8, size, back) == 0 && rd32(back) == 0xCAFE_F00D)
            #expect(tb_RemoveWindowProperty(w, fourCC("Test"), fourCC("prop")) == 0)
            #expect(tb_GetWindowPropertySize(w, fourCC("Test"), fourCC("prop"), size) == -5604)

            let color = tb_NewPtr(6, false, true)
            #expect(tb_GetWindowContentColor(w, color) == 0 && rd16(color) == 0xFFFF) // white
            wr16(color, 0x1111); wr16(color + 2, 0x2222); wr16(color + 4, 0x3333)
            #expect(tb_SetWindowContentColor(w, color) == 0)
            gmemset(color, 0, 6)
            #expect(tb_GetWindowContentColor(w, color) == 0 && rd16(color + 2) == 0x2222)

            #expect(tb_SetWindowModified(w, 1) == 0 && tb_IsWindowModified(w) != 0)
            #expect(tb_SetWindowModified(w, 0) == 0 && tb_IsWindowModified(w) == 0)
            #expect(tb_GetWindowProxyFSSpec(w, data) == -5602)
            tb_DisposeWindow(w)
        }
    }
}
