import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension Classic {
    @Suite struct ThreadManager {
        init() { bootMachine() }

        func code(_ words: [UInt16]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
            return p
        }

        func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
        func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }

        /// YieldToAnyThread: YieldToThread(kNoThreadID), its result dropped.
        let yield: [UInt16] = [0x4267, 0x42A7, 0x303C, 0x0205, 0xABF2, 0x548F]

        /// A cooperative thread runs when the application thread yields, yields
        /// back, runs again, and its entry routine's result reaches threadResult.
        @Test func cooperativeThreadsTakeTurns() {
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let vars = tb_NewPtr(24, false, true) // made, result, step1, step2, main
            let made = vars, result = vars + 4, step1 = vars + 8, step2 = vars + 12, main = vars + 16
            // pascal void *entry(void *param)
            let entry = code(
                [0x23FC, 0, 1, hi(step1), lo(step1)] // MOVE.L #1,step1
                    + yield
                    + [0x23FC, 0, 2, hi(step2), lo(step2)] // MOVE.L #2,step2
                    + [0x205F, 0x588F, 0x2EBC, 0x0000, 0x1234, 0x4ED0]) // return $1234
            let app = code(
                [0x4267, 0x2F3C, 0, 1, 0x4879, hi(entry), lo(entry), 0x42A7, 0x42A7, 0x42A7,
                 0x4879, hi(result), lo(result), 0x4879, hi(made), lo(made),
                 0x303C, 0x0E03, 0xABF2, 0x548F] // NewThread(cooperative, entry, nil, 0, 0, &result, &made)
                    + yield // the thread runs to its first yield
                    + [0x2039, hi(step1), lo(step1), 0x23C0, hi(main), lo(main)] // main := step1
                    + yield // the thread finishes
                    + [0x4E75])
            guest_call(app)
            #expect(rd32(made) > 2)
            #expect(rd32(main) == 1)      // the thread had run once when the application resumed
            #expect(rd32(step2) == 2)     // and ran on when it yielded again
            #expect(rd32(result) == 0x1234)
            let state = tb_NewPtr(4, false, true)
            #expect(tb_GetThreadState(Int32(bitPattern: rd32(made)), state) == -618) // gone: threadNotFoundErr
            #expect(tb_GetCurrentThread(state) == 0 && rd32(state) == 2) // back in the application thread
        }

        /// With no other thread ready, yielding returns to the caller at once.
        @Test func yieldingAloneReturns() {
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let out = tb_NewPtr(4, false, true)
            guest_call(code(yield + [0x23FC, 0, 7, hi(out), lo(out), 0x4E75]))
            #expect(rd32(out) == 7)
        }
    }
}
