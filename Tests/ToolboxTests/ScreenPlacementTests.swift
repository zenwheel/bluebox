import BlueBoxHost
import Testing

/// A program's period screen: the setting, its place on the display
/// rootless and with the whole display, and the size remembered for it.
@Suite struct ScreenPlacement {
    @Test func setting() {
        #expect(ScreenSetting("1024x768") == .size(1024, 768))
        #expect(ScreenSetting("640X480") == .size(640, 480))
        #expect(ScreenSetting("display") == .display)
        // A Mac screen's shape: a whole number of 64 wide, 2 high.
        #expect(ScreenSetting("1000x701") == .size(960, 700))
        #expect(ScreenSetting("1000x701")?.text == "960x700")
        #expect(ScreenSetting("display")?.text == "display")
        for bad in ["", "default", "1024", "1024x", "x768", "1024x768x2", "100x100", "5000x768", "1024x300", "big"] {
            #expect(ScreenSetting(bad) == nil, "\(bad)")
        }
        #expect(ScreenSetting.display.size == nil)
    }

    /// Rootless: the screen centred across the visible area, its top
    /// (less the hidden menu bar) at the visible area's top, at the display
    /// scale; the display's own screen placed as it always was.
    @Test func rootless() {
        // A 6K display at 1x: visible 3360 by 1865 under a 25-point menu bar.
        let display = ScreenMapping.rootless(visibleMinX: 0, visibleWidth: 3360, visibleMaxY: 1865, menuBar: 20, scale: 1,
                                             mac: (3328, 1884))
        #expect(display.left == 16 && display.top == 1885 && display.scale == 1)
        let period = ScreenMapping.rootless(visibleMinX: 0, visibleWidth: 3360, visibleMaxY: 1865, menuBar: 20, scale: 1,
                                            mac: (1024, 768))
        #expect(period.left == 1168 && period.top == 1885)
        // Mac y 20 (the first row under the menu bar) is the visible area's top.
        #expect(period.host(0, 20) == (1168, 1865))
        #expect(period.mac(1168, 1865) == (0, 20))
        #expect(period.host(1024, 768) == (2192, 1117))
        // At 2x, on a display whose visible area starts at 100.
        let two = ScreenMapping.rootless(visibleMinX: 100, visibleWidth: 2560, visibleMaxY: 1415, menuBar: 20, scale: 2,
                                         mac: (1024, 768))
        #expect(two.left == 356 && two.top == 1455)
        #expect(two.host(10, 30) == (376, 1395))
        #expect(two.mac(376, 1395) == (10, 30))
        #expect(two.mac(377.9, 1393.1) == (10, 30)) // within the same Mac pixel
    }

    /// Mac to host and back lands on the same pixel, wherever it is.
    @Test func roundTrip() {
        let mappings = [
            ScreenMapping.rootless(visibleMinX: 0, visibleWidth: 3360, visibleMaxY: 1865, menuBar: 20, scale: 1.5,
                                   mac: (1024, 768)),
            ScreenMapping.wholeDisplay(mac: (1024, 768), displayX: 0, displayY: 0, displayWidth: 3360, displayHeight: 1890,
                                       backing: 2),
            ScreenMapping.wholeDisplay(mac: (640, 480), displayX: -1920, displayY: 300, displayWidth: 1920,
                                       displayHeight: 1080, backing: 1),
        ]
        for m in mappings {
            for (x, y) in [(0, 0), (1, 1), (511, 383), (639, 479), (1023, 767), (-5, -3)] {
                let h = m.host(Double(x), Double(y))
                // The pixel's centre, as a pointer over it would be.
                let c = m.mac(h.x + m.scale / 2, h.y - m.scale / 2)
                #expect(c == (Int32(x), Int32(y)), "\(m) \(x),\(y)")
            }
        }
    }

    /// With the whole display: the largest fit, or a whole number of
    /// device pixels a Mac pixel when that fills nearly as much; centred,
    /// black around.
    @Test func wholeDisplay() {
        // 6K: 1024x768 fits at 2.4609 (the height); 4 device pixels (2
        // points) would fill only 81%, so the largest fit, black at the sides.
        let sixK = ScreenMapping.wholeDisplay(mac: (1024, 768), displayX: 0, displayY: 0, displayWidth: 3360,
                                              displayHeight: 1890, backing: 2)
        #expect(abs(sixK.scale - 1890.0 / 768) < 1e-9)
        let f = sixK.frame((1024, 768))
        #expect(abs(f.height - 1890) < 1e-6 && f.y == 0)
        #expect(f.x == ((3360 - 1024 * sixK.scale) / 2).rounded(.down))
        #expect(abs(f.x * 2 + f.width - 3360) <= 1) // centred: the same black either side
        // 2560x1600: fits at 2.083; 2 points (4 device pixels) is 96% of it.
        let mbp = ScreenMapping.wholeDisplay(mac: (1024, 768), displayX: 0, displayY: 0, displayWidth: 2560,
                                             displayHeight: 1600, backing: 2)
        #expect(mbp.scale == 2)
        #expect(mbp.left == 256 && mbp.top == 1568) // 2048x1536, 256 at the sides, 32 above and below
        // 640x480 on 1920x1080 at 1x: fits at 2.25, 2 is 89%: the fit.
        let hd = ScreenMapping.wholeDisplay(mac: (640, 480), displayX: 0, displayY: 0, displayWidth: 1920,
                                            displayHeight: 1080, backing: 1)
        #expect(hd.scale == 2.25 && hd.left == 240 && hd.top == 1080)
        // At 2x the same display has 4.5 device pixels a Mac pixel: 4 is 89%.
        let hd2 = ScreenMapping.wholeDisplay(mac: (640, 480), displayX: 0, displayY: 0, displayWidth: 1920,
                                             displayHeight: 1080, backing: 2)
        #expect(hd2.scale == 2.25)
        // 640x480 on 2560x1440: exactly 3.
        let qhd = ScreenMapping.wholeDisplay(mac: (640, 480), displayX: 0, displayY: 0, displayWidth: 2560,
                                             displayHeight: 1440, backing: 2)
        #expect(qhd.scale == 3 && qhd.left == 320 && qhd.top == 1440)
        // A display that is not the first: placed on it.
        let other = ScreenMapping.wholeDisplay(mac: (1024, 768), displayX: 2560, displayY: -200, displayWidth: 2560,
                                               displayHeight: 1600, backing: 2)
        #expect(other.left == 2816 && other.top == 1368)
        // A screen larger than the display is scaled down to fit.
        let small = ScreenMapping.wholeDisplay(mac: (1024, 768), displayX: 0, displayY: 0, displayWidth: 1280,
                                               displayHeight: 720, backing: 2)
        #expect(abs(small.scale - 720.0 / 768) < 1e-9)
        // The pointer in the black: off the screen, kept at its edge.
        let left = sixK.mac(1, 945), above = mbp.mac(1280, 1590)
        #expect(left.0 < 0 && above.1 < 0)
        #expect(ScreenMapping.clamp(left, to: (1024, 768)).0 == 0)
        #expect(ScreenMapping.clamp(above, to: (1024, 768)) == (512, 0))
        #expect(ScreenMapping.clamp((5000, 5000), to: (1024, 768)) == (1023, 767))
    }

    /// The first time a program takes the whole screen: 1024x768 with its
    /// display setting, when the display is larger and nothing is set.
    @Test func remembered() {
        #expect(ScreenSetting.toRemember(set: nil, runHasPeriodScreen: false, display: (3328, 1884)) == .size(1024, 768))
        #expect(ScreenSetting.toRemember(set: nil, runHasPeriodScreen: false, display: (1408, 900)) == .size(1024, 768))
        #expect(ScreenSetting.toRemember(set: nil, runHasPeriodScreen: false, display: (1088, 768)) == .size(1024, 768))
        // Set already (a size, or the display's), or given for the run.
        #expect(ScreenSetting.toRemember(set: "640x480", runHasPeriodScreen: false, display: (3328, 1884)) == nil)
        #expect(ScreenSetting.toRemember(set: "display", runHasPeriodScreen: false, display: (3328, 1884)) == nil)
        #expect(ScreenSetting.toRemember(set: nil, runHasPeriodScreen: true, display: (3328, 1884)) == nil)
        // A display no larger, or one 1024x768 doesn't fit on.
        #expect(ScreenSetting.toRemember(set: nil, runHasPeriodScreen: false, display: (1024, 768)) == nil)
        #expect(ScreenSetting.toRemember(set: nil, runHasPeriodScreen: false, display: (1280, 700)) == nil)
        #expect(ScreenSetting.toRemember(set: nil, runHasPeriodScreen: false, display: (960, 1200)) == nil)
    }
}
