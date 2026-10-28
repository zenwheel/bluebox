import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// SoundLib's and FontManager's remaining names that Blue Box can back.
    @Suite(.serialized) struct OS9LeftSoundFont {
        init() { Rest.boot() }

        func pstr(_ s: String) -> UInt32 {
            let p = Rest.ptr(256)
            guestPString(s, at: p)
            return p
        }

        @Test func defaultInputDevice() {
            let name = Rest.ptr(256)
            #expect(Rest.signed("SPBGetDefaultDevice", [name]) == 0)
            #expect(pstring(at: name) == "Built-in")
            #expect(Rest.signed("SPBSetDefaultDevice", [name]) == 0)
            #expect(Rest.signed("SPBSetDefaultDevice", [pstr("Nothing")]) == -228)
            #expect(Rest.signed("SPBSetDefaultDevice", [pstr("Built-in")]) == 0)
            #expect(Rest.signed("SPBGetDefaultDevice", [0]) == -50)
        }

        @Test func outputDeviceAndChain() {
            let inst = Rest.ptr(4)
            wr32(inst, 7)
            // no sound output device component is registered
            #expect(Rest.signed("OpenSoundOutputDevice", [0, inst]) == -3000 && rd32(inst) == 0)
            #expect(Rest.signed("OpenSoundOutputDevice", [0, 0]) == -50)
            #expect(Rest.signed("CloseSoundOutputDevice", [0]) == -50)
            // a chain of one is itself; of two needs real components
            let list = Rest.ptr(8), last = Rest.ptr(4)
            wr32(list, 0x1111); wr32(list + 4, 0x2222)
            #expect(Rest.signed("MakeSoundComponentChain", [list, 1, last]) == 0 && rd32(last) == 0x1111)
            #expect(Rest.signed("MakeSoundComponentChain", [list, 2, last]) != 0) // not components
            #expect(Rest.signed("MakeSoundComponentChain", [0, 1, last]) == -50)
            #expect(Rest.signed("MakeSoundComponentChain", [list, 0, last]) == -50)
        }

        @Test func dispatchTools() {
            let proc = Rest.routine([0x4E80_0020])
            #expect(Rest.call("SndDispatchToolAddress", [0x120]) == 0)
            #expect(Rest.signed("SndDispatchAddTool", [0x120, proc]) == 0)
            #expect(Rest.call("SndDispatchToolAddress", [0x120]) == proc)
            #expect(Rest.signed("SndDispatchAddTool", [0x120, proc]) == -50) // taken
            #expect(Rest.signed("SndDispatchAddTool", [0x121, 0]) == -50)
            #expect(Rest.signed("SndDispatchRemoveTool", [0x120]) == 0)
            #expect(Rest.call("SndDispatchToolAddress", [0x120]) == 0)
            #expect(Rest.signed("SndDispatchRemoveTool", [0x120]) == -50)
        }

        @Test func disposingTheGlobalsDisposesChannels() {
            let chan = Rest.ptr(4)
            wr32(chan, 0)
            #expect(tb_SndNewChannel(chan, 5, 0, 0) == 0)
            #expect(rd32(chan) != 0)
            #expect(Rest.signed("DisposeSoundGlobals") == 0)
            // the channel is gone: the Sound Manager says so
            #expect(tb_SndDoImmediate(rd32(chan), Rest.ptr(8)) == -205)
        }

        @Test func fontObjectsAndTheRest() {
            _ = initToolbox()
            #expect(Rest.call("FontRenumberingAllowed") == 1)
            #expect(Rest.call("QDFontFromFontObject", [0]) == 0)
            let font = Rest.ptr(4)
            wr32(font, 99)
            #expect(Rest.signed("FontObjectForQDFontRequest", [30000, 0, font]) != 0 && rd32(font) == 0)
            #expect(Rest.signed("FontObjectForQDFontRequest", [3, 0, 0]) == -50)
            // a family there is: round trip, if the system has one
            for family: UInt32 in [0, 3, 4, 20] {
                if Rest.signed("FontObjectForQDFontRequest", [family, 0, font]) == 0 {
                    #expect(Rest.call("QDFontFromFontObject", [rd32(font)]) == family)
                }
            }
            #expect(Rest.signed("NotifyFontManager", [1, 0]) == 0)
            #expect(Rest.signed("QDTextINIT") == 0)
            #expect(Rest.call("__LowestPPEM", [3, 12]) >= 1)
        }

        @Test func imageBoundingBox() {
            _ = initToolbox()
            tb_TextFont(0)
            tb_TextSize(12)
            let text = Rest.cstr("Hello")
            let box = Rest.ptr(8), info = Rest.ptr(8)
            #expect(Rest.signed("QDGetImageBoundingBox", [text, 5, box]) == 0)
            tb_GetFontInfo(info)
            let ascent = Int16(bitPattern: rd16(info)), descent = Int16(bitPattern: rd16(info + 2))
            #expect(Int16(bitPattern: rd16(box)) == -ascent && Int16(bitPattern: rd16(box + 4)) == descent)
            #expect(rd16(box + 2) == 0 && rd16(box + 6) == UInt16(bitPattern: tb_TextWidth(text, 0, 5)))
            #expect(Rest.signed("QDGetImageBoundingBoxWithWidth", [text, 5, 99, box]) == 0)
            #expect(rd16(box + 6) == 99)
            #expect(Rest.signed("QDGetImageBoundingBox", [text, 5, 0]) == -50)
            // the Font Manager's scratch storage: a pointer to a block
            let cell = ppc_data_export("gQDTextRenderingScratchStorage")
            #expect(cell != 0 && rd32(cell) != 0 && ppc_data_export("gQDTextRenderingScratchStorage") == cell)
            #expect(ppc_data_export_known("gQDTextRenderingScratchStorage"))
        }
    }
}
