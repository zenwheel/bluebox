// The Blue Box Browser's rules that don't need AppKit, kept here so the
// tests reach them: which part of a name an inline rename selects and
// which names it refuses, the browser's own pasteboard type of paths
// (which a run's window also takes, so a drag from the browser opens in
// the program), the facts its preview column shows, and when Quick Look
// is given Blue Box's own preview of a classic file.

import Foundation
#if canImport(UniformTypeIdentifiers)
import UniformTypeIdentifiers
#endif

public enum BrowserNames {
    /// The part of a name selected when it is edited in place: a file's
    /// name before its extension, as the Finder selects it; all of a
    /// folder's, or of a name whose "extension" is no extension (long, or
    /// with a space in it, or the whole of a dot name).
    public static func editableRange(_ name: String, isFolder: Bool) -> NSRange {
        let ns = name as NSString
        let all = NSRange(location: 0, length: ns.length)
        let ext = ns.pathExtension
        guard !isFolder, !ext.isEmpty, ext.count <= 5, !ext.contains(" "),
              ns.length > (ext as NSString).length + 1 else { return all }
        return NSRange(location: 0, length: ns.length - (ext as NSString).length - 1)
    }

    /// Why a new name can't be used, or nil when it can.
    public static func problem(_ name: String) -> String? {
        if name.trimmingCharacters(in: .whitespaces).isEmpty { return "A name is needed." }
        if name.contains("/") { return "Use a name that doesn't contain “/”." }
        if name == "." || name == ".." || name.hasPrefix("._") {
            return "“\(name)” is a name the system keeps for itself."
        }
        if name.utf8.count > 255 { return "The name is too long." }
        return nil
    }
}

/// The paths a browser drags or copies, on a pasteboard of its own type
/// beside the file promise: a program's window in a Blue Box run takes
/// them as the files they are (an Open Documents event, or its own drag
/// handlers), where the promise would give it a copy.
public enum BrowserPaths {
    public static let pasteboardType = "com.bluebox.browser-paths"

    /// The paths in the strings of the pasteboard's items (one each from a
    /// drag, a line each from Copy), absolute ones only, each once.
    public static func parse(_ strings: [String]) -> [String] {
        var seen = Set<String>(), out: [String] = []
        for s in strings {
            for line in s.split(whereSeparator: \.isNewline) {
                let p = String(line)
                if p.hasPrefix("/"), seen.insert(p).inserted { out.append(p) }
            }
        }
        return out
    }
}

/// What the column view's preview column, and Get Info, say of a file.
public enum ClassicItemFacts {
    public static func rows(kind: String, dataSize: Int, resourceSize: Int, created: Date, modified: Date,
                            type: UInt32, creator: UInt32, isClassic: Bool,
                            dates: DateFormatter) -> [(label: String, value: String)] {
        func bytes(_ n: Int) -> String { ByteCountFormatter.string(fromByteCount: Int64(n), countStyle: .file) }
        var size = bytes(dataSize + resourceSize)
        if resourceSize > 0 { size += " (data \(bytes(dataSize)), resource \(bytes(resourceSize)))" }
        var rows: [(String, String)] = [("Kind", kind), ("Size", size),
                                        ("Created", dates.string(from: created)), ("Modified", dates.string(from: modified))]
        if isClassic && (type != 0 || creator != 0) {
            rows.append(("Type", "‘\(fourCCText(type))’"))
            rows.append(("Creator", "‘\(fourCCText(creator))’"))
        }
        return rows
    }
}

extension ClassicPreviewPage {
    /// Whether Quick Look should be given Blue Box's own preview of a
    /// file rather than its own: a classic file whose name says nothing
    /// macOS can show (no extension, or one of a type that isn't content:
    /// an archive, data).  A classic JPEG or text file with its extension
    /// is shown by Quick Look as it is.
    public static func wanted(name: String, isClassic: Bool, isFolder: Bool) -> Bool {
        guard isClassic, !isFolder else { return false }
        let ext = (name as NSString).pathExtension
        guard !ext.isEmpty, ext.count <= 5 else { return true }
        #if canImport(UniformTypeIdentifiers)
        guard let type = UTType(filenameExtension: ext), !type.isDynamic else { return true }
        return !type.conforms(to: .content)
        #else
        return true
        #endif
    }

    /// The page as one HTML file, its pictures and sounds inline (data:
    /// URLs), for a viewer given a file (the browser's Quick Look panel).
    public var selfContainedHTML: String {
        var out = html
        for (name, a) in attachments {
            let mime: String
            switch a.contentType {
            case "public.png": mime = "image/png"
            case "com.microsoft.waveform-audio": mime = "audio/wav"
            default: mime = "application/octet-stream"
            }
            out = out.replacingOccurrences(of: "\"cid:\(name)\"", with: "\"data:\(mime);base64,\(a.data.base64EncodedString())\"")
        }
        return out
    }
}
