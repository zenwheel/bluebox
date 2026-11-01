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
  bluebox screensaver <module> [--window|--fullscreen|--headless] [--seconds N]
            [--png-every N dir] [--sound] [--control I=V]... [--button B]... [--frames-per-tick N]
            [--tick-budget N] [run options: --depth, --screen, --machine, --log ...]
      Run an After Dark graphics module (a file of type ADgm, or an archive
      holding one) as After Dark did: Initialize, Blank, then DrawFrame each
      tick until a key, a click or the mouse moving wakes it (or N seconds,
      --seconds).  The default is a window; --fullscreen covers the display;
      --headless (implied by --png-every) draws off-screen for N seconds
      (default 30), --png-every N writing the screen to dir/frame-NNNNN.png
      every N seconds.  The screen is 8 bits deep unless --depth says.
      --control I=V gives control I (0-3 from the top, or its ID 1000-1003)
      value V in place of the module's own setting; --button B presses
      button B first (its message, or its bVal ID 1000-1003), as in the
      control panel (with --file-answer for the file dialog it may show).  Up to 4 frames a tick
      (--frames-per-tick), fewer once a frame's code has run 100000
      instructions in the tick (--tick-budget).  Sound is off unless --sound.
"""

func screensaverCommand(_ args: [String]) {
    var display = "window"
    var seconds: Double?
    var runArgs: [String] = []
    var module: String?
    var depthGiven = false
    var maxGiven = false
    var framesPerTick: Int32 = 4
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
        case "--frames-per-tick":
            guard let n = Int32(next()), n > 0 else { fail("--frames-per-tick takes a number") }
            framesPerTick = n
        case "--tick-budget":
            guard let n = UInt32(next()), n > 0 else { fail("--tick-budget takes a number of instructions") }
            budget = n
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
            } else if module == nil {
                module = a
            } else {
                fail("one module at a time")
            }
        }
        i += 1
    }
    guard let module else { eprint(screensaverUsage + "\n"); exit(2) }
    let path = FileManager.default.fileExists(atPath: module) ? URL(fileURLWithPath: module).standardizedFileURL.path : module
    let file = resolveFile(path, kinds: ["ADgm", "A4gm"])
    machine_screensaver_set_pace(framesPerTick, budget)
    if display == "headless" && seconds == nil { seconds = 30 }
    if let seconds { machine_screensaver_set_seconds(seconds) }
    var run = [file] + runArgs
    if !depthGiven { run += ["--depth", "8"] }
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

/// After a screen saver run: what the module did.
func screensaverReport() {
    guard screensaverRun else { return }
    let calls = (0...8).map { machine_screensaver_calls(Int32($0)) }
    print("screen saver: \(machine_screensaver_frames()) frames; Initialize \(calls[0]), Blank \(calls[2]), "
          + "DrawFrame \(calls[3]), Close \(calls[1])")
    let err = String(cString: machine_screensaver_error())
    if !err.isEmpty { print("module stopped: \(err)") }
}
