import CCore
import Darwin
import Testing
import Toolbox

extension BlueBox {
    /// Open Transport's AppleTalk from PowerPC code (the exports called as
    /// a PowerPC caller would, arguments in r3 on), on the stack the .MPP
    /// driver runs: LocalTalk over UDP on the loopback group.
    @Suite struct OpenTransportAppleTalk {
        /// An Open Transport call: its arguments in r3 on, its result.
        @discardableResult
        func ot(_ name: String, _ args: UInt32...) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { raw in
                for (i, a) in args.enumerated() { raw.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("\(name) is not exported")
                return 0xFFFF_FFFF
            }
            fn(&g_ppc)
            return g_ppc.gpr.3
        }

        func err(_ v: UInt32) -> Int32 { Int32(bitPattern: v) }

        /// This machine's AppleTalk node.
        func node() -> UInt8 { at_node() }

        func cstr(_ s: String) -> UInt32 {
            let bytes = Array(s.utf8)
            let p = tb_NewPtr(Int32(bytes.count + 1), false, true)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func cstring(at p: UInt32) -> String {
            var out = [UInt8]()
            var i: UInt32 = 0
            while rd8(p + i) != 0 { out.append(rd8(p + i)); i += 1 }
            return String(decoding: out, as: UTF8.self)
        }

        func bytes(_ p: UInt32, _ n: Int) -> String {
            String(decoding: (0..<n).map { rd8(p + UInt32($0)) }, as: UTF8.self)
        }

        /// A TNetbuf at p: maxlen, len, buf (a fresh buffer of maxlen).
        @discardableResult
        func netbuf(_ p: UInt32, max: Int, text: String? = nil) -> UInt32 {
            let buf = tb_NewPtr(Int32(max), false, true)
            wr32(p, UInt32(max))
            wr32(p + 8, buf)
            wr32(p + 4, 0)
            if let text {
                for (i, b) in text.utf8.enumerated() { wr8(buf + UInt32(i), b) }
                wr32(p + 4, UInt32(text.utf8.count))
            }
            return buf
        }

        func start() {
            #expect(machine_set_appletalk("local"))
            launchTestApp()
        }

        /// The stack runs until done (which may call Open Transport) says so.
        func pump(until done: () -> Bool) {
            for _ in 0..<500 {
                if done() { return }
                usleep(1000)
                appletalk_poll()
            }
        }

        func open(_ config: String) -> UInt32 {
            let e = tb_NewPtr(4, false, true)
            let ep = ot("OTOpenEndpoint", ot("OTCreateConfiguration", cstr(config)), 0, 0, e)
            #expect(rd32(e) == 0 && ep != 0, "\(config)")
            return ep
        }

        /// Bound to any socket: the socket.
        func bind(_ ep: UInt32, qlen: UInt32 = 0) -> UInt8 {
            let req = tb_NewPtr(16, false, true), ret = tb_NewPtr(16, false, true)
            wr32(req + 12, qlen)
            netbuf(ret, max: 8)
            #expect(ot("OTBind", ep, qlen > 0 ? req : 0, ret) == 0)
            #expect(rd32(ret + 4) == 8 && rd16(rd32(ret + 8)) == 0x0100)
            return rd8(rd32(ret + 8) + 5)
        }

        /// A DDP address for this node.
        func ddp(_ skt: UInt8, type: UInt8 = 0) -> UInt32 {
            let a = tb_NewPtr(8, false, true)
            ot("OTInitDDPAddress", a, 0, UInt32(node()), UInt32(skt), UInt32(type))
            return a
        }

        /// Two ddp endpoints: a datagram from one to the other, with its
        /// sender's address and DDP type.
        @Test func ddpDatagram() {
            start()
            gestalt_powerpc() // a PowerPC program's Gestalt: AppleTalk under Open Transport too
            var answer: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("otan")), &answer) == 0 && answer & 0x0C == 0x0C)
            let a = open("ddp"), b = open("ddp")
            let sa = bind(a), sb = bind(b)
            #expect(sa != 0 && sb != 0 && sa != sb)

            let ud = tb_NewPtr(36, false, true)
            wr32(ud + 0, 8); wr32(ud + 4, 8); wr32(ud + 8, ddp(sb, type: 77))
            netbuf(ud + 24, max: 32, text: "hello")
            #expect(ot("OTSndUData", a, ud) == 0)

            let rd = tb_NewPtr(36, false, true)
            netbuf(rd, max: 8)
            netbuf(rd + 24, max: 32)
            let flags = tb_NewPtr(4, false, true)
            var r = UInt32(0)
            pump { r = ot("OTRcvUData", b, rd, flags); return r != UInt32(bitPattern: -3162) }
            #expect(r == 0)
            #expect(rd32(rd + 28) == 5 && bytes(rd32(rd + 32), 5) == "hello")
            let from = rd32(rd + 8)
            #expect(rd16(from) == 0x0100 && rd8(from + 4) == node() && rd8(from + 5) == sa && rd8(from + 6) == 77)
            #expect(err(ot("OTRcvUData", b, rd, flags)) == -3162) // kOTNoDataErr: nothing more
            #expect(ot("OTCloseProvider", a) == 0 && ot("OTCloseProvider", b) == 0)
        }

        /// NBP through the mapper: a name registered, found by a wildcard
        /// lookup, refused a second time, and gone once deleted.
        @Test func nbpRegisterAndLookup() {
            start()
            let e = tb_NewPtr(4, false, true)
            let m = ot("OTOpenMapper", ot("OTCreateConfiguration", cstr("nbp")), 0, e)
            #expect(rd32(e) == 0 && m != 0)

            let reg = tb_NewPtr(28, false, true), regReply = tb_NewPtr(16, false, true)
            netbuf(reg, max: 64, text: "Tester:BlueBoxOT@*")
            netbuf(regReply, max: 8)
            #expect(ot("OTRegisterName", m, reg, regReply) == 0)
            let id = rd32(regReply + 12)
            #expect(id != 0 && rd8(rd32(regReply + 8) + 4) == node())
            #expect(err(ot("OTRegisterName", m, reg, regReply)) == -3216) // kOTDuplicateFoundErr

            let look = tb_NewPtr(36, false, true), reply = tb_NewPtr(16, false, true)
            netbuf(look, max: 64, text: "=:BlueBoxOT@*")
            wr32(look + 24, 1)   // maxcnt
            wr32(look + 28, 1000) // timeout
            let names = netbuf(reply, max: 512)
            #expect(ot("OTLookupName", m, look, reply) == 0)
            #expect(rd32(reply + 12) == 1)
            #expect(rd16(names) == 8 && rd8(names + 4 + 4) == node())
            #expect(bytes(names + 12, Int(rd16(names + 2))) == "Tester:BlueBoxOT@*")
            #expect(rd8(names + 4 + 5) == rd8(rd32(regReply + 8) + 5))

            #expect(ot("OTDeleteNameByID", m, id) == 0)
            wr32(look + 28, 250)
            #expect(ot("OTLookupName", m, look, reply) == 0)
            #expect(rd32(reply + 12) == 0)
            #expect(ot("OTCloseProvider", m) == 0)

            // AppleTalk services: this zone, and our address.
            let svc = ot("OTOpenAppleTalkServices", 0xFFFF_FFFD, 0, e)
            #expect(rd32(e) == 0 && svc != 0)
            let zone = tb_NewPtr(12, false, true)
            netbuf(zone, max: 33)
            #expect(ot("OTATalkGetMyZone", svc, zone) == 0)
            #expect(pstring(at: rd32(zone + 8)) == "*")
            let info = tb_NewPtr(12, false, true)
            netbuf(info, max: 22)
            #expect(ot("OTATalkGetInfo", svc, info) == 0)
            #expect(rd32(info + 4) == 22 && rd8(rd32(info + 8) + 4) == node())
        }

        /// The NBP entity helpers: an address's name split and escaped,
        /// parts replaced, and back to a string.
        @Test func nbpEntityStrings() {
            bootMachine()
            let addr = tb_NewPtr(120, false, true), entity = tb_NewPtr(99, false, true)
            let len = ot("OTInitNBPAddress", addr, cstr("My\\\\Machine:My\\:Server@My\\@Zone"))
            #expect(len == 2 + 31 && rd16(addr) == 0x0102)
            #expect(ot("OTSetNBPEntityFromAddress", entity, addr, len) == 1)
            #expect(pstring(at: entity) == "My\\Machine" && pstring(at: entity + 11) == "My:Server")
            let out = tb_NewPtr(80, false, true)
            ot("OTExtractNBPType", entity, out)
            #expect(cstring(at: out) == "My\\:Server")
            ot("OTExtractNBPZone", entity, out)
            #expect(cstring(at: out) == "My\\@Zone")

            #expect(ot("OTSetNBPName", entity, cstr("Printer")) == 1)
            #expect(ot("OTSetNBPZone", entity, cstr("")) == 1)
            #expect(ot("OTSetNBPType", entity, cstr(String(repeating: "x", count: 40))) == 0) // cut to 32
            ot("OTExtractNBPName", entity, out)
            #expect(cstring(at: out) == "Printer")
            let n = ot("OTGetNBPEntityLengthAsAddress", entity)
            #expect(n == 7 + 1 + 32)
            #expect(ot("OTSetAddressFromNBPEntity", out, entity) == n)
            #expect(bytes(out, Int(n)) == "Printer:" + String(repeating: "x", count: 32))

            // A bare string (a lookup's), and the string copy.
            let s = cstr("Laser:LaserWriter@*")
            #expect(ot("OTSetNBPEntityFromAddress", entity, s, 19) == 1)
            ot("OTExtractNBPName", entity, out)
            #expect(cstring(at: out) == "Laser")
            #expect(ot("OTSetAddressFromNBPString", out, s, 0xFFFF_FFFF) == 19)
            #expect(ot("OTSetAddressFromNBPString", out, s, 5) == 5)
            ot("OTInitNBPEntity", entity)
            #expect(rd8(entity) == 0 && rd8(entity + 1) == 0 && rd8(entity + 2) == 0)

            let x = ddp(9), y = tb_NewPtr(8, false, true)
            ot("OTInitDDPAddress", y, 0, 0, 9, 0) // node 0 matches any
            #expect(ot("OTCompareDDPAddresses", x, y) == 1)
            ot("OTInitDDPAddress", y, 0, 0, 10, 0)
            #expect(ot("OTCompareDDPAddresses", x, y) == 0)
        }

        /// ADSP: a listener hears a connection request, a second endpoint
        /// accepts it, data goes across, and the orderly release reaches
        /// both ends.
        @Test func adspConnection() {
            start()
            let listener = open("adsp"), worker = open("adsp"), client = open("adsp")
            let ls = bind(listener, qlen: 1)
            _ = bind(client)
            let call = tb_NewPtr(40, false, true)
            wr32(call + 0, 8); wr32(call + 4, 8); wr32(call + 8, ddp(ls))
            #expect(err(ot("OTConnect", client, call, 0)) == -3162) // kOTNoDataErr: T_CONNECT to come

            let heard = tb_NewPtr(40, false, true)
            netbuf(heard, max: 8)
            var r = UInt32(0)
            pump { r = ot("OTListen", listener, heard); return r == 0 }
            #expect(r == 0 && rd32(heard + 36) == 1)
            #expect(ot("OTAccept", listener, worker, heard) == 0)
            #expect(ot("OTGetEndpointState", worker) == 5) // T_DATAXFER
            pump { ot("OTRcvConnect", client, 0) == 0 }
            #expect(ot("OTGetEndpointState", client) == 5)

            let text = cstr("over ADSP")
            #expect(ot("OTSnd", client, text, 9, 0) == 9)
            let buf = tb_NewPtr(64, false, true), flags = tb_NewPtr(4, false, true)
            pump { ot("OTLook", worker) == 0x0004 } // T_DATA
            #expect(ot("OTRcv", worker, buf, 64, flags) == 9)
            #expect(bytes(buf, 9) == "over ADSP")

            #expect(ot("OTSndOrderlyDisconnect", client) == 0)
            pump { ot("OTLook", worker) == 0x0080 } // T_ORDREL
            #expect(ot("OTLook", worker) == 0x0080)
            #expect(ot("OTRcvOrderlyDisconnect", worker) == 0)
            #expect(ot("OTSndOrderlyDisconnect", worker) == 0)
            #expect(ot("OTGetEndpointState", worker) == 2) // T_IDLE
            pump { ot("OTLook", client) == 0x0080 }
            #expect(ot("OTRcvOrderlyDisconnect", client) == 0)
            #expect(ot("OTGetEndpointState", client) == 2)
            for ep in [listener, worker, client] { #expect(ot("OTCloseProvider", ep) == 0) }
        }

        /// ATP: an exactly-once request, heard with its user bytes, and
        /// the reply back to the requester.
        @Test func atpTransaction() {
            start()
            let asker = open("atp"), answerer = open("atp")
            _ = bind(asker)
            let sb = bind(answerer)
            let req = tb_NewPtr(40, false, true)
            wr32(req + 0, 8); wr32(req + 4, 8); wr32(req + 8, ddp(sb))
            netbuf(req + 24, max: 16, text: "USERping")
            #expect(ot("OTSndURequest", asker, req, 0x0004) == 0) // T_ACKNOWLEDGED
            let seq = rd32(req + 36)

            let got = tb_NewPtr(40, false, true), flags = tb_NewPtr(4, false, true)
            netbuf(got, max: 8)
            netbuf(got + 24, max: 64)
            var r = UInt32(0)
            pump { r = ot("OTRcvURequest", answerer, got, flags); return r == 0 }
            #expect(r == 0 && rd32(flags) == 0x0004)
            #expect(rd32(got + 28) == 8 && bytes(rd32(got + 32), 8) == "USERping")

            let rep = tb_NewPtr(28, false, true)
            netbuf(rep + 12, max: 16, text: "RESPpong")
            wr32(rep + 24, rd32(got + 36))
            #expect(ot("OTSndUReply", answerer, rep, 0) == 0)

            let back = tb_NewPtr(28, false, true)
            netbuf(back + 12, max: 64)
            pump { r = ot("OTRcvUReply", asker, back, flags); return r == 0 }
            #expect(r == 0 && rd32(back + 24) == seq)
            #expect(rd32(back + 16) == 8 && bytes(rd32(back + 20), 8) == "RESPpong")
            #expect(ot("OTCloseProvider", asker) == 0 && ot("OTCloseProvider", answerer) == 0)
        }
    }
}
