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

        /// A snapshot another build wrote is refused, and nothing changes.
        @Test func anotherBuildIsRefused() {
            let out = setUp()
            _ = machine_run(50_000)
            var snap = save()
            snap[16] ^= 0xFF  // a byte of the build's UUID
            let before = fingerprint(out)
            let (ok, err, _) = restore(snap)
            #expect(!ok && err.contains("another build"))
            #expect(fingerprint(out) == before)
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
