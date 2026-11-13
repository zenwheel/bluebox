// Hibernated programs, as `bluebox run`, the shell and a program's own
// process all see them: a snapshot file (hibernate.c's, with the run's
// launch record in its head, a thumbnail of the screen among it) and an
// entry in the registry of running programs with the state hibernated,
// which outlives the process.  Each hibernation adds a snapshot and an
// entry; resuming one keeps both, so a program can go back to any of its
// snapshots until it is deleted (to the Trash).  A program keeps its newest
// few (HibernatedSnapshots.tidy, after each hibernation).
//
// A save point (SavePoint.swift) is a snapshot too, made while the program
// goes on running, and marked so in its record (kind "savepoint") and its
// entry: the picker offers it with the hibernated ones, saying which is
// which, and a program keeps its newest few save points apart from its
// hibernated snapshots (a burst of quick saves never pushes out where it
// was left when it hibernated, nor the other way round).
//
// What opening a hibernated program does (ResumePolicy): an interactive
// run of it (a double-click in the shell, its bundle, the Dock, an exported
// application, a login item, `bluebox run` from a terminal) shows its
// snapshots (SnapshotPicker) to resume one, start fresh or delete some;
// --fresh starts it fresh and --new starts another copy, both leaving the
// snapshots alone.  A headless run never resumes unless asked (--resume
// takes the newest, --resume FILE that one), so a census or a menu walk of
// a program the user hibernated leaves it be.  A snapshot resumes in any
// build of Blue Box whose saved state has the same shape (hibernate.c's
// state fingerprint), not only the one that saved it; one that does not
// fit this build is never resumed: the picker says why, and it can be
// deleted there.

import Foundation
import Toolbox

/// What a snapshot file says of the run that made it (its uncompressed
/// head), enough to boot a machine the same way again.
public struct HibernateRecord: Codable, Equatable, Sendable {
    public var version = 1
    /// The run's arguments, the application first, without the ones that
    /// belong to that run alone.
    public var args: [String]
    public var program: String
    public var title: String
    public var saved: Date
    public var instructions: UInt64
    public var drainedAt: String
    /// The Mac screen the machine was booted with (width, height) and its
    /// depth: a resumed machine is booted with the same, whatever the
    /// display it resumes on (a rootless run's screen is the display's).
    public var screen: [Int32]?
    public var depth: Int32?
    /// How it was shown: rootless, screen, background or headless.
    public var display: String?
    /// The Mac it was presented as ("Macintosh Quadra 800").
    public var machine: String?
    /// The screen when it hibernated, small (a PNG at most
    /// SnapshotThumbnail.width pixels wide), for choosing between
    /// snapshots.  Older snapshots have none.
    public var thumbnail: Data?
    /// What made it: nil for Hibernate, "savepoint" for a save point (the
    /// program went on running).
    public var kind: String?

    public static let savePointKind = "savepoint"
    public var isSavePoint: Bool { kind == Self.savePointKind }

    public init(args: [String], program: String, title: String, saved: Date, instructions: UInt64, drainedAt: String,
                screen: [Int32]? = nil, depth: Int32? = nil, display: String? = nil, machine: String? = nil,
                thumbnail: Data? = nil, kind: String? = nil) {
        self.args = args
        self.program = program
        self.title = title
        self.saved = saved
        self.instructions = instructions
        self.drainedAt = drainedAt
        self.screen = screen
        self.depth = depth
        self.display = display
        self.machine = machine
        self.thumbnail = thumbnail
        self.kind = kind
    }

    public var json: String {
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .iso8601
        enc.outputFormatting = [.sortedKeys]
        return String(decoding: (try? enc.encode(self)) ?? Data(), as: UTF8.self)
    }

    public static func decode(_ data: Data) -> HibernateRecord? {
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        return try? dec.decode(HibernateRecord.self, from: data)
    }
}

/// Why a snapshot cannot be resumed here.
public struct SnapshotProblem: Error, Equatable, Sendable {
    public var message: String
    public init(_ message: String) { self.message = message }

    /// The words for a snapshot this build cannot resume, when hibernate.c
    /// gives none: its saved state is laid out otherwise.
    public static let otherBuild = "it was saved by a build of Blue Box whose saved state differs from this one's"
}

public enum SnapshotFile {
    /// A snapshot file's launch record, and whether this build can resume
    /// it (the build that saved it, or one whose saved state has the same
    /// shape), and if not, why.
    public static func read(_ path: String)
        -> Result<(record: HibernateRecord, resumable: Bool, why: String), SnapshotProblem> {
        var extra: UnsafeMutablePointer<UInt8>?
        var len = 0
        var fits = false
        var why = [CChar](repeating: 0, count: 768)
        var err = [CChar](repeating: 0, count: 512)
        func text(_ c: [CChar]) -> String { String(decoding: c.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self) }
        guard hb_file_extra(path, &extra, &len, &fits, &why, why.count, &err, err.count), let extra else {
            return .failure(SnapshotProblem(text(err)))
        }
        defer { hb_free_buffer(extra) }
        guard let record = HibernateRecord.decode(Data(bytes: extra, count: len)) else {
            return .failure(SnapshotProblem("\(path): the snapshot has no launch record Blue Box reads"))
        }
        let reason = text(why)
        return .success((record, fits, fits ? "" : reason.isEmpty ? SnapshotProblem.otherBuild : reason))
    }

    /// What resuming it here would meet: nil when it can be resumed.
    public static func problem(_ path: String) -> SnapshotStatus {
        guard FileManager.default.fileExists(atPath: path) else { return .missing }
        switch read(path) {
        case .success(let r): return r.resumable ? .resumable : .cannotResume(r.why)
        case .failure(let p): return .cannotResume(p.message)
        }
    }

    /// "Hibernated 2026-11-06 01:23" (or "Save point ..."), in the host's
    /// time zone.
    public static func label(_ date: Date, savePoint: Bool = false, timeZone: TimeZone = .current) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = timeZone
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return (savePoint ? "Save point " : "Hibernated ") + f.string(from: date)
    }
}

/// A snapshot as a run about to resume it finds it.
public enum SnapshotStatus: Equatable, Sendable {
    case resumable
    /// Its file is gone (moved, deleted): the entry is forgotten.
    case missing
    /// Saved by a build whose saved state differs, or unreadable: the user
    /// decides.
    case cannotResume(String)
}

/// What a run of a program does about its hibernated snapshots.
public enum ResumeChoice: Equatable, Sendable {
    /// Launched fresh (nothing hibernated, --fresh or --new, or a run that
    /// never resumes); the snapshots are kept.
    case launch
    case resume(snapshot: String)
    /// The snapshot's file is gone: a headless run says so (its entry
    /// forgotten; one named with --resume FILE ends the run).
    case missing(snapshot: String)
    /// It cannot resume here (its saved state does not fit this build, or it
    /// is unreadable): a headless
    /// run says so and ends.
    case ask(snapshot: String, reason: String)
    /// An interactive run: the user picks one of these (newest first) or
    /// starts fresh (SnapshotPicker).
    case pick([String])
}

public enum ResumePolicy {
    /// `snapshots`: the program's hibernated snapshots, newest first (their
    /// files there: HibernatedSnapshots.snapshots(of:)).  `resumes`: the run
    /// resumes a hibernated program (an interactive run, or a headless one
    /// with --resume); `interactive` runs ask which; `named` is --resume
    /// FILE's; `fresh` and `newCopy` are --fresh and --new.  A program
    /// running already is brought forward before this is asked
    /// (registerRun), so its live entry never gets here.
    public static func choose(snapshots: [String], resumes: Bool, interactive: Bool, fresh: Bool, newCopy: Bool,
                              named: String? = nil,
                              status: (String) -> SnapshotStatus = SnapshotFile.problem) -> ResumeChoice {
        if let named { return verdict(named, status) }
        guard resumes, !fresh, !newCopy, let newest = snapshots.first else { return .launch }
        if interactive { return .pick(snapshots) }
        return verdict(newest, status)
    }

    static func verdict(_ snapshot: String, _ status: (String) -> SnapshotStatus) -> ResumeChoice {
        switch status(snapshot) {
        case .resumable: return .resume(snapshot: snapshot)
        case .missing: return .missing(snapshot: snapshot)
        case .cannotResume(let why): return .ask(snapshot: snapshot, reason: why)
        }
    }
}

/// Whether Hibernate can be chosen now, and why not.
public enum HibernateAvailability: Equatable, Sendable {
    case available
    case unavailable(String)

    public var enabled: Bool { self == .available }
    public var reason: String? {
        if case .unavailable(let why) = self { return why }
        return nil
    }

    /// Running, Hibernate is offered: choosing it suspends the program, which
    /// stops at a safe point (drain.c) and is saved there, or says why not.
    /// Suspended, it is offered once the machine stopped at a safe point
    /// (machine_hibernatable); a suspension that stopped elsewhere, or a
    /// program holding what a snapshot cannot carry, has the drain's reason.
    public static func evaluate(suspended: Bool, parked: Bool, drainState: Int32, reason: String,
                                hibernating: Bool) -> HibernateAvailability {
        if hibernating { return .unavailable("Hibernating…") }
        if !suspended { return .available }
        if !parked && drainState == Int32(MACHINE_DRAIN_GOING) { return .unavailable("Stopping at a safe point…") }
        if drainState == Int32(MACHINE_DRAIN_PARKED) { return .available }
        let why = reason.isEmpty ? "it stopped where it cannot be saved; resume it and try again" : reason
        return .unavailable("It cannot be hibernated now: \(why).")
    }

    /// This process's machine as it is now.
    public static func current(suspended: Bool, hibernating: Bool) -> HibernateAvailability {
        evaluate(suspended: suspended, parked: machine_pause_parked(), drainState: machine_drain_state(),
                 reason: String(cString: machine_drain_reason()), hibernating: hibernating)
    }
}

extension ProgramRegistry {
    /// The program's hibernated entries, newest first.
    public func hibernated(for program: String) -> [RunningProgram] {
        let path = RunningProgram.canonical(program)
        return hibernatedEntries().filter { RunningProgram.canonical($0.program) == path }
    }

    /// Every hibernated entry, newest first.
    public func hibernatedEntries() -> [RunningProgram] {
        entries().filter { $0.state == .hibernated }.sorted { $0.hibernatedDate > $1.hibernatedDate }
    }

    /// The hibernated entries naming a snapshot removed (it was resumed,
    /// deleted or discarded).  Returns how many.
    @discardableResult public func forget(snapshot: String) -> Int {
        let target = RunningProgram.canonical(snapshot)
        return exclusively {
            var n = 0
            for name in namedEntries() {
                guard let data = FileManager.default.contents(atPath: json(name)),
                      let e = try? Self.decoder.decode(RunningProgram.self, from: data),
                      e.state == .hibernated, let s = e.snapshot, RunningProgram.canonical(s) == target else { continue }
                unlink(json(name))
                unlink(lock(name))
                n += 1
            }
            return n
        }
    }

    /// The entries' names (without .json).
    func namedEntries() -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: directory)) ?? [])
            .filter { $0.hasSuffix(".json") && !$0.hasPrefix(".") }.map { String($0.dropLast(5)) }
    }
}

extension RunningProgram {
    /// When it was hibernated (older entries say only when it started).
    public var hibernatedDate: Date { hibernatedAt ?? started }
}

/// One snapshot file, for listing and deleting.
public struct SnapshotInfo: Equatable, Sendable {
    public var path: String
    public var bytes: UInt64
    public var modified: Date
    public var record: HibernateRecord?
    /// This build can resume it (the build that saved it, or one whose
    /// saved state has the same shape); else why not.
    public var resumable: Bool
    public var incompatible: String?
    /// Why it could not be read.
    public var problem: String?
    /// A hibernated entry names it: opening its program resumes it.
    public var entered: Bool

    /// When its entry says it hibernated (a program's own listing).
    public var hibernatedAt: Date?

    /// What it is of: the record's title, else the file's name.
    public var title: String { record?.title ?? (path as NSString).lastPathComponent }
    public var saved: Date { record?.saved ?? hibernatedAt ?? modified }
    public var thumbnail: Data? { record?.thumbnail }
    /// A save point's (the program went on), not Hibernate's.
    public var isSavePoint: Bool { record?.isSavePoint ?? false }
    /// Why it cannot be resumed here, or nil.
    public var unresumable: String? { problem ?? (resumable ? nil : incompatible ?? SnapshotProblem.otherBuild) }

    /// "Macintosh Quadra 800 · 640×480, 256 colors · 3.2 MB": what the
    /// picker says under its date.
    public var details: String {
        var parts: [String] = []
        if let m = record?.machine { parts.append(m) }
        if let sc = record?.screen, sc.count == 2 {
            parts.append("\(sc[0])×\(sc[1])" + (record?.depth.map { ", " + Self.colors($0) } ?? ""))
        }
        parts.append(ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file))
        return parts.joined(separator: " · ")
    }

    /// A screen depth in words.
    public static func colors(_ depth: Int32) -> String {
        switch depth {
        case 1: return "black and white"
        case 2: return "4 colors"
        case 4: return "16 colors"
        case 8: return "256 colors"
        case 16: return "thousands of colors"
        case 32, 24: return "millions of colors"
        default: return "\(depth)-bit"
        }
    }
}

/// The picture of the screen each snapshot carries.
public enum SnapshotThumbnail {
    /// Its width at most, in pixels (its height in proportion).
    public static let width: Int32 = 160

    /// A PNG's width and height (from its header), or nil if it is none.
    public static func size(_ png: Data) -> (width: Int, height: Int)? {
        let b = [UInt8](png.prefix(24))
        guard b.count == 24, b[0..<8] == [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A],
              b[12..<16] == [0x49, 0x48, 0x44, 0x52] else { return nil } // IHDR
        func be(_ i: Int) -> Int { Int(b[i]) << 24 | Int(b[i + 1]) << 16 | Int(b[i + 2]) << 8 | Int(b[i + 3]) }
        return (be(16), be(20))
    }
}

/// Choosing a snapshot when a program with several (or one) is opened: one
/// row each, newest first, with its thumbnail (or a placeholder), when it
/// hibernated and on what; Resume goes on from the chosen one, Start Fresh
/// launches the program anew (the snapshots kept), Move to Trash deletes
/// the chosen one there and then.  The model of the window the run shows
/// (Hibernate.swift), without AppKit.
public struct SnapshotPicker: Equatable {
    public enum Choice: Equatable, Sendable {
        case resume(String)
        case startFresh
        /// Nothing started (the window closed, or Cancel).
        case cancel
    }

    public struct Row: Equatable, Identifiable, Sendable {
        public var info: SnapshotInfo
        public var id: String { info.path }
        public var path: String { info.path }
        /// "Today 14:05", "Yesterday 09:30", "2026-11-06 01:23".
        public var when: String
        public var details: String { info.details }
        public var thumbnail: Data? { info.thumbnail }
        /// No thumbnail (a snapshot from before they were kept): the
        /// window draws a placeholder.
        public var placeholder: Bool { thumbnail == nil }
        public var resumable: Bool { info.unresumable == nil }
        public var problem: String? { info.unresumable }
        public var isSavePoint: Bool { info.isSavePoint }
    }

    public let title: String
    public private(set) var rows: [Row]
    /// The chosen row's path.
    public var selection: String?

    public init(title: String, snapshots: [SnapshotInfo], now: Date = Date(), timeZone: TimeZone = .current) {
        self.title = title
        rows = HibernatedSnapshots.newestFirst(snapshots).map {
            Row(info: $0, when: Self.when($0.saved, now: now, timeZone: timeZone))
        }
        selection = rows.first { $0.resumable }?.path ?? rows.first?.path
    }

    /// The program's snapshots, as opening it finds them.
    public init(program: String, title: String, registry: ProgramRegistry) {
        self.init(title: title, snapshots: HibernatedSnapshots.snapshots(of: program, registry: registry))
    }

    public var selected: Row? { rows.first { $0.path == selection } }
    /// What the window says of the program: hibernated, or (with only save
    /// points) that it has save points to go on from.
    public var heading: String {
        rows.contains { !$0.isSavePoint } || rows.isEmpty ? "\(title) is hibernated" : "\(title) has save points"
    }
    /// Resume is offered: the chosen one resumes here.
    public var canResume: Bool { selected?.resumable ?? false }

    /// Resume: the chosen snapshot, if it resumes here.
    public func resume() -> Choice? {
        guard let s = selected, s.resumable else { return nil }
        return .resume(s.path)
    }

    /// Move to Trash: the chosen one deleted (delete's way) and its row
    /// gone, the one after it chosen (or before it, at the end).
    public mutating func deleteSelected(registry: ProgramRegistry, toTrash: Bool = true) throws {
        guard let path = selection, let i = rows.firstIndex(where: { $0.path == path }) else { return }
        try HibernatedSnapshots.delete(path, registry: registry, toTrash: toTrash)
        rows.remove(at: i)
        selection = rows.isEmpty ? nil : rows[min(i, rows.count - 1)].path
    }

    /// When it hibernated, in words relative to now.
    public static func when(_ date: Date, now: Date = Date(), timeZone: TimeZone = .current) -> String {
        var cal = Calendar(identifier: .gregorian)
        cal.timeZone = timeZone
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = timeZone
        f.dateFormat = "HH:mm"
        let time = f.string(from: date)
        if cal.isDate(date, inSameDayAs: now) { return "Today \(time)" }
        if let y = cal.date(byAdding: .day, value: -1, to: now), cal.isDate(date, inSameDayAs: y) { return "Yesterday \(time)" }
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return f.string(from: date)
    }
}

public enum HibernatedSnapshots {
    /// The snapshots in the folders and those the registry's entries name
    /// (one hibernated elsewhere with --hibernate-to), newest first.
    public static func list(folders: [String], registry: ProgramRegistry) -> [SnapshotInfo] {
        let fm = FileManager.default
        var paths: [String] = []
        for folder in folders {
            for name in (try? fm.contentsOfDirectory(atPath: folder)) ?? [] where name.hasSuffix(".bbhib") {
                paths.append(RunningProgram.canonical((folder as NSString).appendingPathComponent(name)))
            }
        }
        let entered = Set(registry.hibernatedEntries().compactMap(\.snapshot).map(RunningProgram.canonical))
        for p in entered where !paths.contains(p) && fm.fileExists(atPath: p) { paths.append(p) }
        return newestFirst(paths.map { info($0, entered: entered.contains($0)) })
    }

    /// One program's snapshots, those its hibernated entries name, newest
    /// first: what opening it offers.  An entry whose file is gone is
    /// forgotten.
    public static func snapshots(of program: String, registry: ProgramRegistry) -> [SnapshotInfo] {
        var out: [SnapshotInfo] = []
        var seen = Set<String>()
        for e in registry.hibernated(for: program) {
            guard let s = e.snapshot.map(RunningProgram.canonical), seen.insert(s).inserted else { continue }
            guard FileManager.default.fileExists(atPath: s) else {
                registry.forget(snapshot: s)
                continue
            }
            var i = info(s, entered: true)
            i.hibernatedAt = e.hibernatedDate
            out.append(i)
        }
        return newestFirst(out)
    }

    /// The file's size, date and launch record.
    public static func info(_ path: String, entered: Bool) -> SnapshotInfo {
        let attrs = (try? FileManager.default.attributesOfItem(atPath: path)) ?? [:]
        var info = SnapshotInfo(path: path, bytes: (attrs[.size] as? NSNumber)?.uint64Value ?? 0,
                                modified: attrs[.modificationDate] as? Date ?? .distantPast, record: nil,
                                resumable: false, incompatible: nil, problem: nil, entered: entered)
        switch SnapshotFile.read(path) {
        case .success(let r):
            info.record = r.record
            info.resumable = r.resumable
            info.incompatible = r.resumable ? nil : r.why
        case .failure(let p):
            info.problem = p.message
        }
        return info
    }

    static func newestFirst(_ list: [SnapshotInfo]) -> [SnapshotInfo] {
        list.sorted { $0.saved == $1.saved ? $0.path < $1.path : $0.saved > $1.saved }
    }

    /// How many of a program's snapshots are kept when it hibernates again
    /// (AppSettings.keepSnapshots overrides it), and how many save points
    /// besides (AppSettings.keepSavePoints).
    public static let defaultKeep = 5
    public static let defaultKeepSavePoints = 5

    /// After a program hibernates (or makes a save point): its snapshots of
    /// that kind in the Hibernated folders beyond the newest `keep` deleted
    /// (to the Trash, see delete), oldest first, so they never pile up
    /// unseen; one saved elsewhere (--hibernate-to) is the user's own and
    /// left alone.  Returns those deleted.
    @discardableResult
    public static func tidy(program: String, keep: Int, folders: [String], registry: ProgramRegistry,
                            savePoints: Bool = false, toTrash: Bool = true) -> [String] {
        let roots = folders.map { RunningProgram.canonical($0) + "/" }
        let mine = snapshots(of: program, registry: registry).filter { s in
            s.isSavePoint == savePoints && roots.contains { s.path.hasPrefix($0) }
        }
        guard mine.count > max(keep, 1) else { return [] }
        var gone: [String] = []
        for s in mine.dropFirst(max(keep, 1)).reversed() {
            if (try? delete(s.path, registry: registry, toTrash: toTrash)) != nil { gone.append(s.path) }
        }
        return gone
    }

    /// A snapshot deleted, to the Trash (where it can be put back from)
    /// unless asked otherwise (or BLUEBOX_NO_TRASH is set, for tests in a
    /// HOME of their own: the Trash is the user's), and the entries naming
    /// it forgotten.  Returns whether it went to the Trash.
    @discardableResult
    public static func delete(_ path: String, registry: ProgramRegistry, toTrash: Bool = true) throws -> Bool {
        let fm = FileManager.default
        var trashed = false
        if fm.fileExists(atPath: path) {
            #if os(macOS)
            if toTrash && ProcessInfo.processInfo.environment["BLUEBOX_NO_TRASH"] == nil {
                try fm.trashItem(at: URL(fileURLWithPath: path), resultingItemURL: nil)
                trashed = true
            } else {
                try fm.removeItem(atPath: path)
            }
            #else
            try fm.removeItem(atPath: path)
            #endif
        }
        registry.forget(snapshot: path)
        return trashed
    }
}
