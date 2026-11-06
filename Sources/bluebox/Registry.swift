// macOS host: a run's entry in the registry of running programs
// (ProgramRegistry), and bringing a program that is running already to the
// front.

#if canImport(AppKit)
import AppKit
import BlueBoxHost
import Toolbox

/// The registry the shell and every run share.
/// An exported application enters itself in Blue Box's registry when Blue
/// Box's folder is there (so the shell lists it), and otherwise in its own.
func defaultRegistry() -> ProgramRegistry {
    let shared = sharedLibrary()
    let there = FileManager.default.fileExists(atPath: (shared as NSString).deletingLastPathComponent)
    return ProgramRegistry(library: exportSupport() == nil || there ? shared : defaultLibrary())
}

/// Enter this run in the registry for its life.  If the program is running
/// already (and another copy was not asked for), that one is brought to
/// the front instead, resumed if it is suspended and given the documents,
/// and this run ends here.
func registerRun(app: String, documents: [String], newCopy: Bool) {
    let registry = defaultRegistry()
    let env = ProcessInfo.processInfo.environment
    let own = bundledRun() != nil
    // A bundle with its own Blue Box runs a copy of the program: it is the
    // library's program all the same.
    let program = RunningProgram.canonical(own ? Bundle.main.object(forInfoDictionaryKey: bundleAppKey) as? String ?? app : app)
    let (launcher, bundle) = RunningProgram.launcher(environment: env, ownBundle: own ? Bundle.main.bundleIdentifier : nil)
    unsetenv(RunningProgram.launchedByVariable) // not the programs this one starts
    let title = (own ? Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String : nil)
        ?? (app as NSString).lastPathComponent
    let executable = Bundle.main.executableURL?.resolvingSymlinksInPath().path ?? CommandLine.arguments[0]
    let entry = RunningProgram(program: program, executable: executable, title: title, launchedBy: launcher, bundle: bundle)
    registry.exclusively {
        if !newCopy, let other = registry.entry(for: program), other.pid != getpid() {
            bringForward(other, documents: documents)
            print("bluebox: \(other.title) is running already (process \(other.pid))"
                  + (other.state == .suspended ? "; resumed" : "") + " (--new starts another)")
            exit(0)
        }
        do {
            Registration.current = try registry.register(entry)
        } catch {
            eprint("bluebox: cannot enter \(title) in \(registry.directory): \(error.localizedDescription)\n")
        }
    }
    atexit { Registration.current?.end() }
}

/// Another run of the program: resumed, handed the documents, and brought
/// to the front.
private func bringForward(_ other: RunningProgram, documents: [String]) {
    if other.state == .suspended { SuspendNotice.ask(pid: other.pid, suspend: false) }
    if !documents.isEmpty {
        var procs = [machine_ae_process](repeating: machine_ae_process(), count: 64)
        let n = Int(procs.withUnsafeMutableBufferPointer { machine_ae_processes($0.baseAddress, 64) })
        if let target = procs.prefix(n).first(where: { $0.pid == other.pid }) {
            var result = [CChar](repeating: 0, count: 256)
            var errn: Int32 = 0
            _ = withCStringArray(documents) { paths in
                machine_ae_send_to(target.pid, target.signature, fourCCValue("aevt"), fourCCValue("odoc"), paths,
                                   Int32(documents.count), nil, false, &result, result.count, &errn)
            }
        }
    }
    MainActor.assumeIsolated { bringToFront(pid: other.pid) }
}

/// Bring a program's process to the front.  The shell, when it is the
/// active application, yields to it; otherwise (the shell's status menu, a
/// run from a terminal, which is no application) Launch Services is also
/// asked to open its bundle, which brings the running one forward.
@MainActor func bringToFront(pid: Int32) {
    guard let app = NSRunningApplication(processIdentifier: pid) else { return }
    let me = NSApp as NSApplication?
    if let me, me.isActive, #available(macOS 14, *) { me.yieldActivation(to: app) }
    app.activate(options: [.activateAllWindows])
    guard me?.isActive != true, let url = app.bundleURL else { return }
    let config = NSWorkspace.OpenConfiguration()
    config.activates = true
    let done = DispatchSemaphore(value: 0)
    NSWorkspace.shared.openApplication(at: url, configuration: config) { _, _ in done.signal() }
    if me == nil { _ = done.wait(timeout: .now() + 5) } // before this run ends
}
#endif
