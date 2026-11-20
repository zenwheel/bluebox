// macOS host: the classic screen drawn by the GPU (present.h, Metal), and
// the display's refreshes that drive it.
//
// Each view showing the Mac screen has a CAMetalLayer and a presenter
// surface: the rectangle of the Mac screen it shows and the layer's
// pixels.  The presenter draws on its own thread at each refresh of the
// display, only what changed.  Without a GPU (or BLUEBOX_METAL=0) the views
// convert the changed rows on the CPU and hand Core Animation an image,
// which it scales on the GPU (no CPU scaling either way).

#if canImport(AppKit)
import Accelerate
import AppKit
import BlueBoxHost
import IOSurface
import QuartzCore
import Toolbox

/// Whether the screen is shown through the GPU presenter (decided once).
@MainActor let presenterOn: Bool = present_start()

/// One view's surface: freed with it.
final class PresenterSurface: @unchecked Sendable {
    let handle: OpaquePointer?
    let layer: CAMetalLayer

    init(layer: CAMetalLayer) {
        self.layer = layer
        handle = present_surface_new(Unmanaged.passUnretained(layer).toOpaque())
    }

    deinit { present_surface_free(handle) }

    /// The Mac screen's rect (x, y its top left) over the layer's size in points.
    func set(x: CGFloat, y: CGFloat, width: CGFloat, height: CGFloat, points: CGSize, scale: CGFloat) {
        let pw = Int32((points.width * scale).rounded()), ph = Int32((points.height * scale).rounded())
        present_surface_set(handle, Double(x), Double(y), Double(width), Double(height), pw, ph)
    }

    func shape(_ mask: [UInt8]?, width: Int, height: Int) {
        if let mask, mask.count == width * height {
            mask.withUnsafeBufferPointer { present_surface_set_shape(handle, $0.baseAddress, Int32(width), Int32(height)) }
        } else {
            present_surface_set_shape(handle, nil, 0, 0)
        }
    }

    func desktop(_ leaveOut: Bool) { present_surface_set_desktop(handle, leaveOut) }
    func visible(_ on: Bool) { present_surface_set_visible(handle, on) }
    func drawNow() { present_surface_draw_now(handle) }
}

/// The display's refreshes, told to the presenter: NSView's (or NSScreen's)
/// display link on macOS 14, a CVDisplayLink of the presenter's own before.
/// Pacing on, 60 a second whatever the display's rate; BLUEBOX_PACE=0, all
/// of them (120 on ProMotion).
@MainActor final class Vsync: NSObject {
    static let shared = Vsync()
    private var link: AnyObject?
    private var display: CGDirectDisplayID = 0
    /// What the link follows: a view (weak, gone with its window) or the
    /// main display, and the display it was on.
    private weak var followedView: NSView?
    private var followsView = false
    private var followed = false
    private var followedDisplay: CGDirectDisplayID = 0
    /// The display link's refreshes so far (macOS 14's; the presenter's
    /// own link counts its own), and the host tick's stand-in for them.
    private var realVsyncs: UInt64 = 0
    private var fallback = VsyncFallback()

    private static let pacing: Bool = ProcessInfo.processInfo.environment["BLUEBOX_PACE"].map { !$0.hasPrefix("0") } ?? true

    /// Refreshes for this view's display (nil: the main display's).
    func follow(_ view: NSView?) {
        guard presenterOn else { return }
        followedView = view
        followsView = view != nil
        followed = true
        followedDisplay = Self.displayID(of: view)
        if #available(macOS 14.0, *) {
            (link as? CADisplayLink)?.invalidate()
            let l: CADisplayLink
            if let view {
                l = view.displayLink(target: self, selector: #selector(refreshed(_:)))
            } else if let screen = NSScreen.main ?? NSScreen.screens.first {
                l = screen.displayLink(target: self, selector: #selector(refreshed(_:)))
            } else {
                return
            }
            l.preferredFrameRateRange = Self.pacing ? CAFrameRateRange(minimum: 60, maximum: 60, preferred: 60)
                : CAFrameRateRange(minimum: 60, maximum: 120, preferred: 120)
            l.add(to: .main, forMode: .common) // through menus, drags and live resizes
            link = l
        } else {
            let screen = view?.window?.screen ?? NSScreen.main
            let id = (screen?.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
                ?? CGMainDisplayID()
            if id != display || link == nil {
                display = id
                link = present_own_display_link(id) ? NSNumber(value: id) : nil
            }
        }
    }

    @available(macOS 14.0, *)
    @objc private func refreshed(_ l: CADisplayLink) {
        realVsyncs &+= 1
        let d = l.targetTimestamp - l.timestamp
        present_vsync(d > 0 ? 1 / d : 60)
    }

    private static func displayID(of view: NSView?) -> CGDirectDisplayID {
        let screen = view?.window?.screen ?? NSScreen.main
        return (screen?.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
            ?? CGMainDisplayID()
    }

    /// Follows this view (nil: the main display) unless it already does,
    /// on the same display: called at every host tick.
    func keepFollowing(_ view: NSView?) {
        guard presenterOn else { return }
        let same = view == nil ? !followsView : followedView === view
        if followed && same && Self.displayID(of: view) == followedDisplay { return }
        follow(view)
    }

    /// Follows the view again at the next keepFollowing (the displays changed).
    func forget() { followed = false }

    /// The host's 60 Hz tick: drives the presenter itself while the display
    /// link has been quiet for a while and a surface is seen, and stops at
    /// the link's first refresh, so the two never both drive it.
    func standIn(surfaceSeen: Bool) {
        guard presenterOn else { return }
        let was = fallback.driving
        let drive = fallback.tick(now: present_now_ns(), real: realVsyncs &+ present_own_link_vsyncs(),
                                  surfaceSeen: surfaceSeen)
        if drive != was && present_stats_on() {
            eprint(drive ? "bluebox: no refreshes from the display link: the host's tick drives the presenter\n"
                : "bluebox: the host's tick stops driving the presenter\n")
        }
        if drive { present_vsync(60) }
    }
}

/// Time spent presenting on the main thread, for BLUEBOX_PRESENT_STATS.
@inline(__always) func timedPresent(_ body: () -> Void) {
    guard present_stats_on() else { return body() }
    let t0 = present_now_ns()
    body()
    present_stats_main(present_now_ns() - t0)
}

/// The CPU path's pictures for one layer, as IOSurfaces: Core Animation
/// matches their colours to the display's and scales them on the GPU when
/// it composites.  A CGImage as the layer's contents was copied and
/// colour-matched on the main thread at every commit (a few ms a frame at
/// 1024 by 768).  A surface is written only when Core Animation is done
/// with it, never the one shown.
final class LayerPixels {
    private var pool: [IOSurface] = []
    private var shown = -1
    private var size = (0, 0)
    private let sRGB = CGColorSpace(name: CGColorSpace.sRGB)?.copyICCData() as Data?

    private func make(_ w: Int, _ h: Int) -> IOSurface? {
        let s = IOSurface(properties: [.width: w, .height: h, .bytesPerElement: 4, .bytesPerRow: w * 4,
                                       .pixelFormat: 0x4247_5241 /* 'BGRA' */])
        if let s, let icc = sRGB { s.setAttachment(icc, forKey: kIOSurfaceColorSpace as String) }
        return s
    }

    /// rgba (premultiplied, w x h) shown as the layer's contents.
    func show(_ rgba: [UInt32], width w: Int, height h: Int, on layer: CALayer?) {
        guard let layer, w > 0, h > 0, rgba.count >= w * h else { return }
        if size != (w, h) {
            pool = []
            shown = -1
            size = (w, h)
        }
        var pick = pool.indices.first { $0 != shown && !pool[$0].isInUse }
        if pick == nil, pool.count < 4, let s = make(w, h) {
            pool.append(s)
            pick = pool.count - 1
        }
        guard let i = pick ?? pool.indices.first(where: { $0 != shown }) else { return }
        let s = pool[i]
        s.lock(options: [], seed: nil)
        rgba.withUnsafeBytes { src in
            var from = vImage_Buffer(data: UnsafeMutableRawPointer(mutating: src.baseAddress!), height: vImagePixelCount(h),
                                     width: vImagePixelCount(w), rowBytes: w * 4)
            var to = vImage_Buffer(data: s.baseAddress, height: vImagePixelCount(h), width: vImagePixelCount(w),
                                   rowBytes: s.bytesPerRow)
            let order: [UInt8] = [2, 1, 0, 3] // RGBA to BGRA
            _ = vImagePermuteChannels_ARGB8888(&from, &to, order, vImage_Flags(kvImageNoFlags))
        }
        s.unlock(options: [], seed: nil)
        layer.contents = s
        shown = i
    }
}

/// The CPU path's frame: the rect's changed rows converted into rgba
/// (shape and desktop left out as asked); false if nothing changed.
func convertChangedRows(x: Int, y: Int, width w: Int, height h: Int, shadow: inout [UInt8], rgba: inout [UInt32],
                        shape: [UInt8]?, desktop: Bool, force: Bool) -> Bool {
    let size = machine_screen_shadow_size(Int32(x), Int32(y), Int32(w), Int32(h))
    if shadow.count != size { shadow = [UInt8](repeating: 0, count: size) }
    var bands = [Int32](repeating: 0, count: 64)
    var n = shadow.withUnsafeMutableBufferPointer { s in
        bands.withUnsafeMutableBufferPointer {
            machine_screen_changed_rows(Int32(x), Int32(y), Int32(w), Int32(h), s.baseAddress!, s.count, $0.baseAddress!, 32)
        }
    }
    if rgba.count != w * h {
        rgba = [UInt32](repeating: 0, count: w * h)
        n = 0
        bands[0] = Int32(y)
        bands[1] = Int32(y + h)
        n = 1
    } else if force {
        bands[0] = Int32(y)
        bands[1] = Int32(y + h)
        n = 1
    }
    guard n > 0 else { return false }
    var rows = 0
    for i in 0..<Int(n) { rows += Int(bands[i * 2 + 1] - bands[i * 2]) }
    rgba.withUnsafeMutableBytes { raw in
        let out = raw.baseAddress!.assumingMemoryBound(to: UInt8.self)
        if let shape, shape.count == w * h {
            shape.withUnsafeBufferPointer {
                machine_screen_rgba_bands(Int32(x), Int32(y), Int32(w), Int32(h), bands, n, $0.baseAddress, desktop, out)
            }
        } else {
            machine_screen_rgba_bands(Int32(x), Int32(y), Int32(w), Int32(h), bands, n, nil, desktop, out)
        }
    }
    present_stats_cpu_frame(Int32(min(rows, h)), Int32(w))
    return true
}
#endif
