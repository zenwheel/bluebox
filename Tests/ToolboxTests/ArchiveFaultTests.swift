import CCore
import CPU68K
import CPUPPC
import ForkStore
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

        /// System 7's pop-up menu CDEF and Notification Manager are in
        /// Gestalt: AgendaMaker stops at launch when either is missing.
        @Test func gestaltHasPopupAndNotification() {
            bootMachine()
            var v: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("pop!")), &v) == 0 && v & 1 == 1)
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("nmgr")), &v) == 0 && v & 1 == 1)
        }

        /// A package with no code of ours stops as an unimplemented trap:
        /// the System's PACK stub jumps through the trap table, which led
        /// back to the package loader for ever.
        @Test func packageWithoutCodeIsUnimplemented() {
            bootMachine()
            let reason = run68k([0x303C, 0x0001, 0xA82C, 0xA9F4]) // MOVE.W #1,D0; _Pack10; _ExitToShell
            #expect(reason.contains("unimplemented trap"))
        }

        /// PPCBrowser (PACK 9) has no dialog to choose a port with: it
        /// answers userCanceledErr and takes its 26 bytes of arguments.
        @Test func ppcBrowserIsCancelled() {
            bootMachine()
            let reason = run68k([
                0x2A0F,                         // MOVE.L SP,D5
                0x4267,                         // CLR.W -(SP)
                0x42A7, 0x42A7, 0x4267,         // prompt, applListLabel, defaultSpecified
                0x42A7, 0x42A7, 0x42A7, 0x42A7, // theLocation, thePortInfo, portFilter, theLocNBPType
                0x303C, 0x0D00,                 // MOVE.W #$0D00,D0
                0xA82B,                         // _Pack9
                0x3E1F,                         // MOVE.W (SP)+,D7
                0x2C0F,                         // MOVE.L SP,D6
                0xA9F4,                         // _ExitToShell
            ])
            #expect(reason == "ExitToShell")
            let cpu = g_cpu!.pointee
            #expect(cpu.d.5 == cpu.d.6 && Int16(truncatingIfNeeded: cpu.d.7) == -128)
        }

        /// A component registered from a resource file keeps its code when
        /// the program closes the file (Internet Config registers its
        /// extension's and closes it), OpenComponentResFile opens the file
        /// again and CloseComponentResFile closes that, and
        /// CallComponentFunction passes no storage (Account Buddy's image
        /// returned into its own parameters).
        @Test func componentOutlivesItsFile() throws {
            launchTestApp()
            let q = QTTune()
            q.install()
            // The component: what 1 calls CallComponentFunction(params, f),
            // f(x) = x + 1; everything else answers 0.
            let code: [UInt16] = [0x206F, 0x0008, 0x3028, 0x0002, 0x0C40, 0x0001, 0x6708, // what = 1?
                                  0x205F, 0x508F, 0x4297, 0x4ED0,                         // else 0
                                  0x42A7, 0x2F08, 0x487A, 0x0010, 0x70FF, 0xA82A,         // CallComponentFunction
                                  0x201F, 0x205F, 0x508F, 0x2E80, 0x4ED0,
                                  0x205F, 0x201F, 0x5280, 0x2E80, 0x4ED0]                 // f
            let thng = cat(Array("testcasebbox".utf8), be32(0), be32(0), Array("PROC".utf8), be16(128),
                           Array("STR ".utf8), be16(128), be32(0), be16(0), be32(0), be16(0))
            var fork = buildFork([
                TestResource(type: "thng", id: 128, data: thng),
                TestResource(type: "PROC", id: 128, data: code.flatMap { be16(Int($0)) }),
                TestResource(type: "STR ", id: 128, data: [6] + Array("Tester".utf8)),
                TestResource(type: "TEST", id: 1, data: [1, 2, 3]),
            ])
            let path = tempDir().appendingPathComponent("Tester").path
            var f = mac_file()
            var empty: [UInt8] = []
            var err = [CChar](repeating: 0, count: 256)
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            #expect(appledouble_write(path, &f, &err, err.count))
            let rn = rm_open_host_file(path, &fork, UInt32(fork.count), true)
            #expect(rn > 0)
            #expect(q.call([0x7014, 0xA82A], [UInt32(rn)]) == 1) // RegisterComponentResourceFile
            tb_CloseResFile(rn)
            for _ in 0..<8 { // reuse what the file's resources held
                let p = tb_NewPtr(64, false, false)
                for i in 0..<16 { wr32(p + UInt32(i * 4), 0x4943_4943) }
            }
            let ci = q.call([0x7021, 0xA82A], [fourCC("test"), fourCC("case")]) // OpenDefaultComponent
            #expect(ci != 0)
            #expect(q.component(ci, 1, [41]) == 42)
            let file = Int16(truncatingIfNeeded: q.call([0x7015, 0xA82A], [ci]) >> 16) // OpenComponentResFile
            #expect(file > 0 && tb_CurResFile() == file)
            #expect(tb_Get1Resource(Int32(bitPattern: fourCC("TEST")), 1) != 0)
            let closed = q.call([0x3F3C, UInt16(bitPattern: file), 0x7018, 0xA82A], []) >> 16 // CloseComponentResFile
            #expect(closed == 0 && tb_CurResFile() != file)
        }

        /// Every document window has zoom states in its dataHandle, a
        /// no-grow one without a zoom box too (iPoker sets them for its game
        /// window); a dialog's window has none.
        @Test func documentWindowsHaveZoomStates() {
            launchTestApp()
            _ = initToolbox()
            for (proc, states) in [(Int16(0), true), (4, true), (8, true), (12, true), (1, false), (2, false)] {
                let w = tb_NewWindow(0, rect(60, 40, 260, 340), pstr("W"), 1, proc, 0xFFFF_FFFF, 0, 0)
                let h = rd32(w + 0x82) // dataHandle
                #expect((h != 0 && tb_GetHandleSize(h) == 16) == states)
                tb_DisposeWindow(w)
            }
        }

        /// A PowerPC program's free stack is r1's room above the bottom of
        /// its stack, not the 68k stack's: Chris' Puzzle fills it, from the
        /// bottom up.
        @Test func powerPCStackSpaceIsR1s() {
            bootMachine()
            _ = cfm_ppc_cpu()
            g_ppc_mode = true
            defer { g_ppc_mode = false }
            withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: cfm_ppc_stack_lo + 0x1230, toByteOffset: 4, as: UInt32.self) }
            #expect(tb_StackSpace() == 0x1230)
            let free = tb_NewPtr(4, false, true)
            #expect(tb_ThreadCurrentStackSpace(1, free) == 0 && rd32(free) == 0x1230) // kCurrentThreadID
        }

        /// SoundGlue starts negative: the Sound Driver glue makes its handle
        /// then, and uses one that is there otherwise (Classic Daleks).
        @Test func soundGlueStartsWithNoHandle() {
            launchTestApp()
            #expect(rd32(UInt32(LM_SoundGlue)) == 0xFFFF_FFFF)
            #expect(pstring(at: UInt32(LM_SysResName)) == "System")
        }

        /// Every dialog has a TERec, with no edit text item too (OfficePool
        /// locks its about box's); an edit text item, appended later, takes
        /// it over with its own text.
        @Test func dialogsAlwaysHaveATERec() {
            let plain = ditl([ditlItem(4, (160, 220, 180, 290), Array("OK".utf8)),
                              ditlItem(8, (50, 20, 66, 280), Array("Hello".utf8))])
            let more = ditl([ditlItem(16, (80, 20, 96, 200), Array("abc".utf8))])
            launchTestApp([TestResource(type: "DLOG", id: 200, data: dlog(items: 200)),
                           TestResource(type: "DITL", id: 200, data: plain),
                           TestResource(type: "DITL", id: 201, data: more)])
            _ = initToolbox()
            let d = tb_GetNewDialog(200, 0, 0xFFFF_FFFF)
            let te = rd32(d + 0xA0) // textH
            #expect(te != 0 && Int16(bitPattern: rd16(d + 0xA4)) == -1) // editField
            tb_HLock(te)
            #expect(memErr() == 0)
            tb_HUnlock(te)
            tb_AppendDITL(d, tb_GetResource(Int32(bitPattern: fourCC("DITL")), 201), 0)
            #expect(rd32(d + 0xA0) == te && rd16(d + 0xA4) == 2)
            let t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
            tb_GetDialogItem(d, 3, t, h, r)
            #expect(rd32(rd32(te) + 0x3E) == rd32(h)) // hText: the item's text
            tb_DisposeDialog(d)
        }

        /// TRAP #0-#15 return to the next instruction: Apple HD SC Setup
        /// makes A/UX's getuid call (TRAP #0) under Mac OS too.
        @Test func trapInstructionsReturn() {
            launchTestApp()
            let reason = run68k([0x7018, 0x4E40, 0x4E4F, 0x7E01, 0xA9F4]) // MOVEQ #24,D0; TRAP #0; TRAP #15; MOVEQ #1,D7
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 == 1 && g_cpu!.pointee.d.0 & 0xFFFF == 0x18)
        }

        /// NewGWorld takes its pixels through a program's NewHandle patch:
        /// the RampChamp demo's patch takes itself out when called, and
        /// was put in twice, round and round, when it never was.
        @Test func newGWorldCallsAPatchedNewHandle() {
            launchTestApp()
            _ = initToolbox()
            let count = tb_NewPtr(4, false, true)
            let old = tb_GetTrapAddress(0xA122, true, false)
            let words: [UInt16] = [0x52B9, hiWord(count), loWord(count), // ADDQ.L #1,count
                                   0x4EF9, hiWord(old), loWord(old)]    // JMP old
            let patch = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(patch + UInt32(i * 2), w) }
            tb_SetTrapAddress(patch, 0xA122, true, false)
            defer { tb_SetTrapAddress(old, 0xA122, true, false) }
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 8, rect(0, 0, 32, 32), 0, 0, 0) == 0)
            #expect(rd32(count) >= 1)
            let pm = rd32(rd32(rd32(gw) + 2)) // portPixMap
            #expect(tb_GetHandleSize(rd32(pm)) > 32 * 32) // the pixels, a handle
            tb_DisposeGWorld(rd32(gw))
        }

        /// HSetFInfo sets a file's Finder information in the folder named:
        /// the GetFInfo before it leaves the file's number where the folder
        /// was (Dr. Max's new preferences file was "not found").
        @Test func hSetFInfoFindsTheFile() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let vref = tb_NewPtr(2, false, true), dir = tb_NewPtr(4, false, true)
            #expect(tb_FindFolder(-1, Int32(bitPattern: fourCC("pref")), 1, vref, dir) == 0)
            let v = UInt32(rd16(vref)) | (rd16(vref) & 0x8000 != 0 ? 0xFFFF_0000 : 0), d = rd32(dir)
            let name = pstr("Max Prefs")
            tb_HCreateResFile(Int16(bitPattern: rd16(vref)), Int32(bitPattern: d), name)
            #expect(tb_ResError() == 0)
            let info = tb_NewPtr(16, false, true)
            #expect(ppcErr("HGetFInfo", [v, d, name, info]) == 0)
            wr32(info, fourCC("pref"))
            wr32(info + 4, fourCC("DrMx"))
            #expect(ppcErr("HSetFInfo", [v, d, name, info]) == 0)
            wr32(info, 0)
            #expect(ppcErr("HGetFInfo", [v, d, name, info]) == 0 && rd32(info) == fourCC("pref"))
        }
    }
}
