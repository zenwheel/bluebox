// Hibernated programs, as `bluebox run`, the shell and a program's own
// process all see them: a snapshot file (hibernate.c's, with the run's
// launch record in its head) and an entry in the registry of running
// programs with the state hibernated, which outlives the process.
//
// What opening a hibernated program does (ResumePolicy): an interactive
// run of it (a double-click in the shell, its bundle, the Dock, an exported
// application, a login item, `bluebox run` from a terminal) resumes its
// newest snapshot instead of launching it; --fresh starts it fresh and
// moves the snapshot to the Trash; --new starts another copy and leaves the
// snapshot alone.  A headless run never resumes unless asked (--resume), so
// a census or a menu walk of a program the user hibernated leaves it be.
// A snapshot another build wrote is never resumed or deleted without the
// user choosing: the run asks.

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

    public init(args: [String], program: String, title: String, saved: Date, instructions: UInt64, drainedAt: String,
                screen: [Int32]? = nil, depth: Int32? = nil, display: String? = nil) {
        self.args = args
        self.program = program
        self.title = title
        self.saved = saved
        self.instructions = instructions
        self.drainedAt = drainedAt
        self.screen = screen
        self.depth = depth
        self.display = display
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

    /// The words for a snapshot of another build's.
    public static let otherBuild = "it resumes only in the build of Blue Box that saved it"
}

public enum SnapshotFile {
    /// A snapshot file's launch record and whether this build wrote it.
    public static func read(_ path: String) -> Result<(record: HibernateRecord, sameBuild: Bool), SnapshotProblem> {
        var extra: UnsafeMutablePointer<UInt8>?
        var len = 0
        var same = false
        var err = [CChar](repeating: 0, count: 512)
        guard hb_file_extra(path, &extra, &len, &same, &err, err.count), let extra else {
            return .failure(SnapshotProblem(String(decoding: err.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)))
        }
        defer { hb_free_buffer(extra) }
        guard let record = HibernateRecord.decode(Data(bytes: extra, count: len)) else {
            return .failure(SnapshotProblem("\(path): the snapshot has no launch record Blue Box reads"))
        }
        return .success((record, same))
    }

    /// What resuming it here would meet: nil when it can be resumed.
    public static func problem(_ path: String) -> SnapshotStatus {
        guard FileManager.default.fileExists(atPath: path) else { return .missing }
        switch read(path) {
        case .success(let r): return r.sameBuild ? .resumable : .cannotResume(SnapshotProblem.otherBuild)
        case .failure(let p): return .cannotResume(p.message)
        }
    }

    /// "Hibernated 2026-11-06 01:23", in the host's time zone.
    public static func label(_ date: Date, timeZone: TimeZone = .current) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = timeZone
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return "Hibernated \(f.string(from: date))"
    }
}

/// A snapshot as a run about to resume it finds it.
public enum SnapshotStatus: Equatable, Sendable {
    case resumable
    /// Its file is gone (moved, deleted): the entry is forgotten.
    case missing
    /// Another build's, or unreadable: the user decides.
    case cannotResume(String)
}

/// What a run of a program does about its hibernated snapshot.
public enum ResumeChoice: Equatable, Sendable {
    /// Launched fresh (nothing hibernated, or a run that never resumes).
    case launch
    case resume(snapshot: String)
    /// The entry's file is gone: the entry is forgotten and the program
    /// launched fresh.
    case missing(snapshot: String)
    /// It cannot resume here (another build's, or unreadable): ask whether to
    /// start fresh (the snapshot to the Trash) or keep it (nothing started).
    case ask(snapshot: String, reason: String)
    /// --fresh: the snapshot to the Trash, its entry forgotten, a fresh launch.
    case discard(snapshot: String)
}

public enum ResumePolicy {
    /// `hibernated`: the program's hibernated entries, newest first.
    /// `resumes`: the run resumes a hibernated program (an interactive run,
    /// or a headless one with --resume); `fresh` and `newCopy` are --fresh and
    /// --new.  A program running already is brought forward before this is
    /// asked (registerRun), so its live entry never gets here.
    public static func choose(hibernated: [RunningProgram], resumes: Bool, fresh: Bool, newCopy: Bool,
                              status: (String) -> SnapshotStatus = SnapshotFile.problem) -> ResumeChoice {
        guard resumes || fresh, !newCopy, let entry = hibernated.first else { return .launch }
        guard let snapshot = entry.snapshot else { return .launch }
        if fresh { return .discard(snapshot: snapshot) }
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
    public var sameBuild: Bool
    /// Why it could not be read.
    public var problem: String?
    /// A hibernated entry names it: opening its program resumes it.
    public var entered: Bool

    /// What it is of: the record's title, else the file's name.
    public var title: String { record?.title ?? (path as NSString).lastPathComponent }
    public var saved: Date { record?.saved ?? modified }
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
        return paths.map { path in
            let attrs = (try? fm.attributesOfItem(atPath: path)) ?? [:]
            var info = SnapshotInfo(path: path, bytes: (attrs[.size] as? NSNumber)?.uint64Value ?? 0,
                                    modified: attrs[.modificationDate] as? Date ?? .distantPast, record: nil,
                                    sameBuild: false, problem: nil, entered: entered.contains(path))
            switch SnapshotFile.read(path) {
            case .success(let r):
                info.record = r.record
                info.sameBuild = r.sameBuild
            case .failure(let p):
                info.problem = p.message
            }
            return info
        }.sorted { $0.saved == $1.saved ? $0.path < $1.path : $0.saved > $1.saved }
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
