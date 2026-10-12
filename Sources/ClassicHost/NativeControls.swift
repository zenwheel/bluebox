// Native controls for the native appearance: AppKit draws real macOS
// buttons, checkboxes, radio buttons and scroll bars offscreen, and the
// classic control definitions show the pixels in the control's place.

#if canImport(AppKit)
import AppKit
import Toolbox

public enum NativeControls {
    nonisolated(unsafe) private static var cache: [String: [UInt8]] = [:]
    private static let lock = NSLock()

    public static func install() {
        machine_set_control_renderer({ rq, rgba, _ in
            guard let rq = rq?.pointee, let rgba else { return false }
            let key = NativeControls.key(rq)
            NativeControls.lock.lock()
            let cached = NativeControls.cache[key]
            NativeControls.lock.unlock()
            let pixels: [UInt8]
            if let cached {
                pixels = cached
            } else {
                var rendered: [UInt8]?
                let render = { rendered = MainActor.assumeIsolated { NativeControls.render(rq) } }
                if Thread.isMainThread { render() } else { DispatchQueue.main.sync(execute: render) }
                guard let r = rendered else { return false }
                pixels = r
                NativeControls.lock.lock()
                if NativeControls.cache.count > 512 { NativeControls.cache.removeAll() }
                NativeControls.cache[key] = r
                NativeControls.lock.unlock()
            }
            guard pixels.count == Int(rq.width) * Int(rq.height) * 4 else { return false }
            pixels.withUnsafeBytes { rgba.update(from: $0.bindMemory(to: UInt8.self).baseAddress!, count: pixels.count) }
            return true
        }, nil)
    }

    private static func title(_ rq: machine_control_request) -> String {
        withUnsafeBytes(of: rq.title) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }

    private static func key(_ rq: machine_control_request) -> String {
        "\(rq.kind) \(rq.width)x\(rq.height) p\(rq.pressed) d\(rq.disabled) o\(rq.on) v\(rq.vertical) " +
            "\(Int(rq.position * 1000)) \(Int(rq.proportion * 1000)) \(rq.pressed_part) \(title(rq))"
    }

    @MainActor private static func control(_ rq: machine_control_request, _ bounds: NSRect) -> NSView {
        switch Int(rq.kind) {
        case Int(MACHINE_CONTROL_BUTTON):
            let b = NSButton(title: title(rq), target: nil, action: nil)
            b.bezelStyle = .push
            b.controlSize = rq.height < 18 ? .small : .regular
            b.frame = bounds
            b.isEnabled = !rq.disabled
            b.highlight(rq.pressed)
            return b
        case Int(MACHINE_CONTROL_CHECKBOX), Int(MACHINE_CONTROL_RADIO):
            let b = Int(rq.kind) == Int(MACHINE_CONTROL_RADIO)
                ? NSButton(radioButtonWithTitle: "", target: nil, action: nil)
                : NSButton(checkboxWithTitle: "", target: nil, action: nil)
            b.controlSize = .small
            b.frame = bounds
            b.state = rq.on ? .on : .off
            b.isEnabled = !rq.disabled
            b.highlight(rq.pressed)
            return b
        default:
            let s = NSScroller(frame: bounds)
            s.scrollerStyle = .legacy
            s.controlSize = .regular
            s.isEnabled = !rq.disabled && rq.proportion > 0
            s.knobProportion = rq.proportion > 0 ? CGFloat(rq.proportion) : 1
            s.doubleValue = rq.position
            return s
        }
    }

    @MainActor static func render(_ rq: machine_control_request) -> [UInt8]? {
        let w = Int(rq.width), h = Int(rq.height)
        guard w > 0, h > 0 else { return nil }
        let bounds = NSRect(x: 0, y: 0, width: w, height: h)
        let container = NSView(frame: bounds)
        container.appearance = NSAppearance(named: .aqua)
        container.addSubview(control(rq, bounds))
        guard let rep = container.bitmapImageRepForCachingDisplay(in: bounds) else { return nil }
        container.cacheDisplay(in: bounds, to: rep)
        // Into plain RGBA (premultiplied), top row first.
        var pixels = [UInt8](repeating: 0, count: w * h * 4)
        let ok = pixels.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
                  let image = rep.cgImage else { return false }
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        return ok ? pixels : nil
    }
}
#endif
