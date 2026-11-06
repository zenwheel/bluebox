import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// A program waiting by spinning lets the host sleep (idle.c): on the
    /// wall clock, a loop of polls doing nothing, or a loop of its own
    /// reading Ticks, finds the emulator thread asleep most of the time; a
    /// loop that counts, draws or calls the Toolbox for work never does.
    @Suite struct IdleSleep {
        /// Launch a one-segment application whose code is `body` (words),
        /// on the wall clock; run it for `seconds`; the sleeps it let the
        /// host take and the time they took.
        func run(_ body: [Int], seconds: Double = 0.3) throws -> (sleeps: UInt64, slept: Double, halted: Bool, wall: Double) {
            bootMachine()
            let jt = cat(be16(0), be16(0x3F3C), be16(1), be16(0xA9F0)) // entry 0: seg 1, offset 0
            let code0 = cat(be32(48), be32(256), be32(8), be32(32), jt)
            let code1 = cat(be16(0), be16(1), body.flatMap { be16($0) })
            var fork = buildFork([
                TestResource(type: "CODE", id: 0, data: code0),
                TestResource(type: "CODE", id: 1, data: code1),
            ])
            let dir = tempDir()
            defer { try? FileManager.default.removeItem(at: dir) }
            let path = dir.appendingPathComponent("Spinner").path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            #expect(machine_launch(path, &err, err.count))
            machine_set_realtime(true)
            defer { machine_set_realtime(false) }
            var ns0: UInt64 = 0, ns1: UInt64 = 0
            let sleeps0 = machine_idle_sleeps(&ns0)
            let start = Date()
            var halted = false
            while Date().timeIntervalSince(start) < seconds {
                if machine_run(200) == RUN_HALTED {
                    halted = true
                    break
                }
            }
            let wall = Date().timeIntervalSince(start)
            let sleeps = machine_idle_sleeps(&ns1) - sleeps0
            return (sleeps, Double(ns1 - ns0) / 1e9, halted, wall)
        }

        /// BRA.S back to the loop's first word, from after `words` words.
        func back(_ words: Int) -> Int { 0x6000 | ((-(words * 2 + 2)) & 0xFF) }

        // The pieces of the loops.
        let tickCount = [0x42A7, 0xA975, 0x201F]               // CLR.L -(SP); _TickCount; MOVE.L (SP)+,D0
        let button = [0x4267, 0xA974, 0x321F]                  // CLR.W -(SP); _Button; MOVE.W (SP)+,D1
        let eventAvail = [0x4267, 0x3F3C, 0xFFFF, 0x486D, 0xFFE0, 0xA971, 0x301F] // EventAvail(everyEvent, -32(A5))

        @Test func aLoopOfPollsLetsTheHostSleep() throws {
            let loop = tickCount + button + eventAvail
            let r = try run(loop + [back(loop.count)])
            #expect(!r.halted)
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
            #expect(r.slept > r.wall / 2, "slept \(r.slept) s of \(r.wall)")
        }

        @Test func notWithTheSleepTurnedOff() throws {
            machine_set_idle_sleep(false)
            defer { machine_set_idle_sleep(true) }
            let loop = tickCount + button
            let r = try run(loop + [back(loop.count)], seconds: 0.1)
            #expect(r.sleeps == 0)
        }

        /// A loop of its own reading Ticks in low memory: waiting 30 ticks
        /// takes half a second still, the host asleep for most of it.
        @Test func aLoopReadingTicksLetsTheHostSleepAndTicksGoOn() throws {
            let body = [0x2238, 0x016A,                 // MOVE.L Ticks,D1
                        0x0681, 0x0000, 0x001E,         // ADDI.L #30,D1
                        0xB2B8, 0x016A,                 // loop: CMP.L Ticks,D1
                        0x62FA,                         // BHI.S loop
                        0xA9F4]                         // _ExitToShell
            let r = try run(body, seconds: 3)
            #expect(r.halted)
            #expect(r.wall > 0.4 && r.wall < 1.5, "30 ticks took \(r.wall) s")
            #expect(r.sleeps > 10 && r.slept > r.wall / 2, "slept \(r.sleeps) times, \(r.slept) s of \(r.wall)")
        }

        /// A count kept in a register: how often the loop goes round shows.
        @Test func aCountInARegisterIsWork() throws {
            let loop = [0x5287] + button                // ADDQ.L #1,D7
            #expect(try run(loop + [back(loop.count)]).sleeps == 0)
        }

        /// ... or in memory.
        @Test func aCountInMemoryIsWork() throws {
            let loop = [0x52AD, 0xFFF8] + button        // ADDQ.L #1,-8(A5)
            #expect(try run(loop + [back(loop.count)]).sleeps == 0)
        }

        /// ... or in memory, with no calls at all (a speed calibration).
        @Test func aCountInMemoryWithNoCallsIsWork() throws {
            let loop = [0x52AD, 0xFFF8]                 // ADDQ.L #1,-8(A5)
            #expect(try run(loop + [back(loop.count)]).sleeps == 0)
        }

        /// A Toolbox call that does something (here SetRect) between polls.
        @Test func aCallThatDoesSomethingIsWork() throws {
            let setRect = [0x486D, 0xFFF0, 0x4267, 0x4267, 0x3F3C, 0x0010, 0x3F3C, 0x0010, 0xA8A7]
            let loop = setRect + button
            #expect(try run(loop + [back(loop.count)]).sleeps == 0)
        }

        /// A computation going round in a few registers, calling nothing.
        @Test func aComputationIsWork() throws {
            let loop = [0xD081, 0xE388, 0x5281]         // ADD.L D1,D0; LSL.L #1,D0; ADDQ.L #1,D1
            #expect(try run(loop + [back(loop.count)]).sleeps == 0)
        }
    }
}
