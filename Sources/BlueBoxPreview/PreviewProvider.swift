// The preview of a classic file: BlueBoxHost's ClassicPreviewPage (its
// icon, Finder information, text, pictures, sounds and resources) as a
// data-based HTML preview.  A file with no classic side is refused, and
// Quick Look shows its own.

#if canImport(QuickLookUI)
import BlueBoxHost
import Foundation
import QuickLookUI
import UniformTypeIdentifiers

/// The user's real home folder (the sandbox's HOME is the extension's
/// container); the entitlements let it read Blue Box's folders there.
func realHome() -> String {
    if let pw = getpwuid(getuid()), let dir = pw.pointee.pw_dir { return String(cString: dir) }
    return NSHomeDirectory()
}

@objc(BBPreviewProvider)
final class PreviewProvider: QLPreviewProvider, QLPreviewingController {
    static let lookup = ClassicIconLookup(
        applications: ClassicApplicationIndex(roots: ClassicApplicationIndex.standardRoots(home: realHome())))

    func providePreview(for request: QLFilePreviewRequest) async throws -> QLPreviewReply {
        guard let page = ClassicPreviewPage(path: request.fileURL.path, lookup: Self.lookup) else {
            throw CocoaError(.fileReadUnsupportedScheme)
        }
        let reply = QLPreviewReply(dataOfContentType: .html, contentSize: CGSize(width: 720, height: 640)) { reply in
            reply.stringEncoding = .utf8
            reply.attachments = page.attachments.mapValues {
                QLPreviewReplyAttachment(data: $0.data, contentType: UTType($0.contentType) ?? .data)
            }
            return Data(page.html.utf8)
        }
        return reply
    }
}
#endif
