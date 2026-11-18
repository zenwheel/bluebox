import CCore
import CPU68K
import Foundation
import QuartzCore
import Testing
import Toolbox

extension BlueBox {
    /// The screen on the host's display (present.h): which rows changed,
    /// their conversion on the CPU, and the GPU's drawing of them, which
    /// at 1x must be machine_screen_rgba's pixels exactly.
    @Suite struct Presenting {
        func screenSize() -> (Int32, Int32) {
            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            return (w, h)
        }

        func cpu(_ x: Int32, _ y: Int32, _ w: Int32, _ h: Int32) -> [UInt8] {
            var px = [UInt8](repeating: 0, count: Int(w) * Int(h) * 4)
            machine_screen_rgba(x, y, w, h, &px)
            return px
        }

        func gpu(_ x: Double, _ y: Double, _ w: Double, _ h: Double, _ ow: Int, _ oh: Int,
                 shape: [UInt8]? = nil, desktop: Bool = false) -> [UInt8]? {
            var px = [UInt8](repeating: 0, count: ow * oh * 4)
            let ok = shape.map { s in s.withUnsafeBufferPointer { present_offscreen(x, y, w, h, Int32(ow), Int32(oh), $0.baseAddress, desktop, &px) } }
                ?? present_offscreen(x, y, w, h, Int32(ow), Int32(oh), nil, desktop, &px)
            return ok ? px : nil
        }

        /// Every byte of the screen something, and every colour of the
        /// video hardware's table.
        func scramble(seed: UInt32 = 1) {
            let (_, h) = screenSize()
            let base = rd32(UInt32(LM_ScrnBase)), n = qd_screen_row_bytes() * UInt32(h)
            var v = seed
            for i in 0..<n {
                v = v &* 1_664_525 &+ 1_013_904_223
                wr8(base + i, UInt8(truncatingIfNeeded: v >> 24))
            }
            for i in 0..<256 {
                let rgb: [UInt16] = [UInt16(i * 257), UInt16((255 - i) * 257), UInt16((i * 7 & 255) * 257)]
                rgb.withUnsafeBufferPointer { qd_screen_set_hw_entry(Int32(i), $0.baseAddress!) }
            }
        }

        func paths() -> [String] {
            ["zero-copy", "shared", "managed"].filter { present_force_path($0) }
        }

        // MARK: Changed rows

        @Test func changedRowsAreBands() {
            launchTestApp()
            _ = initToolbox()
            let (w, h) = screenSize()
            var shadow = [UInt8](repeating: 0, count: machine_screen_shadow_size(0, 0, w, h))
            var bands = [Int32](repeating: 0, count: 16)
            func changed() -> [Int32] {
                let n = shadow.withUnsafeMutableBufferPointer { s in
                    bands.withUnsafeMutableBufferPointer { machine_screen_changed_rows(0, 0, w, h, s.baseAddress!, s.count, $0.baseAddress!, 8) }
                }
                return Array(bands.prefix(Int(n) * 2))
            }
            #expect(changed() == [0, h], "the first time, everything")
            #expect(changed() == [])
            let base = rd32(UInt32(LM_ScrnBase)), rb = qd_screen_row_bytes()
            // The Toolbox's own writes.
            wr8(base + 100 * rb + 3, rd8(base + 100 * rb + 3) ^ 1)
            wr8(base + 101 * rb + 9, rd8(base + 101 * rb + 9) ^ 1)
            wr8(base + 300 * rb, rd8(base + 300 * rb) ^ 1)
            #expect(changed() == [100, 102, 300, 301])
            // QuickDraw's.
            let r = tb_NewPtr(8, false, true)
            wr16(r, 50); wr16(r + 2, 10); wr16(r + 4, 60); wr16(r + 6, 40)
            tb_InvertRect(r)
            #expect(changed() == [50, 60])
            // The processor's own stores: MOVE.B #$5A,(A0) and (A0+row).
            let a = base + 200 * rb + 7
            let reason = run68k([0x207C, UInt16(a >> 16), UInt16(a & 0xFFFF), // MOVEA.L #a,A0
                                 0x10BC, 0x005A,                                // MOVE.B #$5A,(A0)
                                 0x117C, 0x005A, UInt16(rb),                    // MOVE.B #$5A,(rb,A0)
                                 0xA9F4])
            #expect(reason.contains("ExitToShell"))
            #expect(changed() == [200, 202])
            // More bands than asked for: the last widens.
            for y in stride(from: 0, to: 40, by: 2) { wr8(base + UInt32(y) * rb, rd8(base + UInt32(y) * rb) ^ 0xFF) }
            #expect(changed() == [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 39])
            // The page shown moved: everything.
            qd_screen_set_display_base(base + rb)
            #expect(changed() == [0, h])
            qd_screen_set_display_base(base)
            #expect(changed() == [0, h])
        }

        @Test func bandsConvertAsTheWholeRect() {
            launchTestApp()
            _ = initToolbox()
            #expect(qd_screen_set_depth(8))
            scramble()
            let x: Int32 = 30, y: Int32 = 40, w: Int32 = 200, h: Int32 = 100
            var shape = [UInt8](repeating: 1, count: Int(w * h))
            for k in stride(from: 0, to: shape.count, by: 7) { shape[k] = 0 }
            var want = cpu(x, y, w, h)
            for k in 0..<shape.count where shape[k] == 0 { want.replaceSubrange(k * 4..<k * 4 + 4, with: [0, 0, 0, 0]) }
            var got = [UInt8](repeating: 0xEE, count: Int(w * h) * 4)
            let bands: [Int32] = [0, 50, 60, 70, 130, 500]
            machine_screen_rgba_bands(x, y, w, h, bands, 3, shape, false, &got)
            for r in 0..<Int(h) {
                let row = Int(y) + r
                let inBand = (row >= 40 && row < 50) || (row >= 60 && row < 70) || (row >= 130 && row < 140)
                let slice = got[r * Int(w) * 4..<(r + 1) * Int(w) * 4]
                if inBand {
                    #expect(Array(slice) == Array(want[r * Int(w) * 4..<(r + 1) * Int(w) * 4]), "row \(row)")
                } else {
                    #expect(slice.allSatisfy { $0 == 0xEE }, "row \(row) untouched")
                }
            }
        }

        // MARK: The GPU's pixels

        @Test(arguments: [Int32(1), 2, 4, 8, 16, 32]) func everyDepthAsTheCPUShowsIt(depth: Int32) throws {
            guard present_start() else { return } // no GPU here: the CPU path
            launchTestApp()
            _ = initToolbox()
            #expect(qd_screen_set_depth(depth))
            scramble(seed: UInt32(depth))
            let (w, h) = screenSize()
            for p in paths() {
                #expect(present_force_path(p))
                #expect(gpu(0, 0, Double(w), Double(h), Int(w), Int(h)) == cpu(0, 0, w, h), "\(p) at \(depth) bits")
                #expect(String(cString: present_path()) == p, "no fallback")
                // A part, and one reaching off the screen (white there).
                #expect(gpu(100, 50, 300, 200, 300, 200) == cpu(100, 50, 300, 200), "\(p) part")
                #expect(gpu(Double(w) - 20, Double(h) - 10, 40, 30, 40, 30) == cpu(w - 20, h - 10, 40, 30), "\(p) edge")
            }
            #expect(present_force_path("zero-copy") || present_force_path("shared"))
        }

        @Test func gammaFadesBrightnessAndMasks() throws {
            guard present_start() else { return }
            launchTestApp()
            _ = initToolbox()
            #expect(qd_screen_set_depth(8))
            scramble(seed: 9)
            let (w, h) = screenSize()
            video_fade(40, 0)
            qd_screen_set_brightness(100)
            defer {
                video_fade(100, 0)
                qd_screen_set_brightness(255)
            }
            #expect(gpu(0, 0, Double(w), Double(h), Int(w), Int(h)) == cpu(0, 0, w, h))
            // A window's shape and the desktop left out.
            let x: Int32 = 64, y: Int32 = 30, sw: Int32 = 128, sh: Int32 = 90
            var shape = [UInt8](repeating: 1, count: Int(sw * sh))
            for k in 0..<shape.count where (k % 13) < 3 { shape[k] = 0 }
            var want = [UInt8](repeating: 0, count: Int(sw * sh) * 4)
            let all: [Int32] = [0, h]
            machine_screen_rgba_bands(x, y, sw, sh, all, 1, shape, true, &want)
            #expect(gpu(Double(x), Double(y), Double(sw), Double(sh), Int(sw), Int(sh), shape: shape, desktop: true) == want)
        }

        @Test func aCompactMacsScreenOffItsPage() throws {
            guard present_start() else { return }
            withMachine("plus") {
                launchTestApp()
                _ = initToolbox()
                scramble(seed: 3)
                let (w, h) = screenSize()
                let base = rd32(UInt32(LM_ScrnBase))
                #expect(base & 0x3FFF != 0, "the test wants a screen inside a page")
                for p in paths() {
                    #expect(present_force_path(p))
                    #expect(gpu(0, 0, Double(w), Double(h), Int(w), Int(h)) == cpu(0, 0, w, h), "\(p)")
                    #expect(String(cString: present_path()) == p, "no fallback")
                    // The alternate page, $8000 below.
                    qd_screen_set_display_base(base - 0x8000)
                    #expect(gpu(0, 0, Double(w), Double(h), Int(w), Int(h)) == cpu(0, 0, w, h), "\(p) alternate")
                    qd_screen_set_display_base(base)
                }
            }
        }

        @Test func aMacIIsSlotScreen() throws {
            guard present_start() else { return }
            withMachine("macii") {
                launchTestApp()
                _ = initToolbox()
                scramble(seed: 4)
                let (w, h) = screenSize()
                for p in paths() {
                    #expect(present_force_path(p))
                    #expect(gpu(0, 0, Double(w), Double(h), Int(w), Int(h)) == cpu(0, 0, w, h), "\(p)")
                    #expect(String(cString: present_path()) == p, "no fallback")
                }
            }
        }

        /// 1, 2 and 3 are whole pixels; 1.5 keeps every pixel's middle its
        /// own colour, blending only at the edges.
        @Test(arguments: [1.0, 1.5, 2.0, 3.0]) func scales(scale: Double) throws {
            guard present_start() else { return }
            launchTestApp()
            _ = initToolbox()
            #expect(qd_screen_set_depth(32))
            scramble(seed: 5)
            let sw: Int32 = 64, sh: Int32 = 48
            let ow = Int(Double(sw) * scale), oh = Int(Double(sh) * scale)
            let src = cpu(10, 10, sw, sh)
            guard let out = gpu(10, 10, Double(sw), Double(sh), ow, oh) else {
                Issue.record("no picture")
                return
            }
            let r = 0.5 - 0.5 / scale
            var exact = 0, blended = 0
            for oy in 0..<oh {
                for ox in 0..<ow {
                    let tx = (Double(ox) + 0.5) / scale, ty = (Double(oy) + 0.5) / scale
                    let fx = tx - tx.rounded(.down) - 0.5, fy = ty - ty.rounded(.down) - 0.5
                    let sx = Int(tx), sy = Int(ty)
                    let o = (oy * ow + ox) * 4
                    let got = Array(out[o..<o + 4])
                    if abs(fx) <= r + 1e-6 && abs(fy) <= r + 1e-6 {
                        let s = (sy * Int(sw) + sx) * 4
                        #expect(got == Array(src[s..<s + 4]), "at \(ox),\(oy) of \(scale)x")
                        exact += 1
                    } else {
                        // Between its neighbours'.
                        var lo = [255, 255, 255], hi = [0, 0, 0]
                        for ny in max(sy - 1, 0)...min(sy + 1, Int(sh) - 1) {
                            for nx in max(sx - 1, 0)...min(sx + 1, Int(sw) - 1) {
                                for k in 0..<3 {
                                    let v = Int(src[(ny * Int(sw) + nx) * 4 + k])
                                    lo[k] = min(lo[k], v)
                                    hi[k] = max(hi[k], v)
                                }
                            }
                        }
                        for k in 0..<3 { #expect(Int(got[k]) >= lo[k] - 1 && Int(got[k]) <= hi[k] + 1) }
                        blended += 1
                    }
                }
            }
            if scale == scale.rounded() { #expect(blended == 0) }
            #expect(exact > ow * oh / 4)
        }

        // MARK: Guest memory moving

        /// The arena's pages mapped again (mem_discard over the screen) while
        /// the GPU reads it in place: the read is waited for and the buffer
        /// made again, never left reading pages gone.
        @Test func memoryRemappedWhilePresenting() throws {
            guard present_start(), present_force_path("zero-copy") else { return }
            defer { _ = present_force_path("zero-copy") }
            launchTestApp()
            _ = initToolbox()
            #expect(qd_screen_set_depth(32))
            let (w, h) = screenSize()
            let base = rd32(UInt32(LM_ScrnBase)), len = qd_screen_row_bytes() * UInt32(h)
            final class Flag: @unchecked Sendable { var stop = false; var frames = 0 }
            let flag = Flag()
            let done = DispatchSemaphore(value: 0)
            Thread.detachNewThread {
                var px = [UInt8](repeating: 0, count: 256 * 256 * 4)
                while !flag.stop {
                    _ = present_offscreen(0, 0, 256, 256, 256, 256, nil, false, &px)
                    flag.frames += 1
                    usleep(500) // a refresh's pace, not a tight loop holding the lock
                }
                done.signal()
            }
            // Discards while frames are being drawn, until enough of each.
            var i = 0
            let start = present_now_ns()
            while (i < 40 || flag.frames < 20) && present_now_ns() - start < 10_000_000_000 {
                mem_discard(base, len)
                wr32(base + UInt32(i % 256) * 4, 0x00FF_00FF)
                i += 1
                usleep(200)
            }
            flag.stop = true
            done.wait()
            #expect(flag.frames > 0)
            scramble(seed: 11)
            #expect(gpu(0, 0, Double(w), Double(h), Int(w), Int(h)) == cpu(0, 0, w, h))
        }

        // MARK: Frame pacing on the refreshes

        /// A shown surface and refreshes coming: pacing goes by them, a wait
        /// ends when the refresh has read the screen; with the refreshes
        /// stopped it ends anyway, on the wall clock.
        @Test func pacingWaitsForTheRefreshAndNeverForever() throws {
            guard present_start() else { return }
            launchTestApp()
            _ = initToolbox()
            let layer = CAMetalLayer()
            let s = present_surface_new(Unmanaged.passUnretained(layer).toOpaque())
            defer { present_surface_free(s) }
            present_surface_set(s, 0, 0, 64, 64, 64, 64)
            for _ in 0..<5 {
                present_vsync(60)
                usleep(5000)
            }
            #expect(present_live())
            final class Flag: @unchecked Sendable { var stop = false }
            let flag = Flag()
            let ticker = DispatchSemaphore(value: 0)
            Thread.detachNewThread {
                while !flag.stop {
                    present_vsync(60)
                    usleep(16_000)
                }
                ticker.signal()
            }
            let seq = present_vsync_seq()
            let t0 = present_now_ns()
            _ = machine_pace_wait_vsync(seq + 1)
            let waited = present_now_ns() - t0
            #expect(present_read_done(seq + 1))
            #expect(waited < 40_000_000, "waited \(waited) ns")
            flag.stop = true
            ticker.wait()
            usleep(60_000)
            #expect(!present_live(), "no refreshes: the wall clock")
            let t1 = present_now_ns()
            _ = machine_pace_wait_vsync(present_vsync_seq() + 5)
            #expect(present_now_ns() - t1 < 40_000_000)
            // Hidden: not live either.
            present_vsync(60)
            present_surface_set_visible(s, false)
            present_vsync(60)
            usleep(20_000)
            #expect(!present_live())
        }
    }
}

extension BlueBox {
    /// BLUEBOX_PRESENT_BENCH=1: the presenting paths timed per frame, at
    /// the sizes of a few displays (printed; nothing checked).
    @Suite struct PresentBench {
        @Test func timings() throws {
            guard ProcessInfo.processInfo.environment["BLUEBOX_PRESENT_BENCH"] == "1" else { return }
            defer { machine_set_screen(1024, 768) }
            func ms(_ t0: UInt64) -> Double { Double(present_now_ns() - t0) / 1e6 }
            func median(_ a: [Double]) -> Double { a.sorted()[a.count / 2] }
            print("size        depth  scan-same  cpu-frame(all rows)  cpu-frame(16 rows)  gpu-read(all)  gpu-read(16 rows)")
            for (w, h, d) in [(1024, 768, 8), (1024, 768, 32), (1920, 1080, 32), (2560, 1440, 32), (3008, 1692, 32)] {
                machine_set_screen(Int32(w), Int32(h))
                launchTestApp()
                _ = initToolbox()
                #expect(qd_screen_set_depth(Int32(d)))
                let base = rd32(UInt32(LM_ScrnBase)), rb = qd_screen_row_bytes()
                var shadow = [UInt8](repeating: 0, count: machine_screen_shadow_size(0, 0, Int32(w), Int32(h)))
                var bands = [Int32](repeating: 0, count: 64)
                var rgba = [UInt8](repeating: 0, count: w * h * 4)
                func scan() -> Int32 {
                    shadow.withUnsafeMutableBufferPointer { s in
                        bands.withUnsafeMutableBufferPointer {
                            machine_screen_changed_rows(0, 0, Int32(w), Int32(h), s.baseAddress!, s.count, $0.baseAddress!, 32)
                        }
                    }
                }
                func touch(_ rows: Int, _ f: Int) {
                    g_cpu!.pointee.icount += 1 // as the processor's own drawing moves it on
                    for r in 0..<rows { wr8(base + UInt32((r * h / rows + f) % h) * rb + UInt32(f % Int(rb)), UInt8(truncatingIfNeeded: f &+ r)) }
                }
                func cpuFrame() {
                    let n = scan()
                    rgba.withUnsafeMutableBytes { raw in
                        machine_screen_rgba_bands(0, 0, Int32(w), Int32(h), bands, n, nil, false, raw.baseAddress!.assumingMemoryBound(to: UInt8.self))
                    }
                    _ = rgba.withUnsafeBytes { Data($0) } // the image's own copy
                }
                _ = scan()
                var same = [Double](), cpuAll = [Double](), cpuFew = [Double]()
                for f in 0..<30 {
                    var t = present_now_ns(); _ = scan(); same.append(ms(t))
                    touch(h, f); t = present_now_ns(); cpuFrame(); cpuAll.append(ms(t))
                    touch(16, f); t = present_now_ns(); cpuFrame(); cpuFew.append(ms(t))
                }
                // The presenter's refresh with a layer: changed rows, then the
                // GPU's read done (what a waiting program is held for).
                var gpuAll = [Double](), gpuFew = [Double]()
                if present_start() {
                    let layer = CAMetalLayer()
                    let s = present_surface_new(Unmanaged.passUnretained(layer).toOpaque())
                    present_surface_set(s, 0, 0, Double(w), Double(h), Int32(w * 2), Int32(h * 2))
                    func refresh() -> Double {
                        let seq = present_vsync_seq() + 1
                        let t = present_now_ns()
                        present_vsync(60)
                        while !present_read_done(seq) { usleep(50) }
                        return ms(t)
                    }
                    _ = refresh()
                    for f in 0..<30 {
                        touch(h, f); gpuAll.append(refresh())
                        usleep(16000)
                        touch(16, f); gpuFew.append(refresh())
                        usleep(16000)
                    }
                    present_surface_free(s)
                }
                print(String(format: "%4dx%-4d  %2d   %7.3f    %7.3f              %7.3f             %7.3f        %7.3f ms  (%@)",
                             w, h, d, median(same), median(cpuAll), median(cpuFew),
                             gpuAll.isEmpty ? 0 : median(gpuAll), gpuFew.isEmpty ? 0 : median(gpuFew),
                             String(cString: present_path())))
            }
        }
    }
}
