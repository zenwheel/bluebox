import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Speech Manager, without a host: utterances end at the next safe point.
    @Suite struct Speech {
        nonisolated(unsafe) static var done: [UInt32] = []

        /// SpeechDone(chan, refCon).
        static let doneProc: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let refcon = cpu_pop32(c)
            _ = cpu_pop32(c)
            Speech.done.append(refcon)
            cpu_push32(c, ret)
        }

        @Test func voicesAreTheClassicOnes() {
            launchTestApp()
            _ = initToolbox()
            let n = tb_NewPtr(2, false, true)
            #expect(tb_CountVoices(n) == 0 && rd16(n) >= 20)
            let spec = tb_NewPtr(8, false, true)
            var names: [String] = []
            let info = tb_NewPtr(362, false, true)
            for i in 1...Int(rd16(n)) {
                #expect(tb_GetIndVoice(Int16(i), spec) == 0)
                #expect(tb_GetVoiceDescription(spec, info, 362) == 0)
                names.append(pstring(at: info + 16))
            }
            #expect(names.contains("Fred") && names.contains("Kathy") && names.contains("Zarvox"))
            var g: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: 0x7474_7363), &g) == 0 && g & 1 == 1) // 'ttsc'
        }

        @Test func aChannelSpeaksAndSaysWhenItIsDone() {
            launchTestApp()
            _ = initToolbox()
            g_cpu!.pointee.a.7 = tb_NewPtr(65536, false, true) + 65536 - 16
            Self.done = []
            let chp = tb_NewPtr(4, false, true)
            #expect(tb_NewSpeechChannel(0, chp) == 0)
            let ch = rd32(chp)
            let proc = native_register(Self.doneProc, "speech done", 0)
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x7265_6663), 1234) == 0) // 'refc'
            #expect(tb_SetSpeechInfo(ch, Int32(bitPattern: 0x646E_6362), proc) == 0) // 'dncb'
            let rate = tb_NewPtr(4, false, true)
            #expect(tb_SetSpeechRate(ch, 200 << 16) == 0)
            #expect(tb_GetSpeechRate(ch, rate) == 0 && rd32(rate) == 200 << 16)
            let text = tb_NewPtr(32, false, true)
            for (i, b) in "Hello, world.".utf8.enumerated() { wr8(text + UInt32(i), b) }
            #expect(tb_SpeakText(ch, text, 13) == 0)
            let status = tb_NewPtr(8, false, true)
            #expect(tb_GetSpeechInfo(ch, Int32(bitPattern: 0x7374_6174), status) == 0 && rd8(status) == 1) // busy
            #expect(tb_SpeechBusy() == 0)                                  // over at the safe point
            #expect(Self.done == [1234])
            #expect(tb_DisposeSpeechChannel(ch) == 0)
        }
    }
}
