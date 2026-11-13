import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Snapshots of the Toolbox's state (hibernate.c): the state sections,
    /// the tracked blocks and the guest's memory, put back elsewhere with
    /// every pointer rewritten.
    @Suite(.serialized) struct Hibernate {
        /// A program counting in D7 for ever, with a VBL task and a Time
        /// Manager task counting their runs at out and out+4, after the
        /// Toolbox's managers are set up, Apple Event handlers installed, a
        /// resource file made and opened and a sound channel made.
        func setUp() -> UInt32 {
            launchTestApp()
            _ = initToolbox()
            let out = tb_NewPtr(8, false, true)
            for (i, id) in ["oapp", "odoc", "quit"].enumerated() {
                #expect(tb_AEInstallEventHandler(Int32(bitPattern: fourCC("aevt")), Int32(bitPattern: fourCC(id)),
                                                 0x1000 + UInt32(i), 0, 0) == 0)
            }
            let name = tb_NewPtr(64, false, true)
            guestPString("Hibernate Test", at: name)
            tb_HCreateResFile(0, 0, name)
            #expect(tb_HOpenResFile(0, 0, name, 3) > 0)
            let chan = tb_NewPtr(4, false, true)
            #expect(tb_SndNewChannel(chan, 5, 0, 0) == 0)
            func code(_ words: [UInt16]) -> UInt32 {
                let p = tb_NewPtr(Int32(words.count * 2), false, false)
                for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
                return p
            }
            let loop = code([0x5287, 0x60FC])                            // ADDQ.L #1,D7; BRA.S loop
            let vblCode = code([0x52B9, UInt16(out >> 16), UInt16(out & 0xFFFF), // ADDQ.L #1,out
                                0x317C, 0x0001, UInt16(VBLTask_vblCount_off),     // MOVE.W #1,vblCount(A0)
                                0x4E75])                                          // RTS
            let tmCode = code([0x52B9, UInt16((out + 4) >> 16), UInt16((out + 4) & 0xFFFF), // ADDQ.L #1,out+4
                               0x4E75])
            let v = tb_NewPtr(Int32(VBLTask_SIZEOF), false, true)
            wr16(v + 4, 1)
            wr32(v + UInt32(VBLTask_vblAddr_off), vblCode)
            wr16(v + UInt32(VBLTask_vblCount_off), 1)
            #expect(tb_VInstall(v) == 0)
            let tm = tb_NewPtr(Int32(TMTask_SIZEOF), false, true)
            wr32(tm + UInt32(TMTask_tmAddr_off), tmCode)
            tb_InsTime(tm)
            tb_PrimeTime(tm, 5)
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000
            cpu.pointee.d.7 = 0
            cpu.pointee.pc = loop
            return out
        }

        /// What a run leaves: the registers, the counters, Ticks, the
        /// instruction count and the low 8 MB of the guest's memory.
        func fingerprint(_ out: UInt32) -> [UInt64] {
            let cpu = g_cpu!.pointee
            var f: [UInt64] = [UInt64(cpu.pc), UInt64(cpu.sr), UInt64(rd32(out)), UInt64(rd32(out + 4)),
                               UInt64(rd32(UInt32(LM_Ticks))), machine_instruction_count()]
            withUnsafeBytes(of: cpu.d) { f += $0.bindMemory(to: UInt32.self).map { UInt64($0) } }
            withUnsafeBytes(of: cpu.a) { f += $0.bindMemory(to: UInt32.self).map { UInt64($0) } }
            var h: UInt64 = 1469598103934665603
            g_mem.withMemoryRebound(to: UInt64.self, capacity: 1 << 20) { p in
                for i in 0..<(1 << 20) { h = (h ^ p[i]) &* 1099511628211 }
            }
            f.append(h)
            return f
        }

        func save() -> [UInt8] {
            var buf: UnsafeMutablePointer<UInt8>?
            var len = 0
            var stats = hb_stats()
            var err = [CChar](repeating: 0, count: 256)
            let ok = hb_save_buffer(&buf, &len, &stats, &err, err.count)
            #expect(ok, Comment(rawValue: String(cString: err)))
            guard ok, let buf else { return [] }
            defer { hb_free_buffer(buf) }
            return Array(UnsafeBufferPointer(start: buf, count: len))
        }

        /// How the two differ (written to stderr); host pointers left out.
        func differences(_ a: [UInt8], _ b: [UInt8], quiet: Bool = false) -> Int32 {
            a.withUnsafeBufferPointer { pa in
                b.withUnsafeBufferPointer { pb in
                    hb_compare(pa.baseAddress, pa.count, pb.baseAddress, pb.count, Int32(HB_COMPARE_IGNORE_HOST),
                               quiet ? nil : stderr, 20)
                }
            }
        }

        @discardableResult
        func restore(_ snap: [UInt8], flags: Int32 = 0) -> (Bool, String, hb_stats) {
            var stats = hb_stats()
            var err = [CChar](repeating: 0, count: 256)
            let ok = snap.withUnsafeBufferPointer { hb_restore_buffer($0.baseAddress, $0.count, flags, &stats, &err, err.count) }
            return (ok, String(cString: err), stats)
        }

        /// The map has every saved static of this binary and every tagged
        /// type at its size (else scripts/state-map.py needs running).
        @Test func mapMatchesTheBinary() {
            #expect(hb_check_map(stderr) == 0)
            var id = [CChar](repeating: 0, count: 33)
            hb_build_id(&id)
            #expect(String(cString: id).count == 32)
        }

        /// Saved after the machine has run, put back in fresh blocks and a
        /// fresh guest address space, the state is the same (a snapshot of
        /// it is the same bytes) and the machine goes on from it exactly as
        /// it went on from the original.
        @Test func roundTripRelocatesEverything() {
            let t0 = Date()
            let out = setUp()
            _ = machine_run(200_000)
            let t1 = Date()
            _ = hb_scan(stderr, 60, 0)
            let s1 = save()
            #expect(!s1.isEmpty)
            print("hibernate round trip: set up and ran in \(t1.timeIntervalSince(t0)) s, saved in \(Date().timeIntervalSince(t1)) s")

            _ = machine_run(300_000)
            let ranA = fingerprint(out), sA = save()
            #expect(rd32(out) > 0 && rd32(out + 4) > 0)  // both tasks ran
            #expect(differences(s1, sA, quiet: true) > 0)  // and the state is not what it was

            let memBefore = UInt(bitPattern: g_mem), firstBefore = UInt(bitPattern: bb_blocks_first())
            let (ok, err, st) = restore(s1, flags: Int32(HB_RESTORE_MOVE_REGIONS))
            #expect(ok, Comment(rawValue: err))
            #expect(UInt(bitPattern: g_mem) != memBefore)           // the guest's memory moved
            #expect(UInt(bitPattern: bb_blocks_first()) != firstBefore) // and the blocks
            #expect(st.blocks > 0 && st.to_blocks > 0 && st.to_statics > 0 && st.to_regions > 0 && st.to_image > 0,
                    "\(st)")
            print("hibernate round trip: \(s1.count) bytes, \(st.blocks) blocks (\(st.block_bytes) bytes), "
                  + "\(st.region_bytes) bytes of guest memory, \(st.relocs) pointers (\(st.to_blocks) to blocks, "
                  + "\(st.to_statics) to statics, \(st.to_regions) to guest memory, \(st.to_image) to the binary, "
                  + "\(st.to_libraries) to libraries, \(st.host) to the host's memory)")
            #expect(differences(s1, save()) == 0)                    // the same state, wherever it is

            let t2 = Date()
            _ = machine_run(300_000)
            print("hibernate round trip: ran again in \(Date().timeIntervalSince(t2)) s")
            #expect(fingerprint(out) == ranA)
            #expect(differences(sA, save()) == 0)
            // Still a working Toolbox.
            let h = tb_NewHandle(1000, false, false)
            #expect(h != 0 && memErr() == 0)
            tb_DisposeHandle(h)
            _ = machine_run(100_000)
            #expect(rd32(out) > 0)
        }

        /// The host's own values (the date by the host's clock, which a
        /// resume moves on, and the guest's Time from it) are left out of a
        /// comparison with the host's pointers: two processes' snapshots
        /// differ there by nature.  Without leaving them out they differ.
        @Test func hostValuesAreLeftOutOfAComparison() {
            _ = setUp()
            _ = machine_run(100_000)
            let a = save()
            machine_set_time(rd32(UInt32(LM_Time)) + 7) // as a resume's date catching up
            let b = save()
            #expect(differences(a, b) == 0)
            let all = a.withUnsafeBufferPointer { pa in
                b.withUnsafeBufferPointer { pb in hb_compare(pa.baseAddress, pa.count, pb.baseAddress, pb.count, 0, nil, 0) }
            }
            #expect(all >= 2) // boot_time and the guest's Time
        }

        /// The place of a line in a snapshot's own manifest (in its body).
        func manifestLine(_ snap: [UInt8], _ prefix: String) -> Int? {
            let key = Array(prefix.utf8)
            return (0...(snap.count - key.count)).first { snap[$0] == key[0] && Array(snap[$0..<$0 + key.count]) == key }
        }

        /// A snapshot another build wrote, a static of whose has another type
        /// here, is refused, naming it, and nothing changes.
        @Test func anotherShapeIsRefused() {
            let out = setUp()
            _ = machine_run(50_000)
            var snap = save()
            snap[16] ^= 0xFF  // a byte of the build's UUID
            snap[32] ^= 0xFF  // and of the state fingerprint
            guard let at = manifestLine(snap, "S\tToolbox/trap.c\tnatives\t") else {
                Issue.record("no manifest in the snapshot")
                return
            }
            var end = at
            while snap[end] != 0x0A { end += 1 }
            snap[end - 1] = snap[end - 1] == UInt8(ascii: "0") ? UInt8(ascii: "1") : UInt8(ascii: "0") // its shape
            let before = fingerprint(out)
            let (ok, err, _) = restore(snap)
            #expect(!ok && err.contains("the static natives (Toolbox/trap.c) has another type"), Comment(rawValue: err))
            #expect(fingerprint(out) == before)
        }

        /// A snapshot of a build whose statics are elsewhere (another
        /// fingerprint, the same shapes) is put back a static at a time by
        /// name, and the machine goes on exactly as from this build's.
        @Test func staticsArePutBackByName() {
            let out = setUp()
            _ = machine_run(200_000)
            var s1 = save()
            _ = machine_run(300_000)
            let ranA = fingerprint(out), sA = save()
            s1[16] ^= 0xFF
            s1[32] ^= 0xFF  // another layout
            let (ok, err, st) = restore(s1, flags: Int32(HB_RESTORE_MOVE_REGIONS))
            #expect(ok, Comment(rawValue: err))
            #expect(st.statics_dropped == 0 && st.statics_new == 0, "\(st)")
            s1[16] ^= 0xFF
            s1[32] ^= 0xFF
            #expect(differences(s1, save()) == 0)
            _ = machine_run(300_000)
            #expect(fingerprint(out) == ranA)
            #expect(differences(sA, save()) == 0)
        }

        /// A static the snapshot's build had and this one has not is dropped;
        /// one this build has and the snapshot's had not keeps its value when
        /// that is as compiled, and is refused, naming it, when booting set it
        /// (it could hold a stub's place among this boot's natives).
        @Test func aStaticOnlyOneBuildHas() {
            let out = setUp()
            _ = machine_run(50_000)
            var snap = save()
            snap[16] ^= 0xFF
            snap[32] ^= 0xFF
            // natives (trap.c), set as the machine booted, renamed in the
            // snapshot: the snapshot's Natives is dropped, this build's natives new.
            var set = snap
            guard let at = manifestLine(set, "S\tToolbox/trap.c\tnatives\t") else {
                Issue.record("no manifest in the snapshot")
                return
            }
            set[at + 17] = UInt8(ascii: "N")
            let before = fingerprint(out)
            let (ok, err, _) = restore(set)
            // (Set, or holding a pointer to a name made as it booted.)
            #expect(!ok && err.contains("the static natives (Toolbox/trap.c), new in this build, ")
                    && err.hasSuffix("as this build booted"), Comment(rawValue: err))
            #expect(fingerprint(out) == before)
            // A static of zeros now, with no pointer: renamed, it is put back
            // as a new static, keeping its zeros.
            let lines = manifest().split(separator: "\n").map(String.init)
            var zero: String?
            for l in lines where l.hasPrefix("S\t") {
                let f = l.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
                guard f.count >= 8, f[3] == "1", let size = Int(f[5]), size > 0, size <= 64,
                      let p = hb_resolve_symbol(f[1], f[2], 0) else { continue }
                if (0..<size).allSatisfy({ p.load(fromByteOffset: $0, as: UInt8.self) == 0 }) {
                    zero = "S\t\(f[1])\t\(f[2])\t"
                    break
                }
            }
            guard let zero, let z = manifestLine(snap, zero) else {
                Issue.record("no static of zeros")
                return
            }
            snap[z + zero.utf8.count - 2] ^= 0x20 // its name's last letter's case
            let (ok2, err2, st) = restore(snap)
            #expect(ok2, Comment(rawValue: err2))
            #expect(st.statics_dropped == 1 && st.statics_new == 1, "\(st)")
        }

        /// Another build's snapshot with this build's state fingerprint is put
        /// back with every pointer into the binary found by name (functions,
        /// constants, string constants) and every block's type by what it is,
        /// none by offset; the machine goes on exactly as from this build's.
        @Test func anotherBuildWithTheSameShapeResumes() {
            let out = setUp()
            _ = machine_run(200_000)
            var s1 = save()
            _ = machine_run(300_000)
            let ranA = fingerprint(out), sA = save()
            s1[16] ^= 0xFF  // another build's UUID, this one's fingerprint
            let (ok, err, st) = restore(s1, flags: Int32(HB_RESTORE_MOVE_REGIONS))
            #expect(ok, Comment(rawValue: err))
            #expect(st.to_functions > 0 && st.to_strings > 0 && st.to_image_unnamed == 0 && st.strings_kept == 0, "\(st)")
            print("hibernate by name: \(st.to_image) pointers into the binary: \(st.to_functions) to functions, "
                  + "\(st.to_code_inside) into them, \(st.to_data) to constants, \(st.to_data_inside) into them, "
                  + "\(st.to_strings) to strings")
            s1[16] ^= 0xFF
            #expect(differences(s1, save()) == 0)
            _ = machine_run(300_000)
            #expect(fingerprint(out) == ranA)
            #expect(differences(sA, save()) == 0)
        }

        /// Another build's snapshot that points to a function this build does
        /// not have is refused, naming it, and nothing changes.
        @Test func aFunctionThisBuildHasNotIsRefused() {
            let out = setUp()
            _ = machine_run(50_000)
            var snap = save()
            snap[16] ^= 0xFF  // another build's UUID, this one's fingerprint
            // The pool, at the end, names the first native's function.
            let name = Array("unimplemented_native\0".utf8)
            guard let at = (0...(snap.count - name.count)).reversed().first(where: { Array(snap[$0..<$0 + name.count]) == name })
            else {
                Issue.record("not in the snapshot")
                return
            }
            snap[at] = UInt8(ascii: "U")
            let before = fingerprint(out)
            let (ok, err, _) = restore(snap)
            #expect(!ok && err.contains("Unimplemented_native (Toolbox/trap.c), which this build does not have"),
                    Comment(rawValue: err))
            #expect(fingerprint(out) == before)
        }

        /// The fingerprint is a hash of the manifest, which names each saved
        /// static (file, name, place, size, layout and shape), each tagged
        /// type, the state sections and the guest-side layout records.
        @Test func fingerprintCoversTheState() {
            var a = [CChar](repeating: 0, count: 33), b = [CChar](repeating: 0, count: 33)
            hb_fingerprint(&a)
            hb_fingerprint(&b)
            #expect(String(cString: a).count == 32 && a == b)
            let lines = manifest().split(separator: "\n").map(String.init)
            #expect(lines.first?.hasPrefix("V\t") == true)
            #expect(lines.contains { $0.hasPrefix("R\t__bbbss\t") })
            #expect(lines.contains { $0.hasPrefix("S\tToolbox/trap.c\tnatives\t") })
            #expect(lines.contains { $0.hasPrefix("S\tToolbox/cfm.c\tnatives\t") })
            #expect(lines.contains { $0.hasPrefix("T\t") })
            for rec in ["trap.c stubs", "trap.c stub size", "mem.c layouts", "drivers.c drivers", "ot_68k_table.c functions",
                        "carbon_cf.c type IDs", "mixedmode.c level frame"] {
                #expect(lines.contains { $0.hasPrefix("L\t\(rec)\t") }, "\(rec)")
            }
            #expect(!lines.contains { $0.hasPrefix("S\tToolbox/trap.c\tdisps\t") }) // the host's: made at each boot
        }

        func manifest() -> String {
            var m: UnsafePointer<CChar>?
            let n = hb_manifest(&m)
            guard let m else { return "" }
            return String(decoding: UnsafeRawBufferPointer(start: m, count: n), as: UTF8.self)
        }

        func explain(_ old: String) -> String {
            var why = [CChar](repeating: 0, count: 512)
            old.withCString { hb_explain_manifest($0, strlen($0), &why, why.count) }
            return String(cString: why)
        }

        /// A refusal names the first thing that differs between the snapshot's
        /// manifest and this build's, and how many more do.
        @Test func refusalNamesTheFirstDifference() {
            let lines = manifest().split(separator: "\n").map(String.init)
            guard let i = lines.firstIndex(where: { $0.hasPrefix("S\tToolbox/trap.c\tnatives\t") }) else {
                Issue.record("no trap.c natives")
                return
            }
            var f = lines[i].split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            let size = f[5]
            f[5] = "8"
            var old = lines
            old[i] = f.joined(separator: "\t")
            #expect(explain(old.joined(separator: "\n") + "\n")
                    == "the static natives (Toolbox/trap.c) is 8 bytes in the snapshot, \(size) here")
            f[5] = size
            f[7] = "0000000000000000"
            old[i] = f.joined(separator: "\t")
            #expect(explain(old.joined(separator: "\n") + "\n") == "the static natives (Toolbox/trap.c) has another type")
            // A static only one build has, or elsewhere, is no difference
            // (put back by name); a layout record's value is.
            old.remove(at: i)
            if let k = old.firstIndex(where: { $0.hasPrefix("S\t") }) {
                var g = old[k].split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
                g[4] = "12345"
                old[k] = g.joined(separator: "\t")
            }
            #expect(explain(old.joined(separator: "\n") + "\n") == "its saved state has this build's shape")
            if let k = old.firstIndex(where: { $0.hasPrefix("L\ttrap.c stub size\t") }) {
                old[k] = "L\ttrap.c stub size\t0000000000000010"
            }
            if let k = old.firstIndex(where: { $0.hasPrefix("L\ttrap.c stubs\t") }) {
                old[k] = "L\ttrap.c stubs\t0000000000002000"
            }
            let why = explain(old.joined(separator: "\n") + "\n")
            #expect(why.hasPrefix("the guest-side layout trap.c stub size differs") && why.contains("1 more"),
                    Comment(rawValue: why))
        }

        /// A pointer into the binary as a snapshot names it: a global function
        /// by its name, a static one with its file, a place inside one by the
        /// offset, a string constant by its text; and each name back.
        @Test func pointersIntoTheBinaryByName() {
            _ = setUp()
            func name(_ p: UnsafeRawPointer?) -> String {
                var out = [CChar](repeating: 0, count: 512)
                return p.map { hb_symbolize($0, &out, out.count) ? String(cString: out) : "" } ?? ""
            }
            let global = unsafeBitCast(hb_save_buffer as @convention(c) (UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?,
                                                                         UnsafeMutablePointer<Int>?, UnsafeMutablePointer<hb_stats>?,
                                                                         UnsafeMutablePointer<CChar>?, Int) -> Bool,
                                       to: UnsafeRawPointer.self)
            #expect(name(global) == "hb_save_buffer+0")
            #expect(name(global + 4) == "hb_save_buffer+4")
            #expect(hb_resolve_symbol(nil, "hb_save_buffer", 4) == global + 4)
            // The first native, a static function of trap.c's.
            let fn = native_fn_at(0).map { unsafeBitCast($0, to: UnsafeRawPointer.self) }
            #expect(name(fn) == "Toolbox/trap.c:unimplemented_native+0")
            #expect(hb_resolve_symbol("Toolbox/trap.c", "unimplemented_native", 0) == fn)
            #expect(hb_resolve_symbol("Toolbox/menu.c", "unimplemented_native", 0) == nil) // another file's: none
            // A string constant: its text.
            let text = UnsafeRawPointer(bb_type_bytes.name)
            #expect(name(text) == "\"bytes\"+0" && name(text! + 2) == "\"bytes\"+2")
            #expect(hb_resolve_symbol(nil, "no such symbol", 0) == nil)
        }

        /// The untracked-pointer scan: no word of a saved static looks like a
        /// pointer the map does not know of, and the host's sections point to
        /// no tracked block (a restore frees them).
        @Test func scanFindsNoUntrackedPointers() {
            _ = setUp()
            _ = machine_run(100_000)
            let r = hb_scan(stderr, 40, 0)
            print("hibernate scan: \(r.host_fields) fields hold the host's memory")
            #expect(r.in_statics == 0)
            #expect(r.host_to_blocks == 0)
            #expect(r.in_blocks == 0)
        }

        /// The scan finds a pointer the map cannot know of: one to a tracked
        /// block, kept in a block no tag describes.
        @Test func scanFindsAPlantedPointer() {
            _ = setUp()
            let target = bb_strdup("a block to point to")!
            let holder = bb_calloc(1, 16, nil)!  // no tag: untyped
            defer {
                bb_free(holder)
                bb_free(target)
            }
            holder.storeBytes(of: UInt(bitPattern: target) + 8, as: UInt.self)
            let r = hb_scan(stderr, 10, 0)
            #expect(r.in_blocks == 1 && r.in_untyped == 1 && r.in_statics == 0)
        }

        /// scripts/state-check.sh: no state outside the sections, no raw
        /// malloc, the census as the map and the allowlist have it.
        @Test func stateCheckPasses() throws {
            let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent()
            let objects = root.appendingPathComponent(".build/out/Intermediates.noindex/bluebox.build/Debug/Toolbox-t.build")
            guard FileManager.default.fileExists(atPath: objects.path) else { return }  // another build layout
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/bin/sh")
            p.arguments = [root.appendingPathComponent("scripts/state-check.sh").path]
            let pipe = Pipe()
            p.standardOutput = pipe
            p.standardError = pipe
            try p.run()
            let text = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            p.waitUntilExit()
            #expect(p.terminationStatus == 0, Comment(rawValue: text))
        }
    }
}
