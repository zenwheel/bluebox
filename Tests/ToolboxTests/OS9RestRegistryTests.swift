import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// NameRegistryLib's Open Firmware paths, device node IDs and property
    /// pointers.
    @Suite struct OS9RestRegistry {
        init() { bootMachine() }

        func id() -> UInt32 { Rest.ptr(16) }

        func lookup(_ path: String) -> UInt32 {
            let e = id()
            #expect(Rest.signed("RegistryCStrEntryLookup", [0, Rest.cstr(path), e]) == 0)
            return e
        }

        func create(_ parent: UInt32, _ name: String) -> UInt32 {
            let e = id()
            #expect(Rest.signed("RegistryCStrEntryCreate", [parent, Rest.cstr(name), e]) == 0)
            return e
        }

        @Test func openFirmwarePaths() {
            Rest.call("NR_Init")
            let tree = lookup("Devices:device-tree")
            let pci = create(tree, "pci@f2000000")
            let ata = create(pci, "ata-3@20")
            let size = Rest.ptr(4), path = Rest.ptr(64)
            #expect(Rest.signed("RegistryEntryToOFPathSize", [tree, size]) == 0 && rd32(size) == 2)
            #expect(Rest.signed("RegistryCStrEntryToOFPath", [tree, path, 64]) == 0 && Rest.string(path) == "/")
            #expect(Rest.signed("RegistryEntryToOFPathSize", [ata, size]) == 0 && rd32(size) == 23)
            #expect(Rest.signed("RegistryCStrEntryToOFPath", [ata, path, 64]) == 0)
            #expect(Rest.string(path) == "/pci@f2000000/ata-3@20")
            #expect(Rest.signed("RegistryCStrEntryToOFPath", [ata, path, 22]) == -2548) // too small
            // the planes and the root are not the firmware's
            let devices = lookup("Devices")
            #expect(Rest.signed("RegistryCStrEntryToOFPath", [devices, path, 64]) == -2539)
            #expect(Rest.signed("RegistryEntryToOFPathSize", [id(), size]) == -2538) // not an entry
        }

        @Test func deviceNodeIDs() {
            let tree = lookup("Devices:device-tree")
            let dev = create(tree, "cpus")
            let node = Rest.ptr(4), again = Rest.ptr(4), back = id()
            #expect(Rest.signed("RegistryEntryIDToDeviceNodeID", [dev, node]) == 0 && rd32(node) != 0)
            #expect(Rest.signed("RegistryEntryIDToDeviceNodeID", [dev, again]) == 0 && rd32(again) == rd32(node))
            let other = create(tree, "memory")
            let node2 = Rest.ptr(4)
            #expect(Rest.signed("RegistryEntryIDToDeviceNodeID", [other, node2]) == 0 && rd32(node2) != rd32(node))
            #expect(Rest.signed("RegistryDeviceNodeIDToEntryID", [rd32(node), back]) == 0)
            #expect(Rest.call("RegistryEntryIDCompare", [back, dev]) & 0xFF == 1)
            #expect(Rest.signed("RegistryDeviceNodeIDToEntryID", [0, back]) == -2538)
            #expect(Rest.signed("RegistryDeviceNodeIDToEntryID", [0x1234, back]) == -2538)
            #expect(Rest.signed("RegistryEntryIDToDeviceNodeID", [id(), node]) == -2538)
            // a deleted entry's node is gone
            #expect(Rest.signed("RegistryEntryDelete", [dev]) == 0)
            #expect(Rest.signed("RegistryDeviceNodeIDToEntryID", [rd32(node), back]) == -2538)
        }

        @Test func propertyPointers() {
            let tree = lookup("Devices:device-tree")
            let e = create(tree, "chosen")
            let value = Rest.cstr("hello")
            #expect(Rest.signed("RegistryPropertyCreate", [e, Rest.cstr("greeting"), value, 6]) == 0)
            let pp = Rest.ptr(4), sp = Rest.ptr(4)
            #expect(Rest.signed("RegistryPropertyGetPtr", [e, Rest.cstr("greeting"), pp, sp]) == 0)
            #expect(rd32(sp) == 6 && Rest.string(rd32(pp)) == "hello")
            // changing the property gives a pointer to the new value
            #expect(Rest.signed("RegistryPropertySet", [e, Rest.cstr("greeting"), Rest.cstr("bye"), 4]) == 0)
            #expect(Rest.signed("RegistryPropertyGetPtr", [e, Rest.cstr("greeting"), pp, sp]) == 0)
            #expect(rd32(sp) == 4 && Rest.string(rd32(pp)) == "bye")
            wr32(sp, 99)
            #expect(Rest.signed("RegistryPropertyGetPtr", [e, Rest.cstr("nothing"), pp, sp]) == -2539 && rd32(sp) == 0)
            #expect(Rest.signed("RegistryPropertyGetPtr", [id(), Rest.cstr("greeting"), pp, sp]) == -2538)
        }
    }
}
