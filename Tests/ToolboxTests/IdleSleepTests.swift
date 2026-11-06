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
        func run(_ body: [Int], seconds: Double = 0.3, depth: Int32 = 1) throws -> (sleeps: UInt64, slept: Double, halted: Bool, wall: Double) {
            machine_set_screen_depth(depth)
            defer { machine_set_screen_depth(1) }
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
        let eventAvailLow = [0x4267, 0x3F3C, 0xFFFF, 0x486D, 0xFF00, 0xA971, 0x301F] // EventAvail(everyEvent, -256(A5))

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

        // Drawing (QuickDraw on the screen, through a port of the program's
        // own).  Below A5: specs at -256 and -248, rects at -232 and -224,
        // QuickDraw's globals from -206.

        /// InitGraf, OpenPort, the rects (10,10)-(50,50) and (10,60)-(50,100),
        /// the first painted.
        let drawSetup = [0x486D, 0xFFFC, 0xA86E,                 // InitGraf(@thePort)
                         0x706C, 0xA11E, 0x2F08, 0xA86F,         // OpenPort(NewPtr(108))
                         0x486D, 0xFF20, 0x3F3C, 10, 0x3F3C, 10, 0x3F3C, 50, 0x3F3C, 50, 0xA8A7,
                         0x486D, 0xFF18, 0x3F3C, 60, 0x3F3C, 10, 0x3F3C, 100, 0x3F3C, 50, 0xA8A7,
                         0x486D, 0xFF20, 0xA8A2]                 // PaintRect
        let paintRect = [0x486D, 0xFF20, 0xA8A2]
        let invertRect = [0x486D, 0xFF20, 0xA8A4]
        /// CopyBits(thePort^.portBits, thePort^.portBits, first, second, srcCopy, nil)
        let copyBits = [0x206D, 0xFFFC, 0x4868, 0x0002, 0x4868, 0x0002, 0x486D, 0xFF20, 0x486D, 0xFF18,
                        0x4267, 0x42A7, 0xA8EC]

        /// A pause screen drawn again and again, the same each time.
        @Test func drawingTheSameFrameAgainLetsTheHostSleep() throws {
            let loop = paintRect + button
            let r = try run(drawSetup + loop + [back(loop.count)])
            #expect(!r.halted)
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
            #expect(r.slept > r.wall / 2, "slept \(r.slept) s of \(r.wall)")
        }

        @Test func copyingTheSameFrameAgainLetsTheHostSleep() throws {
            let loop = copyBits + eventAvailLow
            let r = try run(drawSetup + loop + [back(loop.count)], depth: 8)
            #expect(!r.halted)
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
            #expect(r.slept > r.wall / 2, "slept \(r.slept) s of \(r.wall)")
        }

        /// Frames that differ: the rect moving along (its place kept in
        /// memory) ...
        @Test func aMovingRectIsWork() throws {
            let loop = [0x526D, 0xFF22, 0x526D, 0xFF26] + paintRect + button // ADDQ.W #1 to left and right
            #expect(try run(drawSetup + loop + [back(loop.count)]).sleeps == 0)
        }

        /// ... or drawn where the pen has got to, which only the port shows
        /// once the line has left the screen ...
        @Test func aLineGoingOnIsWork() throws {
            let loop = [0x3F3C, 1, 0x4267, 0xA892] + button // Line(1, 0)
            #expect(try run(drawSetup + loop + [back(loop.count)]).sleeps == 0)
        }

        /// ... or two frames in turn, as fast as it goes round (nothing but
        /// the pixels says so).
        @Test func twoFramesInTurnAreWork() throws {
            let loop = invertRect + button
            #expect(try run(drawSetup + loop + [back(loop.count)]).sleeps == 0)
        }

        /// A blinking frame timed by the clock: the same frame drawn over and
        /// over between the changes, the host asleep through it.
        @Test func aFrameBlinkingByTheClockLetsTheHostSleep() throws {
            // TickCount; BTST #3,D0; BEQ.S erase; PaintRect; BRA.S on; erase: EraseRect; on: Button
            let loop = tickCount + [0x0800, 3, 0x6708] + paintRect + [0x6006, 0x486D, 0xFF20, 0xA8A3] + button
            let r = try run(drawSetup + loop + [back(loop.count)], seconds: 0.5)
            #expect(!r.halted)
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
            #expect(r.slept > r.wall / 2, "slept \(r.slept) s of \(r.wall)")
        }

        /// The program's own stores into the screen are pixels too: a frame
        /// it draws itself, the same each time, is nothing new; a count it
        /// keeps there is.
        @Test func theSameFrameStoredOnTheScreenLetsTheHostSleep() throws {
            let loop = [0x2078, 0x0824, 0x20BC, 0xFFFF, 0xFFFF, 0x217C, 0x00FF, 0x00FF, 0x0040] + button
            let r = try run(drawSetup + loop + [back(loop.count)]) // MOVE.L ScrnBase,A0; two longs stored
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
        }

        @Test func aCountOnTheScreenIsWork() throws {
            let loop = [0x2078, 0x0824, 0x5290] + button // ADDQ.L #1,(A0)
            #expect(try run(drawSetup + loop + [back(loop.count)]).sleeps == 0)
        }

        /// The screen's colours set again: the same colours each time are
        /// nothing new, though the table's seed moves on, and two colours set
        /// in turn between two polls are a loop going round the same; a
        /// colour fading on is not.
        let specs = [0x2B7C, 0x0005, 0xFFFF, 0xFF00, 0x2B7C, 0x0000, 0x0000, 0xFF04,  // 5: red at -256
                     0x2B7C, 0x0005, 0x0000, 0xFF08, 0x2B7C, 0xFFFF, 0x0000, 0xFF0C]  // 5: green at -248
        func setEntries(_ at: Int) -> [Int] { [0x3F3C, 5, 0x4267, 0x486D, at, 0xAA3F] }

        @Test func theSameColoursSetAgainLetTheHostSleep() throws {
            let loop = setEntries(0xFF00) + button + setEntries(0xFF00) + button
            let r = try run(drawSetup + specs + loop + [back(loop.count)], depth: 8)
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
        }

        @Test func twoColoursInTurnLetTheHostSleep() throws {
            let loop = setEntries(0xFF00) + button + setEntries(0xFF08) + button
            let r = try run(drawSetup + specs + loop + [back(loop.count)], depth: 8)
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
        }

        @Test func aColourFadingIsWork() throws {
            let loop = [0x046D, 0x0100, 0xFF02] + setEntries(0xFF00) + button // SUBI.W #$100,-254(A5): less red
            #expect(try run(drawSetup + specs + loop + [back(loop.count)], depth: 8).sleeps == 0)
        }

        /// A program at work for a while (a count kept in memory, which the
        /// watch finds again and again and so looks less often), then
        /// waiting: the wait is still found.
        @Test func aWaitAfterWorkIsStillFound() throws {
            let body = [0x3B7C, 3000, 0xFF10] + button + [0x536D, 0xFF10, 0x66F4] // 3000 times: Button, SUBQ.W #1; then
                + button + [back(button.count)]                                      // Button for ever
            let r = try run(body, seconds: 0.5)
            #expect(r.sleeps > 20, "slept \(r.sleeps) times")
        }

        /// A computation going round in a few registers, calling nothing.
        @Test func aComputationIsWork() throws {
            let loop = [0xD081, 0xE388, 0x5281]         // ADD.L D1,D0; LSL.L #1,D0; ADDQ.L #1,D1
            #expect(try run(loop + [back(loop.count)]).sleeps == 0)
        }
    }
}
