import CCore
import Darwin
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Open Transport for 68k code: the calls arrive as the glue Apple linked into 68k
    /// programs makes them, through a stub's record to the Shared Library Manager's
    /// dispatcher.  A call here builds the glue's records (a library record naming the
    /// class, a stub record with the function's index), puts the arguments on the 68k
    /// stack by the function's convention and runs the dispatcher.
    @Suite struct OpenTransport68k {
        func stack() {
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
        }

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
        func err(_ v: UInt32) -> Int32 { Int32(bitPattern: v) }

        /// The glue's records for a function by name; the record's address.
        nonisolated(unsafe) static var records: [String: UInt32] = [:]
        nonisolated(unsafe) static var clientRecord: UInt32 = 0

        /// Call an Open Transport function as the glue's stub would, the arguments as longs
        /// (words and bytes are narrowed by the signature).
        func call(_ name: String, _ args: UInt32...) -> UInt32 {
            callArgs(name, args)
        }

        func callArgs(_ name: String, _ args: [UInt32]) -> UInt32 {
            var lib = [CChar](repeating: 0, count: 64), sig = [CChar](repeating: 0, count: 24)
            var index: Int32 = 0, conv: CChar = 0, ret: CChar = 0
            guard ot_68k_find(name, &lib, &index, &conv, &sig, &ret) else {
                Issue.record("\(name) is not in the glue's table")
                return 0xFFFF_FFFF
            }
            let sigs = Array(String(cString: sig))
            var all = args
            if sigs.count > args.count, name.hasSuffix("Priv") { // the client record comes last
                if Self.clientRecord == 0 { Self.clientRecord = block(64) }
                all.append(Self.clientRecord)
            }
            #expect(sigs.count == all.count, "\(name): \(sigs.count) arguments in the table, \(all.count) given")
            let em = rd32(0x2B6), glob = rd32(em + 0x10C), dispatch = rd32(glob + 0x88)
            var rec = Self.records[name] ?? 0
            if rec == 0 {
                let libRec = block(48), r = block(16)
                for (i, b) in "OTClnt".utf8.enumerated() where false { _ = (i, b) }
                wr32(libRec + 12, 0x0100_0100)
                for (i, b) in String(cString: lib).utf8.enumerated() { wr8(libRec + 16 + UInt32(i), b) }
                wr32(r + 4, libRec)
                wr16(r + 12, UInt16(index))
                Self.records[name] = r
                rec = r
            }
            wr32(rec, 0) // the dispatcher binds it afresh each time
            let c = g_cpu!
            let sp = c.pointee.a.7
            let pascal = Character(UnicodeScalar(UInt8(bitPattern: conv))) == "P"
            let retc = Character(UnicodeScalar(UInt8(bitPattern: ret)))
            if pascal {
                if retc == "l" { cpu_push32(c, 0) } else if retc == "w" || retc == "b" { cpu_push16(c, 0) }
                for (i, a) in all.enumerated() {
                    switch sigs[i] {
                    case "l": cpu_push32(c, a)
                    case "w": cpu_push16(c, UInt16(truncatingIfNeeded: a))
                    default: cpu_push16(c, UInt16(truncatingIfNeeded: a) << 8) // a byte in the high half
                    }
                }
            } else {
                for a in all.reversed() { cpu_push32(c, a) }
            }
            // The glue's stub: A0 at the record, on to the dispatcher.
            let stub = code([0x207C, UInt16(rec >> 16), UInt16(rec & 0xFFFF), 0x4EF9, UInt16(dispatch >> 16), UInt16(dispatch & 0xFFFF)])
            guest_call_regs(stub)
            var result: UInt32
            if pascal {
                // the callee popped its arguments: the result is on top
                switch retc {
                case "l": result = cpu_pop32(c)
                case "w": result = UInt32(cpu_pop16(c))
                case "b": result = UInt32(cpu_pop16(c) >> 8)
                default: result = 0
                }
            } else {
                result = c.pointee.d.0
                c.pointee.a.7 += UInt32(4 * all.count)
            }
            #expect(c.pointee.a.7 == sp, "\(name) left the stack unbalanced")
            c.pointee.a.7 = sp
            return result
        }

        func start() {
            Self.records = [:]
            Self.clientRecord = 0
            launchTestApp()
            stack()
            ot_68k_prepare()
        }

        /// A 68k routine, from these words, at an address.
        func code(_ words: [UInt16]) -> UInt32 {
            let p = block(words.count * 2)
            for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
            return p
        }

        @Test func glueFindsTheManager() {
            start()
            // The Expand Memory record, the manager's global at $10C, the manager object and its
            // dispatcher, as the glue reads them.
            let em = rd32(0x2B6)
            #expect(em != 0 && tb_GetPtrSize(em) >= 0x110)
            let glob = rd32(em + 0x10C)
            #expect(glob != 0 && rd32(glob + 0x10) != 0 && rd32(glob + 0x88) != 0)
            let mgr = rd32(glob + 0x10), vtable = rd32(mgr)
            #expect(vtable != 0 && rd32(vtable + 0x9C) != 0 && rd32(vtable + 0xA0) != 0)
            // InitLibraryManager(this, &gLibraryManager, ...) sets the glue's variable.
            let variable = block(4)
            let c = g_cpu!
            let sp = c.pointee.a.7
            for v in [0, 0, 0, 0, 0, variable, mgr] as [UInt32] { cpu_push32(c, v) }
            guest_call_regs(rd32(vtable + 0x9C))
            c.pointee.a.7 = sp
            #expect(c.pointee.d.0 & 0xFFFF == 0 && rd32(variable) == mgr)
            // Asking again keeps what's there.
            ot_68k_prepare()
            #expect(rd32(rd32(0x2B6) + 0x10C) == glob)
            // Gestalt, as 68k code gets it.
            var answer: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("otan")), &answer) == 0 && answer & 1 == 1)
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("otvr")), &answer) == 0 && answer != 0)
        }

        @Test func configurationsAndSizesOfResults() {
            start()
            // Pascal: a long result, a word, a byte; the arguments popped.
            let cfg = call("OTCreateConfiguration", cstr("tcp"))
            #expect(cfg != 0 && cstring(at: cfg) == "tcp")
            let ref = call("OTCreatePortRef", 3, 10, 5, 7)
            #expect(call("OTGetBusTypeFromPortRef", ref) == 3 && call("OTGetDeviceTypeFromPortRef", ref) == 10)
            let other = block(2)
            #expect(call("OTGetSlotFromPortRef", ref, other) == 5 && rd16(other) == 7)
            #expect(call("OTCanMakeSyncCall") == 1)
            let ref2 = call("OTSetBusTypeInPortRef", ref, 5)
            #expect(call("OTGetBusTypeFromPortRef", ref2) == 5)
            // C: the arguments popped by the caller.
            let map = block(4)
            wr32(map, 0)
            #expect(call("OTSetBit", map, 9) == 0 && call("OTTestBit", map, 9) == 1 && rd8(map + 1) == 2)
            #expect(call("OTSetFirstClearBit", map, 0, 4) == 0)
            #expect(err(call("OTSetFirstClearBit", map, 0, 0)) == -3201)
            // A function the glue lists that isn't supported says so, and pops its arguments.
            #expect(err(call("OTGetMessage", 0, 0, 0, 0)) == -3167)
        }

        @Test func tcpEndpointFromGlue() {
            start()
            var lfd: Int32 = socket(AF_INET, SOCK_STREAM, 0)
            var sa = sockaddr_in()
            sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_addr.s_addr = inet_addr("127.0.0.1")
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            withUnsafeMutablePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    _ = bind(lfd, $0, len)
                    _ = listen(lfd, 1)
                    _ = getsockname(lfd, $0, &len)
                }
            }
            let port = UInt16(bigEndian: sa.sin_port)
            defer { close(lfd); lfd = -1 }

            // InitOpenTransport, then the endpoint as OTOpenEndpoint's wrapper opens it: the
            // wrapper adds the client record.
            #expect(call("InitOpenTransportPriv", 1) == 0)
            let e = block(4)
            let ep = call("OTOpenEndpointPriv", call("OTCreateConfiguration", cstr("tcp")), 0, 0, e)
            #expect(rd32(e) == 0 && ep != 0)
            #expect(call("OTSetBlocking", ep) == 0 && call("OTBind", ep, 0, 0) == 0)
            let dns = block(260)
            let alen = call("OTInitDNSAddress", dns, cstr("127.0.0.1:\(port)"))
            let sndCall = block(40)
            wr32(sndCall, alen); wr32(sndCall + 4, alen); wr32(sndCall + 8, dns)
            #expect(call("OTConnect", ep, sndCall, 0) == 0)
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            #expect(call("OTSnd", ep, cstr("from 68k"), 8, 0) == 8)
            var buf = [UInt8](repeating: 0, count: 16)
            #expect(recv(conn, &buf, 16, 0) == 8 && String(decoding: buf[0..<8], as: UTF8.self) == "from 68k")
            _ = "back".withCString { send(conn, $0, 4, 0) }
            let rbuf = block(16), flags = block(4)
            #expect(call("OTRcv", ep, rbuf, 16, flags) == 4 && cstring(at: rbuf, max: 4) == "back")
            // Options through the glue.
            let (obuf, olen) = (block(32), UInt32(20))
            wr32(obuf, 20); wr32(obuf + 4, 6); wr32(obuf + 8, 1); wr32(obuf + 12, 0); wr32(obuf + 16, 1)
            let req = block(16), ret = block(16), out = block(64)
            wr32(req, olen); wr32(req + 4, olen); wr32(req + 8, obuf); wr32(req + 12, 4)
            wr32(ret, 64); wr32(ret + 8, out)
            #expect(call("OTOptionManagement", ep, req, ret) == 0 && rd32(ret + 12) == 0x20)
            #expect(call("OTGetEndpointState", ep) == 5)
            #expect(call("OTCloseProviderPriv", ep) == 0)
        }

        @Test func notifiersAndTasksAreCalledAs68kCode() {
            start()
            // A Pascal notifier: it stores its event code at a cell and pops its 4 arguments.
            let cell = block(8)
            gmemset(cell, 0, 8)
            let notifier = code([
                0x23EF, 0x000C, UInt16(cell >> 16), UInt16(cell & 0xFFFF), // move.l 12(a7),cell
                0x23EF, 0x0004, UInt16((cell + 4) >> 16), UInt16((cell + 4) & 0xFFFF), // move.l 4(a7),cell+4
                0x205F, 0x4FEF, 0x0010, 0x4ED0, // movea.l (a7)+,a0; lea 16(a7),a7; jmp (a0)
            ])
            let e = block(4)
            // Asynchronous open: T_OPENCOMPLETE reaches the notifier at the next poll.
            #expect(call("OTAsyncOpenEndpointPriv", call("OTCreateConfiguration", cstr("udp")), 0, 0, notifier, 0) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x2000_0007) // T_OPENCOMPLETE
            let ref = rd32(cell + 4) // the cookie is the new endpoint
            #expect(ref != 0)
            #expect(call("OTSetAsynchronous", ref) == 0)
            // Bind: its completion comes the same way.
            #expect(call("OTBind", ref, 0, 0) == 0)
            opentransport_poll()
            #expect(rd32(cell) == 0x2000_0001) // T_BINDCOMPLETE
            #expect(call("OTCloseProviderPriv", ref) == 0)
            _ = e
            // A deferred task: a Pascal routine (one long) that stores its argument.
            let tcell = block(4)
            wr32(tcell, 0)
            let task = code([
                0x23EF, 0x0004, UInt16(tcell >> 16), UInt16(tcell & 0xFFFF), // move.l 4(a7),tcell
                0x205F, 0x588F, 0x4ED0, // movea.l (a7)+,a0; addq.l #4,a7; jmp (a0)
            ])
            let t = call("OTCreateDeferredTaskPriv", task, 0x1234_5678)
            #expect(t != 0 && call("OTScheduleDeferredTask", t) == 1)
            ot_tasks_pump()
            #expect(rd32(tcell) == 0x1234_5678)
            #expect(call("OTDestroyDeferredTask", t) == 0)
        }

        @Test func callbacksOfUtilities() {
            start()
            // A hash list whose hash procedure is 68k code: the link's address.
            let hash = code([0x202F, 0x0004, 0x4E75]) // move.l 4(a7),d0; rts
            let match = code([0x202F, 0x0004, 0xB0AF, 0x0008, 0x57C0, 0x4480, 0x4E75]) // d0=ref; cmp.l 8(a7),d0; seq d0; neg.l d0; rts
            let need = call("OTCalculateHashListMemoryNeeds", 8)
            let mem = block(Int(need))
            #expect(call("OTInitHashList", mem, need, hash) == 0)
            let l1 = block(8), l2 = block(8)
            call2("OTAddToHashList", mem, l1)
            call2("OTAddToHashList", mem, l2)
            #expect(call("OTIsInHashList", mem, l1) == 1 && call("OTIsInHashList", mem, l2) == 1)
            #expect(call("OTFindInHashList", mem, match, l2, l2) == l2)
            #expect(call("OTFindInHashList", mem, match, 0x77, l2) == 0)
            #expect(call("OTRemoveLinkFromHashList", mem, l1) == 1 && call("OTIsInHashList", mem, l1) == 0)
        }

        func call2(_ name: String, _ a: UInt32, _ b: UInt32) { _ = call(name, a, b) }

        @Test func internetServicesFromGlue() {
            start()
            let e = block(4)
            let svc = call("OTOpenInternetServicesPriv", 0xFFFF_FFFD, 0, e)
            #expect(rd32(e) == 0 && svc != 0)
            let hinfo = block(296)
            #expect(call("OTInetStringToAddress", svc, cstr("127.0.0.1"), hinfo) == 0 && rd32(hinfo + 256) == 0x7F00_0001)
            let str = block(32)
            _ = call("OTInetHostToString", 0xC0A8_0102, str)
            #expect(cstring(at: str) == "192.168.1.2")
            let host = block(4)
            #expect(call("OTInetStringToHost", cstr("10.1.2.3"), host) == 0 && rd32(host) == 0x0A01_0203)
            let addr = block(16)
            _ = call("OTInitInetAddress", addr, 8080, 0x7F00_0001) // a word port
            #expect(rd16(addr) == 2 && rd16(addr + 2) == 8080 && rd32(addr + 4) == 0x7F00_0001)
            let info = block(552)
            let r = err(call("OTInetGetInterfaceInfo", info, 0xFFFF_FFFF))
            #expect(r == 0 || r == -3201)
            #expect(call("OTCloseProviderPriv", svc) == 0)
        }
    }
}
