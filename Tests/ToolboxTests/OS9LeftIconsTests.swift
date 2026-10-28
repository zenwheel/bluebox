import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// IconServicesLib's remaining names: icons by name, by file type and
    /// creator, from files and FSRefs; suites and families made from refs;
    /// registering and overriding.
    @Suite(.serialized) struct OS9LeftIcons {
        init() {
            _ = launchTestApp()
            cfm_reset()
            _ = cfm_ppc_cpu()
        }

        func os(_ s: String) -> UInt32 { fourCC(s) }

        func pname(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
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

        func icnPound(width: Int) -> [UInt8] {
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

        /// The ICN# of a ref, by way of its family.
        func icn(_ ref: UInt32) -> [UInt8] {
            let fam = tb_NewPtr(4, false, true), h = tb_NewHandle(0, false, false)
            #expect(tb_IconRefToIconFamily(ref, 0xFFFF_FFFF, fam) == 0)
            #expect(tb_GetIconFamilyData(rd32(fam), Int32(bitPattern: os("ICN#")), h) == 0)
            return bytes(h)
        }

        @Test func byNameAndByTypeAndCreator() {
            let a = Rest.ptr(4), b = Rest.ptr(4), c = Rest.ptr(4)
            #expect(Rest.signed("GetIconRefByName", [pname("Folder"), a]) == 0)
            #expect(Rest.signed("GetIconRefByName", [pname("generic FOLDER"), b]) == 0)
            #expect(rd32(a) != 0 && rd32(a) == rd32(b))
            #expect(tb_GetIconRef(0, Int32(bitPattern: os("macs")), Int32(bitPattern: os("fldr")), c) == 0)
            #expect(rd32(c) == rd32(a))
            let folder = rd32(a)
            #expect(Rest.signed("GetIconRefByName", [pname("No Such Icon"), a]) == -2581 && rd32(a) == 0)
            #expect(Rest.signed("GetIconRefByName", [0, a]) == -50)
            wr32(a, folder)
            // by file type and creator: the system's for 'macs'
            #expect(Rest.signed("GetIconRefFromFileTypeAndCreator", [os("fldr"), os("macs"), b]) == 0)
            #expect(rd32(b) == rd32(a))
            #expect(Rest.signed("GetIconRefFromFileTypeAndCreator", [os("fldr"), os("macs"), 0]) == -50)
        }

        @Test func flushingByName() {
            let a = Rest.ptr(4)
            #expect(Rest.signed("GetIconRefByName", [pname("Trash"), a]) == 0)
            let ref = rd32(a)
            #expect(tb_ReleaseIconRef(ref) == 0)
            #expect(Rest.signed("FlushIconRefsByName", [pname("Trash")]) == 0)
            #expect(tb_IsValidIconRef(ref) == 0)
            #expect(Rest.signed("FlushIconRefsByName", [pname("Nothing")]) == -2581)
        }

        @Test func suitesAndFamiliesFromRefs() {
            let r = Rest.ptr(4), fam = Rest.ptr(4), suite = Rest.ptr(4), got = Rest.ptr(4)
            #expect(Rest.signed("GetIconRefByName", [pname("Document"), r]) == 0)
            #expect(Rest.signed("NewIconFamilyFromIconRef", [rd32(r), 0x1, fam]) == 0) // kSelectorLarge1Bit
            let h = tb_NewHandle(0, false, false)
            #expect(tb_GetIconFamilyData(rd32(fam), Int32(bitPattern: os("ICN#")), h) == 0 && tb_GetHandleSize(h) == 256)
            #expect(Rest.signed("NewIconSuiteFromIconRef", [rd32(r), 0x1, suite]) == 0)
            #expect(tb_GetIconFromSuite(got, rd32(suite), Int32(bitPattern: os("ICN#"))) == 0)
            #expect(bytes(rd32(got)) == bytes(h))
            #expect(Rest.signed("NewIconSuiteByName", [pname("Document"), 0x1, suite]) == 0 && rd32(suite) != 0)
            #expect(Rest.signed("NewIconSuiteByName", [pname("Unknown"), 0x1, suite]) == -2581)
            #expect(Rest.signed("NewIconFamilyFromIconRef", [0x1234, 1, fam]) == -2580)
            #expect(Rest.signed("NewIconSuiteFromIconRef", [rd32(r), 1, 0]) == -50)
        }

        /// A file with a custom icon of the given width.
        func customFile(_ name: String, width: Int) -> UInt32 {
            let spec = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname(name), spec)
            tb_FSpCreateResFile(spec, Int32(bitPattern: os("Icnc")), Int32(bitPattern: os("Doc ")), 0)
            let rn = tb_FSpOpenResFile(spec, 3)
            tb_AddResource(handle(icnPound(width: width)), Int32(bitPattern: os("ICN#")), -16455, 0)
            tb_CloseResFile(rn)
            let info = tb_NewPtr(16, false, true)
            _ = tb_FSpGetFInfo(spec, info)
            wr16(info + 8, rd16(info + 8) | 0x0400)
            _ = tb_FSpSetFInfo(spec, info)
            return spec
        }

        @Test func fromFiles() {
            let spec = customFile("Pictured", width: 20)
            let suite = Rest.ptr(4), got = Rest.ptr(4)
            #expect(Rest.signed("NewIconSuiteFromFile", [spec, 0x1, suite]) == 0)
            #expect(tb_GetIconFromSuite(got, rd32(suite), Int32(bitPattern: os("ICN#"))) == 0)
            #expect(bytes(rd32(got)) == icnPound(width: 20))
            #expect(Rest.signed("NewIconSuiteFromFile", [0, 0x1, suite]) == -50)

            // registered under a creator and type
            let reg = Rest.ptr(4), found = Rest.ptr(4)
            #expect(Rest.signed("RegisterIconRefFromFile", [os("RegT"), os("Reg1"), spec, reg]) == 0)
            #expect(icn(rd32(reg)) == icnPound(width: 20))
            #expect(tb_GetIconRef(0, Int32(bitPattern: os("RegT")), Int32(bitPattern: os("Reg1")), found) == 0)
            #expect(rd32(found) == rd32(reg))

            // overriding
            let victim = Rest.ptr(4)
            #expect(Rest.signed("GetIconRefByName", [pname("Application"), victim]) == 0)
            let before = icn(rd32(victim))
            let other = customFile("Replacement", width: 8)
            #expect(Rest.signed("OverrideIconRefFromFile", [rd32(victim), other]) == 0)
            #expect(icn(rd32(victim)) == icnPound(width: 8) && before != icnPound(width: 8))
            #expect(tb_RemoveIconRefOverride(rd32(victim)) == 0)
            #expect(icn(rd32(victim)) == before)
            #expect(Rest.signed("OverrideIconRefFromFile", [0x1234, other]) == -2580)
            #expect(Rest.signed("OverrideIconRefFromFile", [rd32(victim), 0]) == -50)
        }

        @Test func registerAnExistingRef() {
            let fam = handle([UInt8]("icns".utf8) + [0, 0, 1, 0x08 + 0x10] + [UInt8]("ICN#".utf8) + [0, 0, 1, 0x08] + icnPound(width: 12))
            // an 'icns' with the one ICN#: 8 + 8 + 256 bytes
            let bytesNeeded = 8 + 8 + 256
            wr32(rd32(fam) + 4, UInt32(bytesNeeded))
            let ref = Rest.ptr(4)
            #expect(tb_RegisterIconRefFromIconFamily(Int32(bitPattern: os("Mine")), Int32(bitPattern: os("Mn01")), fam, ref) == 0)
            #expect(Rest.signed("RegisterIconRef", [os("Mine"), os("Mn02"), rd32(ref)]) == 0)
            let got = Rest.ptr(4)
            #expect(tb_GetIconRef(0, Int32(bitPattern: os("Mine")), Int32(bitPattern: os("Mn02")), got) == 0)
            #expect(rd32(got) != 0 && icn(rd32(got)) == icnPound(width: 12))
            #expect(Rest.signed("RegisterIconRef", [os("Mine"), os("Mn03"), 0x1234]) == -2580)
        }

        @Test func fromFolderInfoAndFileInfo() {
            let folder = Rest.ptr(4), generic = Rest.ptr(4)
            #expect(tb_GetIconRef(0, Int32(bitPattern: os("macs")), Int32(bitPattern: os("fldr")), generic) == 0)
            let plain = Rest.ptr(4)
            #expect(Rest.signed("GetIconRefFromFolderInfo", [0, 0, 2, 0, 0, 0, folder]) == 0)
            #expect(tb_GetIconRefFromFolder(0, 0, 2, 0, 0, plain) == 0)
            #expect(rd32(folder) != 0 && rd32(folder) == rd32(plain))
            // by catalog info alone: a folder, then a type and creator
            let info = Rest.ptr(144), out = Rest.ptr(4), label = Rest.ptr(2)
            wr16(info, 0x10)
            #expect(Rest.signed("GetIconRefFromFileInfo", [0, 0, 0, 0x800, info, 0, out, label]) == 0)
            #expect(rd32(out) == rd32(generic))
            wr16(info, 0)
            wr32(info + 72, os("APPL")); wr32(info + 76, os("macs"))
            #expect(Rest.signed("GetIconRefFromFileInfo", [0, 0, 0, 0x800, info, 0, out, label]) == 0)
            #expect(rd32(out) != 0)
            #expect(Rest.signed("GetIconRefFromFileInfo", [0, 0, 0, 0, info, 0, out, label]) == -50)
            #expect(Rest.signed("GetIconRefFromFileInfo", [0, 0, 0, 0x800, info, 0, 0, label]) == -50)
        }
    }
}
