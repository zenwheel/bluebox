import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Carbon Scrap Manager on the desk scrap: refs, flavours and their
    /// flags, promises kept on demand, and text standing in for Unicode.
    @Suite struct CarbonScrap {
        nonisolated(unsafe) static var kept: [UInt32] = []

        /// pascal OSStatus (ScrapRef, ScrapFlavorType, void *userData): keeps
        /// a promise with four bytes.
        static let keeper: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)
            let type = cpu_pop32(c)
            let scrap = cpu_pop32(c)
            CarbonScrap.kept.append(type)
            let data = svcPtr(4)
            wr32(data, 0xCAFE_F00D)
            let e = svcCall("PutScrapFlavor", scrap, type, 0, 4, data)
            wr32(c.pointee.a.7, e)
            cpu_push32(c, ret)
        }

        @Test func flavoursAndPromises() {
            launchTestApp()
            let ref = svcPtr(4), flags = svcPtr(4), size = svcPtr(4), count = svcPtr(4)
            #expect(svcCall("ClearCurrentScrap") == 0)
            #expect(svcCall("GetCurrentScrap", ref) == 0)
            let scrap = rd32(ref)
            #expect(scrap != 0)

            // Text put, read back with its size and flags.
            let text = svcPStr("hello")
            #expect(svcCall("PutScrapFlavor", scrap, fourCC("TEXT"), 1, 5, text + 1) == 0)
            #expect(svcStatus(svcCall("PutScrapFlavor", scrap, fourCC("TEXT"), 0, 5, text + 1)) == -4989) // duplicate
            #expect(svcCall("GetScrapFlavorFlags", scrap, fourCC("TEXT"), flags) == 0 && rd32(flags) == 1)
            #expect(svcCall("GetScrapFlavorSize", scrap, fourCC("TEXT"), size) == 0 && rd32(size) == 5)
            let buf = svcPtr(16)
            wr32(size, 16)
            #expect(svcCall("GetScrapFlavorData", scrap, fourCC("TEXT"), size, buf) == 0)
            #expect(rd32(size) == 5 && String(decoding: svcBytes(buf, 5), as: UTF8.self) == "hello")
            // The classic call sees it too.
            #expect(tb_GetScrap(0, Int32(bitPattern: fourCC("TEXT")), svcPtr(4)) == 5)

            // Unicode text, translated from it.
            #expect(svcCall("GetScrapFlavorFlags", scrap, fourCC("utxt"), flags) == 0 && rd32(flags) == 2)
            wr32(size, 32)
            #expect(svcCall("GetScrapFlavorData", scrap, fourCC("utxt"), size, buf) == 0 && rd32(size) == 10)
            #expect(rd16(buf) == 0x68 && rd16(buf + 8) == 0x6F)
            #expect(svcStatus(svcCall("GetScrapFlavorFlags", scrap, fourCC("snd "), flags)) == -102)

            // A promise, kept when asked for.
            let keeper = svcUPP68k(Self.keeper, "ScrapKeeperTest", 0xFF0)
            #expect(svcCall("SetScrapPromiseKeeper", scrap, keeper, 0) == 0)
            #expect(svcCall("PutScrapFlavor", scrap, fourCC("PICT"), 0, 4, 0) == 0)
            #expect(svcCall("GetScrapFlavorCount", scrap, count) == 0 && rd32(count) == 3) // TEXT, PICT, utxt
            Self.kept = []
            wr32(size, 4)
            #expect(svcCall("GetScrapFlavorData", scrap, fourCC("PICT"), size, buf) == 0)
            #expect(Self.kept == [fourCC("PICT")] && rd32(buf) == 0xCAFE_F00D)
            let info = svcPtr(64)
            wr32(count, 8)
            #expect(svcCall("GetScrapFlavorInfoList", scrap, count, info) == 0 && rd32(count) == 3)
            #expect(rd32(info) == fourCC("TEXT") && rd32(info + 4) == 1 && rd32(info + 8) == fourCC("PICT"))

            // Cleared: the old ref is no good.
            #expect(svcCall("ClearCurrentScrap") == 0)
            #expect(svcStatus(svcCall("GetScrapFlavorFlags", scrap, fourCC("TEXT"), flags)) == -4990)
            #expect(svcCall("GetCurrentScrap", ref) == 0 && rd32(ref) != scrap)

            // Unicode alone brings plain text with it.
            let (u, n) = svcUTF16("caf\u{e9}")
            #expect(svcCall("PutScrapFlavor", rd32(ref), fourCC("utxt"), 0, 2 * n, u) == 0)
            #expect(tb_GetScrap(0, Int32(bitPattern: fourCC("TEXT")), svcPtr(4)) == 4)
            #expect(svcStatus(svcCall("PutScrapFlavor", rd32(ref), fourCC("srft"), 0, 1, u)) == -4998)
        }
    }
}
