// Hibernating a program: its whole machine saved to a file and resumed
// later in a fresh process of the same build (after a reboot, say), as a
// virtual machine is suspended to disk.
//
//   Hibernate (the application menu, the status item's menu, the suspended
//   overlay, Control-Option-Command-H; Suspend.swift)
//       drains the machine to a safe point (drain.c), writes a new snapshot
//       (with a thumbnail of the screen), enters it in the registry as
//       hibernated, deletes the program's oldest beyond the ones it keeps
//       (HibernatedSnapshots.tidy) and quits
//   bluebox run <app> ...
//       an interactive run of a program with snapshots shows them (the
//       snapshot picker, below; ResumePolicy, BlueBoxHost/Hibernation.swift):
//       the shell's double-click, the Dock, its bundle, an exported
//       application and a login item all come here; --fresh starts it fresh
//       and --new another copy (both leave the snapshots as they are), and a
//       headless run resumes only with --resume (the newest) or --resume
//       FILE.bbhib
//   bluebox run <app> --headless ... --hibernate-at K [--hibernate-to F]
//       runs K instructions, drains to a safe point, writes the snapshot
//       and ends
//   bluebox resume <snapshot> ...
//       boots a machine as the run did (the snapshot's launch record has its
//       arguments, its screen and its depth), puts the snapshot back and
//       goes on
//   bluebox snapshots [list | delete ... | thumbnail ...]
//       the snapshots kept (a resumed one stays until deleted)
//
// Snapshots go to Application Support/Blue Box/Hibernated/<Program>
// <date>.bbhib (an exported application's to its own folder there); each
// has an entry in the registry of running programs saying where, with the
// state hibernated, which stays after the process ends and after the
// snapshot is resumed, until the snapshot is deleted.

#if canImport(AppKit)
import AppKit
import SwiftUI
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
    let name = title.replacingOccurrences(of: "/", with: ":") + " " + f.string(from: Date())
    // A program keeps several: one saved within the same second is another.
    var path = (hibernatedFolder() as NSString).appendingPathComponent(name + ".bbhib")
    var n = 2
    while FileManager.default.fileExists(atPath: path) {
        path = (hibernatedFolder() as NSString).appendingPathComponent("\(name) \(n).bbhib")
        n += 1
    }
    return path
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
        if dropWithValue.contains(a) || (a == "--resume" && resumeTakesFile(args, at: i)) {
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

/// --resume at args[i] names a snapshot (the next argument, a .bbhib
/// file), rather than standing alone for the newest.
func resumeTakesFile(_ args: [String], at i: Int) -> Bool {
    i + 1 < args.count && args[i + 1].lowercased().hasSuffix(".bbhib")
}

/// The Mac the machine presents, in words.
func presentedMachine() -> String {
    if hw_machine_chosen(), let m = hw_machine() { return String(cString: m.pointee.title) }
    switch machine_gestalt_type() {
    case 5: return "Macintosh SE"
    case 6: return "Macintosh II"
    case 11: return "Macintosh IIci"
    default: return "Macintosh Quadra 800"
    }
}

/// The screen as it is now, small, for the snapshot's record.
func screenThumbnail() -> Data? {
    var png: UnsafeMutablePointer<UInt8>?
    var len = 0
    guard machine_screen_thumbnail(SnapshotThumbnail.width, &png, &len), let png else { return nil }
    defer { free(png) }
    return Data(bytes: png, count: len)
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
                                 depth: ctx?.depth, display: ctx?.display, machine: presentedMachine(),
                                 thumbnail: screenThumbnail())
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

/// The registry's entry for a hibernated program's new snapshot: kept when
/// the process ends, and after the snapshot is resumed, so opening the
/// program offers it.  A run already entered changes its own entry; a
/// headless one (never entered) makes one.  The program's oldest snapshots
/// beyond those it keeps then go (to the Trash).
func enterHibernated(app: String, title: String, snapshot: String) {
    let registry = hibernationRegistry()
    var program = RunningProgram.canonical(app)
    if let r = Registration.current {
        program = r.entry.program
        r.hibernated(snapshot: snapshot)
        r.end(keeping: true)
        Registration.current = nil
    } else {
        let executable = Bundle.main.executableURL?.resolvingSymlinksInPath().path ?? CommandLine.arguments[0]
        let entry = RunningProgram(program: program, executable: executable, title: title, launchedBy: .commandLine)
        registry.exclusively {
            guard let r = try? registry.register(entry) else { return }
            r.hibernated(snapshot: snapshot)
            r.end(keeping: true)
        }
    }
    let keep = (settings(for: app) ?? AppSettings()).over(defaultSettings()).keepSnapshots ?? HibernatedSnapshots.defaultKeep
    for gone in HibernatedSnapshots.tidy(program: program, keep: keep, folders: hibernatedFolders(), registry: registry) {
        print("snapshot deleted (\(title) keeps its newest \(keep)): \(gone)")
    }
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

/// A run of a program (`program`, as the registry knows it) that may have
/// hibernated snapshots: one resumed, the program launched fresh, or
/// neither (the picker cancelled; a headless run's snapshot this build
/// cannot resume).  Returns when the run launches the program fresh;
/// otherwise resumes it (runCommand, never returning) or ends.  `extra`:
/// the run's arguments but the application; `named`: --resume FILE's.
func resumeIfHibernated(app: String, program: String, extra: [String], resumes: Bool, named: String?, fresh: Bool,
                        newCopy: Bool, interactive: Bool) {
    let registry = hibernationRegistry()
    let title = (app as NSString).lastPathComponent
    let asked = named != nil || (resumes && !fresh && !newCopy)
    let snapshots = asked ? HibernatedSnapshots.snapshots(of: program, registry: registry).map(\.path) : []
    let choice = ResumePolicy.choose(snapshots: snapshots, resumes: resumes, interactive: interactive, fresh: fresh,
                                     newCopy: newCopy, named: named)
    switch choice {
    case .launch:
        return
    case .missing(let snapshot):
        if named != nil { fail("\(snapshot): no such snapshot") }
        eprint("bluebox: \(title)'s snapshot \(snapshot) is gone; starting it fresh\n")
        registry.forget(snapshot: snapshot)
    case .ask(let snapshot, let reason):
        print("\(title) is hibernated in \(snapshot), which cannot be resumed here: \(reason).")
        print("It is kept. --fresh starts \(title) fresh; bluebox snapshots delete deletes it.")
        exit(1)
    case .pick:
        switch pickSnapshot(program: program, title: title, registry: registry) {
        case .resume(let snapshot):
            resumeSnapshot(snapshot, app: app, title: title, extra: extra, interactive: interactive)
        case .startFresh:
            // A run that starts again at another depth starts fresh too.
            setenv(startFreshVariable, "1", 1)
            print("\(title): starting fresh; its snapshots are kept")
        case .cancel:
            Registration.current?.end()
            exit(0)
        }
    case .resume(let snapshot):
        resumeSnapshot(snapshot, app: app, title: title, extra: extra, interactive: interactive)
    }
}

/// Set (to 1) for a run that is to start fresh without asking: the shell's
/// Start Fresh for a program hibernated from its own bundle, and a run
/// started again at another depth after the picker's Start Fresh.
let startFreshVariable = "BLUEBOX_START_FRESH"

private func resumeSnapshot(_ snapshot: String, app: String, title: String, extra: [String], interactive: Bool) -> Never {
    let (record, _) = readHibernateRecord(snapshot)
    resumeRecord = record
    var more = extra
    // As it was shown: one hibernated in the background resumes there.
    if interactive && record.display == "background" && !more.contains("--background") { more.append("--background") }
    print("resuming \(title) from \(snapshot)")
    runCommand([app] + record.args.dropFirst() + more, resume: snapshot)
    exit(0)
}

/// The snapshot picker: the program's snapshots, newest first, each with
/// its thumbnail, when it hibernated and on what; Resume, Start Fresh, Move
/// to Trash and Cancel.  A snapshot another build saved is listed, says
/// so, and can only be deleted.
private func pickSnapshot(program: String, title: String, registry: ProgramRegistry) -> SnapshotPicker.Choice {
    #if canImport(AppKit)
    return MainActor.assumeIsolated {
        let model = SnapshotPickerModel(SnapshotPicker(program: program, title: title, registry: registry), registry: registry)
        if model.picker.rows.isEmpty { return .startFresh }
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        app.activate(ignoringOtherApps: true)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 420),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.title = title
        window.contentView = NSHostingView(rootView: SnapshotPickerView(model: model))
        window.center()
        let closer = PickerWindowDelegate()
        window.delegate = closer
        window.makeKeyAndOrderFront(nil)
        withExtendedLifetime(closer) { _ = app.runModal(for: window) }
        window.orderOut(nil)
        return model.choice ?? .cancel
    }
    #else
    return .startFresh
    #endif
}

#if canImport(AppKit)
@MainActor final class SnapshotPickerModel: ObservableObject {
    @Published var picker: SnapshotPicker
    @Published var message: String?
    let registry: ProgramRegistry
    private(set) var choice: SnapshotPicker.Choice?

    init(_ picker: SnapshotPicker, registry: ProgramRegistry) {
        self.picker = picker
        self.registry = registry
    }

    func finish(_ c: SnapshotPicker.Choice) {
        choice = c
        NSApp.stopModal()
    }

    func resume(_ path: String? = nil) {
        if let path { picker.selection = path }
        if let c = picker.resume() { finish(c) }
    }

    func trash() {
        do {
            try picker.deleteSelected(registry: registry)
            message = nil
        } catch {
            message = "It could not be moved to the Trash: \(error.localizedDescription)"
        }
    }
}

/// Closing the picker's window is Cancel.
final class PickerWindowDelegate: NSObject, NSWindowDelegate {
    func windowWillClose(_ notification: Notification) {
        MainActor.assumeIsolated { NSApp.stopModal() }
    }
}

struct SnapshotPickerView: View {
    @ObservedObject var model: SnapshotPickerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("\(model.picker.title) is hibernated").font(.headline)
            Text("Choose where to go on from, or start it fresh. The snapshots are kept until you move them to the Trash.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if model.picker.rows.isEmpty {
                Text("No snapshots left").foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(model.picker.rows, selection: $model.picker.selection) { row in
                    HStack(spacing: 12) {
                        SnapshotThumbnailView(png: row.thumbnail)
                        VStack(alignment: .leading, spacing: 3) {
                            Text(row.when).font(.body.weight(.medium))
                            Text(row.details).font(.caption).foregroundStyle(.secondary)
                            if let p = row.problem {
                                Text("Cannot be resumed here: \(p)").font(.caption).foregroundStyle(.red)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                        }
                        Spacer()
                    }
                    .padding(.vertical, 2)
                    .contentShape(Rectangle())
                    .help(row.path)
                    .tag(row.path)
                    .onTapGesture(count: 2) { model.resume(row.path) }
                    .onTapGesture { model.picker.selection = row.path }
                }
            }
            if let m = model.message { Text(m).font(.caption).foregroundStyle(.red) }
            HStack {
                Button("Move to Trash") { model.trash() }
                    .disabled(model.picker.selected == nil)
                    .help("Move the chosen snapshot to the Trash (you can put it back from there)")
                Spacer()
                Button("Cancel") { model.finish(.cancel) }.keyboardShortcut(.cancelAction)
                Button("Start Fresh") { model.finish(.startFresh) }
                    .help("Launch it anew; the snapshots are kept")
                Button("Resume") { model.resume() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!model.picker.canResume)
            }
        }
        .padding(16)
        .frame(minWidth: 460, minHeight: 300)
    }
}

/// A snapshot's thumbnail, or for one without (saved before they were
/// kept) a placeholder of the same size.
struct SnapshotThumbnailView: View {
    let png: Data?
    static let size = CGSize(width: 128, height: 96)

    var body: some View {
        Group {
            if let png, let image = NSImage(data: png) {
                Image(nsImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
            } else {
                ZStack {
                    RoundedRectangle(cornerRadius: 4).fill(Color.secondary.opacity(0.15))
                    Image(systemName: "moon.zzz").font(.title2).foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: Self.size.width, height: Self.size.height)
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.secondary.opacity(0.4), lineWidth: 0.5))
    }
}
#endif

/// bluebox snapshots [list] | delete <snapshot|program>... [--permanently]
/// | thumbnail <snapshot> <file.png>: the snapshots kept, deleting them (to
/// the Trash), and the picture of the screen one carries.
func snapshotsCommand(_ args: [String]) {
    let registry = hibernationRegistry()
    let all = HibernatedSnapshots.list(folders: hibernatedFolders(), registry: registry)
    switch args.first ?? "list" {
    case "list":
        if all.isEmpty { print("no hibernated snapshots (in \(hibernatedFolders().joined(separator: ", ")))") }
        for s in all {
            let state = s.problem.map { "unreadable: \($0)" }
                ?? (!s.sameBuild ? "another build's" : s.entered ? "opening \(s.title) offers it" : "not offered (resume it by name)")
            let thumb = s.thumbnail.flatMap(SnapshotThumbnail.size).map { "thumbnail \($0.width)×\($0.height)" } ?? "no thumbnail"
            print("\(s.path)\n    \(s.title), \(SnapshotFile.label(s.saved)), \(s.details), \(thumb), \(state)")
        }
    case "thumbnail":
        guard args.count == 3 else { usage() }
        let path = URL(fileURLWithPath: args[1]).standardizedFileURL.path
        let (record, _) = readHibernateRecord(path)
        guard let png = record.thumbnail else { fail("\(path): saved without a thumbnail") }
        do { try png.write(to: URL(fileURLWithPath: args[2])) } catch { fail("\(args[2]): \(error.localizedDescription)") }
        let size = SnapshotThumbnail.size(png)
        print("\(args[2]): \(size.map { "\($0.width)×\($0.height)" } ?? "?"), \(png.count) bytes")
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
