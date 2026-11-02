// The Blue Box machine behind the screen saver: one per process (the
// emulator's state is global), on a thread of its own, running the chosen
// After Dark module as `bluebox screensaver` does; the views show its screen.

#if canImport(ScreenSaver)
import AppKit
import BlueBoxHost
import ScreenSaver
import Toolbox

/// The screen saver's settings, kept in its ScreenSaverDefaults.
struct SaverSettings: Equatable {
    static let domain = "org.bluebox.afterdark-saver"

    var module: String?
    var folders: [String] = []
    var scale = 2          /* host points per Mac pixel: 2 gives a classic monitor's size */
    var depth = 8
    var sound = false
    var backdrop = true    /* the desktop picture on the screen when it starts */
    var controls: [String: [Int]] = [:] /* by module path, from the first version: kept in the settings files now */

    static func load() -> SaverSettings {
        var s = SaverSettings()
        guard let d = ScreenSaverDefaults(forModuleWithName: domain) else { return s }
        s.module = d.string(forKey: "module")
        s.folders = d.stringArray(forKey: "folders") ?? []
        if d.object(forKey: "scale") != nil { s.scale = max(1, min(4, d.integer(forKey: "scale"))) }
        if d.object(forKey: "depth") != nil { s.depth = [1, 2, 4, 8, 16, 32].contains(d.integer(forKey: "depth")) ? d.integer(forKey: "depth") : 8 }
        s.sound = d.bool(forKey: "sound")
        if d.object(forKey: "backdrop") != nil { s.backdrop = d.bool(forKey: "backdrop") }
        s.controls = (d.dictionary(forKey: "controls") as? [String: [Int]]) ?? [:]
        // For trying the bundle outside the screen saver host.
        if let m = ProcessInfo.processInfo.environment["BLUEBOX_SAVER_MODULE"] { s.module = m }
        if let f = ProcessInfo.processInfo.environment["BLUEBOX_SAVER_FOLDERS"] { s.folders = f.split(separator: ":").map(String.init) }
        return s
    }

    func save() {
        guard let d = ScreenSaverDefaults(forModuleWithName: Self.domain) else { return }
        d.set(module, forKey: "module")
        d.set(folders, forKey: "folders")
        d.set(scale, forKey: "scale")
        d.set(depth, forKey: "depth")
        d.set(sound, forKey: "sound")
        d.set(backdrop, forKey: "backdrop")
        d.set(controls, forKey: "controls")
        d.synchronize()
    }

    var allFolders: [String] { defaultModuleFolders() + folders }

    /// Where the modules' settings are kept (afterdark.c): the screen
    /// saver's own container, the one place the sandbox lets it write.
    static var settingsDir: String {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?.path
            ?? NSHomeDirectory() + "/Library/Application Support"
        return base + "/Blue Box After Dark/Settings"
    }

    /// The module to show: the one chosen, else the first found (Flying
    /// Toasters if it is there).
    func chosenModule() -> String? {
        if let module, moduleExists(module) { return module }
        let all = ModuleCatalog.scan(allFolders, limit: 500)
        return all.first { ($0 as NSString).lastPathComponent.hasPrefix("Flying Toasters") } ?? all.first
    }
}

final class SaverMachine: @unchecked Sendable {
    static let shared = SaverMachine()

    private struct Run: Equatable {
        var module: String
        var width: Int, height: Int
        var settings: SaverSettings
    }

    private let lock = NSCondition()
    private var wanted: Run?          /* what should be running */
    private var current: Run?         /* what is */
    private var thread: Thread?
    private var audioInstalled = false

    /// The last frame, made once for all the views.
    private var image: CGImage?
    private var imageTime: TimeInterval = 0
    private var pixels = [UInt8]()

    /// Run the chosen module on a screen of this many Mac pixels (the
    /// machine starts again when either changes).
    func show(screenPoints: CGSize, settings: SaverSettings) {
        guard let module = settings.chosenModule() else { return }
        let scale = CGFloat(max(1, settings.scale))
        let w = max(320, Int(screenPoints.width / scale)) & ~7, h = max(240, Int(screenPoints.height / scale))
        let run = Run(module: module, width: w, height: h, settings: settings)
        lock.lock()
        if wanted != run {
            wanted = run
            if current != nil { wake() }
        }
        if thread == nil {
            let t = Thread { [weak self] in self?.loop() }
            t.name = "Blue Box"
            t.stackSize = 8 << 20
            thread = t
            t.start()
        }
        lock.signal()
        lock.unlock()
    }

    /// Stop the module (its Close runs) and leave the machine idle.
    func stop() {
        lock.lock()
        wanted = nil
        finished = nil
        if current != nil { wake() }
        lock.signal()
        lock.unlock()
    }

    /// A key, as After Dark woke on: the module closes and the run ends;
    /// one stuck in its own loop is stopped at its next instruction.
    private func wake() {
        machine_post_key(0x20, 49, 0, 1, 0)
        machine_post_key(0x20, 49, 0, 0, 0)
        let run = runs
        DispatchQueue.global().asyncAfter(deadline: .now() + 1.5) { [weak self] in
            guard let self else { return }
            self.lock.lock()
            defer { self.lock.unlock() }
            if self.runs == run && self.booted && !machine_halted() {
                machine_set_instruction_limit(machine_instruction_count() + 1)
            }
        }
    }

    /// The machine's thread: runs what is wanted until it ends, then waits
    /// for something else (a module that stopped by itself isn't started
    /// again until the settings or the screen change, or it is stopped).
    private var finished: Run?
    private var booted = false /* the screen is there to read */
    private var runs = 0       /* machines started */

    private func loop() {
        while true {
            lock.lock()
            while wanted == nil || wanted == finished { lock.wait() }
            let run = wanted!
            current = run
            booted = false
            runs += 1
            lock.unlock()
            start(run)
            lock.lock()
            booted = true
            lock.unlock()
            while !machine_halted() { _ = machine_run(1_000_000) }
            NSLog("Blue Box After Dark: %@: %@", (run.module as NSString).lastPathComponent,
                  String(cString: machine_halt_reason()))
            lock.lock()
            current = nil
            finished = run
            lock.unlock()
        }
    }

    /// Boot a machine with the module as its program (afterdark.c's host).
    private func start(_ run: Run) {
        let s = run.settings
        if ProcessInfo.processInfo.environment["BLUEBOX_SAVER_DEBUG"] != nil { NSLog("start %@ %d x %d", run.module, run.width, run.height) }
        machine_screensaver_reset()
        machine_screensaver_set_settings_dir(SaverSettings.settingsDir)
        machine_screensaver_set_sound(s.sound)
        let values = s.controls[run.module] ?? [] // the first version's; the settings file has them now
        for (i, v) in values.enumerated() where i < 4 && v >= 0 { _ = machine_screensaver_set_control(Int32(i), Int32(v)) }
        if s.backdrop, let (rgba, w, h) = desktopPicture() {
            rgba.withUnsafeBufferPointer { machine_screensaver_set_backdrop($0.baseAddress, Int32(w), Int32(h)) }
        } else {
            machine_screensaver_set_backdrop(nil, 0, 0)
        }
        machine_set_screen(Int32(run.width), Int32(run.height))
        machine_set_screen_depth(Int32(s.depth))
        machine_set_addressing(machine_app_addressing(run.module))
        machine_set_instruction_limit(0)
        machine_attach_host(true) // real time: the clock and the waits are the host's
        if s.sound && !audioInstalled {
            ClassicAudio.shared.install()
            audioInstalled = true
        }
        guard machine_boot(false) else { return }
        var err = [CChar](repeating: 0, count: 256)
        if !machine_launch(run.module, &err, err.count) {
            NSLog("Blue Box After Dark: %@", String(cString: err))
        }
    }

    /// The screen now, at most once a frame time for all the views.
    func frame() -> CGImage? {
        let now = ProcessInfo.processInfo.systemUptime
        if let image, now - imageTime < 1.0 / 40 { return image }
        lock.lock()
        let running = current != nil && booted
        lock.unlock()
        guard running else { return image }
        var w: Int32 = 0, h: Int32 = 0
        machine_screen_size(&w, &h)
        if ProcessInfo.processInfo.environment["BLUEBOX_SAVER_DEBUG"] != nil { NSLog("frame %d x %d, %llu instructions", w, h, machine_instruction_count()) }
        let n = Int(w) * Int(h) * 4
        guard n > 0 else { return nil }
        if pixels.count != n { pixels = [UInt8](repeating: 0, count: n) }
        pixels.withUnsafeMutableBufferPointer { machine_screen_rgba(0, 0, w, h, $0.baseAddress) }
        let data = Data(pixels) as CFData
        guard let provider = CGDataProvider(data: data) else { return nil }
        image = CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(w) * 4,
                        space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                        provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        imageTime = now
        return image
    }

    /// The main display's desktop picture, as RGBA.
    private func desktopPicture() -> ([UInt8], Int, Int)? {
        var url: URL?
        DispatchQueue.main.sync { url = NSScreen.main.flatMap { NSWorkspace.shared.desktopImageURL(for: $0) } }
        guard let url, let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(src, 0, [kCGImageSourceShouldAllowFloat: false] as CFDictionary)
        else { return nil }
        // At most the screen's size: the machine scales it to its own.
        let w = min(image.width, 2048), h = image.height * w / max(1, image.width)
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        let ok = rgba.withUnsafeMutableBytes { raw -> Bool in
            guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            ctx.interpolationQuality = .medium
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        return ok ? (rgba, w, h) : nil
    }
}
#endif
