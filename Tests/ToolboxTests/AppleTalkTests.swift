import CCore
import Darwin
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

        /// On a network interface that isn't there, .MPP doesn't open
        /// (portNotCf), rather than joining the group on another.
        @Test func missingInterfaceIsNotConfigured() {
            #expect(machine_set_appletalk("lan:nosuchif9"))
            #expect(!machine_set_appletalk("lan:"))
            launchTestApp()
            let pb = tb_NewPtr(80, false, true), name = tb_NewPtr(8, false, true)
            guestPString(".MPP", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == -98)
            #expect(machine_set_appletalk("local"))
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

        func open(_ name: String) -> Int16 {
            let pb = tb_NewPtr(80, false, true), np = tb_NewPtr(8, false, true)
            guestPString(name, at: np)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), np)
            #expect(tb_PBOpen(pb, false) == 0)
            return Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off)))
        }

        func call(_ ref: Int16, _ code: Int, _ pb: UInt32, async: Bool = false) -> Int16 {
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: ref))
            wr16(pb + UInt32(CntrlParam_csCode_off), UInt16(code))
            return driver_call(pb, async, Int32(DRIVER_CONTROL))
        }

        func poll(until done: () -> Bool) {
            for _ in 0..<200 where !done() {
                usleep(2000)
                appletalk_poll()
            }
        }

        /// An ATP transaction to our own node: a request, the responder's
        /// GetRequest hearing it, and two responses filling the BDS.
        @Test func atpTransaction() {
            #expect(machine_set_appletalk("local"))
            launchTestApp()
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let mpp = open(".MPP"), atp = open(".ATP")
            #expect(atp == -11)
            let pb = tb_NewPtr(80, false, true)
            wr8(pb + 28, 1)
            #expect(call(mpp, 256, pb) == 0) // setSelfSend
            let node = rd8(rd32(UInt32(LM_ABusVars)))

            gmemset(pb, 0, 80)
            #expect(call(atp, 254, pb) == 0) // openATPSkt
            let skt = rd8(pb + 28)

            let get = tb_NewPtr(80, false, true), reqbuf = tb_NewPtr(16, false, true)
            wr8(get + 28, skt)
            wr16(get + 34, 16)
            wr32(get + 36, reqbuf)
            #expect(call(atp, 253, get, async: true) == 1) // getRequest

            let req = tb_NewPtr(80, false, true), data = tb_NewPtr(8, false, true)
            let bds = tb_NewPtr(12 * 2, false, true), b0 = tb_NewPtr(16, false, true), b1 = tb_NewPtr(16, false, true)
            for (i, b) in [b0, b1].enumerated() {
                wr16(bds + UInt32(i * 12), 16)
                wr32(bds + UInt32(i * 12 + 2), b)
            }
            for (i, c) in "hi".utf8.enumerated() { wr8(data + UInt32(i), c) }
            wr8(req + 29, 32) // exactly once
            wr8(req + 32, node)
            wr8(req + 33, skt)
            wr16(req + 34, 2)
            wr32(req + 36, data)
            wr32(req + 40, bds)
            wr8(req + 44, 2) // numOfBuffs
            wr8(req + 45, 1) // timeout, seconds
            wr8(req + 47, 3) // retries
            wr32(req + 18, 0x1234_5678)
            #expect(call(atp, 255, req, async: true) == 1) // sendRequest
            poll { rd16(get + 16) != 1 }
            #expect(rd16(get + 16) == 0)
            #expect(rd16(get + 34) == 2 && rd8(reqbuf) == 0x68 && rd8(reqbuf + 1) == 0x69)
            #expect(rd32(get + 18) == 0x1234_5678 && rd8(get + 29) == 32)

            let rsp = tb_NewPtr(80, false, true), rbds = tb_NewPtr(24, false, true)
            for (i, text) in ["one", "two!"].enumerated() {
                let p = tb_NewPtr(8, false, true)
                for (k, c) in text.utf8.enumerated() { wr8(p + UInt32(k), c) }
                wr32(rbds + UInt32(i * 12 + 2), p)
                wr16(rbds + UInt32(i * 12 + 6), UInt16(text.utf8.count))
            }
            wr8(rsp + 28, skt)
            wr8(rsp + 29, 16) // EOM
            wr32(rsp + 30, rd32(get + 30))
            wr32(rsp + 40, rbds)
            wr8(rsp + 45, 2) // bdsSize
            wr16(rsp + 46, rd16(get + 46))
            #expect(call(atp, 252, rsp) == 0) // sendResponse
            poll { rd16(req + 16) != 1 }
            #expect(rd16(req + 16) == 0)
            #expect(rd8(req + 46) == 2) // numOfResps
            #expect(rd16(bds + 6) == 3 && rd16(bds + 18) == 4)
            #expect(rd8(b1 + 3) == 0x21)

            let xpp = open(".XPP"), zone = tb_NewPtr(40, false, true), x = tb_NewPtr(80, false, true)
            wr16(x + 28, 7) // zipGetMyZone
            wr32(x + 34, zone)
            #expect(call(xpp, 246, x) == 0)
            #expect(pstring(at: zone) == "*")
        }

        /// An ADSP session to ourselves: a listener hears the request, a
        /// connection end accepts it, a message goes across with its end
        /// marked, and the close reaches the other end.
        @Test func adspSession() {
            #expect(machine_set_appletalk("local"))
            launchTestApp()
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let mpp = open(".MPP"), dsp = open(".DSP")
            let pb = tb_NewPtr(80, false, true)
            wr8(pb + 28, 1)
            #expect(call(mpp, 256, pb) == 0) // setSelfSend
            let node = rd8(rd32(UInt32(LM_ABusVars)))

            func newEnd(_ code: Int) -> (ref: UInt16, skt: UInt8, ccb: UInt32) {
                let p = tb_NewPtr(80, false, true), ccb = tb_NewPtr(242, false, true)
                wr32(p + 34, ccb)
                wr16(p + 42, 600)
                wr16(p + 48, 600)
                wr32(p + 54, tb_NewPtr(570, false, true))
                #expect(call(dsp, code, p) == 0)
                return (rd16(p + 32), rd8(p + 58), ccb)
            }
            let listener = newEnd(251), client = newEnd(255), server = newEnd(255)

            let listen = tb_NewPtr(80, false, true)
            wr16(listen + 32, listener.ref)
            #expect(call(dsp, 249, listen, async: true) == 1) // dspCLListen

            let req = tb_NewPtr(80, false, true)
            wr16(req + 32, client.ref)
            wr8(req + 40, node)
            wr8(req + 41, listener.skt)
            wr8(req + 64, 1) // ocRequest
            #expect(call(dsp, 253, req, async: true) == 1)
            poll { rd16(listen + 16) != 1 }
            #expect(rd16(listen + 16) == 0)

            let acc = tb_NewPtr(80, false, true)
            gmemcpy(acc + 34, listen + 34, 30)
            wr16(acc + 32, server.ref)
            wr8(acc + 64, 3) // ocAccept
            #expect(call(dsp, 253, acc, async: true) == 1)
            poll { rd16(req + 16) != 1 && rd16(acc + 16) != 1 }
            #expect(rd16(req + 16) == 0 && rd16(acc + 16) == 0)
            #expect(rd16(client.ccb + 6) == 4 && rd16(server.ccb + 6) == 4) // sOpen

            let w = tb_NewPtr(80, false, true), text = tb_NewPtr(8, false, true)
            for (i, c) in "hello".utf8.enumerated() { wr8(text + UInt32(i), c) }
            wr16(w + 32, client.ref)
            wr16(w + 34, 5)
            wr32(w + 38, text)
            wr8(w + 42, 1) // eom
            wr8(w + 43, 1) // flush
            #expect(call(dsp, 245, w) == 0)

            let r = tb_NewPtr(80, false, true), buf = tb_NewPtr(100, false, true)
            wr16(r + 32, server.ref)
            wr16(r + 34, 100)
            wr32(r + 38, buf)
            _ = call(dsp, 246, r, async: true)
            poll { rd16(r + 16) != 1 }
            #expect(rd16(r + 16) == 0 && rd16(r + 36) == 5 && rd8(r + 42) == 1)
            #expect(String(decoding: (0..<5).map { rd8(buf + UInt32($0)) }, as: UTF8.self) == "hello")

            let c = tb_NewPtr(80, false, true)
            wr16(c + 32, client.ref)
            #expect(call(dsp, 252, c) == 0) // dspClose
            poll { rd16(server.ccb + 6) == 6 }
            #expect(rd16(server.ccb + 6) == 6 && rd8(server.ccb + 8) & 0x80 != 0) // sClosed, eClosed
        }
    }
}
