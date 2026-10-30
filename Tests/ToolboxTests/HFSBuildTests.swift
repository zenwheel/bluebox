import CCore
import ForkStore
import Foundation
import Testing
import Toolbox

/// A synthetic HFS volume (ForkStore's hfs_build): files and folders by ID.
struct HFSTestFile {
    var id: UInt32
    var parent: UInt32 = 2
    var dir = false
    var name: String
    var type = "TEXT", creator = "ttxt"
    var data: [UInt8] = []
    var rsrc: [UInt8] = []
}

func buildHFS(name: String, size: Int, files: [HFSTestFile], blockSize: UInt32 = 0) -> [UInt8]? {
    var entries = [hfs_build_entry](repeating: hfs_build_entry(), count: files.count)
    var buffers: [UnsafeMutablePointer<UInt8>] = []
    defer { buffers.forEach { $0.deallocate() } }
    func copy(_ b: [UInt8]) -> UnsafePointer<UInt8>? {
        guard !b.isEmpty else { return nil }
        let p = UnsafeMutablePointer<UInt8>.allocate(capacity: b.count)
        p.initialize(from: b, count: b.count)
        buffers.append(p)
        return UnsafePointer(p)
    }
    for (i, f) in files.enumerated() {
        entries[i].cnid = f.id
        entries[i].parent = f.parent
        entries[i].is_dir = f.dir
        withUnsafeMutableBytes(of: &entries[i].name) { n in
            let b = Array(f.name.utf8)
            n[0] = UInt8(b.count)
            for (k, c) in b.enumerated() { n[1 + k] = c }
        }
        if !f.dir {
            withUnsafeMutableBytes(of: &entries[i].finder_info) { fi in
                for (k, c) in Array((f.type + f.creator).utf8).enumerated() { fi[k] = c }
            }
        }
        entries[i].data = copy(f.data)
        entries[i].data_len = UInt32(f.data.count)
        entries[i].rsrc = copy(f.rsrc)
        entries[i].rsrc_len = UInt32(f.rsrc.count)
        entries[i].create_date = 0xB000_0000
        entries[i].modify_date = 0xB000_0001
    }
    var p = hfs_build_params()
    withUnsafeMutableBytes(of: &p.name) { n in
        let b = Array(name.utf8)
        n[0] = UInt8(b.count)
        for (k, c) in b.enumerated() { n[1 + k] = c }
    }
    p.size = UInt32(size)
    p.block_size = blockSize
    p.create_date = 0xB000_0000
    p.modify_date = 0xB000_0000
    var out: UnsafeMutablePointer<UInt8>?
    var err = [CChar](repeating: 0, count: 256)
    guard hfs_build(&p, entries, entries.count, &out, &err, err.count), let out else { return nil }
    defer { free(out) }
    return Array(UnsafeBufferPointer(start: out, count: size))
}

/// Every entry of a volume held in host memory, with both forks' bytes.
func readHFS(_ image: [UInt8]) -> [(entry: hfs_entry, name: String, data: [UInt8], rsrc: [UInt8])]? {
    var bytes = image
    return bytes.withUnsafeMutableBufferPointer { buf -> [(hfs_entry, String, [UInt8], [UInt8])]? in
        var err = [CChar](repeating: 0, count: 256)
        guard let v = hfs_open(buf.baseAddress, buf.count, &err, err.count) else { return nil }
        defer { hfs_close(v) }
        var list: UnsafeMutablePointer<hfs_entry>?
        var n = 0
        guard hfs_list(v, &list, &n, &err, err.count), let list else { return nil }
        defer { free(list) }
        var out: [(hfs_entry, String, [UInt8], [UInt8])] = []
        for i in 0..<n {
            var e = list[i]
            let name = withUnsafeBytes(of: &e.name) { String(decoding: $0[1..<(1 + Int($0[0]))], as: UTF8.self) }
            var data: [UInt8] = [], rsrc: [UInt8] = []
            if !e.is_dir {
                for fork in [false, true] {
                    var p: UnsafeMutablePointer<UInt8>?
                    var len = 0
                    guard hfs_read_fork(v, &e, fork, &p, &len, &err, err.count) else { return nil }
                    let bytes = Array(UnsafeBufferPointer(start: p, count: len))
                    free(p)
                    if fork { rsrc = bytes } else { data = bytes }
                }
            }
            out.append((e, name, data, rsrc))
        }
        return out
    }
}

extension BlueBox {
    /// HFS volumes written by ForkStore and read back by its reader.
    @Suite struct HFSBuild {
        @Test func manyFilesMakeAnIndexedCatalog() throws {
            var files: [HFSTestFile] = [HFSTestFile(id: 16, dir: true, name: "Sub")]
            for i in 0..<400 {
                files.append(HFSTestFile(id: UInt32(17 + i), parent: i % 3 == 0 ? 16 : 2, name: "File \(i)",
                                         data: Array(repeating: UInt8(i & 0xFF), count: i * 7),
                                         rsrc: i % 5 == 0 ? [UInt8(i & 0xFF), 9] : []))
            }
            let image = try #require(buildHFS(name: "Big", size: 4 * 1024 * 1024, files: files))
            // The catalog's tree has index nodes above its leaves.
            let mdb = Array(image[1024..<1536])
            func be16(_ p: Int) -> Int { Int(mdb[p]) << 8 | Int(mdb[p + 1]) }
            func be32(_ a: [UInt8], _ p: Int) -> Int { Int(a[p]) << 24 | Int(a[p + 1]) << 16 | Int(a[p + 2]) << 8 | Int(a[p + 3]) }
            let catStart = be16(28) * 512 + be16(150) * be32(mdb, 20)
            #expect(be16(0) == 0x4244)
            let depth = Int(image[catStart + 14]) << 8 | Int(image[catStart + 15])
            #expect(depth >= 2)
            #expect(be32(mdb, 84) == 400 && be32(mdb, 88) == 1) // files, folders
            let back = try #require(readHFS(image))
            #expect(back.count == 402) // with the root
            for i in 0..<400 {
                let f = back.first { $0.name == "File \(i)" }
                #expect(f?.data == Array(repeating: UInt8(i & 0xFF), count: i * 7))
                #expect(f?.entry.parent == (i % 3 == 0 ? 16 : 2))
            }
            // The alternate master directory block matches.
            #expect(Array(image[(image.count - 1024)..<(image.count - 512)]) == mdb)
        }

        @Test func filesThatDontFitAreRefused() {
            let big = HFSTestFile(id: 16, name: "Huge", data: [UInt8](repeating: 1, count: 500 * 1024))
            #expect(buildHFS(name: "Small", size: 400 * 1024, files: [big]) == nil)
        }

        /// An MFS disk (the 400K disks'): a flat directory and a block map.
        @Test func mfsVolumesAreRead() throws {
            var d = [UInt8](repeating: 0, count: 400 * 1024)
            func put16(_ at: Int, _ v: Int) { d[at] = UInt8(v >> 8 & 0xFF); d[at + 1] = UInt8(v & 0xFF) }
            func put32(_ at: Int, _ v: UInt32) { for k in 0..<4 { d[at + k] = UInt8(v >> (24 - 8 * UInt32(k)) & 0xFF) } }
            let m = 1024
            put16(m, 0xD2D7)
            put16(m + 12, 1)      // files
            put16(m + 14, 4)      // directory at sector 4
            put16(m + 16, 12)     // 12 sectors long
            put16(m + 18, 391)    // allocation blocks
            put32(m + 20, 1024)   // of 1K
            put16(m + 28, 16)     // from sector 16
            put32(m + 30, 2)
            let name = Array("Old File".utf8)
            d[m + 36] = 6
            for (i, c) in Array("MFS 1K".utf8).enumerated() { d[m + 37 + i] = c }
            // The map: block 2 links to 3, 3 is the last (12-bit entries from block 2).
            d[m + 64] = 0x00; d[m + 65] = 0x30; d[m + 66] = 0x01
            // The directory entry: in use, FInfo, file 1, data from block 2, 1500 bytes.
            let e = 4 * 512
            d[e] = 0x80
            for (i, c) in Array("TEXTMACA".utf8).enumerated() { d[e + 2 + i] = c }
            put32(e + 18, 1)
            put16(e + 22, 2)
            put32(e + 24, 1500)
            put32(e + 28, 2048)
            d[e + 50] = UInt8(name.count)
            for (i, c) in name.enumerated() { d[e + 51 + i] = c }
            for i in 0..<1500 { d[16 * 512 + i] = UInt8(i % 251) }
            let back = try #require(readHFS(d))
            #expect(back.count == 2)
            let f = try #require(back.first { $0.name == "Old File" })
            #expect(f.data == (0..<1500).map { UInt8($0 % 251) })
            #expect(f.entry.parent == 2)
        }
    }
}
