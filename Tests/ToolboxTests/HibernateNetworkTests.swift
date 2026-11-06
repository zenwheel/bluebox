import CCore
import CPU68K
import CPUPPC
import Darwin
import Foundation
import Testing
import Toolbox

/// What the test routines heard.
nonisolated(unsafe) private var asrEvents: [(event: UInt16, reason: UInt16)] = []
nonisolated(unsafe) private var completions: [Int16] = []
nonisolated(unsafe) private var notified: [UInt32] = []

extension BlueBox {
    /// Hibernating with the network open (hibernate milestone M5): a
    /// snapshot taken while a program has MacTCP streams, Open Transport
    /// endpoints and AppleTalk sockets, names and connections, put back as a
    /// resumed process would (its old sockets closed, as when the process
    /// that hibernated ended).  What listens or is registered is made again
    /// on the new host; a connection is over, and the program hears so as
    /// each protocol says it, at the next poll; a synchronous call waiting
    /// gives up for the drain and is made again.
    @Suite(.serialized) struct HibernateNetwork {
        // ---- The machine

        /// The program's event loop (_SystemTask; BRA.S), with a stack.
        func startLoop() {
            _ = program([0xA9B4, 0x60FC])
        }

        func program(_ words: [UInt16]) -> UInt32 {
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000
            cpu.pointee.pc = code
            cpu.pointee.halted = false
            cpu.pointee.d.7 = 0x1234
            return code
        }

        /// Stopped at a safe point and saved; the machine then goes on.
        func hibernate(_ dir: URL, _ name: String) -> String {
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            let snap = dir.appendingPathComponent(name).path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            return snap
        }

        /// Put back as a new process would, the old one's sockets closed.
        func resume(_ snap: String) {
            network_close_sockets()
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
        }

        // ---- The host's end

        func sockaddr(_ port: UInt16) -> sockaddr_in {
            var sa = sockaddr_in()
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_addr.s_addr = inet_addr("127.0.0.1")
            sa.sin_port = port.bigEndian
            return sa
        }

        /// A listening socket on the loopback address; returns it and its port.
        func listener() -> (Int32, UInt16) {
            let fd = socket(AF_INET, SOCK_STREAM, 0)
            var sa = sockaddr(0)
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            withUnsafeMutablePointer(to: &sa) {
                $0.withMemoryRebound(to: Darwin.sockaddr.self, capacity: 1) {
                    _ = bind(fd, $0, len)
                    _ = listen(fd, 1)
                    _ = getsockname(fd, $0, &len)
                }
            }
            return (fd, UInt16(bigEndian: sa.sin_port))
        }

        /// A port nothing is bound to now.
        func freePort(_ type: Int32) -> UInt16 {
            let fd = socket(AF_INET, type, 0)
            defer { close(fd) }
            var sa = sockaddr(0)
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            withUnsafeMutablePointer(to: &sa) {
                $0.withMemoryRebound(to: Darwin.sockaddr.self, capacity: 1) {
                    _ = bind(fd, $0, len)
                    _ = getsockname(fd, $0, &len)
                }
            }
            return UInt16(bigEndian: sa.sin_port)
        }

        /// A connection to the port, made at once (-1 if refused).
        func connectTo(_ port: UInt16) -> Int32 {
            let fd = socket(AF_INET, SOCK_STREAM, 0)
            var sa = sockaddr(port)
            let r = withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: Darwin.sockaddr.self, capacity: 1) {
                    Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            if r < 0 {
                close(fd)
                return -1
            }
            return fd
        }

        func sendDatagram(_ text: String, to port: UInt16) {
            let fd = socket(AF_INET, SOCK_DGRAM, 0)
            defer { close(fd) }
            var sa = sockaddr(port)
            _ = withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: Darwin.sockaddr.self, capacity: 1) { to in
                    text.withCString { sendto(fd, $0, text.utf8.count, 0, to, socklen_t(MemoryLayout<sockaddr_in>.size)) }
                }
            }
        }

        // ---- MacTCP

        /// pascal void notify(stream, eventCode, userData, terminReason, icmpMsg *)
        static let asr: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)
            let reason = cpu_pop16(c)
            _ = cpu_pop32(c)
            let event = cpu_pop16(c)
            _ = cpu_pop32(c)
            asrEvents.append((event, reason))
            cpu_push32(c, ret)
        }

        /// A completion routine (C, the block in A0 and on the stack).
        static let completion: native_fn = { c, _ in
            guard let c else { return }
            completions.append(Int16(bitPattern: rd16(c.pointee.a.0 + UInt32(IOParam_ioResult_off))))
        }

        func ipp() -> UInt32 {
            let pb = tb_NewPtr(128, false, true)
            let name = tb_NewPtr(8, false, true)
            guestPString(".IPP", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == 0)
            return pb
        }

        /// A parameter block for csCode on the stream (0: none yet).
        func tcpPB(_ stream: UInt32, _ code: Int, completion: UInt32 = 0) -> UInt32 {
            let pb = tb_NewPtr(128, false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -49))
            wr32(pb + 28, stream)
            wr16(pb + UInt32(CntrlParam_csCode_off), UInt16(code))
            wr32(pb + UInt32(IOParam_ioCompletion_off), completion)
            return pb
        }

        func newStream(udp: Bool, port: UInt16 = 0, notify: UInt32 = 0) -> (UInt32, UInt16) {
            let pb = tcpPB(0, udp ? 20 : 30)
            let rcv = tb_NewPtr(8192, false, true)
            wr32(pb + 32, rcv)
            wr32(pb + 36, 8192)
            wr32(pb + 40, notify)       // the notify routine (both)
            if udp { wr16(pb + 44, port) }
            #expect(tb_PBControl(pb, false) == 0)
            return (rd32(pb + 28), udp ? rd16(pb + 44) : 0)
        }

        /// MacTCP resumed: a UDP stream is bound again to its port, a passive
        /// open listens again on its own and takes the connection made to it
        /// after resuming; a connection is over, its receive under way ending
        /// with connectionTerminated and its notification routine hearing
        /// TCPTerminate (a network failure).
        @Test func macTCPListensAgainAndEndsConnections() {
            let dir = launchTestApp()
            startLoop()
            _ = ipp()
            asrEvents = []
            completions = []
            let asr = native_register(Self.asr, "hibernate ASR", 0)
            let done = native_register(Self.completion, "hibernate MacTCP completion", 0)

            let (udp, uport) = newStream(udp: true)
            #expect(uport != 0)
            let (passive, _) = newStream(udp: false)
            let open = tcpPB(passive, 31, completion: done)               // TCPPassiveOpen
            #expect(tb_PBControl(open, true) == 1)
            let qport = rd16(open + 32 + UInt32(TCPOpenPB_localPort_off))
            #expect(qport != 0)

            let (lfd, port) = listener()
            defer { close(lfd) }
            let (conn, _) = newStream(udp: false, notify: asr)
            let active = tcpPB(conn, 32)                                     // TCPActiveOpen
            wr32(active + 32 + UInt32(TCPOpenPB_remoteHost_off), 0x7F00_0001)
            wr16(active + 32 + UInt32(TCPOpenPB_remotePort_off), port)
            #expect(tb_PBControl(active, false) == 0)
            let peer = accept(lfd, nil, nil)
            #expect(peer >= 0)
            let rcvBuf = tb_NewPtr(64, false, true)
            let rcv = tcpPB(conn, 37, completion: done)                       // TCPRcv
            wr32(rcv + 32 + UInt32(TCPReceivePB_rcvBuff_off), rcvBuf)
            wr16(rcv + 32 + UInt32(TCPReceivePB_rcvBuffLen_off), 64)
            #expect(tb_PBControl(rcv, true) == 1)

            let snap = hibernate(dir, "mactcp.bbhib")
            close(peer)
            resume(snap)
            #expect(completions.isEmpty && asrEvents.isEmpty)               // nothing during the restore
            mactcp_poll()
            #expect(completions == [-23012])                                  // connectionTerminated
            #expect(asrEvents.count == 1 && asrEvents.first?.event == 3 && asrEvents.first?.reason == 3)
            let status = tcpPB(conn, 39)                                      // TCPStatus
            #expect(tb_PBControl(status, false) == 0)
            #expect(rd8(status + 32 + UInt32(TCPStatusPB_connectionState_off)) == 0)

            // The UDP stream reads what comes to its port.
            sendDatagram("again", to: uport)
            let read = tcpPB(udp, 21)                                         // UDPRead
            wr16(read + 32, 2)                                                // a two-second timeout
            #expect(tb_PBControl(read, false) == 0)
            #expect(rd16(read + 32 + 12) == 5)

            // The passive open takes a connection made now.
            let caller = connectTo(qport)
            #expect(caller >= 0)
            defer { close(caller) }
            for _ in 0..<200 where completions.count < 2 {
                usleep(2000)
                mactcp_poll()
            }
            #expect(completions == [-23012, 0])
            for s in [udp, passive, conn] {
                #expect(tb_PBControl(tcpPB(s, s == udp ? 24 : 42), false) == 0)   // released
            }
        }

        /// A synchronous TCPRcv waiting for data gives its call up for the
        /// drain; made again going on it gets the data, and resumed (the
        /// connection gone) it ends with connectionTerminated.
        @Test func aSynchronousReceiveGivesUpForTheDrain() {
            let dir = launchTestApp()
            _ = ipp()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let (conn, _) = newStream(udp: false)
            let active = tcpPB(conn, 32)
            wr32(active + 32 + UInt32(TCPOpenPB_remoteHost_off), 0x7F00_0001)
            wr16(active + 32 + UInt32(TCPOpenPB_remotePort_off), port)
            #expect(tb_PBControl(active, false) == 0)
            let peer = accept(lfd, nil, nil)
            #expect(peer >= 0)
            defer { close(peer) }
            let buf = tb_NewPtr(64, false, true)
            let pb = tcpPB(conn, 37)
            wr32(pb + 32 + UInt32(TCPReceivePB_rcvBuff_off), buf)
            wr16(pb + 32 + UInt32(TCPReceivePB_rcvBuffLen_off), 64)
            // MOVEA.L #pb,A0; _Control; MOVE.W D0,D7; loop: _SystemTask; BRA.S loop
            let code = program([0x207C, UInt16(pb >> 16), UInt16(pb & 0xFFFF), 0xA004, 0x3E00, 0xA9B4, 0x60FC])
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_pc() == code + 6)                                 // on _Control
            #expect(String(cString: machine_drain_point()).hasSuffix("(abandoned)"))
            let snap = dir.appendingPathComponent("tcprcv.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            _ = "hi".withCString { send(peer, $0, 2, 0) }
            _ = machine_run(100_000)                                          // going on: the data
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd16(pb + 32 + UInt32(TCPReceivePB_rcvBuffLen_off)) == 2)

            resume(snap)
            _ = machine_run(100_000)
            #expect(Int16(bitPattern: UInt16(g_cpu!.pointee.d.7 & 0xFFFF)) == -23012)
            #expect(tb_PBControl(tcpPB(conn, 42), false) == 0)
        }

        // ---- Open Transport

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

        func cstr(_ s: String) -> UInt32 {
            let bytes = Array(s.utf8)
            let p = tb_NewPtr(Int32(bytes.count + 1), false, true)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func endpoint(_ config: String) -> UInt32 {
            let e = tb_NewPtr(4, false, true)
            let ep = ot("OTOpenEndpoint", ot("OTCreateConfiguration", cstr(config)), 0, 0, e)
            #expect(rd32(e) == 0 && ep != 0)
            return ep
        }

        /// A TBind for the loopback address and port, with a queue.
        func tbind(_ port: UInt16, qlen: UInt32 = 0) -> UInt32 {
            let b = tb_NewPtr(16, false, true), a = tb_NewPtr(16, false, true)
            wr16(a, 2)                                                        // AF_INET
            wr16(a + 2, port)
            wr32(a + 4, 0x7F00_0001)
            wr32(b, 16); wr32(b + 4, 16); wr32(b + 8, a); wr32(b + 12, qlen)
            return b
        }

        /// void notifier(context, code, result, cookie)
        static let notifier: @convention(c) (UnsafeMutablePointer<ppc_cpu>?) -> Void = { c in
            notified.append(withUnsafeBytes(of: c!.pointee.gpr) { $0.load(fromByteOffset: 4 * 4, as: UInt32.self) })
            c!.pointee.pc = c!.pointee.lr
        }

        /// Open Transport resumed: a UDP endpoint is bound again to its port,
        /// a listener listens again with its queue, and a connected endpoint
        /// has lost its connection: T_DISCONNECT for its notifier at the next
        /// poll, kOTLookErr for its calls until OTRcvDisconnect, after which
        /// it can connect again.
        @Test func openTransportListensAgainAndDisconnects() {
            let dir = launchTestApp()
            _ = cfm_ppc_cpu()                                                 // for the notifier
            g_ppc.halted = false
            startLoop()
            notified = []
            let uport = freePort(SOCK_DGRAM), lport = freePort(SOCK_STREAM)
            let udp = endpoint("udp")
            #expect(ot("OTBind", udp, tbind(uport), 0) == 0)
            let server = endpoint("tcp")
            #expect(ot("OTBind", server, tbind(lport, qlen: 1), 0) == 0)

            let (lfd, port) = listener()
            defer { close(lfd) }
            let conn = endpoint("tcp")
            #expect(ot("OTSetBlocking", conn) == 0)
            #expect(ot("OTBind", conn, 0, 0) == 0)
            let tcall = tb_NewPtr(40, false, true)
            wr32(tcall, 16); wr32(tcall + 4, 16); wr32(tcall + 8, rd32(tbind(port) + 8))
            #expect(ot("OTConnect", conn, tcall, 0) == 0)
            let peer = accept(lfd, nil, nil)
            #expect(peer >= 0)
            let tv = cfm_native_tvector(Self.notifier, "hibernate OT notifier")
            #expect(ot("OTInstallNotifier", conn, tv, 0) == 0)
            #expect(ot("OTSetNonBlocking", conn) == 0)

            let snap = hibernate(dir, "ot.bbhib")
            close(peer)
            resume(snap)
            #expect(notified.isEmpty)
            opentransport_poll()
            #expect(notified == [0x10])                                       // T_DISCONNECT
            #expect(ot("OTLook", conn) == 0x10)
            let buf = tb_NewPtr(64, false, true), flags = tb_NewPtr(4, false, true)
            #expect(Int32(bitPattern: ot("OTRcv", conn, buf, 64, flags)) == -3158) // kOTLookErr
            #expect(ot("OTRcvDisconnect", conn, 0) == 0)
            #expect(ot("OTGetEndpointState", conn) == 2)                      // T_IDLE
            #expect(ot("OTSetBlocking", conn) == 0)
            #expect(ot("OTConnect", conn, tcall, 0) == 0)                     // and again
            let again = accept(lfd, nil, nil)
            #expect(again >= 0)
            close(again)

            // The UDP endpoint reads what comes to its port.
            sendDatagram("datagram", to: uport)
            usleep(20_000)
            let ud = tb_NewPtr(36, false, true), from = tb_NewPtr(16, false, true), data = tb_NewPtr(64, false, true)
            wr32(ud, 16); wr32(ud + 8, from); wr32(ud + 24, 64); wr32(ud + 32, data)
            #expect(ot("OTRcvUData", udp, ud, flags) == 0)
            #expect(rd32(ud + 28) == 8)

            // The listener hears a connection made to its port now.
            let caller = connectTo(lport)
            #expect(caller >= 0)
            defer { close(caller) }
            usleep(20_000)
            #expect(ot("OTLook", server) == 0x01)                             // T_LISTEN
            for ep in [udp, server, conn] { #expect(ot("OTCloseProvider", ep) == 0) }
        }

        /// PowerPC code from its words.
        func ppcCode(_ words: [UInt32]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 4), false, false)
            for (i, w) in words.enumerated() { wr32(p + UInt32(i * 4), w) }
            return p
        }

        func load(_ r: UInt32, _ v: UInt32) -> [UInt32] {
            [0x3C00_0000 | r << 21 | v >> 16, 0x6000_0000 | r << 21 | r << 16 | (v & 0xFFFF)]
        }

        func call(_ tv: UInt32) -> [UInt32] {
            load(12, tv) + [0x800C_0000, 0x7C09_03A6, 0x4E80_0421] // lwz r0,0(r12); mtctr r0; bctrl
        }

        func export(_ name: String) -> UInt32 { cfm_native_tvector(cfm_export_fn(name)!, name) }

        func gpr(_ n: Int) -> UInt32 { withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: n * 4, as: UInt32.self) } }

        /// A PowerPC program's blocking OTRcv waiting for data gives its call
        /// up for the drain on its NATIVE word; made again going on it gets
        /// the data, and resumed it finds the connection gone (kOTLookErr).
        @Test func aBlockingOTRcvGivesUpForTheDrain() {
            let dir = launchTestApp()
            _ = cfm_ppc_cpu()
            g_cpu!.pointee.halted = false
            g_ppc.halted = false
            defer { g_ppc_mode = false }
            let (lfd, port) = listener()
            defer { close(lfd) }
            let conn = endpoint("tcp")
            #expect(ot("OTSetBlocking", conn) == 0)
            #expect(ot("OTBind", conn, 0, 0) == 0)
            let tcall = tb_NewPtr(40, false, true)
            wr32(tcall, 16); wr32(tcall + 4, 16); wr32(tcall + 8, rd32(tbind(port) + 8))
            #expect(ot("OTConnect", conn, tcall, 0) == 0)
            let peer = accept(lfd, nil, nil)
            #expect(peer >= 0)
            defer { close(peer) }
            let buf = tb_NewPtr(64, false, true), flags = tb_NewPtr(4, false, true)
            let rcv = export("OTRcv"), task = export("SystemTask")
            // r3-r6 the call's; OTRcv; mr r31,r3; loop: SystemTask; b loop
            let words = load(3, conn) + load(4, buf) + load(5, 64) + load(6, flags) + call(rcv) + [0x7C7F_1B78]
                + call(task) + [0x4BFF_FFEC]
            g_ppc_mode = true
            g_ppc.pc = ppcCode(words)
            withUnsafeMutableBytes(of: &g_ppc.gpr) {
                $0.storeBytes(of: tb_NewPtr(65536, false, true) + 60000, toByteOffset: 4, as: UInt32.self)
                $0.storeBytes(of: UInt32(0x1234), toByteOffset: 31 * 4, as: UInt32.self)
            }
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(String(cString: machine_drain_point()) == "OTRcv (abandoned)")
            #expect(g_ppc.pc == rd32(rcv))
            let snap = dir.appendingPathComponent("otrcv.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            _ = "data".withCString { send(peer, $0, 4, 0) }
            _ = machine_run(100_000)                                          // going on: the data
            #expect(gpr(31) == 4)

            resume(snap)
            _ = machine_run(100_000)
            #expect(Int32(bitPattern: gpr(31)) == -3158)                      // kOTLookErr: T_DISCONNECT
            #expect(ot("OTLook", conn) == 0x10)
        }

        // ---- AppleTalk

        func atOpen(_ name: String) -> Int16 {
            let pb = tb_NewPtr(80, false, true), np = tb_NewPtr(8, false, true)
            guestPString(name, at: np)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), np)
            #expect(tb_PBOpen(pb, false) == 0)
            return Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off)))
        }

        func atCall(_ ref: Int16, _ code: Int, _ pb: UInt32, async: Bool = false) -> Int16 {
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: ref))
            wr16(pb + UInt32(CntrlParam_csCode_off), UInt16(code))
            return driver_call(pb, async, Int32(DRIVER_CONTROL))
        }

        func atPoll(until done: () -> Bool) {
            for _ in 0..<500 where !done() {
                usleep(2000)
                appletalk_poll()
            }
        }

        func entity(_ parts: [String], at p: UInt32) {
            var q = p
            for s in parts {
                guestPString(s, at: q)
                q += UInt32(s.utf8.count + 1)
            }
        }

        /// Another LocalTalk node on the loopback group (LToUDP), as another
        /// copy of Blue Box would be: node 250.
        struct Peer {
            let fd: Int32
            static let node: UInt8 = 250
            static let id: [UInt8] = [0x50, 0x45, 0x45, 0x52]

            init() {
                fd = socket(AF_INET, SOCK_DGRAM, 0)
                var one: Int32 = 1
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
                setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, socklen_t(MemoryLayout<Int32>.size))
                var sa = sockaddr_in()
                sa.sin_family = sa_family_t(AF_INET)
                sa.sin_port = UInt16(1954).bigEndian
                _ = withUnsafePointer(to: &sa) {
                    $0.withMemoryRebound(to: Darwin.sockaddr.self, capacity: 1) {
                        bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
                var mr = ip_mreq()
                mr.imr_multiaddr.s_addr = inet_addr("239.192.76.84")
                mr.imr_interface.s_addr = inet_addr("127.0.0.1")
                setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, socklen_t(MemoryLayout<ip_mreq>.size))
                var ifa = in_addr(s_addr: inet_addr("127.0.0.1"))
                setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, socklen_t(MemoryLayout<in_addr>.size))
                var loop: UInt8 = 1
                setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1)
                let fl = fcntl(fd, F_GETFL)
                _ = fcntl(fd, F_SETFL, fl | O_NONBLOCK)
            }

            /// A DDP datagram (short header) to a node's socket.
            func send(to node: UInt8, socket dskt: UInt8, from sskt: UInt8, type: UInt8, _ data: [UInt8]) {
                let len = 5 + data.count
                let frame = Self.id + [node, Self.node, 1, UInt8(len >> 8), UInt8(len & 0xFF), dskt, sskt, type] + data
                var to = sockaddr_in()
                to.sin_family = sa_family_t(AF_INET)
                to.sin_port = UInt16(1954).bigEndian
                to.sin_addr.s_addr = inet_addr("239.192.76.84")
                _ = withUnsafePointer(to: &to) {
                    $0.withMemoryRebound(to: Darwin.sockaddr.self, capacity: 1) {
                        sendto(fd, frame, frame.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                    }
                }
            }

            /// The DDP data of the frames to this node of a DDP type (the
            /// stack polled meanwhile).
            func receive(type: UInt8, timeout ms: Int = 1000) -> [UInt8]? {
                for _ in 0..<(ms / 2) {
                    appletalk_poll()
                    var buf = [UInt8](repeating: 0, count: 2048)
                    while true {
                        let n = recv(fd, &buf, buf.count, 0)
                        if n < 4 + 3 + 5 { break }
                        if Array(buf[0..<4]) == Self.id || buf[4] != Self.node || buf[6] != 1 || buf[11] != type { continue }
                        return Array(buf[12..<n])
                    }
                    usleep(2000)
                }
                return nil
            }
        }

        /// AppleTalk resumed: the link is opened again and the node claimed
        /// again (the same address, no one having taken it); another node's
        /// NBP lookup finds the name the program registered, and a request
        /// to the program's ATP socket reaches the GetRequest it had waiting.
        @Test func appleTalkNodeNamesAndSocketsComeBack() {
            #expect(machine_set_appletalk("local"))
            let dir = launchTestApp()
            startLoop()
            let mpp = atOpen(".MPP"), atp = atOpen(".ATP")
            let node = at_node()
            let pb = tb_NewPtr(80, false, true)
            #expect(atCall(atp, 254, pb) == 0)                                // openATPSkt
            let skt = rd8(pb + 28)
            let nte = tb_NewPtr(120, false, true)
            wr8(nte + 7, skt)
            entity(["Sleeper", "BlueBoxHib", "*"], at: nte + 9)
            gmemset(pb + 28, 0, 40)
            wr32(pb + 30, nte)
            #expect(atCall(mpp, 253, pb) == 0)                                // registerName
            let get = tb_NewPtr(80, false, true), reqbuf = tb_NewPtr(16, false, true)
            wr8(get + 28, skt)
            wr16(get + 34, 16)
            wr32(get + 36, reqbuf)
            #expect(atCall(atp, 253, get, async: true) == 1)                  // getRequest

            let snap = hibernate(dir, "atalk.bbhib")
            resume(snap)
            #expect(at_node() == node)
            #expect(rd8(rd32(UInt32(LM_ABusVars))) == node)
            let peer = Peer()
            defer { close(peer.fd) }
            // LkUp (1 tuple, ID 9): the asker at node 250 socket 200, "=:BlueBoxHib@*"
            peer.send(to: 255, socket: 2, from: 200, type: 2,
                      [0x21, 9, 0, 0, Peer.node, 200, 0, 1, 0x3D, 10] + Array("BlueBoxHib".utf8) + [1, 0x2A])
            let reply = peer.receive(type: 2)
            #expect(reply != nil)
            if let r = reply {
                #expect(r[0] == 0x31 && r[1] == 9)                            // LkUp-Reply, one tuple
                #expect(r[4] == node && r[5] == skt)
                #expect(String(decoding: r[8..<(8 + Int(r[7]))], as: UTF8.self) == "Sleeper")
            }
            // TReq, TID 77, bitmap 1, "hi"
            peer.send(to: node, socket: skt, from: 201, type: 3, [0x40, 1, 0, 77, 0, 0, 0, 0, 0x68, 0x69])
            atPoll { rd16(get + 16) != 1 }
            #expect(rd16(get + 16) == 0 && rd16(get + 34) == 2 && rd8(get + 32) == Peer.node)
        }

        /// ADSP resumed: an open connection is torn down at the next poll (its
        /// ends closed with eTearDown, a read under way finished as for a
        /// close), as when the other end stops answering.
        @Test func anADSPConnectionIsTornDown() {
            #expect(machine_set_appletalk("local"))
            let dir = launchTestApp()
            startLoop()
            let mpp = atOpen(".MPP"), dsp = atOpen(".DSP")
            let pb = tb_NewPtr(80, false, true)
            wr8(pb + 28, 1)
            #expect(atCall(mpp, 256, pb) == 0)                                // setSelfSend
            let node = at_node()
            func newEnd(_ code: Int) -> (ref: UInt16, skt: UInt8, ccb: UInt32) {
                let p = tb_NewPtr(80, false, true), ccb = tb_NewPtr(242, false, true)
                wr32(p + 34, ccb)
                wr16(p + 42, 600)
                wr16(p + 48, 600)
                wr32(p + 54, tb_NewPtr(570, false, true))
                #expect(atCall(dsp, code, p) == 0)
                return (rd16(p + 32), rd8(p + 58), ccb)
            }
            let listener = newEnd(251), client = newEnd(255), server = newEnd(255)
            let listen = tb_NewPtr(80, false, true)
            wr16(listen + 32, listener.ref)
            #expect(atCall(dsp, 249, listen, async: true) == 1)              // dspCLListen
            let req = tb_NewPtr(80, false, true)
            wr16(req + 32, client.ref)
            wr8(req + 40, node)
            wr8(req + 41, listener.skt)
            wr8(req + 64, 1)                                                  // ocRequest
            #expect(atCall(dsp, 253, req, async: true) == 1)
            atPoll { rd16(listen + 16) != 1 }
            let acc = tb_NewPtr(80, false, true)
            gmemcpy(acc + 34, listen + 34, 30)
            wr16(acc + 32, server.ref)
            wr8(acc + 64, 3)                                                  // ocAccept
            #expect(atCall(dsp, 253, acc, async: true) == 1)
            atPoll { rd16(req + 16) != 1 && rd16(acc + 16) != 1 }
            #expect(rd16(client.ccb + 6) == 4 && rd16(server.ccb + 6) == 4)   // sOpen
            let r = tb_NewPtr(80, false, true), buf = tb_NewPtr(100, false, true)
            wr16(r + 32, server.ref)
            wr16(r + 34, 100)
            wr32(r + 38, buf)
            #expect(atCall(dsp, 246, r, async: true) == 1)                    // dspRead, waiting

            let snap = hibernate(dir, "adsp.bbhib")
            resume(snap)
            #expect(rd16(server.ccb + 6) == 4)                                // nothing during the restore
            appletalk_poll()
            #expect(rd16(client.ccb + 6) == 6 && rd16(server.ccb + 6) == 6)   // sClosed
            #expect(rd8(client.ccb + 8) & 0x40 != 0 && rd8(server.ccb + 8) & 0x40 != 0) // eTearDown
            #expect(rd16(r + 16) == 0 && rd16(r + 36) == 0)
            #expect(rd16(listener.ccb + 6) == 1)                              // the listener goes on: sListening
        }

        /// A synchronous ATP SendRequest gives its call up for the drain; made
        /// again (going on, or resumed) it waits for the same transaction,
        /// which the responder answers: no second request.
        @Test func aSynchronousATPRequestGivesUpForTheDrain() {
            #expect(machine_set_appletalk("local"))
            let dir = launchTestApp()
            let mpp = atOpen(".MPP"), atp = atOpen(".ATP")
            let pb = tb_NewPtr(80, false, true)
            wr8(pb + 28, 1)
            #expect(atCall(mpp, 256, pb) == 0)                                // setSelfSend
            let node = at_node()
            gmemset(pb, 0, 80)
            #expect(atCall(atp, 254, pb) == 0)
            let skt = rd8(pb + 28)
            let get = tb_NewPtr(80, false, true), reqbuf = tb_NewPtr(16, false, true)
            wr8(get + 28, skt)
            wr16(get + 34, 16)
            wr32(get + 36, reqbuf)
            #expect(atCall(atp, 253, get, async: true) == 1)

            let req = tb_NewPtr(80, false, true)
            let bds = tb_NewPtr(12, false, true), b0 = tb_NewPtr(16, false, true)
            wr16(bds, 16)
            wr32(bds + 2, b0)
            wr16(req + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: atp))
            wr16(req + UInt32(CntrlParam_csCode_off), 255)                    // sendRequest
            wr8(req + 32, node)
            wr8(req + 33, skt)
            wr32(req + 40, bds)
            wr8(req + 44, 1)
            wr8(req + 45, 1)                                                  // a second
            wr8(req + 47, 255)                                                // for ever
            // MOVEA.L #req,A0; _Control; MOVE.W D0,D7; loop: _SystemTask; BRA.S loop
            _ = program([0x207C, UInt16(req >> 16), UInt16(req & 0xFFFF), 0xA004, 0x3E00, 0xA9B4, 0x60FC])
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(String(cString: machine_drain_point()).hasSuffix("(abandoned)"))
            atPoll { rd16(get + 16) != 1 }
            #expect(rd16(get + 16) == 0)                                      // the responder heard it
            let tid = rd16(req + 22)
            let snap = dir.appendingPathComponent("atpreq.bbhib").path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()

            func respond() {
                let rsp = tb_NewPtr(80, false, true), rbds = tb_NewPtr(12, false, true), p = tb_NewPtr(8, false, true)
                for (k, c) in "ok".utf8.enumerated() { wr8(p + UInt32(k), c) }
                wr32(rbds + 2, p)
                wr16(rbds + 6, 2)
                wr8(rsp + 28, skt)
                wr8(rsp + 29, 16)                                             // EOM
                wr32(rsp + 30, rd32(get + 30))
                wr32(rsp + 40, rbds)
                wr8(rsp + 45, 1)
                wr16(rsp + 46, rd16(get + 46))
                #expect(atCall(atp, 252, rsp) == 0)                           // sendResponse
            }
            respond()
            _ = machine_run(100_000)                                          // going on
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd8(req + 46) == 1 && rd16(req + 22) == tid)

            resume(snap)
            respond()
            _ = machine_run(100_000)
            #expect(g_cpu!.pointee.d.7 & 0xFFFF == 0 && rd8(req + 46) == 1 && rd16(req + 22) == tid)
            #expect(rd8(b0) == 0x6F)
        }

        // ---- The PPC Toolbox

        func portName(_ name: String) -> UInt32 {
            let p = tb_NewPtr(72, false, true)
            guestPString(name, at: p + 2)
            guestPString("Test", at: p + 38)
            return p
        }

        func ppcOpen(_ name: String) -> UInt16 {
            let pb = tb_NewPtr(64, false, true)
            wr8(pb + 44, 1) // ppcServiceRealTime
            wr32(pb + 46, portName(name))
            #expect(tb_PPCOpenSync(pb) == 0)
            return rd16(pb + 38)
        }

        func ppcPump() {
            for _ in 0..<100 { usleep(2000); ppc_toolbox_poll() }
        }

        /// The PPC Toolbox resumed: a port listens again and a PPCInform
        /// waiting on it hears a session started after resuming; a session
        /// is over, its read under way ending with sessClosedErr.
        @Test func ppcToolboxPortsListenAgain() {
            let dir = launchTestApp()
            startLoop()
            let tag = String(getpid() % 10000)
            let a = ppcOpen("HibA" + tag), b = ppcOpen("HibB" + tag)
            let inform = tb_NewPtr(64, false, true), who = tb_NewPtr(72, false, true)
            wr16(inform + 38, b); wr8(inform + 45, 1); wr32(inform + 46, who)
            #expect(tb_PPCInformAsync(inform) == 0)
            let start = tb_NewPtr(70, false, true)
            wr16(start + 38, a); wr32(start + 46, portName("HibB" + tag))
            #expect(tb_PPCStartSync(start) == 0)
            ppcPump()
            #expect(rd16(inform + 16) == 0)
            let r = tb_NewPtr(70, false, true), buf = tb_NewPtr(64, false, true)
            wr32(r + 40, rd32(inform + 40)); wr32(r + 44, 64); wr32(r + 52, buf)
            #expect(tb_PPCReadAsync(r) == 0 && rd16(r + 16) == 1)
            let inform2 = tb_NewPtr(64, false, true)
            wr16(inform2 + 38, b); wr8(inform2 + 45, 1); wr32(inform2 + 46, tb_NewPtr(72, false, true))
            #expect(tb_PPCInformAsync(inform2) == 0)

            let snap = hibernate(dir, "ppc.bbhib")
            resume(snap)
            ppc_toolbox_poll()
            #expect(Int16(bitPattern: rd16(r + 16)) == -917)                  // sessClosedErr
            #expect(rd16(inform2 + 16) == 1)
            let again = tb_NewPtr(70, false, true)
            wr16(again + 38, a); wr32(again + 46, portName("HibB" + tag))
            #expect(tb_PPCStartSync(again) == 0)
            ppcPump()
            #expect(rd16(inform2 + 16) == 0)
            for port in [a, b] {
                let c = tb_NewPtr(64, false, true)
                wr16(c + 38, port)
                #expect(tb_PPCCloseSync(c) == 0)
            }
        }

        // ---- The serial ports

        func serOpen(_ name: String) -> Int16 {
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true), np = tb_NewPtr(8, false, true)
            guestPString(name, at: np)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), np)
            #expect(tb_PBOpen(pb, false) == 0)
            return Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off)))
        }

        func serWrite(_ ref: Int16, _ s: String) {
            let bytes = Array(s.utf8)
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true), buf = tb_NewPtr(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(buf + UInt32(i), b) }
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: ref))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), UInt32(bytes.count))
            #expect(tb_PBWrite(pb, false) == 0)
        }

        func serAvailable(_ ref: Int16) -> String {
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: ref))
            wr16(pb + UInt32(CntrlParam_csCode_off), 2)
            #expect(tb_PBStatus(pb, false) == 0)
            let n = rd32(pb + UInt32(CntrlParam_csParam_off))
            guard n > 0 else { return "" }
            let buf = tb_NewPtr(Int32(n), false, false)
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), n)
            #expect(tb_PBRead(pb, false) == 0)
            return String(decoding: (0..<rd32(pb + UInt32(IOParam_ioActCount_off))).map { rd8(buf + $0) }, as: UTF8.self)
        }

        func serWait(_ ref: Int16, _ text: String) -> String {
            var got = ""
            for _ in 0..<100 where !got.contains(text) {
                usleep(10000)
                serial_poll()
                got += serAvailable(ref)
            }
            return got
        }

        /// The modem port resumed: its call is over (NO CARRIER), and the
        /// port it answers calls on rings again for a call made then.
        @Test func theModemHangsUpAndRingsAgain() {
            let mport = freePort(SOCK_STREAM)
            #expect(machine_set_serial(0, "modem:\(mport)"))
            defer { _ = machine_set_serial(0, "modem") }
            let dir = launchTestApp()
            startLoop()
            let input = serOpen(".AIn"), output = serOpen(".AOut")
            let (lfd, port) = listener()
            defer { close(lfd) }
            serWrite(output, "ATDT127.0.0.1:\(port)\r")
            #expect(serWait(input, "CONNECT").contains("CONNECT"))
            let conn = accept(lfd, nil, nil)
            #expect(conn >= 0)

            let snap = hibernate(dir, "modem.bbhib")
            close(conn)
            resume(snap)
            #expect(serWait(input, "NO CARRIER").contains("NO CARRIER"))
            let caller = connectTo(mport)
            #expect(caller >= 0)
            defer { close(caller) }
            #expect(serWait(input, "RING").contains("RING"))
        }
    }
}
