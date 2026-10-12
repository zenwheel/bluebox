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

/// An image for the Dock: the large icon blown up crisply, plus the small one.
public func dockImage(_ icons: [IconImage]) -> NSImage? {
    guard let large = icons.first(where: { $0.size == 32 }) ?? icons.first else { return nil }
    let image = NSImage(size: NSSize(width: 512, height: 512))
    if let rep = bitmapRep(scaled(large, to: 512)) {
        rep.size = NSSize(width: 512, height: 512)
        image.addRepresentation(rep)
    }
    return image
}

/// An .icns file: PNG entries at every size the Finder asks for, all
/// nearest-neighbour scaled from the 32 and 16 pixel originals.
public func icnsData(_ icons: [IconImage]) -> Data? {
    guard let large = icons.first(where: { $0.size == 32 }) ?? icons.first else { return nil }
    let small = icons.first(where: { $0.size == 16 })
    var entries: [(String, IconImage)] = [
        ("icp5", scaled(large, to: 32)), ("ic11", scaled(large, to: 32)), ("ic12", scaled(large, to: 64)),
        ("ic07", scaled(large, to: 128)), ("ic13", scaled(large, to: 256)), ("ic08", scaled(large, to: 256)),
        ("ic14", scaled(large, to: 512)), ("ic09", scaled(large, to: 512)), ("ic10", scaled(large, to: 1024)),
    ]
    if let small { entries.insert(("icp4", small), at: 0) }
    var body = Data()
    for (type, img) in entries {
        guard let png = pngData(img) else { return nil }
        body.append(contentsOf: Array(type.utf8))
        body.append(bigEndian32(UInt32(png.count + 8)))
        body.append(png)
    }
    var file = Data("icns".utf8)
    file.append(bigEndian32(UInt32(body.count + 8)))
    file.append(body)
    return file
}

private func bigEndian32(_ v: UInt32) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }

#endif
