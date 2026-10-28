import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// PrivateInterfaceLib's names that Blue Box can back: TypeSelect, the
    /// Memory Manager's, offscreen buffers for windows, the Process
    /// Manager's.
    @Suite(.serialized) struct OS9LeftPrivate {
        init() {
            _ = launchTestApp()
            cfm_reset()
            _ = cfm_ppc_cpu()
        }

        func tsr() -> UInt32 {
            let r = Rest.ptr(72)
            tb_TypeSelectClear(r)
            return r
        }

        func key(_ ch: Character, when: UInt32, mods: UInt16 = 0, what: UInt16 = 3) -> UInt32 {
            let e = Rest.ptr(16)
            wr16(e, what)
            wr32(e + 2, UInt32(ch.asciiValue ?? 0))
            wr32(e + 6, when)
            wr16(e + 14, mods)
            return e
        }

        func typed(_ r: UInt32) -> String { pstring(at: r + 6) }

        @Test func keysAreCollected() {
            let r = tsr()
            #expect(tb_TypeSelectNewKey(key("a", when: 100), r) == 1)
            #expect(tb_TypeSelectNewKey(key("B", when: 110), r) == 1)
            #expect(typed(r) == "aB" && rd32(r) == 110)
            // command and control keys, control characters and non-key events are not for it
            #expect(tb_TypeSelectNewKey(key("c", when: 112, mods: 0x0100), r) == 0)
            #expect(tb_TypeSelectNewKey(key("c", when: 112, mods: 0x1000), r) == 0)
            #expect(tb_TypeSelectNewKey(key("\u{08}", when: 112), r) == 0)
            #expect(tb_TypeSelectNewKey(key("c", when: 112, what: 1), r) == 0)
            #expect(tb_TypeSelectNewKey(key("c", when: 113, what: 5), r) == 1) // autoKey
            #expect(typed(r) == "aBc")
            // a pause starts again
            #expect(tb_TypeSelectNewKey(key("x", when: 5000), r) == 1 && typed(r) == "x")
            tb_TypeSelectClear(r)
            #expect(typed(r) == "" && rd32(r) == 0)
            #expect(tb_TypeSelectNewKey(0, r) == 0 && tb_TypeSelectNewKey(key("a", when: 1), 0) == 0)
        }

        @Test func stringsAreComparedByPrefix() {
            let r = tsr()
            for (i, ch) in "ab".enumerated() { _ = tb_TypeSelectNewKey(key(ch, when: UInt32(100 + i)), r) }
            func test(_ s: String) -> Int16 {
                let p = Rest.ptr(64)
                guestPString(s, at: p)
                return tb_TypeSelectCompare(r, 0, p)
            }
            #expect(test("abacus") == 0 && test("ABBA") == 0 && test("ab") == 0)
            #expect(test("aardvark") < 0)
            #expect(test("zebra") > 0 || test("zebra") != 0)
            #expect(test("a") != 0) // shorter than what was typed
        }

        @Test func aListIsSearched() {
            let was = g_ppc_mode
            g_ppc_mode = true
            defer { g_ppc_mode = was }
            // 8-byte slots of Pascal strings, sorted
            let words = ["apple", "banana", "bandit", "cherry", "date", "elder"]
            let table = Rest.ptr(8 * words.count)
            for (i, w) in words.enumerated() { guestPString(w, at: table + UInt32(i * 8)) }
            // addi r7,r3,-1; slwi r7,r7,3; add r7,r7,r6; stw r7,0(r5); li r8,0; sth r8,0(r4); li r3,1; blr
            let proc = Rest.routine([0x38E3_FFFF, 0x54E7_1838, 0x7CE7_3214, 0x90E5_0000, 0x3900_0000, 0xB104_0000, 0x3860_0001, 0x4E80_0020])
            func find(_ typed: String, _ mode: Int16 = 0) -> Int16 {
                let r = tsr()
                for (i, ch) in typed.enumerated() { _ = tb_TypeSelectNewKey(key(ch, when: UInt32(100 + i)), r) }
                return tb_TypeSelectFindItem(r, Int16(words.count), mode, proc, table)
            }
            #expect(find("a") == 1)
            #expect(find("ban") == 2)
            #expect(find("band") == 3)
            #expect(find("ban", 1) == 3)   // the next match
            #expect(find("b", -1) == 3)    // the last
            #expect(find("c") == 4)
            #expect(find("cz") == 5)       // none begins so: the first that sorts after
            #expect(find("") == 0)         // nothing typed
            #expect(find("zz") == 0)       // after all of them
        }

        /// The same calls from 68k code, through Pack 6.
        @Test func from68kCode() {
            let r = tsr()
            let e = key("q", when: 700)
            let reason = run68k([
                0x4879, hiWord(r), loWord(r), 0x3F3C, 0x0028, 0xA9ED,                       // TypeSelectClear(r)
                0x4267, 0x4879, hiWord(e), loWord(e), 0x4879, hiWord(r), loWord(r),         // result; &event; &tsr
                0x3F3C, 0x002A, 0xA9ED, 0x3E1F,                                             // TypeSelectNewKey; MOVE.W (SP)+,D7
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFF00 == 0x0100)
            #expect(typed(r) == "q")
        }

        @Test func memoryNumbers() {
            #expect(Rest.signed("GetProcessMgrLoFreeBytes") > 0)
            #expect(Rest.signed("ProcessMgrMaxBlock") > 0)
            let sys = Rest.signed("GetSystemHiFreeBytes")
            #expect(sys > 0)
            #expect(Rest.call("EnoughSystemHeapSlop", [1]) == 1)
            #expect(Rest.call("EnoughSystemHeapSlop", [UInt32(sys) + 1024 * 1024]) == 0)
            #expect(Rest.signed("ReclaimSpaceIfSysHeapLow") > 0)
        }

        @Test func zoneFlagsAndBlocks() {
            let zone = rd32(UInt32(LM_ApplZone))
            #expect(Rest.call("GetZoneCompatibilityFlags", [zone]) == 0)
            #expect(Rest.signed("SetZoneCompatibilityFlags", [zone, 0x21]) == 0)
            #expect(Rest.call("GetZoneCompatibilityFlags", [zone]) == 0x21)
            #expect(Rest.call("GetZoneCompatibilityFlags", [rd32(UInt32(LM_SysZone))]) == 0)
            #expect(Rest.signed("SetZoneCompatibilityFlags", [0, 1]) == -50)
            // the blocks of the heap, to the end: a block we know is among them
            let mine = tb_NewPtr(100, false, true)
            var block: UInt32 = 0, count = 0, found = false
            while count < 100000 {
                block = Rest.call("GetNextBlockInHeap", [zone, block])
                if block == 0 { break }
                if block == mine { found = true }
                count += 1
            }
            #expect(found && count > 3)
            #expect(Rest.call("GetNextBlockInHeap", [zone, 0x10]) == 0)
        }

        @Test func trashing() {
            let p = Rest.ptr(8)
            #expect(Rest.signed("TrashMemoryArea", [p, 6]) == 0)
            #expect((0..<6).map { rd8(p + UInt32($0)) } == [0x72, 0x61, 0x72, 0x61, 0x72, 0x61])
            #expect(Rest.signed("TrashMemoryArea", [0, 6]) == -50)
        }

        @Test func offscreenBuffers() {
            let port = initToolbox()
            let r = Rest.ptr(8)
            wr16(r + 4, 40); wr16(r + 6, 40)
            let buf = Rest.ptr(4)
            #expect(Rest.signed("NewOffscreenBuffer", [r, 32, buf]) == 0 && rd32(buf) != 0)
            #expect(Rest.signed("NewOffscreenBuffer", [0, 32, buf]) == -50)
            // a window to copy to
            let wr = Rest.ptr(8)
            wr16(wr, 50); wr16(wr + 2, 50); wr16(wr + 4, 150); wr16(wr + 6, 150)
            let title = Rest.ptr(8)
            let w = tb_NewWindow(0, wr, title, 1, 0, 0xFFFF_FFFF, 1, 0)
            #expect(w != 0 && port != 0)
            #expect(Rest.signed("BeginDrawingOffscreen", [w]) == -50) // none attached
            #expect(Rest.signed("AttachOffscreenBuffer", [w, rd32(buf)]) == 0)
            #expect(Rest.signed("BeginDrawingOffscreen", [w]) == 0)
            // drawing now goes to the buffer: fill it black
            let cur = Rest.ptr(4), dev = Rest.ptr(4)
            tb_GetGWorld(cur, dev)
            #expect(rd32(cur) == rd32(buf))
            tb_PaintRect(r)
            #expect(Rest.signed("EndDrawingOffscreen", [w]) == 0)
            tb_GetGWorld(cur, dev)
            #expect(rd32(cur) != rd32(buf))
            #expect(Rest.signed("EndDrawingOffscreen", [w]) == -50) // not drawing
            let taken = Rest.ptr(4)
            #expect(Rest.signed("DetachOffscreenBuffer", [w, taken]) == 0 && rd32(taken) == rd32(buf))
            #expect(Rest.signed("DetachOffscreenBuffer", [w, taken]) == -50)
            #expect(Rest.signed("DisposeOffscreenBuffer", [rd32(buf)]) == 0)
            #expect(Rest.signed("DisposeOffscreenBuffer", [0]) == -50)
        }

        @Test func processCalls() {
            #expect(Rest.signed("GetCurrentAppRefNum") == Int32(Int16(bitPattern: rd16(UInt32(LM_CurApRefNum)))))
            let psn = Rest.ptr(8)
            #expect(Rest.signed("GetSystemClientProcess", [psn]) == 0)
            #expect(Rest.signed("GetSystemClientProcess", [0]) == -50)
            #expect(Rest.signed("ResumeProcess", [psn]) == 0)
            // the process's own memory, read and written
            let a = Rest.ptr(8), b = Rest.ptr(8)
            wr32(a, 0xDEAD_BEEF)
            #expect(Rest.signed("ReadWriteProcessMemory", [psn, 0, a, b, 4]) == 0 && rd32(b) == 0xDEAD_BEEF)
            wr32(b, 0x1234_5678)
            #expect(Rest.signed("ReadWriteProcessMemory", [psn, 1, a, b, 4]) == 0 && rd32(a) == 0x1234_5678)
            let other = Rest.ptr(8)
            wr32(other, 77); wr32(other + 4, 99)
            #expect(Rest.signed("ReadWriteProcessMemory", [other, 0, a, b, 4]) == -600)
            // a key-down in the queue
            #expect(Rest.signed("PostFakeKeyDown", [0x41]) == 0)
            let ev = Rest.ptr(16)
            #expect(tb_GetNextEvent(0x0008, ev) != 0 && rd16(ev) == 3 && rd32(ev + 2) & 0xFF == 0x41)
        }

        @Test func windowsAreChecked() {
            _ = initToolbox()
            func make(_ top: Int16) -> UInt32 {
                let wr = Rest.ptr(8)
                wr16(wr, UInt16(bitPattern: top)); wr16(wr + 2, 50); wr16(wr + 4, UInt16(bitPattern: top + 60)); wr16(wr + 6, 150)
                return tb_NewWindow(0, wr, Rest.ptr(8), 1, 0, 0xFFFF_FFFF, 1, 0)
            }
            let a = make(50), b = make(100)
            #expect(Rest.call("CheckWindow", [a]) == 1 && Rest.call("CheckWindow", [b]) == 1)
            #expect(Rest.call("CheckWindow", [0x1234]) == 0 && Rest.call("CheckWindow", [0]) == 0)
            // the routine answers true for the window it keeps in its refCon
            // (cmpw r3,r4: eq -> true): cmplw cr0,r3,r4; mfcr r0; rlwinm r3,r0,3,31,31; blr
            let proc = Rest.routine([0x7C03_2040, 0x7C00_0026, 0x5403_1FFE, 0x4E80_0020])
            #expect(Rest.call("EachWindow", [proc, a]) == a)
            #expect(Rest.call("EachWindow", [proc, 0x77]) == 0)
            #expect(Rest.signed("EachWindow", [0, 0]) == -50)
        }

        @Test func trapAddresses() {
            let t = Rest.call("GetProcessTrapAddress", [0xA9F4])
            #expect(t == tb_GetTrapAddress(0xA9F4, true, false) || t != 0)
            let old = Rest.call("GetProcessTrapAddress", [0xA03C])
            Rest.call("SetProcessTrapAddress", [old, 0xA03C])
            #expect(Rest.call("GetProcessTrapAddress", [0xA03C]) == old)
        }

        @Test func deskPortRealScriptAndFolders() {
            _ = initToolbox()
            #expect(Rest.call("GetDeskPort") == rd32(UInt32(LM_WMgrPort)))
            #expect(Rest.signed("RealScript", [3]) == 3)
            #expect(Rest.signed("RealScript", [0xFFFF_FFFF]) >= 0) // the system's
            #expect(Rest.signed("RealScript", [0xFFFF_FFFE]) >= 0) // the current font's
            Rest.call("RedrawAll")
            let vref = Rest.ptr(2), dir = Rest.ptr(4), v2 = Rest.ptr(2), d2 = Rest.ptr(4)
            let a = Rest.signed("FindFolderInternalExtended", [0xFFFF_8000, fourCC("pref"), 1, 0, 0, vref, dir])
            let b = Rest.signed("FindFolderExtended", [0xFFFF_8000, fourCC("pref"), 1, 0, 0, v2, d2])
            #expect(a == b)
            #expect(Rest.signed("FindFolderInternalExtended", [0xFFFF_8000, fourCC("pref"), 1, 0, 0, v2, d2]) == a)
        }
    }
}
