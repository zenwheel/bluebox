// `classic install`: wrap a library application in a macOS .app bundle so it
// has its own Dock icon, can be launched from the Finder, and accepts
// documents dropped on it.
//
// The bundle's executable is a small sh script that execs `classic run`
// with the application, so the process Launch Services started is ours.

#if canImport(AppKit)
import AppKit
import Foundation
import ForkStore

let bundleAppKey = "ClassicApplicationPath"

// MARK: - Images

func pngData(_ img: IconImage) -> Data? {
    guard let rep = bitmapRep(img) else { return nil }
    return rep.representation(using: .png, properties: [:])
}

private func bitmapRep(_ img: IconImage) -> NSBitmapImageRep? {
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: img.size, pixelsHigh: img.size,
                                     bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                     colorSpaceName: .deviceRGB, bytesPerRow: img.size * 4, bitsPerPixel: 32),
          let dst = rep.bitmapData else { return nil }
    img.rgba.withUnsafeBufferPointer { dst.update(from: $0.baseAddress!, count: img.rgba.count) }
    return rep
}

/// An image for the Dock: the large icon blown up crisply, plus the small one.
func dockImage(_ icons: [IconImage]) -> NSImage? {
    guard let large = icons.first(where: { $0.size == 32 }) ?? icons.first else { return nil }
    let image = NSImage(size: NSSize(width: 512, height: 512))
    if let rep = bitmapRep(scaled(large, to: 512)) {
        rep.size = NSSize(width: 512, height: 512)
        image.addRepresentation(rep)
    }
    return image
}

/// An .icns file: PNG entries at every size the Finder asks for, all
/// nearest-neighbour scaled from the 32 and 16 pixel originals.
func icnsData(_ icons: [IconImage]) -> Data? {
    guard let large = icons.first(where: { $0.size == 32 }) ?? icons.first else { return nil }
    let small = icons.first(where: { $0.size == 16 })
    var entries: [(String, IconImage)] = [
        ("icp5", scaled(large, to: 32)), ("ic11", scaled(large, to: 32)), ("ic12", scaled(large, to: 64)),
        ("ic07", scaled(large, to: 128)), ("ic13", scaled(large, to: 256)), ("ic08", scaled(large, to: 256)),
        ("ic14", scaled(large, to: 512)), ("ic09", scaled(large, to: 512)), ("ic10", scaled(large, to: 1024)),
    ]
    if let small { entries.insert(("icp4", small), at: 0) }
    var body = Data()
    for (type, img) in entries {
        guard let png = pngData(img) else { return nil }
        body.append(contentsOf: Array(type.utf8))
        body.append(bigEndian32(UInt32(png.count + 8)))
        body.append(png)
    }
    var file = Data("icns".utf8)
    file.append(bigEndian32(UInt32(body.count + 8)))
    file.append(body)
    return file
}

private func bigEndian32(_ v: UInt32) -> Data { withUnsafeBytes(of: v.bigEndian) { Data($0) } }

// MARK: - Bundle

/// Uniform type identifiers for classic file types macOS also knows.
private func contentTypes(for type: UInt32) -> [String]? {
    switch fourCCString(type) {
    case "TEXT", "ttro": return ["public.plain-text"]
    case "PICT": return ["com.apple.pict"]
    case "GIFf": return ["com.compuserve.gif"]
    case "JPEG": return ["public.jpeg"]
    case "PNGf": return ["public.png"]
    case "TIFF": return ["public.tiff"]
    case "AIFF", "AIFC": return ["public.aiff-audio"]
    case "MooV": return ["com.apple.quicktime-movie"]
    case "PDF ": return ["com.adobe.pdf"]
    case "rtf ", "RTF ": return ["public.rtf"]
    default: return nil
    }
}

private func versionString(_ path: String) -> String? {
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    guard appledouble_read(path, &mf, &err, err.count) else { return nil }
    defer { mac_file_free(&mf) }
    // 'vers' 1: major, minor, stage, release, region, then Pascal short version.
    guard let fork = ResourceFork(fork: mf.rsrc, length: mf.rsrc_len), let v = fork["vers", 1], v.count > 7 else {
        return nil
    }
    let n = Int(v[6])
    guard v.count >= 7 + n else { return nil }
    var out = [CChar](repeating: 0, count: n * 3 + 1)
    _ = v[7..<7 + n].withContiguousStorageIfAvailable { macroman_to_utf8($0.baseAddress, n, &out, out.count) }
    let s = str(out).trimmingCharacters(in: .whitespaces)
    return s.isEmpty ? nil : s
}

private func infoPlist(app: String, name: String, executable: String, bundle: FinderBundle, creator: UInt32,
                       hasIcon: Bool) -> [String: Any] {
    let slug = name.lowercased().map { $0.isLetter || $0.isNumber ? String($0) : "-" }.joined()
    var info: [String: Any] = [
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": executable,
        "CFBundleIdentifier": "local.classic.\(slug)",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name,
        "CFBundleDisplayName": name,
        "CFBundlePackageType": "APPL",
        "CFBundleVersion": "1",
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": true,
        "NSPrincipalClass": "NSApplication",
        bundleAppKey: app,
    ]
    let sig = fourCCString(creator)
    if !sig.contains("?") { info["CFBundleSignature"] = sig }
    if let v = versionString(app) { info["CFBundleShortVersionString"] = v }
    if hasIcon { info["CFBundleIconFile"] = "AppIcon" }

    var docTypes: [[String: Any]] = []
    for type in bundle.documentTypes {
        let code = fourCCString(type)
        var entry: [String: Any] = [
            "CFBundleTypeName": "\(name) document (\(code))",
            "CFBundleTypeOSTypes": [code],
            "CFBundleTypeRole": code == "ttro" ? "Viewer" : "Editor",
            "LSHandlerRank": "Alternate",
        ]
        if let utis = contentTypes(for: type) { entry["LSItemContentTypes"] = utis }
        docTypes.append(entry)
    }
    // Classic documents rarely carry a type macOS knows; accept any file
    // dropped on the icon without claiming to be its usual application.
    docTypes.append([
        "CFBundleTypeName": "Classic document",
        "CFBundleTypeRole": "Viewer",
        "LSHandlerRank": "None",
        "LSItemContentTypes": ["public.data", "public.content"],
    ])
    info["CFBundleDocumentTypes"] = docTypes
    return info
}

private func shellQuote(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

private func launcherScript(classic: String, app: String) -> String {
    """
    #!/bin/sh
    # Made by classic install.
    exec \(shellQuote(classic)) run \(shellQuote(app)) "$@"

    """
}

func defaultInstallFolder() -> String {
    (NSHomeDirectory() as NSString).appendingPathComponent("Applications/Classic")
}

func installCommand(_ args: [String]) {
    var source: String?
    var appName: String?
    var folder = defaultInstallFolder()
    var force = false
    var i = 0
    while i < args.count {
        switch args[i] {
        case "--to":
            i += 1
            guard i < args.count else { usage() }
            folder = (args[i] as NSString).expandingTildeInPath
        case "--app":
            i += 1
            guard i < args.count else { usage() }
            appName = args[i]
        case "--force": force = true
        default:
            if args[i].hasPrefix("-") || source != nil { usage() }
            source = args[i]
        }
        i += 1
    }
    guard let source else { usage() }
    let app = URL(fileURLWithPath: resolveApplication(source, named: appName)).standardizedFileURL.path
    let name = (app as NSString).lastPathComponent
    let bundlePath = (folder as NSString).appendingPathComponent(name + ".app")
    let fm = FileManager.default

    if fm.fileExists(atPath: bundlePath) {
        let existing = NSDictionary(contentsOfFile: bundlePath + "/Contents/Info.plist")
        guard force || existing?[bundleAppKey] != nil else {
            fail("\(bundlePath) exists and was not made by classic install (use --force to replace it)")
        }
        do { try fm.removeItem(atPath: bundlePath) } catch { fail("cannot replace \(bundlePath): \(error)") }
    }

    let finder = finderBundle(applicationAt: app)
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    var creator: UInt32 = 0
    if appledouble_read(app, &mf, &err, err.count) {
        creator = mac_file_creator(&mf)
        mac_file_free(&mf)
    }

    guard let selfPath = Bundle.main.executableURL?.resolvingSymlinksInPath().path else {
        fail("cannot find the classic executable")
    }
    let executable = name.replacingOccurrences(of: "/", with: ":")
    let icns = icnsData(finder.appIcons)
    do {
        try fm.createDirectory(atPath: bundlePath + "/Contents/MacOS", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: bundlePath + "/Contents/Resources", withIntermediateDirectories: true)
        let launcher = bundlePath + "/Contents/MacOS/" + executable
        try Data(launcherScript(classic: selfPath, app: app).utf8).write(to: URL(fileURLWithPath: launcher))
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: launcher)
        if let icns { try icns.write(to: URL(fileURLWithPath: bundlePath + "/Contents/Resources/AppIcon.icns")) }
        let info = infoPlist(app: app, name: name, executable: executable, bundle: finder, creator: creator,
                             hasIcon: icns != nil)
        let data = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
        try data.write(to: URL(fileURLWithPath: bundlePath + "/Contents/Info.plist"))
        try Data("APPL\(info["CFBundleSignature"] as? String ?? "????")".utf8)
            .write(to: URL(fileURLWithPath: bundlePath + "/Contents/PkgInfo"))
    } catch {
        fail("cannot create \(bundlePath): \(error)")
    }

    // Tell Launch Services about the new bundle and its document types.
    let lsregister = "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
    if let p = try? Process.run(URL(fileURLWithPath: lsregister), arguments: ["-f", bundlePath]) {
        p.waitUntilExit()
    }
    print(bundlePath)
    if finder.appIcons.isEmpty { print("  note: no Finder icon found; the bundle uses the generic icon") }
    let types = finder.documentTypes.map(fourCCString).joined(separator: ", ")
    if !types.isEmpty { print("  documents: \(types)") }
}

#endif
