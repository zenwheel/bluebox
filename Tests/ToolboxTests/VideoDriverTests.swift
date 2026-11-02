import CCore
import Testing
import Toolbox

extension BlueBox {
    /// The screen's video driver: its Control and Status calls (Designing
    /// Cards and Drivers, Video.h) on the main device's gdRefNum.
    @Suite struct VideoDriver {
        var refNum: Int16 { Int16(bitPattern: rd16(rd32(rd32(UInt32(LM_MainDevice))) + UInt32(GDevice_gdRefNum_off))) }

        /// A fresh machine at a depth.
        func setUp(depth: Int32 = 8) {
            launchTestApp()
            _ = initToolbox()
            #expect(qd_screen_set_depth(depth))
        }

        /// A Control (or Status) call with csParam pointing at rec.
        @discardableResult
        func call(_ code: Int16, _ rec: UInt32, status: Bool = false) -> Int16 {
            let pb = tb_NewPtr(Int32(CntrlParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: refNum))
            wr16(pb + UInt32(CntrlParam_csCode_off), UInt16(bitPattern: code))
            wr32(pb + UInt32(CntrlParam_csParam_off), rec)
            return status ? tb_PBStatus(pb, false) : tb_PBControl(pb, false)
        }

        func pixel(_ x: Int32, _ y: Int32) -> [UInt8] {
            var px = [UInt8](repeating: 0, count: 4)
            machine_screen_rgba(x, y, 1, 1, &px)
            return Array(px.prefix(3))
        }

        /// A gamma table ('gama'): one channel of 256 bytes, each value scaled.
        func gammaTable(scale: Double, channels: Int = 1) -> UInt32 {
            let t = tb_NewPtr(Int32(12 + 256 * channels), false, true)
            wr16(t + 6, UInt16(channels))
            wr16(t + 8, 256)
            wr16(t + 10, 8)
            for c in 0..<channels {
                for i in 0..<256 { wr8(t + 12 + UInt32(c * 256 + i), UInt8(Double(i) * (c == 0 ? scale : 1))) }
            }
            return t
        }

        /// The driver is in the unit table, open, with its slot.
        @Test func theMainDeviceHasTheDriver() {
            setUp()
            #expect(refNum == -51)
            let h = rd32(rd32(UInt32(LM_UTableBase)) + UInt32(-1 - Int(refNum)) * 4)
            #expect(h != 0)
            let dce = rd32(h)
            #expect(rd16(dce + UInt32(DCtlEntry_dCtlFlags_off)) & 0x20 != 0) // dOpened
            #expect(rd8(dce + 41) == 0x80)                                    // dCtlSlotId
            let hdr = rd32(dce + UInt32(DCtlEntry_dCtlDriver_off))
            #expect(pstring(at: hdr + 18) == ".Display_Video_Apple_BlueBox")
        }

        /// The gamma table starts linear, in 'gama' format; a table set dims
        /// what the screen shows, not the pixels; nil sets it linear again.
        @Test func gammaChangesTheColoursShown() {
            setUp(depth: 32)
            let rec = tb_NewPtr(4, false, true)
            #expect(call(8, rec, status: true) == 0) // cscGetGamma
            let t = rd32(rec)
            #expect(rd16(t) == 0 && rd16(t + 4) == 0 && rd16(t + 6) == 1 && rd16(t + 8) == 256 && rd16(t + 10) == 8)
            #expect(rd8(t + 12 + 200) == 200)

            let base = rd32(UInt32(LM_ScrnBase)), rb = qd_screen_row_bytes()
            wr32(base + 10 * rb + 40, 0x00C8_6432) // (10, 10): 200, 100, 50
            #expect(pixel(10, 10) == [200, 100, 50])
            wr32(rec, gammaTable(scale: 0.5))
            #expect(call(4, rec) == 0) // cscSetGamma
            #expect(pixel(10, 10) == [100, 50, 25])
            #expect(rd32(base + 10 * rb + 40) == 0x00C8_6432)

            wr32(rec, gammaTable(scale: 0.5, channels: 3)) // red alone dimmed
            #expect(call(4, rec) == 0)
            #expect(pixel(10, 10) == [100, 100, 50])

            wr32(rec, 0)
            #expect(call(4, rec) == 0)
            #expect(pixel(10, 10) == [200, 100, 50])

            let bad = gammaTable(scale: 1)
            wr16(bad, 1) // gVersion 1
            wr32(rec, bad)
            #expect(call(4, rec) == -50)
        }

        /// A program that stores its gamma record's address through a nil
        /// csParam (into address 0) has the call find it there.
        @Test func aNilParameterIsTheRecordAtZero() {
            setUp(depth: 32)
            let base = rd32(UInt32(LM_ScrnBase))
            wr32(base, 0x00C8_C8C8)
            let saved = rd32(0)
            wr32(0, gammaTable(scale: 0.5))
            #expect(call(4, 0) == 0)
            wr32(0, saved)
            #expect(pixel(0, 0) == [100, 100, 100])
        }

        /// SetEntries changes the colours the screen shows, not the device's
        /// colour table; GetEntries reads them back.
        @Test func entriesAreTheHardwareTable() {
            setUp(depth: 8)
            let base = rd32(UInt32(LM_ScrnBase))
            wr8(base, 5)
            let ct = rd32(PixMap_pmTable(rd32(GDevice_gdPMap(rd32(rd32(UInt32(LM_MainDevice)))))))
            let before = rd16(ct + 8 + 5 * 8 + 2)
            let specs = tb_NewPtr(16, false, true)
            for (i, v) in [0, 0xFFFF, 0x8000, 0].enumerated() { wr16(specs + UInt32(i * 2), UInt16(v)) }
            let rec = tb_NewPtr(8, false, true)
            wr32(rec, specs)
            wr16(rec + 4, 5) // csStart
            wr16(rec + 6, 0) // one entry
            #expect(call(3, rec) == 0) // cscSetEntries
            #expect(pixel(0, 0) == [255, 128, 0])
            #expect(rd16(ct + 8 + 5 * 8 + 2) == before)

            let out = tb_NewPtr(8, false, true)
            wr16(out, 5)
            wr32(rec, out)
            wr16(rec + 4, 0xFFFF) // indexed by the entries' values
            #expect(call(3, rec, status: true) == 0) // cscGetEntries
            #expect(rd16(out + 2) == 0xFFFF && rd16(out + 4) == 0x8080 && rd16(out + 6) == 0)

            wr16(rec + 4, 300) // past the table
            #expect(call(3, rec) == -50)
            #expect(call(8, rec) == -17) // cscDirectSetEntries: an indexed mode
        }

        /// The direct modes' table (DirectSetEntries) maps each component.
        @Test func directEntriesMapComponents() {
            setUp(depth: 32)
            let base = rd32(UInt32(LM_ScrnBase))
            wr32(base, 0x0040_4040)
            let specs = tb_NewPtr(8, false, true)
            for (i, v) in [0x40, 0xFFFF, 0, 0x2000].enumerated() { wr16(specs + UInt32(i * 2), UInt16(v)) }
            let rec = tb_NewPtr(8, false, true)
            wr32(rec, specs)
            wr16(rec + 4, 0xFFFF)
            wr16(rec + 6, 0)
            #expect(call(3, rec) == -17)     // cscSetEntries: a direct mode
            #expect(call(8, rec) == 0)       // cscDirectSetEntries
            #expect(pixel(0, 0) == [255, 0, 32])
            #expect(qd_screen_set_depth(16) && qd_screen_set_depth(32)) // a new mode: linear again
            wr32(base, 0x0040_4040)
            #expect(pixel(0, 0) == [64, 64, 64])
        }

        /// Modes, pages and base addresses; SetMode changes the screen's depth.
        @Test func modesAndPages() {
            setUp(depth: 8)
            let p = tb_NewPtr(32, false, true)
            #expect(call(2, p, status: true) == 0) // cscGetMode
            #expect(rd16(p) == 0x83 && rd16(p + 6) == 0 && rd32(p + 8) == rd32(UInt32(LM_ScrnBase)))
            wr16(p, 0x85)
            #expect(call(4, p, status: true) == 0) // cscGetPageCnt
            #expect(rd16(p + 6) == 1)
            wr16(p + 6, 0)
            #expect(call(5, p, status: true) == 0) // cscGetPageBase
            #expect(rd32(p + 8) == rd32(UInt32(LM_ScrnBase)))
            wr16(p + 6, 1)
            #expect(call(5, p, status: true) == -50)

            gmemset(p, 0, 32)
            #expect(call(10, p, status: true) == 0) // cscGetCurMode
            #expect(rd16(p) == 0x83 && rd32(p + 2) == 0x80)

            wr16(p, 0x85)
            wr16(p + 6, 0)
            #expect(call(2, p) == 0) // cscSetMode
            #expect(qd_screen_depth() == 32)
            #expect(rd16(rd32(rd32(rd32(rd32(UInt32(LM_MainDevice))) + UInt32(GDevice_gdPMap_off))) + UInt32(PixMap_pixelSize_off)) == 32)
            wr16(p, 0x90)
            #expect(call(2, p) == -50)
        }

        /// The Display Manager's questions: the one resolution, its timing,
        /// its parameters at each depth, the connection.
        @Test func resolutionsAndTimings() {
            setUp(depth: 8)
            let p = tb_NewPtr(64, false, true)
            wr32(p, 0xFFFF_FFFE) // kDisplayModeIDFindFirstResolution
            #expect(call(17, p, status: true) == 0)
            #expect(rd32(p + 4) == 0x80 && rd32(p + 8) == 1024 && rd32(p + 12) == 768 && rd16(p + 20) == 0x85)
            wr32(p, 0x80)
            #expect(call(17, p, status: true) == 0)
            #expect(rd32(p + 4) == 0xFFFF_FFFD) // no more

            gmemset(p, 0, 64)
            wr32(p, 0x80)
            #expect(call(13, p, status: true) == 0) // cscGetModeTiming
            #expect(rd32(p + 8) == 0x6465_636C && rd32(p + 16) & 3 == 3)

            let vp = tb_NewPtr(42, false, true)
            gmemset(p, 0, 64)
            wr32(p, 0x80)
            wr16(p + 4, 0x84)
            wr32(p + 6, vp)
            #expect(call(18, p, status: true) == 0) // cscGetVideoParameters
            #expect(rd16(vp + 32) == 16 && rd16(vp + 4) == 2048 && rd32(p + 10) == 1 && rd32(p + 14) == 2)

            gmemset(p, 0, 64)
            #expect(call(12, p, status: true) == 0) // cscGetConnection
            #expect(rd32(p + 4) & 3 == 3)
            #expect(call(27, p, status: true) == -18) // no DDC block
        }

        /// Gray mode maps the colours set to their luminance; GrayPage fills
        /// the screen with gray; a display powered down shows black.
        @Test func grayAndPower() {
            setUp(depth: 8)
            let g = tb_NewPtr(2, false, true)
            wr8(g, 1)
            #expect(call(6, g) == 0) // cscSetGray
            wr8(g, 0)
            #expect(call(6, g, status: true) == 0)
            #expect(rd8(g) == 1)
            let specs = tb_NewPtr(8, false, true)
            for (i, v) in [7, 0xFFFF, 0, 0].enumerated() { wr16(specs + UInt32(i * 2), UInt16(v)) }
            let rec = tb_NewPtr(8, false, true)
            wr32(rec, specs)
            wr16(rec + 4, 0xFFFF)
            #expect(call(3, rec) == 0)
            let base = rd32(UInt32(LM_ScrnBase))
            wr8(base, 7)
            let px = pixel(0, 0)
            #expect(px[0] == px[1] && px[1] == px[2] && px[0] == 76)

            let page = tb_NewPtr(12, false, true)
            #expect(call(5, page) == 0) // cscGrayPage
            #expect(rd8(base) == 0 && rd8(base + 1) == 0xFF && rd8(base + qd_screen_row_bytes()) == 0xFF)

            setUp(depth: 32)
            wr32(base, 0x00FF_FFFF)
            let power = tb_NewPtr(16, false, true)
            wr32(power, 0) // kAVPowerOff
            #expect(call(25, power) == 0)
            #expect(pixel(0, 0) == [0, 0, 0])
            #expect(call(25, power, status: true) == 0 && rd32(power) == 0)
            wr32(power, 3)
            #expect(call(25, power) == 0)
            #expect(pixel(0, 0) == [255, 255, 255])
        }

        /// The Display Manager's gamma is the driver's.
        @Test func displayManagerGamma() {
            setUp(depth: 32)
            let base = rd32(UInt32(LM_ScrnBase))
            wr32(base, 0x00C8_C8C8)
            let h = tb_NewHandle(12 + 256, false, true)
            gmemcpy(rd32(h), gammaTable(scale: 0.5), 12 + 256)
            #expect(tb_DMSetGammaByAVID(1, 0, h) == 0)
            #expect(pixel(0, 0) == [100, 100, 100])
            let got = tb_NewPtr(4, false, true)
            #expect(tb_DMGetGammaByAVID(1, 0, got) == 0)
            #expect(tb_GetHandleSize(rd32(got)) == 12 + 256 && rd8(rd32(rd32(got)) + 12 + 200) == 100)
            #expect(tb_DMSetGammaByAVID(1, 0, 0) == 0)
            #expect(pixel(0, 0) == [200, 200, 200])
        }
    }
}
