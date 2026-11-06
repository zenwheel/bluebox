import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Draining the machine to a safe point (drain.c) and what hibernating
    /// carries across (reattach.c): the machine stops at its program's idle
    /// call, or between slices, and a snapshot taken there puts back open
    /// forks checked against their files.
    @Suite(.serialized) struct Drain {
        func code(_ words: [UInt16]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
            return p
        }

        /// The program: `words` from the start, with a stack and interrupts on.
        func start(_ words: [UInt16]) -> UInt32 {
            let pc = code(words)
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000
            cpu.pointee.pc = pc
            return pc
        }

        func ptr(_ n: UInt32) -> UInt32 { tb_NewPtr(Int32(n), false, true) }
        func put64(_ p: UInt32, _ v: UInt64) {
            wr32(p, UInt32(v >> 32))
            wr32(p + 4, UInt32(truncatingIfNeeded: v))
        }

        /// An event loop calling SystemTask: the machine stops on the trap,
        /// the call not made, and goes on from it.
        @Test func stopsAtTheProgramsIdleCall() {
            launchTestApp()
            let pc = start([0x5287, 0xA9B4, 0x60FA]) // ADDQ.L #1,D7; _SystemTask; BRA.S loop
            _ = machine_run(100_000)
            machine_drain_begin(true)
            let status = machine_run(10_000_000)
            #expect(status == RUN_DRAINED)
            #expect(machine_drain_state() == Int32(MACHINE_DRAIN_PARKED))
            #expect(machine_hibernatable())
            #expect(machine_pc() == pc + 2)                 // on the trap
            #expect(String(cString: machine_drain_point()) == "SystemTask")
            let d7 = g_cpu!.pointee.d.7
            machine_drain_end()
            _ = machine_run(100_000)
            #expect(!machine_hibernatable())
            #expect(g_cpu!.pointee.d.7 > d7)                // and on it goes
        }

        /// A program making no idle call: between slices, after the grace.
        @Test func stopsBetweenSlicesWithoutAnIdleCall() {
            launchTestApp()
            _ = start([0x5287, 0x60FC]) // ADDQ.L #1,D7; BRA.S loop
            let from = machine_instruction_count()
            machine_drain_begin(true)
            #expect(machine_run(50_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable())
            #expect(machine_instruction_count() - from >= 5_000_000)
            #expect(String(cString: machine_drain_point()).hasPrefix("between instructions"))
            machine_drain_end()
        }

        /// Suspend drains too: stopped at a safe point, the machine could be
        /// hibernated, and goes on when resumed.
        @Test func suspendStopsAtASafePoint() {
            launchTestApp()
            _ = start([0x5287, 0x60FC])
            machine_pause_for(200)
            _ = machine_run(2_000_000) // the timed pause parks at the next safe point and ends by itself
            #expect(!machine_paused())
            #expect(machine_drain_state() == Int32(MACHINE_DRAIN_NONE))
        }

        /// No snapshot is written of a machine not at a safe point.
        @Test func savingNeedsASafePoint() {
            launchTestApp()
            _ = start([0x5287, 0x60FC])
            _ = machine_run(10_000)
            var err = [CChar](repeating: 0, count: 256)
            let path = tempDir().appendingPathComponent("x.bbhib").path
            #expect(!machine_hibernate_save(path, "{}", nil, &err, err.count))
            #expect(!FileManager.default.fileExists(atPath: path))
        }

        /// An open fork is put back as it was while its file is unchanged;
        /// one whose file changed while hibernated gives I/O errors, and
        /// closing it writes nothing over the new file.
        @Test func forksAreCheckedAgainstTheirFiles() throws {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("doc.txt")
            try Data().write(to: file)
            let spec = ptr(UInt32(FSSpec_SIZEOF))
            fm_spec_for_path(file.path, spec)
            let ref = ptr(80)
            #expect(tb_FSpMakeFSRef(spec, ref) == 0)
            let rnp = ptr(2), actual = ptr(4), pos = ptr(8), buf = ptr(16)
            #expect(tb_FSOpenFork(ref, 0, 0, 3, rnp) == 0)
            let rn = Int16(bitPattern: rd16(rnp))
            for (i, b) in Array("abc".utf8).enumerated() { wr8(buf + UInt32(i), b) }
            put64(pos, 0)
            #expect(tb_FSWriteFork(rn, 1, pos, 3, buf, actual) == 0)

            _ = start([0xA9B4, 0x60FC]) // _SystemTask; BRA.S loop
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            let snap = dir.appendingPathComponent("doc.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            var bytes: UInt64 = 0
            #expect(machine_hibernate_save(snap, "{}", &bytes, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(bytes > 0)
            #expect(try String(contentsOf: file, encoding: .utf8) == "abc") // written out first

            // Unchanged: the fork reads as before.
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
            put64(pos, 0)
            #expect(tb_FSReadFork(rn, 1, pos, 3, buf, actual) == 0)
            #expect(rd32(actual) == 3 && rd8(buf) == UInt8(ascii: "a"))

            // Changed while hibernated: I/O errors, and the new file stays.
            try Data("changed".utf8).write(to: file)
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
            put64(pos, 0)
            #expect(tb_FSReadFork(rn, 1, pos, 3, buf, actual) == -36)  // ioErr
            #expect(tb_FSWriteFork(rn, 1, pos, 3, buf, actual) == -36)
            #expect(tb_FSCloseFork(rn) == 0)
            #expect(try String(contentsOf: file, encoding: .utf8) == "changed")
        }

        /// A snapshot file's launch record reads back without the rest.
        @Test func theLaunchRecordReadsBack() {
            let dir = launchTestApp()
            _ = start([0xA9B4, 0x60FC])
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            let snap = dir.appendingPathComponent("r.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{\"args\":[]}", nil, &err, err.count))
            machine_drain_end()
            var extra: UnsafeMutablePointer<UInt8>?
            var len = 0
            var same = false
            #expect(hb_file_extra(snap, &extra, &len, &same, &err, err.count))
            #expect(same)
            if let extra {
                #expect(String(decoding: UnsafeBufferPointer(start: extra, count: len), as: UTF8.self) == "{\"args\":[]}")
                hb_free_buffer(extra)
            }
        }
    }
}
