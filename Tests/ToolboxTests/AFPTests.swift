import CCore
import Darwin
import Foundation
import Testing
import Toolbox

/// A small AFP 2 server: one volume ("StubVol", ID 1) of a few files and a
/// folder, enough for afp.c's calls.  Names are MacRoman bytes.
final class AFPStub {
    struct Node {
        var name: [UInt8]
        var parent: UInt32
        var dir: Bool
        var data: [UInt8] = []
        var rsrc: [UInt8] = []
        var finder = [UInt8](repeating: 0, count: 32)
    }
    var nodes: [UInt32: Node] = [:]
    var nextID: UInt32 = 100
    var forks: [UInt16: (UInt32, Bool)] = [:]
    var nextFork: UInt16 = 1
    var versions: Set<String> = ["AFP2.2", "AFPVersion 2.1"]
    var commands: [UInt8] = []
    var reads = 0, writes = 0

    static func pattern(_ n: Int, _ k: Int) -> [UInt8] { (0..<n).map { UInt8(truncatingIfNeeded: $0 * k + ($0 >> 8)) } }
    static let readMe = pattern(10000, 3), readMeRsrc = pattern(700, 5)

    init() {
        nodes[2] = Node(name: Array("StubVol".utf8), parent: 1, dir: true)
        nodes[17] = Node(name: Array("Folder".utf8), parent: 2, dir: true)
        nodes[18] = Node(name: Array("ReadMe".utf8), parent: 2, dir: false, data: AFPStub.readMe, rsrc: AFPStub.readMeRsrc,
                         finder: Array("TEXTttxt".utf8) + [UInt8](repeating: 0, count: 24))
        nodes[19] = Node(name: [0x43, 0x61, 0x66, 0x8E], parent: 2, dir: false) // "Café" in MacRoman
        nodes[20] = Node(name: Array("Inner".utf8), parent: 17, dir: false, data: [1, 2, 3])
    }

    func children(_ dir: UInt32) -> [UInt32] { nodes.filter { $0.value.parent == dir && $0.key != 2 }.map { $0.key }.sorted() }

    func resolve(_ dir: UInt32, _ name: [UInt8]) -> UInt32? {
        if name.isEmpty { return nodes[dir] != nil ? dir : nil }
        return children(dir).first { nodes[$0]!.name == name }
    }

    /// A node's parameters, packed for a bitmap.
    func params(_ id: UInt32, _ bitmap: UInt16) -> [UInt8] {
        let n = nodes[id]!
        var p = [UInt8]()
        var nameAt: Int? = nil
        for bit in 0..<16 where bitmap & (1 << bit) != 0 {
            switch 1 << bit {
            case 0x1: p += be16(0)
            case 0x2: p += be32(n.parent)
            case 0x4: p += be32(0)              // created 2000-01-01 00:00 GMT
            case 0x8: p += be32(366 * 86400)    // modified 2001-01-01
            case 0x10: p += be32(0x8000_0000)
            case 0x20: p += n.finder
            case 0x40: nameAt = p.count; p += be16(0)
            case 0x100: p += be32(id)
            case 0x200: p += n.dir ? be16(children(id).count) : be32(UInt32(n.data.count))
            case 0x400: p += be32(n.dir ? 0 : UInt32(n.rsrc.count))
            default: p += be32(0)
            }
        }
        if let at = nameAt {
            p[at] = UInt8(p.count >> 8)
            p[at + 1] = UInt8(p.count & 0xFF)
            p += [UInt8(n.name.count)] + n.name
        }
        return p
    }

    func u16(_ c: [UInt8], _ i: Int) -> Int { Int(c[i]) << 8 | Int(c[i + 1]) }
    func u32(_ c: [UInt8], _ i: Int) -> UInt32 { UInt32(u16(c, i)) << 16 | UInt32(u16(c, i + 2)) }
    /// A path (type 2 and a name) at i: the name and where it ends.
    func path(_ c: [UInt8], _ i: Int) -> ([UInt8], Int) {
        let n = Int(c[i + 1])
        return (Array(c[(i + 2)..<(i + 2 + n)]), i + 2 + n)
    }
    func pstr(_ c: [UInt8], _ i: Int) -> (String, Int) {
        let n = Int(c[i])
        return (String(decoding: c[(i + 1)..<(i + 1 + n)], as: UTF8.self), i + 1 + n)
    }

    func create(_ dir: UInt32, _ name: [UInt8], _ isDir: Bool) -> (Int32, [UInt8]) {
        guard nodes[dir]?.dir == true else { return (-5018, []) }
        if resolve(dir, name) != nil { return (-5017, []) }
        nextID += 1
        nodes[nextID] = Node(name: name, parent: dir, dir: isDir)
        return (0, isDir ? be32(nextID) : [])
    }

    /// A command and the data written with it: the result and reply.
    func handle(_ c: [UInt8], _ wdata: [UInt8] = []) -> (Int32, [UInt8]) {
        commands.append(c[0])
        switch c[0] {
        case 18: // FPLogin
            let (version, a) = pstr(c, 1)
            let (uam, b) = pstr(c, a)
            if !versions.contains(version) { return (-5003, []) }
            if uam == "No User Authent" { return (0, []) }
            if uam != "Cleartxt Passwrd" { return (-5002, []) }
            let (user, d) = pstr(c, b)
            let pw = Array(c[(d + (d & 1))..<(d + (d & 1) + 8)])
            return user == "joe" && pw == Array("secret".utf8) + [0, 0] ? (0, []) : (-5023, [])
        case 20, 2: return (0, []) // FPLogout, FPCloseVol
        case 16: // FPGetSrvrParms
            return (0, be32(0) + [1, 0, 7] + Array("StubVol".utf8))
        case 24: // FPOpenVol
            let (name, _) = pstr(c, 4)
            return name == "StubVol" && u16(c, 2) == 0x20 ? (0, be16(0x20) + be16(1)) : (-5018, [])
        case 9: // FPEnumerate
            let dir = u32(c, 4), fb = UInt16(u16(c, 8)), db = UInt16(u16(c, 10))
            let count = u16(c, 12), start = u16(c, 14)
            guard nodes[dir]?.dir == true else { return (-5018, []) }
            let all = children(dir)
            if start > all.count { return (-5018, []) }
            let some = all[(start - 1)...].prefix(min(count, 2)) // two at a time: the client asks again
            var out = be16(Int(fb)) + be16(Int(db)) + be16(some.count)
            for id in some {
                let isDir = nodes[id]!.dir
                var rec = [UInt8(0), isDir ? 0x80 : 0] + params(id, isDir ? db : fb)
                if rec.count & 1 != 0 { rec.append(0) }
                rec[0] = UInt8(rec.count)
                out += rec
            }
            return (0, out)
        case 34: // FPGetFileDirParms
            let fb = UInt16(u16(c, 8)), db = UInt16(u16(c, 10))
            guard let id = resolve(u32(c, 4), path(c, 12).0) else { return (-5018, []) }
            let isDir = nodes[id]!.dir
            return (0, be16(Int(fb)) + be16(Int(db)) + [isDir ? 0x80 : 0, 0] + params(id, isDir ? db : fb))
        case 26: // FPOpenFork
            guard let id = resolve(u32(c, 4), path(c, 12).0), !nodes[id]!.dir else { return (-5018, []) }
            let bitmap = UInt16(u16(c, 8))
            nextFork += 1
            forks[nextFork] = (id, c[1] & 0x80 != 0)
            return (0, be16(Int(bitmap)) + be16(Int(nextFork)) + params(id, bitmap))
        case 27: // FPRead
            guard let (id, rsrc) = forks[UInt16(u16(c, 2))] else { return (-5000, []) }
            reads += 1
            let fork = rsrc ? nodes[id]!.rsrc : nodes[id]!.data
            let off = Int(u32(c, 4)), want = Int(u32(c, 8))
            let end = min(fork.count, off + want)
            return (off + want > fork.count ? -5009 : 0, off < end ? Array(fork[off..<end]) : [])
        case 33: // FPWrite
            guard let (id, rsrc) = forks[UInt16(u16(c, 2))] else { return (-5000, []) }
            writes += 1
            let off = Int(u32(c, 4)), n = Int(u32(c, 8))
            guard n == wdata.count else { return (-5019, []) }
            var fork = rsrc ? nodes[id]!.rsrc : nodes[id]!.data
            if fork.count < off + n { fork += [UInt8](repeating: 0, count: off + n - fork.count) }
            fork.replaceSubrange(off..<(off + n), with: wdata)
            if rsrc { nodes[id]!.rsrc = fork } else { nodes[id]!.data = fork }
            return (0, be32(UInt32(off + n)))
        case 31: // FPSetForkParms
            guard let (id, rsrc) = forks[UInt16(u16(c, 2))] else { return (-5000, []) }
            let len = Int(u32(c, 6))
            var fork = rsrc ? nodes[id]!.rsrc : nodes[id]!.data
            fork = len <= fork.count ? Array(fork[0..<len]) : fork + [UInt8](repeating: 0, count: len - fork.count)
            if rsrc { nodes[id]!.rsrc = fork } else { nodes[id]!.data = fork }
            return (0, [])
        case 4: // FPCloseFork
            return forks.removeValue(forKey: UInt16(u16(c, 2))) != nil ? (0, []) : (-5000, [])
        case 7: return create(u32(c, 4), path(c, 8).0, false) // FPCreateFile
        case 6: return create(u32(c, 4), path(c, 8).0, true)  // FPCreateDir
        case 8: // FPDelete
            guard let id = resolve(u32(c, 4), path(c, 8).0) else { return (-5018, []) }
            if !children(id).isEmpty { return (-5007, []) }
            nodes[id] = nil
            return (0, [])
        case 35: // FPSetFileDirParms
            let (name, e) = path(c, 10)
            guard let id = resolve(u32(c, 4), name) else { return (-5018, []) }
            let at = e + (e & 1)
            if u16(c, 8) == 0x20 { nodes[id]!.finder = Array(c[at..<(at + 32)]) }
            return (0, [])
        case 28: // FPRename
            let (name, e) = path(c, 8)
            guard let id = resolve(u32(c, 4), name) else { return (-5018, []) }
            let (new, _) = path(c, e)
            if resolve(u32(c, 4), new) != nil { return (-5017, []) }
            nodes[id]!.name = new
            return (0, [])
        case 23: // FPMoveAndRename
            let (name, e) = path(c, 12)
            let (dst, f) = path(c, e)
            let (new, _) = path(c, f)
            guard let id = resolve(u32(c, 4), name), let to = resolve(u32(c, 8), dst) else { return (-5018, []) }
            nodes[id]!.parent = to
            if !new.isEmpty { nodes[id]!.name = new }
            return (0, [])
        default: return (-5024, [])
        }
    }
}

/// A thread serving until stopped.
final class StubThread {
    var stop = false
    let done = DispatchSemaphore(value: 0)
    init(_ body: @escaping (StubThread) -> Void) {
        let t = Thread {
            body(self)
            self.done.signal()
        }
        t.start()
    }
    func finish() {
        stop = true
        done.wait()
    }
}

/// AFP over TCP: DSI on a loopback port, a connection at a time.
final class DSIStub {
    let afp: AFPStub
    let lfd: Int32
    var port: UInt16 = 0
    var thread: StubThread!
    var dropNext = false
    var sessions = 0, tickles = 0
    let quantum: UInt32 = 4096

    init(_ afp: AFPStub) {
        self.afp = afp
        lfd = socket(AF_INET, SOCK_STREAM, 0)
        var sa = sockaddr_in()
        sa.sin_family = sa_family_t(AF_INET)
        sa.sin_addr.s_addr = inet_addr("127.0.0.1")
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        withUnsafeMutablePointer(to: &sa) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                _ = bind(lfd, $0, len)
                _ = listen(lfd, 4)
                _ = getsockname(lfd, $0, &len)
            }
        }
        port = UInt16(bigEndian: sa.sin_port)
        thread = StubThread { [unowned self] t in
            while !t.stop {
                var pf = pollfd(fd: self.lfd, events: Int16(POLLIN), revents: 0)
                if poll(&pf, 1, 50) == 1 {
                    let c = accept(self.lfd, nil, nil)
                    if c >= 0 { self.serve(c) }
                }
            }
            close(self.lfd)
        }
    }

    func readN(_ fd: Int32, _ n: Int) -> [UInt8]? {
        var out = [UInt8](repeating: 0, count: n)
        var got = 0
        while got < n {
            let k = out.withUnsafeMutableBytes { recv(fd, $0.baseAddress! + got, n - got, 0) }
            if k <= 0 { return nil }
            got += k
        }
        return out
    }

    func send(_ fd: Int32, _ flags: UInt8, _ cmd: UInt8, _ id: [UInt8], _ code: UInt32, _ data: [UInt8]) {
        let p = [flags, cmd] + id + be32(code) + be32(UInt32(data.count)) + be32(0) + data
        _ = p.withUnsafeBytes { Darwin.send(fd, $0.baseAddress, p.count, 0) }
    }

    func serve(_ c: Int32) {
        var noise = false
        defer { close(c) }
        while let h = readN(c, 16) {
            let len = Int(be32val(h, 8))
            guard let payload = readN(c, len) else { return }
            let id = Array(h[2...3])
            switch h[1] {
            case 4: // DSIOpenSession
                sessions += 1
                send(c, 1, 4, id, 0, [0, 4] + be32(quantum))
            case 2, 6: // DSICommand, DSIWrite
                if dropNext {
                    dropNext = false
                    return
                }
                if !noise { // a tickle and an attention first, which the client gets past
                    send(c, 0, 5, [0, 9], 0, [])
                    send(c, 0, 8, [0, 10], 0, [0x80, 0])
                    noise = true
                }
                let off = h[1] == 6 ? Int(be32val(h, 4)) : payload.count
                let (r, d) = afp.handle(Array(payload[0..<off]), Array(payload[off...]))
                send(c, 1, h[1], id, UInt32(bitPattern: r), d)
            case 5: tickles += 1
            case 1: return
            default: break
            }
        }
    }

    func be32val(_ b: [UInt8], _ i: Int) -> UInt32 { UInt32(b[i]) << 24 | UInt32(b[i + 1]) << 16 | UInt32(b[i + 2]) << 8 | UInt32(b[i + 3]) }
}

/// AFP over AppleTalk: another LocalTalk node on the LToUDP loopback group,
/// answering NBP lookups for "Stub:AFPServer" and speaking ATP and ASP.
final class ASPStub {
    let afp: AFPStub
    let fd: Int32
    let sender = UInt32.random(in: 1...0xFFFF_FFF0)
    let node: UInt8 = 222, sls: UInt8 = 0x90, sss: UInt8 = 0x91, sid: UInt8 = 7
    var wss: UInt8 = 0, client: UInt8 = 0
    var thread: StubThread!
    var answered: [UInt16: [[UInt8]]] = [:] // XO requests' responses, by TID
    var write: (tid: UInt16, cmd: [UInt8], ours: UInt16, got: [Int: [UInt8]], total: Int)? = nil
    var tid: UInt16 = 0x4000
    var lookups = 0, statuses = 0, writeContinues = 0, attentionsAnswered = 0, closed = false

    init(_ afp: AFPStub) {
        self.afp = afp
        fd = socket(AF_INET, SOCK_DGRAM, 0)
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(4))
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, socklen_t(4))
        var sa = sockaddr_in()
        sa.sin_family = sa_family_t(AF_INET)
        sa.sin_port = in_port_t(1954).bigEndian
        withUnsafePointer(to: &sa) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { _ = bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        var mr = ip_mreq()
        mr.imr_multiaddr.s_addr = inet_addr("239.192.76.84")
        mr.imr_interface.s_addr = inet_addr("127.0.0.1")
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, socklen_t(MemoryLayout<ip_mreq>.size))
        var ifa = in_addr(s_addr: inet_addr("127.0.0.1"))
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, socklen_t(MemoryLayout<in_addr>.size))
        var loop: UInt8 = 1
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1)
        thread = StubThread { [unowned self] t in
            var buf = [UInt8](repeating: 0, count: 2048)
            while !t.stop {
                var pf = pollfd(fd: self.fd, events: Int16(POLLIN), revents: 0)
                if poll(&pf, 1, 20) != 1 { continue }
                let n = buf.withUnsafeMutableBytes { recv(self.fd, $0.baseAddress, 2048, 0) }
                if n >= 7 { self.frame(Array(buf[0..<n])) }
            }
            close(self.fd)
        }
    }

    func sendFrame(_ dst: UInt8, _ dskt: UInt8, _ sskt: UInt8, _ type: UInt8, _ data: [UInt8]) {
        let len = data.count + 5
        let f = be32(sender) + [dst, node, 1, UInt8(len >> 8), UInt8(len & 0xFF), dskt, sskt, type] + data
        var to = sockaddr_in()
        to.sin_family = sa_family_t(AF_INET)
        to.sin_port = in_port_t(1954).bigEndian
        to.sin_addr.s_addr = inet_addr("239.192.76.84")
        _ = f.withUnsafeBytes { p in
            withUnsafePointer(to: &to) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, p.baseAddress, f.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    func atp(_ control: UInt8, _ bitmap: UInt8, _ tid: UInt16, _ user: UInt32, _ data: [UInt8]) -> [UInt8] {
        [control, bitmap] + be16(Int(tid)) + be32(user) + data
    }

    /// A transaction's responses (the last with EOM), kept if it was XO.
    func respond(_ tid: UInt16, _ to: UInt8, _ skt: UInt8, _ from: UInt8, _ packets: [(UInt32, [UInt8])], xo: Bool) {
        let pkts = packets.enumerated().map { i, p in
            atp(0x80 | (i == packets.count - 1 ? 0x10 : 0), UInt8(i), tid, p.0, p.1)
        }
        if xo { answered[tid] = pkts }
        for p in pkts { sendFrame(to, skt, from, 3, p) }
    }

    /// A command's reply: the result in the first response's user bytes.
    func reply(_ r: Int32, _ d: [UInt8]) -> [(UInt32, [UInt8])] {
        var out = [(UInt32, [UInt8])]()
        var i = 0
        repeat {
            out.append((out.isEmpty ? UInt32(bitPattern: r) : 0, Array(d[i..<min(d.count, i + 578)])))
            i += 578
        } while i < d.count
        return out
    }

    func frame(_ b: [UInt8]) {
        let id = UInt32(b[0]) << 24 | UInt32(b[1]) << 16 | UInt32(b[2]) << 8 | UInt32(b[3])
        if id == sender { return }
        let f = Array(b[4...])
        if f[2] == 0x81 { // LLAP enquiry: our node is taken
            if f[0] == node { sendRaw([node, node, 0x82]) }
            return
        }
        guard f[2] == 1, f.count >= 8, f[0] == node || f[0] == 255 else { return }
        let snode = f[1], dskt = f[5], sskt = f[6], type = f[7]
        let d = Array(f[8...])
        if type == 2 && dskt == 2 && d.count >= 7 && d[0] >> 4 == 2 { // NBP LkUp
            var p = 7
            var parts = [String]()
            for _ in 0..<3 where p < d.count {
                let n = Int(d[p])
                parts.append(String(decoding: d[(p + 1)..<min(d.count, p + 1 + n)], as: UTF8.self))
                p += 1 + n
            }
            guard parts.count == 3, parts[0] == "Stub" || parts[0] == "=", parts[1] == "AFPServer" else { return }
            lookups += 1
            let tuple: [UInt8] = [0, 0, node, sls, 0] + [4] + Array("Stub".utf8) + [9] + Array("AFPServer".utf8) + [1, 0x2A]
            sendFrame(d[4], d[5], 2, 2, [0x31, d[1]] + tuple)
            return
        }
        guard type == 3, d.count >= 8 else { return }
        let control = d[0], bitmap = d[1], t = UInt16(d[2]) << 8 | UInt16(d[3])
        let user = Array(d[4..<8]), data = Array(d[8...])
        switch control & 0xC0 {
        case 0x40: // TReq
            if let again = answered[t] {
                for (i, p) in again.enumerated() where bitmap & (1 << i) != 0 { sendFrame(snode, sskt, dskt, 3, p) }
                return
            }
            if write?.tid == t { return }
            let xo = control & 0x20 != 0
            if dskt == sls {
                switch user[0] {
                case 3: // GetStatus
                    statuses += 1
                    respond(t, snode, sskt, sls, [(0, Array("stub status".utf8))], xo: false)
                case 4: // OpenSess
                    wss = user[1]
                    client = snode
                    respond(t, snode, sskt, sls, [(UInt32(sss) << 24 | UInt32(sid) << 16, [])], xo: xo)
                    // A tickle and an attention for the workstation.
                    tid += 1
                    sendFrame(client, wss, sss, 3, atp(0x40, 1, tid, UInt32(5) << 24 | UInt32(sid) << 16, []))
                    tid += 1
                    sendFrame(client, wss, sss, 3, atp(0x60, 1, tid, UInt32(8) << 24 | UInt32(sid) << 16 | 0x8000, []))
                default: break
                }
            } else if dskt == sss && user[1] == sid {
                switch user[0] {
                case 2: // Command
                    let (r, out) = afp.handle(data)
                    respond(t, snode, sskt, sss, reply(r, out), xo: xo)
                case 6: // Write: ask for the data
                    tid += 1
                    write = (t, data, tid, [:], -1)
                    writeContinues += 1
                    sendFrame(client, wss, sss, 3, atp(0x60, 0xFF, tid, UInt32(7) << 24 | UInt32(sid) << 16 | UInt32(user[2]) << 8 | UInt32(user[3]), be16(4624)))
                case 1: // CloseSess
                    closed = true
                    respond(t, snode, sskt, sss, [(0, [])], xo: xo)
                default: break
                }
            }
        case 0x80: // TResp: the data for a write, or an attention's answer
            if t != write?.ours {
                attentionsAnswered += 1
                return
            }
            guard var w = write else { return }
            w.got[Int(bitmap)] = data
            if control & 0x10 != 0 { w.total = Int(bitmap) + 1 }
            write = w
            if w.total > 0 && (0..<w.total).allSatisfy({ w.got[$0] != nil }) {
                sendFrame(snode, sskt, dskt, 3, atp(0xC0, 0, t, 0, [])) // TRel
                let all = (0..<w.total).flatMap { w.got[$0]! }
                let (r, out) = afp.handle(w.cmd, all)
                write = nil
                respond(w.tid, client, wss, sss, reply(r, out), xo: true)
            }
        default: break
        }
    }

    func sendRaw(_ llap: [UInt8]) {
        let f = be32(sender) + llap
        var to = sockaddr_in()
        to.sin_family = sa_family_t(AF_INET)
        to.sin_port = in_port_t(1954).bigEndian
        to.sin_addr.s_addr = inet_addr("239.192.76.84")
        _ = f.withUnsafeBytes { p in
            withUnsafePointer(to: &to) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, p.baseAddress, f.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }
}

extension BlueBox {
    /// The AppleShare client (afp.c, asp.c) against stub servers: AFP over
    /// TCP (DSI) on a loopback port, and over AppleTalk (ASP on ATP) from
    /// another node on the LToUDP loopback group.
    @Suite struct AFP {
        func cstr(_ b: [UInt8]) -> [CChar] { b.map { CChar(bitPattern: $0) } + [0] }

        func name(_ e: afp_entry) -> [UInt8] {
            withUnsafeBytes(of: e.name) { Array($0.prefix { $0 != 0 }) }
        }

        func volumes(_ s: OpaquePointer) -> [String] {
            var count: Int32 = 0
            var out = [String]()
            func take<T>(_ p: UnsafeMutablePointer<T>?) {
                for i in 0..<Int(count) {
                    withUnsafeBytes(of: p![i]) { out.append(String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self)) }
                }
                free(p)
            }
            var names: UnsafeMutablePointer<VolName>? = nil
            #expect(afp_volumes(s, &names, &count) == 0)
            take(names)
            return out
        }

        func list(_ s: OpaquePointer, _ dir: UInt32) -> [afp_entry] {
            var p: UnsafeMutablePointer<afp_entry>? = nil
            var n: Int32 = 0
            #expect(afp_list(s, 1, dir, &p, &n) == 0)
            defer { free(p) }
            return (0..<Int(n)).map { p![$0] }
        }

        func read(_ s: OpaquePointer, _ dir: UInt32, _ name: [UInt8], rsrc: Bool = false) -> [UInt8]? {
            var p: UnsafeMutablePointer<UInt8>? = nil
            var n = 0
            guard afp_read_fork(s, 1, dir, cstr(name), rsrc, &p, &n) == 0 else { return nil }
            defer { free(p) }
            return Array(UnsafeBufferPointer(start: p, count: n))
        }

        /// The calls a mirrored volume makes, on either transport.
        func exercise(_ s: OpaquePointer, _ stub: AFPStub, reads: Int, writes: Int) {
            #expect(volumes(s) == ["StubVol"])
            var vol: UInt16 = 0
            #expect(afp_open_volume(s, "StubVol", &vol) == 0 && vol == 1)
            #expect(afp_open_volume(s, "Nothing", &vol) == -5018)

            // The root, two entries a call.
            let root = list(s, 2)
            #expect(root.map { name($0) } == [Array("Folder".utf8), Array("ReadMe".utf8), [0x43, 0x61, 0x66, 0x8E]])
            #expect(root[0].dir && root[0].id == 17 && root[0].parent == 2)
            let readMe = root[1]
            #expect(!readMe.dir && readMe.id == 18 && readMe.data_len == 10000 && readMe.rsrc_len == 700)
            #expect(withUnsafeBytes(of: readMe.finder_info) { Array($0.prefix(8)) } == Array("TEXTttxt".utf8))
            // Dates: from 2000 GMT to local seconds since 1904.
            let off = UInt32(bitPattern: Int32(TimeZone.current.secondsFromGMT(for: Date(timeIntervalSince1970: 946_684_800))))
            #expect(readMe.create_date == 3_029_529_600 &+ off)
            let off2 = UInt32(bitPattern: Int32(TimeZone.current.secondsFromGMT(for: Date(timeIntervalSince1970: 946_684_800 + 366 * 86400))))
            #expect(readMe.modify_date == 3_029_529_600 + 366 * 86400 &+ off2)
            #expect(list(s, 17).map { name($0) } == [Array("Inner".utf8)])

            var e = afp_entry()
            #expect(afp_stat(s, 1, 2, "ReadMe", &e) == 0 && e.id == 18 && e.rsrc_len == 700)
            #expect(afp_stat(s, 1, 2, cstr([0x43, 0x61, 0x66, 0x8E]), &e) == 0 && e.id == 19)
            #expect(afp_stat(s, 1, 2, "Folder", &e) == 0 && e.dir && e.id == 17)
            #expect(afp_stat(s, 1, 2, "Missing", &e) == -5018)

            // Whole forks, in pieces the quantum allows.
            stub.reads = 0
            #expect(read(s, 2, Array("ReadMe".utf8)) == AFPStub.readMe)
            #expect(stub.reads == reads)
            #expect(read(s, 2, Array("ReadMe".utf8), rsrc: true) == AFPStub.readMeRsrc)
            #expect(read(s, 17, Array("Inner".utf8)) == [1, 2, 3])

            // A new file: both forks, Finder info, renamed and moved.
            #expect(afp_create_file(s, 1, 17, "New") == 0)
            #expect(afp_create_file(s, 1, 17, "New") == -5017)
            let data = AFPStub.pattern(9000, 11), rsrc = AFPStub.pattern(300, 13)
            stub.writes = 0
            #expect(afp_write_fork(s, 1, 17, "New", false, data, data.count) == 0)
            #expect(stub.writes == writes)
            #expect(afp_write_fork(s, 1, 17, "New", true, rsrc, rsrc.count) == 0)
            #expect(afp_write_fork(s, 1, 17, "New", true, rsrc, 100) == 0) // shorter: truncated
            let info: [UInt8] = Array("APPLtest".utf8) + [0x01, 0x00] + [UInt8](repeating: 0, count: 22)
            #expect(afp_set_finder_info(s, 1, 17, "New", info) == 0)
            #expect(afp_move(s, 1, 17, "New", 17, "Newer") == 0)
            var sub: UInt32 = 0
            #expect(afp_create_dir(s, 1, 2, "Sub", &sub) == 0 && sub > 100)
            #expect(afp_move(s, 1, 17, "Newer", sub, "Moved") == 0)
            #expect(afp_stat(s, 1, sub, "Moved", &e) == 0 && e.data_len == 9000 && e.rsrc_len == 100 && e.parent == sub)
            #expect(withUnsafeBytes(of: e.finder_info) { Array($0) } == info)
            #expect(read(s, sub, Array("Moved".utf8)) == data)
            #expect(read(s, sub, Array("Moved".utf8), rsrc: true) == Array(rsrc.prefix(100)))
            #expect(afp_delete(s, 1, 2, "Sub") == -5007)
            #expect(afp_delete(s, 1, sub, "Moved") == 0)
            #expect(afp_delete(s, 1, 2, "Sub") == 0)
            #expect(afp_stat(s, 1, 2, "Sub", &e) == -5018)
            #expect(stub.forks.isEmpty)
        }

        @Test func overTCP() {
            let afp = AFPStub()
            let dsi = DSIStub(afp)
            var err = [CChar](repeating: 0, count: 256)
            let s = afp_connect_tcp("127.0.0.1:\(dsi.port)", nil, nil, &err, 256)
            #expect(s != nil, "\(String(cString: err))")
            if let s {
                exercise(s, afp, reads: 3, writes: 3) // a quantum of 4096 for each request
                #expect(dsi.tickles >= 1) // the stub's tickle answered
                // A dropped session is opened again, its volume too, for the next call.
                dsi.dropNext = true
                var e = afp_entry()
                #expect(afp_stat(s, 1, 2, "ReadMe", &e) == 0 && e.id == 18)
                #expect(dsi.sessions == 2)
                afp_close(s)
            }
            #expect(afp.commands.first == 18 && afp.commands.last == 20) // FPLogin ... FPLogout
            dsi.thread.finish()

            // A refused login.
            let afp2 = AFPStub(), dsi2 = DSIStub(afp2)
            #expect(afp_connect_tcp("127.0.0.1:\(dsi2.port)", "joe", "wrong", &err, 256) == nil)
            let ok = afp_connect_tcp("127.0.0.1:\(dsi2.port)", "joe", "secret", &err, 256)
            #expect(ok != nil)
            afp_close(ok)
            dsi2.thread.finish()
        }

        @Test func overAppleTalk() {
            #expect(machine_set_appletalk("local"))
            launchTestApp()
            let afp = AFPStub()
            afp.versions = ["AFPVersion 2.1"] // as AppleShare 3 speaks: the client asks again
            let asp = ASPStub(afp)
            defer { asp.thread.finish() }
            var err = [CChar](repeating: 0, count: 256)
            let s = afp_connect_atalk("Stub:AFPServer@*", "joe", "secret", &err, 256)
            #expect(s != nil, "\(String(cString: err))")
            guard let s else { return }
            #expect(asp.lookups >= 1 && asp.statuses == 1 && asp.wss >= 128)
            #expect(afp.commands.prefix(2) == [18, 18]) // AFP2.2 refused, then 2.1
            exercise(s, afp, reads: 3, writes: 2) // 4624 bytes: eight ATP responses
            #expect(asp.writeContinues >= 4)
            #expect(asp.attentionsAnswered == 1)
            afp_close(s)
            #expect(asp.closed)
            #expect(afp_connect_atalk("Nobody:AFPServer@*", nil, nil, &err, 256) == nil)
        }
    }
}

typealias VolName = (CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar,
                     CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar)

/// An AppleShare volume mirrored through the File Manager: the stub's
/// volume mounted, read and written by ordinary File Manager calls.
extension BlueBox {
    @Suite struct AppleShareVolume {
        func pstr(_ s: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            wr8(p, UInt8(s.count))
            for (i, b) in s.enumerated() { wr8(p + 1 + UInt32(i), b) }
            return p
        }

        func io(_ ref: Int16, _ buf: UInt32, _ count: Int, from: Int = 0) -> UInt32 {
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: ref))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), UInt32(count))
            wr16(pb + UInt32(IOParam_ioPosMode_off), 1) // fsFromStart
            wr32(pb + UInt32(IOParam_ioPosOffset_off), UInt32(from))
            return pb
        }

        @Test func filesComeFromAndGoToTheServer() {
            let afp = AFPStub()
            let dsi = DSIStub(afp)
            defer { machine_close_appleshares(); dsi.thread.finish() }
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_mount_appleshare("tcp:127.0.0.1:\(dsi.port)/StubVol", &err, err.count), "\(String(cString: err))")
            _ = launchTestApp()
            _ = initToolbox()
            // The mounted volume, by its name.
            let vpb = tb_NewPtr(Int32(HVolumeParam_SIZEOF), false, true), vname = tb_NewPtr(256, false, true)
            wr32(vpb + UInt32(IOParam_ioNamePtr_off), vname)
            wr16(vpb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: -2))
            #expect(tb_PBGetVInfo(vpb, false) == 0)
            #expect((0..<Int(rd8(vname))).map { rd8(vname + 1 + UInt32($0)) } == Array("StubVol".utf8))
            // The host's file panels list it among the program's disks.
            #expect(machine_volume_count() == 2)
            var dname = [CChar](repeating: 0, count: 64), droot = [CChar](repeating: 0, count: 1024)
            #expect(machine_volume_info(1, &dname, dname.count, &droot, droot.count))
            #expect(String(decoding: dname.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self) == "StubVol")
            #expect(String(decoding: droot.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self).hasSuffix("/StubVol"))

            // A file from the server: both forks and its type.
            let spec = tb_NewPtr(70, false, true)
            #expect(tb_FSMakeFSSpec(-2, 2, pstr(Array("ReadMe".utf8)), spec) == 0)
            let ref = tb_NewPtr(2, false, true)
            #expect(tb_FSpOpenDF(spec, 1, ref) == 0)
            let buf = tb_NewPtr(16, false, true)
            let pb = io(Int16(bitPattern: rd16(ref)), buf, 16, from: 100)
            #expect(tb_PBRead(pb, false) == 0)
            #expect((0..<16).map { rd8(buf + UInt32($0)) } == Array(AFPStub.readMe[100..<116]))
            #expect(tb_PBGetEOF(pb, false) == 0 && rd32(pb + UInt32(IOParam_ioMisc_off)) == 10000)
            #expect(tb_PBClose(pb, false) == 0)
            // A subfolder's file, filled when the folder is looked into.
            #expect(tb_FSMakeFSSpec(-2, 2, pstr(Array(":Folder:Inner".utf8)), spec) == 0)

            // A new file goes to the server, its data and type too.
            #expect(tb_FSMakeFSSpec(-2, 2, pstr(Array("Note".utf8)), spec) == -43)  // fnfErr: not there yet
            #expect(tb_FSpCreate(spec, 0x7474_7874, 0x5445_5854, 0) == 0)            // 'ttxt' 'TEXT'
            #expect(tb_FSpOpenDF(spec, 3, ref) == 0)
            let text = Array("hello, server".utf8)
            let data = tb_NewPtr(Int32(text.count), false, true)
            for (i, b) in text.enumerated() { wr8(data + UInt32(i), b) }
            let wpb = io(Int16(bitPattern: rd16(ref)), data, text.count)
            #expect(tb_PBWrite(wpb, false) == 0)
            #expect(tb_PBClose(wpb, false) == 0)
            let note = afp.nodes.values.first { $0.name == Array("Note".utf8) }
            #expect(note?.data == text)
            #expect(note.map { Array($0.finder.prefix(8)) } == Array("TEXTttxt".utf8))
            // Renamed, then deleted, on the server as well.
            #expect(tb_FSpRename(spec, pstr(Array("Memo".utf8))) == 0)
            #expect(afp.nodes.values.contains { $0.name == Array("Memo".utf8) })
            #expect(tb_FSMakeFSSpec(-2, 2, pstr(Array("Memo".utf8)), spec) == 0)
            #expect(tb_FSpDelete(spec) == 0)
            #expect(!afp.nodes.values.contains { $0.name == Array("Memo".utf8) })
        }
    }
}
