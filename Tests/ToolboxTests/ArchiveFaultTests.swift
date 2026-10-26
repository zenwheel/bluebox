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

        /// AEManagerInfo(keyWord, &result) takes four words of arguments,
        /// as its selector says: ChordBook Lite returned to the keyword
        /// left on the stack.
        @Test func aeManagerInfoTakesAKeyword() {
            bootMachine()
            let out = tb_NewPtr(8, false, true)
            let vers = fourCC("vers"), recr = fourCC("recr")
            let reason = run68k([
                0x2A0F,                                   // MOVE.L SP,D5
                0x4267,                                   // CLR.W -(SP)
                0x2F3C, hiWord(vers), loWord(vers),       // MOVE.L #'vers',-(SP)
                0x4879, hiWord(out), loWord(out),         // PEA out
                0x303C, 0x0441,                           // MOVE.W #$0441,D0
                0xA816,                                   // _Pack8
                0x3E1F,                                   // MOVE.W (SP)+,D7
                0x4267,                                   // CLR.W -(SP)
                0x2F3C, hiWord(recr), loWord(recr),       // MOVE.L #'recr',-(SP)
                0x4879, hiWord(out + 4), loWord(out + 4), // PEA out+4
                0x303C, 0x0441,                           // MOVE.W #$0441,D0
                0xA816,                                   // _Pack8
                0x3C1F,                                   // MOVE.W (SP)+,D6
                0x280F,                                   // MOVE.L SP,D4
                0xA9F4,                                   // _ExitToShell
            ])
            #expect(reason == "ExitToShell")
            let cpu = g_cpu!.pointee
            #expect(cpu.d.4 == cpu.d.5)
            #expect(cpu.d.7 & 0xFFFF == 0 && cpu.d.6 & 0xFFFF == 0)
            #expect(rd32(out) == 0x0103_8000 && rd32(out + 4) == 0)
        }

        /// A census run installs dispatchers with nothing behind them, to
        /// see their calls, but GetTrapAddress finds Unimplemented there as
        /// an ordinary run does: Tubular Worlds tests _CodeFragmentDispatch
        /// to decide whether to load its PowerPC code.
        @Test func censusOnlyDispatchersLookUnimplemented() {
            #expect(machine_boot(true))
            let unimplemented = tb_GetTrapAddress(0xA89F, true, true)
            #expect(tb_GetTrapAddress(0xAA5A, false, false) == unimplemented) // CodeFragmentDispatch
            #expect(tb_GetTrapAddress(0xAA59, false, false) == unimplemented) // MixedModeDispatch
            #expect(tb_GetTrapAddress(0xA9A0, false, false) != unimplemented) // GetResource
            bootMachine()
        }

        /// A PEF container with a data section and two exports, alpha and
        /// beta, at its offsets 0 and 4.
        func exportContainer() -> (UInt32, UInt32) {
            let strings = Array("alphabeta".utf8)
            var loader = be32(UInt32(bitPattern: -1)) + be32(0) + be32(UInt32(bitPattern: -1)) + be32(0)
            loader += be32(UInt32(bitPattern: -1)) + be32(0)
            let stringsOff: UInt32 = 56, hashOff: UInt32 = 68
            loader += be32(0) + be32(0) + be32(0) + be32(56) + be32(stringsOff) + be32(hashOff) + be32(0) + be32(2)
            loader += strings
            while loader.count < Int(hashOff) { loader.append(0) }
            loader += be32(2 << 18 | 0)                         // one hash slot: both exports
            loader += be32(5 << 16) + be32(4 << 16)             // the keys: name lengths
            loader += be32(1 << 24 | 0) + be32(0) + be16(0)     // alpha: data at 0 in section 0
            loader += be32(1 << 24 | 5) + be32(4) + be16(0)     // beta: data at 4
            let dataOff = UInt32(40 + 2 * 28), loaderOff = dataOff + 8
            var pef = Array("Joy!peffpwpc".utf8) + be32(1) + be32(0) + be32(0) + be32(0) + be32(0)
            pef += be16(2) + be16(1) + be32(0)
            pef += be32(UInt32(bitPattern: -1)) + be32(0) + be32(8) + be32(8) + be32(8) + be32(dataOff) + [1, 1, 0, 0]
            pef += be32(UInt32(bitPattern: -1)) + be32(0) + be32(0) + be32(0) + be32(UInt32(loader.count))
            pef += be32(loaderOff) + [4, 1, 0, 0]
            pef += [UInt8](repeating: 0, count: 8) + loader
            let p = tb_NewPtr(Int32(pef.count), false, true)
            for (i, b) in pef.enumerated() { wr8(p + UInt32(i), b) }
            return (p, UInt32(pef.count))
        }

        /// GetIndSymbol's index counts from 0 to CountSymbols - 1: the
        /// mTropolis Player asks each plug-in for its symbol 0.
        @Test func getIndSymbolCountsFromZero() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let (pef, len) = exportContainer()
            let conn = tb_NewPtr(4, false, true), main = tb_NewPtr(4, false, true)
            let name = pstr("frag"), err = tb_NewPtr(256, false, true)
            #expect(ppcErr("GetMemFragment", [pef, len, name, 1, conn, main, err]) == 0)
            let count = tb_NewPtr(4, false, true)
            #expect(ppcErr("CountSymbols", [rd32(conn), count]) == 0 && rd32(count) == 2)
            let sym = tb_NewPtr(256, false, true), addr = tb_NewPtr(4, false, true), cls = tb_NewPtr(2, false, true)
            #expect(ppcErr("GetIndSymbol", [rd32(conn), 0, sym, addr, cls]) == 0)
            #expect(pstring(at: sym) == "alpha" && rd8(cls) == 1)
            let alpha = rd32(addr)
            #expect(ppcErr("GetIndSymbol", [rd32(conn), 1, sym, addr, cls]) == 0)
            #expect(pstring(at: sym) == "beta" && rd32(addr) == alpha + 4)
            #expect(ppcErr("GetIndSymbol", [rd32(conn), 2, sym, addr, cls]) == -2802) // cfragNoSymbolErr
        }
    }
}
