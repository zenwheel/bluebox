import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// AppleScriptLib's storage type of script data.
    @Suite struct OS9RestScript {
        init() { Rest.boot() }

        @Test func storageTypes() {
            let h = tb_NewHandle(10, false, true)
            for i in 0..<10 { wr8(rd32(h) + UInt32(i), UInt8(i)) }
            let ty = Rest.ptr(4)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("scpt")) // none: the generic one
            #expect(Rest.signed("OSAAddStorageType", [h, fourCC("jsrc")]) == 0)
            #expect(tb_GetHandleSize(h) == 22)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("jsrc"))
            #expect(rd32(rd32(h) + 18) == 0xFADE_DEAD && rd16(rd32(h) + 14) == 1) // the mark and the version
            #expect((0..<10).map { rd8(rd32(h) + UInt32($0)) } == Array(0..<10)) // the data kept
            #expect(Rest.signed("OSAAddStorageType", [h, fourCC("ascr")]) == 0 && tb_GetHandleSize(h) == 22)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("ascr"))
            #expect(Rest.signed("OSARemoveStorageType", [h]) == 0 && tb_GetHandleSize(h) == 10)
            #expect(Rest.signed("OSAGetStorageType", [h, ty]) == 0 && rd32(ty) == fourCC("scpt"))
            #expect(Rest.signed("OSARemoveStorageType", [h]) == 0 && tb_GetHandleSize(h) == 10) // none: nothing
            // odd-sized data is padded before the trailer
            let odd = tb_NewHandle(5, false, true)
            #expect(Rest.signed("OSAAddStorageType", [odd, fourCC("jsrc")]) == 0 && tb_GetHandleSize(odd) == 18)
            #expect(Rest.signed("OSAGetStorageType", [odd, ty]) == 0 && rd32(ty) == fourCC("jsrc"))
            #expect(Rest.signed("OSAGetStorageType", [0, ty]) == -1752)
        }
    }
}
