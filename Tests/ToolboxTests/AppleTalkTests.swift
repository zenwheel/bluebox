import CCore
import Testing
import Toolbox

extension BlueBox {
    /// The .MPP driver on LocalTalk over UDP (the loopback group): NBP
    /// names, and a datagram to our own socket through a 68k listener.
    @Suite struct AppleTalk {
        func control(_ pb: UInt32, _ code: Int) -> Int16 {
            wr16(pb + UInt32(CntrlParam_csCode_off), UInt16(code))
            return tb_PBControl(pb, false)
        }

        /// object, type and zone, packed.
        func entity(_ parts: [String], at p: UInt32) {
            var q = p
            for s in parts {
                guestPString(s, at: q)
                q += UInt32(s.utf8.count + 1)
            }
        }

        @Test func namesAndDatagrams() {
            #expect(machine_set_appletalk("local"))
            launchTestApp()
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let pb = tb_NewPtr(80, false, true), name = tb_NewPtr(8, false, true)
            guestPString(".MPP", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == 0)
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off))) == -10)
            let vars = rd32(UInt32(LM_ABusVars))
            let node = rd8(vars)
            #expect(node >= 1 && node <= 254)

            // A listener: read the rest into buf, keep D3 and the first byte.
            let buf = tb_NewPtr(128, false, true), out = tb_NewPtr(4, false, true)
            let code = tb_NewPtr(32, false, true)
            let words: [UInt16] = [0x47F9, UInt16(buf >> 16), UInt16(buf & 0xFFFF), // LEA buf,A3
                                   0x363C, 100,                                       // MOVE.W #100,D3
                                   0x4EAC, 2,                                         // JSR 2(A4)
                                   0x33C3, UInt16(out >> 16), UInt16(out & 0xFFFF),   // MOVE.W D3,out
                                   0x4E75]                                            // RTS
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            wr8(pb + 28, 0)
            wr32(pb + 30, code)
            #expect(control(pb, 248) == 0) // openSkt
            let socket = rd8(pb + 28)
            #expect(socket >= 128)
            wr8(pb + 28, 1)
            #expect(control(pb, 256) == 0) // setSelfSend

            // NBP: register (no verify) and find ourselves.
            let nte = tb_NewPtr(120, false, true)
            wr8(nte + 7, socket)
            entity(["Tester", "BlueBoxTest", "*"], at: nte + 9)
            gmemset(pb + 28, 0, 40)
            wr32(pb + 30, nte)
            #expect(control(pb, 253) == 0) // registerName
            let want = tb_NewPtr(64, false, true), ret = tb_NewPtr(200, false, true)
            entity(["=", "BlueBoxTest", "*"], at: want)
            gmemset(pb + 28, 0, 40)
            wr8(pb + 28, 1)  // interval
            wr8(pb + 29, 1)  // count
            wr32(pb + 30, want)
            wr32(pb + 34, ret)
            wr16(pb + 38, 200)
            wr16(pb + 40, 1)
            #expect(control(pb, 251) == 0) // lookupName
            #expect(rd16(pb + 42) == 1)
            #expect(rd8(ret + 2) == node && rd8(ret + 3) == socket)
            #expect(pstring(at: ret + 5) == "Tester")

            // A datagram to our own socket, through the listener.
            let hdr = tb_NewPtr(17, false, true) + 1, wds = tb_NewPtr(14, false, true)
            let data = tb_NewPtr(8, false, true)
            for (i, b) in "hello".utf8.enumerated() { wr8(data + UInt32(i), b) }
            wr16(hdr + 7, 0)
            wr8(hdr + 11, node)
            wr8(hdr + 13, socket)
            wr8(hdr + 15, 77)
            wr32(wds + 2, hdr)
            wr16(wds + 6, 5)
            wr32(wds + 8, data)
            gmemset(pb + 28, 0, 40)
            wr8(pb + 28, socket)
            wr32(pb + 30, wds)
            #expect(control(pb, 246) == 0) // writeDDP
            appletalk_poll()
            #expect(rd16(out) == 95)
            #expect(String(decoding: (0..<5).map { rd8(buf + UInt32($0)) }, as: UTF8.self) == "hello")
            #expect(rd8(vars + 1 + 2) == 1 && rd8(vars + 1 + 3 + 4) == 77) // short DDP, type 77, in the RHA
        }
    }
}
