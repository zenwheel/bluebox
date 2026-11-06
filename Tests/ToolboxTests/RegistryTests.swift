import BlueBoxHost
import Foundation
import Testing

extension BlueBox {
    /// The registry of running programs: entries made, changed and removed,
    /// a dead process's found by its lock, and the shell's view of them.
    @Suite(.serialized) struct Registry {
        /// A registry of its own in a temporary folder, removed after.
        func withRegistry(_ body: (ProgramRegistry) throws -> Void) rethrows {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-registry-\(UUID().uuidString)").path
            defer { try? FileManager.default.removeItem(atPath: root) }
            try body(ProgramRegistry(library: root + "/Blue Box/Library"))
        }

        func entry(_ program: String, pid: Int32 = getpid(), started: Date = Date()) -> RunningProgram {
            RunningProgram(program: program, pid: pid, executable: "/usr/local/bin/bluebox", started: started,
                           title: (program as NSString).lastPathComponent, launchedBy: .commandLine)
        }

        func files(_ registry: ProgramRegistry) -> [String] {
            ((try? FileManager.default.contentsOfDirectory(atPath: registry.directory)) ?? [])
                .filter { !$0.hasPrefix(".") }.sorted()
        }

        /// Beside the library; entered, found by its path, and gone when it
        /// ends.
        @Test func registerAndEnd() throws {
            try withRegistry { registry in
                #expect(registry.directory.hasSuffix("/Blue Box/Running"))
                #expect(registry.entries().isEmpty)
                let r = try registry.register(entry("/lib/Games/Apeiron"))
                #expect(files(registry) == [r.name + ".json", r.name + ".lock"])
                let found = registry.entries()
                #expect(found.count == 1 && found.first == r.entry)
                #expect(found.first?.pid == getpid() && found.first?.state == .running)
                #expect(registry.entry(for: "/lib/Games/./Apeiron")?.program == "/lib/Games/Apeiron")
                #expect(registry.entry(for: "/lib/Games/Bolo") == nil)
                // The JSON as written: the fields hibernation will add to.
                let data = try #require(FileManager.default.contents(atPath: registry.directory + "/" + r.name + ".json"))
                let json = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
                #expect(Set(json.keys) == ["program", "pid", "executable", "state", "started", "title", "launchedBy"])
                #expect(json["state"] as? String == "running" && json["launchedBy"] as? String == "commandLine")
                r.end()
                #expect(files(registry).isEmpty && registry.entries().isEmpty)
                r.end() // twice is nothing
            }
        }

        /// Suspended and resumed: the entry rewritten whole, the lock kept.
        @Test func stateChanges() throws {
            try withRegistry { registry in
                let r = try registry.register(entry("/lib/Bolo"))
                defer { r.end() }
                r.setState(.suspended)
                #expect(registry.entries().first?.state == .suspended)
                #expect(files(registry).count == 2) // no temporary left
                r.setState(.running)
                #expect(registry.entries().first?.state == .running)
                // Hibernated, the entry stays after the process lets go.
                r.hibernated(snapshot: "/snapshots/Bolo.bbhib")
                r.end(keeping: true)
                let kept = registry.entries()
                #expect(kept.count == 1 && kept.first?.state == .hibernated && kept.first?.snapshot == "/snapshots/Bolo.bbhib")
                #expect(registry.entry(for: "/lib/Bolo") == nil) // not running
            }
        }

        /// A process killed holding its lock: its entry is alive while it
        /// lives, and found dead and removed after.
        @Test func staleAfterKill() throws {
            try withRegistry { registry in
                try FileManager.default.createDirectory(atPath: registry.directory, withIntermediateDirectories: true)
                let name = "child"
                let lock = registry.directory + "/" + name + ".lock"
                let child = Process()
                child.executableURL = URL(fileURLWithPath: "/usr/bin/perl")
                child.arguments = ["-MFcntl=:flock", "-e",
                                   "$| = 1; open(my $f, '>>', $ARGV[0]) or die; flock($f, LOCK_EX) or die; print \"locked\\n\"; sleep 60",
                                   lock]
                let out = Pipe()
                child.standardOutput = out
                try child.run()
                defer { if child.isRunning { child.terminate() } }
                let line = String(data: out.fileHandleForReading.availableData, encoding: .utf8)
                #expect(line == "locked\n")
                let e = entry("/lib/Apps/apple", pid: child.processIdentifier)
                let data = try JSONEncoder.iso8601.encode(e)
                try data.write(to: URL(fileURLWithPath: registry.directory + "/" + name + ".json"))
                #expect(registry.entries().map(\.pid) == [child.processIdentifier])
                kill(child.processIdentifier, SIGKILL)
                child.waitUntilExit()
                #expect(registry.entries().isEmpty)
                #expect(files(registry).isEmpty)
            }
        }

        /// One process's entries are each held; a lock file with no entry is
        /// left a minute (its run may be writing it).
        @Test func severalAndOrphans() throws {
            try withRegistry { registry in
                let a = try registry.register(entry("/lib/A", started: Date(timeIntervalSince1970: 2)))
                let b = try registry.register(entry("/lib/B", started: Date(timeIntervalSince1970: 1)))
                #expect(registry.entries().map(\.program) == ["/lib/B", "/lib/A"]) // oldest first
                let orphan = registry.directory + "/orphan.lock"
                FileManager.default.createFile(atPath: orphan, contents: nil)
                _ = registry.entries()
                #expect(FileManager.default.fileExists(atPath: orphan))
                try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSinceNow: -120)], ofItemAtPath: orphan)
                _ = registry.entries()
                #expect(!FileManager.default.fileExists(atPath: orphan))
                a.end()
                b.end()
            }
        }

        /// How a run was started, from its environment.
        @Test func launchers() {
            #expect(RunningProgram.launcher(environment: ["BLUEBOX_LAUNCHED_BY": "shell"], ownBundle: nil) == (.shell, nil))
            #expect(RunningProgram.launcher(environment: [:], ownBundle: nil) == (.commandLine, nil))
            #expect(RunningProgram.launcher(environment: ["__CFBundleIdentifier": "local.bluebox.simpletext"], ownBundle: nil)
                    == (.bundle, "local.bluebox.simpletext"))
            // The bundle a terminal's run starts itself again from is not one.
            #expect(RunningProgram.launcher(environment: ["__CFBundleIdentifier": "local.bluebox.run.simpletext"], ownBundle: nil)
                    == (.commandLine, nil))
            #expect(RunningProgram.launcher(environment: ["__CFBundleIdentifier": "com.apple.Terminal"],
                                            ownBundle: "local.bluebox.moose.background") == (.bundle, "local.bluebox.moose.background"))
        }

        /// A headless run (census, menu walk, screenshot, printing to a
        /// file) is not entered; a windowed or background one is.
        @Test func headlessRunsDoNotRegister() {
            #expect(runRegisters(headless: false, census: false, walkMenus: false, screenshot: false, printToFile: false))
            #expect(!runRegisters(headless: true, census: false, walkMenus: false, screenshot: false, printToFile: false))
            #expect(!runRegisters(headless: false, census: true, walkMenus: false, screenshot: false, printToFile: false))
            #expect(!runRegisters(headless: true, census: true, walkMenus: true, screenshot: false, printToFile: false))
            #expect(!runRegisters(headless: false, census: false, walkMenus: true, screenshot: false, printToFile: false))
            #expect(!runRegisters(headless: false, census: false, walkMenus: false, screenshot: true, printToFile: false))
            #expect(!runRegisters(headless: false, census: false, walkMenus: false, screenshot: false, printToFile: true))
        }
    }
}

extension JSONEncoder {
    static var iso8601: JSONEncoder {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        return e
    }
}
