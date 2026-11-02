// The Chooser's host side: the host's printers for its Printer device,
// what it changes kept in the settings, and `bluebox chooser`, which runs
// it on its own.

import Foundation
import BlueBoxHost
import ForkStore
import Toolbox
#if canImport(AppKit)
import AppKit
#endif

/// The account's full name: the Chooser's user name until one is set.
func hostFullUserName() -> String {
    let full = NSFullUserName()
    return String((full.isEmpty ? NSUserName() : full).prefix(31))
}

/// The host's printers, by queue name: those set up, and those CUPS finds
/// on the network (AirPrint), which it prints to without setting them up.
func hostPrinterNames() -> [String] {
    var names: [String] = []
    #if canImport(AppKit)
    names = NSPrinter.printerNames
    #endif
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/lpstat")
    p.arguments = ["-e"]
    let out = Pipe()
    p.standardOutput = out
    p.standardError = FileHandle.nullDevice
    if (try? p.run()) != nil {
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        for n in String(decoding: data, as: UTF8.self).split(separator: "\n").map(String.init) where !names.contains(n) {
            names.append(n)
        }
    }
    return names
}

/// A PDF printed on a host printer CUPS knows but AppKit doesn't (one on
/// the network not set up): lp, without a panel.
func printWithLP(_ pdf: Data, printer: String, title: String, copies: Int) -> Bool {
    let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-print-\(getpid()).pdf")
    guard (try? pdf.write(to: tmp)) != nil else { return false }
    defer { try? FileManager.default.removeItem(at: tmp) }
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/lp")
    p.arguments = ["-d", printer, "-t", title, "-n", String(max(1, copies)), tmp.path]
    p.standardOutput = FileHandle.nullDevice
    guard (try? p.run()) != nil else { return false }
    p.waitUntilExit()
    return p.terminationStatus == 0
}

/// The program the run's settings are kept for.
nonisolated(unsafe) var chooserApp = ""
nonisolated(unsafe) var chooserKeeps = true

func installChooserHost(app: String, keep: Bool) {
    chooserApp = app
    chooserKeeps = keep
    machine_set_chooser_host({ names, max, _ in
        guard let names else { return 0 }
        let list = hostPrinterNames().prefix(Int(max))
        for (i, name) in list.enumerated() {
            let bytes = Array(name.utf8.prefix(255)) + [0]
            withUnsafeMutableBytes(of: &names[i]) { buf in
                buf.copyBytes(from: bytes)
            }
        }
        return Int32(list.count)
    }, { key, value, _ in
        guard let key, let value, chooserKeeps else { return }
        keepChooserSetting(app: chooserApp, key: String(cString: key), value: String(cString: value))
    }, nil)
}

/// Headless printing with no --print-to: the Chooser's Save as PDF writes
/// its file; a printer or the panel would need someone there, so nothing
/// goes to a real printer.
func installHeadlessPrinting() {
    machine_set_print_handlers({ job, _ in
        guard let job else { return false }
        let folder = withUnsafeBytes(of: job.pointee.folder) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        let title = withUnsafeBytes(of: job.pointee.title) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        guard Int(job.pointee.destination) == Int(MACHINE_PRINT_TO_PDF), !job.pointee.save_pdf else {
            eprint("bluebox: \(job.pointee.npages) page(s) not printed: a headless run prints only to PDF files "
                   + "(the Chooser's Save as PDF, or --print-to)\n")
            return false
        }
        #if canImport(AppKit)
        let url = pdfDestination(folder, title: title.isEmpty ? ProcessInfo.processInfo.processName : title)
        do {
            try ClassicPrint.pdf(job).write(to: url)
            print("printed \(job.pointee.npages) page(s) to \(url.path)")
            return true
        } catch {
            eprint("could not write \(url.path): \(error)\n")
            return false
        }
        #else
        _ = (folder, title)
        return false
        #endif
    }, nil, nil)
}

/// `bluebox chooser [run options]`: the Chooser on its own, as the desk
/// accessory host runs a desk accessory file; its file holds an empty
/// DRVR named for it, which stands for the System's.
func chooserCommand(_ args: [String]) {
    let dir = (defaultLibrary() as NSString).appendingPathComponent("Desk Accessories")
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    let path = (dir as NSString).appendingPathComponent("Chooser")
    if !FileManager.default.fileExists(atPath: path) {
        // A resource fork with DRVR 16, "\0Chooser", no data.
        var name: [CChar] = [0] + Array("Chooser".utf8).map { CChar(bitPattern: $0) } + [0]
        var fork: UnsafeMutablePointer<UInt8>?
        var len = 0
        let ok = name.withUnsafeMutableBufferPointer { nb -> Bool in
            var spec = rsrc_spec(type: 0x4452_5652 /* DRVR */, id: 16, name: nb.baseAddress, attrs: 0, data: nil, len: 0,
                                 name_len: 8)
            return rsrc_build(&spec, 1, &fork, &len)
        }
        guard ok, let fork else { fail("could not make \(path)") }
        defer { free(fork) }
        var f = mac_file()
        var empty: [UInt8] = []
        var err = [CChar](repeating: 0, count: 256)
        let written = empty.withUnsafeMutableBufferPointer { e -> Bool in
            f.data = e.baseAddress
            f.data_len = 0
            f.rsrc = fork
            f.rsrc_len = len
            // dfil/movr: a desk accessory file, as Font/DA Mover made them.
            let info: [UInt8] = Array("dfilmovr".utf8) + [UInt8](repeating: 0, count: 24)
            withUnsafeMutableBytes(of: &f.finder_info) { $0.copyBytes(from: info) }
            f.has_finder_info = true
            return appledouble_write(path, &f, &err, err.count)
        }
        guard written else { fail("could not make \(path): \(String(cString: err))") }
    }
    runCommand([path] + args)
}
