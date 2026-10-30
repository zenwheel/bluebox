import CCore
import Darwin
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Open Transport's raw streams (OTNativeClientLib) and configurations
    /// (OTNtvUtilLib's OTCfig* calls), called as a PowerPC caller would.
    @Suite struct OpenTransportStreams {
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

        /// A strbuf (maxlen, len, buf) over bytes, or room for maxlen of them.
        func strbuf(_ bytes: [UInt8]? = nil, room: Int = 64) -> UInt32 {
            let sb = block(12), buf = block(max(room, bytes?.count ?? 0, 1))
            wr32(sb, UInt32(room))
            wr32(sb + 4, bytes.map { UInt32($0.count) } ?? 0xFFFF_FFFF)
            wr32(sb + 8, buf)
            for (i, b) in (bytes ?? []).enumerated() { wr8(buf + UInt32(i), b) }
            return sb
        }

        func bytes(of sb: UInt32) -> [UInt8]? {
            let len = Int32(bitPattern: rd32(sb + 4))
            return len < 0 ? nil : (0..<UInt32(len)).map { rd8(rd32(sb + 8) + $0) }
        }

        func long(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255)] }

        func open(_ name: String) -> UInt32 {
            let e = block(4)
            let s = ot("OTStreamOpen", cstr(name), 0, e)
            #expect(rd32(e) == 0 && s != 0, "\(name)")
            return s
        }

        let I_NREAD: UInt32 = 0x4101, I_PUSH: UInt32 = 0x4102, I_POP: UInt32 = 0x4103, I_LOOK: UInt32 = 0x4104
        let I_STR: UInt32 = 0x4108, I_FIND: UInt32 = 0x410B, I_LIST: UInt32 = 0x4116

        @Test func echoCarriesMessagesBack() {
            launchTestApp()
            let s = open("echo")
            // A message with a control part and data comes back whole.
            #expect(ot("OTStreamPutMessage", s, strbuf([1, 2, 3]), strbuf([9, 8]), 0) == 0)
            let n = block(4)
            #expect(ot("OTStreamIoctl", s, I_NREAD, n) == 1 && rd32(n) == 2)
            let ctl = strbuf(), data = strbuf(), flags = block(4)
            wr32(flags, 0)
            #expect(ot("OTStreamGetMessage", s, ctl, data, flags) == 0)
            #expect(bytes(of: ctl) == [1, 2, 3] && bytes(of: data) == [9, 8] && rd32(flags) == 0)
            // Nothing waiting: a non-blocking stream says so.
            #expect(err(ot("OTStreamGetMessage", s, ctl, data, flags)) == -3210)
            // A control part bigger than the buffer: MORECTL, the rest next time.
            #expect(ot("OTStreamPutMessage", s, strbuf([1, 2, 3, 4, 5, 6]), 0, 0) == 0)
            let small = strbuf(room: 4)
            #expect(ot("OTStreamGetMessage", s, small, data, flags) == 1 && bytes(of: small) == [1, 2, 3, 4])
            #expect(ot("OTStreamGetMessage", s, small, data, flags) == 0 && bytes(of: small) == [5, 6])
            #expect(bytes(of: data) == nil)
            // A priority message goes ahead of an ordinary one.
            #expect(ot("OTStreamPutMessage", s, strbuf([7]), 0, 0) == 0)
            #expect(ot("OTStreamPutMessage", s, strbuf([8]), 0, 1) == 0) // RS_HIPRI
            #expect(ot("OTStreamGetMessage", s, ctl, data, flags) == 0 && bytes(of: ctl) == [8] && rd32(flags) == 1)
            // Bytes written are read as bytes, in pieces.
            let text = cstr("hello")
            #expect(ot("OTStreamWrite", s, text, 5) == 5)
            let buf = block(16)
            wr32(flags, 0)
            #expect(ot("OTStreamGetMessage", s, ctl, data, flags) == 0 && bytes(of: ctl) == [7]) // left from before
            #expect(ot("OTStreamRead", s, buf, 3) == 3 && cstring(at: buf, max: 3) == "hel")
            #expect(ot("OTStreamRead", s, buf, 16) == 2 && cstring(at: buf, max: 2) == "lo")
            // Polled: readable once something is there.
            let fds = block(12)
            wr16(fds + 4, 0x41); wr32(fds + 8, s) // POLLIN | POLLRDNORM
            #expect(ot("OTStreamPoll", fds, 1, 0) == 0 && rd16(fds + 6) == 0)
            _ = ot("OTStreamWrite", s, text, 1)
            #expect(ot("OTStreamPoll", fds, 1, 0) == 1 && rd16(fds + 6) == 0x41)
            #expect(ot("OTStreamClose", s) == 0)
            #expect(err(ot("OTStreamClose", s)) == -3208)
        }

        @Test func modulesAndDevices() {
            launchTestApp()
            let e = block(4)
            #expect(ot("OTStreamOpen", cstr("nosuch"), 0, e) == 0 && err(rd32(e)) == -3205) // ENXIO
            _ = open("/dev/nuls")
            let s = open("echo")
            let name = block(32)
            #expect(err(ot("OTStreamIoctl", s, I_LOOK, name)) == -3221)
            #expect(ot("OTStreamIoctl", s, I_PUSH, cstr("timod")) == 0)
            #expect(ot("OTStreamIoctl", s, I_LOOK, name) == 0 && cstring(at: name) == "timod")
            #expect(ot("OTStreamIoctl", s, I_FIND, cstr("timod")) == 1 && ot("OTStreamIoctl", s, I_FIND, cstr("tirdwr")) == 0)
            // A module Open Transport doesn't have can't be pushed.
            #expect(err(ot("OTStreamIoctl", s, I_PUSH, cstr("OTModl$Proxy"))) == -3221)
            #expect(ot("OTStreamIoctl", s, I_LIST, 0) == 2)
            let list = block(8), names = block(64)
            wr32(list, 2); wr32(list + 4, names)
            #expect(ot("OTStreamIoctl", s, I_LIST, list) == 0)
            #expect(cstring(at: names) == "timod" && cstring(at: names + 32) == "echo")
            #expect(ot("OTStreamIoctl", s, I_POP, 0) == 0 && err(ot("OTStreamIoctl", s, I_POP, 0)) == -3221)
            // An ioctl no driver knows.
            let sio = block(16)
            wr32(sio, 0x6B01)
            #expect(err(ot("OTStreamIoctl", s, I_STR, sio)) == -3221)
        }

        @Test func pipesAndTransports() {
            launchTestApp()
            let pair = block(8)
            #expect(ot("OTStreamPipe", pair) == 0)
            let a = rd32(pair), b = rd32(pair + 4)
            #expect(a != 0 && b != 0 && a != b)
            #expect(ot("OTStreamWrite", a, cstr("ping"), 4) == 4)
            let buf = block(8)
            #expect(ot("OTStreamRead", b, buf, 8) == 4 && cstring(at: buf, max: 4) == "ping")
            #expect(err(ot("OTStreamRead", a, buf, 8)) == -3210)
            _ = ot("OTStreamClose", b)
            #expect(err(ot("OTStreamWrite", a, buf, 1)) == -3231) // EPIPE: the other end has gone

            // A transport answers T_INFO_REQ, and says it supports no other request.
            let tcp = open("tcp")
            #expect(ot("OTStreamPutMessage", tcp, strbuf(long(107)), 0, 1) == 0)
            let ctl = strbuf(room: 64), flags = block(4)
            wr32(flags, 0)
            #expect(ot("OTStreamGetMessage", tcp, ctl, 0, flags) == 0)
            let ack = bytes(of: ctl) ?? []
            #expect(ack.count == 44 && ack[3] == 129 && rd32(flags) == 1) // T_INFO_ACK, priority
            #expect(ack[35] == 2) // SERV_type T_COTS_ORD
            #expect(ot("OTStreamPutMessage", tcp, strbuf(long(101) + long(0)), 0, 0) == 0) // T_BIND_REQ
            #expect(ot("OTStreamGetMessage", tcp, ctl, 0, flags) == 0)
            let nak = bytes(of: ctl) ?? []
            #expect(nak.count == 16 && nak[3] == 127 && nak[7] == 101 && nak[11] == 18) // TNOTSUPPORT
            // The stream becomes an endpoint of its transport.
            let e = block(4)
            let ep = ot("OTOpenEndpointOnStream", tcp, e)
            #expect(ep != 0 && rd32(e) == 0)
            let info = block(40)
            #expect(ot("OTGetEndpointInfo", ep, info) == 0 && rd32(info + 24) == 2)
            #expect(err(ot("OTStreamClose", tcp)) == -3208)
            #expect(ot("OTCloseProvider", ep) == 0)
        }

        @Test func namedParametersAndAutopush() {
            launchTestApp()
            let ip = open("ip")
            func nd(_ cmd: UInt32, _ text: [UInt8]) -> (Int32, String) {
                let buf = block(64), sio = block(16)
                for (i, b) in text.enumerated() { wr8(buf + UInt32(i), b) }
                wr32(sio, cmd); wr32(sio + 8, 64); wr32(sio + 12, buf)
                return (err(ot("OTStreamIoctl", ip, I_STR, sio)), cstring(at: buf))
            }
            let ttl = nd(0x6300, Array("ip_def_ttl".utf8) + [0])
            #expect(ttl.0 == 0 && (Int(ttl.1) ?? 0) > 0)
            #expect(nd(0x6301, Array("ip_forwarding".utf8) + [0] + Array("1".utf8) + [0]).0 == 0)
            #expect(nd(0x6300, Array("ip_forwarding".utf8) + [0]) == (0, "1"))
            #expect(nd(0x6300, Array("tcp_nothing".utf8) + [0]).0 == -3201)

            // The STREAMS administrative driver: modules pushed on a device's streams.
            let sad = open("sad")
            let info = block(48 + 8 * 32)
            func sio(_ cmd: UInt32) -> Int32 {
                let s = block(16)
                wr32(s, cmd); wr32(s + 8, UInt32(48 + 8 * 32)); wr32(s + 12, info)
                return err(ot("OTStreamIoctl", sad, I_STR, s))
            }
            wr32(info, 3) // kSAP_ALL
            for (i, b) in "echo".utf8.enumerated() { wr8(info + 4 + UInt32(i), b) }
            wr32(info + 44, 1)
            for (i, b) in "OTModl$Proxy".utf8.enumerated() { wr8(info + 48 + UInt32(i), b) }
            #expect(sio(0x6701) == -3221) // a module Open Transport can't load
            for i in 0..<32 { wr8(info + 48 + UInt32(i), 0) }
            for (i, b) in "timod".utf8.enumerated() { wr8(info + 48 + UInt32(i), b) }
            #expect(sio(0x6701) == 0)
            wr32(info + 44, 0)
            #expect(sio(0x6702) == 0 && rd32(info + 44) == 1 && cstring(at: info + 48) == "timod")
            let s = open("echo"), name = block(32)
            #expect(ot("OTStreamIoctl", s, I_LOOK, name) == 0 && cstring(at: name) == "timod")
            wr32(info, 4) // kSAP_CLEAR
            #expect(sio(0x6701) == 0 && sio(0x6702) == -3218)
        }

        @Test func readMessagesAsBuffers() {
            launchTestApp()
            let s = open("echo")
            #expect(ot("OTStreamPutMessage", s, strbuf([1, 2]), strbuf([3, 4, 5]), 0) == 0)
            let ri = block(20)
            #expect(ot("OTPeekMessage", s, ri) == 0 && rd32(ri) == 1 && rd32(ri + 12) == 5) // M_PROTO, 5 bytes
            let chain = ot("OTReadMessage", s, ri)
            #expect(chain != 0 && rd32(chain + 16) == 2 && rd8(chain + 25) == 1)
            let data = rd32(chain + 8)
            #expect(data != 0 && rd32(data + 16) == 3 && rd8(rd32(data + 12)) == 3)
            #expect(err(ot("OTPeekMessage", s, ri)) == -3162)
            ot("OTPutBackBuffer", s, chain)
            #expect(ot("OTPeekMessage", s, ri) == 0 && rd32(ri + 12) == 5)
        }

        @Test func configurationTrees() {
            launchTestApp()
            let top = ot("OTCfigNewConfiguration", cstr("tilisten,tcp(Opt=1)"))
            #expect(top != 0 && cstring(at: top) == "tilisten,tcp(Opt=1)")
            #expect(cstring(at: ot("OTCfigGetProviderName", top)) == "tilisten")
            #expect(ot("OTCfigNumberOfChildren", top) == 1)
            let tcp = ot("OTCfigGetChild", top, 0)
            #expect(cstring(at: ot("OTCfigGetProviderName", tcp)) == "tcp" && ot("OTCfigGetParent", tcp) == top)
            let nb = ot("OTCfigGetOptionNetbuf", tcp)
            #expect(rd32(nb + 4) == 5 && cstring(at: rd32(nb + 8), max: 5) == "Opt=1")
            // What is under tcp, as the configurators built it.
            #expect(ot("OTConfigureChildren", tcp) == 0)
            let ip = ot("OTCfigGetChild", tcp, 0)
            #expect(cstring(at: ot("OTCfigGetProviderName", ip)) == "ip")
            let port = ot("OTCfigGetChild", ip, 0)
            #expect(cstring(at: ot("OTCfigGetProviderName", port)) == "enet" && ot("OTCfigIsPort", port) == 1)
            #expect(ot("OTCfigGetPortRef", port) != 0 && ot("OTCfigIsPort", ip) == 0)
            // A module pushed above: the path the opens read has it first.
            let e = block(4)
            let mod = ot("OTCfigPushParent", top, cstr("pipemod"), e)
            #expect(mod != 0 && rd32(e) == 0 && cstring(at: mod) == "pipemod,tilisten,tcp(Opt=1)")
            #expect(ot("OTCfigGetParent", top) == mod)
            // Children replaced, popped and removed.
            let ddp = ot("OTCfigNewConfiguration", cstr("ddp"))
            #expect(ot("OTCfigPushNewSingleChild", ddp, cstr("enet"), e) != 0 && ot("OTCfigNumberOfChildren", ddp) == 1)
            #expect(ot("OTCfigPopChild", ddp, 0) == 0 && ot("OTCfigNumberOfChildren", ddp) == 0)
            let child = ot("OTCfigNewChild", ddp, cstr("ltlk"), e)
            #expect(child != 0 && ot("OTCfigRemoveChild", ddp, 0) == child && ot("OTCfigGetParent", child) == 0)
            #expect(ot("OTCfigAddChild", ddp, child) == 0 && ot("OTCfigGetChild", ddp, 0) == child)
            #expect(ot("OTCfigSetPath", child, cstr("enet(Port=2)")) == 0 && cstring(at: child) == "enet(Port=2)")
            // A copy is its own tree; a stream opens on the configuration's driver.
            let copy = ot("OTCfigCloneConfiguration", mod)
            #expect(copy != mod && cstring(at: copy) == cstring(at: mod))
            ot("OTCfigDeleteConfiguration", mod)
            let s = ot("OTCreateStream", ot("OTCreateConfiguration", cstr("udp")), 0, e)
            #expect(s != 0 && rd32(e) == 0)
            #expect(ot("OTStreamPutMessage", s, strbuf(long(107)), 0, 1) == 0)
            let ctl = strbuf(room: 64), flags = block(4)
            wr32(flags, 0)
            #expect(ot("OTStreamGetMessage", s, ctl, 0, flags) == 0 && (bytes(of: ctl) ?? [])[35] == 3) // T_CLTS
            // The special path for Internet services stays as it is.
            #expect(ot("OTCreateConfiguration", 0xFFFF_FFFD) == 0xFFFF_FFFD)
        }

        /// A program importing them finds Open Transport's native client, utility
        /// and configuration libraries.
        @Test func theLibrariesAreThere() {
            launchTestApp()
            let name = block(64), conn = block(4)
            for lib in ["OTNativeClientLib", "OTConfigLib", "OTNtvUtilLib"] {
                guestPString(lib, at: name)
                wr32(conn, 0)
                #expect(err(ot("GetSharedLibrary", name, fourCC("pwpc"), 1, conn, 0, 0)) == 0, "\(lib)")
                #expect(rd32(conn) != 0)
            }
        }
    }
}
