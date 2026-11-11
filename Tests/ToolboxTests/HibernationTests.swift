#if canImport(AppKit)
import AppKit
#endif
import BlueBoxHost
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Hibernated programs as the host sees them: when Hibernate can be
    /// chosen, its menu item and overlay button, the registry's hibernated
    /// entries and the shell's view of them, what opening a hibernated
    /// program does, another build's snapshot, and listing and deleting
    /// snapshots.
    @Suite(.serialized) struct Hibernation {
        func withFolder(_ body: (String) throws -> Void) rethrows {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-hibernation-\(UUID().uuidString)").path
            try? FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(atPath: root) }
            try body(root)
        }

        func record(_ title: String, saved: Date = Date(timeIntervalSince1970: 1_800_000_000)) -> HibernateRecord {
            HibernateRecord(args: ["/lib/" + title, "--jit"], program: "/lib/" + title, title: title, saved: saved,
                            instructions: 9_000_000, drainedAt: "WaitNextEvent", screen: [1024, 768], depth: 8,
                            display: "rootless")
        }

        /// A snapshot file's head as hibernate.c writes it (BBHIBZ01, this
        /// build's UUID or another's, the record), with no snapshot after.
        func writeSnapshot(_ path: String, _ record: HibernateRecord, sameBuild: Bool = true) {
            var id = [CChar](repeating: 0, count: 33)
            hb_build_id(&id)
            let hex = String(decoding: id.prefix(32).map { UInt8(bitPattern: $0) }, as: UTF8.self)
            var uuid = stride(from: 0, to: 32, by: 2).map { i -> UInt8 in
                let s = hex.index(hex.startIndex, offsetBy: i)
                return UInt8(hex[s..<hex.index(s, offsetBy: 2)], radix: 16)!
            }
            if !sameBuild { uuid[0] ^= 0xFF }
            let extra = Data(record.json.utf8)
            var d = Data("BBHIBZ01".utf8)
            func le<T: FixedWidthInteger>(_ v: T) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
            le(UInt32(1))
            le(UInt32(0))
            d.append(contentsOf: uuid)
            le(UInt64(0))
            le(UInt64(0))
            le(UInt64(extra.count))
            d.append(extra)
            FileManager.default.createFile(atPath: path, contents: d)
        }

        func entry(_ program: String, started: Date = Date()) -> RunningProgram {
            RunningProgram(program: program, executable: "/usr/local/bin/bluebox", started: started,
                           title: (program as NSString).lastPathComponent, launchedBy: .shell)
        }

        /// Enter a program and hibernate it at a date, as a process does.
        func hibernate(_ registry: ProgramRegistry, _ program: String, snapshot: String, at date: Date) throws {
            let r = try registry.register(entry(program))
            r.hibernated(snapshot: snapshot, at: date)
            r.end(keeping: true)
        }

        // MARK: Choosing Hibernate

        /// Running, it is offered (choosing it suspends first and the drain
        /// decides); suspended, once stopped at a safe point; while stopping,
        /// while hibernating, or stopped where it cannot be saved, not, with
        /// why not.
        @Test func availability() {
            let going = Int32(MACHINE_DRAIN_GOING), parked = Int32(MACHINE_DRAIN_PARKED)
            let refused = Int32(MACHINE_DRAIN_REFUSED)
            func eval(_ suspended: Bool, _ isParked: Bool, _ state: Int32, _ why: String = "", busy: Bool = false) -> HibernateAvailability {
                HibernateAvailability.evaluate(suspended: suspended, parked: isParked, drainState: state, reason: why,
                                               hibernating: busy)
            }
            #expect(eval(false, false, Int32(MACHINE_DRAIN_NONE)) == .available)
            #expect(eval(true, true, parked) == .available)
            #expect(eval(true, false, going).reason == "Stopping at a safe point…")
            #expect(eval(false, false, going, busy: true).reason == "Hibernating…")
            let open = eval(true, true, refused, "the program has a MacTCP stream open")
            #expect(!open.enabled && open.reason == "It cannot be hibernated now: the program has a MacTCP stream open.")
            #expect(eval(true, true, refused).reason?.contains("resume it and try again") == true)
            // Suspended where the drain gave up (it parks anywhere then).
            #expect(!eval(true, true, refused, "the program did not come to a point where it could be saved").enabled)
        }

        #if canImport(AppKit)
        final class Target: NSObject {
            @objc func quit(_ sender: Any?) {}
            @objc func suspend(_ sender: Any?) {}
            @objc func force(_ sender: Any?) {}
            @objc func hibernate(_ sender: Any?) {}
            @objc func resume(_ sender: Any?) {}
        }
        let target = Target()

        /// Hibernate after Suspend, before Quit; disabled with its reason as
        /// the tooltip, and updated where it is in a menu.
        @Test @MainActor func menuItem() {
            let m = HostQuitItems.items(title: "TeachText", suspended: false, target: target,
                                        quit: #selector(Target.quit(_:)), suspend: #selector(Target.suspend(_:)),
                                        forceQuit: #selector(Target.force(_:)), hibernate: #selector(Target.hibernate(_:)))
            #expect(m.map(\.title) == ["Suspend TeachText", "Hibernate TeachText", "", "Quit TeachText",
                                       "Force Quit TeachText"])
            #expect(m[1].action == #selector(Target.hibernate(_:)) && m[1].target === target && m[1].isEnabled)
            #expect(m[1].keyEquivalent == "h" && m[1].keyEquivalentModifierMask == [.control, .option, .command])
            #expect(!m[1].isAlternate)
            let menu = NSMenu()
            menu.autoenablesItems = false
            let sub = NSMenu()
            m.forEach { sub.addItem($0) }
            let holder = NSMenuItem(title: "TeachText", action: nil, keyEquivalent: "")
            holder.submenu = sub
            menu.addItem(holder)
            HostQuitItems.update(menu, hibernate: #selector(Target.hibernate(_:)), .unavailable("It is printing."))
            #expect(!m[1].isEnabled && m[1].toolTip == "It is printing.")
            #expect(m[0].isEnabled) // Suspend is left alone
            HostQuitItems.update(menu, hibernate: #selector(Target.hibernate(_:)), .available)
            #expect(m[1].isEnabled && m[1].toolTip?.contains("resumes it") == true)
            // In a status item's menu: no keys.
            let s = HostQuitItems.items(title: "Moose", suspended: true, target: target, quit: #selector(Target.quit(_:)),
                                        suspend: #selector(Target.suspend(_:)), forceQuit: #selector(Target.force(_:)),
                                        hibernate: #selector(Target.hibernate(_:)),
                                        hibernation: .unavailable("Stopping at a safe point…"), quitKeys: false)
            #expect(s.map(\.title) == ["Resume Moose", "Hibernate Moose", "Quit Moose"])
            #expect(!s[1].isEnabled && s.allSatisfy { $0.keyEquivalent.isEmpty })
        }

        /// The overlay's Hibernate, between Resume and Hide, only when the
        /// host offers it.
        @Test @MainActor func overlayButton() {
            final class Count { var resumed = 0, hibernated = 0 }
            let count = Count()
            let view = NSView(frame: NSRect(x: 0, y: 0, width: 400, height: 300))
            let overlay = SuspendedOverlay(frame: view.bounds, resume: { count.resumed += 1 },
                                           hibernate: { count.hibernated += 1 }, hide: {})
            view.addSubview(overlay)
            view.layoutSubtreeIfNeeded()
            #expect(overlay.hibernateButton.title == "Hibernate" && !overlay.hibernateButton.isHidden)
            #expect(overlay.resumeButton.frame.minY > overlay.hibernateButton.frame.minY)
            #expect(overlay.hibernateButton.frame.minY > overlay.hideButton.frame.minY)
            overlay.hibernateButton.performClick(nil)
            #expect(count.hibernated == 1 && count.resumed == 0)
            overlay.show(.unavailable("It cannot be hibernated now: the program is speaking."))
            #expect(!overlay.hibernateButton.isEnabled)
            #expect(overlay.hibernateButton.toolTip == "It cannot be hibernated now: the program is speaking.")
            overlay.show(.available)
            #expect(overlay.hibernateButton.isEnabled)
            let plain = SuspendedOverlay(frame: view.bounds, resume: {}, hide: {})
            #expect(plain.hibernateButton.isHidden)
        }

        /// The status item's section of hibernated programs.
        @Test @MainActor func statusSection() {
            #expect(HibernatedMenuSection.items([], target: target, resume: #selector(Target.resume(_:))).isEmpty)
            let at = Date(timeIntervalSince1970: 1_800_000_000)
            let m = HibernatedMenuSection.items([("/lib/Apeiron", "Apeiron", at)], target: target,
                                                resume: #selector(Target.resume(_:)))
            #expect(m.map(\.title) == ["Hibernated", "Apeiron", ""])
            #expect(!m[0].isEnabled && m[2].isSeparatorItem)
            #expect(m[1].representedObject as? String == "/lib/Apeiron" && m[1].action == #selector(Target.resume(_:)))
            #expect(m[1].toolTip?.hasPrefix("Hibernated 20") == true)
        }
        #endif

        // MARK: The registry and the shell

        /// Hibernated: the entry stays after its process, with when and
        /// where; the newest of a program's comes first; forgetting a
        /// snapshot removes its entries and leaves the others.
        @Test func registryTransitions() throws {
            try withFolder { root in
                let registry = ProgramRegistry(library: root + "/Blue Box/Library")
                let live = try registry.register(entry("/lib/Bolo"))
                live.setState(.suspended)
                let t1 = Date(timeIntervalSince1970: 1_800_000_000), t2 = t1.addingTimeInterval(60)
                try hibernate(registry, "/lib/TeachText", snapshot: root + "/a.bbhib", at: t1)
                try hibernate(registry, "/lib/TeachText", snapshot: root + "/b.bbhib", at: t2)
                let all = registry.entries()
                #expect(all.count == 3)
                let h = registry.hibernated(for: "/lib/./TeachText")
                #expect(h.map(\.snapshot) == [root + "/b.bbhib", root + "/a.bbhib"])
                #expect(h[0].hibernatedAt == t2 && h[0].state == .hibernated)
                #expect(registry.entry(for: "/lib/TeachText") == nil) // not running
                #expect(registry.forget(snapshot: root + "/b.bbhib") == 1)
                #expect(registry.hibernated(for: "/lib/TeachText").map(\.snapshot) == [root + "/a.bbhib"])
                #expect(registry.entry(for: "/lib/Bolo")?.state == .suspended)
                live.end()
                #expect(registry.entries().count == 1)
            }
        }

        /// The shell: a hibernated program is neither running nor suspended,
        /// its button resumes it, and a copy running (--new) is running.
        @Test func shellStates() {
            let t1 = Date(timeIntervalSince1970: 1_800_000_000)
            var old = entry("/lib/TeachText")
            old.state = .hibernated
            old.snapshot = "/s/a.bbhib"
            old.hibernatedAt = t1
            var new = old
            new.snapshot = "/s/b.bbhib"
            new.hibernatedAt = t1.addingTimeInterval(5)
            new.pid = 1
            var s = ProgramStates()
            s.registry([new, old])
            #expect(s.isHibernated("/lib/TeachText") && !s.isRunning("/lib/TeachText"))
            #expect(!s.isSuspended("/lib/TeachText"))
            #expect(s.hibernated["/lib/TeachText"]?.snapshot == "/s/b.bbhib") // the newest
            #expect(s.snapshots["/lib/TeachText"] == ["/s/b.bbhib", "/s/a.bbhib"]) // all, newest first
            #expect(s.action(for: "/lib/TeachText") == .wake && s.action(for: "/lib/TeachText").title == "Resume")
            #expect(s.open("/lib/TeachText") == .launch) // the run resumes it
            #expect(s.hibernatedPaths == ["/lib/TeachText"] && s.runningPaths.isEmpty)
            s.launched("/lib/TeachText", pid: 4242) // another copy
            #expect(!s.isHibernated("/lib/TeachText") && s.action(for: "/lib/TeachText") == .suspend)
            #expect(s.hibernatedPaths.isEmpty)
            s.ended("/lib/TeachText")
            #expect(s.isHibernated("/lib/TeachText"))
        }

        // MARK: Opening a hibernated program

        /// An interactive run with snapshots asks which (all of them, newest
        /// first); a headless one with --resume takes the newest, or the one
        /// named; --fresh and --new launch, keeping them.
        @Test func resumeOrFresh() {
            let snaps = ["/s/b.bbhib", "/s/a.bbhib"]
            func choose(_ status: SnapshotStatus, _ list: [String]? = nil, resumes: Bool = true, interactive: Bool = false,
                        fresh: Bool = false, new: Bool = false, named: String? = nil) -> ResumeChoice {
                ResumePolicy.choose(snapshots: list ?? snaps, resumes: resumes, interactive: interactive, fresh: fresh,
                                    newCopy: new, named: named) { _ in status }
            }
            #expect(choose(.resumable, interactive: true) == .pick(snaps))
            #expect(choose(.cannotResume(SnapshotProblem.otherBuild), interactive: true) == .pick(snaps)) // it says so
            #expect(choose(.resumable) == .resume(snapshot: "/s/b.bbhib"))             // --resume: the newest
            #expect(choose(.resumable, []) == .launch)
            #expect(choose(.resumable, [], interactive: true) == .launch)
            #expect(choose(.resumable, resumes: false) == .launch)                      // headless, without --resume
            #expect(choose(.resumable, interactive: true, new: true) == .launch)        // --new: kept
            #expect(choose(.resumable, interactive: true, fresh: true) == .launch)      // --fresh: kept too
            #expect(choose(.missing) == .missing(snapshot: "/s/b.bbhib"))
            #expect(choose(.cannotResume(SnapshotProblem.otherBuild))
                == .ask(snapshot: "/s/b.bbhib", reason: SnapshotProblem.otherBuild))
            // --resume FILE: that one, whatever else there is.
            #expect(choose(.resumable, [], named: "/s/a.bbhib") == .resume(snapshot: "/s/a.bbhib"))
            #expect(choose(.missing, named: "/s/c.bbhib") == .missing(snapshot: "/s/c.bbhib"))
        }

        /// A record with a thumbnail and the machine, written and read back;
        /// a PNG's size from its header.
        @Test func thumbnailRecords() {
            withFolder { root in
                let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52,
                                0, 0, 0, 160, 0, 0, 0, 120, 8, 2, 0, 0, 0])
                #expect(SnapshotThumbnail.size(png)?.width == 160 && SnapshotThumbnail.size(png)?.height == 120)
                #expect(SnapshotThumbnail.size(Data("not a png at all, no".utf8)) == nil)
                var r = record("TeachText")
                r.machine = "Macintosh Quadra 800"
                r.thumbnail = png
                writeSnapshot(root + "/t.bbhib", r)
                guard case .success(let back) = SnapshotFile.read(root + "/t.bbhib") else {
                    Issue.record("not read")
                    return
                }
                #expect(back.record == r && back.record.thumbnail == png)
            }
        }

        /// The screen's thumbnail: at most 160 pixels wide, its height in
        /// proportion, a PNG the host reads, each pixel the screen's average
        /// under it.
        @Test func screenThumbnail() {
            _ = launchTestApp()
            var w: Int32 = 0, h: Int32 = 0
            machine_screen_size(&w, &h)
            var png: UnsafeMutablePointer<UInt8>?
            var len = 0
            #expect(machine_screen_thumbnail(SnapshotThumbnail.width, &png, &len))
            guard let png else { return }
            let data = Data(bytes: png, count: len)
            free(png)
            let size = SnapshotThumbnail.size(data)
            #expect(size?.width == 160)
            #expect(size?.height == Int((Int64(h) * 160 + Int64(w) / 2) / Int64(w)))
            #expect(len < 160 * 120 * 3) // deflated
            #if canImport(AppKit)
            let rep = NSBitmapImageRep(data: data)
            #expect(rep?.pixelsWide == 160)
            #endif
        }

        /// One program's snapshots, newest first (its entries'; one whose
        /// file is gone forgotten); the picker's rows, its choice, its
        /// thumbnails and placeholders, and deleting from it.
        @Test func pickerModel() throws {
            try withFolder { root in
                let folder = root + "/Blue Box/Hibernated"
                try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
                let registry = ProgramRegistry(library: root + "/Blue Box/Library")
                let t = Date(timeIntervalSince1970: 1_800_000_000)
                let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52,
                                0, 0, 0, 160, 0, 0, 0, 120, 8, 2, 0, 0, 0])
                func make(_ name: String, _ at: Date, thumbnail: Bool = true, sameBuild: Bool = true) throws {
                    var r = record("TeachText", saved: at)
                    r.thumbnail = thumbnail ? png : nil
                    r.machine = "Macintosh Quadra 800"
                    writeSnapshot(folder + "/" + name, r, sameBuild: sameBuild)
                    try hibernate(registry, "/lib/TeachText", snapshot: folder + "/" + name, at: at)
                }
                try make("one.bbhib", t, thumbnail: false)                           // before thumbnails
                try make("two.bbhib", t.addingTimeInterval(60))
                try make("three.bbhib", t.addingTimeInterval(120), sameBuild: false) // another build's
                try make("four.bbhib", t.addingTimeInterval(180))
                try make("gone.bbhib", t.addingTimeInterval(240))
                try FileManager.default.removeItem(atPath: folder + "/gone.bbhib")
                try hibernate(registry, "/lib/Bolo", snapshot: folder + "/bolo.bbhib", at: t)

                let list = HibernatedSnapshots.snapshots(of: "/lib/TeachText", registry: registry)
                #expect(list.map { ($0.path as NSString).lastPathComponent } == ["four.bbhib", "three.bbhib", "two.bbhib", "one.bbhib"])
                #expect(registry.hibernated(for: "/lib/TeachText").count == 4) // the gone one forgotten

                let now = t.addingTimeInterval(3600)
                var picker = SnapshotPicker(title: "TeachText", snapshots: list, now: now, timeZone: TimeZone(identifier: "UTC")!)
                #expect(picker.rows.map(\.path) == list.map(\.path))
                #expect(picker.rows.map(\.placeholder) == [false, false, false, true])
                #expect(picker.rows.map(\.resumable) == [true, false, true, true])
                #expect(picker.rows[1].problem == SnapshotProblem.otherBuild)
                #expect(picker.rows[0].details.hasPrefix("Macintosh Quadra 800 · 1024×768, 256 colors · "))
                #expect(picker.rows[0].when == "Today 08:03" && picker.rows[3].when == "Today 08:00")
                #expect(picker.selection == folder + "/four.bbhib")                  // the newest
                #expect(picker.resume() == .resume(folder + "/four.bbhib"))
                picker.selection = folder + "/three.bbhib"
                #expect(!picker.canResume && picker.resume() == nil)                 // another build's: delete only

                // Move to Trash: gone, its entry forgotten, the next one chosen.
                try picker.deleteSelected(registry: registry, toTrash: false)
                #expect(!FileManager.default.fileExists(atPath: folder + "/three.bbhib"))
                #expect(picker.rows.count == 3 && picker.selection == folder + "/two.bbhib")
                #expect(registry.hibernated(for: "/lib/TeachText").count == 3)
                picker.selection = folder + "/one.bbhib"
                try picker.deleteSelected(registry: registry, toTrash: false)        // the last: the one before
                #expect(picker.selection == folder + "/two.bbhib")
                #expect(registry.hibernated(for: "/lib/Bolo").count == 1)            // another program's untouched

                // Loaded as the run does.
                let loaded = SnapshotPicker(program: "/lib/./TeachText", title: "TeachText", registry: registry)
                #expect(loaded.rows.map(\.path) == [folder + "/four.bbhib", folder + "/two.bbhib"])
                // Another build's only: nothing chosen that resumes, but it is listed.
                let foreign = SnapshotPicker(title: "X", snapshots: [HibernatedSnapshots.info(folder + "/four.bbhib", entered: true)])
                #expect(foreign.selection == folder + "/four.bbhib")
            }
        }

        /// Dates as the picker words them.
        @Test func pickerDates() {
            let utc = TimeZone(identifier: "UTC")!
            let now = Date(timeIntervalSince1970: 1_800_000_000) // 2027-01-15 08:00 UTC
            #expect(SnapshotPicker.when(now.addingTimeInterval(-60), now: now, timeZone: utc) == "Today 07:59")
            #expect(SnapshotPicker.when(now.addingTimeInterval(-86400), now: now, timeZone: utc) == "Yesterday 08:00")
            #expect(SnapshotPicker.when(now.addingTimeInterval(-3 * 86400), now: now, timeZone: utc) == "2027-01-12 08:00")
        }

        /// Hibernating again keeps the program's newest few: the oldest in
        /// the Hibernated folder go, one saved elsewhere stays, and another
        /// program's are not counted.
        @Test func tidying() throws {
            try withFolder { root in
                let folder = root + "/Blue Box/Hibernated", elsewhere = root + "/elsewhere"
                try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
                try FileManager.default.createDirectory(atPath: elsewhere, withIntermediateDirectories: true)
                let registry = ProgramRegistry(library: root + "/Blue Box/Library")
                let t = Date(timeIntervalSince1970: 1_800_000_000)
                for i in 0..<6 {
                    let at = t.addingTimeInterval(Double(i) * 60)
                    let dir = i == 0 ? elsewhere : folder
                    writeSnapshot(dir + "/\(i).bbhib", record("TeachText", saved: at))
                    try hibernate(registry, "/lib/TeachText", snapshot: dir + "/\(i).bbhib", at: at)
                }
                writeSnapshot(folder + "/bolo.bbhib", record("Bolo", saved: t))
                try hibernate(registry, "/lib/Bolo", snapshot: folder + "/bolo.bbhib", at: t)
                #expect(HibernatedSnapshots.tidy(program: "/lib/TeachText", keep: 5, folders: [folder], registry: registry,
                                                 toTrash: false).isEmpty)  // 5 in the folder, 1 elsewhere
                let gone = HibernatedSnapshots.tidy(program: "/lib/TeachText", keep: 3, folders: [folder],
                                                    registry: registry, toTrash: false)
                #expect(gone == [folder + "/1.bbhib", folder + "/2.bbhib"])     // oldest first
                #expect(!FileManager.default.fileExists(atPath: folder + "/1.bbhib"))
                #expect(FileManager.default.fileExists(atPath: elsewhere + "/0.bbhib"))
                #expect(registry.hibernated(for: "/lib/TeachText").compactMap(\.snapshot)
                    == [folder + "/5.bbhib", folder + "/4.bbhib", folder + "/3.bbhib", elsewhere + "/0.bbhib"])
                #expect(registry.hibernated(for: "/lib/Bolo").count == 1)
                // Never fewer than one.
                HibernatedSnapshots.tidy(program: "/lib/TeachText", keep: 0, folders: [folder], registry: registry, toTrash: false)
                #expect(registry.hibernated(for: "/lib/TeachText").compactMap(\.snapshot) == [folder + "/5.bbhib", elsewhere + "/0.bbhib"])
            }
        }

        /// A snapshot's record read back; another build's is told apart; a
        /// file that is no snapshot, or gone, says so.
        @Test func snapshotFiles() {
            withFolder { root in
                let r = record("TeachText")
                writeSnapshot(root + "/same.bbhib", r)
                writeSnapshot(root + "/other.bbhib", r, sameBuild: false)
                FileManager.default.createFile(atPath: root + "/junk.bbhib", contents: Data("not one".utf8))
                guard case .success(let same) = SnapshotFile.read(root + "/same.bbhib") else {
                    Issue.record("not read")
                    return
                }
                #expect(same.sameBuild && same.record == r)
                #expect(same.record.screen == [1024, 768] && same.record.depth == 8 && same.record.display == "rootless")
                guard case .success(let other) = SnapshotFile.read(root + "/other.bbhib") else {
                    Issue.record("not read")
                    return
                }
                #expect(!other.sameBuild && other.record.title == "TeachText")
                #expect(SnapshotFile.problem(root + "/same.bbhib") == .resumable)
                #expect(SnapshotFile.problem(root + "/other.bbhib") == .cannotResume(SnapshotProblem.otherBuild))
                #expect(SnapshotFile.problem(root + "/gone.bbhib") == .missing)
                if case .cannotResume(let why) = SnapshotFile.problem(root + "/junk.bbhib") {
                    #expect(why.contains("not a snapshot"))
                } else {
                    Issue.record("junk read as a snapshot")
                }
            }
        }

        /// M1's records (no screen, depth or display) still read.
        @Test func olderRecords() {
            let json = #"{"args":["/lib/TeachText"],"drainedAt":"WaitNextEvent","instructions":5,"program":"/lib/TeachText","#
                + #""saved":"2026-11-06T06:00:00Z","title":"TeachText","version":1}"#
            let r = HibernateRecord.decode(Data(json.utf8))
            #expect(r?.title == "TeachText" && r?.screen == nil && r?.display == nil)
            #expect(SnapshotFile.label(Date(timeIntervalSince1970: 1_793_340_000), timeZone: TimeZone(identifier: "UTC")!)
                == "Hibernated 2026-10-30 06:00")
        }

        /// The snapshots kept, newest first, with whether opening their
        /// program resumes them; deleting one forgets its entry.
        @Test func listAndDelete() throws {
            try withFolder { root in
                let folder = root + "/Blue Box/Hibernated", elsewhere = root + "/elsewhere"
                try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
                try FileManager.default.createDirectory(atPath: elsewhere, withIntermediateDirectories: true)
                let registry = ProgramRegistry(library: root + "/Blue Box/Library")
                let t = Date(timeIntervalSince1970: 1_800_000_000)
                writeSnapshot(folder + "/old.bbhib", record("Bolo", saved: t))
                writeSnapshot(folder + "/new.bbhib", record("TeachText", saved: t.addingTimeInterval(100)))
                writeSnapshot(folder + "/foreign.bbhib", record("Apeiron", saved: t.addingTimeInterval(50)), sameBuild: false)
                writeSnapshot(elsewhere + "/given.bbhib", record("Moose", saved: t.addingTimeInterval(10)))
                FileManager.default.createFile(atPath: folder + "/notes.txt", contents: Data())
                try hibernate(registry, "/lib/TeachText", snapshot: folder + "/new.bbhib", at: t)
                try hibernate(registry, "/lib/Moose", snapshot: elsewhere + "/given.bbhib", at: t)
                let list = HibernatedSnapshots.list(folders: [folder], registry: registry)
                #expect(list.map(\.title) == ["TeachText", "Apeiron", "Moose", "Bolo"])
                #expect(list.map(\.entered) == [true, false, true, false])
                #expect(list.map(\.sameBuild) == [true, false, true, true])
                #expect(list.allSatisfy { $0.bytes > 56 && $0.problem == nil })
                try HibernatedSnapshots.delete(folder + "/new.bbhib", registry: registry, toTrash: false)
                #expect(!FileManager.default.fileExists(atPath: folder + "/new.bbhib"))
                #expect(registry.hibernated(for: "/lib/TeachText").isEmpty)
                #expect(registry.hibernated(for: "/lib/Moose").count == 1)
                // An entry whose file went: deleting it still forgets the entry.
                try FileManager.default.removeItem(atPath: elsewhere + "/given.bbhib")
                try HibernatedSnapshots.delete(elsewhere + "/given.bbhib", registry: registry, toTrash: false)
                #expect(registry.hibernatedEntries().isEmpty)
                #expect(HibernatedSnapshots.list(folders: [folder], registry: registry).map(\.title) == ["Apeiron", "Bolo"])
            }
        }
    }
}
