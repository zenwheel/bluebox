import CCore
import Darwin
import ForkStore
import Testing
import Toolbox

extension BlueBox {
    /// MacTCP's .IPP driver, on the host's own sockets, and its resolver.
    @Suite struct MacTCP {
        func setStack() {
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
        }

        /// A listening socket on the loopback address; returns it and its port.
        func listener() -> (Int32, UInt16) {
            let fd = socket(AF_INET, SOCK_STREAM, 0)
            var sa = sockaddr_in()
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

        func control(_ pb: UInt32, _ code: Int) -> Int16 {
            wr16(pb + UInt32(CntrlParam_csCode_off), UInt16(code))
            return tb_PBControl(pb, false)
        }

        /// MacTCP's completion routines are C functions taking the
        /// parameter block on the stack (QuickDNS Lite's TCPCreate one),
        /// not only in A0 as the File Manager's do.
        @Test func completionGetsTheBlockOnTheStack() {
            launchTestApp()
            setStack()
            let pb = tb_NewPtr(128, false, true), cs = pb + 32
            let name = tb_NewPtr(8, false, true)
            guestPString(".IPP", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == 0)
            let got = tb_NewPtr(4, false, true)
            // MOVE.L 4(SP),got; RTS
            let words: [UInt16] = [0x23EF, 0x0004, UInt16(got >> 16), UInt16(got & 0xFFFF), 0x4E75]
            let code = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(code + UInt32(i * 2), w) }
            wr32(pb + UInt32(IOParam_ioCompletion_off), code)
            let rcv = tb_NewPtr(8192, false, true)
            wr32(cs + UInt32(TCPCreatePB_rcvBuff_off), rcv)
            wr32(cs + UInt32(TCPCreatePB_rcvBuffLen_off), 8192)
            wr16(pb + UInt32(CntrlParam_csCode_off), 30) // TCPCreate
            let sp = g_cpu!.pointee.a.7
            #expect(pb_call_done(pb, tb_PBControl(pb, true), true) == 0)
            #expect(rd32(got) == pb)
            #expect(g_cpu!.pointee.a.7 == sp)
            wr32(pb + UInt32(IOParam_ioCompletion_off), 0)
            #expect(control(pb, 42) == 0) // TCPRelease
        }

        @Test func tcpStreamTalksToTheHost() {
            launchTestApp()
            setStack()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let pb = tb_NewPtr(128, false, true), cs = pb + 32
            let name = tb_NewPtr(8, false, true)
            guestPString(".IPP", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == 0)
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off))) == -49)

            let rcv = tb_NewPtr(8192, false, true)
            wr32(cs + UInt32(TCPCreatePB_rcvBuff_off), rcv)
            wr32(cs + UInt32(TCPCreatePB_rcvBuffLen_off), 8192)
            #expect(control(pb, 30) == 0) // TCPCreate
            #expect(rd32(pb + 28) != 0)

            gmemset(cs, 0, 64)
            wr32(cs + UInt32(TCPOpenPB_remoteHost_off), 0x7F00_0001)
            wr16(cs + UInt32(TCPOpenPB_remotePort_off), port)
            #expect(control(pb, 32) == 0) // TCPActiveOpen
            let conn = accept(lfd, nil, nil)
            #expect(conn >= 0)
            defer { close(conn) }

            let data = tb_NewPtr(16, false, true), wds = tb_NewPtr(12, false, true)
            for (i, b) in "hello".utf8.enumerated() { wr8(data + UInt32(i), b) }
            wr16(wds, 5)
            wr32(wds + 2, data)
            gmemset(cs, 0, 64)
            wr32(cs + UInt32(TCPSendPB_wdsPtr_off), wds)
            #expect(control(pb, 34) == 0) // TCPSend
            var got = [UInt8](repeating: 0, count: 16)
            #expect(recv(conn, &got, 16, 0) == 5)
            #expect(String(decoding: got.prefix(5), as: UTF8.self) == "hello")

            _ = "world".withCString { send(conn, $0, 5, 0) }
            gmemset(cs, 0, 64)
            wr8(cs + UInt32(TCPReceivePB_commandTimeoutValue_off), 5)
            wr32(cs + UInt32(TCPReceivePB_rcvBuff_off), data)
            wr16(cs + UInt32(TCPReceivePB_rcvBuffLen_off), 16)
            #expect(control(pb, 37) == 0) // TCPRcv
            #expect(rd16(cs + UInt32(TCPReceivePB_rcvBuffLen_off)) == 5)
            #expect(String(decoding: (0..<5).map { rd8(data + UInt32($0)) }, as: UTF8.self) == "world")

            #expect(control(pb, 42) == 0) // TCPRelease
            #expect(control(pb, 37) == -23010) // invalidStreamPtr
        }

        /// Programs look for MacTCP by Gestalt and in the unit table.
        @Test func presentInGestaltAndUnitTable() {
            launchTestApp()
            setStack()
            var v: Int32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("mtcp")), &v) == 0 && v >> 16 == 0x0206)
            let table = rd32(UInt32(LM_UTableBase))
            #expect(rd16(UInt32(LM_UnitNtryCnt)) >= 49)
            let dce = rd32(rd32(table + 48 * 4))
            let hdr = rd32(dce + UInt32(DCtlEntry_dCtlDriver_off))
            #expect(String(decoding: (0..<Int(rd8(hdr + 18))).map { rd8(hdr + 19 + UInt32($0)) }, as: UTF8.self) == ".IPP")
            #expect(Int16(bitPattern: rd16(dce + UInt32(DCtlEntry_dCtlRefNum_off))) == -49)
            let pb = tb_NewPtr(128, false, true), name = tb_NewPtr(8, false, true)
            guestPString(".IPP", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == 0)
            #expect(rd16(dce + UInt32(DCtlEntry_dCtlFlags_off)) & 0x20 != 0) // dOpened
        }

        /// Fetch sends an FTP PORT command before it opens the listener the
        /// command names; a server that connects at once still gets through,
        /// and the passive open made afterwards has its connection.
        @Test func portCommandListensEarly() {
            launchTestApp()
            setStack()
            let (lfd, port) = listener()
            defer { close(lfd) }
            let (spare, dataPort) = listener() // a free port, for the data connection
            close(spare)
            let pb = tb_NewPtr(128, false, true), cs = pb + 32
            let name = tb_NewPtr(8, false, true)
            guestPString(".IPP", at: name)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), name)
            #expect(tb_PBOpen(pb, false) == 0)
            let rcv = tb_NewPtr(8192, false, true)
            wr32(cs + UInt32(TCPCreatePB_rcvBuff_off), rcv)
            wr32(cs + UInt32(TCPCreatePB_rcvBuffLen_off), 8192)
            #expect(control(pb, 30) == 0) // TCPCreate
            let ctl = rd32(pb + 28)
            gmemset(cs, 0, 64)
            wr32(cs + UInt32(TCPOpenPB_remoteHost_off), 0x7F00_0001)
            wr16(cs + UInt32(TCPOpenPB_remotePort_off), port)
            #expect(control(pb, 32) == 0) // TCPActiveOpen
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }

            let cmd = "PORT 127,0,0,1,\(dataPort >> 8),\(dataPort & 0xFF)\r\n"
            let data = tb_NewPtr(64, false, true), wds = tb_NewPtr(12, false, true)
            for (i, b) in cmd.utf8.enumerated() { wr8(data + UInt32(i), b) }
            wr16(wds, UInt16(cmd.utf8.count))
            wr32(wds + 2, data)
            gmemset(cs, 0, 64)
            wr32(cs + UInt32(TCPSendPB_wdsPtr_off), wds)
            #expect(control(pb, 34) == 0) // TCPSend

            // The server connects as soon as it reads the command.
            let dfd = socket(AF_INET, SOCK_STREAM, 0)
            defer { close(dfd) }
            var sa = sockaddr_in()
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_addr.s_addr = inet_addr("127.0.0.1")
            sa.sin_port = dataPort.bigEndian
            let r = withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    connect(dfd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            #expect(r == 0)

            let rcv2 = tb_NewPtr(8192, false, true)
            gmemset(cs, 0, 64)
            wr32(cs + UInt32(TCPCreatePB_rcvBuff_off), rcv2)
            wr32(cs + UInt32(TCPCreatePB_rcvBuffLen_off), 8192)
            #expect(control(pb, 30) == 0) // TCPCreate
            gmemset(cs, 0, 64)
            wr8(cs + UInt32(TCPOpenPB_commandTimeoutValue_off), 5)
            wr16(cs + UInt32(TCPOpenPB_localPort_off), dataPort)
            #expect(control(pb, 31) == 0) // TCPPassiveOpen
            #expect(rd32(cs + UInt32(TCPOpenPB_remoteHost_off)) == 0x7F00_0001)

            #expect(control(pb, 42) == 0) // TCPRelease
            wr32(pb + 28, ctl)
            #expect(control(pb, 42) == 0)
        }

        /// The resolver dnr.c finds: a 'ztcp' control panel whose 'dnrp'
        /// jumps to us.  A dotted address resolves to itself, with no name.
        @Test func resolverTakesDottedAddresses() throws {
            launchTestApp()
            setStack()
            let path = testSystemFolder + "/Control Panels/MacTCP"
            var f = mac_file()
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_read(path, &f, &err, err.count))
            defer { mac_file_free(&f) }
            #expect(withUnsafeBytes(of: f.finder_info) { Array($0.prefix(8)) } == Array("cdevztcp".utf8))
            var idx = rsrc_index()
            #expect(rsrc_parse(f.rsrc, f.rsrc_len, &idx, &err, err.count))
            defer { rsrc_index_free(&idx) }
            let entries = (0..<idx.count).map { idx.entries[$0] }
            var dnrp = try #require(entries.first { $0.type == fourCC("dnrp") && $0.id == 128 })
            let code = UnsafeBufferPointer(start: rsrc_data(f.rsrc, &idx, &dnrp), count: Int(dnrp.data_len))
            #expect(code.count == 6 && code[0] == 0x4E && code[1] == 0xF9) // JMP
            let entry = code[2...5].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }

            let host = tb_NewPtr(16, false, true), info = tb_NewPtr(276, false, true)
            for (i, b) in "127.0.0.1".utf8.enumerated() { wr8(host + UInt32(i), b) }
            let c = g_cpu!
            let sp = c.pointee.a.7
            for v in [0, 0, info, host, 3] as [UInt32] { cpu_push32(c, v) } // StrToAddr
            guest_call_regs(entry)
            c.pointee.a.7 = sp
            #expect(c.pointee.d.0 == 0)
            #expect(rd32(info) == 0) // rtnCode
            #expect(rd8(info + 4) == 0) // no cname
            #expect(rd32(info + 260) == 0x7F00_0001, "\(String(rd32(info + 260), radix: 16)) entry \(String(entry, radix: 16))")
        }
    }
}
