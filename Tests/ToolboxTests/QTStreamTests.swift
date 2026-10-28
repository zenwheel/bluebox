import CCore
import CPUTestSupport
import Foundation
import Testing
import Toolbox

extension BlueBox {
    @Suite struct QTStream {
        static let sdp = """
            v=0\r
            o=- 1 1 IN IP4 127.0.0.1\r
            s=Blue Box test\r
            t=0 0\r
            a=control:*\r
            a=range:npt=0-10.000\r
            m=audio 0 RTP/AVP 11\r
            a=control:trackID=1\r
            m=video 0 RTP/AVP 96\r
            a=rtpmap:96 X-QT/600\r
            a=x-dimensions:4,4\r
            a=control:trackID=2\r

            """

        func rtp(_ pt: UInt8, marker: Bool, seq: UInt16, ts: UInt32, _ payload: [UInt8]) -> [UInt8] {
            var p: [UInt8] = [0x80, pt | (marker ? 0x80 : 0), UInt8(seq >> 8), UInt8(seq & 0xFF)]
            p += [UInt8(ts >> 24), UInt8((ts >> 16) & 0xFF), UInt8((ts >> 8) & 0xFF), UInt8(ts & 0xFF)]
            p += [0x12, 0x34, 0x56, 0x78]
            return p + payload
        }

        func be16(_ v: Int) -> [UInt8] { [UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }
        func be32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 0xFF), UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }

        /// A 4 by 4 picture in QuickTime's own payload: its image description
        /// ('raw ', 32 bits) in the payload description, then the sample.
        func xqtPicture() -> [UInt8] {
            var desc = [UInt8](repeating: 0, count: 86)
            desc.replaceSubrange(0..<4, with: be32(86))
            desc.replaceSubrange(4..<8, with: be32(fourCC("raw ")))
            desc[15] = 1
            desc.replaceSubrange(32..<34, with: be16(4))
            desc.replaceSubrange(34..<36, with: be16(4))
            desc.replaceSubrange(82..<84, with: be16(32))
            desc.replaceSubrange(84..<86, with: be16(0xFFFF))
            let tlv = be16(4 + desc.count) + Array("sd".utf8) + desc
            let dlen = 12 + tlv.count
            var pd: [UInt8] = [0x80, 0] + be16(dlen) + be32(fourCC("vide")) + be32(600) + tlv
            while pd.count % 4 != 0 { pd.append(0) }
            var sample: [UInt8] = []
            for _ in 0..<16 { sample += [0, 0xFF, 0, 0] } // red
            return [0x0A, 0, 0, 1] + pd + sample
        }

        func interleaved(_ ch: UInt8, _ p: [UInt8]) -> [UInt8] { [0x24, ch] + be16(p.count) + p }

        func handle(_ b: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(b.count), false, false)
            for (i, v) in b.enumerated() { wr8(rd32(h) + UInt32(i), v) }
            return h
        }

        func urlRef(_ s: String) -> UInt32 { handle(Array(s.utf8) + [0]) }

        func frames() -> [UInt8] {
            let audio = rtp(11, marker: true, seq: 1, ts: 0, [UInt8](repeating: 0x10, count: 200))
            let video = rtp(96, marker: true, seq: 1, ts: 0, xqtPicture())
            return interleaved(0, audio) + interleaved(2, video)
        }

        /// Session descriptions: media, payloads (static and mapped),
        /// dimensions, controls made absolute and the range.
        @Test func sessionDescriptions() {
            var d = qts_sdp()
            let text = Self.sdp
            #expect(qts_sdp_parse(text, text.utf8.count, "rtsp://host/movie.mov/", &d))
            #expect(d.nmedia == 2 && d.range_end == 10)
            #expect(withUnsafeBytes(of: d.control) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
                    == "rtsp://host/movie.mov/")
            let a = d.m.0, v = d.m.1
            #expect(a.media_type == fourCC("soun") && a.pt == 11 && a.clock == 44100 && a.channels == 1)
            #expect(withUnsafeBytes(of: a.encoding) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) } == "L16")
            #expect(v.media_type == fourCC("vide") && v.clock == 600 && v.width == 4 && v.height == 4)
            #expect(withUnsafeBytes(of: v.control) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
                    == "rtsp://host/movie.mov/trackID=2")
            #expect(qts_depay_supported(&d.m.0) && qts_depay_supported(&d.m.1))
        }

        /// RFC 2435's headers: quantization tables, the frame's size and
        /// the scan after them.
        @Test func jpegHeaders() {
            let scan: [UInt8] = [1, 2, 3]
            var n = 0
            let j = qts_jpeg_frame(1, 50, 64, 48, nil, 0, 0, scan, scan.count, &n)!
            let b = Array(UnsafeBufferPointer(start: j, count: n))
            free(j)
            #expect(b[0] == 0xFF && b[1] == 0xD8 && b[n - 2] == 0xFF && b[n - 1] == 0xD9)
            let sof = (0..<n - 1).first { b[$0] == 0xFF && b[$0 + 1] == 0xC0 }!
            #expect(Int(b[sof + 5]) << 8 | Int(b[sof + 6]) == 48 && Int(b[sof + 7]) << 8 | Int(b[sof + 8]) == 64)
            #expect(b[sof + 11] == 0x22) // 4:2:0
            #expect(Array(b[n - 5..<n - 2]) == scan)
        }

        /// A presentation from an rtsp:// URL: described, set up, played,
        /// its picture drawn into the GWorld given and its statistics kept;
        /// stopping pauses and disposing tears it down.
        @Test func rtspPresentationPlays() throws {
            qtMachine()
            let port = rtsp_server_start(Self.sdp, frames(), frames().count)
            #expect(port > 0)
            defer { rtsp_server_stop() }
            let out = tb_NewPtr(4, false, true)
            #expect(qtCallErr("InitializeQTS", []) == 0)
            #expect(qtCallErr("QTSNewPresentationFromDataRef", [urlRef("rtsp://127.0.0.1:\(port)/test.mov"), fourCC("url "), 0, out]) == 0)
            let pres = rd32(out)
            #expect(pres != 0)
            #expect(qtCall("QTSPresGetNumStreams", [pres]) == 2)
            let video = qtCall("QTSPresGetIndStream", [pres, 2])
            #expect(qtCall("QTSGetStreamPresentation", [video]) == pres)
            let w = tb_NewPtr(8, false, true)
            #expect(qtCallErr("QTSPresGetDimensions", [pres, video, w, w + 4]) == 0)
            #expect(rd32(w) == 4 << 16 && rd32(w + 4) == 4 << 16)
            let dur = tb_NewPtr(16, false, true)
            #expect(qtCallErr("QTSPresGetInfo", [pres, 0, fourCC("dura"), dur]) == 0)
            #expect(rd32(dur + 4) == 600 && rd32(dur + 12) == 6000)
            let gw = tb_NewPtr(4, false, true)
            let r = tb_NewPtr(8, false, true)
            wr16(r + 4, 8); wr16(r + 6, 8)
            #expect(tb_NewGWorld(gw, 32, r, 0, 0, 0) == 0)
            let port32 = rd32(gw)
            #expect(qtCallErr("QTSPresSetGWorld", [pres, 0, port32, 0]) == 0)
            #expect(qtCallErr("QTSPresPreroll", [pres, 0, 0, 0x10000, 0]) == 0)
            #expect(qtCallErr("QTSPresStart", [pres, 0, 0]) == 0)
            let rgb = tb_NewPtr(6, false, true)
            let saved = tb_NewPtr(8, false, true)
            var red = false
            for _ in 0..<60 where !red {
                qtCall("QTSPresIdle", [pres, 0])
                tb_GetGWorld(saved, saved + 4)
                tb_SetGWorld(port32, 0)
                tb_GetCPixel(1, 1, rgb)
                tb_SetGWorld(rd32(saved), rd32(saved + 4))
                red = rd16(rgb) == 0xFFFF && rd16(rgb + 2) == 0
                if !red { usleep(50_000) }
            }
            #expect(red)
            // statistics: the packets that came
            #expect(qtCallErr("QTSNewStatHelper", [pres, 0, fourCC("summ"), 0, out]) == 0)
            let helper = rd32(out)
            #expect(qtCallErr("QTSStatHelperGetStats", [helper]) == 0)
            #expect(qtCall("QTSStatHelperGetNumStats", [helper]) > 0)
            let next = tb_NewPtr(36, false, true), name = tb_NewPtr(64, false, true), value = tb_NewPtr(64, false, true)
            wr32(next + 12, 64); wr32(next + 16, name); wr32(next + 20, 64); wr32(next + 24, value)
            var packets = ""
            while qtCall("QTSStatHelperNext", [helper, next]) & 0xFF != 0 {
                if rd32(next + 4) == fourCC("rpkt") && rd32(next + 8) == 0 { packets = guestCString(value) }
            }
            #expect(packets == "2")
            #expect(qtCallErr("QTSDisposeStatHelper", [helper]) == 0)
            #expect(qtCallErr("QTSPresStop", [pres, 0, 0]) == 0)
            #expect(qtCallErr("QTSDisposePresentation", [pres, 0]) == 0)
            usleep(200_000)
            var m = [CChar](repeating: 0, count: 256)
            rtsp_server_methods(&m, m.count)
            #expect(String(cString: m) == "DESCRIBE SETUP SETUP PLAY PAUSE TEARDOWN")
            tb_DisposeGWorld(port32)
        }

        /// NewMovieFromDataRef with an rtsp:// URL: a movie of one streaming
        /// track as long as the presentation; starting it plays the stream.
        @Test func rtspMovie() {
            qtMachine()
            let port = rtsp_server_start(Self.sdp, frames(), frames().count)
            defer { rtsp_server_stop() }
            let out = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromDataRef(out, 0, 0, Int32(bitPattern: urlRef("rtsp://127.0.0.1:\(port)/test.mov")),
                                           Int32(bitPattern: fourCC("url "))) == 0)
            let movie = rd32(out)
            #expect(movie != 0)
            #expect(tb_GetMovieTrackCount(Int32(bitPattern: movie)) == 1)
            #expect(tb_GetMovieDuration(Int32(bitPattern: movie)) == 6000)
            tb_StartMovie(movie)
            tb_MoviesTask(movie, 0)
            tb_DisposeMovie(movie)
            tb_MoviesTask(0, 0)
            usleep(200_000)
            var m = [CChar](repeating: 0, count: 256)
            rtsp_server_methods(&m, m.count)
            #expect(String(cString: m).hasPrefix("DESCRIBE SETUP SETUP PLAY"))
        }

        /// A server that isn't there: the documented failure, not a presentation.
        @Test func connectionRefused() {
            qtMachine()
            let out = tb_NewPtr(4, false, true)
            #expect(qtCallErr("QTSNewPresentationFromDataRef", [urlRef("rtsp://127.0.0.1:1/none"), fourCC("url "), 0, out]) == -5420)
            #expect(rd32(out) == 0)
            #expect(qtCallErr("QTSNewPresentation", [0, out]) == -50)
        }

        /// Messages: buffers in a chain, duplicates sharing data, copies and
        /// flattening, lengths, and the data's place.
        @Test func messages() {
            qtMachine()
            let out = tb_NewPtr(4, false, true)
            #expect(qtCallErr("QTSNewStreamBuffer", [16, 0, out]) == 0)
            let a = rd32(out)
            let b = qtCall("QTSAllocBuffer", [8])
            for (buf, n) in [(a, UInt32(5)), (b, UInt32(3))] {
                let start = tb_NewPtr(4, false, true), max = tb_NewPtr(4, false, true)
                qtCall("QTSStreamBufferDataInfo", [buf, start, max])
                #expect(rd32(max) == (buf == a ? 16 : 8))
                for i in 0..<n { wr8(rd32(start) + i, UInt8(0x41 + i)) }
                wr32(buf + 16, rd32(buf + 12) + n) // wptr
            }
            wr32(a + 8, b) // next
            #expect(qtCall("QTSMessageLength", [a]) == 8)
            let dup = qtCall("QTSDupMessage", [a])
            #expect(rd32(dup + 12) == rd32(a + 12)) // shares the bytes
            let copy = qtCall("QTSCopyMessage", [a])
            #expect(rd32(copy + 12) != rd32(a + 12) && qtCall("QTSMessageLength", [copy]) == 8)
            #expect(qtCallErr("QTSDuplicateMessage", [a, 2, out]) == 0)
            let flat = rd32(out)
            #expect(rd32(flat + 8) == 0 && qtCall("QTSMessageLength", [flat]) == 8)
            #expect(rd8(rd32(flat + 12) + 5) == 0x41)
            let flattened = qtCall("QTSFlattenMessage", [copy])
            #expect(rd32(flattened + 8) == 0 && qtCall("QTSMessageLength", [flattened]) == 8)
            for m in [a, dup, flat, flattened] { qtCall("QTSFreeMessage", [m]) }
            let p = qtCall("QTSNewPtr", [32, 1, out])
            #expect(p != 0 && rd32(out) == 0 && rd32(p + 28) == 0)
            let h = qtCall("QTSNewHandle", [32, 0x11, out])
            #expect(h != 0 && rd32(out) == 2)
            let mp = qtCall("QTSAllocMemPtr", [64, 1])
            #expect(mp != 0)
            qtCall("QTSReleaseMemPtr", [mp, 0])
        }

        /// Preferences, the network name and error strings.
        @Test func prefsAndStrings() {
            qtMachine()
            let info = tb_NewPtr(16, false, true), out = tb_NewPtr(4, false, true), count = tb_NewPtr(2, false, true)
            #expect(qtCallErr("QTSPrefsGetActiveConnection", [fourCC("rtsp"), info]) == 0)
            #expect(rd32(info) == fourCC("tcp ") && rd32(info + 4) == 554)
            #expect(qtCallErr("QTSPrefsAddConnectionSetting", [fourCC("udp "), 7000, 0, 5]) == 0)
            #expect(qtCallErr("QTSPrefsFindConnectionByType", [fourCC("udp "), 0, 0, out, count]) == 0)
            #expect(rd16(count) == 1 && rd32(rd32(rd32(out)) + 4) == 7000)
            #expect(qtCallErr("QTSPrefsFindProxyByType", [fourCC("http"), 0, 0, out, count]) == -5407)
            let urls = tb_NewPtr(32, false, true)
            for (i, ch) in "a.com,b.com".utf8.enumerated() { wr8(urls + UInt32(i), ch) }
            #expect(qtCallErr("QTSPrefsSetNoProxyURLs", [urls, 0, 3]) == 0)
            #expect(qtCallErr("QTSPrefsGetNoProxyURLs", [out]) == 0)
            #expect(guestCString(rd32(rd32(out)) + 8) == "a.com,b.com")
            let name = tb_NewPtr(16, false, true)
            for (i, ch) in "Player".utf8.enumerated() { wr8(name + UInt32(i), ch) }
            #expect(qtCallErr("QTSSetNetworkAppName", [name, 0]) == 0)
            #expect(qtCallErr("QTSGetNetworkAppName", [0, out]) == 0 && guestCString(rd32(out)) == "Player")
            let text = tb_NewPtr(128, false, true)
            #expect(qtCall("QTSGetErrorString", [UInt32(bitPattern: -5408), 128, text, 0]) & 0xFF == 1)
            #expect(guestCString(text).contains("respond"))
            #expect(qtCall("QTSGetErrorString", [1234, 128, text, 0]) & 0xFF == 0)
            // no packetizers or reassemblers registered: none found
            #expect(qtCallErr("QTSFindReassemblerForPayloadName", [name, 0, out]) == -2003)
            #expect(qtCallErr("QTSPresSetPreferredRate", [0, 0x10000, 0]) == -50)
        }

        /// The DV isochronous data handler with no FireWire: an empty device
        /// list, and opening or reading fail as documented.
        @Test func dvHandlerWithoutDevices() {
            qtMachine()
            let idh = components_open_default(fourCC("ihlr"), fourCC("dv  "))
            #expect(idh != 0)
            let out = tb_NewPtr(8, false, true)
            #expect(qtCallErr("IDHGetDeviceList", [idh, out]) == 0)
            let list = rd32(out)
            let dlst = qtCall("QTFindChildByIndex", [list, 0, fourCC("dlst"), 1, 0])
            #expect(dlst != 0)
            #expect(qtCall("QTCountChildrenOfType", [list, dlst, fourCC("devc")]) == 0)
            #expect(qtCallErr("IDHOpenDevice", [idh, 1]) == -14109)
            #expect(qtCallErr("IDHRead", [idh, out]) == -14105)
            #expect(qtCallErr("IDHGetDeviceConfiguration", [idh, out]) == -14109)
            #expect(qtCallErr("IDHNewNotification", [idh, 0xFFFF_FFFF, 0, 0, out]) == 0)
            let note = rd32(out)
            #expect(qtCallErr("IDHNotifyMeWhen", [idh, note, 1]) == 0)
            #expect(qtCallErr("IDHDisposeNotification", [idh, note]) == 0)
            #expect(qtCallErr("IDHDisposeNotification", [idh, note]) == -50)
            components_close(idh)
        }

        func guestCString(_ p: UInt32) -> String {
            var b: [UInt8] = []
            var a = p
            while rd8(a) != 0 && b.count < 1024 { b.append(rd8(a)); a += 1 }
            return String(decoding: b, as: UTF8.self)
        }
    }
}
