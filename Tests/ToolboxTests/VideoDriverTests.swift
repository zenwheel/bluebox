import CCore
import Testing
import Toolbox

extension BlueBox {
    /// The screen's video driver: the main device's gdRefNum, answering the
    /// Control and Status calls of Designing Cards and Drivers and Video.h
    /// (TestGDVideo found every one "n/a" before there was one).
    @Suite struct VideoDriver {
        var refNum: Int16 { Int16(bitPattern: rd16(rd32(rd32(UInt32(LM_MainDevice))) + UInt32(GDevice_gdRefNum_off))) }

        /// A Control or Status call whose csParam points at `record`.
        @discardableResult
        func call(control: Bool, _ code: Int16, _ record: UInt32) -> Int16 {
            let pb = tb_NewPtr(80, false, true)
            defer { tb_DisposePtr(pb) }
            wr16(pb + 24, UInt16(bitPattern: refNum))
            wr16(pb + 26, UInt16(bitPattern: code))
            wr32(pb + 28, record)
            return control ? tb_PBControl(pb, false) : tb_PBStatus(pb, false)
        }

        @Test func theMainDeviceHasTheDriver() {
            launchTestApp()
            #expect(refNum == -1 - 50)
            let table = rd32(UInt32(LM_UTableBase))
            let dce = rd32(rd32(table + 50 * 4))
            #expect(Int16(bitPattern: rd16(dce + UInt32(DCtlEntry_dCtlRefNum_off))) == refNum)
            #expect(rd32(dce + 42) == rd32(UInt32(LM_ScrnBase))) // dCtlDevBase
            let header = rd32(dce)
            #expect(pstring(at: header + 18) == ".Display_Video_Apple_BlueBox")
            let page = tb_NewPtr(12, false, true)
            #expect(call(control: false, 2, page) == 0) // cscGetMode
            #expect(Int(rd16(page)) == 0x80 + [1, 2, 4, 8, 16, 32].firstIndex(of: Int(qd_screen_depth()))!)
            #expect(rd32(page + 8) == rd32(UInt32(LM_ScrnBase)))
            wr16(page, 0x83)
            #expect(call(control: false, 4, page) == 0 && rd16(page + 6) == 1) // cscGetPageCnt: one page
        }

        /// cscSetMode switches the depth as SetDepth does: the device follows.
        @Test func setModeChangesTheDepth() {
            launchTestApp()
            _ = initToolbox()
            let page = tb_NewPtr(12, false, true)
            wr16(page, 0x83) // 8 bits
            #expect(call(control: true, 2, page) == 0)
            #expect(qd_screen_depth() == 8)
            let pm = rd32(GDevice_gdPMap(rd32(rd32(UInt32(LM_MainDevice)))))
            #expect(rd16(pm + UInt32(PixMap_pixelSize_off)) == 8)
            #expect(rd32(page + 8) == rd32(UInt32(LM_ScrnBase)))
            wr16(page, 0x8F)
            #expect(call(control: true, 2, page) == -50) // no such mode
            wr16(page, 0x85)
            #expect(call(control: true, 2, page) == 0 && qd_screen_depth() == 32)
        }

        /// cscSetEntries loads the hardware's colours, not the device's
        /// table; cscGetEntries reads them back.
        @Test func setEntriesLoadsTheHardware() {
            launchTestApp()
            _ = initToolbox()
            let page = tb_NewPtr(12, false, true)
            wr16(page, 0x83)
            #expect(call(control: true, 2, page) == 0)
            let ct = rd32(rd32(rd32(GDevice_gdPMap(rd32(rd32(UInt32(LM_MainDevice))))) + UInt32(PixMap_pmTable_off)))
            let before = rd16(ct + 8 + 5 * 8 + 2)
            let specs = tb_NewPtr(8, false, true)
            wr16(specs, 5)
            wr16(specs + 2, 0x1200); wr16(specs + 4, 0x3400); wr16(specs + 6, 0x5600)
            let rec = tb_NewPtr(8, false, true)
            wr32(rec, specs)
            wr16(rec + 4, 5)
            wr16(rec + 6, 0)
            #expect(call(control: true, 3, rec) == 0)
            var rgb: [UInt8] = [0, 0, 0]
            qd_screen_hw_entry(5, &rgb)
            #expect(rgb == [0x12, 0x34, 0x56])
            #expect(rd16(ct + 8 + 5 * 8 + 2) == before) // the device's table is QuickDraw's
            wr32(specs + 2, 0); wr16(specs + 6, 0)
            #expect(call(control: false, 3, rec) == 0)
            #expect(rd16(specs + 2) == 0x1212 && rd16(specs + 4) == 0x3434 && rd16(specs + 6) == 0x5656)
            #expect(call(control: true, 8, rec) == -17) // cscDirectSetEntries: not at 8 bits
        }

        /// A gamma table dims what the display shows (a gamma fade); a nil
        /// one puts it back; cscGetGamma gives the driver's copy.
        @Test func gammaFadesTheDisplay() {
            launchTestApp()
            _ = initToolbox()
            let table = tb_NewPtr(12 + 256, false, true)
            wr16(table + 6, 1); wr16(table + 8, 256); wr16(table + 10, 8)
            for i in 0..<256 { wr8(table + 12 + UInt32(i), UInt8(i / 2)) }
            let rec = tb_NewPtr(4, false, true)
            wr32(rec, table)
            var white: [UInt8] = [0, 0, 0, 0]
            machine_screen_rgba(100, 5, 1, 1, &white)
            #expect(call(control: true, 4, rec) == 0)
            var dimmed: [UInt8] = [0, 0, 0, 0]
            machine_screen_rgba(100, 5, 1, 1, &dimmed)
            #expect(white[0] == 255 && dimmed[0] == 127)
            let out = tb_NewPtr(4, false, true)
            #expect(call(control: false, 8, out) == 0)
            let copy = rd32(out)
            #expect(copy != 0 && copy != table && rd8(copy + 12 + 200) == 100)
            wr32(rec, 0)
            #expect(call(control: true, 4, rec) == 0)
            var back: [UInt8] = [0, 0, 0, 0]
            machine_screen_rgba(100, 5, 1, 1, &back)
            #expect(back == white)
            wr16(table + 6, 2) // two channels: no such table
            wr32(rec, table)
            #expect(call(control: true, 4, rec) == -50)
        }

        /// The Display Manager's calls: the one resolution, its timing and
        /// the parameters of each depth.
        @Test func resolutionsAndParameters() {
            launchTestApp()
            let res = tb_NewPtr(30, false, true)
            wr32(res, 0xFFFF_FFFE) // kDisplayModeIDFindFirstResolution
            #expect(call(control: false, 17, res) == 0)
            let id = rd32(res + 4)
            #expect(Int(rd32(res + 8)) == Int(g_screen_width) && Int(rd32(res + 12)) == Int(g_screen_height))
            #expect(rd16(res + 20) >= 0x83)
            wr32(res, id)
            #expect(call(control: false, 17, res) == 0 && rd32(res + 4) == 0xFFFF_FFFD) // no more
            let vp = tb_NewPtr(42, false, true)
            let params = tb_NewPtr(22, false, true)
            wr32(params, id)
            wr16(params + 4, 0x83)
            wr32(params + 6, vp)
            #expect(call(control: false, 18, params) == 0)
            #expect(rd16(vp + 28) == 8 && Int(rd16(vp + 12)) == Int(g_screen_width)) // vpPixelSize, bounds
            #expect(rd32(params + 14) == 0) // clutType
            let timing = tb_NewPtr(20, false, true)
            wr32(timing, id)
            #expect(call(control: false, 13, timing) == 0 && rd32(timing + 8) == 0x6465_636C) // 'decl'
            #expect(call(control: false, 22, timing) == 0 && rd32(timing) == 0) // no hardware cursor
        }
    }
}
