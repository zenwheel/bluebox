import CCore
import Foundation
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// The Desktop Manager: icons from applications' bundles and those
    /// programs add, applications by creator, and comments.
    @Suite struct DesktopDatabase {
        let afpItemNotFound: Int16 = -5018

        func handle(_ b: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(b.count), false, false)
            for (i, x) in b.enumerated() { wr8(rd32(h) + UInt32(i), x) }
            return h
        }

        func os(_ s: String) -> Int32 { Int32(bitPattern: fourCC(s)) }

        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func guestBytes(_ b: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        func read(_ p: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { rd8(p + UInt32($0)) } }

        /// A recognisable icon of `size` bytes.
        func pattern(_ size: Int, _ seed: UInt8) -> [UInt8] { (0..<size).map { UInt8(truncatingIfNeeded: $0) &+ seed } }

        /// A parameter block with the database's refnum, for a creator, type and kind.
        func dtpb(creator: String, type: String = "\0\0\0\0", kind: UInt8 = 0) -> UInt32 {
            let dtp = tb_NewPtr(Int32(DTPBRec_SIZEOF), false, true)
            #expect(tb_PBDTGetPath(dtp) == 0)
            wr32(dtp + UInt32(DTPBRec_ioFileCreator_off), fourCC(creator))
            wr32(dtp + UInt32(DTPBRec_ioFileType_off), fourCC(type))
            wr8(dtp + UInt32(DTPBRec_ioIconType_off), kind)
            return dtp
        }

        func getIcon(_ creator: String, _ type: String, kind: UInt8, size: Int) -> (Int16, [UInt8], UInt32) {
            let dtp = dtpb(creator: creator, type: type, kind: kind)
            let buf = tb_NewPtr(1024, false, true)
            wr32(dtp + UInt32(DTPBRec_ioDTBuffer_off), buf)
            wr32(dtp + UInt32(DTPBRec_ioDTReqCount_off), 1024)
            let err = tb_PBDTGetIcon(dtp, false)
            let n = Int(rd32(dtp + UInt32(DTPBRec_ioDTActCount_off)))
            #expect(err != 0 || n == size)
            return (err, read(buf, n), rd32(dtp + UInt32(DTPBRec_ioTagInfo_off)))
        }

        /// An application whose bundle gives its documents an ICN# and an
        /// icl8 (ID 129) and itself an ICN# and ics# (ID 128).
        func makeApplication() {
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Bundled"), spec)
            tb_FSpCreateResFile(spec, os("DtDb"), os("APPL"), 0)
            let rn = tb_FSpOpenResFile(spec, 3)
            #expect(rn > 0)
            let bndl = cat(Array("DtDb".utf8), be16(0), be16(1),
                           Array("ICN#".utf8), be16(1), be16(0), be16(128), be16(1), be16(129),
                           Array("FREF".utf8), be16(1), be16(0), be16(128), be16(1), be16(129))
            tb_AddResource(handle(bndl), os("BNDL"), 128, 0)
            tb_AddResource(handle(cat(Array("APPL".utf8), be16(0), [0])), os("FREF"), 128, 0)
            tb_AddResource(handle(cat(Array("DtDc".utf8), be16(1), [0])), os("FREF"), 129, 0)
            tb_AddResource(handle(pattern(256, 1)), os("ICN#"), 128, 0)
            tb_AddResource(handle(pattern(64, 2)), os("ics#"), 128, 0)
            tb_AddResource(handle(pattern(256, 3)), os("ICN#"), 129, 0)
            tb_AddResource(handle(pattern(1024, 4)), os("icl8"), 129, 0)
            tb_CloseResFile(rn)
        }

        @Test func iconsComeFromBundles() {
            launchTestApp()
            makeApplication()
            var (err, icon, tag) = getIcon("DtDb", "DtDc", kind: 1, size: 256)
            #expect(err == 0 && icon == pattern(256, 3) && tag == 0)
            (err, icon, _) = getIcon("DtDb", "DtDc", kind: 3, size: 1024)
            #expect(err == 0 && icon == pattern(1024, 4))
            (err, icon, _) = getIcon("DtDb", "APPL", kind: 4, size: 64)
            #expect(err == 0 && icon == pattern(64, 2))
            #expect(getIcon("DtDb", "DtDc", kind: 2, size: 0).0 == afpItemNotFound)
            #expect(getIcon("None", "APPL", kind: 1, size: 0).0 == afpItemNotFound)

            // A short buffer takes as much as it holds.
            let dtp = dtpb(creator: "DtDb", type: "APPL", kind: 1)
            let buf = tb_NewPtr(16, false, true)
            wr32(dtp + UInt32(DTPBRec_ioDTBuffer_off), buf)
            wr32(dtp + UInt32(DTPBRec_ioDTReqCount_off), 16)
            #expect(tb_PBDTGetIcon(dtp, false) == 0)
            #expect(rd32(dtp + UInt32(DTPBRec_ioDTActCount_off)) == 16 && read(buf, 16) == Array(pattern(256, 1)[0..<16]))

            // Every icon the creator has, by index.
            var found: [String] = []
            for i in 1... {
                let info = dtpb(creator: "DtDb")
                wr16(info + UInt32(DTPBRec_ioIndex_off), UInt16(i))
                let e = tb_PBDTGetIconInfo(info, false)
                if e != 0 {
                    #expect(e == afpItemNotFound)
                    break
                }
                let type = String(decoding: be32(rd32(info + UInt32(DTPBRec_ioFileType_off))), as: UTF8.self)
                found.append("\(type) \(rd8(info + UInt32(DTPBRec_ioIconType_off))) \(rd32(info + UInt32(DTPBRec_ioDTActCount_off)))")
            }
            #expect(found.sorted() == ["APPL 1 256", "APPL 4 64", "DtDc 1 256", "DtDc 3 1024"])

            // The application by its creator.
            let appl = dtpb(creator: "DtDb")
            let name = tb_NewPtr(64, false, true)
            wr32(appl + UInt32(DTPBRec_ioNamePtr_off), name)
            #expect(tb_PBDTGetAPPL(appl, false) == 0 && pstring(at: name) == "Bundled")
        }

        @Test func addedIconsAreReturned() {
            launchTestApp()
            makeApplication()
            func add(_ type: String, kind: UInt8, _ data: [UInt8], tag: UInt32) -> Int16 {
                let dtp = dtpb(creator: "DtDb", type: type, kind: kind)
                wr32(dtp + UInt32(DTPBRec_ioDTBuffer_off), guestBytes(data))
                wr32(dtp + UInt32(DTPBRec_ioDTReqCount_off), UInt32(data.count))
                wr32(dtp + UInt32(DTPBRec_ioTagInfo_off), tag)
                return tb_PBDTAddIcon(dtp, false)
            }
            #expect(add("Xtra", kind: 5, pattern(128, 9), tag: 7) == 0)
            var (err, icon, tag) = getIcon("DtDb", "Xtra", kind: 5, size: 128)
            #expect(err == 0 && icon == pattern(128, 9) && tag == 7)
            // In place of the bundle's.
            #expect(add("APPL", kind: 1, pattern(256, 20), tag: 0) == 0)
            (err, icon, _) = getIcon("DtDb", "APPL", kind: 1, size: 256)
            #expect(err == 0 && icon == pattern(256, 20))
            // The size must be the kind's.
            #expect(add("Xtra", kind: 1, pattern(100, 0), tag: 0) == -5030)
        }

        @Test func commentsAreKept() {
            let dir = launchTestApp()
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Noted"), spec)
            #expect(tb_FSpCreate(spec, os("DtDb"), os("DtDc"), 0) == 0)
            let parID = rd32(spec + UInt32(FSSpec_parID_off))
            func call(_ fn: (UInt32, Bool) -> Int16, _ name: String, _ text: String? = nil) -> (Int16, String) {
                let dtp = dtpb(creator: "DtDb")
                wr32(dtp + UInt32(DTPBRec_ioNamePtr_off), pname(name))
                wr32(dtp + UInt32(DTPBRec_ioDirID_off), parID)
                let buf = text.map { guestBytes(Array($0.utf8)) } ?? tb_NewPtr(256, false, true)
                wr32(dtp + UInt32(DTPBRec_ioDTBuffer_off), buf)
                wr32(dtp + UInt32(DTPBRec_ioDTReqCount_off), UInt32(text?.utf8.count ?? 256))
                let err = fn(dtp, false)
                let n = text == nil ? Int(rd32(dtp + UInt32(DTPBRec_ioDTActCount_off))) : 0
                return (err, String(decoding: read(buf, n), as: UTF8.self))
            }
            #expect(call(tb_PBDTGetComment, "Noted").0 == afpItemNotFound)
            #expect(call(tb_PBDTSetComment, "Noted", "Hello there").0 == 0)
            #expect(call(tb_PBDTGetComment, "Noted") == (0, "Hello there"))
            // In the file's sidecar, for the next run.
            var f = mac_file()
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_read(dir.appendingPathComponent("Noted").path, &f, &err, err.count))
            #expect(f.comment_len == 11 && mac_file_type(&f) == fourCC("DtDc"))
            mac_file_free(&f)
            #expect(call(tb_PBDTRemoveComment, "Noted").0 == 0)
            #expect(call(tb_PBDTGetComment, "Noted").0 == afpItemNotFound)
            // Cut to 200 characters.
            #expect(call(tb_PBDTSetComment, "Noted", String(repeating: "x", count: 250)).0 == 0)
            #expect(call(tb_PBDTGetComment, "Noted") == (0, String(repeating: "x", count: 200)))

            // A plain host text file takes a sidecar for its comment.
            try? Data("text".utf8).write(to: dir.appendingPathComponent("plain.txt"))
            #expect(call(tb_PBDTSetComment, "plain.txt", "Notes").0 == 0)
            #expect(call(tb_PBDTGetComment, "plain.txt") == (0, "Notes"))
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("._plain.txt").path))
            #expect(call(tb_PBDTRemoveComment, "plain.txt").0 == 0)
            #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("._plain.txt").path))

            // A folder's.
            let fspec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true), dirID = tb_NewPtr(4, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Folder"), fspec)
            #expect(tb_FSpDirCreate(fspec, 0, dirID) == 0)
            #expect(call(tb_PBDTSetComment, "Folder", "A folder").0 == 0)
            #expect(call(tb_PBDTGetComment, "Folder") == (0, "A folder"))
            #expect(call(tb_PBDTGetComment, "Missing").0 == -43)
        }
    }
}
