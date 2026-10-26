import CCore
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// PrivateInterfaceLib's names that Mac OS 9 added and Blue Box backs
    /// (private_os9.c).
    @Suite struct PrivateInterfaceOS9 {
        func ptr(_ n: UInt32) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

        @Test func blockClearZeroesWhateverTheCount() {
            bootMachine()
            for n: UInt32 in [0, 1, 7, 8, 9, 16, 21] {
                let p = ptr(32)
                for i in 0..<32 { wr8(p + UInt32(i), 0xAA) }
                ilCall("BlockClear", [p + 3, n])
                for i in 0..<32 {
                    let inside = UInt32(i) >= 3 && UInt32(i) < 3 + n
                    #expect(rd8(p + UInt32(i)) == (inside ? 0 : 0xAA), "count \(n) byte \(i)")
                }
            }
        }

        @Test func fontsBelongToTheirScript() {
            bootMachine()
            for font: Int16 in [0, 3, 21, 2000] {
                #expect(Int16(truncatingIfNeeded: ilCall("Font2RealScript", [UInt32(UInt16(bitPattern: font))]))
                        == tb_FontToScript(font))
            }
        }

        @Test func layersAndTheActiveWindow() {
            bootMachine()
            let app = tb_GetCurLayer()
            let root = ilCall("GetRootLayer", [])
            #expect(root != 0 && root == tb_GetParent(app))
            #expect(ilCall("FrontWindowIn", [app]) == 0 && ilCall("ActiveWindow", []) == 0)
            let r = ptr(8)
            wr16(r + 4, 100)
            wr16(r + 6, 100)
            let name = ptr(4)
            guestPString("W", at: name)
            let w = tb_NewWindow(0, r, name, 1, 0, 0xFFFF_FFFF, 0, 0)
            #expect(ilCall("FrontWindowIn", [app]) == w)
            #expect(ilCall("FrontWindowIn", [root]) == app)
            tb_HiliteWindow(w, 1)
            #expect(ilCall("ActiveWindow", []) == w)
            tb_HiliteWindow(w, 0)
            #expect(ilCall("ActiveWindow", []) == 0)
        }

        @Test func switchingNests() {
            bootMachine()
            #expect(ilCall("GetSwitchingEnabled", []) == 1)
            ilCall("DisableSwitching", [])
            ilCall("DisableSwitching", [])
            #expect(ilCall("GetSwitchingEnabled", []) == 0)
            ilCall("EnableSwitching", [])
            #expect(ilCall("GetSwitchingEnabled", []) == 0)
            ilCall("EnableSwitching", [])
            #expect(ilCall("GetSwitchingEnabled", []) == 1)
            ilCall("EnableSwitching", []) // more enables than disables change nothing
            #expect(ilCall("GetSwitchingEnabled", []) == 1)
        }

        @Test func fileSystemChecks() throws {
            let dir = launchTestApp()
            let file = dir.appendingPathComponent("data.bin")
            try Data("x".utf8).write(to: file)
            let spec = ptr(UInt32(FSSpec_SIZEOF))
            fm_spec_for_path(file.path, spec)
            let r = ptr(80), rnp = ptr(2)
            #expect(tb_FSpMakeFSRef(spec, r) == 0)
            #expect(tb_FSOpenFork(r, 0, 0, 1, rnp) == 0) // read only
            let rn = UInt32(rd16(rnp))

            #expect(ilCall16("UTCheckFileRefNum", [rn]) == 0)
            #expect(ilCall16("UTCheckFileRefNum", [rn + 94]) == -51)
            #expect(ilCall16("UTCheckFileRefNum", [0]) == -51)
            #expect(ilCall16("UTCheckFileModifiable", [rn]) == -61) // not opened for writing
            #expect(ilCall16("UTCheckFileModifiable", [0]) == -51)
            tb_FSCloseFork(Int16(rn))
            let rnw = ptr(2)
            #expect(tb_FSOpenFork(r, 0, 0, 3, rnw) == 0)
            #expect(ilCall16("UTCheckFileModifiable", [UInt32(rd16(rnw))]) == 0)

            #expect(ilCall16("UTCheckVolRefNum", [0]) == 0)
            #expect(ilCall16("UTCheckVolOffline", [0]) == 0)
            #expect(ilCall16("UTCheckVolModifiable", [0]) == 0)
            #expect(ilCall16("UTCheckVolRefNum", [UInt32(UInt16(bitPattern: -999))]) == -35)
            #expect(ilCall16("UTCheckVolOffline", [UInt32(UInt16(bitPattern: -999))]) == -35)
            #expect(ilCall16("UTCheckVolModifiable", [UInt32(UInt16(bitPattern: -999))]) == -35)
        }
    }
}
