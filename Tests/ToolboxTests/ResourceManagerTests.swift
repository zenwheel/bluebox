import CCore
import Testing
import Toolbox

extension Classic {
    @Suite struct ResourceManager {
        let refnum: Int16

        init() {
            bootMachine()
            var fork = buildFork([
                TestResource(type: "TEST", id: 128, name: "hello", data: Array("first".utf8)),
                TestResource(type: "TEST", id: 129, attrs: 0x01, // compressed
                             data: dcmp0Resource(decompressedSize: 4, body: cat([0x02], Array("WXYZ".utf8), [0xFF]))),
                TestResource(type: "STR ", id: 1, data: cat([3], Array("abc".utf8))),
                TestResource(type: "PURG", id: 7, attrs: 0x20, data: [1, 2, 3]),
            ])
            refnum = rm_open_host_file(nil, &fork, UInt32(fork.count), true)
        }

        func bytes(_ h: UInt32) -> [UInt8] {
            let n = Int(tb_GetHandleSize(h))
            return (0..<n).map { rd8(rd32(h) + UInt32($0)) }
        }

        @Test func openedFileBecomesCurrentAndTopOfChain() {
            #expect(refnum > 0)
            #expect(tb_CurResFile() == refnum)
            #expect(rd32(UInt32(LM_TopMapHndl)) != rd32(UInt32(LM_SysMapHndl)))
        }

        @Test func getResourceLoadsAndCaches() {
            let h = tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128)
            #expect(h != 0)
            #expect(resErr() == 0)
            #expect(bytes(h) == Array("first".utf8))
            #expect(tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128) == h)
            #expect(tb_HomeResFile(h) == refnum)
            #expect(UInt8(bitPattern: tb_HGetState(h)) & 0x20 != 0) // resource bit
        }

        @Test func compressedResourcesAreExpanded() {
            let h = tb_GetResource(Int32(bitPattern: fourCC("TEST")), 129)
            #expect(bytes(h) == Array("WXYZ".utf8))
            #expect(tb_GetMaxResourceSize(h) == 4)
        }

        @Test func missingResourceSetsResError() {
            #expect(tb_GetResource(Int32(bitPattern: fourCC("TEST")), 999) == 0)
            #expect(resErr() == -192)
        }

        @Test func namedAndIndexedAccess() {
            let name = tb_NewPtr(256, false, true)
            guestPString("HELLO", at: name) // names compare case-insensitively
            let h = tb_GetNamedResource(Int32(bitPattern: fourCC("TEST")), name)
            #expect(bytes(h) == Array("first".utf8))
            #expect(tb_CountResources(Int32(bitPattern: fourCC("TEST"))) == 2)
            #expect(tb_Count1Resources(Int32(bitPattern: fourCC("TEST"))) == 2)
            #expect(tb_GetIndResource(Int32(bitPattern: fourCC("TEST")), 2) != 0)
            #expect(tb_GetIndResource(Int32(bitPattern: fourCC("TEST")), 3) == 0)
            #expect(tb_CountTypes() == 3)
            let t = tb_NewPtr(4, false, true)
            tb_GetIndType(t, 2)
            #expect(rd32(t) == fourCC("STR "))
        }

        @Test func resourceInfo() {
            let h = tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128)
            let id = tb_NewPtr(2, false, true), type = tb_NewPtr(4, false, true), name = tb_NewPtr(256, false, true)
            tb_GetResInfo(h, id, type, name)
            #expect(Int16(bitPattern: rd16(id)) == 128)
            #expect(rd32(type) == fourCC("TEST"))
            #expect(rd8(name) == 5)
            #expect(tb_GetResourceSizeOnDisk(h) == 5)
        }

        @Test func purgeableAttributeMakesHandlePurgeable() {
            let h = tb_GetResource(Int32(bitPattern: fourCC("PURG")), 7)
            #expect(UInt8(bitPattern: tb_HGetState(h)) & 0x40 != 0)
        }

        @Test func resLoadFalseGivesEmptyHandleUntilLoadResource() {
            tb_SetResLoad(0)
            let h = tb_GetResource(Int32(bitPattern: fourCC("STR ")), 1)
            tb_SetResLoad(1)
            #expect(h != 0)
            #expect(rd32(h) == 0)
            tb_LoadResource(h)
            #expect(bytes(h) == cat([3], Array("abc".utf8)))
        }

        @Test func releaseAndDetach() {
            let h = tb_GetResource(Int32(bitPattern: fourCC("STR ")), 1)
            tb_DetachResource(h)
            #expect(tb_HomeResFile(h) == -1)
            let again = tb_GetResource(Int32(bitPattern: fourCC("STR ")), 1)
            #expect(again != h)
            tb_ReleaseResource(again)
            #expect(resErr() == 0)
        }

        @Test func uniqueIDAvoidsExistingIDs() {
            let id = tb_UniqueID(Int32(bitPattern: fourCC("TEST")))
            #expect(id != 128 && id != 129 && id >= 128)
        }

        @Test func closingRemovesTheFileFromTheChain() {
            tb_CloseResFile(refnum)
            #expect(resErr() == 0)
            #expect(tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128) == 0)
            #expect(tb_CurResFile() != refnum)
        }
    }
}
