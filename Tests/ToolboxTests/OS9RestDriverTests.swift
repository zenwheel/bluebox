import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox


extension BlueBox {
    /// DriverServicesLib's interrupt source trees and I/O commands, and the
    /// Driver Loader's descriptions and candidate comparison.
    @Suite struct OS9RestDriver {
        init() { Rest.boot() }

        // MARK: Driver Services

        @Test func ioCommands() {
            let cmd = Rest.ptr(16)
            wr16(cmd + 6, 0x0402) // async kind (4 here), Read
            let contents = Rest.ptr(4), code = Rest.ptr(4), kind = Rest.ptr(4)
            #expect(Rest.signed("GetIOCommandInfo", [cmd, contents, code, kind]) == 0)
            #expect(rd32(contents) == cmd && rd32(code) == 2 && rd32(kind) == 4)
            #expect(Rest.signed("GetIOCommandInfo", [0, contents, code, kind]) == -50)
            #expect(Rest.signed("IOCommandIsComplete", [cmd, UInt32(bitPattern: -36)]) == 0)
            #expect(rd16(cmd + 8) == UInt16(bitPattern: -36) && rd16(cmd + 10) == 1)
            #expect(Rest.signed("IOCommandIsComplete", [0, 0]) == -50)
            #expect(Rest.signed("DSL_Init") == 0)
        }

        func newSet(parent: UInt32 = 0, member: UInt32 = 0, size: UInt32 = 4) -> UInt32 {
            let id = Rest.ptr(4)
            #expect(Rest.signed("CreateInterruptSet", [parent, member, size, id, 0]) == 0)
            return rd32(id)
        }

        @Test func sourceTrees() {
            let root = newSet()
            let child = newSet(parent: root, member: 2)
            let vec = Rest.ptr(4)
            #expect(Rest.signed("GetInterruptSourceTreeVector", [root, vec]) == -50) // not a tree yet
            #expect(Rest.signed("CreateInterruptSourceTree", [child]) == -50)        // only a root can be
            #expect(Rest.signed("CreateInterruptSourceTree", [root]) == 0)
            #expect(Rest.signed("GetInterruptSourceTreeVector", [root, vec]) == 0 && rd32(vec) == 0xFFFF_FFFF)
            #expect(Rest.signed("DeactivateInterruptSourceTree", [root]) == -2421)
            #expect(Rest.signed("ActivateInterruptSourceTree", [root]) == -2401)      // nothing to run
            #expect(Rest.signed("InstallInterruptFunctions", [child, 1, 0, 0x1000, 0, 0]) == 0)
            #expect(Rest.signed("ActivateInterruptSourceTree", [root]) == -2401)      // the root's own members count
            #expect(Rest.signed("InstallInterruptFunctions", [root, 1, 0, 0x1000, 0, 0]) == 0)
            #expect(Rest.signed("ActivateInterruptSourceTree", [root]) == 0)
            #expect(Rest.signed("GetInterruptSourceTreeVector", [root, vec]) == 0 && rd32(vec) == 1)
            #expect(Rest.signed("ActivateInterruptSourceTree", [root]) == -2416)
            // a second tree takes the next vector
            let other = newSet()
            #expect(Rest.signed("InstallInterruptFunctions", [other, 1, 0, 0x1000, 0, 0]) == 0)
            #expect(Rest.signed("CreateInterruptSourceTree", [other]) == 0)
            #expect(Rest.signed("ActivateInterruptSourceTree", [other]) == 0)
            #expect(Rest.signed("GetInterruptSourceTreeVector", [other, vec]) == 0 && rd32(vec) == 2)
            #expect(Rest.signed("DeactivateInterruptSourceTree", [root]) == 0)
            #expect(Rest.signed("GetInterruptSourceTreeVector", [root, vec]) == 0 && rd32(vec) == 0xFFFF_FFFF)
            #expect(Rest.signed("ActivateInterruptSourceTree", [root]) == 0 && true)
            #expect(Rest.signed("GetInterruptSourceTreeVector", [root, vec]) == 0 && rd32(vec) == 1) // the free one
        }

        @Test func lookingUpTheTree() {
            let root = newSet()
            let mid = newSet(parent: root, member: 3)
            let leaf = newSet(parent: mid, member: 2)
            let out = Rest.ptr(4), member = Rest.ptr(4)
            #expect(Rest.signed("LookupRootSet", [leaf, out]) == 0 && rd32(out) == root)
            #expect(Rest.signed("LookupRootSet", [root, out]) == 0 && rd32(out) == root)
            #expect(Rest.signed("LookupParentMember", [leaf, out, member]) == 0 && rd32(out) == mid && rd32(member) == 2)
            #expect(Rest.signed("LookupParentMember", [mid, out, member]) == 0 && rd32(out) == root && rd32(member) == 3)
            #expect(Rest.signed("LookupParentMember", [root, out, member]) == 0 && rd32(out) == 0 && rd32(member) == 0)
            #expect(Rest.signed("LookupRootSet", [0x1234, out]) == -50)
        }

        // MARK: Driver Loader

        /// A PEF container with a data section holding a description of the
        /// given services and exports DoDriverIO (at 0) and TheDriverDescription (at 8).
        func driverContainer(services: [(String, String, UInt32)]) -> (UInt32, UInt32) {
            func be32(_ v: UInt32) -> [UInt8] { Rest.b32(v) }
            func be16(_ v: UInt16) -> [UInt8] { Rest.b16(v) }
            var desc = be32(0x7064_6573) + be32(0) // 'pdes', version
            desc += [UInt8(7)] + Array("mydrv.x".utf8) + [UInt8](repeating: 0, count: 24) + be32(0x0102_0000)
            desc += be32(1) + [UInt8(6)] + Array("MyDrvr".utf8) + [UInt8](repeating: 0, count: 25) + [UInt8](repeating: 0, count: 32)
            desc += be32(UInt32(services.count))
            for s in services { desc += Array(s.0.utf8) + Array(s.1.utf8) + be32(s.2) }
            let data = [UInt8](repeating: 0, count: 8) + desc
            let names = Array("DoDriverIOTheDriverDescription".utf8)
            var loader = be32(UInt32(bitPattern: -1)) + be32(0) + be32(UInt32(bitPattern: -1)) + be32(0)
            loader += be32(UInt32(bitPattern: -1)) + be32(0)
            let stringsOff: UInt32 = 56, hashOff = (stringsOff + UInt32(names.count) + 3) & ~3
            loader += be32(0) + be32(0) + be32(0) + be32(56) + be32(stringsOff) + be32(hashOff) + be32(0) + be32(2)
            loader += names
            while loader.count < Int(hashOff) { loader.append(0) }
            loader += be32(2 << 18 | 0)
            loader += be32(10 << 16) + be32(20 << 16)
            loader += be32(1 << 24 | 0) + be32(0) + be16(0)
            loader += be32(1 << 24 | 10) + be32(8) + be16(0)
            let dataOff = UInt32(40 + 2 * 28), loaderOff = dataOff + UInt32(data.count)
            var pef = Array("Joy!peffpwpc".utf8) + be32(1) + be32(0) + be32(0) + be32(0) + be32(0)
            pef += be16(2) + be16(1) + be32(0)
            pef += be32(UInt32(bitPattern: -1)) + be32(0) + be32(UInt32(data.count)) + be32(UInt32(data.count))
            pef += be32(UInt32(data.count)) + be32(dataOff) + [1, 1, 0, 0]
            pef += be32(UInt32(bitPattern: -1)) + be32(0) + be32(0) + be32(0) + be32(UInt32(loader.count))
            pef += be32(loaderOff) + [4, 1, 0, 0]
            pef += data + loader
            let p = tb_NewPtr(Int32(pef.count), false, true)
            for (i, b) in pef.enumerated() { wr8(p + UInt32(i), b) }
            return (p, UInt32(pef.count))
        }

        @Test func driverDescriptionFromMemory() {
            launchTestApp()
            cfm_reset()
            _ = cfm_ppc_cpu()
            let (pef, len) = driverContainer(services: [("ndrv", "blok", 0x0100_8000), ("file", "gnrc", 0x0200_8000)])
            let out = Rest.ptr(4)
            #expect(Rest.signed("GetDriverDescription", [pef, out]) == 0)
            let d = rd32(out)
            #expect(d != 0 && rd32(d) == 0x7064_6573)
            #expect(pstring(at: d + 8) == "mydrv.x" && pstring(at: d + 44 + 4) == "MyDrvr")
            #expect(rd32(d + 0x70) == 2 && rd32(d + 0x74) == 0x6E64_7276 && rd32(d + 0x84) == 0x676E_7263)
            #expect(len > 0)
            #expect(Rest.signed("GetDriverDescription", [pef, 0]) == -50)
            #expect(Rest.signed("GetDriverDescription", [0, out]) == -2806 && rd32(out) == 0)
            let junk = Rest.ptr(64)
            #expect(Rest.signed("GetDriverDescription", [junk, out]) == -2806)
        }

        @Test func candidates() {
            let name = Rest.ptr(32)
            guestPString("pci106b,12", at: name)
            func prop(_ n: String, _ version: UInt32) -> UInt32 {
                let p = Rest.ptr(36)
                guestPString(n, at: p)
                wr32(p + 32, version)
                return p
            }
            func file(_ n: String, _ version: UInt32, compatible: Bool) -> UInt32 {
                let f = Rest.ptr(112)
                guestPString(n, at: f + 70)
                wr32(f + 70 + 32, version)
                wr8(f + 0x6A, compatible ? 1 : 0)
                return f
            }
            let dev = Rest.ptr(16)
            func cmp(_ p: UInt32, _ f: UInt32) -> Int32 {
                Int32(Int16(truncatingIfNeeded: Rest.call("CompareFileCandToPropCand", [dev, name, p, f])))
            }
            let exact = prop("pci106b,12", 0x0100_8000)
            #expect(cmp(exact, file("pci106b,12", 0x0200_8000, compatible: false)) == -1) // higher file
            #expect(cmp(exact, file("pci106b,12", 0x0100_8000, compatible: false)) == 0)
            #expect(cmp(exact, file("pci106b,12", 0x0050_8000, compatible: false)) == 1)
            #expect(cmp(exact, file("other", 0x0900_8000, compatible: true)) == 1) // exact name beats compatible
            let compat = prop("other", 0x0100_8000)
            #expect(cmp(compat, file("pci106b,12", 0x0050_8000, compatible: false)) == -1)
            #expect(cmp(compat, file("other", 0x0200_8000, compatible: true)) == -1)
            #expect(Rest.signed("CompareFileCandToPropCand", [dev, 0, exact, 0]) == -50)
        }
    }
}
