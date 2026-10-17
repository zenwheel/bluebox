#if canImport(AppKit)
import AppKit

/// Blue Box's icon: a parcel, a box in deep blue taped shut with lighter
/// blue tape, on a light blue tile, drawn at any size.  The shell shows it
/// in the Dock; make-iconset writes it for Blue Box.app (scripts/make-app.sh).
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
    func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> NSColor {
        NSColor(calibratedRed: r, green: g, blue: b, alpha: a)
    }
    func shape(_ points: [NSPoint], _ fill: NSColor, stroke: NSColor? = nil, width: CGFloat = 0) {
        let path = NSBezierPath()
        path.move(to: points[0])
        for q in points.dropFirst() { path.line(to: q) }
        path.close()
        fill.setFill()
        path.fill()
        if let stroke {
            stroke.setStroke()
            path.lineWidth = width * s
            path.lineJoinStyle = .round
            path.stroke()
        }
    }

    // The tile: macOS's rounded square, light blue.
    let tile = NSBezierPath(roundedRect: r(100, 100, 824, 824), xRadius: 185 * s, yRadius: 185 * s)
    NSGradient(starting: rgb(0.80, 0.90, 1.00), ending: rgb(0.56, 0.76, 0.98))?.draw(in: tile, angle: -90)

    // Its shadow on the tile.
    rgb(0.10, 0.25, 0.55, 0.22).setFill()
    NSBezierPath(ovalIn: r(232, 186, 560, 120)).fill()

    // The box, seen from above and to the front: the top, the left and the
    // right faces, deep blue in three shades.
    let back = p(512, 790), left = p(232, 655), front = p(512, 520), right = p(792, 655)
    let drop: CGFloat = 300
    func down(_ q: NSPoint) -> NSPoint { NSPoint(x: q.x, y: q.y - drop * s) }
    let edge = rgb(0.02, 0.12, 0.40, 0.55)
    shape([left, front, down(front), down(left)], rgb(0.07, 0.33, 0.86), stroke: edge, width: 6)
    shape([front, right, down(right), down(front)], rgb(0.03, 0.21, 0.64), stroke: edge, width: 6)
    // The top: two flaps closed together, meeting along the middle (from the
    // back-left edge to the front-right one), the far one in a little more
    // light than the near one.
    let m1 = p(372, 722.5), m2 = p(652, 587.5)
    shape([back, m1, m2, right], rgb(0.25, 0.53, 1.00))
    shape([m1, left, front, m2], rgb(0.19, 0.46, 0.96))
    shape([back, left, front, right], .clear, stroke: edge, width: 6)

    // The tape: lighter blue, along the seam and a little way down the right face.
    let a1 = p(372, 722.5), a2 = p(652, 587.5)  // the seam's ends, on the back-left and front-right edges
    let w = NSPoint(x: 42 * s, y: 20.25 * s)      // half the tape's width, along the top's other edges
    func add(_ q: NSPoint, _ v: NSPoint, _ k: CGFloat) -> NSPoint { NSPoint(x: q.x + v.x * k, y: q.y + v.y * k) }
    let tape = rgb(0.70, 0.85, 1.00), tapeSide = rgb(0.56, 0.73, 0.96)
    shape([add(a1, w, -1), add(a1, w, 1), add(a2, w, 1), add(a2, w, -1)], tape)
    // A slight crease down the middle of the tape, over the flaps' seam.
    let crease = NSBezierPath()
    crease.move(to: a1); crease.line(to: a2)
    rgb(0.48, 0.66, 0.92, 0.9).setStroke(); crease.lineWidth = 4 * s; crease.stroke()
    let shine = NSBezierPath()
    shine.move(to: NSPoint(x: a1.x + 2 * s, y: a1.y + 4 * s)); shine.line(to: NSPoint(x: a2.x + 2 * s, y: a2.y + 4 * s))
    rgb(0.88, 0.95, 1.00, 0.9).setStroke(); shine.lineWidth = 3 * s; shine.stroke()
    // Down the right face only about 15% of the way, ending in a torn edge.
    let tear: CGFloat = 0.15 * drop * s
    let t0 = add(a2, w, -1), t1 = add(a2, w, 1)
    var torn = [t0, t1]
    let teeth = 7
    for i in 0...teeth { /* from t1's side back to t0's, zigzagging up and down */
        let f = CGFloat(teeth - i) / CGFloat(teeth)
        let x = t0.x + (t1.x - t0.x) * f, y = t0.y + (t1.y - t0.y) * f
        let jag: CGFloat = (i % 2 == 0 ? 0 : 9) * s + (i % 3 == 1 ? 4 * s : 0)
        torn.append(NSPoint(x: x, y: y - tear + jag))
    }
    shape(torn, tapeSide)
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
