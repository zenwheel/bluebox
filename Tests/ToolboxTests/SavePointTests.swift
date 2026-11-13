#if canImport(AppKit)
import AppKit
#endif
import BlueBoxHost
import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

/// What a scripted save point step asked for (the handler is a C function).
nonisolated(unsafe) private var savePointSteps: [(action: Int32, at: UInt64)] = []

extension BlueBox {
    /// Save points (reattach.c, SavePoint.swift): a snapshot taken while the
    /// program goes on, and put back over the running machine in the same
    /// process; the machine as it was, its open files with it, its scripted
    /// input after it; and how the host marks, keeps and offers them.
    @Suite(.serialized) struct SavePoints {
        func code(_ words: [UInt16]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
            return p
        }

        func start(_ words: [UInt16]) -> UInt32 {
            let pc = code(words)
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000
            cpu.pointee.pc = pc
            return pc
        }

        func ptr(_ n: UInt32) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

        /// Stopped at a safe point as the host stops it for a save point
        /// (machine_pause runs a drain that does not end machine_run; here
        /// one that does, the machine on this thread).
        func park() {
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
        }

        func take() -> (UnsafeMutablePointer<UInt8>, Int)? {
            park()
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            var buf: UnsafeMutablePointer<UInt8>?
            var len = 0
            var err = [CChar](repeating: 0, count: 256)
            let ok = machine_savepoint_take(&buf, &len, &err, err.count)
            #expect(ok, Comment(rawValue: String(cString: err)))
            machine_drain_end()
            guard ok, let buf else { return nil }
            return (buf, len)
        }

        /// Taken, the program goes on (its count and its memory move on);
        /// put back, everything is as it was (hb_compare finds nothing but
        /// the host's pointers), and it goes on again from there.
        @Test func goesOnAndComesBack() {
            launchTestApp()
            let counter = ptr(4)
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            // loop: ADDQ.L #1,counter; _SystemTask; BRA.S loop
            _ = start([0x52B9, hi(counter), lo(counter), 0xA9B4, 0x60F6])
            _ = machine_run(200_000)
            guard let (buf, len) = take() else { return }
            defer { machine_savepoint_free(buf) }
            let saved = rd32(counter), at = machine_instruction_count()
            _ = machine_run(300_000)
            #expect(rd32(counter) > saved)
            let block = ptr(64) // made since: gone again after
            wr32(block, 0x1234_5678)

            park()
            var err = [CChar](repeating: 0, count: 256)
            var differences: Int32 = -1
            #expect(machine_savepoint_restore(buf, len, &differences, &err, err.count),
                    Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(differences == 0)
            #expect(rd32(counter) == saved)
            #expect(machine_instruction_count() == at)
            _ = machine_run(300_000)
            #expect(rd32(counter) > saved) // and on it goes
            // Again, from the same save point.
            park()
            #expect(machine_savepoint_restore(buf, len, nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(rd32(counter) == saved)
        }

        /// Only where nothing of the Toolbox is on the machine's stack: not
        /// while it runs (no drain), and the machine is left as it was.
        @Test func refusedAwayFromASafePoint() {
            launchTestApp()
            _ = start([0x5287, 0xA9B4, 0x60FA]) // ADDQ.L #1,D7; _SystemTask; BRA.S loop
            _ = machine_run(100_000)
            guard let (buf, len) = take() else { return }
            defer { machine_savepoint_free(buf) }
            _ = machine_run(100_000)
            let d7 = g_cpu!.pointee.d.7
            var err = [CChar](repeating: 0, count: 256)
            #expect(!machine_savepoint_restore(buf, len, nil, &err, err.count))
            #expect(!String(cString: err).isEmpty)
            #expect(g_cpu!.pointee.d.7 == d7)
            var tbuf: UnsafeMutablePointer<UInt8>?
            var tlen = 0
            #expect(!machine_savepoint_take(&tbuf, &tlen, &err, err.count)) // nor taken
        }

        /// A file open for writing that the program changed after the save
        /// point goes back to the save point's copy (its own view of it),
        /// not gone: reading and writing it go on.
        @Test func openFilesGoBackWithIt() throws {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("scores")
            try Data().write(to: file)
            let spec = ptr(UInt32(FSSpec_SIZEOF))
            fm_spec_for_path(file.path, spec)
            let ref = ptr(80)
            #expect(tb_FSpMakeFSRef(spec, ref) == 0)
            let rnp = ptr(2), actual = ptr(4), pos = ptr(8), buf = ptr(16)
            #expect(tb_FSOpenFork(ref, 0, 0, 3, rnp) == 0)
            let rn = Int16(bitPattern: rd16(rnp))
            func write(_ s: String) {
                for (i, b) in Array(s.utf8).enumerated() { wr8(buf + UInt32(i), b) }
                wr32(pos, 0)
                wr32(pos + 4, 0)
                #expect(tb_FSWriteFork(rn, 1, pos, UInt32(s.utf8.count), buf, actual) == 0)
            }
            write("abc")
            _ = start([0xA9B4, 0x60FC]) // _SystemTask; BRA.S loop
            guard let (snap, len) = take() else { return }
            defer { machine_savepoint_free(snap) }
            #expect(try String(contentsOf: file, encoding: .utf8) == "abc")
            write("xyz")
            #expect(tb_FSFlushFork(rn) == 0) // written out
            #expect(try String(contentsOf: file, encoding: .utf8) == "xyz")

            park()
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_savepoint_restore(snap, len, nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(try String(contentsOf: file, encoding: .utf8) == "abc")
            wr32(pos, 0)
            wr32(pos + 4, 0)
            #expect(tb_FSReadFork(rn, 1, pos, 3, buf, actual) == 0)
            #expect(rd32(actual) == 3 && rd8(buf) == UInt8(ascii: "a"))
            write("def")
            #expect(tb_FSCloseFork(rn) == 0)
            #expect(try String(contentsOf: file, encoding: .utf8) == "def")
        }

        /// Scripted steps: savepoint and restore reach the host's handler
        /// as they come; after a restore the steps still to come follow
        /// the save point's step as they followed the restore's, and the
        /// keys held then are let go.
        @Test func scriptedSteps() {
            launchTestApp()
            let counter = ptr(4)
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            _ = start([0x52B9, hi(counter), lo(counter), 0xA9B4, 0x60F6])
            savePointSteps = []
            machine_set_savepoint_handler({ action, _ in
                savePointSteps.append((action, machine_instruction_count()))
                machine_drain_begin(true)
            }, nil)
            defer { machine_set_savepoint_handler(nil, nil) }
            let t0 = machine_instruction_count()
            machine_script_add(t0 + 100_000, 11, Int32(MACHINE_SAVEPOINT_SAVE), 0, 0)
            machine_script_add(t0 + 600_000, 11, Int32(MACHINE_SAVEPOINT_RESTORE), 0, 0)
            machine_script_add(t0 + 650_000, 3, 0x61, 0, 0) // keydown a, 50,000 after the restore step
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(savePointSteps.map(\.action) == [Int32(MACHINE_SAVEPOINT_SAVE)])
            var buf: UnsafeMutablePointer<UInt8>?
            var len = 0
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_savepoint_take(&buf, &len, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            defer { machine_savepoint_free(buf) }
            machine_post_key(0x62, 11, 0, 1, 0) // b held now
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(savePointSteps.map(\.action) == [Int32(MACHINE_SAVEPOINT_SAVE), Int32(MACHINE_SAVEPOINT_RESTORE)])
            #expect(machine_savepoint_restore(buf, len, nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            // Let go of b; a comes 50,000 after the save point's step.
            let keys = ptr(16)
            tb_GetKeys(keys)
            #expect(rd8(keys + 1) & 0x08 == 0) // key code 11 (b) is up
            #expect(events_script_next_at() == t0 + 150_000)
            #expect(machine_run(1_000_000) != RUN_DRAINED) // no restore step again
            #expect(savePointSteps.count == 2)
        }

        /// A step queued before the save point's but due after it (the
        /// input leading up to it: a mouse move, a key let go of) is the
        /// save point's own: after a restore it comes again as it came
        /// after the save point, before the steps carried from the restore.
        @Test func stepsQueuedBeforeTheSavePoint() {
            launchTestApp()
            let counter = ptr(4)
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            _ = start([0x52B9, hi(counter), lo(counter), 0xA9B4, 0x60F6])
            savePointSteps = []
            machine_set_savepoint_handler({ action, _ in
                savePointSteps.append((action, machine_instruction_count()))
                machine_drain_begin(true)
            }, nil)
            defer { machine_set_savepoint_handler(nil, nil) }
            let t0 = machine_instruction_count()
            machine_script_add(t0 + 120_000, 1, 300, 200, -1) // move:300/200, queued first
            machine_script_add(t0 + 100_000, 11, Int32(MACHINE_SAVEPOINT_SAVE), 0, 0)
            machine_script_add(t0 + 600_000, 11, Int32(MACHINE_SAVEPOINT_RESTORE), 0, 0)
            machine_script_add(t0 + 650_000, 3, 0x61, 0, 0)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            var buf: UnsafeMutablePointer<UInt8>?
            var len = 0
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_savepoint_take(&buf, &len, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            defer { machine_savepoint_free(buf) }
            #expect(machine_run(10_000_000) == RUN_DRAINED) // the move, then the restore step
            #expect(machine_savepoint_restore(buf, len, nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(events_script_next_at() == t0 + 120_000) // the move again
            _ = machine_run(40_000)
            var x: Int32 = 0, y: Int32 = 0
            var relative = false
            _ = machine_mouse_state(&x, &y, &relative)
            #expect(x == 300 && y == 200)
            #expect(events_script_next_at() == t0 + 150_000) // then the restore's own
        }

        /// The 68020's instruction cache is the machine's: put back as it
        /// was (a line the program's own store left stale stays stale), and
        /// the Toolbox's stores still flush its lines, which a resumed
        /// process's code map never saw.
        @Test func instructionCacheComesBack() {
            machine_set_cpu(68020)
            defer {
                machine_set_cpu(68040)
                bootMachine()
            }
            launchTestApp()
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            let target = code([0x7E01, 0x4E75]) // MOVEQ #1,D7; RTS
            let src = code([0x7E03]) // MOVEQ #3,D7
            // Code whose lines are not target's (64 lines of a long each).
            func apart(_ words: [UInt16]) -> UInt32 {
                let block = tb_NewPtr(512, false, false)
                let p = block + ((target &+ 64 &- block) & 0xFF)
                for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
                return p
            }
            // JSR target (cached); MOVE.W #$7E02,target (stale now); loop: _SystemTask; BRA.S loop
            let first = apart([0x4EB9, hi(target), lo(target), 0x33FC, 0x7E02, hi(target), lo(target), 0xA9B4, 0x60FC])
            _ = start([0x4EF9, hi(first), lo(first)]) // JMP first
            _ = machine_run(100_000)
            guard let (buf, len) = take() else { return }
            defer { machine_savepoint_free(buf) }
            _ = machine_run(100_000)
            // As a new process has it: no mark on the cached code.
            mem_code_unmark((target & ~0xFF) &- 0x100, (target & ~0xFF) + 0x100, 4)
            park()
            var err = [CChar](repeating: 0, count: 256)
            var differences: Int32 = -1
            #expect(machine_savepoint_restore(buf, len, &differences, &err, err.count),
                    Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(differences == 0)
            // MOVEQ #0,D7; JSR target; MOVE.L D7,D6; BlockMove src over it; JSR target; MOVE.L D7,D4; loop
            let after = apart([0x7E00, 0x4EB9, hi(target), lo(target), 0x2C07,
                              0x41F9, hi(src), lo(src), 0x227C, hi(target), lo(target), 0x7002, 0xA02E,
                              0x4EB9, hi(target), lo(target), 0x2807, 0xA9B4, 0x60FC])
            g_cpu!.pointee.pc = after
            _ = machine_run(100_000)
            #expect(g_cpu!.pointee.d.6 == 1) // still the cached MOVEQ #1
            #expect(g_cpu!.pointee.d.4 == 3) // BlockMove flushed it
        }

        // MARK: The host's side

        func withFolder(_ body: (String) throws -> Void) rethrows {
            let root = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-savepoints-\(UUID().uuidString)").path
            try? FileManager.default.createDirectory(atPath: root, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(atPath: root) }
            try body(root)
        }

        func record(_ title: String, saved: Date, kind: String? = nil) -> HibernateRecord {
            HibernateRecord(args: ["/lib/" + title], program: "/lib/" + title, title: title, saved: saved,
                            instructions: 9_000_000, drainedAt: "WaitNextEvent", screen: [640, 480], depth: 8,
                            display: "rootless", kind: kind)
        }

        /// A snapshot file's head as hibernate.c writes it, this build's.
        func writeHead(_ path: String, _ record: HibernateRecord) {
            var id = [CChar](repeating: 0, count: 33)
            hb_build_id(&id)
            let hex = String(decoding: id.prefix(32).map { UInt8(bitPattern: $0) }, as: UTF8.self)
            let uuid = stride(from: 0, to: 32, by: 2).map { i -> UInt8 in
                let s = hex.index(hex.startIndex, offsetBy: i)
                return UInt8(hex[s..<hex.index(s, offsetBy: 2)], radix: 16)!
            }
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

        /// A save point's record and entry say so; an older one's (no kind)
        /// is Hibernate's.
        @Test func marked() throws {
            let t = Date(timeIntervalSince1970: 1_800_000_000)
            let r = record("Glider", saved: t, kind: HibernateRecord.savePointKind)
            #expect(r.isSavePoint && HibernateRecord.decode(Data(r.json.utf8))?.isSavePoint == true)
            #expect(!record("Glider", saved: t).isSavePoint)
            #expect(HibernateRecord.decode(Data(#"{"version":1,"args":[],"program":"/a","title":"a","saved":"2027-01-15T08:00:00Z","instructions":1,"drainedAt":"x"}"#.utf8))?.isSavePoint == false)
            let utc = TimeZone(identifier: "UTC")!
            #expect(SnapshotFile.label(t, savePoint: true, timeZone: utc) == "Save point 2027-01-15 08:00")
            #expect(SnapshotFile.label(t, timeZone: utc) == "Hibernated 2027-01-15 08:00")
            var e = RunningProgram(program: "/lib/Glider", executable: "/bin/bluebox", title: "Glider", launchedBy: .shell)
            #expect(e.savePoint == nil)
            e.savePoint = true
            let back = try JSONDecoder().decode(RunningProgram.self, from: JSONEncoder().encode(e))
            #expect(back.savePoint == true)
        }

        /// Save points and hibernated snapshots are kept apart: a burst of
        /// save points never pushes out where it hibernated, and the
        /// picker offers both, saying which is which.
        @Test func keptApartAndOffered() throws {
            try withFolder { root in
                let folder = root + "/Blue Box/Hibernated"
                try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
                let registry = ProgramRegistry(library: root + "/Blue Box/Library")
                let t = Date(timeIntervalSince1970: 1_800_000_000)
                func enter(_ name: String, _ at: Date, savePoint: Bool) throws {
                    let path = folder + "/\(name).bbhib"
                    writeHead(path, record("Glider", saved: at, kind: savePoint ? HibernateRecord.savePointKind : nil))
                    let r = try registry.register(RunningProgram(program: "/lib/Glider", executable: "/bin/bluebox",
                                                                 title: "Glider", launchedBy: .shell))
                    r.hibernated(snapshot: path, at: at, savePoint: savePoint)
                    r.end(keeping: true)
                }
                try enter("h1", t, savePoint: false)
                for i in 0..<4 { try enter("s\(i)", t.addingTimeInterval(Double(i + 1) * 60), savePoint: true) }
                // Save points beyond 2 go; the hibernated one stays.
                let gone = HibernatedSnapshots.tidy(program: "/lib/Glider", keep: 2, folders: [folder], registry: registry,
                                                    savePoints: true, toTrash: false)
                #expect(gone == [folder + "/s0.bbhib", folder + "/s1.bbhib"])
                // Hibernation's tidying leaves save points alone.
                #expect(HibernatedSnapshots.tidy(program: "/lib/Glider", keep: 1, folders: [folder], registry: registry,
                                                 toTrash: false).isEmpty)
                let picker = SnapshotPicker(program: "/lib/Glider", title: "Glider", registry: registry)
                #expect(picker.rows.map(\.isSavePoint) == [true, true, false])
                #expect(picker.heading == "Glider is hibernated")
                try HibernatedSnapshots.delete(folder + "/h1.bbhib", registry: registry, toTrash: false)
                #expect(SnapshotPicker(program: "/lib/Glider", title: "Glider", registry: registry).heading
                    == "Glider has save points")
            }
        }

        #if canImport(AppKit)
        /// Control-Option-Command-S and -R are the host's, running or
        /// suspended; other chords with S and R stay the program's.
        @Test func keys() {
            let mods: NSEvent.ModifierFlags = [.control, .option, .command]
            #expect(HostKey.classify(mods, "s", suspended: false) == .savePoint)
            #expect(HostKey.classify(mods, "S", suspended: true) == .savePoint)
            #expect(HostKey.classify(mods, "r", suspended: false) == .restoreSavePoint)
            #expect(HostKey.classify(mods.union([.capsLock]), "r", suspended: false) == .restoreSavePoint)
            #expect(HostKey.classify([.command], "s", suspended: false) == .classic)
            #expect(HostKey.classify([.command, .option], "r", suspended: false) == .classic)
            #expect(HostKey.classify(mods.union([.shift]), "s", suspended: false) == .classic)
        }

        final class Target: NSObject {
            @objc func quit(_ sender: Any?) {}
            @objc func suspend(_ sender: Any?) {}
            @objc func force(_ sender: Any?) {}
            @objc func hibernate(_ sender: Any?) {}
            @objc func save(_ sender: Any?) {}
            @objc func restore(_ sender: Any?) {}
        }

        /// Make Save Point and Restore Save Point after Hibernate, with
        /// their keys (no other item's), enabled as the host says with why
        /// not; in a status item's menu without keys.
        @Test @MainActor func menuItems() {
            let target = Target()
            let sp = HostQuitItems.SavePointItems(save: #selector(Target.save(_:)), restore: #selector(Target.restore(_:)),
                                                  saving: .available, restoring: .unavailable("No save point yet."))
            let m = HostHideItems.items(title: "Glider", programHasH: false)
                + HostQuitItems.items(title: "Glider", suspended: false, target: target, quit: #selector(Target.quit(_:)),
                                      suspend: #selector(Target.suspend(_:)), forceQuit: #selector(Target.force(_:)),
                                      hibernate: #selector(Target.hibernate(_:)), savePoints: sp)
            let titles = m.map(\.title)
            #expect(titles.firstIndex(of: "Make Save Point") == titles.firstIndex(of: "Hibernate Glider")! + 1)
            #expect(titles.firstIndex(of: "Restore Save Point") == titles.firstIndex(of: "Make Save Point")! + 1)
            let keys = m.filter { !$0.keyEquivalent.isEmpty }.map { "\($0.keyEquivalent)\($0.keyEquivalentModifierMask.rawValue)" }
            #expect(keys.count == 7 && Set(keys).count == keys.count)
            let save = m.first { $0.action == #selector(Target.save(_:)) }!
            let restore = m.first { $0.action == #selector(Target.restore(_:)) }!
            #expect(save.keyEquivalent == "s" && save.keyEquivalentModifierMask == [.control, .option, .command])
            #expect(save.isEnabled && !restore.isEnabled && restore.toolTip == "No save point yet.")
            let menu = NSMenu()
            menu.autoenablesItems = false
            m.forEach { menu.addItem($0) }
            HostQuitItems.update(menu, save: #selector(Target.save(_:)), restore: #selector(Target.restore(_:)),
                                 saving: .unavailable("Saving…"), restoring: .available)
            #expect(!save.isEnabled && save.toolTip == "Saving…" && restore.isEnabled)
            let s = HostQuitItems.items(title: "Glider", suspended: false, target: target, quit: #selector(Target.quit(_:)),
                                        suspend: #selector(Target.suspend(_:)), forceQuit: #selector(Target.force(_:)),
                                        hibernate: #selector(Target.hibernate(_:)), savePoints: sp, quitKeys: false)
            #expect(s.allSatisfy { $0.keyEquivalent.isEmpty })
        }
        #endif
    }
}

extension BlueBox.HibernateNetwork {
    /// A save point leaves a connection open (only Hibernate's quit, or
    /// going back, ends it): the program reads what comes after it.  Gone
    /// back to, the connection is over, as after resuming.
    @Test func aSavePointKeepsConnections() {
        _ = launchTestApp()
        startLoop()
        _ = ipp()
        let (lfd, port) = listener()
        defer { close(lfd) }
        let (conn, _) = newStream(udp: false)
        let active = tcpPB(conn, 32)                                     // TCPActiveOpen
        wr32(active + 32 + UInt32(TCPOpenPB_remoteHost_off), 0x7F00_0001)
        wr16(active + 32 + UInt32(TCPOpenPB_remotePort_off), port)
        #expect(tb_PBControl(active, false) == 0)
        let peer = accept(lfd, nil, nil)
        #expect(peer >= 0)
        defer { close(peer) }

        machine_drain_begin(true)
        #expect(machine_run(10_000_000) == RUN_DRAINED)
        var buf: UnsafeMutablePointer<UInt8>?
        var len = 0
        var err = [CChar](repeating: 0, count: 256)
        #expect(machine_savepoint_take(&buf, &len, &err, err.count), Comment(rawValue: String(cString: err)))
        machine_drain_end()
        defer { machine_savepoint_free(buf) }
        _ = machine_run(100_000)

        _ = "after".withCString { send(peer, $0, 5, 0) }
        let rcvBuf = tb_NewPtr(64, false, true)
        let rcv = tcpPB(conn, 37)                                         // TCPRcv
        wr8(rcv + 32, 2)                                                  // a two-second timeout
        wr32(rcv + 32 + UInt32(TCPReceivePB_rcvBuff_off), rcvBuf)
        wr16(rcv + 32 + UInt32(TCPReceivePB_rcvBuffLen_off), 64)
        #expect(tb_PBControl(rcv, false) == 0)
        #expect(rd16(rcv + 32 + UInt32(TCPReceivePB_rcvBuffLen_off)) == 5 && rd8(rcvBuf) == UInt8(ascii: "a"))

        machine_drain_begin(true)
        #expect(machine_run(10_000_000) == RUN_DRAINED)
        #expect(machine_savepoint_restore(buf, len, nil, &err, err.count), Comment(rawValue: String(cString: err)))
        machine_drain_end()
        mactcp_poll()
        let status = tcpPB(conn, 39)                                      // TCPStatus
        #expect(tb_PBControl(status, false) == 0)
        #expect(rd8(status + 32 + UInt32(TCPStatusPB_connectionState_off)) == 0)
        #expect(tb_PBControl(tcpPB(conn, 42), false) == 0)                // released
    }
}

