import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// The test natives' record of their calls.
nonisolated(unsafe) private var abandonCalls = 0
nonisolated(unsafe) private var levelSeen: [Bool] = []

extension BlueBox {
    /// Draining a PowerPC program (drain.c, cfm.c's dispatcher) and its
    /// threads (threadmgr.c): the machine stops on its idle call's NATIVE
    /// word, a native abandoning its call has the registers put back, a
    /// call under a host frame is not the program's level, and a thread
    /// waiting in a Thread Manager call is saved and goes on after a restore
    /// with its call answered.
    @Suite(.serialized) struct DrainPPC {
        func ptr(_ n: UInt32) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

        /// PowerPC code from its words.
        func code(_ words: [UInt32]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 4), false, false)
            for (i, w) in words.enumerated() { wr32(p + UInt32(i * 4), w) }
            return p
        }

        func tvector(_ code: UInt32) -> UInt32 {
            let tv = ptr(8)
            wr32(tv, code)
            return tv
        }

        /// lis/ori a register with a value.
        func load(_ r: UInt32, _ v: UInt32) -> [UInt32] {
            [0x3C00_0000 | r << 21 | v >> 16, 0x6000_0000 | r << 21 | r << 16 | (v & 0xFFFF)]
        }

        /// A call through a transition vector, as cross-TOC glue makes it.
        func call(_ tv: UInt32) -> [UInt32] {
            load(12, tv) + [0x800C_0000, 0x7C09_03A6, 0x4E80_0421] // lwz r0,0(r12); mtctr r0; bctrl
        }

        func branch(back n: Int) -> UInt32 { 0x4800_0000 | (UInt32(bitPattern: Int32(-4 * n)) & 0x03FF_FFFC) }

        func export(_ name: String) -> UInt32 {
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            return cfm_native_tvector(fn, name)
        }

        func gpr(_ n: Int) -> UInt32 { withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: n * 4, as: UInt32.self) } }
        func setGPR(_ n: Int, _ v: UInt32) {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: v, toByteOffset: n * 4, as: UInt32.self) }
        }

        /// A launch with the PowerPC processor, for a PowerPC program.
        init() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            g_cpu!.pointee.halted = false
            g_ppc.halted = false
        }

        /// The PowerPC program at the top of machine_run: `words` from the start.
        func start(_ words: [UInt32]) -> UInt32 {
            g_ppc_mode = true
            let pc = code(words)
            g_ppc.pc = pc
            setGPR(1, ptr(65536) + 60000)
            return pc
        }

        func snapshot(_ name: String) -> String {
            let path = tempDir().appendingPathComponent(name).path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(path, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            return path
        }

        func restore(_ path: String) {
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_restore(path, &err, err.count), Comment(rawValue: String(cString: err)))
        }

        /// An event loop calling SystemTask: the machine stops on its NATIVE
        /// word, the call not made, and goes on from it.
        @Test func stopsAtTheProgramsIdleCall() {
            defer { g_ppc_mode = false }
            let task = export("SystemTask")
            let pc = start([0x3BFF_0001] + call(task) + [branch(back: 6)]) // addi r31,r31,1; SystemTask; loop
            _ = machine_run(100_000)
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable())
            #expect(g_ppc.pc == rd32(task))                 // on the NATIVE word
            #expect(g_ppc.lr == pc + 24)                    // after the bctrl: the call was being made
            #expect(String(cString: machine_drain_point()) == "SystemTask")
            let r31 = gpr(31)
            machine_drain_end()
            _ = machine_run(100_000)
            #expect(gpr(31) > r31)                          // and on it goes
        }

        static let abandoning: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
            abandonCalls += 1
            _ = drain_abandon()
            withUnsafeMutableBytes(of: &c!.pointee.gpr) { $0.storeBytes(of: UInt32(0xDEAD), toByteOffset: 12, as: UInt32.self) }
            c!.pointee.pc = c!.pointee.lr
        }

        /// A native abandoning its call while the machine drains: the
        /// registers as the call was made, the program counter on its NATIVE
        /// word, and the call made again when the machine goes on.
        @Test func anAbandonedCallIsMadeAgain() {
            defer { g_ppc_mode = false }
            let tv = cfm_native_tvector(Self.abandoning, "TestAbandon")
            _ = start([0x3860_0055] + call(tv) + [0x3BFF_0001, branch(back: 7)]) // li r3,$55; TestAbandon; addi r31
            setGPR(31, 0)
            abandonCalls = 0
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(abandonCalls == 1)
            #expect(String(cString: machine_drain_point()) == "TestAbandon (abandoned)")
            #expect(g_ppc.pc == rd32(tv))
            #expect(gpr(3) == 0x55)                         // not the 0xDEAD it left
            #expect(gpr(31) == 0)                           // the code after the call has not run
            machine_drain_end()
            _ = machine_run(1000)
            #expect(abandonCalls > 1 && gpr(31) > 0)
        }

        /// A PowerPC program's PBReadSync to a 68k driver of its own whose
        /// Prime waits for input in GetNextEvent: the routine is left at the
        /// idle call, the program's call put back on its NATIVE word, and
        /// made again after a restore, when the driver answers it.
        @Test func aSynchronousReadWaitingInsideItsDriverHibernates() {
            defer { g_ppc_mode = false }
            let (storage, pb, buf) = waitingDriverRead()
            let result = ptr(4)
            wr32(result, 0x7777_7777)
            let read = export("PBReadSync"), task = export("SystemTask")
            // r30 = result; r3 = pb; PBReadSync; stw r3,0(r30); loop: SystemTask
            _ = start(load(30, result) + load(3, pb) + call(read) + [0x907E_0000] + call(task) + [branch(back: 5)])
            machine_set_instruction_limit(machine_instruction_count() + 60_000_000)
            machine_drain_begin(true)
            #expect(machine_run(50_000_000) == RUN_DRAINED)
            machine_set_instruction_limit(0)
            #expect(rd32(storage + 4) > 1000)               // it waited in Prime
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            #expect(String(cString: machine_drain_point()) == "PBReadSync (abandoned)")
            #expect(g_ppc.pc == rd32(read))                 // on the NATIVE word
            #expect(gpr(3) == pb)
            #expect(rd32(waitingDriverQueue()) == 0)
            #expect(rd32(result) == 0x7777_7777)
            let snap = snapshot("ppcdrvwait.bbhib")
            machine_drain_end()
            restore(snap)
            wr16(storage, 1)                                // the line comes
            _ = machine_run(5_000_000)
            #expect(rd16(storage + 2) == 2)                 // the call made again, and answered
            #expect(rd32(result) == 0)
            #expect(rd8(buf) == 0x58)
        }

        static let noteLevel: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
            levelSeen.append(drain_program_level())
            c!.pointee.pc = c!.pointee.lr
        }

        /// PowerPC code a native calls (a callback, a 68k program's call
        /// through Mixed Mode) runs under a host frame: not the program's
        /// level, so nothing stops or abandons there.
        @Test func aCallFromNativeCodeIsNotTheProgramsLevel() {
            defer { g_ppc_mode = false }
            let note = cfm_native_tvector(Self.noteLevel, "TestLevel")
            let routine = tvector(code([0x7FC8_02A6] + call(note) + [0x7FC8_03A6, 0x4E80_0020])) // mflr r30; TestLevel; mtlr r30; blr
            _ = start(call(note) + [branch(back: 5)])
            levelSeen = []
            _ = machine_run(20)
            #expect(levelSeen.first == true)               // the program's own call
            levelSeen = []
            machine_drain_begin(true)
            _ = ppc_call(routine, nil, 0)
            #expect(levelSeen == [false])
            #expect(machine_drain_state() == Int32(MACHINE_DRAIN_GOING)) // no stop under the host frame
            machine_drain_end()
        }

        /// A PowerPC program running its event loop inside its Apple Event
        /// handler (UnrealFinder, HexaSuper): the handler is called from the
        /// program's own code, so the machine stops on its idle call there,
        /// and when the handler returns, restored, AEProcessAppleEvent
        /// answers its result.
        @Test func aLoopInsideAnAppleEventHandlerHibernates() {
            defer { g_ppc_mode = false }
            launchTestApp(sizeFlags: 0x0040)
            _ = cfm_ppc_cpu()
            g_cpu!.pointee.halted = false
            g_ppc.halted = false
            let task = export("SystemTask"), process = export("AEProcessAppleEvent")
            let vars = ptr(12) // count, flag, result
            // handler: loop { count += 1; SystemTask } until flag; return -1700
            let handler = tvector(code(
                [0x7FC8_02A6] + load(29, vars)                                   // mflr r30
                    + [0x839D_0000, 0x3B9C_0001, 0x939D_0000] + call(task)      // count += 1; SystemTask
                    + [0x839D_0004, 0x2C1C_0000, 0x4182_FFD8]                   // until flag
                    + [0x3860_F95C, 0x7FC8_03A6, 0x4E80_0020]))                 // li r3,-1700; mtlr r30; blr
            #expect(tb_AEInstallEventHandler(Int32(bitPattern: fourCC("aevt")), Int32(bitPattern: fourCC("oapp")),
                                             handler, 0, 0) == 0)
            machine_send_apple_event(fourCC("aevt"), fourCC("oapp"), nil, 0, nil, nil, nil)
            let e = ptr(UInt32(EventRecord_SIZEOF))
            #expect(tb_GetNextEvent(0x0400, e) != 0 && rd16(e) == 23)
            wr32(vars + 8, 0x7777)
            // result := AEProcessAppleEvent(e); loop { SystemTask }
            _ = start(load(29, vars) + load(3, e) + call(process) + [0x907D_0008] + call(task) + [branch(back: 5)])
            _ = machine_run(200_000)
            #expect(rd32(vars) > 0)                         // in the handler
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            #expect(String(cString: machine_drain_point()) == "SystemTask")
            let snap = snapshot("ppcaehandler.bbhib")
            machine_drain_end()
            restore(snap)
            let n = rd32(vars)
            _ = machine_run(200_000)
            #expect(rd32(vars) > n)
            #expect(rd32(vars + 8) == 0x7777)
            wr32(vars + 4, 1)
            _ = machine_run(200_000)
            #expect(Int32(bitPattern: rd32(vars + 8)) == -1700) // the handler's result, returned to the call
        }

        /// A PowerPC program waiting in 68k code it called through
        /// CallUniversalProc (Disk Copy's dialogs, in ModalDialog): the
        /// routine runs on the guest, so the machine stops on its idle trap
        /// there; restored, it goes on, and when it returns its result
        /// reaches the PowerPC code and the 68k registers are as they were.
        @Test func waitingIn68kCodeCalledThroughMixedModeHibernates() {
            defer { g_ppc_mode = false }
            let cup = export("CallUniversalProc"), task = export("SystemTask")
            // pascal long routine(long *vars): loop { vars[0] += 1; SystemTask } until vars[1]; return $BEE
            let words: [UInt16] = [0x206F, 0x0004, 0x5290, 0xA9B4, 0x206F, 0x0004, 0x4AA8, 0x0004, 0x67F2,
                                   0x205F, 0x588F, 0x2EBC, 0x0000, 0x0BEE, 0x4ED0]
            let routine = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(routine + UInt32(i * 2), w) }
            let vars = ptr(12) // count, flag, result
            wr32(vars + 8, 0x7777)
            let sp68 = ptr(65536) + 65000
            g_cpu!.pointee.a.7 = sp68
            g_cpu!.pointee.d.7 = 0x1234_5678
            // vars[2] := CallUniversalProc(routine, kPascalStackBased | RESULT_SIZE(4) | STACK_ROUTINE_PARAMETER(1, 4), vars)
            _ = start(load(29, vars) + load(3, routine) + [0x3880_00F0, 0x7FA5_EB78] + call(cup) + [0x907D_0008]
                      + call(task) + [branch(back: 5)])
            _ = machine_run(200_000)
            #expect(rd32(vars) > 0)                         // in the 68k routine
            #expect(mm_level_68k())
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            #expect(String(cString: machine_drain_point()) == "SystemTask")
            let snap = snapshot("ppc68klevel.bbhib")
            machine_drain_end()
            restore(snap)
            #expect(mm_level_68k())
            let n = rd32(vars)
            _ = machine_run(200_000)
            #expect(rd32(vars) > n)
            #expect(rd32(vars + 8) == 0x7777)
            wr32(vars + 4, 1)
            _ = machine_run(200_000)
            #expect(rd32(vars + 8) == 0xBEE)                // the routine's result, returned to the call
            #expect(!mm_level_68k())
            #expect(g_cpu!.pointee.a.7 == sp68)             // the 68k registers as they were
            #expect(g_cpu!.pointee.d.7 == 0x1234_5678)
        }

        /// 68k code called through CallUniversalProc from a callback (under
        /// a host frame) is not the program's level: run from native code
        /// as before, and a drain there is refused, the program being in a
        /// routine of its own the Toolbox called.
        @Test func waitingIn68kCodeCalledFromACallbackIsRefused() {
            defer { g_ppc_mode = false }
            let cup = export("CallUniversalProc")
            let words: [UInt16] = [0xA9B4, 0x60FC] // loop: _SystemTask; BRA.S loop
            let routine = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(routine + UInt32(i * 2), w) }
            g_cpu!.pointee.a.7 = ptr(65536) + 65000
            // a callback: CallUniversalProc(routine, kPascalStackBased)
            let callback = tvector(code(load(3, routine) + [0x3880_0000] + call(cup) + [branch(back: 0)]))
            _ = start([branch(back: 0)])
            machine_hibernate_at(machine_instruction_count() + 1_000_000)
            machine_set_instruction_limit(machine_instruction_count() + 6_000_000)
            _ = ppc_call(callback, nil, 0)
            #expect(!mm_level_68k())
            _ = drain_slice_point() // machine_run's, the machine stopped at the limit
            machine_hibernate_at(0)
            machine_set_instruction_limit(0)
            #expect(machine_drain_state() == Int32(MACHINE_DRAIN_REFUSED))
            let why = String(cString: machine_drain_reason())
            #expect(why.contains("a routine of its own called by the Toolbox"), Comment(rawValue: why))
            machine_drain_end()
        }

        /// A PowerPC thread waiting in YieldToThread is saved with the
        /// program; restored, it goes on: its YieldToThread answers noErr and
        /// its loop runs again.
        @Test func aPowerPCThreadGoesOnAfterARestore() {
            defer { g_ppc_mode = false }
            let task = export("SystemTask"), yield = export("YieldToThread")
            let vars = ptr(8) // result, count
            _ = start([0x3BFF_0001] + call(task) + [0x3860_0000] + call(yield) + [branch(back: 12)])
            let entry = tvector(code(
                load(29, vars)
                    + [0x839D_0004, 0x3B9C_0001, 0x939D_0004, 0x3860_0000] // count += 1; r3 = 0
                    + call(yield) + [0x907D_0000, branch(back: 10)]))       // result := YieldToThread(0)
            let made = ptr(4)
            #expect(tb_NewThread(1, entry, 0, 0, 0, 0, made) == 0)
            _ = machine_run(200_000)
            #expect(rd32(vars + 4) > 0)
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernate_blocker() == nil)
            #expect(threads_parked() == 1)
            let snap = snapshot("ppcthread.bbhib")
            machine_drain_end()
            restore(snap)
            let count = rd32(vars + 4)
            wr32(vars, 0x7777)
            _ = machine_run(200_000)
            #expect(rd32(vars + 4) > count)
            #expect(rd32(vars) == 0)                       // noErr
        }

        /// A program whose event loop runs in a thread, its main thread
        /// waiting in YieldToThread (Diablo): the thread is set aside at its
        /// idle call, the main thread's YieldToThread abandoned, and the
        /// machine drains there.  Going on, here or restored, the main
        /// thread yields again and the thread's loop runs on; the main
        /// thread's YieldToThread never returns meanwhile.
        @Test func anEventLoopInAThreadHibernates() {
            defer {
                g_ppc_mode = false
                machine_set_instruction_limit(0)
                machine_hibernate_at(0)
            }
            let task = export("SystemTask"), yield = export("YieldToThread")
            let vars = ptr(8) // the main thread's count, the thread's
            let entry = tvector(code(load(29, vars) + [0x839D_0004, 0x3B9C_0001, 0x939D_0004] // count += 1
                                     + call(task) + [branch(back: 8)]))                    // SystemTask; loop
            let made = ptr(4)
            #expect(tb_NewThread(1, entry, 0, 0, 0, 0, made) == 0)
            // the main thread: loop { YieldToThread(thread); main count += 1 }
            _ = start(load(29, vars) + load(3, rd32(made)) + call(yield)
                      + [0x839D_0000, 0x3B9C_0001, 0x939D_0000, branch(back: 10)])
            machine_hibernate_at(machine_instruction_count() + 300_000)
            machine_set_instruction_limit(machine_instruction_count() + 20_000_000)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            #expect(String(cString: machine_drain_point()) == "YieldToThread (abandoned)")
            #expect(thread_current_index() == 0)
            #expect(threads_parked() == 1)
            #expect(rd32(vars) == 0 && rd32(vars + 4) > 0)
            let snap = snapshot("ppcthreadloop.bbhib")
            machine_drain_end()
            func goOn() {
                let count = rd32(vars + 4)
                machine_set_instruction_limit(machine_instruction_count() + 400_000)
                _ = machine_run(10_000_000)
                #expect(rd32(vars + 4) > count)              // the thread's loop runs on
                #expect(rd32(vars) == 0)                     // the main thread still waits
            }
            goOn()
            restore(snap)
            goOn()
        }

        /// The same for a 68k program: its thread set aside at _SystemTask,
        /// its main thread's _ThreadDispatch abandoned and made again.
        @Test func a68kEventLoopInAThreadHibernates() {
            defer {
                machine_set_instruction_limit(0)
                machine_hibernate_at(0)
            }
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            func code68(_ words: [UInt16]) -> UInt32 {
                let p = tb_NewPtr(Int32(words.count * 2), false, false)
                for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
                return p
            }
            let vars = ptr(8) // the main thread's count, the thread's
            // the thread: loop { count += 1; _SystemTask }
            let entry = code68([0x52B9, hi(vars + 4), lo(vars + 4), 0xA9B4, 0x60F6])
            let made = ptr(4)
            #expect(tb_NewThread(1, entry, 0, 0, 0, 0, made) == 0)
            let id = rd32(made)
            // the program: loop { YieldToThread(thread); main count += 1 }
            let app = code68([0x4267, 0x2F3C, hi(id), lo(id), 0x303C, 0x0205, 0xABF2, 0x548F,
                              0x52B9, hi(vars), lo(vars), 0x60E8])
            g_cpu!.pointee.a.7 = ptr(65536) + 65000
            g_cpu!.pointee.sr = 0x2000
            g_cpu!.pointee.pc = app
            machine_hibernate_at(machine_instruction_count() + 300_000)
            machine_set_instruction_limit(machine_instruction_count() + 20_000_000)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            #expect(String(cString: machine_drain_point()).hasSuffix("(abandoned)"))
            #expect(g_cpu!.pointee.pc == app + 12)          // on the _ThreadDispatch, to be made again
            #expect(threads_parked() == 1)
            #expect(rd32(vars) == 0 && rd32(vars + 4) > 0)
            let snap = snapshot("68kthreadloop.bbhib")
            machine_drain_end()
            func goOn() {
                let count = rd32(vars + 4)
                machine_set_instruction_limit(machine_instruction_count() + 400_000)
                _ = machine_run(10_000_000)
                #expect(rd32(vars + 4) > count)
                #expect(rd32(vars) == 0)
            }
            goOn()
            restore(snap)
            goOn()
        }

        /// The same for a 68k thread waiting in _ThreadDispatch: its
        /// YieldToThread's result on the stack, its loop going on; also
        /// with D0's high word set, as the selector is loaded with MOVE.W
        /// (IPNetRouter's threads had FFFF0512).
        @Test(arguments: [false, true]) func a68kThreadGoesOnAfterARestore(highWordSet: Bool) {
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            func code68(_ words: [UInt16]) -> UInt32 {
                let p = tb_NewPtr(Int32(words.count * 2), false, false)
                for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
                return p
            }
            let vars = ptr(8) // result (word), count
            let dirty: [UInt16] = highWordSet ? [0x70FF] : [] // moveq #-1,d0
            let yieldCall: [UInt16] = [0x4267, 0x42A7] + dirty + [0x303C, 0x0205, 0xABF2] // YieldToThread(kNoThreadID)
            let back = UInt16(yieldCall.count * 2)
            // pascal entry: loop { count += 1; result := YieldToThread(0) }
            let entry = code68([0x52B9, hi(vars + 4), lo(vars + 4)] + yieldCall + [0x33DF, hi(vars), lo(vars), 0x6000 | (0xF2 - back)])
            let made = ptr(4)
            #expect(tb_NewThread(1, entry, 0, 0, 0, 0, made) == 0)
            // the program: loop { _SystemTask; YieldToThread(0) }
            let app = code68([0xA9B4] + yieldCall + [0x548F, 0x6000 | (0xFA - back)])
            g_cpu!.pointee.a.7 = ptr(65536) + 65000
            g_cpu!.pointee.sr = 0x2000
            g_cpu!.pointee.pc = app
            _ = machine_run(200_000)
            #expect(rd32(vars + 4) > 0)
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(String(cString: machine_drain_point()) == "SystemTask")
            #expect(machine_hibernate_blocker() == nil)
            #expect(threads_parked() == 1)
            let snap = snapshot("68kthread\(highWordSet ? "-d0" : "").bbhib")
            machine_drain_end()
            restore(snap)
            let count = rd32(vars + 4)
            wr16(vars, 0x7777)
            _ = machine_run(200_000)
            #expect(rd32(vars + 4) > count)
            #expect(rd16(vars) == 0)                       // noErr
        }
    }
}
