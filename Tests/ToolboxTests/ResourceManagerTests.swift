import CCore
import Foundation
import ForkStore
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

        @Test func missingResourceLeavesResErrorAlone() {
            #expect(tb_GetResource(Int32(bitPattern: fourCC("TEST")), 999) == 0)
            #expect(resErr() == 0) // as on real System 7.1 (oracle probe)
            #expect(tb_GetIndResource(Int32(bitPattern: fourCC("TEST")), 99) == 0)
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
            #expect(tb_Count1Types() == 3)
            #expect(tb_CountTypes() > 3) // the chain includes the System file
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
    
    @Suite struct ResourceWriting {
        let type = Int32(bitPattern: fourCC("PREF"))

        func handle(_ bytes: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            return h
        }

        func bytes(_ h: UInt32) -> [UInt8] {
            (0..<Int(tb_GetHandleSize(h))).map { rd8(rd32(h) + UInt32($0)) }
        }

        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        @Test func createAddCloseAndReopen() throws {
            let dir = launchTestApp()
            let name = pname("Test Prefs")
            tb_CreateResFile(name)
            #expect(tb_ResError() == 0)
            tb_CreateResFile(name)
            #expect(tb_ResError() == -48) // already has a resource fork
            let rn = tb_OpenResFile(name)
            #expect(rn > 0)
            tb_AddResource(handle([1, 2, 3]), type, 128, pname("one"))
            #expect(tb_ResError() == 0)
            tb_AddResource(handle(Array("hello".utf8)), Int32(bitPattern: fourCC("STR ")), 200, 0)
            tb_AddResource(handle([9]), type, 129, 0)
            tb_CloseResFile(rn)
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("._Test Prefs").path))

            let rn2 = tb_OpenResFile(name)
            #expect(rn2 > 0)
            #expect(tb_Count1Resources(type) == 2)
            let one = tb_Get1NamedResource(type, pname("one"))
            #expect(one != 0 && bytes(one) == [1, 2, 3])
            // Change one resource, remove another, rename a third.
            tb_SetHandleSize(one, 4)
            wr8(rd32(one) + 3, 4)
            tb_ChangedResource(one)
            let nine = tb_Get1Resource(type, 129)
            tb_RemoveResource(nine)
            #expect(tb_ResError() == 0)
            tb_DisposeHandle(nine)
            let str = tb_Get1Resource(Int32(bitPattern: fourCC("STR ")), 200)
            tb_SetResInfo(str, 201, pname("greeting"))
            tb_UpdateResFile(rn2)
            #expect(tb_ResError() == 0)
            tb_CloseResFile(rn2)

            let rn3 = tb_OpenResFile(name)
            #expect(bytes(tb_Get1Resource(type, 128)) == [1, 2, 3, 4])
            #expect(tb_Get1Resource(type, 129) == 0)
            let greeting = tb_Get1NamedResource(Int32(bitPattern: fourCC("STR ")), pname("greeting"))
            #expect(greeting != 0 && bytes(greeting) == Array("hello".utf8))
            let id = tb_NewPtr(2, false, true)
            tb_GetResInfo(greeting, id, 0, 0)
            #expect(rd16(id) == 201)
            tb_CloseResFile(rn3)
        }

        @Test func fspCreateResFileSetsTypeAndCreator() {
            let dir = launchTestApp()
            let spec = tb_NewPtr(70, false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname("Doc"), spec) == -43)
            tb_FSpCreateResFile(spec, Int32(bitPattern: fourCC("CRea")), Int32(bitPattern: fourCC("TYpe")), 0)
            #expect(tb_ResError() == 0)
            var f = mac_file()
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_read(dir.appendingPathComponent("Doc").path, &f, &err, err.count))
            #expect(mac_file_type(&f) == fourCC("TYpe") && mac_file_creator(&f) == fourCC("CRea"))
            #expect(f.rsrc_len > 0)
            mac_file_free(&f)
            let rn = tb_FSpOpenResFile(spec, 0)
            #expect(rn > 0 && tb_Count1Types() == 0)
            tb_CloseResFile(rn)
        }
    }
}
}
