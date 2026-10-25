// Choosing one of the applications an archive holds (run --app).

import Foundation

/// The applications `wanted` names, given each one's path inside the
/// archive.  The closest kind of match wins: the whole path, then the end
/// of a path ("Folder/App"), then the file's name, then a part of the
/// name, so "Register" picks Register over "Register Cruciverbalist", and
/// "Agenda (Grape)/Register" picks one of several Registers.  Comparisons
/// ignore Unicode normalisation (macOS keeps names decomposed), and case
/// unless that leaves two; a name's spaces at either end count only when
/// they are needed.
public func matchApplications(_ paths: [String], wanted: String) -> [Int] {
    func same(_ a: String, _ b: String) -> Bool {
        a.compare(b, options: [.caseInsensitive]) == .orderedSame
    }
    let trimmed = wanted.trimmingCharacters(in: .whitespaces)
    let names = paths.map { ($0 as NSString).lastPathComponent }
    let parts = paths.map { $0.split(separator: "/", omittingEmptySubsequences: false).map(String.init) }
    let tail = wanted.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
    let kinds: [(Int) -> Bool] = [
        { paths[$0] == wanted },
        { same(paths[$0], wanted) },
        { tail.count > 1 && parts[$0].count >= tail.count && zip(parts[$0].suffix(tail.count), tail).allSatisfy(same) },
        { names[$0] == wanted },
        { same(names[$0], wanted) },
        { same(names[$0].trimmingCharacters(in: .whitespaces), trimmed) },
        { !trimmed.isEmpty && names[$0].range(of: trimmed, options: [.caseInsensitive]) != nil },
    ]
    for kind in kinds {
        let found = paths.indices.filter(kind)
        if !found.isEmpty { return found }
    }
    return []
}
