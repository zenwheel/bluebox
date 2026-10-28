import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// PrivateInterfaceLib's and InterfaceLib's private names that Blue
    /// Box has state to answer on (os9rest_priv.c).
    @Suite struct OS9RestPrivate {
        init() { Rest.boot() }

        @Test func resourceMaps() {
            launchTestApp()
            let sys = Int16(bitPattern: rd16(UInt32(LM_SysMap)))
            let map = Rest.call("GetMap", [UInt32(UInt16(bitPattern: sys))])
            #expect(map != 0 && map == rd32(UInt32(LM_SysMapHndl)))
            #expect(Rest.call("IsThisASystemResourceMap", [map]) & 0xFF == 1)
            let cur = Int16(bitPattern: rd16(UInt32(LM_CurMap)))
            let app = Rest.call("GetMap", [UInt32(UInt16(bitPattern: cur))])
            #expect(app != 0)
            if cur != sys { #expect(Rest.call("IsThisASystemResourceMap", [app]) & 0xFF == 0) }
            #expect(Rest.call("GetMap", [0x1234]) == 0)
            #expect(Rest.call("IsThisASystemResourceMap", [0]) & 0xFF == 0)
        }

        @Test func quickDrawGlobalsAndPortRects() {
            launchTestApp()
            _ = initToolbox()
            let a5 = rd32(UInt32(LM_CurrentA5))
            let globals = Rest.call("GetQDGlobals")
            #expect(globals != 0 && globals + UInt32(QDGlobals_thePort_off) == rd32(a5))
            let r = Rest.ptr(8), name = Rest.ptr(4)
            wr16(r, 100); wr16(r + 2, 120); wr16(r + 4, 180); wr16(r + 6, 220)
            guestPString("W", at: name)
            let w = tb_NewWindow(0, r, name, 1, 0, 0xFFFF_FFFF, 0, 0)
            let out = Rest.ptr(8)
            Rest.call("GlobalPortRect", [w, out])
            #expect([rd16(out), rd16(out + 2), rd16(out + 4), rd16(out + 6)] == [100, 120, 180, 220])
            Rest.call("GlobalPortRect", [0, out]) // nothing to do
        }

        @Test func emulatorRegisters() {
            launchTestApp()
            withUnsafeMutableBytes(of: &g_cpu!.pointee.d) { for i in 0..<8 { $0.storeBytes(of: UInt32(0x100 + i), toByteOffset: i * 4, as: UInt32.self) } }
            withUnsafeMutableBytes(of: &g_cpu!.pointee.a) { for i in 0..<7 { $0.storeBytes(of: UInt32(0x200 + i), toByteOffset: i * 4, as: UInt32.self) } }
            #expect(Rest.call("GetEmulatorRegister", [0]) == 0x100 && Rest.call("GetEmulatorRegister", [7]) == 0x107)
            #expect(Rest.call("GetEmulatorRegister", [8]) == 0x200 && Rest.call("GetEmulatorRegister", [14]) == 0x206)
            #expect(Rest.call("GetEmulatorRegister", [15]) == 15 && Rest.call("GetEmulatorRegister", [0x1234]) == 0x1234)
            Rest.call("SetEmulatorRegister", [3, 0xABCD])
            Rest.call("SetEmulatorRegister", [9, 0x4321])
            Rest.call("SetEmulatorRegister", [40, 0x1111])
            #expect(Rest.call("GetEmulatorRegister", [3]) == 0xABCD && Rest.call("GetEmulatorRegister", [9]) == 0x4321)
            #expect(Rest.call("GetEmulatorRegister", [2]) == 0x102)
        }

        @Test func exceptionHandlerIsSwapped() {
            #expect(Rest.call("InstallSystemExceptionHandler", [0x5000]) == 0)
            #expect(Rest.call("InstallSystemExceptionHandler", [0x6000]) == 0x5000)
            #expect(Rest.call("InstallSystemExceptionHandler", [0]) == 0x6000)
        }

        @Test func startUpAndCleanUpCalls() {
            for n in ["InitScripts", "ReInitScripts", "InitScriptApp", "CleanupScriptApp", "InitLayers",
                      "InitializeFolderManager", "InitKeybdMenu", "InitApplication", "CleanupApplication",
                      "IUClearCacheSys"] {
                #expect(Rest.signed(n) == 0, "\(n)")
            }
            Rest.call("MakeExecutable", [0x1000, 64])
            Rest.call("FlushToExecute")
            Rest.call("FlushEmulatorCacheRange", [0x1000, 64])
            Rest.call("PrintCopyright")
            wr16(UInt32(LM_MemErr), 0xFFFF)
            Rest.call("MoveHLow", [0])
            #expect(memErr() == 0)
        }

        @Test func menuBarRectangles() {
            launchTestApp()
            let r = Rest.ptr(8)
            #expect(Rest.signed("GetMBARRect", [r]) == 0)
            #expect(rd16(r) == 0 && rd16(r + 2) == 0 && rd16(r + 4) == rd16(UInt32(LM_MBarHeight)) && rd16(r + 6) > 100)
            #expect(Rest.signed("GetMBARRect", [0]) == -50)
            let title = Rest.ptr(16)
            guestPString("Test", at: title)
            let menu = tb_NewMenu(300, title)
            tb_InsertMenu(menu, 0)
            tb_DrawMenuBar()
            #expect(Rest.signed("GetMenuTitleRect", [300, r]) == 0)
            #expect(rd16(r) == 0 && rd16(r + 4) < rd16(UInt32(LM_MBarHeight)) + 1 && rd16(r + 6) > rd16(r + 2))
            #expect(Rest.signed("GetMenuTitleRect", [9999, r]) == -127)
            tb_DeleteMenu(300)
            #expect(Rest.signed("GetMenuTitleRect", [300, r]) == -127)
        }

        @Test func processesAndLayers() {
            launchTestApp()
            let psn = Rest.ptr(8)
            let layer = tb_GetCurLayer()
            #expect(Rest.signed("GetProcessFromLayer", [layer, psn]) == 0 && rd32(psn) == 0 && rd32(psn + 4) == 0x1000)
            #expect(Rest.signed("GetProcessFromLayer", [0, psn]) == -600)
            #expect(Rest.signed("GetProcessFromLayer", [layer, 0]) == -50)
            wr32(psn, 0); wr32(psn + 4, 0x1001) // the Finder is not ours to kill
            #expect(Rest.signed("KillProcess", [psn]) == -600)
            #expect(Rest.signed("KillProcess", [0]) == -50)
        }

        @Test func tableclothPerDisplay() {
            launchTestApp()
            let gd = rd32(UInt32(LM_MainDevice))
            let out = Rest.ptr(4)
            wr32(out, 7)
            #expect(Rest.signed("DMGetGDeviceTablecloth", [gd, out]) == -6231 && rd32(out) == 0)
            let cloth = tb_NewHandle(16, false, true)
            #expect(Rest.signed("DMSetGDeviceTablecloth", [gd, cloth]) == 0)
            #expect(Rest.signed("DMGetGDeviceTablecloth", [gd, out]) == 0 && rd32(out) == cloth)
            #expect(Rest.signed("DMSetGDeviceTablecloth", [gd, 0]) == 0)
            #expect(Rest.signed("DMGetGDeviceTablecloth", [gd, out]) == -6231)
            #expect(Rest.signed("DMGetGDeviceTablecloth", [0, out]) == -50)
        }
    }
}
