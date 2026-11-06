// `bluebox export`: a classic program as a macOS application that needs no
// Blue Box: its own copy of Blue Box (without Sparkle: an exported
// application must not update itself into another build), the program's
// folder, the fonts, extensions and theme a run uses, its settings as the
// bundle's defaults, its Finder icon and document types, signed ad hoc or
// with a Developer ID, and notarized if asked.  See BlueBoxHost/Export.swift
// for the layout and for where an exported program's files go.
//
// An exported application's executable is this program: started with no
// arguments (from the Finder), with options only, or with documents, it
// runs the program it carries.

import Foundation
import BlueBoxHost
import ForkStore

/// What an exported application's bundle says it runs.
struct ExportedProgram: Sendable {
    /// The program's path inside the Program folder.
    var program: String
    var command: String
    var options: [String]
    /// Its settings, as a property list (AppSettings).
    var settings: Data?
    var stamp: String
    var bundleIdentifier: String
    var resources: String
    var bundlePath: String
}

/// The exported application this process is, if it is one.
let currentExport: ExportedProgram? = {
    #if os(macOS)
    guard let info = Bundle.main.object(forInfoDictionaryKey: ExportBundle.infoKey) as? [String: Any],
          let program = info[ExportInfo.program] as? String,
          let id = Bundle.main.bundleIdentifier, let resources = Bundle.main.resourcePath else { return nil }
    let settings = (info[ExportInfo.settings]).flatMap {
        try? PropertyListSerialization.data(fromPropertyList: $0, format: .binary, options: 0)
    }
    return ExportedProgram(program: program, command: info[ExportInfo.command] as? String ?? "run",
                           options: info[ExportInfo.options] as? [String] ?? [], settings: settings,
                           stamp: info[ExportInfo.stamp] as? String ?? "", bundleIdentifier: id,
                           resources: resources, bundlePath: Bundle.main.bundlePath)
    #else
    return nil
    #endif
}()

/// An exported application's own folder in Application Support: its
/// library (the settings it changes), its Macintosh HD and its program.
func exportSupport() -> String? {
    guard let e = currentExport else { return nil }
    let home = ProcessInfo.processInfo.environment["HOME"] ?? NSHomeDirectory()
    return ExportBundle.supportFolder(home: home, bundleIdentifier: e.bundleIdentifier)
}

/// The settings an exported application was made with: under whatever its
/// run has changed since (kept in its own library).
func exportedSettings(for app: String) -> AppSettings? {
    guard let e = currentExport, let data = e.settings,
          settingsKey(app) == ((e.program as NSString).lastPathComponent) else { return nil }
    return try? PropertyListDecoder().decode(AppSettings.self, from: data)
}

/// An exported application's theme, by the file name its appearance setting
/// gives (copied out of the bundle with the program).
func exportedTheme(_ name: String) -> String? {
    guard let support = exportSupport() else { return nil }
    let path = support + "/" + ExportBundle.themesFolder + "/" + name
    return FileManager.default.fileExists(atPath: path) ? path : nil
}

/// The settings that go with an exported program to another Mac: how its
/// machine runs, but not what belongs to this Mac or this user (the
/// owner's name, a folder for PDFs, a printer driver's file, a file server
/// and its password, the user's Internet identity).
func exportableSettings(_ s: AppSettings) -> AppSettings {
    var out = s
    out.userName = nil
    out.printFolder = nil
    out.appleShare = nil
    if let p = out.printer, p != "panel" { out.printer = nil }
    if var i = out.internet {
        i.realName = nil
        i.email = nil
        i.organization = nil
        i.mailAccount = nil
        i.smtpHost = nil
        i.newsHost = nil
        out.internet = i.isEmpty ? nil : i
    }
    return out
}

/// The running-programs registry's hook.  An exported application runs
/// through runCommand like every other run, so it registers there as other
/// runs do; an entry for one can say which exported application it is
/// with these (nil when this process is not one).
func exportRegistryInfo() -> [String: String]? {
    guard let e = currentExport else { return nil }
    return ["bundleIdentifier": e.bundleIdentifier, "bundlePath": e.bundlePath,
            "support": exportSupport() ?? ""]
}

/// An exported application started: its program run, unless the
/// arguments are a bluebox command (then this returns).  With no
/// arguments, options (--headless ...) or documents, the program runs
/// from its folder in Application Support, copied there first.
func runExported(_ argv: [String]) {
    guard let e = currentExport, let support = exportSupport() else { return }
    if let first = argv.first, !first.hasPrefix("-"), !FileManager.default.fileExists(atPath: first) { return }
    let app: String
    do {
        app = try ExportBundle.prepare(resources: e.resources, support: support, program: e.program, stamp: e.stamp)
    } catch {
        fail("cannot copy \(e.program) to \(support): \(error)")
    }
    if e.command == "screensaver" {
        screensaverCommand([app] + e.options + argv)
    }
    runCommand([app] + argv + e.options)
}

// MARK: - The command

func exportUsage() -> String {
    """
      bluebox export <app-or-archive> [--app name] [--to folder] [--name N] [--bundle-id ID] [--background]
                [--sign identity] [--notarize profile] [--force]
          Make a macOS application of a classic program that runs on its own, on a
          Mac without Blue Box: it carries Blue Box, the program's folder, the
          fonts, extensions and Apple menu items of Blue Box's System Folder, the
          program's settings and theme, its Finder icon and its document types.
          What the program writes goes to ~/Library/Application Support/<bundle id>,
          where its folder is copied the first time it opens; the bundle itself is
          never changed.  --to: the folder to make it in (default the current one);
          --name: its name (default the program's); --bundle-id: its identifier
          (default local.bluebox.export.<creator>.<name>); --background: run as
          run --background does.  --sign: a code-signing identity (default -, ad
          hoc; a "Developer ID Application" one for other Macs to open it without
          a warning); --notarize: notarize with that identity and the notarytool
          keychain profile given, and staple the ticket (needs the Command Line
          Tools).  An After Dark module becomes a screen saver in a window.
          Whether you may share a program with others is up to you and its licence.
    """
}

#if canImport(AppKit)
import AppKit

/// The folder an export carries: the disk the program came on (its own
/// files at the volume's root), else the program's folder, else, for one
/// at the top of the library or the disk, just the program.
private func exportSource(app: String) -> (folder: String, single: Bool) {
    if let (root, _) = volumeRoot(containing: app) { return (root, false) }
    let parent = (app as NSString).deletingLastPathComponent
    let top = [defaultLibrary(), defaultDisk(), NSHomeDirectory(), "/"].map { ($0 as NSString).standardizingPath }
    if top.contains((parent as NSString).standardizingPath) { return (app, true) }
    return (parent, false)
}

private func folderSize(_ path: String) -> Int {
    let fm = FileManager.default
    var total = 0
    for rel in (try? fm.subpathsOfDirectory(atPath: path)) ?? [] {
        total += ((try? fm.attributesOfItem(atPath: path + "/" + rel))?[.size] as? Int) ?? 0
    }
    return total
}

func exportCommand(_ args: [String]) {
    var source: String?
    var appName: String?
    var folder = FileManager.default.currentDirectoryPath
    var bundleName: String?
    var bundleID: String?
    var background = false
    var identity = "-"
    var profile: String?
    var force = false
    var i = 0
    func value() -> String {
        i += 1
        guard i < args.count, !args[i].isEmpty else { usage() }
        return args[i]
    }
    while i < args.count {
        switch args[i] {
        case "--app": appName = value()
        case "--to": folder = (value() as NSString).expandingTildeInPath
        case "--name": bundleName = value()
        case "--bundle-id": bundleID = value()
        case "--background": background = true
        case "--sign": identity = value()
        case "--notarize": profile = value()
        case "--force": force = true
        default:
            if args[i].hasPrefix("-") || source != nil { usage() }
            source = args[i]
        }
        i += 1
    }
    guard let source else { usage() }
    if let bundleID, bundleID.isEmpty || bundleID.contains(where: { !($0.isASCII && ($0.isLetter || $0.isNumber || $0 == "." || $0 == "-")) }) {
        fail("--bundle-id: \(bundleID) is not a bundle identifier (letters, digits, '.' and '-')")
    }
    var notary: NotaryTools?
    if let profile {
        guard identity != "-" else {
            fail("--notarize needs a Developer ID identity (--sign \"Developer ID Application: ...\"); ad hoc signatures can't be notarized")
        }
        guard let tools = ExportSigning.notaryTools() else { fail(ExportSigning.toolsNote) }
        notary = tools
        _ = profile
    }
    if identity != "-" {
        let known = ExportSigning.identities()
        guard known.contains(where: { $0 == identity || $0.contains(identity) }) else {
            fail("no code-signing identity \"\(identity)\" in the keychain"
                 + (known.isEmpty ? "" : "; there are: " + known.joined(separator: "; ")))
        }
        if !known.contains(where: { $0.contains(identity) && $0.hasPrefix("Developer ID Application:") }) {
            eprint("note: \(identity) is not a Developer ID identity; other Macs will refuse it as they refuse an ad hoc one\n")
        }
    }

    let app = URL(fileURLWithPath: resolveApplication(source, named: appName)).standardizedFileURL.path
    let file = (app as NSString).lastPathComponent
    let name = bundleName ?? file
    let fm = FileManager.default
    let bundlePath = (folder as NSString).appendingPathComponent(name.replacingOccurrences(of: "/", with: ":") + ".app")
    if fm.fileExists(atPath: bundlePath) {
        let existing = NSDictionary(contentsOfFile: bundlePath + "/Contents/Info.plist")
        guard force || existing?[ExportBundle.infoKey] != nil else {
            fail("\(bundlePath) exists and was not made by bluebox export (use --force to replace it)")
        }
    }
    guard let selfPath = Bundle.main.executableURL?.resolvingSymlinksInPath().path else {
        fail("cannot find the bluebox executable")
    }

    // What the program is: its creator, its kind.
    var mf = mac_file()
    var err = [CChar](repeating: 0, count: 256)
    var creator: UInt32 = 0, type: UInt32 = 0
    var isDeskAccessory = false
    if appledouble_read(app, &mf, &err, err.count) {
        creator = mac_file_creator(&mf)
        type = mac_file_type(&mf)
        isDeskAccessory = type == fourCCValue("dfil") || !deskAccessoryNames(fork: mf.rsrc, length: mf.rsrc_len).isEmpty
            && type != fourCCValue("APPL")
        mac_file_free(&mf)
    }
    let isModule = [fourCCValue("ADgm"), fourCCValue("A4gm")].contains(type)
    // (A desk accessory file's creator is the Font/DA Mover's, not its own.)
    let id = bundleID ?? ExportBundle.bundleIdentifier(creator: isDeskAccessory ? 0 : creator, name: name)

    // Its settings over the defaults, as the run would have them, less
    // what belongs to this Mac; and the theme they name.
    var settings = exportableSettings((settings(for: app) ?? AppSettings()).over(defaultSettings()))
    var themeFiles: [String] = []
    if let a = settings.appearance, !["bw", "classic", "native"].contains(a) {
        if let theme = findTheme(a) {
            themeFiles = [theme] + scan((theme as NSString).deletingLastPathComponent)
                .filter { fourCCString($0.type) == "tsnd" }.map(\.path)
            settings.appearance = (theme as NSString).lastPathComponent
        } else {
            eprint("note: no theme \"\(a)\"; the export uses the default appearance\n")
            settings.appearance = nil
        }
    }

    // Made beside where it goes, then moved into place.
    let work = (folder as NSString).appendingPathComponent(".\(name.replacingOccurrences(of: "/", with: ":")).export-\(UUID().uuidString).app")
    let executable = name.replacingOccurrences(of: "/", with: ":")
    let resources = work + "/Contents/Resources"
    let (sourceFolder, single) = exportSource(app: app)
    let inProgram = single ? file : (sourceFolder as NSString).lastPathComponent + "/" + String(app.dropFirst(sourceFolder.count + 1))
    let finder = finderBundle(applicationAt: app)
    do {
        try fm.createDirectory(atPath: work + "/Contents/MacOS", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: resources, withIntermediateDirectories: true)
        try fm.copyItem(atPath: selfPath, toPath: work + "/Contents/MacOS/" + executable)
        try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: work + "/Contents/MacOS/" + executable)

        let programDest = resources + "/" + ExportBundle.programFolder + "/" + (single ? file : (sourceFolder as NSString).lastPathComponent)
        try ExportBundle.pack(sourceFolder, to: programDest)
        let system = defaultDisk() + "/System Folder"
        for sub in ExportBundle.carriedSystemFolders {
            let from = system + "/" + sub
            guard let items = try? fm.contentsOfDirectory(atPath: from), !items.isEmpty else { continue }
            try ExportBundle.pack(from, to: resources + "/" + ExportBundle.systemFolder + "/" + sub)
        }
        for theme in themeFiles {
            try ExportBundle.pack(theme, to: resources + "/" + ExportBundle.themesFolder + "/" + (theme as NSString).lastPathComponent)
        }

        let icns = icnsData(finder.appIcons)
        if let icns { try icns.write(to: URL(fileURLWithPath: resources + "/AppIcon.icns")) }
        var info = infoPlist(app: app, name: name, executable: executable, bundle: finder, creator: creator,
                             hasIcon: icns != nil)
        info[bundleAppKey] = nil // the program is in the bundle
        info["CFBundleIdentifier"] = id
        info["LSApplicationCategoryType"] = isModule ? "public.app-category.entertainment" : nil
        if isDeskAccessory { info["CFBundleSignature"] = nil } // a desk accessory file's creator is the Font/DA Mover's
        let settingsPlist = try PropertyListSerialization.propertyList(
            from: PropertyListEncoder().encode(settings), options: [], format: nil)
        info[ExportBundle.infoKey] = [
            ExportInfo.program: inProgram,
            ExportInfo.command: isModule ? "screensaver" : "run",
            ExportInfo.options: background ? ["--background"] : [],
            ExportInfo.settings: settingsPlist,
            ExportInfo.stamp: UUID().uuidString,
            ExportInfo.version: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev",
        ] as [String: Any]
        if background || isModule {
            info["CFBundleDocumentTypes"] = nil // it takes no documents
        }
        if background { info["LSUIElement"] = true }
        try addScripting(&info, resources: resources)
        let data = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
        try data.write(to: URL(fileURLWithPath: work + "/Contents/Info.plist"))
        try Data("APPL\(info["CFBundleSignature"] as? String ?? "????")".utf8)
            .write(to: URL(fileURLWithPath: work + "/Contents/PkgInfo"))
    } catch {
        try? fm.removeItem(atPath: work)
        fail("cannot make \(bundlePath): \(error)")
    }

    // Signed from the inside out, then checked.
    let entitlements = NSTemporaryDirectory() + "bluebox-export-\(UUID().uuidString).entitlements"
    defer { try? fm.removeItem(atPath: entitlements) }
    do { try Data(ExportSigning.entitlements.utf8).write(to: URL(fileURLWithPath: entitlements)) } catch {
        try? fm.removeItem(atPath: work)
        fail("cannot write \(entitlements): \(error)")
    }
    let signing = ExportSigning.signCommands(bundle: work, nested: nestedCode(work, executable: executable),
                                             identity: identity, entitlements: entitlements)
    for cmd in signing {
        let r = ExportSigning.runTool(cmd[0], Array(cmd.dropFirst()), [:])
        guard let r, r.status == 0 else {
            try? fm.removeItem(atPath: work)
            let out = r?.output.trimmingCharacters(in: .whitespacesAndNewlines) ?? "could not be started"
            fail("\((cmd[0] as NSString).lastPathComponent) failed: \(out)")
        }
    }
    do {
        if fm.fileExists(atPath: bundlePath) { try fm.removeItem(atPath: bundlePath) }
        try fm.moveItem(atPath: work, toPath: bundlePath)
    } catch {
        try? fm.removeItem(atPath: work)
        fail("cannot move the export to \(bundlePath): \(error)")
    }
    print(bundlePath)
    print("  bundle id \(id); its files go to ~/Library/Application Support/\(id)")
    let size = folderSize(bundlePath)
    print("  \(String(format: "%.1f", Double(size) / 1_048_576)) MB, signed \(identity == "-" ? "ad hoc" : "by \(identity)")")
    if finder.appIcons.isEmpty { print("  note: no Finder icon found; the application has the generic icon") }
    let types = finder.documentTypes.map(fourCCString).joined(separator: ", ")
    if !types.isEmpty && !background && !isModule { print("  documents: \(types)") }

    if let profile, let notary {
        let zip = NSTemporaryDirectory() + "bluebox-export-\(UUID().uuidString)/" + (bundlePath as NSString).lastPathComponent + ".zip"
        try? fm.createDirectory(atPath: (zip as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        defer { try? fm.removeItem(atPath: (zip as NSString).deletingLastPathComponent) }
        print("  notarizing (this waits for Apple's notary service)...")
        for cmd in ExportSigning.notarizeCommands(bundle: bundlePath, zip: zip, profile: profile) {
            guard let r = ExportSigning.runTool(cmd[0], Array(cmd.dropFirst()), ["DEVELOPER_DIR": notary.developerDir]) else {
                fail("cannot run \(cmd[0])")
            }
            if r.status != 0 {
                fail("\(cmd.dropFirst().first ?? cmd[0]) failed: \(r.output.trimmingCharacters(in: .whitespacesAndNewlines))")
            }
        }
        print("  notarized and stapled")
    }
    print("  sharing someone else's program is up to you and its licence")
}

/// Code inside the bundle other than its executable, deepest first:
/// frameworks and helper programs (an export has none of its own today).
private func nestedCode(_ bundle: String, executable: String) -> [String] {
    let fm = FileManager.default
    var out: [String] = []
    let macos = bundle + "/Contents/MacOS"
    for f in (try? fm.contentsOfDirectory(atPath: macos)) ?? [] where f != executable {
        out.append(macos + "/" + f)
    }
    let frameworks = bundle + "/Contents/Frameworks"
    for f in (try? fm.contentsOfDirectory(atPath: frameworks)) ?? [] {
        out.append(frameworks + "/" + f)
    }
    return out.sorted { $0.split(separator: "/").count > $1.split(separator: "/").count }
}
#endif
