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

    /// The Slot Manager's declaration ROM, slot information and table.
    /// SpBlock: spResult 0, spsPointer 4, spSize 8, spOffsetData 12,
    /// spParamData 24, spRefNum 38, spCategory 40, spCType 42, spSlot 49,
    /// spID 50, spExtDev 51, spByteLanes 53, spFlags 54.
    @Suite struct SlotManager {
        init() { bootMachine() }

        func spBlock(slot: UInt8 = 0, id: UInt8 = 0) -> UInt32 {
            let spb = tb_NewPtr(56, false, true)
            wr8(spb + 49, slot)
            wr8(spb + 50, id)
            return spb
        }

        @Test func theBuiltInSlotAndEmptyNuBusSlots() {
            let info = tb_NewPtr(24, false, true), spb = spBlock()
            wr32(spb, info)
            #expect(ppcErr("SReadInfo", [spb]) == 0)
            let dir = rd32(info)
            #expect(ppcErr("SCkCardStat", [spb]) == 0)
            wr8(spb + 49, 9)
            #expect(ppcErr("SReadInfo", [spb]) == -300 && ppcErr("SCkCardStat", [spb]) == -300) // smEmptySlot
            wr8(spb + 49, 3)
            #expect(ppcErr("SReadInfo", [spb]) == -337) // smSlotOOBErr

            // The format block at the top of the ROM, its directory offset
            // leading to the directory SReadInfo gave.
            let fh = tb_NewPtr(20, false, true)
            wr8(spb + 49, 0)
            wr32(spb, fh)
            #expect(ppcErr("SReadFHeader", [spb]) == 0)
            #expect(rd32(fh + 14) == 0x5A93_2BC7 && rd8(fh + 13) == 1 && rd8(fh + 19) == 0x0F)
            #expect(ppcErr("SFindSInfoRecPtr", [spb]) == 0 && rd32(rd32(spb)) == dir)
            // The video's entry in the directory, and its list.
            wr32(spb + 4, dir)
            wr8(spb + 50, 0x80)
            #expect(ppcErr("SFindStruct", [spb]) == 0)
            let vlist = rd32(spb + 4)
            #expect(ppcErr("SFindSRsrcPtr", [spb]) == 0 && rd32(spb + 4) == vlist)
            #expect(ppcErr("SPtrToSlot", [spb]) == 0 && rd8(spb + 49) == 0)
            // Its device base is the screen's.
            #expect(ppcErr("SFindDevBase", [spb]) == 0 && rd32(spb) == rd32(UInt32(LM_ScrnBase)))
            wr8(spb + 50, 1)
            #expect(ppcErr("SFindDevBase", [spb]) == -330) // the board has no device memory
            #expect(ppcErr("SCardChanged", [spb]) == 0 && rd32(spb) == 0)
            #expect(ppcErr("SExec", [spb]) != 0)
            #expect(slot_manager(0x7F, spb) == -338) // smSelOOBErr
        }

        @Test func sResourcesDisabledInsertedAndDeleted() {
            let spb = spBlock(slot: 0, id: 0x80)
            wr32(spb + 24, 1) // disable
            #expect(ppcErr("SetSRsrcState", [spb]) == 0)
            let find = spBlock()
            wr16(find + 40, 3); wr16(find + 42, 1); wr16(find + 44, 1); wr8(find + 48, 1)
            #expect(ppcErr("SGetTypeSRsrc", [find]) == -344)
            wr32(find + 24, 1) // fall: disabled ones too
            wr8(find + 50, 0)
            #expect(ppcErr("SGetTypeSRsrc", [find]) == 0 && rd8(find + 50) == 0x80)
            wr32(spb + 24, 0)
            #expect(ppcErr("SetSRsrcState", [spb]) == 0)
            #expect(ppcErr("SRsrcInfo", [spb]) == 0)

            // A program's own sResource from a list in RAM: catDisplay,
            // typeVideo, its name.
            let type = tb_NewPtr(8, false, true)
            wr16(type, 3); wr16(type + 2, 1); wr16(type + 4, 0x55); wr16(type + 6, 2)
            let name = tb_NewPtr(8, false, true)
            for (i, b) in Array("Mine".utf8).enumerated() { wr8(name + UInt32(i), b) }
            let list = tb_NewPtr(16, false, true)
            wr32(list, 0x0100_0000 | ((type &- list) & 0xFFFFFF))
            wr32(list + 4, 0x0200_0000 | ((name &- (list + 4)) & 0xFFFFFF))
            wr32(list + 8, 0xFF00_0000)
            let ins = spBlock(slot: 0, id: 0x90)
            wr32(ins + 4, list)
            wr16(ins + 38, UInt16(bitPattern: -60))
            #expect(ppcErr("InsertSRTRec", [ins]) == 0)
            let q = spBlock(slot: 0, id: 0x90)
            #expect(ppcErr("SRsrcInfo", [q]) == 0)
            #expect(rd16(q + 44) == 0x55 && Int16(bitPattern: rd16(q + 38)) == -60 && rd32(q + 4) == list)
            let str = tb_NewPtr(256, false, true)
            wr32(q, str)
            #expect(ppcErr("SReadDrvrName", [q]) == 0 && pstring(at: str) == ".Mine")
            #expect(ppcErr("SDeleteSRTRec", [q]) == 0 && ppcErr("SRsrcInfo", [q]) == -351)
            wr8(ins + 49, 10)
            #expect(ppcErr("InsertSRTRec", [ins]) == -300) // nothing in slot $A
            // Built again from the ROM at startup.
            #expect(ppcErr("SInitSRsrcTable", [spb]) == 0 && ppcErr("SRsrcInfo", [spBlock(slot: 0, id: 0x80)]) == 0)
        }

        @Test func blocksAndSlotInterrupts() {
            let spb = spBlock(slot: 0, id: 0x80)
            #expect(ppcErr("SFindSRsrcPtr", [spb]) == 0)
            wr8(spb + 50, 0x83) // the 8-bit mode's list
            #expect(ppcErr("SFindStruct", [spb]) == 0)
            wr8(spb + 50, 1)    // mVidParams: a block
            #expect(ppcErr("SReadPBSize", [spb]) == 0 && rd32(spb + 8) == 46)
            #expect(ppcErr("SOffsetData", [spb]) == 0 && rd8(spb + 53) == 0x0F)
            wr8(spb + 54, 0)
            #expect(ppcErr("SCalcStep", [spb]) == 0 && rd32(spb) == 4)
            let el = tb_NewPtr(16, false, true)
            let reason = run68k([
                0x207C, hiWord(el), loWord(el), 0x700B, 0xA075, 0x3E00, // SIntInstall(el, $B)
                0x207C, hiWord(el), loWord(el), 0x700B, 0xA076, 0x3C00, // SIntRemove
                0x207C, hiWord(el), loWord(el), 0x700B, 0xA076, 0x3A00, // again: not there
                0xA9F4,
            ])
            #expect(reason == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && g_cpu!.pointee.d.6 & 0xFFFF == 0)
            #expect(Int16(bitPattern: UInt16(g_cpu!.pointee.d.5 & 0xFFFF)) == -1) // qErr
            #expect(ppcErr("SIntInstall", [el, 2]) == -337)
        }
    }

    /// The Data Access Manager with no database extension installed.
    @Suite struct DataAccessManager {
        init() { bootMachine() }

        @Test func noDdevToOpenASessionWith() {
            let sess = tb_NewPtr(4, false, true), name = tb_NewPtr(64, false, true)
            guestPString("DAL", at: name)
            // DBInit before InitDBPack, then InitDBPack(4) and DBInit again.
            let words: [UInt16] = [
                0x4267, 0x2F3C, hiWord(sess), loWord(sess), 0x2F3C, hiWord(name), loWord(name),
                0x42A7, 0x42A7, 0x42A7, 0x42A7, 0x42A7, 0x303C, 0x0E02, 0xA82F, 0x3E1F,
                0x4267, 0x3F3C, 0x0004, 0x303C, 0x0100, 0xA82F, 0x3C1F,
                0x4267, 0x2F3C, hiWord(sess), loWord(sess), 0x2F3C, hiWord(name), loWord(name),
                0x42A7, 0x42A7, 0x42A7, 0x42A7, 0x42A7, 0x303C, 0x0E02, 0xA82F, 0x3A1F,
                0xA9F4,
            ]
            #expect(run68k(words) == "ExitToShell")
            #expect(Int16(bitPattern: UInt16(g_cpu!.pointee.d.7 & 0xFFFF)) == -813) // rcDBPackNotInited
            #expect(g_cpu!.pointee.d.6 & 0xFFFF == 0)
            #expect(Int16(bitPattern: UInt16(g_cpu!.pointee.d.5 & 0xFFFF)) == -808) // rcDBBadDDEV
            #expect(ppcErr("DBEnd", [1, 0]) == -806 && ppcErr("DBExec", [1, 0]) == -806)
            #expect(ppcErr("DBGetConnInfo", [0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) == -807)
            #expect(ppcErr("DBKill", [tb_NewPtr(16, false, true)]) == -810)
            // An asynchronous call's block gets the result as well.
            let apb = tb_NewPtr(16, false, true)
            #expect(ppcErr("DBState", [5, apb]) == -806 && Int16(bitPattern: rd16(apb + 4)) == -806)
            #expect(ppcErr("DBStartQuery", [sess, 0, 0, 0]) == -808)
            let q = tb_NewPtr(4, false, true)
            #expect(ppcErr("DBGetNewQuery", [128, q]) == -192 && rd32(q) == 0) // no 'qrsc' 128
            let qh = tb_NewHandle(1118, false, true), list = tb_NewHandle(4, false, true)
            wr32(rd32(list), tb_NewHandle(10, false, false))
            wr16(rd32(qh) + 1098, 1)
            wr32(rd32(qh) + 1100, list)
            #expect(ppcErr("DBDisposeQuery", [qh]) == 0)
        }

        @Test func resultsBecomeTextThroughTheHandlers() {
            #expect(ppcErr("InitDBPack", [4]) == 0)
            // Two rows of a text column and a short column.
            let types = tb_NewHandle(8, false, false), info = tb_NewHandle(24, false, false)
            wr32(rd32(types), fourCC("TEXT")); wr32(rd32(types) + 4, fourCC("shor"))
            let items: [(Int, [UInt8])] = [(2, Array("ab".utf8)), (2, be16(7)), (3, Array("xyz".utf8) + [0]), (2, be16(-1 & 0xFFFF))]
            let data = tb_NewHandle(Int32(items.reduce(0) { $0 + $1.1.count }), false, false)
            var at: UInt32 = 0
            for (i, it) in items.enumerated() {
                wr16(rd32(info) + UInt32(i * 6), UInt16(it.0)); wr16(rd32(info) + UInt32(i * 6 + 2), 0); wr16(rd32(info) + UInt32(i * 6 + 4), 0)
                for b in it.1 { wr8(rd32(data) + at, b); at += 1 }
            }
            let rr = tb_NewPtr(16, false, true)
            wr16(rr, 2); wr16(rr + 2, 2); wr32(rr + 4, types); wr32(rr + 8, data); wr32(rr + 12, info)
            let out = tb_NewPtr(4, false, true)
            #expect(ppcErr("DBResultsToText", [rr, out]) == 0)
            let text = rd32(out)
            let n = tb_GetHandleSize(text)
            #expect(String(decoding: (0..<UInt32(n)).map { rd8(rd32(text) + $0) }, as: UTF8.self) == "ab\t7\rxyz\t-1\r")
            wr32(rd32(types) + 4, fourCC("blob"))
            #expect(ppcErr("DBResultsToText", [rr, out]) == -811) // rcDBNoHandler

            // A program's handler, found and removed again.
            let h = tb_NewPtr(2, false, true), got = tb_NewPtr(4, false, true)
            #expect(ppcErr("DBInstallResultHandler", [fourCC("blob"), h, 0]) == 0)
            #expect(ppcErr("DBGetResultHandler", [fourCC("blob"), got, 0]) == 0 && rd32(got) == h)
            #expect(ppcErr("DBGetResultHandler", [fourCC("blob"), got, 1]) == -811)
            #expect(ppcErr("DBRemoveResultHandler", [fourCC("blob")]) == 0)
            #expect(ppcErr("DBRemoveResultHandler", [fourCC("blob")]) == -811)
        }
    }

    /// AppleTalk's older InterfaceLib calls: .XPP's ASP and AFP calls with
    /// a server (AFPTests' stub on the LToUDP loopback), LAP protocol
    /// handlers, the transition queue, and Ethernet with no driver open.
    @Suite struct AppleTalkLegacy {
        func block(_ bytes: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(bytes.count, 1)), false, true)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }
        func pas(_ s: String) -> [UInt8] { [UInt8(s.utf8.count)] + Array(s.utf8) }

        @Test func aspAndAfpWithAServer() {
            #expect(machine_set_appletalk("local"))
            _ = launchTestApp()
            let afp = AFPStub()
            let asp = ASPStub(afp)
            defer { asp.thread.finish() }
            // XPPPrmBlk: cmdResult 18, sessRefnum 28, cbSize 32, cbPtr 34,
            // rbSize 38, rbPtr 40, wdSize 44, wdPtr 46; serverAddr 32.
            let pb = tb_NewPtr(400, false, true), reply = tb_NewPtr(8000, false, true)
            let server = UInt32(asp.node) << 8 | UInt32(asp.sls)
            wr32(pb + 32, server); wr16(pb + 38, 100); wr32(pb + 40, reply)
            #expect(ppcErr("ASPGetStatus", [pb, 0]) == 0)
            #expect(String(decoding: (0..<UInt32(rd16(pb + 38))).map { rd8(reply + $0) }, as: UTF8.self) == "stub status")

            // A login opens the session at afpAddrBlock (44).
            let login = block([18] + pas("AFPVersion 2.1") + pas("No User Authent"))
            gmemset(pb, 0, 400)
            wr16(pb + 32, UInt16(1 + 15 + 16)); wr32(pb + 34, login); wr16(pb + 38, 0); wr32(pb + 40, reply)
            wr32(pb + 44, server)
            #expect(ppcErr("AFPCommand", [pb, 0]) == 0 && rd32(pb + 18) == 0)
            let sess = rd16(pb + 28)
            #expect(sess != 0)

            // An ASP command: FPGetSrvrParms.
            let cmd = block([16, 0])
            wr16(pb + 32, 2); wr32(pb + 34, cmd); wr16(pb + 38, 100)
            #expect(ppcErr("ASPUserCommand", [pb, 0]) == 0 && rd32(pb + 18) == 0 && rd16(pb + 38) == 14)
            #expect(rd8(reply + 4) == 1 && rd8(reply + 6) == 7)

            // The volume, a fork, and a read across two transactions.
            let open = block([24, 0, 0, 0x20] + pas("StubVol"))
            wr16(pb + 32, 12); wr32(pb + 34, open); wr16(pb + 38, 100)
            #expect(ppcErr("AFPCommand", [pb, 0]) == 0 && rd32(pb + 18) == 0 && rd16(reply + 2) == 1)
            let fork = block([26, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 2] + pas("ReadMe"))
            wr16(pb + 32, 20); wr32(pb + 34, fork); wr16(pb + 38, 100)
            #expect(ppcErr("AFPCommand", [pb, 0]) == 0 && rd32(pb + 18) == 0)
            let ref = rd16(reply + 2)
            let rw = block([27, 0] + be16(Int(ref)) + be32(0) + be32(6000) + [0, 0])
            wr32(pb + 34, rw); wr16(pb + 38, 6000)
            let reads = afp.reads
            #expect(ppcErr("AFPCommand", [pb, 0]) == 0 && rd32(pb + 18) == 0 && rd16(pb + 38) == 6000)
            #expect((0..<6000).map { rd8(reply + UInt32($0)) } == Array(AFPStub.readMe[0..<6000]))
            #expect(afp.reads - reads == 2)
            // A write of the program's data.
            let data = block([UInt8](repeating: 0x5A, count: 100))
            wr8(rw, 33); wr32(rw + 8, 100)
            wr16(pb + 44, 100); wr32(pb + 46, data); wr16(pb + 38, 4)
            #expect(ppcErr("AFPCommand", [pb, 0]) == 0 && rd32(pb + 18) == 0 && afp.writes == 1)

            wr16(pb + 28, 0)
            #expect(ppcErr("ASPGetParms", [pb, 0]) == 0 && rd16(pb + 28) == 578 && rd16(pb + 30) == 4624)
            wr16(pb + 28, sess)
            let logout = block([20, 0])
            wr16(pb + 32, 2); wr32(pb + 34, logout); wr16(pb + 38, 0)
            #expect(ppcErr("AFPCommand", [pb, 0]) == 0)
            #expect(asp.closed)
            #expect(ppcErr("ASPCloseSession", [pb, 0]) == -1070) // aspParamErr: closed with the logout
            #expect(ppcErr("ASPAbortOS", [pb, 0]) == -1102)
            // No server at another address.
            wr32(pb + 32, 0x0000_7790); wr8(pb + 30, 1); wr8(pb + 31, 0) // one try, a second
            #expect(ppcErr("ASPOpenSession", [pb, 0]) == -1069)
            // ZIP: no router, so no zones.
            #expect(ppcErr("GetZoneList", [pb, 0]) == -93 && ppcExport("GetBridgeAddress", []) == 0)
        }

        @Test func lapProtocolHandlersAndTheTransitionQueue() {
            #expect(machine_set_appletalk("local"))
            _ = launchTestApp()
            #expect(ppcErr("MPPOpen", []) == 0)
            let flag = tb_NewPtr(4, false, true)
            // MOVE.W D1,flag; RTS: the bytes after the LAP header.
            let handler = block([0x33, 0xC1] + be32(flag) + [0x4E, 0x75])
            let pb = tb_NewPtr(64, false, true)
            wr8(pb + 28, 0x50); wr32(pb + 30, handler)
            #expect(ppcErr("PAttachPH", [pb, 0]) == 0)
            #expect(ppcErr("PAttachPH", [pb, 0]) == -94) // lapProtErr: taken
            let stub = ASPStub(AFPStub())
            defer { stub.thread.finish() }
            let me = tb_NewPtr(4, false, true)
            #expect(ppcErr("GetNodeAddress", [me, me + 2]) == 0)
            stub.sendRaw([UInt8(rd16(me)), stub.node, 0x50, 1, 2, 3])
            for _ in 0..<200 where rd16(flag) == 0 { usleep(5000); appletalk_poll() }
            #expect(rd16(flag) == 3)
            // A frame of our own: BuildLAPwds, then PWriteLAP.
            let wds = tb_NewPtr(16, false, true), frame = tb_NewPtr(16, false, true)
            ppcExport("BuildLAPwds", [wds, frame, UInt32(stub.node), 0x50, 8])
            #expect(rd8(frame) == stub.node && rd8(frame + 2) == 0x50 && rd16(wds) == 8 && rd32(wds + 2) == frame)
            wr32(pb + 30, wds)
            #expect(ppcErr("PWriteLAP", [pb, 0]) == 0)
            #expect(ppcErr("PDetachPH", [pb, 0]) == 0 && ppcErr("PDetachPH", [pb, 0]) == -94)
            wr8(pb + 28, 2)
            #expect(ppcErr("PAttachPH", [pb, 0]) == -94) // DDP's own type

            // Transition queue entries: each records the event; the second refuses.
            let f1 = tb_NewPtr(4, false, true), f2 = tb_NewPtr(4, false, true)
            func entry(_ f: UInt32, _ answer: UInt8) -> UInt32 {
                let code = block([0x20, 0x2F, 0x00, 0x04, 0x23, 0xC0] + be32(f) + [0x70, answer, 0x4E, 0x75])
                let q = tb_NewPtr(10, false, true)
                wr32(q + 6, code)
                return q
            }
            let e1 = entry(f1, 0), e2 = entry(f2, 7)
            #expect(ppcErr("LAPAddATQ", [e1]) == 0 && ppcErr("LAPAddATQ", [e2]) == 0)
            #expect(ppcErr("ATPreFlightEvent", [3, 4, 0]) == 7) // ClosePrep refused
            #expect(rd32(f1) == 4 && rd32(f2) == 3)                 // the first told to cancel
            ppcExport("ATEvent", [2, 0])
            #expect(rd32(f1) == 2 && rd32(f2) == 2)
            #expect(ppcErr("LAPRmvATQ", [e2]) == 0 && ppcErr("LAPRmvATQ", [e2]) == -1)
            #expect(ppcErr("ATPUnload", []) == 0)

            // Ethernet: .ENET isn't open.
            let epb = tb_NewPtr(80, false, true)
            for name in ["EAttachPH", "EWrite", "ERead", "EGetInfo", "EAddMulti", "EDelMulti", "ERdCancel",
                         "ESetGeneral", "EDetachPH"] {
                #expect(ppcErr(name, [epb, 0]) == -28)
            }
        }
    }

    /// The PPC Toolbox: ports and sessions within a run (between runs the
    /// same sockets carry them).
    @Suite struct PPCToolbox {
        init() { bootMachine() }

        /// A PPCPortRec: smRoman, the name, ppcByString and the type.
        func portName(_ name: String, _ type: String = "Test") -> UInt32 {
            let p = tb_NewPtr(72, false, true)
            guestPString(name, at: p + 2)
            guestPString(type, at: p + 38)
            return p
        }

        func open(_ name: String) -> UInt16 {
            let pb = tb_NewPtr(64, false, true)
            wr8(pb + 44, 1) // ppcServiceRealTime
            wr32(pb + 46, portName(name))
            #expect(ppcErr("PPCOpenSync", [pb]) == 0)
            return rd16(pb + 38)
        }

        func pump() {
            for _ in 0..<200 { usleep(2000); ppc_toolbox_poll() }
        }

        @Test func sessionsBetweenTwoPorts() {
            let a = open("Alpha"), b = open("Beta")
            #expect(a != 0 && b != 0 && a != b)
            let dup = tb_NewPtr(64, false, true)
            wr8(dup + 44, 1); wr32(dup + 46, portName("Beta"))
            #expect(ppcErr("PPCOpenSync", [dup]) == -910) // portNameExistsErr

            // Beta waits for a session, accepting it as it comes.
            let inform = tb_NewPtr(64, false, true), who = tb_NewPtr(72, false, true)
            wr16(inform + 38, b); wr8(inform + 45, 1); wr32(inform + 46, who)
            #expect(ppcErr("PPCInformAsync", [inform]) == 0 && rd16(inform + 16) == 1)
            let start = tb_NewPtr(70, false, true)
            wr16(start + 38, a); wr32(start + 46, portName("Beta")); wr32(start + 58, 0x1234)
            #expect(ppcErr("PPCStartSync", [start]) == 0)
            let sa = rd32(start + 40)
            pump()
            #expect(rd16(inform + 16) == 0 && rd32(inform + 58) == 0x1234 && rd8(inform + 62) == 1)
            #expect(pstring(at: who + 2) == "Alpha")
            let sb = rd32(inform + 40)

            // A block from Alpha, read whole by Beta.
            let w = tb_NewPtr(70, false, true), r = tb_NewPtr(70, false, true), buf = tb_NewPtr(64, false, true)
            let text = tb_NewPtr(16, false, true)
            for (i, c) in Array("hello".utf8).enumerated() { wr8(text + UInt32(i), c) }
            wr32(w + 40, sa); wr32(w + 44, 5); wr32(w + 52, text); wr32(w + 58, 7)
            wr32(w + 62, fourCC("BBOX")); wr32(w + 66, fourCC("TEXT"))
            #expect(ppcErr("PPCWriteSync", [w]) == 0 && rd32(w + 48) == 5)
            wr32(r + 40, sb); wr32(r + 44, 64); wr32(r + 52, buf)
            #expect(ppcErr("PPCReadSync", [r]) == 0)
            #expect(rd32(r + 48) == 5 && rd8(r + 56) == 0 && rd32(r + 58) == 7 && rd32(r + 66) == fourCC("TEXT"))
            #expect(String(decoding: (0..<5).map { rd8(buf + UInt32($0)) }, as: UTF8.self) == "hello")

            // A block in two writes, read three bytes at a time.
            wr8(w + 56, 1)
            #expect(ppcErr("PPCWriteSync", [w]) == 0)
            wr8(w + 56, 0); wr32(w + 44, 2)
            #expect(ppcErr("PPCWriteSync", [w]) == 0)
            var got = [UInt8](), mores = [UInt8]()
            for _ in 0..<3 {
                wr32(r + 44, 3)
                #expect(ppcErr("PPCReadSync", [r]) == 0)
                got += (0..<rd32(r + 48)).map { rd8(buf + $0) }
                mores.append(rd8(r + 56))
            }
            #expect(String(decoding: got, as: UTF8.self) == "hellohe" && mores == [1, 1, 0])

            // An asynchronous read finishes when Alpha ends the session.
            #expect(ppcErr("PPCReadAsync", [r]) == 0 && rd16(r + 16) == 1)
            let end = tb_NewPtr(64, false, true)
            wr32(end + 40, sa)
            #expect(ppcErr("PPCEndSync", [end]) == 0)
            pump()
            #expect(Int16(bitPattern: rd16(r + 16)) == -917) // sessClosedErr
            #expect(ppcErr("PPCEndSync", [end]) == -908)      // noSessionErr

            // Ports listed, then a request rejected.
            let list = tb_NewPtr(64, false, true), infos = tb_NewPtr(74 * 4, false, true)
            wr16(list + 42, 4); wr32(list + 46, portName("=")); wr32(list + 54, infos)
            #expect(ppcErr("IPCListPortsSync", [list]) == 0)
            let names = (0..<Int(rd16(list + 44))).map { pstring(at: infos + UInt32($0 * 74) + 4) }
            #expect(names.contains("Alpha") && names.contains("Beta"))
            wr8(inform + 45, 0)
            #expect(ppcErr("PPCInformAsync", [inform]) == 0)
            #expect(ppcErr("PPCStartAsync", [start]) == 0 && rd16(start + 16) == 1)
            pump()
            #expect(rd16(inform + 16) == 0)
            let rej = tb_NewPtr(64, false, true)
            wr32(rej + 40, rd32(inform + 40)); wr32(rej + 54, 42)
            #expect(ppcErr("PPCRejectSync", [rej]) == 0)
            pump()
            #expect(Int16(bitPattern: rd16(start + 16)) == -912 && rd32(start + 54) == 42) // userRejectErr
            wr32(start + 46, portName("Nobody"))
            #expect(ppcErr("PPCStartSync", [start]) == -906) // destPortErr

            // Closing a port ends the call waiting on it.
            #expect(ppcErr("PPCInformAsync", [inform]) == 0)
            let close = tb_NewPtr(64, false, true)
            wr16(close + 38, b)
            #expect(ppcErr("PPCCloseSync", [close]) == 0)
            pump()
            #expect(Int16(bitPattern: rd16(inform + 16)) == -916) // portClosedErr
            #expect(ppcErr("PPCCloseSync", [close]) == -903)
            #expect(ppcErr("DeleteUserIdentity", [1]) == 0 && ppcErr("DeleteUserIdentity", [99]) == -924)
            wr16(close + 38, a)
            #expect(ppcErr("PPCCloseSync", [close]) == 0)
        }

        @Test func the68kTrapTakesItsAsyncBit() {
            let pb = tb_NewPtr(64, false, true)
            wr8(pb + 44, 1)
            wr32(pb + 46, portName("Gamma"))
            let reason = run68k([0x207C, hiWord(pb), loWord(pb), 0x7001, 0xA0DD, 0x3E00, 0xA9F4])
            #expect(reason == "ExitToShell" && g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd16(pb + 38) != 0)
            let inform = tb_NewPtr(64, false, true)
            wr16(inform + 38, rd16(pb + 38))
            // _PPC with the async bit: PPCInformAsync waits on.
            #expect(run68k([0x207C, hiWord(inform), loWord(inform), 0x7003, 0xA4DD, 0x3E00, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd16(inform + 16) == 1)
            wr16(pb + 38, rd16(pb + 38))
            #expect(run68k([0x207C, hiWord(pb), loWord(pb), 0x7009, 0xA0DD, 0xA9F4]) == "ExitToShell")
            ppc_toolbox_poll()
            #expect(Int16(bitPattern: rd16(inform + 16)) == -916)
        }
    }

    /// The Device Manager's installation and immediate calls, the drive
    /// queue, slot retraces, deferred functions, the Disk Initialization
    /// Package's extended calls and the Extensions Manager.
    @Suite struct DevicesAndDrives {
        init() { bootMachine() }

        @Test func driversInstalledAndRemoved() {
            let drvr = tb_NewPtr(32, false, true)
            #expect(ppcErr("DriverInstall", [drvr, UInt32(bitPattern: -60)]) == 0)
            let h = rd32(rd32(UInt32(LM_UTableBase)) + 4 * 59)
            #expect(h != 0 && rd32(rd32(h)) == drvr)
            // _DrvrRemove: gone, then not there to remove.
            #expect(run68k([0x303C, 0xFFC4, 0xA03E, 0x3E00, 0x303C, 0xFFC4, 0xA03E, 0x3C00, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && Int16(bitPattern: UInt16(g_cpu!.pointee.d.6 & 0xFFFF)) == -1)
            #expect(rd32(rd32(UInt32(LM_UTableBase)) + 4 * 59) == 0)
            #expect(ppcErr("DriverInstallReserveMem", [drvr, 5]) == -21) // badUnitErr
            #expect(ppcErr("DrvrRemove", [UInt32(bitPattern: -1000)]) == -21)
        }

        @Test func drivesQueuedAndAsked() {
            let el = tb_NewPtr(16, false, true)
            ppcExport("AddDrive", [UInt32(bitPattern: -5), 9, el])
            #expect(rd16(el + 6) == 9 && Int16(bitPattern: rd16(el + 8)) == -5)
            var found = false
            var e = rd32(UInt32(LM_DrvQHdr) + 2)
            while e != 0 { if e == el { found = true }; e = rd32(e) }
            #expect(found)
            let st = tb_NewPtr(22, false, true)
            #expect(ppcErr("DriveStatus", [9, st]) != 0)    // its driver (-5) has no status call
            #expect(ppcErr("DriveStatus", [77, st]) == -56) // nsDrvErr
            #expect(ppcErr("DIXFormat", [77, 0, 0, st]) == -64)
            #expect(ppcErr("DIXFormat", [9, 0, 0, st]) != 0)
            let name = tb_NewPtr(8, false, true)
            wr8(name, 0x41)
            #expect(ppcErr("dizero", [77, name]) == -64)
        }

        @Test func waitsRetracesAndDeferredFunctions() {
            // An I/O already done; one that never ends times out.
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioResult_off), UInt16(bitPattern: -43))
            #expect(ppcErr("PBWaitIOComplete", [pb, 10]) == -43)
            wr16(pb + UInt32(IOParam_ioResult_off), 1)
            #expect(ppcErr("PBWaitIOComplete", [pb, 5]) == -29296)
            #expect(ppcErr("AttachVBL", [0]) == 0 && ppcErr("AttachVBL", [9]) == -360)
            // A VBL task with a count of one runs on DoVBLTask.
            let flag = tb_NewPtr(4, false, true)
            let code = tb_NewPtr(8, false, false)
            for (i, w) in [0x52B9, Int(hiWord(flag)), Int(loWord(flag)), 0x4E75].enumerated() { wr16(code + UInt32(i * 2), UInt16(w)) }
            let task = tb_NewPtr(Int32(VBLTask_SIZEOF), false, true)
            wr16(task + 4, 1); wr32(task + UInt32(VBLTask_vblAddr_off), code); wr16(task + UInt32(VBLTask_vblCount_off), 1)
            #expect(tb_VInstall(task) == 0)
            #expect(run68k([0x7000, 0xA072, 0x3E00, 0xA9F4]) == "ExitToShell" && g_cpu!.pointee.d.7 & 0xFFFF == 0)
            #expect(rd32(flag) == 1)
            // DeferUserFn: the function at once, A0 its argument.
            let arg = tb_NewPtr(4, false, true), stack = tb_NewPtr(4096, false, true)
            g_cpu!.pointee.a.7 = stack + 4096 - 16
            g_cpu!.pointee.halted = false // run68k's ExitToShell
            let fn = tb_NewPtr(8, false, false)
            for (i, w) in [0x5290, 0x4E75].enumerated() { wr16(fn + UInt32(i * 2), UInt16(w)) } // ADDQ.L #1,(A0)
            #expect(ppcErr("DeferUserFn", [fn, arg]) == 0 && rd32(arg) == 1)
        }

        @Test func noExtensionsLoad() {
            let proc = tb_NewPtr(2, false, true), table = tb_NewPtr(4, false, true)
            #expect(ppcErr("InstallExtensionNotificationProc", [proc]) == 0)
            #expect(ppcErr("RemoveExtensionNotificationProc", [proc]) == 0)
            #expect(ppcErr("RemoveExtensionNotificationProc", [proc]) == -50)
            #expect(ppcErr("InstallExtensionTableHandlerProc", [proc, table]) == 0)
            let t = rd32(rd32(table))
            #expect(rd32(t) == 20 && rd32(t + 4) == 0x100 && rd32(t + 12) == 94 && rd32(t + 16) == 0)
            #expect(ppcErr("RemoveExtensionTableHandlerProc", [proc]) == 0)
            #expect(ppcErr("FTSendAsync", [0, 1, 0, 0]) == 2) // ftFailed: no transfer record
            // ADBOp: Talk register 3 of the mouse (address 3): its address and handler.
            let buf = tb_NewPtr(9, false, true)
            #expect(ppcErr("ADBOp", [0, 0, buf, 0x3F]) == 0 && rd8(buf) == 2)
        }
    }
}
