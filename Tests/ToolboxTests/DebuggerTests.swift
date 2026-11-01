import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// The log written while `body` runs.
func capturingLog(_ body: () -> Void) -> String {
    let file = tmpfile()
    let saved = g_log_file
    g_log_file = file
    body()
    g_log_file = saved
    guard let file else { return "" }
    fflush(file)
    rewind(file)
    var out = [UInt8]()
    var buf = [UInt8](repeating: 0, count: 4096)
    while true {
        let n = fread(&buf, 1, buf.count, file)
        if n == 0 { break }
        out += buf[0..<n]
    }
    fclose(file)
    return String(decoding: out, as: UTF8.self)
}

extension BlueBox {
    /// Blue Box is a Mac with MacsBug installed: Debugger, DebugStr and
    /// SysBreak log their message and go on, as "g" in MacsBug, unless
    /// the run is to halt there.
    @Suite struct DebuggerEntries {
        init() { bootMachine() }

        func pstring(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        @Test func debugStrIsLoggedAndTheRunGoesOn() {
            let s = pstring("hello\rthere")
            var reason = ""
            let log = capturingLog {
                reason = run68k([0x4879, hiWord(s), loWord(s), 0xABFF, // PEA s; _DebugStr
                                 0xA9FF,                               // _Debugger
                                 0xA9F4])                              // _ExitToShell
            }
            #expect(reason.hasPrefix("ExitToShell"))
            #expect(machine_debugger_entries() == 2 && machine_debugger_places() == 2)
            #expect(String(cString: machine_debugger_last()) == "Debugger")
            #expect(log.contains("debugger: DebugStr \"hello there\" at "))
            #expect(log.contains("debugger: Debugger at "))
            var line = [CChar](repeating: 0, count: 512)
            machine_debugger_summary(&line, line.count)
            #expect(String(cString: line).hasPrefix("debugger entered 2 times at 2 places, went on"))
        }

        @Test func aLoopIsLoggedByPowersOfTen() {
            let s = pstring("assert")
            var reason = ""
            let log = capturingLog {
                reason = run68k([0x3E3C, 0x03E7,                        // MOVE.W #999,D7
                                 0x4879, hiWord(s), loWord(s), 0xABFF,  // PEA s; _DebugStr
                                 0x51CF, 0xFFF6,                        // DBRA D7 back to the PEA
                                 0xA9F4], max: 1_000_000)
            }
            #expect(reason.hasPrefix("ExitToShell"))
            #expect(machine_debugger_entries() == 1000 && machine_debugger_places() == 1)
            let lines = log.split(separator: "\n").filter { $0.hasPrefix("debugger: DebugStr \"assert\"") }
            #expect(lines.count == 4)
            #expect(lines.last?.contains("(1000 times)") == true)
        }

        @Test func haltingStopsTheRunAsBefore() {
            machine_set_debugger(Int32(DEBUGGER_HALT))
            defer { machine_set_debugger(Int32(DEBUGGER_LOG)) }
            let s = pstring("stop here")
            #expect(run68k([0x4879, hiWord(s), loWord(s), 0xABFF, 0xA9F4]) == "DebugStr: stop here")
            #expect(run68k([0xA9FF, 0xA9F4]) == "Debugger() called")
        }

        @Test func sysBreakCodesEnterTheDebuggerAndPopTheirArgument() {
            let s = pstring("break")
            let reason = run68k([0x2C0F,                                // MOVE.L A7,D6
                                 0x4879, hiWord(s), loWord(s),          // PEA s
                                 0x303C, 0xFE15, 0xA9C9,                // SysBreakStr
                                 0x303C, 0xFE16, 0xA9C9,                // SysBreak
                                 0x2A0F,                                // MOVE.L A7,D5
                                 0xA9F4])
            #expect(reason.hasPrefix("ExitToShell"))
            #expect(g_cpu.pointee.d.5 == g_cpu.pointee.d.6)
            #expect(machine_debugger_entries() == 2)
            #expect(String(cString: machine_debugger_last()) == "SysBreak")
            // Any other code is still the bomb box.
            #expect(run68k([0x7019, 0xA9C9, 0xA9F4]) == "SysError 25")
        }

        @Test func callingThroughMacJmpReturns() {
            let reason = run68k([0x2078, 0x0120,  // MOVEA.L $120.w,A0
                                 0x4E90,          // JSR (A0)
                                 0xA9F4])
            #expect(reason.hasPrefix("ExitToShell"))
            #expect(String(cString: machine_debugger_last()) == "MacJmp")
        }

        @Test func theRoutinesMacsBugNameIsLogged() {
            // DebugStr in a routine whose symbol follows its RTS.
            let s = pstring("named")
            let name = Array("MYROUTINE".utf8)
            var words: [UInt16] = [0x4879, hiWord(s), loWord(s), 0xABFF, 0xA9F4, 0x4E75, UInt16(0x80 + name.count) << 8 | UInt16(name[0])]
            for i in stride(from: 1, to: name.count, by: 2) {
                words.append(UInt16(name[i]) << 8 | UInt16(i + 1 < name.count ? name[i + 1] : 0))
            }
            let log = capturingLog { run68k(words) }
            #expect(log.contains("\"named\" at ") && log.contains(" in MYROUTINE"))
        }

        @Test func powerPCEntriesLogAndReturn() {
            let s = pstring("from PowerPC")
            let c = tb_NewPtr(16, false, true)
            for (i, b) in "C string".utf8.enumerated() { wr8(c + UInt32(i), b) }
            g_ppc.lr = 0x1234_5678
            _ = ilCall("DebugStr", [s])
            #expect(g_ppc.pc == 0x1234_5678)
            #expect(String(cString: machine_debugger_last()) == "DebugStr: from PowerPC")
            _ = ilCall("debugstr", [c])
            #expect(String(cString: machine_debugger_last()) == "DebugStr: C string")
            _ = ilCall("SysBreakStr", [s])
            _ = ilCall("Debugger", [])
            _ = ilCall("SysDebug", [])
            #expect(machine_debugger_entries() == 5)
            #expect(String(cString: machine_debugger_last()) == "Debugger")
        }
    }
}
