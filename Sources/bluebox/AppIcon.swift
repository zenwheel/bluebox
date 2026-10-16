#if canImport(AppKit)
import AppKit

/// Blue Box's icon: a compact Mac with the happy face on a blue tile, drawn
/// at any size.  The shell shows it in the Dock; make-iconset writes it for
/// Blue Box.app (scripts/make-app.sh).
func blueBoxIcon(pixels px: Int) -> NSBitmapImageRep? {
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                                     samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                     colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return nil }
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    defer { NSGraphicsContext.restoreGraphicsState() }
    let s = CGFloat(px) / 1024
    func r(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> NSRect {
        NSRect(x: x * s, y: y * s, width: w * s, height: h * s)
    }
    func p(_ x: CGFloat, _ y: CGFloat) -> NSPoint { NSPoint(x: x * s, y: y * s) }

    // The tile: macOS's rounded square, blue.
    let tile = NSBezierPath(roundedRect: r(100, 100, 824, 824), xRadius: 185 * s, yRadius: 185 * s)
    NSGradient(starting: NSColor(calibratedRed: 0.33, green: 0.62, blue: 0.98, alpha: 1),
               ending: NSColor(calibratedRed: 0.10, green: 0.33, blue: 0.80, alpha: 1))?.draw(in: tile, angle: -90)

    // The Mac: a beige case with a foot, a screen, a floppy slot.
    let beige = NSColor(calibratedRed: 0.93, green: 0.90, blue: 0.82, alpha: 1)
    let shade = NSColor(calibratedRed: 0.72, green: 0.69, blue: 0.62, alpha: 1)
    shade.setFill()
    NSBezierPath(roundedRect: r(322, 205, 380, 60), xRadius: 14 * s, yRadius: 14 * s).fill()
    beige.setFill()
    NSBezierPath(roundedRect: r(302, 240, 420, 560), xRadius: 34 * s, yRadius: 34 * s).fill()
    shade.setFill()
    NSBezierPath(roundedRect: r(460, 318, 180, 18), xRadius: 9 * s, yRadius: 9 * s).fill()
    NSBezierPath(roundedRect: r(342, 395, 340, 360), xRadius: 26 * s, yRadius: 26 * s).fill()
    NSColor(calibratedRed: 0.16, green: 0.18, blue: 0.22, alpha: 1).setFill()
    NSBezierPath(roundedRect: r(362, 415, 300, 320), xRadius: 18 * s, yRadius: 18 * s).fill()

    // The happy face, in light lines on the screen.
    NSColor(calibratedRed: 0.85, green: 0.92, blue: 1.0, alpha: 1).setStroke()
    let line = NSBezierPath()
    line.lineWidth = 16 * s
    line.lineCapStyle = .round
    line.lineJoinStyle = .round
    line.move(to: p(452, 660)); line.line(to: p(452, 615)) // eyes
    line.move(to: p(572, 660)); line.line(to: p(572, 615))
    line.move(to: p(517, 650)); line.line(to: p(497, 560)); line.line(to: p(522, 560)) // nose
    line.move(to: p(447, 515))                                                        // smile
    line.curve(to: p(577, 515), controlPoint1: p(482, 470), controlPoint2: p(542, 470))
    line.stroke()
    return rep
}

/// The icon as an image for the Dock.
func blueBoxIconImage() -> NSImage {
    let image = NSImage(size: NSSize(width: 512, height: 512))
    for px in [128, 256, 512, 1024] {
        if let rep = blueBoxIcon(pixels: px) {
            rep.size = NSSize(width: 512, height: 512)
            image.addRepresentation(rep)
        }
    }
    return image
}

/// bluebox make-iconset <folder>.iconset: the icon at the sizes iconutil wants.
func makeIconsetCommand(_ args: [String]) -> Never {
    guard let out = args.first else { fail("usage: bluebox make-iconset <folder>.iconset") }
    try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
    for base in [16, 32, 128, 256, 512] {
        for scale in [1, 2] {
            let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
            guard let png = blueBoxIcon(pixels: base * scale)?.representation(using: .png, properties: [:]) else {
                fail("could not draw the icon")
            }
            do { try png.write(to: URL(fileURLWithPath: out + "/" + name)) } catch { fail("\(out): \(error)") }
        }
    }
    exit(0)
}
#endif
