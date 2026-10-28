import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Network Services Location manager: its helpers, its requests, and a search with no plug-ins.
    @Suite struct CarbonNSL {
        func cstr(_ s: String) -> UInt32 {
            let p = svcPtr(s.utf8.count + 2)
            for (i, b) in Array(s.utf8).enumerated() { wr8(p + UInt32(i), b) }
            return p
        }

        func text(_ p: UInt32, _ n: Int) -> String { String(decoding: svcBytes(p, n), as: UTF8.self) }

        /// An NSLError-returning call: the result block goes first.
        func nslError(_ name: String, _ args: UInt32...) -> Int32 {
            let out = svcPtr(8)
            svcCallArray(name, [out] + args)
            return svcStatus(rd32(out))
        }

        @Test func hexEncodingAndDecoding() {
            bootMachine()
            let raw = cstr("a b,c%d"), out = svcPtr(64), len = svcPtr(2), changed = svcPtr(1)
            wr16(len, 64)
            #expect(svcCall("NSLHexEncodeText", raw, 7, out, len, changed) == 0)
            #expect(text(out, Int(rd16(len))) == "a%20b%2Cc%25d" && rd8(changed) == 1)
            let back = svcPtr(64)
            wr16(len, 64)
            let enc = cstr("a%20b%2Cc%25d")
            #expect(svcCall("NSLHexDecodeText", enc, 13, back, len, changed) == 0)
            #expect(text(back, Int(rd16(len))) == "a b,c%d" && rd8(changed) == 1)
            // Nothing to change
            wr16(len, 64)
            #expect(svcCall("NSLHexEncodeText", cstr("plain"), 5, out, len, changed) == 0 && rd8(changed) == 0 && rd16(len) == 5)
            // Too small a buffer
            wr16(len, 3)
            #expect(svcStatus(svcCall("NSLHexEncodeText", raw, 7, out, len, changed)) == -4187)
            // A stray percent is left alone
            wr16(len, 64)
            #expect(svcCall("NSLHexDecodeText", cstr("50%"), 3, back, len, changed) == 0 && text(back, 3) == "50%" && rd8(changed) == 0)
        }

        @Test func servicesListsAndRequests() {
            bootMachine()
            let list = svcCall("NSLMakeNewServicesList", cstr("afp,ftp"))
            #expect(list != 0 && rd32(rd32(list)) == 2)
            #expect(nslError("NSLAddServiceToServicesList", list, cstr("http")) == 0)
            #expect(rd32(rd32(list)) == 3)
            #expect(svcCall("NSLServiceIsInServiceList", list, cstr("FTP")) == 1)
            #expect(svcCall("NSLServiceIsInServiceList", list, cstr("ft")) == 0)
            #expect(svcCall("NSLServiceIsInServiceList", list, cstr("http")) == 1)
            #expect(nslError("NSLAddServiceToServicesList", 0, cstr("x")) == -4184)
            let req = svcPtr(4)
            #expect(svcCall("NSLMakeServicesRequestPB", list, req) == 0)
            let typed = rd32(req)
            let sp = svcPtr(4), sl = svcPtr(2)
            #expect(svcCall("NSLParseServicesRequestPB", typed, sp, sl) == 0)
            #expect(text(rd32(sp), Int(rd16(sl))) == "afp,ftp,http")
            wr32(typed, 99)
            #expect(svcStatus(svcCall("NSLParseServicesRequestPB", typed, sp, sl)) == -4193)
            #expect(svcCall("NSLFreeTypedDataPtr", typed) == 0)
            svcCall("NSLDisposeServicesList", list)
            let empty = svcCall("NSLMakeNewServicesList", 0)
            #expect(rd32(rd32(empty)) == 0)
        }

        @Test func neighborhoodsAndURLs() {
            bootMachine()
            let n = svcCall("NSLMakeNewNeighborhood", cstr("Engineering"), cstr("atlk,slp"))
            #expect(n != 0 && svcCall("NSLGetNeighborhoodLength", n) == 2 + 11 + 8)
            let name = svcPtr(4), len = svcPtr(4)
            svcCall("NSLGetNameFromNeighborhood", n, name, len)
            #expect(text(rd32(name), Int(rd32(len))) == "Engineering")
            let copy = svcCall("NSLCopyNeighborhood", n)
            #expect(copy != n && svcBytes(copy, 21) == svcBytes(n, 21))
            #expect(svcCall("NSLFreeNeighborhood", copy) == 0)
            let sv = svcPtr(4), sl = svcPtr(2)
            #expect(svcCall("NSLGetServiceFromURL", cstr("afp://at/Server:AFPServer@Zone"), sv, sl) == 0)
            #expect(text(rd32(sv), Int(rd16(sl))) == "afp")
            #expect(svcStatus(svcCall("NSLGetServiceFromURL", cstr("nonsense"), sv, sl)) == -4172)
        }

        @Test func requestsAndSearchesWithoutPlugins() {
            bootMachine()
            let client = svcPtr(4)
            #expect(svcCall("NSLOpenNavigationAPI", client) == 0 && rd32(client) != 0)
            let buf = svcPtr(256), ref = svcPtr(4), info = svcPtr(4)
            #expect(nslError("NSLPrepareRequest", 0, 0, rd32(client), ref, buf, 256, info) == 0)
            let infoPtr = rd32(info)
            #expect(rd32(infoPtr + 8) == buf && rd32(infoPtr + 16) == 256 && rd16(infoPtr + 44) == 3)
            #expect(nslError("NSLPrepareRequest", 0, 0, 12345, ref, buf, 256, info) == -4195)
            // A lookup with no plug-in
            let n = svcCall("NSLMakeNewNeighborhood", cstr("Zone"), cstr("atlk"))
            let list = svcCall("NSLMakeNewServicesList", cstr("afp"))
            let req = svcPtr(4)
            #expect(svcCall("NSLMakeServicesRequestPB", list, req) == 0)
            #expect(nslError("NSLStartServicesLookup", rd32(ref), n, rd32(req), infoPtr) == -4179)
            #expect(svcStatus(rd32(infoPtr + 46)) == -4179)
            #expect(nslError("NSLStartServicesLookup", rd32(ref), 0, rd32(req), infoPtr) == -4178)
            #expect(nslError("NSLStartNeighborhoodLookup", rd32(ref), 0, infoPtr) == -4179)
            #expect(nslError("NSLStartNeighborhoodLookup", 77, 0, infoPtr) == -4195)
            #expect(nslError("NSLContinueLookup", infoPtr) == -4186)
            #expect(nslError("NSLCancelRequest", rd32(ref)) == 0)
            // Results a plug-in left: URLs, then neighborhoods
            let urls = "afp://a,afp://bb,x"
            for (i, b) in Array(urls.utf8).enumerated() { wr8(buf + UInt32(i), b) }
            wr32(infoPtr + 12, UInt32(urls.utf8.count))
            wr32(infoPtr + 4, 0)
            let up = svcPtr(4), ul = svcPtr(4)
            var found: [String] = []
            while svcCall("NSLGetNextUrl", infoPtr, up, ul) == 1 { found.append(text(rd32(up), Int(rd32(ul)))) }
            #expect(found == ["afp://a", "afp://bb", "x"])
            let two = svcCall("NSLMakeNewNeighborhood", cstr("B"), cstr("slp"))
            for (i, b) in (svcBytes(n, Int(svcCall("NSLGetNeighborhoodLength", n))) + svcBytes(two, 6)).enumerated() { wr8(buf + UInt32(i), b) }
            wr32(infoPtr + 12, 16)
            wr32(infoPtr + 4, 0)
            var names: [String] = []
            while svcCall("NSLGetNextNeighborhood", infoPtr, up, ul) == 1 {
                let ptr = svcPtr(4), l = svcPtr(4)
                svcCall("NSLGetNameFromNeighborhood", rd32(up), ptr, l)
                names.append(text(rd32(ptr), Int(rd32(l))))
            }
            #expect(names == ["Zone", "B"])
            #expect(nslError("NSLDeleteRequest", rd32(ref)) == 0)
            #expect(nslError("NSLDeleteRequest", rd32(ref)) == -4195)
            svcCall("NSLCloseNavigationAPI", rd32(client))
            // Registration finds no plug-in; the standard dialog is canceled
            #expect(nslError("NSLStandardRegisterURL", cstr("afp://x"), n) == -4181)
            #expect(nslError("NSLStandardRegisterURL", cstr("afp://x"), 0) == -4178)
            let opts = svcPtr(1030), url = svcPtr(4)
            wr8(opts + 6, 9)
            #expect(svcCall("NSLGetDefaultDialogOptions", opts) == 0 && rd8(opts + 6) == 0)
            wr32(url, 5)
            #expect(svcStatus(svcCall("NSLStandardGetURL", opts, 0, 0, 0, cstr("afp"), url)) == -128 && rd32(url) == 0)
            #expect(svcCall("NSLFreeURL", 0) == 0)
        }

        @Test func errorStrings() {
            bootMachine()
            let e = svcPtr(128), s = svcPtr(128)
            #expect(svcCall("NSLErrorToString", UInt32(bitPattern: -4179), 0, e, s) == 0)
            #expect(text(e, 20).hasPrefix("No network service"))
            #expect(svcCall("NSLErrorToString", UInt32(bitPattern: -1), 0, e, s) == 0)
            #expect(text(e, 11) == "An unknown ")
            #expect(svcStatus(svcCall("NSLErrorToString", 0, 0, 0, s)) == -4176)
            #expect(svcCall("NSLLibraryVersion") != 0)
        }

        @Test func urlAliasFiles() {
            launchTestApp()
            let fileName = "Example " + UUID().uuidString.prefix(8) // the Desktop Folder outlives a run
            let e = svcStatus(svcCall("NSLSaveURLAliasToFolder", fourCC("desk"), cstr("http://example.org/a"), cstr(fileName)))
            #expect(e == 0)
            let spec = svcPtr(70), info = svcPtr(16)
            let vref = svcPtr(2), dir = svcPtr(4)
            #expect(tb_FindFolder(-1, Int32(bitPattern: fourCC("desk")), 0, vref, dir) == 0)
            #expect(tb_FSMakeFSSpec(Int16(bitPattern: rd16(vref)), Int32(bitPattern: rd32(dir)), svcPStr(fileName), spec) == 0)
            #expect(tb_FSpGetFInfo(spec, info) == 0 && rd32(info) == fourCC("ilht") && rd32(info + 4) == fourCC("MACS"))
            #expect(svcStatus(svcCall("NSLSaveURLAliasToFolder", fourCC("desk"), cstr("http://example.org/a"), cstr(fileName))) == -48)
            #expect(svcStatus(svcCall("NSLSaveURLAliasToFolder", fourCC("desk"), cstr("nonsense"), cstr("X"))) == -4172)
            #expect(svcStatus(svcCall("NSLSaveURLAliasToFolder", fourCC("desk"), 0, cstr("X"))) == -4176)
        }
    }
}
