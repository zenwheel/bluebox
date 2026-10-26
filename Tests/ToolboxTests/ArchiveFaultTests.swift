import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// What programs from the Vintage Apple Macintosh archive walk stopped
    /// on: definition procedures entered through A0, AEManagerInfo's
    /// arguments, census-only dispatchers and CFM's symbol indexes.
    @Suite struct ArchiveFaults {
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

        /// A handle to 68k code that saves A0 in out, then returns from a
        /// Pascal call with `args` bytes of arguments.
        func a0Saver(_ out: UInt32, args: UInt16) -> UInt32 {
            let words: [UInt16] = [0x23C8, hiWord(out), loWord(out), // MOVE.L A0,out
                                   0x225F,                           // MOVEA.L (SP)+,A1
                                   0x4FEF, args,                     // LEA args(SP),SP
                                   0x4ED1]                           // JMP (A1)
            let h = tb_NewHandle(Int32(words.count * 2), false, true)
            for (i, w) in words.enumerated() { wr16(rd32(h) + UInt32(i * 2), w) }
            return h
        }

        /// The Control Manager enters a CDEF with A0 at its code, as it
        /// loaded A0 to jump there: GameCheater's installer replaces a
        /// control's definition with a stub that finds the one it replaced
        /// six bytes past A0.
        @Test func cdefIsEnteredWithA0AtItsCode() {
            launchTestApp()
            _ = initToolbox()
            let w = tb_NewWindow(0, rect(40, 40, 240, 340), pstr("Test"), 1, 0, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            let ch = tb_NewControl(w, rect(10, 10, 30, 100), pstr("OK"), 1, 0, 0, 1, 0, 0)
            let out = tb_NewPtr(4, false, true)
            let def = a0Saver(out, args: 12)
            wr32(rd32(ch) + 24, def) // contrlDefProc
            tb_Draw1Control(ch)
            #expect(rd32(out) == rd32(def))
        }

        /// The Window Manager does the same for a WDEF.
        @Test func wdefIsEnteredWithA0AtItsCode() {
            launchTestApp()
            _ = initToolbox()
            let w = tb_NewWindow(0, rect(40, 40, 240, 340), pstr("Test"), 1, 0, 0xFFFF_FFFF, 0, 0)
            let out = tb_NewPtr(4, false, true)
            let def = a0Saver(out, args: 12)
            wr32(w + 0x7E, def) // windowDefProc
            tb_DrawGrowIcon(w)
            #expect(rd32(out) == rd32(def))
        }
    }
}
