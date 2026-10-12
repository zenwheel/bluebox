// Recorded QuickDraw pages to PDF.
//
// Shapes become paths with QuickDraw's pen and frame rules, patterns become
// grey levels, and text runs are set in a host font (with the LaserWriter's
// classic substitutions) scaled to the width the classic metrics gave, so
// lines break and align as they did on screen.

#if canImport(AppKit)
import AppKit
import CoreText
import Toolbox

public enum ClassicPrint {
    /// Render a print job as PDF data, one PDF page per recorded page.
    public static func pdf(_ job: UnsafePointer<machine_print_job>) -> Data {
        let j = job.pointee
        let paper = j.paper
        let size = CGSize(width: CGFloat(paper.right - paper.left), height: CGFloat(paper.bottom - paper.top))
        let data = NSMutableData()
        var box = CGRect(origin: .zero, size: size)
        guard let consumer = CGDataConsumer(data: data as CFMutableData),
              let ctx = CGContext(consumer: consumer, mediaBox: &box, nil) else { return Data() }
        for p in 0..<Int(j.npages) {
            ctx.beginPDFPage(nil)
            // QuickDraw coordinates: y down, 0,0 at the printable area's corner.
            ctx.translateBy(x: 0, y: size.height)
            ctx.scaleBy(x: 1, y: -1)
            ctx.translateBy(x: CGFloat(-paper.left), y: CGFloat(-paper.top))
            let page = j.pages[p]
            for i in 0..<Int(page.nops) { draw(page.ops[i], in: ctx) }
            ctx.endPDFPage()
        }
        ctx.closePDF()
        return data as Data
    }

    static func rect(_ r: mprect) -> CGRect {
        CGRect(x: CGFloat(r.left), y: CGFloat(r.top), width: CGFloat(r.right - r.left), height: CGFloat(r.bottom - r.top))
    }

    static func draw(_ op: machine_print_op, in ctx: CGContext) {
        guard op.nclip > 0 else { return } // clipped away entirely
        ctx.saveGState()
        defer { ctx.restoreGState() }
        ctx.clip(to: (0..<Int(op.nclip)).map { rect(op.clip[$0]) })
        if Int(op.kind) == Int(MACHINE_PRINT_TEXT) {
            text(op, in: ctx)
            return
        }
        if Int(op.kind) == Int(MACHINE_PRINT_IMAGE) {
            image(op, in: ctx)
            return
        }
        guard let (path, evenOdd) = shape(op) else { return }
        paint(path, evenOdd: evenOdd, op: op, in: ctx)
    }

    // MARK: Shapes

    static func shape(_ op: machine_print_op) -> (CGPath, Bool)? {
        let r = rect(op.r)
        let pw = CGFloat(op.pen_w), ph = CGFloat(op.pen_h)
        let frame = Int(op.verb) == Int(MACHINE_VERB_FRAME)
        let path = CGMutablePath()
        switch Int(op.kind) {
        case Int(MACHINE_PRINT_RECT):
            path.addRect(r)
            if frame { path.addRect(r.insetBy(dx: pw, dy: ph)) }
        case Int(MACHINE_PRINT_OVAL):
            path.addEllipse(in: r)
            if frame { path.addEllipse(in: r.insetBy(dx: pw, dy: ph)) }
        case Int(MACHINE_PRINT_RRECT):
            addRoundRect(path, r, CGFloat(op.a), CGFloat(op.b))
            if frame { addRoundRect(path, r.insetBy(dx: pw, dy: ph), CGFloat(op.a) - 2 * pw, CGFloat(op.b) - 2 * ph) }
        case Int(MACHINE_PRINT_ARC):
            let start = Double(op.a), angle = Double(op.b)
            if frame {
                let outer = arcPoints(r, start, angle), inner = arcPoints(r.insetBy(dx: pw, dy: ph), start, angle)
                path.addLines(between: outer + inner.reversed())
            } else {
                path.addLines(between: [CGPoint(x: r.midX, y: r.midY)] + arcPoints(r, start, angle))
            }
            path.closeSubpath()
            return (path, false)
        case Int(MACHINE_PRINT_REGION):
            for i in 0..<Int(op.nrects) { path.addRect(rect(op.rects[i])) }
            return (path, false)
        case Int(MACHINE_PRINT_LINE):
            guard op.pen_w > 0, op.pen_h > 0 else { return nil }
            path.addLines(between: penHull(op))
            path.closeSubpath()
            return (path, false)
        default:
            return nil
        }
        return (path, frame)
    }

    static func addRoundRect(_ path: CGMutablePath, _ r: CGRect, _ ow: CGFloat, _ oh: CGFloat) {
        guard r.width > 0, r.height > 0 else { return }
        let cw = max(0, min(ow / 2, r.width / 2)), ch = max(0, min(oh / 2, r.height / 2))
        path.addRoundedRect(in: r, cornerWidth: cw, cornerHeight: ch)
    }

    /// Points on an oval's edge: QuickDraw angles start at 12 o'clock and run clockwise.
    static func arcPoints(_ r: CGRect, _ start: Double, _ angle: Double) -> [CGPoint] {
        let steps = max(2, Int(abs(angle) / 2))
        return (0...steps).map { i in
            let a = (start + angle * Double(i) / Double(steps)) * .pi / 180
            return CGPoint(x: r.midX + r.width / 2 * CGFloat(sin(a)), y: r.midY - r.height / 2 * CGFloat(cos(a)))
        }
    }

    /// The area a rectangular pen covers moving from one end to the other.
    static func penHull(_ op: machine_print_op) -> [CGPoint] {
        let w = CGFloat(op.pen_w), h = CGFloat(op.pen_h)
        var pts: [CGPoint] = []
        for (x, y) in [(op.h0, op.v0), (op.h1, op.v1)] {
            let px = CGFloat(x), py = CGFloat(y)
            pts += [CGPoint(x: px, y: py), CGPoint(x: px + w, y: py), CGPoint(x: px, y: py + h), CGPoint(x: px + w, y: py + h)]
        }
        return convexHull(pts)
    }

    static func convexHull(_ points: [CGPoint]) -> [CGPoint] {
        let unique: Set<[CGFloat]> = Set(points.map { [$0.x, $0.y] })
        let p: [CGPoint] = unique.map { CGPoint(x: $0[0], y: $0[1]) }.sorted { (a: CGPoint, b: CGPoint) -> Bool in
            a.x != b.x ? a.x < b.x : a.y < b.y
        }
        guard p.count > 2 else { return p }
        func cross(_ o: CGPoint, _ a: CGPoint, _ b: CGPoint) -> CGFloat {
            (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x)
        }
        var lower: [CGPoint] = [], upper: [CGPoint] = []
        for pt in p {
            while lower.count >= 2 && cross(lower[lower.count - 2], lower[lower.count - 1], pt) <= 0 { lower.removeLast() }
            lower.append(pt)
        }
        for pt in p.reversed() {
            while upper.count >= 2 && cross(upper[upper.count - 2], upper[upper.count - 1], pt) <= 0 { upper.removeLast() }
            upper.append(pt)
        }
        return Array(lower.dropLast() + upper.dropLast())
    }

    // MARK: Patterns and transfer modes

    /// Fraction of a pattern's pixels that are set.
    static func coverage(_ pat: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8)) -> CGFloat {
        let bytes = [pat.0, pat.1, pat.2, pat.3, pat.4, pat.5, pat.6, pat.7]
        return CGFloat(bytes.reduce(0) { $0 + $1.nonzeroBitCount }) / 64
    }

    static func paint(_ path: CGPath, evenOdd: Bool, op: machine_print_op, in ctx: CGContext) {
        var cov = coverage(op.pattern)
        if op.mode & 4 != 0 { cov = 1 - cov }      // notPat / notSrc modes
        let ink: CGFloat = op.fg_black ? 0 : 1      // grey level of the foreground
        ctx.addPath(path)
        switch op.mode & 3 {
        case 0:                                      // copy: pattern over the background
            ctx.setFillColor(gray: 1 - cov * (1 - ink), alpha: 1)
        case 1:                                      // or: only the set pixels
            guard cov > 0 else { ctx.beginPath(); return }
            ctx.setFillColor(gray: ink, alpha: cov)
        case 2:                                      // xor: invert under the set pixels
            guard cov > 0 else { ctx.beginPath(); return }
            ctx.setBlendMode(.difference)
            ctx.setFillColor(gray: 1, alpha: cov)
        default:                                     // bic: clear under the set pixels
            guard cov > 0 else { ctx.beginPath(); return }
            ctx.setFillColor(gray: 1, alpha: cov)
        }
        ctx.fillPath(using: evenOdd ? .evenOdd : .winding)
    }

    // MARK: Images

    static func image(_ op: machine_print_op, in ctx: CGContext) {
        let w = Int(op.image_w), h = Int(op.image_h)
        guard w > 0, h > 0, let px = op.image else { return }
        let data = Data(bytes: px, count: w * h * 4)
        guard let provider = CGDataProvider(data: data as CFData),
              let img = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                space: CGColorSpaceCreateDeviceRGB(),
                                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return }
        let r = rect(op.r)
        // Enlarged bitmaps keep their pixels crisp, as on a LaserWriter.
        ctx.interpolationQuality = r.width >= CGFloat(w) ? .none : .high
        ctx.translateBy(x: r.minX, y: r.maxY)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(img, in: CGRect(origin: .zero, size: r.size))
    }

    // MARK: Text

    /// A host font for a classic family, as the LaserWriter substituted them.
    static func font(_ op: machine_print_op) -> NSFont {
        let size = CGFloat(op.size > 0 ? op.size : 12)
        let name = withUnsafeBytes(of: op.font_name) { raw in
            String(data: Data(raw.prefix { $0 != 0 }), encoding: .macOSRoman) ?? ""
        }
        let substitute: [Int16: String] = [
            0: "Helvetica-Bold", 2: "Times-Roman", 3: "Helvetica", 4: "Courier",
            20: "Times-Roman", 21: "Helvetica", 22: "Courier", 23: "Symbol",
        ]
        var f = substitute[op.font_id].flatMap { NSFont(name: $0, size: size) }
            ?? NSFont(name: name, size: size)
            ?? NSFont(name: name.replacingOccurrences(of: " ", with: ""), size: size)
            ?? NSFont(name: "Helvetica", size: size)!
        if op.face & 1 != 0 { f = NSFontManager.shared.convert(f, toHaveTrait: .boldFontMask) }
        if op.face & 2 != 0 { f = NSFontManager.shared.convert(f, toHaveTrait: .italicFontMask) }
        if op.face & 32 != 0 { f = NSFontManager.shared.convert(f, toHaveTrait: .condensedFontMask) }
        if op.face & 64 != 0 { f = NSFontManager.shared.convert(f, toHaveTrait: .expandedFontMask) }
        return f
    }

    static func text(_ op: machine_print_op, in ctx: CGContext) {
        guard op.len > 0, let raw = op.text else { return }
        let bytes = Data(bytes: raw, count: Int(op.len))
        guard var s = String(data: bytes, encoding: .macOSRoman) else { return }
        s = s.replacingOccurrences(of: "\r", with: " ")
        let white = !op.fg_black || (op.mode & 3) == 3   // srcBic or a light foreground
        var attrs: [NSAttributedString.Key: Any] = [
            .font: font(op),
            .foregroundColor: white ? NSColor.white.cgColor : NSColor.black.cgColor,
        ]
        if op.face & 8 != 0 { attrs[.strokeWidth] = 3.0 }  // outline
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attrs))
        let hostWidth = CTLineGetTypographicBounds(line, nil, nil, nil)
        let sx = hostWidth > 0 && op.width > 0 ? CGFloat(op.width) / CGFloat(hostWidth) : 1
        ctx.saveGState()
        ctx.translateBy(x: CGFloat(op.h0), y: CGFloat(op.v0))
        ctx.scaleBy(x: sx, y: -1)                        // undo the page flip for glyphs
        ctx.textPosition = .zero
        CTLineDraw(line, ctx)
        ctx.restoreGState()
        if op.face & 4 != 0 {                            // underline, as QuickDraw draws it
            ctx.setFillColor(gray: white ? 1 : 0, alpha: 1)
            ctx.fill(CGRect(x: CGFloat(op.h0), y: CGFloat(op.v0) + 1, width: CGFloat(op.width), height: 1))
        }
    }
}
#endif
