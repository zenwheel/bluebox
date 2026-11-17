// Type-to-select, as the Finder has it: letters typed in quick succession
// make a name, and the item chosen is the first whose name, in the
// Finder's order (localizedStandardCompare: case and digits as the Finder
// sorts them), comes at or after what was typed, so "dr" finds "Dropbox"
// and "zz" the last item.  A pause longer than the timeout starts again.

import Foundation

public struct TypeSelect {
    public var timeout: TimeInterval
    private(set) public var typed = ""
    private var last: TimeInterval = -.infinity

    public init(timeout: TimeInterval = 1.0) { self.timeout = timeout }

    /// Characters typed at `time` (seconds, any clock): the name so far,
    /// or nil when they aren't for type-select (control characters, or a
    /// space that would start a name).
    public mutating func feed(_ characters: String, at time: TimeInterval) -> String? {
        let printable = characters.unicodeScalars.allSatisfy {
            !CharacterSet.controlCharacters.contains($0) && !(0xF700...0xF8FF).contains($0.value) // function keys
        }
        guard !characters.isEmpty, printable else { return nil }
        if time - last > timeout { typed = "" }
        if typed.isEmpty && characters.allSatisfy({ $0 == " " }) { return nil }
        typed += characters
        last = time
        return typed
    }

    /// Whether a name is being typed at `time`: a space then is part of
    /// it, not the space bar's own (Quick Look).
    public func isTyping(at time: TimeInterval) -> Bool { !typed.isEmpty && time - last <= timeout }

    public mutating func reset() {
        typed = ""
        last = -.infinity
    }

    /// The index in `names` to select for `typed`: the first, in the
    /// Finder's order, whose name starts with it, else the first that
    /// comes after it, else the last.
    public static func match(_ typed: String, in names: [String]) -> Int? {
        guard !typed.isEmpty, !names.isEmpty else { return nil }
        let opts: String.CompareOptions = [.caseInsensitive, .diacriticInsensitive]
        let order = names.indices.sorted { names[$0].localizedStandardCompare(names[$1]) == .orderedAscending }
        if let i = order.first(where: { names[$0].range(of: typed, options: opts.union(.anchored)) != nil }) { return i }
        if let i = order.first(where: { names[$0].localizedStandardCompare(typed) != .orderedAscending }) { return i }
        return order.last
    }
}
