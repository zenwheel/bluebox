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
