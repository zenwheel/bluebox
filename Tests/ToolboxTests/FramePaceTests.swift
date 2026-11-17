import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// A program drawing frames faster than the display shows them waits
    /// for the next refresh once it has finished one in this refresh
    /// (pace.c): on the wall clock, a loop drawing on the screen and
    /// polling goes round about sixty times a second; one drawing
    /// elsewhere, computing, or already slower than that is never held up,
    /// and the instruction clock never waits.
    @Suite struct FramePace {
        struct Result {
            var waits: UInt64, drawn: UInt64, shown: UInt64, waited: Double, wall: Double, ran: Double
            var rounds: UInt32 /* D7, counted up by a loop that counts its rounds */
            var drawnPerSecond: Double { Double(drawn) / wall }
        }

        /// Launch a one-segment application whose code is `body` (words)
        /// on the wall clock (`realtime`) or the instruction clock; run it
        /// for `seconds`; what pacing did meanwhile.
        func run(_ body: [Int], seconds: Double = 0.5, pacing: Bool = true, realtime: Bool = true) throws -> Result {
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
            let path = dir.appendingPathComponent("Animator").path
            var f = mac_file()
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            #expect(machine_launch(path, &err, err.count))
            machine_set_realtime(realtime)
            defer { machine_set_realtime(false) }
            machine_set_frame_pacing(pacing)
            defer { machine_set_frame_pacing(true) }
            var d0: UInt64 = 0, s0: UInt64 = 0, n0: UInt64 = 0, d1: UInt64 = 0, s1: UInt64 = 0, n1: UInt64 = 0
            let w0 = machine_frames_paced(&d0, &s0, &n0)
            let t0 = machine_time_ns(), start = Date()
            if realtime {
                while Date().timeIntervalSince(start) < seconds {
                    if machine_run(200) == RUN_HALTED { break }
                }
            } else {
                _ = machine_run(UInt64(seconds * 1e7))
            }
            let wall = Date().timeIntervalSince(start)
            let w1 = machine_frames_paced(&d1, &s1, &n1)
            return Result(waits: w1 - w0, drawn: d1 - d0, shown: s1 - s0, waited: Double(n1 - n0) / 1e9, wall: wall,
                          ran: Double(machine_time_ns() - t0) / 1e9, rounds: g_cpu!.pointee.d.7)
        }

        func back(_ words: Int) -> Int { 0x6000 | ((-(words * 2 + 2)) & 0xFF) }

        let button = [0x4267, 0xA974, 0x321F]                  // CLR.W -(SP); _Button; MOVE.W (SP)+,D1
        let getKeys = [0x486D, 0xFFD0, 0xA976]                 // GetKeys(-48(A5))
        /// InitGraf, OpenPort (on the screen), the rects (10,10)-(50,50) and
        /// (10,60)-(50,100) at -224 and -232.
        let drawSetup = [0x486D, 0xFFFC, 0xA86E,                 // InitGraf(@thePort)
                         0x706C, 0xA11E, 0x2F08, 0xA86F,         // OpenPort(NewPtr(108))
                         0x486D, 0xFF20, 0x3F3C, 10, 0x3F3C, 10, 0x3F3C, 50, 0x3F3C, 50, 0xA8A7,
                         0x486D, 0xFF18, 0x3F3C, 60, 0x3F3C, 10, 0x3F3C, 100, 0x3F3C, 50, 0xA8A7]
        let invertRect = [0x486D, 0xFF20, 0xA8A4]
        let invertRect2 = [0x486D, 0xFF18, 0xA8A4]
        /// The port's pixels moved off the screen, into a buffer of their own.
        let offscreen = [0x203C, 0x0004, 0x0000, 0xA11E,         // NewPtr(256K)
                         0x226D, 0xFFFC, 0x2348, 0x0002] // thePort^.portBits.baseAddr := it

        /// An animation drawing on the screen and polling, as fast as it
        /// goes: a frame a refresh, the clock going on as ever.
        @Test func framesFasterThanTheDisplayWaitForTheRefresh() throws {
            let loop = invertRect + button
            let r = try run(drawSetup + loop + [back(loop.count)])
            #expect(r.waits > 10, "waited \(r.waits) times")
            #expect(r.drawnPerSecond < 70, "\(r.drawnPerSecond) frames a second")
            #expect(r.drawnPerSecond > 40, "\(r.drawnPerSecond) frames a second")
            #expect(r.waited > r.wall / 2, "waited \(r.waited) s of \(r.wall)")
            #expect(abs(r.ran - r.wall) < 0.05, "the clock moved \(r.ran) s in \(r.wall)")
        }

        @Test func notWithPacingTurnedOff() throws {
            let loop = invertRect + button
            let r = try run(drawSetup + loop + [back(loop.count)], pacing: false)
            #expect(r.waits == 0)
            #expect(r.drawnPerSecond > 200, "\(r.drawnPerSecond) frames a second")
            #expect(Double(r.shown) / r.wall < 62, "\(r.shown) refreshes showed a new frame in \(r.wall) s")
        }

        /// The program's own stores into the screen are frames too.
        @Test func storesIntoTheScreenWaitForTheRefresh() throws {
            let loop = [0x2078, 0x0824, 0x5290] + button       // MOVE.L ScrnBase,A0; ADDQ.L #1,(A0)
            let r = try run(drawSetup + loop + [back(loop.count)])
            #expect(r.waits > 10, "waited \(r.waits) times")
            #expect(r.drawnPerSecond < 70, "\(r.drawnPerSecond) frames a second")
        }

        /// A second poll within the frame (GetKeys between two halves of it
        /// drawn) leaves the frame whole: the loop goes round once a
        /// refresh, not once in two.
        @Test func aPollWithinTheFrameDoesNotSplitIt() throws {
            let loop = [0x5287] + button + invertRect + getKeys + invertRect2 // ADDQ.L #1,D7
            let r = try run(drawSetup + [0x7E00] + loop + [back(loop.count)]) // MOVEQ #0,D7
            let perSecond = Double(r.rounds) / r.wall
            #expect(r.waits > 10, "waited \(r.waits) times")
            #expect(perSecond > 45 && perSecond < 70, "\(perSecond) rounds a second")
        }

        /// Drawing elsewhere than on the screen isn't a frame shown.
        @Test func drawingOffTheScreenIsNotPaced() throws {
            let loop = invertRect + button
            let r = try run(drawSetup + offscreen + loop + [back(loop.count)], seconds: 0.3)
            #expect(r.waits == 0)
            #expect(r.drawn == 0)
        }

        /// Computing between polls, nothing drawn (a chess program thinking).
        @Test func computingIsNotPaced() throws {
            let loop = [0x5287] + button                        // ADDQ.L #1,D7
            let r = try run(loop + [back(loop.count)], seconds: 0.3)
            #expect(r.waits == 0)
        }

        /// A program already slower than the display (each frame a Delay of
        /// two ticks) is never held up.
        @Test func framesSlowerThanTheDisplayDoNotWait() throws {
            let loop = invertRect + [0x307C, 2, 0xA03B] + button // Delay(2)
            let r = try run(drawSetup + loop + [back(loop.count)])
            #expect(r.drawn > 5, "drew \(r.drawn) frames")
            #expect(r.waits == 0, "waited \(r.waits) times")
        }

        /// The instruction clock (a headless run, the corpus walk) never
        /// waits, so its runs stay the same.
        @Test func theInstructionClockIsNotPaced() throws {
            let loop = invertRect + button
            let r = try run(drawSetup + loop + [back(loop.count)], seconds: 0.5, realtime: false)
            #expect(r.waits == 0)
            #expect(r.drawn == 0)
        }
    }
}
