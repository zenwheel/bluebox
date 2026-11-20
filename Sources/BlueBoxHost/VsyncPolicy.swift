// The GPU presenter's refreshes in rootless mode: which window's display
// link they follow, and the host's own 60 Hz tick standing in when none
// come.  Kept free of AppKit so the choices can be tested without windows
// (the bluebox target's Vsync acts on them).

/// A window the refreshes could follow.
public struct VsyncCandidate<Item> {
    public let item: Item
    /// Ordered in (on screen, perhaps covered).
    public let ordered: Bool
    /// Some of it seen: a covered window's display link may pause.
    public let seen: Bool
    public let key: Bool

    public init(_ item: Item, ordered: Bool, seen: Bool, key: Bool) {
        self.item = item
        self.ordered = ordered
        self.seen = seen && ordered
        self.key = key
    }
}

public enum VsyncTarget {
    /// The whole screen's window when there is one; otherwise the key
    /// window, else the frontmost (windows given front to back).  A window
    /// seen goes before one only ordered in; nil when none is ordered in
    /// (the refreshes then come from the main display).
    public static func choose<Item>(wholeScreen: VsyncCandidate<Item>?, frontToBack: [VsyncCandidate<Item>]) -> Item? {
        let all = (wholeScreen.map { [$0] } ?? []) + frontToBack
        for pass in [\VsyncCandidate<Item>.seen, \.ordered] {
            if let w = wholeScreen, w[keyPath: pass] { return w.item }
            if let k = all.first(where: { $0.key && $0[keyPath: pass] }) { return k.item }
            if let f = all.first(where: { $0[keyPath: pass] }) { return f.item }
        }
        return nil
    }
}

/// The host's tick standing in for a display link that has gone quiet:
/// after `quiet` nanoseconds with no real refresh while a surface is seen,
/// each tick drives the presenter, and the first real refresh stops it.
/// The quiet time counts only while a surface is seen, so a link paused
/// over a covered window has that long to start again.
public struct VsyncFallback {
    public static let quietNs: UInt64 = 100_000_000

    public private(set) var driving = false
    private var realSeen: UInt64 = 0
    private var quietSince: UInt64?

    public init() {}

    /// One host tick at `now` (ns, monotonic), `real` the real refreshes
    /// counted so far: whether this tick drives the presenter.
    public mutating func tick(now: UInt64, real: UInt64, surfaceSeen: Bool) -> Bool {
        if real != realSeen || quietSince == nil || !surfaceSeen {
            realSeen = real
            quietSince = now
            driving = false
            return false
        }
        if !driving, now &- quietSince! >= Self.quietNs { driving = true }
        return driving
    }
}
