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
            Thread.sleep(forTimeInterval: 0.005)
            time_pump()
            #expect(rd32(out) == tm && rd32(out + 8) == 1)
            #expect(g_cpu!.pointee.d.0 == 7)
            #expect(rd16(tm + 4) & 0x8000 == 0)
            time_pump()
            #expect(rd32(out + 8) == 1) // once only
            // RmvTime on a long wait leaves the time remaining in negated microseconds.
            tb_PrimeTime(tm, 10_000)
            tb_RmvTime(tm)
            let left = Int32(bitPattern: rd32(tm + UInt32(TMTask_tmCount_off)))
            #expect(left < -9_000_000 && left >= -10_000_000)
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
