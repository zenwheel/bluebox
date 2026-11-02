// Icons as files and images for macOS: PNG, .icns and the Dock image.

#if canImport(AppKit)
import AppKit

public func pngData(_ img: IconImage) -> Data? {
    guard let rep = bitmapRep(img) else { return nil }
    return rep.representation(using: .png, properties: [:])
}

private func bitmapRep(_ img: IconImage) -> NSBitmapImageRep? {
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: img.size, pixelsHigh: img.size,
                                     bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                     colorSpaceName: .deviceRGB, bytesPerRow: img.size * 4, bitsPerPixel: 32),
          let dst = rep.bitmapData else { return nil }
    img.rgba.withUnsafeBufferPointer { dst.update(from: $0.baseAddress!, count: img.rgba.count) }
    return rep
}

/// An image for the Dock: the large icon blown up crisply.
public func dockImage(_ icons: [IconImage]) -> NSImage? {
    guard let large = icons.max(by: { $0.size < $1.size }) else { return nil }
    let image = NSImage(size: NSSize(width: 512, height: 512))
    if let rep = bitmapRep(scaled(large, to: 512)) {
        rep.size = NSSize(width: 512, height: 512)
        image.addRepresentation(rep)
    }
    return image
}

/// An .icns file: PNG entries at every size the Finder asks for, from
/// the icons drawn for them or the largest (icon(_:at:)).
public func icnsData(_ icons: [IconImage]) -> Data? {
    guard !icons.isEmpty else { return nil }
    var body = Data()
    // 16 and 32 pixels as ARGB, as iconutil writes them (macOS reads its
    // icp4 and icp5 PNGs wrongly); the rest as PNG.
    for (type, size) in [("ic04", 16), ("ic05", 32), ("ic11", 32), ("ic12", 64), ("ic07", 128), ("ic13", 256),
                         ("ic08", 256), ("ic14", 512), ("ic09", 512), ("ic10", 1024)] {
        guard let img = icon(icons, at: size) else { continue }
        guard let data = size <= 32 && type != "ic11" ? argbData(img) : pngData(img) else { return nil }
        body.append(contentsOf: Array(type.utf8))
        body.append(bigEndian32(UInt32(data.count + 8)))
        body.append(data)
    }
    var file = Data("icns".utf8)
    file.append(bigEndian32(UInt32(body.count + 8)))
    file.append(body)
    return file
}

/// An icns ARGB image: "ARGB", then the alpha, red, green and blue planes
/// each run-length coded (here as runs of up to 128 bytes as they are).
private func argbData(_ img: IconImage) -> Data {
    var out = Data("ARGB".utf8)
    for channel in [3, 0, 1, 2] {
        let plane = stride(from: channel, to: img.rgba.count, by: 4).map { img.rgba[$0] }
        for start in stride(from: 0, to: plane.count, by: 128) {
            let run = plane[start..<min(start + 128, plane.count)]
            out.append(UInt8(run.count - 1))
            out.append(contentsOf: run)
        }
    }
    return out
}

private func bigEndian32(_ v: UInt32) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }

#endif
