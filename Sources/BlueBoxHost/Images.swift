// Still images for the Toolbox through ImageIO: the frames of QuickTime's
// Photo-JPEG codec, decoded, and encoded as baseline JPEG for its
// compressor.

#if canImport(ImageIO)
import CoreGraphics
import Foundation
import ImageIO
import Toolbox
import UniformTypeIdentifiers

public enum ClassicImages {
    public static func install() {
        machine_set_image_decoder({ data, len, rgba, w, h, _ in
            guard let data, let rgba, let w, let h,
                  let source = CGImageSourceCreateWithData(Data(bytes: data, count: len) as CFData, nil),
                  let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return false }
            let width = image.width, height = image.height
            guard width > 0, height > 0, let out = malloc(width * height * 4) else { return false }
            guard let ctx = CGContext(data: out, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
                free(out)
                return false
            }
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            let bytes = out.assumingMemoryBound(to: UInt8.self)
            for i in 0..<width * height { bytes[i * 4 + 3] = 255 }
            rgba.pointee = bytes
            w.pointee = Int32(width)
            h.pointee = Int32(height)
            return true
        }, nil)
        machine_set_image_encoder({ rgba, w, h, quality, out, len, _ in
            guard let rgba, let out, let len, w > 0, h > 0 else { return false }
            let width = Int(w), height = Int(h)
            guard let provider = CGDataProvider(data: Data(bytes: rgba, count: width * height * 4) as CFData),
                  let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                      bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                                      provider: provider, decode: nil, shouldInterpolate: false,
                                      intent: .defaultIntent) else { return false }
            let encoded = NSMutableData()
            guard let dest = CGImageDestinationCreateWithData(encoded as CFMutableData,
                                                              UTType.jpeg.identifier as CFString, 1, nil)
            else { return false }
            CGImageDestinationAddImage(dest, image,
                                       [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
            guard CGImageDestinationFinalize(dest), encoded.length > 0,
                  let bytes = malloc(encoded.length) else { return false }
            memcpy(bytes, encoded.bytes, encoded.length)
            out.pointee = bytes.assumingMemoryBound(to: UInt8.self)
            len.pointee = encoded.length
            return true
        }, nil)
    }
}
#endif
