import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// NameRegistryLib: entries, properties, paths and iteration over the
    /// tree that starts with the "Devices" plane's device-tree.
    @Suite struct OS9NameRegistry {
        init() { bootMachine() }

        func cstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(Int32(s.utf8.count + 1), false, true)
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func err(_ name: String, _ args: [UInt32]) -> Int32 { Int32(bitPattern: ilCall(name, args)) }

        func id() -> UInt32 { tb_NewPtr(16, false, true) }

        func lookup(_ path: String) -> UInt32? {
            let e = id()
            return err("RegistryCStrEntryLookup", [0, cstr(path), e]) == 0 ? e : nil
        }

        func create(_ parent: UInt32, _ name: String) -> UInt32 {
            let e = id()
            #expect(err("RegistryCStrEntryCreate", [parent, cstr(name), e]) == 0)
            return e
        }

        func string(_ p: UInt32) -> String {
            var s = [UInt8]()
            var i: UInt32 = 0
            while rd8(p + i) != 0 { s.append(rd8(p + i)); i += 1 }
            return String(decoding: s, as: UTF8.self)
        }

        @Test func idsAndTheStartingTree() {
            let a = id(), b = id()
            #expect(err("RegistryEntryIDInit", [a]) == 0 && err("RegistryEntryIDInit", [b]) == 0)
            #expect(ilCall("RegistryEntryIDCompare", [a, b]) & 0xFF == 1)
            #expect(err("RegistryEntryToPathSize", [a, tb_NewPtr(4, false, true)]) == -2538) // not an entry
            let tree = lookup("Devices:device-tree")
            #expect(tree != nil)
            #expect(lookup("Devices:nothing") == nil)
            #expect(err("RegistryEntryIDCopy", [tree!, a]) == 0)
            #expect(ilCall("RegistryEntryIDCompare", [a, tree!]) & 0xFF == 1)
            #expect(ilCall("RegistryEntryIDCompare", [a, b]) & 0xFF == 0)
            let size = tb_NewPtr(4, false, true), path = tb_NewPtr(64, false, true)
            #expect(err("RegistryEntryToPathSize", [tree!, size]) == 0 && rd32(size) == 20)
            #expect(err("RegistryCStrEntryToPath", [tree!, path, 64]) == 0 && string(path) == "Devices:device-tree")
            #expect(err("RegistryCStrEntryToPath", [tree!, path, 5]) == -2548)
            #expect(err("RegistryEntryIDDispose", [a]) == 0 && err("RegistryEntryToPathSize", [a, size]) == -2538)
        }

        @Test func entriesAndPaths() {
            let tree = lookup("Devices:device-tree")!
            let pci = create(tree, "pci@80000000")
            let slot = create(pci, "ethernet")
            #expect(err("RegistryCStrEntryCreate", [tree, cstr("pci@80000000"), id()]) == -2540) // there already
            #expect(err("RegistryCStrEntryCreate", [tree, cstr(String(repeating: "x", count: 48)), id()]) == -2541)
            let found = lookup("Devices:device-tree:pci@80000000:ethernet")
            #expect(found != nil && ilCall("RegistryEntryIDCompare", [found!, slot]) & 0xFF == 1)
            // relative lookup, and creating by path
            let rel = id()
            #expect(err("RegistryCStrEntryLookup", [pci, cstr("ethernet"), rel]) == 0)
            let deep = id()
            #expect(err("RegistryCStrEntryCreate", [tree, cstr("pci@80000000:scsi"), deep]) == 0)
            let name = tb_NewPtr(64, false, true), parent = id(), done = tb_NewPtr(1, false, true)
            #expect(err("RegistryCStrEntryToName", [deep, parent, name, done]) == 0)
            #expect(string(name) == "scsi" && ilCall("RegistryEntryIDCompare", [parent, pci]) & 0xFF == 1 && rd8(done) == 0)
            // Deleting takes what is under it
            #expect(err("RegistryEntryDelete", [pci]) == 0)
            #expect(lookup("Devices:device-tree:pci@80000000") == nil)
            #expect(err("RegistryEntryToPathSize", [slot, tb_NewPtr(4, false, true)]) == -2538)
            #expect(err("RegistryEntryDelete", [lookup("Devices")!]) == -2536) // a plane
        }

        @Test func properties() {
            let tree = lookup("Devices:device-tree")!
            let dev = create(tree, "gadget")
            let size = tb_NewPtr(4, false, true), buf = tb_NewPtr(64, false, true)
            // The name is a property already.
            #expect(err("RegistryPropertyGetSize", [dev, cstr("name"), size]) == 0 && rd32(size) == 7)
            for (i, b) in [1, 2, 3, 4].enumerated() { wr8(buf + UInt32(i), UInt8(b)) }
            #expect(err("RegistryPropertyCreate", [dev, cstr("reg"), buf, 4]) == 0)
            #expect(err("RegistryPropertyCreate", [dev, cstr("reg"), buf, 4]) == -2553)
            wr32(size, 8)
            let out = tb_NewPtr(8, false, true)
            #expect(err("RegistryPropertyGet", [dev, cstr("reg"), out, size]) == 0 && rd32(size) == 4 && rd32(out) == 0x01020304)
            wr32(size, 2)
            #expect(err("RegistryPropertyGet", [dev, cstr("reg"), out, size]) == -2548 && rd32(size) == 4)
            wr8(buf, 9)
            #expect(err("RegistryPropertySet", [dev, cstr("reg"), buf, 2]) == 0)
            #expect(err("RegistryPropertyGetSize", [dev, cstr("reg"), size]) == 0 && rd32(size) == 2)
            #expect(err("RegistryPropertySet", [dev, cstr("nope"), buf, 1]) == -2539)
            #expect(err("RegistryPropertyRename", [dev, cstr("reg"), cstr("assigned-addresses")]) == 0)
            #expect(err("RegistryPropertyGetSize", [dev, cstr("reg"), size]) == -2539)
            #expect(err("RegistryPropertyRename", [dev, cstr("assigned-addresses"), cstr("name")]) == -2553)
            // Modifiers
            let m = tb_NewPtr(4, false, true)
            #expect(err("RegistryPropertySetMod", [dev, cstr("assigned-addresses"), 0x20]) == 0)
            #expect(err("RegistryPropertyGetMod", [dev, cstr("assigned-addresses"), m]) == 0 && rd32(m) == 0x20)
            #expect(err("RegistryEntrySetMod", [dev, 0x40]) == 0)
            #expect(err("RegistryEntryGetMod", [dev, m]) == 0 && rd32(m) == 0x40)
            // Iterating properties
            let cookie = tb_NewPtr(4, false, true), n = tb_NewPtr(64, false, true), done = tb_NewPtr(1, false, true)
            #expect(err("RegistryPropertyIterateCreate", [dev, cookie]) == 0)
            #expect(err("RegistryPropertyIterate", [cookie, n, done]) == 0 && string(n) == "name" && rd8(done) == 0)
            #expect(err("RegistryPropertyIterate", [cookie, n, done]) == 0 && string(n) == "assigned-addresses" && rd8(done) == 1)
            #expect(err("RegistryPropertyIterate", [cookie, n, done]) == -2554)
            #expect(err("RegistryPropertyIterateDispose", [cookie]) == 0)
            #expect(err("RegistryPropertyDelete", [dev, cstr("assigned-addresses")]) == 0)
            #expect(err("RegistryPropertyDelete", [dev, cstr("assigned-addresses")]) == -2539)
        }

        @Test func iterationAndSearch() {
            let tree = lookup("Devices:device-tree")!
            let a = create(tree, "a"), b = create(tree, "b"), c = create(tree, "c")
            let a1 = create(a, "a1")
            let v = tb_NewPtr(4, false, true)
            wr32(v, 7)
            #expect(err("RegistryPropertyCreate", [c, cstr("tag"), v, 4]) == 0)
            #expect(err("RegistryPropertyCreate", [a1, cstr("tag"), v, 4]) == 0)
            let cookie = tb_NewPtr(4, false, true), found = id(), done = tb_NewPtr(1, false, true)
            #expect(err("RegistryEntryIterateCreate", [cookie]) == 0)
            #expect(err("RegistryEntryIterateSet", [cookie, tree]) == 0)
            func nameOf(_ e: UInt32) -> String {
                let n = tb_NewPtr(64, false, true), p = id(), d = tb_NewPtr(1, false, true)
                _ = err("RegistryCStrEntryToName", [e, p, n, d])
                return string(n)
            }
            var names: [String] = []
            #expect(err("RegistryEntryIterate", [cookie, 4, found, done]) == 0) // children
            names.append(nameOf(found))
            var last = rd8(done)
            while last == 0 {
                #expect(err("RegistryEntryIterate", [cookie, 1, found, done]) == 0) // continue
                names.append(nameOf(found))
                last = rd8(done)
            }
            #expect(names == ["a", "b", "c"])
            #expect(err("RegistryEntryIterate", [cookie, 1, found, done]) == -2554)
            // Descendants, depth first
            names = []
            #expect(err("RegistryEntryIterateSet", [cookie, tree]) == 0)
            #expect(err("RegistryEntryIterate", [cookie, 5, found, done]) == 0)
            names.append(nameOf(found))
            while rd8(done) == 0 {
                #expect(err("RegistryEntryIterate", [cookie, 1, found, done]) == 0)
                names.append(nameOf(found))
            }
            #expect(names == ["a", "a1", "b", "c"])
            // Parents of a1, and siblings of b
            #expect(err("RegistryEntryIterateSet", [cookie, a1]) == 0)
            #expect(err("RegistryEntryIterate", [cookie, 3, found, done]) == 0 && nameOf(found) == "a")
            #expect(err("RegistryEntryIterateSet", [cookie, b]) == 0)
            #expect(err("RegistryEntryIterate", [cookie, 6, found, done]) == 0 && nameOf(found) == "a")
            // Search the tree for entries with tag = 7, and for any with the property
            #expect(err("RegistryEntryIterateSet", [cookie, tree]) == 0)
            names = []
            var r = err("RegistryEntrySearch", [cookie, 5, found, done, cstr("tag"), v, 4])
            while r == 0 {
                names.append(nameOf(found))
                if rd8(done) == 1 { break }
                r = err("RegistryEntrySearch", [cookie, 1, found, done, cstr("tag"), v, 4])
            }
            #expect(names == ["a1", "c"])
            wr32(v, 8)
            #expect(err("RegistryEntryIterateSet", [cookie, tree]) == 0)
            #expect(err("RegistryEntrySearch", [cookie, 5, found, done, cstr("tag"), v, 4]) == -2539)
            // By modifiers
            #expect(err("RegistryEntrySetMod", [b, 0x40]) == 0)
            #expect(err("RegistryEntryIterateSet", [cookie, tree]) == 0)
            #expect(err("RegistryEntryMod", [cookie, 4, found, done, 0x40]) == 0 && nameOf(found) == "b")
            #expect(err("RegistryEntryIterateDispose", [cookie]) == 0)
        }

        @Test func copies() {
            let tree = lookup("Devices:device-tree")!
            let src = create(tree, "src"), sub = create(src, "sub")
            let v = tb_NewPtr(4, false, true)
            wr32(v, 5)
            #expect(err("RegistryPropertyCreate", [sub, cstr("x"), v, 4]) == 0)
            let dst = create(tree, "dstParent"), copy = id()
            #expect(err("RegistryEntryCopy", [dst, src, copy]) == 0)
            let found = lookup("Devices:device-tree:dstParent:src:sub")
            #expect(found != nil)
            let size = tb_NewPtr(4, false, true)
            #expect(err("RegistryPropertyGetSize", [found!, cstr("x"), size]) == 0 && rd32(size) == 4)
        }

        @Test func aNewRunStartsFresh() {
            let tree = lookup("Devices:device-tree")!
            _ = create(tree, "temporary")
            bootMachine()
            #expect(lookup("Devices:device-tree") != nil && lookup("Devices:device-tree:temporary") == nil)
        }
    }
}
