import CCore
import Foundation
import Testing
import Toolbox

extension Classic {
    @Suite struct ColorQuickDraw {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(v)) }
            return p
        }

        func rgb(_ r: UInt16, _ g: UInt16, _ b: UInt16) -> UInt32 {
            let p = tb_NewPtr(6, false, true)
            wr16(p, r); wr16(p + 2, g); wr16(p + 4, b)
            return p
        }

        @Test func gworldDrawsIntoItsOwnPixels() {
            launchTestApp()
            _ = initToolbox()
            let screen = tb_NewPtr(4, false, true), dev = tb_NewPtr(4, false, true)
            tb_GetGWorld(screen, dev)
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 20, 30), 0, 0, 0) == 0)
            let port = rd32(gw)
            #expect(port != 0)
            tb_SetGWorld(port, 0)
            #expect(tb_LockPixels(tb_GetGWorldPixMap(port)) != 0)
            #expect(tb_GetPixBaseAddr(tb_GetGWorldPixMap(port)) != 0)
            let red = rgb(0xFFFF, 0, 0)
            tb_SetCPixel(5, 5, red)
            let back = rgb(0, 0, 0)
            tb_GetCPixel(5, 5, back)
            #expect(rd16(back) == 0xFFFF && rd16(back + 2) == 0 && rd16(back + 4) == 0)
            tb_GetCPixel(6, 6, back)
            #expect(rd16(back + 2) == 0xFFFF) // still white
            tb_SetGWorld(rd32(screen), rd32(dev))
            tb_DisposeGWorld(port)
        }

        @Test func colorTablesAndIndices() {
            launchTestApp()
            _ = initToolbox()
            let ct = tb_GetCTable(8)
            #expect(ct != 0 && rd16(rd32(ct) + 6) == 255)
            let black = rgb(0, 0, 0)
            let i = tb_Color2Index(black)
            let c = rgb(1, 1, 1)
            tb_Index2Color(i, c)
            #expect(rd16(c) == 0 && rd16(c + 2) == 0 && rd16(c + 4) == 0)
            tb_InvertColor(c)
            #expect(rd16(c) == 0xFFFF)
            tb_DisposeCTable(ct)
        }

        @Test func palettesKeepTheirColours() {
            launchTestApp()
            let w = initToolbox()
            let pal = tb_NewPalette(4, 0, 2, 0)
            #expect(pal != 0)
            tb_SetEntryColor(pal, 2, rgb(0x1234, 0x5678, 0x9ABC))
            let c = rgb(0, 0, 0)
            tb_GetEntryColor(pal, 2, c)
            #expect(rd16(c) == 0x1234 && rd16(c + 4) == 0x9ABC)
            tb_SetPalette(w, pal, 1)
            #expect(tb_GetPalette(w) == pal)
            tb_ResizePalette(pal, 8)
            tb_GetEntryColor(pal, 2, c)
            #expect(rd16(c + 2) == 0x5678)
            tb_DisposePalette(pal)
            #expect(tb_GetPalette(w) == 0)
        }

        @Test func auxiliaryControlRecord() {
            launchTestApp()
            _ = initToolbox()
            let h = tb_NewPtr(4, false, true)
            #expect(tb_GetAuxiliaryControlRecord(0x1000, h) != 0)
            #expect(rd32(h) != 0 && rd32(rd32(rd32(h)) + 4) == 0x1000)
        }
    }
}
