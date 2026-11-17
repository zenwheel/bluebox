// The Quick Look preview of a classic file (Blue Box Preview.appex shows
// it in the macOS Finder): its icon, kind, type and creator, both forks'
// sizes, label and comment; a text file's text; its pictures and sounds,
// decoded by Blue Box's own QuickDraw and Sound Manager (a machine booted
// for them, as bluebox inspect does); and what its resource fork holds,
// with inspect's summary of each kind.  An HTML page whose pictures and
// sounds are attachments ("cid:" URLs), as QLPreviewReply takes them.

import CCore
import Foundation
import ForkStore
import Toolbox

public struct ClassicPreviewPage {
    public struct Attachment {
        public let data: Data
        public let contentType: String // a UTI
    }

    public var html = ""
    public var attachments: [String: Attachment] = [:]
    /// What the page shows, for tests and the page's size.
    public var pictures = 0, sounds = 0, textLength = 0

    /// How much of a text file the page shows, and how many pictures and
    /// sounds.
    static let maxText = 64 * 1024, maxPictures = 6, maxSounds = 4

    /// nil for a file with no classic side.
    public init?(path: String, lookup: ClassicIconLookup, decode: Bool = true) {
        guard let file = ClassicFileContents(path: path) else { return nil }
        let meta = file.meta
        var body = ""
        let icons = lookup.icons(forFileAt: path, meta: meta)
        var iconHTML = ""
        if let small = icon(icons, at: 32), case let big = scaled(small, to: 64),
           let png = pngData(rgba: big.rgba, width: 64, height: 64) {
            attachments["icon.png"] = Attachment(data: png, contentType: "public.png")
            iconHTML = "<img class=icon src=\"cid:icon.png\" width=64 height=64>"
        }
        let type = fourCCText(meta.type), creator = fourCCText(meta.creator)
        body += "<header>\(iconHTML)<div><h1>\(esc(file.name))</h1><p class=kind>\(esc(Self.kind(meta)))</p></div></header>"
        var rows: [(String, String)] = [
            ("Type", "'\(type)'"), ("Creator", "'\(creator)'"),
            ("Data fork", byteCount(file.dataLength)), ("Resource fork", byteCount(meta.resourceFork.count)),
        ]
        if meta.label != 0 { rows.append(("Label", ClassicLabel.all[meta.label].name)) }
        let flagNames = [(FinderFlags.isAlias, "alias"), (FinderFlags.isStationery, "stationery pad"),
                         (FinderFlags.isInvisible, "invisible"), (FinderFlags.hasCustomIcon, "custom icon"),
                         (FinderFlags.nameLocked, "name locked")].filter { meta.flags & $0.0 != 0 }.map(\.1)
        if !flagNames.isEmpty { rows.append(("Finder flags", flagNames.joined(separator: ", "))) }
        if !meta.comment.isEmpty { rows.append(("Comment", meta.commentText)) }
        if file.isMacBinary { rows.append(("Packed as", "MacBinary")) }
        body += "<table class=info>" + rows.map { "<tr><th>\(esc($0.0))</th><td>\(esc($0.1))</td></tr>" }.joined()
            + "</table>"

        // A text file's text (TEXT, SimpleText's read-only ttro).
        if ["TEXT", "ttro", "sEXT"].contains(type) {
            let bytes = file.data(max: Self.maxText)
            let text = macRomanText(bytes).replacingOccurrences(of: "\r\n", with: "\n")
                .replacingOccurrences(of: "\r", with: "\n")
            textLength = text.count
            body += "<h2>Text</h2><pre>\(esc(text))\(file.dataLength > Self.maxText ? "\n…" : "")</pre>"
        }

        let resources = meta.resourceFork.withUnsafeBufferPointer {
            (try? inspectResources(fork: $0.baseAddress, length: $0.count)) ?? []
        }
        if decode {
            var pictureData: [(String, [UInt8])] = []
            // A PICT file: its picture after the 512-byte header.
            if type == "PICT" {
                let d = file.data(max: 16 << 20)
                if d.count > 512 + 10 { pictureData.append(("Picture", Array(d[512...]))) }
            }
            for r in resources where r.typeText == "PICT" && pictureData.count < Self.maxPictures {
                pictureData.append(("PICT \(r.id)" + (r.name.isEmpty ? "" : " “\(r.name)”"), r.data))
            }
            let soundData = resources.filter { $0.typeText == "snd " }.prefix(Self.maxSounds)
            // Without a machine (its memory refused), the page goes without them.
            if (!pictureData.isEmpty || !soundData.isEmpty) && !Self.bootMachine() {
                pictureData = []
            }
            let soundList = Self.booted ? Array(soundData) : []
            var shown = ""
            for (i, (title, data)) in pictureData.enumerated() {
                guard let png = Self.picturePNG(data) else { continue }
                let name = "pict\(i).png"
                attachments[name] = Attachment(data: png.data, contentType: "public.png")
                shown += "<figure><img src=\"cid:\(name)\" width=\(png.width) height=\(png.height)>"
                    + "<figcaption>\(esc(title))</figcaption></figure>"
                pictures += 1
            }
            if !shown.isEmpty { body += "<h2>Pictures</h2>" + shown }
            var played = ""
            for (i, r) in soundList.enumerated() {
                guard let wav = Self.soundWAV(r.data) else { continue }
                let name = "snd\(i).wav"
                attachments[name] = Attachment(data: wav, contentType: "com.microsoft.waveform-audio")
                played += "<figure><audio controls src=\"cid:\(name)\"></audio><figcaption>snd \(r.id)"
                    + (r.name.isEmpty ? "" : " “\(esc(r.name))”") + "</figcaption></figure>"
                sounds += 1
            }
            if !played.isEmpty { body += "<h2>Sounds</h2>" + played }
        }

        // The resource fork: each type, how many, their size, and the first
        // one's summary.
        if !resources.isEmpty {
            var byType: [(type: String, list: [InspectedResource])] = []
            for r in resources {
                if let i = byType.firstIndex(where: { $0.type == r.typeText }) {
                    byType[i].list.append(r)
                } else {
                    byType.append((r.typeText, [r]))
                }
            }
            body += "<h2>Resources</h2><table class=rsrc><tr><th>Type</th><th>Count</th><th>Size</th><th>First</th></tr>"
            for t in byType {
                let first = t.list[0]
                let summary = resourceSummary(first).map { " " + $0 } ?? ""
                let label = "\(first.id)" + (first.name.isEmpty ? "" : " “\(first.name)”") + summary
                body += "<tr><td><code>\(esc(t.type))</code></td><td>\(t.list.count)</td>"
                    + "<td>\(byteCount(t.list.reduce(0) { $0 + $1.storedSize }))</td><td>\(esc(label))</td></tr>"
            }
            body += "</table>"
        }
        html = "<!doctype html><html><head><meta charset=utf-8><style>\(Self.style)</style></head><body>"
            + body + "</body></html>"
    }

    /// What the file is, in the Finder's words.
    static func kind(_ meta: ClassicMeta) -> String {
        switch fourCCText(meta.type) {
        case "APPL": return "Classic application program"
        case "appe": return "Classic background application"
        case "cdev": return "Control panel"
        case "INIT": return "System extension"
        case "dfil": return "Desk accessory"
        case "FFIL", "tfil", "ffil": return "Font"
        case "shlb": return "Shared library"
        case "PICT": return "Picture document"
        case "TEXT", "ttro": return "Text document"
        case "fdrp": return "Folder alias"
        case "adrp": return "Application alias"
        default: return meta.isAlias ? "Alias" : "Classic document"
        }
    }

    // MARK: Decoding, in a machine of its own

    private static let machineLock = NSLock()
    nonisolated(unsafe) private static var booted = false

    /// A machine booted for the decoders (again for each page: a test or
    /// run between may have booted another, which serves as well).
    static func bootMachine() -> Bool {
        machineLock.lock()
        defer { machineLock.unlock() }
        booted = machine_boot(false)
        return booted
    }

    /// A picture through QuickDraw, as PNG.
    static func picturePNG(_ pict: [UInt8]) -> (data: Data, width: Int, height: Int)? {
        machineLock.lock()
        defer { machineLock.unlock() }
        var px: UnsafeMutablePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0
        guard pict.withUnsafeBufferPointer({ machine_picture_rgba($0.baseAddress, $0.count, &px, &w, &h) }), let px,
              w > 0, h > 0 else { return nil }
        defer { bb_free(px) }
        let rgba = Array(UnsafeBufferPointer(start: px, count: Int(w) * Int(h) * 4))
        guard let png = pngData(rgba: rgba, width: Int(w), height: Int(h)) else { return nil }
        return (png, Int(w), Int(h))
    }

    /// A sampled sound through the Sound Manager's decoders, as WAV.
    static func soundWAV(_ snd: [UInt8]) -> Data? {
        machineLock.lock()
        defer { machineLock.unlock() }
        var pcm: UnsafeMutablePointer<Int16>?
        var frames: UInt32 = 0
        var channels: Int32 = 0, bits: Int32 = 0
        var rate = 0.0
        guard snd.withUnsafeBufferPointer({ machine_snd_pcm($0.baseAddress, $0.count, &pcm, &frames, &channels, &rate, &bits) }),
              let pcm, frames > 0 else { return nil }
        defer { bb_free(pcm) }
        return wavData(pcm: UnsafeBufferPointer(start: pcm, count: Int(frames) * Int(channels)), channels: Int(channels),
                       rate: Int(rate.rounded()), bits: Int(bits))
    }

    static let style = """
        :root { color-scheme: light dark; font: 13px -apple-system, sans-serif; }
        body { margin: 20px; }
        header { display: flex; align-items: center; gap: 16px; }
        .icon { image-rendering: pixelated; }
        h1 { font-size: 18px; margin: 0; }
        h2 { font-size: 13px; margin: 20px 0 6px; color: GrayText; text-transform: uppercase; letter-spacing: .05em; }
        .kind { margin: 2px 0 0; color: GrayText; }
        table { border-collapse: collapse; }
        th, td { text-align: left; padding: 2px 12px 2px 0; vertical-align: top; }
        .info th { color: GrayText; font-weight: normal; }
        .rsrc th { border-bottom: 1px solid GrayText; }
        pre { white-space: pre-wrap; font: 12px ui-monospace, Menlo, monospace; }
        figure { display: inline-block; margin: 0 16px 12px 0; }
        figure img { image-rendering: pixelated; max-width: 100%; height: auto; }
        figcaption { color: GrayText; font-size: 11px; }
        """
}

func esc(_ s: String) -> String {
    s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
        .replacingOccurrences(of: ">", with: "&gt;").replacingOccurrences(of: "\"", with: "&quot;")
}

/// "1.2 KB (1,234 bytes)", as Get Info said sizes.
public func byteCount(_ n: Int) -> String {
    let f = NumberFormatter()
    f.numberStyle = .decimal
    let exact = f.string(from: NSNumber(value: n)) ?? "\(n)"
    if n < 1000 { return "\(exact) bytes" }
    return ByteCountFormatter.string(fromByteCount: Int64(n), countStyle: .file) + " (\(exact) bytes)"
}
