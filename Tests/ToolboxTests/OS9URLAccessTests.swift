import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// URLAccessLib on a machine with no network service: references and
    /// properties work, transfers answer kURLAccessNotAvailableError.
    @Suite struct OS9URLAccess {
        init() { bootMachine() }

        func cstr(_ s: String) -> UInt32 {
            let p = tb_NewPtr(Int32(s.utf8.count + 1), false, true)
            for (i, b) in s.utf8.enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func err(_ name: String, _ args: [UInt32]) -> Int32 { Int32(bitPattern: ilCall(name, args)) }

        func ref(_ url: String) -> UInt32 {
            let out = tb_NewPtr(4, false, true)
            #expect(err("URLNewReference", [cstr(url), out]) == 0)
            return rd32(out)
        }

        @Test func versionAndURLs() {
            let v = tb_NewPtr(4, false, true)
            #expect(err("URLGetURLAccessVersion", [v]) == 0 && rd32(v) >> 24 == 2)
            let out = tb_NewPtr(4, false, true)
            #expect(err("URLNewReference", [cstr("no scheme here"), out]) == -30773)
            #expect(err("URLNewReference", [cstr("gopher://host/x"), out]) == -30774)
            let r = ref("http://example.org/pub/readme.txt")
            #expect(err("URLDisposeReference", [r]) == 0)
            #expect(err("URLDisposeReference", [r]) == -30770)
        }

        @Test func properties() {
            let r = ref("ftp://scott:secret@files.example.org:21/pub/readme.txt")
            let buf = tb_NewPtr(256, false, true), size = tb_NewPtr(4, false, true)
            func text(_ p: String) -> String? {
                guard err("URLGetProperty", [r, cstr(p), buf, 256]) == 0 else { return nil }
                var s = [UInt8]()
                var i: UInt32 = 0
                while rd8(buf + i) != 0 { s.append(rd8(buf + i)); i += 1 }
                return String(decoding: s, as: UTF8.self)
            }
            #expect(text("URLString") == "ftp://scott:secret@files.example.org:21/pub/readme.txt")
            #expect(text("URLHost") == "files.example.org")
            #expect(text("URLUserName") == "scott" && text("URLPassword") == "secret")
            #expect(text("URLResourceName") == "readme.txt")
            #expect(err("URLGetPropertySize", [r, cstr("URLHost"), size]) == 0 && rd32(size) == 18)
            #expect(err("URLGetProperty", [r, cstr("URLHost"), buf, 4]) == -30779) // too small
            #expect(err("URLGetProperty", [r, cstr("URLMIMEType"), buf, 256]) == -30777) // not yet known
            #expect(err("URLGetProperty", [r, cstr("URLNoSuch"), buf, 256]) == -30778)
            // Settable ones
            #expect(err("URLSetProperty", [r, cstr("URLUserName"), cstr("anonymous"), 10]) == 0)
            #expect(text("URLUserName") == "anonymous")
            let http = ref("http://example.org/")
            #expect(err("URLSetProperty", [http, cstr("URLHTTPRequestMethod"), cstr("POST"), 5]) == 0)
            #expect(err("URLGetProperty", [http, cstr("URLHTTPRequestMethod"), buf, 256]) == 0 && rd8(buf) == 0x50)
            #expect(err("URLSetProperty", [r, cstr("URLString"), cstr("x"), 2]) == -30780) // can't be set
            #expect(err("URLSetProperty", [r, cstr("URLNoSuch"), cstr("x"), 2]) == -30778)
            #expect(err("URLGetProperty", [0x1234, cstr("URLHost"), buf, 256]) == -30770)
        }

        @Test func transfersAreNotAvailable() {
            let r = ref("http://example.org/file.sit")
            let st = tb_NewPtr(4, false, true), e = tb_NewPtr(4, false, true)
            #expect(err("URLGetCurrentState", [r, st]) == 0 && rd32(st) == 0)
            let spec = tb_NewPtr(80, false, true)
            #expect(err("URLOpen", [r, spec, 0, 0, 0, 0]) == -30787)
            #expect(err("URLGetCurrentState", [r, st]) == 0 && rd32(st) == 7) // kURLErrorOccurredState
            #expect(err("URLGetError", [r, e]) == 0 && Int32(bitPattern: rd32(e)) == -30787)
            #expect(err("URLDownload", [r, spec, 0, 0, 0, 0]) == -30787)
            #expect(err("URLDownload", [r, 0, 0, 0, 0, 0]) == -30781) // nowhere to put it
            #expect(err("URLUpload", [r, spec, 0, 0, 0]) == -30787)
            #expect(err("URLSimpleDownload", [cstr("ftp://example.org/x"), spec, 0, 0, 0, 0]) == -30787)
            #expect(err("URLSimpleDownload", [cstr("not a url"), spec, 0, 0, 0, 0]) == -30773)
            #expect(err("URLSimpleUpload", [cstr("http://example.org/x"), 0, 0, 0, 0]) == -30781)
            let sz = tb_NewPtr(4, false, true)
            #expect(err("URLGetDataAvailable", [r, sz]) == -30781)
            #expect(err("URLGetBuffer", [r, sz, sz]) == -30781)
            #expect(err("URLAbort", [r]) == 0 && err("URLGetCurrentState", [r, st]) == 0 && rd32(st) == 0)
            #expect(err("URLIdle", []) == 0)
            #expect(err("URLOpen", [0x99, spec, 0, 0, 0, 0]) == -30770)
        }

        @Test func fileInfoFromTheMappings() {
            launchTestApp()
            let name = tb_NewPtr(256, false, true), t = tb_NewPtr(8, false, true)
            guestPString("nothing.zzzzz", at: name)
            #expect(err("URLGetFileInfo", [name, t, t + 4]) == 0)
            #expect(rd32(t) == fourCC("????") && rd32(t + 4) == fourCC("????"))
        }
    }
}
