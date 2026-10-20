import CCore
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
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

        /// A resource file's reference number reads its fork through the
        /// File Manager (installers read their compressed resources so).
        @Test func resourceFileReadsThroughTheFileManager() {
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            let buf = tb_NewPtr(16, false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: refnum))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), 16)
            wr16(pb + UInt32(IOParam_ioPosMode_off), 1) // fsFromStart
            wr32(pb + UInt32(IOParam_ioPosOffset_off), 0)
            #expect(tb_PBRead(pb, false) == 0)
            #expect(rd32(pb + UInt32(IOParam_ioActCount_off)) == 16)
            let map = rd32(rd32(0xA50)) // TopMapHndl: its map starts with the fork's header
            #expect((0..<16).allSatisfy { rd8(buf + UInt32($0)) == rd8(map + UInt32($0)) })
            #expect(tb_PBGetEOF(pb, false) == 0)
            #expect(rd32(pb + UInt32(IOParam_ioMisc_off)) > 16)
            #expect(tb_PBFlushFile(pb, false) == 0) // ZTerm flushes its phone book so
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

        /// ReadPartialResource reads the file, not the handle the program
        /// may have changed (PopChar Pro's installer checksums its own
        /// code, relocated in memory), and leaves an empty handle empty;
        /// a resource marked changed comes from memory.
        @Test func readPartialResourceReadsTheFile() {
            let buf = tb_NewPtr(8, false, true)
            let h = tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128)
            wr8(rd32(h), UInt8(ascii: "F"))
            tb_ReadPartialResource(h, 1, buf, 4)
            #expect(resErr() == 0 && (0..<4).map { rd8(buf + UInt32($0)) } == Array("irst".utf8))
            tb_ReadPartialResource(h, 0, buf, 1)
            #expect(rd8(buf) == UInt8(ascii: "f"))
            tb_ReadPartialResource(h, 2, buf, 4)
            #expect(resErr() == -39) // eofErr
            tb_SetResLoad(0)
            let s = tb_GetResource(Int32(bitPattern: fourCC("STR ")), 1)
            tb_SetResLoad(1)
            tb_ReadPartialResource(s, 1, buf, 3)
            #expect((0..<3).map { rd8(buf + UInt32($0)) } == Array("abc".utf8) && rd32(s) == 0)
            tb_SetResAttrs(h, Int16(tb_GetResAttrs(h)) | 2) // resChanged
            tb_ReadPartialResource(h, 0, buf, 1)
            #expect(rd8(buf) == UInt8(ascii: "F"))
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

        /// A changed resource isn't released (noErr all the same) or detached
        /// (resAttrErr): it stays to be written (ResEdit releases what an
        /// editor changed before Save).
        @Test func changedResourceIsKept() {
            let h = tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128)
            tb_SetResAttrs(h, Int16(tb_GetResAttrs(h)) | 2) // resChanged, as ResEdit sets it
            tb_ReleaseResource(h)
            #expect(resErr() == 0)    // kept, but noErr, as Inside Macintosh says
            tb_DetachResource(h)
            #expect(resErr() == -198)
            #expect(tb_HomeResFile(h) == refnum && bytes(h) == Array("first".utf8))
            #expect(tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128) == h)
        }

        /// A purgeable resource added to a file or changed stays until it is
        /// written (SoundApp adds its purgeable default PREF to its prefs).
        @Test func addedAndChangedResourcesAreNotPurged() {
            let h = tb_GetResource(Int32(bitPattern: fourCC("PURG")), 7)
            tb_ChangedResource(h)
            #expect(UInt8(bitPattern: tb_HGetState(h)) & 0x40 == 0)
            let added = tb_NewHandle(3, false, true)
            tb_HPurge(added)
            tb_AddResource(added, Int32(bitPattern: fourCC("PREF")), 300, 0)
            #expect(UInt8(bitPattern: tb_HGetState(added)) & 0x40 == 0)
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

        /// A file kept as its AppleDouble sidecar alone (an empty data fork,
        /// as archives give "Icon\r") resolves as an alias file to itself:
        /// ShatterBat walks its folder that way and stopped at its icon file.
        @Test func resolveAliasFileFindsASidecarOnlyFile() throws {
            let dir = launchTestApp()
            let spec = tb_NewPtr(70, false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname("Icon"), spec) == -43)
            tb_FSpCreateResFile(spec, Int32(bitPattern: fourCC("MACS")), Int32(bitPattern: fourCC("icon")), 0)
            try? FileManager.default.removeItem(at: dir.appendingPathComponent("Icon")) // the data fork goes
            #expect(tb_FSMakeFSSpec(0, 0, pname("Icon"), spec) == 0)
            let folder = tb_NewPtr(2, false, true), aliased = tb_NewPtr(2, false, true)
            #expect(tb_ResolveAliasFile(spec, 1, folder, aliased) == 0)
            #expect(rd8(folder) == 0 && rd8(aliased) == 0)
        }
    }

    @Suite struct SystemFolder {
        @Test func findFolderGivesPreferencesInsideTheSystemFolder() {
            launchTestApp()
            let vref = tb_NewPtr(2, false, true), dir = tb_NewPtr(4, false, true)
            #expect(tb_FindFolder(-1, Int32(bitPattern: fourCC("pref")), 1, vref, dir) == 0)
            let prefs = URL(fileURLWithPath: testSystemFolder).appendingPathComponent("Preferences")
            var isDir: ObjCBool = false
            #expect(FileManager.default.fileExists(atPath: prefs.path, isDirectory: &isDir) && isDir.boolValue)
            // A preferences file made there lands in the host folder.
            let name = tb_NewPtr(256, false, true)
            guestPString("App Prefs", at: name)
            tb_HCreateResFile(Int16(bitPattern: rd16(vref)), Int32(bitPattern: rd32(dir)), name)
            #expect(tb_ResError() == 0)
            #expect(FileManager.default.fileExists(atPath: prefs.appendingPathComponent("App Prefs").path))
            #expect(tb_FindFolder(-1, Int32(bitPattern: fourCC("zzzz")), 1, vref, dir) == -43)
        }

        @Test func sysEnvironsPointsAtTheSystemFolder() {
            launchTestApp()
            let env = tb_NewPtr(16, false, true)
            _ = tb_SysEnvirons(2, env)
            let vref = Int16(bitPattern: rd16(env + 14))
            #expect(vref != -1)
            #expect(rd16(UInt32(LM_BootDrive)) == UInt16(bitPattern: vref))
            let pb = tb_NewPtr(128, false, true)
            let buf = tb_NewPtr(32, false, true)
            wr16(pb + 22, rd16(UInt32(LM_BootDrive)))
            wr32(pb + 32, buf)
            wr32(pb + 36, 32)
            #expect(tb_PBHGetVolParms(pb, false) == 0)
            #expect(rd16(buf) == 2 && rd32(pb + 40) == 20)
        }
    }

    @Suite struct ResourceStubPatch {
        /// Installers patch OS trap A0FC to expand resources as they load.
        @Test func patchedStubSeesEveryLoad() {
            bootMachine()
            var fork = buildFork([TestResource(type: "TEST", id: 128, data: [10, 20])])
            #expect(rm_open_host_file(nil, &fork, UInt32(fork.count), true) > 0)
            let original = tb_GetTrapAddress(0xA0FC, true, false)
            let words: [UInt16] = [
                0x4EB9, UInt16(original >> 16), UInt16(original & 0xFFFF), // JSR original
                0x2250,                                                     // MOVEA.L (A0),A1
                0x5211,                                                     // ADDQ.B #1,(A1)
                0x4E75,                                                     // RTS
            ]
            let patch = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(patch + UInt32(i * 2), w) }
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            tb_SetTrapAddress(patch, 0xA0FC, true, false)
            defer { tb_SetTrapAddress(original, 0xA0FC, true, false) }
            let h = tb_GetResource(Int32(bitPattern: fourCC("TEST")), 128)
            #expect(h != 0 && rd8(rd32(h)) == 11 && rd8(rd32(h) + 1) == 20)
        }
    }

    @Suite struct DefaultDirectory {
        /// GetVol gives the application's folder as a working directory, so a
        /// bare name opens files next to the application.
        @Test func getVolGivesTheApplicationsFolder() {
            launchTestApp()
            let pb = tb_NewPtr(128, false, true)
            #expect(tb_PBGetVol(pb, false) == 0)
            let vref = Int16(bitPattern: rd16(pb + 22))
            #expect(vref != -1)
            let name = tb_NewPtr(64, false, true), spec = tb_NewPtr(70, false, true)
            guestPString("Test App", at: name)
            #expect(tb_FSMakeFSSpec(vref, 0, name, spec) == 0)
        }
    }

    @Suite struct FileControlBlocks {
        /// Applications read the FCB table and the volume's VCB directly.
        @Test func fcbTableDescribesOpenFiles() {
            launchTestApp()
            let table = rd32(UInt32(LM_FCBSPtr))
            #expect(table != 0 && rd16(UInt32(LM_FSFCBLen)) == 94)
            // The application's resource file is in the table, found from CurResFile.
            let rn = UInt32(UInt16(bitPattern: tb_CurResFile()))
            let fcb = table + rn
            let vcb = rd32(fcb + 20)
            #expect(vcb != 0 && Int16(bitPattern: rd16(vcb + 78)) == -1)
            #expect(rd8(fcb + 4) & 0x02 != 0) // resource fork
            #expect(rd32(UInt32(LM_VCBQHdr) + 2) == vcb)
            // GetFCBInfo on it gives the application's name.
            let pb = tb_NewPtr(128, false, true), name = tb_NewPtr(64, false, true)
            wr32(pb + 18, name)
            wr16(pb + 24, UInt16(rn))
            #expect(tb_PBGetFCBInfo(pb, false) == 0)
            #expect(pstring(at: name) == "Test App")
            // A data fork opened through the File Manager appears too, and goes on close.
            wr32(pb + 18, name)
            wr16(pb + 22, 0)
            wr8(pb + 27, 1)
            #expect(tb_PBOpen(pb, false) == 0)
            let drn = UInt32(rd16(pb + 24))
            #expect(rd32(table + drn) != 0 && rd8(table + drn + 4) & 0x02 == 0)
            #expect(tb_PBClose(pb, false) == 0)
            #expect(rd32(table + drn) == 0)
        }
    }

    @Suite struct SidecarOnlyFiles {
        /// A file with an empty data fork may be stored as just its sidecar.
        @Test func sidecarOnlyFileIsFoundAndListed() throws {
            let dir = launchTestApp()
            let side = dir.appendingPathComponent("._Lonely")
            try FileManager.default.copyItem(at: dir.appendingPathComponent("._Test App"), to: side)
            let name = tb_NewPtr(64, false, true), spec = tb_NewPtr(70, false, true)
            guestPString("Lonely", at: name)
            #expect(tb_FSMakeFSSpec(0, 0, name, spec) == 0)
            // Indexed: "Lonely" and "Test App" are the folder's two files.
            let pb = tb_NewPtr(128, false, true), out = tb_NewPtr(64, false, true)
            wr32(pb + 18, out)
            wr16(pb + 28, 1)
            #expect(tb_PBGetCatInfo(pb, false) == 0)
            #expect(pstring(at: out) == "Lonely")
        }
    }

    @Suite struct RootParent {
        /// Walking up parents ends at the root: its parent (directory 1)
        /// has no record of its own.
        @Test func rootParentIsNotFound() {
            launchTestApp()
            let pb = tb_NewPtr(128, false, true), name = tb_NewPtr(64, false, true)
            wr32(pb + 18, name)
            wr16(pb + 28, UInt16(bitPattern: -1))
            wr32(pb + 48, 2)
            #expect(tb_PBGetCatInfo(pb, false) == 0)
            #expect(rd32(pb + 100) == 1) // ioDrParID
            wr32(pb + 48, 1)
            #expect(tb_PBGetCatInfo(pb, false) == -43)
        }
    }

    @Suite struct LongNames {
        /// Host names longer than HFS's 31 characters get a stable short form
        /// that resolves back; names also match ignoring case.
        @Test func longNamesShortenAndResolve() throws {
            let dir = launchTestApp()
            let long = "A very long folder name that HFS could never hold"
            let sub = dir.appendingPathComponent(long)
            try FileManager.default.createDirectory(at: sub, withIntermediateDirectories: true)
            try Data("hi".utf8).write(to: sub.appendingPathComponent("Note"))
            // Index the folder to learn its Mac name.
            let pb = tb_NewPtr(128, false, true), name = tb_NewPtr(64, false, true)
            wr32(pb + 18, name)
            var found = ""
            for idx in 1...3 {
                wr16(pb + 28, UInt16(idx))
                wr16(pb + 22, 0); wr32(pb + 48, 0)
                if tb_PBGetCatInfo(pb, false) != 0 { break }
                let n = pstring(at: name)
                if n.hasPrefix("A very long") { found = n }
            }
            #expect(found.count == 31 && found.contains("#"))
            // ":<short>:note" (lower case) finds the file.
            let path = tb_NewPtr(256, false, true)
            guestPString(":" + found + ":note", at: path)
            let spec = tb_NewPtr(70, false, true)
            #expect(tb_FSMakeFSSpec(0, 0, path, spec) == 0)
        }
    }

    @Suite struct CatalogOrder {
        /// Indexed entries come in HFS catalog order, which ignores case:
        /// programs that look for a file by walking a folder (Bad Day on
        /// the Midway's CD check) expect "badday.dxr" before "CREDIT.DIR".
        @Test func indexingIgnoresCase() throws {
            let dir = launchTestApp()
            let sub = dir.appendingPathComponent("Disc")
            try FileManager.default.createDirectory(at: sub, withIntermediateDirectories: true)
            for n in ["CRXC.DIR", "credits.aif", "badday.dxr", "CREDIT.DIR"] {
                try Data("x".utf8).write(to: sub.appendingPathComponent(n))
            }
            let pb = tb_NewPtr(128, false, true), name = tb_NewPtr(64, false, true)
            guestPString("Disc", at: name)
            wr32(pb + 18, name)
            #expect(tb_PBGetCatInfo(pb, false) == 0)
            let dirID = rd32(pb + 48)
            var names: [String] = []
            for idx in 1...4 {
                wr32(pb + 18, name)
                wr16(pb + 22, 0); wr16(pb + 28, UInt16(idx)); wr32(pb + 48, dirID)
                #expect(tb_PBGetCatInfo(pb, false) == 0)
                names.append(pstring(at: name))
            }
            #expect(names == ["badday.dxr", "CREDIT.DIR", "credits.aif", "CRXC.DIR"])
        }
    }
}
}

extension BlueBox {
    /// Damaged resource forks: the parser and the Resource Manager refuse
    /// or read what they can, and never crash or read past the fork.
    @Suite struct DamagedForks {
        @Test func mutatedForksDoNotCrash() {
            bootMachine()
            let good = buildFork([
                TestResource(type: "TEST", id: 128, name: "hello", data: Array("first".utf8)),
                TestResource(type: "TEST", id: 129, attrs: 0x01, // compressed
                             data: dcmp0Resource(decompressedSize: 4, body: cat([0x02], Array("WXYZ".utf8), [0xFF]))),
                TestResource(type: "STR ", id: 1, data: cat([3], Array("abc".utf8))),
                TestResource(type: "PURG", id: 7, attrs: 0x20, data: [1, 2, 3]),
            ])
            var seed: UInt64 = 0x9E37_79B9_7F4A_7C15
            func next() -> Int {
                seed = seed &* 6364136223846793005 &+ 1442695040888963407
                return Int(truncatingIfNeeded: seed >> 33)
            }
            var opened = 0
            for round in 0..<20000 {
                var fork = good
                switch round % 4 {
                case 0: // a few bytes anywhere
                    for _ in 0..<(1 + next() % 4) { fork[next() % fork.count] = UInt8(truncatingIfNeeded: next()) }
                case 1: // the header or the map, where offsets and counts live
                    let mapAt = Int(fork[4]) << 24 | Int(fork[5]) << 16 | Int(fork[6]) << 8 | Int(fork[7])
                    for _ in 0..<(1 + next() % 3) {
                        let i = round % 8 == 1 ? next() % 16 : mapAt + next() % max(1, fork.count - mapAt)
                        if i < fork.count { fork[i] = UInt8(truncatingIfNeeded: next()) }
                    }
                case 2: // cut short
                    fork = Array(fork.prefix(next() % fork.count))
                default: // extreme values in the map's words
                    let mapAt = Int(fork[7]) | Int(fork[6]) << 8
                    let i = min(fork.count - 2, mapAt + 22 + next() % 40)
                    if i >= 0 { fork[i] = 0xFF; fork[i + 1] = 0xFF }
                }
                var idx = rsrc_index()
                var err = [CChar](repeating: 0, count: 256)
                if rsrc_parse(fork, fork.count, &idx, &err, err.count) {
                    for k in 0..<idx.count {
                        var e = idx.entries[Int(k)]
                        _ = rsrc_data(fork, &idx, &e)
                    }
                    rsrc_index_free(&idx)
                }
                let rn = rm_open_host_file(nil, &fork, UInt32(fork.count), true)
                guard rn > 0 else { continue }
                opened += 1
                tb_UseResFile(rn)
                let types = tb_Count1Types()
                for t in 0..<max(0, min(Int(types), 8)) {
                    let tp = tb_NewPtr(4, false, true)
                    tb_Get1IndType(tp, Int16(t + 1))
                    let type = Int32(bitPattern: rd32(tp))
                    tb_DisposePtr(tp)
                    for k in 0..<max(0, min(Int(tb_Count1Resources(type)), 8)) {
                        let h = tb_Get1IndResource(type, Int16(k + 1))
                        if h != 0 { _ = tb_GetHandleSize(h) }
                    }
                    _ = tb_Get1Resource(type, 128)
                }
                tb_CloseResFile(rn)
            }
            #expect(opened > 0) // some damage leaves a usable map
        }
    }
}
