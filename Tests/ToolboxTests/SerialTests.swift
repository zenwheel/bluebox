import CCore
import Darwin
import Testing
import Toolbox

extension BlueBox {
    /// The Serial Driver, with the modem port as a Hayes modem on TCP.
    @Suite struct Serial {
        /// Opens a driver by name; returns its reference number.
        func open(_ name: String) -> Int16 {
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true), np = tb_NewPtr(8, false, true)
            guestPString(name, at: np)
            wr32(pb + UInt32(IOParam_ioNamePtr_off), np)
            #expect(tb_PBOpen(pb, false) == 0)
            return Int16(bitPattern: rd16(pb + UInt32(IOParam_ioRefNum_off)))
        }

        func write(_ ref: Int16, _ s: String) {
            let bytes = Array(s.utf8)
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true), buf = tb_NewPtr(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(buf + UInt32(i), b) }
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: ref))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), UInt32(bytes.count))
            #expect(tb_PBWrite(pb, false) == 0)
        }

        /// SerGetBuf, then a read of that many bytes.
        func available(_ ref: Int16) -> String {
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

        /// Polls until the text arrives (or a second passes).
        func waitFor(_ ref: Int16, _ text: String) -> String {
            var got = ""
            for _ in 0..<100 where !got.contains(text) {
                usleep(10000)
                serial_poll()
                got += available(ref)
            }
            return got
        }

        @Test func modemDialsAndHangsUp() {
            #expect(machine_set_serial(0, "modem"))
            launchTestApp()
            let input = open(".AIn"), output = open(".AOut")
            #expect(input == -6 && output == -7)

            write(output, "AT\r")
            #expect(available(input) == "AT\r\r\nOK\r\n")
            write(output, "ATE0Q0V0\r")
            #expect(available(input).hasSuffix("0\r"))
            write(output, "ATV1\r")
            _ = available(input)

            let lfd = socket(AF_INET, SOCK_STREAM, 0)
            defer { close(lfd) }
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
            write(output, "ATDT127.0.0.1:\(UInt16(bigEndian: sa.sin_port))\r")
            #expect(waitFor(input, "CONNECT").contains("CONNECT"))
            let conn = accept(lfd, nil, nil)
            #expect(conn >= 0)
            _ = "login: ".withCString { send(conn, $0, 7, 0) }
            #expect(waitFor(input, "login: ") == "login: ")
            write(output, "guest\r")
            var got = [UInt8](repeating: 0, count: 16)
            #expect(recv(conn, &got, 16, 0) == 6)
            #expect(String(decoding: got.prefix(6), as: UTF8.self) == "guest\r")

            close(conn)
            #expect(waitFor(input, "NO CARRIER").contains("NO CARRIER"))
            write(output, "ATDT\r") // nothing to dial
            #expect(available(input).contains("ERROR"))
        }

        /// PortAUse/PortBUse say which port is open; handshaking, break and
        /// DTR reach a port with control lines (a pty here).
        @Test func portUseAndHandshake() {
            #expect(machine_set_serial(1, "pty"))
            defer { _ = machine_set_serial(1, "none") }
            launchTestApp()
            #expect(rd8(0x290) == 0xFF && rd8(UInt32(LM_PortBUse)) == 0xFF)
            let input = open(".BIn")
            #expect(rd8(UInt32(LM_PortBUse)) == 2 && rd8(0x290) == 0xFF) // useAsync
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true), cs = pb + UInt32(CntrlParam_csParam_off)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: input))
            for (code, bytes) in [(14, [1, 1, 0x11, 0x13, 0, 0, 1, 1]), (12, []), (11, []), (18, []), (17, [])] as [(UInt16, [UInt8])] {
                for (i, b) in bytes.enumerated() { wr8(cs + UInt32(i), b) }
                wr16(pb + UInt32(CntrlParam_csCode_off), code)
                #expect(tb_PBControl(pb, false) == 0, "csCode \(code)")
            }
            wr16(pb + UInt32(CntrlParam_csCode_off), 9)
            #expect(tb_PBStatus(pb, false) == 0)
            #expect(tb_PBClose(pb, false) == 0)
            #expect(rd8(UInt32(LM_PortBUse)) == 0xFF)
        }

        /// The Communications Resource Manager lists both ports by name and driver.
        @Test func resourceManagerListsThePorts() {
            launchTestApp()
            let req = tb_NewPtr(Int32(CRMRec_SIZEOF), false, true)
            wr32(req + UInt32(CRMRec_crmDeviceType_off), 1)
            var names = [String]()
            while true {
                let rec = serial_crm_search(req)
                if rec == 0 { break }
                let ser = rd32(rec + UInt32(CRMRec_crmAttributes_off))
                names.append([10, 2, 6].map { pstring(at: rd32(rd32(ser + $0))) }.joined(separator: " "))
                wr32(req + UInt32(CRMRec_crmDeviceID_off), rd32(rec + UInt32(CRMRec_crmDeviceID_off)))
            }
            #expect(names == ["Modem Port .AIn .AOut", "Printer Port .BIn .BOut"])
        }
    }
}
