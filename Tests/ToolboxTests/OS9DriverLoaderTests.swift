import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// DriverLoaderLib: the unit table's drivers, version comparison, the
    /// registry-based searches, and PowerPC drivers it cannot install.
    @Suite struct OS9DriverLoader {
        init() { bootMachine() }

        func err(_ name: String, _ args: [UInt32]) -> Int32 { Int32(bitPattern: ilCall(name, args)) }

        func cstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(Int32(s.utf8.count + 1), false, true)
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        @Test func theUnitTable() {
            launchTestApp()
            #expect(ilCall("HighestUnitNumber", []) == 63)
            // The serial and MacTCP drivers are in it.
            let n = tb_NewPtr(4, false, true), refs = tb_NewPtr(256, false, true)
            wr32(n, 100)
            #expect(err("LookupDrivers", [0, 63, 0, n, refs]) == 0)
            let found = rd32(n)
            #expect(found >= 4 && found <= 64)
            // Empty units are the rest
            wr32(n, 100)
            #expect(err("LookupDrivers", [0, 63, 1, n, refs]) == 0 && rd32(n) + found == 64)
            wr32(n, 2)
            #expect(err("LookupDrivers", [0, 63, 0, n, refs]) == 0 && rd32(n) == 2) // room for two
            let ref = rd16(refs)
            #expect(Int16(bitPattern: ref) < 0)

            let unit = tb_NewPtr(2, false, true), flags = tb_NewPtr(2, false, true), count = tb_NewPtr(4, false, true)
            let name = tb_NewPtr(64, false, true), dev = tb_NewPtr(16, false, true), loc = tb_NewPtr(16, false, true)
            #expect(err("GetDriverInformation", [UInt32(ref), unit, flags, count, name, dev, loc, 0, 0, 0]) == 0)
            #expect(Int16(bitPattern: rd16(unit)) == -1 - Int16(bitPattern: ref) && rd8(name) > 0)
            #expect(err("GetDriverInformation", [UInt32(bitPattern: -64), unit, flags, count, name, dev, loc, 0, 0, 0]) == -21)
            // Driver Gestalt
            #expect(ilCall("DriverGestaltIsOn", [0]) & 0xFF == 0 && ilCall("DriverGestaltIsOn", [4]) & 0xFF == 1)
            #expect(err("DriverGestaltOn", [UInt32(ref)]) == 0)
            #expect(err("GetDriverInformation", [UInt32(ref), unit, flags, count, name, dev, loc, 0, 0, 0]) == 0)
            #expect(ilCall("DriverGestaltIsOn", [UInt32(rd16(flags))]) & 0xFF == 1)
            #expect(err("DriverGestaltOff", [UInt32(ref)]) == 0)
            #expect(err("GetDriverInformation", [UInt32(ref), unit, flags, count, name, dev, loc, 0, 0, 0]) == 0)
            #expect(ilCall("DriverGestaltIsOn", [UInt32(rd16(flags))]) & 0xFF == 0)
            #expect(err("DriverGestaltOn", [UInt32(bitPattern: -64)]) == -21)
        }

        @Test func versions() {
            let a = tb_NewPtr(4, false, true), b = tb_NewPtr(4, false, true)
            wr32(a, 0x0201_8000); wr32(b, 0x0200_8000)       // 2.0.1 and 2.0
            #expect(ilCall("HigherDriverVersion", [a, b]) == 1)
            #expect(Int32(bitPattern: ilCall("HigherDriverVersion", [b, a])) == -1)
            wr32(b, 0x0201_8000)
            #expect(ilCall("HigherDriverVersion", [a, b]) == 0)
            wr32(b, 0x0201_6003)                              // a beta of the same
            #expect(ilCall("HigherDriverVersion", [a, b]) == 1)
        }

        @Test func renamingAndRemoving() {
            launchTestApp()
            let empty = UInt32(bitPattern: -33) // unit 32, nothing installed
            let name = tb_NewPtr(64, false, true)
            guestPString(".Renamed", at: name)
            #expect(err("RenameDriver", [empty, name]) == -21)
            #expect(err("RemoveDriver", [empty, 0]) == -1)
            let n = tb_NewPtr(4, false, true), refs = tb_NewPtr(256, false, true)
            wr32(n, 1)
            #expect(err("LookupDrivers", [0, 63, 0, n, refs]) == 0 && rd32(n) == 1)
            let ref = UInt32(rd16(refs))
            #expect(err("RenameDriver", [ref, name]) == 0)
            let got = tb_NewPtr(64, false, true), scratch = tb_NewPtr(64, false, true)
            #expect(err("GetDriverInformation", [ref, scratch, scratch + 2, scratch + 4, got, scratch + 8, scratch + 8, 0, 0, 0]) == 0)
            #expect(pstring(at: got) == ".Renamed")
            // an installed driver that isn't open can be removed
            #expect(err("RemoveDriver", [ref, 0]) == 0)
            #expect(err("GetDriverInformation", [ref, scratch, scratch + 2, scratch + 4, got, scratch + 8, scratch + 8, 0, 0, 0]) == -21)
        }

        @Test func driversForADevice() {
            let reg = { (name: String, args: [UInt32]) in Int32(bitPattern: ilCall(name, args)) }
            let tree = tb_NewPtr(16, false, true), dev = tb_NewPtr(16, false, true)
            #expect(reg("RegistryCStrEntryLookup", [0, cstr("Devices:device-tree"), tree]) == 0)
            #expect(reg("RegistryCStrEntryCreate", [tree, cstr("widget"), dev]) == 0)
            // compatible: two strings
            let compat = tb_NewPtr(32, false, true)
            let bytes = Array("widget-a".utf8) + [0] + Array("widget-b".utf8) + [0]
            for (i, b) in bytes.enumerated() { wr8(compat + UInt32(i), b) }
            #expect(reg("RegistryPropertyCreate", [dev, cstr("compatible"), compat, UInt32(bytes.count)]) == 0)
            let names = tb_NewPtr(4, false, true), count = tb_NewPtr(4, false, true)
            ilCall("GetCompatibleProperty", [dev, names, count])
            #expect(rd32(count) == 2 && rd8(rd32(names)) == 0x77)
            let want = tb_NewPtr(16, false, true), idx = tb_NewPtr(4, false, true)
            guestPString("widget-b", at: want)
            #expect(ilCall("CompatibleDriverNames", [want, rd32(names), 2, idx]) & 0xFF == 1 && rd32(idx) == 1)
            guestPString("gadget", at: want)
            #expect(ilCall("CompatibleDriverNames", [want, rd32(names), 2, idx]) & 0xFF == 0)
            // No driver for it anywhere
            let prop = tb_NewPtr(4, false, true), psize = tb_NewPtr(4, false, true), dname = tb_NewPtr(40, false, true)
            let ptype = tb_NewPtr(40, false, true), got = tb_NewPtr(1, false, true), nfiles = tb_NewPtr(4, false, true)
            wr32(nfiles, 5)
            #expect(err("FindDriverCandidates", [dev, prop, psize, dname, ptype, got, 0, nfiles]) == 0)
            #expect(rd8(got) == 0 && rd32(nfiles) == 0 && rd8(dname) == 6) // "widget"
            #expect(err("ScanDriverCandidates", [dev, 0, 0, 0, nfiles]) == 0 && rd32(nfiles) == 0)
            #expect(err("GetDriverForDevice", [dev, 0, 0, 0]) == -43)
            #expect(err("InstallDriverForDevice", [dev, 10, 12, 0]) == -43)
            #expect(err("InstallDriverFromFile", [0x100, dev, 10, 12, 0]) == -4)
            // A device with a driver property
            let blob = tb_NewPtr(8, false, true)
            for i in 0..<8 { wr8(blob + UInt32(i), UInt8(i)) }
            #expect(reg("RegistryPropertyCreate", [dev, cstr("driver,AAPL,MacOS,PowerPC"), blob, 8]) == 0)
            #expect(err("FindDriverCandidates", [dev, prop, psize, dname, ptype, got, 0, nfiles]) == 0)
            #expect(rd8(got) == 1 && rd32(psize) == 8 && rd8(rd32(prop) + 5) == 5)
            // An entry that isn't
            let gone = tb_NewPtr(16, false, true)
            #expect(err("FindDriverCandidates", [gone, prop, psize, dname, ptype, got, 0, nfiles]) == -2538)
        }

        @Test func fragmentsThatAreNotDrivers() {
            launchTestApp()
            let conn = tb_NewPtr(4, false, true), mainp = tb_NewPtr(4, false, true), desc = tb_NewPtr(4, false, true)
            #expect(err("VerifyFragmentAsDriver", [0x1234, mainp, desc]) < 0)
            #expect(err("GetDriverMemoryFragment", [0, 10, 0, conn, mainp, desc]) == -50)
            #expect(err("GetDriverDiskFragment", [0, conn, mainp, desc]) == -50)
            let junk = tb_NewPtr(64, false, true)
            let nm = tb_NewPtr(64, false, true)
            guestPString("junk", at: nm)
            #expect(err("GetDriverMemoryFragment", [junk, 64, nm, conn, mainp, desc]) < 0)
            #expect(err("SetDriverClosureMemory", [0, 1]) == -50)
        }
    }
}
