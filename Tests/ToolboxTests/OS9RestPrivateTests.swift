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

        @Test func virtualMemoryAnswersAsAMacWithoutIt() {
            launchTestApp()
            var ram: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("ram ")), &ram) == 0 && ram > 0)
            #expect(Rest.call("VM_GetPhysicalBytes") == ram && Rest.call("VM_GetLogicalBytes") == ram)
            #expect(Rest.signed("VM_HoldSystemHeap") == 0 && Rest.signed("VM_ReleaseMemory", [0x1000, 64]) == 0)
            #expect(Rest.signed("VM_IterateMappedRanges", [0]) == 0)
            Rest.call("VM_FinishInstall")
            Rest.call("VM_SetVMJCrsrTask", [0])
            for n in ["WriteProtectMemory", "UnWriteProtectMemory", "VMWriteProtectMemory", "VMUnWriteProtectMemory",
                      "MapFile", "UnMapFile", "VM_GrowFileMapping"] {
                #expect(Rest.signed(n, [0x1000, 64]) == -4, "\(n)")
            }
        }

        @Test func soundChannelsGoWithTheirProgram() {
            launchTestApp()
            let chanp = Rest.ptr(4)
            #expect(tb_SndNewChannel(chanp, 5, 0, 0) == 0)
            let chan = rd32(chanp)
            let status = Rest.ptr(24)
            #expect(tb_SndChannelStatus(chan, 24, status) == 0)
            Rest.call("SndAppDead")
            #expect(tb_SndChannelStatus(chan, 24, status) != 0) // no such channel now
            Rest.call("SynchIdleTime")
        }

        @Test func batteryInfoAndScreenBits() {
            launchTestApp()
            #expect(Rest.call("PMScaledBattery", [0]) == 0 && Rest.call("PMScaledBattery", [1]) == 0) // no battery
            func same(_ a: String, _ b: String) -> Bool {
                guard let x = cfm_export_fn(a), let y = cfm_export_fn(b) else { return false }
                return unsafeBitCast(x, to: Int.self) == unsafeBitCast(y, to: Int.self)
            }
            #expect(same("SaveBits", "SaveScreenBits") && same("RestoreBits", "RestoreScreenBits"))
            #expect(same("DiscardBits", "DisposeSavedScreenBits"))
        }

        @Test func speechChannelsGoWithTheirProgram() {
            launchTestApp()
            let chan = Rest.ptr(4)
            #expect(tb_NewSpeechChannel(0, chan) == 0)
            #expect(tb_GetSpeechRate(rd32(chan), Rest.ptr(4)) == 0)
            #expect(Rest.signed("KillIdlePrivateChannels", [1]) == 0)
            #expect(tb_GetSpeechRate(rd32(chan), Rest.ptr(4)) == 0) // not a private one
            #expect(Rest.signed("KillProcessChannels") == 0)
            #expect(tb_GetSpeechRate(rd32(chan), Rest.ptr(4)) != 0)
            #expect(tb_NewSpeechChannel(0, chan) == 0)
            #expect(Rest.signed("ShutDownSpeechManager") == 0)
            #expect(tb_GetSpeechRate(rd32(chan), Rest.ptr(4)) != 0)
        }

        @Test func fontManagersScalerAndCaches() {
            let inp = Rest.ptr(64), out = Rest.ptr(64)
            for i in 0..<16 { wr32(inp + UInt32(4 * i), 0xFFFF_FFFF) }
            for name in ["fs_OpenFonts", "fs__OpenFonts"] {
                for i in 0..<16 { wr32(inp + UInt32(4 * i), 0xFFFF_FFFF); wr32(out + UInt32(4 * i), 0xFFFF_FFFF) }
                #expect(Rest.call(name, [inp, out]) == 0)
                #expect((0..<9).allSatisfy { rd32(inp + UInt32(4 * $0)) == 0 } && rd32(inp + 36) == 0xFFFF_FFFF)
                #expect([rd32(out), rd32(out + 4), rd32(out + 8), rd32(out + 12)] == [0x33C, 0x404, 0x834, 0])
                #expect(rd32(out + 28) == 0 && rd32(out + 32) == 0xFFFF_FFFF)
            }
            #expect(Rest.call("fs_CloseFonts", [inp, out]) == 0 && Rest.call("fs__CloseFonts", [inp, out]) == 0)
            for n in ["Initialize", "NewSfnt", "NewTransformation", "NewGlyph", "ContourGridFit", "ContourNoGridFit",
                      "ContourScan", "GetAdvanceWidth", "SizeOfOutlines", "SaveOutlines", "RestoreOutlines"] {
                #expect(Rest.signed("fs_" + n, [inp, out]) == -4 && Rest.signed("fs__" + n, [inp, out]) == -4, "\(n)")
            }
            #expect(Rest.signed("fs_FindBitMapSize", [inp, out]) == -4)
            for n in ["__FillWidthTab", "__InitializeDiskCache", "__InitializePartialFonts", "__KillSomeCaches",
                      "__PreFlightFontMem", "__RetrieveGlyph", "__SearchForCache", "sb__FillWidthTab",
                      "sb__KillSomeCaches", "sb__PreFlightFontMem", "sb__RetrieveGlyph", "sb__SearchForCache",
                      "InitializeDiskCache", "InitializePartialFonts", "CleanUpGlyphRecordArray"] {
                #expect(Rest.call(n, [1, 2, 3]) == 0, "\(n)")
            }
            #expect(Rest.call("__SFNTMatchSize", [1, 12]) == 1 && Rest.call("sb__SFNTMatchSize", [1, 12]) == 1)
        }
    }
}
