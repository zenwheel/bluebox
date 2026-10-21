import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Icon Services (IconServicesLib): IconRefs for the system's icons,
    /// applications' bundles, files and folders; 'icns' families; drawing
    /// and hit testing.
    @Suite struct IconServices {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func handle(_ b: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(b.count), false, false)
            for (i, x) in b.enumerated() { wr8(rd32(h) + UInt32(i), x) }
            return h
        }

        func bytes(_ h: UInt32) -> [UInt8] {
            (0..<Int(tb_GetHandleSize(h))).map { rd8(rd32(h) + UInt32($0)) }
        }

        func os(_ s: String) -> Int32 { Int32(bitPattern: fourCC(s)) }

        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        /// An 'icns' family of the given elements.
        func family(_ elements: [(String, [UInt8])]) -> [UInt8] {
            var body = [UInt8]()
            for (t, d) in elements { body += cat(Array(t.utf8), be32(UInt32(d.count + 8)), d) }
            return cat(Array("icns".utf8), be32(UInt32(body.count + 8)), body)
        }

        /// An ICN#: black where `on` (columns below `width`), masked the same.
        func icnPound(width: Int = 32) -> [UInt8] {
            var plane = [UInt8]()
            for _ in 0..<32 {
                for byte in 0..<4 {
                    var b: UInt8 = 0
                    for bit in 0..<8 where byte * 8 + bit < width { b |= 0x80 >> UInt8(bit) }
                    plane.append(b)
                }
            }
            return plane + plane
        }

        /// A 32-bit GWorld, current, filled white.
        func gworld(_ w: Int = 64, _ h: Int = 64) {
            _ = initToolbox()
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, h, w), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            _ = tb_LockPixels(tb_GetGWorldPixMap(rd32(gw)))
            tb_EraseRect(rect(0, 0, h, w))
        }

        func pixel(_ h: Int, _ v: Int) -> (UInt16, UInt16, UInt16) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return (rd16(c), rd16(c + 2), rd16(c + 4))
        }

        func ref(_ out: UInt32) -> UInt32 { rd32(out) }

        func owners(_ r: UInt32) -> UInt16 {
            let o = tb_NewPtr(2, false, true)
            #expect(tb_GetIconRefOwners(r, o) == 0)
            return rd16(o)
        }

        /// The ICN# an IconRef's family holds.
        func icn(_ r: UInt32) -> [UInt8] {
            let fam = tb_NewPtr(4, false, true)
            #expect(tb_IconRefToIconFamily(r, 0xFFFF_FFFF, fam) == 0)
            let h = tb_NewHandle(0, false, false)
            #expect(tb_GetIconFamilyData(rd32(fam), os("ICN#"), h) == 0)
            return bytes(h)
        }

        @Test func gestaltAndTheLibrary() {
            launchTestApp()
            var resp: UInt32 = 0
            #expect(tb_Gestalt(os("icon"), &resp) == 0 && resp & 0x10 != 0)
            #expect(cfm_export_fn("GetIconRef") != nil && cfm_export_fn("PlotIconRef") != nil)
        }

        /// The system's icons are shared: one IconRef per type, counted.
        @Test func systemIconsAreSharedAndCounted() {
            launchTestApp()
            let a = tb_NewPtr(4, false, true), b = tb_NewPtr(4, false, true)
            #expect(tb_GetIconRef(0, os("macs"), os("fldr"), a) == 0)
            #expect(tb_GetIconRef(0, os("macs"), os("fldr"), b) == 0)
            #expect(ref(a) != 0 && ref(a) == ref(b))
            #expect(tb_IsValidIconRef(ref(a)) != 0)
            #expect(owners(ref(a)) == 2)
            #expect(tb_AcquireIconRef(ref(a)) == 0 && owners(ref(a)) == 3)
            #expect(tb_ReleaseIconRef(ref(a)) == 0 && tb_ReleaseIconRef(ref(a)) == 0 && owners(ref(a)) == 1)
            #expect(icn(ref(a)).count == 256)
            // Unknown types get the generic document; unknown refs are refused.
            #expect(tb_GetIconRef(0, os("macs"), os("zzzz"), b) == 0)
            let doc = tb_NewPtr(4, false, true)
            #expect(tb_GetIconRef(0, os("macs"), os("docu"), doc) == 0 && ref(b) == ref(doc))
            #expect(tb_ReleaseIconRef(0x1234) == -2580)
            #expect(tb_IsValidIconRef(0x1234) == 0)
        }

        @Test func familiesHoldTheirData() {
            launchTestApp()
            let icon = icnPound(width: 16)
            let fam = handle(family([("ICN#", icon)]))
            let h = tb_NewHandle(0, false, false)
            #expect(tb_GetIconFamilyData(fam, os("ICN#"), h) == 0 && bytes(h) == icon)
            #expect(tb_GetIconFamilyData(fam, os("il32"), h) == 0 && tb_GetHandleSize(h) == 0)
            // Adding an element, then replacing it.
            #expect(tb_SetIconFamilyData(fam, os("ics#"), handle([UInt8](repeating: 0xFF, count: 64))) == 0)
            #expect(tb_SetIconFamilyData(fam, os("ics#"), handle([UInt8](repeating: 0x0F, count: 64))) == 0)
            #expect(tb_GetIconFamilyData(fam, os("ics#"), h) == 0 && bytes(h) == [UInt8](repeating: 0x0F, count: 64))
            #expect(tb_GetHandleSize(fam) == 8 + 264 + 72 && rd32(rd32(fam) + 4) == 8 + 264 + 72)
            // To a suite and back.
            let suite = tb_NewPtr(4, false, true), icnp = tb_NewPtr(4, false, true)
            #expect(tb_IconFamilyToIconSuite(fam, 0xFFFF_FFFF, suite) == 0)
            #expect(tb_GetIconFromSuite(icnp, rd32(suite), os("ICN#")) == 0 && bytes(rd32(icnp)) == icon)
            let back = tb_NewPtr(4, false, true)
            #expect(tb_IconSuiteToIconFamily(rd32(suite), 0x1, back) == 0) // kSelectorLarge1Bit
            #expect(tb_GetIconFamilyData(rd32(back), os("ICN#"), h) == 0 && bytes(h) == icon)
            #expect(tb_GetIconFamilyData(rd32(back), os("ics#"), h) == 0 && tb_GetHandleSize(h) == 0)
        }

        @Test func registeredIconsAreFoundAndDrawn() {
            launchTestApp()
            gworld()
            let fam = handle(family([("ICN#", icnPound(width: 16))]))
            let r = tb_NewPtr(4, false, true), again = tb_NewPtr(4, false, true)
            #expect(tb_RegisterIconRefFromIconFamily(os("Tst1"), os("TYP1"), fam, r) == 0)
            #expect(tb_GetIconRef(0, os("Tst1"), os("TYP1"), again) == 0 && ref(again) == ref(r))
            #expect(tb_IsDataAvailableInIconRef(os("ICN#"), ref(r)) != 0)
            #expect(tb_IsDataAvailableInIconRef(os("il32"), ref(r)) == 0)
            let sizes = tb_NewPtr(4, false, true)
            #expect(tb_GetIconSizesFromIconRef(0xFFFF_FFFF, sizes, 0, ref(r)) == 0 && rd32(sizes) == 0x1)
            // Black on the left half only.
            let box = rect(10, 10, 42, 42)
            #expect(tb_PlotIconRef(box, 0, 0, 0, ref(r)) == 0)
            #expect(pixel(12, 20) == (0, 0, 0))
            #expect(pixel(35, 20) == (0xFFFF, 0xFFFF, 0xFFFF))
            // Hit testing follows the mask.
            let pt = tb_NewPtr(4, false, true)
            wr16(pt, 20); wr16(pt + 2, 12)
            #expect(tb_PtInIconRef(pt, box, 0, 0, ref(r)) != 0)
            wr16(pt + 2, 35)
            #expect(tb_PtInIconRef(pt, box, 0, 0, ref(r)) == 0)
            #expect(tb_RectInIconRef(rect(0, 30, 60, 60), box, 0, 0, ref(r)) == 0)
            #expect(tb_RectInIconRef(rect(0, 20, 60, 60), box, 0, 0, ref(r)) != 0)
            let rgn = tb_NewRgn()
            #expect(tb_IconRefToRgn(rgn, box, 0, 0, ref(r)) == 0)
            let bbox = rd32(rgn) + 2
            #expect([rd16(bbox), rd16(bbox + 2), rd16(bbox + 4), rd16(bbox + 6)] == [10, 10, 42, 26])
            #expect(tb_IsIconRefMaskEmpty(ref(r)) == 0)
            // Unregistered, it is still valid while owned.
            #expect(tb_UnregisterIconRef(os("Tst1"), os("TYP1")) == 0)
            #expect(tb_IsValidIconRef(ref(r)) != 0)
            #expect(tb_ReleaseIconRef(ref(r)) == 0 && tb_ReleaseIconRef(ref(r)) == 0)
            #expect(tb_IsValidIconRef(ref(r)) == 0)
        }

        /// 32-bit icons: run-length coded red, green and blue planes and an
        /// 8-bit mask.
        @Test func thirtyTwoBitIconsAreDecoded() {
            launchTestApp()
            gworld()
            func plane(_ v: UInt8) -> [UInt8] {
                // 1024 bytes: seven runs of 130 and one of 114.
                var out = [UInt8]()
                for _ in 0..<7 { out += [0xFF, v] }
                out += [0x80 + 114 - 3, v]
                return out
            }
            var mask = [UInt8](repeating: 0, count: 1024)
            for y in 0..<32 { for x in 0..<8 { mask[y * 32 + x] = 0xFF } }
            let fam = handle(family([("il32", plane(0xFF) + plane(0) + plane(0)), ("l8mk", mask)]))
            let r = tb_NewPtr(4, false, true)
            #expect(tb_RegisterIconRefFromIconFamily(os("Tst2"), os("TYP2"), fam, r) == 0)
            #expect(tb_PlotIconRef(rect(0, 0, 32, 32), 0, 0, 0, ref(r)) == 0)
            #expect(pixel(3, 3) == (0xFFFF, 0, 0))
            #expect(pixel(20, 3) == (0xFFFF, 0xFFFF, 0xFFFF))
        }

        @Test func overridesAndComposites() {
            launchTestApp()
            let a = tb_NewPtr(4, false, true), b = tb_NewPtr(4, false, true)
            #expect(tb_RegisterIconRefFromIconFamily(os("Tst3"), os("AAAA"), handle(family([("ICN#", icnPound(width: 8))])), a) == 0)
            #expect(tb_RegisterIconRefFromIconFamily(os("Tst3"), os("BBBB"), handle(family([("ICN#", icnPound(width: 24))])), b) == 0)
            #expect(tb_OverrideIconRef(ref(a), ref(b)) == 0)
            #expect(icn(ref(a)) == icnPound(width: 24))
            #expect(tb_RemoveIconRefOverride(ref(a)) == 0)
            #expect(icn(ref(a)) == icnPound(width: 8))
            let c = tb_NewPtr(4, false, true), bg = tb_NewPtr(4, false, true), fg = tb_NewPtr(4, false, true)
            #expect(tb_CompositeIconRef(ref(a), ref(b), c) == 0)
            #expect(tb_IsIconRefComposite(ref(c), bg, fg) == 0 && rd32(bg) == ref(a) && rd32(fg) == ref(b))
            gworld()
            #expect(tb_PlotIconRef(rect(0, 0, 32, 32), 0, 0, 0, ref(c)) == 0)
            #expect(pixel(20, 5) == (0, 0, 0)) // the foreground's, over the background
        }

        /// Files: an application's icon from its bundle, a document's from
        /// its application's, a custom icon, a folder.
        @Test func filesAndFolders() {
            launchTestApp()
            let spec = tb_NewPtr(70, false, true)
            #expect(tb_FSMakeFSSpec(0, 0, pname("Iconic"), spec) == -43)
            tb_FSpCreateResFile(spec, os("Icnc"), os("APPL"), 0)
            let rn = tb_FSpOpenResFile(spec, 3)
            #expect(rn > 0)
            // BNDL: owner Icnc; ICN# local 0 -> 128, local 1 -> 129; FREFs.
            let bndl = cat(Array("Icnc".utf8), be16(0), be16(1),
                           Array("ICN#".utf8), be16(1), be16(0), be16(128), be16(1), be16(129),
                           Array("FREF".utf8), be16(1), be16(0), be16(128), be16(1), be16(129))
            tb_AddResource(handle(bndl), os("BNDL"), 128, 0)
            tb_AddResource(handle(cat(Array("APPL".utf8), be16(0), [0])), os("FREF"), 128, 0)
            tb_AddResource(handle(cat(Array("Doc ".utf8), be16(1), [0])), os("FREF"), 129, 0)
            tb_AddResource(handle(icnPound(width: 4)), os("ICN#"), 128, 0)
            tb_AddResource(handle(icnPound(width: 12)), os("ICN#"), 129, 0)
            tb_CloseResFile(rn)
            let r = tb_NewPtr(4, false, true), label = tb_NewPtr(2, false, true)
            #expect(tb_GetIconRefFromFile(spec, r, label) == 0 && icn(ref(r)) == icnPound(width: 4))
            let same = tb_NewPtr(4, false, true)
            #expect(tb_GetIconRef(0, os("Icnc"), os("APPL"), same) == 0 && ref(same) == ref(r))
            // Its documents, through the desktop database.
            let dtp = tb_NewPtr(128, false, true)
            _ = tb_PBDTAddAPPL(dtp, false)
            let doc = tb_NewPtr(4, false, true)
            #expect(tb_GetIconRef(0, os("Icnc"), os("Doc "), doc) == 0 && icn(ref(doc)) == icnPound(width: 12))
            // A custom icon.
            let cspec = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Custom"), cspec)
            tb_FSpCreateResFile(cspec, os("Icnc"), os("Doc "), 0)
            let crn = tb_FSpOpenResFile(cspec, 3)
            tb_AddResource(handle(icnPound(width: 20)), os("ICN#"), -16455, 0)
            tb_CloseResFile(crn)
            let info = tb_NewPtr(16, false, true)
            #expect(tb_FSpGetFInfo(cspec, info) == 0)
            wr16(info + 8, rd16(info + 8) | 0x0400 | 0x0004) // kHasCustomIcon, label 2
            #expect(tb_FSpSetFInfo(cspec, info) == 0)
            #expect(tb_GetIconRefFromFile(cspec, r, label) == 0 && icn(ref(r)) == icnPound(width: 20))
            #expect(rd16(label) == 2)
            let enabled = tb_NewPtr(1, false, true)
            #expect(tb_SetCustomIconsEnabled(0, 0) == 0 && tb_GetCustomIconsEnabled(0, enabled) == 0 && rd8(enabled) == 0)
            #expect(tb_GetIconRefFromFile(cspec, r, label) == 0 && icn(ref(r)) == icnPound(width: 12))
            #expect(tb_SetCustomIconsEnabled(0, 1) == 0)
            // A folder, and the volume.
            let fspec = tb_NewPtr(70, false, true), dirID = tb_NewPtr(4, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Folder"), fspec)
            #expect(tb_FSpDirCreate(fspec, 0, dirID) == 0)
            let folder = tb_NewPtr(4, false, true), generic = tb_NewPtr(4, false, true)
            #expect(tb_GetIconRef(0, os("macs"), os("fldr"), generic) == 0)
            #expect(tb_GetIconRefFromFile(fspec, folder, label) == 0 && ref(folder) == ref(generic))
            #expect(tb_GetIconRefFromFolder(0, 0, Int32(bitPattern: rd32(dirID)), 0, 0, folder) == 0 && ref(folder) == ref(generic))
            // A volume's FSSpec: its name in the root's parent.
            let vspec = tb_NewPtr(70, false, true), disk = tb_NewPtr(4, false, true)
            wr16(vspec, rd16(spec))
            wr32(vspec + 2, 1)
            guestPString("Macintosh HD", at: vspec + 6)
            #expect(tb_GetIconRef(0, os("macs"), os("hdsk"), generic) == 0)
            #expect(tb_GetIconRefFromFile(vspec, disk, label) == 0 && ref(disk) == ref(generic))
        }

        /// An 'icns' file written and read back, and registered from.
        @Test func iconFiles() {
            launchTestApp()
            let data = family([("ICN#", icnPound(width: 28))])
            let spec = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Icons.icns"), spec)
            #expect(tb_WriteIconFile(handle(data), spec) == 0)
            let fam = tb_NewPtr(4, false, true)
            #expect(tb_ReadIconFile(spec, fam) == 0 && bytes(rd32(fam)) == data)
            let r = tb_NewPtr(4, false, true)
            #expect(tb_RegisterIconRefFromIconFile(os("Tst4"), os("FILE"), spec, r) == 0)
            #expect(icn(ref(r)) == icnPound(width: 28))
        }
    }
}
