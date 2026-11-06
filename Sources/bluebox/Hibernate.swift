// Hibernating a program: its whole machine saved to a file and resumed
// later in a fresh process of the same build (after a reboot, say), as a
// virtual machine is suspended to disk.
//
//   Hibernate (the application menu, the status item's menu, the suspended
//   overlay; Suspend.swift)
//       drains the machine to a safe point (drain.c), writes the snapshot,
//       enters the program in the registry as hibernated and quits
//   bluebox run <app> ...
//       an interactive run of a hibernated program resumes its newest
//       snapshot (ResumePolicy, BlueBoxHost/Hibernation.swift): the shell's
//       double-click, the Dock, its bundle, an exported application and a
//       login item all come here; --fresh starts it fresh (the snapshot to
//       the Trash), --new another copy (the snapshot left as it is), and a
//       headless run resumes only with --resume
//   bluebox run <app> --headless ... --hibernate-at K [--hibernate-to F]
//       runs K instructions, drains to a safe point, writes the snapshot
//       and ends
//   bluebox resume <snapshot> ...
//       boots a machine as the run did (the snapshot's launch record has its
//       arguments, its screen and its depth), puts the snapshot back and
//       goes on
//   bluebox snapshots [list | delete ...]
//       the snapshots kept (a resumed one stays until deleted)
//
// Snapshots go to Application Support/Blue Box/Hibernated/<Program>
// <date>.bbhib (an exported application's to its own folder there); the
// program's entry in the registry of running programs says where, with the
// state hibernated, and stays after the process ends, until the program is
// resumed or started fresh.

#if canImport(AppKit)
import AppKit
#endif
import Foundation
import BlueBoxHost
import Toolbox

/// The registry hibernated programs are entered in: the one this run
/// registers in.
func hibernationRegistry() -> ProgramRegistry {
    #if canImport(AppKit)
    return defaultRegistry()
    #else
    return ProgramRegistry(library: sharedLibrary())
    #endif
}

/// Where hibernated programs go: beside the library, or an exported
/// application's own folder (its snapshots resume only in its own copy of
/// Blue Box).
func hibernatedFolder() -> String {
    if let support = exportSupport() { return (support as NSString).appendingPathComponent("Hibernated") }
    return ((sharedLibrary() as NSString).deletingLastPathComponent as NSString).appendingPathComponent("Hibernated")
}

/// Every folder snapshots are kept in that this process knows of.
func hibernatedFolders() -> [String] {
    let shared = ((sharedLibrary() as NSString).deletingLastPathComponent as NSString).appendingPathComponent("Hibernated")
    return Array(Set([shared, hibernatedFolder()])).sorted()
}

func newSnapshotPath(title: String) -> String {
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX")
    f.dateFormat = "yyyy-MM-dd HH.mm.ss"
    let name = title.replacingOccurrences(of: "/", with: ":")
    return (hibernatedFolder() as NSString).appendingPathComponent("\(name) \(f.string(from: Date())).bbhib")
}

/// The arguments of a run that its resumption repeats: everything that
/// sets the machine up (its processor, memory, screen, fonts, settings),
/// not the run's own input, limits, logging or output, nor its documents
/// (open already).  Flags taking a value are listed with it.  The
/// application itself is the record's first argument, as resolved.
func runArgsToKeep(_ args: [String]) -> [String] {
    let dropWithValue: Set<String> = ["--keys", "--max", "--hibernate-at", "--hibernate-to", "--then", "--snapshot-end",
                                      "--screenshot", "--walk-avoid", "--log", "--debugger", "--print-to",
                                      "--file-answer", "--redraw-check", "--app", "--sound"]
    let dropAlone: Set<String> = ["--census", "--walk-menus", "--headless", "--background", "--new", "--screen-window",
                                  "--realtime", "--fresh", "--resume"]
    var out: [String] = []
    var i = 0
    while i < args.count {
        let a = args[i]
        if dropWithValue.contains(a) {
            i += 2
            continue
        }
        if dropAlone.contains(a) || a.hasPrefix("-psn_") {
            i += 1
            continue
        }
        if a.hasPrefix("-") {
            out.append(a)
            // The flags with a value (the rest of runCommand's).
            if ["--fonts", "--appearance", "--speech", "--appleshare", "--scale", "--jit-cache", "--depth", "--machine",
                "--floppy", "--screensaver", "--every", "--idle", "--init", "--cpu", "--fpu", "--addressing",
                "--appletalk", "--modem-port", "--printer-port", "--screen", "--gestalt", "--lowmem"].contains(a),
               i + 1 < args.count {
                out.append(args[i + 1])
                i += 1
            }
        }
        i += 1 // the application (given again, resolved) and documents (open already)
    }
    return out
}

/// What this run's snapshot records of it (runCommand sets it once the
/// machine is booted): the program, its arguments, its title, and the
/// screen it was booted with.
struct HibernateContext: Sendable {
    var app: String
    var args: [String]
    var title: String
    var screen: [Int32]
    var depth: Int32
    var display: String
}
nonisolated(unsafe) var hibernateContext: HibernateContext?
/// The launch record of the snapshot this run resumes, if it resumes one.
nonisolated(unsafe) var resumeRecord: HibernateRecord?

struct SavedSnapshot {
    var path: String
    var bytes: UInt64
    var ms: Int
    var record: HibernateRecord
}

/// The machine stopped at a safe point (machine_hibernatable): its snapshot
/// written, atomically (hibernate.c writes beside it and renames).  From
/// any thread, the machine standing still.
func writeSnapshot(app: String, runArgs: [String], to given: String?) -> Result<SavedSnapshot, SnapshotProblem> {
    guard machine_hibernatable() else {
        return .failure(SnapshotProblem(String(cString: machine_drain_reason())))
    }
    let ctx = hibernateContext
    let title = ctx?.title ?? (app as NSString).lastPathComponent
    let path = given ?? newSnapshotPath(title: title)
    try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                             withIntermediateDirectories: true)
    let record = HibernateRecord(args: [app] + runArgsToKeep(runArgs), program: RunningProgram.canonical(app), title: title,
                                 saved: Date(), instructions: machine_instruction_count(),
                                 drainedAt: String(cString: machine_drain_point()), screen: ctx?.screen,
                                 depth: ctx?.depth, display: ctx?.display)
    var err = [CChar](repeating: 0, count: 512)
    var bytes: UInt64 = 0
    let t0 = Date()
    guard machine_hibernate_save(path, record.json, &bytes, &err, err.count) else {
        return .failure(SnapshotProblem(str(err)))
    }
    return .success(SavedSnapshot(path: path, bytes: bytes, ms: Int(Date().timeIntervalSince(t0) * 1000), record: record))
}

/// A headless run stopped at a safe point (RUN_DRAINED): its snapshot
/// written and entered in the registry as hibernated.  Returns the path,
/// or nil (the reason said) when it could not be saved.
func hibernateRun(app: String, runArgs: [String], to given: String?, startedAt: UInt64) -> String? {
    guard machine_hibernatable() else {
        print("hibernate refused: \(String(cString: machine_drain_reason()))")
        return nil
    }
    switch writeSnapshot(app: app, runArgs: runArgs, to: given) {
    case .failure(let p):
        print("hibernate failed: \(p.message)")
        return nil
    case .success(let s):
        print("hibernated: \(s.path) (\(s.bytes) bytes in \(s.ms) ms; stopped at \(s.record.drainedAt) "
              + "after \(s.record.instructions) instructions)")
        enterHibernated(app: app, title: s.record.title, snapshot: s.path)
        return s.path
    }
}

/// The registry's entry for a hibernated program: kept when the process
/// ends, so the shell can offer to resume it.  A run already entered
/// changes its own entry; a headless one (never entered) makes one.
func enterHibernated(app: String, title: String, snapshot: String) {
    if let r = Registration.current {
        r.hibernated(snapshot: snapshot)
        r.end(keeping: true)
        Registration.current = nil
        return
    }
    let registry = hibernationRegistry()
    let executable = Bundle.main.executableURL?.resolvingSymlinksInPath().path ?? CommandLine.arguments[0]
    let entry = RunningProgram(program: RunningProgram.canonical(app), executable: executable, title: title,
                               launchedBy: .commandLine)
    registry.exclusively {
        guard let r = try? registry.register(entry) else { return }
        r.hibernated(snapshot: snapshot)
        r.end(keeping: true)
    }
}

/// A resumed program's entry, if the registry has one for this snapshot:
/// gone, as the snapshot now runs again (in this process).  The file stays.
func forgetHibernated(snapshot: String) {
    hibernationRegistry().forget(snapshot: snapshot)
}

/// The launch record of a snapshot file.
func readHibernateRecord(_ path: String) -> (HibernateRecord, sameBuild: Bool) {
    switch SnapshotFile.read(path) {
    case .success(let r): return (r.record, r.sameBuild)
    case .failure(let p): fail(p.message)
    }
}

/// bluebox resume <snapshot> [run flags]: the machine booted as the
/// record's run did, the snapshot put back, and on it goes (runCommand).
func resumeCommand(_ args: [String]) {
    guard let snapshot = args.first, !snapshot.hasPrefix("-") else { usage() }
    let path = URL(fileURLWithPath: snapshot).standardizedFileURL.path
    let (record, sameBuild) = readHibernateRecord(path)
    guard sameBuild else {
        fail("\(path): saved by another build of Blue Box (\(SnapshotProblem.otherBuild))")
    }
    resumeRecord = record
    runCommand(record.args + args.dropFirst(), resume: path)
}

/// A run of a program (`program`, as the registry knows it) that may be
/// hibernated: resumed from its snapshot, launched fresh, or neither (the
/// user keeps a snapshot this build cannot resume).  Returns when the run
/// launches the program fresh; otherwise resumes it (runCommand, never
/// returning) or ends.  `extra`: the run's arguments but the application.
func resumeIfHibernated(app: String, program: String, extra: [String], resumes: Bool, fresh: Bool, newCopy: Bool,
                        interactive: Bool) {
    let registry = hibernationRegistry()
    let choice = ResumePolicy.choose(hibernated: registry.hibernated(for: program), resumes: resumes, fresh: fresh,
                                     newCopy: newCopy)
    let title = (app as NSString).lastPathComponent
    switch choice {
    case .launch:
        return
    case .missing(let snapshot):
        eprint("bluebox: \(title)'s snapshot \(snapshot) is gone; starting it fresh\n")
        registry.forget(snapshot: snapshot)
    case .discard(let snapshot):
        discardSnapshot(snapshot, registry: registry, title: title)
    case .ask(let snapshot, let reason):
        if interactive && askStartFresh(title: title, snapshot: snapshot, reason: reason) {
            discardSnapshot(snapshot, registry: registry, title: title)
            return
        }
        print("\(title) is hibernated in \(snapshot), which cannot be resumed here: \(reason).")
        print("It is kept. --fresh starts \(title) fresh (the snapshot to the Trash); --new starts it and keeps the snapshot.")
        exit(interactive ? 0 : 1)
    case .resume(let snapshot):
        let (record, _) = readHibernateRecord(snapshot)
        resumeRecord = record
        var more = extra
        // As it was shown: one hibernated in the background resumes there.
        if interactive && record.display == "background" && !more.contains("--background") { more.append("--background") }
        print("resuming \(title) from \(snapshot)")
        runCommand([app] + record.args.dropFirst() + more, resume: snapshot)
    }
}

private func discardSnapshot(_ snapshot: String, registry: ProgramRegistry, title: String) {
    do {
        let trashed = try HibernatedSnapshots.delete(snapshot, registry: registry)
        print("\(title): starting fresh; its snapshot \(snapshot) is \(trashed ? "in the Trash" : "deleted")")
    } catch {
        eprint("bluebox: cannot move \(snapshot) to the Trash (\(error.localizedDescription)); starting \(title) fresh, the snapshot kept\n")
        registry.forget(snapshot: snapshot)
    }
}

/// A snapshot this build cannot resume: Start Fresh (true: the snapshot to
/// the Trash) or Keep Snapshot (false: nothing started).
private func askStartFresh(title: String, snapshot: String, reason: String) -> Bool {
    #if canImport(AppKit)
    return MainActor.assumeIsolated {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        app.activate(ignoringOtherApps: true)
        let alert = NSAlert()
        alert.messageText = "\(title) is hibernated, but its snapshot cannot be resumed here."
        alert.informativeText = (reason.prefix(1).uppercased() + reason.dropFirst()) + "."
            + " You can keep the snapshot (nothing is started), or start \(title) fresh and move the snapshot to the Trash."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Keep Snapshot")
        alert.addButton(withTitle: "Start Fresh")
        return alert.runModal() == .alertSecondButtonReturn
    }
    #else
    return false
    #endif
}

/// bluebox snapshots [list] | delete <snapshot|program>... [--permanently]:
/// the snapshots kept, and deleting them (to the Trash).
func snapshotsCommand(_ args: [String]) {
    let registry = hibernationRegistry()
    let all = HibernatedSnapshots.list(folders: hibernatedFolders(), registry: registry)
    switch args.first ?? "list" {
    case "list":
        if all.isEmpty { print("no hibernated snapshots (in \(hibernatedFolders().joined(separator: ", ")))") }
        for s in all {
            let kb = (s.bytes + 1023) / 1024
            let state = s.problem.map { "unreadable: \($0)" }
                ?? (!s.sameBuild ? "another build's" : s.entered ? "hibernated: opening \(s.title) resumes it" : "resumed or set aside")
            print("\(s.path)\n    \(s.title), \(SnapshotFile.label(s.saved)), \(kb) KB, \(state)")
        }
    case "delete":
        let permanently = args.contains("--permanently")
        let names = args.dropFirst().filter { $0 != "--permanently" }
        guard !names.isEmpty else { usage() }
        var failed = false
        for name in names {
            let given = URL(fileURLWithPath: name).standardizedFileURL.path
            // A file, or every snapshot of a program by its title.
            let matches = FileManager.default.fileExists(atPath: given) ? [given] : all.filter { $0.title == name }.map(\.path)
            if matches.isEmpty {
                eprint("bluebox: no snapshot \(name)\n")
                failed = true
            }
            for path in matches {
                do {
                    let trashed = try HibernatedSnapshots.delete(path, registry: registry, toTrash: !permanently)
                    print(trashed ? "moved to the Trash: \(path)" : "deleted: \(path)")
                } catch {
                    eprint("bluebox: cannot delete \(path): \(error.localizedDescription)\n")
                    failed = true
                }
            }
        }
        exit(failed ? 1 : 0)
    default:
        usage()
    }
    exit(0)
}

/// bluebox hibernate-diff A B: where two snapshots differ (hb_compare),
/// pointers to the host's memory left out.
func hibernateDiffCommand(_ args: [String]) {
    guard args.count >= 2 else { usage() }
    let max = args.count > 2 ? Int32(args[2]) ?? 40 : 40
    var a: UnsafeMutablePointer<UInt8>?, b: UnsafeMutablePointer<UInt8>?
    var alen = 0, blen = 0
    var err = [CChar](repeating: 0, count: 512)
    guard hb_file_load(args[0], &a, &alen, &err, err.count), hb_file_load(args[1], &b, &blen, &err, err.count) else {
        fail(str(err))
    }
    let n = hb_compare(a, alen, b, blen, Int32(HB_COMPARE_IGNORE_HOST), stdout, max)
    print("\(n) difference\(n == 1 ? "" : "s")")
    exit(n == 0 ? 0 : 1)
}
