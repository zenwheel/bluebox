import BlueBoxHost
import Testing

/// Rootless mode's refreshes: the window whose display link they follow,
/// and the host tick standing in for a quiet link.
@Suite struct VsyncPolicy {
    typealias C = VsyncCandidate<String>

    static func w(_ name: String, ordered: Bool = true, seen: Bool = true, key: Bool = false) -> C {
        C(name, ordered: ordered, seen: seen, key: key)
    }

    @Test func wholeScreenFirst() {
        let pick = VsyncTarget.choose(wholeScreen: Self.w("screen"), frontToBack: [Self.w("a", key: true)])
        #expect(pick == "screen")
    }

    @Test func keyThenFrontmost() {
        #expect(VsyncTarget.choose(wholeScreen: nil, frontToBack: [Self.w("a"), Self.w("b", key: true)]) == "b")
        #expect(VsyncTarget.choose(wholeScreen: nil, frontToBack: [Self.w("a"), Self.w("b")]) == "a")
    }

    @Test func closedOrHiddenWindowsLeftOut() {
        // The key window closed (ordered out): the next one, front to back.
        let pick = VsyncTarget.choose(wholeScreen: nil,
                                      frontToBack: [Self.w("a", ordered: false, key: true), Self.w("b"), Self.w("c")])
        #expect(pick == "b")
        // A whole screen's window hidden (the app hidden) and nothing else: none.
        #expect(VsyncTarget.choose(wholeScreen: Self.w("screen", ordered: false), frontToBack: [C]()) == nil)
        #expect(VsyncTarget.choose(wholeScreen: nil, frontToBack: [C]()) == nil)
    }

    @Test func seenBeforeCovered() {
        // The key window wholly covered: a seen one's link keeps running.
        let pick = VsyncTarget.choose(wholeScreen: nil, frontToBack: [Self.w("a", seen: false, key: true), Self.w("b")])
        #expect(pick == "b")
        // All covered: still a program window, the key one.
        let covered = VsyncTarget.choose(wholeScreen: nil,
                                         frontToBack: [Self.w("a", seen: false), Self.w("b", seen: false, key: true)])
        #expect(covered == "b")
        // Ordered out is never seen, whatever the occlusion says.
        #expect(!Self.w("x", ordered: false, seen: true).seen)
    }

    static let ms: UInt64 = 1_000_000

    @Test func standsInAfterQuiet() {
        var f = VsyncFallback()
        var t: UInt64 = 1000 * Self.ms
        var drives = 0
        for _ in 0..<5 { // 5 ticks, 67 ms: not yet
            if f.tick(now: t, real: 0, surfaceSeen: true) { drives += 1 }
            t += 16 * Self.ms + 666_667
        }
        #expect(drives == 0)
        for _ in 0..<10 {
            if f.tick(now: t, real: 0, surfaceSeen: true) { drives += 1 }
            t += 16 * Self.ms + 666_667
        }
        #expect(drives >= 8)
        #expect(f.driving)
    }

    @Test func realRefreshesStopIt() {
        var f = VsyncFallback()
        _ = f.tick(now: 0, real: 0, surfaceSeen: true)
        do { let d = f.tick(now: 200 * Self.ms, real: 0, surfaceSeen: true); #expect(d) }
        // The link's first refresh: this tick does not drive, nor later ones while they come.
        do { let d = f.tick(now: 217 * Self.ms, real: 1, surfaceSeen: true); #expect(!d) }
        #expect(!f.driving)
        var real: UInt64 = 1
        for i in 0..<20 {
            real += 1
            do { let d = f.tick(now: (233 + 17 * UInt64(i)) * Self.ms, real: real, surfaceSeen: true); #expect(!d) }
        }
        // Quiet again: 100 ms from the last refresh (the last tick that saw one, 556 ms), not before.
        do { let d = f.tick(now: 600 * Self.ms, real: real, surfaceSeen: true); #expect(!d) }
        do { let d = f.tick(now: 655 * Self.ms, real: real, surfaceSeen: true); #expect(!d) }
        do { let d = f.tick(now: 657 * Self.ms, real: real, surfaceSeen: true); #expect(d) }
    }

    @Test func nothingSeenNothingDriven() {
        var f = VsyncFallback()
        for i in 0..<30 {
            do { let d = f.tick(now: UInt64(i) * 17 * Self.ms, real: 0, surfaceSeen: false); #expect(!d) }
        }
        // Seen again: the link gets the quiet time to start before the tick drives.
        // (the last tick unseen was at 493 ms)
        do { let d = f.tick(now: 510 * Self.ms, real: 0, surfaceSeen: true); #expect(!d) }
        do { let d = f.tick(now: 590 * Self.ms, real: 0, surfaceSeen: true); #expect(!d) }
        do { let d = f.tick(now: 594 * Self.ms, real: 0, surfaceSeen: true); #expect(d) }
        // Hidden while driving: stops at once.
        do { let d = f.tick(now: 611 * Self.ms, real: 0, surfaceSeen: false); #expect(!d) }
        #expect(!f.driving)
    }
}
