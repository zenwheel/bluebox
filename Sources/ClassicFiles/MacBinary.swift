// MacBinary III: a classic file whole in one host file (.bin), for sending
// where neither forks nor sidecars survive (mail, the web).  ForkStore
// reads it back (macbinary_parse), as do StuffIt Expander and The
// Unarchiver.

import Foundation

public enum MacBinary {
    /// CRC-16/XMODEM, as MacBinary II and III check their header with.
    static func crc(_ bytes: ArraySlice<UInt8>) -> UInt16 {
        var crc: UInt16 = 0
        for b in bytes {
            crc ^= UInt16(b) << 8
            for _ in 0..<8 { crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1 }
        }
        return crc
    }

    /// A file's data fork and classic side as MacBinary III; name is the
    /// Mac name (MacRoman, at most 63 bytes).
    public static func encode(name: [UInt8], data: [UInt8], meta: ClassicMeta) -> [UInt8] {
        var h = [UInt8](repeating: 0, count: 128)
        func put32(_ at: Int, _ v: UInt32) { for i in 0..<4 { h[at + i] = UInt8(truncatingIfNeeded: v >> UInt32(24 - i * 8)) } }
        let n = name.prefix(63)
        h[1] = UInt8(n.count)
        h.replaceSubrange(2..<2 + n.count, with: n)
        let fi = meta.finderInfo
        h.replaceSubrange(65..<73, with: fi[0..<8])  // type, creator
        h[73] = fi[8] & ~0x01                          // flags' high byte, not "inited"
        h.replaceSubrange(75..<81, with: fi[10..<16]) // location, folder
        put32(83, UInt32(data.count))
        put32(87, UInt32(meta.resourceFork.count))
        put32(91, meta.createDate)
        put32(95, meta.modifyDate)
        h[101] = fi[9]                                 // flags' low byte
        h.replaceSubrange(102..<106, with: Array("mBIN".utf8))
        h[106] = fi[16 + 8]                            // the script (FXInfo.fdScript)
        h[107] = fi[16 + 9]                            // the extended flags
        h[122] = 130                                   // MacBinary III
        h[123] = 129                                   // readable as MacBinary II
        let c = crc(h[0..<124])
        h[124] = UInt8(c >> 8)
        h[125] = UInt8(c & 0xFF)
        func padded(_ b: [UInt8]) -> [UInt8] { b + [UInt8](repeating: 0, count: (128 - b.count % 128) % 128) }
        return h + padded(data) + padded(meta.resourceFork)
    }
}
