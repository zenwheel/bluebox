// bluebox screensaver: an After Dark graphics module run as the screen
// saver (afterdark.c), in a window, the whole display, or headless with
// PNGs of its frames.

import Foundation
import Toolbox
#if canImport(AppKit)
import AppKit
#endif

/// A screen saver run reports what the module did when it ends.
nonisolated(unsafe) var screensaverRun = false
/// The whole display: a borderless window covering it, the cursor hidden.
nonisolated(unsafe) var hostFullScreen = false

let screensaverUsage = """
  bluebox screensaver <module>... [--window|--fullscreen|--headless] [--seconds N] [--every N] [--random]
            [--png-every N dir] [--sound] [--control I=V]... [--button B]... [--backdrop IMAGE] [--frames-per-tick N]
            [--tick-budget N] [--settings DIR | --no-settings] [--monitor WxH+X+Y]...
            [run options: --depth, --screen, --log ...]
      Run an After Dark graphics module (a file of type ADgm, or an archive
      holding one) as After Dark did: Initialize, Blank, then DrawFrame each
      tick until a key, a click or the mouse moving wakes it (or N seconds,
      --seconds).  The default is a window; --fullscreen covers the display;
      --headless (implied by --png-every) draws off-screen for N seconds
      (default 30), --png-every N writing the screen to dir/frame-NNNNN.png
      every N seconds.  The screen is 8 bits deep unless --depth says (a
      module that says it needs another depth runs again at that one).
      --control I=V gives control I (0-3 from the top, or its ID 1000-1003)
      value V in place of the module's own setting; --button B presses
      button B first (its message, or its bVal ID 1000-1003), as in the
      control panel (with --file-answer for the file dialog it may show).
      --backdrop IMAGE puts a picture on the screen before the module starts
      (in place of Blue Box's desktop): what Puzzle or Spotlight work on.  Up to 4 frames a tick
      (--frames-per-tick), fewer once a frame's code has run 100000
      instructions in the tick (--tick-budget).  Sound is off unless --sound.
      Settings are kept as After Dark kept them in the module: the controls
      given and what the module changes in its own resources (at a button,
      in its setup) are there the next time.  They go in a file per module
      in Blue Box's "After Dark Settings" folder (--settings DIR another;
      --no-settings: not kept); the module file itself isn't changed.
      --monitor WxH+X+Y makes the screen several displays, as After Dark's
      monitors (given again for each, the main one first): the module gets
      each in its list and blanks them all; the screen is the box around
      them unless --screen says.
      Several modules, or a folder of them, take turns as After Dark's
      Randomize had them: each runs --every N seconds (default 60), in
      the order given, or picked at random with --random.
"""

func screensaverCommand(_ args: [String]) {
    var display = "window"
    var seconds: Double?
    var runArgs: [String] = []
    var modules: [String] = []
    var every = 60.0
    var monitorRects: [(Int, Int, Int, Int)] = [] // left, top, right, bottom
    var random = false
    var depthGiven = false
    var maxGiven = false
    var framesPerTick: Int32 = 4
    var settings: String? = defaultScreensaverSettings()
    var budget: UInt32 = 100_000
    var i = 0
    func next() -> String {
        i += 1
        guard i < args.count else { eprint(screensaverUsage + "\n"); exit(2) }
        return args[i]
    }
    while i < args.count {
        let a = args[i]
        switch a {
        case "--window": display = "window"
        case "--fullscreen": display = "fullscreen"
        case "--headless": display = "headless"
        case "--seconds":
            guard let s = Double(next()), s > 0 else { fail("--seconds takes a number of seconds") }
            seconds = s
        case "--png-every":
            guard let s = Double(next()), s > 0 else { fail("--png-every takes a number of seconds and a folder") }
            let dir = next()
            try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            machine_screensaver_set_snapshots(s, dir)
            display = "headless"
        case "--sound": machine_screensaver_set_sound(true)
        case "--control":
            let parts = next().split(separator: "=", maxSplits: 1)
            guard parts.count == 2, let n = Int32(parts[0]), let v = Int32(parts[1]),
                  machine_screensaver_set_control(n, v) else { fail("--control takes I=V, I 0-3 or 1000-1003") }
        case "--button":
            guard let n = Int32(next()), machine_screensaver_press_button(n) else {
                fail("--button takes a button's message (8 or more) or its bVal ID (1000 and up)")
            }
        case "--backdrop":
            let image = next()
            guard let (rgba, w, h) = decodeImageRGBA(image) else { fail("--backdrop: cannot read \(image)") }
            rgba.withUnsafeBufferPointer { machine_screensaver_set_backdrop($0.baseAddress, Int32(w), Int32(h)) }
        case "--frames-per-tick":
            guard let n = Int32(next()), n > 0 else { fail("--frames-per-tick takes a number") }
            framesPerTick = n
        case "--tick-budget":
            guard let n = UInt32(next()), n > 0 else { fail("--tick-budget takes a number of instructions") }
            budget = n
        case "--every":
            guard let s = Double(next()), s > 0 else { fail("--every takes a number of seconds") }
            every = s
        case "--random": random = true
        case "--monitor":
            let g = next()
            let parts = g.split(whereSeparator: { "x+".contains($0) }).compactMap { Int($0) }
            guard parts.count == 4, parts[0] > 0, parts[1] > 0 else { fail("--monitor takes WxH+X+Y") }
            monitorRects.append((parts[2], parts[3], parts[2] + parts[0], parts[3] + parts[1]))
        case "--settings": settings = next()
        case "--no-settings": settings = nil
        case "--help", "-h":
            print(screensaverUsage)
            exit(0)
        default:
            if a.hasPrefix("-") {
                // The rest go to the run, with their values.
                runArgs.append(a)
                if a == "--depth" { depthGiven = true }
                if a == "--max" { maxGiven = true }
                let takesValue: Set<String> = ["--depth", "--screen", "--machine", "--cpu", "--fpu", "--addressing", "--log",
                                               "--max", "--init", "--keys", "--gestalt", "--lowmem", "--fonts", "--app",
                                               "--screenshot", "--appearance", "--scale", "--file-answer"]
                if takesValue.contains(a) { runArgs.append(next()) }
            } else {
                modules.append(a)
            }
        }
        i += 1
    }
    guard !modules.isEmpty else { eprint(screensaverUsage + "\n"); exit(2) }
    var files = modules.flatMap(resolveModules)
    if random { files.shuffle() }
    guard let file = files.first else { fail("no After Dark modules in \(modules.joined(separator: ", "))") }
    machine_screensaver_set_pace(framesPerTick, budget)
    machine_screensaver_set_settings_dir(settings)
    for m in files.dropFirst() { _ = machine_screensaver_add_module(m) }
    machine_screensaver_set_rotation(every, random)
    for (l, t, r, b) in monitorRects { _ = machine_screensaver_add_monitor(Int32(l), Int32(t), Int32(r), Int32(b)) }
    if !monitorRects.isEmpty && !runArgs.contains("--screen") {
        let w = monitorRects.map(\.2).max()!, h = monitorRects.map(\.3).max()!
        runArgs += ["--screen", "\((w + 7) & ~7)x\(h)"]
    }
    if display == "headless" && seconds == nil { seconds = 30 }
    if let seconds { machine_screensaver_set_seconds(seconds) }
    var run = [file] + runArgs
    if !depthGiven {
        run += ["--depth", "8"]
        // A module that says it needs another depth ("requires Millions
        // of colours") runs again at that one.
        depthRelaunch = (file, false)
    }
    switch display {
    case "headless":
        run.append("--headless")
        // Instructions enough for the seconds: a tick's frames and its wait.
        if !maxGiven {
            let perTick = UInt64(budget) * UInt64(framesPerTick) + 400_000
            run += ["--max", String(UInt64((seconds ?? 30) * 60) * perTick + 50_000_000)]
        }
    case "fullscreen":
        hostFullScreen = true
        #if canImport(AppKit)
        if !runArgs.contains("--screen"), let s = NSScreen.main?.frame.size {
            run += ["--screen", "\(Int(s.width))x\(Int(s.height))"]
        }
        #endif
        run += ["--screen-window", "--max", String(UInt64.max / 2)]
    default:
        run += ["--screen-window", "--max", String(UInt64.max / 2)]
    }
    screensaverRun = true
    runCommand(run)
}

/// Where modules' settings are kept: beside Blue Box's library.
func defaultScreensaverSettings() -> String {
    ((defaultLibrary() as NSString).deletingLastPathComponent as NSString).appendingPathComponent("After Dark Settings")
}

/// The modules a name stands for: a folder's (and its folders'), by name,
/// or the one module resolveModule finds.
func resolveModules(_ spec: String) -> [String] {
    var isDir: ObjCBool = false
    guard FileManager.default.fileExists(atPath: spec, isDirectory: &isDir), isDir.boolValue else {
        return [resolveModule(spec)]
    }
    let types = [fourCCValue("ADgm"), fourCCValue("A4gm")]
    return scan(URL(fileURLWithPath: spec).standardizedFileURL.path).filter { types.contains($0.type) }.map(\.path)
        .sorted { ($0 as NSString).lastPathComponent.localizedStandardCompare(($1 as NSString).lastPathComponent) == .orderedAscending }
}

/// A module by path (a file, or an archive holding one), or by name from
/// the library and the Mac's disk.
func resolveModule(_ spec: String) -> String {
    let fm = FileManager.default
    if fm.fileExists(atPath: spec) || fm.fileExists(atPath: (spec as NSString).deletingLastPathComponent + "/._" + (spec as NSString).lastPathComponent) {
        return resolveFile(URL(fileURLWithPath: spec).standardizedFileURL.path, kinds: ["ADgm", "A4gm"])
    }
    let types = [fourCCValue("ADgm"), fourCCValue("A4gm")]
    var found: [String] = []
    for root in [defaultLibrary(), defaultDisk()] where fm.fileExists(atPath: root) {
        found += scan(root).filter { types.contains($0.type) && ($0.path as NSString).lastPathComponent == spec }.map(\.path)
    }
    guard let first = found.first else { fail("no After Dark module \"\(spec)\" (a file, or one in the library)") }
    if found.count > 1 { eprint("bluebox: several modules named \(spec); using \(first)\n") }
    return first
}

/// An image file's pixels as RGBA (ImageIO reads PNG, JPEG, HEIC ...).
func decodeImageRGBA(_ path: String) -> ([UInt8], Int, Int)? {
    #if canImport(AppKit)
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let image = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return nil }
    let w = image.width, h = image.height
    var rgba = [UInt8](repeating: 0, count: w * h * 4)
    let drawn = rgba.withUnsafeMutableBytes { raw -> Bool in
        guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        return true
    }
    return drawn ? (rgba, w, h) : nil
    #else
    return nil
    #endif
}

/// After a screen saver run: what the module did.
func screensaverReport() {
    guard screensaverRun else { return }
    let calls = (0...8).map { machine_screensaver_calls(Int32($0)) }
    print("screen saver: \(machine_screensaver_frames()) frames; Initialize \(calls[0]), Blank \(calls[2]), "
          + "DrawFrame \(calls[3]), Close \(calls[1])")
    let err = String(cString: machine_screensaver_error())
    if !err.isEmpty { print("module stopped: \(err)") }
}
