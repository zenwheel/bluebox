import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// A MacsBug command run at once, its output.
func debugCommand(_ line: String) -> String {
    var buf = [CChar](repeating: 0, count: 1 << 18)
    machine_debug_command(line, &buf, buf.count)
    return String(cString: buf)
}

/// What the debugger printed at its stops since the last take.
func debugOutput() -> String {
    guard let p = machine_debug_take_output() else { return "" }
    defer { free(p) }
    return String(cString: p)
}

extension BlueBox {
    /// Blue Box's MacsBug, driven headlessly: commands in, text out, the
    /// machine stopping at breakpoints, trap breaks, steps and the
    /// program's own Debugger calls, each stop running the script's next
    /// commands until one goes on.
    @Suite struct MacsBug {
        init() {
            bootMachine()
            machine_debug_reset()
            _ = debugOutput()
        }

        func code(_ words: [UInt16]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
            return p
        }

        /// Run 68k code from p with a stack of its own.
        func run(_ p: UInt32, max: UInt64 = 100_000) -> String {
            let cpu = g_cpu!
            let stack = tb_NewPtr(65536, false, true)
            cpu.pointee.a.7 = stack + 65536 - 16
            cpu.pointee.a.6 = 0
            cpu.pointee.pc = p
            cpu.pointee.halted = false
            _ = cpu_run(cpu, max)
            return String(cString: machine_halt_reason())
        }

        @Test func aTrapBreakStopsAtTheTrapAndAStepShowsTheCall() {
            // MOVEQ #16,D0; _NewPtr; MOVE.L A0,D7; _ExitToShell
            let p = code([0x7010, 0xA11E, 0x2E08, 0xA9F4])
            #expect(debugCommand("atb NewPtr").contains("A-trap break at NewPtr"))
            machine_debug_script("rd;t;g")
            #expect(run(p).hasPrefix("ExitToShell"))
            let out = debugOutput()
            #expect(out.contains("A-trap break at NewPtr(size=16"), "\(out)")
            #expect(out.contains(" * \(hex(p + 2))  _NewPtr"), "\(out)")
            #expect(out.contains("D0 = 00000010"))
            // The step ran the trap: its result and where it stopped.
            let a0 = g_cpu!.pointee.d.7
            #expect(out.contains("NewPtr(size=16, sys_p=false, clear_p=false) = $\(hex(a0))"), "\(out)")
            #expect(out.contains("Step\n  68k at \(hex(p + 4))"), "\(out)")
            #expect(debugCommand("atd").contains("A-trap break      NewPtr"))
            #expect(debugCommand("atc").contains("cleared"))
        }

        @Test func aBreakpointStopsBeforeItsInstructionAndStepsGoOneAtATime() {
            // MOVEQ #1,D1; MOVEQ #2,D2; MOVEQ #3,D3; _ExitToShell
            let p = code([0x7201, 0x7402, 0x7603, 0xA9F4])
            #expect(debugCommand("br \(hex(p + 2))").contains("Breakpoint set at \(hex(p + 2))"))
            machine_debug_script("d2 = 1234;s;rd;g")
            #expect(run(p).hasPrefix("ExitToShell"))
            let out = debugOutput()
            #expect(out.contains("Breakpoint at $\(hex(p + 2))"), "\(out)")
            // The breakpoint's instruction ran over D2's new value; the step stopped after it.
            #expect(out.contains("Step\n  68k at \(hex(p + 4))"), "\(out)")
            #expect(out.contains("D2 = 00000002"))
            #expect(g_cpu!.pointee.d.3 == 3)
            #expect(debugCommand("brd").contains(hex(p + 2)))
            _ = debugCommand("brc")
            #expect(debugCommand("brd").contains("No breakpoints"))
        }

        @Test func aConditionalBreakpointWaitsForItsCondition() {
            // MOVEQ #0,D0; loop: ADDQ.L #1,D0; CMPI.L #10,D0; BNE loop; _ExitToShell
            let p = code([0x7000, 0x5280, 0x0C80, 0x0000, 0x000A, 0x66F6, 0xA9F4])
            _ = debugCommand("br \(hex(p + 2)) d0 = 5")
            machine_debug_script("rd;brc;g")
            #expect(run(p).hasPrefix("ExitToShell"))
            let out = debugOutput()
            #expect(out.contains("condition true"))
            #expect(out.contains("D0 = 00000005"), "\(out)")
        }

        @Test func memoryIsShownSetAndFound() {
            let buf = tb_NewPtr(64, false, true)
            _ = debugCommand("sm \(hex(buf)) 'MacsBug' 00 1234")
            #expect(rd8(buf) == 0x4D && rd8(buf + 7) == 0 && rd16(buf + 8) == 0x1234)
            let dm = debugCommand("dm \(hex(buf)) 16")
            #expect(dm.contains("\(hex(buf))  4D61 6373 4275 6700 1234"), "\(dm)")
            #expect(dm.contains("MacsBug."))
            #expect(debugCommand("dm \(hex(buf)) cstring").contains("\"MacsBug\""))
            #expect(debugCommand("f \(hex(buf)) 40 'Bug'").contains("Found at \(hex(buf + 4))"))
            #expect(debugCommand("dw \(hex(buf + 8))").contains("$00001234"))
            #expect(debugCommand("sl \(hex(buf + 12)) DEADBEEF").contains("Memory set"))
            #expect(rd32(buf + 12) == 0xDEAD_BEEF)
        }

        @Test func expressionsKnowRegistersLowMemoryAndIndirection() {
            g_cpu!.pointee.d.0 = 0x10
            wr32(0x1000, 0x1234_5678)
            #expect(debugCommand("d0 + #16").contains("$00000020"))
            #expect(debugCommand("@1000").contains("$12345678"))
            #expect(debugCommand("1000^").contains("$12345678"))
            #expect(debugCommand("ev SysZone").contains("$000002A6"))
            #expect(debugCommand("'CODE'").contains("$434F4445"))
            #expect(debugCommand("(2+3)*4").contains("#20"))
            #expect(debugCommand("ev nosuchname").contains("unknown name"))
            _ = debugCommand("a3 = 1000 + 4")
            #expect(g_cpu!.pointee.a.3 == 0x1004)
        }

        @Test func codeIsDisassembledWithTrapNames() {
            let p = code([0x4E56, 0xFFF8, 0x7001, 0xA9A0, 0x4E5E, 0x4E75])
            let il = debugCommand("il \(hex(p)) 5")
            #expect(il.contains("LINK      A6,#-$0008"), "\(il)")
            #expect(il.contains("MOVEQ     #$01,D0"))
            #expect(il.contains("_GetResource"))
            #expect(il.contains("UNLK      A6"))
            #expect(il.contains("RTS"))
            #expect(debugCommand("id \(hex(p + 4))").contains("MOVEQ"))
        }

        @Test func theStackCrawlFollowsA6Links() {
            // main: LINK A6,#0; BSR sub; UNLK A6; _ExitToShell
            // sub:  LINK A6,#0; NOP; UNLK A6; RTS
            let p = code([0x4E56, 0x0000, 0x6100, 0x0006, 0x4E5E, 0xA9F4,
                          0x4E56, 0x0000, 0x4E71, 0x4E5E, 0x4E75])
            _ = debugCommand("br \(hex(p + 16))")
            machine_debug_script("sc;mr;g")
            #expect(run(p).hasPrefix("ExitToShell"))
            let out = debugOutput()
            #expect(out.contains("return \(hex(p + 8))"), "\(out)")
            #expect(out.contains("Magic return to \(hex(p + 8))"), "\(out)")
            #expect(out.contains("68k at \(hex(p + 8))"), "\(out)")
        }

        @Test func theHeapsAreShownTotalledAndChecked() {
            let h = tb_NewHandle(100, false, false)
            #expect(h != 0)
            let hz = debugCommand("hz")
            #expect(hz.contains("System") && hz.contains(hex(rd32(UInt32(LM_SysZone)))))
            let hd = debugCommand("hd r")
            #expect(hd.contains(hex(rd32(h))), "\(hd)")
            #expect(hd.contains(" R "))
            #expect(debugCommand("ht").contains("Relocatable blocks"))
            #expect(debugCommand("hc").contains("checks out"))
            let wh = debugCommand("wh \(hex(rd32(h) + 4))")
            #expect(wh.contains("relocatable block at \(hex(rd32(h)))"), "\(wh)")
            #expect(wh.contains("master pointer) is \(hex(h))"))
            #expect(debugCommand("wh GetResource").contains("Trap GetResource ($A9A0)"))
        }

        @Test func theProgramsDebugStrStopsInMacsBugWhenAsked() {
            machine_set_debugger(Int32(DEBUGGER_STOP))
            defer { machine_set_debugger(Int32(DEBUGGER_LOG)) }
            let s = tb_NewPtr(32, false, true)
            guestPString("look here", at: s)
            // PEA s; _DebugStr; MOVEQ #7,D7; _ExitToShell
            let p = code([0x4879, hiWord(s), loWord(s), 0xABFF, 0x7E07, 0xA9F4])
            machine_debug_script("how;g")
            #expect(run(p).hasPrefix("ExitToShell"))
            let out = debugOutput()
            #expect(out.contains("DebugStr: look here\n  68k at \(hex(p + 8))"), "\(out)")
            #expect(g_cpu!.pointee.d.7 == 7)
            #expect(machine_debugger_entries() == 1)
            // DX off: passed by.
            #expect(debugCommand("dx off").contains("DX off"))
            machine_debug_script("rd;g")
            #expect(run(p).hasPrefix("ExitToShell"))
            #expect(!debugOutput().contains("DebugStr: look here"))
            machine_debug_reset()
        }

        @Test func trapRecordsKeepCallsAndResults() {
            _ = debugCommand("atr on")
            // MOVEQ #8,D0; _NewHandle; _ExitToShell
            #expect(run(code([0x7008, 0xA122, 0xA9F4])).hasPrefix("ExitToShell"))
            let atp = debugCommand("atp")
            #expect(atp.contains("NewHandle(size=8, sys_p=false, clear_p=false) = $"), "\(atp)")
            _ = debugCommand("atr off")
        }

        @Test func aStepSpyStopsWhenMemoryChanges() {
            let v = tb_NewPtr(4, false, true)
            // MOVEQ #1,D0; MOVEQ #2,D1; MOVE.L D1,(v).L; MOVEQ #3,D2; _ExitToShell
            let p = code([0x7001, 0x7202, 0x23C1, hiWord(v), loWord(v), 0x7403, 0xA9F4])
            _ = debugCommand("ss \(hex(v))")
            machine_debug_script("ss;g")
            #expect(run(p).hasPrefix("ExitToShell"))
            let out = debugOutput()
            #expect(out.contains("Step spy"), "\(out)")
            #expect(out.contains("68k at \(hex(p + 10))"), "\(out)")
        }

        @Test func theHostsBreakStopsAtTheNextInstruction() {
            let p = code([0x7001, 0x7202, 0xA9F4])
            machine_debug_break()
            macsbug_poll() // as the machine's run loop takes it
            machine_debug_script("g")
            #expect(run(p).hasPrefix("ExitToShell"))
            #expect(debugOutput().contains("User break\n  68k at \(hex(p))"))
        }

        /// PowerPC: a call to a Toolbox routine breaks before it is made,
        /// with its arguments from r3 on; stepping makes it and shows its
        /// result; a breakpoint in PowerPC code stops there.
        @Test func powerPCCallsBreakAndStep() {
            _ = launchTestApp()
            _ = cfm_ppc_cpu()
            g_cpu!.pointee.halted = false
            g_ppc.halted = false
            g_ppc_mode = true
            defer { g_ppc_mode = false }
            let tv = cfm_native_tvector(cfm_export_fn("NewPtr")!, "NewPtr")
            func load(_ r: UInt32, _ v: UInt32) -> [UInt32] {
                [0x3C00_0000 | r << 21 | v >> 16, 0x6000_0000 | r << 21 | r << 16 | (v & 0xFFFF)]
            }
            // r3 = 24; r12 = tv; lwz r0,0(r12); mtctr r0; bctrl; mr r31,r3; li r30,5; b .
            let words = load(3, 24) + load(12, tv) + [0x800C_0000, 0x7C09_03A6, 0x4E80_0421, 0x7C7F_1B78,
                                                     0x3BC0_0005, 0x4800_0000]
            let p = tb_NewPtr(Int32(words.count * 4), false, false)
            for (i, w) in words.enumerated() { wr32(p + UInt32(i * 4), w) }
            withUnsafeMutableBytes(of: &g_ppc.gpr) {
                $0.storeBytes(of: tb_NewPtr(65536, false, true) + 60000, toByteOffset: 4, as: UInt32.self)
            }
            _ = debugCommand("atb NewPtr")
            _ = debugCommand("br \(hex(p + 0x20))")
            machine_debug_script("rd;t;g;il pc 2;g")
            g_ppc.pc = p
            _ = ppc_run(&g_ppc, 200)
            let out = debugOutput()
            #expect(out.contains("PowerPC call break at NewPtr(size=24)"), "\(out)")
            #expect(out.contains("R3 = 00000018"), "\(out)")
            let r31 = withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 31 * 4, as: UInt32.self) }
            #expect(out.contains("NewPtr(size=24) = $\(hex(r31))"), "\(out)")
            #expect(out.contains("Step\n  PowerPC at \(hex(p + 0x1C))"), "\(out)")
            #expect(out.contains("Breakpoint at $\(hex(p + 0x20))"), "\(out)")
            #expect(out.contains("li        r30,0x0005"), "\(out)")
        }

        /// The stack crawl crosses from PowerPC code to the 68k code that
        /// called it through Mixed Mode.
        @Test func theStackCrawlCrossesMixedMode() {
            let stack68 = tb_NewPtr(4096, false, true) + 4000
            let ppcStack = tb_NewPtr(4096, false, true) + 4000
            // A 68k frame: A6 -> (old A6 = 0, return 68kRet).
            let frame68 = stack68 - 64
            wr32(frame68, 0)
            wr32(frame68 + 4, 0x0001_2340)
            let cpu = g_cpu!
            cpu.pointee.a.6 = frame68
            cpu.pointee.a.7 = frame68 - 16
            cpu.pointee.pc = 0x0001_2000
            // Into PowerPC code: its first frame at ppcStack, the callee's
            // frames below it with their saved LRs.
            macsbug_switch_push(true, ppcStack)
            defer { macsbug_switch_pop() }
            let inner = ppcStack - 128, mid = ppcStack - 64
            wr32(inner, mid)
            wr32(mid, ppcStack)
            wr32(mid + 8, 0x0003_0010) // inner's caller
            wr32(ppcStack, 0)
            withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: inner, toByteOffset: 4, as: UInt32.self) }
            g_ppc.pc = 0x0003_0100
            g_ppc.lr = 0x0003_0050
            g_ppc_mode = true
            defer { g_ppc_mode = false }
            let sc = debugCommand("sc")
            #expect(sc.contains("PowerPC  PC 00030100"), "\(sc)")
            #expect(sc.contains("return 00030010"), "\(sc)")
            #expect(sc.contains("-- Mixed Mode: called from 68k code --"), "\(sc)")
            #expect(sc.contains("68k      PC 00012000"), "\(sc)")
            #expect(sc.contains("return 00012340"), "\(sc)")
        }

        @Test func helpListsTheCommands() {
            let h = debugCommand("help")
            for c in ["ATB", "BR addr", "SC7", "HD", "WH", "MR", "IL"] { #expect(h.contains(c)) }
        }
    }
}

private func hex(_ v: UInt32) -> String { String(format: "%08X", v) }
