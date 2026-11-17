// Classic aliases, as the Finder's Make Alias made them: a file flagged as
// an alias whose 'alis' resource 0 is an Alias Manager record (System 7's
// version 2).  Blue Box's Alias Manager (Toolbox/alias.c) resolves a
// record first by the host path in its own tag 0x434C, then by the Mac
// path; both are written here, so a classic program opening the alias
// finds the original as it would one the Finder made.

import CCore
import Foundation
import ForkStore

public enum ClassicAlias {
    static let tagParentName: Int16 = 0, tagAbsolutePath: Int16 = 2, tagHostPath: Int16 = 0x434C, tagEnd: Int16 = -1
    static let fixedSize = 150

    /// An alias record for the host path, whose Mac path is volume:...:name.
    public static func record(hostPath: String, macPath: [UInt8], isFolder: Bool) -> [UInt8] {
        var rec = [UInt8](repeating: 0, count: fixedSize)
        func put16(_ at: Int, _ v: UInt16) { rec[at] = UInt8(v >> 8); rec[at + 1] = UInt8(v & 0xFF) }
        func pstr(_ at: Int, _ max: Int, _ s: ArraySlice<UInt8>) {
            let n = min(s.count, max)
            rec[at] = UInt8(n)
            rec.replaceSubrange(at + 1..<at + 1 + n, with: s.prefix(n))
        }
        let parts = macPath.split(separator: 0x3A, omittingEmptySubsequences: false) // ':'
        let volume = parts.first ?? []
        let name = parts.count > 1 ? parts[parts.count - 1] : volume
        put16(6, 2)                    // version 2
        put16(8, isFolder ? 1 : 0)
        pstr(10, 27, volume)
        put16(42, 0x4244)              // 'BD': HFS
        pstr(50, 63, name)
        put16(130, 0xFFFF)             // nlvlFrom, nlvlTo: none
        put16(132, 0xFFFF)
        func tag(_ t: Int16, _ data: [UInt8]) {
            rec += [UInt8(UInt16(bitPattern: t) >> 8), UInt8(UInt16(bitPattern: t) & 0xFF),
                    UInt8(data.count >> 8), UInt8(data.count & 0xFF)] + data
            if data.count & 1 != 0 { rec.append(0) }
        }
        if parts.count > 2 { tag(tagParentName, Array(parts[parts.count - 2])) }
        tag(tagAbsolutePath, macPath)
        tag(tagHostPath, Array(hostPath.utf8))
        tag(tagEnd, [])
        put16(4, UInt16(rec.count))
        return rec
    }

    /// The host path an alias record names (its own tag), if it has one.
    public static func hostPath(of rec: [UInt8]) -> String? {
        guard rec.count >= fixedSize else { return nil }
        let size = min(rec.count, Int(rec[4]) << 8 | Int(rec[5]))
        var off = fixedSize
        while off + 4 <= size {
            let t = Int16(bitPattern: UInt16(rec[off]) << 8 | UInt16(rec[off + 1]))
            let l = Int(rec[off + 2]) << 8 | Int(rec[off + 3])
            if t == tagEnd || off + 4 + l > size { break }
            if t == tagHostPath { return String(decoding: rec[off + 4..<off + 4 + l], as: UTF8.self) }
            off += 4 + ((l + 1) & ~1)
        }
        return nil
    }

    /// The Finder information an alias of a file with this Finder
    /// information (or of a folder) gets: an application's alias is
    /// 'adrp', a folder's 'fdrp', a document's keeps its type; the
    /// creator stays.
    public static func finderInfo(target: ClassicMeta?, isFolder: Bool) -> ClassicMeta {
        var meta = ClassicMeta()
        if isFolder {
            meta.type = fourCharCode("fdrp")
            meta.creator = fourCharCode("MACS")
        } else if let t = target, t.type == fourCharCode("APPL") {
            meta.type = fourCharCode("adrp")
            meta.creator = t.creator
        } else {
            meta.type = target?.type ?? 0
            meta.creator = target?.creator ?? 0
        }
        meta.flags = FinderFlags.isAlias
        return meta
    }

    /// A resource fork holding one 'alis' resource 0.
    public static func resourceFork(record: [UInt8]) -> [UInt8] {
        record.withUnsafeBufferPointer { r -> [UInt8] in
            var spec = rsrc_spec()
            spec.type = fourCharCode("alis")
            spec.id = 0
            spec.data = r.baseAddress
            spec.len = UInt32(r.count)
            var out: UnsafeMutablePointer<UInt8>?
            var len = 0
            guard rsrc_build(&spec, 1, &out, &len), let out else { return [] }
            defer { bb_free(out) }
            return Array(UnsafeBufferPointer(start: out, count: len))
        }
    }

    /// The 'alis' resource 0 of a fork.
    public static func record(inFork fork: [UInt8]) -> [UInt8]? {
        fork.withUnsafeBufferPointer { ResourceFork(fork: $0.baseAddress, length: $0.count) }?["alis", 0]
    }
}
