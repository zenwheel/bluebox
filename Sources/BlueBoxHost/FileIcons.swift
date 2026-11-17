// Icons Blue Box itself supplies for classic files that have none of their
// own (the System's generic ones, desk accessories' pictures), on top of
// ClassicFiles' Finder bundles, which this module passes on to its users.

@_exported import ClassicFiles
import CCore
import Foundation
import ForkStore
import Toolbox

extension ResourceFork {
    /// The System file's icon family with this ID (its generic icons).
    public init(systemIcons id: Int16) {
        self.init()
        for type in ["ICN#", "icl4", "icl8", "ics#", "ics4", "ics8"] {
            var len: UInt32 = 0
            let code = type.utf8.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
            if let p = machine_system_resource(code, id, &len) {
                set(type, id, Array(UnsafeBufferPointer(start: p, count: Int(len))))
            }
        }
    }
}

private enum CustomIcon {
    static let id: Int16 = -16455 // kCustomIconResource
    static let flag: UInt16 = 0x0400 // kHasCustomIcon
}

/// Desk accessories that draw their own picture from a bitmap in their
/// code, by name: where it is in the DRVR (checked by the DRVR's size),
/// its row bytes and rows, and the part of it to show.
private let deskAccessoryPictures: [String: (drvrSize: Int, offset: Int, rowBytes: Int, rows: Int,
                                              crop: (x: Int, y: Int, width: Int, height: Int))] = [
    // The Moose in its box, mouth shut (1.21 and EDUCORP's New Talking Moose!).
    "Talking Moose": (11656, 0x2756, 10, 58, (1, 6, 75, 46)),
]

/// A desk accessory's own picture, in a square, its box drawn round it.
func deskAccessoryPicture(name: String, drvr: [UInt8]) -> IconImage? {
    guard let p = deskAccessoryPictures[name], drvr.count == p.drvrSize,
          p.offset + p.rowBytes * p.rows <= drvr.count else { return nil }
    let c = p.crop
    let side = max(c.width, c.height) + 2
    var img = IconImage(size: side, rgba: [UInt8](repeating: 0, count: side * side * 4))
    let left = (side - c.width) / 2, top = (side - c.height) / 2
    for y in -1...c.height {
        for x in -1...c.width {
            var black = y == -1 || y == c.height || x == -1 || x == c.width
            if !black {
                let byte = drvr[p.offset + (c.y + y) * p.rowBytes + (c.x + x) / 8]
                black = byte & (0x80 >> UInt8((c.x + x) % 8)) != 0
            }
            let d = ((top + y) * side + left + x) * 4
            let v: UInt8 = black ? 0 : 255
            img.rgba[d] = v
            img.rgba[d + 1] = v
            img.rgba[d + 2] = v
            img.rgba[d + 3] = 255
        }
    }
    return img
}

/// The icons of a file with no bundle of its own, as the Finder showed it:
/// its custom icon, else a desk accessory's own (resources its DRVR owns,
/// or the picture it draws), else the System's for its type: a desk
/// accessory, or a suitcase of them (the Font/DA Mover's) or of fonts.
public func fileIcons(fork: ResourceFork?, rsrc: UnsafePointer<UInt8>?, length: Int, type: UInt32,
                      flags: UInt16) -> [IconImage] {
    if let fork, flags & CustomIcon.flag != 0 {
        let custom = icons(fork: fork, id: CustomIcon.id)
        if !custom.isEmpty { return custom }
    }
    let das = deskAccessories(fork: rsrc, length: length)
    if let fork {
        for da in das {
            // Owned resources: bits 15-14 set, the DRVR's ID in 13-5.
            let first = Int16(truncatingIfNeeded: 0xC000 | (Int(da.id) & 0x3F) << 5)
            for sub in 0..<32 {
                let own = icons(fork: fork, id: first + Int16(sub))
                if !own.isEmpty { return own }
            }
        }
        for da in das {
            if let drvr = fork["DRVR", da.id], let picture = deskAccessoryPicture(name: da.name, drvr: drvr) {
                return [picture]
            }
        }
    }
    let text = fourCCText(type)
    if text == "dfil" || (!das.isEmpty && text != "DFIL") {
        return icons(fork: ResourceFork(systemIcons: -3991), id: -3991)
    }
    guard ["DFIL", "FFIL", "suit"].contains(text) else { return [] }
    var suitcase = icons(fork: ResourceFork(systemIcons: -3970), id: -3970)
    if text == "DFIL", let i = suitcase.firstIndex(where: { $0.size == 32 }),
       let da = icons(fork: ResourceFork(systemIcons: -3991), id: -3991).first(where: { $0.size == 16 }) {
        // Desk accessories': the desk accessory's mark on the suitcase.
        for y in 0..<16 {
            for x in 0..<16 where da.rgba[(y * 16 + x) * 4 + 3] != 0 {
                let s = (y * 16 + x) * 4, d = ((y + 12) * 32 + x + 8) * 4
                suitcase[i].rgba[d..<d + 4] = da.rgba[s..<s + 4]
            }
        }
    }
    return suitcase
}

/// The application's Finder bundle, read from its AppleDouble or MacBinary
/// file; with no icon of its own there, a file's other icons.
public func finderBundle(applicationAt path: String) -> FinderBundle {
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    guard appledouble_read(path, &mf, &err, err.count) else { return FinderBundle() }
    defer { mac_file_free(&mf) }
    let fork = ResourceFork(fork: mf.rsrc, length: mf.rsrc_len)
    var result = fork.map { finderBundle(fork: $0, creator: mac_file_creator(&mf)) } ?? FinderBundle()
    if result.appIcons.isEmpty {
        let flags = UInt16(mf.finder_info.8) << 8 | UInt16(mf.finder_info.9)
        result.appIcons = fileIcons(fork: fork, rsrc: mf.rsrc, length: mf.rsrc_len, type: mac_file_type(&mf),
                                    flags: flags)
    }
    return result
}
