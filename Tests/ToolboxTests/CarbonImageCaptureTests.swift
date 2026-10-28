import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Image Capture's object tree: empty without devices, built by a device module's ICD calls.
    @Suite struct CarbonImageCapture {
        /// A parameter block of `n` bytes, header zeroed.
        func pb(_ n: Int = 48) -> UInt32 { svcPtr(n) }
        func err(_ p: UInt32) -> Int16 { Int16(bitPattern: rd16(p)) }

        /// A PowerPC routine void (ICAHeader *pb): stores 0x7777 into the header's refcon high half.
        func completionRoutine() -> UInt32 {
            let code = svcPtr(16), tv = svcPtr(8)
            wr32(code, 0x3880_7777)     // li r4, 0x7777
            wr32(code + 4, 0xB083_0002) // sth r4, 2(r3)
            wr32(code + 8, 0x4E80_0020) // blr
            wr32(tv, code)
            wr32(tv + 4, 0)
            return tv
        }

        /// A routine that stores 0x55 at an address.
        func markerRoutine(_ at: UInt32) -> UInt32 {
            let code = svcPtr(32), tv = svcPtr(8)
            wr32(code, 0x3C80_0000 | (at >> 16))        // lis r4, hi
            wr32(code + 4, 0x6084_0000 | (at & 0xFFFF)) // ori r4, r4, lo
            wr32(code + 8, 0x38A0_0055)                 // li r5, 0x55
            wr32(code + 12, 0x90A4_0000)                // stw r5, 0(r4)
            wr32(code + 16, 0x4E80_0020)                // blr
            wr32(tv, code)
            wr32(tv + 4, 0)
            return tv
        }

        @Test func emptyWithoutDevices() {
            bootMachine()
            let a = pb(), c = pb()
            #expect(svcCall("ICAGetDeviceList", a, 0) == 0)
            let list = rd32(a + 6)
            #expect(list != 0)
            wr32(c + 6, list)
            #expect(svcCall("ICAGetChildCount", c, 0) == 0 && rd32(c + 10) == 0)
            let n = pb()
            wr32(n + 6, list)
            wr32(n + 10, 0)
            #expect(svcStatus(svcCall("ICAGetNthChild", n, 0)) == -9907 && err(n) == -9907)
            let info = pb()
            wr32(info + 6, list)
            #expect(svcCall("ICAGetObjectInfo", info, 0) == 0 && rd32(info + 10) == fourCC("objl"))
            // The list is a root, and has no parent; a made-up object is invalid
            let parent = pb()
            wr32(parent + 6, list)
            #expect(svcStatus(svcCall("ICAGetParentOfObject", parent, 0)) == -9905)
            wr32(info + 6, 0x1234)
            #expect(svcStatus(svcCall("ICAGetObjectInfo", info, 0)) == -9905)
            // No device module serves a message
            let m = pb()
            wr32(m + 6, list)
            #expect(svcStatus(svcCall("ICAObjectSendMessage", m, 0)) == -9900)
        }

        @Test func aDeviceModuleBuildsATree() {
            bootMachine()
            let a = pb()
            #expect(svcCall("ICAGetDeviceList", a, 0) == 0)
            let list = rd32(a + 6)
            // A camera and a picture in it
            let n = pb()
            wr32(n + 6, list)
            wr32(n + 10, fourCC("icdv"))
            wr32(n + 14, fourCC("cmra"))
            #expect(svcCall("ICDNewObject", n, 0) == 0)
            let camera = rd32(n + 18)
            wr32(n + 6, camera)
            wr32(n + 10, fourCC("file"))
            wr32(n + 14, fourCC("imag"))
            #expect(svcCall("ICDNewObject", n, 0) == 0)
            let picture = rd32(n + 18)
            #expect(camera != 0 && picture != camera)
            let c = pb()
            wr32(c + 6, list)
            #expect(svcCall("ICAGetChildCount", c, 0) == 0 && rd32(c + 10) == 1)
            wr32(c + 6, camera)
            #expect(svcCall("ICAGetChildCount", c, 0) == 0 && rd32(c + 10) == 1)
            let nth = pb()
            wr32(nth + 6, list)
            wr32(nth + 10, 0)
            #expect(svcCall("ICAGetNthChild", nth, 0) == 0 && rd32(nth + 14) == camera)
            #expect(rd32(nth + 18) == fourCC("icdv") && rd32(nth + 22) == fourCC("cmra"))
            wr32(nth + 10, 1)
            #expect(svcStatus(svcCall("ICAGetNthChild", nth, 0)) == -9907)
            // Parent and root of the picture
            let p = pb()
            wr32(p + 6, picture)
            #expect(svcCall("ICAGetParentOfObject", p, 0) == 0 && rd32(p + 10) == camera)
            #expect(svcCall("ICAGetRootOfObject", p, 0) == 0 && rd32(p + 10) == list && rd32(p + 14) == fourCC("objl"))
            // A property of the picture: width, writable, four bytes
            let np = pb()
            wr32(np + 6, picture)
            wr32(np + 10, fourCC("iwid"))
            wr32(np + 14, fourCC("ui32"))
            wr32(np + 18, 4)
            wr32(np + 22, 1)
            #expect(svcCall("ICDNewProperty", np, 0) == 0)
            let width = rd32(np + 26)
            let count = pb()
            wr32(count + 6, picture)
            #expect(svcCall("ICAGetPropertyCount", count, 0) == 0 && rd32(count + 10) == 1)
            let data = svcPtr(8)
            wr32(data, 640)
            let set = pb()
            wr32(set + 6, width)
            wr32(set + 10, 0)
            wr32(set + 14, data)
            wr32(set + 18, 4)
            #expect(svcCall("ICASetPropertyData", set, 0) == 0)
            let out = svcPtr(8), get = pb()
            wr32(get + 6, width)
            wr32(get + 10, 0)
            wr32(get + 14, UInt32.max)
            wr32(get + 18, out)
            #expect(svcCall("ICAGetPropertyData", get, 0) == 0 && rd32(get + 22) == 4 && rd32(out) == 640 && rd32(get + 26) == fourCC("ui32"))
            // Past the end
            wr32(get + 10, 9)
            #expect(svcStatus(svcCall("ICAGetPropertyData", get, 0)) == -9907)
            // By index and by type
            let byType = pb()
            wr32(byType + 6, picture)
            wr32(byType + 10, fourCC("iwid"))
            #expect(svcCall("ICAGetPropertyByType", byType, 0) == 0 && rd32(byType + 14) == width && rd32(byType + 26) == 4)
            wr32(byType + 10, fourCC("ihei"))
            #expect(svcStatus(svcCall("ICAGetPropertyByType", byType, 0)) == -9908)
            let nthp = pb()
            wr32(nthp + 6, picture)
            #expect(svcCall("ICAGetNthProperty", nthp, 0) == 0 && rd32(nthp + 14) == width)
            let pi = pb()
            wr32(pi + 6, width)
            #expect(svcCall("ICAGetPropertyInfo", pi, 0) == 0 && rd32(pi + 10) == fourCC("iwid") && rd32(pi + 18) == 4)
            #expect(svcCall("ICAGetParentOfProperty", pi, 0) == 0 && rd32(pi + 10) == picture)
            // Refcons
            let rc = pb()
            wr32(rc + 6, picture)
            wr32(rc + 10, 0xBEEF)
            #expect(svcCall("ICASetObjectRefCon", rc, 0) == 0)
            wr32(rc + 10, 0)
            #expect(svcCall("ICAGetObjectRefCon", rc, 0) == 0 && rd32(rc + 10) == 0xBEEF)
            wr32(rc + 6, width)
            wr32(rc + 10, 7)
            #expect(svcCall("ICASetPropertyRefCon", rc, 0) == 0)
            wr32(rc + 10, 0)
            #expect(svcCall("ICAGetPropertyRefCon", rc, 0) == 0 && rd32(rc + 10) == 7)
            // Read-only properties refuse writes
            wr32(np + 10, fourCC("isiz"))
            wr32(np + 22, 2)
            #expect(svcCall("ICDNewProperty", np, 0) == 0)
            wr32(set + 6, rd32(np + 26))
            #expect(svcStatus(svcCall("ICASetPropertyData", set, 0)) == -9906)
            // A completion routine is called after the call
            let done = pb()
            wr32(done + 6, picture)
            #expect(svcCall("ICAGetChildCount", done, completionRoutine()) == 0)
            #expect(rd16(done + 2) == 0x7777)
            // Disposing a device takes its picture and properties with it
            let d = pb()
            wr32(d + 6, camera)
            #expect(svcCall("ICDDisposeObject", d, 0) == 0)
            wr32(count + 6, picture)
            #expect(svcStatus(svcCall("ICAGetPropertyCount", count, 0)) == -9905)
            wr32(pi + 6, width)
            #expect(svcStatus(svcCall("ICAGetPropertyInfo", pi, 0)) == -9906)
            #expect(svcStatus(svcCall("ICDDisposeObject", d, 0)) == -9905)
            wr32(c + 6, list)
            #expect(svcCall("ICAGetChildCount", c, 0) == 0 && rd32(c + 10) == 0)
        }

        @Test func eventsReachWhoRegistered() {
            bootMachine()
            let a = pb()
            #expect(svcCall("ICAGetDeviceList", a, 0) == 0)
            let list = rd32(a + 6)
            let reg = pb()
            wr32(reg + 6, 0)
            wr32(reg + 10, fourCC("eoba"))
            let flag = svcPtr(8)
            wr32(reg + 14, markerRoutine(flag))
            #expect(svcCall("ICARegisterEventNotification", reg, 0) == 0)
            let n = pb()
            wr32(n + 6, list)
            wr32(n + 10, fourCC("icdv"))
            #expect(svcCall("ICDNewObject", n, 0) == 0)
            #expect(rd32(flag) == 0x55) // the routine ran
            #expect(rd32(flag + 4) == 0) // ...once, for the one object
            wr32(reg + 6, 0x1234)
            #expect(svcStatus(svcCall("ICARegisterEventNotification", reg, 0)) == -9905)
        }
    }
}
