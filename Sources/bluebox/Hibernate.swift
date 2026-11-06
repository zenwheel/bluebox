// Hibernating a program: its whole machine saved to a file and resumed
// later in a fresh process of the same build (after a reboot, say), as a
// virtual machine is suspended to disk.
//
//   bluebox run <app> --headless ... --hibernate-at K [--hibernate-to F]
//       runs K instructions, drains to a safe point (the program's event
//       loop: drain.c), writes the snapshot and ends
//   bluebox resume <snapshot> --headless ...
//       boots a machine as the run did (the snapshot's launch record has its
//       arguments), puts the snapshot back and goes on
//
// Snapshots go to Application Support/Blue Box/Hibernated/<Program>
// <date>.bbhib; the program's entry in the registry of running programs says
// where, with the state hibernated, and stays after the process ends.

import Foundation
import BlueBoxHost
import Toolbox

/// What a snapshot file says of the run that made it (its uncompressed
/// head), enough to boot a machine the same way again.
struct HibernateRecord: Codable {
    var version = 1
    /// The run's arguments, the application first, without the ones that
    /// belong to that run alone (runArgsToKeep).
    var args: [String]
    var program: String
    var title: String
    var saved: Date
    var instructions: UInt64
    var drainedAt: String
}

/// Where hibernated programs go: beside the library.
func hibernatedFolder() -> String {
    ((sharedLibrary() as NSString).deletingLastPathComponent as NSString).appendingPathComponent("Hibernated")
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
                                      "--file-answer", "--redraw-check", "--app"]
    let dropAlone: Set<String> = ["--census", "--walk-menus", "--headless", "--background", "--new", "--screen-window",
                                  "--realtime"]
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

/// The machine stopped at a safe point (RUN_DRAINED): write its snapshot
/// and enter it in the registry as hibernated.  Returns the path, or nil
/// (the reason said) when it could not be saved.
func hibernateRun(app: String, runArgs: [String], to given: String?, startedAt: UInt64) -> String? {
    guard machine_hibernatable() else {
        print("hibernate refused: \(String(cString: machine_drain_reason()))")
        return nil
    }
    let title = (app as NSString).lastPathComponent
    let path = given ?? newSnapshotPath(title: title)
    try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                             withIntermediateDirectories: true)
    let record = HibernateRecord(args: [app] + runArgsToKeep(runArgs), program: RunningProgram.canonical(app), title: title,
                                 saved: Date(), instructions: machine_instruction_count(),
                                 drainedAt: String(cString: machine_drain_point()))
    let enc = JSONEncoder()
    enc.dateEncodingStrategy = .iso8601
    enc.outputFormatting = [.sortedKeys]
    let json = String(decoding: (try? enc.encode(record)) ?? Data(), as: UTF8.self)
    var err = [CChar](repeating: 0, count: 512)
    var bytes: UInt64 = 0
    let t0 = Date()
    guard machine_hibernate_save(path, json, &bytes, &err, err.count) else {
        print("hibernate failed: \(str(err))")
        return nil
    }
    let ms = Int(Date().timeIntervalSince(t0) * 1000)
    print("hibernated: \(path) (\(bytes) bytes in \(ms) ms; stopped at \(record.drainedAt) "
          + "after \(record.instructions) instructions)")
    enterHibernated(app: app, title: title, snapshot: path)
    return path
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
    let registry = ProgramRegistry(library: sharedLibrary())
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
/// gone, as the snapshot now runs again (in this process).
func forgetHibernated(snapshot: String) {
    let registry = ProgramRegistry(library: sharedLibrary())
    registry.exclusively {
        for name in (try? FileManager.default.contentsOfDirectory(atPath: registry.directory)) ?? []
        where name.hasSuffix(".json") {
            let file = registry.directory + "/" + name
            guard let data = FileManager.default.contents(atPath: file),
                  let text = String(data: data, encoding: .utf8), text.contains("\"hibernated\""),
                  text.contains(snapshot.replacingOccurrences(of: "/", with: "\\/")) || text.contains(snapshot) else { continue }
            unlink(file)
            unlink(registry.directory + "/" + String(name.dropLast(5)) + ".lock")
        }
    }
}

/// The launch record of a snapshot file.
func readHibernateRecord(_ path: String) -> (HibernateRecord, sameBuild: Bool) {
    var extra: UnsafeMutablePointer<UInt8>?
    var len = 0
    var same = false
    var err = [CChar](repeating: 0, count: 512)
    guard hb_file_extra(path, &extra, &len, &same, &err, err.count), let extra else { fail(str(err)) }
    defer { hb_free_buffer(extra) }
    let dec = JSONDecoder()
    dec.dateDecodingStrategy = .iso8601
    guard let record = try? dec.decode(HibernateRecord.self, from: Data(bytes: extra, count: len)) else {
        fail("\(path): the snapshot has no launch record Blue Box reads")
    }
    return (record, same)
}

/// bluebox resume <snapshot> [run flags]: the machine booted as the
/// record's run did, the snapshot put back, and on it goes (runCommand).
func resumeCommand(_ args: [String]) {
    guard let snapshot = args.first, !snapshot.hasPrefix("-") else { usage() }
    let path = URL(fileURLWithPath: snapshot).standardizedFileURL.path
    let (record, sameBuild) = readHibernateRecord(path)
    guard sameBuild else {
        fail("\(path): saved by another build of Blue Box (it resumes only in the build that saved it)")
    }
    runCommand(record.args + args.dropFirst(), resume: path)
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
