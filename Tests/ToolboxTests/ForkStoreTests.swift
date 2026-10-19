import CCore
import Foundation
import ForkStore
import Testing

struct ForkStoreTests {
    @Test func appleDoubleRoundTrip() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let path = dir.appendingPathComponent("Doc").path

        var data: [UInt8] = Array("hello\rworld".utf8)
        var rsrc = buildFork([TestResource(type: "STR ", id: 128, name: "greeting", data: [3, 0x61, 0x62, 0x63])])
        var f = mac_file()
        data.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = $0.count }
        rsrc.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
        withUnsafeMutableBytes(of: &f.finder_info) { fi in
            for (i, b) in cat(be32(fourCC("TEXT")), be32(fourCC("ttxt"))).enumerated() { fi[i] = b }
        }
        f.create_date = 3_000_000_000
        f.modify_date = 3_000_000_100
        var err = [CChar](repeating: 0, count: 256)
        #expect(appledouble_write(path, &f, &err, err.count))
        #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("._Doc").path))

        var back = mac_file()
        #expect(appledouble_read(path, &back, &err, err.count))
        defer { mac_file_free(&back) }
        #expect(Array(UnsafeBufferPointer(start: back.data, count: back.data_len)) == data)
        #expect(Array(UnsafeBufferPointer(start: back.rsrc, count: back.rsrc_len)) == rsrc)
        #expect(mac_file_type(&back) == fourCC("TEXT"))
        #expect(mac_file_creator(&back) == fourCC("ttxt"))
        #expect(back.create_date == 3_000_000_000)
        #expect(back.modify_date == 3_000_000_100)
    }

    @Test func missingSidecarMeansEmptyResourceFork() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let path = dir.appendingPathComponent("plain.txt").path
        try Data("abc".utf8).write(to: URL(fileURLWithPath: path))
        var f = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        #expect(appledouble_read(path, &f, &err, err.count))
        defer { mac_file_free(&f) }
        #expect(f.data_len == 3)
        #expect(f.rsrc_len == 0)
    }

    #if os(macOS)
    /// No sidecar: the file's own Finder info and resource fork, as macOS
    /// keeps them for Finder copies and file sharing.
    @Test func nativeForksWithoutASidecar() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let path = dir.appendingPathComponent("Tune").path
        try Data("data".utf8).write(to: URL(fileURLWithPath: path))
        let info = cat(be32(fourCC("MooV")), be32(fourCC("TVOD"))) + [UInt8](repeating: 0, count: 24)
        let rsrc = Array("resource fork".utf8)
        #expect(setxattr(path, "com.apple.FinderInfo", info, info.count, 0, 0) == 0)
        #expect(setxattr(path, "com.apple.ResourceFork", rsrc, rsrc.count, 0, 0) == 0)
        var f = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        #expect(appledouble_read(path, &f, &err, err.count))
        defer { mac_file_free(&f) }
        #expect(f.has_finder_info && mac_file_type(&f) == fourCC("MooV"))
        #expect(Array(UnsafeBufferPointer(start: f.rsrc, count: f.rsrc_len)) == rsrc)
        #expect(appledouble_has_native(path))
    }
    #endif

    @Test func macBinaryII() throws {
        let data: [UInt8] = [1, 2, 3]
        let rsrc: [UInt8] = buildFork([TestResource(type: "abcd", id: 1, data: [9])])
        var h = [UInt8](repeating: 0, count: 128)
        let name = Array("Test App".utf8)
        h[1] = UInt8(name.count)
        for (i, b) in name.enumerated() { h[2 + i] = b }
        for (i, b) in cat(be32(fourCC("APPL")), be32(fourCC("TEST"))).enumerated() { h[65 + i] = b }
        for (i, b) in be32(UInt32(data.count)).enumerated() { h[83 + i] = b }
        for (i, b) in be32(UInt32(rsrc.count)).enumerated() { h[87 + i] = b }
        h[122] = 129
        h[123] = 129
        var crc: UInt16 = 0
        for b in h[0..<124] {
            crc ^= UInt16(b) << 8
            for _ in 0..<8 { crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1 }
        }
        h[124] = UInt8(crc >> 8)
        h[125] = UInt8(crc & 0xFF)
        let pad = { (n: Int) in [UInt8](repeating: 0, count: (128 - n % 128) % 128) }
        var file = cat(h, data, pad(data.count), rsrc, pad(rsrc.count))

        #expect(macbinary_detect(&file, file.count))
        var f = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        #expect(macbinary_parse(&file, file.count, &f, &err, err.count))
        defer { mac_file_free(&f) }
        #expect(Array(UnsafeBufferPointer(start: f.data, count: f.data_len)) == data)
        #expect(Array(UnsafeBufferPointer(start: f.rsrc, count: f.rsrc_len)) == rsrc)
        #expect(mac_file_type(&f) == fourCC("APPL"))
    }

    @Test func resourceForkParse() throws {
        var fork = buildFork([
            TestResource(type: "STR ", id: 1, name: "one", attrs: 0x20, data: [1, 0x41]),
            TestResource(type: "CODE", id: 0, data: [0, 0, 0, 0x20]),
            TestResource(type: "STR ", id: -2, data: []),
        ])
        var idx = rsrc_index()
        var err = [CChar](repeating: 0, count: 256)
        #expect(rsrc_parse(&fork, fork.count, &idx, &err, err.count))
        defer { rsrc_index_free(&idx) }
        #expect(idx.count == 3)
        let entries = (0..<idx.count).map { idx.entries[$0] }
        #expect(entries.filter { $0.type == fourCC("STR ") }.map(\.id).sorted() == [-2, 1])
        let one = entries.first { $0.id == 1 }!
        #expect(one.attrs == 0x20)
        #expect(one.data_len == 2)
    }

    @Test func truncatedForkIsRejected() {
        var fork = buildFork([TestResource(type: "STR ", id: 1, data: [1, 2, 3])])
        fork.removeLast(10)
        var idx = rsrc_index()
        var err = [CChar](repeating: 0, count: 256)
        #expect(!rsrc_parse(&fork, fork.count, &idx, &err, err.count))
    }

    @Test func dcmp0Codes() throws {
        // literal(4), fixed table[0] (00 00), stored literal "XY", back-reference
        // to it, repeat 'A' x3, end.  Odd decompressed size drops the last byte.
        let body = cat([0x02], Array("ABCD".utf8), [0x4B], [0x11], Array("XY".utf8), [0x23],
                       [0xFE, 0x02, 0x41, 0x02], [0xFF])
        let expected = cat(Array("ABCD".utf8), [0, 0], Array("XYXY".utf8), Array("AAA".utf8))
        var res = dcmp0Resource(decompressedSize: expected.count, body: body)
        #expect(rsrc_is_compressed(&res, res.count))
        #expect(rsrc_decompressed_size(&res, res.count) == UInt32(expected.count))
        var out: UnsafeMutablePointer<UInt8>?
        var outLen = 0
        var err = [CChar](repeating: 0, count: 256)
        #expect(rsrc_decompress(&res, res.count, &out, &outLen, &err, err.count))
        defer { free(out) }
        #expect(Array(UnsafeBufferPointer(start: out, count: outLen)) == expected)
    }

    @Test func dcmp0JumpTable() throws {
        // Literal first address (00 00), then FE 00: segment 1, one more entry at 0x0010.
        let body = cat([0x01, 0x00, 0x00], [0xFE, 0x00, 0x01, 0x01, 0x10], [0xFF])
        let entry = { (addr: Int) -> [UInt8] in cat(be16(addr), [0x3F, 0x3C, 0x00, 0x01, 0xA9, 0xF0]) }
        let expected = cat(entry(0), entry(0x10))
        var res = dcmp0Resource(decompressedSize: expected.count, body: body)
        var out: UnsafeMutablePointer<UInt8>?
        var outLen = 0
        var err = [CChar](repeating: 0, count: 256)
        #expect(rsrc_decompress(&res, res.count, &out, &outLen, &err, err.count))
        defer { free(out) }
        #expect(Array(UnsafeBufferPointer(start: out, count: outLen)) == expected)
    }

    @Test func dcmp2DefaultTableUntagged() throws {
        // Untagged, default table: each byte indexes a 2-byte entry.
        // Entries 0..2 are 00 00, 00 08, 4E BA.
        var res = cat([0xA8, 0x9F, 0x65, 0x72], be16(0x12), be16(0x0901), be32(6),
                      be16(2), [0, 0, 0, 0], [0, 1, 2])
        var out: UnsafeMutablePointer<UInt8>?
        var outLen = 0
        var err = [CChar](repeating: 0, count: 256)
        #expect(rsrc_decompress(&res, res.count, &out, &outLen, &err, err.count))
        defer { free(out) }
        #expect(Array(UnsafeBufferPointer(start: out, count: outLen)) == [0, 0, 0, 8, 0x4E, 0xBA])
    }

    @Test func corruptCompressedDataIsRejected() {
        var res = dcmp0Resource(decompressedSize: 100, body: [0x02, 0x41])
        var out: UnsafeMutablePointer<UInt8>?
        var outLen = 0
        var err = [CChar](repeating: 0, count: 256)
        #expect(!rsrc_decompress(&res, res.count, &out, &outLen, &err, err.count))
    }

    @Test func adcLiteralsAndBackReferences() {
        // literal "abc"; two-byte code: copy 6 from distance 3; three-byte code:
        // copy 5 from distance 1 (overlapping run of the last byte).
        let input: [UInt8] = [0x82, 0x61, 0x62, 0x63,  // literal, 3 bytes
                              0x0C, 0x02,              // len ((0x0C>>2)&0xF)+3 = 6, dist 2+1 = 3
                              0x41, 0x00, 0x00]        // len (1)+4 = 5, dist 0+1 = 1
        var out = [UInt8](repeating: 0, count: 64)
        let n = adc_decompress(input, input.count, &out, out.count)
        #expect(n == 14)
        #expect(Array(out[0..<14]) == Array("abcabcabcccccc".utf8))
    }

    @Test func adcRejectsReferencesBeforeTheStart() {
        let input: [UInt8] = [0x0C, 0x05]
        var out = [UInt8](repeating: 0, count: 16)
        #expect(adc_decompress(input, input.count, &out, out.count) == -1)
    }

    #if os(macOS)
    /// An HFS+ volume made by macOS (hdiutil) read by our own reader: a
    /// file's data and resource forks and Finder info, a folder, and a name
    /// macOS stores decomposed ("Café") coming out as one MacRoman é.
    @Test func hfsPlusVolume() throws {
        let dir = tempDir()
        defer { try? FileManager.default.removeItem(at: dir) }
        let src = dir.appendingPathComponent("src")
        let folder = src.appendingPathComponent("Folder")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let file = folder.appendingPathComponent("Café")
        try Data("data fork".utf8).write(to: file)
        try Data("resource fork".utf8).write(to: URL(fileURLWithPath: file.path + "/..namedfork/rsrc"))
        var info = [UInt8](repeating: 0, count: 32)
        info.replaceSubrange(0..<8, with: Array("TEXTttxt".utf8))
        #expect(setxattr(file.path, "com.apple.FinderInfo", info, 32, 0, 0) == 0)
        let image = dir.appendingPathComponent("vol.cdr").path
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/hdiutil")
        p.arguments = ["create", "-quiet", "-fs", "HFS+", "-layout", "NONE", "-volname", "Plus", "-format", "UDTO",
                       "-srcfolder", src.path, image]
        try p.run()
        p.waitUntilExit()
        guard p.terminationStatus == 0 else { return } // no hdiutil here: nothing to test
        var bytes = [UInt8](try Data(contentsOf: URL(fileURLWithPath: image)))
        var err = [CChar](repeating: 0, count: 256)
        let v = try #require(bytes.withUnsafeMutableBufferPointer { hfs_open($0.baseAddress, $0.count, &err, err.count) })
        defer { hfs_close(v) }
        var entries: UnsafeMutablePointer<hfs_entry>?
        var count = 0
        #expect(hfs_list(v, &entries, &count, &err, err.count))
        defer { free(entries) }
        let all = Array(UnsafeBufferPointer(start: entries, count: count))
        func name(_ e: hfs_entry) -> [UInt8] {
            var e = e
            return withUnsafeBytes(of: &e.name) { Array($0[1..<(1 + Int($0[0]))]) }
        }
        let volName = hfs_volume_name(v)!
        #expect(Array(UnsafeBufferPointer(start: volName + 1, count: Int(volName[0]))) == Array("Plus".utf8))
        let dirEntry = try #require(all.first { $0.is_dir && name($0) == Array("Folder".utf8) })
        let cafe: [UInt8] = Array("Caf".utf8) + [0x8E] // é in MacRoman
        var fileEntry = try #require(all.first { !$0.is_dir && name($0) == cafe })
        #expect(fileEntry.parent == dirEntry.cnid)
        let type = withUnsafeBytes(of: &fileEntry.finder_info) { Array($0[0..<8]) }
        #expect(type == Array("TEXTttxt".utf8))
        var data: UnsafeMutablePointer<UInt8>?, rsrc: UnsafeMutablePointer<UInt8>?
        var dlen = 0, rlen = 0
        #expect(hfs_read_fork(v, &fileEntry, false, &data, &dlen, &err, err.count))
        #expect(hfs_read_fork(v, &fileEntry, true, &rsrc, &rlen, &err, err.count))
        defer { free(data); free(rsrc) }
        #expect(Array(UnsafeBufferPointer(start: data, count: dlen)) == Array("data fork".utf8))
        #expect(Array(UnsafeBufferPointer(start: rsrc, count: rlen)) == Array("resource fork".utf8))
    }
    #endif
}
