import CCore
import Darwin
import Testing
import Toolbox

extension BlueBox {
    /// Open Transport's TCP/IP from PowerPC code, on the host's sockets:
    /// the exports called as a PowerPC caller would, arguments in r3 on.
    @Suite struct OpenTransportInet {
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

        /// A DNS address ("host:port", as Fetch 4 connects) is looked up
        /// by OTConnect, and OTSnd takes its data as a chain of OTData.
        @Test func dnsAddressAndOTDataChain() {
            launchTestApp()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let e = tb_NewPtr(4, false, true)
            let ep = ot("OTOpenEndpoint", ot("OTCreateConfiguration", cstr("tcp")), 0, 0, e)
            #expect(rd32(e) == 0 && ep != 0)
            #expect(ot("OTSetBlocking", ep) == 0)
            #expect(ot("OTBind", ep, 0, 0) == 0)

            let dns = tb_NewPtr(260, false, true)
            let len = ot("OTInitDNSAddress", dns, cstr("localhost:\(port)"))
            #expect(rd16(dns) == 42 && len == UInt32(2 + "localhost:\(port)".utf8.count))
            let call = tb_NewPtr(40, false, true)
            wr32(call + 0, len); wr32(call + 4, len); wr32(call + 8, dns)
            #expect(ot("OTConnect", ep, call, 0) == 0)
            let conn = accept(lfd, nil, nil)
            #expect(conn >= 0)
            defer { close(conn) }

            // "Open " then "Transport", in two OTData pieces.
            let second = tb_NewPtr(12, false, true), first = tb_NewPtr(12, false, true)
            wr32(second, 0); wr32(second + 4, cstr("Transport")); wr32(second + 8, 9)
            wr32(first, second); wr32(first + 4, cstr("Open ")); wr32(first + 8, 5)
            #expect(ot("OTSnd", ep, first, 0xFFFF_FFFE, 0) == 14) // kNetbufDataIsOTData
            var buf = [UInt8](repeating: 0, count: 32)
            var got = 0
            while got < 14 {
                let n = buf.withUnsafeMutableBytes { recv(conn, $0.baseAddress! + got, 32 - got, 0) }
                if n <= 0 { break }
                got += n
            }
            #expect(String(decoding: buf[0..<got], as: UTF8.self) == "Open Transport")
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        /// The run's own Gestalt answers (--gestalt) stand over the ones a
        /// PowerPC program is given (Spades Deluxe 2.0 wants Mac OS 8.1).
        @Test func gestaltOverrideKeptForPowerPC() {
            bootMachine()
            gestalt_override(fourCC("sysv"), 0x0810)
            gestalt_powerpc()
            var answer: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("sysv")), &answer) == 0 && answer == 0x0810)
            #expect(rd16(UInt32(LM_SysVersion)) == 0x0810)
        }
    }
}
