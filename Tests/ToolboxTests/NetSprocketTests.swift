import CCore
import CPUPPC
import Darwin
import Testing
import Toolbox

extension BlueBox {
    /// NetSprocket (NetSprocketLib) called as a PowerPC game calls it: a
    /// host and a player in this one run, joined over TCP/IP on the
    /// loopback (and over AppleTalk on its loopback group), messages both
    /// ways, groups, leaving and the end of the game.
    @Suite struct NetSprocket {
        static let hdr: UInt32 = 28
        static let kNSpHostOnly: UInt32 = 0xFFFF_FFFF
        static let kNSpPlayerJoined: UInt32 = 0x8000_0004, kNSpPlayerLeft: UInt32 = 0x8000_0005
        static let kNSpJoinApproved: UInt32 = 0x8000_0002, kNSpGameTerminated: UInt32 = 0x8000_0007
        static let kNSpGroupCreated: UInt32 = 0x8000_0008, kNSpPlayerAddedToGroup: UInt32 = 0x8000_000A

        @discardableResult
        func nsp(_ name: String, _ args: UInt32...) -> UInt32 {
            if args.count > 8 {
                for i in 8..<args.count { wr32(g_ppc.gpr.1 + 24 + UInt32(4 * i), args[i]) }
            }
            withUnsafeMutableBytes(of: &g_ppc.gpr) { raw in
                for (i, a) in args.prefix(8).enumerated() { raw.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("\(name) is not exported")
                return 0xFFFF_FFFF
            }
            fn(&g_ppc)
            return g_ppc.gpr.3
        }

        func err(_ v: UInt32) -> Int32 { Int32(bitPattern: v) }

        func start(appletalk: Bool = false) {
            if appletalk { #expect(machine_set_appletalk("local")) }
            launchTestApp()
            #expect(cfm_ppc_cpu())
            g_ppc.gpr.1 -= 1024 // room for a parameter area above
        }

        func pstr(_ s: String) -> UInt32 {
            let b = Array(s.utf8)
            let p = tb_NewPtr(256, false, true)
            wr8(p, UInt8(b.count))
            for (i, c) in b.enumerated() { wr8(p + 1 + UInt32(i), c) }
            return p
        }

        func pstring(at p: UInt32) -> String {
            String(decoding: (0..<Int(rd8(p))).map { rd8(p + 1 + UInt32($0)) }, as: UTF8.self)
        }

        func cstr(_ s: String) -> UInt32 {
            let b = Array(s.utf8)
            let p = tb_NewPtr(Int32(b.count + 1), false, true)
            for (i, c) in b.enumerated() { wr8(p + UInt32(i), c) }
            return p
        }

        func cstring(at p: UInt32) -> String {
            var out = [UInt8]()
            var i: UInt32 = 0
            while rd8(p + i) != 0 { out.append(rd8(p + i)); i += 1 }
            return String(decoding: out, as: UTF8.self)
        }

        /// A port of our own for the game, away from other runs'.
        func freePort() -> UInt16 {
            let fd = socket(AF_INET, SOCK_STREAM, 0)
            defer { close(fd) }
            var sa = sockaddr_in()
            sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_addr.s_addr = inet_addr("127.0.0.1")
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            withUnsafeMutablePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    _ = bind(fd, $0, len)
                    _ = getsockname(fd, $0, &len)
                }
            }
            return UInt16(bigEndian: sa.sin_port)
        }

        /// Host a game on TCP/IP: the game reference.
        func host(port: UInt16, max: UInt32 = 4, password: String = "", player: String? = "Host") -> UInt32 {
            let proto = nsp("NSpProtocol_CreateIP", UInt32(port), 0, 0)
            #expect(proto != 0)
            let list = tb_NewPtr(4, false, true)
            #expect(nsp("NSpProtocolList_New", proto, list) == 0)
            let out = tb_NewPtr(4, false, true)
            let r = nsp("NSpGame_Host", out, rd32(list), max, pstr("Test Game"), pstr(password),
                        player.map { pstr($0) } ?? 0, 0x1234, 1, 0)
            #expect(r == 0)
            return rd32(out)
        }

        /// An address reference for 127.0.0.1:port.
        func loopback(_ port: UInt16) -> UInt32 {
            let a = tb_NewPtr(16, false, true)
            wr16(a, 2) // AF_INET
            wr16(a + 2, port)
            wr32(a + 4, 0x7F00_0001)
            return nsp("NSpConvertOTAddrToAddressReference", a)
        }

        func join(_ address: UInt32, name: String, password: String = "") -> (Int32, UInt32) {
            let out = tb_NewPtr(4, false, true)
            let r = err(nsp("NSpGame_Join", out, address, pstr(name), pstr(password), 7, 0, 0, 0))
            return (r, rd32(out))
        }

        /// The game's next message (the network run until one comes): its
        /// what, from, and the message itself (released by the caller).
        func next(_ game: UInt32, tries: Int = 2000) -> (what: UInt32, from: UInt32, msg: UInt32)? {
            for _ in 0..<tries {
                let m = nsp("NSpMessage_Get", game)
                if m != 0 { return (rd32(m + 4), rd32(m + 8), m) }
                usleep(1000)
            }
            return nil
        }

        func expectNext(_ game: UInt32, _ what: UInt32, _ comment: Comment? = nil) -> UInt32 {
            guard let m = next(game) else {
                Issue.record("no message: \(comment?.description ?? "")")
                return 0
            }
            #expect(m.what == what, comment)
            return m.msg
        }

        func release(_ game: UInt32, _ msg: UInt32) { nsp("NSpMessage_Release", game, msg) }

        /// Every export Mac OS 9's NetSprocketLib 1.7 has.
        @Test func exportsAll() {
            let names = [
                "GetQState", "NSpClearMessageHeader", "NSpConvertAddressReferenceToOTAddr",
                "NSpConvertOTAddrToAddressReference", "NSpDoModalHostDialog", "NSpDoModalJoinDialog", "NSpGame_Dispose",
                "NSpGame_EnableAdvertising", "NSpGame_GetInfo", "NSpGame_Host", "NSpGame_Join", "NSpGetCurrentTimeStamp",
                "NSpGetVersion", "NSpGroup_AddPlayer", "NSpGroup_Dispose", "NSpGroup_GetEnumeration", "NSpGroup_GetInfo",
                "NSpGroup_New", "NSpGroup_ReleaseEnumeration", "NSpGroup_ReleaseInfo", "NSpGroup_RemovePlayer",
                "NSpInitialize", "NSpInstallAsyncMessageHandler", "NSpInstallCallbackHandler",
                "NSpInstallJoinRequestHandler", "NSpMessage_Get", "NSpMessage_Release", "NSpMessage_Send",
                "NSpMessage_SendTo", "NSpPlayer_ChangeType", "NSpPlayer_GetAddress", "NSpPlayer_GetEnumeration",
                "NSpPlayer_GetInfo", "NSpPlayer_GetMyID", "NSpPlayer_GetRoundTripTime", "NSpPlayer_GetThruput",
                "NSpPlayer_ReleaseEnumeration", "NSpPlayer_ReleaseInfo", "NSpPlayer_Remove", "NSpProtocolList_Append",
                "NSpProtocolList_Dispose", "NSpProtocolList_GetCount", "NSpProtocolList_GetIndexedRef",
                "NSpProtocolList_New", "NSpProtocolList_Remove", "NSpProtocolList_RemoveIndexed",
                "NSpProtocol_CreateAppleTalk", "NSpProtocol_CreateIP", "NSpProtocol_Dispose",
                "NSpProtocol_ExtractDefinitionString", "NSpProtocol_New", "NSpReleaseAddressReference",
                "NSpSetConnectTimeout",
            ]
            for n in names { #expect(cfm_export_fn(n) != nil, "\(n)") }
        }

        /// Protocols and lists: the definition strings Apple's form has,
        /// and back again.
        @Test func protocolsAndLists() {
            start()
            #expect(nsp("NSpInitialize", 128, 65536, 32, 0x5445_5354, 0) == 0)
            #expect(err(nsp("NSpInitialize", 128, 65536, 32, 0x5445_5354, 0)) == -30361) // already
            let ip = nsp("NSpProtocol_CreateIP", 25710, 100, 2000)
            let at = nsp("NSpProtocol_CreateAppleTalk", pstr("Game"), pstr("NSpT"), 50, 300)
            let def = tb_NewPtr(256, false, true)
            #expect(nsp("NSpProtocol_ExtractDefinitionString", ip, def) == 0)
            #expect(cstring(at: def) == "ot:inet$\r100\r2000\r25710")
            #expect(nsp("NSpProtocol_ExtractDefinitionString", at, def) == 0)
            #expect(cstring(at: def) == "ot:atlk$\r50\r300\rGame\rNSpT")
            let out = tb_NewPtr(4, false, true)
            #expect(nsp("NSpProtocol_New", def, out) == 0 && rd32(out) != 0)
            let def2 = tb_NewPtr(256, false, true)
            #expect(nsp("NSpProtocol_ExtractDefinitionString", rd32(out), def2) == 0)
            #expect(cstring(at: def2) == "ot:atlk$\r50\r300\rGame\rNSpT")
            #expect(err(nsp("NSpProtocol_New", cstr("bogus"), out)) == -30390) // kNSpInvalidDefinitionErr

            let list = tb_NewPtr(4, false, true)
            #expect(nsp("NSpProtocolList_New", ip, list) == 0)
            #expect(nsp("NSpProtocolList_Append", rd32(list), at) == 0)
            #expect(nsp("NSpProtocolList_GetCount", rd32(list)) == 2)
            #expect(nsp("NSpProtocolList_GetIndexedRef", rd32(list), 1) == at)
            #expect(nsp("NSpProtocolList_Remove", rd32(list), ip) == 0)
            #expect(nsp("NSpProtocolList_GetIndexedRef", rd32(list), 0) == at)
            #expect(nsp("NSpProtocolList_RemoveIndexed", rd32(list), 0) == 0)
            #expect(nsp("NSpProtocolList_GetCount", rd32(list)) == 0)
            nsp("NSpProtocolList_Dispose", rd32(list))

            let v = tb_NewPtr(4, false, true)
            nsp("NSpGetVersion", v)
            #expect(g_ppc.gpr.3 == 0x0170_8000 || rd32(v) == 0x0170_8000)
            let h = tb_NewPtr(28, false, true)
            for i in 0..<28 { wr8(h + UInt32(i), 0xAA) }
            nsp("NSpClearMessageHeader", h)
            #expect(rd32(h + 4) == 0 && rd32(h + 24) == 28)
        }

        /// A host and a player over TCP/IP on the loopback: the join, its
        /// messages, a message each way, groups, then the player leaving.
        @Test func hostAndJoinOverTCP() {
            start()
            #expect(nsp("NSpInitialize", 256, 131072, 64, 0x5445_5354, 0) == 0)
            let port = freePort()
            let hg = host(port: port)
            #expect(hg != 0)
            #expect(nsp("NSpPlayer_GetMyID", hg) == 1)

            let (r, cg) = join(loopback(port), name: "Guest")
            #expect(r == 0 && cg != 0)
            #expect(nsp("NSpPlayer_GetMyID", cg) == 2)

            // the player: approved, then itself joining
            release(cg, expectNext(cg, Self.kNSpJoinApproved, "approved"))
            let pj = expectNext(cg, Self.kNSpPlayerJoined, "player joined (player)")
            #expect(rd32(pj + Self.hdr) == 2 && rd32(pj + Self.hdr + 4) == 2)
            #expect(pstring(at: pj + Self.hdr + 12) == "Guest")
            release(cg, pj)
            // the host hears of the player
            let hj = expectNext(hg, Self.kNSpPlayerJoined, "player joined (host)")
            #expect(rd32(hj + Self.hdr + 4) == 2 && rd32(hj + Self.hdr + 8) == 7)
            release(hg, hj)

            // the game as each sees it
            let info = tb_NewPtr(84, false, true)
            #expect(nsp("NSpGame_GetInfo", cg, info) == 0)
            #expect(rd32(info) == 4 && rd32(info + 4) == 2 && rd32(info + 12) == 1)
            #expect(pstring(at: info + 20) == "Test Game")
            let en = tb_NewPtr(4, false, true)
            #expect(nsp("NSpPlayer_GetEnumeration", cg, en) == 0)
            #expect(rd32(rd32(en)) == 2)
            nsp("NSpPlayer_ReleaseEnumeration", cg, rd32(en))
            let pi = tb_NewPtr(4, false, true)
            #expect(nsp("NSpPlayer_GetInfo", cg, 1, pi) == 0)
            #expect(pstring(at: rd32(pi) + 8) == "Host" && rd32(rd32(pi) + 4) == 0x1234)
            nsp("NSpPlayer_ReleaseInfo", cg, rd32(pi))

            // player to host, with data
            let data = cstr("hello, host")
            #expect(nsp("NSpMessage_SendTo", cg, Self.kNSpHostOnly, 1000, data, 12, 0x0040_0000) == 0)
            let m1 = expectNext(hg, 1000, "player's message")
            #expect(rd32(m1 + 8) == 2 && rd32(m1 + 24) == 28 + 12)
            #expect(cstring(at: m1 + Self.hdr) == "hello, host")
            release(hg, m1)

            // host to everyone, as a whole message
            let msg = tb_NewPtr(40, false, true)
            nsp("NSpClearMessageHeader", msg)
            wr32(msg + 4, 2000)
            wr32(msg + 12, 0) // kNSpAllPlayers
            wr32(msg + 24, 36)
            wr32(msg + 28, 0xCAFE_F00D)
            wr32(msg + 32, 42)
            #expect(nsp("NSpMessage_Send", hg, msg, 0x0020_0000) == 0)
            #expect(rd32(msg + 8) == 1) // filled in: from the host's player
            let m2 = expectNext(cg, 2000, "host's message")
            #expect(rd32(m2 + 8) == 1 && rd32(m2 + 28) == 0xCAFE_F00D && rd32(m2 + 32) == 42)
            release(cg, m2)
            // not to the sender without kNSpSendFlag_SelfSend; with it, too
            #expect(nsp("NSpMessage_Get", hg) == 0)
            #expect(nsp("NSpMessage_Send", hg, msg, 0x0020_0002) == 0)
            release(hg, expectNext(hg, 2000, "self-sent"))
            release(cg, expectNext(cg, 2000, "self-sent, to the player"))

            // a group the player makes, with the player in it
            let gid = tb_NewPtr(4, false, true)
            #expect(nsp("NSpGroup_New", cg, gid) == 0)
            #expect(Int32(bitPattern: rd32(gid)) < -1)
            #expect(nsp("NSpGroup_AddPlayer", cg, rd32(gid), 2) == 0)
            release(hg, expectNext(hg, Self.kNSpGroupCreated, "group created (host)"))
            release(hg, expectNext(hg, Self.kNSpPlayerAddedToGroup, "added (host)"))
            release(cg, expectNext(cg, Self.kNSpGroupCreated, "group created (player)"))
            release(cg, expectNext(cg, Self.kNSpPlayerAddedToGroup, "added (player)"))
            let gi = tb_NewPtr(4, false, true)
            #expect(nsp("NSpGroup_GetInfo", hg, rd32(gid), gi) == 0)
            #expect(rd32(rd32(gi) + 4) == 1 && rd32(rd32(gi) + 8) == 2)
            nsp("NSpGroup_ReleaseInfo", hg, rd32(gi))
            // a message to the group reaches its member
            #expect(nsp("NSpMessage_SendTo", hg, rd32(gid), 3000, 0, 0, 0x0040_0000) == 0)
            release(cg, expectNext(cg, 3000, "to the group"))
            #expect(nsp("NSpPlayer_GetRoundTripTime", hg, 2) < 1000)

            // the player leaves
            #expect(nsp("NSpGame_Dispose", cg, 0) == 0)
            let left = expectNext(hg, Self.kNSpPlayerLeft, "player left")
            #expect(rd32(left + Self.hdr) == 1 && rd32(left + Self.hdr + 4) == 2)
            #expect(pstring(at: left + Self.hdr + 8) == "Guest")
            release(hg, left)
            #expect(nsp("NSpGame_Dispose", hg, 0) == 0)
        }

        /// A wrong password is refused (kNSpJoinFailedErr); a host ending
        /// the game tells its players (kNSpGameTerminated).
        @Test func deniedAndTerminated() {
            start()
            let port = freePort()
            let hg = host(port: port, password: "sesame")
            let (bad, _) = join(loopback(port), name: "Mallory", password: "guess")
            #expect(bad == -30399)
            let (ok, cg) = join(loopback(port), name: "Alice", password: "sesame")
            #expect(ok == 0)
            #expect(nsp("NSpGame_Dispose", hg, 0) == 0)
            var ended = false
            for _ in 0..<5 {
                guard let m = next(cg) else { break }
                if m.what == Self.kNSpGameTerminated { ended = true; break }
                release(cg, m.msg)
            }
            #expect(ended)
            #expect(err(nsp("NSpMessage_SendTo", cg, Self.kNSpHostOnly, 1, 0, 0, 0)) == -30394)
            #expect(nsp("NSpGame_Dispose", cg, 0) == 0)
        }

        /// The program's handlers: one that denies joins with a reason, and
        /// a message handler that sees each message and keeps none.
        @Test func handlers() {
            start()
            // join handler: deny (r3 = 0)
            let deny = routine([0x3860_0000, 0x4E80_0020])
            #expect(nsp("NSpInstallJoinRequestHandler", deny, 0) == 0)
            let port = freePort()
            let hg = host(port: port)
            let (r, _) = join(loopback(port), name: "Bob")
            #expect(r == -30399)
            #expect(nsp("NSpInstallJoinRequestHandler", 0, 0) == 0)
            let (r2, cg) = join(loopback(port), name: "Bob")
            #expect(r2 == 0)
            // message handler: *context = its sender if its what is 77;
            // return false (both games' messages come to it, in this run)
            let seen = tb_NewPtr(4, false, true)
            let handler = routine([0x80C4_0004, 0x2C06_004D, 0x4082_000C, 0x80E4_0008, 0x90E5_0000, 0x3860_0000, 0x4E80_0020])
            #expect(nsp("NSpInstallAsyncMessageHandler", handler, seen) == 0)
            #expect(nsp("NSpMessage_SendTo", cg, Self.kNSpHostOnly, 77, 0, 0, 0) == 0)
            for _ in 0..<2000 where rd32(seen) == 0 {
                usleep(1000)
                netsprocket_poll()
            }
            #expect(rd32(seen) == 2) // from the player
            // none kept for NSpMessage_Get
            var got77 = false
            while let m = next(hg, tries: 50) {
                if m.what == 77 { got77 = true }
                release(hg, m.msg)
            }
            #expect(!got77)
            #expect(nsp("NSpInstallAsyncMessageHandler", 0, 0) == 0)
            nsp("NSpGame_Dispose", cg, 0)
            nsp("NSpGame_Dispose", hg, 0)
        }

        func routine(_ code: [UInt32]) -> UInt32 {
            let p = tb_NewPtr(Int32(code.count * 4), false, true)
            for (i, w) in code.enumerated() { wr32(p + UInt32(i * 4), w) }
            let tv = tb_NewPtr(8, false, true)
            wr32(tv, p)
            return tv
        }

        /// Over AppleTalk: the host's game registered with NBP, joined by
        /// its name, a message across ADSP.
        @Test func overAppleTalk() {
            start(appletalk: true)
            let type = "NSp\(getpid() % 10000)"
            let proto = nsp("NSpProtocol_CreateAppleTalk", pstr("Test Game"), pstr(type), 0, 0)
            let list = tb_NewPtr(4, false, true)
            #expect(nsp("NSpProtocolList_New", proto, list) == 0)
            let out = tb_NewPtr(4, false, true)
            #expect(nsp("NSpGame_Host", out, rd32(list), 2, pstr("Test Game"), pstr(""), pstr("Host"), 0, 1, 0) == 0)
            let hg = rd32(out)
            // an NBP address: "object:type@zone"
            let name = "Test Game:\(type)@*"
            let a = tb_NewPtr(128, false, true)
            wr16(a, 0x0102)
            for (i, c) in name.utf8.enumerated() { wr8(a + 2 + UInt32(i), c) }
            let ref = nsp("NSpConvertOTAddrToAddressReference", a)
            #expect(ref != 0)
            let (r, cg) = join(ref, name: "Talker")
            #expect(r == 0)
            #expect(nsp("NSpMessage_SendTo", cg, Self.kNSpHostOnly, 4242, cstr("over ADSP"), 10, 0x0040_0000) == 0)
            var got = false
            for _ in 0..<6 {
                guard let m = next(hg) else { break }
                if m.what == 4242 {
                    #expect(cstring(at: m.msg + Self.hdr) == "over ADSP")
                    got = true
                }
                release(hg, m.msg)
                if got { break }
            }
            #expect(got)
            nsp("NSpReleaseAddressReference", ref)
            nsp("NSpGame_Dispose", cg, 0)
            nsp("NSpGame_Dispose", hg, 0)
        }

        /// Headless, the join dialog is cancelled and the host dialog too.
        @Test func dialogsHeadless() {
            start()
            #expect(nsp("NSpDoModalJoinDialog", pstr("NSpT"), pstr("Games:"), pstr("Me"), pstr(""), 0) == 0)
            let list = tb_NewPtr(4, false, true)
            #expect(nsp("NSpProtocolList_New", 0, list) == 0)
            #expect(nsp("NSpDoModalHostDialog", rd32(list), pstr("Game"), pstr("Me"), pstr(""), 0) & 0xFF == 0)
        }
    }
}
