// A program's period screen: a Mac screen of the size its games were
// written for (640x480 to 1024x768), set per program, rather than one the
// size of the host display.  On a 6K display the display's own is
// 3360x1890, 6.3 million pixels a frame for a program that redraws the
// whole screen on the emulated processor; PixelToy drew about 12 frames a
// second on it and 60 on 1024x768.  Here: the setting, where the Mac
// screen sits on the display, and the size remembered for a program that
// takes the whole screen.  Kept free of AppKit so it can be tested without
// windows (the bluebox target's HostRootless places windows by it).

import Toolbox

/// The screen setting (bluebox set <app> screen ...): a size, or the
/// display's (never given a period size by itself).
public enum ScreenSetting: Equatable, Sendable {
    case display
    case size(Int32, Int32)

    /// "1024x768" (made a Mac's shape: as wide as a whole number of 64, as
    /// tall as a whole number of 2, machine_fit_screen) or "display"; nil
    /// for anything else, or a size outside 512x342 to 4096x4096.
    public init?(_ text: String) {
        if text == "display" {
            self = .display
            return
        }
        let parts = text.lowercased().split(separator: "x", omittingEmptySubsequences: false)
        guard parts.count == 2, var w = Int32(parts[0]), var h = Int32(parts[1]),
              (512...4096).contains(w), (342...4096).contains(h) else { return nil }
        machine_fit_screen(&w, &h)
        self = .size(w, h)
    }

    /// As the setting keeps it.
    public var text: String {
        switch self {
        case .display: return "display"
        case let .size(w, h): return "\(w)x\(h)"
        }
    }

    public var size: (Int32, Int32)? {
        if case let .size(w, h) = self { return (w, h) }
        return nil
    }

    /// The period size remembered for a program the first time it takes
    /// the whole screen: 1024x768 on a display whose Mac screen holds it
    /// and is larger, unless a screen is set already (its own or the
    /// defaults', "display" included) or the run was given one.
    public static let remembered = ScreenSetting.size(1024, 768)

    public static func toRemember(set: String?, runHasPeriodScreen: Bool, display: (Int32, Int32)) -> ScreenSetting? {
        guard set == nil, !runHasPeriodScreen, let (w, h) = remembered.size else { return nil }
        guard display.0 >= w, display.1 >= h, display.0 > w || display.1 > h else { return nil }
        return remembered
    }
}

/// Where the Mac screen is on the host display, in host points (y up, as
/// AppKit's screen coordinates): its top-left corner and the host points
/// per Mac pixel.
public struct ScreenMapping: Equatable, Sendable {
    public var left: Double
    public var top: Double
    public var scale: Double

    public init(left: Double, top: Double, scale: Double) {
        self.left = left
        self.top = top
        self.scale = scale
    }

    /// Rootless (and the whole screen of a program with the display's own
    /// size): at the display scale, centred across the visible area (its
    /// minX and width), the Mac's top `menuBar` rows under the host's menu
    /// bar, so Mac y = menuBar is the visible area's top (visibleMaxY).
    /// A period screen is placed the same way: centred across, its top under
    /// the menu bar, its windows the size they are on any Mac.
    public static func rootless(visibleMinX: Double, visibleWidth: Double, visibleMaxY: Double, menuBar: Double,
                                scale: Double, mac: (Int32, Int32)) -> ScreenMapping {
        let left = visibleMinX + ((visibleWidth - Double(mac.0) * scale) / 2).rounded(.down)
        return ScreenMapping(left: left, top: visibleMaxY + menuBar * scale, scale: scale)
    }

    /// A period screen with the whole display: scaled up (or down) to fit
    /// the display (its whole frame: the menu bar and Dock are hidden
    /// behind black), centred, the rest black.  Scaled by a whole number of
    /// device pixels per Mac pixel when that fills at least `integerFill`
    /// of what the largest fit does, so every pixel is the same size;
    /// otherwise by the largest fit (aspect kept), the presenter's sharp
    /// filter then making a few pixels a device pixel wider.  On a 6K
    /// display (3360x1890 points, 2 device pixels a point) 1024x768 fits at
    /// 2.46, 4 device pixels only 81% of that: the largest fit, filling the
    /// height; on a 2560x1600 display it fits at 2.08 and 4 device pixels
    /// (2 points) is 96%: those.
    public static let integerFill = 0.9

    public static func wholeDisplay(mac: (Int32, Int32), displayX: Double, displayY: Double, displayWidth: Double,
                                    displayHeight: Double, backing: Double) -> ScreenMapping {
        let w = Double(max(mac.0, 1)), h = Double(max(mac.1, 1))
        let fit = min(displayWidth / w, displayHeight / h)
        let b = max(backing, 1)
        let whole = (fit * b + 1e-9).rounded(.down) / b
        let scale = whole > 0 && whole >= fit * integerFill ? whole : fit
        // Whole points, so the edges fall on pixels.
        let left = displayX + ((displayWidth - w * scale) / 2).rounded(.down)
        let top = displayY + displayHeight - ((displayHeight - h * scale) / 2).rounded(.down)
        return ScreenMapping(left: left, top: top, scale: scale)
    }

    /// The host point of a Mac point (global coordinates).
    public func host(_ x: Double, _ y: Double) -> (x: Double, y: Double) {
        (left + x * scale, top - y * scale)
    }

    /// The Mac pixel under a host point.
    public func mac(_ x: Double, _ y: Double) -> (Int32, Int32) {
        (Int32(((x - left) / scale).rounded(.down)), Int32(((top - y) / scale).rounded(.down)))
    }

    /// The Mac screen's rectangle in host points: x, y (its bottom), width, height.
    public func frame(_ mac: (Int32, Int32)) -> (x: Double, y: Double, width: Double, height: Double) {
        let w = Double(mac.0) * scale, h = Double(mac.1) * scale
        return (left, top - h, w, h)
    }

    /// A Mac point kept on a screen of this size, as a Mac's pointer
    /// stopped at the screen's edges.
    public static func clamp(_ p: (Int32, Int32), to mac: (Int32, Int32)) -> (Int32, Int32) {
        (min(max(p.0, 0), mac.0 - 1), min(max(p.1, 0), mac.1 - 1))
    }
}
