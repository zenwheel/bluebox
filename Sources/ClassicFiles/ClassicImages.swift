// Classic icons and pictures as CoreGraphics images and PNG (macOS), for
// Quick Look and the browser, without AppKit (which app extensions may
// use only in part).

import CCore
import Foundation
import ForkStore
#if canImport(CoreGraphics) && canImport(ImageIO)
import CoreGraphics
import ImageIO
#endif

/// A classic file's side and where its data fork is: a file with a sidecar
/// or forks of its own, or a MacBinary file (.bin) holding both.
public struct ClassicFileContents {
    public var meta: ClassicMeta
    /// The Mac name: the MacBinary header's, else the host file's.
    public var name: String
    /// The data fork's length.
    public var dataLength: Int
    private let macBinaryData: [UInt8]?
    private let path: String

    /// nil for a file with no classic side.
    public init?(path: String) {
        self.path = path
        name = (path as NSString).lastPathComponent
        if hasClassicSide(path), let meta = ClassicMeta.read(path), meta.isClassic {
            self.meta = meta
            var st = stat()
            dataLength = stat(path, &st) == 0 ? Int(st.st_size) : 0
            macBinaryData = nil
            return
        }
        // MacBinary: only the header is read to decide.
        guard let h = FileHandle(forReadingAtPath: path) else { return nil }
        let head = Array((try? h.read(upToCount: 128)) ?? Data())
        try? h.close()
        guard head.count == 128, head.withUnsafeBufferPointer({ macbinary_detect($0.baseAddress, $0.count) }),
              let whole = try? Data(contentsOf: URL(fileURLWithPath: path)) else { return nil }
        var mf = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        let bytes = Array(whole)
        guard bytes.withUnsafeBufferPointer({ macbinary_parse($0.baseAddress, $0.count, &mf, &err, err.count) })
        else { return nil }
        defer { mac_file_free(&mf) }
        meta = ClassicMeta(mf)
        dataLength = mf.data_len
        macBinaryData = mf.data.map { Array(UnsafeBufferPointer(start: $0, count: mf.data_len)) } ?? []
        let n = macRomanText(meta.name)
        if !n.isEmpty { name = n }
    }

    public var isMacBinary: Bool { macBinaryData != nil }

    /// The data fork, or its first max bytes.
    public func data(max: Int = Int.max) -> [UInt8] {
        if let macBinaryData { return Array(macBinaryData.prefix(max)) }
        guard let h = FileHandle(forReadingAtPath: path) else { return [] }
        defer { try? h.close() }
        return Array((try? h.read(upToCount: max)) ?? Data())
    }
}

#if canImport(CoreGraphics) && canImport(ImageIO)
/// An RGBA image (not premultiplied: alpha 0 or 255, as icons and drawn
/// pictures are) as a CGImage.
public func cgImage(rgba: [UInt8], width: Int, height: Int) -> CGImage? {
    guard width > 0, height > 0, rgba.count >= width * height * 4,
          let provider = CGDataProvider(data: Data(rgba) as CFData) else { return nil }
    return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                   space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                   provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
}

public func cgImage(_ icon: IconImage) -> CGImage? { cgImage(rgba: icon.rgba, width: icon.size, height: icon.size) }

/// PNG of an RGBA image.
public func pngData(rgba: [UInt8], width: Int, height: Int) -> Data? {
    guard let image = cgImage(rgba: rgba, width: width, height: height) else { return nil }
    let out = NSMutableData()
    guard let dest = CGImageDestinationCreateWithData(out as CFMutableData, "public.png" as CFString, 1, nil)
    else { return nil }
    CGImageDestinationAddImage(dest, image, nil)
    return CGImageDestinationFinalize(dest) ? out as Data : nil
}

/// Draw an icon (pixel art) as large as fits a square of side points in
/// ctx, centred, in whole multiples of its pixels where it can.
public func drawIcon(_ icons: [IconImage], in ctx: CGContext, size: CGSize) {
    guard let large = icons.max(by: { $0.size < $1.size }), let image = cgImage(large) else { return }
    let side = min(size.width, size.height)
    let whole = floor(side / CGFloat(large.size))
    let drawn = whole >= 1 ? whole * CGFloat(large.size) : side
    ctx.interpolationQuality = .none
    ctx.draw(image, in: CGRect(x: (size.width - drawn) / 2, y: (size.height - drawn) / 2, width: drawn, height: drawn))
}
#endif
