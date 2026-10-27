import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Weak imports the archive walk's PowerPC programs called without
    /// checking: CarbonLib's memory checks, UTCUtils and the Data Browser's
    /// first callback names.
    @Suite struct WalkImports {
        init() { bootMachine() }

        @Test func memoryChecks() {
            let p = tb_NewPtr(40, false, true), h = tb_NewHandle(40, false, true)
            #expect(ilCall("IsPointerValid", [p]) & 0xFF == 1)
            #expect(ilCall("IsPointerValid", [p + 4]) & 0xFF == 0)
            #expect(ilCall("IsPointerValid", [0]) & 0xFF == 0)
            #expect(ilCall("IsPointerValid", [rd32(h)]) & 0xFF == 0) // a relocatable block
            #expect(ilCall("IsHandleValid", [h]) & 0xFF == 1)
            wr32(p, p) // pointing at a nonrelocatable block, as no master pointer does
            #expect(ilCall("IsHandleValid", [p]) & 0xFF == 0)
            wr32(p, rd32(h)) // at a handle's block, which doesn't point back to it
            #expect(ilCall("IsHandleValid", [p]) & 0xFF == 0)
            #expect(ilCall("IsHandleValid", [0]) & 0xFF == 0)
            tb_EmptyHandle(h)
            #expect(ilCall("IsHandleValid", [h]) & 0xFF == 1) // empty, still a handle
            #expect(ilCall("IsHeapValid", []) & 0xFF == 1)
        }

        @Test func utcAndLocalTime() {
            let secs = tb_NewPtr(4, false, true)
            tb_GetDateTime(secs)
            let local = rd32(secs)
            // The machine is at Greenwich unless told otherwise: UTC is local time.
            let utc = tb_NewPtr(8, false, true), back = tb_NewPtr(8, false, true)
            #expect(ilCall("GetUTCDateTime", [utc, 0]) == 0)
            #expect(rd16(utc) == 0 && rd32(utc + 2) &- local < 5)
            #expect(ilCall("ConvertUTCToLocalDateTime", [utc, back]) == 0 && rd32(back + 2) == rd32(utc + 2))
            // Five hours west: local time is behind.
            let loc = tb_NewPtr(12, false, true)
            wr32(loc + 8, UInt32(bitPattern: -5 * 3600) & 0x00FF_FFFF)
            _ = ilCall("WriteLocation", [loc])
            defer { wr32(loc + 8, 0); _ = ilCall("WriteLocation", [loc]) }
            let out = tb_NewPtr(4, false, true)
            #expect(ilCall("ConvertLocalTimeToUTC", [1_000_000, out]) == 0 && rd32(out) == 1_000_000 + 5 * 3600)
            #expect(ilCall("ConvertUTCToLocalTime", [1_000_000, out]) == 0 && rd32(out) == 1_000_000 - 5 * 3600)
            #expect(ilCall("ConvertUTCToLocalTime", [10, out]) == UInt32(bitPattern: -8850)) // kUTCUnderflowErr
            wr16(utc, 0)
            wr32(utc + 2, 2_000_000)
            wr16(utc + 6, 0x8000)
            #expect(ilCall("ConvertUTCToLocalDateTime", [utc, back]) == 0)
            #expect(rd32(back + 2) == 2_000_000 - 5 * 3600 && rd16(back + 6) == 0x8000)
            #expect(ilCall("ConvertLocalToUTCDateTime", [back, utc]) == 0 && rd32(utc + 2) == 2_000_000)
        }

        /// The Unicode Converter's mappings, all or those to one encoding (Eudora counts them).
        @Test func unicodeMappings() {
            let count = tb_NewPtr(4, false, true), find = tb_NewPtr(12, false, true), out = tb_NewPtr(12 * 200, false, true)
            #expect(ilCall("CountUnicodeMappings", [0, 0, count]) == 0)
            let all = rd32(count)
            #expect(all > 1)
            wr32(find, 0x0100)
            wr32(find + 4, 0) // MacRoman
            #expect(ilCall("CountUnicodeMappings", [1 << 3, find, count]) == 0 && rd32(count) == 1) // other base
            #expect(ilCall("QueryUnicodeMappings", [1 << 3, find, 200, count, out]) == 0 && rd32(count) == 1)
            #expect(rd32(out) == 0x0100 && rd32(out + 4) == 0 && rd32(out + 8) == 0xFFFF_FFFF)
            #expect(ilCall("QueryUnicodeMappings", [0, 0, 2, count, out]) == 0 && rd32(count) == 2)
        }

        @Test func moreFirstNames() {
            #expect(ilCall("OTUseSyncIdleEvents", [0x1234, 1]) == 0)
            #expect(ilCall("TXNTSMCheck", [0x1234, 0]) & 0xFF == 0)
            let gw = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true), buf = tb_NewPtr(16 * 16 * 4, false, true)
            wr16(r + 4, 16)
            wr16(r + 6, 16)
            #expect(ilCall("NewGWorldFromPtr", [gw, 32, r, 0, 0, 0, buf, 64]) == 0 && rd32(gw) != 0)
            let pm = rd32(rd32(rd32(gw) + UInt32(CGrafPort_portPixMap_off)))
            #expect(rd32(pm) == buf && rd16(pm + 4) & 0x3FFF == 64) // the caller's pixels
            #expect(ilCall("NewGWorldFromPtr", [gw, 3, r, 0, 0, 0, buf, 64]) == UInt32(bitPattern: -50))
        }

        @Test func dataBrowserFirstNames() {
            let upp = ilCall("NewDataBrowserGetSetItemDataUPP", [0x0040_1000])
            #expect(upp != 0 && rd16(upp) == 0xAAFE)
            let cmp = ilCall("NewDataBrowserCompareUPP", [0x0040_2000])
            #expect(cmp != 0 && rd16(cmp) == 0xAAFE)
            _ = ilCall("DisposeDataBrowserCompareUPP", [cmp])
            _ = ilCall("DisposeDataBrowserGetSetItemDataUPP", [upp])
        }
    }
}

/// What the test sequence grabber was called with: the selector, the
/// parameter size and the parameters' first word.
nonisolated(unsafe) var sgCalls: [(what: Int16, size: UInt8, first: UInt16)] = []

extension BlueBox {
    /// QuickTimeLib's sequence grabber and video digitizer calls go to the
    /// instance through the Component Manager with their selectors.
    @Suite(.serialized) struct QuickTimeCapture {
        @Test func callsReachTheInstance() {
            bootMachine()
            cfm_reset()
            _ = cfm_ppc_cpu()
            #expect(ilCall("SGInitialize", [0]) == 0x8000_8001) // badComponentInstance
            let entry = native_register({ c, _ in
                guard let c else { return }
                let ret = cpu_pop32(c)
                _ = cpu_pop32(c) // storage
                let params = cpu_pop32(c)
                sgCalls.append((Int16(bitPattern: rd16(params + 2)), rd8(params + 1), rd16(params + 4)))
                wr32(c.pointee.a.7, 0)
                cpu_push32(c, ret)
            }, "TestSequenceGrabber", 0)
            let comp = components_register_native(fourCC("barg"), 0, fourCC("test"), entry)
            #expect(comp != 0)
            let sg = components_open(comp)
            #expect(sg != 0)
            sgCalls = []
            #expect(ilCall("SGInitialize", [sg]) == 0)
            #expect(ilCall("SGPause", [sg, 1]) == 0)
            #expect(ilCall("VDSetPlayThruOnOff", [sg, 3]) == 0)
            let calls = sgCalls
            #expect(calls.map(\.what) == [0x1, 0x14, 0x2E])
            #expect(calls.map(\.size) == [0, 2, 2])
            #expect(calls.count == 3 && calls[1].first == 0x0100 && calls[2].first == 3) // a byte goes high in its word
        }
    }
}
