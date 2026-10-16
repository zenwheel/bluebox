// Draw Blue Box's application icon into an .iconset folder (for iconutil):
// a compact Mac with the happy face, on a blue tile.
//
// usage: swift scripts/make-icon.swift <folder>.iconset
import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "BlueBox.iconset"
try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)

func draw(_ px: Int) -> Data? {
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                                     samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                     colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return nil }
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px) / 1024
    func r(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> NSRect {
        NSRect(x: x * s, y: y * s, width: w * s, height: h * s)
    }

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
    let face = NSColor(calibratedRed: 0.85, green: 0.92, blue: 1.0, alpha: 1)
    face.setStroke()
    let line = NSBezierPath()
    line.lineWidth = 16 * s
    line.lineCapStyle = .round
    line.lineJoinStyle = .round
    line.move(to: NSPoint(x: 452 * s, y: 660 * s)); line.line(to: NSPoint(x: 452 * s, y: 615 * s))  // eyes
    line.move(to: NSPoint(x: 572 * s, y: 660 * s)); line.line(to: NSPoint(x: 572 * s, y: 615 * s))
    line.move(to: NSPoint(x: 517 * s, y: 650 * s))                                                    // nose
    line.line(to: NSPoint(x: 497 * s, y: 560 * s)); line.line(to: NSPoint(x: 522 * s, y: 560 * s))
    line.move(to: NSPoint(x: 447 * s, y: 515 * s))                                                    // smile
    line.curve(to: NSPoint(x: 577 * s, y: 515 * s), controlPoint1: NSPoint(x: 482 * s, y: 470 * s),
               controlPoint2: NSPoint(x: 542 * s, y: 470 * s))
    line.stroke()

    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])
}

for base in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
        if let png = draw(base * scale) {
            try png.write(to: URL(fileURLWithPath: out + "/" + name))
        }
    }
}
