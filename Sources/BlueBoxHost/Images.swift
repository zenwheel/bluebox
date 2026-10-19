// Still images for the Toolbox, decoded by ImageIO: the frames of
// QuickTime's Photo-JPEG codec.

#if canImport(ImageIO)
import CoreGraphics
import Foundation
import ImageIO
import Toolbox

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
    }
}
#endif
