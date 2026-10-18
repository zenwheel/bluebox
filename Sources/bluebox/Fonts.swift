// Fonts from the user's own System disk images.
//
// Blue Box draws with free look-alikes of the classic fonts.  The real ones
// are Apple's and do not ship with it, but a user with a System disk image
// can bring them in: `bluebox fonts import <image>` (or File > Import Fonts
// from Disk Image… in Blue Box.app) copies the image's font files into Blue
// Box's System Folder, whose Fonts folder every run opens (machine.c), so
// its families take the place of ours.

import CCore
import Foundation
import ForkStore

/// Blue Box's own Fonts folder, which runs open at startup.
func fontsFolder() -> String { defaultDisk() + "/System Folder/Fonts" }

private let fontTypes: [UInt32] = [0x464F_4E44 /* FOND */, 0x4E46_4E54 /* NFNT */, 0x464F_4E54 /* FONT */]

/// Copy the fonts of the System Folder on a disk image (raw, DiskCopy,
/// NDIF, .dmg, or any of these gzipped) into the Fonts folder.  A System 7.1
/// or later image has a Fonts folder of suitcases; an older one keeps its
/// fonts in the System file, and those become one suitcase, "System Fonts".
/// Returns the names of the files added.
func importFonts(fromImage path: String) throws -> [String] {
    let fm = FileManager.default
    let work = NSTemporaryDirectory() + "bluebox-fonts-\(UUID().uuidString)"
    try fm.createDirectory(atPath: work, withIntermediateDirectories: true)
    defer { try? fm.removeItem(atPath: work) }
    var image = path
    if path.lowercased().hasSuffix(".gz") {
        image = work + "/image"
        guard fm.createFile(atPath: image, contents: nil), let out = FileHandle(forWritingAtPath: image) else {
            throw IngestError.unreadable(path)
        }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/gunzip")
        p.arguments = ["-c", path]
        p.standardOutput = out
        try p.run()
        p.waitUntilExit()
        try? out.close()
        guard p.terminationStatus == 0 else { throw IngestError.diskImage(path, "could not decompress it") }
    }
    // Only what is in a System Folder's Fonts folder, and System files.
    var notes: [String] = []
    let root = try extractDiskImage(image, into: work + "/volume", notes: &notes) { rel in
        let parts = rel.split(separator: "/").map(String.init)
        guard let i = parts.firstIndex(of: "System Folder") else { return false }
        return (i + 2 < parts.count && parts[i + 1] == "Fonts") || (i + 2 == parts.count && parts[i + 1] == "System")
    }
    // The System Folder nearest the top of the volume.
    var systemFolders: [String] = []
    if let e = fm.enumerator(atPath: root) {
        for case let rel as String in e where (rel as NSString).lastPathComponent == "System Folder" {
            systemFolders.append(root + "/" + rel)
        }
    }
    guard let system = systemFolders.min(by: { $0.split(separator: "/").count < $1.split(separator: "/").count }) else {
        throw IngestError.diskImage(path, "no System Folder on it")
    }
    let dest = fontsFolder()
    try fm.createDirectory(atPath: dest, withIntermediateDirectories: true)
    var added: [String] = []
    let fonts = system + "/Fonts"
    if let names = try? fm.contentsOfDirectory(atPath: fonts), !names.isEmpty {
        for name in names.sorted() where !name.hasPrefix(".") && fontResources(fonts + "/" + name) != nil {
            for f in [name, "._" + name] {
                try? fm.removeItem(atPath: dest + "/" + f)
                try fm.copyItem(atPath: fonts + "/" + f, toPath: dest + "/" + f)
            }
            added.append(name)
        }
    } else if let fork = fontResources(system + "/System") {
        // Before System 7.1: the fonts are in the System file.
        var mf = mac_file()
        let data = fork.withUnsafeBytes { buf -> UnsafeMutablePointer<UInt8> in
            let p = UnsafeMutablePointer<UInt8>.allocate(capacity: max(buf.count, 1))
            p.initialize(from: buf.bindMemory(to: UInt8.self).baseAddress!, count: buf.count)
            return p
        }
        mf.rsrc = data
        mf.rsrc_len = fork.count
        withUnsafeMutableBytes(of: &mf.finder_info) { fi in
            for (k, b) in Array("FFILDMOV".utf8).enumerated() { fi[k] = b }
        }
        mf.has_finder_info = true
        withUnsafeMutableBytes(of: &mf.name) { n in
            for (k, b) in Array("System Fonts".utf8).enumerated() { n[k] = b }
        }
        let out = dest + "/System Fonts"
        try? fm.removeItem(atPath: out)
        try? fm.removeItem(atPath: dest + "/._System Fonts")
        defer { data.deallocate() }
        try writeAppleDouble(&mf, to: out)
        added.append("System Fonts")
    }
    guard !added.isEmpty else { throw IngestError.diskImage(path, "no fonts in its System Folder") }
    return added
}

/// A file's font resources (FOND, NFNT, FONT) as a resource fork, or nil
/// if it has none.
private func fontResources(_ path: String) -> Data? {
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    guard appledouble_read(path, &mf, &err, err.count) else { return nil }
    defer { mac_file_free(&mf) }
    guard mf.rsrc_len > 0 else { return nil }
    var idx = rsrc_index()
    guard rsrc_parse(mf.rsrc, mf.rsrc_len, &idx, &err, err.count) else { return nil }
    let hasFont = (0..<idx.count).contains { fontTypes.contains(idx.entries[$0].type) }
    rsrc_index_free(&idx)
    guard hasFont else { return nil }
    var out: UnsafeMutablePointer<UInt8>?
    var len = 0
    guard rsrc_filter(mf.rsrc, mf.rsrc_len, fontTypes, fontTypes.count, &out, &len, &err, err.count), let out else {
        return nil
    }
    defer { free(out) }
    return Data(bytes: out, count: len)
}

/// The font files in the Fonts folder.
func installedFonts() -> [String] {
    ((try? FileManager.default.contentsOfDirectory(atPath: fontsFolder())) ?? [])
        .filter { !$0.hasPrefix(".") }.sorted()
}

/// bluebox fonts [import <disk image>...]
func fontsCommand(_ args: [String]) {
    guard let first = args.first else {
        let fonts = installedFonts()
        print(fonts.isEmpty ? "no fonts in \(fontsFolder()): Blue Box's own look-alikes are used" :
              fonts.joined(separator: "\n"))
        return
    }
    guard first == "import", args.count > 1 else { usage() }
    var failed = false
    for image in args.dropFirst() {
        do {
            let added = try importFonts(fromImage: image)
            print("\((image as NSString).lastPathComponent): \(added.joined(separator: ", "))")
        } catch {
            FileHandle.standardError.write("\((image as NSString).lastPathComponent): \(error)\n".data(using: .utf8)!)
            failed = true
        }
    }
    if failed { exit(1) }
}
