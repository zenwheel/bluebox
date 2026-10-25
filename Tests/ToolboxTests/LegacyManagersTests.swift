import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

/// Call a PowerPC export with its arguments in r3 on; r3 after.
@discardableResult
func ppcExport(_ name: String, _ args: [UInt32]) -> UInt32 {
    withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
        for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
    }
    guard let fn = cfm_export_fn(name) else {
        Issue.record("no export \(name)")
        return 0
    }
    fn(&g_ppc)
    return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
}

func ppcErr(_ name: String, _ args: [UInt32]) -> Int16 { Int16(truncatingIfNeeded: ppcExport(name, args)) }

/// Run hand-assembled 68k code (ending in _ExitToShell) with a fresh stack.
@discardableResult
func run68k(_ words: [UInt16], max: UInt64 = 100_000) -> String {
    let cpu = g_cpu!
    let code = tb_NewPtr(Int32(words.count * 2), false, false)
    for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
    let stack = tb_NewPtr(65536, false, true)
    cpu.pointee.a.7 = stack + 65536 - 16
    cpu.pointee.pc = code
    cpu.pointee.halted = false
    _ = cpu_run(cpu, max)
    return String(cString: machine_halt_reason())
}

func hiWord(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
func loWord(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }

extension BlueBox {
    /// The Power Manager of a desktop Mac (no 'powr' in Gestalt), and the
    /// Start Manager's defaults in parameter RAM.
    @Suite struct PowerAndStartManagers {
        init() { bootMachine() }

        @Test func aDesktopHasNothingToManage() {
            var r: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("powr")), &r) == -5550)
            // _PowerDispatch: no selectors, features, batteries or timeouts.
            let reason = run68k([
                0x7000, 0xA09E, 0x3E00,         // PMSelectorCount; MOVE.W D0,D7
                0x701A, 0xA09E, 0x3C00,         // BatteryCount; MOVE.W D0,D6
                0x7001, 0xA09E, 0x2A00,         // PMFeatures; MOVE.L D0,D5
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && g_cpu!.pointee.d.6 & 0xFFFF == 0 && g_cpu!.pointee.d.5 == 0)
            #expect(ppcExport("PMSelectorCount", []) == 0 && ppcExport("BatteryCount", []) == 0)
            #expect(ppcExport("PMFeatures", []) == 0)
            #expect(ppcExport("GetSleepTimeout", []) == 0 && ppcExport("GetDimmingTimeout", []) == 0)
            // What went to a portable's PMU: unimpErr.
            let b = tb_NewPtr(8, false, true)
            #expect(ppcErr("BatteryStatus", [b, b + 1]) == -4)
            #expect(ppcErr("ModemStatus", [b]) == -4)
            #expect(ppcErr("GetWUTime", [b, b + 4]) == -4)
            #expect(ppcErr("SetWUTime", [1000]) == -4 && ppcErr("DisableWUTime", []) == -4)
            for name in ["AOn", "AOnIgnoreModem", "BOn", "AOff", "BOff"] { ppcExport(name, []) }
            #expect(ppcExport("GetCPUSpeed", []) == 33) // the Quadra 800
        }

        @Test func startupDefaultsStayInParameterRAM() {
            let rec = tb_NewPtr(8, false, true), back = tb_NewPtr(8, false, true)
            wr32(rec, 0xFFDF_0000) // sdRefNum -33: a SCSI drive
            let reason = run68k([
                0x207C, hiWord(rec), loWord(rec), 0xA07E,     // SetDefaultStartup
                0x207C, hiWord(back), loWord(back), 0xA07D,   // GetDefaultStartup
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(rd32(back) == 0xFFDF_0000)
            wr16(rec, 0x0001)
            ppcExport("SetOSDefault", [rec])
            ppcExport("GetOSDefault", [back + 4])
            #expect(rd16(back + 4) == 1)
            wr16(rec, 0x0080) // slot 0, sResource $80
            ppcExport("SetVideoDefault", [rec])
            gmemset(back, 0, 8)
            ppcExport("GetVideoDefault", [back])
            #expect(rd16(back) == 0x0080)
            ppcExport("SetTimeout", [15])
            ppcExport("GetTimeout", [back])
            #expect(rd16(back) == 15)
        }
    }

    /// The SCSI Manager for a bus with only the Mac on it, and an ATA
    /// Manager with no buses.
    @Suite struct SCSIManager {
        init() { bootMachine() }

        @Test func noTargetAnswersSelection() {
            let reason = run68k([
                0x4267, 0x3F3C, 0x0001, 0xA815, 0x3E1F,         // SCSIGet -> D7
                0x4267, 0x3F3C, 0x0003, 0x3F3C, 0x0002, 0xA815, 0x3C1F, // SCSISelect(3) -> D6
                0x4267, 0x42A7, 0x3F3C, 0x0006, 0x3F3C, 0x0003, 0xA815, 0x3A1F, // SCSICmd(nil, 6) -> D5
                0x4267, 0x3F3C, 0x000A, 0xA815, 0x381F,         // SCSIStat -> D4
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            let c = g_cpu!.pointee
            #expect(c.d.7 & 0xFFFF == 0)   // the bus is ours
            #expect(c.d.6 & 0xFFFF == 2)   // scCommErr: selection timed out
            #expect(c.d.5 & 0xFFFF == 8)   // scSequenceErr: nobody connected
            #expect(c.d.4 & 0xFFFF == 0)   // no lines asserted
            #expect(ppcErr("SCSISelect", [1]) == 8) // without SCSIGet
            #expect(ppcErr("SCSIGet", []) == 0 && ppcErr("SCSIGet", []) == 7) // scMgrBusyErr
            #expect(ppcErr("SCSIComplete", [0, 0, 60]) == 8)
            #expect(ppcErr("SCSIGet", []) == 0 && ppcErr("SCSIReset", []) == 0 && ppcErr("SCSIGet", []) == 0)
        }

        @Test func scsiManager43SeesOneEmptyBus() {
            var r: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("scsi")), &r) == 0 && r & 1 == 1)
            let pb = tb_NewPtr(176, false, true)
            wr16(pb + 6, 176)
            wr8(pb + 8, 3)                   // SCSIBusInquiry
            #expect(run68k([0x207C, hiWord(pb), loWord(pb), 0x7001, 0xA089, 0x3E00, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            #expect(rd8(pb + 69) == 7 && rd8(pb + 68) == 0 && rd16(pb + 84) == 7 && rd16(pb + 44) == 176)
            wr8(pb + 8, 1)                   // SCSIExecIO to target 2
            wr32(pb + 12, 0x0000_0200)
            wr32(pb + 44, 512)               // scsiDataLength
            #expect(ppcErr("SCSIAction", [pb]) == -7926) // scsiSelectTimeout
            #expect(Int16(bitPattern: rd16(pb + 10)) == -7926 && rd32(pb + 64) == 512)
            wr32(pb + 12, 0x0000_0700)       // the Mac itself
            #expect(ppcErr("SCSIAction", [pb]) == -7868)
            wr32(pb + 12, 0x0001_0200)       // bus 1
            #expect(ppcErr("SCSIAction", [pb]) == -7869)
            wr8(pb + 8, 0x80)                // SCSIGetVirtualIDInfo: ID 4 isn't there
            wr16(pb + 36, 4)
            wr8(pb + 38, 1)
            #expect(ppcErr("SCSIAction", [pb]) == 0 && rd8(pb + 38) == 0 && rd8(pb + 14) == 4)
            // A driver's cross-reference, looked up and removed.
            wr8(pb + 8, 0x85)
            wr32(pb + 12, 0x0000_0300)
            wr16(pb + 36, UInt16(bitPattern: -40))
            #expect(ppcErr("SCSIAction", [pb]) == 0)
            wr16(pb + 36, 0)
            wr8(pb + 8, 0x86)
            #expect(ppcErr("SCSIAction", [pb]) == 0 && Int16(bitPattern: rd16(pb + 36)) == -40)
            #expect(rd32(pb + 40) == 0xFFFF_FFFF)
            wr8(pb + 8, 0x87)
            #expect(ppcErr("SCSIAction", [pb]) == 0)
            wr8(pb + 8, 0x86)
            #expect(ppcErr("SCSIAction", [pb]) == -7882) // scsiNoSuchXref
            #expect(ppcErr("SCSIRegisterBus", [pb]) == -7871 && ppcErr("SCSIKillXPT", [pb]) == -7871)
            #expect(ppcErr("SCSIDeregisterBus", [pb]) == -7869)
        }

        @Test func ataManagerHasNoBuses() {
            let pb = tb_NewPtr(96, false, true)
            wr8(pb + 6, 1)
            wr8(pb + 18, 0x90) // kATAMgrManagerInquiry
            #expect(ppcErr("ataManager", [pb]) == 0)
            #expect(rd8(pb + 52) == 1 && rd16(pb + 54) == 0 && rd16(pb + 56) == 0)
            wr8(pb + 18, 0x01) // kATAMgrExecIO
            #expect(ppcErr("ataManager", [pb]) == -9406 + 0x2F)
            wr8(pb + 18, 0x77)
            #expect(ppcErr("ataManager", [pb]) == -9406 + 0x30)
        }
    }
}
