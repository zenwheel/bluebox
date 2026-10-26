import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// FontManager as Mac OS 9 exports it: the text calls under the Font
    /// Manager's own names, and fonts registered from a file.
    @Suite struct OS9FontManager {
        func os(_ s: String) -> Int32 { Int32(bitPattern: fourCC(s)) }
        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func fn(_ name: String) -> UInt? {
            cfm_export_fn(name).map { unsafeBitCast($0, to: UInt.self) }
        }

        @Test func namesAreTheTextCalls() {
            bootMachine()
            for (alias, target) in [("NQDDrawString", "DrawString"), ("NQDTextFont", "TextFont"),
                                    ("NQDStdText", "StdText"), ("X_DrawJustified", "DrawJustified"),
                                    ("X_PixelToChar", "PixelToChar"), ("__IsOutline", "IsOutline"),
                                    ("__FMSwapFont", "FMSwapFont"), ("FastFixMul", "FixMul")] {
                #expect(fn(alias) != nil && fn(alias) == fn(target), "\(alias)")
            }
        }

        @Test func aliasChangesThePort() {
            launchTestApp()
            _ = initToolbox()
            _ = ilCall("NQDTextFont", [21])
            _ = ilCall("NQDTextSize", [14])
            _ = ilCall("NQDTextFace", [1])
            let port = tb_NewPtr(4, false, true)
            tb_GetPort(port)
            let p = rd32(port)
            #expect(rd16(p + UInt32(GrafPort_txFont_off)) == 21)
            #expect(rd16(p + UInt32(GrafPort_txSize_off)) == 14)
            #expect(rd8(p + UInt32(GrafPort_txFace_off)) == 1)
        }

        @Test func fontFilesJoinTheResourceChain() {
            launchTestApp()
            let spec = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Fonts X"), spec)
            #expect(ilCall("RegisterFontsInFile", [spec]) != 0) // no such file
            tb_FSpCreateResFile(spec, os("movr"), os("FFIL"), 0)
            let rn = tb_FSpOpenResFile(spec, 3)
            let h = tb_NewHandle(4, false, true)
            wr32(rd32(h), 0x1234_5678)
            tb_AddResource(h, os("tstF"), 9000, 0)
            tb_CloseResFile(rn)
            #expect(tb_GetResource(os("tstF"), 9000) == 0)
            #expect(ilCall("RegisterFontsInFile", [spec]) == 0)
            #expect(ilCall("RegisterFontsInFile", [spec]) == 0) // again: already
            #expect(tb_GetResource(os("tstF"), 9000) != 0)
            #expect(ilCall("UnregisterFontsInFile", [spec]) == 0)
            tb_SetResLoad(1)
            #expect(tb_GetResource(os("tstF"), 9000) == 0)
            #expect(Int32(bitPattern: ilCall("UnregisterFontsInFile", [spec])) == -43)
        }
    }
}
