import CCore
import CPU68K
import CPUPPC
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// Mac OS 9's File Manager calls: FSRefs, forks with 64-bit positions,
    /// iterators, catalog information and Unicode names, and the Alias,
    /// Folder, Resource and File System Manager calls that go with them.
    @Suite struct HFSPlus {
        let fnfErr: Int16 = -43, dupFNErr: Int16 = -48, eofErr: Int16 = -39, nsvErr: Int16 = -35
        let errFSBadFSRef: Int16 = -1401, errFSRefsDifferent: Int16 = -1420, errFSNoMoreItems: Int16 = -1417
        let errFSForkNotFound: Int16 = -1409, errFSBadPosMode: Int16 = -1412, errFSNotAFolder: Int16 = -1407
        let errFSIteratorNotSupported: Int16 = -1424, errFSIteratorNotFound: Int16 = -1423

        func ptr(_ n: UInt32) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

        /// An FSRef for a host path, through its FSSpec.
        func ref(_ path: String) -> UInt32 {
            let spec = ptr(UInt32(FSSpec_SIZEOF))
            fm_spec_for_path(path, spec)
            let r = ptr(80)
            #expect(tb_FSpMakeFSRef(spec, r) == 0, "FSpMakeFSRef \(path)")
            return r
        }

        /// A Unicode name in guest memory: (pointer, length in characters).
        func uni(_ s: String) -> (UInt32, UInt32) {
            let u = Array(s.utf16)
            let p = ptr(UInt32(max(u.count * 2, 2)))
            for (i, c) in u.enumerated() { wr16(p + UInt32(i * 2), c) }
            return (p, UInt32(u.count))
        }

        func uniString(_ p: UInt32) -> String {
            let n = Int(rd16(p))
            return String(decoding: (0..<n).map { rd16(p + 2 + UInt32($0 * 2)) }, as: UTF16.self)
        }

        func get64(_ p: UInt32) -> UInt64 { UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4)) }
        func put64(_ p: UInt32, _ v: UInt64) {
            wr32(p, UInt32(v >> 32))
            wr32(p + 4, UInt32(v & 0xFFFF_FFFF))
        }

        func bytes(_ b: [UInt8]) -> UInt32 {
            let p = ptr(UInt32(max(b.count, 1)))
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        func read(_ p: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { rd8(p + UInt32($0)) } }

        func catInfo(_ r: UInt32, _ which: UInt32 = 0x3FFFF) -> UInt32 {
            let ci = ptr(UInt32(FSCatalogInfo_SIZEOF))
            #expect(tb_FSGetCatalogInfo(r, which, ci, 0, 0, 0) == 0)
            return ci
        }

        func nodeID(_ r: UInt32) -> UInt32 { rd32(catInfo(r, 0x10) + UInt32(FSCatalogInfo_nodeID_off)) }

        func hostFile(_ url: URL, _ text: String) {
            try? Data(text.utf8).write(to: url)
        }

        @Test func refsNameObjectsAndCompare() {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("Résumé.txt")
            hostFile(file, "hello")
            let r1 = ref(file.path), r2 = ref(file.path), folder = ref(dir.path)
            #expect(tb_FSCompareFSRefs(r1, r2) == 0)
            #expect(tb_FSCompareFSRefs(r1, folder) == errFSRefsDifferent)

            let ci = ptr(UInt32(FSCatalogInfo_SIZEOF)), name = ptr(512), spec = ptr(UInt32(FSSpec_SIZEOF)), parent = ptr(80)
            #expect(tb_FSGetCatalogInfo(r1, 0x3FFFF, ci, name, spec, parent) == 0)
            #expect(uniString(name) == "Résumé.txt")
            #expect(rd16(ci + UInt32(FSCatalogInfo_nodeFlags_off)) & 0x10 == 0)
            #expect(rd32(ci + UInt32(FSCatalogInfo_finderInfo_off)) == fourCC("TEXT"))
            #expect(get64(ci + UInt32(FSCatalogInfo_dataLogicalSize_off)) == 5)
            #expect(Int16(bitPattern: rd16(ci + UInt32(FSCatalogInfo_volume_off))) == -1)
            #expect(tb_FSCompareFSRefs(parent, folder) == 0)
            #expect(rd32(ci + UInt32(FSCatalogInfo_parentDirID_off)) == nodeID(folder))
            #expect(rd32(spec + UInt32(FSSpec_parID_off)) == nodeID(folder))
            // The same object by its parent and Unicode name.
            let (n, len) = uni("résumé.TXT") // HFS ignores case
            let r3 = ptr(80)
            #expect(tb_FSMakeFSRefUnicode(folder, len, n, 0, r3) == 0)
            #expect(tb_FSCompareFSRefs(r3, r1) == 0)
            let (m, mlen) = uni("missing")
            #expect(tb_FSMakeFSRefUnicode(folder, mlen, m, 0, r3) == fnfErr)
            #expect(tb_FSMakeFSRefUnicode(r1, len, n, 0, r3) == errFSNotAFolder)
            // Not an FSRef at all.
            #expect(tb_FSGetCatalogInfo(ptr(80), 0, 0, 0, 0, 0) == errFSBadFSRef)
            // A folder: its ID and number of entries.
            let fci = catInfo(folder)
            #expect(rd16(fci + UInt32(FSCatalogInfo_nodeFlags_off)) & 0x10 != 0)
            #expect(rd32(fci + UInt32(FSCatalogInfo_valence_off)) == 2) // Test App and the file
            // The volume's root: in 1, its ID 2, named for the volume.
            let rootSpec = ptr(UInt32(FSSpec_SIZEOF))
            #expect(tb_FSMakeFSSpec(-1, 2, 0, rootSpec) == 0)
            #expect(rd32(rootSpec + UInt32(FSSpec_parID_off)) == 1)
            let root = ptr(80)
            #expect(tb_FSpMakeFSRef(rootSpec, root) == 0)
            let rci = catInfo(root)
            #expect(rd32(rci + UInt32(FSCatalogInfo_nodeID_off)) == 2)
            #expect(rd32(rci + UInt32(FSCatalogInfo_parentDirID_off)) == 1)
            #expect(tb_FSGetCatalogInfo(root, 0, 0, name, 0, 0) == 0)
            #expect(uniString(name) == "Macintosh HD")
            // PBMakeFSRef: (vRefNum, dirID, name).
            let pb = ptr(UInt32(FSRefParam_SIZEOF))
            wr16(pb + UInt32(FSRefParam_ioVRefNum_off), 0xFFFF)
            wr32(pb + UInt32(FSRefParam_ioDirID_off), 2)
            wr32(pb + UInt32(FSRefParam_newRef_off), r3)
            #expect(tb_PBMakeFSRef(pb, false) == 0)
            #expect(tb_FSCompareFSRefs(r3, root) == 0)
        }

        @Test func createRenameMoveAndDelete() throws {
            let dir = launchTestApp()
            let top = ref(dir.path)
            let (sub, sublen) = uni("Sub Folder")
            let subRef = ptr(80), subID = ptr(4)
            #expect(tb_FSCreateDirectoryUnicode(top, sublen, sub, 0, 0, subRef, 0, subID) == 0)
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("Sub Folder").path))
            #expect(rd32(subID) == nodeID(subRef))
            #expect(tb_FSCreateDirectoryUnicode(top, sublen, sub, 0, 0, 0, 0, 0) == dupFNErr)

            // A long name, with Finder information set as it is made.
            let longName = "A file whose name is longer than thirty-one characters.dat"
            let (ln, llen) = uni(longName)
            let ci = ptr(UInt32(FSCatalogInfo_SIZEOF))
            wr32(ci + UInt32(FSCatalogInfo_finderInfo_off), fourCC("BINA"))
            wr32(ci + UInt32(FSCatalogInfo_finderInfo_off) + 4, fourCC("TEST"))
            let fileRef = ptr(80), spec = ptr(UInt32(FSSpec_SIZEOF))
            #expect(tb_FSCreateFileUnicode(subRef, llen, ln, 0x800, ci, fileRef, spec) == 0)
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("Sub Folder/\(longName)").path))
            let got = catInfo(fileRef)
            #expect(rd32(got + UInt32(FSCatalogInfo_finderInfo_off)) == fourCC("BINA"))
            #expect(rd32(got + UInt32(FSCatalogInfo_finderInfo_off) + 4) == fourCC("TEST"))
            let finfo = ptr(16)
            #expect(tb_FSpGetFInfo(spec, finfo) == 0) // its FSSpec names it too
            #expect(rd32(finfo) == fourCC("BINA"))

            // Renamed: the old FSRef follows the file.
            let (rn, rlen) = uni("Renamed ✓.dat")
            let renamed = ptr(80)
            #expect(tb_FSRenameUnicode(fileRef, rlen, rn, 0, renamed) == 0)
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("Sub Folder/Renamed ✓.dat").path))
            let name = ptr(512)
            #expect(tb_FSGetCatalogInfo(fileRef, 0, 0, name, 0, 0) == 0)
            #expect(uniString(name) == "Renamed ✓.dat")
            #expect(tb_FSCompareFSRefs(renamed, fileRef) == 0)

            // Moved up a level, still the same object.
            let moved = ptr(80)
            #expect(tb_FSMoveObject(fileRef, top, moved) == 0)
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("Renamed ✓.dat").path))
            #expect(tb_FSCompareFSRefs(moved, fileRef) == 0)
            #expect(tb_FSMoveObject(subRef, subRef, 0) == -122) // badMovErr: into itself
            let parent = ptr(80)
            #expect(tb_FSGetCatalogInfo(fileRef, 0, 0, 0, 0, parent) == 0)
            #expect(tb_FSCompareFSRefs(parent, top) == 0)

            // Two files trade contents.
            let other = dir.appendingPathComponent("other.dat")
            hostFile(other, "other")
            hostFile(dir.appendingPathComponent("Renamed ✓.dat"), "first")
            #expect(tb_FSExchangeObjects(fileRef, ref(other.path)) == 0)
            #expect(try String(contentsOf: other, encoding: .utf8) == "first")

            // Deleted: gone, and its FSRef with it; a folder must be empty.
            #expect(tb_FSDeleteObject(fileRef) == 0)
            #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("Renamed ✓.dat").path))
            #expect(tb_FSGetCatalogInfo(fileRef, 0, 0, 0, 0, 0) == fnfErr)
            #expect(tb_FSDeleteObject(subRef) == 0)
        }

        @Test func catalogInfoIsSetOnFiles() {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("doc")
            hostFile(file, "x")
            let r = ref(file.path)
            let ci = ptr(UInt32(FSCatalogInfo_SIZEOF))
            wr32(ci + UInt32(FSCatalogInfo_finderInfo_off), fourCC("TEXT"))
            wr32(ci + UInt32(FSCatalogInfo_finderInfo_off) + 4, fourCC("ttxt"))
            // 2000-01-01 00:00:00 UTC.
            wr16(ci + UInt32(FSCatalogInfo_createDate_off), 0)
            wr32(ci + UInt32(FSCatalogInfo_createDate_off) + 2, 3_029_529_600)
            #expect(tb_FSSetCatalogInfo(r, 0x820, ci) == 0)
            #expect(tb_FSSetCatalogInfo(r, 0x4000, ci) == -1405) // errFSBadInfoBitmap: sizes aren't set
            let got = catInfo(r)
            #expect(rd32(got + UInt32(FSCatalogInfo_finderInfo_off) + 4) == fourCC("ttxt"))
            #expect(rd32(got + UInt32(FSCatalogInfo_createDate_off) + 2) == 3_029_529_600)
        }

        @Test func forksReadAndWriteAnywhere() throws {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("data.bin")
            hostFile(file, "")
            let r = ref(file.path)
            let rnp = ptr(2), actual = ptr(4), pos = ptr(8)
            #expect(tb_FSOpenFork(r, 0, 0, 3, rnp) == 0)
            let rn = Int16(bitPattern: rd16(rnp))
            put64(pos, 0)
            #expect(tb_FSWriteFork(rn, 1, pos, 10, bytes(Array("0123456789".utf8)), actual) == 0)
            #expect(rd32(actual) == 10)
            #expect(tb_FSGetForkSize(rn, pos) == 0)
            #expect(get64(pos) == 10)
            put64(pos, 4)
            #expect(tb_FSSetForkPosition(rn, 1, pos) == 0)
            #expect(tb_FSGetForkPosition(rn, pos) == 0)
            #expect(get64(pos) == 4)
            let buf = ptr(16)
            #expect(tb_FSReadFork(rn, 0, 0, 3, buf, actual) == 0) // at the mark
            #expect(read(buf, 3) == Array("456".utf8))
            put64(pos, 8)
            #expect(tb_FSReadFork(rn, 1, pos, 5, buf, actual) == eofErr)
            #expect(rd32(actual) == 2)
            #expect(tb_FSReadFork(rn, 0x80 | 1, pos, 5, buf, actual) == errFSBadPosMode) // no newline mode
            put64(pos, 0xFFFF_FFFF_FFFF_FFFE) // -2 from the end
            #expect(tb_FSSetForkPosition(rn, 2, pos) == 0)
            #expect(tb_FSGetForkPosition(rn, pos) == 0)
            #expect(get64(pos) == 8)
            put64(pos, 4)
            #expect(tb_FSSetForkSize(rn, 1, pos) == 0)
            #expect(tb_FSGetForkSize(rn, pos) == 0)
            #expect(get64(pos) == 4)

            // The open fork, as GetForkCBInfo reports it.
            let info = ptr(UInt32(FSForkInfo_SIZEOF)), cbref = ptr(80), it = ptr(2), act = ptr(2)
            #expect(tb_FSGetForkCBInfo(rn, 0, 0, act, info, cbref, 0) == 0)
            #expect(Int16(bitPattern: rd16(act)) == rn)
            #expect(get64(info + UInt32(FSForkInfo_logicalEOF_off)) == 4)
            #expect(tb_FSCompareFSRefs(cbref, r) == 0)
            wr16(it, 0)
            var seen = false
            while tb_FSGetForkCBInfo(0, 0, it, act, 0, 0, 0) == 0 {
                seen = seen || Int16(bitPattern: rd16(act)) == rn
            }
            #expect(seen)
            let size = ptr(8), alloc = ptr(8)
            put64(size, 4096)
            #expect(tb_FSAllocateFork(rn, 0, 0, 0, size, alloc) == 0)
            #expect(get64(alloc) == 4096)
            #expect(tb_FSCloseFork(rn) == 0)
            #expect(try Data(contentsOf: file) == Data("0123".utf8))

            // The resource fork, by name; others there are none of.
            let rsrcName = ptr(512)
            #expect(tb_FSGetResourceForkName(rsrcName) == 0)
            #expect(uniString(rsrcName) == "RESOURCE_FORK")
            #expect(tb_FSOpenFork(r, 13, rsrcName + 2, 3, rnp) == 0)
            let rrn = Int16(bitPattern: rd16(rnp))
            #expect(tb_FSWriteFork(rrn, 2, 0, 3, bytes([1, 2, 3]), actual) == 0)
            #expect(tb_FSCloseFork(rrn) == 0)
            let (other, olen) = uni("com.example.fork")
            #expect(tb_FSOpenFork(r, olen, other, 1, rnp) == errFSForkNotFound)

            // Forks with something in them, the data fork first.
            let iter = ptr(16), fname = ptr(512), fsize = ptr(8)
            #expect(tb_FSIterateForks(r, iter, fname, fsize, 0) == 0)
            #expect(uniString(fname) == "" && get64(fsize) == 4)
            #expect(tb_FSIterateForks(r, iter, fname, fsize, 0) == 0)
            #expect(uniString(fname) == "RESOURCE_FORK" && get64(fsize) == 3)
            #expect(tb_FSIterateForks(r, iter, fname, fsize, 0) == errFSNoMoreItems)
            // Deleting a fork empties it.
            #expect(tb_FSDeleteFork(r, 13, rsrcName + 2) == 0)
            let ci = catInfo(r, 0x8000)
            #expect(get64(ci + UInt32(FSCatalogInfo_rsrcLogicalSize_off)) == 0)
        }

        @Test func iteratorsListAFolderAndSearchUnderIt() {
            let dir = launchTestApp()
            let list = dir.appendingPathComponent("list")
            try? FileManager.default.createDirectory(at: list.appendingPathComponent("c"), withIntermediateDirectories: true)
            hostFile(list.appendingPathComponent("b.txt"), "b")
            hostFile(list.appendingPathComponent("A.txt"), "a")
            hostFile(list.appendingPathComponent("c/d.txt"), "d")
            let lr = ref(list.path)
            let itp = ptr(4), actual = ptr(4), names = ptr(512 * 4), refs = ptr(80 * 4), cis = ptr(144 * 4)
            #expect(tb_FSOpenIterator(lr, 0, itp) == 0)
            let it = rd32(itp)
            #expect(tb_FSGetCatalogInfoBulk(it, 2, actual, 0, 0x2010, cis, refs, 0, names) == 0)
            #expect(rd32(actual) == 2)
            #expect(uniString(names) == "A.txt" && uniString(names + 512) == "b.txt")
            #expect(rd32(cis + UInt32(FSCatalogInfo_nodeID_off)) == nodeID(refs))
            #expect(tb_FSGetCatalogInfoBulk(it, 2, actual, 0, 0, 0, 0, 0, names) == errFSNoMoreItems)
            #expect(rd32(actual) == 1)
            #expect(uniString(names) == "c")
            #expect(tb_FSCatalogSearch(it, 0, 1, actual, 0, 0, 0, 0, 0, 0) == errFSIteratorNotSupported)
            #expect(tb_FSCloseIterator(it) == 0)
            #expect(tb_FSCloseIterator(it) == errFSIteratorNotFound)

            // Everything under the folder with "TXT" in its name, two at a time.
            #expect(tb_FSOpenIterator(lr, 1, itp) == 0)
            let sit = rd32(itp)
            let params = ptr(UInt32(FSSearchParams_SIZEOF))
            let (n, len) = uni("TXT")
            wr32(params + UInt32(FSSearchParams_searchBits_off), 1) // fsSBPartialName
            wr32(params + UInt32(FSSearchParams_searchNameLength_off), len)
            wr32(params + UInt32(FSSearchParams_searchName_off), n)
            #expect(tb_FSCatalogSearch(sit, params, 2, actual, 0, 0, 0, 0, 0, names) == 0)
            var found = (0..<Int(rd32(actual))).map { uniString(names + UInt32($0 * 512)) }
            #expect(tb_FSCatalogSearch(sit, params, 2, actual, 0, 0, 0, 0, 0, names) == errFSNoMoreItems)
            found += (0..<Int(rd32(actual))).map { uniString(names + UInt32($0 * 512)) }
            #expect(found == ["A.txt", "b.txt", "d.txt"])
            #expect(tb_FSGetCatalogInfoBulk(sit, 1, actual, 0, 0, 0, 0, 0, 0) == errFSIteratorNotSupported)
            #expect(tb_FSCloseIterator(sit) == 0)
            // Folders only, by their node flags.
            #expect(tb_FSOpenIterator(lr, 1, itp) == 0)
            let i1 = ptr(144), i2 = ptr(144)
            wr16(i1 + UInt32(FSCatalogInfo_nodeFlags_off), 0x10)
            wr16(i2 + UInt32(FSCatalogInfo_nodeFlags_off), 0x10)
            wr32(params + UInt32(FSSearchParams_searchBits_off), 4) // fsSBFlAttrib
            wr32(params + UInt32(FSSearchParams_searchInfo1_off), i1)
            wr32(params + UInt32(FSSearchParams_searchInfo2_off), i2)
            #expect(tb_FSCatalogSearch(rd32(itp), params, 10, actual, 0, 0, 0, 0, 0, names) == errFSNoMoreItems)
            #expect(rd32(actual) == 1 && uniString(names) == "c")
            #expect(tb_FSCloseIterator(rd32(itp)) == 0)
        }

        @Test func volumesBySizeAndName() {
            launchTestApp()
            let actual = ptr(2), info = ptr(UInt32(FSVolumeInfo_SIZEOF)), name = ptr(512), root = ptr(80)
            #expect(tb_FSGetVolumeInfo(0, 1, actual, 0xFFFF, info, name, root) == 0)
            #expect(Int16(bitPattern: rd16(actual)) == -1)
            #expect(uniString(name) == "Macintosh HD")
            #expect(rd16(info + UInt32(FSVolumeInfo_signature_off)) == 0x482B) // 'H+'
            #expect(get64(info + UInt32(FSVolumeInfo_totalBytes_off)) > 0)
            #expect(rd16(info + UInt32(FSVolumeInfo_driveNumber_off)) == 1)
            #expect(nodeID(root) == 2)
            #expect(tb_FSGetVolumeInfo(-1, 0, actual, 0, 0, 0, 0) == 0) // by refnum
            #expect(tb_FSGetVolumeInfo(0, 9, actual, 0, 0, 0, 0) == nsvErr)
            #expect(tb_FSSetVolumeInfo(-1, 0x2000, info) == 0)

            // PBXGetVolInfo: PBHGetVInfo's answer and the size in bytes.
            let xpb = ptr(UInt32(XVolumeParam_SIZEOF))
            wr16(xpb + UInt32(IOParam_ioVRefNum_off), 0xFFFF)
            #expect(tb_PBXGetVolInfo(xpb, false) == 0)
            #expect(rd16(xpb + UInt32(HVolumeParam_ioVSigWord_off)) == 0x4244)
            #expect(get64(xpb + UInt32(XVolumeParam_ioVTotalBytes_off)) == get64(info + UInt32(FSVolumeInfo_totalBytes_off)))

            // PBMakeFSSpec: FSMakeFSSpec with the FSSpec in ioMisc.
            let pb = ptr(UInt32(HParamBlockRec_SIZEOF)), spec = ptr(UInt32(FSSpec_SIZEOF))
            wr16(pb + UInt32(IOParam_ioVRefNum_off), 0xFFFF)
            wr32(pb + UInt32(HFileParam_ioDirID_off), 2)
            wr32(pb + UInt32(HIoParam_ioMisc_off), spec)
            #expect(tb_PBMakeFSSpec(pb, false) == 0)
            #expect(rd32(spec + UInt32(FSSpec_parID_off)) == 1)
            #expect(tb_PBGetXCatInfo(pb, false) == -50) // no ProDOS or AFP short names here
        }

        @Test func resourceFilesByFSRef() {
            let dir = launchTestApp()
            let top = ref(dir.path)
            let (n, len) = uni("Resources ƒ")
            let ci = ptr(UInt32(FSCatalogInfo_SIZEOF))
            wr32(ci + UInt32(FSCatalogInfo_finderInfo_off), fourCC("rsrc"))
            wr32(ci + UInt32(FSCatalogInfo_finderInfo_off) + 4, fourCC("RSED"))
            let r = ptr(80), spec = ptr(UInt32(FSSpec_SIZEOF)), inChain = ptr(2), rnp = ptr(2)
            tb_FSCreateResFile(top, len, n, 0x800, ci, r, spec)
            #expect(resErr() == 0)
            tb_FSCreateResFile(top, len, n, 0, 0, 0, 0)
            #expect(resErr() == dupFNErr)
            #expect(rd32(catInfo(r) + UInt32(FSCatalogInfo_finderInfo_off)) == fourCC("rsrc"))
            #expect(tb_FSResourceFileAlreadyOpen(r, inChain, rnp) == 0)
            let rn = tb_FSOpenResFile(r, 3)
            #expect(rn > 0)
            #expect(tb_FSResourceFileAlreadyOpen(r, inChain, rnp) != 0)
            #expect(Int16(bitPattern: rd16(rnp)) == rn && rd8(inChain) == 1)
            #expect(tb_FSpResourceFileAlreadyOpen(spec, inChain, rnp) != 0)
            tb_CloseResFile(rn)
            #expect(tb_FSpResourceFileAlreadyOpen(spec, inChain, rnp) == 0)
            #expect(tb_FSOpenResFile(ref(dir.path), 1) == -1) // a folder
        }

        @Test func aliasesAndFoldersByFSRef() {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("target")
            hostFile(file, "t")
            let r = ref(file.path), folder = ref(dir.path)
            let ah = ptr(4), target = ptr(80), changed = ptr(2), isAlias = ptr(2), isFolder = ptr(2)
            #expect(tb_FSNewAlias(0, r, ah) == 0)
            #expect(tb_FSResolveAlias(0, rd32(ah), target, changed) == 0)
            #expect(tb_FSCompareFSRefs(target, r) == 0)
            #expect(tb_FSIsAliasFile(r, isAlias, isFolder) == 0)
            #expect(rd8(isAlias) == 0 && rd8(isFolder) == 0)
            #expect(tb_FSIsAliasFile(folder, isAlias, isFolder) == 0)
            #expect(rd8(isFolder) == 1)
            // A file with the Finder's isAlias flag is an alias file.
            let ci = catInfo(r)
            wr16(ci + UInt32(FSCatalogInfo_finderInfo_off) + 8, 0x8000)
            #expect(tb_FSSetCatalogInfo(r, 0x800, ci) == 0)
            #expect(tb_FSIsAliasFile(r, isAlias, isFolder) == 0)
            #expect(rd8(isAlias) == 1)
            let resolved = ptr(80)
            memcpyGuest(resolved, folder, 80)
            #expect(tb_FSResolveAliasFile(resolved, 1, isFolder, isAlias) == 0)
            #expect(tb_FSCompareFSRefs(resolved, folder) == 0 && rd8(isFolder) == 1)

            // FSFindFolder agrees with FindFolder.
            let pref = ptr(80), vref = ptr(2), dirID = ptr(4)
            let prefs = Int32(bitPattern: fourCC("pref"))
            #expect(tb_FSFindFolder(-32768, fourCC("pref"), 1, pref) == 0)
            #expect(tb_FindFolder(-32768, prefs, 1, vref, dirID) == 0)
            #expect(nodeID(pref) == rd32(dirID))
            #expect(tb_FindFolderExtended(-32768, fourCC("pref"), 1, 0, 0, vref, dirID) == 0)
            #expect(nodeID(pref) == rd32(dirID))
            #expect(tb_ReleaseFolder(-32768, fourCC("pref")) == 0)
            #expect(tb_ReleaseFolder(-32768, fourCC("zzzz")) == fnfErr)
            let proc = ptr(4)
            #expect(tb_FolderManagerRegisterNotificationProc(proc, 7, 0) == 0)
            #expect(tb_FolderManagerUnregisterNotificationProc(proc, 7) == 0)
            #expect(tb_FolderManagerUnregisterNotificationProc(proc, 7) == -50)
        }

        func memcpyGuest(_ to: UInt32, _ from: UInt32, _ n: UInt32) {
            for i in 0..<n { wr8(to + i, rd8(from + i)) }
        }

        @Test func fileSystemManagerHasOnlyWhatIsInstalled() {
            launchTestApp()
            let size = ptr(2), buf = ptr(200)
            wr16(size, 200)
            #expect(tb_GetFSInfo(-1, 1, size, buf) == -431) // fsmFFSNotFoundErr: HFS has no FSD
            #expect(tb_GetFSInfo(1, -1, size, buf) == -431)
            let fsd = ptr(120)
            wr16(fsd + 4, 120)          // fsdLength
            wr16(fsd + 8, 0x4142)       // fileSystemFSID
            #expect(tb_InstallFS(fsd) == 0)
            #expect(tb_InstallFS(fsd) == -435) // fsmDuplicateFSIDErr
            #expect(tb_GetFSInfo(0, 0x4142, size, buf) == 0)
            #expect(rd16(size) == 120 && rd16(buf + 8) == 0x4142)
            #expect(tb_InformFFS(0x4142, 0) == 0)
            #expect(tb_RemoveFS(0x4142) == 0)
            #expect(tb_RemoveFS(0x4142) == -431)
            #expect(tb_InformFSM(0, 0) == 0)
            #expect(tb_InformFSM(9, 0) == -438)
        }

        /// 68k programs reach the calls through _HFSDispatch ($A260),
        /// $A2A8 (fork reads and writes) and _HighLevelFSDispatch ($AA52).
        @Test func sixtyEightKCallsDispatch() {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("68k.dat")
            hostFile(file, "abcdef")
            let r = ref(file.path)
            func hi(_ v: UInt32) -> UInt16 { UInt16(v >> 16) }
            func lo(_ v: UInt32) -> UInt16 { UInt16(v & 0xFFFF) }
            func run(_ words: [UInt16]) -> String {
                let code = tb_NewPtr(Int32(words.count * 2), false, false)
                for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
                let stack = tb_NewPtr(65536, false, true)
                g_cpu!.pointee.a.7 = stack + 65536 - 16
                g_cpu!.pointee.pc = code
                g_cpu!.pointee.halted = false
                _ = cpu_run(g_cpu!, 100_000)
                return String(cString: machine_halt_reason())
            }
            // PBGetCatalogInfoSync: MOVEQ #$76,D0; _HFSDispatch.
            let pb = ptr(UInt32(FSRefParam_SIZEOF)), ci = ptr(UInt32(FSCatalogInfo_SIZEOF))
            wr32(pb + UInt32(FSRefParam_ref_off), r)
            wr32(pb + UInt32(FSRefParam_whichInfo_off), 0x4000)
            wr32(pb + UInt32(FSRefParam_catInfo_off), ci)
            wr16(pb + UInt32(FSRefParam_ioResult_off), 1)
            #expect(run([0x207C, hi(pb), lo(pb), 0x7076, 0xA260, 0x3E00, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            #expect(rd16(pb + UInt32(FSRefParam_ioResult_off)) == 0)
            #expect(get64(ci + UInt32(FSCatalogInfo_dataLogicalSize_off)) == 6)

            // PBOpenForkSync ($A260, $79), then PBReadForkSync ($A2A8, $51).
            let fpb = ptr(UInt32(FSForkIOParam_SIZEOF)), buf = ptr(8)
            wr32(fpb + UInt32(FSForkIOParam_ref_off), r)
            wr8(fpb + UInt32(FSForkIOParam_permissions_off), 1)
            #expect(run([0x207C, hi(fpb), lo(fpb), 0x7079, 0xA260, 0x3E00, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            wr32(fpb + UInt32(FSForkIOParam_buffer_off), buf)
            wr32(fpb + UInt32(FSForkIOParam_requestCount_off), 3)
            wr16(fpb + UInt32(FSForkIOParam_positionMode_off), 1)
            put64(fpb + UInt32(FSForkIOParam_positionOffset_off), 2)
            #expect(run([0x207C, hi(fpb), lo(fpb), 0x7051, 0xA2A8, 0x3E00, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            #expect(rd32(fpb + UInt32(FSForkIOParam_actualCount_off)) == 3)
            #expect(read(buf, 3) == Array("cde".utf8))
            #expect(tb_FSCloseFork(Int16(bitPattern: rd16(fpb + UInt32(FSForkIOParam_forkRefNum_off)))) == 0)

            // FSGetResourceForkName: Pascal, MOVE.W #$0219,D0; _HighLevelFSDispatch.
            let name = ptr(512)
            #expect(run([0x4267, 0x2F3C, hi(name), lo(name), 0x303C, 0x0219, 0xAA52, 0x3E1F, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            #expect(uniString(name) == "RESOURCE_FORK")
            // FSSetForkPosition takes its 64-bit position by address.
            let rnp = ptr(2), pos = ptr(8)
            #expect(tb_FSOpenFork(r, 0, 0, 1, rnp) == 0)
            put64(pos, 5)
            #expect(run([0x4267, 0x3F3C, rd16(rnp), 0x3F3C, 0x0001, 0x2F3C, hi(pos), lo(pos),
                         0x303C, 0x042C, 0xAA52, 0x3E1F, 0xA9F4]) == "ExitToShell")
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0)
            #expect(tb_FSGetForkPosition(Int16(bitPattern: rd16(rnp)), pos) == 0)
            #expect(get64(pos) == 5)
            #expect(tb_FSCloseFork(Int16(bitPattern: rd16(rnp))) == 0)
        }

        /// PowerPC passes a 64-bit value in two registers, high word first.
        @Test func powerPCExportsTakeSixtyFourBitValues() {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("ppc.dat")
            hostFile(file, "0123456789")
            let r = ref(file.path)
            func call(_ name: String, _ args: [UInt32]) -> Int16 {
                withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                    for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
                }
                guard let fn = cfm_export_fn(name) else {
                    Issue.record("no export \(name)")
                    return 0
                }
                fn(&g_ppc)
                return Int16(truncatingIfNeeded: withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) })
            }
            let rnp = ptr(2), actual = ptr(4), buf = ptr(8), pos = ptr(8)
            #expect(call("FSOpenFork", [r, 0, 0, 3, rnp]) == 0)
            let rn = UInt32(rd16(rnp))
            #expect(call("FSReadFork", [rn, 1, 0, 7, 2, buf, actual]) == 0)
            #expect(read(buf, 2) == Array("78".utf8))
            #expect(call("FSWriteFork", [rn, 1, 0, 1, 3, bytes(Array("abc".utf8)), actual]) == 0)
            #expect(call("FSSetForkPosition", [rn, 1, 0, 3]) == 0)
            #expect(call("FSGetForkPosition", [rn, pos]) == 0)
            #expect(get64(pos) == 3)
            #expect(call("FSSetForkSize", [rn, 1, 0, 6]) == 0)
            let req = ptr(8)
            #expect(call("FSAllocateFork", [rn, 0, 0, 0, 0, 0, 100, req]) == 0)
            #expect(get64(req) == 100)
            #expect(call("FSCloseFork", [rn]) == 0)
            #expect(String(decoding: (try? Data(contentsOf: file)) ?? Data(), as: UTF8.self) == "0abc45")
            // The parameter block forms set ioResult.
            let pb = ptr(UInt32(FSRefParam_SIZEOF)), name = ptr(512)
            wr32(pb + UInt32(FSRefParam_ref_off), r)
            wr32(pb + UInt32(FSRefParam_outName_off), name)
            wr16(pb + UInt32(FSRefParam_ioResult_off), 1)
            #expect(call("PBGetCatalogInfoSync", [pb]) == 0)
            #expect(rd16(pb + UInt32(FSRefParam_ioResult_off)) == 0)
            #expect(uniString(name) == "ppc.dat")
            for export in ["PBOpenImmed", "PBCloseImmed", "PBReadImmed", "PBWriteImmed", "PBStatusImmed",
                           "PBKillIOImmed", "PBUnmountVolImmed", "PBAllocContigSync", "AllocContig",
                           "PBHTrashVolumeCachesSync", "PBWaitIOComplete", "FSpResourceFileAlreadyOpen",
                           "ResolveAliasFileWithMountFlags", "FolderManagerRegisterCallNotificationProcs"] {
                #expect(cfm_export_fn(export) != nil, "\(export)")
            }
        }
    }
}
