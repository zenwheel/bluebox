import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// CarbonLib's remaining universal procedure pointer calls (carbon_upps.c)
    /// and its small additions to older managers (carbon_gaps.c).
    @Suite(.serialized) struct CarbonUPPs {
        func ptr(_ n: Int = 4) -> UInt32 { tb_NewPtr(Int32(n), false, true) }

        func launch() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            g_cpu!.pointee.halted = false // the test application never ran
        }

        /// A transition vector for PowerPC code.
        func routine(_ words: [UInt32]) -> UInt32 {
            let code = ptr(words.count * 4), tv = ptr(8)
            for (i, w) in words.enumerated() { wr32(code + UInt32(i * 4), w) }
            wr32(tv, code)
            return tv
        }

        /// Call an export with its arguments in r3 on (the ninth on in the
        /// caller's parameter area); r3 after.
        @discardableResult
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            let sp = tb_NewPtr(8192, false, true) + 4096
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                gpr.storeBytes(of: sp, toByteOffset: 4, as: UInt32.self)
                for (i, a) in args.enumerated() where i < 8 { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            for (i, a) in args.enumerated() where i >= 8 { wr32(sp + 24 + UInt32(i * 4), a) }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        func pstring(_ p: UInt32) -> String {
            String(decoding: (0..<Int(rd8(p))).map { rd8(p + 1 + UInt32($0)) }, as: UTF8.self)
        }

        /// A UPP of a 16-bit-result type: the descriptor carries the type's
        /// procInfo, the routine runs with the arguments before the UPP, and
        /// its result comes back sign-extended.
        @Test func resultOfAType() {
            launch()
            let add = routine([0x7C63_2214, 0x4E80_0020]) // add r3, r3, r4; blr
            let upp = call("NewAECoercePtrUPP", [add])
            #expect(rd16(upp) == 0xAAFE && rd32(upp + 12) == 0x0003_FFE0 && rd32(upp + 20) == add)
            // AECoercePtr: OSErr (typeCode, dataPtr, dataSize, toType, refCon, result).
            let u6 = upp
            #expect(call("InvokeAECoercePtrUPP", [40, 2, 0, 0, 0, 0, u6]) == 42)
            #expect(call("InvokeAECoercePtrUPP", [0xFFFF, 0, 0, 0, 0, 0, u6]) == 0xFFFF_FFFF)
            call("DisposeAECoercePtrUPP", [upp])
        }

        /// Mixed argument sizes (a pointer, a pointer, a byte and a pointer):
        /// the routine returns its third argument.
        @Test func mixedArgumentSizes() {
            launch()
            let third = routine([0x7CA3_2B78, 0x4E80_0020]) // mr r3, r5; blr
            let upp = call("NewHRNewCFURLUPP", [third])
            #expect(rd32(upp + 12) == 0x37F0)
            #expect(call("InvokeHRNewCFURLUPP", [1, 2, 1, 4, upp]) == 1)
            // Four arguments, a four-byte result.
            let fourth = routine([0x7C83_2378, 0x4E80_0020]) // mr r3, r4; blr
            let u2 = call("NewHRNewCFURLUPP", [fourth])
            #expect(call("InvokeHRNewCFURLUPP", [0x1111, 0x2222_3333, 0, 0, u2]) == 0x2222_3333)
            // A Boolean result is one byte; a nil UPP calls nothing.
            let visited = call("NewHRWasCFURLVisitedUPP", [routine([0x3860_0001, 0x4E80_0020])])
            #expect(rd32(visited + 12) == 0x3D0)
            #expect(call("InvokeHRWasCFURLVisitedUPP", [0, 0, visited]) == 1)
            #expect(call("InvokeHRWasCFURLVisitedUPP", [0, 0, 0]) == 0)
            call("DisposeHRWasCFURLVisitedUPP", [visited])
        }

        /// NewComponentFunctionUPP takes its procInfo from the caller.
        @Test func componentFunctionTakesItsProcInfo() {
            launch()
            let upp = call("NewComponentFunctionUPP", [routine([0x4E80_0020]), 0x2F0])
            #expect(rd32(upp + 12) == 0x2F0)
            call("DisposeComponentFunctionUPP", [upp])
        }

        /// Every type of the interfaces has its three calls.
        @Test func everyUPPHasItsCalls() {
            for t in ["AVLCompareItems", "CMFlatten", "DMNotification", "IconGetter", "OTNotify", "SpeechDone",
                      "MovieProgress", "OSLAccessor", "Purge", "SndCallBack", "URLNotify", "ExceptionHandler"] {
                for k in ["New", "Dispose", "Invoke"] {
                    #expect(cfm_export_fn("\(k)\(t)UPP") != nil, "\(k)\(t)UPP")
                }
            }
        }

        @Test func descriptorDataOfAnAEDesc() {
            launch()
            let bytes: [UInt8] = [1, 2, 3, 4, 5]
            let buf = ptr(8), desc = ptr(8)
            for (i, b) in bytes.enumerated() { wr8(buf + UInt32(i), b) }
            #expect(tb_AECreateDesc(Int32(bitPattern: fourCC("TEXT")), buf, 5, desc) == 0)
            #expect(call("AEGetDescDataSize", [desc]) == 5)
            let out = ptr(8)
            #expect(call("AEGetDescData", [desc, out, 8]) == 0)
            #expect((0..<5).map { rd8(out + UInt32($0)) } == bytes)
            let small = ptr(8)
            #expect(call("AEGetDescData", [desc, small, 3]) == 0)
            #expect(rd8(small + 2) == 3 && rd8(small + 3) == 0)
            tb_AEDisposeDesc(desc)
        }

        @Test func longDatesAndTimes() {
            launch()
            let dt = ptr(8)
            wr32(dt, 0); wr32(dt + 4, 3_300_000_000)
            let a = ptr(256), b = ptr(256)
            for form: Int8 in 0...2 {
                call("LongDateString", [dt, UInt32(form), a, 0])
                tb_IUDateString(Int32(bitPattern: 3_300_000_000), form, b)
                #expect(rd8(a) > 0 && pstring(a) == pstring(b))
            }
            for secs: UInt32 in 0...1 {
                call("LongTimeString", [dt, secs, a, 0])
                tb_IUTimeString(Int32(bitPattern: 3_300_000_000), Int8(secs), b)
                #expect(rd8(a) > 0 && pstring(a) == pstring(b))
            }
        }

        @Test func compareTextByTheItl2Order() {
            launch()
            func cmp(_ x: String, _ y: String) -> Int16 {
                let p = ptr(32), q = ptr(32)
                for (i, v) in x.utf8.enumerated() { wr8(p + UInt32(i), v) }
                for (i, v) in y.utf8.enumerated() { wr8(q + UInt32(i), v) }
                return Int16(truncatingIfNeeded: call("CompareText", [p, q, UInt32(x.utf8.count), UInt32(y.utf8.count), 0]))
            }
            #expect(cmp("abc", "abd") == -1)
            #expect(cmp("b", "a") == 1)
            #expect(cmp("abc", "abc") == 0)
            #expect(cmp("ab", "abc") == -1)
            #expect(cmp("ABC", "abd") == -1) // case is secondary
            #expect(cmp("Abc", "abc") == cmpDirect("Abc", "abc"))
        }

        func cmpDirect(_ x: String, _ y: String) -> Int16 {
            let p = ptr(32), q = ptr(32)
            for (i, v) in x.utf8.enumerated() { wr8(p + UInt32(i), v) }
            for (i, v) in y.utf8.enumerated() { wr8(q + UInt32(i), v) }
            return tb_IUMagString(p, q, Int16(x.utf8.count), Int16(y.utf8.count))
        }

        @Test func moreMasterPointersAddsABlock() {
            launch()
            let before = tb_FreeMem(false)
            call("MoreMasterPointers", [500])
            #expect(before - tb_FreeMem(false) >= 2000)
        }

        /// CallComponentDispatch runs a call built as the 68k glue builds it;
        /// the generic scripting component answers AppleScript's subtype, and
        /// OSASetDefaultScriptingComponent accepts only that.
        @Test func componentDispatchAndTheDefaultScriptingComponent() {
            launch()
            let ci = call("OpenDefaultComponent", [fourCC("osa "), fourCC("scpt")])
            guard ci != 0 else { Issue.record("no generic scripting component"); return }
            let cp = ptr(16), out = ptr(4)
            wr8(cp, 0); wr8(cp + 1, 4); wr16(cp + 2, 0x1001) // GetDefaultScriptingComponent
            wr32(cp + 4, out); wr32(cp + 8, ci)
            #expect(call("CallComponentDispatch", [cp]) == 0)
            #expect(rd32(out) == fourCC("ascr"))
            #expect(call("OSASetDefaultScriptingComponent", [ci, fourCC("ascr")]) == 0)
            #expect(Int32(bitPattern: call("OSASetDefaultScriptingComponent", [ci, fourCC("xxxx")])) == -1761)
        }
    }
}
