import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Which rows of the screen were written (dirty.c), as the presenters
    /// take them: by every writer, with no comparison of the screen.
    @Suite struct DirtyRows {
        /// A presenter's view: the rows written since its last look.
        final class Look {
            var seen: UInt64 = 0
            /// [-1] for everything; bands otherwise.
            func take(_ limit: Int32 = 16) -> [Int32] {
                machine_screen_publish() // as the machine's slice ends
                var w: Int32 = 0, h: Int32 = 0
                machine_screen_size(&w, &h)
                var bands = [Int32](repeating: 0, count: Int(limit) * 2)
                let n = present_dirty_rows(&seen, 0, h, &bands, limit)
                if n == Int32(PRESENT_DIRTY_ALL) { return [-1] }
                #expect(n != Int32(PRESENT_DIRTY_UNKNOWN), "writes tracked")
                return Array(bands.prefix(Int(max(n, 0)) * 2))
            }
        }

        /// Rows y0..<y1 among the bands (a band may run a row on: a region's
        /// stores can reach 3 bytes past it).
        func covers(_ bands: [Int32], _ y0: Int32, _ y1: Int32) -> Bool {
            (y0..<y1).allSatisfy { y in stride(from: 0, to: bands.count, by: 2).contains { bands[$0] <= y && y < bands[$0 + 1] } }
        }

        /// The rows marked, beyond those written, are few.
        func rowsIn(_ bands: [Int32]) -> Int32 {
            stride(from: 0, to: bands.count, by: 2).reduce(0) { $0 + bands[$1 + 1] - bands[$1] }
        }

        func screen() -> (base: UInt32, rb: UInt32, h: Int32) {
            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            return (qd_screen_display_base(), qd_screen_row_bytes(), h)
        }

        /// The writers every layout has: the Toolbox's stores, QuickDraw,
        /// BlockMove, the processor's own stores; and nothing written,
        /// nothing to do.
        func writers(_ look: Look) {
            let (base, rb, h) = screen()
            #expect(look.take() == [-1], "the first look: everything")
            #expect(look.take() == [], "nothing written")
            // The Toolbox's own stores.
            wr8(base + 100 * rb + 3, rd8(base + 100 * rb + 3) ^ 1)
            wr32(base + 300 * rb + 8, 0x1234_5678)
            let a = look.take()
            #expect(covers(a, 100, 101) && covers(a, 300, 301), "\(a)")
            #expect(rowsIn(a) <= 2 + 2 * (256 / Int32(rb) + 1), "\(a)")
            #expect(look.take() == [])
            // QuickDraw's.
            let r = tb_NewPtr(8, false, true)
            wr16(r, 50); wr16(r + 2, 10); wr16(r + 4, 60); wr16(r + 6, 40)
            _ = look.take()
            tb_InvertRect(r)
            let q = look.take()
            #expect(covers(q, 50, 60) && rowsIn(q) <= 10 + 2 * (256 / Int32(rb) + 1), "\(q)")
            // BlockMove into the screen.
            let src = tb_NewPtr(Int32(rb) * 3, false, true)
            for i in 0..<Int(rb) * 3 { wr8(src + UInt32(i), UInt8(truncatingIfNeeded: i * 7 + 1)) }
            _ = look.take()
            tb_BlockMove(src, base + 20 * rb, Int32(rb) * 3, false)
            #expect(covers(look.take(), 20, 23))
            // The processor's: MOVE.B #$5A,(A0) and (rb,A0).
            let p = base + 200 * rb + 7
            _ = look.take()
            let reason = run68k([0x207C, UInt16(p >> 16), UInt16(p & 0xFFFF), // MOVEA.L #p,A0
                                 0x10BC, 0x005A,                                // MOVE.B #$5A,(A0)
                                 0x117C, 0x005A, UInt16(rb),                    // MOVE.B #$5A,(rb,A0)
                                 0xA9F4])
            #expect(reason.contains("ExitToShell"))
            let c = look.take()
            #expect(covers(c, 200, 202), "\(c)")
            // A long store across a region's end, written again: both regions.
            let y = UInt32(h - 20), edge = (base + y * rb + 255) & ~255
            wr32(edge - 2, 0xAAAA_AAAA)
            _ = look.take()
            wr32(edge - 2, 0x5555_5555)
            let e = look.take()
            #expect(covers(e, Int32((edge - 2 - base) / rb), Int32((edge + 1 - base) / rb) + 1), "\(e)")
        }

        @Test func everyWriterIsSeen() {
            launchTestApp()
            _ = initToolbox()
            for d: Int32 in [1, 8, 32] {
                #expect(qd_screen_set_depth(d))
                writers(Look())
            }
        }

        @Test func aMacIIIn24BitMode() {
            withMachine("macii") {
                machine_set_addressing(24)
                launchTestApp()
                _ = initToolbox()
                #expect(g_mem_24bit)
                let look = Look()
                writers(look)
                // A pointer with flags in its top byte (the Toolbox's own
                // stores through a master pointer's value).
                let (base, rb, _) = screen()
                wr8((base + 10 * rb) | 0x8000_0000, 0x77)
                #expect(covers(look.take(), 10, 11))
            }
        }

        @Test func aCompactMacAndItsAlternatePage() {
            withMachine("plus") {
                launchTestApp()
                _ = initToolbox()
                let look = Look()
                writers(look)
                // The video shows the other page: everything.
                let (base, rb, _) = screen()
                qd_screen_set_display_base(base - 0x8000)
                #expect(look.take() == [-1])
                wr8(base - 0x8000 + 5 * rb, 0xFF)
                #expect(covers(look.take(), 5, 6), "the alternate page is watched now")
                qd_screen_set_display_base(base)
                #expect(look.take() == [-1])
            }
        }

        /// Depth changes and the page moved are everything; the arena's
        /// pages mapped again under the screen are its rows.
        @Test func modesAndRemaps() {
            launchTestApp()
            _ = initToolbox()
            #expect(qd_screen_set_depth(32))
            let look = Look()
            _ = look.take()
            #expect(qd_screen_set_depth(8))
            #expect(look.take() == [-1])
            let (base, rb, h) = screen()
            mem_discard(base + 64 * rb, 32 * rb)
            #expect(covers(look.take(), 64, 96))
            screen_dirty_all()
            #expect(look.take() == [-1])
            #expect(look.take() == [])
            #expect(h > 96)
        }

        /// Two presenters each see every write, whenever they look.
        @Test func presentersLookIndependently() {
            launchTestApp()
            _ = initToolbox()
            let (base, rb, _) = screen()
            let a = Look(), b = Look()
            _ = a.take(); _ = b.take()
            wr8(base + 7 * rb, 1)
            #expect(covers(a.take(), 7, 8))
            wr8(base + 9 * rb, 1)
            let bb = b.take()
            #expect(covers(bb, 7, 8) && covers(bb, 9, 10))
            #expect(!covers(a.take(), 7, 8))
        }

        /// A writer that isn't seen (the host's own pointer, no mem_wrote):
        /// the CPU path finds it all the same within a second of looks.
        @Test func aMissedWriterIsFoundByTheSafetyNet() {
            launchTestApp()
            _ = initToolbox()
            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            let (base, rb, _) = screen()
            var shadow = [UInt8](repeating: 0, count: machine_screen_shadow_size(0, 0, w, h))
            var bands = [Int32](repeating: 0, count: 16)
            func changed() -> [Int32] {
                machine_screen_publish()
                let n = shadow.withUnsafeMutableBufferPointer { s in
                    bands.withUnsafeMutableBufferPointer { machine_screen_changed_rows(0, 0, w, h, s.baseAddress!, s.count, $0.baseAddress!, 8) }
                }
                return Array(bands.prefix(Int(n) * 2))
            }
            #expect(changed() == [0, h])
            #expect(changed() == [])
            gptr(base + 33 * rb)!.pointee ^= 0xFF // unseen
            var found = false
            for _ in 0..<61 where !found { found = changed() == [33, 34] }
            #expect(found)
        }
    }
}
