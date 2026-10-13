import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension Classic {
    @Suite struct TimeManager {
        init() { bootMachine() }

        /// A task that stores A1 and A0 and counts its runs.
        func task(_ out: UInt32) -> UInt32 {
            let words: [UInt16] = [
                0x23C9, UInt16(out >> 16), UInt16(out & 0xFFFF),             // MOVE.L A1,out
                0x23C8, UInt16((out + 4) >> 16), UInt16((out + 4) & 0xFFFF), // MOVE.L A0,out+4
                0x52B9, UInt16((out + 8) >> 16), UInt16((out + 8) & 0xFFFF), // ADDQ.L #1,out+8
                0x7063,                                                      // MOVEQ #99,D0 (clobbered)
                0x4E75,                                                      // RTS
            ]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            return code
        }

        func setStack() {
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
        }

        @Test func primedTaskRunsOnceWithA1AndRegistersPreserved() throws {
            setStack()
            let out = tb_NewPtr(12, false, true)
            let tm = tb_NewPtr(Int32(TMTask_SIZEOF), false, true)
            wr32(tm + UInt32(TMTask_tmAddr_off), task(out))
            tb_InsTime(tm)
            tb_PrimeTime(tm, 1)
            #expect(rd16(tm + 4) & 0x8000 != 0)
            g_cpu!.pointee.d.0 = 7
            g_cpu!.pointee.sr = 0x2004 // Z set: the interrupted code may be about to branch on it
            machine_skip_time(5_000_000) // headless, the clock follows instructions
            time_pump()
            #expect(rd32(out) == tm && rd32(out + 8) == 1)
            #expect(g_cpu!.pointee.d.0 == 7)
            #expect(g_cpu!.pointee.sr == 0x2004)
            #expect(rd16(tm + 4) & 0x8000 == 0)
            time_pump()
            #expect(rd32(out + 8) == 1) // once only
            // RmvTime on a long wait leaves the time remaining in negated microseconds.
            tb_PrimeTime(tm, 10_000)
            tb_RmvTime(tm)
            let left = Int32(bitPattern: rd32(tm + UInt32(TMTask_tmCount_off)))
            #expect(left < -9_000_000 && left >= -10_000_000)
        }

        /// Time (seconds since 1904) moves on with the clock, as Ticks does.
        @Test func timeFollowsTheClock() {
            machine_advance_ticks()
            let t0 = rd32(0x20C)
            machine_skip_time(2_000_000_000)
            machine_advance_ticks()
            #expect(rd32(0x20C) - t0 == 2)
        }

        /// Keys held down show in the low-memory KeyMap, which games read
        /// in place of GetKeys.
        @Test func keyMapInLowMemory() {
            machine_post_key(Int32(UInt8(ascii: "p")), 35, 0, 1, 0)
            events_sync_lowmem()
            #expect(rd8(0x174 + 4) & 0x08 != 0) // key code 35: byte 4, bit 3
            #expect(rd16(0x184) == 0x2370) // KeyLast: key code and character
            machine_post_key(Int32(UInt8(ascii: "p")), 35, 0, 0, 0)
            events_sync_lowmem()
            #expect(rd8(0x174 + 4) & 0x08 == 0)
            // MBTicks: Ticks when the button last changed.
            machine_skip_time(1_000_000_000)
            machine_advance_ticks()
            machine_post_mouse(10, 10, 1)
            events_sync_lowmem()
            #expect(rd32(0x16E) == rd32(0x16A) && rd32(0x16E) >= 60)
            machine_post_mouse(10, 10, 0)
        }

        /// The VBL queue in low memory lists installed tasks after the
        /// system's own, as programs that look for theirs expect.
        @Test func vblQueueInLowMemory() {
            let v = tb_NewPtr(Int32(VBLTask_SIZEOF), false, true)
            wr16(v + 4, 1)
            #expect(tb_VInstall(v) == 0)
            let head = rd32(0x160 + 2)
            #expect(head != 0 && head != v)       // a system task comes first
            #expect(rd32(head) == v)              // and links to ours
            #expect(rd32(0x160 + 6) == v)         // the tail
            #expect(tb_VRemove(v) == 0)
            #expect(rd32(head) == 0)
        }

        /// SysEnvirons agrees with Gestalt: a Quadra with a 68040 and its FPU.
        @Test func sysEnvironsFollowsGestalt() {
            let rec = tb_NewPtr(16, false, true)
            #expect(tb_SysEnvirons(2, rec) == 0)
            #expect(rd16(rec + 2) == 33)          // machineType: Quadra 800 (Gestalt 35 - 2)
            #expect(rd16(rec + 6) == 5)           // processor: 68040
            #expect(rd8(rec + 8) != 0)            // hasFPU
        }

        /// Low memory programs read directly holds what a real Mac's would.
        @Test func lowMemoryAtBoot() {
            launchTestApp() // the file system's tables come with a launch
            #expect(Int16(bitPattern: rd16(0x100)) < 0)  // MonkeyLives: no Monkey
            #expect(rd8(0x21E) == 2)                     // KbdType: extended keyboard
            #expect(rd8(0x260) == 7)                     // SdVolume
            #expect(rd16(0xD00) > 0)                     // TimeDBRA
            // Hardware and the unit table: memory of their own, not address 0.
            for a in [0x1D4, 0x1D8, 0x1DC, 0x1E0, 0x266, 0x11C] as [UInt32] { #expect(rd32(a) >= 0x1000) }
            #expect(rd16(0x1D2) > 0)                     // UnitNtryCnt
            #expect(rd8(0x1F8) == 0xA8)                  // SPValid: parameter RAM valid
            #expect(Int16(bitPattern: rd16(0x9DC)) == -1) // PaintWhite
            // The drive queue holds the volume as drive 1.
            let drive = rd32(0x308 + 2)
            #expect(drive != 0 && rd16(drive + 6) == 1)
            let scrap = rd32(0x96C)                      // ScrapName
            #expect(scrap != 0 && rd8(scrap) == 14)
        }

        /// ResErrProc hears of resource errors; RomMapInsert lasts one call.
        @Test func resourceManagerLowMemory() {
            setStack()
            let out = tb_NewPtr(4, false, true)
            let words: [UInt16] = [0x23C0, UInt16(out >> 16), UInt16(out & 0xFFFF), 0x4E75] // MOVE.L D0,out; RTS
            let proc = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(proc + UInt32(i * 2), w) }
            wr32(0xAF2, proc)
            _ = tb_HomeResFile(0)
            #expect(Int32(bitPattern: rd32(out)) == -192) // resNotFound
            wr32(0xAF2, 0)
            wr8(0xB9E, 0xFF)
            _ = tb_GetResource(Int32(bitPattern: 0x43555253), 1) // 'CURS' 1
            #expect(rd8(0xB9E) == 0)
        }

        @Test func vblTaskRunsWhenItsCountReachesZero() {
            setStack()
            let out = tb_NewPtr(12, false, true)
            let v = tb_NewPtr(Int32(VBLTask_SIZEOF), false, true)
            wr16(v + 4, 1) // vType
            wr32(v + UInt32(VBLTask_vblAddr_off), task(out))
            wr16(v + UInt32(VBLTask_vblCount_off), 2)
            #expect(tb_VInstall(v) == 0)
            time_pump()
            wr32(UInt32(LM_Ticks), rd32(UInt32(LM_Ticks)) + 1)
            time_pump()
            #expect(rd32(out + 8) == 0)
            wr32(UInt32(LM_Ticks), rd32(UInt32(LM_Ticks)) + 1)
            time_pump()
            #expect(rd32(out + 8) == 1 && rd32(out + 4) == v)
            #expect(tb_VRemove(v) == 0)
        }
    }
}
