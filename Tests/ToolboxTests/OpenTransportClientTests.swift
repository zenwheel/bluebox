import CCore
import Darwin
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Open Transport's client interface from PowerPC code, beyond the
    /// endpoint calls: the utilities, the port registry, option management,
    /// XTI, the Internet services and the provider calls.  The exports are
    /// called as a PowerPC caller would, arguments in r3 on.
    @Suite struct OpenTransportClient {
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

        func cstr(_ s: String) -> UInt32 {
            let bytes = Array(s.utf8)
            let p = tb_NewPtr(Int32(bytes.count + 1), false, true)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func cstring(at p: UInt32, max: Int = 256) -> String {
            var out = [UInt8]()
            var i: UInt32 = 0
            while rd8(p + i) != 0 && out.count < max { out.append(rd8(p + i)); i += 1 }
            return String(decoding: out, as: UTF8.self)
        }

        func block(_ n: Int) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

        /// A PowerPC routine of these instruction words, as a transition vector.
        func proc(_ words: [UInt32]) -> UInt32 {
            let code = tb_NewPtr(Int32(words.count * 4), false, true), tv = tb_NewPtr(8, false, true)
            for (i, w) in words.enumerated() { wr32(code + UInt32(i * 4), w) }
            wr32(tv, code)
            return tv
        }

        /// A listening socket on 127.0.0.1: the descriptor and its port.
        func listener() -> (Int32, UInt16) {
            let fd = socket(AF_INET, SOCK_STREAM, 0)
            var sa = sockaddr_in()
            sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_addr.s_addr = inet_addr("127.0.0.1")
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            withUnsafeMutablePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    _ = bind(fd, $0, len)
                    _ = listen(fd, 1)
                    _ = getsockname(fd, $0, &len)
                }
            }
            return (fd, UInt16(bigEndian: sa.sin_port))
        }

        func openEndpoint(_ config: String) -> UInt32 {
            let e = tb_NewPtr(4, false, true)
            let ep = ot("OTOpenEndpoint", ot("OTCreateConfiguration", cstr(config)), 0, 0, e)
            #expect(rd32(e) == 0 && ep != 0, "\(config)")
            return ep
        }

        // MARK: - Utilities

        @Test func bitMaps() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            let map = block(4)
            wr32(map, 0)
            #expect(ot("OTSetBit", map, 0) == 0)
            #expect(ot("OTSetBit", map, 0) == 1) // it returns the bit's previous value
            #expect(ot("OTSetBit", map, 9) == 0)
            #expect(rd8(map) == 1 && rd8(map + 1) == 2)
            #expect(ot("OTTestBit", map, 9) == 1 && ot("OTTestBit", map, 8) == 0)
            #expect(ot("OTClearBit", map, 9) == 1 && rd8(map + 1) == 0)
            // The first clear bit from bit 0, within 3 bits: bit 1; then 2; then none.
            #expect(ot("OTSetFirstClearBit", map, 0, 3) == 1)
            #expect(ot("OTSetFirstClearBit", map, 0, 3) == 2)
            #expect(err(ot("OTSetFirstClearBit", map, 0, 3)) == -3201) // kOTNotFoundErr
            #expect(ot("OTSetFirstClearBit", map, 0, 32) == 3)
        }

        @Test func enqueueAndDequeue() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            // Objects with their link at offset 4.
            let a = block(8), b = block(8), head = block(4)
            wr32(head, 0)
            ot("OTEnqueue", head, a, 4)
            ot("OTEnqueue", head, b, 4)
            #expect(rd32(head) == b && rd32(b + 4) == a)
            #expect(ot("OTDequeue", head, 4) == b)
            #expect(ot("OTDequeue", head, 4) == a)
            #expect(ot("OTDequeue", head, 4) == 0)
        }

        @Test func timeStamps() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            let t0 = block(8), t1 = block(8), delta = block(8)
            ot("OTGetTimeStamp", t0)
            usleep(20_000)
            ot("OTGetTimeStamp", t1)
            // result = first minus second: the later stamp is named first.
            #expect(ot("OTSubtractTimeStamps", delta, t1, t0) == delta)
            let us = ot("OTTimeStampInMicroseconds", delta), ms = ot("OTTimeStampInMilliseconds", delta)
            #expect(us >= 20_000 && us < 5_000_000)
            #expect(ms >= 20 && ms < 5_000)
            #expect(ot("OTGetClockTimeInSecs") < 60)
            #expect(ot("OTGetTimerTicks") <= tb_TickCount() + 1)
        }

        @Test func hashList() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            // A hash procedure that is the link's address; a search procedure
            // that says yes to the link whose address is the reference.
            let hash = proc([0x4E80_0020]) // blr: r3 stays the link
            let match = proc([0x7C03_2000, 0x3860_0000, 0x4C82_0020, 0x3860_0001, 0x4E80_0020])
            let need = ot("OTCalculateHashListMemoryNeeds", 10)
            #expect(need >= 12 + 40)
            let mem = block(Int(need))
            #expect(ot("OTInitHashList", mem, need, hash) == 0)
            #expect(err(ot("OTInitHashList", mem, 8, hash)) < 0)
            let links = (0..<5).map { _ in block(8) }
            for l in links { ot("OTAddToHashList", mem, l) }
            for l in links {
                #expect(ot("OTIsInHashList", mem, l) == 1)
                #expect(ot("OTFindInHashList", mem, match, l, l) == l)
            }
            #expect(ot("OTFindInHashList", mem, match, 0x1234, links[0]) == 0)
            #expect(ot("OTRemoveFromHashList", mem, match, links[2], links[2]) == links[2])
            #expect(ot("OTIsInHashList", mem, links[2]) == 0)
            #expect(ot("OTRemoveLinkFromHashList", mem, links[3]) == 1)
            #expect(ot("OTRemoveLinkFromHashList", mem, links[3]) == 0)
            #expect(ot("OTIsInHashList", mem, links[0]) == 1 && ot("OTIsInHashList", mem, links[4]) == 1)
        }

        @Test func randomNumbers() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            let seed = block(4)
            wr32(seed, ot("OTGetRandomSeed"))
            var seen = Set<UInt32>()
            for _ in 0..<200 {
                let r = ot("OTGetRandomNumber", seed, 10, 20)
                #expect(r >= 10 && r <= 20)
                seen.insert(r)
            }
            #expect(seen.count > 5)
            wr32(seed, 7)
            let first = ot("OTGetRandomNumber", seed, 0, 1000)
            wr32(seed, 7)
            #expect(ot("OTGetRandomNumber", seed, 0, 1000) == first)
        }

        @Test func gates() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            let gate = block(20)
            let run = proc([0x3860_0001, 0x4E80_0020]) // li r3,1; blr
            ot("OTInitGate", gate, run)
            let l1 = block(4), l2 = block(4), l3 = block(4)
            #expect(ot("OTEnterGate", gate, l1) == 1) // the gate was open: go on
            #expect(ot("OTEnterGate", gate, l2) == 0) // inside already: queued
            #expect(ot("OTEnterGate", gate, l3) == 0)
            #expect(rd32(gate + 12) == 2)
            #expect(ot("OTLeaveGate", gate) == 1) // the procedure ran for each; the gate is open
            #expect(rd32(gate + 12) == 0 && rd32(gate + 16) == 0)
            #expect(ot("OTEnterGate", gate, l1) == 1)
            // A procedure that says no keeps the gate.
            let hold = proc([0x3860_0000, 0x4E80_0020])
            ot("OTInitGate", gate, hold)
            #expect(ot("OTEnterGate", gate, l1) == 1)
            #expect(ot("OTEnterGate", gate, l2) == 0)
            #expect(ot("OTLeaveGate", gate) == 0)
        }

        @Test func interruptLevelAndSyncCalls() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            #expect(ot("OTIsAtInterruptLevel") == 0 && ot("OTCanMakeSyncCall") == 1 && ot("OTCanLoadLibraries") == 1)
            ot("OTEnterInterrupt")
            #expect(ot("OTIsAtInterruptLevel") == 1 && ot("OTCanMakeSyncCall") == 0 && ot("OTCanLoadLibraries") == 0)
            ot("OTLeaveInterrupt")
            #expect(ot("OTIsAtInterruptLevel") == 0 && ot("OTCanMakeSyncCall") == 1)
            ot("OTLeaveInterrupt") // unbalanced: harmless
            #expect(ot("OTIsAtInterruptLevel") == 0)
        }

        @Test func portReferences() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            let ref = ot("OTCreatePortRef", 3, 10, 5, 7) // PCI, Ethernet, slot 5, other 7
            #expect(ot("OTGetBusTypeFromPortRef", ref) == 3)
            #expect(ot("OTGetDeviceTypeFromPortRef", ref) == 10)
            let other = block(2)
            #expect(ot("OTGetSlotFromPortRef", ref, other) == 5 && rd16(other) == 7)
            let r2 = ot("OTSetDeviceTypeInPortRef", ref, 14)
            #expect(ot("OTGetDeviceTypeFromPortRef", r2) == 14 && ot("OTGetBusTypeFromPortRef", r2) == 3)
            let r3 = ot("OTSetBusTypeInPortRef", ref, 5)
            #expect(ot("OTGetBusTypeFromPortRef", r3) == 5 && ot("OTGetDeviceTypeFromPortRef", r3) == 10)
        }

        @Test func noCopyBuffers() {
            launchTestApp()
            _ = cfm_ppc_cpu() // callbacks run on the PowerPC processor
            let (lfd, port) = listener()
            defer { close(lfd) }
            let ep = openEndpoint("tcp")
            #expect(ot("OTSetBlocking", ep) == 0 && ot("OTBind", ep, 0, 0) == 0)
            let dns = block(260)
            let len = ot("OTInitDNSAddress", dns, cstr("127.0.0.1:\(port)"))
            let call = block(40)
            wr32(call, len); wr32(call + 4, len); wr32(call + 8, dns)
            #expect(ot("OTConnect", ep, call, 0) == 0)
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            _ = "hello, buffers".withCString { send(conn, $0, 14, 0) }
            usleep(30_000)
            // Receive with kOTNetbufDataIsOTBufferStar: the address of an OTBuffer * given as the buffer.
            let slot = block(4), flags = block(4)
            let n = ot("OTRcv", ep, slot, 0xFFFF_FFFD, flags)
            #expect(n == 14)
            let buffer = rd32(slot)
            #expect(buffer != 0 && ot("OTBufferDataSize", buffer) == 14)
            let info = block(12), out = block(32), cnt = block(4)
            wr32(info, buffer); wr32(info + 4, 0); wr32(cnt, 32)
            ot("OTReadBuffer", info, out, cnt)
            #expect(rd32(cnt) == 14 && cstring(at: out, max: 14) == "hello, buffers")
            ot("OTReleaseBuffer", buffer)
            // the buffer was disposed; its magic is gone
            #expect(rd32(buffer + 20) == 0)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        // MARK: - The port registry

        /// All the ports the registry lists, as records.
        func allPorts() -> [UInt32] {
            var out = [UInt32]()
            for i in 0..<64 {
                let rec = block(296)
                if ot("OTGetIndexedPort", rec, UInt32(i)) == 0 { break }
                out.append(rec)
            }
            return out
        }

        @Test func portsListTheHostsEthernet() {
            launchTestApp()
            let ports = allPorts()
            #expect(!ports.isEmpty) // a Mac with Ethernet, whatever the host has
            let first = ports[0]
            #expect(cstring(at: first + 24) == "enet" && cstring(at: first + 60) == "enet")
            let ref = rd32(first)
            #expect(ot("OTGetDeviceTypeFromPortRef", ref) == 10) // kOTEthernetDevice
            #expect(ot("OTGetBusTypeFromPortRef", ref) == 1) // kOTMotherboardBus
            #expect(rd32(first + 8) & 1 == 1) // kOTPortIsDLPI
            // Found again by name and by reference.
            let byName = block(296), byRef = block(296)
            #expect(ot("OTFindPort", byName, cstr("enet")) == 1 && rd32(byName) == ref)
            #expect(ot("OTFindPortByRef", byRef, ref) == 1 && cstring(at: byRef + 24) == "enet")
            #expect(ot("OTFindPort", byName, cstr("nosuchport")) == 0)
            #expect(ot("OTFindPortByRef", byRef, 0x1234_5678) == 0)
            // A person-readable name and no icon.
            let name = block(256)
            ot("OTGetUserPortNameFromPortRef", ref, name)
            #expect(pstring(at: name) == "Ethernet (built-in)")
            #expect(ot("OTGetPortIconFromPortRef", ref, block(80)) == 0)
            // What it carries.
            #expect(ot("OTIsPortCompatibleWith", first, cstr("tcp")) == 1)
            #expect(ot("OTIsPortCompatibleWith", first, cstr("ddp")) == 1)
            #expect(ot("OTIsPortCompatibleWith", first, cstr("ppp")) == 0)
            // The indexed list ends.
            #expect(ot("OTGetIndexedPort", block(296), UInt32(ports.count)) == 0)
            // Endpoints run on the first port.
            let ep = openEndpoint("tcp")
            #expect(ot("OTGetProviderPortRef", ep) == ref)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func aProgramRegistersAPort() {
            launchTestApp()
            let before = allPorts().count
            let rec = block(296)
            gmemset(rec, 0, 296)
            let ref = ot("OTCreatePortRef", 3, 10, 2, 0)
            wr32(rec, ref)
            wr32(rec + 4, 1) // active
            wr32(rec + 8, 1) // DLPI
            for (i, b) in "enet".utf8.enumerated() { wr8(rec + 24 + UInt32(i), b) }
            for (i, b) in "enet".utf8.enumerated() { wr8(rec + 60 + UInt32(i), b) }
            let cookie = block(4)
            #expect(ot("OTRegisterPort", rec, cookie) == 0)
            // "enet" was taken: it is listed under the next free name, which comes back.
            let name = cstring(at: rec + 24)
            #expect(name != "enet" && name.hasPrefix("enet"))
            #expect(allPorts().count == before + 1)
            let found = block(296)
            #expect(ot("OTFindPort", found, cstr(name)) == 1 && rd32(found) == ref)
            let back = block(4)
            #expect(ot("OTUnregisterPort", cstr(name), back) == 0 && rd32(back) == cookie)
            #expect(allPorts().count == before)
            #expect(err(ot("OTUnregisterPort", cstr(name), back)) == -3201)
        }

        /// A client notifier that records its last event: code, result and cookie, at a cell.
        func recorder(into cell: UInt32) -> UInt32 {
            let hi = (cell >> 16) & 0xFFFF, lo = cell & 0xFFFF
            return proc([0x3D20_0000 | hi, 0x6129_0000 | lo, 0x9089_0000, 0x90A9_0004, 0x90C9_0008, 0x4E80_0020])
        }

        @Test func clientsHearOfPortsAndTheStack() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let cell = block(16)
            gmemset(cell, 0, 16)
            let name = block(8)
            wr8(name, 4)
            for (i, b) in "test".utf8.enumerated() { wr8(name + 1 + UInt32(i), b) }
            #expect(ot("OTRegisterAsClient", name, recorder(into: cell)) == 0)
            let port = allPorts()[0]
            let ref = rd32(port)
            #expect(ot("OTChangePortState", ref, 0x2500_0003, 7) == 0) // kOTPortOffline
            opentransport_poll()
            #expect(rd32(cell) == 0x2500_0003 && rd32(cell + 4) == 7 && rd32(cell + 8) == ref)
            let rec = block(296)
            #expect(ot("OTFindPortByRef", rec, ref) == 1 && rd32(rec + 4) & 8 == 8) // kOTPortIsOffline
            #expect(ot("OTChangePortState", ref, 0x2500_0004, 0) == 0) // kOTPortOnline
            opentransport_poll()
            #expect(rd32(cell) == 0x2500_0004)
            #expect(ot("OTFindPortByRef", rec, ref) == 1 && rd32(rec + 4) & 8 == 0)
            #expect(err(ot("OTChangePortState", 0x1234_5678, 0x2500_0003, 0)) == -3201)
            #expect(err(ot("OTChangePortState", ref, 0x2500_0099, 0)) == -3167)
            // The first use of TCP/IP loads the stack: the clients are told.
            let ep = openEndpoint("tcp")
            opentransport_poll()
            #expect(rd32(cell) == 0x2700_0002) // kOTStackWasLoaded
            // Another client's own notification.
            ot("OTNotifyAllClients", 0x2600_0001, 3, 99)
            opentransport_poll()
            #expect(rd32(cell) == 0x2600_0001 && rd32(cell + 4) == 3 && rd32(cell + 8) == 99)
            #expect(ot("OTUnregisterAsClient") == 0)
            ot("OTNotifyAllClients", 0x2600_0002, 0, 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x2600_0001) // no longer registered
            #expect(err(ot("OTUnregisterAsClient")) == -3201)
            #expect(ot("OTCloseProvider", ep) == 0)
            // Scanning the ports again finds the same ones.
            let n = allPorts().count
            #expect(ot("OTScanPorts") == 0 && allPorts().count == n)
        }

        // MARK: - Options

        /// An option buffer in guest memory: its address and length.
        func optBuf(_ items: [(UInt32, UInt32, [UInt8])]) -> (UInt32, UInt32) {
            var bytes = [UInt8]()
            func be(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255)] }
            for (level, name, value) in items {
                bytes += be(UInt32(16 + value.count)) + be(level) + be(name) + be(0) + value
                while bytes.count % 4 != 0 { bytes.append(0) }
            }
            let p = block(max(bytes.count, 4))
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return (p, UInt32(bytes.count))
        }

        func long(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255)] }

        struct Reply {
            var status: Int32
            var flags: UInt32
            var options: [(level: UInt32, name: UInt32, status: UInt32, value: [UInt8])]
        }

        /// OTOptionManagement with these options and flags.
        func optMgmt(_ ep: UInt32, _ flags: UInt32, _ items: [(UInt32, UInt32, [UInt8])]) -> Reply {
            let (buf, len) = optBuf(items)
            let req = block(16), ret = block(16), out = block(2048)
            wr32(req, len); wr32(req + 4, len); wr32(req + 8, buf); wr32(req + 12, flags)
            wr32(ret, 2048); wr32(ret + 4, 0); wr32(ret + 8, out); wr32(ret + 12, 0)
            let st = err(ot("OTOptionManagement", ep, req, ret))
            var options: [(level: UInt32, name: UInt32, status: UInt32, value: [UInt8])] = []
            var at: UInt32 = 0
            let total = rd32(ret + 4)
            while at + 16 <= total {
                let l = rd32(out + at)
                options.append((rd32(out + at + 4), rd32(out + at + 8), rd32(out + at + 12),
                                (16..<l).map { rd8(out + at + $0) }))
                at += (l + 3) & ~3
            }
            return Reply(status: st, flags: rd32(ret + 12), options: options)
        }

        func u32(_ b: [UInt8]) -> UInt32 { b.count >= 4 ? UInt32(b[0]) << 24 | UInt32(b[1]) << 16 | UInt32(b[2]) << 8 | UInt32(b[3]) : 0 }

        let T_NEGOTIATE: UInt32 = 4, T_CHECK: UInt32 = 8, T_DEFAULT: UInt32 = 0x10, T_CURRENT: UInt32 = 0x80
        let T_SUCCESS: UInt32 = 0x20, T_FAILURE: UInt32 = 0x40, T_NOTSUPPORT: UInt32 = 0x400, T_READONLY: UInt32 = 0x200
        let INET_IP: UInt32 = 0, INET_TCP: UInt32 = 6, INET_UDP: UInt32 = 0x11, XTI_GENERIC: UInt32 = 0xFFFF

        @Test func tcpOptionsAreTheHostSocketsOwn() {
            launchTestApp()
            let ep = openEndpoint("tcp")
            // No delay: off by default, then on - read from the host socket.
            var r = optMgmt(ep, T_CURRENT, [(INET_TCP, 1, long(0))])
            #expect(r.status == 0 && r.flags == T_SUCCESS && u32(r.options[0].value) == 0)
            r = optMgmt(ep, T_NEGOTIATE, [(INET_TCP, 1, long(1))])
            #expect(r.status == 0 && r.flags == T_SUCCESS && r.options[0].status == T_SUCCESS)
            r = optMgmt(ep, T_CURRENT, [(INET_TCP, 1, long(0))])
            #expect(u32(r.options[0].value) == 1)
            // A check doesn't change anything.
            r = optMgmt(ep, T_CHECK, [(INET_TCP, 1, long(0))])
            #expect(r.options[0].status == T_SUCCESS)
            #expect(u32(optMgmt(ep, T_CURRENT, [(INET_TCP, 1, long(9))]).options[0].value) == 1)
            // Keep-alive: on, 5 minutes.
            r = optMgmt(ep, T_NEGOTIATE, [(XTI_GENERIC, 8, long(1) + long(5))])
            #expect(r.flags == T_SUCCESS)
            r = optMgmt(ep, T_CURRENT, [(XTI_GENERIC, 8, long(0) + long(0))])
            #expect(u32(r.options[0].value) == 1 && u32(Array(r.options[0].value[4...])) == 5)
            // The same option at the TCP level.
            r = optMgmt(ep, T_CURRENT, [(INET_TCP, 8, long(0) + long(0))])
            #expect(u32(r.options[0].value) == 1)
            // Reuse address, a linger time, buffer sizes, the TTL and TOS bytes.
            r = optMgmt(ep, T_NEGOTIATE, [(INET_IP, 4, long(1)), (XTI_GENERIC, 0x80, long(1) + long(7)),
                                           (XTI_GENERIC, 0x1002, long(65536)), (INET_IP, 3, [17]), (INET_IP, 2, [0x10])])
            #expect(r.status == 0 && r.flags == T_SUCCESS && r.options.count == 5)
            r = optMgmt(ep, T_CURRENT, [(INET_IP, 4, long(0)), (XTI_GENERIC, 0x80, long(0) + long(0)),
                                         (XTI_GENERIC, 0x1002, long(0)), (INET_IP, 3, [0]), (INET_IP, 2, [0])])
            #expect(u32(r.options[0].value) == 1)
            #expect(u32(r.options[1].value) == 1 && u32(Array(r.options[1].value[4...])) == 7)
            #expect(u32(r.options[2].value) >= 65536)
            #expect(r.options[3].value == [17] && r.options[4].value == [0x10])
            // Unknown, read-only and mistaken ones say so.
            r = optMgmt(ep, T_NEGOTIATE, [(INET_TCP, 0x77, long(1))])
            #expect(r.status == 0 && r.flags == T_NOTSUPPORT && r.options[0].status == T_NOTSUPPORT)
            r = optMgmt(ep, T_NEGOTIATE, [(XTI_GENERIC, 0x1005, long(3))]) // XTI_PROTOTYPE
            #expect(r.flags == T_READONLY && u32(r.options[0].value) == 6)
            r = optMgmt(ep, T_NEGOTIATE, [(INET_TCP, 1, long(1) + long(1))]) // wrong size
            #expect(r.flags == T_FAILURE)
            // Multicast belongs to datagram endpoints.
            r = optMgmt(ep, T_NEGOTIATE, [(INET_IP, 0x1011, [4])])
            #expect(r.flags == T_NOTSUPPORT)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func defaultsAndAllOptions() {
            launchTestApp()
            let ep = openEndpoint("tcp")
            _ = optMgmt(ep, T_NEGOTIATE, [(INET_TCP, 1, long(1))])
            var r = optMgmt(ep, T_DEFAULT, [(INET_TCP, 1, long(7))])
            #expect(u32(r.options[0].value) == 0) // the default, not the negotiated value
            // Every option at a level: name 0.
            r = optMgmt(ep, T_CURRENT, [(INET_TCP, 0, [])])
            #expect(r.status == 0 && r.options.count >= 8)
            #expect(r.options.contains { $0.name == 1 && u32($0.value) == 1 })
            #expect(r.options.contains { $0.name == 0x14 })
            r = optMgmt(ep, T_DEFAULT, [(XTI_GENERIC, 0, [])])
            #expect(r.options.contains { $0.name == 0x1002 && u32($0.value) == 32768 })
            #expect(ot("OTCloseProvider", ep) == 0)
            // A request the reply can't hold.
            let (buf, len) = optBuf([(INET_TCP, 0, [])])
            let req = block(16), ret = block(16), out = block(32)
            wr32(req, len); wr32(req + 4, len); wr32(req + 8, buf); wr32(req + 12, T_CURRENT)
            wr32(ret, 32); wr32(ret + 8, out)
            let ep2 = openEndpoint("tcp")
            #expect(err(ot("OTOptionManagement", ep2, req, ret)) == -3160) // kOTBufferOverflowErr
            #expect(ot("OTCloseProvider", ep2) == 0)
        }

        @Test func absoluteRequestsAreAllOrNothing() {
            launchTestApp()
            let ep = openEndpoint("tcp")
            // One option of the pair can't be had: with T_ABSREQ neither is applied.
            var r = optMgmt(ep, T_NEGOTIATE | 0x8000, [(INET_TCP, 1, long(1)), (INET_TCP, 0x77, long(1))])
            #expect(r.flags == T_NOTSUPPORT)
            r = optMgmt(ep, T_CURRENT, [(INET_TCP, 1, long(0))])
            #expect(u32(r.options[0].value) == 0)
            r = optMgmt(ep, T_NEGOTIATE, [(INET_TCP, 1, long(1)), (INET_TCP, 0x77, long(1))])
            r = optMgmt(ep, T_CURRENT, [(INET_TCP, 1, long(0))])
            #expect(u32(r.options[0].value) == 1) // without it, the good one is applied
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func udpOptionsBroadcastAndMulticast() {
            launchTestApp()
            let ep = openEndpoint("udp")
            var r = optMgmt(ep, T_NEGOTIATE, [(INET_IP, 0x20, long(1)), (INET_IP, 0x1011, [4]), (INET_IP, 0x1012, [0]),
                                               (INET_IP, 0x1010, long(0x7F00_0001))])
            #expect(r.status == 0 && r.flags == T_SUCCESS && r.options.count == 4)
            r = optMgmt(ep, T_CURRENT, [(INET_IP, 0x20, long(0)), (INET_IP, 0x1011, [0]), (INET_IP, 0x1012, [9]),
                                         (INET_IP, 0x1010, long(0))])
            #expect(u32(r.options[0].value) == 1 && r.options[1].value == [4] && r.options[2].value == [0])
            #expect(u32(r.options[3].value) == 0x7F00_0001)
            // Joining and leaving a group (on the loopback interface).
            r = optMgmt(ep, T_NEGOTIATE, [(INET_IP, 0x1013, long(0xEFFF_0001) + long(0x7F00_0001))])
            #expect(r.options[0].status == T_SUCCESS)
            r = optMgmt(ep, T_NEGOTIATE, [(INET_IP, 0x1014, long(0xEFFF_0001) + long(0x7F00_0001))])
            #expect(r.options[0].status == T_SUCCESS)
            r = optMgmt(ep, T_NEGOTIATE, [(INET_IP, 0x1014, long(0xEFFF_0001) + long(0x7F00_0001))])
            #expect(r.options[0].status == T_FAILURE) // not a member any longer
            r = optMgmt(ep, T_CURRENT, [(INET_IP, 0x1013, long(0) + long(0))]) // write-only
            #expect(r.flags == T_NOTSUPPORT)
            // UDP's own: checksum on by default, kept as set.
            r = optMgmt(ep, T_CURRENT, [(INET_UDP, 0x600, long(0))])
            #expect(u32(r.options[0].value) == 1)
            r = optMgmt(ep, T_NEGOTIATE, [(INET_UDP, 0x600, long(0))])
            r = optMgmt(ep, T_CURRENT, [(INET_UDP, 0x600, long(1))])
            #expect(u32(r.options[0].value) == 0)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func connectionOptionsStayWithTheWorker() {
            launchTestApp()
            let lis = openEndpoint("tcp"), worker = openEndpoint("tcp")
            #expect(ot("OTSetBlocking", lis) == 0 && ot("OTSetBlocking", worker) == 0)
            let bind = block(16), addr = block(16)
            wr16(addr, 2); wr16(addr + 2, 0); wr32(addr + 4, 0x7F00_0001)
            wr32(bind, 16); wr32(bind + 4, 16); wr32(bind + 8, addr); wr32(bind + 12, 1)
            let ret = block(16), retaddr = block(16)
            wr32(ret, 16); wr32(ret + 8, retaddr)
            #expect(ot("OTBind", lis, bind, ret) == 0)
            let port = rd16(retaddr + 2)
            // The worker's options are negotiated before it has a connection.
            _ = optMgmt(worker, T_NEGOTIATE, [(INET_TCP, 1, long(1)), (INET_IP, 3, [33])])
            let cfd = socket(AF_INET, SOCK_STREAM, 0)
            var sa = sockaddr_in()
            sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_port = port.bigEndian
            sa.sin_addr.s_addr = inet_addr("127.0.0.1")
            let rc = withUnsafePointer(to: &sa) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(cfd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
            #expect(rc == 0)
            defer { close(cfd) }
            let call = block(40)
            gmemset(call, 0, 40)
            let a2 = block(16)
            wr32(call, 16); wr32(call + 8, a2)
            #expect(ot("OTListen", lis, call) == 0)
            #expect(ot("OTAccept", lis, worker, call) == 0)
            #expect(ot("OTGetEndpointState", worker) == 5) // T_DATAXFER
            // The connection the worker got has them.
            let r = optMgmt(worker, T_CURRENT, [(INET_TCP, 1, long(0)), (INET_IP, 3, [0])])
            #expect(u32(r.options[0].value) == 1 && r.options[1].value == [33])
            #expect(ot("OTCloseProvider", worker) == 0 && ot("OTCloseProvider", lis) == 0)
        }

        @Test func connectTakesOptions() {
            launchTestApp()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let ep = openEndpoint("tcp")
            #expect(ot("OTSetBlocking", ep) == 0 && ot("OTBind", ep, 0, 0) == 0)
            let dns = block(260)
            let len = ot("OTInitDNSAddress", dns, cstr("127.0.0.1:\(port)"))
            let (obuf, olen) = optBuf([(INET_TCP, 1, long(1))])
            let call = block(40)
            gmemset(call, 0, 40)
            wr32(call, len); wr32(call + 4, len); wr32(call + 8, dns)
            wr32(call + 12, olen); wr32(call + 16, olen); wr32(call + 20, obuf)
            #expect(ot("OTConnect", ep, call, 0) == 0)
            #expect(rd32(obuf + 12) == T_SUCCESS) // the option's status is written back
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            let r = optMgmt(ep, T_CURRENT, [(INET_TCP, 1, long(0))])
            #expect(u32(r.options[0].value) == 1)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func optionBuffersAndStrings() {
            launchTestApp()
            // Text into options.
            let text = cstr("TCP_NODELAY=1, TTL=30, KeepAlive=1:10 ,NoSuchThing=3")
            let strp = block(4)
            wr32(strp, text)
            let nb = block(12), nbuf = block(256)
            wr32(nb, 256); wr32(nb + 4, 0); wr32(nb + 8, nbuf)
            #expect(err(ot("OTCreateOptions", cstr("tcp"), strp, nb)) == -3151) // kOTBadOptionErr at the fourth
            #expect(cstring(at: rd32(strp)).hasPrefix("NoSuchThing"))
            let len = rd32(nb + 4)
            #expect(len == 20 + 20 + 24)
            // Walk them.
            let pp = block(4)
            wr32(pp, 0)
            var found: [(UInt32, UInt32)] = []
            while ot("OTNextOption", nbuf, len, pp) == 0 { found.append((rd32(rd32(pp) + 4), rd32(rd32(pp) + 8))) }
            #expect(found.count == 3)
            #expect(found[0] == (6, 1) && found[1] == (0, 3) && found[2] == (0xFFFF, 8))
            #expect(err(ot("OTNextOption", nbuf, len, pp)) == -3162) // no more
            // Find one.
            let ttl = ot("OTFindOption", nbuf, len, 0, 3)
            #expect(ttl != 0 && rd8(ttl + 16) == 30)
            #expect(ot("OTFindOption", nbuf, len, 6, 0x99) == 0)
            // A malformed buffer.
            wr32(nbuf, 4) // len less than a header
            wr32(pp, 0)
            #expect(err(ot("OTNextOption", nbuf, len, pp)) == -3151)
            // Options into text.
            wr32(strp, cstr("udp"))
            let (buf, blen) = optBuf([(INET_IP, 0x20, long(1)), (INET_IP, 0x1013, long(0xE000_0001) + long(0xC0A8_0001)),
                                       (XTI_GENERIC, 0x80, long(1) + long(30))])
            let cursor = block(4)
            wr32(cursor, buf)
            let out = block(128)
            var lines: [String] = []
            for _ in 0..<3 {
                #expect(ot("OTCreateOptionString", cstr("udp"), cursor, buf + blen, out, 128) == 0)
                lines.append(cstring(at: out))
            }
            #expect(lines == ["IP_BROADCAST=yes", "IP_ADD_MEMBERSHIP=224.0.0.1:192.168.0.1", "XTI_LINGER=1:30"])
            #expect(rd32(cursor) == 0) // the end
            #expect(err(ot("OTCreateOptionString", cstr("udp"), cursor, buf + blen, out, 128)) == -3162)
        }

        @Test func whatIsKnownOfTheOptions() {
            launchTestApp()
            #expect(ot("OTHasOptionInfo", 6, 1) == 1 && ot("OTHasOptionInfo", 6, 0x4242) == 0)
            let info = block(52)
            #expect(ot("OTGetOptionInfoByID", 0, 3, info) == 0)
            #expect(rd32(info) == 0 && rd32(info + 4) == 3 && rd32(info + 8) == 1 && cstring(at: info + 16) == "IP_TTL")
            #expect(err(ot("OTGetOptionInfoByID", 0, 0x4242, info)) == -3201)
            #expect(ot("OTGetOptionInfoByName", cstr("NoDelay"), info) == 0 && rd32(info) == 6 && rd32(info + 4) == 1)
            #expect(ot("OTGetOptionInfoByName", cstr("tcp_nodelay"), info) == 0 && rd32(info + 12) & 4 == 4) // a boolean
            #expect(err(ot("OTGetOptionInfoByName", cstr("Bogus"), info)) == -3201)
            #expect(ot("OTGetProtocolLevel", cstr("tcp")) == 6 && ot("OTGetProtocolLevel", cstr("udp")) == 0x11)
            #expect(ot("OTGetProtocolLevel", cstr("ip")) == 0 && ot("OTGetProtocolLevel", cstr("nothing")) == 0xFFFF)
        }

        // MARK: - Internet services

        /// A name server on the loopback with a few records, for the queries.
        final class FakeDNS: @unchecked Sendable {
            let fd: Int32
            let port: UInt16
            var running = true

            init() {
                let sock = socket(AF_INET, SOCK_DGRAM, 0)
                var sa = sockaddr_in()
                sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
                sa.sin_family = sa_family_t(AF_INET)
                sa.sin_addr.s_addr = inet_addr("127.0.0.1")
                var len = socklen_t(MemoryLayout<sockaddr_in>.size)
                withUnsafeMutablePointer(to: &sa) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        _ = bind(sock, $0, len)
                        _ = getsockname(sock, $0, &len)
                    }
                }
                var tv = timeval(tv_sec: 0, tv_usec: 100_000)
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
                fd = sock
                port = UInt16(bigEndian: sa.sin_port)
                Thread.detachNewThread { [self] in serve() }
            }

            func stop() {
                running = false
                usleep(150_000)
                close(fd)
            }

            static func name(_ n: String) -> [UInt8] {
                var out = [UInt8]()
                for label in n.split(separator: ".") { out.append(UInt8(label.utf8.count)); out += Array(label.utf8) }
                return out + [0]
            }

            static func be16(_ v: Int) -> [UInt8] { [UInt8(v >> 8), UInt8(v & 255)] }
            static func be32(_ v: Int) -> [UInt8] { be16(v >> 16) + be16(v & 0xFFFF) }

            /// (owner, type, rdata) for what a name holds.
            func records(_ qname: String, _ qtype: Int) -> (rcode: Int, rrs: [(String, Int, [UInt8])]) {
                typealias F = FakeDNS
                switch qname {
                case "a.test": return (0, [("a.test", 1, [192, 0, 2, 7]), ("a.test", 1, [192, 0, 2, 8])])
                case "mx.test":
                    return (0, [("mx.test", 15, F.be16(20) + F.name("mail2.mx.test")),
                                ("mx.test", 15, F.be16(10) + F.name("mail.mx.test"))])
                case "txt.test": return (0, [("txt.test", 16, [11] + Array("hello world".utf8))])
                case "alias.test": return (0, [("alias.test", 5, F.name("real.test")), ("real.test", 1, [10, 1, 2, 3])])
                case "info.test": return (0, [("info.test", 13, [6] + Array("PPC750".utf8) + [7] + Array("MacOS 9".utf8))])
                case "ns.test": return (0, [("ns.test", 2, F.name("ns1.test"))])
                case "soa.test":
                    return (0, [("soa.test", 6, F.name("ns1.test") + F.name("hostmaster.test") + F.be32(2024) +
                                  F.be32(3600) + F.be32(600) + F.be32(86400) + F.be32(300))])
                case "7.2.0.192.in-addr.arpa": return (0, [("7.2.0.192.in-addr.arpa", 12, F.name("host7.test"))])
                case "none.test": return (3, [])
                default: return (0, [])
                }
            }

            func serve() {
                var buf = [UInt8](repeating: 0, count: 600)
                while running {
                    var from = sockaddr_in()
                    var flen = socklen_t(MemoryLayout<sockaddr_in>.size)
                    let n = withUnsafeMutablePointer(to: &from) {
                        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                            recvfrom(fd, &buf, buf.count, 0, $0, &flen)
                        }
                    }
                    if n < 17 { continue }
                    var i = 12
                    var labels = [String]()
                    while buf[i] != 0 {
                        let l = Int(buf[i])
                        labels.append(String(decoding: buf[(i + 1)..<(i + 1 + l)], as: UTF8.self))
                        i += l + 1
                    }
                    let qname = labels.joined(separator: ".")
                    let qtype = Int(buf[i + 1]) << 8 | Int(buf[i + 2])
                    let question = Array(buf[12..<(i + 5)])
                    let (rcode, rrs) = records(qname, qtype)
                    let wanted = rrs.filter { qtype == 255 || $0.1 == qtype || $0.1 == 5 }
                    var reply: [UInt8] = [buf[0], buf[1], 0x81, UInt8(0x80 | rcode)] + FakeDNS.be16(1) + FakeDNS.be16(wanted.count) + [0, 0, 0, 0]
                    reply += question
                    for (owner, type, rdata) in wanted {
                        reply += FakeDNS.name(owner) + FakeDNS.be16(type) + FakeDNS.be16(1) + FakeDNS.be32(300) + FakeDNS.be16(rdata.count) + rdata
                    }
                    _ = withUnsafePointer(to: &from) {
                        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                            sendto(fd, reply, reply.count, 0, $0, flen)
                        }
                    }
                }
            }
        }

        func openServices() -> UInt32 {
            let e = tb_NewPtr(4, false, true)
            let svc = ot("OTOpenInternetServices", 0xFFFF_FFFD, 0, e)
            #expect(rd32(e) == 0 && svc != 0)
            return svc
        }

        /// A DNSQueryInfo at p: type, class, ttl, name, section, data.
        func queryInfo(_ p: UInt32) -> (type: UInt32, cls: UInt32, ttl: UInt32, name: String, section: UInt32, data: [UInt8]) {
            let n = Int(rd16(p + 266))
            return (UInt32(rd16(p)), UInt32(rd16(p + 2)), rd32(p + 4), cstring(at: p + 8), UInt32(rd16(p + 264)),
                    (0..<n).map { rd8(p + 268 + UInt32($0)) })
        }

        func query(_ svc: UInt32, _ name: String, _ type: UInt32, bufsize: Int = 4096) -> (Int32, [(type: UInt32, cls: UInt32, ttl: UInt32, name: String, section: UInt32, data: [UInt8])]) {
            let buf = block(bufsize), argv = block(64 * 4)
            let r = err(ot("OTInetQuery", svc, cstr(name), 1, type, buf, UInt32(bufsize), argv, 64 * 4, 0))
            var out = [(type: UInt32, cls: UInt32, ttl: UInt32, name: String, section: UInt32, data: [UInt8])]()
            var i: UInt32 = 0
            while rd32(argv + 4 * i) != 0 && i < 63 { out.append(queryInfo(rd32(argv + 4 * i))); i += 1 }
            return (r, out)
        }

        @Test func dnsQueries() {
            launchTestApp()
            let dns = FakeDNS()
            defer { dns.stop(); ot_inet_set_dns(0, 0) }
            ot_inet_set_dns(0x7F00_0001, dns.port)
            let svc = openServices()

            var (r, recs) = query(svc, "a.test", 1)
            #expect(r == 0 && recs.count == 2)
            #expect(recs[0].type == 1 && recs[0].cls == 1 && recs[0].ttl == 300 && recs[0].name == "a.test")
            #expect(recs[0].section == 1 && recs[0].data == [192, 0, 2, 7] && recs[1].data == [192, 0, 2, 8])

            (r, recs) = query(svc, "mx.test", 15)
            #expect(r == 0 && recs.count == 2)
            #expect(recs[0].data == [0, 20] + Array("mail2.mx.test".utf8) + [0]) // preference, then the name as a C string
            #expect(recs[1].data == [0, 10] + Array("mail.mx.test".utf8) + [0])

            (r, recs) = query(svc, "txt.test", 16)
            #expect(r == 0 && recs.count == 1 && recs[0].data == [11] + Array("hello world".utf8)) // the strings as sent

            (r, recs) = query(svc, "alias.test", 1)
            #expect(r == 0 && recs.count == 2 && recs[0].type == 5)
            #expect(recs[0].data == Array("real.test".utf8) + [0] && recs[1].data == [10, 1, 2, 3] && recs[1].name == "real.test")

            (r, recs) = query(svc, "info.test", 13)
            #expect(r == 0 && recs[0].data == [6] + Array("PPC750".utf8) + [7] + Array("MacOS 9".utf8))

            (r, recs) = query(svc, "ns.test", 2)
            #expect(r == 0 && recs[0].data == Array("ns1.test".utf8) + [0])

            (r, recs) = query(svc, "soa.test", 6)
            #expect(r == 0 && recs.count == 1)
            let soa = recs[0].data
            #expect(soa.count == 9 + 16 + 20 && Array(soa[0..<9]) == Array("ns1.test".utf8) + [0])
            #expect(Array(soa[9..<25]) == Array("hostmaster.test".utf8) + [0] && soa[25...28] == [0, 0, 7, 232])

            (r, recs) = query(svc, "7.2.0.192.in-addr.arpa", 12)
            #expect(r == 0 && recs[0].data == Array("host7.test".utf8) + [0])

            // A name that isn't, a name with nothing of the kind, and a buffer too small.
            #expect(query(svc, "none.test", 1).0 == -3170) // kOTBadNameErr
            #expect(query(svc, "empty.test", 1).0 == -3162) // kOTNoDataErr
            #expect(query(svc, "a.test", 1, bufsize: 300).0 == -3160) // kOTBufferOverflowErr
            #expect(err(ot("OTInetQuery", 0x1234, cstr("a.test"), 1, 1, block(100), 100, 0, 0, 0)) == -3153)
        }

        @Test func mailExchangersAndHostInformation() {
            launchTestApp()
            let dns = FakeDNS()
            defer { dns.stop(); ot_inet_set_dns(0, 0) }
            ot_inet_set_dns(0x7F00_0001, dns.port)
            let svc = openServices()
            // Mail exchangers, most preferred first, at most as many as asked.
            let num = block(2), mx = block(258 * 4)
            wr16(num, 4)
            #expect(ot("OTInetMailExchange", svc, cstr("mx.test"), num, mx) == 0)
            #expect(rd16(num) == 2)
            #expect(rd16(mx) == 10 && cstring(at: mx + 2) == "mail.mx.test")
            #expect(rd16(mx + 258) == 20 && cstring(at: mx + 260) == "mail2.mx.test")
            wr16(num, 1)
            #expect(ot("OTInetMailExchange", svc, cstr("mx.test"), num, mx) == 0 && rd16(num) == 1)
            #expect(err(ot("OTInetMailExchange", svc, cstr("a.test"), num, mx)) == -3162)
            #expect(err(ot("OTInetMailExchange", svc, cstr("none.test"), num, mx)) == -3170)
            // HINFO: the CPU and the operating system.
            let info = block(64)
            #expect(ot("OTInetSysInfo", svc, cstr("info.test"), info) == 0)
            #expect(cstring(at: info) == "PPC750" && cstring(at: info + 32) == "MacOS 9")
            #expect(err(ot("OTInetSysInfo", svc, cstr("a.test"), info)) == -3162)
            // This machine, with an empty name.
            #expect(ot("OTInetSysInfo", svc, cstr(""), info) == 0 && !cstring(at: info).isEmpty && !cstring(at: info + 32).isEmpty)
        }

        @Test func asynchronousServicesTellTheNotifier() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let dns = FakeDNS()
            defer { dns.stop(); ot_inet_set_dns(0, 0) }
            ot_inet_set_dns(0x7F00_0001, dns.port)
            let svc = openServices()
            let cell = block(16)
            gmemset(cell, 0, 16)
            #expect(ot("OTInstallNotifier", svc, recorder(into: cell), 0) == 0)
            #expect(ot("OTSetAsynchronous", svc) == 0)
            let buf = block(2048), argv = block(64)
            #expect(ot("OTInetQuery", svc, cstr("a.test"), 1, 1, buf, 2048, argv, 64, 0) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x1000_0005 && rd32(cell + 4) == 0 && rd32(cell + 8) == buf) // T_DNRQUERYCOMPLETE
            #expect(rd32(argv) != 0)
            let name = block(256)
            #expect(ot("OTInetAddressToName", svc, 0x7F00_0001, name) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x1000_0002 && rd32(cell + 4) == 0 && cstring(at: name).contains("localhost"))
            let num = block(2), mx = block(258)
            wr16(num, 1)
            #expect(ot("OTInetMailExchange", svc, cstr("mx.test"), num, mx) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x1000_0004)
            let info = block(64)
            #expect(ot("OTInetSysInfo", svc, cstr("info.test"), info) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x1000_0003 && cstring(at: info) == "PPC750")
            // An answer that's an error is the notifier's result.
            #expect(ot("OTInetQuery", svc, cstr("none.test"), 1, 1, buf, 2048, argv, 64, 0) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x1000_0005 && err(rd32(cell + 4)) == -3170)
        }

        @Test func addressToNameUsesTheHost() {
            launchTestApp()
            let svc = openServices()
            let name = block(256)
            #expect(ot("OTInetAddressToName", svc, 0x7F00_0001, name) == 0)
            #expect(cstring(at: name).lowercased().contains("localhost"))
        }

        @Test func interfaceInformationIsTheHosts() {
            launchTestApp()
            let info = block(552)
            let r = err(ot("OTInetGetInterfaceInfo", info, 0xFFFF_FFFF)) // kDefaultInetInterface
            #expect(r == 0 || r == -3201) // a host with no address has none to report
            if r == 0 {
                #expect(rd32(info) != 0 && rd32(info + 4) != 0 && rd32(info + 8) != 0)
                #expect(rd32(info) & rd32(info + 4) == rd32(info + 8) & rd32(info + 4)) // the broadcast is on the network
                #expect(rd16(info + 20) == 3 && rd16(info + 22) == 6 && rd32(info + 28) >= 576)
                let mac = rd32(info + 24)
                #expect(mac != 0 && (0..<6).contains { rd8(mac + UInt32($0)) != 0 })
                let n = block(4), addrs = block(64)
                wr32(n, 16)
                #expect(ot("OTInetGetSecondaryAddresses", addrs, n, 0xFFFF_FFFF) == 0)
                #expect(rd32(n) == rd32(info + 296))
                // The library's own names do the same.
                let again = block(552)
                #expect(ot("OTGetInterfaceInfo", again, 0) == 0 && rd32(again) == rd32(info))
            }
            #expect(err(ot("OTInetGetInterfaceInfo", info, 99)) == -3201)
            let n = block(4)
            wr32(n, 4)
            #expect(err(ot("OTInetGetSecondaryAddresses", block(16), n, 99)) == -3201)
            // DHCP options: none for no such interface; the lease's, if the host has one, in tag/length/value form.
            let buf = block(1024)
            #expect(err(ot("OTInetGetDHCPConfigInfo", buf, 1024, 99, 0xFFFF_FFFF)) == -3201)
            let d = err(ot("OTInetGetDHCPConfigInfo", buf, 1024, 0xFFFF_FFFF, 0xFFFF_FFFF))
            #expect(d == 0 || d == -3201)
            if d == 0 {
                var at: UInt32 = 0
                var tags = [UInt8]()
                while at < 1024 && rd8(buf + at) != 0 {
                    tags.append(rd8(buf + at))
                    at += 2 + UInt32(rd8(buf + at + 1))
                }
                #expect(!tags.isEmpty)
            }
        }

        // MARK: - XTI

        /// t_errno, as a program reads it.
        func tErrno() -> UInt32 { rd32(ppc_data_export("t_errno")) }

        func dnsCall(_ port: UInt16) -> UInt32 {
            let dns = block(260)
            let len = ot("OTInitDNSAddress", dns, cstr("127.0.0.1:\(port)"))
            let call = block(40)
            gmemset(call, 0, 40)
            wr32(call, len); wr32(call + 4, len); wr32(call + 8, dns)
            return call
        }

        @Test func xtiConnectionOriented() {
            launchTestApp()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let info = block(32)
            let fd = ot("t_open", cstr("tcp"), 0, info)
            #expect(fd != 0 && fd != 0xFFFF_FFFF)
            #expect(rd32(info) == 16 && rd32(info + 24) == 2) // a TCP endpoint: T_COTS_ORD
            #expect(ot("t_getstate", fd) == 1) // T_UNBND
            #expect(ot("t_bind", fd, 0, 0) == 0)
            #expect(ot("t_getstate", fd) == 2) // T_IDLE
            #expect(ot("t_blocking", fd) == 0 && ot("t_isnonblocking", fd) == 0)
            #expect(ot("t_connect", fd, dnsCall(port), 0) == 0)
            #expect(ot("t_getstate", fd) == 5) // T_DATAXFER
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            // t_snd and t_rcv count bytes.
            #expect(ot("t_snd", fd, cstr("XTI data"), 8, 0) == 8)
            var buf = [UInt8](repeating: 0, count: 16)
            #expect(recv(conn, &buf, 16, 0) == 8 && String(decoding: buf[0..<8], as: UTF8.self) == "XTI data")
            _ = "reply".withCString { send(conn, $0, 5, 0) }
            usleep(20_000)
            let rbuf = block(16), flags = block(4)
            #expect(ot("t_rcv", fd, rbuf, 16, flags) == 5 && cstring(at: rbuf, max: 5) == "reply")
            // Non-blocking, with nothing to read: -1 and TNODATA.
            #expect(ot("t_nonblocking", fd) == 0 && ot("t_isnonblocking", fd) == 1)
            #expect(ot("t_rcv", fd, rbuf, 16, flags) == 0xFFFF_FFFF && tErrno() == 13)
            #expect(ot("t_look", fd) == 0)
            // The peer's release is an event for t_look, TLOOK for t_rcv, and t_rcvrel takes it.
            shutdown(conn, SHUT_WR)
            usleep(30_000)
            #expect(ot("t_look", fd) == 0x80) // T_ORDREL
            #expect(ot("t_rcv", fd, rbuf, 16, flags) == 0xFFFF_FFFF && tErrno() == 9) // TLOOK
            #expect(ot("t_rcvrel", fd) == 0)
            #expect(ot("t_sndrel", fd) == 0)
            #expect(ot("t_close", fd) == 0)
            // A closed endpoint is a bad descriptor.
            #expect(ot("t_getstate", fd) == 0xFFFF_FFFF && tErrno() == 4) // TBADF
            #expect(ot("t_look", 0x1234) == 0xFFFF_FFFF && tErrno() == 4)
        }

        @Test func xtiConnectRefusedAndStates() {
            launchTestApp()
            let (lfd, port) = listener()
            close(lfd) // nothing listening now
            let fd = ot("t_open_priv", cstr("tcp"), 0, 0, 0)
            #expect(fd != 0xFFFF_FFFF)
            #expect(ot("t_blocking", fd) == 0)
            // Connecting an unbound endpoint is the wrong state.
            #expect(ot("t_connect", fd, dnsCall(port), 0) == 0xFFFF_FFFF && tErrno() == 6) // TOUTSTATE
            #expect(ot("t_bind", fd, 0, 0) == 0)
            #expect(ot("t_connect", fd, dnsCall(port), 0) == 0xFFFF_FFFF)
            #expect(tErrno() == 8) // TSYSERR: connection refused is a UNIX error
            #expect(ot("t_unbind", fd) == 0 && ot("t_getstate", fd) == 1)
            // A path no provider answers to.
            let bad = block(8)
            #expect(ot("t_open", cstr("tcp"), 0, 0) != 0xFFFF_FFFF)
            #expect(bad != 0)
            #expect(ot("t_close_priv", fd, 0) == 0)
        }

        @Test func xtiDatagramsAndStructures() {
            launchTestApp()
            let fd = ot("t_open", cstr("udp"), 0, 0)
            #expect(fd != 0xFFFF_FFFF)
            // t_alloc: a t_unitdata with its three buffers, sized for UDP.
            let ud = ot("t_alloc", fd, 5, 0xFFFF)
            #expect(ud != 0 && rd32(ud) >= 16 && rd32(ud + 12) >= 1024 && rd32(ud + 24) >= 1024)
            #expect(ot("t_alloc_priv", fd, 99, 1, 0) == 0 && tErrno() == 8 || tErrno() == 20) // a bad structure type
            // Bind to a port of the loopback's, find it, send to ourselves.
            let bind = block(16), addr = block(16), ret = block(16), retaddr = block(16)
            wr16(addr, 2); wr16(addr + 2, 0); wr32(addr + 4, 0x7F00_0001)
            wr32(bind, 16); wr32(bind + 4, 16); wr32(bind + 8, addr)
            wr32(ret, 16); wr32(ret + 8, retaddr)
            #expect(ot("t_bind", fd, bind, ret) == 0)
            let port = rd16(retaddr + 2)
            #expect(port != 0)
            let bound = block(16), boundaddr = block(16)
            wr32(bound, 16); wr32(bound + 8, boundaddr)
            #expect(ot("t_getprotaddr", fd, bound, 0) == 0 && rd16(boundaddr + 2) == port)
            let to = block(16)
            wr16(to, 2); wr16(to + 2, port); wr32(to + 4, 0x7F00_0001)
            wr32(ud, 16); wr32(ud + 4, 16); wr32(ud + 8, to)
            let payload = cstr("datagram")
            wr32(ud + 24, 1024); wr32(ud + 28, 8)
            gmemcpy(rd32(ud + 32), payload, 8)
            wr32(ud + 12, 0); wr32(ud + 16, 0)
            #expect(ot("t_sndudata", fd, ud) == 0)
            usleep(20_000)
            wr32(ud + 28, 0)
            wr32(ud + 4, 0)
            let fl = block(4)
            #expect(ot("t_rcvudata", fd, ud, fl) == 0)
            #expect(rd32(ud + 28) == 8 && cstring(at: rd32(ud + 32), max: 8) == "datagram" && rd16(rd32(ud + 8) + 2) == port)
            // Nothing more: TNODATA.
            #expect(ot("t_nonblocking", fd) == 0)
            #expect(ot("t_rcvudata", fd, ud, fl) == 0xFFFF_FFFF && tErrno() == 13)
            #expect(ot("t_free", ud, 5) == 0)
            #expect(ot("t_free", 0, 99) == 0xFFFF_FFFF)
            #expect(ot("t_close", fd) == 0)
        }

        @Test func xtiOptionsAndModes() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let fd = ot("t_open", cstr("tcp"), 0, 0)
            // t_optmgmt is OTOptionManagement.
            let (buf, len) = optBuf([(INET_TCP, 1, long(1))])
            let req = block(16), ret = block(16), out = block(256)
            wr32(req, len); wr32(req + 4, len); wr32(req + 8, buf); wr32(req + 12, T_NEGOTIATE)
            wr32(ret, 256); wr32(ret + 8, out)
            #expect(ot("t_optmgmt", fd, req, ret) == 0 && rd32(ret + 12) == T_SUCCESS)
            // Modes.
            #expect(ot("t_issynchronous", fd) == 1)
            #expect(ot("t_asynchronous", fd) == 0 && ot("t_issynchronous", fd) == 0)
            #expect(ot("t_synchronous", fd) == 0 && ot("t_issynchronous", fd) == 1)
            // A notifier and sync idle events.
            let cell = block(16)
            gmemset(cell, 0, 16)
            #expect(ot("t_installnotifier", fd, recorder(into: cell), 0) == 0)
            #expect(ot("t_usesyncidleevents", fd, 1) == 0)
            ot("t_removenotifier", fd)
            // t_getinfo and t_sync.
            let info = block(32)
            #expect(ot("t_getinfo", fd, info) == 0 && rd32(info + 24) == 2)
            #expect(ot("t_sync", fd) == 1)
            // Cancelled synchronous calls: a blocking read with a pending cancel returns that error.
            #expect(ot("t_bind", fd, 0, 0) == 0)
            #expect(ot("t_cancelsynchronouscalls", fd) == 0)
            #expect(ot("t_close", fd) == 0)
            ot("t_error", cstr("done"))
        }

        // MARK: - Providers and endpoints

        @Test func endpointInformationAndModes() {
            launchTestApp()
            let tcp = openEndpoint("tcp"), udp = openEndpoint("udp")
            let info = block(32)
            #expect(ot("OTGetEndpointInfo", tcp, info) == 0)
            #expect(rd32(info) == 16 && rd32(info + 24) == 2 && rd32(info + 28) & 0x40000000 != 0) // T_CAN_RESOLVE_ADDR
            #expect(ot("OTGetEndpointInfo", udp, info) == 0 && rd32(info + 24) == 3)
            #expect(err(ot("OTGetEndpointInfo", 0x4242, info)) == -3153)
            #expect(ot("OTSync", tcp) == 1 && ot("OTSync", udp) == 1) // T_UNBND
            // The modes read back.
            #expect(ot("OTIsSynchronous", tcp) == 1 && ot("OTIsBlocking", tcp) == 0)
            ot("OTSetBlocking", tcp)
            ot("OTSetAsynchronous", tcp)
            #expect(ot("OTIsSynchronous", tcp) == 0 && ot("OTIsBlocking", tcp) == 1)
            // Who owns it.
            let e = block(4)
            #expect(ot("OTTransferProviderOwnership", tcp, ot("OTWhoAmI"), e) == tcp && rd32(e) == 0)
            #expect(ot("OTWhoAmI") != 0)
            #expect(ot("OTCloseProvider", tcp) == 0 && ot("OTCloseProvider", udp) == 0)
        }

        @Test func aCancelledBlockedCallReturnsTheError() {
            launchTestApp()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let ep = openEndpoint("tcp")
            #expect(ot("OTSetBlocking", ep) == 0 && ot("OTBind", ep, 0, 0) == 0)
            #expect(ot("OTConnect", ep, dnsCall(port), 0) == 0)
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            let buf = block(16), flags = block(4)
            // The cancel is waiting when the read finds nothing to read.
            #expect(ot("OTCancelSynchronousCalls", ep, 0) == 0)
            #expect(err(ot("OTRcv", ep, buf, 16, flags)) == -3180) // kOTCanceledErr
            // And only once: the next read blocks again (until data comes).
            _ = "x".withCString { send(conn, $0, 1, 0) }
            #expect(ot("OTRcv", ep, buf, 16, flags) == 1)
            #expect(ot("OTCancelSynchronousCalls", ep, UInt32(bitPattern: -4000)) == 0)
            #expect(err(ot("OTRcv", ep, buf, 16, flags)) == -4000)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func configurationsCloneAndOpen() {
            launchTestApp()
            let cfg = ot("OTCreateConfiguration", cstr("udp"))
            let copy = ot("OTCloneConfiguration", cfg)
            #expect(copy != 0 && copy != cfg && cstring(at: copy) == "udp")
            ot("OTDestroyConfiguration", cfg)
            let ep = openEndpoint("udp") // (a clone is a configuration for an endpoint too)
            let e = block(4)
            let ep2 = ot("OTOpenEndpoint", copy, 0, 0, e)
            #expect(rd32(e) == 0 && ep2 != 0)
            #expect(ot("OTCloseProvider", ep) == 0 && ot("OTCloseProvider", ep2) == 0)
        }

        @Test func openProviderPicksTheKindOfProvider() {
            launchTestApp()
            let e = block(4)
            // An endpoint for "tcp".
            let ep = ot("OTOpenProvider", ot("OTCreateConfiguration", cstr("tcp")), 0, e)
            #expect(rd32(e) == 0 && ep != 0 && ot("OTGetEndpointState", ep) == 1)
            // Internet services for "dnr".
            let svc = ot("OTOpenProvider", ot("OTCreateConfiguration", cstr("dnr")), 0, e)
            #expect(rd32(e) == 0 && svc != 0)
            let hinfo = block(296)
            #expect(ot("OTInetStringToAddress", svc, cstr("127.0.0.1"), hinfo) == 0 && rd32(hinfo + 256) == 0x7F00_0001)
            #expect(ot("OTCloseProvider", ep) == 0 && ot("OTCloseProvider", svc) == 0)
            // Asynchronously, the opened provider and its OPENCOMPLETE.
            _ = cfm_ppc_cpu()
            let cell = block(16)
            gmemset(cell, 0, 16)
            #expect(ot("OTAsyncOpenProvider", ot("OTCreateConfiguration", cstr("udp")), 0, recorder(into: cell), 0) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x2000_0007 && rd32(cell + 4) == 0 && rd32(cell + 8) != 0) // T_OPENCOMPLETE, the new ref
            #expect(ot("OTCloseProvider", rd32(cell + 8)) == 0)
        }

        @Test func allocatedStructuresAreSizedForTheEndpoint() {
            launchTestApp()
            let tcp = openEndpoint("tcp")
            let e = block(4)
            // Each structure type, with all its fields.
            let types: [(UInt32, Int)] = [(1, 1), (2, 1), (3, 3), (4, 1), (5, 3), (6, 2), (7, 0), (8, 2), (9, 2), (10, 3), (11, 2)]
            for (type, nbufs) in types {
                let t = ot("OTAlloc", tcp, type, 0xFFFF, e)
                #expect(rd32(e) == 0 && t != 0, "type \(type)")
                for i in 0..<nbufs { #expect(rd32(t + UInt32(12 * i)) > 0 && rd32(t + UInt32(12 * i) + 8) != 0, "type \(type) buffer \(i)") }
                #expect(ot("OTFree", t, type) == 0)
            }
            // A TCall's address buffer holds an InetAddress; its udata is a page's worth for a stream.
            let call = ot("OTAlloc", tcp, 3, 0xFFFF, e)
            #expect(rd32(call) == 16 && rd32(call + 12) == 1024 && rd32(call + 24) >= 1024)
            // No fields, no buffers.
            let none = ot("OTAlloc", tcp, 3, 0, e)
            #expect(rd32(none + 8) == 0 && rd32(none + 20) == 0)
            #expect(err(ot("OTFree", none, 99)) == -3169) // TNOSTRUCTYPE
            var bad = block(4)
            bad = ot("OTAlloc", tcp, 99, 1, bad) // the error is written through the pointer
            #expect(bad == 0)
            #expect(ot("OTCloseProvider", tcp) == 0)
        }

        /// The host's TCP delivers urgent data to a loopback peer unreliably (it sometimes
        /// arrives in line, unmarked), so each direction is tried until it shows once.
        @Test func expeditedDataIsTCPUrgentData() {
            launchTestApp()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let ep = openEndpoint("tcp")
            #expect(ot("OTSetBlocking", ep) == 0 && ot("OTBind", ep, 0, 0) == 0)
            #expect(ot("OTConnect", ep, dnsCall(port), 0) == 0)
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            // Sent expedited, it goes as TCP urgent data.  (The host's loopback loses a lone
            // urgent byte now and then, so only the call is checked, and that the connection
            // goes on working.)
            #expect(ot("OTSnd", ep, cstr("!"), 1, 2) == 1) // T_EXPEDITED
            #expect(ot("OTSnd", ep, cstr("ok"), 2, 0) == 2)
            usleep(60_000)
            // Urgent data from the peer is T_EXDATA, and arrives flagged T_EXPEDITED.
            let buf = block(8), flags = block(4)
            var saw = false
            for _ in 0..<10 where !saw {
                var one: UInt8 = 0x40
                #expect(send(conn, &one, 1, Int32(MSG_OOB)) == 1)
                usleep(60_000)
                if ot("OTLook", ep) == 8 { // T_EXDATA
                    #expect(ot("OTRcv", ep, buf, 8, flags) == 1 && rd8(buf) == 0x40 && rd32(flags) & 2 != 0)
                    saw = true
                } else {
                    ot("OTSetNonBlocking", ep)
                    while ot("OTRcv", ep, buf, 8, flags) == 1 {} // it came in line: drain it
                    ot("OTSetBlocking", ep)
                }
            }
            #expect(saw)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func resolveAddressTurnsADNSAddressIntoAnInetAddress() {
            launchTestApp()
            let ep = openEndpoint("tcp")
            let req = block(12 + 4), dns = block(260)
            let len = ot("OTInitDNSAddress", dns, cstr("127.0.0.1:4242"))
            wr32(req, len); wr32(req + 4, len); wr32(req + 8, dns)
            let ret = block(16), retaddr = block(16)
            wr32(ret, 16); wr32(ret + 8, retaddr)
            #expect(ot("OTResolveAddress", ep, req, ret, 1000) == 0)
            #expect(rd32(ret + 4) == 16 && rd16(retaddr) == 2 && rd16(retaddr + 2) == 4242 && rd32(retaddr + 4) == 0x7F00_0001)
            wr32(req + 4, 0)
            #expect(err(ot("OTResolveAddress", ep, req, ret, 1000)) == -3150)
            #expect(ot("OTCloseProvider", ep) == 0)
        }
    }
}
