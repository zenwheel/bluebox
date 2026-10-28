import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// PCILib and VideoServicesLib: no bus and no card to reach.
    @Suite struct OS9RestHardware {
        init() { bootMachine() }

        func id() -> UInt32 { Rest.ptr(16) }

        func entry(_ parent: UInt32, _ name: String) -> UInt32 {
            let e = id()
            #expect(Rest.signed("RegistryCStrEntryCreate", [parent, Rest.cstr(name), e]) == 0)
            return e
        }

        func tree() -> UInt32 {
            let e = id()
            #expect(Rest.signed("RegistryCStrEntryLookup", [0, Rest.cstr("Devices:device-tree"), e]) == 0)
            return e
        }

        @Test func endianSwaps() {
            #expect(Rest.call("EndianSwap16Bit", [0x1234]) == 0x3412)
            #expect(Rest.call("EndianSwap16Bit", [0xABCD_1234]) == 0x3412)
            #expect(Rest.call("EndianSwap32Bit", [0x1234_5678]) == 0x7856_3412)
        }

        @Test func expansionManagerAccesses() {
            let root = tree()
            let bridge = entry(root, "pci@f2000000")
            #expect(Rest.signed("RegistryPropertyCreate", [bridge, Rest.cstr("device_type"), Rest.cstr("pci"), 4]) == 0)
            let ata = entry(bridge, "ata-3@20")
            let plain = entry(root, "cpus")
            let v = Rest.ptr(4)
            let reads = ["ExpMgrConfigReadByte", "ExpMgrConfigReadWord", "ExpMgrConfigReadLong",
                         "ExpMgrIOReadByte", "ExpMgrIOReadWord", "ExpMgrIOReadLong"]
            let writes = ["ExpMgrConfigWriteByte", "ExpMgrConfigWriteWord", "ExpMgrConfigWriteLong",
                          "ExpMgrIOWriteByte", "ExpMgrIOWriteWord", "ExpMgrIOWriteLong"]
            for r in reads {
                #expect(Rest.signed(r, [ata, 0x10, v]) == -200, "\(r) on a device behind a PCI bridge")
                #expect(Rest.signed(r, [bridge, 0x10, v]) == -200)
                #expect(Rest.signed(r, [plain, 0x10, v]) == -2539, "\(r) on a node off the bus")
                #expect(Rest.signed(r, [id(), 0x10, v]) == -2538)
                #expect(Rest.signed(r, [0, 0x10, v]) == -50)
            }
            for w in writes {
                #expect(Rest.signed(w, [ata, 0x10, 1]) == -200)
                #expect(Rest.signed(w, [plain, 0x10, 1]) == -2539)
                #expect(Rest.signed(w, [id(), 0x10, 1]) == -2538)
            }
            for r in ["ExpMgrInterruptAcknowledgeReadByte", "ExpMgrInterruptAcknowledgeReadWord",
                      "ExpMgrInterruptAcknowledgeReadLong"] {
                #expect(Rest.signed(r, [ata, v]) == -200 && Rest.signed(r, [plain, v]) == -2539)
            }
            #expect(Rest.signed("ExpMgrSpecialCycleWriteLong", [ata, 7]) == -200)
            #expect(Rest.signed("ExpMgrSpecialCycleWriteLong", [plain, 7]) == -2539)
            #expect(Rest.signed("ExpMgrSpecialCycleBroadcastLong", [7]) == -200)
        }

        func fourCC(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

        @Test func videoInterruptServices() {
            let dev = entry(tree(), "display")
            let sid = Rest.ptr(4), sid2 = Rest.ptr(4)
            #expect(Rest.signed("VSLNewInterruptService", [dev, fourCC("vbl "), sid]) == 0 && rd32(sid) != 0)
            #expect(Rest.signed("VSLNewInterruptService", [dev, fourCC("fram"), sid2]) == 0 && rd32(sid2) != rd32(sid))
            #expect(Rest.signed("VSLNewInterruptService", [dev, fourCC("nope"), sid2]) == -50)
            #expect(Rest.signed("VSLNewInterruptService", [id(), fourCC("vbl "), sid2]) == -2538)
            #expect(Rest.signed("VSLDoInterruptService", [rd32(sid)]) == 0)
            #expect(Rest.signed("VSLDoInterruptService", [0x1234]) == -50)
            #expect(Rest.signed("VSLDisposeInterruptService", [rd32(sid)]) == 0)
            #expect(Rest.signed("VSLDisposeInterruptService", [rd32(sid)]) == -50)
            #expect(Rest.signed("VSLDoInterruptService", [rd32(sid)]) == -50)
            #expect(Rest.call("VSLPrepareCursorForHardwareCursor", [0, 0, 0]) & 0xFF == 0)
        }
    }
}
