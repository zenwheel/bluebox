// Blue Box After Dark.saver: a macOS screen saver that runs After Dark
// graphics modules in Blue Box (afterdark.c), built into a .saver bundle by
// scripts/build-saver.sh.
//
// Current macOS runs third-party screen savers in legacyScreenSaver, a
// sandboxed host: the bundle's code runs there with its own container for
// a home (Blue Box's System Folder and Macintosh HD are made there), and
// may read the whole disk (the host's read-only exception for "/"), so the
// modules imported into Blue Box's library are found where they are.  No
// process is started and nothing is written outside the container.  The
// host doesn't always stop a saver's views when the screen saver ends, and
// keeps the process; it says so with com.apple.screensaver.willstop, when
// the module is closed.

#if canImport(ScreenSaver)
import AppKit
import ScreenSaver
import Toolbox

@objc(BlueBoxAfterDarkView)
final class BlueBoxAfterDarkView: ScreenSaverView {
    private var sheet: ConfigureSheet?
    private static var observing = false

    override init?(frame: NSRect, isPreview: Bool) {
        super.init(frame: frame, isPreview: isPreview)
        animationTimeInterval = 1.0 / 30
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect // its part of the screen, not stretched
        layer?.magnificationFilter = .nearest
        Self.observeStop()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
    }

    private static func observeStop() {
        guard !observing else { return }
        observing = true
        DistributedNotificationCenter.default().addObserver(forName: Notification.Name("com.apple.screensaver.willstop"),
                                                            object: nil, queue: .main) { _ in
            SaverMachine.shared.stop()
        }
    }

    override func startAnimation() {
        super.startAnimation()
        let settings = SaverSettings.load()
        SaverMachine.shared.show(displays: displays(), settings: isPreview ? previewSettings(settings) : settings)
    }

    /// The machine's screen: every display (one module across them all, as
    /// After Dark had it on a Mac with several monitors), main first; a
    /// preview gets a classic 640 by 480.
    private func displays() -> [CGRect] {
        if isPreview { return [CGRect(x: 0, y: 0, width: 640, height: 480)] }
        let all = NSScreen.screens.map(\.frame)
        return all.isEmpty ? [window?.screen?.frame ?? bounds] : all
    }

    /// A preview shows the module at 1 point a pixel, small.
    private func previewSettings(_ s: SaverSettings) -> SaverSettings {
        var p = s
        p.scale = 1
        return p
    }

    override func stopAnimation() {
        super.stopAnimation()
        SaverMachine.shared.stop()
    }

    override func animateOneFrame() {
        guard let image = SaverMachine.shared.frame(for: isPreview ? nil : window?.screen?.frame) else { return }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layer?.contents = image
        CATransaction.commit()
    }

    override var hasConfigureSheet: Bool { true }

    override var configureSheet: NSWindow? {
        let s = ConfigureSheet { [weak self] settings in
            guard let self else { return }
            if self.isAnimating {
                SaverMachine.shared.show(displays: self.displays(), settings: self.isPreview ? self.previewSettings(settings) : settings)
            }
        }
        sheet = s
        return s.window
    }
}
#endif
