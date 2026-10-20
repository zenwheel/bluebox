// bluebox inspect: a file's resources described, and exported as files a
// modern Mac opens: pictures and icons as PNG, sounds as WAV, strings and
// text as UTF-8.
//
// Pictures and sounds go through Blue Box's own QuickDraw and Sound Manager
// decoders (a machine booted for them, nothing launched); the rest is read
// here.

import CCore
import Foundation
import ForkStore
import Toolbox

/// One resource of a file, decompressed.
public struct InspectedResource: Sendable {
    public let type: UInt32
    public let id: Int16
    public let name: String     // UTF-8; empty if unnamed
    public let attrs: UInt8
    public let storedSize: Int  // in the fork, compressed or not
    public let compressed: Bool
    public let data: [UInt8]

    public init(type: UInt32, id: Int16, name: String = "", attrs: UInt8 = 0, storedSize: Int? = nil,
                compressed: Bool = false, data: [UInt8]) {
        self.type = type
        self.id = id
        self.name = name
        self.attrs = attrs
        self.storedSize = storedSize ?? data.count
        self.compressed = compressed
        self.data = data
    }

    public var typeText: String { fourCCText(type) }
}

public struct InspectError: Error, CustomStringConvertible {
    public let description: String
    /// The resource isn't one Blue Box can decode (nothing failed).
    public let undecodable: Bool
    public init(_ description: String, undecodable: Bool = false) {
        self.description = description
        self.undecodable = undecodable
    }
}

/// MacRoman text as a String.
public func macRomanString<C: Collection>(_ bytes: C) -> String where C.Element == UInt8 {
    let a = Array(bytes)
    var out = [CChar](repeating: 0, count: a.count * 3 + 1)
    let n = macroman_to_utf8(a, a.count, &out, out.count)
    return String(decoding: out[0..<n].map { UInt8(bitPattern: $0) }, as: UTF8.self)
}

/// Every resource of a fork, in the fork's order.
public func inspectResources(fork: UnsafePointer<UInt8>?, length: Int) throws -> [InspectedResource] {
    guard let fork, length > 0 else { return [] }
    var idx = rsrc_index()
    var err = [CChar](repeating: 0, count: 256)
    guard rsrc_parse(fork, length, &idx, &err, err.count) else {
        throw InspectError(String(decoding: err.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self))
    }
    defer { rsrc_index_free(&idx) }
    var out: [InspectedResource] = []
    for i in 0..<idx.count {
        var e = idx.entries[i]
        guard let p = rsrc_data(fork, &idx, &e) else { continue }
        var bytes = Array(UnsafeBufferPointer(start: p, count: Int(e.data_len)))
        let compressed = rsrc_is_compressed(p, Int(e.data_len))
        if compressed {
            var dec: UnsafeMutablePointer<UInt8>?
            var decLen = 0
            if rsrc_decompress(p, Int(e.data_len), &dec, &decLen, &err, err.count), let dec {
                bytes = Array(UnsafeBufferPointer(start: dec, count: decLen))
                free(dec)
            }
        }
        let name = withUnsafeBytes(of: e.name) { macRomanString($0.prefix { $0 != 0 }) }
        out.append(InspectedResource(type: e.type, id: e.id, name: name, attrs: e.attrs, storedSize: Int(e.data_len),
                                     compressed: compressed, data: bytes))
    }
    return out
}

// MARK: - Reading resource data

/// Bounds-checked big-endian reads (zero past the end).
private struct Bytes {
    let b: [UInt8]
    init(_ b: [UInt8]) { self.b = b }
    var count: Int { b.count }
    func u8(_ o: Int) -> Int { o >= 0 && o < b.count ? Int(b[o]) : 0 }
    func u16(_ o: Int) -> Int { u8(o) << 8 | u8(o + 1) }
    func s16(_ o: Int) -> Int { Int(Int16(truncatingIfNeeded: u16(o))) }
    func u32(_ o: Int) -> UInt32 { UInt32(u16(o)) << 16 | UInt32(u16(o + 2)) }
    /// A Pascal string at o, and the offset after it.
    func pstring(_ o: Int) -> (String, Int) {
        let n = u8(o)
        let end = min(o + 1 + n, b.count)
        return (o + 1 < end ? macRomanString(b[(o + 1)..<end]) : "", o + 1 + n)
    }
    /// A Rect (top, left, bottom, right) at o.
    func rect(_ o: Int) -> (top: Int, left: Int, bottom: Int, right: Int) { (s16(o), s16(o + 2), s16(o + 4), s16(o + 6)) }
}

private func rectText(_ r: (top: Int, left: Int, bottom: Int, right: Int)) -> String {
    "\(r.top),\(r.left),\(r.bottom),\(r.right) (\(r.right - r.left)×\(r.bottom - r.top))"
}

/// Classic line ends (CR) as the host's.
private func hostLines(_ s: String) -> String {
    s.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
}

private func truncated(_ s: String, _ max: Int = 60) -> String {
    let flat = s.replacingOccurrences(of: "\r", with: " ").replacingOccurrences(of: "\n", with: " ")
    return flat.count > max ? String(flat.prefix(max - 1)) + "…" : flat
}

private func quoted(_ s: String, _ max: Int = 60) -> String { "\"\(truncated(s, max))\"" }

/// A STR# resource's strings.
private func stringList(_ d: Bytes) -> [String] {
    var out: [String] = []
    var o = 2
    for _ in 0..<d.u16(0) where o < d.count {
        let (s, next) = d.pstring(o)
        out.append(s)
        o = next
    }
    return out
}

/// A 'vers' resource's short and long version strings.
private func versionStrings(_ d: Bytes) -> (short: String, long: String) {
    let (short, next) = d.pstring(6)
    return (short, d.pstring(next).0)
}

/// The sound header of a 'snd ' resource (format 1 or 2): the offset its
/// first sound or buffer command points to.
private func soundHeaderOffset(_ d: Bytes) -> Int? {
    let format = d.s16(0)
    let q = format == 1 ? 4 + d.s16(2) * 6 : format == 2 ? 4 : -1
    guard q >= 0 else { return nil }
    for i in 0..<max(0, d.s16(q)) {
        let c = q + 2 + i * 8
        guard c + 8 <= d.count else { break }
        let cmd = d.u16(c) & 0x7FFF
        if cmd == 80 || cmd == 81 { return Int(d.u32(c + 4)) } // soundCmd, bufferCmd
    }
    return nil
}

/// The rate, as the Sound Manager plays it (a nonsense rate: 22 kHz).
private func soundRate(_ fixed: UInt32) -> Double {
    let r = Double(fixed) / 65536
    return r < 1000 || r > 100000 ? 22254.5454 : r
}

private func soundSummary(_ d: Bytes) -> String {
    let format = d.s16(0)
    guard format == 1 || format == 2 else { return "format \(format)" }
    guard let h = soundHeaderOffset(d), h + 22 <= d.count else { return "format \(format), no sampled sound" }
    let rate = soundRate(d.u32(h + 8))
    var channels = 1, bits = 8, frames = 0
    var coding = ""
    switch d.u8(h + 20) {
    case 0x00: // standard
        frames = Int(d.u32(h + 4))
    case 0xFF: // extended
        channels = Int(d.u32(h + 4))
        frames = Int(d.u32(h + 22))
        bits = d.s16(h + 48)
    case 0xFE: // compressed
        channels = Int(d.u32(h + 4))
        frames = Int(d.u32(h + 22))
        let fmt = fourCCText(d.u32(h + 40)), cid = d.s16(h + 56)
        bits = 16
        if fmt == "ima4" {
            coding = "IMA 4:1, "
            frames *= 64
        } else if cid == 3 || fmt == "MAC3" {
            coding = "MACE 3:1, "
            frames *= 6
        } else if cid == 4 || fmt == "MAC6" {
            coding = "MACE 6:1, "
            frames *= 6
        } else if ["twos", "sowt", "raw ", "NONE"].contains(fmt) {
            bits = d.s16(h + 62)
        } else {
            coding = "'\(fmt)', "
        }
    default:
        return "format \(format), unknown sound header"
    }
    let chans = channels == 1 ? "mono" : channels == 2 ? "stereo" : "\(channels) channels"
    let seconds = String(format: "%.2f s", Double(frames) / rate)
    return "format \(format), \(coding)\(bits)-bit \(chans), \(Int(rate.rounded())) Hz, \(seconds)"
}

private func pictureSummary(_ d: Bytes) -> String {
    let frame = d.rect(2)
    var version = "version 1"
    if d.u16(10) == 0x0011 && d.u16(12) == 0x02FF {
        version = d.u16(14) == 0x0C00 && d.s16(16) == -2 ? "extended version 2" : "version 2"
    } else if d.u16(10) != 0x1101 {
        version = "unknown version"
    }
    let origin = frame.top != 0 || frame.left != 0 ? " at \(frame.left),\(frame.top)" : ""
    return "\(frame.right - frame.left)×\(frame.bottom - frame.top)\(origin), \(version)"
}

private func menuSummary(_ d: Bytes) -> String {
    let (title, start) = d.pstring(14)
    var items: [String] = []
    var o = start
    while o < d.count && d.u8(o) != 0 {
        let (item, next) = d.pstring(o)
        items.append(item.hasPrefix("-") ? "-" : item)
        o = next + 4 // icon, key, mark, style
    }
    let shown = d.u8(14) == 1 && d.u8(15) == 0x14 ? "Apple" : "\"\(title)\"" // the Apple menu's title
    return truncated("\(shown): " + items.joined(separator: ", "), 72)
}

private func dialogItemsSummary(_ d: Bytes) -> String {
    let n = d.s16(0) + 1
    var kinds: [String: Int] = [:]
    var order: [String] = []
    var o = 2
    for _ in 0..<max(0, n) where o + 14 <= d.count {
        let kind: String
        switch d.u8(o + 12) & 0x7F {
        case 0: kind = "user item"
        case 1: kind = "help item"
        case 4: kind = "button"
        case 5: kind = "check box"
        case 6: kind = "radio button"
        case 7: kind = "control"
        case 8: kind = "static text"
        case 16: kind = "edit text"
        case 32: kind = "icon"
        case 64: kind = "picture"
        default: kind = "other"
        }
        if kinds[kind] == nil { order.append(kind) }
        kinds[kind, default: 0] += 1
        let len = d.u8(o + 13)
        o += 14 + len + (len & 1)
    }
    let parts = order.map { "\(kinds[$0]!) \($0)\(kinds[$0]! > 1 ? (($0.hasSuffix("x") ? "es" : "s")) : "")" }
    return "\(n) item\(n == 1 ? "" : "s")" + (parts.isEmpty ? "" : ": " + parts.joined(separator: ", "))
}

private func sizeSummary(_ d: Bytes) -> String {
    let flags = d.u16(0)
    let names: [(Int, String)] = [
        (14, "suspend/resume"), (12, "can background"), (11, "activates on switch"), (10, "only background"),
        (9, "front clicks"), (8, "app-died events"), (7, "32-bit clean"), (6, "high-level events"),
        (5, "remote events"), (4, "stationery"), (3, "TSM text"),
    ]
    let set = names.filter { flags & (1 << $0.0) != 0 }.map(\.1)
    let k = { (v: UInt32) in "\(v / 1024)K" }
    return "preferred \(k(d.u32(2))), minimum \(k(d.u32(6))), flags $\(String(format: "%04X", flags))"
        + (set.isEmpty ? "" : " (" + set.joined(separator: ", ") + ")")
}

private func bundleSummary(_ d: Bytes) -> String {
    var parts: [String] = []
    var o = 8
    for _ in 0..<(d.u16(6) + 1) where o + 6 <= d.count {
        let n = d.u16(o + 4) + 1
        parts.append("\(fourCCText(d.u32(o))) ×\(n)")
        o += 6 + n * 4
    }
    return "owner '\(fourCCText(d.u32(0)))': " + parts.joined(separator: ", ")
}

/// Pixel size and depth of a 'cicn'.
private func colorIconInfo(_ d: Bytes) -> (width: Int, height: Int, depth: Int) {
    let b = d.rect(6)
    return (b.right - b.left, b.bottom - b.top, d.s16(32))
}

/// A short description of a resource's contents, for the kinds worth one.
public func resourceSummary(_ r: InspectedResource) -> String? {
    let d = Bytes(r.data)
    switch r.typeText {
    case "STR ":
        return quoted(d.pstring(0).0)
    case "STR#":
        let list = stringList(d)
        return truncated("\(list.count) string\(list.count == 1 ? "" : "s"): "
                         + list.map { "\"\($0)\"" }.joined(separator: ", "), 72)
    case "TEXT":
        let text = hostLines(macRomanString(r.data))
        let lines = text.split(separator: "\n", omittingEmptySubsequences: false)
        let first = lines.first.map(String.init) ?? ""
        return quoted(first) + (lines.count > 1 ? " (\(lines.count) lines)" : "")
    case "vers":
        let v = versionStrings(d)
        return v.long.isEmpty ? v.short : "\(v.short), \(quoted(v.long, 50))"
    case "PICT":
        return d.count >= 10 ? pictureSummary(d) : nil
    case "ICON": return "32×32, 1-bit"
    case "ICN#": return "32×32, 1-bit with mask"
    case "ics#": return "16×16, 1-bit with mask"
    case "icl4": return "32×32, 4-bit"
    case "icl8": return "32×32, 8-bit"
    case "ics4": return "16×16, 4-bit"
    case "ics8": return "16×16, 8-bit"
    case "cicn":
        let c = colorIconInfo(d)
        return "\(c.width)×\(c.height), \(c.depth)-bit"
    case "snd ":
        return soundSummary(d)
    case "MENU":
        return menuSummary(d)
    case "DLOG":
        return "\(quoted(d.pstring(20).0)), bounds \(rectText(d.rect(0))), items \(d.s16(18))"
    case "WIND":
        return "\(quoted(d.pstring(18).0)), bounds \(rectText(d.rect(0)))"
    case "ALRT":
        return "bounds \(rectText(d.rect(0))), items \(d.s16(8))"
    case "DITL":
        return dialogItemsSummary(d)
    case "SIZE":
        return sizeSummary(d)
    case "BNDL":
        return bundleSummary(d)
    case "FREF":
        let file = d.pstring(6).0
        return "'\(fourCCText(d.u32(0)))', icon \(d.s16(4))" + (file.isEmpty ? "" : ", \(quoted(file))")
    default:
        return nil
    }
}

// MARK: - Export

/// PCM as a WAV file: 8-bit (unsigned) or 16-bit (little-endian) samples.
public func wavData(pcm: UnsafeBufferPointer<Int16>, channels: Int, rate: Int, bits: Int) -> Data {
    let bytesPerSample = bits == 8 ? 1 : 2
    var body = Data(capacity: pcm.count * bytesPerSample)
    for s in pcm {
        if bits == 8 {
            body.append(UInt8(Int(s >> 8) + 128))
        } else {
            body.append(UInt8(truncatingIfNeeded: s))
            body.append(UInt8(truncatingIfNeeded: s >> 8))
        }
    }
    func le32(_ v: Int) -> [UInt8] { (0..<4).map { UInt8((v >> ($0 * 8)) & 0xFF) } }
    func le16(_ v: Int) -> [UInt8] { [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF)] }
    var out = Data("RIFF".utf8)
    out.append(contentsOf: le32(36 + body.count))
    out.append(contentsOf: Array("WAVEfmt ".utf8))
    out.append(contentsOf: le32(16) + le16(1) + le16(channels) + le32(rate)
               + le32(rate * channels * bytesPerSample) + le16(channels * bytesPerSample) + le16(bits))
    out.append(contentsOf: Array("data".utf8))
    out.append(contentsOf: le32(body.count))
    out.append(body)
    return out
}

/// A 'cicn': its pixels through its own colour table, its mask as alpha.
private func decodeColorIcon(_ d: Bytes) -> (rgba: [UInt8], width: Int, height: Int)? {
    // PixMap (50 bytes), mask BitMap and icon BitMap (14 each), a handle;
    // then the mask's bits, the 1-bit icon's, the colour table, the pixels.
    let pmRow = d.u16(4) & 0x3FFF, bounds = d.rect(6), depth = d.s16(32)
    let maskRow = d.u16(54), bmapRow = d.u16(68)
    let w = bounds.right - bounds.left, h = bounds.bottom - bounds.top
    guard w > 0, h > 0, w <= 256, h <= 256, [1, 2, 4, 8].contains(depth), pmRow * 8 >= w * depth else { return nil }
    let maskOff = 82, ctabOff = maskOff + maskRow * h + bmapRow * h
    let ctSize = d.s16(ctabOff + 6) + 1
    let pixOff = ctabOff + 8 + max(0, ctSize) * 8
    guard pixOff + pmRow * h <= d.count else { return nil }
    var colors = [UInt32: (UInt8, UInt8, UInt8)]()
    let device = d.u16(ctabOff + 4) & 0x8000 != 0
    for i in 0..<max(0, ctSize) {
        let e = ctabOff + 8 + i * 8
        colors[UInt32(device ? i : d.u16(e))] = (UInt8(d.u8(e + 2)), UInt8(d.u8(e + 4)), UInt8(d.u8(e + 6)))
    }
    var rgba = [UInt8](repeating: 0, count: w * h * 4)
    for y in 0..<h {
        for x in 0..<w {
            let visible = maskRow == 0 || d.u8(maskOff + y * maskRow + x / 8) & (0x80 >> (x % 8)) != 0
            guard visible else { continue }
            let bit = x * depth
            let v = (d.u8(pixOff + y * pmRow + bit / 8) >> (8 - depth - bit % 8)) & ((1 << depth) - 1)
            let c = colors[UInt32(v)] ?? (v == 0 ? (255, 255, 255) : (0, 0, 0))
            let p = (y * w + x) * 4
            rgba[p] = c.0
            rgba[p + 1] = c.1
            rgba[p + 2] = c.2
            rgba[p + 3] = 255
        }
    }
    return (rgba, w, h)
}

/// Writes resources into a folder as host files.
public final class ResourceExporter {
    public let directory: String
    private let resources: [InspectedResource]
    private var booted = false

    /// resources: the whole file's, where icons find their masks.
    public init(directory: String, resources: [InspectedResource]) {
        self.directory = directory
        self.resources = resources
    }

    /// The file's name: type and ID, and the resource's name if it has one,
    /// all safe for the host's file system.
    public static func fileName(_ r: InspectedResource, extension ext: String) -> String {
        func safe(_ s: String) -> String {
            String(s.unicodeScalars.map { c -> Character in
                c.value < 0x20 || c == "/" || c == ":" || c == "\\" ? "_" : Character(c)
            })
        }
        let type = safe(r.typeText.trimmingCharacters(in: .whitespaces))
        var name = safe(r.name).trimmingCharacters(in: .whitespaces)
        if name.count > 64 { name = String(name.prefix(64)) }
        while name.hasPrefix(".") { name.removeFirst() }
        return "\(type)_\(r.id)" + (name.isEmpty ? "" : " \(name)") + ".\(ext)"
    }

    /// Export one resource.  Kinds without a host format are written raw
    /// (.bin) when raw is set, else skipped (nil).  Returns the file's name.
    public func export(_ r: InspectedResource, raw: Bool = false) throws -> String? {
        do {
            return try convert(r, raw: raw)
        } catch let e as InspectError where e.undecodable && raw {
            return try write(r, "bin", Data(r.data))
        }
    }

    private func convert(_ r: InspectedResource, raw: Bool) throws -> String? {
        switch r.typeText {
        case "PICT":
            return try writePicture(r)
        case "ICON", "ICN#", "ics#", "icl4", "icl8", "ics4", "ics8", "cicn":
            return try writeIcon(r)
        case "snd ":
            return try writeSound(r)
        case "STR ", "STR#", "TEXT", "vers":
            return try write(r, "txt", Data(text(r).utf8))
        default:
            return raw ? try write(r, "bin", Data(r.data)) : nil
        }
    }

    /// A text resource as UTF-8 with the host's line ends.
    private func text(_ r: InspectedResource) -> String {
        let d = Bytes(r.data)
        switch r.typeText {
        case "STR ": return hostLines(d.pstring(0).0) + "\n"
        case "STR#": return stringList(d).map { hostLines($0) + "\n" }.joined()
        case "vers":
            let v = versionStrings(d)
            return "\(v.short)\n\(hostLines(v.long))\n"
        default: return hostLines(macRomanString(r.data))
        }
    }

    private func write(_ r: InspectedResource, _ ext: String, _ data: Data) throws -> String {
        let name = Self.fileName(r, extension: ext)
        do {
            try data.write(to: URL(fileURLWithPath: directory).appendingPathComponent(name))
        } catch {
            throw InspectError("\(name): \(error.localizedDescription)")
        }
        return name
    }

    private func writePNG(_ r: InspectedResource, _ rgba: [UInt8], _ w: Int, _ h: Int, alpha: Bool) throws -> String {
        let name = Self.fileName(r, extension: "png")
        let path = (directory as NSString).appendingPathComponent(name)
        let ok = rgba.withUnsafeBufferPointer {
            alpha ? machine_write_png_alpha(path, $0.baseAddress, Int32(w), Int32(h))
                  : machine_write_png(path, $0.baseAddress, Int32(w), Int32(h))
        }
        guard ok else { throw InspectError("\(name): could not write") }
        return name
    }

    /// Pictures and sounds are decoded in a machine of their own.
    private func machine() throws {
        if booted { return }
        guard machine_boot(false) else { throw InspectError("could not set up guest memory") }
        booted = true
    }

    private func writePicture(_ r: InspectedResource) throws -> String {
        try machine()
        var px: UnsafeMutablePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0
        guard machine_picture_rgba(r.data, r.data.count, &px, &w, &h), let px else {
            throw InspectError("\(r.typeText) \(r.id): not a picture QuickDraw can draw", undecodable: true)
        }
        defer { free(px) }
        return try writePNG(r, Array(UnsafeBufferPointer(start: px, count: Int(w) * Int(h) * 4)), Int(w), Int(h),
                            alpha: false)
    }

    private func writeSound(_ r: InspectedResource) throws -> String {
        try machine()
        var pcm: UnsafeMutablePointer<Int16>?
        var frames: UInt32 = 0
        var channels: Int32 = 0, bits: Int32 = 0
        var rate = 0.0
        guard machine_snd_pcm(r.data, r.data.count, &pcm, &frames, &channels, &rate, &bits), let pcm else {
            throw InspectError("\(r.typeText) \(r.id): no sampled sound Blue Box can decode", undecodable: true)
        }
        defer { free(pcm) }
        let samples = UnsafeBufferPointer(start: pcm, count: Int(frames) * Int(channels))
        return try write(r, "wav", wavData(pcm: samples, channels: Int(channels), rate: Int(rate.rounded()),
                                            bits: Int(bits)))
    }

    private func writeIcon(_ r: InspectedResource) throws -> String {
        let type = r.typeText, d = r.data
        let size = type.hasPrefix("ics") ? 16 : 32
        let maskBytes = size * size / 8
        /// The mask of a colour icon: its black-and-white family member's.
        func familyMask() -> [UInt8] {
            let mono = resources.first { $0.typeText == (size == 16 ? "ics#" : "ICN#") && $0.id == r.id }
            if let m = mono?.data, m.count >= maskBytes * 2 { return Array(m[maskBytes..<maskBytes * 2]) }
            return [UInt8](repeating: 0xFF, count: maskBytes)
        }
        let bad = InspectError("\(type) \(r.id): too short for an icon", undecodable: true)
        let img: IconImage
        switch type {
        case "cicn":
            guard let c = decodeColorIcon(Bytes(d)) else { throw InspectError("cicn \(r.id): not a colour icon", undecodable: true) }
            return try writePNG(r, c.rgba, c.width, c.height, alpha: true)
        case "ICON":
            guard d.count >= 128 else { throw bad }
            img = colorIcon(size: 32, depth: 1, pixels: Array(d[0..<128]), mask: [UInt8](repeating: 0xFF, count: 128))
        case "ICN#", "ics#":
            guard d.count >= maskBytes * 2 else { throw bad }
            img = colorIcon(size: size, depth: 1, pixels: Array(d[0..<maskBytes]),
                            mask: Array(d[maskBytes..<maskBytes * 2]))
        default: // icl4, icl8, ics4, ics8
            let depth = type.hasSuffix("8") ? 8 : 4
            guard d.count >= size * size * depth / 8 else { throw bad }
            img = colorIcon(size: size, depth: depth, pixels: d, mask: familyMask())
        }
        return try writePNG(r, img.rgba, img.size, img.size, alpha: true)
    }
}
