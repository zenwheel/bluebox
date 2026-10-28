import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The File System Manager's HFS utilities (FSM.h) on the File
    /// Manager's FCB table, volumes, drives and working directories.
    @Suite struct OS9RestFSM {
        func openFile(_ dir: URL, _ name: String, perm: Int8 = 3, bytes: Int = 1500) throws -> UInt32 {
            let file = dir.appendingPathComponent(name)
            try Data((0..<bytes).map { UInt8($0 & 0xFF) }).write(to: file)
            let spec = Rest.ptr(Int(FSSpec_SIZEOF))
            fm_spec_for_path(file.path, spec)
            let r = Rest.ptr(80), rnp = Rest.ptr(2)
            #expect(tb_FSpMakeFSRef(spec, r) == 0)
            #expect(tb_FSOpenFork(r, 0, 0, perm, rnp) == 0)
            return UInt32(rd16(rnp))
        }

        func errs(_ name: String, _ args: [UInt32]) -> Int16 { Int16(truncatingIfNeeded: Rest.call(name, args)) }

        @Test func fileControlBlocks() throws {
            let dir = launchTestApp()
            let rn = Rest.ptr(2), fcb = Rest.ptr(4)
            #expect(errs("UTAllocateFCB", [rn, fcb]) == 0)
            let mine = UInt32(rd16(rn))
            #expect(mine >= 2 && rd32(fcb) != 0)
            // a claimed slot is not handed to a file that opens
            let open = try openFile(dir, "a.bin")
            #expect(open != mine)
            #expect(errs("UTResolveFCB", [mine, fcb]) == 0 && rd32(fcb) == rd32(fcb))
            let back = Rest.ptr(2)
            #expect(errs("UTResolveFileRefNum", [rd32(fcb), back]) == 0 && UInt32(rd16(back)) == mine)
            #expect(errs("UTResolveFileRefNum", [rd32(fcb) + 3, back]) == -51)
            #expect(errs("UTCheckFCB", [rd32(fcb)]) == 0 && errs("UTCheckFCB", [12]) == -51)
            #expect(errs("UTReleaseFCB", [mine]) == 0)
            #expect(errs("UTResolveFCB", [mine, fcb]) == -51)
            #expect(errs("UTReleaseFCB", [mine]) == -51)

            // an open file's FCB, by number
            #expect(errs("UTResolveFCB", [open, fcb]) == 0)
            let f = rd32(fcb), number = rd32(f)
            #expect(number != 0)
            let found = Rest.ptr(2), foundFCB = Rest.ptr(4)
            #expect(errs("UTLocateFCB", [0, number, 0, found, foundFCB]) == 0)
            #expect(UInt32(rd16(found)) == open && rd32(foundFCB) == f)
            #expect(errs("UTLocateFCB", [0, number + 7, 0, found, foundFCB]) == -43)
            let vcb = rd32(f + 20)
            #expect(errs("UTLocateFCB", [vcb, number, f + 62, found, foundFCB]) == 0) // by name too
            #expect(errs("UTLocateFCB", [vcb + 4, number, 0, found, foundFCB]) == -43) // another volume's
            wr16(found, UInt16(open))
            #expect(errs("UTLocateNextFCB", [0, number, 0, found, foundFCB]) == -43)  // no other
            // walking the volume's FCBs
            wr16(found, 0)
            var seen = [UInt32]()
            while errs("UTIndexFCB", [vcb, found, foundFCB]) == 0 { seen.append(UInt32(rd16(found))) }
            #expect(seen.contains(open))
            #expect(Rest.signed("UTGetForkControlBlockSize", [found]) == 0 && rd32(found) == 94)
            #expect(errs("UTAdjustEOF", [open]) == 0 && errs("UTAdjustEOF", [0]) == -51)
            // released: closes the fork
            #expect(errs("UTReleaseFCB", [open]) == 0)
            #expect(errs("UTCheckFileRefNum", [open]) == -51)
        }

        @Test func searchList() throws {
            let dir = launchTestApp()
            let open = try openFile(dir, "s.bin")
            let rn = Rest.ptr(2), fcb = Rest.ptr(4)
            #expect(errs("UTResolveFCB", [open, fcb]) == 0)
            let f = rd32(fcb), number = rd32(f), vcb = rd32(f + 20)
            let found = Rest.ptr(2), foundFCB = Rest.ptr(4)
            // the File Manager's own open files are in it already
            #expect(errs("UTLocateFCBInSearchList", [vcb, number, found, foundFCB]) == 0 && UInt32(rd16(found)) == open)
            // a claimed one is when it is added
            #expect(errs("UTAllocateFCB", [rn, fcb]) == 0)
            wr32(rd32(fcb), number + 1)
            wr32(rd32(fcb) + 20, vcb)
            #expect(errs("UTLocateFCBInSearchList", [vcb, number + 1, found, foundFCB]) == -43)
            #expect(errs("UTAddFCBToSearchList", [rd32(fcb)]) == 0)
            #expect(errs("UTLocateFCBInSearchList", [vcb, number + 1, found, foundFCB]) == 0 && UInt32(rd16(found)) == UInt32(rd16(rn)))
            #expect(errs("UTRemoveFCBFromSearchList", [rd32(fcb)]) == 0)
            #expect(errs("UTLocateFCBInSearchList", [vcb, number + 1, found, foundFCB]) == -43)
            #expect(errs("UTAddFCBToSearchList", [33]) == -51)
        }

        @Test func volumesDrivesAndWorkingDirectories() throws {
            let dir = launchTestApp()
            let vref = Rest.ptr(2), vcb = Rest.ptr(4)
            #expect(errs("UTLocateVCBByRefNum", [0, vref, vcb]) == 0)
            let main = rd32(vcb)
            #expect(main != 0 && main == rd32(UInt32(LM_DefVCBPtr)))
            let id = Int16(bitPattern: rd16(vref))
            #expect(errs("UTLocateVCBByRefNum", [UInt32(UInt16(bitPattern: id)), vref, vcb]) == 0 && rd32(vcb) == main)
            #expect(errs("UTLocateVCBByRefNum", [1, vref, vcb]) == 0 && rd32(vcb) == main) // the drive's number
            #expect(errs("UTLocateVCBByRefNum", [UInt32(UInt16(bitPattern: -999)), vref, vcb]) == -35)
            #expect(errs("UTLocateVCBByRefNum", [77, vref, vcb]) == -35)
            // by name, and the case does not matter
            let name = Rest.ptr(40), more = Rest.ptr(2)
            for i in 0...Int(rd8(main + 44)) { wr8(name + UInt32(i), rd8(main + 44 + UInt32(i))) }
            wr8(name + 1, rd8(name + 1) | 0x20)
            #expect(errs("UTLocateVCBByName", [name, more, vref, vcb]) == 0 && rd32(vcb) == main && rd16(more) == 0)
            #expect(errs("UTLocateNextVCB", [name, more, vref, vcb]) == -35) // no second of the name
            guestPString("No Such Disk", at: name)
            #expect(errs("UTLocateVCBByName", [name, more, vref, vcb]) == -35)
            wr8(name, 0)
            #expect(errs("UTLocateVCBByName", [name, more, vref, vcb]) == -37)

            // a drive
            let dq = Rest.ptr(4)
            #expect(errs("UTFindDrive", [1, dq]) == 0 && rd16(rd32(dq) + 6) == 1)
            #expect(errs("UTFindDrive", [9, dq]) == -56)
            #expect(errs("UTEjectVol", [main]) == -17) // a fixed disk
            #expect(errs("UTEjectVol", [main + 1]) == -35)

            // a working directory
            let sub = dir.appendingPathComponent("wdir")
            try FileManager.default.createDirectory(at: sub, withIntermediateDirectories: true)
            let spec = Rest.ptr(Int(FSSpec_SIZEOF))
            fm_spec_for_path(sub.path, spec)
            let pb = Rest.ptr(128)
            wr16(pb + UInt32(IOParam_ioVRefNum_off), rd16(spec))
            wr32(pb + UInt32(WDPBRec_ioWDDirID_off), rd32(spec + 2))
            wr32(pb + UInt32(IOParam_ioNamePtr_off), 0)
            #expect(errs("UTAllocateWDCB", [pb]) == 0)
            let wd = UInt32(rd16(pb + UInt32(IOParam_ioVRefNum_off)))
            #expect(Int16(bitPattern: UInt16(wd)) <= -32000)
            #expect(errs("UTCheckWDRefNum", [wd]) == 0)
            #expect(errs("UTLocateVCBByRefNum", [wd, vref, vcb]) == 0 && rd32(vcb) == main)
            let wdcb = Rest.ptr(4)
            #expect(errs("UTResolveWDCB", [9, 0, wd, wdcb]) == 0)
            #expect(rd32(rd32(wdcb)) == main && rd32(rd32(wdcb) + 4) == rd32(spec + 2) && rd32(rd32(wdcb) + 12) == 9)
            #expect(errs("UTResolveWDCB", [0, 1, 0, wdcb]) == 0) // the first one in use
            // a directory with a working directory on it is busy
            #expect(errs("UTCheckDirBusy", [main, rd32(spec + 2)]) == -47)
            #expect(errs("UTCheckDirBusy", [main, 0x7777]) == 0)
            #expect(errs("UTReleaseWDCB", [wd]) == 0)
            #expect(errs("UTCheckWDRefNum", [wd]) == -35)
            #expect(errs("UTReleaseWDCB", [wd]) == -35)
            #expect(errs("UTCheckDirBusy", [main, rd32(spec + 2)]) == 0)

            // the default volume
            wr32(pb + UInt32(IOParam_ioNamePtr_off), 0)
            #expect(errs("UTGetDefaultVol", [pb]) == 0)
            #expect(errs("UTSetDefaultVol", [0, rd32(spec + 2), UInt32(rd16(spec))]) == 0)
        }

        @Test func addingAndDisposingVolumes() {
            launchTestApp()
            let len = Rest.ptr(2), vcbp = Rest.ptr(4), vref = Rest.ptr(2)
            #expect(errs("UTAllocateVCB", [len, vcbp, 40]) == 0 && rd16(len) == UInt16(VCB_SIZEOF))
            let v = rd32(vcbp)
            guestPString("Plug-in Disk", at: v + 44)
            wr32(v + 10, 0x1234)
            #expect(errs("UTAddNewVCB", [5, vref, v]) == 0)
            let id = rd16(vref)
            #expect(Int16(bitPattern: id) < -1)
            let got = Rest.ptr(4), got2 = Rest.ptr(2)
            #expect(errs("UTLocateVCBByRefNum", [UInt32(id), got2, got]) == 0 && rd32(got) == v)
            #expect(errs("UTLocateVCBByRefNum", [5, got2, got]) == 0 && rd32(got) == v) // by its drive
            #expect(errs("UTCheckVolRefNum", [UInt32(id)]) == 0 && errs("UTCheckVolModifiable", [UInt32(id)]) == 0)
            // the same name and date is the same volume
            #expect(errs("UTAllocateVCB", [len, vcbp, 0]) == 0)
            let v2 = rd32(vcbp)
            guestPString("PLUG-IN DISK", at: v2 + 44)
            wr32(v2 + 10, 0x1234)
            #expect(errs("UTAddNewVCB", [6, vref, v2]) == -55)
            wr32(v2 + 10, 0x1235)
            #expect(errs("UTAddNewVCB", [6, vref, v2]) == 0)
            let name = Rest.ptr(32), more = Rest.ptr(2)
            guestPString("plug-in disk", at: name)
            #expect(errs("UTLocateVCBByName", [name, more, got2, got]) == 0 && rd32(got) == v && rd16(more) == 1)
            #expect(errs("UTLocateNextVCB", [name, more, got2, got]) == 0 && rd32(got) == v2 && rd16(more) == 0)
            #expect(errs("UTDisposeVCB", [v]) == 0 && errs("UTDisposeVCB", [v]) == -35)
            #expect(errs("UTDisposeVCB", [v2]) == 0)
            #expect(errs("UTLocateVCBByRefNum", [UInt32(id), got2, got]) == -35)
            // the startup volume stays
            let vref0 = Rest.ptr(2)
            #expect(errs("UTLocateVCBByRefNum", [0, vref0, got]) == 0)
            #expect(errs("UTDisposeVCB", [rd32(got)]) == -55)
            #expect(errs("UTAddNewVCB", [0, vref, v2]) == -50)
        }

        @Test func permissions() throws {
            let dir = launchTestApp()
            let open = try openFile(dir, "p.bin", perm: 3)
            let fcb = Rest.ptr(4)
            #expect(errs("UTResolveFCB", [open, fcb]) == 0)
            let f = rd32(fcb), number = rd32(f), vcb = rd32(f + 20)
            let mod = Rest.ptr(2)
            func check(_ asked: UInt16) -> Int16 {
                wr16(mod, asked)
                return errs("UTCheckPermission", [vcb, mod, number, 0])
            }
            #expect(check(1) == 0 && rd16(mod) == 1)      // reading is fine beside a writer
            #expect(check(2) == -49)                       // writing is not
            #expect(check(3) == -49)
            #expect(check(0) == 0 && rd16(mod) == 1)       // the current permission: read only
            #expect(check(9) == -50)
            // another file has no writer
            wr16(mod, 3)
            #expect(errs("UTCheckPermission", [vcb, mod, number + 99, 0]) == 0 && rd16(mod) == 3)

            let flags = Rest.ptr(1), same = Rest.ptr(2)
            func fork(_ number: UInt32, id: UInt32, locked: Bool, perm: Int8, search: Bool = false) -> Int16 {
                errs("UTCheckForkPermissions", [vcb, number, id, locked ? 1 : 0, UInt32(UInt8(bitPattern: perm)), search ? 1 : 0, flags, same])
            }
            #expect(fork(number, id: 0, locked: false, perm: 1) == 0 && UInt32(rd16(same)) == open && rd8(flags) == 0)
            #expect(fork(number, id: 0, locked: false, perm: 3) == -49)
            #expect(fork(number, id: 0xFF, locked: false, perm: 3) == 0 && rd16(same) == 0) // the resource fork is another
            #expect(rd8(flags) == 0x03)                    // write, resource
            #expect(fork(number + 5, id: 0, locked: true, perm: 2) == -54)
            #expect(fork(number + 5, id: 0, locked: true, perm: 1) == 0 && rd8(flags) == 0x20)
            #expect(fork(number + 5, id: 0, locked: false, perm: 4) == 0 && rd8(flags) == 0x11)
            #expect(fork(number + 5, id: 0, locked: false, perm: 0) == 0 && rd8(flags) == 0x01)
            #expect(fork(number, id: 0, locked: false, perm: 0) == 0 && rd8(flags) == 0)   // beside a writer: read
        }

        @Test func pathNames() {
            launchTestApp()
            let name = Rest.ptr(64), len = Rest.ptr(2)
            guestPString("Disk:folder:file", at: name)
            #expect(errs("UTParsePathname", [len, name]) == 0 && rd16(len) == 4)
            guestPString(":folder:file", at: name)
            #expect(errs("UTParsePathname", [len, name]) == 0 && rd16(len) == 0)
            guestPString("file", at: name)
            #expect(errs("UTParsePathname", [len, name]) == 0 && rd16(len) == 0)
            guestPString("Disk:", at: name)
            #expect(errs("UTParsePathname", [len, name]) == 0 && rd16(len) == 4)
            guestPString("A Disk Name That Is Far Too Long:x", at: name)
            #expect(errs("UTParsePathname", [len, name]) == -37)

            let rec = Rest.ptr(12)
            guestPString("Disk:folder:file", at: name)
            wr32(rec, name)
            func component(_ start: UInt16) -> (UInt16, UInt8, UInt8) {
                wr16(rec + 4, start)
                #expect(errs("UTGetPathComponentName", [rec]) == 0)
                return (rd16(rec + 6), rd8(rec + 8), rd8(rec + 9))
            }
            #expect(component(0) == (4, 1, 1))   // "Disk", a colon, more after
            #expect(component(5) == (6, 1, 1))   // "folder"
            #expect(component(12) == (4, 0, 0))  // "file", the end
            #expect(errs("UTGetPathComponentName", [0]) == -50)
        }

        @Test func determiningTheVolume() {
            launchTestApp()
            let status = Rest.ptr(2), more = Rest.ptr(2), vref = Rest.ptr(2), vcb = Rest.ptr(4)
            let pb = Rest.ptr(128), name = Rest.ptr(64)
            // the default volume
            #expect(errs("UTDetermineVol", [pb, status, more, vref, vcb]) == 0 && rd16(status) == 5)
            let main = rd32(vcb)
            // a volume reference, a drive number
            wr16(pb + UInt32(IOParam_ioVRefNum_off), rd16(vref))
            #expect(errs("UTDetermineVol", [pb, status, more, vref, vcb]) == 0 && rd16(status) == 2 && rd32(vcb) == main)
            wr16(pb + UInt32(IOParam_ioVRefNum_off), 1)
            #expect(errs("UTDetermineVol", [pb, status, more, vref, vcb]) == 0 && rd16(status) == 4)
            wr16(pb + UInt32(IOParam_ioVRefNum_off), 0xFFFF - 998) // an unknown volume
            #expect(errs("UTDetermineVol", [pb, status, more, vref, vcb]) == -35 && rd16(status) == 0)
            // a full path name
            wr16(pb + UInt32(IOParam_ioVRefNum_off), 0)
            let n = Int(rd8(main + 44))
            wr8(name, UInt8(n + 6))
            for i in 1...n { wr8(name + UInt32(i), rd8(main + 44 + UInt32(i))) }
            for (i, ch) in ":a:b:c".utf8.enumerated() { wr8(name + UInt32(n + 1 + i), ch) }
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(errs("UTDetermineVol", [pb, status, more, vref, vcb]) == 0 && rd16(status) == 1 && rd32(vcb) == main)
            guestPString("Nowhere:x", at: name)
            #expect(errs("UTDetermineVol", [pb, status, more, vref, vcb]) == -35 && rd16(status) == 0)
            // a partial path falls to the default
            guestPString(":x:y", at: name)
            #expect(errs("UTDetermineVol", [pb, status, more, vref, vcb]) == 0 && rd16(status) == 5)
            #expect(errs("UTDetermineVol", [0, status, more, vref, vcb]) == -50)
        }

        @Test func theCacheOfAFork() throws {
            let dir = launchTestApp()
            let open = try openFile(dir, "c.bin", perm: 3, bytes: 1500)
            let buf = Rest.ptr(4), buf2 = Rest.ptr(4)
            #expect(errs("UTGetBlock", [open, 0, 1, 0, buf]) == 0)
            let b = rd32(buf)
            #expect(b != 0 && rd8(b) == UInt8(512 & 0xFF) && rd8(b + 1) == UInt8(513 & 0xFF)) // block 1 of the file
            #expect(errs("UTGetBlock", [open, 0, 1, 0, buf2]) == 2)                 // chInUse
            wr8(b, 0xEE)
            #expect(errs("UTMarkDirty", [b]) == 0)
            #expect(errs("UTReleaseBlock", [b, 0]) == 0)
            #expect(errs("UTReleaseBlock", [b, 0]) == 4)                              // chNotInUse
            #expect(errs("UTGetBlock", [open, 0, 1, 2, buf2]) == 0 && rd32(buf2) == b) // gbExist: still there
            #expect(errs("UTReleaseBlock", [b, 1]) == 0)                              // write it
            let into = Rest.ptr(4), got = Rest.ptr(4)
            #expect(errs("UTCacheReadIP", [0, 512, into, open, 4, got, 0]) == 0 && rd32(got) == 4 && rd8(into) == 0xEE)
            #expect(errs("UTGetBlock", [open, 0, 77, 2, buf2]) == 3)                  // gbExist: not there
            // the last, short block: bytes past the end of file stay as they were
            #expect(errs("UTGetBlock", [open, 0, 2, 0, buf2]) == 0)
            let last = rd32(buf2)
            #expect(rd8(last + 475) == UInt8(1499 & 0xFF) && rd8(last + 476) == 0)    // 1500 - 1024 = 476 bytes
            wr8(last + 100, 0x42)
            #expect(errs("UTReleaseBlock", [last, 5]) == 0)                           // dirty, write
            #expect(errs("UTCacheReadIP", [0, 1124, into, open, 1, got, 0]) == 0 && rd8(into) == 0x42)
            // flushing and trashing
            #expect(errs("UTGetBlock", [open, 0, 0, 8, buf2]) == 0)                   // get and release at once
            #expect(errs("UTFlushCache", [open, 1]) == 0)
            #expect(errs("UTGetBlock", [open, 0, 0, 2, buf2]) == 3)                   // trashed
            #expect(errs("UTGetBlock", [open, 0, 0, 8, buf2]) == 0)
            #expect(errs("UTTrashBlocks", [0, 100, 0, open, 0]) == 0)
            #expect(errs("UTGetBlock", [open, 0, 0, 2, buf2]) == 3)
            #expect(errs("UTBlockInFQHashP", [UInt32(UInt16(bitPattern: -1)), 5]) == 3)
            // writing through
            wr8(into, 0x99)
            #expect(errs("UTCacheWriteIP", [0, 8, into, open, 1, got, 0]) == 0 && rd32(got) == 1)
            let again = Rest.ptr(2)
            #expect(errs("UTCacheReadIP", [0, 8, again, open, 1, got, 0]) == 0 && rd8(again) == 0x99)
            // no blocks for a volume
            let fcb = Rest.ptr(4)
            #expect(errs("UTResolveFCB", [open, fcb]) == 0)
            let vcb = rd32(rd32(fcb) + 20)
            #expect(errs("UTGetBlock", [UInt32(UInt16(bitPattern: -1)), 0, 0, 0, buf2]) == -53)
            #expect(errs("UTVolCacheReadIP", [vcb, 0, into, 512, got, 0]) == -53 && rd32(got) == 0)
            #expect(errs("UTVolCacheWriteIP", [vcb, 0, into, 512, got, 0]) == -53)
            #expect(errs("UTVolCacheReadIP", [vcb + 2, 0, into, 512, got, 0]) == -35)
            #expect(errs("UTTrashVolBlocks", [vcb]) == 0 && errs("UTTrashFileBlocks", [vcb, 5]) == 0)
        }
    }
}
