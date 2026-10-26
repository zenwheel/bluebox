import CCore
import CPU68K
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// Routines the walk of the Vintage Apple Macintosh archive found
    /// missing: Window Manager layers, Open Transport's tasks, Carbon's
    /// international string names.
    @Suite struct ArchiveWalk {
        @discardableResult
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// The program's layer holds the window list, under the root; a
        /// layer NewLayer makes is the current one's child, and swapping
        /// and setting the current layer go between them (Audion, RegEdit).
        @Test func windowLayers() {
            bootMachine()
            let app = tb_GetCurLayer()
            #expect(app != 0 && tb_IsLayer(app) != 0)
            let root = tb_GetParent(app)
            #expect(root != 0 && tb_GetParent(root) == 0 && tb_GetSubWindows(root) == app)
            let r = tb_NewPtr(8, false, true)
            wr16(r + 4, 100)
            wr16(r + 6, 100)
            let w = tb_NewWindow(0, r, pstr("W"), 1, 0, 0xFFFF_FFFF, 0, 0)
            #expect(tb_GetSubWindows(app) == w && tb_GetParent(w) == app && tb_IsLayer(w) == 0)
            let out = tb_NewPtr(4, false, true)
            #expect(tb_NewLayer(out, 1, 0, 0xFFFF_FFFF, 7) == 0)
            let mine = rd32(out)
            #expect(mine != 0 && tb_GetParent(mine) == app && rd32(mine + UInt32(WindowRecord_refCon_off)) == 7)
            #expect(tb_SwapCurLayer(mine) == app && tb_GetCurLayer() == mine)
            #expect(tb_SetCurLayer(app) == 0 && tb_GetCurLayer() == app)
            #expect(tb_SetCurLayer(w) != 0 && tb_GetCurLayer() == app) // a window isn't a layer
        }

        /// Open Transport's timer and deferred tasks run their procedure,
        /// with its argument, from the Time Manager's pump once due.
        @Test func openTransportTasks() {
            bootMachine()
            _ = cfm_ppc_cpu()
            // li r4,1; stw r4,0(r3); blr: *arg = 1
            let code = tb_NewPtr(12, false, true)
            wr32(code, 0x3880_0001)
            wr32(code + 4, 0x9083_0000)
            wr32(code + 8, 0x4E80_0020)
            let tv = tb_NewPtr(8, false, true)
            wr32(tv, code)
            let flag = tb_NewPtr(4, false, true)
            let timer = call("OTCreateTimerTask", [tv, flag])
            #expect(timer != 0)
            #expect(call("OTScheduleTimerTask", [timer, 1_000_000]) != 0)
            ot_tasks_pump()
            #expect(rd32(flag) == 0) // not yet due
            #expect(call("OTCancelTimerTask", [timer]) != 0 && call("OTCancelTimerTask", [timer]) == 0)
            #expect(call("OTScheduleTimerTask", [timer, 0]) != 0)
            ot_tasks_pump()
            #expect(rd32(flag) == 1)
            call("OTDestroyTimerTask", [timer])
            #expect(call("OTScheduleTimerTask", [timer, 0]) == 0) // gone
            wr32(flag, 0)
            let deferred = call("OTCreateDeferredTask", [tv, flag])
            #expect(call("OTScheduleDeferredTask", [deferred]) != 0)
            #expect(call("OTScheduleDeferredTask", [deferred]) == 0) // already scheduled
            ot_tasks_pump()
            #expect(rd32(flag) == 1)
            #expect(call("OTDestroyDeferredTask", [deferred]) == 0)
        }

        /// An ICMapEntry (unpacked: Str255s) for the mappings database.
        func mapEntry(_ ext: String, _ type: String, _ creator: String, _ name: String, flags: UInt32 = 0) -> UInt32 {
            let e = tb_NewPtr(22 + 5 * 256, false, true)
            wr32(e + 6, fourCC(type))
            wr32(e + 10, fourCC(creator))
            wr32(e + 18, flags)
            guestPString(ext, at: e + 22)
            guestPString(name, at: e + 22 + 4 * 256)
            return e
        }

        /// Internet Config's mappings database calls over a handle (Audion,
        /// BBEdit 6), and ICParseURL.
        @Test func internetConfigMappings() {
            bootMachine()
            let h = tb_NewHandle(0, false, false)
            #expect(call("ICAddMapEntry", [1, h, mapEntry(".txt", "TEXT", "ttxt", "Text")]) == 0)
            #expect(call("ICAddMapEntry", [1, h, mapEntry(".gif", "GIFf", "ogle", "GIF")]) == 0)
            #expect(call("ICAddMapEntry", [1, h, mapEntry(".html", "TEXT", "MOSS", "HTML")]) == 0)
            let n = tb_NewPtr(4, false, true)
            #expect(call("ICCountMapEntries", [1, h, n]) == 0 && rd32(n) == 3)
            let out = tb_NewPtr(22 + 5 * 256, false, true), pos = tb_NewPtr(4, false, true)
            #expect(call("ICGetIndMapEntry", [1, h, 2, pos, out]) == 0 && rd32(out + 6) == fourCC("GIFf"))
            #expect(rd16(out + 2) == 22 && rd8(out + 22) == 4 && rd8(out + 22 + 4 * 256) == 3)
            #expect(Int16(truncatingIfNeeded: call("ICGetIndMapEntry", [1, h, 4, pos, out])) == -666)
            // The name's extension, longest first; type and creator, then type alone.
            #expect(call("ICMapEntriesFilename", [1, h, pstr("index.HTML"), out]) == 0 && rd32(out + 10) == fourCC("MOSS"))
            #expect(call("ICMapEntriesTypeCreator", [1, h, fourCC("TEXT"), fourCC("MOSS"), pstr(""), out]) == 0)
            #expect(rd32(out + 10) == fourCC("MOSS"))
            #expect(call("ICMapEntriesTypeCreator", [1, h, fourCC("TEXT"), fourCC("R*ch"), pstr("a.txt"), out]) == 0)
            #expect(rd32(out + 10) == fourCC("ttxt"))
            #expect(Int16(truncatingIfNeeded: call("ICMapEntriesFilename", [1, h, pstr("a.zip"), out])) == -666)
            // Replace the second, delete the first.
            call("ICGetIndMapEntry", [1, h, 2, pos, out])
            #expect(call("ICSetMapEntry", [1, h, rd32(pos), mapEntry(".jpeg", "JPEG", "ogle", "JPEG")]) == 0)
            #expect(call("ICGetMapEntry", [1, h, rd32(pos), out]) == 0 && rd32(out + 6) == fourCC("JPEG"))
            #expect(call("ICDeleteMapEntry", [1, h, 0]) == 0)
            #expect(call("ICCountMapEntries", [1, h, n]) == 0 && rd32(n) == 2)
            #expect(call("ICGetIndMapEntry", [1, h, 1, pos, out]) == 0 && rd32(out + 6) == fourCC("JPEG"))

            let text = Array("see <URL:http://example.com/x> or me@example.org".utf8)
            let data = tb_NewPtr(Int32(text.count), false, true)
            for (i, b) in text.enumerated() { wr8(data + UInt32(i), b) }
            let a = tb_NewPtr(4, false, true), b = tb_NewPtr(4, false, true), url = tb_NewHandle(0, false, false)
            wr32(a, 12)
            wr32(b, 12)
            #expect(call("ICParseURL", [1, pstr("mailto"), data, UInt32(text.count), a, b, url]) == 0)
            #expect(read(rd32(url), Int(tb_GetHandleSize(url))) == Array("http://example.com/x".utf8))
            wr32(a, UInt32(text.count))
            wr32(b, UInt32(text.count))
            #expect(call("ICParseURL", [1, pstr("mailto"), data, UInt32(text.count), a, b, url]) == 0)
            #expect(read(rd32(url), Int(tb_GetHandleSize(url))) == Array("mailto:me@example.org".utf8))
            #expect(Int16(truncatingIfNeeded: call("ICParseURL", [1, pstr(""), data, UInt32(text.count), a, b, url])) == -673)
        }

        func read(_ p: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { rd8(p + UInt32($0)) } }

        /// The main device's mode follows its depth, numbered as Apple's
        /// video drivers did (Bubblomania 1.0 wants 8 bits: mode 131).
        @Test func screenModeFollowsDepth() {
            bootMachine()
            let gd = rd32(rd32(UInt32(LM_MainDevice)))
            for (depth, mode) in [(8, 0x83), (1, 0x80), (32, 0x85), (16, 0x84)] {
                #expect(qd_screen_set_depth(Int32(depth)))
                #expect(rd32(gd + UInt32(GDevice_gdMode_off)) == UInt32(mode))
            }
        }

        /// TruncText, now a 68k call too (Pentominoes): the text cut to the
        /// width with an ellipsis, its length updated; FindWordBreaks finds
        /// a word's bounds (OpenMe First).
        @Test func scriptManagerTextCalls() {
            bootMachine()
            let text = Array("Pentominoes and more".utf8)
            let p = tb_NewPtr(64, false, true), len = tb_NewPtr(2, false, true)
            for (i, b) in text.enumerated() { wr8(p + UInt32(i), b) }
            wr16(len, UInt16(text.count))
            wr32(UInt32(LM_CurrentA5), tb_NewPtr(64, false, true) + 32) // an A5 world for QuickDraw's globals
            _ = initToolbox()
            tb_TextFont(4) // Monaco
            tb_TextSize(9)
            #expect(tb_TruncText(1000, p, len, 0) == 0 && rd16(len) == UInt16(text.count))
            let cut = tb_TruncText(40, p, len, 0)
            #expect(cut == 1 && rd16(len) < UInt16(text.count), "\(cut) \(rd16(len))")
            let offsets = tb_NewPtr(12, false, true)
            for (i, b) in text.enumerated() { wr8(p + UInt32(i), b) }
            tb_FindWordBreaks(p, Int16(text.count), 2, 1, 0, offsets, 0)
            #expect(rd16(offsets) == 0 && rd16(offsets + 2) == 11)
        }

        /// Run 68k code with a fresh stack until it halts (it ends in _ExitToShell).
        func run68k(_ words: [UInt16]) -> String {
            let cpu = g_cpu!
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let stack = tb_NewPtr(65536, false, true)
            cpu.pointee.a.7 = stack + 65536 - 16
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            _ = cpu_run(cpu, 100_000)
            return String(cString: machine_halt_reason())
        }

        /// The 68k forms the walk's programs used: _LayerDispatch's
        /// GetCurLayer (Virtual), the Control Strip (Kinga), QDExtensions
        /// with only the selector in D0 (Drol), SndDispVersion with a word
        /// (Anthem) and _IconServicesDispatch (X-Launch).
        @Test func dispatchersFor68kPrograms() {
            bootMachine()
            let vars = tb_NewPtr(16, false, true)
            let hi = UInt16(vars >> 16), lo = UInt16(vars & 0xFFFF)
            #expect(run68k([
                0x598F, 0x7003, 0xA829, 0x2E1F,         // SUBQ.L #4,SP; MOVEQ #3,D0; _LayerDispatch; MOVE.L (SP)+,D7
                0x598F, 0x2F07, 0x7006, 0xA829, 0x261F, // GetSubWindows(D7) into D3
                0x1F3C, 0x0001, 0x303C, 0x0101, 0xAAF2, // MOVE.B #1,-(SP); MOVE.W #$0101,D0; SBShowHideControlStrip
                0x554F, 0x7000, 0xAAF2, 0x1C1F,         // SUBQ #2,SP; MOVEQ #0,D0; SBIsControlStripVisible; MOVE.B (SP)+,D6
                0x4879, hi, lo, 0x4879, hi, lo + 4,      // PEA vars; PEA vars+4
                0x7005, 0xAB1D,                         // MOVEQ #5,D0; _QDExtensions (GetGWorld)
                0x598F, 0x3F3C, 0x0004, 0x7000, 0xA800, 0x2A1F, // SndDispVersion(4) into D5
                0x554F, 0x3F3C, 0xFFFF, 0x1F3C, 0x0001, 0x701F, 0xAA75, 0x381F, // SetCustomIconsEnabled(-1, true) into D4
                0xA9F4,
            ]) == "ExitToShell")
            let cpu = g_cpu!
            #expect(cpu.pointee.d.7 == tb_GetCurLayer() && cpu.pointee.d.7 != 0)
            #expect(cpu.pointee.d.3 == rd32(UInt32(LM_WindowList)))
            #expect(cpu.pointee.d.6 & 0xFF == 1)
            #expect(rd32(vars + 4) == rd32(UInt32(LM_MainDevice)))  // the device GetGWorld gave
            #expect(cpu.pointee.d.5 == UInt32(bitPattern: tb_DirectorUnknown3()))
            #expect(cpu.pointee.d.4 & 0xFFFF == 0)
        }

        /// AddDrive from 68k code: drive number in D0's high word, driver
        /// in its low word, the element in A0 (AppDisk's RAM disk).
        @Test func addDriveFrom68k() {
            bootMachine()
            let q = tb_NewPtr(32, false, true)
            #expect(run68k([
                0x203C, 0x0009, 0xFFD0,                 // MOVE.L #$0009FFD0,D0: drive 9, driver -48
                0x207C, UInt16(q >> 16), UInt16(q & 0xFFFF), // MOVEA.L #q,A0
                0xA04E, 0x2E00,                         // _AddDrive; MOVE.L D0,D7
                0xA9F4,
            ]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 == 0)
            #expect(rd16(q + UInt32(DrvQEl_dQDrive_off)) == 9 && Int16(bitPattern: rd16(q + UInt32(DrvQEl_dQRefNum_off))) == -48)
            var found = false
            var e = rd32(UInt32(LM_DrvQHdr) + 2)
            while e != 0 { if e == q { found = true }; e = rd32(e) }
            #expect(found)
        }

        /// QuickTime's microsecond clock, as a sound channel gives it for
        /// getClockComponentCmd (MacMAME): its time and rate.
        @Test func microsecondClock() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let clock = qt_clock_open()
            #expect(clock != 0)
            let t = tb_NewPtr(16, false, true), rate = tb_NewPtr(4, false, true)
            #expect(call("ClockGetTime", [clock, t]) == 0)
            #expect(rd32(t + 8) == 1_000_000 && rd32(t + 12) == 0)
            let first = UInt64(rd32(t)) << 32 | UInt64(rd32(t + 4))
            machine_skip_time(5_000_000) // 5 ms
            call("ClockGetTime", [clock, t])
            let second = UInt64(rd32(t)) << 32 | UInt64(rd32(t + 4))
            #expect(second >= first + 5000)
            #expect(call("ClockGetRate", [clock, rate]) == 0 && rd32(rate) == 0x0001_0000)
        }

        /// SetDialogItemText given the control GetDialogItemAsControl made
        /// for a static text item sets the item's text and leaves the
        /// control alone (MacMAME; the control's record was resized to the
        /// text, and the heap broke when it was moved).
        @Test func dialogTextThroughItsControl() {
            func text(_ s: String) -> [UInt8] { Array(s.utf8) }
            launchTestApp([TestResource(type: "DLOG", id: 230, data: dlog(items: 230)),
                           TestResource(type: "DITL", id: 230, data: ditl([
                               ditlItem(4, (160, 220, 180, 290), text("OK")),
                               ditlItem(8, (50, 20, 66, 280), text("Hello")),
                           ])),
                           TestResource(type: "dlgx", id: 230, data: cat(be16(0), be32(0x0F)))])
            _ = initToolbox()
            let d = tb_GetNewDialog(230, 0, 0xFFFF_FFFF)
            let out = tb_NewPtr(4, false, true)
            #expect(tb_GetDialogItemAsControl(d, 2, out) == 0)
            let ch = rd32(out)
            let size = tb_GetHandleSize(ch)
            tb_SetDialogItemText(ch, pstr("Goodbye, world"))
            #expect(tb_GetHandleSize(ch) == size)
            let t = tb_NewPtr(2, false, true), h = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
            tb_GetDialogItem(d, 2, t, h, r)
            let got = tb_NewPtr(256, false, true)
            tb_GetDialogItemText(rd32(h), got)
            #expect(pstring(at: got) == "Goodbye, world")
            tb_GetDialogItemText(ch, got)
            #expect(pstring(at: got) == "Goodbye, world")
        }

        /// The Standard Image Compression component's calls from PowerPC
        /// programs (Play it Cool's SCGetInfo): settings set and read back.
        @Test func standardCompressionFromPowerPC() {
            launchTestApp()
            _ = initToolbox()
            _ = cfm_ppc_cpu()
            let ci = components_open_default(fourCC("scdi"), fourCC("imag"))
            #expect(ci != 0)
            let sp = tb_NewPtr(12, false, true)
            wr32(sp, fourCC("rle "))
            wr16(sp + 8, 16)
            wr32(sp + 10, 0x300) // spatialQuality, unaligned as SCSpatialSettings has it
            #expect(call("SCSetInfo", [ci, fourCC("sptl"), sp]) == 0)
            let back = tb_NewPtr(12, false, true)
            #expect(call("SCGetInfo", [ci, fourCC("sptl"), back]) == 0)
            #expect(rd32(back) == fourCC("rle ") && rd16(back + 8) == 16)
            let flags = tb_NewPtr(4, false, true)
            #expect(call("SCSetCompressFlags", [ci, 4]) == 0)
            #expect(call("SCGetCompressFlags", [ci, flags]) == 0 && rd32(flags) == 4)
            components_close(ci)
        }

        /// SVersion is the Slot Manager's: noErr and its version in
        /// spResult (Peng! takes anything else for a missing Display
        /// Manager and says it needs 16-bit colour).
        @Test func slotManagerVersion() {
            bootMachine()
            let spb = tb_NewPtr(64, false, true)
            wr32(spb, 0xDEAD_BEEF)
            #expect(call("SVersion", [spb]) == 0 && rd32(spb) == 1)
        }

        /// OTCompareAndSwap8 and 16, and Carbon's CompareString and
        /// TimeString (Turtle Turmoil, The Cow Catching Game, Amorphium).
        @Test func smallCalls() {
            bootMachine()
            let b = tb_NewPtr(4, false, true)
            wr8(b, 5)
            #expect(call("OTCompareAndSwap8", [4, 9, b]) == 0 && rd8(b) == 5)
            #expect(call("OTCompareAndSwap8", [5, 9, b]) == 1 && rd8(b) == 9)
            wr16(b, 0x1234)
            #expect(call("OTCompareAndSwap16", [0x1234, 0x4321, b]) == 1 && rd16(b) == 0x4321)
            #expect(Int16(truncatingIfNeeded: call("CompareString", [pstr("apple"), pstr("banana"), 0])) == -1)
            #expect(Int16(truncatingIfNeeded: call("CompareString", [pstr("pear"), pstr("pear"), 0])) == 0)
            let out = tb_NewPtr(256, false, true)
            call("TimeString", [0, 0, out, 0])
            #expect(rd8(out) > 0)
        }
    }
}
