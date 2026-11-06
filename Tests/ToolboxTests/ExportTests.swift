import BlueBoxHost
import Foundation
import ForkStore
import Testing

/// bluebox export: the bundle's layout and Info.plist, the settings baked
/// in, where an exported program's files go, its run from the bundle with
/// no Blue Box library, and the signing choices and commands.
@Suite struct ExportTests {
    // MARK: - Bundle identifiers

    @Test func bundleIdentifierFromCreatorAndName() {
        #expect(ExportBundle.bundleIdentifier(creator: fourCC("ttxt"), name: "TeachText") == "local.bluebox.export.ttxt.teachtext")
        // A creator's characters a bundle identifier can't hold, as hex; a name's, as hyphens.
        #expect(ExportBundle.bundleIdentifier(creator: 0x506F_C450, name: "Prince of Persia™")
                    == "local.bluebox.export.Po-c4P.prince-of-persia")
        #expect(ExportBundle.bundleIdentifier(creator: fourCC("SP  "), name: "PaintScroll 1.0")
                    == "local.bluebox.export.SP-20-20.paintscroll-1-0")
        #expect(ExportBundle.bundleIdentifier(creator: 0, name: "ƒ") == "local.bluebox.export.program")
        #expect(ExportBundle.bundleIdentifier(creator: fourCC("????"), name: "Moose") == "local.bluebox.export.moose")
    }

    @Test func supportAndCacheFolders() {
        #expect(ExportBundle.supportFolder(home: "/Users/u", bundleIdentifier: "local.x")
                    == "/Users/u/Library/Application Support/local.x")
        #expect(ExportBundle.cacheFolder(home: "/Users/u", bundleIdentifier: "local.x") == "/Users/u/Library/Caches/local.x")
    }

    // MARK: - Packing and the first run

    /// A program folder: an application (data and sidecar), a data file of
    /// its own in a folder, the import's note of its source, and a file
    /// whose resource fork is in its extended attributes.
    private func programFolder() throws -> String {
        let dir = tempDir().appendingPathComponent("Game").path
        let fm = FileManager.default
        try fm.createDirectory(atPath: dir + "/Data", withIntermediateDirectories: true)
        try Data("app".utf8).write(to: URL(fileURLWithPath: dir + "/Game"))
        try Data("sidecar".utf8).write(to: URL(fileURLWithPath: dir + "/._Game"))
        try Data("level 1".utf8).write(to: URL(fileURLWithPath: dir + "/Data/Levels"))
        try Data("/Users/someone/Downloads/Game.sit".utf8).write(to: URL(fileURLWithPath: dir + "/.classic-source"))
        let native = dir + "/Native"
        try Data("data".utf8).write(to: URL(fileURLWithPath: native))
        let fork = buildFork([TestResource(type: "STR ", id: 128, data: [3, 0x61, 0x62, 0x63])])
        #expect(fork.withUnsafeBufferPointer { setxattr(native, "com.apple.ResourceFork", $0.baseAddress, $0.count, 0, 0) } == 0)
        return dir
    }

    @Test func packRenamesSidecarsAndCarriesNativeForks() throws {
        let src = try programFolder()
        let dest = tempDir().appendingPathComponent("Program/Game").path
        try ExportBundle.pack(src, to: dest)
        let fm = FileManager.default
        #expect(fm.fileExists(atPath: dest + "/Game"))
        #expect(fm.fileExists(atPath: dest + "/.ad_Game"))
        #expect(!fm.fileExists(atPath: dest + "/._Game"))
        #expect(fm.fileExists(atPath: dest + "/Data/Levels"))
        #expect(!fm.fileExists(atPath: dest + "/.classic-source"))
        // The fork from the extended attributes, now in a (renamed) sidecar.
        #expect(fm.fileExists(atPath: dest + "/.ad_Native"))
        // The source is untouched.
        #expect(fm.fileExists(atPath: src + "/._Game") && !fm.fileExists(atPath: src + "/._Native"))

        // Unpacked: the sidecars under their own names, and readable.
        let out = tempDir().appendingPathComponent("Game").path
        #expect(try ExportBundle.unpack(dest, to: out) == 5) // Game, Levels, Native and two sidecars
        #expect(fm.contents(atPath: out + "/._Game") == Data("sidecar".utf8))
        var mf = mac_file()
        var err = [CChar](repeating: 0, count: 256)
        #expect(appledouble_read(out + "/Native", &mf, &err, err.count))
        #expect(mf.rsrc_len > 0)
        mac_file_free(&mf)

        // Again over a folder the program has changed: only what is missing comes back.
        try Data("saved".utf8).write(to: URL(fileURLWithPath: out + "/Data/Levels"))
        try fm.removeItem(atPath: out + "/Game")
        #expect(try ExportBundle.unpack(dest, to: out) == 1)
        #expect(fm.contents(atPath: out + "/Data/Levels") == Data("saved".utf8))
    }

    @Test func prepareCopiesOncePerExport() throws {
        let resources = tempDir().path
        try ExportBundle.pack(try programFolder(), to: resources + "/Program/Game")
        let fonts = tempDir().appendingPathComponent("Fonts").path
        try FileManager.default.createDirectory(atPath: fonts, withIntermediateDirectories: true)
        try Data("font".utf8).write(to: URL(fileURLWithPath: fonts + "/Charcoal"))
        try ExportBundle.pack(fonts, to: resources + "/System Folder/Fonts")

        let support = tempDir().appendingPathComponent("local.bluebox.export.game").path
        let app = try ExportBundle.prepare(resources: resources, support: support, program: "Game/Game", stamp: "one")
        let fm = FileManager.default
        #expect(app == support + "/Program/Game/Game")
        #expect(fm.fileExists(atPath: support + "/Program/Game/._Game"))
        #expect(fm.fileExists(atPath: support + "/Macintosh HD/System Folder/Fonts/Charcoal"))
        #expect(fm.contents(atPath: support + "/.export-stamp") == Data("one".utf8))

        // The same export: nothing copied again (a file the user removed stays removed).
        try fm.removeItem(atPath: support + "/Program/Game/Data/Levels")
        _ = try ExportBundle.prepare(resources: resources, support: support, program: "Game/Game", stamp: "one")
        #expect(!fm.fileExists(atPath: support + "/Program/Game/Data/Levels"))
        // A newer export adds what is missing.
        _ = try ExportBundle.prepare(resources: resources, support: support, program: "Game/Game", stamp: "two")
        #expect(fm.fileExists(atPath: support + "/Program/Game/Data/Levels"))
    }

    // MARK: - Signing

    /// A stand-in for the tools, recording what was run.
    final class Tools: @unchecked Sendable {
        var answers: [String: (Int32, String)] = [:]
        var ran: [String] = []
        func run(_ tool: String, _ args: [String], _ env: [String: String]) -> (status: Int32, output: String)? {
            let line = ([tool] + args).joined(separator: " ")
            ran.append(line)
            guard let a = answers[line] else { return nil }
            return (a.0, a.1)
        }
    }

    @Test func developerIDOnlyWhenTheKeychainHasOne() {
        let tools = Tools()
        let find = "/usr/bin/security find-identity -v -p codesigning"
        tools.answers[find] = (0, """
              1) 0123456789ABCDEF0123456789ABCDEF01234567 "Apple Development: Some One (ABCDE12345)"
                 1 valid identities found

            """)
        #expect(ExportSigning.developerIDIdentities(run: tools.run).isEmpty)
        #expect(ExportSigning.identities(run: tools.run) == ["Apple Development: Some One (ABCDE12345)"])
        tools.answers[find] = (0, """
              1) 0123456789ABCDEF0123456789ABCDEF01234567 "Apple Development: Some One (ABCDE12345)"
              2) 89ABCDEF0123456789ABCDEF0123456789ABCDEF "Developer ID Application: Some One (ABCDE12345)"
                 2 valid identities found

            """)
        #expect(ExportSigning.developerIDIdentities(run: tools.run) == ["Developer ID Application: Some One (ABCDE12345)"])
        tools.answers[find] = (1, "security: error")
        #expect(ExportSigning.identities(run: tools.run).isEmpty)
    }

    @Test func notaryToolsNeverAskTheInstallerStub() {
        // No developer directory: xcode-select says so, and xcrun (the stub
        // that offers to install the tools) is never run.
        let tools = Tools()
        tools.answers["/usr/bin/xcode-select -p"] = (2, "xcode-select: error: unable to get active developer directory")
        #expect(ExportSigning.notaryTools(environment: [:], fileExists: { _ in true }, run: tools.run) == nil)
        #expect(!tools.ran.contains { $0.hasPrefix("/usr/bin/xcrun") })

        // A developer directory that has gone, or lacks the tools: still no xcrun.
        let clt = "/Library/Developer/CommandLineTools"
        tools.answers["/usr/bin/xcode-select -p"] = (0, clt + "\n")
        #expect(ExportSigning.notaryTools(environment: [:], fileExists: { _ in false }, run: tools.run) == nil)
        #expect(ExportSigning.notaryTools(environment: [:], fileExists: { !$0.hasSuffix("stapler") }, run: tools.run) == nil)
        #expect(!tools.ran.contains { $0.hasPrefix("/usr/bin/xcrun") })

        // Both there: xcrun finds them.
        tools.answers["/usr/bin/xcrun --find notarytool"] = (0, clt + "/usr/bin/notarytool\n")
        tools.answers["/usr/bin/xcrun --find stapler"] = (0, clt + "/usr/bin/stapler\n")
        let found = ExportSigning.notaryTools(environment: [:], fileExists: { _ in true }, run: tools.run)
        #expect(found == NotaryTools(developerDir: clt, notarytool: clt + "/usr/bin/notarytool", stapler: clt + "/usr/bin/stapler"))

        // DEVELOPER_DIR wins over xcode-select.
        tools.ran = []
        _ = ExportSigning.notaryTools(environment: ["DEVELOPER_DIR": clt], fileExists: { _ in true }, run: tools.run)
        #expect(!tools.ran.contains("/usr/bin/xcode-select -p"))
    }

    @Test func signingChoicesSayWhyNotarizingIsOff() {
        #expect(SigningChoices().adHoc)
        #expect(SigningChoices().notaryNote == ExportSigning.toolsNote)
        #expect(!SigningChoices().canNotarize)
        let tools = NotaryTools(developerDir: "/d", notarytool: "/d/n", stapler: "/d/s")
        #expect(SigningChoices(notary: tools).notaryNote == ExportSigning.developerIDNote)
        let both = SigningChoices(developerIDs: ["Developer ID Application: X (T)"], notary: tools)
        #expect(both.canNotarize && both.notaryNote == nil)
    }

    @Test func codesignRunsInsideOut() {
        let adHoc = ExportSigning.signCommands(bundle: "/x/A.app", nested: ["/x/A.app/Contents/Frameworks/F.framework",
                                                                             "/x/A.app/Contents/MacOS/helper"],
                                               identity: "-", entitlements: "/t/e.plist")
        #expect(adHoc == [
            ["/usr/bin/xattr", "-cr", "/x/A.app"],
            ["/usr/bin/codesign", "--force", "--sign", "-", "--options", "runtime", "/x/A.app/Contents/Frameworks/F.framework"],
            ["/usr/bin/codesign", "--force", "--sign", "-", "--options", "runtime", "/x/A.app/Contents/MacOS/helper"],
            ["/usr/bin/codesign", "--force", "--sign", "-", "--options", "runtime", "--entitlements", "/t/e.plist", "/x/A.app"],
            ["/usr/bin/codesign", "--verify", "--deep", "--strict", "/x/A.app"],
        ])
        let id = "Developer ID Application: X (T)"
        let signed = ExportSigning.signCommands(bundle: "/x/A.app", identity: id, entitlements: "/t/e.plist")
        #expect(signed[1] == ["/usr/bin/codesign", "--force", "--sign", id, "--options", "runtime", "--timestamp",
                              "--entitlements", "/t/e.plist", "/x/A.app"])
    }

    @Test func notarizeThenStaple() {
        #expect(ExportSigning.notarizeCommands(bundle: "/x/A.app", zip: "/t/A.app.zip", profile: "notary") == [
            ["/usr/bin/ditto", "-c", "-k", "--keepParent", "/x/A.app", "/t/A.app.zip"],
            ["/usr/bin/xcrun", "notarytool", "submit", "/t/A.app.zip", "--keychain-profile", "notary", "--wait"],
            ["/usr/bin/xcrun", "stapler", "staple", "/x/A.app"],
        ])
    }

    /// The entitlements an export is signed with are Blue Box.app's.
    @Test func entitlementsMatchTheApp() throws {
        let file = repositoryRoot() + "/scripts/BlueBox.entitlements"
        let theirs = try PropertyListSerialization.propertyList(from: Data(contentsOf: URL(fileURLWithPath: file)),
                                                                format: nil) as? NSDictionary
        let ours = try PropertyListSerialization.propertyList(from: Data(ExportSigning.entitlements.utf8),
                                                              format: nil) as? NSDictionary
        #expect(theirs != nil && theirs == ours)
    }

    // MARK: - A real export, run from its bundle

    /// A 68k program that creates a file "Saved" in the default folder
    /// (its own, as the Finder launched programs) and quits.
    private func savingProgram(in dir: String) -> String {
        let code0 = cat(be32(40), be32(256), be32(8), be32(32), be16(0), be16(0x3F3C), be16(1), be16(0xA9F0))
        let code1 = cat(be16(0), be16(1),
                        be16(0x9EFC), be16(80),           // SUBA.W #80,SP: a parameter block
                        be16(0x204F),                     // MOVEA.L SP,A0
                        be16(0x42A8), be16(12),           // CLR.L ioCompletion(A0)
                        be16(0x43FA), be16(18),           // LEA name(PC),A1
                        be16(0x2149), be16(18),           // MOVE.L A1,ioNamePtr(A0)
                        be16(0x4268), be16(22),           // CLR.W ioVRefNum(A0)
                        be16(0x4228), be16(26),           // CLR.B ioFVersNum(A0)
                        be16(0xA008),                     // _Create
                        be16(0xA9F4),                     // _ExitToShell
                        [5] + Array("Saved".utf8))
        var fork = buildFork([
            TestResource(type: "CODE", id: 0, data: code0),
            TestResource(type: "CODE", id: 1, data: code1),
            TestResource(type: "SIZE", id: -1, data: cat(be16(0), be32(0x80000), be32(0x80000))),
        ])
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let path = dir + "/Saver"
        var f = mac_file()
        f.has_finder_info = true
        withUnsafeMutableBytes(of: &f.finder_info) { b in
            for (i, c) in "APPLSvTt".utf8.enumerated() { b[i] = c }
        }
        var empty: [UInt8] = []
        empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
        fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
        var err = [CChar](repeating: 0, count: 256)
        #expect(appledouble_write(path, &f, &err, err.count))
        return path
    }

    private func run(_ tool: String, _ args: [String], home: String) -> (Int32, String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        var env = ProcessInfo.processInfo.environment
        env["HOME"] = home
        env["BLUEBOX_LIBRARY"] = nil
        env["TMPDIR"] = NSTemporaryDirectory()
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return (-1, "cannot run \(tool)") }
        let out = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(decoding: out, as: UTF8.self))
    }

    @Test(.enabled(if: blueboxExecutable() != nil, "the bluebox executable is built"))
    func exportedProgramRunsFromItsBundle() throws {
        let bluebox = blueboxExecutable()!
        let fm = FileManager.default
        let home = tempDir().path
        let src = savingProgram(in: tempDir().path + "/Saver Folder")
        try Data("level".utf8).write(to: URL(fileURLWithPath: (src as NSString).deletingLastPathComponent + "/Levels"))

        // Its settings, with some that stay on this Mac.
        var r = run(bluebox, ["set", src, "depth", "8", "scale", "2", "user-name", "Some One", "email", "a@b.c"], home: home)
        #expect(r.0 == 0, "\(r.1)")
        r = run(bluebox, ["set", "--default", "cpu", "68020"], home: home)
        #expect(r.0 == 0, "\(r.1)")

        let out = tempDir().path
        r = run(bluebox, ["export", src, "--to", out, "--name", "Saver Test"], home: home)
        #expect(r.0 == 0, "\(r.1)")
        let app = out + "/Saver Test.app"
        let contents = app + "/Contents"

        // The layout: Blue Box without Sparkle, the program's folder, its sidecars renamed.
        #expect(fm.isExecutableFile(atPath: contents + "/MacOS/Saver Test"))
        #expect(!fm.fileExists(atPath: contents + "/Frameworks"))
        #expect(fm.fileExists(atPath: contents + "/Resources/Program/Saver Folder/.ad_Saver"))
        #expect(fm.fileExists(atPath: contents + "/Resources/Program/Saver Folder/Levels"))
        #expect(fm.fileExists(atPath: contents + "/_CodeSignature/CodeResources"))
        let info = try #require(NSDictionary(contentsOfFile: contents + "/Info.plist") as? [String: Any])
        #expect(info["CFBundleIdentifier"] as? String == "local.bluebox.export.SvTt.saver-test")
        #expect(info["CFBundleExecutable"] as? String == "Saver Test")
        #expect(info["CFBundleName"] as? String == "Saver Test")
        #expect(info["CFBundleSignature"] as? String == "SvTt")
        #expect(info["SUFeedURL"] == nil && info["ClassicApplicationPath"] == nil)
        #expect((info["CFBundleDocumentTypes"] as? [Any])?.isEmpty == false)
        let export = try #require(info[ExportBundle.infoKey] as? [String: Any])
        #expect(export[ExportInfo.program] as? String == "Saver Folder/Saver")
        #expect(export[ExportInfo.command] as? String == "run")
        // The settings baked in: the program's over the defaults, less the user's own.
        let settings = try #require(export[ExportInfo.settings] as? [String: Any])
        #expect(settings["depth"] as? Int == 8)
        #expect(settings["scale"] as? Double == 2)
        #expect(settings["cpu"] as? Int == 68020)
        #expect(settings["userName"] == nil && settings["internet"] == nil)
        #expect(run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app], home: home).0 == 0)

        // Run from the bundle on a Mac without Blue Box: a fresh home.
        let elsewhere = tempDir().path
        r = run(contents + "/MacOS/Saver Test", ["--headless", "--max", "5000000"], home: elsewhere)
        #expect(r.0 == 0, "\(r.1)")
        #expect(r.1.contains("ExitToShell"), "\(r.1)")
        let support = elsewhere + "/Library/Application Support/local.bluebox.export.SvTt.saver-test"
        #expect(fm.fileExists(atPath: support + "/Program/Saver Folder/._Saver"))
        #expect(fm.fileExists(atPath: support + "/Program/Saver Folder/Saved"), "\(r.1)")
        #expect(fm.fileExists(atPath: support + "/Macintosh HD"))
        #expect(!fm.fileExists(atPath: elsewhere + "/Library/Application Support/Blue Box"))
        #expect(!fm.fileExists(atPath: contents + "/Resources/Program/Saver Folder/Saved"))
        // The bundle is as it was signed.
        #expect(run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app], home: home).0 == 0)

        // Made again over it, and a bundle export didn't make is not replaced.
        #expect(run(bluebox, ["export", src, "--to", out, "--name", "Saver Test"], home: home).0 == 0)
        try fm.createDirectory(atPath: out + "/Other.app/Contents", withIntermediateDirectories: true)
        #expect(run(bluebox, ["export", src, "--to", out, "--name", "Other"], home: home).0 != 0)
    }

    /// A theme the program's appearance names goes with it, with the sound
    /// sets beside it, and the setting names the copy.
    @Test(.enabled(if: blueboxExecutable() != nil, "the bluebox executable is built"))
    func exportCarriesTheTheme() throws {
        let bluebox = blueboxExecutable()!
        let fm = FileManager.default
        let home = tempDir().path
        let src = savingProgram(in: tempDir().path + "/Themed")
        let themes = home + "/Library/Application Support/Blue Box/Library/Themes"
        try fm.createDirectory(atPath: themes, withIntermediateDirectories: true)
        for (name, type) in [("Gilded", "thmeappr"), ("Gilded Sounds", "tsndappr")] {
            var f = mac_file()
            f.has_finder_info = true
            withUnsafeMutableBytes(of: &f.finder_info) { b in
                for (i, c) in type.utf8.enumerated() { b[i] = c }
            }
            var fork = buildFork([TestResource(type: "STR ", id: 128, data: [1, 0x61])])
            var empty: [UInt8] = []
            empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(themes + "/" + name, &f, &err, err.count))
        }
        var r = run(bluebox, ["set", src, "appearance", "Gilded"], home: home)
        #expect(r.0 == 0, "\(r.1)")
        let out = tempDir().path
        r = run(bluebox, ["export", src, "--to", out], home: home)
        #expect(r.0 == 0, "\(r.1)")
        let resources = out + "/Saver.app/Contents/Resources"
        #expect(fm.fileExists(atPath: resources + "/Themes/Gilded"))
        #expect(fm.fileExists(atPath: resources + "/Themes/.ad_Gilded"))
        #expect(fm.fileExists(atPath: resources + "/Themes/.ad_Gilded Sounds"))
        let info = try #require(NSDictionary(contentsOfFile: out + "/Saver.app/Contents/Info.plist") as? [String: Any])
        let settings = (info[ExportBundle.infoKey] as? [String: Any])?[ExportInfo.settings] as? [String: Any]
        #expect(settings?["appearance"] as? String == "Gilded")
    }
}

/// The package's folder.
func repositoryRoot() -> String {
    URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().path
}

/// The bluebox program `swift build` made (debug, else release).
func blueboxExecutable() -> String? {
    let root = repositoryRoot()
    return [root + "/.build/debug/bluebox", root + "/.build/release/bluebox"]
        .first { FileManager.default.isExecutableFile(atPath: $0) }
}
