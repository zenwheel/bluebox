import CCore
import CPU68K
import Darwin
import Foundation
import Testing
import Toolbox

/// What the test natives below were given: bytes each side of an XMODEM
/// transfer sent the other, search hits, completions, a terminal's replies.
private var pipes: [[UInt8]] = [[], []]
private var searchHits = [UInt32]()
private var completions = 0
private var termSent = [UInt8]()

extension BlueBox {
    /// The Communications Toolbox: connections, terminals and file transfers.
    @Suite struct CommToolbox {
        enum Arg {
            case w(Int)
            case l(UInt32)
            case b(Bool)
        }

        func code(_ words: [UInt16]) -> UInt32 {
            let p = tb_NewPtr(Int32(words.count * 2), false, false)
            for (i, w) in words.enumerated() { wr16(p + UInt32(i * 2), w) }
            return p
        }

        /// A call as the glue makes it: the arguments pushed in order, the
        /// selector last, A0 at it, _CommToolboxDispatch; D0 is the result.
        @discardableResult
        func ctb(_ sel: UInt16, _ args: [Arg]) -> UInt32 {
            let out = tb_NewPtr(4, false, true)
            var w = [UInt16](), bytes = 2
            for a in args {
                switch a {
                case .w(let v): w += [0x3F3C, UInt16(truncatingIfNeeded: v)]; bytes += 2
                case .b(let v): w += [0x3F3C, v ? 0x0100 : 0]; bytes += 2
                case .l(let v): w += [0x2F3C, UInt16(v >> 16), UInt16(v & 0xFFFF)]; bytes += 4
                }
            }
            w += [0x3F3C, sel, 0x204F, 0xA08B, 0x4FEF, UInt16(bytes), 0x23C0, UInt16(out >> 16), UInt16(out & 0xFFFF), 0x4E75]
            guest_call(code(w))
            return rd32(out)
        }

        func pstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(256, false, true)
            guestPString(s, at: p)
            return p
        }

        func cstr(_ s: String) -> UInt32 {
            let bytes = Array(s.utf8) + [0]
            let p = tb_NewPtr(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func cstring(at p: UInt32) -> String {
            var bytes = [UInt8]()
            var i: UInt32 = 0
            while rd8(p + i) != 0 { bytes.append(rd8(p + i)); i += 1 }
            return String(decoding: bytes, as: UTF8.self)
        }

        func start() {
            launchTestApp()
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
        }

        /// A listening socket on a free port of this machine.
        func server() -> (Int32, UInt16) {
            let lfd = socket(AF_INET, SOCK_STREAM, 0)
            var sa = sockaddr_in()
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
            return (lfd, UInt16(bigEndian: sa.sin_port))
        }

        func send(_ fd: Int32, _ s: String) { _ = s.withCString { Darwin.send(fd, $0, strlen($0), 0) } }

        func receive(_ fd: Int32, _ n: Int) -> String {
            var got = [UInt8]()
            var buf = [UInt8](repeating: 0, count: 256)
            for _ in 0..<100 where got.count < n {
                var pf = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
                if poll(&pf, 1, 10) > 0 {
                    let r = recv(fd, &buf, 256, 0)
                    if r <= 0 { break }
                    got += buf.prefix(r)
                }
            }
            return String(decoding: got, as: UTF8.self)
        }

        /// CMStatus until that much has come in (or a second passes).
        func waitForData(_ h: UInt32, _ n: UInt32) -> UInt32 {
            let sizes = tb_NewPtr(32, false, true), flags = tb_NewPtr(4, false, true)
            for _ in 0..<100 {
                #expect(ctb(0x0110, [.l(h), .l(sizes), .l(flags)]) == 0)
                if rd32(sizes) >= n { break }
                usleep(10000)
            }
            return rd32(sizes)
        }

        func read(_ h: UInt32, _ n: UInt32) -> String {
            let buf = tb_NewPtr(Int32(n), false, true), count = tb_NewPtr(4, false, true), flags = tb_NewPtr(2, false, true)
            wr32(count, n)
            let e = Int16(truncatingIfNeeded: ctb(0x0111, [.l(h), .l(buf), .l(count), .w(1), .b(false), .l(0), .l(0), .l(flags)]))
            #expect(e == 0)
            return String(decoding: (0..<rd32(count)).map { rd8(buf + $0) }, as: UTF8.self)
        }

        func write(_ h: UInt32, _ s: String) {
            let bytes = Array(s.utf8)
            let buf = tb_NewPtr(Int32(bytes.count), false, false), count = tb_NewPtr(4, false, true)
            for (i, b) in bytes.enumerated() { wr8(buf + UInt32(i), b) }
            wr32(count, UInt32(bytes.count))
            #expect(ctb(0x0112, [.l(h), .l(buf), .l(count), .w(1), .b(false), .l(0), .l(0), .w(0)]) == 0)
            #expect(rd32(count) == UInt32(bytes.count))
        }

        /// A connection by name, configured.
        func newConnection(_ tool: String, _ config: String) -> UInt32 {
            #expect(ctb(0x0101, []) == 0) // InitCM
            let id = Int16(truncatingIfNeeded: ctb(0x0107, [.l(pstr(tool))]))
            #expect(id > 0)
            let h = ctb(0x0108, [.w(Int(id)), .l(0), .l(0), .l(0x1234), .l(0)])
            #expect(h != 0)
            #expect(ctb(0x0103, [.l(h)]) == 0x1234) // CMGetRefCon
            #expect(ctb(0x011D, [.l(h), .l(cstr(config))]) == 0) // CMSetConfig
            #expect(ctb(0x0117, [.l(h)]) == 0) // CMValidate: all's well
            return h
        }

        @Test func toolsAndVersions() {
            start()
            var answer: UInt32 = 0
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("ctbv")), &answer) == 0 && answer == 2)
            #expect(tb_Gestalt(Int32(bitPattern: fourCC("conn")), &answer) == 0 && answer & 1 == 1)
            let name = tb_NewPtr(256, false, true)
            for (bundle, tools) in [("cbnd", ["Serial Tool", "Apple Modem Tool", "TCP Tool"]), ("tbnd", ["TTY Tool", "VT102 Tool"]),
                                    ("fbnd", ["Text", "XMODEM Tool"])] {
                var found = [String]()
                var i = 1
                while ctb(0x0511, [.l(fourCC(bundle)), .w(i), .l(name)]) == 0 { // CRMGetIndToolName
                    found.append(pstring(at: name))
                    i += 1
                }
                #expect(found == tools)
            }
            #expect(ctb(0x0121, []) == 2 && ctb(0x0326, []) == 2 && ctb(0x021B, []) == 2) // the managers' versions
            #expect(Int16(truncatingIfNeeded: ctb(0x0107, [.l(pstr("No Such Tool"))])) == -1)
            #expect(cfm_export_fn("CMNew") != nil && cfm_export_fn("TMStream") != nil && cfm_export_fn("FTExec") != nil)
            // From PowerPC code: InterfaceLib's CMGetProcID, its argument in r3.
            g_ppc.gpr.3 = pstr("TCP Tool")
            cfm_export_fn("CMGetProcID")!(&g_ppc)
            #expect(g_ppc.gpr.3 == 3)

            // A configuration as a string, a bad one refused where it goes wrong.
            let h = newConnection("Serial Tool", "Baud 19200 Parity Even Port \"Printer Port\"")
            let s = ctb(0x011C, [.l(h)])
            #expect(cstring(at: s) == "Baud 19200 DataBits 8 Parity Even StopBits 1 Port \"Printer Port\" Handshake None HoldConnection False RemindDisconnect False")
            #expect(ctb(0x011D, [.l(h), .l(cstr("Baud 9600 Parity Sideways"))]) == 11)
            let version = ctb(0x0120, [.l(h)]) // CMGetVersion: a 'vers'
            #expect(version != 0 && rd8(rd32(version)) == 1)
            ctb(0x0109, [.l(h)])
        }

        @Test func tcpToolReadsWritesAndSearches() {
            start()
            let (lfd, port) = server()
            defer { close(lfd) }
            let h = newConnection("TCP Tool", "Host 127.0.0.1 Port \(port) Telnet False")
            #expect(ctb(0x010B, [.l(h), .b(false), .l(0), .l(600)]) == 0) // CMOpen
            let conn = accept(lfd, nil, nil)
            #expect(conn >= 0)
            defer { close(conn) }

            let hit = native_register({ c, _ in
                let ret = cpu_pop32(c)
                let refNum = cpu_pop32(c), match = cpu_pop32(c)
                _ = cpu_pop32(c)
                searchHits += [refNum, UInt32(rd8(match))]
                cpu_push32(c, ret)
            }, "SearchHit", 0)
            searchHits = []
            let id = ctb(0x0126, [.l(h), .l(pstr("ogin:")), .w(0), .l(hit)]) // CMAddSearch
            send(conn, "Welcome\r\nlogin: ")
            #expect(waitForData(h, 16) == 16)
            #expect(read(h, 16) == "Welcome\r\nlogin: ")
            #expect(searchHits == [id, UInt32(UInt8(ascii: ":"))])
            write(h, "guest\r")
            #expect(receive(conn, 6) == "guest\r")

            // An asynchronous read, finished at interrupt time.
            let done = native_register({ c, _ in
                let ret = cpu_pop32(c)
                _ = cpu_pop32(c)
                completions += 1
                cpu_push32(c, ret)
            }, "ReadDone", 0)
            completions = 0
            let buf = tb_NewPtr(8, false, true), count = tb_NewPtr(4, false, true)
            wr32(count, 4)
            #expect(ctb(0x0111, [.l(h), .l(buf), .l(count), .w(1), .b(true), .l(done), .l(UInt32(bitPattern: -1)), .l(0)]) == 0)
            send(conn, "$ ok")
            for _ in 0..<100 where completions == 0 {
                usleep(10000)
                ctb_poll()
            }
            #expect(completions == 1)
            #expect(rd32(rd32(h) + UInt32(ConnRecord_asyncCount_off)) == 4)
            #expect(String(decoding: (0..<4).map { rd8(buf + $0) }, as: UTF8.self) == "$ ok")

            // The other end goes: the connection isn't open any more.
            close(conn)
            let sizes = tb_NewPtr(32, false, true), flags = tb_NewPtr(4, false, true)
            for _ in 0..<100 {
                ctb(0x0110, [.l(h), .l(sizes), .l(flags)])
                if rd32(flags) & 2 == 0 { break }
                usleep(10000)
            }
            #expect(rd32(flags) & 2 == 0)
            #expect(ctb(0x010E, [.l(h), .b(false), .l(0), .l(0), .b(true)]) == 0) // CMClose
            ctb(0x0109, [.l(h)])
        }

        /// The Serial Tool on a port that is a TCP connection (tcp:host:port).
        @Test func serialToolOverATcpPort() {
            let (lfd, port) = server()
            defer { close(lfd) }
            #expect(machine_set_serial(0, "tcp:127.0.0.1:\(port)"))
            defer { _ = machine_set_serial(0, "modem") }
            start()
            let h = newConnection("Serial Tool", "Baud 57600 Port \"Modem Port\"")
            #expect(ctb(0x010B, [.l(h), .b(false), .l(0), .l(0)]) == 0)
            #expect(rd8(0x290) != 0xFF) // the modem port in use
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            write(h, "ATZ")
            #expect(receive(conn, 3) == "ATZ")
            send(conn, "OK")
            #expect(waitForData(h, 2) == 2)
            #expect(read(h, 2) == "OK")
            let env = tb_NewPtr(Int32(ConnEnvironRec_SIZEOF), false, true)
            #expect(ctb(0x012C, [.l(h), .l(env)]) == 0)
            #expect(rd32(env + UInt32(ConnEnvironRec_baudRate_off)) == 57600)
            ctb(0x010E, [.l(h), .b(false), .l(0), .l(0), .b(true)])
            #expect(rd8(0x290) == 0xFF)
            ctb(0x0109, [.l(h)])
        }

        /// The Apple Modem Tool dials the modem port's modem.
        @Test func modemToolDials() {
            let (lfd, port) = server()
            defer { close(lfd) }
            start()
            let h = newConnection("Apple Modem Tool", "PhoneNumber \"127.0.0.1:\(port)\"")
            #expect(ctb(0x010B, [.l(h), .b(false), .l(0), .l(600)]) == 0)
            let conn = accept(lfd, nil, nil)
            defer { close(conn) }
            send(conn, "BBS")
            #expect(waitForData(h, 3) == 3)
            #expect(read(h, 3) == "BBS")
            ctb(0x010E, [.l(h), .b(false), .l(0), .l(0), .b(true)])
            ctb(0x0109, [.l(h)])
        }

        // MARK: Terminals

        func line(_ h: UInt32, _ n: Int) -> (String, [UInt8]) {
            let block = tb_NewPtr(Int32(TermDataBlock_SIZEOF), false, true)
            wr32(block + UInt32(TermDataBlock_theData_off), tb_NewHandle(0, false, false))
            wr32(block + UInt32(TermDataBlock_auxData_off), tb_NewHandle(0, false, false))
            ctb(0x0310, [.l(h), .w(n), .l(block)])
            let data = rd32(block + UInt32(TermDataBlock_theData_off)), aux = rd32(block + UInt32(TermDataBlock_auxData_off))
            let n = UInt32(tb_GetHandleSize(data))
            let text = String(decoding: (0..<n).map { rd8(rd32(data) + $0) }, as: UTF8.self)
            let attrs = (0..<n).map { rd8(rd32(aux) + $0) }
            var trimmed = text
            while trimmed.hasSuffix(" ") { trimmed.removeLast() }
            return (trimmed, attrs)
        }

        func stream(_ h: UInt32, _ s: String) {
            let bytes = Array(s.utf8)
            let buf = tb_NewPtr(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(ctb(0x030A, [.l(h), .l(buf), .l(UInt32(bytes.count)), .w(0)]) == UInt32(bytes.count))
        }

        func newTerminal(_ tool: String, owner: UInt32) -> UInt32 {
            #expect(ctb(0x0301, []) == 0) // InitTM
            let id = Int(Int16(truncatingIfNeeded: ctb(0x031F, [.l(pstr(tool))])))
            #expect(id > 0)
            let rect = tb_NewPtr(8, false, true)
            wr16(rect + 4, 486); wr16(rect + 6, 486) // 0, 0, 486, 486
            let sendProc = native_register({ c, _ in
                let ret = cpu_pop32(c)
                c!.pointee.a.7 += 2 // flags
                _ = cpu_pop32(c)  // refCon
                let size = cpu_pop32(c), ptr = cpu_pop32(c)
                termSent += (0..<size).map { rd8(ptr + $0) }
                wr32(c!.pointee.a.7, size)
                cpu_push32(c, ret)
            }, "TermSend", 0)
            termSent = []
            let h = ctb(0x0302, [.l(rect), .l(rect), .l(0), .w(id), .l(owner), .l(sendProc), .l(0), .l(0), .l(0), .l(0), .l(0), .l(0)])
            #expect(h != 0)
            return h
        }

        @Test func vt102Screen() {
            start()
            let port = initToolbox()
            let h = newTerminal("VT102 Tool", owner: port)
            stream(h, "\u{1B}[2J\u{1B}[HHello\r\nWorld")
            #expect(line(h, 1).0 == "Hello" && line(h, 2).0 == "World")
            #expect(ctb(0x032A, [.l(h), .w(1)]) == (2 << 16 | 6)) // TMGetCursor: row 2, column 6

            // Cursor addressing, attributes, erasing.
            stream(h, "\u{1B}[5;10HX\u{1B}[1;7mB\u{1B}[0;4mU\u{1B}[m")
            let (five, attrs) = line(h, 5)
            #expect(five == String(repeating: " ", count: 9) + "XBU")
            #expect(attrs[9] == 0 && attrs[10] == 1 | 8 && attrs[11] == 2)
            stream(h, "\u{1B}[1;3H\u{1B}[K")
            #expect(line(h, 1).0 == "He")
            stream(h, "\u{1B}[2;3H\u{1B}[1P\u{1B}[2@")
            #expect(line(h, 2).0 == "Wo  ld")

            // A scrolling region: lines 3 to 5 move up, 1, 2 and 6 stay.
            stream(h, "\u{1B}[3;1Hthree\u{1B}[4;1Hfour\u{1B}[6;1Hsix\u{1B}[3;5r\u{1B}[5;1H\n")
            #expect(line(h, 3).0 == "four" && line(h, 4).0.hasSuffix("XBU") && line(h, 5).0 == "")
            #expect(line(h, 2).0 == "Wo  ld" && line(h, 6).0 == "six")
            stream(h, "\u{1B}[r\u{1B}M") // the whole screen again; reverse index at the top scrolls down
            #expect(ctb(0x032A, [.l(h), .w(1)]) == (1 << 16 | 1))
            #expect(line(h, 1).0 == "" && line(h, 2).0 == "He")

            // The line drawing set, wrapping at the right margin.
            stream(h, "\u{1B}[10;1H\u{1B}(0lqk\u{1B}(Bx\u{1B}[11;79Habc")
            #expect(line(h, 10).0 == "+-+x")
            #expect(line(h, 11).0.hasSuffix("ab") && line(h, 12).0 == "c")

            // Reports go back through the sendProc.
            stream(h, "\u{1B}[6n\u{1B}[c")
            #expect(String(decoding: termSent, as: UTF8.self) == "\u{1B}[12;2R\u{1B}[?6c")

            // Keys: arrows as the cursor key mode has them, Return.
            termSent = []
            let evt = tb_NewPtr(16, false, true)
            for msg: UInt32 in [0x7E1E, 0x240D] {
                wr32(evt + UInt32(EventRecord_message_off), msg)
                ctb(0x0304, [.l(h), .l(evt)])
            }
            stream(h, "\u{1B}[?1h")
            wr32(evt + UInt32(EventRecord_message_off), 0x7B1C)
            ctb(0x0304, [.l(h), .l(evt)])
            #expect(String(decoding: termSent, as: UTF8.self) == "\u{1B}[A\r\u{1B}OD")
            #expect(ctb(0x032F, [.l(h)]) > 4) // TMCountTermKeys
            termSent = []
            #expect(ctb(0x032E, [.l(h), .l(pstr("PF1"))]) & 0xFF == 1) // TMDoTermKey
            #expect(String(decoding: termSent, as: UTF8.self) == "\u{1B}OP")

            // A selection's text.
            let sel = tb_NewPtr(8, false, true)
            wr16(sel, 10); wr16(sel + 2, 1); wr16(sel + 4, 10); wr16(sel + 6, 4)
            ctb(0x0311, [.l(h), .l(sel), .w(1)]) // TMSetSelection
            let text = tb_NewHandle(0, false, false), type = tb_NewPtr(4, false, true)
            #expect(ctb(0x030F, [.l(h), .l(text), .l(type)]) == 3)
            #expect(rd32(type) == fourCC("TEXT") && rd8(rd32(text)) == UInt8(ascii: "+"))

            let env = tb_NewPtr(Int32(TermEnvironRec_SIZEOF), false, true)
            #expect(ctb(0x032B, [.l(h), .l(env)]) == 0)
            #expect(rd16(env + UInt32(TermEnvironRec_textRows_off)) == 24 && rd16(env + UInt32(TermEnvironRec_textCols_off)) == 80)
            ctb(0x0305, [.l(h), .l(0)]) // TMUpdate draws it all
            tb_SetPort(port)
            // "He" on the second line (11 pixels a line in Monaco 9), black on white.
            let ink = (11..<22).flatMap { v in (0..<12).map { h in tb_GetPixel(Int16(h), Int16(v)) } }.filter { $0 != 0 }.count
            #expect(ink > 10)
            #expect(tb_GetPixel(200, 100) == 0)
            ctb(0x0303, [.l(h)])
        }

        @Test func ttyIgnoresEscapes() {
            start()
            let h = newTerminal("TTY Tool", owner: 0)
            stream(h, "one\r\ntwo\u{1B}[2Jthree\u{8}\u{8}X")
            #expect(line(h, 1).0 == "one" && line(h, 2).0 == "two[2JthrXe")
            ctb(0x0303, [.l(h)])
        }

        // MARK: File transfers

        func spec(_ path: String) -> UInt32 {
            let s = tb_NewPtr(70, false, true)
            fm_spec_for_path(path, s)
            return s
        }

        /// Two transfers, each side's sendProc feeding the other's recvProc.
        func transfer(tool: String, config: String, payload: [UInt8], dir: URL) -> (Int16, Int16, [UInt8]) {
            let up = dir.appendingPathComponent("Upload").path, down = dir.appendingPathComponent("Download").path
            try? Data(payload).write(to: URL(fileURLWithPath: up))
            try? FileManager.default.removeItem(atPath: down)
            pipes = [[], []]
            // sendProc (thePtr, theSize, refCon, channel, flags): Size
            let sendProc = native_register({ c, _ in
                let ret = cpu_pop32(c)
                c!.pointee.a.7 += 4 // flags, channel
                let refCon = cpu_pop32(c), size = cpu_pop32(c), ptr = cpu_pop32(c)
                pipes[Int(refCon)] += (0..<size).map { rd8(ptr + $0) }
                wr32(c!.pointee.a.7, size)
                cpu_push32(c, ret)
            }, "FTSendProc", 0)
            // recvProc (thePtr, theSize, refCon, channel, &flags): Size
            let recvProc = native_register({ c, _ in
                let ret = cpu_pop32(c)
                _ = cpu_pop32(c)
                c!.pointee.a.7 += 2
                let refCon = cpu_pop32(c), size = cpu_pop32(c), ptr = cpu_pop32(c)
                let from = 1 - Int(refCon)
                let n = min(Int(size), pipes[from].count)
                for i in 0..<n { wr8(ptr + UInt32(i), pipes[from][i]) }
                pipes[from].removeFirst(n)
                wr32(c!.pointee.a.7, UInt32(n))
                cpu_push32(c, ret)
            }, "FTRecvProc", 0)
            #expect(ctb(0x0201, []) == 0) // InitFT
            let id = Int(Int16(truncatingIfNeeded: ctb(0x0207, [.l(pstr(tool))])))
            #expect(id > 0)
            func new(_ refCon: UInt32) -> UInt32 {
                let h = ctb(0x0208, [.w(id), .l(0), .l(sendProc), .l(recvProc), .l(0), .l(0), .l(0), .l(0), .l(refCon), .l(0)])
                #expect(h != 0)
                #expect(ctb(0x0217, [.l(h), .l(cstr(config))]) == 0)
                return h
            }
            let sender = new(0), receiver = new(1)
            #expect(ctb(0x0225, [.l(receiver), .l(spec(down)), .l(0)]) == 0) // FTReceive
            let one = spec(up)
            #expect(ctb(0x0224, [.l(sender), .w(1), .l(one), .l(0)]) == 0) // FTSend
            func busy(_ h: UInt32) -> Bool { rd32(rd32(h) + 2) & 1 != 0 } // ftIsFTMode
            for _ in 0..<5000 where busy(sender) || busy(receiver) {
                ctb(0x020A, [.l(sender)])
                ctb(0x020A, [.l(receiver)])
                if tool == "Text" && !busy(sender) && pipes[0].isEmpty {
                    ctb(0x020D, [.l(receiver)]) // FTAbort: the capture is done
                }
            }
            let errs = (Int16(bitPattern: rd16(rd32(sender) + 6)), Int16(bitPattern: rd16(rd32(receiver) + 6)))
            #expect(rd32(rd32(sender) + 2) & 0x80 != 0 && rd32(rd32(receiver) + 2) & 0x80 != 0) // ftSucc
            ctb(0x0209, [.l(sender)])
            ctb(0x0209, [.l(receiver)])
            fm_flush_dirty()
            let got = (try? Data(contentsOf: URL(fileURLWithPath: down))).map { [UInt8]($0) } ?? []
            return (errs.0, errs.1, got)
        }

        @Test func xmodemSendsAndReceives() {
            let dir = launchTestApp()
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            var seed: UInt32 = 12345
            let payload = (0..<3000).map { _ -> UInt8 in
                seed = seed &* 1103515245 &+ 12345
                return UInt8(truncatingIfNeeded: seed >> 16) | 1 // never SUB at the end
            }
            for method in ["Checksum", "CRC", "1K"] {
                let (s, r, got) = transfer(tool: "XMODEM Tool", config: "Method \(method)", payload: payload, dir: dir)
                #expect(s == 0 && r == 0, "\(method)")
                #expect(got == payload, "\(method): \(got.count) bytes")
            }
            let text = Array("Some text\rfor the Text tool.\r".utf8)
            let (s, r, got) = transfer(tool: "Text", config: "Timeout 0", payload: text, dir: dir)
            #expect(s == 0 && r == 0)
            #expect(got == text)
        }
    }
}
