// `bluebox install`: wrap a library application in a macOS .app bundle so it
// has its own Dock icon, can be launched from the Finder, and accepts
// documents dropped on it.
//
// The bundle's executable is a small sh script that execs `bluebox run`
// with the application, so the process Launch Services started is ours.

#if canImport(AppKit)
import AppKit
import Foundation
import BlueBoxHost
import ForkStore

let bundleAppKey = "ClassicApplicationPath"

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

/// Why the application may ask for the microphone (the Sound Input Manager).
private let microphoneUsage = "Classic programs record sound through the microphone."

/// A scripting dictionary for the bundle, so AppleScript can say
/// `tell application "SimpleText" to do script "..."`: Do Script goes to
/// the classic program (HostAppleEvents), as do the core events.
let scriptingDefinitionName = "BlueBox.sdef"
private let scriptingDefinition = """
    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE dictionary SYSTEM "file://localhost/System/Library/DTDs/sdef.dtd">
    <dictionary title="Classic Application">
      <suite name="Classic Suite" code="bbox" description="Apple Events for the classic application.">
        <command name="do script" code="miscdosc" description="Send the classic application a script to run.">
          <direct-parameter type="text" description="The script, in the application's own language."/>
          <result type="text" description="The application's result."/>
        </command>
      </suite>
    </dictionary>

    """

/// The keys and the file that make a bundle scriptable.
func addScripting(_ info: inout [String: Any], resources: String) throws {
    info["NSAppleScriptEnabled"] = true
    info["OSAScriptingDefinition"] = scriptingDefinitionName
    let url = URL(fileURLWithPath: resources + "/" + scriptingDefinitionName)
    let data = Data(scriptingDefinition.utf8)
    if (try? Data(contentsOf: url)) != data { try data.write(to: url) }
}

private func infoPlist(app: String, name: String, executable: String, bundle: FinderBundle, creator: UInt32,
                       hasIcon: Bool) -> [String: Any] {
    let slug = name.lowercased().map { $0.isLetter || $0.isNumber ? String($0) : "-" }.joined()
    var info: [String: Any] = [
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": executable,
        "CFBundleIdentifier": "local.bluebox.\(slug)",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name,
        "CFBundleDisplayName": name,
        "CFBundlePackageType": "APPL",
        "CFBundleVersion": "1",
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": true,
        "NSMicrophoneUsageDescription": microphoneUsage,
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
    # Made by bluebox install.
    exec \(shellQuote(classic)) run \(shellQuote(app)) "$@"

    """
}

func defaultInstallFolder() -> String {
    (NSHomeDirectory() as NSString).appendingPathComponent("Applications/Blue Box")
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
            fail("\(bundlePath) exists and was not made by bluebox install (use --force to replace it)")
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
        var info = infoPlist(app: app, name: name, executable: executable, bundle: finder, creator: creator,
                             hasIcon: icns != nil)
        try addScripting(&info, resources: bundlePath + "/Contents/Resources")
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

// MARK: - Dock name

/// The Dock names a tile after the bundle a process was started from, and
/// after the executable's file name without one, so an interactive run
/// outside a bundle starts itself again from a small bundle of the
/// application's name: a link to this program, an Info.plist and the
/// Finder icon, kept with the library.  Returns only if that fails.
/// Blue Box.app's bundle identifier (scripts/make-app.sh).  Not under
/// "local.bluebox.", which marks an installed application's bundle.
let shellBundleIdentifier = "local.bluebox-shell"

func reexecInNamedBundle(app: String) {
    let env = ProcessInfo.processInfo.environment
    // Started by Launch Services from a `bluebox install` bundle: the Dock
    // already has that bundle's name (and it may hold documents to open).
    let launcher = env["__CFBundleIdentifier"] ?? ""
    for prefix in ["local.bluebox.", "local.classic."] where launcher.hasPrefix(prefix) && !launcher.hasPrefix(prefix + "run.") {
        return // (local.classic: a bundle installed before the rename)
    }
    // Outside a bundle, or inside Blue Box.app (whose name the Dock would
    // show for every application).
    guard Bundle.main.bundleIdentifier == nil || Bundle.main.bundleIdentifier == shellBundleIdentifier,
          env["BLUEBOX_NO_BUNDLE"] == nil,
          let me = Bundle.main.executableURL?.resolvingSymlinksInPath().path else { return }
    let name = (app as NSString).lastPathComponent
    let file = name.replacingOccurrences(of: "/", with: ":")
    let support = ((defaultLibrary() as NSString).deletingLastPathComponent as NSString)
    let bundle = support.appendingPathComponent("Bundles/\(file).app")
    let exe = bundle + "/Contents/MacOS/bluebox"
    let fm = FileManager.default
    let slug = name.lowercased().map { $0.isLetter || $0.isNumber ? String($0) : "-" }.joined()
    var info: [String: Any] = [
        "CFBundleExecutable": "bluebox",
        "CFBundleIdentifier": "local.bluebox.run.\(slug)",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": name,
        "CFBundleDisplayName": name,
        "CFBundlePackageType": "APPL",
        "NSHighResolutionCapable": true,
        "NSMicrophoneUsageDescription": microphoneUsage,
        "NSPrincipalClass": "NSApplication",
        bundleAppKey: app,
    ]
    let icns = icnsData(finderBundle(applicationAt: app).appIcons)
    if icns != nil { info["CFBundleIconFile"] = "AppIcon" }
    do {
        try fm.createDirectory(atPath: bundle + "/Contents/MacOS", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: bundle + "/Contents/Resources", withIntermediateDirectories: true)
        if (try? fm.destinationOfSymbolicLink(atPath: exe)) != me {
            try? fm.removeItem(atPath: exe)
            try fm.createSymbolicLink(atPath: exe, withDestinationPath: me)
        }
        try addScripting(&info, resources: bundle + "/Contents/Resources")
        let plist = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
        let plistURL = URL(fileURLWithPath: bundle + "/Contents/Info.plist")
        if (try? Data(contentsOf: plistURL)) != plist { try plist.write(to: plistURL) }
        if let icns { try icns.write(to: URL(fileURLWithPath: bundle + "/Contents/Resources/AppIcon.icns")) }
    } catch {
        return
    }
    let args = [exe] + CommandLine.arguments.dropFirst()
    var cargs = args.map { strdup($0) } + [nil]
    execv(exe, &cargs)
    cargs.forEach { free($0) }
}

#endif
