import CCore
import ForkStore
import Foundation
import Testing
import Toolbox

/// A RAM disk driver of the kind a disk image's is: Prime reads and writes
/// the image at dCtlPosition, Control counts ejects (csCode 7).  Storage at
/// 0x100: the image's address, a write-protect word, the eject count.
private enum RamDisk {
    static let storage = 0x100
    static func blob() -> [UInt8] {
        var b = [UInt8](repeating: 0, count: 0x110)
        func put(_ at: Int, _ words: [Int]) {
            for (i, w) in words.enumerated() {
                b[at + 2 * i] = UInt8((w >> 8) & 0xFF)
                b[at + 2 * i + 1] = UInt8(w & 0xFF)
            }
        }
        put(0, [0x0F00, 0, 0, 0, 0x20, 0x60, 0xC0, 0x40, 0x30]) // read, write, control, status
        let name = Array(".RAMDisk".utf8)
        b[18] = UInt8(name.count)
        for (i, c) in name.enumerated() { b[19 + i] = c }
        let jio = [0x2678, 0x08FC, 0x4ED3] // MOVEA.L JIODone,A3; JMP (A3)
        put(0x20, [0x7000, 0x4E75])          // Open: MOVEQ #0,D0; RTS
        put(0x30, [0x7000, 0x4E75])          // Close
        put(0x40, [0x70EE] + jio)            // Status: statusErr
        // Prime: A3 = image + dCtlPosition, A4 = ioBuffer, D1 = ioReqCount;
        // a write (ioTrap's low byte 3) swaps them, unless write-protected.
        put(0x60, [0x45FA, storage - 0x62,   // LEA storage(PC),A2
                   0x2652,                   // MOVEA.L (A2),A3
                   0x2429, 0x0010,           // MOVE.L dCtlPosition(A1),D2
                   0xD7C2,                   // ADDA.L D2,A3
                   0x2228, 0x0024,           // MOVE.L ioReqCount(A0),D1
                   0x2868, 0x0020,           // MOVEA.L ioBuffer(A0),A4
                   0x0C28, 0x0002, 0x0007,   // CMPI.B #2,ioTrap+1(A0)
                   0x6708,                   // BEQ.S copy
                   0x4A6A, 0x0004,           // TST.W 4(A2)
                   0x6620,                   // BNE.S protected
                   0xC74C,                   // EXG A3,A4
                   0x6002,                   // copy: BRA.S test
                   0x18DB,                   // loop: MOVE.B (A3)+,(A4)+
                   0x5381,                   // test: SUBQ.L #1,D1
                   0x64FA,                   // BCC.S loop
                   0x2168, 0x0024, 0x0028,   // MOVE.L ioReqCount(A0),ioActCount(A0)
                   0x2228, 0x0024,           // MOVE.L ioReqCount(A0),D1
                   0xD3A9, 0x0010,           // ADD.L D1,dCtlPosition(A1)
                   0x7000] + jio +           // MOVEQ #0,D0
            [0x70D4] + jio)                  // protected: MOVEQ #wPrErr,D0
        // Control: csCode 7 (eject) counted, others controlErr.
        put(0xC0, [0x45FA, storage - 0xC2, 0x0C68, 0x0007, 0x001A, 0x660C, 0x526A, 0x0008, 0x7000] + jio +
            [0x70EF] + jio)
        return b
    }
}

extension BlueBox {
    /// Volumes on drives a program's driver serves: mounted through the
    /// driver's reads, written back through its writes.
    @Suite struct DriveVolumes {
        let files: [HFSTestFile] = [
            HFSTestFile(id: 16, dir: true, name: "Folder"),
            HFSTestFile(id: 17, name: "Read Me", data: Array("Hello from a disk.\r".utf8), rsrc: [1, 2, 3, 4, 5]),
            HFSTestFile(id: 18, parent: 16, name: "Inner", type: "APPL", creator: "abcd", data: Array(repeating: 7, count: 3000)),
        ]

        /// The driver installed and opened at unit 62, its disk at drive 9 in
        /// guest memory; returns the image's address and the driver's header.
        func attach(_ image: [UInt8], drive: Int16 = 9, locked: Bool = false) -> (image: UInt32, hdr: UInt32, el: UInt32) {
            let img = tb_NewPtr(Int32(image.count), true, false)
            for (i, b) in image.enumerated() where b != 0 { wr8(img + UInt32(i), b) }
            let hdr = tb_NewPtr(0x110, true, true)
            for (i, b) in RamDisk.blob().enumerated() { wr8(hdr + UInt32(i), b) }
            wr32(hdr + UInt32(RamDisk.storage), img)
            let refnum: Int16 = -63
            if tb_DriverInstall(hdr, refnum) != 0 { Issue.record("DriverInstall failed") }
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), true, true)
            let name = tb_NewPtr(16, false, true)
            guestPString(".RAMDisk", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == 0)
            let q = tb_NewPtr(Int32(4 + DrvQEl_SIZEOF), true, true)
            if locked { wr8(q, 0x80) }
            let el = q + 4
            tb_AddDrive(refnum, drive, el)
            wr16(el + UInt32(DrvQEl_dQDrvSz_off), UInt16(image.count / 512))
            return (img, hdr, el)
        }

        func mount(_ drive: Int16) -> (Int16, Int16) {
            let pb = tb_NewPtr(Int32(HParamBlockRec_SIZEOF), true, true)
            wr16(pb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: drive))
            let err = tb_PBMountVol(pb)
            return (err, Int16(bitPattern: rd16(pb + UInt32(IOParam_ioVRefNum_off))))
        }

        func vinfo(_ vref: Int16) -> (err: Int16, pb: UInt32, name: String) {
            let pb = tb_NewPtr(Int32(HParamBlockRec_SIZEOF), true, true)
            let name = tb_NewPtr(256, true, true)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            wr16(pb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: vref))
            let err = tb_PBHGetVInfo(pb, false)
            return (err, pb, pstring(at: name))
        }

        func volCall(_ call: (UInt32) -> Int16, _ vref: Int16) -> Int16 {
            let pb = tb_NewPtr(Int32(HParamBlockRec_SIZEOF), true, true)
            wr16(pb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: vref))
            return call(pb)
        }

        func readFile(_ vref: Int16, _ path: String) -> [UInt8]? {
            let spec = tb_NewPtr(70, true, true)
            let name = tb_NewPtr(256, true, true)
            guestPString(path, at: name)
            guard tb_FSMakeFSSpec(vref, 2, name, spec) == 0 else { return nil }
            let ref = tb_NewPtr(2, true, true)
            guard tb_FSpOpenDF(spec, 1, ref) == 0 else { return nil }
            let buf = tb_NewPtr(8192, true, true)
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), true, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), rd16(ref))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), 8192)
            let err = tb_PBRead(pb, false)
            let n = Int(rd32(pb + UInt32(IOParam_ioActCount_off)))
            _ = tb_PBClose(pb, false)
            guard err == 0 || err == -39 else { return nil }
            return (0..<n).map { rd8(buf + UInt32($0)) }
        }

        func writeFile(_ vref: Int16, _ path: String, _ bytes: [UInt8]) -> Int16 {
            let spec = tb_NewPtr(70, true, true)
            let name = tb_NewPtr(256, true, true)
            guestPString(path, at: name)
            _ = tb_FSMakeFSSpec(vref, 2, name, spec)
            let err = tb_FSpCreate(spec, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("TEXT")), 0)
            guard err == 0 else { return err }
            let ref = tb_NewPtr(2, true, true)
            guard tb_FSpOpenDF(spec, 3, ref) == 0 else { return -1 }
            let buf = tb_NewPtr(Int32(bytes.count), true, true)
            for (i, b) in bytes.enumerated() { wr8(buf + UInt32(i), b) }
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), true, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), rd16(ref))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), UInt32(bytes.count))
            let w = tb_PBWrite(pb, false)
            _ = tb_PBClose(pb, false)
            return w
        }

        func guestImage(_ at: UInt32, _ size: Int) -> [UInt8] { (0..<size).map { rd8(at + UInt32($0)) } }

        @Test func aDiskIsMountedThroughItsDriverAndItsFilesRead() throws {
            launchTestApp()
            let image = try #require(buildHFS(name: "Test Disk", size: 800 * 1024, files: files))
            _ = attach(image)
            let (err, vref) = mount(9)
            #expect(err == 0)
            #expect(vref < -1)
            #expect(mount(9).0 == -55) // volOnLinErr: mounted already
            let v = vinfo(vref)
            #expect(v.err == 0 && v.name == "Test Disk")
            #expect(rd16(v.pb + UInt32(HVolumeParam_ioVSigWord_off)) == 0x4244)
            #expect(rd16(v.pb + UInt32(HVolumeParam_ioVDrvInfo_off)) == 9)
            #expect(Int16(bitPattern: rd16(v.pb + UInt32(HVolumeParam_ioVDRefNum_off))) == -63)
            #expect(rd16(v.pb + UInt32(VolumeParam_ioVAtrb_off)) & 0x8080 == 0)
            #expect(rd32(v.pb + UInt32(HVolumeParam_ioVFilCnt_off)) == 2)
            #expect(rd32(v.pb + UInt32(HVolumeParam_ioVDirCnt_off)) == 1)
            #expect(readFile(vref, "Read Me") == Array("Hello from a disk.\r".utf8))
            #expect(readFile(vref, ":Folder:Inner") == Array(repeating: 7, count: 3000))
            // Its VCB is in the queue, with the drive's number and driver.
            var vcb = rd32(UInt32(LM_VCBQHdr) + 2), found = false
            while vcb != 0 {
                if Int16(bitPattern: rd16(vcb + UInt32(VCB_vcbVRefNum_off))) == vref {
                    found = rd16(vcb + UInt32(VCB_vcbDrvNum_off)) == 9
                }
                vcb = rd32(vcb)
            }
            #expect(found)
        }

        @Test func changesAreWrittenBackThroughTheDriver() throws {
            launchTestApp()
            let image = try #require(buildHFS(name: "Writable", size: 800 * 1024, files: files))
            let (img, hdr, _) = attach(image)
            let (err, vref) = mount(9)
            #expect(err == 0)
            #expect(writeFile(vref, "New Note", Array("written back".utf8)) == 0)
            #expect(volCall({ tb_PBFlushVol($0, false) }, vref) == 0)
            // The image in the driver's memory now holds the new file, and
            // the old ones are as they were.
            let after = try #require(readHFS(guestImage(img, image.count)))
            let note = after.first { $0.name == "New Note" }
            #expect(note?.data == Array("written back".utf8))
            #expect(note.map { withUnsafeBytes(of: $0.entry.finder_info) { Array($0[0..<8]) } } == Array("TEXTttxt".utf8))
            #expect(after.first { $0.name == "Read Me" }?.rsrc == [1, 2, 3, 4, 5])
            #expect(after.first { $0.name == "Inner" }?.data == Array(repeating: 7, count: 3000))
            // Put away and mounted again: the file is still there.
            #expect(volCall(tb_PBUnmountVol, vref) == 0)
            #expect(vinfo(vref).err == -35) // nsvErr
            let (again, vref2) = mount(9)
            #expect(again == 0)
            #expect(readFile(vref2, "New Note") == Array("written back".utf8))
            // Ejected: the driver is told.
            #expect(volCall(tb_PBEject, vref2) == 0)
            #expect(rd16(hdr + UInt32(RamDisk.storage + 8)) == 1)
            #expect(vinfo(vref2).err == -35)
        }

        @Test func aWriteProtectedDiskMountsLocked() throws {
            launchTestApp()
            let image = try #require(buildHFS(name: "Locked", size: 400 * 1024, files: files))
            let (img, _, _) = attach(image, locked: true)
            let (err, vref) = mount(9)
            #expect(err == 0)
            #expect(rd16(vinfo(vref).pb + UInt32(VolumeParam_ioVAtrb_off)) & 0x8080 != 0)
            #expect(readFile(vref, "Read Me") == Array("Hello from a disk.\r".utf8))
            #expect(writeFile(vref, "Nope", [1]) == -44) // wPrErr
            #expect(volCall({ tb_PBFlushVol($0, false) }, vref) == 0)
            #expect(guestImage(img, image.count) == image)
        }

        @Test func anUnreadableDiskIsEjectedAndCanBeInitialized() throws {
            launchTestApp()
            let (img, hdr, _) = attach([UInt8](repeating: 0, count: 400 * 1024))
            #expect(mount(9).0 == -57) // noMacDskErr
            #expect(tb_DIBadMount(0, Int32(bitPattern: UInt32(UInt16(bitPattern: -57)) << 16 | 9)) == 1)
            #expect(rd16(hdr + UInt32(RamDisk.storage + 8)) == 1)
            // DIZero writes an empty volume, which then mounts.
            let name = tb_NewPtr(32, true, true)
            guestPString("Fresh", at: name)
            #expect(tb_DIZero(9, name) == 0)
            let fresh = try #require(readHFS(guestImage(img, 400 * 1024)))
            #expect(fresh.count == 1 && fresh[0].name == "Fresh" && fresh[0].entry.is_dir)
            let (err, vref) = mount(9)
            #expect(err == 0)
            #expect(vinfo(vref).name == "Fresh")
            #expect(tb_DIZero(9, name) == -55) // mounted: not zeroed under it
        }

        @Test func aDiskInsertedEventMountsTheDisk() throws {
            launchTestApp()
            _ = initToolbox()
            let image = try #require(buildHFS(name: "Inserted", size: 400 * 1024, files: files))
            _ = attach(image, drive: 12)
            #expect(tb_PostEvent(7, 12) == 0) // diskEvt
            let evt = tb_NewPtr(16, true, true)
            var got = false
            for _ in 0..<5 where !got {
                got = tb_GetNextEvent(-1, evt) != 0 && rd16(evt) == 7
            }
            #expect(got)
            #expect(rd32(evt + 2) == 12) // noErr in the high word, the drive in the low
            let pb = tb_NewPtr(Int32(HParamBlockRec_SIZEOF), true, true)
            let name = tb_NewPtr(256, true, true)
            guestPString("Inserted:", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            wr16(pb + UInt32(VolumeParam_ioVolIndex_off), UInt16(bitPattern: -1))
            #expect(tb_PBHGetVInfo(pb, false) == 0)
            #expect(rd16(pb + UInt32(HVolumeParam_ioVDrvInfo_off)) == 12)
        }
    }
}
