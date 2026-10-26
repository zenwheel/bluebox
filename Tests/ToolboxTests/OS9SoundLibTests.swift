import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

nonisolated(unsafe) var sndSeen: [(what: Int16, p: [UInt32])] = []

/// A mixer component: records each call's selector and the first parameters,
/// and answers with the selector.
let sndMixer: native_fn = { c, _ in
    guard let c else { return }
    let ret = cpu_pop32(c)
    _ = cpu_pop32(c)
    let params = cpu_pop32(c)
    let what = Int16(bitPattern: rd16(params + 2))
    if what >= 0 {
        sndSeen.append((what, [rd32(params + 4), rd32(params + 8), rd32(params + 12)]))
    }
    wr32(c.pointee.a.7, what >= 0 ? UInt32(bitPattern: Int32(what)) : 0)
    cpu_push32(c, ret)
}

extension BlueBox {
    /// SoundLib's calls beyond the channels: component calls, compression
    /// names, the mixer, AIFF headers and the Sound Converter's settings.
    @Suite struct OS9SoundLib {
        init() {
            bootMachine()
            sndSeen = []
        }

        func pname() -> UInt32 { tb_NewPtr(256, false, true) }

        @Test func compressionNames() {
            let p = pname()
            #expect(ilCall16("GetCompressionName", [fourCC("MAC3"), p]) == 0 && pstring(at: p) == "MACE 3:1")
            #expect(ilCall16("GetCompressionName", [fourCC("ima4"), p]) == 0 && pstring(at: p) == "IMA 4:1")
            #expect(ilCall16("GetCompressionName", [fourCC("zzzz"), p]) == -3000)
        }

        @Test func mixerAndSoundComponentCalls() {
            guestStack()
            let mixer = ilCall("OpenMixerSoundComponent", [0, 0, tb_NewPtr(4, false, true)])
            #expect(Int16(truncatingIfNeeded: mixer) == -3000) // none registered
            let entry = native_register(sndMixer, "Mixer", 0)
            let cd = tb_NewPtr(20, false, true)
            wr32(cd, fourCC("mixr"))
            #expect(ilCall("RegisterComponent", [cd, entry, 0, 0, 0, 0]) != 0)
            let out = tb_NewPtr(4, false, true)
            #expect(ilCall16("OpenMixerSoundComponent", [0, 0, out]) == 0 && rd32(out) != 0)
            let ci = rd32(out)

            #expect(ilCall("SoundComponentSetSource", [ci, 7, 0x1234]) == 0x0002)
            #expect(sndSeen.last?.what == 2 && sndSeen.last?.p[0] == 0x1234 && sndSeen.last?.p[1] == 7)
            #expect(ilCall("SoundComponentGetInfo", [ci, 5, fourCC("srat"), 0x99]) == 0x0103)
            #expect(sndSeen.last?.p == [0x99, fourCC("srat"), 5])
            #expect(ilCall("SoundComponentPlaySourceBuffer", [ci, 1, 2, 3]) == 0x0108)
            #expect(ilCall("SoundComponentStartSource", [ci, 2, 0x4000]) == 0x0105)
            #expect(ilCall("AudioGetVolume", [ci, 1, 0x500]) == 0x0000)
            #expect(sndSeen.last?.what == 0)
            #expect(ilCall("AudioSetMute", [ci, 0, 1]) == 0x0003)
            #expect(ilCall("AudioSetToDefaults", [ci]) == 0x0004)
            #expect(ilCall("AudioMuteOnEvent", [ci, 1]) == 0x0081)
            #expect(sndSeen.count == 8)

            #expect(ilCall16("CloseMixerSoundComponent", [ci]) == 0)
        }

        @Test func defaultOutput() {
            let p = tb_NewPtr(4, false, true)
            #expect(ilCall16("GetDefaultSoundOutput", [p]) == -3000)
            #expect(ilCall16("SetDefaultSoundOutput", [0x1234]) == 0)
            #expect(ilCall16("GetDefaultSoundOutput", [p]) == 0 && rd32(p) == 0x1234)
        }

        @Test func aiffHeader() throws {
            let dir = launchTestApp()
            let path = dir.appendingPathComponent("tone.aiff").path
            let samples = [UInt8](repeating: 0x11, count: 40)
            let comm = cat(be16(2), be32(10), be16(16), [0x40, 0x0D, 0xAC, 0x44, 0, 0, 0, 0, 0, 0])
            let ssnd = cat(be32(0), be32(0), samples)
            let file = cat(Array("FORM".utf8), be32(UInt32(4 + 8 + comm.count + 8 + ssnd.count)), Array("AIFF".utf8),
                           Array("COMM".utf8), be32(UInt32(comm.count)), comm,
                           Array("SSND".utf8), be32(UInt32(ssnd.count)), ssnd)
            try Data(file).write(to: URL(fileURLWithPath: path))
            let spec = tb_NewPtr(80, false, true), ref = tb_NewPtr(2, false, true)
            fm_spec_for_path(path, spec)
            #expect(tb_FSpOpenDF(spec, 1, ref) == 0)
            let info = tb_NewPtr(28, false, true), frames = tb_NewPtr(4, false, true), off = tb_NewPtr(4, false, true)
            #expect(ilCall16("ParseAIFFHeader", [UInt32(rd16(ref)), info, frames, off]) == 0)
            #expect(rd32(info + 4) == fourCC("twos") && rd16(info + 8) == 2 && rd16(info + 10) == 16)
            #expect(rd32(info + 12) == 22050 << 16)
            #expect(rd32(frames) == 10 && rd32(info + 16) == 10)
            #expect(rd32(off) == 12 + 8 + UInt32(comm.count) + 8 + 8) // FORM header, COMM, SSND header and offset fields
            // Not an AIFF file
            let other = dir.appendingPathComponent("other").path
            try Data("not a sound".utf8).write(to: URL(fileURLWithPath: other))
            let spec2 = tb_NewPtr(80, false, true), ref2 = tb_NewPtr(2, false, true)
            fm_spec_for_path(other, spec2)
            #expect(tb_FSpOpenDF(spec2, 1, ref2) == 0)
            #expect(ilCall16("ParseAIFFHeader", [UInt32(rd16(ref2)), info, frames, off]) == -206)
        }

        @Test func converterSettings() {
            launchTestApp()
            let inf = tb_NewPtr(28, false, true), outf = tb_NewPtr(28, false, true)
            wr32(inf + 4, fourCC("twos")); wr16(inf + 8, 1); wr16(inf + 10, 16); wr32(inf + 12, 11025 << 16)
            wr32(outf + 4, fourCC("twos")); wr16(outf + 8, 2); wr16(outf + 10, 16); wr32(outf + 12, 22050 << 16)
            let sc = tb_NewPtr(4, false, true)
            #expect(ilCall16("SoundConverterOpen", [inf, outf, sc]) == 0)
            let v = tb_NewPtr(4, false, true)
            #expect(ilCall16("SoundConverterGetInfo", [rd32(sc), fourCC("srat"), v]) == 0 && rd32(v) == 22050 << 16)
            #expect(ilCall16("SoundConverterGetInfo", [rd32(sc), fourCC("chan"), v]) == 0 && rd16(v) == 2)
            #expect(ilCall16("SoundConverterGetInfo", [rd32(sc), fourCC("ssiz"), v]) == 0 && rd16(v) == 16)
            #expect(ilCall16("SoundConverterGetInfo", [rd32(sc), fourCC("rcdb"), v]) == 0 && rd32(v) == 60)
            #expect(ilCall16("SoundConverterSetInfo", [rd32(sc), fourCC("rcdb"), 80]) == 0)
            #expect(ilCall16("SoundConverterGetInfo", [rd32(sc), fourCC("rcdb"), v]) == 0 && rd32(v) == 80)
            #expect(ilCall16("SoundConverterSetInfo", [rd32(sc), fourCC("rcdb"), 10]) == -50)
            #expect(ilCall16("SoundConverterGetInfo", [rd32(sc), fourCC("zzzz"), v]) == -231)
            #expect(ilCall16("SoundConverterGetInfo", [0x1234, fourCC("srat"), v]) == -50)
        }
    }
}
