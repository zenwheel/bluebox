import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// DisplayLib's remaining names on the one display.
    @Suite(.serialized) struct OS9LeftDisplay {
        init() { Rest.boot() }

        let notFound: Int32 = -6229

        var main: UInt32 { rd32(UInt32(LM_MainDevice)) }

        @Test func displayIDAndFlags() {
            #expect(Rest.signed("DMSetDisplayID", [main, 7]) == 0)
            let id = Rest.ptr(4)
            #expect(tb_DMGetDisplayIDByGDevice(main, id, 0) == 0 && rd32(id) == 7)
            let gd = Rest.ptr(4)
            #expect(tb_DMGetGDeviceByDisplayID(7, gd, 0) == 0 && rd32(gd) == main)
            #expect(tb_DMGetGDeviceByDisplayID(1, gd, 0) == Int16(notFound))
            #expect(Rest.signed("DMSetDisplayID", [0x1234, 1]) == notFound)
            #expect(Rest.signed("DMSetDisplayID", [main, 0]) == -50)
            #expect(Rest.signed("DMSetDisplayID", [main, 1]) == 0)

            #expect(Rest.call("DMTestAuxGDeviceFlag", [main, 3]) == 0)
            #expect(Rest.signed("DMSetAuxGDeviceFlag", [main, 3, 1]) == 0)
            #expect(Rest.call("DMTestAuxGDeviceFlag", [main, 3]) == 1)
            #expect(Rest.call("DMTestAuxGDeviceFlag", [0, 3]) == 1) // nil is the main device
            #expect(Rest.call("DMTestAuxGDeviceFlag", [main, 4]) == 0)
            #expect(Rest.signed("DMSetAuxGDeviceFlag", [main, 3, 0]) == 0)
            #expect(Rest.call("DMTestAuxGDeviceFlag", [main, 3]) == 0)
            #expect(Rest.signed("DMSetAuxGDeviceFlag", [main, 40, 1]) == -50)
            #expect(Rest.signed("DMSetAuxGDeviceFlag", [0x1234, 1, 1]) == notFound)
        }

        @Test func bestDepth() {
            let mode = Rest.ptr(4), depth = Rest.ptr(2)
            #expect(Rest.signed("DMGetBestDepthModeInfo", [main, 1, mode, depth]) == 0)
            #expect(rd16(depth) == 8 && rd32(mode) == 0x83)
            #expect(Rest.signed("DMGetBestDepthModeInfo", [main, 9, mode, depth]) == 0)
            #expect(rd16(depth) == 16 && rd32(mode) == 0x84)
            #expect(Rest.signed("DMGetBestDepthModeInfo", [main, 24, mode, depth]) == 0)
            #expect(rd16(depth) == 32 && rd32(mode) == 0x85)
            #expect(Rest.signed("DMGetBestDepthModeInfo", [main, 64, mode, depth]) == 0)
            #expect(rd16(depth) == 32)
            #expect(Rest.signed("DMGetBestDepthModeInfo", [0x1234, 8, mode, depth]) == notFound)
        }

        @Test func multiModeTable() {
            let t = Rest.call("DMGetROMMultiModeTable")
            #expect(t != 0 && t == Rest.call("DMGetROMMultiModeTable"))
            let n = rd32(t)
            #expect(n >= 1)
            #expect(rd16(t + 4) == 640 && rd16(t + 6) == 480 && rd32(t + 8) == 0x38)
        }

        @Test func startingUp() {
            let flags = Rest.ptr(4)
            #expect(Rest.signed("DMGetInitializationFlags", [flags]) == 0 && rd32(flags) == 0)
            #expect(Rest.signed("DMInitDisplayManager") == 0)
            #expect(Rest.signed("DMInstallDisplayManager") == 0)
            #expect(Rest.signed("DMInstallDisplays") == 0)
            #expect(Rest.signed("DMDoToolboxPatch") == 0)
            #expect(Rest.signed("DMFinalInit") == 0)
            #expect(Rest.signed("DMGetInitializationFlags", [flags]) == 0 && rd32(flags) == 31)
            #expect(Rest.signed("DMGetInitializationFlags", [0]) == -50)
            #expect(Rest.signed("DMConfigureHardware", [0]) == 0)
            let state = Rest.ptr(4)
            #expect(Rest.signed("DMPrivateBeginConfigureDisplays", [state]) == 0)
            #expect(tb_DMEndConfigureDisplays(rd32(state)) == 0)
        }

        @Test func deskIconRegions() {
            let ref = Rest.ptr(4), out = Rest.ptr(4)
            #expect(Rest.signed("DMNewDeskIconRegionRef", [ref]) == 0)
            let r = rd32(ref)
            let rgn = tb_NewRgn()
            tb_SetRectRgn(rgn, 10, 20, 30, 40)
            #expect(Rest.signed("DMChangedDesktopIconRgn", [r, rgn]) == 0)
            #expect(Rest.signed("DMGetDeskIconRegionByRef", [r, out]) == 0)
            let copy = rd32(out)
            #expect(copy != rgn && tb_EqualRgn(copy, rgn) != 0)
            #expect(Rest.signed("DMDisposeDeskIconRegionRef", [r]) == 0)
            #expect(Rest.signed("DMGetDeskIconRegionByRef", [r, out]) == notFound)
            #expect(Rest.signed("DMDisposeDeskIconRegionRef", [r]) == notFound)
        }

        @Test func delayedUpdatesCallTheDrawProc() {
            let seen = Rest.ptr(8)
            // stw r3,0(seen); blr
            let proc = Rest.routine(Rest.stash(seen) + [0x906B_0000, 0x4E80_0020])
            #expect(Rest.signed("DMSetDeskDrawProc", [proc]) == 0)
            #expect(Rest.signed("DMDoDelayedScreenUpdates") == 0)
            #expect(rd32(seen) == 0) // nothing changed
            let ref = Rest.ptr(4)
            #expect(Rest.signed("DMNewDeskIconRegionRef", [ref]) == 0)
            #expect(Rest.signed("DMChangedDesktopIconRgn", [rd32(ref), 0]) == 0)
            #expect(Rest.signed("DMDoDelayedScreenUpdates") == 0)
            #expect(rd32(seen) == rd32(UInt32(LM_GrayRgn)))
            wr32(seen, 0)
            #expect(Rest.signed("DMDoDelayedScreenUpdates") == 0 && rd32(seen) == 0) // once
        }

        @Test func deskClick() {
            _ = initToolbox()
            let gray = rd32(UInt32(LM_GrayRgn))
            let b = rd32(gray)
            let top = Int16(bitPattern: rd16(b + 2)), left = Int16(bitPattern: rd16(b + 4))
            let inside = UInt32(UInt16(bitPattern: top + 100)) << 16 | UInt32(UInt16(bitPattern: left + 100))
            #expect(Rest.call("DMDeskClick", [inside, 0]) == 1)
            #expect(Rest.call("DMDeskClick", [0x7FFF_7FFF, 0]) == 0)
        }

        @Test func movingTheCursor() {
            #expect(Rest.signed("DMMoveCursor", [120, 80]) == 0)
            #expect(rd32(UInt32(LM_MouseLocation)) == 80 << 16 | 120)
            #expect(rd8(0x08CE) == 1)
        }

        @Test func notificationsAreCalled() {
            let seen = Rest.ptr(16)
            // stw r3,0(seen); stw r4,4(seen); stw r5,8(seen); blr
            let proc = Rest.routine(Rest.stash(seen) + [0x906B_0000, 0x908B_0004, 0x90AB_0008, 0x4E80_0020])
            #expect(tb_DMRegisterExtendedNotifyProc(proc, 0x55, 0, 0) == 0)
            #expect(Rest.signed("DMNotifyAllProcesses", [2, 0x66]) == 0)
            #expect(rd32(seen) == 0x55 && rd32(seen + 4) == 2 && rd32(seen + 8) == 0x66)
            #expect(Rest.signed("DMRemoveCurrentProcessProcs") == 0)
            wr32(seen, 0)
            #expect(Rest.signed("DMNotifyAllProcesses", [2, 0x66]) == 0 && rd32(seen) == 0)
        }

        @Test func baseComponents() {
            let comp = Rest.ptr(4)
            #expect(Rest.signed("DMNewDisplayBaseComponent", [1, comp]) == 0)
            let c = rd32(comp)
            #expect(c != 0)
            #expect(Rest.signed("DMNewDisplayBaseComponent", [99, comp]) == notFound)
            #expect(Rest.signed("DMDisposeDisplayBaseComponent", [c]) == 0)
            #expect(Rest.signed("DMDisposeDisplayBaseComponent", [c]) == notFound)
        }

        @Test func profiles() {
            let out = Rest.ptr(4)
            #expect(Rest.signed("DMGetProfileByAVID", [1, out]) == notFound)
            #expect(Rest.signed("DMUpdateProfileCache", [1]) == notFound)
            let profile = tb_NewHandle(128, false, true)
            let seen = Rest.ptr(4)
            let proc = Rest.routine(Rest.stash(seen) + [0x90AB_0000, 0x4E80_0020]) // stw r5,0(r11): the notification's data
            #expect(tb_DMRegisterExtendedNotifyProc(proc, 0, 0, 0) == 0)
            #expect(Rest.signed("DMSetProfileByAVID", [1, profile]) == 0)
            #expect(Rest.signed("DMGetProfileByAVID", [1, out]) == 0 && rd32(out) == profile)
            #expect(Rest.signed("DMUpdateProfileCache", [1]) == 0)
            #expect(Rest.signed("DMSetProfileByAVID", [9, profile]) == notFound)
            #expect(Rest.signed("DMGetProfileByAVID", [1, 0]) == -50)
        }

        @Test func moveWindowOnscreen() {
            _ = initToolbox()
            let r = Rest.ptr(8)
            wr16(r, 100); wr16(r + 2, 100); wr16(r + 4, 300); wr16(r + 6, 300)
            let title = Rest.ptr(8); wr8(title, 1); wr8(title + 1, 0x54)
            let w = tb_NewWindow(0, r, title, 1, 0, 0xFFFF_FFFF, 1, 0)
            #expect(w != 0)
            let before = rd32(rd32(w + 114)) + 2
            let top0 = Int16(bitPattern: rd16(before)), left0 = Int16(bitPattern: rd16(before + 2))
            #expect(Rest.signed("DMKeepWindowOnscreen", [w]) == 0) // already inside: stays
            let s1 = rd32(rd32(w + 114)) + 2
            #expect(Int16(bitPattern: rd16(s1)) == top0 && Int16(bitPattern: rd16(s1 + 2)) == left0)
            tb_MoveWindow(w, 4000, 3000, 0)
            #expect(Rest.signed("DMKeepWindowOnscreen", [w]) == 0)
            let gray = rd32(rd32(UInt32(LM_GrayRgn))) + 2
            let s2 = rd32(rd32(w + 114)) + 2
            #expect(Int16(bitPattern: rd16(s2 + 6)) <= Int16(bitPattern: rd16(gray + 6)))
            #expect(Int16(bitPattern: rd16(s2 + 4)) <= Int16(bitPattern: rd16(gray + 4)))
            #expect(Rest.signed("DMKeepWindowOnscreen", [0]) == -50)
        }
    }
}
