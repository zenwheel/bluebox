import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// NavigationLib's previews, SpeechLib's registered voices and defaults,
    /// ControlStripLib, and the calls that are other calls' work.
    @Suite(.serialized) struct OS9LeftMisc {
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

        /// A version 1 'PICT' of a 10 by 10 frame and nothing in it.
        func pict() -> UInt32 {
            let bytes: [UInt8] = [0, 0, 0, 0, 0, 0, 0, 10, 0, 10, 0x11, 0x01, 0xFF]
            let p = tb_NewPtr(Int32(bytes.count), false, true)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func file(_ name: String) -> UInt32 {
            let spec = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname(name), spec)
            tb_FSpCreateResFile(spec, Int32(bitPattern: os("Prev")), Int32(bitPattern: os("TEXT")), 0)
            return spec
        }

        /// An AEDesc of type 'fss ' for a spec.
        func desc(_ spec: UInt32) -> UInt32 {
            let d = tb_NewPtr(8, false, true)
            #expect(tb_AECreateDesc(Int32(bitPattern: os("fss ")), spec, 70, d) == 0)
            return d
        }

        @Test func aPreviewIsStoredAndFound() {
            let spec = file("Previewed")
            let reference = Rest.ptr(4)
            #expect(Rest.signed("NavCreatePreviewReference", [spec, reference]) == 0)
            #expect(Rest.call("NavHasPreview", [rd32(reference)]) == 0)
            #expect(Rest.signed("NavDisposePreviewReference", [rd32(reference)]) == 0)

            #expect(tb_NavCreatePreview(desc(spec), Int32(bitPattern: os("PICT")), pict(), 13) == 0)
            #expect(Rest.signed("NavCreatePreviewReference", [spec, reference]) == 0)
            let ref = rd32(reference)
            #expect(Rest.call("NavHasPreview", [ref]) == 1)
            // a second preview replaces the first (one 'PICT' and one 'pnot' remain)
            #expect(tb_NavCreatePreview(desc(spec), Int32(bitPattern: os("PICT")), pict(), 13) == 0)
            let rn = tb_FSpOpenResFile(spec, 1)
            #expect(tb_Count1Resources(Int32(bitPattern: os("PICT"))) == 1)
            #expect(tb_Count1Resources(Int32(bitPattern: os("pnot"))) == 1)
            tb_CloseResFile(rn)

            #expect(Rest.signed("NavDisposePreviewReference", [ref]) == 0)
            #expect(Rest.signed("NavDisposePreviewReference", [ref]) == -50)
            #expect(Rest.call("NavHasPreview", [ref]) == 0)
        }

        @Test func previewErrors() {
            let spec = file("Errors")
            #expect(tb_NavCreatePreview(0, Int32(bitPattern: os("PICT")), pict(), 13) == -50)
            #expect(tb_NavCreatePreview(desc(spec), Int32(bitPattern: os("TEXT")), pict(), 13) == -50)
            #expect(tb_NavCreatePreview(desc(spec), Int32(bitPattern: os("PICT")), 0, 13) == -50)
            let missing = tb_NewPtr(70, false, true)
            _ = tb_FSMakeFSSpec(0, 0, pname("Not There"), missing)
            let reference = Rest.ptr(4)
            #expect(Rest.signed("NavCreatePreviewReference", [missing, reference]) != 0)
            #expect(Rest.signed("NavCreatePreviewReference", [0, reference]) == -50)
        }

        @Test func areaAndEvents() {
            let spec = file("Area")
            #expect(tb_NavCreatePreview(desc(spec), Int32(bitPattern: os("PICT")), pict(), 13) == 0)
            let reference = Rest.ptr(4)
            #expect(Rest.signed("NavCreatePreviewReference", [spec, reference]) == 0)
            let ref = rd32(reference)
            let rect = Rest.ptr(8), back = Rest.ptr(8)
            for (i, v) in [10, 20, 90, 120].enumerated() { wr16(rect + UInt32(i * 2), UInt16(v)) }
            let ev = Rest.ptr(16)
            wr16(ev, 6) // updateEvt
            #expect(Rest.call("NavHandlePreviewEvent", [ref, ev]) == 0) // not set up yet
            #expect(Rest.signed("NavSetPreviewArea", [ref, rect]) == 0)
            #expect(Rest.signed("NavGetPreviewArea", [ref, back]) == 0)
            #expect((0..<4).map { rd16(back + UInt32($0 * 2)) } == [10, 20, 90, 120])
            #expect(Rest.signed("NavSetupPreview", [ref, rect]) == 0)
            _ = initToolbox()
            #expect(Rest.call("NavHandlePreviewEvent", [ref, ev]) == 1)
            #expect(Rest.signed("NavDrawPreview", [ref]) == 0)
            wr16(ev, 1)                      // mouseDown
            wr16(ev + 10, 50); wr16(ev + 12, 60) // where: v, h
            #expect(Rest.call("NavHandlePreviewEvent", [ref, ev]) == 1)
            wr16(ev + 10, 500)
            #expect(Rest.call("NavHandlePreviewEvent", [ref, ev]) == 0)
            wr16(ev, 8) // activateEvt
            #expect(Rest.call("NavHandlePreviewEvent", [ref, ev]) == 0)
            #expect(Rest.signed("NavSetPreviewArea", [0x1234, rect]) == -50)
            #expect(Rest.signed("NavDrawPreview", [0x1234]) == -50)
        }

        @Test func voicesAreRegisteredAndListed() {
            let n = Rest.ptr(2), voice = Rest.ptr(8)
            #expect(tb_CountVoices(n) == 0)
            let before = rd16(n)
            let specs = Rest.ptr(16)
            wr32(specs, os("test")); wr32(specs + 4, 7)
            wr32(specs + 8, os("test")); wr32(specs + 12, 8)
            #expect(Rest.signed("RegisterVoices", [specs, 2]) == 0)
            #expect(Rest.signed("RegisterVoices", [specs, 2]) == 0) // again: no more of them
            #expect(tb_CountVoices(n) == 0 && rd16(n) == before + 2)
            #expect(tb_GetIndVoice(Int16(before + 2), voice) == 0)
            #expect(rd32(voice) == os("test") && rd32(voice + 4) == 8)
            let info = Rest.ptr(362)
            wr32(info, 362)
            #expect(tb_GetVoiceDescription(voice, info, 362) == 0)
            #expect(pstring(at: info + 16) == "Voice 8")
            // a channel can be opened with it, and is on the list
            let chan = Rest.ptr(4)
            #expect(tb_NewSpeechChannel(voice, chan) == 0)
            #expect(Rest.signed("AddSpeechChannelToList", [rd32(chan)]) == 0)
            #expect(Rest.signed("AddSpeechChannelToList", [0x1234]) == -50)
            #expect(tb_DisposeSpeechChannel(rd32(chan)) == 0)
            #expect(Rest.signed("UnRegisterVoices", [specs, 2]) == 0)
            #expect(tb_CountVoices(n) == 0 && rd16(n) == before)
            #expect(Rest.signed("UnRegisterVoices", [specs, 1]) == -244)
            #expect(Rest.signed("RegisterVoices", [0, 2]) == -50)
        }

        @Test func defaultsApplyToNewChannels() {
            let rec = Rest.ptr(20)
            #expect(Rest.signed("GetSpeechDefaults", [rec]) == 0)
            #expect(rd32(rec) == os("gala") && rd32(rec + 8) == 180 << 16)
            wr32(rec + 8, 120 << 16); wr32(rec + 12, 60 << 16); wr32(rec + 16, 0x8000)
            #expect(Rest.signed("SetSpeechDefaults", [rec]) == 0)
            let back = Rest.ptr(20)
            #expect(Rest.signed("GetSpeechDefaults", [back]) == 0 && rd32(back + 8) == 120 << 16)
            let chan = Rest.ptr(4), rate = Rest.ptr(4)
            #expect(tb_NewSpeechChannel(0, chan) == 0)
            #expect(tb_GetSpeechRate(rd32(chan), rate) == 0 && rd32(rate) == 120 << 16)
            #expect(tb_GetSpeechPitch(rd32(chan), rate) == 0 && rd32(rate) == 60 << 16)
            #expect(Rest.signed("GetSpeechDefaults", [0]) == -50)
        }

        @Test func controlStrip() {
            #expect(Rest.call("SBGetPopupMenuCheckMark") == 0x12)
            #expect(Rest.signed("SBControlStripSetup", [0]) == -4)
            // the proc is called with its argument, here: r3 := r3 + 1
            let proc = Rest.routine([0x3863_0001, 0x4E80_0020])
            #expect(Rest.call("SBCallProcInFrontProcess", [proc, 41]) == 42)
            #expect(Rest.signed("SBCallProcInFrontProcess", [0, 41]) == -50)
        }

        @Test func callsThatAreOthersWork() {
            // FindFolderInternal is FindFolder
            let vref = Rest.ptr(2), dir = Rest.ptr(4), vref2 = Rest.ptr(2), dir2 = Rest.ptr(4)
            let e1 = Rest.signed("FindFolderInternal", [0xFFFF_8000, os("pref"), 1, vref, dir])
            let e2 = Rest.signed("FindFolder", [0xFFFF_8000, os("pref"), 1, vref2, dir2])
            #expect(e1 == e2 && e1 == 0)
            #expect(rd32(dir) == rd32(dir2) && rd16(vref) == rd16(vref2))
            // RegistryDeviceSearch and PopUpMenuSelectSticky are present
            #expect(cfm_export_fn("RegistryDeviceSearch") != nil && cfm_export_fn("PopUpMenuSelectSticky") != nil)
        }

        @Test func interruptMembersAreLookedUp() {
            let set = Rest.ptr(4), refcon = Rest.ptr(4)
            _ = refcon
            #expect(Rest.signed("CreateInterruptSet", [0, 0, 3, set, 0]) == 0)
            #expect(Rest.signed("InstallInterruptFunctions", [rd32(set), 1, 0x11, 0x21, 0x31, 0x41]) == 0)
            #expect(Rest.signed("InstallInterruptFunctions", [rd32(set), 3, 0x13, 0x23, 0x33, 0x43]) == 0)
            let out = Rest.ptr(48)
            #expect(Rest.signed("LookupInterruptMembers", [rd32(set), 1, 3, out]) == 0)
            #expect(rd32(out) == 0x11 && rd32(out + 4) == 0x21 && rd32(out + 12) == 0x41)
            #expect(rd32(out + 16) == 0 && rd32(out + 32) == 0x13 && rd32(out + 44) == 0x43)
            #expect(Rest.signed("LookupInterruptMembers", [rd32(set), 2, 3, out]) == -50)
            #expect(Rest.signed("LookupInterruptMembers", [0x77, 1, 1, out]) == -50)
        }
    }
}
