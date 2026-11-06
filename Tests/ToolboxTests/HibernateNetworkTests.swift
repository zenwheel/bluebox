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

    }
}
