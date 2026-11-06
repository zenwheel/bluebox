// The running programs, as the shell, `bluebox run` and installed bundles
// all see them: one entry each in a folder (Application Support/Blue
// Box/Running, beside the library), so a program started from a terminal
// or from its own bundle is as known to the shell as one it launched.
//
// An entry is two files with one name: NAME.json says what is running
// (rewritten whole, through a rename, when its state changes), and
// NAME.lock is held locked (flock, exclusive) by its process for its whole
// life.  The lock goes with the process however it ends, a crash too, so
// an entry whose lock can be taken is a dead process's, and whoever finds
// it removes it.  The lock is a file of its own because the rename puts a
// new file where the old one was: a lock on the JSON would be left on a
// file no longer there.  Names are unique (not process IDs), so that a
// hibernated program's entry, which outlives its process, never meets a
// new process with the same ID.
//
// The registry is the shared state, not a way to ask anything: the shell
// still asks a program to suspend or resume with a distributed
// notification (SuspendNotice).

import Foundation
#if canImport(Glibc)
import Glibc
#endif

/// One running (or suspended, or later hibernated) program.
public struct RunningProgram: Codable, Equatable, Sendable {
    public enum State: String, Codable, Sendable {
        case running, suspended
        /// Saved to disk and its process gone (hibernation, still to come):
        /// the entry stays, with `snapshot`, after the lock is let go.
        case hibernated
    }

    /// How it was started.
    public enum Launcher: String, Codable, Sendable {
        case shell, commandLine, bundle
    }

    /// The program: its path in the library (an installed bundle's
    /// original), or the file run.
    public var program: String
    public var pid: Int32
    /// The host executable running it.
    public var executable: String
    public var state: State
    public var started: Date
    /// What the Dock calls it.
    public var title: String
    public var launchedBy: Launcher
    /// The bundle it was started from (launchedBy .bundle).
    public var bundle: String?
    /// Where a hibernated program's saved state is.
    public var snapshot: String?

    public init(program: String, pid: Int32 = getpid(), executable: String, state: State = .running,
                started: Date = Date(), title: String, launchedBy: Launcher, bundle: String? = nil,
                snapshot: String? = nil) {
        self.program = program
        self.pid = pid
        self.executable = executable
        self.state = state
        self.started = Date(timeIntervalSince1970: started.timeIntervalSince1970.rounded(.down)) // as written
        self.title = title
        self.launchedBy = launchedBy
        self.bundle = bundle
        self.snapshot = snapshot
    }

    /// The paths of one program compare equal however they were written.
    public static func canonical(_ path: String) -> String {
        URL(fileURLWithPath: path).standardizedFileURL.path
    }

    /// How a run was started, from its environment: the shell says so
    /// (BLUEBOX_LAUNCHED_BY=shell), Launch Services names an installed
    /// bundle (not the one a terminal's run starts itself again from,
    /// local.bluebox.run.*), and a bundle with its own Blue Box is its own.
    public static func launcher(environment env: [String: String], ownBundle: String?)
        -> (Launcher, bundle: String?) {
        if env[launchedByVariable] == "shell" { return (.shell, nil) }
        if let id = env["__CFBundleIdentifier"], id.hasPrefix("local.bluebox."), !id.hasPrefix("local.bluebox.run.") {
            return (.bundle, id)
        }
        if let ownBundle { return (.bundle, ownBundle) }
        return (.commandLine, nil)
    }

    /// The environment variable the shell sets for a program it launches.
    public static let launchedByVariable = "BLUEBOX_LAUNCHED_BY"
}

/// Whether a run is entered in the registry: an interactive run (in a
/// window or rootless, or in the background) is; a headless one (a census,
/// a menu walk, a screenshot, printing to a file) is not.
public func runRegisters(headless: Bool, census: Bool, walkMenus: Bool, screenshot: Bool, printToFile: Bool) -> Bool {
    !headless && !census && !walkMenus && !screenshot && !printToFile
}

/// The folder of entries.
public struct ProgramRegistry: Sendable {
    public let directory: String

    public init(directory: String) { self.directory = directory }

    /// The registry beside a library (Application Support/Blue Box/Running
    /// for the default one, ~/.local/share/bluebox/Running on Linux), so a
    /// HOME or BLUEBOX_LIBRARY of its own has a registry of its own.
    public init(library: String) {
        self.init(directory: ((library as NSString).deletingLastPathComponent as NSString)
            .appendingPathComponent("Running"))
    }

    func json(_ name: String) -> String { directory + "/" + name + ".json" }
    func lock(_ name: String) -> String { directory + "/" + name + ".lock" }

    /// The live entries (and hibernated ones), oldest first; a dead
    /// process's entry is removed.
    public func entries() -> [RunningProgram] {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: directory)) ?? []
        var found: [RunningProgram] = []
        for file in names where file.hasSuffix(".json") && !file.hasPrefix(".") {
            let name = String(file.dropLast(5))
            guard let data = FileManager.default.contents(atPath: json(name)),
                  let entry = try? Self.decoder.decode(RunningProgram.self, from: data) else { continue }
            if entry.state == .hibernated || isHeld(name) {
                found.append(entry)
            } else {
                removeStale(name)
            }
        }
        // Lock files with no entry: a run that died between taking its lock
        // and writing its entry (one just starting has a minute).
        for file in names where file.hasSuffix(".lock") && !file.hasPrefix(".") {
            let name = String(file.dropLast(5))
            guard !names.contains(name + ".json"),
                  let made = (try? FileManager.default.attributesOfItem(atPath: lock(name)))?[.modificationDate] as? Date,
                  Date().timeIntervalSince(made) > 60 else { continue }
            removeStale(name)
        }
        return found.sorted { $0.started == $1.started ? $0.pid < $1.pid : $0.started < $1.started }
    }

    /// The program's live entry, the oldest if there are several.
    public func entry(for program: String) -> RunningProgram? {
        let path = RunningProgram.canonical(program)
        return entries().first { $0.state != .hibernated && RunningProgram.canonical($0.program) == path }
    }

    /// Someone holds the entry's lock: its process is alive.
    func isHeld(_ name: String) -> Bool {
        let fd = open(lock(name), O_RDWR | O_CLOEXEC)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        if flock(fd, LOCK_EX | LOCK_NB) == 0 {
            flock(fd, LOCK_UN)
            return false
        }
        return errno == EWOULDBLOCK
    }

    /// Remove a dead process's entry, holding its lock while doing so (a
    /// process taking the name again checks it got the file it locked).
    func removeStale(_ name: String) {
        let fd = open(lock(name), O_RDWR | O_CLOEXEC)
        if fd >= 0 {
            defer { close(fd) }
            guard flock(fd, LOCK_EX | LOCK_NB) == 0 else { return } // alive after all
            unlink(json(name))
            unlink(lock(name))
        } else {
            unlink(json(name))
        }
    }

    /// Hold the registry's own lock while looking for a program and entering
    /// it, so two runs started together don't both start.
    public func exclusively<T>(_ body: () throws -> T) rethrows -> T {
        try? FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        let fd = open(directory + "/.registry.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0o644)
        if fd >= 0 { flock(fd, LOCK_EX) }
        defer { if fd >= 0 { close(fd) } }
        return try body()
    }

    /// Enter a program: its lock taken, then its entry written.
    public func register(_ entry: RunningProgram) throws -> Registration {
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
        let name = "\(entry.pid)-" + UUID().uuidString.prefix(8).lowercased()
        let fd = try takeLock(name)
        let registration = Registration(registry: self, name: name, fd: fd, entry: entry)
        do { try write(entry, name: name) } catch {
            registration.end()
            throw error
        }
        return registration
    }

    /// Open and lock NAME.lock; it must still be the file at that name once
    /// locked (whoever removes a dead entry's lock file holds it first).
    func takeLock(_ name: String) throws -> Int32 {
        for _ in 0..<8 {
            let fd = open(lock(name), O_RDWR | O_CREAT | O_CLOEXEC, 0o644)
            guard fd >= 0 else { throw CocoaError(.fileWriteNoPermission) }
            guard flock(fd, LOCK_EX | LOCK_NB) == 0 else {
                close(fd)
                throw CocoaError(.fileLocking)
            }
            var held = stat(), there = stat()
            if fstat(fd, &held) == 0, stat(lock(name), &there) == 0, held.st_ino == there.st_ino, held.st_dev == there.st_dev {
                return fd
            }
            close(fd)
        }
        throw CocoaError(.fileLocking)
    }

    /// Write an entry whole: a file beside it, renamed over it.
    func write(_ entry: RunningProgram, name: String) throws {
        let temp = directory + "/." + name + ".json." + UUID().uuidString.prefix(6)
        do {
            try Self.encoder.encode(entry).write(to: URL(fileURLWithPath: temp))
            guard rename(temp, json(name)) == 0 else { throw CocoaError(.fileWriteUnknown) }
        } catch {
            unlink(temp)
            throw error
        }
    }

    static var encoder: JSONEncoder {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        return e
    }

    static var decoder: JSONDecoder {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }
}

/// A process's own entry, held for its life: changed when it suspends or
/// resumes (and, to come, hibernates), removed when it ends.
public final class Registration: @unchecked Sendable {
    public let registry: ProgramRegistry
    /// The entry's file name, without .json or .lock.
    public let name: String
    private var fd: Int32
    private var current: RunningProgram
    private let lock = NSLock()

    init(registry: ProgramRegistry, name: String, fd: Int32, entry: RunningProgram) {
        self.registry = registry
        self.name = name
        self.fd = fd
        current = entry
    }

    public var entry: RunningProgram { lock.withLock { current } }

    /// Change the entry and write it again.
    public func update(_ change: (inout RunningProgram) -> Void) {
        lock.withLock {
            guard fd >= 0 else { return }
            change(&current)
            try? registry.write(current, name: name)
        }
    }

    public func setState(_ state: RunningProgram.State) { update { $0.state = state } }

    /// Hibernated: the entry says where the saved state is and stays when
    /// the process ends (end(keeping: true)).
    public func hibernated(snapshot: String) {
        update {
            $0.state = .hibernated
            $0.snapshot = snapshot
        }
    }

    /// Ended: the entry removed (unless kept, for a hibernated program),
    /// then the lock let go.
    public func end(keeping: Bool = false) {
        lock.withLock {
            guard fd >= 0 else { return }
            if !keeping {
                unlink(registry.json(name))
                unlink(registry.lock(name))
            }
            close(fd)
            fd = -1
        }
    }

    deinit { end() }

    /// This process's registration, if it has one.
    public static var current: Registration? {
        get { currentLock.withLock { currentStorage } }
        set { currentLock.withLock { currentStorage = newValue } }
    }

    nonisolated(unsafe) private static var currentStorage: Registration?
    private static let currentLock = NSLock()
}
