import CCore
import Foundation
import Testing
import Toolbox

/// A LaserWriter on the LToUDP loopback group: NBP answers for
/// "Test LW:LaserWriter", and PAP's server side, which reads the job with
/// SendData requests and answers the workstation's reads with its EOF.
final class PAPStub {
    let fd: Int32
    let sender = UInt32.random(in: 1...0xFFFF_FFF0)
    let node: UInt8 = 223, sls: UInt8 = 0x80, rs: UInt8 = 0x81
    var thread: StubThread!
    var lock = NSLock()
    var client: UInt8 = 0, clientSkt: UInt8 = 0, conn: UInt8 = 0
    var tid: UInt16 = 0x5000, seq: UInt16 = 0
    var reading: (tid: UInt16, got: [Int: [UInt8]], eof: Bool, total: Int)?
    var job: [UInt8] = []
    var done = false, closed = false, busyOnce: Bool
    var opens = 0
    var heldRead: (UInt16, UInt8, UInt8)? // the workstation's SendData, answered once the job is in

    init(busyOnce: Bool = false) {
        self.busyOnce = busyOnce
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
                if n >= 7 {
                    self.lock.lock()
                    self.frame(Array(buf[0..<n]))
                    self.lock.unlock()
                }
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

    func atp(_ control: UInt8, _ bitmap: UInt8, _ tid: UInt16, _ user: [UInt8], _ data: [UInt8]) -> [UInt8] {
        [control, bitmap] + be16(Int(tid)) + user + data
    }

    /// The next piece of the job asked for.
    func askForData() {
        tid &+= 1
        seq = seq == 0xFFFF ? 1 : seq + 1
        reading = (tid, [:], false, -1)
        sendFrame(client, clientSkt, rs, 3, atp(0x60, 0xFF, tid, [conn, 3] + be16(Int(seq)), []))
    }

    func frame(_ b: [UInt8]) {
        let id = UInt32(b[0]) << 24 | UInt32(b[1]) << 16 | UInt32(b[2]) << 8 | UInt32(b[3])
        if id == sender { return }
        let f = Array(b[4...])
        if f[2] == 0x81 {
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
            guard parts.count == 3, parts[0] == "Test LW" || parts[0] == "=", parts[1] == "LaserWriter" else { return }
            let tuple: [UInt8] = [0, 0, node, sls, 0] + [7] + Array("Test LW".utf8) + [11] + Array("LaserWriter".utf8) + [1, 0x2A]
            sendFrame(d[4], d[5], 2, 2, [0x31, d[1]] + tuple)
            return
        }
        guard type == 3, d.count >= 8 else { return }
        let control = d[0], bitmap = d[1], t = UInt16(d[2]) << 8 | UInt16(d[3])
        let user = Array(d[4..<8]), data = Array(d[8...])
        switch control & 0xC0 {
        case 0x40: // TReq
            if dskt == sls && user[1] == 1 { // OpenConn
                opens += 1
                client = snode
                clientSkt = data[0]
                conn = user[0]
                let busy = busyOnce && opens == 1
                let status = Array("status: \(busy ? "busy" : "idle")".utf8)
                let reply = [rs, 8] + (busy ? [0xFF, 0xFF] : [0, 0]) + [UInt8(status.count)] + status
                sendFrame(snode, sskt, sls, 3, atp(0x90, 0, t, [conn, 2, 0, 0], reply))
                if !busy { askForData() }
            } else if dskt == rs && user[0] == conn && user[1] == 3 { // the workstation reads what we say
                if done {
                    let text = Array("%%[ status: idle ]%%\r".utf8)
                    sendFrame(snode, sskt, rs, 3, atp(0x90, 0, t, [conn, 4, 1, 0], text))
                } else {
                    heldRead = (t, snode, sskt)
                }
            } else if dskt == rs && user[0] == conn && user[1] == 6 { // CloseConn
                closed = true
                sendFrame(snode, sskt, rs, 3, atp(0x90, 0, t, [conn, 7, 0, 0], []))
            }
        case 0x80: // TResp: the job's data
            guard var r = reading, t == r.tid else { return }
            r.got[Int(bitmap)] = data
            if user[1] == 4 && user[2] != 0 { r.eof = true }
            if control & 0x10 != 0 { r.total = Int(bitmap) + 1 }
            reading = r
            if r.total > 0 && (0..<r.total).allSatisfy({ r.got[$0] != nil }) {
                sendFrame(snode, sskt, dskt, 3, atp(0xC0, 0, t, [0, 0, 0, 0], [])) // TRel
                for i in 0..<r.total { job += r.got[i]! }
                reading = nil
                if r.eof {
                    done = true
                    if let (ht, hn, hs) = heldRead {
                        let text = Array("%%[ status: idle ]%%\r".utf8)
                        sendFrame(hn, hs, rs, 3, atp(0x90, 0, ht, [conn, 4, 1, 0], text))
                        heldRead = nil
                    }
                } else {
                    askForData()
                }
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
    /// Printing on a LaserWriter the Chooser chose: PostScript over PAP.
    @Suite struct PAP {
        func page(text: String, more: ((UInt32) -> Void)? = nil) {
            tb_PrOpen()
            let h = tb_NewHandle(120, false, true)
            tb_PrintDefault(h)
            let port = tb_PrOpenDoc(h, 0, 0)
            tb_PrOpenPage(port, 0)
            let r = tb_NewPtr(8, false, true)
            wr16(r, 10); wr16(r + 2, 10); wr16(r + 4, 100); wr16(r + 6, 200)
            tb_FrameRect(r)
            tb_TextFont(20) // Times
            tb_TextSize(12)
            tb_MoveTo(20, 60)
            let s = tb_NewPtr(256, false, true)
            guestPString(text, at: s)
            tb_DrawString(s)
            more?(r)
            tb_PrClosePage(port)
            tb_PrCloseDoc(port)
            tb_PrClose()
        }

        @Test func printsPostScriptOnALaserWriter() {
            #expect(machine_set_appletalk("local"))
            #expect(machine_set_printer("laserwriter:Test LW@*"))
            defer { machine_set_printer("panel") }
            launchTestApp()
            _ = initToolbox()
            guestStack()
            let lw = PAPStub(busyOnce: true)
            defer { lw.thread.finish() }
            // A long page: several SendData rounds of eight packets.
            page(text: String(repeating: "Hello, LaserWriter (C) ", count: 4))
            lw.lock.lock()
            defer { lw.lock.unlock() }
            let ps = String(decoding: lw.job, as: UTF8.self)
            #expect(lw.opens == 2) // busy, then open
            #expect(ps.hasPrefix("%!PS-Adobe-3.0"))
            #expect(ps.contains("%%Pages: 1") && ps.contains("showpage") && ps.hasSuffix("%%EOF\n"))
            #expect(ps.contains("/Times-Roman-Mac /Times-Roman RF"))
            #expect(ps.contains("(Hello, LaserWriter \\(C\\) "))
            #expect(lw.closed)
        }

        /// The PostScript itself: the page size, a frame, the text in the
        /// printer's font fitted to its width on screen.
        @Test func pagesAsPostScript() {
            launchTestApp()
            _ = initToolbox()
            guestStack()
            nonisolated(unsafe) var ps = ""
            machine_set_print_handlers({ job, ctx in
                var len = 0
                guard let job, let p = machine_postscript(job, &len) else { return false }
                ctx!.assumingMemoryBound(to: String.self).pointee = String(decoding: UnsafeRawBufferPointer(start: p, count: len), as: UTF8.self)
                free(p)
                return true
            }, nil, &ps)
            defer { machine_set_print_handlers(nil, nil, nil) }
            page(text: "Text") { r in
                // more of QuickDraw for the printer: an oval, a round rectangle, a wedge,
                // a line, bold italic text with a Macintosh character (\xA9)
                wr16(r, 120); wr16(r + 2, 20); wr16(r + 4, 200); wr16(r + 6, 160)
                tb_PaintOval(r)
                wr16(r, 120); wr16(r + 2, 200); wr16(r + 4, 200); wr16(r + 6, 340)
                tb_FrameRoundRect(r, 30, 30)
                wr16(r, 220); wr16(r + 2, 20); wr16(r + 4, 320); wr16(r + 6, 120)
                tb_PaintArc(r, 45, 90)
                tb_PenSize(3, 3)
                tb_MoveTo(150, 220)
                tb_LineTo(300, 320)
                tb_PenNormal()
                tb_TextFont(21) // Helvetica
                tb_TextSize(18)
                tb_TextFace(3)
                tb_MoveTo(20, 360)
                let s = tb_NewPtr(16, false, true)
                for (i, b) in [UInt8(9), 0x43, 0x6F, 0x70, 0x79, 0x20, 0xA9, 0x20, 0x31, 0x39].enumerated() { wr8(s + UInt32(i), b) }
                tb_DrawString(s)
            }
            if let path = ProcessInfo.processInfo.environment["BLUEBOX_TEST_PS"] {
                try? ps.write(toFile: path, atomically: true, encoding: .utf8)
            }
            #expect(ps.contains("(Copy \\251 19) "))
            #expect(ps.contains("/Helvetica-BoldOblique-Mac 18 F"))
            #expect(ps.contains("%%BoundingBox: 0 0 612 792"))
            #expect(ps.contains("10 10 190 90 R 11.00 11.00 188.00 88.00 R 0.000 setgray eofill")) // the frame
            #expect(ps.contains("/Times-Roman-Mac 12 F 20 60 translate (Text) "))
        }
    }
}

/// A router on the LToUDP loopback group: RTMP data every so often, and
/// ZIP's GetMyZone and GetZoneList over ATP on socket 6.
final class RouterStub {
    let fd: Int32
    let sender = UInt32.random(in: 1...0xFFFF_FFF0)
    let node: UInt8 = 224
    let zones = ["Engineering", "Accounting", "Front Office"]
    var thread: StubThread!
    var asked = 0

    init() {
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
            var last = Date.distantPast
            while !t.stop {
                if Date().timeIntervalSince(last) > 0.1 { // RTMP data: network 0, node, no tuples beyond ours
                    self.sendFrame(255, 1, 1, 1, [0, 0, 8, self.node, 0, 0, 0])
                    last = Date()
                }
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

    func frame(_ b: [UInt8]) {
        let id = UInt32(b[0]) << 24 | UInt32(b[1]) << 16 | UInt32(b[2]) << 8 | UInt32(b[3])
        if id == sender { return }
        let f = Array(b[4...])
        guard f.count >= 8, f[2] == 1, f[0] == node else { return }
        let snode = f[1], dskt = f[5], sskt = f[6], type = f[7]
        let d = Array(f[8...])
        guard type == 3, dskt == 6, d.count >= 8, d[0] & 0xC0 == 0x40 else { return }
        asked += 1
        let tid = Array(d[2..<4])
        switch d[4] {
        case 7: // GetMyZone
            let z = Array(zones[0].utf8)
            sendFrame(snode, sskt, 6, 3, [0x90, 0] + tid + [1, 0, 0, 1] + [UInt8(z.count)] + z)
        case 8: // GetZoneList from an index
            let start = Int(d[6]) << 8 | Int(d[7])
            let list = zones.dropFirst(max(0, start - 1))
            let data = list.flatMap { [UInt8($0.utf8.count)] + Array($0.utf8) }
            sendFrame(snode, sskt, 6, 3, [0x90, 0] + tid + [1, 0, 0, UInt8(list.count)] + data)
        default: break
        }
    }
}

extension BlueBox {
    /// ZIP: the zones a router knows, for the Chooser and programs' calls.
    @Suite struct Zones {
        @Test func zonesFromTheRouter() {
            #expect(machine_set_appletalk("local"))
            launchTestApp()
            _ = initToolbox()
            guestStack()
            let router = RouterStub()
            defer { router.thread.finish() }
            #expect(at_open() == 0)
            // Until RTMP has been heard.
            var net: UInt16 = 0, node: UInt8 = 0
            let until = Date().addingTimeInterval(3)
            while !at_router(&net, &node) && Date() < until {
                usleep(5000)
                appletalk_poll()
            }
            #expect(node == 224)
            var names = [[CChar]](repeating: [CChar](repeating: 0, count: 33), count: 8)
            var flat = [CChar](repeating: 0, count: 33 * 8)
            let n = flat.withUnsafeMutableBytes {
                at_zone_list($0.baseAddress!.assumingMemoryBound(to: (CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar, CChar).self), 8)
            }
            for i in 0..<Int(n) { names[i] = Array(flat[(i * 33)..<(i * 33 + 33)]) }
            #expect(n == 3)
            #expect(names.prefix(Int(n)).map { String(cString: $0) } == router.zones)
            var mine = [CChar](repeating: 0, count: 33)
            at_my_zone(&mine)
            #expect(String(cString: mine) == "Engineering")
            if let path = ProcessInfo.processInfo.environment["BLUEBOX_TEST_SHOT"] {
                let rn = Int16(machine_open_chooser())
                let w = tb_FrontWindow()
                let ev = tb_NewPtr(16, false, true)
                wr16(ev, 6)
                wr32(ev + 2, w)
                _ = tb_SystemEvent(ev)
                _ = machine_screenshot("\(path)-chooser-zones.png")
                tb_CloseDeskAcc(rn)
            }
        }
    }
}
