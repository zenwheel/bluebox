import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The host's Suspend: the machine held still, its clock with it, and
    /// going on from where it stopped.
    @Suite(.serialized) struct Suspend {
        /// Set by the test, read by the machine's thread.
        final class Flag: @unchecked Sendable {
            private let lock = NSLock()
            private var value = false
            var isSet: Bool { lock.withLock { value } }
            func set() { lock.withLock { value = true } }
        }

        /// A program counting in D7 for ever, a VBL task and a Time Manager
        /// task counting their runs at out and out+4.
        func setUp() -> (out: UInt32, tm: UInt32) {
            launchTestApp()
            let out = tb_NewPtr(8, false, true)
            func code(_ words: [UInt16]) -> UInt32 {
                let p = tb_NewPtr(Int32(words.count * 2), false, false)
                for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
                return p
            }
            let loop = code([0x5287, 0x60FC])                            // ADDQ.L #1,D7; BRA.S loop
            let vblCode = code([0x52B9, UInt16(out >> 16), UInt16(out & 0xFFFF), // ADDQ.L #1,out
                                0x317C, 0x0001, UInt16(VBLTask_vblCount_off),     // MOVE.W #1,vblCount(A0)
                                0x4E75])                                          // RTS
            let tmCode = code([0x52B9, UInt16((out + 4) >> 16), UInt16((out + 4) & 0xFFFF), // ADDQ.L #1,out+4
                               0x4E75])
            let v = tb_NewPtr(Int32(VBLTask_SIZEOF), false, true)
            wr16(v + 4, 1)                                                 // vType
            wr32(v + UInt32(VBLTask_vblAddr_off), vblCode)
            wr16(v + UInt32(VBLTask_vblCount_off), 1)
            #expect(tb_VInstall(v) == 0)
            let tm = tb_NewPtr(Int32(TMTask_SIZEOF), false, true)
            wr32(tm + UInt32(TMTask_tmAddr_off), tmCode)
            tb_InsTime(tm)
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000                                        // interrupts on
            cpu.pointee.d.7 = 0
            cpu.pointee.pc = loop
            return (out, tm)
        }

        func ms(_ n: UInt32) { usleep(n * 1000) }

        /// Suspended mid-run, no instruction runs, Ticks and the machine's
        /// clock stand still, and no VBL or Time Manager task fires; resumed,
        /// Ticks go on from where they stopped and a task primed before
        /// comes due after the time it was primed for, not at once.
        @Test func suspendedMachineStandsStill() {
            let (out, tm) = setUp()
            machine_set_realtime(true) // the clock is the host's, as with a window
            defer { machine_set_realtime(false) }
            tb_PrimeTime(tm, 400)
            let stop = Flag(), done = DispatchSemaphore(value: 0)
            Thread.detachNewThread {
                while !stop.isSet && !machine_halted() { _ = machine_run(50_000) }
                done.signal()
            }
            defer {
                machine_unpause()
                stop.set()
                done.wait()
            }
            ms(100)
            #expect(rd32(out) > 0)                       // the VBL task runs
            machine_pause()
            #expect(machine_paused())
            #expect(machine_wait_parked(2000))
            let cpu = g_cpu!
            let count = machine_instruction_count(), d7 = cpu.pointee.d.7
            let ticks = rd32(UInt32(LM_Ticks)), vbls = rd32(out), clock = machine_time_ns()
            #expect(d7 > 0)
            ms(600)
            #expect(machine_instruction_count() == count)
            #expect(cpu.pointee.d.7 == d7)
            #expect(rd32(UInt32(LM_Ticks)) == ticks)
            #expect(rd32(out) == vbls)
            #expect(rd32(out + 4) == 0)                  // due 400 ms in: 100 ms of it had passed
            #expect(machine_time_ns() == clock)
            machine_unpause()
            #expect(!machine_paused())
            ms(100)
            let after = rd32(UInt32(LM_Ticks))
            #expect(after >= ticks && after - ticks <= 12)   // about 6 ticks on, not 40
            #expect(cpu.pointee.d.7 != d7)
            #expect(rd32(out + 4) == 0)                  // 200 ms of the machine's time so far
            for _ in 0..<40 where rd32(out + 4) == 0 { ms(50) }
            #expect(rd32(out + 4) == 1)
        }

        /// pause:N, the scripted Suspend, holds the machine for N ms of real
        /// time and then lets it go on by itself.
        @Test func scriptedPauseEndsByItself() {
            _ = setUp()
            let start = machine_instruction_count()
            machine_script_add(start + 1000, 9, 300, 0, 0)
            let t0 = Date()
            _ = machine_run(200_000)
            #expect(Date().timeIntervalSince(t0) >= 0.28)
            #expect(!machine_paused())
            #expect(machine_instruction_count() >= start + 200_000)
        }
    }
}
