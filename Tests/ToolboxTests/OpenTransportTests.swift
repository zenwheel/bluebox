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

        /// A connection the server closes, read without blocking by a
        /// program with a notifier: the read that finds the end has no data
        /// until the notifier has been told T_ORDREL, then kOTLookErr and
        /// T_ORDREL (Internet Explorer 3 took an early kOTLookErr as the
        /// page failing to load).
        @Test func releaseToldBeforeLookError() {
            launchTestApp()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let e = tb_NewPtr(4, false, true)
            let ep = ot("OTOpenEndpoint", ot("OTCreateConfiguration", cstr("tcp")), 0, 0, e)
            #expect(rd32(e) == 0 && ep != 0)
            // A PowerPC notifier that only returns: blr, through a transition vector.
            let code = tb_NewPtr(4, false, true), tv = tb_NewPtr(8, false, true)
            wr32(code, 0x4E80_0020)
            wr32(tv, code)
            #expect(ot("OTSetBlocking", ep) == 0)
            #expect(ot("OTBind", ep, 0, 0) == 0)
            let dns = tb_NewPtr(260, false, true)
            let len = ot("OTInitDNSAddress", dns, cstr("127.0.0.1:\(port)"))
            let call = tb_NewPtr(40, false, true)
            wr32(call + 0, len); wr32(call + 4, len); wr32(call + 8, dns)
            #expect(ot("OTConnect", ep, call, 0) == 0)
            let conn = accept(lfd, nil, nil)
            #expect(conn >= 0)
            _ = "page".withCString { send(conn, $0, 4, 0) }
            close(conn)
            usleep(50_000)
            #expect(ot("OTSetNonBlocking", ep) == 0)
            opentransport_poll() // the connection's own events, before the notifier
            #expect(ot("OTInstallNotifier", ep, tv, 0) == 0)
            let buf = tb_NewPtr(64, false, true), flags = tb_NewPtr(4, false, true)
            #expect(ot("OTRcv", ep, buf, 64, flags) == 4)
            #expect(Int32(bitPattern: ot("OTRcv", ep, buf, 64, flags)) == -3162) // kOTNoDataErr: not told yet
            #expect(Int32(bitPattern: ot("OTRcv", ep, buf, 64, flags)) == -3162)
            opentransport_poll() // the notifier hears T_ORDREL
            g_cpu!.pointee.halted = false // (the stub notifier has nowhere to return to here)
            #expect(Int32(bitPattern: ot("OTRcv", ep, buf, 64, flags)) == -3158) // kOTLookErr
            #expect(ot("OTLook", ep) == 0x80) // T_ORDREL
            #expect(ot("OTRcvOrderlyDisconnect", ep) == 0)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        nonisolated(unsafe) static var opened: [String] = []

        /// Internet Config's ICLaunchURL hands the selected URL, less its
        /// "<URL:...>" wrapping, to the host (Skittles 2's home page link).
        @Test func launchURLGoesToHost() {
            launchTestApp()
            Self.opened = []
            machine_set_url_handler({ u, _ in
                OpenTransportInet.opened.append(String(cString: u!))
                return true
            }, nil)
            defer { machine_set_url_handler(nil, nil) }
            let text = "see <URL:http://www.skittles2.com/> now"
            let data = cstr(text), ps = tb_NewPtr(4, false, true), pe = tb_NewPtr(4, false, true)
            wr32(ps, 4); wr32(pe, 35)
            #expect(ot("ICLaunchURL", 0, 0, data, UInt32(text.utf8.count), ps, pe) == 0)
            wr32(ps, 0); wr32(pe, 0) // no selection: the whole text
            let bare = cstr("ftp://a.example/"), none = cstr("not a url")
            #expect(ot("ICLaunchURL", 0, 0, bare, 16, ps, pe) == 0)
            #expect(Int32(bitPattern: ot("ICLaunchURL", 0, 0, none, 9, ps, pe)) == -673) // icNoURLErr
            #expect(Self.opened == ["http://www.skittles2.com/", "ftp://a.example/"])
        }

        func pstr(_ s: String) -> UInt32 {
            let bytes = Array(s.utf8)
            let p = tb_NewPtr(Int32(bytes.count + 1), false, true)
            wr8(p, UInt8(bytes.count))
            for (i, b) in bytes.enumerated() { wr8(p + 1 + UInt32(i), b) }
            return p
        }

        /// Internet Config's preferences: what the host set (the Internet
        /// settings), found whatever the key's case, truncated to the room
        /// given; set, counted, listed and deleted by the program.
        @Test func internetConfigPreferences() {
            launchTestApp()
            let proxy: [UInt8] = [14] + Array("proxy.lan:8080".utf8)
            proxy.withUnsafeBufferPointer { machine_set_internet_pref("HTTPProxyHost", $0.baseAddress, Int32($0.count)) }
            machine_set_internet_pref("UseHTTPProxy", [1], 1)
            let attr = tb_NewPtr(4, false, true), size = tb_NewPtr(4, false, true), buf = tb_NewPtr(64, false, true)
            wr32(size, 64)
            #expect(ot("ICGetPref", 0, pstr("httpproxyhost"), attr, buf, size) == 0)
            #expect(rd32(size) == 15 && rd8(buf) == 14 && rd8(buf + 1) == UInt8(ascii: "p"))
            wr32(size, 4) // too little room: what fits, and the length
            #expect(Int32(bitPattern: ot("ICGetPref", 0, pstr("HTTPProxyHost"), attr, buf, size)) == -664)
            #expect(rd32(size) == 15)
            wr32(size, 1)
            #expect(ot("ICGetPref", 0, pstr("UseHTTPProxy"), attr, buf, size) == 0 && rd8(buf) == 1)
            #expect(Int32(bitPattern: ot("ICGetPref", 0, pstr("NoSuchPref"), attr, buf, size)) == -666)

            let hp = tb_NewPtr(4, false, true)
            #expect(ot("ICGetPrefHandle", 0, pstr("HTTPProxyHost"), attr, hp) == 0)
            #expect(tb_GetHandleSize(rd32(hp)) == 15 && rd8(rd32(rd32(hp)) + 14) == UInt8(ascii: "0"))

            let before = tb_NewPtr(4, false, true), count = tb_NewPtr(4, false, true)
            ot("ICGetSeed", 0, before)
            ot("ICCountPref", 0, count)
            let n = rd32(count)
            let data = cstr("abc")
            #expect(ot("ICSetPref", 0, pstr("TestPref"), 0, data, 3) == 0)
            let after = tb_NewPtr(4, false, true)
            ot("ICGetSeed", 0, after)
            #expect(rd32(after) != rd32(before))
            ot("ICCountPref", 0, count)
            #expect(rd32(count) == n + 1)
            let key = tb_NewPtr(256, false, true)
            let names = (1...Int(rd32(count))).map { i -> String in
                _ = ot("ICGetIndPref", 0, UInt32(i), key)
                return String(decoding: (0..<Int(rd8(key))).map { rd8(key + 1 + UInt32($0)) }, as: UTF8.self)
            }
            #expect(names.contains("TestPref") && names.contains("HTTPProxyHost"))
            #expect(ot("ICDeletePref", 0, pstr("testpref")) == 0)
            #expect(Int32(bitPattern: ot("ICDeletePref", 0, pstr("TestPref"))) == -666)
        }

        /// A 68k installer is told it runs on a PowerPC Mac, so a fat
        /// installer installs the PowerPC build (Internet Explorer 4.01's);
        /// other 68k programs still see a 68k Mac.
        @Test func installersSeeAPowerPC() {
            var answer: UInt32 = 0
            launchTestApp()
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("sysa")), &answer) == 0 && answer == 1)
            launchTestApp(name: "IE 4.01 Full Fat Installer")
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("sysa")), &answer) == 0 && answer == 2)
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("otan")), &answer) == 0 && answer & 1 != 0) // Open Transport
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
