import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// FontSync: references to the machine's fonts, matched, flattened and kept in profiles.
    @Suite struct CarbonFontSync {
        func font(_ name: String) -> UInt32 {
            let family = svcCall("FMGetFontFamilyFromName", svcPStr(name))
            #expect(family != 0xFFFF && family != 0xFFFF_FFFF)
            let out = svcPtr(8)
            #expect(svcCall("FMGetFontFromFontFamilyInstance", family, 0, out, out + 4) == 0)
            return rd32(out)
        }

        func reference(_ font: UInt32) -> UInt32 {
            let out = svcPtr(4)
            #expect(svcCall("FNSReferenceCreate", font, 0, out) == 0 && rd32(out) != 0)
            return rd32(out)
        }

        @Test func referencesCompareAndFlatten() {
            bootMachine()
            #expect(svcCall("FNSEnabled") == 1 && svcCall("FNSMatchDefaultsGet") == 0x3FF)
            let info = svcPtr(28)
            wr32(info, 1)
            svcCall("FNSSysInfoGet", info)
            #expect(rd32(info + 8) == 1 && rd16(info + 24) != 0)
            let geneva = reference(font("Geneva")), geneva2 = reference(font("Geneva")), chicago = reference(font("Chicago"))
            let failed = svcPtr(4)
            #expect(svcCall("FNSReferenceMatch", geneva, geneva2, 0x3FF, failed) == 0 && rd32(failed) == 0)
            // Another font differs by its names at least
            #expect(svcStatus(svcCall("FNSReferenceMatch", geneva, chicago, 0x3FF, failed)) == -29585)
            #expect(rd32(failed) & 1 == 1)
            let version = svcPtr(4)
            #expect(svcCall("FNSReferenceGetVersion", geneva, version) == 0 && rd32(version) == 1)
            // Family information
            let name = svcPtr(256), script = svcPtr(2), style = svcPtr(2)
            #expect(svcCall("FNSReferenceGetFamilyInfo", geneva, name, script, style) == 0)
            #expect(String(decoding: svcBytes(name + 1, Int(rd8(name))), as: UTF8.self) == "Geneva")
            // Flatten, and make another from it
            let size = svcPtr(4)
            #expect(svcCall("FNSReferenceFlattenedSize", geneva, size) == 0 && rd32(size) > 256)
            let flat = svcPtr(Int(rd32(size))), room = svcPtr(4)
            wr32(room, rd32(size))
            #expect(svcCall("FNSReferenceFlatten", geneva, flat, room) == 0)
            wr32(room, 10)
            #expect(svcStatus(svcCall("FNSReferenceFlatten", geneva, flat, room)) == -29587)
            let back = svcPtr(4)
            #expect(svcCall("FNSReferenceUnflatten", flat, rd32(size), back) == 0)
            #expect(svcCall("FNSReferenceMatch", geneva, rd32(back), 0x3FF, failed) == 0)
            #expect(svcStatus(svcCall("FNSReferenceUnflatten", flat, 12, back)) == -29587)
            wr8(flat, 0)
            #expect(svcStatus(svcCall("FNSReferenceUnflatten", flat, rd32(size), back)) == -29580)
            // The installed fonts that match
            let fonts = svcPtr(64 * 4), n = svcPtr(4)
            #expect(svcCall("FNSReferenceMatchFonts", geneva, 0x3FF, 64, fonts, n) == 0 && rd32(n) >= 1)
            #expect(svcCall("FNSReferenceMatchFamilies", geneva, 0x3FF, 64, fonts, n) == 0 && rd32(n) >= 1)
            // Disposed references are not references
            #expect(svcCall("FNSReferenceDispose", chicago) == 0)
            #expect(svcStatus(svcCall("FNSReferenceDispose", chicago)) == -29580)
            #expect(svcStatus(svcCall("FNSReferenceMatch", geneva, chicago, 0, failed)) == -29580)
            #expect(svcStatus(svcCall("FNSReferenceCreate", 0xDEAD, 0, svcPtr(4))) == -29580)
            #expect(svcStatus(svcCall("FNSReferenceCreate", font("Geneva"), 9, svcPtr(4))) == -29581)
        }

        @Test func namesOfAReference() {
            bootMachine()
            let ref = reference(font("Geneva"))
            let count = svcPtr(4)
            #expect(svcCall("FNSReferenceCountNames", ref, count) == 0)
            if rd32(count) > 0 {
                let name = svcPtr(128), len = svcPtr(4), code = svcPtr(4), plat = svcPtr(4), scr = svcPtr(4), lang = svcPtr(4)
                #expect(svcCall("FNSReferenceGetIndName", ref, 1, 128, name, len, code, plat, scr, lang) == 0)
                let idx = svcPtr(4)
                #expect(svcCall("FNSReferenceFindName", ref, rd32(code), rd32(plat), rd32(scr), rd32(lang), 128, name, len, idx) == 0 && rd32(idx) >= 1)
            }
            #expect(svcStatus(svcCall("FNSReferenceGetIndName", ref, 999, 8, svcPtr(8), svcPtr(4), 0, 0, 0, 0)) == -29589)
            #expect(svcStatus(svcCall("FNSReferenceFindName", ref, 77, 77, 77, 77, 8, svcPtr(8), svcPtr(4), svcPtr(4))) == -29589)
        }

        @Test func profilesKeepReferencesInAFile() {
            launchTestApp()
            let geneva = reference(font("Geneva")), chicago = reference(font("Chicago"))
            let spec = svcPtr(70), profile = svcPtr(4), count = svcPtr(4)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr("Fonts.fnsp"), spec)
            #expect(svcCall("FNSProfileCreate", spec, 0, 4, 1, profile) == 0)
            #expect(svcStatus(svcCall("FNSProfileCreate", spec, 0, 4, 1, svcPtr(4))) == -48)
            let p = rd32(profile)
            #expect(svcCall("FNSProfileAddReference", p, geneva) == 0 && svcCall("FNSProfileAddReference", p, chicago) == 0)
            #expect(svcStatus(svcCall("FNSProfileAddReference", p, geneva)) == -29584) // already there
            #expect(svcCall("FNSProfileCountReferences", p, count) == 0 && rd32(count) == 2)
            let version = svcPtr(4)
            #expect(svcCall("FNSProfileGetVersion", p, version) == 0 && rd32(version) == 1)
            #expect(svcCall("FNSProfileCompact", p) == 0 && svcCall("FNSProfileClose", p) == 0)
            #expect(svcStatus(svcCall("FNSProfileClose", p)) == -29582)
            // Read back, read only
            #expect(svcCall("FNSProfileOpen", spec, 0, profile) == 0)
            let q = rd32(profile)
            #expect(svcCall("FNSProfileCountReferences", q, count) == 0 && rd32(count) == 2)
            let matches = svcPtr(16), n = svcPtr(4)
            #expect(svcCall("FNSProfileMatchReference", q, geneva, 0x3FF, 4, matches, n) == 0 && rd32(n) == 1 && rd32(matches) == 0)
            let got = svcPtr(4), failed = svcPtr(4)
            #expect(svcCall("FNSProfileGetIndReference", q, 1, got) == 0)
            #expect(svcCall("FNSReferenceMatch", rd32(got), chicago, 0x3FF, failed) == 0)
            #expect(svcStatus(svcCall("FNSProfileGetIndReference", q, 5, got)) == -29585)
            #expect(svcStatus(svcCall("FNSProfileRemoveIndReference", q, 0)) == -61) // not open for writing
            #expect(svcCall("FNSProfileClose", q) == 0)
            // Open for writing: remove one, clear
            #expect(svcCall("FNSProfileOpen", spec, 1, profile) == 0)
            let w = rd32(profile)
            #expect(svcCall("FNSProfileRemoveReference", w, chicago) == 0)
            #expect(svcStatus(svcCall("FNSProfileRemoveReference", w, chicago)) == -29585)
            #expect(svcCall("FNSProfileRemoveIndReference", w, 0) == 0)
            #expect(svcCall("FNSProfileCountReferences", w, count) == 0 && rd32(count) == 0)
            #expect(svcStatus(svcCall("FNSProfileMatchReference", w, geneva, 0x3FF, 4, matches, n)) == -29585)
            #expect(svcCall("FNSProfileAddReference", w, geneva) == 0 && svcCall("FNSProfileClear", w) == 0)
            #expect(svcCall("FNSProfileClose", w) == 0)
            // Not a profile
            let plain = svcPtr(70)
            _ = tb_FSMakeFSSpec(0, 0, svcPStr("Plain"), plain)
            #expect(tb_FSpCreate(plain, Int32(bitPattern: fourCC("ttxt")), Int32(bitPattern: fourCC("TEXT")), 0) == 0)
            #expect(svcStatus(svcCall("FNSProfileOpen", plain, 0, profile)) == -29582)
        }

        /// A TrueType font ('sfnt') of tables given: the directory then the tables.
        func sfnt(_ tables: [(String, [UInt8])]) -> [UInt8] {
            var out = cat(be32(0x0001_0000), be16(tables.count), be16(0), be16(0), be16(0))
            var offset = 12 + 16 * tables.count
            var body: [UInt8] = []
            for (tag, data) in tables {
                out += cat(Array(tag.utf8), be32(0), be32(UInt32(offset)), be32(UInt32(data.count)))
                body += data + [UInt8](repeating: 0, count: (4 - data.count % 4) % 4)
                offset += data.count + (4 - data.count % 4) % 4
            }
            return out + body
        }

        func nameTable(_ names: [(Int, String)]) -> [UInt8] {
            var records: [UInt8] = [], strings: [UInt8] = []
            for (id, text) in names {
                records += cat(be16(1), be16(0), be16(0), be16(id), be16(text.utf8.count), be16(strings.count))
                strings += Array(text.utf8)
            }
            return cat(be16(0), be16(names.count), be16(6 + records.count), records, strings)
        }

        @Test func trueTypeFontsAreComparedByTheirTables() {
            // Two families of outline fonts: the same names and glyph count, different metrics
            let names = nameTable([(1, "Test Sans"), (4, "Test Sans Regular")])
            let common: [(String, [UInt8])] = [("name", names), ("maxp", be32(0x0001_0000) + be16(3)), ("cmap", be16(0) + be16(0))]
            let a = sfnt(common + [("hmtx", be16(500) + be16(0) + be16(600) + be16(0))])
            let b = sfnt(common + [("hmtx", be16(500) + be16(0) + be16(650) + be16(0))])
            func fond(_ font: Int) -> [UInt8] {
                cat([UInt8](repeating: 0, count: 52), be16(0), be16(0), be16(0), be16(font)) // one entry: outline, plain, the sfnt
            }
            launchTestApp([TestResource(type: "FOND", id: 300, name: "Test Sans", data: fond(300)),
                           TestResource(type: "FOND", id: 301, name: "Test Sans Two", data: fond(301)),
                           TestResource(type: "sfnt", id: 300, data: a), TestResource(type: "sfnt", id: 301, data: b)])
            let ra = reference(font("Test Sans")), rb = reference(font("Test Sans Two")), ra2 = reference(font("Test Sans"))
            let failed = svcPtr(4)
            #expect(svcCall("FNSReferenceMatch", ra, ra2, 0x3FF, failed) == 0)
            #expect(svcStatus(svcCall("FNSReferenceMatch", ra, rb, 0x3FF, failed)) == -29585)
            // The metrics tables differ (QuickDraw's, and ATSUI's); glyphs and encodings do not
            #expect(rd32(failed) & 0x10 != 0 && rd32(failed) & 0x04 == 0 && rd32(failed) & 0x08 == 0)
            // Asking only for what is the same matches
            #expect(svcCall("FNSReferenceMatch", ra, rb, 0x04 | 0x08 | 0x02, failed) == 0)
            // The layout tables neither has: missing data matches unless it must not
            #expect(svcCall("FNSReferenceMatch", ra, rb, 0x100, failed) == 0)
            #expect(svcStatus(svcCall("FNSReferenceMatch", ra, rb, 0x100 | 0x8000_0000, failed)) == -29585)
            // The font's own names
            let count = svcPtr(4), name = svcPtr(64), len = svcPtr(4), code = svcPtr(4), plat = svcPtr(4), scr = svcPtr(4), lang = svcPtr(4)
            #expect(svcCall("FNSReferenceCountNames", ra, count) == 0 && rd32(count) == 2)
            #expect(svcCall("FNSReferenceGetIndName", ra, 2, 64, name, len, code, plat, scr, lang) == 0)
            #expect(rd32(code) == 4 && rd32(plat) == 1 && String(decoding: svcBytes(name, Int(rd32(len))), as: UTF8.self) == "Test Sans Regular")
            let idx = svcPtr(4)
            #expect(svcCall("FNSReferenceFindName", ra, 1, 1, 0, 0, 64, name, len, idx) == 0 && rd32(idx) == 1 && rd32(len) == 9)
            // Flattened and back, it still matches in everything
            let size = svcPtr(4)
            #expect(svcCall("FNSReferenceFlattenedSize", ra, size) == 0)
            let flat = svcPtr(Int(rd32(size))), room = svcPtr(4), back = svcPtr(4)
            wr32(room, rd32(size))
            #expect(svcCall("FNSReferenceFlatten", ra, flat, room) == 0 && svcCall("FNSReferenceUnflatten", flat, rd32(size), back) == 0)
            #expect(svcCall("FNSReferenceMatch", ra, rd32(back), 0x3FF, failed) == 0)
            #expect(svcCall("FNSReferenceCountNames", rd32(back), count) == 0 && rd32(count) == 2)
        }
    }
}
