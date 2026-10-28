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

            // The data as a chain of OTData (Shadow Warrior sends so).
            let piece2 = tb_NewPtr(12, false, true), piece1 = tb_NewPtr(12, false, true)
            wr32(piece2, 0); wr32(piece2 + 4, cstr("Talk")); wr32(piece2 + 8, 4)
            wr32(piece1, piece2); wr32(piece1 + 4, cstr("Apple")); wr32(piece1 + 8, 5)
            wr32(ud + 28, 0xFFFF_FFFE); wr32(ud + 32, piece1) // kNetbufDataIsOTData
            #expect(ot("OTSndUData", a, ud) == 0)
            netbuf(rd + 24, max: 32)
            pump { r = ot("OTRcvUData", b, rd, flags); return r != UInt32(bitPattern: -3162) }
            #expect(r == 0 && rd32(rd + 28) == 9 && bytes(rd32(rd + 32), 9) == "AppleTalk")
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

        /// A connected pair of adsp endpoints (with the configuration
        /// given): the listener, the worker that took the call, the client.
        func adspPair(_ config: String) -> (UInt32, UInt32, UInt32) {
            let listener = open(config), worker = open(config), client = open(config)
            let ls = bind(listener, qlen: 1)
            _ = bind(client)
            let call = tb_NewPtr(40, false, true)
            wr32(call + 0, 8); wr32(call + 4, 8); wr32(call + 8, ddp(ls))
            #expect(err(ot("OTConnect", client, call, 0)) == -3162)
            let heard = tb_NewPtr(40, false, true)
            netbuf(heard, max: 8)
            pump { ot("OTListen", listener, heard) == 0 }
            #expect(ot("OTAccept", listener, worker, heard) == 0)
            pump { ot("OTRcvConnect", client, 0) == 0 }
            #expect(ot("OTGetEndpointState", client) == 5 && ot("OTGetEndpointState", worker) == 5)
            return (listener, worker, client)
        }

        /// ADSP attention messages both ways: T_EXDATA, and OTRcv gives
        /// the code and the message flagged T_EXPEDITED, before the data.
        @Test func adspAttention() {
            start()
            let (listener, worker, client) = adspPair("adsp")
            let buf = tb_NewPtr(64, false, true), flags = tb_NewPtr(4, false, true)
            let msg = tb_NewPtr(8, false, true)
            wr16(msg, 0x1234); wr8(msg + 2, 0x41); wr8(msg + 3, 0x42) // code $1234, "AB"
            #expect(ot("OTSnd", client, cstr("data"), 4, 0) == 4)
            #expect(ot("OTSnd", client, msg, 4, 0x0002) == 4) // T_EXPEDITED
            pump { ot("OTLook", worker) == 0x0008 } // T_EXDATA
            #expect(ot("OTLook", worker) == 0x0008)
            #expect(ot("OTRcv", worker, buf, 64, flags) == 4)
            #expect(rd32(flags) == 0x0002 && rd16(buf) == 0x1234 && bytes(buf + 2, 2) == "AB")
            #expect(ot("OTLook", worker) == 0x0004) // then the data
            #expect(ot("OTRcv", worker, buf, 64, flags) == 4 && bytes(buf, 4) == "data")

            wr16(msg, 0x0042)
            #expect(ot("OTSnd", worker, msg, 2, 0x0002) == 2) // a code alone
            pump { ot("OTLook", client) == 0x0008 }
            #expect(ot("OTRcv", client, buf, 64, flags) == 2 && rd16(buf) == 0x0042 && rd32(flags) == 0x0002)
            #expect(err(ot("OTRcv", client, buf, 64, flags)) == -3162)
            for ep in [listener, worker, client] { #expect(ot("OTCloseProvider", ep) == 0) }
        }

        /// "adsp(EnableEOM=1)": a send without T_MORE ends a message, and
        /// a receive stops at its end, T_MORE set until then; a stream
        /// (no EOM) never sets T_MORE.
        @Test func adspEndOfMessage() {
            start()
            let (listener, worker, client) = adspPair("adsp(EnableEOM=1)")
            let buf = tb_NewPtr(64, false, true), flags = tb_NewPtr(4, false, true)
            #expect(ot("OTSnd", client, cstr("one-"), 4, 0x0001) == 4) // T_MORE: the message goes on
            #expect(ot("OTSnd", client, cstr("two"), 3, 0) == 3)
            #expect(ot("OTSnd", client, cstr("three"), 5, 0) == 5)
            var got = ""
            let count = tb_NewPtr(4, false, true)
            pump { ot("OTCountDataBytes", worker, count) == 0 && rd32(count) >= 12 }
            #expect(ot("OTRcv", worker, buf, 3, flags) == 3 && rd32(flags) == 0x0001) // "one", more to come
            got += bytes(buf, 3)
            #expect(ot("OTRcv", worker, buf, 64, flags) == 4 && rd32(flags) == 0) // "-two": the end
            got += bytes(buf, 4)
            #expect(got == "one-two")
            #expect(ot("OTRcv", worker, buf, 64, flags) == 5 && rd32(flags) == 0 && bytes(buf, 5) == "three")
            for ep in [listener, worker, client] { #expect(ot("OTCloseProvider", ep) == 0) }

            let (l2, w2, c2) = adspPair("adsp")
            #expect(ot("OTSnd", c2, cstr("stream"), 6, 0) == 6)
            pump { ot("OTLook", w2) == 0x0004 }
            #expect(ot("OTRcv", w2, buf, 3, flags) == 3 && rd32(flags) == 0)
            for ep in [l2, w2, c2] { #expect(ot("OTCloseProvider", ep) == 0) }
        }

        /// An NBP address ("name:type@zone") in a fresh buffer: its length.
        func nbpAddress(_ name: String) -> (UInt32, UInt32) {
            let a = tb_NewPtr(120, false, true)
            return (a, ot("OTInitNBPAddress", a, cstr(name)))
        }

        /// A TCall (or TBind) at p for an address: addr's netbuf.
        func call(to addr: (UInt32, UInt32)) -> UInt32 {
            let c = tb_NewPtr(40, false, true)
            wr32(c + 0, addr.1); wr32(c + 4, addr.1); wr32(c + 8, addr.0)
            return c
        }

        /// A listener bound with an NBP name and a queue.
        func listen(_ config: String, as name: String) -> UInt32 {
            let ep = open(config)
            let req = call(to: nbpAddress(name)), ret = tb_NewPtr(16, false, true)
            wr32(req + 12, 1)
            netbuf(ret, max: 8)
            #expect(ot("OTBind", ep, req, ret) == 0)
            return ep
        }

        /// What an atp endpoint hears (the request's data, its user bytes
        /// first), from the TUnitRequest at got.
        func hear(_ ep: UInt32, _ got: UInt32) -> [UInt8] {
            netbuf(got, max: 8)
            netbuf(got + 24, max: 600)
            var r = UInt32(0)
            pump { r = ot("OTRcvURequest", ep, got, 0); return r == 0 }
            #expect(r == 0)
            return (0..<Int(rd32(got + 28))).map { rd8(rd32(got + 32) + UInt32($0)) }
        }

        /// PAP's packets as Inside AppleTalk has them: OpenConn to the SLS
        /// with our responding socket and flow quantum, and after the
        /// reply, SendData (sequence 1) to the printer's responding socket.
        @Test func papWire() {
            start()
            let printer = open("atp"), pap = open("pap")
            let sls = bind(printer)
            let rs = bind(pap)
            let c = tb_NewPtr(40, false, true)
            wr32(c + 0, 8); wr32(c + 4, 8); wr32(c + 8, ddp(sls))
            #expect(err(ot("OTConnect", pap, c, 0)) == -3162)
            let got = tb_NewPtr(40, false, true)
            let open = hear(printer, got)
            #expect(open.count == 8 && open[1] == 1 && open[2] == 0 && open[4] == rs && open[5] == 8)
            let connID = open[0]

            let rep = tb_NewPtr(28, false, true)
            let body = tb_NewPtr(16, false, true)
            for (i, b) in [connID, 2, 0, 0, sls, 8, 0, 0, 0].enumerated() { wr8(body + UInt32(i), b) }
            wr32(rep + 12, 16); wr32(rep + 16, 9); wr32(rep + 20, body)
            wr32(rep + 24, rd32(got + 36))
            #expect(ot("OTSndUReply", printer, rep, 0) == 0)
            pump { ot("OTRcvConnect", pap, 0) == 0 }
            #expect(ot("OTGetEndpointState", pap) == 5)
            let send = hear(printer, got)
            #expect(send.count == 4 && send == [connID, 3, 0, 1])
            #expect(rd8(rd32(got + 8) + 5) == rs)
            #expect(ot("OTCloseProvider", pap) == 0 && ot("OTCloseProvider", printer) == 0)
        }

        /// PAP both ends: a job found by its NBP name, more than a flow
        /// quantum of it written, the end of it (EOF) as T_ORDREL, the
        /// printer's answer back, and both ends idle again; then a
        /// connection refused.
        @Test func papJob() {
            start()
            let listener = listen("pap", as: "Printer:LaserWriter@*")
            let worker = open("pap"), client = open("pap")
            _ = bind(client)
            #expect(err(ot("OTConnect", client, call(to: nbpAddress("Printer:LaserWriter@*")), 0)) == -3162)
            let heard = tb_NewPtr(40, false, true)
            netbuf(heard, max: 8)
            pump { ot("OTListen", listener, heard) == 0 }
            #expect(ot("OTAccept", listener, worker, heard) == 0)
            #expect(ot("OTGetEndpointState", worker) == 5)
            pump { ot("OTRcvConnect", client, 0) == 0 }
            #expect(ot("OTGetEndpointState", client) == 5)

            let job = (0..<5000).map { UInt8(truncatingIfNeeded: $0 &* 7) }
            let src = tb_NewPtr(5000, false, true)
            for (i, b) in job.enumerated() { wr8(src + UInt32(i), b) }
            #expect(ot("OTSnd", client, src, 5000, 0) == 5000)
            #expect(ot("OTSndOrderlyDisconnect", client) == 0)
            #expect(ot("OTGetEndpointState", client) == 6) // T_OUTREL

            let buf = tb_NewPtr(8192, false, true), flags = tb_NewPtr(4, false, true)
            var got = [UInt8]()
            pump {
                let n = Int(Int32(bitPattern: ot("OTRcv", worker, buf, 8192, flags)))
                let more = (0..<max(n, 0)).map { rd8(buf + UInt32($0)) }
                got += more
                return got.count >= 5000 && ot("OTLook", worker) == 0x0080 // T_ORDREL
            }
            #expect(got.count == job.count)
            #expect(got.count != job.count || got == job)
            #expect(ot("OTRcvOrderlyDisconnect", worker) == 0)
            #expect(ot("OTSnd", worker, cstr("%%[ done ]%%"), 12, 0) == 12)
            #expect(ot("OTSndOrderlyDisconnect", worker) == 0)
            #expect(ot("OTGetEndpointState", worker) == 2)

            pump { ot("OTLook", client) == 0x0004 }
            #expect(ot("OTRcv", client, buf, 64, flags) == 12 && bytes(buf, 12) == "%%[ done ]%%")
            pump { ot("OTLook", client) == 0x0080 }
            #expect(ot("OTRcvOrderlyDisconnect", client) == 0)
            #expect(ot("OTGetEndpointState", client) == 2)

            // A second connection, refused: T_DISCONNECT for the client.
            #expect(err(ot("OTConnect", client, call(to: nbpAddress("Printer:LaserWriter@*")), 0)) == -3162)
            pump { ot("OTListen", listener, heard) == 0 }
            #expect(ot("OTSndDisconnect", listener, heard) == 0)
            pump { ot("OTLook", client) == 0x0010 }
            let discon = tb_NewPtr(20, false, true)
            #expect(ot("OTRcvDisconnect", client, discon) == 0 && err(rd32(discon + 12)) == -3260) // ECONNREFUSED
            for ep in [listener, worker, client] { #expect(ot("OTCloseProvider", ep) == 0) }
        }

        /// ASP's OpenSess as asp.c (the AppleShare client) sends it: the
        /// function, the workstation's session socket, version 1.0.
        @Test func aspWire() {
            start()
            let server = open("atp"), asp = open("asp")
            let sls = bind(server)
            let wss = bind(asp)
            let c = tb_NewPtr(40, false, true)
            wr32(c + 0, 8); wr32(c + 4, 8); wr32(c + 8, ddp(sls))
            #expect(err(ot("OTConnect", asp, c, 0)) == -3162)
            let open = hear(server, tb_NewPtr(40, false, true))
            #expect(open == [4, wss, 0x01, 0x00])
            #expect(ot("OTCloseProvider", asp) == 0 && ot("OTCloseProvider", server) == 0)
        }

        /// ASP both ends: the status, a session, a command and its reply
        /// (the result in the first four bytes), an attention from the
        /// server, and the workstation closing the session.
        @Test func aspSession() {
            start()
            let listener = listen("asp", as: "Server:AFPServer@*")
            let worker = open("asp"), client = open("asp")
            _ = bind(client)

            let sreq = call(to: nbpAddress("Server:AFPServer@*"))
            #expect(ot("OTSndURequest", client, sreq, 0) == 0)
            let status = tb_NewPtr(28, false, true), flags = tb_NewPtr(4, false, true)
            netbuf(status + 12, max: 64)
            var r = UInt32(0)
            pump { r = ot("OTRcvUReply", client, status, flags); return r == 0 }
            #expect(r == 0 && rd32(status + 24) == rd32(sreq + 36))

            #expect(err(ot("OTConnect", client, call(to: nbpAddress("Server:AFPServer@*")), 0)) == -3162)
            let heard = tb_NewPtr(40, false, true)
            netbuf(heard, max: 8)
            pump { ot("OTListen", listener, heard) == 0 }
            #expect(ot("OTAccept", listener, worker, heard) == 0)
            pump { ot("OTRcvConnect", client, 0) == 0 }
            #expect(ot("OTGetEndpointState", client) == 5)

            let req = tb_NewPtr(28, false, true)
            netbuf(req, max: 16, text: "GetParms")
            #expect(ot("OTSndRequest", client, req, 0) == 0)
            let seq = rd32(req + 24)
            let got = tb_NewPtr(28, false, true)
            netbuf(got, max: 600)
            pump { ot("OTLook", worker) == 0x0400 } // T_REQUEST
            #expect(ot("OTRcvRequest", worker, got, flags) == 0)
            #expect(bytes(rd32(got + 8), Int(rd32(got + 4))) == "GetParms")

            let rep = tb_NewPtr(28, false, true)
            let body = netbuf(rep, max: 16)
            for (i, b) in Array("\u{0}\u{0}\u{0}\u{0}params".utf8).enumerated() { wr8(body + UInt32(i), b) }
            wr32(rep + 4, 10)
            wr32(rep + 24, rd32(got + 24))
            #expect(ot("OTSndReply", worker, rep, 0) == 0)
            let back = tb_NewPtr(28, false, true)
            netbuf(back, max: 64)
            pump { r = ot("OTRcvReply", client, back, flags); return r == 0 }
            #expect(r == 0 && rd32(back + 24) == seq && rd32(back + 4) == 10)
            #expect(rd32(rd32(back + 8)) == 0 && bytes(rd32(back + 8) + 4, 6) == "params")

            let code = tb_NewPtr(2, false, true)
            wr16(code, 0x8001)
            #expect(ot("OTSnd", worker, code, 2, 0x0002) == 2) // an attention
            pump { ot("OTLook", client) == 0x0008 }
            let buf = tb_NewPtr(8, false, true)
            #expect(ot("OTRcv", client, buf, 8, flags) == 2 && rd16(buf) == 0x8001 && rd32(flags) == 0x0002)

            #expect(ot("OTSndDisconnect", client, 0) == 0)
            #expect(ot("OTGetEndpointState", client) == 2)
            pump { ot("OTLook", worker) == 0x0010 } // T_DISCONNECT
            #expect(ot("OTRcvDisconnect", worker, 0) == 0)
            #expect(ot("OTGetEndpointState", worker) == 2)
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

        /// OTOptionManagement on an AppleTalk endpoint: a request, its options.
        func negotiate(_ ep: UInt32, level: UInt32, name: UInt32, value: UInt32, size: Int = 4) -> (UInt32, UInt32) {
            let n = 16 + ((size + 3) & ~3)
            let buf = tb_NewPtr(Int32(n), false, true), out = tb_NewPtr(64, false, true)
            wr32(buf, UInt32(16 + size)); wr32(buf + 4, level); wr32(buf + 8, name)
            if size == 1 { wr8(buf + 16, UInt8(value)) } else { wr32(buf + 16, value) }
            let req = tb_NewPtr(16, false, true), ret = tb_NewPtr(16, false, true)
            wr32(req, UInt32(n)); wr32(req + 4, UInt32(n)); wr32(req + 8, buf); wr32(req + 12, 4) // T_NEGOTIATE
            wr32(ret, 64); wr32(ret + 8, out)
            #expect(ot("OTOptionManagement", ep, req, ret) == 0)
            return (rd32(ret + 12), size == 1 ? UInt32(rd8(out + 16)) : rd32(out + 16))
        }

        /// The options an ATP request takes: retries and the interval between them (a request to
        /// a socket nobody has gives up in a moment instead of Open Transport's 8 times 2 seconds).
        @Test func atpOptions() {
            start()
            let asker = open("atp")
            _ = bind(asker)
            // Defaults.
            #expect(negotiate(asker, level: 0xFFFF, name: 0x601, value: 8) == (0x20, 8))
            // T_SUCCESS (0x20) for the ones ATP has; an ADSP-only option isn't there.
            #expect(negotiate(asker, level: 0xFFFF, name: 0x603, value: 1, size: 1).0 == 0x400) // T_NOTSUPPORT
            #expect(negotiate(asker, level: 0xFFFF, name: 0x601, value: 0).0 == 0x20)
            #expect(negotiate(asker, level: 0xFFFF, name: 0x602, value: 60).0 == 0x20)
            #expect(negotiate(asker, level: 0x4154_5020, name: 0x2110, value: 2).0 == 0x20) // ATP_OPT_REPLYCNT
            let req = tb_NewPtr(40, false, true)
            wr32(req + 0, 8); wr32(req + 4, 8); wr32(req + 8, ddp(77))
            netbuf(req + 24, max: 16, text: "USERnobody")
            #expect(ot("OTSndURequest", asker, req, 0) == 0)
            let back = tb_NewPtr(28, false, true), flags = tb_NewPtr(4, false, true)
            netbuf(back + 12, max: 64)
            var r = UInt32(0)
            for _ in 0..<2000 {
                r = ot("OTRcvUReply", asker, back, flags)
                if r != UInt32(bitPattern: -3162) { break }
                usleep(1000)
                appletalk_poll()
            }
            #expect(err(r) == -3259) // kETIMEDOUTErr, after one try of 60 ms
            #expect(ot("OTCloseProvider", asker) == 0)
        }

        @Test func adspOptions() {
            start()
            let a = open("adsp")
            #expect(negotiate(a, level: 0xFFFF, name: 0x603, value: 0, size: 1) == (0x20, 0)) // OPT_ENABLEEOM, off
            #expect(negotiate(a, level: 0xFFFF, name: 0x603, value: 1, size: 1) == (0x20, 1))
            #expect(negotiate(a, level: 0xFFFF, name: 0x602, value: 1).0 == 0x400) // ADSP has no interval
            #expect(ot("OTCloseProvider", a) == 0)
        }
    }
}
