import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The idle watch's record of an iteration's stores (idle.c's
    /// store_place): the places stored to, in order, with their lengths and
    /// what they held before; past 256 places, stretches of memory; past 8
    /// stretches, an overflow.  The record decides whether an iteration
    /// changed anything, so however it is kept it must come out the same:
    /// each pattern is recorded by idle.c and by the plain model here (the
    /// places searched one by one, as idle.c did) and the two compared.
    @Suite(.serialized) struct IdleStores {
        static let mostPlaces = 256, maxRanges = 8, rangeFrom = 17, rangeMost: UInt32 = 65536

        /// idle.c's record, as it was kept before the places were indexed.
        struct Model {
            struct Place { var a: UInt32, len: UInt32, had: Bool, before: [UInt8] }
            var places: [Place] = []
            var overflow = false
            var ranges: [(a: UInt32, len: UInt32)] = []
            var rangesOverflow = false

            mutating func place(_ a: UInt32, _ length: UInt32, before: Bool) {
                if overflow { return }
                let len = min(length, 4)
                if let i = places.firstIndex(where: { $0.a == a }) {
                    places[i].len = max(places[i].len, len)
                    return
                }
                if places.count == IdleStores.mostPlaces {
                    run(a, len)
                    return
                }
                places.append(Place(a: a, len: len, had: before,
                                    before: before ? (0..<4).map { rd8(a &+ UInt32($0)) } : []))
            }

            mutating func run(_ a: UInt32, _ len: UInt32) {
                for i in ranges.indices {
                    let r = ranges[i]
                    if a &+ len < r.a || a > r.a &+ r.len { continue }
                    let lo = min(a, r.a), hi = max(a &+ len, r.a &+ r.len)
                    if hi - lo > IdleStores.rangeMost { break }
                    ranges[i] = (lo, hi - lo)
                    return
                }
                if ranges.count == IdleStores.maxRanges { overflow = true } else { ranges.append((a, len)) }
            }

            mutating func wrote(_ a: UInt32, _ len: UInt32) {
                if a == 0 { return }
                if len < UInt32(IdleStores.rangeFrom) {
                    for k in stride(from: UInt32(0), to: len, by: 4) { place(a + k, min(4, len - k), before: false) }
                    return
                }
                if ranges.contains(where: { $0.a == a && $0.len == len }) { return }
                if ranges.count == IdleStores.maxRanges || len > IdleStores.rangeMost {
                    rangesOverflow = true
                } else {
                    ranges.append((a, len))
                }
            }

            func hash() -> UInt64 {
                var h: UInt64 = 0xCBF2_9CE4_8422_2325
                func mix(_ v: UInt64) { h = (h ^ v) &* 0x1_0000_0001_B3 }
                mix(UInt64(places.count))
                for p in places {
                    mix(UInt64(p.a))
                    mix(UInt64(p.len))
                    mix(p.had ? 1 : 0)
                    for b in p.before { mix(UInt64(b)) }
                }
                mix(overflow ? 1 : 0)
                mix(UInt64(ranges.count))
                mix(rangesOverflow ? 1 : 0)
                for r in ranges {
                    mix(UInt64(r.a))
                    mix(UInt64(r.len))
                    mix(0)
                }
                mix(0) // nothing drawn
                mix(0)
                return h
            }
        }

        /// A block of guest memory, its bytes set, for the stores to land in.
        func block(_ size: Int) -> UInt32 {
            launchTestApp()
            let b = tb_NewPtr(Int32(size), false, true)
            #expect(b != 0)
            for i in 0..<size { wr8(b + UInt32(i), UInt8((i * 31 + 7) & 0xFF)) }
            return b
        }

        /// The pattern recorded by idle.c and by the model: the same hash.
        @discardableResult
        func same(_ addrs: [UInt32], _ ops: [UInt8], reps: Int = 1) -> Model {
            var m = Model()
            for _ in 0..<reps {
                for (a, op) in zip(addrs, ops) {
                    if op & 0x80 != 0 { m.wrote(a, UInt32(op & 0x7F)) } else { m.place(a, UInt32(op), before: true) }
                }
            }
            let got = idle_test_store_pattern(addrs, ops, Int32(addrs.count), Int32(reps))
            #expect(got == m.hash(), "\(m.places.count) places, \(m.ranges.count) stretches, overflow \(m.overflow)")
            return m
        }

        /// A deterministic sequence (an LCG), for the mixed pattern.
        struct Rand {
            var s: UInt32
            mutating func next(_ n: Int) -> Int {
                s = s &* 1_664_525 &+ 1_013_904_223
                return Int(s >> 8) % n
            }
        }

        /// A loop's own variables stored to again and again, in varying
        /// widths, with calls' answers among them (a rectangle set, a point
        /// converted, a stretch BlockMoved).
        @Test func variablesStoredAgain() {
            let b = block(4096)
            var addrs: [UInt32] = [], ops: [UInt8] = []
            for i in 0..<40 {
                addrs.append(b + UInt32(i * 6))
                ops.append([1, 2, 4][i % 3])
            }
            addrs += [b + 512, b + 520, b + 1024, b + 2, b + 0]
            ops += [0x80 | 8, 0x80 | 4, 0x80 | 32, 4, 1]
            let m = same(addrs, ops, reps: 5)
            #expect(m.places.count == 44 && m.ranges.count == 1 && !m.overflow)
        }

        /// Past the places' room: a buffer filled a long at a time (a
        /// program's own pixels, PixelToy's) taken as stretches.
        @Test func aBufferFilledPastThePlaces() {
            let b = block(160 * 1024)
            var addrs: [UInt32] = [], ops: [UInt8] = []
            for i in 0..<200 {
                addrs.append(b + UInt32(i * 52 + 1))
                ops.append(2)
            }
            for i in stride(from: 0, to: 48 * 1024, by: 4) {
                addrs.append(b + 16384 + UInt32(i))
                ops.append(4)
            }
            let m = same(addrs, ops, reps: 2)
            #expect(m.places.count == 256 && m.ranges.count == 1 && !m.overflow)
        }

        /// Past the stretches' room too: the record overflows (work).
        @Test func scatteredPastTheStretches() {
            let b = block(160 * 1024)
            var addrs: [UInt32] = [], ops: [UInt8] = []
            for i in 0..<270 {
                addrs.append(b + UInt32(i * 512 + 3))
                ops.append(1)
            }
            let m = same(addrs, ops)
            #expect(m.overflow)
        }

        /// A mixed pattern, clustered as a program's variables are.
        @Test func aMixedPattern() {
            let b = block(64 * 1024)
            var r = Rand(s: 1993)
            var addrs: [UInt32] = [], ops: [UInt8] = []
            let clusters = (0..<24).map { _ in UInt32(r.next(60 * 1024)) }
            for _ in 0..<6000 {
                let a = b + clusters[r.next(clusters.count)] + UInt32(r.next(48))
                let k = r.next(20)
                addrs.append(a)
                ops.append(k == 0 ? 0x80 | UInt8(1 + r.next(24)) : [1, 2, 4][k % 3])
            }
            let m = same(addrs, ops, reps: 2)
            #expect(m.places.count == 256)
        }

        /// What recording costs a store, for the profile's pattern: a
        /// frame's variables and a buffer filled (BLUEBOX_IDLE_BENCH=1 runs
        /// it longer and prints the time).
        @Test func recordingCost() {
            let b = block(160 * 1024)
            var addrs: [UInt32] = [], ops: [UInt8] = []
            for i in 0..<200 {
                addrs.append(b + UInt32(i * 8))
                ops.append(4)
            }
            for i in stride(from: 0, to: 60 * 1024, by: 4) {
                addrs.append(b + 8192 + UInt32(i))
                ops.append(4)
                if i % 64 == 0 { // the loop's own variables between
                    addrs.append(b + UInt32((i / 64) % 200 * 8))
                    ops.append(4)
                }
            }
            let bench = ProcessInfo.processInfo.environment["BLUEBOX_IDLE_BENCH"] == "1"
            let reps = bench ? 40 : 2
            let t0 = DispatchTime.now().uptimeNanoseconds
            _ = idle_test_store_pattern(addrs, ops, Int32(addrs.count), Int32(reps))
            let ns = Double(DispatchTime.now().uptimeNanoseconds - t0) / Double(addrs.count * reps)
            if bench { print("idle store record: \(String(format: "%.1f", ns)) ns a store over \(addrs.count * reps)") }
            same(addrs, ops)
        }
    }
}
