// The Blue Box Browser's file operations (BlueBoxHost's ClassicFileOps):
// copying, moving, renaming, duplicating, aliases and the Trash keep a
// classic file's fork, Finder information and comment, and a folder's
// catalog entries; drags out give the host a file with its own forks, and
// drags in a sidecar.  Classic icons are found as the Finder found them,
// and Quick Look's thumbnail and preview are drawn from them.  All in
// temporary folders, with a catalog of their own.

import BlueBoxHost
import CCore
import ClassicFiles
import Foundation
import ForkStore
import Testing
import Toolbox
#if canImport(CoreGraphics)
import CoreGraphics
#endif

extension BlueBox {
    @Suite struct Browser {
        /// A temporary folder with a disk (Macintosh HD), a library, a
        /// Trash, and the catalog beside the disk.
        struct Place {
            let root: String, disk: String, library: String, trash: String
            let ops: ClassicFileOps

            init() throws {
                root = FileManager.default.temporaryDirectory.appendingPathComponent("bluebox-browser-\(UUID().uuidString)")
                    .path
                disk = root + "/Macintosh HD"
                library = root + "/Library"
                trash = root + "/Trash"
                for d in [disk, library, trash] { try FileManager.default.createDirectory(atPath: d, withIntermediateDirectories: true) }
                ops = ClassicFileOps(disk: disk)
            }

            /// A test Trash: names taken get " 2", " 3" ... as the system's
            /// get a time after them.
            func trashItem(_ url: URL) throws -> URL {
                var name = url.lastPathComponent, n = 2
                while FileManager.default.fileExists(atPath: trash + "/" + name) {
                    name = url.lastPathComponent + " \(n)"
                    n += 1
                }
                let to = URL(fileURLWithPath: trash + "/" + name)
                try FileManager.default.moveItem(at: url, to: to)
                return to
            }

            func cleanUp() {
                catalog_close()
                try? FileManager.default.removeItem(atPath: root)
            }
        }

        /// A classic file: a data fork, a resource fork, Finder information
        /// (type, creator, label 3) and a comment, in a sidecar.
        static func makeClassic(_ path: String, type: String = "TEXT", creator: String = "TEST",
                                fork: [UInt8] = buildFork([TestResource(type: "STR ", id: 128, data: [3, 0x61, 0x62, 0x63])]),
                                data: String = "hello") throws -> ClassicMeta {
            var meta = ClassicMeta()
            meta.type = fourCC(type)
            meta.creator = fourCC(creator)
            meta.label = 3
            meta.resourceFork = fork
            meta.commentText = "A comment"
            try Data(data.utf8).write(to: URL(fileURLWithPath: path))
            try meta.write(sidecarOf: path)
            return meta
        }

        static func sameClassicSide(_ a: ClassicMeta?, _ b: ClassicMeta) -> Bool {
            guard let a else { return false }
            return a.type == b.type && a.creator == b.creator && a.label == b.label && a.resourceFork == b.resourceFork
                && a.comment == b.comment
        }

        @Test func copyKeepsForkFinderInfoAndComment() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let src = p.library + "/Read Me"
            let meta = try Self.makeClassic(src)
            let dest = try p.ops.copy(src, into: p.disk)
            #expect(dest == p.disk + "/Read Me")
            #expect(Self.sameClassicSide(ClassicMeta.read(dest), meta))
            #expect(try String(contentsOfFile: dest, encoding: .utf8) == "hello")
            #expect(FileManager.default.fileExists(atPath: p.library + "/._Read Me")) // the original stays
            // A name taken is refused, not overwritten.
            #expect(throws: ClassicFileError.self) { try p.ops.copy(src, into: p.disk) }
        }

        @Test func moveRenameAndDuplicateTakeTheSidecar() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let src = p.disk + "/Doc"
            let meta = try Self.makeClassic(src)
            let renamed = try p.ops.rename(src, to: "Letter")
            #expect(!p.ops.exists(src) && Self.sameClassicSide(ClassicMeta.read(renamed), meta))
            #expect(!FileManager.default.fileExists(atPath: p.disk + "/._Doc"))
            let moved = try p.ops.move(renamed, into: p.library)
            #expect(Self.sameClassicSide(ClassicMeta.read(moved), meta))
            let copy1 = try p.ops.duplicate(moved), copy2 = try p.ops.duplicate(moved)
            #expect((copy1 as NSString).lastPathComponent == "Letter copy")
            #expect((copy2 as NSString).lastPathComponent == "Letter copy 2")
            #expect(Self.sameClassicSide(ClassicMeta.read(copy2), meta))
            #expect(p.ops.freeName("Picture.png", in: p.library, copy: true) == "Picture copy.png")
            // A file that is only a sidecar (an empty data fork) moves too.
            let lone = p.library + "/Empty"
            var m = ClassicMeta()
            m.type = fourCC("PREF")
            try m.write(sidecarOf: lone)
            let movedLone = try p.ops.move(lone, into: p.disk)
            #expect(ClassicMeta.read(movedLone)?.type == fourCC("PREF") && p.ops.exists(movedLone))
            // Nothing goes inside itself.
            try FileManager.default.createDirectory(atPath: p.disk + "/F", withIntermediateDirectories: true)
            #expect(throws: ClassicFileError.self) { try p.ops.move(p.disk + "/F", into: p.disk + "/F") }
        }

        @Test func foldersKeepTheirCatalogEntries() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let folder = p.disk + "/Games", inner = folder + "/Old"
            try FileManager.default.createDirectory(atPath: inner, withIntermediateDirectories: true)
            _ = try Self.makeClassic(inner + "/Save")
            try p.ops.update(folder) { $0.label = 2; $0.commentText = "Folder comment" }
            try p.ops.update(inner) { $0.label = 5 }
            #expect(p.ops.info(folder).meta.label == 2 && p.ops.info(folder).meta.commentText == "Folder comment")
            // Its ID, kept by its place on the disk, goes with it.
            #expect(catalog_add_node("Games", 1000))
            let copy = try p.ops.copy(folder, into: p.library)
            #expect(p.ops.info(copy).meta.label == 2 && p.ops.info(copy).meta.commentText == "Folder comment")
            #expect(p.ops.info(copy + "/Old").meta.label == 5)
            #expect(ClassicMeta.read(copy + "/Old/Save")?.label == 3)
            let moved = try p.ops.rename(folder, to: "Play")
            #expect(p.ops.info(moved).meta.label == 2 && p.ops.info(moved + "/Old").meta.label == 5)
            #expect(p.ops.info(folder).meta.label == 0)
            #expect(catalog_node_id("Play") == 1000 && catalog_node_id("Games") == 0)
        }

        @Test func trashKeepsPairsAndEmptyingForgets() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let a = p.disk + "/Note", b = p.library + "/Note"
            let meta = try Self.makeClassic(a)
            _ = try Self.makeClassic(b)
            let first = try p.ops.moveToTrash(a, trash: p.trashItem)
            let second = try p.ops.moveToTrash(b, trash: p.trashItem)
            #expect(first == p.trash + "/Note" && second == p.trash + "/Note 2")
            #expect(Self.sameClassicSide(ClassicMeta.read(second), meta))
            #expect(FileManager.default.fileExists(atPath: p.trash + "/._Note 2"))
            let folder = p.disk + "/Stuff"
            try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
            try p.ops.update(folder) { $0.label = 4 }
            let gone = try p.ops.moveToTrash(folder, trash: p.trashItem)
            #expect(p.ops.info(gone).meta.label == 4)
            // A locked file stays where it is.
            let locked = p.disk + "/Locked"
            _ = try Self.makeClassic(locked)
            try p.ops.setLocked(locked, true)
            #expect(p.ops.info(locked).locked)
            #expect(throws: ClassicFileError.self) { try p.ops.moveToTrash(locked, trash: p.trashItem) }
            try p.ops.setLocked(locked, false)
            #expect(try p.ops.emptyTrash(p.trash) == 3) // two files (their sidecars with them) and a folder
            #expect(directoryEntries(p.trash).isEmpty)
            #expect(p.ops.info(gone).meta.label == 0)
        }

        @Test func aliasesLeadToTheirOriginals() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let app = p.disk + "/Writer"
            _ = try Self.makeClassic(app, type: "APPL", creator: "WRTR")
            let alias = try p.ops.makeAlias(app)
            #expect((alias as NSString).lastPathComponent == "Writer alias")
            let meta = try #require(ClassicMeta.read(alias))
            #expect(meta.isAlias && meta.type == fourCC("adrp") && meta.creator == fourCC("WRTR"))
            #expect(p.ops.aliasTarget(alias) == (app as NSString).standardizingPath)
            let rec = try #require(ClassicAlias.record(inFork: meta.resourceFork))
            #expect(rec[6] == 0 && rec[7] == 2)                       // version 2
            #expect(Array(rec[51..<57]) == Array("Writer".utf8))       // the target's name
            #expect(String(decoding: rec[11..<11 + Int(rec[10])], as: UTF8.self) == "Macintosh HD")
            try FileManager.default.createDirectory(atPath: p.disk + "/Folder", withIntermediateDirectories: true)
            let folderAlias = try p.ops.makeAlias(p.disk + "/Folder")
            #expect(ClassicMeta.read(folderAlias)?.type == fourCC("fdrp"))
            #expect(try p.ops.makeAlias(app).hasSuffix("Writer alias 2"))
        }

        #if canImport(Darwin)
        @Test func dragsOutGiveOwnForksAndDragsInGiveSidecars() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let src = p.library + "/Chart"
            let meta = try Self.makeClassic(src, type: "PICT", creator: "ttxt")
            let out = p.root + "/Desktop"
            try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
            try p.ops.exportForHost(src, to: out + "/Chart")
            #expect(!FileManager.default.fileExists(atPath: out + "/._Chart"))
            #expect(appledouble_has_native(out + "/Chart"))
            // Read as the host keeps it: its own forks, the same.
            var hostSide = meta
            hostSide.comment = [] // the host has no place for the Finder's comment
            #expect(Self.sameClassicSide(ClassicMeta.read(out + "/Chart"), hostSide))
            // Brought back in: a sidecar again, the file's own forks gone.
            let back = try p.ops.copy(out + "/Chart", into: p.disk)
            #expect(FileManager.default.fileExists(atPath: p.disk + "/._Chart"))
            #expect(!appledouble_has_native(back))
            #expect(ClassicMeta.read(back)?.resourceFork == meta.resourceFork)
            // A folder goes out with each file's forks its own.
            let folder = p.library + "/Set"
            try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true)
            _ = try Self.makeClassic(folder + "/One")
            try p.ops.exportForHost(folder, to: out + "/Set")
            #expect(appledouble_has_native(out + "/Set/One") && !FileManager.default.fileExists(atPath: out + "/Set/._One"))
            let backFolder = try p.ops.move(out + "/Set", into: p.disk)
            #expect(FileManager.default.fileExists(atPath: backFolder + "/._One"))
        }
        #endif

        @Test func macBinaryRoundTrips() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let src = p.library + "/Report"
            let meta = try Self.makeClassic(src, data: String(repeating: "x", count: 300))
            let bin = try p.ops.exportMacBinary(src, into: p.root)
            #expect(bin == p.root + "/Report.bin")
            let bytes = Array(try Data(contentsOf: URL(fileURLWithPath: bin)))
            #expect(bytes.count % 128 == 0 && Array(bytes[102..<106]) == Array("mBIN".utf8))
            var mf = mac_file()
            var err = [CChar](repeating: 0, count: 256)
            #expect(bytes.withUnsafeBufferPointer { macbinary_parse($0.baseAddress, $0.count, &mf, &err, err.count) })
            defer { mac_file_free(&mf) }
            #expect(mf.data_len == 300 && mf.rsrc_len == meta.resourceFork.count)
            #expect(mac_file_type(&mf) == fourCC("TEXT") && mac_file_creator(&mf) == fourCC("TEST"))
            #expect(withUnsafeBytes(of: mf.name) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) } == "Report")
            // The thumbnail reads a MacBinary file whole.
            let contents = try #require(ClassicFileContents(path: bin))
            #expect(contents.isMacBinary && contents.name == "Report" && contents.data().count == 300)
        }

        /// FinderIcons' application, its bundle also giving TEXT documents
        /// (local ID 1) ICN# 129: black, whole.
        static func bundleWithDocumentIcon() -> [UInt8] {
            let bndl = cat(Array("TEST".utf8), be16(0), be16(1),
                           Array("ICN#".utf8), be16(1), be16(0), be16(128), be16(1), be16(129),
                           Array("FREF".utf8), be16(1), be16(0), be16(128), be16(1), be16(129))
            var icon = [UInt8](repeating: 0, count: 128)
            for i in 0..<128 { icon[i] = 0xFF }
            var half = [UInt8](repeating: 0, count: 128)
            for i in 0..<64 { half[i] = 0xFF }
            return buildFork([
                TestResource(type: "BNDL", id: 128, data: bndl),
                TestResource(type: "FREF", id: 128, data: cat(Array("APPL".utf8), be16(0), [0])),
                TestResource(type: "FREF", id: 129, data: cat(Array("TEXT".utf8), be16(1), [0])),
                TestResource(type: "ICN#", id: 128, data: icon + half),
                TestResource(type: "icl8", id: 128, data: [UInt8](repeating: 5, count: 1024)),
                TestResource(type: "ICN#", id: 129, data: icon + icon),
            ])
        }

        @Test func iconsComeFromBundles() throws {
            let p = try Place()
            defer { p.cleanUp() }
            // An application whose bundle gives TEXT documents an icon.
            let app = p.library + "/Games/Tester"
            try FileManager.default.createDirectory(atPath: p.library + "/Games", withIntermediateDirectories: true)
            _ = try Self.makeClassic(app, type: "APPL", creator: "TEST", fork: Self.bundleWithDocumentIcon())
            let doc = p.disk + "/Doc"
            _ = try Self.makeClassic(doc, type: "TEXT", creator: "TEST", fork: [])
            let index = ClassicApplicationIndex(roots: [p.library, p.disk])
            #expect(index.applications(creator: fourCC("TEST")) == [app])
            let lookup = ClassicIconLookup(applications: index)
            let appIcons = lookup.icons(forFileAt: app)
            #expect(appIcons.first?.size == 32)
            let docIcons = lookup.icons(forFileAt: doc)
            #expect(docIcons.first?.size == 32)
            // FREF 129 (TEXT): ICN# 129, black.
            #expect(docIcons.first.map { Array($0.rgba[0..<4]) } == [0, 0, 0, 255])
            #expect(docIcons.first?.rgba != appIcons.first?.rgba)
            // A document of an unknown creator, and a host file, have none.
            let orphan = p.disk + "/Orphan"
            _ = try Self.makeClassic(orphan, type: "TEXT", creator: "NONE", fork: [])
            #expect(lookup.icons(forFileAt: orphan).isEmpty)
            try Data("plain".utf8).write(to: URL(fileURLWithPath: p.disk + "/plain.txt"))
            #expect(lookup.icons(forFileAt: p.disk + "/plain.txt").isEmpty)
            // A custom icon wins.
            let custom = p.disk + "/Custom"
            var mask = [UInt8](repeating: 0xFF, count: 256)
            for i in 0..<128 { mask[i] = 0 } // white everywhere
            var meta = try Self.makeClassic(custom, type: "TEXT", creator: "TEST",
                                            fork: buildFork([TestResource(type: "ICN#", id: -16455, data: mask)]))
            meta.flags |= FinderFlags.hasCustomIcon
            try meta.write(sidecarOf: custom)
            #expect(lookup.icons(forFileAt: custom).first.map { Array($0.rgba[0..<4]) } == [255, 255, 255, 255])

            #if canImport(CoreGraphics)
            // The thumbnail: the icon drawn crisply, a whole multiple of its size.
            let side = 128
            let ctx = try #require(CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: side * 4,
                                             space: CGColorSpaceCreateDeviceRGB(),
                                             bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
            drawIcon(appIcons, in: ctx, size: CGSize(width: side, height: side))
            let px = try #require(ctx.data).assumingMemoryBound(to: UInt8.self)
            let (r, g, b) = macPalette8(5)
            // Top-left of the icon (its top half is masked in): the icl8 colour.
            #expect([px[0], px[1], px[2], px[3]] == [r, g, b, 255])
            // Its lower half is masked out: clear.
            #expect(px[(side * 3 / 4) * side * 4 + 3] == 0)
            #endif
        }

        @Test func previewPageShowsTextAndResources() throws {
            let p = try Place()
            defer { p.cleanUp() }
            let app = p.library + "/Tester"
            _ = try Self.makeClassic(app, type: "APPL", creator: "TEST", fork: Self.bundleWithDocumentIcon())
            let doc = p.disk + "/Notes"
            _ = try Self.makeClassic(doc, type: "TEXT", creator: "TEST", data: "Line one\rLine <two> & caf")
            try Data(Array("Line one\rLine <two> & caf".utf8) + [0x8E]).write(to: URL(fileURLWithPath: doc)) // MacRoman é
            let lookup = ClassicIconLookup(applications: ClassicApplicationIndex(roots: [p.library]))
            let page = try #require(ClassicPreviewPage(path: doc, lookup: lookup, decode: false))
            #expect(page.html.contains("<h1>Notes</h1>"))
            #expect(page.html.contains("'TEXT'") && page.html.contains("'TEST'"))
            #expect(page.html.contains("Line one\nLine &lt;two&gt; &amp; café"))  // MacRoman, CR as lines
            #expect(page.html.contains("<code>STR </code>") && page.html.contains("&quot;abc&quot;"))
            #expect(page.html.contains("In Progress") && page.html.contains("A comment"))
            #expect(page.attachments["icon.png"]?.contentType == "public.png")
            #expect(page.attachments["icon.png"].map { Array($0.data.prefix(4)) } == [0x89, 0x50, 0x4E, 0x47])
            let appPage = try #require(ClassicPreviewPage(path: app, lookup: lookup, decode: false))
            #expect(appPage.html.contains("Classic application program") && appPage.html.contains("<code>BNDL</code>"))
            // A host file is not one to preview.
            try Data("x".utf8).write(to: URL(fileURLWithPath: p.disk + "/host.txt"))
            #expect(ClassicPreviewPage(path: p.disk + "/host.txt", lookup: lookup) == nil)
        }

        @Test func previewPageDecodesPicturesAndSounds() throws {
            let p = try Place()
            defer { p.cleanUp() }
            // A version 1 PICT: a 4x4 frame, a black rectangle painted.
            let pict = cat(be16(0), be16(0), be16(0), be16(4), be16(4),
                           [0x11, 0x01],                                   // version 1
                           [0x31], be16(0), be16(0), be16(4), be16(4),     // paintRect
                           [0xFF])
            // A format 1 'snd ': one sampled sound command, 8-bit, 11 kHz.
            let samples = [UInt8](repeating: 0x80, count: 64)
            let header = cat(be32(0), be32(UInt32(samples.count)), be32(0x2B77_45D1), be32(0), be32(0), [0, 0x3C])
            let snd = cat(be16(1), be16(1), be16(5), be32(0x80), be16(1), be16(0x8051), be16(0), be32(20), header, samples)
            let doc = p.disk + "/Media"
            _ = try Self.makeClassic(doc, type: "rsrc", creator: "RSED",
                                     fork: buildFork([TestResource(type: "PICT", id: 128, data: pict),
                                                      TestResource(type: "snd ", id: 128, data: snd)]))
            let lookup = ClassicIconLookup(applications: ClassicApplicationIndex(roots: []))
            let page = try #require(ClassicPreviewPage(path: doc, lookup: lookup))
            #expect(page.pictures == 1 && page.attachments["pict0.png"] != nil)
            #expect(page.sounds == 1 && page.attachments["snd0.wav"].map { Array($0.data.prefix(4)) } == Array("RIFF".utf8))
            #expect(page.html.contains("cid:pict0.png") && page.html.contains("cid:snd0.wav"))
        }
    }
}

@Suite("Browser type-select") struct BrowserTypeSelectTests {
    @Test func matchesAsTheFinderDoes() {
        let names = ["Zork", "apple", "Dropbox", "drawings", "File 10", "File 9", "Émile"]
        #expect(TypeSelect.match("d", in: names) == 3) // "drawings" sorts before "Dropbox"
        #expect(TypeSelect.match("dro", in: names) == 2)
        #expect(TypeSelect.match("APP", in: names) == 1) // case doesn't matter
        #expect(TypeSelect.match("emi", in: names) == 6) // nor accents
        #expect(TypeSelect.match("file 1", in: names) == 4)
        #expect(TypeSelect.match("c", in: names) == 3) // none starts so: the next one
        #expect(TypeSelect.match("zz", in: names) == 0) // past them all: the last
        #expect(TypeSelect.match("", in: names) == nil)
        #expect(TypeSelect.match("a", in: []) == nil)
    }

    @Test func typingBuildsANameUntilAPause() {
        var t = TypeSelect(timeout: 1)
        #expect(t.feed("d", at: 10) == "d")
        #expect(t.feed("r", at: 10.5) == "dr")
        #expect(t.feed(" ", at: 10.8) == "dr ") // a space inside a name counts
        #expect(t.feed("x", at: 12) == "x") // after a pause it starts again
        #expect(t.feed(" ", at: 20) == nil) // but doesn't start one
        #expect(t.feed("\u{F700}", at: 20.1) == nil) // arrow keys aren't letters
        #expect(t.feed("\t", at: 20.2) == nil)
        t.reset()
        #expect(t.feed("q", at: 20.3) == "q")
    }
}
