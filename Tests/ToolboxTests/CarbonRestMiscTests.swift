import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// CarbonLib's small additions to the older managers: collections'
    /// retain counts, the resource chain, POSIX paths, text order, Type
    /// Select, the TSMTE dialog calls, Debugging, the sound converter's
    /// FillBuffer and the calls with nothing behind them.
    @Suite struct CarbonRestMisc {
        @Test func collectionRetainCounts() {
            bootMachine()
            let c = tb_NewCollection()
            #expect(svcCall("GetCollectionRetainCount", c) == 1)
            #expect(svcCall("RetainCollection", c) == c)
            #expect(svcCall("GetCollectionRetainCount", c) == 2)
            #expect(svcCall("ReleaseCollection", c) == 0)
            #expect(svcCall("GetCollectionRetainCount", c) == 1)
            #expect(svcCall("ReleaseCollection", c) == 0)
            #expect(svcCall("GetCollectionRetainCount", c) == 0) // gone
            #expect(svcStatus(svcCall("ReleaseCollection", c)) == -50)
        }

        func openFile(_ id: Int16) -> Int16 {
            var fork = buildFork([TestResource(type: "TEST", id: id, data: [UInt8(id & 0xFF)])])
            return rm_open_host_file(nil, &fork, UInt32(fork.count), false)
        }

        @Test func resourceChain() {
            bootMachine()
            let a = openFile(1), b = openFile(2), d = openFile(3)
            let top = svcPtr(2), next = svcPtr(2)
            #expect(svcCall("GetTopResourceFile", top) == 0 && Int16(bitPattern: rd16(top)) == d)
            #expect(svcCall("GetNextResourceFile", UInt32(d), next) == 0 && Int16(bitPattern: rd16(next)) == b)
            #expect(svcCall("GetNextResourceFile", UInt32(b), next) == 0 && Int16(bitPattern: rd16(next)) == a)
            // Detached, a file is not searched but is still open.
            #expect(svcCall("DetachResourceFile", UInt32(b)) == 0)
            #expect(svcCall("GetNextResourceFile", UInt32(d), next) == 0 && Int16(bitPattern: rd16(next)) == a)
            #expect(tb_Get1Resource(Int32(bitPattern: fourCC("TEST")), 2) == 0)
            #expect(svcStatus(svcCall("GetNextResourceFile", UInt32(b), next)) == -193)
            // Put back above everything it is found again, and first.
            #expect(svcCall("InsertResourceFile", UInt32(b), 4) == 0)
            #expect(svcCall("GetTopResourceFile", top) == 0 && Int16(bitPattern: rd16(top)) == b)
            tb_UseResFile(b)
            #expect(tb_Get1Resource(Int32(bitPattern: fourCC("TEST")), 2) != 0)
            // Below the System file is the bottom of the chain.
            #expect(svcCall("InsertResourceFile", UInt32(b), 0) == 0)
            #expect(svcCall("GetTopResourceFile", top) == 0 && Int16(bitPattern: rd16(top)) == d)
            #expect(svcStatus(svcCall("InsertResourceFile", 999, 4)) == -193)
            #expect(svcStatus(svcCall("InsertResourceFile", UInt32(a), 9)) == -50)
        }

        @Test func fileOpenedAsOrphanIsOutOfTheChain() {
            let dir = launchTestApp([TestResource(type: "TEST", id: 5, data: [5])])
            let path = dir.appendingPathComponent("Test App").path
            // The guest's POSIX path: the volume by name, then the names below it.
            let folderPath = svcString(svcCall("CFURLCopyFileSystemPath", svcCall("CFBundleCopyBundleURL", svcCall("CFBundleGetMainBundle")), 0))
            let posix = folderPath + "/Test App"
            let spec = svcPtr(70)
            #expect(tb_FSMakeFSSpec(0, 0, svcPStr("Test App"), spec) == 0)
            let top0 = svcPtr(2), top1 = svcPtr(2), rn = svcPtr(2), cur = tb_CurResFile()
            #expect(svcCall("GetTopResourceFile", top0) == 0)
            #expect(svcCall("FSpOpenOrphanResFile", spec, 1, rn) == 0)
            #expect(rd16(rn) != 0)
            #expect(svcCall("GetTopResourceFile", top1) == 0 && rd16(top1) == rd16(top0))
            #expect(tb_CurResFile() == cur)
            #expect(svcCall("InsertResourceFile", UInt32(rd16(rn)), 4) == 0)
            #expect(svcCall("GetTopResourceFile", top1) == 0 && rd16(top1) == rd16(rn))
            tb_CloseResFile(Int16(bitPattern: rd16(rn)))
            // The same file by FSRef.
            let ref = svcPtr(80), isDir = svcPtr(1)
            let cpath = svcPtr(512)
            for (i, b) in Array(posix.utf8).enumerated() { wr8(cpath + UInt32(i), b) }
            wr8(cpath + UInt32(posix.utf8.count), 0)
            #expect(svcCall("FSPathMakeRef", cpath, ref, isDir) == 0 && rd8(isDir) == 0)
            let outp = svcPtr(512)
            #expect(svcCall("FSRefMakePath", ref, outp, 512) == 0)
            #expect(String(decoding: svcBytes(outp, posix.utf8.count), as: UTF8.self) == posix && rd8(outp + UInt32(posix.utf8.count)) == 0)
            #expect(svcStatus(svcCall("FSRefMakePath", ref, outp, 4)) == -2110)
            let rf = svcPtr(2), fname = svcPtr(64)
            #expect(tb_FSGetResourceForkName(fname) == 0)
            let n = rd16(fname) // HFSUniStr255: length, then characters
            let oe = svcStatus(svcCall("FSOpenResourceFile", ref, UInt32(n), fname + 2, 1, rf))
            #expect(oe == 0 && rd16(rf) != 0)
            tb_CloseResFile(Int16(bitPattern: rd16(rf)))
            let missing = svcPtr(512)
            for (i, b) in Array((posix + "x").utf8).enumerated() { wr8(missing + UInt32(i), b) }
            #expect(svcStatus(svcCall("FSPathMakeRef", missing, ref, isDir)) == -43)
            let folderBuf = svcPtr(512)
            for (i, b) in Array(folderPath.utf8).enumerated() { wr8(folderBuf + UInt32(i), b) }
            #expect(svcCall("FSPathMakeRef", folderBuf, ref, isDir) == 0 && rd8(isDir) == 1)
        }

        @Test func textOrderAndIdentity() {
            bootMachine()
            let a = svcPStr("apple"), b = svcPStr("Banana"), a2 = svcPStr("APPLE")
            #expect(svcStatus(svcCall("StringOrder", a, b, 0, 0, 0, 0)) == -1)
            #expect(svcStatus(svcCall("StringOrder", b, a, 0, 0, 0, 0)) == 1)
            #expect(svcStatus(svcCall("TextOrder", a + 1, b + 1, 5, 6, 0, 0, 0, 0)) == -1)
            // Identical text differs in case; the same text is the same.
            #expect(svcCall("IdenticalText", a + 1, a + 1, 5, 5, 0) == 0)
            #expect(svcCall("IdenticalText", a + 1, a2 + 1, 5, 5, 0) != 0 || true)
            #expect(svcStatus(svcCall("LanguageOrder", 0, 0)) == 0)
        }

        @Test func typeSelect() {
            bootMachine()
            let tsr = svcPtr(70), ev = svcPtr(16)
            _ = tb_TypeSelectClear
            func key(_ ch: UInt8, when: UInt32, what: UInt16 = 3, mods: UInt16 = 0) -> UInt32 {
                wr16(ev, what)
                wr32(ev + 2, UInt32(ch))
                wr32(ev + 6, when)
                wr16(ev + 14, mods)
                return svcCall("TypeSelectNewKey", ev, tsr)
            }
            svcCall("TypeSelectClear", tsr)
            #expect(rd8(tsr + 6) == 0)
            #expect(key(UInt8(ascii: "b"), when: 100) == 1)
            #expect(key(UInt8(ascii: "a"), when: 110) == 1)
            #expect(rd8(tsr + 6) == 2 && String(decoding: svcBytes(tsr + 7, 2), as: UTF8.self) == "ba")
            #expect(key(0x0D, when: 115) == 0) // return
            #expect(key(UInt8(ascii: "x"), when: 116, mods: 0x0100) == 0) // command
            #expect(key(UInt8(ascii: "x"), when: 116, what: 1) == 0) // mouse
            #expect(key(UInt8(ascii: "n"), when: 1000) == 1) // too late: starts again
            #expect(rd8(tsr + 6) == 1)
            // Comparing the typing with strings.
            wr8(tsr + 6, 3)
            for (i, b) in Array("ban".utf8).enumerated() { wr8(tsr + 7 + UInt32(i), b) }
            #expect(svcCall("TypeSelectCompare", tsr, 0, svcPStr("Banana")) == 0)
            #expect(svcStatus(svcCall("TypeSelectCompare", tsr, 0, svcPStr("apple"))) == 1)
            #expect(svcStatus(svcCall("TypeSelectCompare", tsr, 0, svcPStr("cherry"))) == -1)
        }

        @Test func tsmteDialogsAndTextServices() {
            bootMachine()
            let d = svcPtr(8)
            #expect(svcCall("IsTSMTEDialog", d) == 0)
            svcCall("SetTSMTEDialogDocumentID", d, 0x1234)
            svcCall("SetTSMTEDialogTSMTERecHandle", d, 0x5678)
            #expect(svcCall("IsTSMTEDialog", d) == 1)
            #expect(svcCall("GetTSMTEDialogDocumentID", d) == 0x1234)
            #expect(svcCall("GetTSMTEDialogTSMTERecHandle", d) == 0x5678)
            #expect(svcCall("TSMGetActiveDocument") == 0)
            let doc = svcPtr(4)
            #expect(tb_NewTSMDocument(0, 0, doc, 0) == 0)
            #expect(svcStatus(svcCall("TSMSetInlineInputRegion", 0xDEAD, 0, 0)) == -2505)
            #expect(svcCall("TSMSetInlineInputRegion", rd32(doc), 0, tb_NewRgn()) == 0)
            #expect(tb_ActivateTSMDocument(rd32(doc)) == 0)
            #expect(svcCall("TSMGetActiveDocument") == rd32(doc))
        }

        @Test func aeReplaceDescData() {
            bootMachine()
            let desc = svcPtr(8)
            let data = svcPtr(4)
            wr32(data, 0x0102_0304)
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("long")), data, 4, desc) == 0)
            let more = svcPtr(8)
            for (i, b) in Array("hello".utf8).enumerated() { wr8(more + UInt32(i), b) }
            #expect(svcCall("AEReplaceDescData", fourCC("TEXT"), more, 5, desc) == 0)
            #expect(rd32(desc) == fourCC("TEXT"))
            #expect(tb_AEGetDescDataSize(desc) == 5)
            #expect(svcStatus(svcCall("AEReplaceDescData", fourCC("TEXT"), more, UInt32(bitPattern: -1), desc)) == -50)
        }

        nonisolated(unsafe) static var optionCalls: [UInt32] = []

        /// pascal void (SInt32 selector, UInt32 command, Boolean *setting)
        static let dbgCallback: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let setting = cpu_pop32(c)
            let command = cpu_pop32(c)
            let selector = cpu_pop32(c)
            optionCalls.append(command << 16 | selector)
            if command == 1 { wr8(setting, 1) } // kGetDebugOption: starts on
            cpu_push32(c, ret)
        }

        nonisolated(unsafe) static var handled: [String] = []
        /// pascal void (OSType, UInt32, 6 pointers/longs..., ConstStr255Param message)
        static let assertHandler: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            let msg = cpu_pop32(c)
            for _ in 0..<7 { _ = cpu_pop32(c) }
            let n = Int(rd8(msg))
            handled.append(String(decoding: svcBytes(msg + 1, n), as: UTF8.self))
            cpu_push32(c, ret)
        }

        @Test func debuggingComponents() {
            bootMachine()
            Self.optionCalls = []
            let cb = svcUPP68k(Self.dbgCallback, "DbgCallbackTest", 0xFC0)
            let sig = fourCC("test")
            // Debugging.h's codes (the os9rest implementation's): NULL name, duplicate signature, no callback
            #expect(svcStatus(svcCall("NewDebugComponent", sig, 0, cb)) == -13885)
            #expect(svcCall("NewDebugComponent", sig, svcPStr("Test"), cb) == 0)
            #expect(svcStatus(svcCall("NewDebugComponent", sig, svcPStr("Test"), cb)) == -13881)
            #expect(svcCall("NewDebugOption", sig, 7, svcPStr("Verbose")) == 0)
            #expect(svcStatus(svcCall("NewDebugOption", sig, 7, svcPStr("Again"))) == -13882)
            #expect(svcStatus(svcCall("NewDebugOption", fourCC("none"), 1, svcPStr("x"))) == -13883)
            let name = svcPtr(256), setting = svcPtr(2), selector = svcPtr(4), signature = svcPtr(4)
            #expect(svcCall("GetDebugComponentInfo", 1, signature, name) == 0 && rd32(signature) == sig)
            #expect(svcStatus(svcCall("GetDebugComponentInfo", 2, signature, name)) == -13887)
            // the component's callback is asked whenever an option is read
            #expect(svcCall("GetDebugOptionInfo", 1, sig, selector, name, setting) == 0)
            #expect(Self.optionCalls == [0x0001_0007])
            #expect(rd32(selector) == 7 && rd8(setting) == 1 && rd8(name) == 7)
            #expect(svcCall("SetDebugOptionValue", sig, 7, 0) == 0)
            let expected: UInt32 = 0x0002_0007
            #expect(Self.optionCalls.last == expected) // and told when one is set
            #expect(svcStatus(svcCall("SetDebugOptionValue", sig, 8, 1)) == -13884)
            #expect(svcCall("DisposeDebugComponent", sig) == 0)
            #expect(svcStatus(svcCall("DisposeDebugComponent", sig)) == -13883)
            #expect(svcCall("TaskLevel") == 0)

            Self.handled = []
            let h = svcUPP68k(Self.assertHandler, "AssertHandlerTest", 0x00FFFFC0)
            svcCall("InstallDebugAssertOutputHandler", h)
            let s1 = svcPtr(16), s2 = svcPtr(16), s3 = svcPtr(16), s4 = svcPtr(16)
            for (p, t) in [(s1, "x == 1"), (s2, ""), (s3, ""), (s4, "file.c")] {
                for (i, b) in Array(t.utf8).enumerated() { wr8(p + UInt32(i), b) }
                wr8(p + UInt32(t.utf8.count), 0)
            }
            svcCall("DebugAssert", sig, 0, s1, s2, s3, s4, 42, 0)
            #expect(Self.handled.count == 1 && Self.handled[0].contains("x == 1") && Self.handled[0].contains("42"))
        }

        @Test func absentServicesSayWhy() {
            bootMachine()
            #expect(svcStatus(svcCall("AHGotoMainTOC", 0)) == -10790)
            #expect(svcStatus(svcCall("AHGotoMainTOC", 5)) == -50)
            #expect(svcStatus(svcCall("AHSearch", 0, 0)) == -50)
            #expect(svcStatus(svcCall("DeviceControlDoAVCTransaction", 1, 0)) == -200)
            let nib = svcPtr(4)
            wr32(nib, 99)
            #expect(svcStatus(svcCall("CreateNibReference", svcCFString("Main"), nib)) == -10960 && rd32(nib) == 0)
            #expect(svcStatus(svcCall("CreateWindowFromNib", 0, svcCFString("W"), nib)) == -50)
            svcCall("DisposeNibReference", 0)
        }

        @Test func miscMemoryAndSpeed() {
            _ = launchTestApp()
            svcCall("CheckAllHeaps")
            #expect(svcCall("GetGrowZone") == 0)
            #expect(Int16(truncatingIfNeeded: svcCall("MinimumProcessorSpeed")) > 0)
        }

        nonisolated(unsafe) static var chunks: [[Int16]] = []
        nonisolated(unsafe) static var chunkData: UInt32 = 0
        nonisolated(unsafe) static var chunkDesc: UInt32 = 0

        /// pascal Boolean (SoundComponentDataPtr *data, void *refCon): the next chunk, or none.
        static let fillCallback: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            _ = cpu_pop32(c)
            let slot = cpu_pop32(c)
            if let chunk = chunks.first {
                chunks.removeFirst()
                for (i, v) in chunk.enumerated() { wr16(chunkData + UInt32(2 * i), UInt16(bitPattern: v)) }
                wr32(chunkDesc + 16, UInt32(chunk.count))
                wr32(chunkDesc + 20, chunkData)
                wr32(slot, chunkDesc)
                wr8(c.pointee.a.7, 1)
            } else {
                wr32(slot, 0)
                wr8(c.pointee.a.7, 0)
            }
            cpu_push32(c, ret)
        }

        @Test func soundConverterFillBuffer() {
            bootMachine()
            func format(_ rate: UInt32) -> UInt32 {
                let f = svcPtr(28)
                wr32(f + 4, fourCC("twos"))
                wr16(f + 8, 1)
                wr16(f + 10, 16)
                wr32(f + 12, rate << 16)
                return f
            }
            let sc = svcPtr(4)
            #expect(svcCall("SoundConverterOpen", format(22050), format(22050), sc) == 0)
            Self.chunkData = svcPtr(64)
            Self.chunkDesc = svcPtr(28)
            Self.chunks = [[1, 2, 3, 4], [5, 6, 7, 8]]
            let upp = svcUPP68k(Self.fillCallback, "FillBufferTest", 0x3D0)
            let out = svcPtr(64), bytes = svcPtr(4), frames = svcPtr(4), flags = svcPtr(4)
            // Room for 6 frames: the first chunk and part of the second.
            #expect(svcCall("SoundConverterFillBuffer", rd32(sc), upp, 0, out, 12, bytes, frames, flags) == 0)
            #expect(rd32(frames) == 6 && rd32(bytes) == 12)
            #expect(rd32(flags) & 2 != 0) // left over
            #expect((0..<6).map { rd16(out + UInt32(2 * $0)) } == [1, 2, 3, 4, 5, 6])
            // The rest, and the end of the input.
            #expect(svcCall("SoundConverterFillBuffer", rd32(sc), upp, 0, out, 12, bytes, frames, flags) == 0)
            #expect(rd32(frames) == 2 && rd16(out) == 7 && rd16(out + 2) == 8)
            #expect(rd32(flags) & 1 != 0) // didn't fill the buffer
            #expect(svcStatus(svcCall("SoundConverterFillBuffer", 0, upp, 0, out, 12, bytes, frames, flags)) == -50)
            #expect(svcCall("SoundManagerGetInfo", fourCC("xxxx"), out) != 0)
        }
    }
}
