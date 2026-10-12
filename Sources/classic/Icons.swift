// Finder icons and document types from an application's resources.
//
// The Finder found an application's icon through its bundle: BNDL maps
// local IDs to ICN# resource IDs, and the FREF for file type 'APPL' names
// the local ID.  The same resource ID is used for the colour (icl8, icl4)
// and small (ics#, ics8, ics4) variants.

import CCore
import Foundation
import ForkStore

/// A square RGBA image (premultiplication not needed: alpha is 0 or 255).
struct IconImage {
    let size: Int
    var rgba: [UInt8]
}

/// What the Finder knew about an application.
struct FinderBundle {
    var appIcons: [IconImage] = []          // 32 and/or 16 pixel
    var documentTypes: [UInt32] = []        // FREF file types other than APPL
}

/// Resources of one fork by type and ID, decompressed if needed.
struct ResourceFork {
    private var table: [UInt32: [Int16: [UInt8]]] = [:]

    init?(fork: UnsafePointer<UInt8>?, length: Int) {
        guard let fork, length > 0 else { return nil }
        var idx = rsrc_index()
        var err = [CChar](repeating: 0, count: 256)
        guard rsrc_parse(fork, length, &idx, &err, err.count) else { return nil }
        defer { rsrc_index_free(&idx) }
        for i in 0..<idx.count {
            var e = idx.entries[i]
            guard let p = rsrc_data(fork, &idx, &e) else { continue }
            var bytes = Array(UnsafeBufferPointer(start: p, count: Int(e.data_len)))
            if rsrc_is_compressed(p, Int(e.data_len)) {
                var out: UnsafeMutablePointer<UInt8>?
                var outLen = 0
                if rsrc_decompress(p, Int(e.data_len), &out, &outLen, &err, err.count), let out {
                    bytes = Array(UnsafeBufferPointer(start: out, count: outLen))
                    free(out)
                }
            }
            table[e.type, default: [:]][e.id] = bytes
        }
    }

    subscript(type: String, id: Int16) -> [UInt8]? { table[fourcc(type)]?[id] }
    func ids(_ type: String) -> [Int16] { (table[fourcc(type)] ?? [:]).keys.sorted() }

    private func fourcc(_ s: String) -> UInt32 { s.utf8.reduce(0) { ($0 << 8) | UInt32($1) } }
}

private func be16(_ b: [UInt8], _ o: Int) -> Int { o + 2 <= b.count ? Int(b[o]) << 8 | Int(b[o + 1]) : 0 }
private func be32(_ b: [UInt8], _ o: Int) -> UInt32 {
    o + 4 <= b.count ? UInt32(b[o]) << 24 | UInt32(b[o + 1]) << 16 | UInt32(b[o + 2]) << 8 | UInt32(b[o + 3]) : 0
}

/// Read the BNDL for the given creator (or the only one) and its icons.
func finderBundle(fork: ResourceFork, creator: UInt32) -> FinderBundle {
    var result = FinderBundle()
    var bndl: [UInt8]?
    for id in fork.ids("BNDL") {
        guard let b = fork["BNDL", id] else { continue }
        if bndl == nil || be32(b, 0) == creator { bndl = b }
    }
    guard let b = bndl else { return result }

    // BNDL: owner(4) ownerID(2) ntypes-1(2), then type(4) n-1(2) (local, rsrc)...
    var maps: [String: [Int: Int16]] = [:]
    var o = 6
    let ntypes = be16(b, o) + 1
    o += 2
    for _ in 0..<ntypes where o + 6 <= b.count {
        let type = fourCCString(be32(b, o))
        let n = be16(b, o + 4) + 1
        o += 6
        for _ in 0..<n where o + 4 <= b.count {
            maps[type, default: [:]][be16(b, o)] = Int16(truncatingIfNeeded: be16(b, o + 2))
            o += 4
        }
    }

    var appLocal: Int?
    for (_, frefID) in maps["FREF"] ?? [:] {
        guard let f = fork["FREF", frefID], f.count >= 6 else { continue }
        let type = be32(f, 0)
        if fourCCString(type) == "APPL" {
            appLocal = be16(f, 4)
        } else if !result.documentTypes.contains(type) {
            result.documentTypes.append(type)
        }
    }
    result.documentTypes.sort()
    if let appLocal, let iconID = maps["ICN#"]?[appLocal] {
        result.appIcons = icons(fork: fork, id: iconID)
    }
    return result
}

/// The 32 and 16 pixel icons with this ID, best colour depth available.
func icons(fork: ResourceFork, id: Int16) -> [IconImage] {
    var out: [IconImage] = []
    if let mono = fork["ICN#", id], mono.count >= 256 {
        let mask = Array(mono[128..<256])
        if let c = fork["icl8", id], c.count >= 1024 {
            out.append(colorIcon(size: 32, depth: 8, pixels: c, mask: mask))
        } else if let c = fork["icl4", id], c.count >= 512 {
            out.append(colorIcon(size: 32, depth: 4, pixels: c, mask: mask))
        } else {
            out.append(colorIcon(size: 32, depth: 1, pixels: Array(mono[0..<128]), mask: mask))
        }
    }
    if let mono = fork["ics#", id], mono.count >= 64 {
        let mask = Array(mono[32..<64])
        if let c = fork["ics8", id], c.count >= 256 {
            out.append(colorIcon(size: 16, depth: 8, pixels: c, mask: mask))
        } else if let c = fork["ics4", id], c.count >= 128 {
            out.append(colorIcon(size: 16, depth: 4, pixels: c, mask: mask))
        } else {
            out.append(colorIcon(size: 16, depth: 1, pixels: Array(mono[0..<32]), mask: mask))
        }
    }
    return out
}

private func colorIcon(size: Int, depth: Int, pixels: [UInt8], mask: [UInt8]) -> IconImage {
    var img = IconImage(size: size, rgba: [UInt8](repeating: 0, count: size * size * 4))
    let maskRow = size / 8
    for y in 0..<size {
        for x in 0..<size {
            let visible = mask[y * maskRow + x / 8] & (0x80 >> UInt8(x % 8)) != 0
            guard visible else { continue }
            let rgb: (UInt8, UInt8, UInt8)
            switch depth {
            case 8:
                rgb = macPalette8(Int(pixels[y * size + x]))
            case 4:
                let byte = pixels[(y * size + x) / 2]
                rgb = macPalette4[Int(x % 2 == 0 ? byte >> 4 : byte & 0xF)]
            default:
                let on = pixels[y * maskRow + x / 8] & (0x80 >> UInt8(x % 8)) != 0
                rgb = on ? (0, 0, 0) : (255, 255, 255)
            }
            let p = (y * size + x) * 4
            img.rgba[p] = rgb.0
            img.rgba[p + 1] = rgb.1
            img.rgba[p + 2] = rgb.2
            img.rgba[p + 3] = 255
        }
    }
    return img
}

/// The standard 8-bit Macintosh colour table: a 6×6×6 cube from white,
/// then ten-step red, green, blue and grey ramps, then black.
func macPalette8(_ i: Int) -> (UInt8, UInt8, UInt8) {
    if i < 215 {
        let r = 5 - i / 36, g = 5 - (i / 6) % 6, b = 5 - i % 6
        return (UInt8(r * 0x33), UInt8(g * 0x33), UInt8(b * 0x33))
    }
    if i == 255 { return (0, 0, 0) }
    let ramp: [UInt8] = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    let v = ramp[(i - 215) % 10]
    switch (i - 215) / 10 {
    case 0: return (v, 0, 0)
    case 1: return (0, v, 0)
    case 2: return (0, 0, v)
    default: return (v, v, v)
    }
}

/// The standard 4-bit Macintosh colour table.
let macPalette4: [(UInt8, UInt8, UInt8)] = [
    (0xFF, 0xFF, 0xFF), (0xFC, 0xF3, 0x05), (0xFF, 0x64, 0x02), (0xDD, 0x08, 0x06),
    (0xF2, 0x08, 0x84), (0x46, 0x00, 0xA5), (0x00, 0x00, 0xD4), (0x02, 0xAB, 0xEA),
    (0x1F, 0xB7, 0x14), (0x00, 0x64, 0x11), (0x56, 0x2C, 0x05), (0x90, 0x71, 0x3A),
    (0xC0, 0xC0, 0xC0), (0x80, 0x80, 0x80), (0x40, 0x40, 0x40), (0x00, 0x00, 0x00),
]

/// Nearest-neighbour scale (icons are pixel art).
func scaled(_ img: IconImage, to size: Int) -> IconImage {
    var out = IconImage(size: size, rgba: [UInt8](repeating: 0, count: size * size * 4))
    for y in 0..<size {
        let sy = y * img.size / size
        for x in 0..<size {
            let sx = x * img.size / size
            let s = (sy * img.size + sx) * 4, d = (y * size + x) * 4
            out.rgba[d..<d + 4] = img.rgba[s..<s + 4]
        }
    }
    return out
}

/// The application's Finder bundle, read from its AppleDouble or MacBinary file.
func finderBundle(applicationAt path: String) -> FinderBundle {
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    guard appledouble_read(path, &mf, &err, err.count) else { return FinderBundle() }
    defer { mac_file_free(&mf) }
    guard let fork = ResourceFork(fork: mf.rsrc, length: mf.rsrc_len) else { return FinderBundle() }
    return finderBundle(fork: fork, creator: mac_file_creator(&mf))
}
