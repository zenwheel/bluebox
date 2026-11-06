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

        /// A program that never comes back from a routine of its own the
        /// Toolbox called (a ModalDialog filter, as Anthem's loop inside its
        /// Apple Event handler): the instruction limit, reached first in a
        /// headless run, is refused for where the program waited, not for
        /// the limit.
        @Test func theLimitInACallbackIsRefusedForWhereItWas() {
            launchTestApp()
            let items = tb_NewHandle(2, false, true)
            wr16(rd32(items), 0xFFFF) // no items
            let r = ptr(8)
            for (i, v) in [100, 100, 200, 300].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
            _ = tb_NewDialog(0, r, ptr(2), 1, 0, 0xFFFF_FFFF, 0, 0, items)
            let filter = code([0x60FE]) // BRA.S self: never returns
            let item = ptr(2)
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            // ModalDialog(filter, &item), for ever
            _ = start([0x2F3C, hi(filter), lo(filter), 0x2F3C, hi(item), lo(item), 0xA991, 0x60F2])
            // In the filter, the drain asked for there; the limit ends it.
            machine_hibernate_at(machine_instruction_count() + 1_000_000)
            machine_set_instruction_limit(machine_instruction_count() + 6_000_000)
            _ = machine_run(10_000_000)
            machine_hibernate_at(0)
            machine_set_instruction_limit(0)
            #expect(machine_drain_state() == Int32(MACHINE_DRAIN_REFUSED))
            let why = String(cString: machine_drain_reason())
            #expect(why.contains("routine of its own called by the Toolbox (ModalDialog)"), Comment(rawValue: why))
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

        /// A program running its event loop inside its Apple Event handler
        /// (Anthem 1.01's Open Application): the handler is called on the
        /// program's own stack, so the machine stops at its idle call there,
        /// its snapshot is put back, and when the handler returns the
        /// program's AEProcessAppleEvent call answers its result, the
        /// registers it had kept.
        @Test func aLoopInsideAnAppleEventHandlerHibernates() {
            launchTestApp(sizeFlags: 0x0040)
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            let vars = ptr(12) // count, flag, result (word)
            let count = vars, flag = vars + 4, result = vars + 8
            // pascal OSErr handler(evt, reply, refcon):
            //   loop { count += 1; _SystemTask } until flag; return -1700 (errAECoercionFail, say)
            let handler = code([0x52B9, hi(count), lo(count), 0xA9B4, 0x4AB9, hi(flag), lo(flag), 0x67F0,
                                0x205F, 0x4FEF, 0x000C, 0x3EBC, 0xF95C, 0x4ED0])
            #expect(tb_AEInstallEventHandler(Int32(bitPattern: fourCC("aevt")), Int32(bitPattern: fourCC("oapp")),
                                             handler, 0, 0) == 0)
            machine_send_apple_event(fourCC("aevt"), fourCC("oapp"), nil, 0, nil, nil, nil)
            let e = ptr(UInt32(EventRecord_SIZEOF))
            #expect(tb_GetNextEvent(0x0400, e) != 0 && rd16(e) == 23)
            wr16(result, 0x7777)
            // MOVEQ #5,D3; CLR.W -(SP); MOVE.L #e,-(SP); MOVE.W #$021B,D0; _Pack8; MOVE.W (SP)+,result;
            // loop: _SystemTask; BRA.S loop
            _ = start([0x7605, 0x4267, 0x2F3C, hi(e), lo(e), 0x303C, 0x021B, 0xA816, 0x33DF, hi(result), lo(result),
                       0xA9B4, 0x60FC])
            g_cpu!.pointee.d.3 = 0
            _ = machine_run(200_000)
            #expect(rd32(count) > 0)                        // in the handler
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            #expect(String(cString: machine_drain_point()) == "SystemTask")
            let snap = tempDir().appendingPathComponent("aehandler.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
            let n = rd32(count)
            _ = machine_run(200_000)
            #expect(rd32(count) > n)                        // the handler's loop goes on
            #expect(rd16(result) == 0x7777)
            g_cpu!.pointee.d.3 = 9                          // a register the handler changed
            wr32(flag, 1)
            _ = machine_run(200_000)
            #expect(Int16(bitPattern: rd16(result)) == -1700) // the handler's result, returned to the call
            #expect(g_cpu!.pointee.d.3 == 5)                // as the program had it
        }

        /// Hibernate in a window: the machine runs on its own thread (as the
        /// hosts run it), Suspend stops it at a safe point, the snapshot is
        /// written from another thread while it stands there, and put back
        /// it goes on from the event loop.
        @Test func suspendedOnItsThreadIsSaved() {
            let dir = launchTestApp()
            let pc = start([0x5287, 0xA9B4, 0x60FA]) // ADDQ.L #1,D7; _SystemTask; BRA.S loop
            _ = machine_run(100_000)
            final class Flag: @unchecked Sendable { var stop = false }
            let flag = Flag(), done = DispatchSemaphore(value: 0)
            Thread.detachNewThread {
                while !flag.stop && !machine_halted() { _ = machine_run(200_000) }
                done.signal()
            }
            machine_pause()
            #expect(machine_wait_parked(3000))
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            let snap = dir.appendingPathComponent("suspended.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            let d7 = g_cpu!.pointee.d.7
            flag.stop = true
            machine_unpause()
            done.wait()
            #expect(!machine_hibernatable())
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
            #expect(g_cpu!.pointee.d.7 == d7)
            #expect(machine_pc() == pc + 2 || machine_pc() == pc || machine_pc() == pc + 4) // at its loop
            _ = machine_run(100_000)
            #expect(g_cpu!.pointee.d.7 > d7)
        }

        /// A resumed machine's clock is rebased on the instructions (no host
        /// has attached yet); the host attaching after (an interactive
        /// resume) switches it to the wall clock, and it goes on from where
        /// it was, not from the host's time since boot.
        @Test func clockGoesOnWhenAHostAttaches() {
            let dir = launchTestApp()
            _ = start([0xA9B4, 0x60FC]) // _SystemTask; BRA.S loop
            _ = machine_run(100_000)
            g_cpu!.pointee.icount += 10_000_000_000 // a thousand seconds of the instruction clock
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            let snap = dir.appendingPathComponent("clock.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
            let before = machine_time_ns()
            #expect(before > 1_000_000_000_000)
            machine_attach_host(true)
            let attached = machine_time_ns()
            machine_attach_host(false)
            let detached = machine_time_ns()
            #expect(attached >= before && attached - before < 500_000_000)
            #expect(detached >= attached && detached - attached < 500_000_000)
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
