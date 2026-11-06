// Exported applications (bluebox export): a classic program as a macOS
// application that stands on its own, with its own copy of Blue Box, the
// program's folder and the System Folder items a run uses, so it can be
// copied to another Mac that has no Blue Box.
//
// The bundle stays as it was signed: nothing is written into it.  On first
// launch the program's folder (and the fonts, extensions and theme it
// carries) is copied from Contents/Resources into the application's own
// folder in Application Support, named by its bundle identifier, and the
// program runs from there: its preferences, saved games and the files it
// changes in place all land there, as does the classic disk (Macintosh HD).
//
// In the bundle the AppleDouble sidecars ("._Name", the resource forks and
// Finder information) are kept under another name, ".ad_Name": the Finder,
// Archive Utility and ditto treat "._" files as macOS's own metadata when a
// copy of the application is zipped or unzipped, and would fold them into
// extended attributes, breaking both the signature and the program.

import Foundation
import ForkStore

public enum ExportBundle {
    /// The Info.plist key holding what the exported program is and how it runs
    /// (a dictionary: the ExportInfo keys).
    public static let infoKey = "BlueBoxExport"
    /// The program's folder, in Contents/Resources and in Application Support.
    public static let programFolder = "Program"
    /// System Folder items carried along, in Contents/Resources/System Folder.
    public static let systemFolder = "System Folder"
    /// What a run opens from the System Folder: the fonts (every run opens
    /// them), shared libraries and plug-ins, and the Apple menu's items.
    /// Not Preferences: they are the user's own, and the program makes new ones.
    public static let carriedSystemFolders = ["Fonts", "Extensions", "Apple Menu Items"]
    /// A theme the program's appearance setting names, with its sound sets.
    public static let themesFolder = "Themes"
    /// In Application Support: the export the files there came from.
    public static let stampFile = ".export-stamp"

    /// Files not carried: the import's note of where the program came from
    /// (a path on this Mac), and the Finder's window settings.
    static let leftOut: Set<String> = [".classic-source", ".DS_Store"]
    static let sidecarPrefix = "._"
    static let packedPrefix = ".ad_"

    /// A bundle identifier from the program's creator code and name, e.g.
    /// local.bluebox.export.ttxt.teachtext; characters a bundle identifier
    /// can't hold become hex (a creator) or hyphens (a name).
    public static func bundleIdentifier(creator: UInt32, name: String) -> String {
        var code = ""
        if creator != 0 && creator != 0x3F3F_3F3F {
            for shift in stride(from: 24, through: 0, by: -8) {
                let b = UInt8((creator >> UInt32(shift)) & 0xFF)
                if (b >= 0x30 && b <= 0x39) || (b >= 0x41 && b <= 0x5A) || (b >= 0x61 && b <= 0x7A) {
                    code.append(Character(UnicodeScalar(b)))
                } else {
                    code += "-" + String(format: "%02x", b)
                }
            }
        }
        var slug = ""
        for scalar in name.lowercased().unicodeScalars {
            if scalar.isASCII, CharacterSet.alphanumerics.contains(scalar) {
                slug.unicodeScalars.append(scalar)
            } else if !slug.isEmpty && !slug.hasSuffix("-") {
                slug += "-"
            }
        }
        while slug.hasSuffix("-") { slug.removeLast() }
        if slug.isEmpty { slug = "program" }
        return "local.bluebox.export." + (code.isEmpty ? "" : code + ".") + slug
    }

    /// The exported application's own folder: everything it writes.
    public static func supportFolder(home: String, bundleIdentifier: String) -> String {
        home + "/Library/Application Support/" + bundleIdentifier
    }

    /// Its JIT translations.
    public static func cacheFolder(home: String, bundleIdentifier: String) -> String {
        home + "/Library/Caches/" + bundleIdentifier
    }

    // MARK: - Packing

    /// Copy a folder (or one file and its sidecar) into the bundle as `dest`:
    /// resource forks macOS keeps with a file (extended attributes) become
    /// sidecars first, and sidecars are renamed so a zip keeps them as files.
    public static func pack(_ source: String, to dest: String) throws {
        let fm = FileManager.default
        try fm.createDirectory(atPath: (dest as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        try fm.copyItem(atPath: source, toPath: dest)
        var isDir: ObjCBool = false
        if fm.fileExists(atPath: source, isDirectory: &isDir), !isDir.boolValue {
            let side = sidecar(source)
            if fm.fileExists(atPath: side) { try fm.copyItem(atPath: side, toPath: sidecar(dest)) }
            try nativeForksToSidecar(dest)
            try renameSidecar(in: (dest as NSString).deletingLastPathComponent, name: (dest as NSString).lastPathComponent)
            return
        }
        for rel in try fm.subpathsOfDirectory(atPath: dest) {
            let name = (rel as NSString).lastPathComponent
            if leftOut.contains(name) {
                try fm.removeItem(atPath: (dest as NSString).appendingPathComponent(rel))
                continue
            }
            guard !name.hasPrefix(sidecarPrefix) else { continue }
            try nativeForksToSidecar((dest as NSString).appendingPathComponent(rel))
        }
        // Deepest first, so a folder is renamed after what is in it.
        for rel in try fm.subpathsOfDirectory(atPath: dest).sorted(by: { $0.count > $1.count }) {
            let path = (dest as NSString).appendingPathComponent(rel)
            let name = (path as NSString).lastPathComponent
            if name.hasPrefix(sidecarPrefix) {
                try renameSidecar(in: (path as NSString).deletingLastPathComponent, name: String(name.dropFirst(2)))
            }
        }
    }

    /// The sidecar of a file in the program's folder ("._Name").
    static func sidecar(_ path: String) -> String {
        ((path as NSString).deletingLastPathComponent as NSString)
            .appendingPathComponent(sidecarPrefix + (path as NSString).lastPathComponent)
    }

    private static func renameSidecar(in dir: String, name: String) throws {
        let from = (dir as NSString).appendingPathComponent(sidecarPrefix + name)
        guard FileManager.default.fileExists(atPath: from) else { return }
        try FileManager.default.moveItem(atPath: from, toPath: (dir as NSString).appendingPathComponent(packedPrefix + name))
    }

    /// A file whose resource fork or Finder information is in its extended
    /// attributes (as copies made by the Finder have them), and no sidecar:
    /// the sidecar is written from them, since the bundle keeps no
    /// extended attributes (codesign refuses them).
    private static func nativeForksToSidecar(_ path: String) throws {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: path, isDirectory: &isDir), !isDir.boolValue,
              !fm.fileExists(atPath: sidecar(path)), appledouble_has_native(path) else { return }
        var mf = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        guard appledouble_read(path, &mf, &err, err.count) else { return }
        defer { mac_file_free(&mf) }
        guard appledouble_write(path, &mf, &err, err.count) else {
            throw ExportError("cannot write the resource fork of \(path): \(String(cString: err))")
        }
    }

    /// Copy a packed folder into place, sidecars under their own names
    /// again, adding only what isn't there yet: a file the program has
    /// changed (or one the user put there) is kept.  Returns how many
    /// files were copied.
    @discardableResult
    public static func unpack(_ packed: String, to dest: String) throws -> Int {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: packed, isDirectory: &isDir) else { return 0 }
        if !isDir.boolValue {
            return try copyMissing(packed, to: dest)
        }
        try fm.createDirectory(atPath: dest, withIntermediateDirectories: true)
        var copied = 0
        for rel in try fm.subpathsOfDirectory(atPath: packed).sorted() {
            let from = (packed as NSString).appendingPathComponent(rel)
            let parts = rel.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
            let to = parts.map(unpackedName).reduce(dest) { ($0 as NSString).appendingPathComponent($1) }
            guard fm.fileExists(atPath: from, isDirectory: &isDir) else { continue }
            if isDir.boolValue {
                try fm.createDirectory(atPath: to, withIntermediateDirectories: true)
            } else {
                copied += try copyMissing(from, to: to)
            }
        }
        return copied
    }

    private static func unpackedName(_ name: String) -> String {
        name.hasPrefix(packedPrefix) ? sidecarPrefix + name.dropFirst(packedPrefix.count) : name
    }

    private static func copyMissing(_ from: String, to: String) throws -> Int {
        let fm = FileManager.default
        guard !fm.fileExists(atPath: to) else { return 0 }
        try fm.createDirectory(atPath: (to as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        try fm.copyItem(atPath: from, toPath: to)
        // Writable, though the bundle's copy may not be.
        if let mode = (try? fm.attributesOfItem(atPath: to))?[.posixPermissions] as? Int {
            try? fm.setAttributes([.posixPermissions: mode | 0o200], ofItemAtPath: to)
        }
        return 1
    }

    /// Before a run: the program's folder, the System Folder items and the
    /// themes from the bundle's Resources into the application's folder in
    /// Application Support (`support`), once for each export (`stamp`), and
    /// again if the program's file has gone.  `program` is the program's
    /// path inside the Program folder.  Returns the program's path there.
    @discardableResult
    public static func prepare(resources: String, support: String, program: String, stamp: String) throws -> String {
        let fm = FileManager.default
        let app = support + "/" + programFolder + "/" + program
        let stampPath = support + "/" + stampFile
        if fm.fileExists(atPath: app),
           let kept = fm.contents(atPath: stampPath), String(decoding: kept, as: UTF8.self) == stamp {
            return app
        }
        try fm.createDirectory(atPath: support, withIntermediateDirectories: true)
        try unpack(resources + "/" + programFolder, to: support + "/" + programFolder)
        for folder in carriedSystemFolders {
            try unpack(resources + "/" + systemFolder + "/" + folder, to: support + "/Macintosh HD/System Folder/" + folder)
        }
        try unpack(resources + "/" + themesFolder, to: support + "/" + themesFolder)
        try Data(stamp.utf8).write(to: URL(fileURLWithPath: stampPath))
        return app
    }
}

/// The keys of the Info.plist's BlueBoxExport dictionary.
public enum ExportInfo {
    /// The program, a path inside the Program folder.
    public static let program = "Program"
    /// "run", or "screensaver" for an After Dark module.
    public static let command = "Command"
    /// Options it runs with (e.g. --background).
    public static let options = "Options"
    /// Its settings (AppSettings, as a property list), the bundle's defaults.
    public static let settings = "Settings"
    /// A new value for each export, so a newer one's files are added.
    public static let stamp = "Stamp"
    /// The Blue Box it carries.
    public static let version = "BlueBoxVersion"
}

public struct ExportError: Error, CustomStringConvertible {
    public let description: String
    public init(_ description: String) { self.description = description }
}

// MARK: - Signing

/// Apple's tools for notarizing, found without the Command Line Tools
/// installer ever being asked to appear.
public struct NotaryTools: Equatable, Sendable {
    /// The developer directory (xcode-select -p), for DEVELOPER_DIR.
    public var developerDir: String
    public var notarytool: String
    public var stapler: String
    public init(developerDir: String, notarytool: String, stapler: String) {
        self.developerDir = developerDir
        self.notarytool = notarytool
        self.stapler = stapler
    }
}

/// How an export can be signed on this Mac.
public struct SigningChoices: Equatable, Sendable {
    /// Ad hoc (codesign is on every Mac): runs on this Mac, and on another
    /// once the Gatekeeper warning is answered.
    public var adHoc = true
    /// The keychain's "Developer ID Application" identities, by name.
    public var developerIDs: [String] = []
    /// Notarizing and stapling, when the tools are there.
    public var notary: NotaryTools?
    /// Why notarizing is not offered, and how to make it possible.
    public var notaryNote: String? {
        if notary == nil { return ExportSigning.toolsNote }
        if developerIDs.isEmpty { return ExportSigning.developerIDNote }
        return nil
    }
    public var canNotarize: Bool { notary != nil && !developerIDs.isEmpty }
    public init(developerIDs: [String] = [], notary: NotaryTools? = nil) {
        self.developerIDs = developerIDs
        self.notary = notary
    }
}

public enum ExportSigning {
    /// Runs a tool and gives its status and output (standard output and
    /// error), or nil when it can't be started.
    public typealias Runner = (_ tool: String, _ arguments: [String], _ environment: [String: String]) -> (status: Int32, output: String)?

    public static let toolsNote = "Notarizing needs Apple's Command Line Tools: run \"xcode-select --install\" in Terminal "
        + "(or install Xcode), then export again."
    public static let developerIDNote = "Notarizing needs a Developer ID Application certificate in the keychain "
        + "(from the Apple Developer Program)."

    /// What this Mac offers.
    public static func detect(environment: [String: String] = ProcessInfo.processInfo.environment,
                              fileExists: (String) -> Bool = { FileManager.default.fileExists(atPath: $0) },
                              run: Runner = runTool) -> SigningChoices {
        SigningChoices(developerIDs: developerIDIdentities(run: run),
                       notary: notaryTools(environment: environment, fileExists: fileExists, run: run))
    }

    /// The valid code-signing identities whose names say Developer ID
    /// Application, as `security find-identity -v -p codesigning` lists them.
    public static func developerIDIdentities(run: Runner = runTool) -> [String] {
        identities(run: run).filter { $0.hasPrefix("Developer ID Application:") }
    }

    /// Every valid code-signing identity's name.
    public static func identities(run: Runner = runTool) -> [String] {
        guard let r = run("/usr/bin/security", ["find-identity", "-v", "-p", "codesigning"], [:]), r.status == 0 else {
            return []
        }
        var names: [String] = []
        for line in r.output.split(separator: "\n") {
            // "  1) 0123ABCD... "Developer ID Application: Name (TEAMID)""
            guard let open = line.firstIndex(of: "\""), let close = line.lastIndex(of: "\""), open < close,
                  line[..<open].contains(")") else { continue }
            let name = String(line[line.index(after: open)..<close])
            if !names.contains(name) { names.append(name) }
        }
        return names
    }

    /// notarytool and stapler, if the developer tools are installed.  On a
    /// Mac without them /usr/bin/xcrun is a stub that offers to install
    /// them, so it is asked only once the developer directory (DEVELOPER_DIR,
    /// else `xcode-select -p`, which never offers) is known to exist and to
    /// hold both tools.
    public static func notaryTools(environment: [String: String] = ProcessInfo.processInfo.environment,
                                   fileExists: (String) -> Bool = { FileManager.default.fileExists(atPath: $0) },
                                   run: Runner = runTool) -> NotaryTools? {
        var dir = environment["DEVELOPER_DIR"] ?? ""
        if dir.isEmpty {
            guard let r = run("/usr/bin/xcode-select", ["-p"], [:]), r.status == 0 else { return nil }
            dir = r.output.trimmingCharacters(in: .whitespacesAndNewlines)
        }
        guard !dir.isEmpty, fileExists(dir), fileExists(dir + "/usr/bin/notarytool"), fileExists(dir + "/usr/bin/stapler") else {
            return nil
        }
        var found: [String] = []
        for tool in ["notarytool", "stapler"] {
            guard let r = run("/usr/bin/xcrun", ["--find", tool], ["DEVELOPER_DIR": dir]), r.status == 0 else { return nil }
            let path = r.output.split(separator: "\n").last.map { $0.trimmingCharacters(in: .whitespaces) } ?? ""
            guard !path.isEmpty, fileExists(path) else { return nil }
            found.append(path)
        }
        return NotaryTools(developerDir: dir, notarytool: found[0], stapler: found[1])
    }

    /// The commands that sign a bundle, in order: its extended attributes
    /// removed (codesign refuses Finder information), then from the inside
    /// out, each with the hardened runtime: `nested` code (deepest first, as
    /// given), then the bundle with the entitlements; ad hoc ("-") without a
    /// timestamp, any other identity with one.  Then a strict check.
    public static func signCommands(bundle: String, nested: [String] = [], identity: String,
                                    entitlements: String) -> [[String]] {
        func sign(_ extra: [String], _ path: String) -> [String] {
            ["/usr/bin/codesign", "--force", "--sign", identity, "--options", "runtime"]
                + (identity == "-" ? [] : ["--timestamp"]) + extra + [path]
        }
        return [["/usr/bin/xattr", "-cr", bundle]]
            + nested.map { sign([], $0) }
            + [sign(["--entitlements", entitlements], bundle),
               ["/usr/bin/codesign", "--verify", "--deep", "--strict", bundle]]
    }

    /// The commands that notarize a signed bundle and staple the ticket to
    /// it: zipped for the notary service (ditto keeps the bundle as it is),
    /// submitted and waited for, then stapled.  Run with DEVELOPER_DIR set
    /// to the tools' developer directory.
    public static func notarizeCommands(bundle: String, zip: String, profile: String) -> [[String]] {
        [["/usr/bin/ditto", "-c", "-k", "--keepParent", bundle, zip],
         ["/usr/bin/xcrun", "notarytool", "submit", zip, "--keychain-profile", profile, "--wait"],
         ["/usr/bin/xcrun", "stapler", "staple", bundle]]
    }

    /// The entitlements Blue Box runs with (scripts/BlueBox.entitlements,
    /// which Blue Box.app does not carry): the translators' JIT, libvlc
    /// from another team, the microphone.
    public static let entitlements = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
        	<key>com.apple.security.cs.allow-jit</key>
        	<true/>
        	<key>com.apple.security.cs.disable-library-validation</key>
        	<true/>
        	<key>com.apple.security.device.audio-input</key>
        	<true/>
        </dict>
        </plist>

        """

    /// Run a tool, waiting for it; its output and errors together.
    public static func runTool(_ tool: String, _ arguments: [String], _ environment: [String: String]) -> (status: Int32, output: String)? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = arguments
        if !environment.isEmpty {
            p.environment = ProcessInfo.processInfo.environment.merging(environment) { _, new in new }
        }
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(decoding: data, as: UTF8.self))
    }
}
