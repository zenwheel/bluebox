import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

extension Classic {
    @Suite struct SoundManager {
        nonisolated(unsafe) static var played: [(frames: Int32, channels: Int32, rate: Double)] = []
        nonisolated(unsafe) static var callbacks = 0

        /// A format 1 'snd ': one sampled-synth modifier, bufferCmd to a standard header.
        static func snd(samples: Int) -> [UInt8] {
            let header = cat(be32(0), be32(UInt32(samples)), be32(0x56EE_8BA3), be32(0), be32(0), [0, 60])
            let body = [UInt8](repeating: 0x80, count: samples)
            let cmdOffset = 2 + 2 + 6 + 2 + 8
            return cat(be16(1), be16(1), be16(5), be32(0), be16(1),
                       be16(0x8000 | 81), be16(0), be32(UInt32(cmdOffset)), header, body)
        }

        static let callback: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)                               // command
            _ = cpu_pop32(c)                               // channel
            SoundManager.callbacks += 1
            cpu_push32(c, ret)
        }

        func install() {
            Self.played = []
            machine_set_sound_handler({ _, pcm, frames, channels, rate, _, _ in
                if pcm != nil { SoundManager.played.append((frames, channels, rate)) }
            }, nil)
        }

        @Test func synchronousPlayWaitsForTheSound() {
            launchTestApp()
            install()
            defer { machine_set_sound_handler(nil, nil) }
            let data = Self.snd(samples: 2225)                   // about 0.1 s
            let h = tb_NewHandle(Int32(data.count), false, false)
            for (i, b) in data.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            let start = Date()
            #expect(tb_SndPlay(0, h, 0) == 0)
            #expect(Date().timeIntervalSince(start) >= 0.09)
            #expect(Self.played.count == 1)
            #expect(Self.played.first?.frames == 2225)
            #expect(abs((Self.played.first?.rate ?? 0) - 22254.5) < 1)
        }

        @Test func callbacksRunAfterTheSoundAtSafePoints() {
            launchTestApp()
            install()
            defer { machine_set_sound_handler(nil, nil) }
            Self.callbacks = 0
            let proc = native_register(Self.callback, "test sound callback", 0)
            let chanp = tb_NewPtr(4, false, true)
            #expect(tb_SndNewChannel(chanp, 5, 0, proc) == 0)
            let chan = rd32(chanp)
            let data = Self.snd(samples: 4450)                   // about 0.2 s
            let h = tb_NewHandle(Int32(data.count), false, false)
            for (i, b) in data.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            #expect(tb_SndPlay(chan, h, 1) == 0)
            let cmd = tb_NewPtr(8, false, true)
            wr16(cmd, 13)                                        // callBackCmd
            #expect(tb_SndDoCommand(chan, cmd, 0) == 0)
            _ = tb_TickCount()
            #expect(Self.callbacks == 0)                         // still playing
            usleep(250_000)
            _ = tb_TickCount()                                   // a safe point
            #expect(Self.callbacks == 1)
            #expect(tb_SndDisposeChannel(chan, 1) == 0)
        }
    }
}

extension Classic {
    @Suite struct StringsAndScripts {
        func compare(_ a: String, _ b: String, trap: UInt16, _ fn: (UnsafeMutablePointer<cpu68k>?, UInt16) -> Void) -> Int32 {
            let pa = tb_NewPtr(64, false, true), pb = tb_NewPtr(64, false, true)
            for (i, c) in a.utf8.enumerated() { wr8(pa + UInt32(i), c) }
            for (i, c) in b.utf8.enumerated() { wr8(pb + UInt32(i), c) }
            let c = g_cpu!
            c.pointee.a.0 = pa
            c.pointee.a.1 = pb
            c.pointee.d.0 = UInt32(a.utf8.count) << 16 | UInt32(b.utf8.count)
            fn(c, trap)
            return Int32(bitPattern: c.pointee.d.0)
        }

        @Test func cmpStringHonoursCaseFlag() {
            launchTestApp()
            #expect(compare("Hello", "hello", trap: 0xA03C, raw_CmpString) == 0)  // case-insensitive
            #expect(compare("Hello", "hello", trap: 0xA43C, raw_CmpString) == 1)  // case-sensitive
            #expect(compare("Hello", "Help", trap: 0xA03C, raw_CmpString) == 1)
        }

        @Test func relStringOrders() {
            launchTestApp()
            #expect(compare("apple", "banana", trap: 0xA050, raw_RelString) == -1)
            #expect(compare("pear", "pea", trap: 0xA050, raw_RelString) == 1)
            #expect(compare("Same", "same", trap: 0xA050, raw_RelString) == 0)
        }

        @Test func scriptManagerReportsRoman() {
            launchTestApp()
            #expect(tb_GetScriptManagerVariable(0) == 0x0710)
            #expect(tb_FontScript() == 0)
            #expect(tb_GetScriptVariable(0, 86) == (3 << 16 | 12))
            let t = tb_NewPtr(32, false, true)
            for (i, c) in "Aa1 .".utf8.enumerated() { wr8(t + UInt32(i), c) }
            #expect(tb_CharType(t, 0) == 0x4001)   // upper-case ASCII letter
            #expect(tb_CharType(t, 1) == 0x0001)
            #expect(tb_CharType(t, 2) == 0x0100)   // digit
            #expect(tb_CharType(t, 3) == 0x0300)   // blank
        }

        @Test func findWordFindsTheWordAroundAnOffset() {
            launchTestApp()
            let t = tb_NewPtr(32, false, true)
            let text = "hello brave world"
            for (i, c) in text.utf8.enumerated() { wr8(t + UInt32(i), c) }
            let offs = tb_NewPtr(12, false, true)
            tb_FindWord(t, Int16(text.utf8.count), 8, 1, 0, offs)
            #expect(rd16(offs) == 6 && rd16(offs + 2) == 11)
        }
    }
}

extension Classic {
    @Suite struct NotificationManager {
        nonisolated(unsafe) static var posted: [(Int32, String, Int32)] = []
        nonisolated(unsafe) static var responses = 0

        static let response: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)
            NotificationManager.responses += 1
            cpu_push32(c, ret)
        }

        func request(text: String?, resp: UInt32) -> UInt32 {
            let nm = tb_NewPtr(Int32(NMRec_SIZEOF), false, true)
            wr16(nm + UInt32(NMRec_qType_off), 8)
            wr16(nm + UInt32(NMRec_nmMark_off), 1)
            if let text {
                let s = tb_NewPtr(256, false, true)
                guestPString(text, at: s)
                wr32(nm + UInt32(NMRec_nmStr_off), s)
            }
            wr32(nm + UInt32(NMRec_nmResp_off), resp)
            return nm
        }

        @Test func requestsReachTheHostAndResponsesRun() {
            launchTestApp()
            Self.posted = []
            Self.responses = 0
            machine_set_notification_handler({ id, text, flags, _ in
                NotificationManager.posted.append((id, text.map { String(cString: $0) } ?? "", flags))
            }, nil)
            defer { machine_set_notification_handler(nil, nil) }
            let proc = native_register(Self.response, "test nm response", 0)
            let nm = request(text: "Backup finished", resp: proc)
            #expect(tb_NMInstall(nm) == 0)
            #expect(Self.posted.count == 1)
            #expect(Self.posted.first?.1 == "Backup finished")
            #expect(Self.posted.first?.2 == Int32(MACHINE_NOTIFY_MARK))
            let evt = tb_NewPtr(16, false, true)
            _ = tb_WaitNextEvent(-1, evt, 0, 0)
            #expect(Self.responses == 1)
            #expect(tb_NMRemove(nm) == 0)
            #expect(Self.posted.last?.0 == -1)                 // withdrawn
        }

        @Test func automaticRemoval() {
            launchTestApp()
            Self.posted = []
            machine_set_notification_handler({ id, _, _, _ in NotificationManager.posted.append((id, "", 0)) }, nil)
            defer { machine_set_notification_handler(nil, nil) }
            let nm = request(text: nil, resp: 0xFFFF_FFFF)
            #expect(tb_NMInstall(nm) == 0)
            let evt = tb_NewPtr(16, false, true)
            _ = tb_WaitNextEvent(-1, evt, 0, 0)
            #expect(Self.posted.map(\.0) == [1, -1])
            #expect(tb_NMRemove(nm) != 0)                      // already gone
        }
    }
}
