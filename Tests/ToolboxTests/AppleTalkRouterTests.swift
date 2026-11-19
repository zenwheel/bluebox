import CCore
import Darwin
import Foundation
import Testing
import Toolbox

/// A DDP datagram as a router sees it (a short header's networks are 0).
struct FakeDDP {
    var dnet: UInt16 = 0, snet: UInt16 = 0
    var dnode: UInt8 = 0, snode: UInt8 = 0
    var dskt: UInt8 = 0, sskt: UInt8 = 0, type: UInt8 = 0
    var data: [UInt8] = []
    var long = true
    var checksumOK = true
    /// Ethernet only: the zone multicast address to send it to.
    var multicast: [UInt8]? = nil
}

func ddpChecksum(_ b: ArraySlice<UInt8>) -> UInt16 {
    var sum: UInt32 = 0
    for x in b {
        sum = (sum + UInt32(x)) & 0xFFFF
        sum = (sum << 1 | sum >> 15) & 0xFFFF
    }
    return sum == 0 ? 0xFFFF : UInt16(sum)
}

func pascalBytes(_ s: String) -> [UInt8] { [UInt8(s.utf8.count)] + Array(s.utf8) }

/// A long DDP header and its data.
func longDDP(_ d: FakeDDP) -> [UInt8] {
    let len = 13 + d.data.count
    return [UInt8(len >> 8), UInt8(len & 0xFF), 0, 0, UInt8(d.dnet >> 8), UInt8(d.dnet & 0xFF), UInt8(d.snet >> 8),
            UInt8(d.snet & 0xFF), d.dnode, d.snode, d.dskt, d.sskt, d.type] + d.data
}

func parseLongDDP(_ p: [UInt8]) -> FakeDDP? {
    guard p.count >= 13 else { return nil }
    let len = min((Int(p[0] & 3) << 8 | Int(p[1])), p.count)
    var d = FakeDDP()
    d.dnet = UInt16(p[4]) << 8 | UInt16(p[5])
    d.snet = UInt16(p[6]) << 8 | UInt16(p[7])
    d.dnode = p[8]
    d.snode = p[9]
    d.dskt = p[10]
    d.sskt = p[11]
    d.type = p[12]
    d.data = Array(p[13..<max(13, len)])
    let sum = UInt16(p[2]) << 8 | UInt16(p[3])
    d.checksumOK = sum == 0 || ddpChecksum(p[4..<max(13, len)]) == sum
    return d
}

/// The protocols of a router, apart from its link: RTMP, ZIP (GetNetInfo,
/// and GetMyZone, GetZoneList and GetLocalZones through ATP) and NBP's
/// broadcast requests, answered from a node "far away" (network 9, node
/// 42, in "Far Zone") or as a lookup on this network for its own zone.
final class FakeRouterBrain {
    let net: UInt16, node: UInt8
    let lo: UInt16, hi: UInt16
    let extended: Bool
    let zone: String
    let zones: [String]
    init(net: UInt16, node: UInt8, lo: UInt16, hi: UInt16, extended: Bool, zone: String, zones: [String]) {
        self.net = net
        self.node = node
        self.lo = lo
        self.hi = hi
        self.extended = extended
        self.zone = zone
        self.zones = zones
    }

    var rtmp: [UInt8] {
        var d = [UInt8(net >> 8), UInt8(net & 0xFF), 8, node]
        d += extended ? [UInt8(lo >> 8), UInt8(lo & 0xFF), 0x80, UInt8(hi >> 8), UInt8(hi & 0xFF), 0x82] : [0, 0, 0x82]
        return d
    }

    func rtmpBroadcast() -> FakeDDP {
        FakeDDP(dnet: 0, snet: net, dnode: 255, snode: node, dskt: 1, sskt: 1, type: 1, data: rtmp, long: extended)
    }

    func multicast(_ zone: String) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: 6)
        let z = Array(zone.utf8)
        z.withUnsafeBufferPointer { at_zone_multicast($0.baseAddress, Int32(z.count), &out) }
        return out
    }

    /// What it answers to a datagram for it.
    func answer(_ d: FakeDDP) -> [FakeDDP] {
        let reply = { (dskt: UInt8, type: UInt8, data: [UInt8]) in
            FakeDDP(dnet: d.snet, snet: self.net, dnode: d.snode, snode: self.node, dskt: d.sskt == 0 ? dskt : d.sskt,
                    sskt: dskt, type: type, data: data, long: self.extended)
        }
        switch (d.dskt, d.type) {
        case (1, 5): return [reply(1, 1, rtmp)]
        case (6, 6) where d.data.first == 5 && d.data.count >= 7: // GetNetInfo
            let asked = String(decoding: d.data[7..<min(d.data.count, 7 + Int(d.data[6]))], as: UTF8.self)
            let valid = zones.contains(asked)
            var r: [UInt8] = [6, valid ? 0 : 0x80, UInt8(lo >> 8), UInt8(lo & 0xFF), UInt8(hi >> 8), UInt8(hi & 0xFF)]
            r += pascalBytes(asked) + [6] + multicast(valid ? asked : zone)
            if !valid { r += pascalBytes(zone) }
            return [FakeDDP(dnet: 0, snet: net, dnode: 255, snode: node, dskt: 6, sskt: 6, type: 6, data: r)]
        case (6, 3) where d.data.count >= 8 && d.data[0] & 0xC0 == 0x40: // ZIP through ATP
            let fn = d.data[4], start = Int(d.data[6]) << 8 | Int(d.data[7])
            let names: [String] = fn == 7 ? [zone] : fn == 9 ? (extended ? zones : [zone]) : zones + ["Far Zone"]
            let given = fn == 7 ? names : Array(names.dropFirst(max(0, start - 1)))
            let body = given.flatMap(pascalBytes)
            return [reply(6, 3, [0x90, 0, d.data[2], d.data[3], 1, 0, 0, UInt8(given.count)] + body)]
        case (2, 2) where d.data.count >= 7 && d.data[0] >> 4 == 1: // BrRq
            let id = d.data[1]
            let t = Array(d.data[2...])
            var k = 5, parts = [[UInt8]]()
            for _ in 0..<3 where k < t.count {
                parts.append(Array(t[(k + 1)..<min(t.count, k + 1 + Int(t[k]))]))
                k += 1 + Int(t[k])
            }
            guard parts.count == 3 else { return [] }
            let tnet = UInt16(t[0]) << 8 | UInt16(t[1])
            let zoneAsked = String(decoding: parts[2], as: UTF8.self)
            if zoneAsked == "Far Zone" { // a node on another network answers
                let tuple: [UInt8] = [0, 9, 42, 200, 0] + pascalBytes("Far") + pascalBytes("Thing") + pascalBytes("Far Zone")
                return [FakeDDP(dnet: tnet, snet: 9, dnode: t[2], snode: 42, dskt: t[3], sskt: 2, type: 2,
                                data: [0x31, id] + tuple)]
            }
            if zoneAsked == zone || (!extended && zoneAsked == "*") { // a lookup on this network
                var l = FakeDDP(dnet: 0, snet: net, dnode: 255, snode: node, dskt: 2, sskt: 2, type: 2,
                                data: [0x21, id] + t, long: extended)
                l.multicast = extended ? multicast(zone) : nil
                return [l]
            }
            return []
        default: return []
        }
    }
}

/// A router on the other end of a socket pair, on its own thread: the
/// frames it hears are kept, its answers sent at once, RTMP broadcast now
/// and then.  `decode` takes a frame to the datagram for the router (or
/// answers the link's own requests itself); `encode` makes a frame.
final class FakeRouterLink {
    let fd: Int32
    let brain: FakeRouterBrain
    let lock = NSLock()
    private var frames = [[UInt8]]()
    private var datagrams = [FakeDDP]()
    var decode: (FakeRouterLink, [UInt8]) -> FakeDDP? = { _, _ in nil }
    var encode: (FakeRouterLink, FakeDDP) -> [UInt8] = { _, _ in [] }
    private var thread: StubThread!

    init(fd: Int32, brain: FakeRouterBrain) {
        self.fd = fd
        self.brain = brain
    }

    func start() {
        thread = StubThread { [unowned self] t in
            var buf = [UInt8](repeating: 0, count: 2048)
            var last = Date.distantPast
            while !t.stop {
                var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
                if poll(&pfd, 1, 10) > 0 {
                    let n = recv(fd, &buf, buf.count, 0)
                    if n > 0 {
                        let f = Array(buf[0..<n])
                        lock.withLock { frames.append(f) }
                        if let d = decode(self, f) {
                            lock.withLock { datagrams.append(d) }
                            for a in brain.answer(d) { send(a) }
                        }
                    }
                }
                if Date().timeIntervalSince(last) > 0.2 {
                    last = Date()
                    send(brain.rtmpBroadcast())
                }
            }
        }
    }

    func send(_ d: FakeDDP) { sendFrame(encode(self, d)) }
    func sendFrame(_ f: [UInt8]) { _ = f.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, f.count, 0) } }
    func heard() -> [FakeDDP] { lock.withLock { datagrams } }
    func finish() {
        thread.finish()
        close(fd)
    }
}

extension BlueBox {
    /// A node on a network with a router (Inside AppleTalk, 2nd edition):
    /// THIS-NET and A-ROUTER from RTMP, ZIP's calls to the router, NBP in
    /// other zones through broadcast requests, and long headers through
    /// the router to and from another network; on LocalTalk over UDP and on
    /// EtherTalk, each with a fake router on a socket pair.
    @Suite struct AppleTalkRouting {
        func open(_ name: String) -> Int16 {
            let pb = tb_NewPtr(80, false, true), np = tb_NewPtr(8, false, true)
            guestPString(name, at: np)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), np)
            #expect(tb_PBOpen(pb, false) == 0)
            return Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off)))
        }

        func call(_ ref: Int16, _ code: Int, _ pb: UInt32) -> Int16 {
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: ref))
            wr16(pb + UInt32(CntrlParam_csCode_off), UInt16(code))
            return driver_call(pb, false, Int32(DRIVER_CONTROL))
        }

        /// Polls AppleTalk until cond holds (or a generous while passes).
        @discardableResult
        func pollUntil(_ cond: () -> Bool) -> Bool {
            let until = Date().addingTimeInterval(10)
            while !cond() && Date() < until {
                usleep(2000)
                appletalk_poll()
            }
            return cond()
        }

        func entity(_ parts: [String], at p: UInt32) {
            var q = p
            for s in parts {
                guestPString(s, at: q)
                q += UInt32(s.utf8.count + 1)
            }
        }

        /// lookupName (synchronous): the tuples found.
        func lookup(_ mpp: Int16, _ parts: [String]) -> [(net: UInt16, node: UInt8, skt: UInt8, name: String)] {
            let pb = tb_NewPtr(80, false, true), want = tb_NewPtr(120, false, true), ret = tb_NewPtr(400, false, true)
            entity(parts, at: want)
            wr8(pb + 28, 2)  // interval
            wr8(pb + 29, 4)  // count
            wr32(pb + 30, want)
            wr32(pb + 34, ret)
            wr16(pb + 38, 400)
            wr16(pb + 40, 1) // the first answer will do
            #expect(call(mpp, 251, pb) == 0)
            var out = [(net: UInt16, node: UInt8, skt: UInt8, name: String)]()
            var p = ret
            for _ in 0..<Int(rd16(pb + 42)) {
                out.append((rd16(p), rd8(p + 2), rd8(p + 3), pstring(at: p + 5)))
                var q = p + 5
                for _ in 0..<3 { q += UInt32(rd8(q)) + 1 }
                p = q
            }
            return out
        }

        /// ZIP's xCall on .XPP: the zones in its buffer.
        func zip(_ xpp: Int16, _ subcode: UInt16) -> [String] {
            let x = tb_NewPtr(80, false, true), buf = tb_NewPtr(600, false, true)
            wr16(x + 28, subcode)
            wr32(x + 34, buf)
            wr16(x + 42, 0)
            #expect(call(xpp, 246, x) == 0)
            var names = [String](), p = buf
            for _ in 0..<Int(rd16(x + 38)) {
                names.append(pstring(at: p))
                p += UInt32(rd8(p)) + 1
            }
            #expect(rd8(x + 40) != 0) // the last of them
            return names
        }

        func register(_ mpp: Int16, _ parts: [String]) {
            let pb = tb_NewPtr(80, false, true), nte = tb_NewPtr(120, false, true)
            wr8(nte + 7, 0x81)
            entity(parts, at: nte + 9)
            wr32(pb + 30, nte)
            #expect(call(mpp, 253, pb) == 0)
        }

        @Test func localTalkThroughARouter() {
            var fds: [Int32] = [0, 0]
            #expect(socketpair(AF_UNIX, SOCK_DGRAM, 0, &fds) == 0)
            #expect(machine_set_appletalk("local"))
            launchTestApp()
            at_use_fake(fds[0])
            let brain = FakeRouterBrain(net: 7, node: 254, lo: 7, hi: 7, extended: false, zone: "Router Zone",
                                        zones: ["Router Zone", "Near Zone"])
            let router = FakeRouterLink(fd: fds[1], brain: brain)
            let id: [UInt8] = [0x52, 0x54, 0x52, 0x21]
            router.decode = { link, f in
                guard f.count >= 7 else { return nil }
                if f[6] == 0x81 && f[4] == 254 { // an enquiry for the router's node
                    link.sendFrame(id + [254, 254, 0x82])
                    return nil
                }
                guard f[4] == 254 || f[4] == 255 else { return nil }
                if f[6] == 1, f.count >= 12 { // short header
                    return FakeDDP(dnet: 0, snet: 0, dnode: f[4], snode: f[5], dskt: f[9], sskt: f[10], type: f[11],
                                   data: Array(f[12...]), long: false)
                }
                return f[6] == 2 ? parseLongDDP(Array(f[7...])) : nil
            }
            router.encode = { _, d in
                if !d.long {
                    let len = 5 + d.data.count
                    return id + [d.dnode, 254, 1, UInt8(len >> 8), UInt8(len & 0xFF), d.dskt, d.sskt, d.type] + d.data
                }
                return id + [d.dnode, 254, 2] + longDDP(d)
            }
            router.start()
            defer { router.finish(); machine_set_appletalk("local") }
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16

            let mpp = open(".MPP")
            var rnet: UInt16 = 0
            var rnode: UInt8 = 0
            #expect(pollUntil { at_router(&rnet, &rnode) })
            #expect(rnet == 7 && rnode == 254)
            let vars = rd32(UInt32(LM_ABusVars)), node = rd8(vars)
            #expect(rd16(vars + 0x1A) == 7) // THIS-NET, where programs read it
            #expect(rd8(vars + 0x19) == 254) // the bridge's node

            let xpp = open(".XPP")
            #expect(zip(xpp, 7) == ["Router Zone"])
            #expect(zip(xpp, 6) == ["Router Zone", "Near Zone", "Far Zone"])

            // Another zone's names: a broadcast request to the router, the
            // answer from another network.
            let far = lookup(mpp, ["=", "Thing", "Far Zone"])
            #expect(far.count == 1 && far.first?.net == 9 && far.first?.node == 42 && far.first?.skt == 200)
            #expect(router.heard().contains { $0.type == 2 && $0.dnode == 254 && $0.data.first.map { $0 >> 4 } == 1 })

            // This zone by name: the router has the network look, we answer.
            register(mpp, ["Me", "Thing", "*"])
            let mine = lookup(mpp, ["=", "Thing", "Router Zone"])
            #expect(mine.count == 1 && mine.first?.net == 7 && mine.first?.node == node && mine.first?.skt == 0x81)

            // An echo request from another network: the reply goes back
            // through the router, with a long header and its checksum.
            router.send(FakeDDP(dnet: 7, snet: 9, dnode: node, snode: 42, dskt: 4, sskt: 0x80, type: 4, data: [1, 0x55]))
            #expect(pollUntil { router.heard().contains { $0.type == 4 && $0.dnet == 9 && $0.dnode == 42 } })
            let echo = router.heard().first { $0.type == 4 && $0.dnet == 9 }
            #expect(echo?.long == true && echo?.checksumOK == true && echo?.snet == 7 && echo?.snode == node)
            #expect(echo?.dskt == 0x80 && echo?.data == [2, 0x55])
        }

        @Test func etherTalkThroughARouter() {
            var fds: [Int32] = [0, 0]
            #expect(socketpair(AF_UNIX, SOCK_DGRAM, 0, &fds) == 0)
            let ours: [UInt8] = [0x02, 0, 0, 0, 0, 0x01], routerMac: [UInt8] = [0x02, 0, 0, 0, 0, 0xFE]
            let bcast: [UInt8] = [0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF]
            let snapDDP: [UInt8] = [0xAA, 0xAA, 0x03, 0x08, 0x00, 0x07, 0x80, 0x9B]
            let snapAARP: [UInt8] = [0xAA, 0xAA, 0x03, 0, 0, 0, 0x80, 0xF3]
            #expect(machine_set_appletalk("bridge:fake0"))
            machine_set_appletalk_zone("")
            launchTestApp()
            et_use_fake(fds[0], ours)
            let brain = FakeRouterBrain(net: 100, node: 254, lo: 100, hi: 101, extended: true, zone: "Ether Zone",
                                        zones: ["Ether Zone", "Other Zone"])
            let router = FakeRouterLink(fd: fds[1], brain: brain)
            func frame(_ dst: [UInt8], _ snap: [UInt8], _ p: [UInt8]) -> [UInt8] {
                let len = snap.count + p.count
                return dst + routerMac + [UInt8(len >> 8), UInt8(len & 0xFF)] + snap + p
            }
            router.decode = { link, f in
                guard f.count >= 22 else { return nil }
                let dst = Array(f[0..<6]), p = Array(f[22...])
                if Array(f[14..<22]) == snapAARP, p.count >= 28, p[7] == 1, p[25] == 0, p[26] == 100, p[27] == 254 {
                    // a request for the router's address
                    let r: [UInt8] = [0, 1, 0x80, 0x9B, 6, 4, 0, 2] + routerMac + [0, 0, 100, 254] + Array(p[8..<18])
                    link.sendFrame(frame(Array(p[8..<14]), snapAARP, r))
                    return nil
                }
                guard Array(f[14..<22]) == snapDDP, dst == routerMac || dst == bcast else { return nil }
                return parseLongDDP(p)
            }
            router.encode = { _, d in
                let dst = d.multicast ?? (d.dnode == 255 ? bcast : ours)
                return frame(dst, snapDDP, longDDP(d))
            }
            router.start()
            defer { router.finish(); machine_set_appletalk("local") }
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16

            // A network in the router's range, and this zone (the default:
            // none was asked for) from GetNetInfo, with its multicast address.
            let mpp = open(".MPP")
            let vars = rd32(UInt32(LM_ABusVars)), node = rd8(vars)
            #expect(pollUntil { var n: UInt16 = 0; var r: UInt8 = 0; return at_router(&n, &r) })
            let net = rd16(vars + 0x1A)
            #expect(net >= 100 && net <= 101)
            #expect(router.heard().contains { $0.type == 6 && $0.data.first == 5 }) // GetNetInfo
            let xpp = open(".XPP")
            #expect(zip(xpp, 7) == ["Ether Zone"])
            #expect(zip(xpp, 5) == ["Ether Zone", "Other Zone"]) // GetLocalZones
            #expect(zip(xpp, 6) == ["Ether Zone", "Other Zone", "Far Zone"])
            // as TashRouter computes them
            #expect(brain.multicast("Ether Zone") == [0x09, 0x00, 0x07, 0x00, 0x00, 0xB7])
            #expect(brain.multicast("ether zone") == [0x09, 0x00, 0x07, 0x00, 0x00, 0xB7])
            #expect(brain.multicast("Other Zone") == [0x09, 0x00, 0x07, 0x00, 0x00, 0x32])

            let far = lookup(mpp, ["=", "Thing", "Far Zone"])
            #expect(far.count == 1 && far.first?.net == 9 && far.first?.node == 42)

            // "*" is this zone by name for the router (an extended network's
            // router can't tell which of its zones is meant).
            register(mpp, ["Me", "Thing", "*"])
            let mine = lookup(mpp, ["=", "Thing", "*"])
            #expect(mine.count == 1 && mine.first?.net == net && mine.first?.node == node)
            let anyone = pascalBytes("=") + pascalBytes("Nobody") + pascalBytes("*")
            let h = anyone.withUnsafeBufferPointer { at_nbp_start($0.baseAddress, Int32($0.count), 1, 100, nil, nil) }
            #expect(pollUntil { router.heard().contains {
                $0.type == 2 && $0.data.first.map { $0 >> 4 } == 1 && $0.data.suffix(11) == pascalBytes("Ether Zone")[...]
            } })
            at_nbp_stop(h)

            // Another zone's multicast isn't ours: a lookup sent to it goes
            // unanswered; the same to this zone's is answered.
            let tuple: [UInt8] = [0, 100, 254, 2, 0] + pascalBytes("=") + pascalBytes("Thing") + pascalBytes("*")
            for (id, zone) in [(UInt8(0x61), "Other Zone"), (UInt8(0x62), "Ether Zone")] {
                var l = FakeDDP(dnet: 0, snet: 100, dnode: 255, snode: 254, dskt: 2, sskt: 2, type: 2, data: [0x21, id] + tuple)
                l.multicast = brain.multicast(zone)
                router.send(l)
            }
            #expect(pollUntil { router.heard().contains { $0.type == 2 && $0.data.prefix(2) == [0x31, 0x62] } })
            #expect(!router.heard().contains { $0.type == 2 && $0.data.prefix(2) == [0x31, 0x61] })

            // A datagram to another network goes to the router's address.
            router.send(FakeDDP(dnet: net, snet: 9, dnode: node, snode: 42, dskt: 4, sskt: 0x80, type: 4, data: [1, 7]))
            #expect(pollUntil { router.heard().contains { $0.type == 4 && $0.dnet == 9 && $0.dnode == 42 } })
            #expect(router.heard().first { $0.type == 4 && $0.dnet == 9 }?.checksumOK == true)
        }
    }
}

/// A router's LToUDP hub on 127.0.0.1, by unicast: what Blue Box sends it,
/// and another node (77) on its segment that answers enquiries and NBP.
final class FakeHub {
    let fd: Int32
    var port: UInt16 = 0
    let lock = NSLock()
    private var got = [[UInt8]]()
    private var client = sockaddr_in()
    private var haveClient = false
    private var thread: StubThread!
    static let peerID: [UInt8] = [0x0B, 0xAD, 0xF0, 0x0D]

    init() {
        fd = socket(AF_INET, SOCK_DGRAM, 0)
        var sa = sockaddr_in()
        sa.sin_family = sa_family_t(AF_INET)
        sa.sin_addr.s_addr = inet_addr("127.0.0.1")
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        withUnsafeMutablePointer(to: &sa) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                _ = bind(fd, $0, len)
                _ = getsockname(fd, $0, &len)
            }
        }
        port = UInt16(bigEndian: sa.sin_port)
        thread = StubThread { [unowned self] t in
            var buf = [UInt8](repeating: 0, count: 2048)
            while !t.stop {
                var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
                guard poll(&pfd, 1, 10) > 0 else { continue }
                var from = sockaddr_in()
                var fl = socklen_t(MemoryLayout<sockaddr_in>.size)
                let n = withUnsafeMutablePointer(to: &from) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buf, buf.count, 0, $0, &fl) }
                }
                guard n >= 4 else { continue }
                let d = Array(buf[0..<n])
                lock.withLock {
                    got.append(d)
                    client = from
                    haveClient = true
                }
                peer(d)
            }
        }
    }

    func send(_ d: [UInt8]) {
        guard var to = lock.withLock({ haveClient ? client : nil }) else { return }
        _ = d.withUnsafeBytes { p in
            withUnsafePointer(to: &to) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, p.baseAddress, d.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    /// Node 77 on the hub's segment.
    func peer(_ d: [UInt8]) {
        guard d.count >= 7 else { return }
        let f = Array(d[4...])
        if f[2] == 0x81 && f[0] == 77 {
            send(Self.peerID + [77, 77, 0x82])
        } else if f[2] == 1, f.count >= 8, f[0] == 255 || f[0] == 77, f[5] == 2, f[7] == 2, f[8] >> 4 == 2 { // LkUp
            let lk = Array(f[8...])
            let tuple: [UInt8] = [0, 0, 77, 0x90, 0] + pascalBytes("Peer") + pascalBytes("Thing") + pascalBytes("*")
            let data: [UInt8] = [0x31, lk[1]] + tuple
            let len = 5 + data.count
            if lk.count > 7, lk[4] != 77, String(decoding: lk[7...], as: UTF8.self).contains("Thing") {
                send(Self.peerID + [lk[4], 77, 1, UInt8(len >> 8), UInt8(len & 0xFF), lk[5], 2, 2] + data)
            }
        }
    }

    func heard() -> [[UInt8]] { lock.withLock { got } }
    func finish() {
        thread.finish()
        close(fd)
    }
}

extension BlueBox.AppleTalkRouting {
    /// --appletalk router:HOST:PORT: LToUDP by unicast to a hub, a
    /// keepalive first and when idle, our own datagrams coming back
    /// ignored, and NBP with a node on the hub's segment both ways.
    @Test func throughARouterHub() {
        let hub = FakeHub()
        defer { hub.finish(); machine_set_appletalk("local"); at_set_keepalive_ms(0) }
        #expect(machine_set_appletalk("router:127.0.0.1:\(hub.port)"))
        at_set_keepalive_ms(200)
        launchTestApp()
        let stack = tb_NewPtr(65536, false, true)
        g_cpu!.pointee.a.7 = stack + 65536 - 16
        let mpp = open(".MPP")
        let node = rd8(rd32(UInt32(LM_ABusVars)))
        #expect(node >= 1 && node <= 254 && node != 77)
        // the keepalive (the sender ID alone) before the first enquiry
        let first = hub.heard().first
        #expect(first?.count == 4)
        let ours = first ?? []
        #expect(hub.heard().dropFirst().first.map { $0.count == 7 && $0[6] == 0x81 } == true)

        // Node 77's name found, and ours found by it.
        let found = lookup(mpp, ["=", "Thing", "*"])
        #expect(found.contains { $0.node == 77 && $0.skt == 0x90 && $0.name == "Peer" })
        register(mpp, ["Me", "Thing", "*"])
        let t: [UInt8] = [0, 0, 77, 2, 0] + pascalBytes("=") + pascalBytes("Thing") + pascalBytes("*")
        let lk: [UInt8] = [0x21, 0x44] + t
        hub.send(FakeHub.peerID + [255, 77, 1, 0, UInt8(5 + lk.count), 2, 2, 2] + lk)
        #expect(pollUntil { hub.heard().contains { $0.count > 12 && $0[4] == 77 && $0[6] == 1 && $0[12] == 0x31 && $0[13] == 0x44 } })

        // Our own datagram repeated back is ignored; another's is answered.
        hub.send(ours + [node, node, 0x81])
        hub.send(FakeHub.peerID + [node, node, 0x81])
        #expect(pollUntil { hub.heard().filter { $0.count == 7 && $0[6] == 0x82 && $0[4] == node }.count >= 1 })
        for _ in 0..<20 {
            usleep(5000)
            appletalk_poll()
        }
        #expect(hub.heard().filter { $0.count == 7 && $0[6] == 0x82 && $0[4] == node }.count == 1)

        // Idle: keepalives again.
        let before = hub.heard().filter { $0.count == 4 }.count
        #expect(pollUntil { hub.heard().filter { $0.count == 4 }.count >= before + 2 })
        #expect(hub.heard().filter { $0.count == 4 }.allSatisfy { $0 == ours })
    }
}
