// The thumbnail of a classic file: its own icon (custom, or from its own
// bundle), or the one its application's bundle gives documents of its type,
// found in Blue Box's library and on its Macintosh HD.  A file with no
// classic side, or none of these icons, gets no thumbnail from here, and
// the Finder draws its own.

#if canImport(QuickLookThumbnailing)
import ClassicFiles
import Foundation
import QuickLookThumbnailing

/// The user's real home folder (the sandbox's HOME is the extension's
/// container); the entitlements let it read Blue Box's folders there.
func realHome() -> String {
    if let pw = getpwuid(getuid()), let dir = pw.pointee.pw_dir { return String(cString: dir) }
    return NSHomeDirectory()
}

@objc(BBThumbnailProvider)
final class ThumbnailProvider: QLThumbnailProvider {
    static let lookup = ClassicIconLookup(
        applications: ClassicApplicationIndex(roots: ClassicApplicationIndex.standardRoots(home: realHome())))

    override func provideThumbnail(for request: QLFileThumbnailRequest,
                                   _ handler: @escaping (QLThumbnailReply?, Error?) -> Void) {
        let path = request.fileURL.path
        guard let contents = ClassicFileContents(path: path) else {
            handler(nil, CocoaError(.fileReadUnsupportedScheme))
            return
        }
        let icons = Self.lookup.icons(forFileAt: path, meta: contents.meta)
        guard !icons.isEmpty else {
            handler(nil, CocoaError(.fileReadUnsupportedScheme))
            return
        }
        let side = min(request.maximumSize.width, request.maximumSize.height)
        let size = CGSize(width: side, height: side)
        handler(QLThumbnailReply(contextSize: size) { ctx in
            drawIcon(icons, in: ctx, size: size)
            return true
        }, nil)
    }
}
#endif
