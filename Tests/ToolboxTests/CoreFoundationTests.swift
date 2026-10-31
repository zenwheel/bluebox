import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Core Foundation as CarbonLib had it, and Math64, called as a
    /// PowerPC program calls them: integers and pointers in r3 on (past r10
    /// in the parameter area), doubles in f1 on, a struct result through a
    /// pointer in r3; callbacks are PowerPC code of the test's own.
    @Suite struct CoreFoundation {
        /// Call an export; r3 after.
        @discardableResult
        func call(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> UInt32 {
            if args.count > 8 {
                for i in 8..<args.count { wr32(g_ppc.gpr.1 + 24 + UInt32(4 * i), args[i]) }
            }
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.prefix(8).enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            for (i, f) in fp.enumerated() { ppc_set_fpr(&g_ppc, Int32(1 + i), f) }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return g_ppc.gpr.3
        }

        func int(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> Int32 {
            Int32(bitPattern: call(name, args, fp: fp))
        }
        func bool(_ name: String, _ args: [UInt32] = []) -> Bool { call(name, args) & 0xFF != 0 }
        var r4: UInt32 { g_ppc.gpr.4 }
        var f1: Double { ppc_fpr(&g_ppc, 1) }
        var f2: Double { ppc_fpr(&g_ppc, 2) }
        /// A 64-bit result, r3 and r4.
        func wide(_ name: String, _ args: [UInt32]) -> UInt64 {
            let hi = call(name, args)
            return UInt64(hi) << 32 | UInt64(r4)
        }
        func words(_ v: UInt64) -> [UInt32] { [UInt32(v >> 32), UInt32(truncatingIfNeeded: v)] }
        func words(_ d: Double) -> [UInt32] { words(d.bitPattern) }

        func bytes(_ b: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }
        func cstr(_ s: String) -> UInt32 { bytes(Array(s.utf8) + [0]) }
        func pstr(_ s: String) -> UInt32 { bytes([UInt8(s.utf8.count)] + Array(s.utf8)) }
        func buffer(_ n: Int) -> UInt32 { tb_NewPtr(Int32(n), false, true) }
        func read(_ p: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { rd8(p + UInt32($0)) } }
        func cString(_ p: UInt32) -> String {
            var b: [UInt8] = []
            var a = p
            while rd8(a) != 0 { b.append(rd8(a)); a += 1 }
            return String(decoding: b, as: UTF8.self)
        }
        func double(_ p: UInt32) -> Double { Double(bitPattern: UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4))) }
        func data(_ name: String) -> UInt32 { rd32(ppc_data_export(name)) }

        /// A CFString from Swift text (UTF-8), and back.
        func str(_ s: String) -> UInt32 { call("CFStringCreateWithCString", [0, cstr(s), 0x0800_0100]) }
        func text(_ s: UInt32) -> String? {
            guard s != 0, call("CFGetTypeID", [s]) == call("CFStringGetTypeID") else { return nil }
            let n = call("CFStringGetLength", [s])
            let buf = buffer(Int(n) * 2 + 2)
            call("CFStringGetCharacters", [s, 0, n, buf])
            return String(utf16CodeUnits: (0..<Int(n)).map { rd16(buf + UInt32($0 * 2)) }, count: Int(n))
        }
        func retainCount(_ cf: UInt32) -> UInt32 { call("CFGetRetainCount", [cf]) }

        /// PowerPC code of the test's own, as a transition vector.
        func routine(_ code: [UInt32]) -> UInt32 {
            let p = tb_NewPtr(Int32(code.count * 4), false, true)
            for (i, w) in code.enumerated() { wr32(p + UInt32(i * 4), w) }
            let tv = tb_NewPtr(8, false, true)
            wr32(tv, p)
            return tv
        }
        func giveStack() {
            #expect(cfm_ppc_cpu())
            g_ppc.gpr.1 -= 1024 // room for a parameter area above
        }
        func start() -> URL {
            let dir = launchTestApp()
            giveStack()
            return dir
        }

        /// compare(a, b, context): a - b
        var subtract: UInt32 { routine([0x7C64_1850, 0x4E80_0020]) }
        /// applier(value, context): *context += value
        var sumInto: UInt32 { routine([0x80A4_0000, 0x7CA5_1A14, 0x90A4_0000, 0x4E80_0020]) }

        // MARK: User and machine names

        /// CSCopyUserName and CSCopyMachineName (CarbonLib 1.3): the host
        /// account's short or full name and the host's name, as strings the
        /// caller owns (Asshole II names its player so).
        @Test func userAndMachineNames() {
            _ = start()
            let short = text(call("CSCopyUserName", [1]))
            #expect(short == NSUserName())
            let full = text(call("CSCopyUserName", [0]))
            #expect(full != nil && !full!.isEmpty)
            let machine = text(call("CSCopyMachineName"))
            #expect(machine != nil && !machine!.isEmpty && !machine!.contains("."))
        }

        // MARK: Objects and allocators

        @Test func objectsCountReferencesAndCompare() {
            _ = start()
            let a = str("hello"), b = str("hello"), c = str("other")
            #expect(retainCount(a) == 1)
            #expect(call("CFRetain", [a]) == a && retainCount(a) == 2)
            call("CFRelease", [a])
            #expect(retainCount(a) == 1)
            #expect(bool("CFEqual", [a, b]) && !bool("CFEqual", [a, c]))
            #expect(call("CFHash", [a]) == call("CFHash", [b]))
            #expect(call("CFGetTypeID", [a]) == call("CFStringGetTypeID"))
            #expect(text(call("CFCopyTypeIDDescription", [call("CFArrayGetTypeID")])) == "CFArray")
            let desc = text(call("CFCopyDescription", [a])) ?? ""
            #expect(desc.hasPrefix("<CFString 0x") && desc.hasSuffix("{contents = \"hello\"}"))
            // Each type its own ID.
            let ids = ["CFStringGetTypeID", "CFArrayGetTypeID", "CFDictionaryGetTypeID", "CFSetGetTypeID",
                       "CFBagGetTypeID", "CFDataGetTypeID", "CFNumberGetTypeID", "CFBooleanGetTypeID",
                       "CFDateGetTypeID", "CFURLGetTypeID", "CFUUIDGetTypeID", "CFCharacterSetGetTypeID",
                       "CFTreeGetTypeID", "CFBundleGetTypeID", "CFXMLNodeGetTypeID", "CFXMLParserGetTypeID",
                       "CFAllocatorGetTypeID", "CFPlugInInstanceGetTypeID"].map { call($0) }
            #expect(Set(ids).count == ids.count)
            #expect(call("CFPlugInGetTypeID") == call("CFBundleGetTypeID"))
            // Not an object: nothing happens.
            let junk = bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16])
            call("CFRelease", [junk])
            #expect(call("CFGetTypeID", [junk]) == 0 && !bool("CFEqual", [junk, a]))
        }

        @Test func allocatorsAndAProgramsOwn() {
            _ = start()
            #expect(data("kCFAllocatorDefault") == 0 && data("kCFAllocatorUseContext") == 1)
            let system = data("kCFAllocatorSystemDefault")
            #expect(call("CFGetTypeID", [system]) == call("CFAllocatorGetTypeID"))
            #expect(call("CFAllocatorGetDefault") == system)
            #expect(call("CFGetAllocator", [str("x")]) == system)
            let p = call("CFAllocatorAllocate", [0, 100, 0])
            #expect(p != 0 && tb_GetPtrSize(p) == 100)
            let q = call("CFAllocatorReallocate", [0, p, 400, 0])
            #expect(q != 0 && tb_GetPtrSize(q) == 400)
            call("CFAllocatorDeallocate", [0, q])
            #expect(call("CFAllocatorAllocate", [data("kCFAllocatorNull"), 16, 0]) == 0)
            #expect(call("CFAllocatorGetPreferredSizeForSize", [0, 77, 0]) == 77)

            // A program's allocator: a bump allocator counting what it does.
            let arena = buffer(4096)
            let info = buffer(12)
            wr32(info + 4, arena)
            let allocate = routine([0x80C5_0000, 0x38C6_0001, 0x90C5_0000, 0x80E5_0004, 0x7D07_1A14, 0x9105_0004,
                                    0x7CE3_3B78, 0x4E80_0020])
            let deallocate = routine([0x80C4_0008, 0x38C6_0001, 0x90C4_0008, 0x4E80_0020])
            let ctx = buffer(36)
            wr32(ctx + 4, info)
            wr32(ctx + 20, allocate)
            wr32(ctx + 28, deallocate)
            let mine = call("CFAllocatorCreate", [0, ctx])
            #expect(call("CFGetTypeID", [mine]) == call("CFAllocatorGetTypeID"))
            let before = rd32(info)
            let s = call("CFStringCreateWithCString", [mine, cstr("in my arena"), 0])
            #expect(s >= arena && s < arena + 4096 && rd32(info) == before + 1)
            #expect(call("CFGetAllocator", [s]) == mine && text(s) == "in my arena")
            #expect(retainCount(mine) == 2) // the string keeps it
            call("CFRelease", [s])
            #expect(rd32(info + 8) == 1 && retainCount(mine) == 1)
            let got = buffer(36)
            call("CFAllocatorGetContext", [mine, got])
            #expect(rd32(got + 4) == info && rd32(got + 20) == allocate)
            // Made with its own allocate callback.
            let selfMade = call("CFAllocatorCreate", [1, ctx])
            #expect(selfMade >= arena && selfMade < arena + 4096)
            call("CFAllocatorSetDefault", [mine])
            #expect(call("CFAllocatorGetDefault") == mine)
            let d = call("CFDataCreate", [0, bytes([1, 2, 3]), 3])
            #expect(d >= arena && d < arena + 4096)
        }

        // MARK: Strings

        @Test func stringsInTheirEncodings() {
            _ = start()
            let cafe = call("CFStringCreateWithCString", [0, bytes([0x43, 0x61, 0x66, 0x8E, 0]), 0]) // Mac OS Roman é
            #expect(text(cafe) == "Café")
            #expect(text(call("CFStringCreateWithPascalString", [0, bytes([3, 0x41, 0x42, 0x43]), 0])) == "ABC")
            let units = bytes([0, 0x48, 0, 0x69, 0x20, 0xAC])
            #expect(text(call("CFStringCreateWithCharacters", [0, units, 3])) == "Hi€")
            #expect(text(call("CFStringCreateWithBytes", [0, bytes([0xE2, 0x82, 0xAC]), 3, 0x0800_0100, 0])) == "€")
            #expect(call("CFStringCreateWithBytes", [0, bytes([0xC3]), 1, 0x0800_0100, 0]) == 0) // cut short
            // UTF-16 with a byte order mark, either order.
            #expect(text(call("CFStringCreateWithBytes", [0, bytes([0xFE, 0xFF, 0, 0x41]), 4, 0x100, 1])) == "A")
            #expect(text(call("CFStringCreateWithBytes", [0, bytes([0xFF, 0xFE, 0x42, 0]), 4, 0x100, 1])) == "B")
            // Through the Text Encoding Converter: ISO Latin 2, Shift-JIS.
            #expect(text(call("CFStringCreateWithCString", [0, bytes([0xB1, 0]), 0x0202])) == "ą")
            #expect(text(call("CFStringCreateWithBytes", [0, bytes([0x93, 0xFA]), 2, 0x0A01, 0])) == "日")

            let buf = buffer(64)
            #expect(bool("CFStringGetCString", [cafe, buf, 64, 0]) && read(buf, 5) == [0x43, 0x61, 0x66, 0x8E, 0])
            #expect(bool("CFStringGetCString", [cafe, buf, 64, 0x0800_0100]) && cString(buf) == "Café")
            #expect(!bool("CFStringGetCString", [cafe, buf, 4, 0]))   // no room for the NUL
            #expect(!bool("CFStringGetCString", [cafe, buf, 64, 0x600])) // no é in ASCII
            #expect(bool("CFStringGetPascalString", [cafe, buf, 64, 0]) && rd8(buf) == 4 && rd8(buf + 4) == 0x8E)
            #expect(call("CFStringGetCharacterAtIndex", [cafe, 3]) == 0xE9)

            // GetBytes: a loss byte, a short buffer, an external representation.
            let used = buffer(4)
            #expect(call("CFStringGetBytes", [cafe, 0, 4, 0x600, 0x3F, 0, buf, 64, used]) == 4)
            #expect(rd32(used) == 4 && read(buf, 4) == Array("Caf?".utf8))
            #expect(call("CFStringGetBytes", [cafe, 0, 4, 0x600, 0, 0, buf, 64, used]) == 3)
            #expect(call("CFStringGetBytes", [cafe, 0, 4, 0x0800_0100, 0, 0, buf, 4, used]) == 3 && rd32(used) == 3)
            #expect(call("CFStringGetBytes", [cafe, 0, 4, 0x0800_0100, 0, 0, 0, 0, used]) == 4 && rd32(used) == 5)
            #expect(call("CFStringGetBytes", [cafe, 1, 1, 0x100, 0, 1, buf, 64, used]) == 1)
            #expect(rd32(used) == 4 && read(buf, 4) == [0xFE, 0xFF, 0, 0x61])
            let ext = call("CFStringCreateExternalRepresentation", [0, cafe, 0x100, 0])
            #expect(call("CFDataGetLength", [ext]) == 10)
            #expect(bool("CFEqual", [call("CFStringCreateFromExternalRepresentation", [0, ext, 0x100]), cafe]))

            // Pointers into a string: what a NoCopy call gave, our copies for
            // strings that can't change, nothing for those that can.
            let lit = cstr("literal")
            let nocopy = call("CFStringCreateWithCStringNoCopy", [0, lit, 0, data("kCFAllocatorNull")])
            #expect(call("CFStringGetCStringPtr", [nocopy, 0]) == lit)
            let ptr = call("CFStringGetCStringPtr", [cafe, 0])
            #expect(ptr != 0 && read(ptr, 5) == [0x43, 0x61, 0x66, 0x8E, 0])
            #expect(call("CFStringGetCStringPtr", [cafe, 0x0800_0100]) == 0)
            let pp = call("CFStringGetPascalStringPtr", [cafe, 0])
            #expect(pp != 0 && rd8(pp) == 4)
            let uni = call("CFStringCreateWithCharactersNoCopy", [0, units, 3, data("kCFAllocatorNull")])
            #expect(call("CFStringGetCharactersPtr", [uni]) == units)
            #expect(call("CFStringGetCharactersPtr", [cafe]) == 0)
            let mutable = call("CFStringCreateMutableCopy", [0, 0, cafe])
            #expect(call("CFStringGetCStringPtr", [mutable, 0]) == 0)

            // CFSTR: one string a text, never freed.
            let k = call("__CFStringMakeConstantString", [cstr("Snake")])
            #expect(k != 0 && call("__CFStringMakeConstantString", [cstr("Snake")]) == k)
            call("CFRelease", [k])
            call("CFRelease", [k])
            #expect(text(k) == "Snake")

            // Encodings.
            #expect(call("CFStringGetSystemEncoding") == 0)
            #expect(call("CFStringGetSmallestEncoding", [cafe]) == 0)
            #expect(call("CFStringGetSmallestEncoding", [str("日本")]) == 0x100)
            #expect(bool("CFStringIsEncodingAvailable", [0x0A01]) && !bool("CFStringIsEncodingAvailable", [0x7777]))
            #expect(call("CFStringGetMaximumSizeForEncoding", [10, 0x0800_0100]) == 30)
            #expect(call("CFStringGetMaximumSizeForEncoding", [10, 0]) == 10)
            let list = call("CFStringGetListOfAvailableEncodings")
            var n: UInt32 = 0
            while rd32(list + 4 * n) != 0xFFFF_FFFF && n < 500 { n += 1 }
            #expect(n > 10 && n < 500 && rd32(list) == 0)
            #expect(text(call("CFStringGetNameOfEncoding", [0])) == "Mac OS Roman")
            #expect(text(call("CFStringGetNameOfEncoding", [0x0800_0100])) == "Unicode (UTF-8)")
            #expect(call("CFStringConvertIANACharSetNameToEncoding", [str("iso-8859-1")]) == 0x201)
            #expect(text(call("CFStringConvertEncodingToIANACharSetName", [0x0A01])) == "Shift_JIS")
            #expect(call("CFStringConvertEncodingToWindowsCodepage", [0x500]) == 1252)
            #expect(call("CFStringConvertWindowsCodepageToEncoding", [10000]) == 0)
            #expect(call("CFStringConvertEncodingToNSStringEncoding", [0x0800_0100]) == 4)
            #expect(call("CFStringConvertNSStringEncodingToEncoding", [30]) == 0)
            #expect(call("CFStringConvertEncodingToNSStringEncoding", [0x0202]) == 9)
            #expect(call("CFStringConvertEncodingToNSStringEncoding", [0x0203]) == 0x8000_0203)
            #expect(call("CFStringGetMostCompatibleMacStringEncoding", [0x0202]) == 29)
            #expect(call("CFStringGetMostCompatibleMacStringEncoding", [0x0A01]) == 1)
            call("CFShow", [cafe])
            call("CFShowStr", [cafe])
        }

        @Test func stringsCompareFindAndSplit() {
            _ = start()
            let s = str("Hello, World")
            #expect(int("CFStringCompare", [str("abc"), str("abd"), 0]) == -1)
            #expect(int("CFStringCompare", [str("ABC"), str("abc"), 0]) == -1)
            #expect(int("CFStringCompare", [str("ABC"), str("abc"), 1]) == 0)
            #expect(int("CFStringCompare", [str("Ärger"), str("ärger"), 1]) == 0)
            #expect(int("CFStringCompare", [str("file9"), str("file10"), 0]) == 1)
            #expect(int("CFStringCompare", [str("file9"), str("file10"), 64]) == -1)
            #expect(int("CFStringCompareWithOptions", [s, str("World"), 7, 5, 0]) == 0)

            let res = buffer(8)
            call("CFStringFind", [res, s, str("o"), 0])
            #expect(rd32(res) == 4 && rd32(res + 4) == 1)
            call("CFStringFind", [res, s, str("o"), 4]) // backwards
            #expect(rd32(res) == 8)
            call("CFStringFind", [res, s, str("world"), 1])
            #expect(rd32(res) == 7)
            call("CFStringFind", [res, s, str("World"), 8]) // anchored
            #expect(Int32(bitPattern: rd32(res)) == -1 && rd32(res + 4) == 0, "\(rd32(res)) \(rd32(res + 4))")
            call("CFStringFind", [res, s, str("World"), 12]) // anchored at the end
            #expect(rd32(res) == 7)
            #expect(bool("CFStringFindWithOptions", [s, str("l"), 5, 7, 0, res]) && rd32(res) == 10)
            #expect(!bool("CFStringFindWithOptions", [s, str("H"), 1, 11, 0, res]))
            #expect(bool("CFStringHasPrefix", [s, str("Hell")]) && !bool("CFStringHasPrefix", [s, str("ell")]))
            #expect(bool("CFStringHasSuffix", [s, str("World")]) && !bool("CFStringHasSuffix", [s, str("")]))

            let found = call("CFStringCreateArrayWithFindResults", [0, s, str("l"), 0, 12, 0])
            #expect(call("CFArrayGetCount", [found]) == 3)
            let third = call("CFArrayGetValueAtIndex", [found, 2])
            #expect(rd32(third) == 10 && rd32(third + 4) == 1)
            #expect(call("CFStringCreateArrayWithFindResults", [0, s, str("z"), 0, 12, 0]) == 0)

            let parts = call("CFStringCreateArrayBySeparatingStrings", [0, str("a::b::::c"), str("::")])
            #expect(call("CFArrayGetCount", [parts]) == 4)
            #expect(text(call("CFArrayGetValueAtIndex", [parts, 2])) == "")
            #expect(text(call("CFStringCreateByCombiningStrings", [0, parts, str("+")])) == "a+b++c")

            let lines = str("one\r\ntwo\nthree")
            let b = buffer(12)
            call("CFStringGetLineBounds", [lines, 6, 0, b, b + 4, b + 8])
            #expect(rd32(b) == 5 && rd32(b + 4) == 9 && rd32(b + 8) == 8)
            call("CFStringGetLineBounds", [lines, 0, 1, b, b + 4, b + 8])
            #expect(rd32(b) == 0 && rd32(b + 4) == 5 && rd32(b + 8) == 3)

            #expect(int("CFStringGetIntValue", [str("  -42abc")]) == -42)
            #expect(int("CFStringGetIntValue", [str("x")]) == 0)
            #expect(int("CFStringGetIntValue", [str("99999999999")]) == Int32.max)
            call("CFStringGetDoubleValue", [str(" 3.25e2")])
            #expect(f1 == 325)
            #expect(text(call("CFStringCreateWithSubstring", [0, s, 7, 5])) == "World")
            #expect(call("CFStringCreateWithSubstring", [0, s, 7, 50]) == 0)
            let copy = call("CFStringCreateCopy", [0, s])
            #expect(copy == s && retainCount(s) == 2)
        }

        @Test func mutableStrings() {
            _ = start()
            let m = call("CFStringCreateMutable", [0, 0])
            call("CFStringAppend", [m, str("Hello")])
            call("CFStringAppendCString", [m, cstr(" there"), 0])
            call("CFStringAppendPascalString", [m, pstr("!"), 0])
            call("CFStringAppendCharacters", [m, bytes([0, 0x3F]), 1])
            #expect(text(m) == "Hello there!?")
            call("CFStringInsert", [m, 5, str(",")])
            call("CFStringDelete", [m, 12, 2])
            #expect(text(m) == "Hello, there")
            call("CFStringReplace", [m, 7, 5, str("world")])
            #expect(text(m) == "Hello, world")
            call("CFStringUppercase", [m, 0])
            #expect(text(m) == "HELLO, WORLD")
            call("CFStringLowercase", [m, 0])
            #expect(text(m) == "hello, world")
            call("CFStringCapitalize", [m, 0])
            #expect(text(m) == "Hello, World")
            call("CFStringReplaceAll", [m, str("  padded \n")])
            call("CFStringTrimWhitespace", [m])
            #expect(text(m) == "padded")
            call("CFStringPad", [m, str("-="), 10, 1])
            #expect(text(m) == "padded=-=-")
            call("CFStringPad", [m, 0, 3, 0])
            #expect(text(m) == "pad")
            call("CFStringReplaceAll", [m, str("xxabcxx")])
            call("CFStringTrim", [m, str("x")])
            #expect(text(m) == "abc")
            call("CFStringReplaceAll", [m, str("é")])
            call("CFStringUppercase", [m, 0])
            #expect(text(m) == "É")

            // A program's own buffer: changes go back into it while they fit.
            let chars = buffer(20)
            for (i, u) in Array("abc".utf16).enumerated() { wr16(chars + UInt32(i * 2), u) }
            let ext = call("CFStringCreateMutableWithExternalCharactersNoCopy", [0, chars, 3, 10, data("kCFAllocatorNull")])
            #expect(call("CFStringGetCharactersPtr", [ext]) == chars)
            call("CFStringAppend", [ext, str("de")])
            #expect(rd16(chars + 6) == 0x64 && rd16(chars + 8) == 0x65)
            wr16(chars, 0x5A) // changed by the program, then told
            call("CFStringSetExternalCharactersNoCopy", [ext, chars, 5, 10])
            #expect(text(ext) == "Zbcde")
        }

        @Test func formats() {
            _ = start()
            let name = str("World")
            let f = call("CFStringCreateWithFormat", [0, 0, str("Hello, %@! %d %s %5.2f|%-4x|%C %%"), name, UInt32(bitPattern: -7),
                                                     cstr("ok")] + words(3.14159) + [0xAB, 0x263A])
            #expect(text(f) == "Hello, World! -7 ok  3.14|ab  |☺ %")
            // Numbered arguments, and ones past the registers.
            let g = call("CFStringCreateWithFormat", [0, 0, str("%2$@ before %1$d"), 12, str("this")])
            #expect(text(g) == "this before 12")
            let many = call("CFStringCreateWithFormat", [0, 0, str("%d%d%d%d%d%d%d%d"), 1, 2, 3, 4, 5, 6, 7, 8])
            #expect(text(many) == "12345678")
            let ll = call("CFStringCreateWithFormat", [0, 0, str("%lld %qu %hd"), 0xFFFF_FFFF, 0xFFFF_FFFE, 0, 3, 0x1_FFFF])
            #expect(text(ll) == "-2 3 -1")
            // An object's description, a NULL one, a precision.
            let num = call("CFNumberCreate", [0, 3, bytes([0, 0, 0, 5])])
            #expect(text(call("CFStringCreateWithFormat", [0, 0, str("%@"), num]))?.contains("value = +5") == true)
            #expect(text(call("CFStringCreateWithFormat", [0, 0, str("[%@][%.3s]"), 0, cstr("abcdef")])) == "[(null)][abc]")
            // A va_list: the arguments in memory.
            let va = buffer(16)
            wr32(va, 42)
            wr32(va + 4, name)
            #expect(text(call("CFStringCreateWithFormatAndArguments", [0, 0, str("%d:%@"), va])) == "42:World")
            let m = call("CFStringCreateMutableCopy", [0, 0, str(">")])
            call("CFStringAppendFormat", [m, 0, str("%03d"), 7])
            call("CFStringAppendFormatAndArguments", [m, 0, str("/%@"), va + 4])
            #expect(text(m) == ">007/World")
        }

        @Test func characterSets() {
            _ = start()
            let letters = call("CFCharacterSetGetPredefined", [5])
            #expect(call("CFCharacterSetGetPredefined", [5]) == letters)
            #expect(bool("CFCharacterSetIsCharacterMember", [letters, 0x41]))
            #expect(bool("CFCharacterSetIsCharacterMember", [letters, 0x4E2D]))
            #expect(!bool("CFCharacterSetIsCharacterMember", [letters, 0x31]))
            let digits = call("CFCharacterSetGetPredefined", [4])
            #expect(bool("CFCharacterSetIsCharacterMember", [digits, 0x37]) && bool("CFCharacterSetIsCharacterMember", [digits, 0x0663]))
            let ws = call("CFCharacterSetGetPredefined", [3])
            #expect(bool("CFCharacterSetIsCharacterMember", [ws, 0x0A]) && !bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [2]), 0x0A]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [7]), 0xC9]))
            #expect(!bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [7]), 0xE9]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [6]), 0xE9]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [11]), 0x2C]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [1]), 0x07]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [9]), 0xE9]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [8]), 0x0301]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [12]), 0xFFFF]))
            #expect(bool("CFCharacterSetIsCharacterMember", [call("CFCharacterSetGetPredefined", [10]), 0xBD]))

            let vowels = call("CFCharacterSetCreateWithCharactersInString", [0, str("aeiou")])
            #expect(bool("CFCharacterSetIsCharacterMember", [vowels, 0x65]) && !bool("CFCharacterSetIsCharacterMember", [vowels, 0x62]))
            let range = call("CFCharacterSetCreateWithCharactersInRange", [0, 0x61, 3])
            let m = call("CFCharacterSetCreateMutableCopy", [0, range])
            call("CFCharacterSetIntersect", [m, vowels])
            #expect(bool("CFCharacterSetIsCharacterMember", [m, 0x61]) && !bool("CFCharacterSetIsCharacterMember", [m, 0x62]))
            call("CFCharacterSetUnion", [m, range])
            call("CFCharacterSetRemoveCharactersInString", [m, str("b")])
            call("CFCharacterSetAddCharactersInRange", [m, 0x7A, 1])
            call("CFCharacterSetRemoveCharactersInRange", [m, 0x63, 1])
            #expect(bool("CFCharacterSetIsCharacterMember", [m, 0x7A]) && !bool("CFCharacterSetIsCharacterMember", [m, 0x63]))
            call("CFCharacterSetAddCharactersInString", [m, str("Q")])
            call("CFCharacterSetInvert", [m])
            #expect(!bool("CFCharacterSetIsCharacterMember", [m, 0x51]) && bool("CFCharacterSetIsCharacterMember", [m, 0x62]))
            let bitmap = call("CFCharacterSetCreateBitmapRepresentation", [0, vowels])
            #expect(call("CFDataGetLength", [bitmap]) == 8192)
            #expect(rd8(call("CFDataGetBytePtr", [bitmap]) + 12) == 0x22) // 'a' and 'e'
            let back = call("CFCharacterSetCreateWithBitmapRepresentation", [0, bitmap])
            #expect(bool("CFEqual", [back, vowels]))
            let empty = call("CFCharacterSetCreateMutable", [0])
            #expect(!bool("CFCharacterSetIsCharacterMember", [empty, 0x61]))
        }

        // MARK: Collections

        @Test func arrays() {
            _ = start()
            let cb = ppc_data_export("kCFTypeArrayCallBacks")
            let a = str("a"), b = str("b"), c = str("c")
            let values = buffer(12)
            for (i, v) in [a, b, c].enumerated() { wr32(values + UInt32(4 * i), v) }
            let arr = call("CFArrayCreate", [0, values, 3, cb])
            #expect(call("CFArrayGetCount", [arr]) == 3 && retainCount(a) == 2)
            #expect(call("CFArrayGetValueAtIndex", [arr, 1]) == b)
            #expect(bool("CFArrayContainsValue", [arr, 0, 3, str("c")]))
            #expect(int("CFArrayGetFirstIndexOfValue", [arr, 0, 3, str("b")]) == 1)
            #expect(int("CFArrayGetFirstIndexOfValue", [arr, 0, 1, str("b")]) == -1)
            let m = call("CFArrayCreateMutableCopy", [0, 0, arr])
            call("CFArrayAppendValue", [m, a])
            #expect(call("CFArrayGetCountOfValue", [m, 0, 4, a]) == 2 && int("CFArrayGetLastIndexOfValue", [m, 0, 4, a]) == 3)
            call("CFArrayInsertValueAtIndex", [m, 0, c])
            call("CFArrayRemoveValueAtIndex", [m, 4])
            call("CFArrayExchangeValuesAtIndices", [m, 0, 3])
            #expect((0..<4).map { text(call("CFArrayGetValueAtIndex", [m, UInt32($0)])) } == ["c", "a", "b", "c"])
            call("CFArraySetValueAtIndex", [m, 4, b]) // at the end: appended
            call("CFArrayReplaceValues", [m, 1, 2, values, 1])
            #expect((0..<4).map { text(call("CFArrayGetValueAtIndex", [m, UInt32($0)])) } == ["c", "a", "c", "b"])
            call("CFArrayAppendArray", [m, arr, 1, 2])
            #expect(call("CFArrayGetCount", [m]) == 6)
            let out = buffer(24)
            call("CFArrayGetValues", [m, 4, 2, out])
            #expect(rd32(out) == b && rd32(out + 4) == c)
            let before = retainCount(b)
            call("CFArrayRemoveAllValues", [m])
            #expect(call("CFArrayGetCount", [m]) == 0 && retainCount(b) < before)
            #expect(bool("CFEqual", [call("CFArrayCreateCopy", [0, arr]), arr]))
            #expect(text(call("CFCopyDescription", [arr]))?.contains("count = 3") == true)

            // No callbacks: plain numbers, sorted and searched by a comparator.
            let nums = call("CFArrayCreateMutable", [0, 0, 0])
            for v: UInt32 in [9, 2, 7, 4, 4] { call("CFArrayAppendValue", [nums, v]) }
            call("CFArraySortValues", [nums, 0, 5, subtract, 0])
            #expect((0..<5).map { call("CFArrayGetValueAtIndex", [nums, UInt32($0)]) } == [2, 4, 4, 7, 9])
            #expect(call("CFArrayBSearchValues", [nums, 0, 5, 7, subtract, 0]) == 3)
            #expect(call("CFArrayBSearchValues", [nums, 0, 5, 5, subtract, 0]) == 3)
            #expect(call("CFArrayBSearchValues", [nums, 0, 5, 100, subtract, 0]) == 5)
            let sum = buffer(4)
            call("CFArrayApplyFunction", [nums, 1, 3, sumInto, sum])
            #expect(rd32(sum) == 15)
        }

        @Test func dictionaries() {
            _ = start()
            let kcb = ppc_data_export("kCFTypeDictionaryKeyCallBacks"), vcb = ppc_data_export("kCFTypeDictionaryValueCallBacks")
            let d = call("CFDictionaryCreateMutable", [0, 0, kcb, vcb])
            let one = call("CFNumberCreate", [0, 3, bytes([0, 0, 0, 1])])
            call("CFDictionaryAddValue", [d, str("one"), one])
            call("CFDictionaryAddValue", [d, str("one"), str("ignored")])
            call("CFDictionarySetValue", [d, str("two"), str("2")])
            call("CFDictionaryReplaceValue", [d, str("three"), str("not added")])
            #expect(call("CFDictionaryGetCount", [d]) == 2)
            #expect(call("CFDictionaryGetValue", [d, str("one")]) == one && retainCount(one) == 2)
            let got = buffer(4)
            #expect(bool("CFDictionaryGetValueIfPresent", [d, str("two"), got]) && text(rd32(got)) == "2")
            #expect(!bool("CFDictionaryGetValueIfPresent", [d, str("three"), got]))
            #expect(bool("CFDictionaryContainsKey", [d, str("two")]) && bool("CFDictionaryContainsValue", [d, str("2")]))
            #expect(call("CFDictionaryGetCountOfKey", [d, str("one")]) == 1 && call("CFDictionaryGetCountOfValue", [d, one]) == 1)
            call("CFDictionaryReplaceValue", [d, str("two"), str("deux")])
            #expect(text(call("CFDictionaryGetValue", [d, str("two")])) == "deux")
            let keys = buffer(8), vals = buffer(8)
            call("CFDictionaryGetKeysAndValues", [d, keys, vals])
            #expect(Set([text(rd32(keys)), text(rd32(keys + 4))]) == ["one", "two"])
            let copy = call("CFDictionaryCreateCopy", [0, d])
            #expect(bool("CFEqual", [copy, d]))
            call("CFDictionaryRemoveValue", [d, str("one")])
            #expect(call("CFDictionaryGetCount", [d]) == 1 && retainCount(one) == 2) // the copy keeps it
            #expect(!bool("CFEqual", [copy, d]))
            call("CFDictionaryRemoveAllValues", [d])
            #expect(call("CFDictionaryGetCount", [d]) == 0)

            // CFDictionaryCreate, and an applier summing the values.
            let ks = buffer(12), vs = buffer(12)
            for (i, v) in [(1, 10), (2, 20), (3, 30)].enumerated() {
                wr32(ks + UInt32(4 * i), UInt32(v.0))
                wr32(vs + UInt32(4 * i), UInt32(v.1))
            }
            let plain = call("CFDictionaryCreate", [0, ks, vs, 3, 0, 0])
            #expect(call("CFDictionaryGetValue", [plain, 2]) == 20)
            let sum = buffer(4)
            let applier = routine([0x80C5_0000, 0x7CC6_2214, 0x90C5_0000, 0x4E80_0020]) // *ctx += value
            call("CFDictionaryApplyFunction", [plain, applier, sum])
            #expect(rd32(sum) == 60)
            // The program's equal and hash: keys alike in their low byte are one key.
            let equal = routine([0x7C63_2278, 0x5463_063E, 0x7C63_0034, 0x5463_D97E, 0x4E80_0020])
            let hash = routine([0x5463_063E, 0x4E80_0020])
            let kc = buffer(24)
            wr32(kc + 16, equal)
            wr32(kc + 20, hash)
            let mine = call("CFDictionaryCreateMutable", [0, 0, kc, 0])
            call("CFDictionarySetValue", [mine, 0x101, 1])
            call("CFDictionarySetValue", [mine, 0x201, 2])
            call("CFDictionarySetValue", [mine, 0x102, 3])
            #expect(call("CFDictionaryGetCount", [mine]) == 2 && call("CFDictionaryGetValue", [mine, 0x301]) == 2)
        }

        @Test func setsAndBags() {
            _ = start()
            let scb = ppc_data_export("kCFTypeSetCallBacks")
            let set = call("CFSetCreateMutable", [0, 0, scb])
            call("CFSetAddValue", [set, str("x")])
            call("CFSetAddValue", [set, str("x")])
            call("CFSetAddValue", [set, str("y")])
            #expect(call("CFSetGetCount", [set]) == 2 && call("CFSetGetCountOfValue", [set, str("x")]) == 1)
            #expect(bool("CFSetContainsValue", [set, str("y")]) && !bool("CFSetContainsValue", [set, str("z")]))
            let y = str("y")
            call("CFSetReplaceValue", [set, y])
            #expect(call("CFSetGetValue", [set, str("y")]) == y)
            let got = buffer(4)
            #expect(bool("CFSetGetValueIfPresent", [set, str("x"), got]) && text(rd32(got)) == "x")
            call("CFSetSetValue", [set, str("z")])
            call("CFSetRemoveValue", [set, str("x")])
            let vals = buffer(12)
            call("CFSetGetValues", [set, vals])
            #expect(Set([text(rd32(vals)), text(rd32(vals + 4))]) == ["y", "z"])
            #expect(bool("CFEqual", [call("CFSetCreateCopy", [0, set]), set]))
            call("CFSetRemoveAllValues", [set])
            #expect(call("CFSetGetCount", [set]) == 0)
            let nums = buffer(12)
            for (i, v) in [5, 6, 5].enumerated() { wr32(nums + UInt32(4 * i), UInt32(v)) }
            let plainSet = call("CFSetCreate", [0, nums, 3, 0])
            #expect(call("CFSetGetCount", [plainSet]) == 2)
            let sum = buffer(4)
            call("CFSetApplyFunction", [plainSet, sumInto, sum])
            #expect(rd32(sum) == 11)

            // A bag counts its values.
            let bag = call("CFBagCreate", [0, nums, 3, 0])
            #expect(call("CFBagGetCount", [bag]) == 3 && call("CFBagGetCountOfValue", [bag, 5]) == 2)
            let m = call("CFBagCreateMutableCopy", [0, 0, bag])
            call("CFBagAddValue", [m, 6])
            call("CFBagRemoveValue", [m, 5])
            #expect(call("CFBagGetCount", [m]) == 3 && call("CFBagGetCountOfValue", [m, 6]) == 2)
            #expect(call("CFBagGetValue", [m, 6]) == 6 && bool("CFBagContainsValue", [m, 5]))
            #expect(bool("CFBagGetValueIfPresent", [m, 5, got]) && rd32(got) == 5)
            call("CFBagGetValues", [m, vals])
            #expect([rd32(vals), rd32(vals + 4), rd32(vals + 8)].sorted() == [5, 6, 6])
            wr32(sum, 0)
            call("CFBagApplyFunction", [m, sumInto, sum])
            #expect(rd32(sum) == 17)
            call("CFBagSetValue", [m, 7])
            call("CFBagReplaceValue", [m, 7])
            #expect(call("CFBagGetCount", [m]) == 4 && !bool("CFEqual", [m, bag]))
            #expect(call("CFBagGetCount", [call("CFBagCreateCopy", [0, m])]) == 4)
            let tb = ppc_data_export("kCFTypeBagCallBacks")
            let sbag = call("CFBagCreateMutable", [0, 0, tb])
            let s = str("s")
            call("CFBagAddValue", [sbag, s])
            call("CFBagAddValue", [sbag, s])
            #expect(retainCount(s) == 3)
            call("CFBagRemoveAllValues", [sbag])
            #expect(retainCount(s) == 1)
            // kCFCopyString...: a copy of a mutable string kept.
            let copying = call("CFSetCreateMutable", [0, 0, ppc_data_export("kCFCopyStringSetCallBacks")])
            let ms = call("CFStringCreateMutableCopy", [0, 0, str("orig")])
            call("CFSetAddValue", [copying, ms])
            call("CFStringAppend", [ms, str("!")])
            #expect(bool("CFSetContainsValue", [copying, str("orig")]))
        }

        @Test func trees() {
            _ = start()
            func tree(_ info: UInt32) -> UInt32 {
                let ctx = buffer(20)
                wr32(ctx + 4, info)
                return call("CFTreeCreate", [0, ctx])
            }
            let root = tree(100), a = tree(3), b = tree(1), c = tree(2)
            call("CFTreeAppendChild", [root, a])
            call("CFTreeAppendChild", [root, b])
            call("CFTreePrependChild", [root, c])
            #expect(call("CFTreeGetChildCount", [root]) == 3 && retainCount(a) == 2)
            #expect(call("CFTreeGetFirstChild", [root]) == c && call("CFTreeGetNextSibling", [c]) == a)
            #expect(call("CFTreeGetParent", [b]) == root && call("CFTreeGetNextSibling", [b]) == 0)
            let d = tree(4)
            call("CFTreeAppendChild", [b, d])
            #expect(call("CFTreeFindRoot", [d]) == root)
            call("CFTreeSortChildren", [root, subtract, 0])
            #expect((0..<3).map { call("CFTreeGetChildAtIndex", [root, UInt32($0)]) } == [b, c, a])
            let kids = buffer(12)
            call("CFTreeGetChildren", [root, kids])
            #expect(rd32(kids + 8) == a)
            let e = tree(5)
            call("CFTreeInsertSibling", [c, e])
            #expect(call("CFTreeGetNextSibling", [c]) == e && call("CFTreeGetChildCount", [root]) == 4)
            let ctx = buffer(20)
            call("CFTreeGetContext", [e, ctx])
            #expect(rd32(ctx + 4) == 5)
            wr32(ctx + 4, 50)
            call("CFTreeSetContext", [e, ctx])
            call("CFTreeGetContext", [e, ctx])
            #expect(rd32(ctx + 4) == 50)
            let sum = buffer(4)
            call("CFTreeApplyFunctionToChildren", [root, routine([0x4E80_0020]), sum])
            call("CFTreeRemove", [e])
            #expect(call("CFTreeGetChildCount", [root]) == 3 && call("CFTreeGetParent", [e]) == 0 && retainCount(e) == 1)
            call("CFTreeRemoveAllChildren", [root])
            #expect(call("CFTreeGetChildCount", [root]) == 0 && retainCount(a) == 1 && call("CFTreeGetParent", [a]) == 0)
        }

        @Test func data() {
            _ = start()
            let d = call("CFDataCreate", [0, bytes([1, 2, 3, 4]), 4])
            #expect(call("CFDataGetLength", [d]) == 4 && read(call("CFDataGetBytePtr", [d]), 4) == [1, 2, 3, 4])
            let m = call("CFDataCreateMutableCopy", [0, 0, d])
            call("CFDataAppendBytes", [m, bytes([5, 6]), 2])
            call("CFDataReplaceBytes", [m, 1, 2, bytes([9]), 1])
            #expect(read(call("CFDataGetMutableBytePtr", [m]), 5) == [1, 9, 4, 5, 6])
            call("CFDataDeleteBytes", [m, 0, 2])
            call("CFDataIncreaseLength", [m, 2])
            #expect(read(call("CFDataGetBytePtr", [m]), 5) == [4, 5, 6, 0, 0])
            call("CFDataSetLength", [m, 2])
            let out = buffer(4)
            call("CFDataGetBytes", [m, 1, 1, out])
            #expect(call("CFDataGetLength", [m]) == 2 && rd8(out) == 5)
            let big = call("CFDataCreateMutable", [0, 0])
            for _ in 0..<100 { call("CFDataAppendBytes", [big, bytes(Array(repeating: 7, count: 100)), 100]) }
            #expect(call("CFDataGetLength", [big]) == 10_000 && rd8(call("CFDataGetBytePtr", [big]) + 9_999) == 7)
            let raw = bytes([8, 8])
            let nocopy = call("CFDataCreateWithBytesNoCopy", [0, raw, 2, data("kCFAllocatorNull")])
            #expect(call("CFDataGetBytePtr", [nocopy]) == raw)
            #expect(bool("CFEqual", [call("CFDataCreateCopy", [0, d]), d]) && !bool("CFEqual", [d, m]))
            #expect(text(call("CFCopyDescription", [d]))?.contains("bytes = 0x01020304") == true)
        }

        // MARK: Numbers, Booleans, dates, UUIDs

        @Test func numbersAndBooleans() {
            _ = start()
            let i = call("CFNumberCreate", [0, 9, bytes([0xFF, 0xFF, 0xFF, 0xFE])]) // kCFNumberIntType, -2
            #expect(call("CFNumberGetType", [i]) == 3 && call("CFNumberGetByteSize", [i]) == 4 && !bool("CFNumberIsFloatType", [i]))
            let out = buffer(8)
            #expect(bool("CFNumberGetValue", [i, 2, out]) && rd16(out) == 0xFFFE)
            #expect(bool("CFNumberGetValue", [i, 4, out]) && rd32(out) == 0xFFFF_FFFF && rd32(out + 4) == 0xFFFF_FFFE)
            #expect(bool("CFNumberGetValue", [i, 6, out]) && double(out) == -2)
            let big = call("CFNumberCreate", [0, 3, bytes([0, 1, 0, 0])])
            #expect(!bool("CFNumberGetValue", [big, 1, out])) // lossy
            let pi = call("CFNumberCreate", [0, 13, bytes(words(3.5).flatMap { be32($0) })])
            #expect(call("CFNumberGetType", [pi]) == 6 && bool("CFNumberIsFloatType", [pi]))
            #expect(!bool("CFNumberGetValue", [pi, 3, out]) && rd32(out) == 3)
            #expect(bool("CFNumberGetValue", [pi, 5, out]) && Float(bitPattern: rd32(out)) == 3.5)
            #expect(int("CFNumberCompare", [i, pi, 0]) == -1 && int("CFNumberCompare", [pi, i, 0]) == 1)
            let two = call("CFNumberCreate", [0, 1, bytes([2])])
            let twoF = call("CFNumberCreate", [0, 12, bytes(be32(Float(2).bitPattern))])
            #expect(bool("CFEqual", [two, twoF]) && call("CFHash", [two]) == call("CFHash", [twoF]))
            let ll = call("CFNumberCreate", [0, 11, bytes([0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0])])
            #expect(call("CFNumberGetType", [ll]) == 4 && call("CFNumberGetByteSize", [ll]) == 8)
            #expect(call("CFNumberCreate", [0, 99, out]) == 0)
            let nan = data("kCFNumberNaN"), inf = data("kCFNumberPositiveInfinity")
            #expect(bool("CFNumberGetValue", [nan, 6, out]) && double(out).isNaN)
            #expect(bool("CFNumberGetValue", [inf, 6, out]) && double(out) == .infinity)
            #expect(bool("CFNumberGetValue", [data("kCFNumberNegativeInfinity"), 6, out]) && double(out) == -.infinity)

            let t = data("kCFBooleanTrue"), f = data("kCFBooleanFalse")
            #expect(call("CFGetTypeID", [t]) == call("CFBooleanGetTypeID") && t != f)
            #expect(bool("CFBooleanGetValue", [t]) && !bool("CFBooleanGetValue", [f]))
            call("CFRelease", [t])
            #expect(bool("CFBooleanGetValue", [t]))
        }

        @Test func datesAndTheCalendar() {
            _ = start()
            #expect(double(ppc_data_export("kCFAbsoluteTimeIntervalSince1970")) == 978_307_200)
            #expect(double(ppc_data_export("kCFAbsoluteTimeIntervalSince1904")) == 3_061_152_000)
            call("CFAbsoluteTimeGetCurrent")
            #expect(abs(f1 - (Date().timeIntervalSince1970 - 978_307_200)) < 5)

            let res = buffer(32)
            func gregorian(_ at: Double) -> (Int32, UInt8, UInt8, UInt8, UInt8, Double) {
                call("CFAbsoluteTimeGetGregorianDate", [res, 0, 0, 0], fp: [at])
                return (Int32(bitPattern: rd32(res)), rd8(res + 4), rd8(res + 5), rd8(res + 6), rd8(res + 7), double(res + 8))
            }
            #expect(gregorian(0) == (2001, 1, 1, 0, 0, 0))
            #expect(gregorian(-1) == (2000, 12, 31, 23, 59, 59))
            #expect(gregorian(97_072_215.5) == (2004, 1, 29, 12, 30, 15.5))
            func gdate(_ y: Int, _ mo: Int, _ d: Int, _ h: Int, _ mi: Int, _ s: Double) -> [UInt32] {
                [UInt32(y), UInt32(mo << 24 | d << 16 | h << 8 | mi)] + words(s)
            }
            call("CFGregorianDateGetAbsoluteTime", gdate(2004, 1, 29, 12, 30, 15.5) + [0])
            #expect(f1 == 97_072_215.5)
            #expect(bool("CFGregorianDateIsValid", gdate(2004, 2, 29, 23, 59, 59) + [0xFF_FFFF]))
            #expect(!bool("CFGregorianDateIsValid", gdate(2001, 2, 29, 0, 0, 0) + [0xFF_FFFF]))
            #expect(bool("CFGregorianDateIsValid", gdate(2001, 2, 29, 0, 0, 0) + [2]))
            #expect(!bool("CFGregorianDateIsValid", gdate(2001, 1, 1, 24, 0, 0) + [8]))

            // 31 January 2001 and a month: 3 March (February has 28 days).
            let jan31 = 30 * 86400.0
            call("CFAbsoluteTimeAddGregorianUnits", [0, 0, 0, 0, 1, 0, 0, 0] + words(0.5), fp: [jan31])
            #expect(gregorian(f1) == (2001, 3, 3, 0, 0, 0.5))
            call("CFAbsoluteTimeAddGregorianUnits", [0, 0, 0, 1, 0, 0, 0, 0, 0, 0], fp: [0])
            #expect(gregorian(f1).0 == 2002)
            call("CFAbsoluteTimeAddGregorianUnits", [0, 0, 0, 0, 0, UInt32(bitPattern: -1), 0, 0, 0, 0], fp: [0])
            #expect(gregorian(f1) == (2000, 12, 31, 0, 0, 0))

            let later = 400 * 86400.0 + 3 * 3600 + 30
            call("CFAbsoluteTimeGetDifferenceAsGregorianUnits", [res, 0, 0, 0, 0, 0, 0x3F], fp: [later, 0])
            #expect((0..<5).map { rd32(res + UInt32(4 * $0)) } == [1, 1, 4, 3, 0] && double(res + 20) == 30)
            call("CFAbsoluteTimeGetDifferenceAsGregorianUnits", [res, 0, 0, 0, 0, 0, 4], fp: [later, 0])
            #expect(rd32(res + 8) == 400)

            #expect(call("CFAbsoluteTimeGetDayOfWeek", [0, 0, 0], fp: [0]) == 1)  // a Monday
            #expect(call("CFAbsoluteTimeGetDayOfWeek", [0, 0, 0], fp: [6 * 86400]) == 7)
            #expect(call("CFAbsoluteTimeGetDayOfYear", [0, 0, 0], fp: [jan31]) == 31)
            #expect(call("CFAbsoluteTimeGetWeekOfYear", [0, 0, 0], fp: [0]) == 1)
            #expect(call("CFAbsoluteTimeGetWeekOfYear", [0, 0, 0], fp: [-86400]) == 52) // 31 Dec 2000

            let d1 = call("CFDateCreate", [0], fp: [100]), d2 = call("CFDateCreate", [0], fp: [250.5])
            call("CFDateGetAbsoluteTime", [d2])
            #expect(f1 == 250.5)
            call("CFDateGetTimeIntervalSinceDate", [d2, d1])
            #expect(f1 == 150.5)
            #expect(int("CFDateCompare", [d1, d2, 0]) == -1 && int("CFDateCompare", [d2, d2, 0]) == 0)
            #expect(bool("CFEqual", [d1, call("CFDateCreate", [0], fp: [100])]))
        }

        @Test func uuids() {
            _ = start()
            let s = "68753A44-4D6F-1226-9C60-0050E4C00067"
            let u = call("CFUUIDCreateFromString", [0, str(s)])
            #expect(text(call("CFUUIDCreateString", [0, u])) == s)
            #expect(call("CFUUIDCreateFromString", [0, str(s.lowercased())]) == u) // unique by bytes
            #expect(call("CFUUIDCreateFromString", [0, str("not a uuid")]) == 0)
            let res = buffer(16)
            call("CFUUIDGetUUIDBytes", [res, u])
            #expect(read(res, 4) == [0x68, 0x75, 0x3A, 0x44] && rd8(res + 15) == 0x67)
            #expect(call("CFUUIDCreateFromUUIDBytes", [0, rd32(res), rd32(res + 4), rd32(res + 8), rd32(res + 12)]) == u)
            let bytes16: [UInt32] = [0x68, 0x75, 0x3A, 0x44, 0x4D, 0x6F, 0x12, 0x26, 0x9C, 0x60, 0, 0x50, 0xE4, 0xC0, 0, 0x67]
            #expect(call("CFUUIDCreateWithBytes", [0] + bytes16) == u)
            let unknown: [UInt32] = [0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46]
            let k = call("CFUUIDGetConstantUUIDWithBytes", [0] + unknown)
            #expect(k != 0 && call("CFUUIDGetConstantUUIDWithBytes", [0] + unknown) == k)
            call("CFRelease", [k])
            #expect(text(call("CFUUIDCreateString", [0, k])) == "00000000-0000-0000-C000-000000000046")
            let r1 = call("CFUUIDCreate", [0]), r2 = call("CFUUIDCreate", [0])
            #expect(r1 != r2 && !bool("CFEqual", [r1, r2]))
            call("CFUUIDGetUUIDBytes", [res, r1])
            #expect(rd8(res + 6) >> 4 == 4 && rd8(res + 8) >> 6 == 2)
        }

        @Test func math64() {
            _ = start()
            func s(_ v: Int64) -> [UInt32] { words(UInt64(bitPattern: v)) }
            #expect(wide("S64Max", []) == UInt64(Int64.max) && wide("S64Min", []) == UInt64(bitPattern: Int64.min))
            #expect(wide("U64Max", []) == UInt64.max)
            #expect(Int64(bitPattern: wide("S64Add", s(-5) + s(3))) == -2)
            #expect(Int64(bitPattern: wide("S64Subtract", s(1) + s(1 << 40))) == 1 - (1 << 40))
            #expect(Int64(bitPattern: wide("S64Multiply", s(-3) + s(1 << 33))) == -3 << 33)
            let rem = buffer(8)
            #expect(Int64(bitPattern: wide("S64Divide", s(-7) + s(2) + [rem])) == -3)
            #expect(Int64(bitPattern: UInt64(rd32(rem)) << 32 | UInt64(rd32(rem + 4))) == -1)
            #expect(Int64(bitPattern: wide("S64Mod", s(-7) + s(2))) == -1)
            #expect(Int64(bitPattern: wide("S64Negate", s(9))) == -9 && wide("S64Absolute", s(-9)) == 9)
            #expect(Int64(bitPattern: wide("S64Set", [UInt32(bitPattern: -4)])) == -4 && wide("S64SetU", [0xFFFF_FFFF]) == 0xFFFF_FFFF)
            #expect(call("S32Set", s(0x1_2345_6789)) == 0x2345_6789)
            #expect(call("S64And", s(1) + s(0)) == 0 && call("S64Or", s(1) + s(0)) == 1)
            #expect(call("S64Eor", s(1) + s(2)) == 0 && call("S64Not", s(0)) == 1)
            #expect(int("S64Compare", s(-1) + s(1)) == -1 && int("U64Compare", s(-1) + s(1)) == 1)
            #expect(wide("S64BitwiseAnd", s(0xF0F) + s(0xFF)) == 0xF && wide("S64BitwiseOr", s(0xF00) + s(0xF)) == 0xF0F)
            #expect(wide("S64BitwiseEor", s(0xFF) + s(0xF)) == 0xF0 && wide("S64BitwiseNot", s(0)) == UInt64.max)
            #expect(Int64(bitPattern: wide("S64ShiftRight", s(-256) + [4])) == -16 && wide("U64ShiftRight", s(-256) + [60]) == 15)
            #expect(wide("S64ShiftLeft", s(3) + [62]) == 0xC000_0000_0000_0000 && wide("U64ShiftLeft", s(3) + [64]) == 0)
            #expect(wide("U64Add", s(-1) + s(2)) == 1 && wide("U64Subtract", s(0) + s(1)) == UInt64.max)
            #expect(wide("U64Multiply", s(1 << 32) + s(1 << 31)) == 1 << 63)
            #expect(wide("U64Divide", s(-1) + s(16) + [rem]) == UInt64.max / 16 && rd32(rem + 4) == 15)
            #expect(wide("U64Mod", s(100) + s(7)) == 2 && wide("U64Set", [UInt32(bitPattern: -1)]) == UInt64.max)
            #expect(wide("U64SetU", [5]) == 5 && call("U32SetU", s(0x7_0000_0001)) == 1)
            #expect(call("U64And", s(2) + s(3)) == 1 && call("U64Or", s(0) + s(0)) == 0)
            #expect(call("U64Eor", s(0) + s(3)) == 1 && call("U64Not", s(3)) == 0)
            #expect(wide("U64BitwiseAnd", s(6) + s(3)) == 2 && wide("U64BitwiseOr", s(6) + s(3)) == 7)
            #expect(wide("U64BitwiseEor", s(6) + s(3)) == 5 && wide("U64BitwiseNot", s(-1)) == 0)
            #expect(wide("UInt64ToSInt64", s(-1)) == UInt64.max && wide("SInt64ToUInt64", s(7)) == 7)
            // long double: a double-double, exact beyond a double's 53 bits.
            call("SInt64ToLongDouble", s(Int64.max))
            #expect(f1 == 9.223372036854775807e18 && f2 == -1)
            call("UInt64ToLongDouble", words(UInt64.max))
            #expect(f1 + f2 == 1.8446744073709552e19 && f2 == -1)
            #expect(Int64(bitPattern: wide("LongDoubleToSInt64", [], fp: [9.223372036854775807e18, -1])) == Int64.max)
            #expect(Int64(bitPattern: wide("LongDoubleToSInt64", [], fp: [-2.75, 0])) == -2)
            #expect(Int64(bitPattern: wide("LongDoubleToSInt64", [], fp: [1e30, 0])) == Int64.max)
            #expect(wide("LongDoubleToUInt64", [], fp: [-5, 0]) == 0 && wide("LongDoubleToUInt64", [], fp: [42.9, 0]) == 42)
        }

        func wide(_ name: String, _ args: [UInt32], fp: [Double]) -> UInt64 {
            let hi = call(name, args, fp: fp)
            return UInt64(hi) << 32 | UInt64(r4)
        }

        // MARK: URLs

        @Test func urlsTakenApartAndResolved() {
            _ = start()
            let u = call("CFURLCreateWithString", [0, str("http://user:pw@www.example.com:8080/a/b/c.html;p?q=1#frag"), 0])
            #expect(text(call("CFURLGetString", [u])) == "http://user:pw@www.example.com:8080/a/b/c.html;p?q=1#frag")
            #expect(bool("CFURLCanBeDecomposed", [u]) && call("CFURLGetBaseURL", [u]) == 0)
            #expect(text(call("CFURLCopyScheme", [u])) == "http")
            #expect(text(call("CFURLCopyNetLocation", [u])) == "user:pw@www.example.com:8080")
            #expect(text(call("CFURLCopyHostName", [u])) == "www.example.com" && call("CFURLGetPortNumber", [u]) == 8080)
            #expect(text(call("CFURLCopyUserName", [u])) == "user" && text(call("CFURLCopyPassword", [u])) == "pw")
            #expect(text(call("CFURLCopyPath", [u])) == "/a/b/c.html")
            let abs = buffer(1)
            #expect(text(call("CFURLCopyStrictPath", [u, abs])) == "a/b/c.html" && rd8(abs) == 1)
            #expect(text(call("CFURLCopyParameterString", [u, str("")])) == "p")
            #expect(text(call("CFURLCopyQueryString", [u, str("")])) == "q=1")
            #expect(text(call("CFURLCopyFragment", [u, str("")])) == "frag")
            #expect(text(call("CFURLCopyResourceSpecifier", [u])) == ";p?q=1#frag")
            #expect(text(call("CFURLCopyLastPathComponent", [u])) == "c.html")
            #expect(text(call("CFURLCopyPathExtension", [u])) == "html")
            #expect(!bool("CFURLHasDirectoryPath", [u]))
            #expect(int("CFURLGetPortNumber", [call("CFURLCreateWithString", [0, str("ftp://host/"), 0])]) == -1)

            let mail = call("CFURLCreateWithString", [0, str("mailto:someone@example.com"), 0])
            #expect(!bool("CFURLCanBeDecomposed", [mail]) && call("CFURLCopyPath", [mail]) == 0)
            #expect(text(call("CFURLCopyResourceSpecifier", [mail])) == "someone@example.com")
            #expect(call("CFURLCreateWithString", [0, str("has space"), 0]) == 0)
            #expect(call("CFURLCreateWithString", [0, str("bad%2"), 0]) == 0)

            // RFC 1808's examples.
            let base = call("CFURLCreateWithString", [0, str("http://a/b/c/d;p?q#f"), 0])
            let cases = [("g", "http://a/b/c/g"), ("./g", "http://a/b/c/g"), ("g/", "http://a/b/c/g/"),
                         ("/g", "http://a/g"), ("//g", "http://g"), ("?y", "http://a/b/c/d;p?y"),
                         ("g?y", "http://a/b/c/g?y"), ("#s", "http://a/b/c/d;p?q#s"), ("g#s", "http://a/b/c/g#s"),
                         (";x", "http://a/b/c/d;x"), ("g;x", "http://a/b/c/g;x"), (".", "http://a/b/c/"),
                         ("..", "http://a/b/"), ("../g", "http://a/b/g"), ("../..", "http://a/"),
                         ("../../g", "http://a/g"), ("g:h", "g:h")]
            for (rel, want) in cases {
                let r = call("CFURLCreateWithString", [0, str(rel), base])
                let a = call("CFURLCopyAbsoluteURL", [r])
                #expect(text(call("CFURLGetString", [a])) == want, "\(rel) -> \(text(call("CFURLGetString", [a])) ?? "nil")")
            }
            let rel = call("CFURLCreateWithString", [0, str("g"), base])
            #expect(call("CFURLGetBaseURL", [rel]) == base && text(call("CFURLCopyScheme", [rel])) == "http")
            #expect(text(call("CFURLCopyNetLocation", [rel])) == "a" && text(call("CFURLCopyPath", [rel])) == "g")
            #expect(!bool("CFEqual", [rel, call("CFURLCreateWithString", [0, str("http://a/b/c/g"), 0])]))
            #expect(bool("CFEqual", [rel, call("CFURLCreateWithString", [0, str("g"), base])]))

            // Copies with their paths changed.
            let dir = call("CFURLCreateWithString", [0, str("file://localhost/Disk/Folder/"), 0])
            let file = call("CFURLCreateCopyAppendingPathComponent", [0, dir, str("My File#1"), 0])
            #expect(text(call("CFURLGetString", [file])) == "file://localhost/Disk/Folder/My%20File%231")
            #expect(text(call("CFURLCopyLastPathComponent", [file])) == "My File#1")
            let ext = call("CFURLCreateCopyAppendingPathExtension", [0, file, str("txt")])
            #expect(text(call("CFURLGetString", [ext])) == "file://localhost/Disk/Folder/My%20File%231.txt")
            #expect(text(call("CFURLGetString", [call("CFURLCreateCopyDeletingPathExtension", [0, ext])])) ==
                    "file://localhost/Disk/Folder/My%20File%231")
            #expect(text(call("CFURLGetString", [call("CFURLCreateCopyDeletingLastPathComponent", [0, ext])])) ==
                    "file://localhost/Disk/Folder/")
            #expect(text(call("CFURLGetString", [call("CFURLCreateCopyAppendingPathComponent", [0, dir, str("Sub"), 1])])) ==
                    "file://localhost/Disk/Folder/Sub/")
            #expect(bool("CFURLHasDirectoryPath", [dir]))

            // Escapes.
            let escaped = call("CFURLCreateStringByAddingPercentEscapes", [0, str("a b/ç?&"), 0, str("&"), 0x0800_0100])
            #expect(text(escaped) == "a%20b/%C3%A7?%26")
            #expect(text(call("CFURLCreateStringByAddingPercentEscapes", [0, str("a b"), str(" "), 0, 0x0800_0100])) == "a b")
            #expect(text(call("CFURLCreateStringByReplacingPercentEscapes", [0, escaped, str("")])) == "a b/ç?&")
            #expect(text(call("CFURLCreateStringByReplacingPercentEscapes", [0, escaped, str("&")])) == "a b/ç?%26")
            #expect(call("CFURLCreateStringByReplacingPercentEscapes", [0, escaped, 0]) == escaped)
            #expect(call("CFURLCreateStringByReplacingPercentEscapes", [0, str("%zz"), str("")]) == 0)
            let fromBytes = call("CFURLCreateWithBytes", [0, bytes(Array("http://x/caf".utf8) + [0x8E]), 13, 0, 0])
            #expect(text(call("CFURLGetString", [fromBytes])) == "http://x/caf%8E")
            let urlData = call("CFURLCreateData", [0, u, 0x0800_0100, 1])
            #expect(call("CFDataGetLength", [urlData]) == 57)
        }

        @Test func urlsOfFilesAndPaths() throws {
            let dir = start()
            // The folder the application is in, as the File Manager names it.
            let bundleURL = call("CFBundleCopyBundleURL", [call("CFBundleGetMainBundle")])
            let hfs = text(call("CFURLCopyFileSystemPath", [bundleURL, 1])) ?? ""
            #expect(hfs.contains(":") && !hfs.hasSuffix(":"))
            let posix = text(call("CFURLCopyFileSystemPath", [bundleURL, 0])) ?? ""
            #expect(posix.hasPrefix("/") && posix.replacingOccurrences(of: "/", with: ":").dropFirst() == hfs[...])
            #expect(text(call("CFURLGetString", [bundleURL]))?.hasPrefix("file://localhost/") == true)
            #expect(bool("CFURLHasDirectoryPath", [bundleURL]))

            // An HFS path to a URL and back.
            let notes = call("CFURLCreateWithFileSystemPath", [0, str(hfs + ":Notes Ω"), 1, 0])
            #expect(text(call("CFURLCopyFileSystemPath", [notes, 1])) == hfs + ":Notes Ω")
            #expect(text(call("CFURLCopyLastPathComponent", [notes])) == "Notes Ω")
            let volume = String(hfs.split(separator: ":")[0])
            let root = call("CFURLCreateWithFileSystemPath", [0, str(volume + ":"), 1, 1])
            #expect(text(call("CFURLGetString", [root])) == "file://localhost/" + volume.replacingOccurrences(of: " ", with: "%20") + "/")
            #expect(text(call("CFURLCopyFileSystemPath", [root, 1])) == volume + ":")
            let relative = call("CFURLCreateWithFileSystemPath", [0, str(":Sub:File"), 1, 0])
            #expect(text(call("CFURLGetString", [relative])) == "Sub/File" && call("CFURLGetBaseURL", [relative]) != 0)
            let viaBase = call("CFURLCreateWithFileSystemPathRelativeToBase", [0, str("x/y"), 0, 1, bundleURL])
            #expect(call("CFURLGetBaseURL", [viaBase]) == bundleURL && text(call("CFURLGetString", [viaBase])) == "x/y/")
            let win = call("CFURLCreateWithFileSystemPath", [0, str("C:\\Dir\\f.txt"), 2, 0])
            #expect(text(call("CFURLGetString", [win])) == "file://localhost/C:/Dir/f.txt")
            #expect(text(call("CFURLCopyFileSystemPath", [win, 2])) == "C:\\Dir\\f.txt")

            // The file system's representation: an HFS path in Mac OS Roman.
            let buf = buffer(512)
            #expect(bool("CFURLGetFileSystemRepresentation", [notes, 1, buf, 512]))
            let macLen = Int(call("CFStringGetBytes", [str(hfs), 0, UInt32(hfs.utf16.count), 0, 0, 0, 0, 0, buffer(4)]))
            let rep = read(buf, macLen + 9)
            #expect(rep.last == 0 && rep[rep.count - 2] == 0xBD) // Ω
            let back = call("CFURLCreateFromFileSystemRepresentation", [0, buf, UInt32(macLen + 8), 0])
            #expect(bool("CFEqual", [back, notes]))
            #expect(!bool("CFURLGetFileSystemRepresentation", [notes, 1, buf, 4]))
            let fromRelRep = call("CFURLCreateFromFileSystemRepresentationRelativeToBase", [0, cstr(":Sub"), 4, 1, bundleURL])
            #expect(text(call("CFURLGetString", [fromRelRep])) == "Sub/")

            // FSRefs.
            try Data("note".utf8).write(to: dir.appendingPathComponent("Notes Ω"))
            let ref = buffer(80)
            #expect(bool("CFURLGetFSRef", [notes, ref]))
            let fromRef = call("CFURLCreateFromFSRef", [0, ref])
            #expect(bool("CFEqual", [fromRef, notes]))
            let missing = call("CFURLCreateCopyAppendingPathComponent", [0, bundleURL, str("Nowhere"), 0])
            #expect(!bool("CFURLGetFSRef", [missing, ref]))
            #expect(bool("CFURLGetFSRef", [bundleURL, ref]))
            #expect(bool("CFEqual", [call("CFURLCreateFromFSRef", [0, ref]), bundleURL]))
        }

        @Test func urlAccessReadsAndWritesFiles() throws {
            let dir = start()
            let folder = call("CFBundleCopyBundleURL", [call("CFBundleGetMainBundle")])
            let file = call("CFURLCreateCopyAppendingPathComponent", [0, folder, str("Data File"), 0])
            let err = buffer(4)
            let payload = call("CFDataCreate", [0, bytes(Array("Hello, file".utf8)), 11])
            #expect(bool("CFURLWriteDataAndPropertiesToResource", [file, payload, 0, err]) && rd32(err) == 0)
            #expect(try String(contentsOf: dir.appendingPathComponent("Data File"), encoding: .utf8) == "Hello, file")

            let dataOut = buffer(4), props = buffer(4)
            #expect(bool("CFURLCreateDataAndPropertiesFromResource", [0, file, dataOut, props, 0, err]))
            #expect(bool("CFEqual", [rd32(dataOut), payload]))
            let p = rd32(props)
            #expect(call("CFDictionaryGetValue", [p, data("kCFURLFileExists")]) == data("kCFBooleanTrue"))
            let len = call("CFDictionaryGetValue", [p, data("kCFFileURLSize")])
            let n = buffer(4)
            #expect(bool("CFNumberGetValue", [len, 3, n]) && rd32(n) == 11)
            let mod = call("CFDictionaryGetValue", [p, data("kCFURLFileLastModificationTime")])
            call("CFDateGetAbsoluteTime", [mod])
            #expect(abs(f1 - (Date().timeIntervalSince1970 - 978_307_200)) < 120)
            #expect(call("CFDictionaryGetValue", [p, data("kCFURLFilePOSIXMode")]) != 0)

            // One property; the folder's contents.
            let exists = call("CFURLCreatePropertyFromResource", [0, file, data("kCFURLFileExists"), err])
            #expect(exists == data("kCFBooleanTrue"))
            let list = call("CFURLCreatePropertyFromResource", [0, folder, data("kCFURLFileDirectoryContents"), err])
            let names = (0..<call("CFArrayGetCount", [list])).map {
                text(call("CFURLCopyLastPathComponent", [call("CFArrayGetValueAtIndex", [list, $0])]))
            }
            #expect(names.contains("Data File") && names.contains("Test App"))
            #expect(call("CFURLCreatePropertyFromResource", [0, file, str("bogus"), err]) == 0 && Int32(bitPattern: rd32(err)) == -16)

            // A folder made, things destroyed, things not there.
            let sub = call("CFURLCreateCopyAppendingPathComponent", [0, folder, str("New Folder"), 1])
            #expect(bool("CFURLWriteDataAndPropertiesToResource", [sub, 0, 0, err]))
            var isDir: ObjCBool = false
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("New Folder").path, isDirectory: &isDir) && isDir.boolValue)
            #expect(bool("CFURLDestroyResource", [file, err]))
            #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("Data File").path))
            #expect(!bool("CFURLCreateDataAndPropertiesFromResource", [0, file, dataOut, props, 0, err]))
            #expect(Int32(bitPattern: rd32(err)) == -12)
            #expect(call("CFDictionaryGetValue", [rd32(props), data("kCFURLFileExists")]) == data("kCFBooleanFalse"))
            #expect(!bool("CFURLDestroyResource", [file, err]) && Int32(bitPattern: rd32(err)) == -12)
            let http = call("CFURLCreateWithString", [0, str("http://www.apple.com/"), 0])
            #expect(!bool("CFURLCreateDataAndPropertiesFromResource", [0, http, dataOut, 0, 0, err]))
            #expect(Int32(bitPattern: rd32(err)) == -11)
        }

        // MARK: Bundles and plug-ins

        static let infoPlist = """
            <?xml version="1.0" encoding="UTF-8"?>
            <!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
            <plist version="1.0">
            <dict>
            \t<key>CFBundleIdentifier</key>
            \t<string>com.example.cftest</string>
            \t<key>CFBundleVersion</key>
            \t<string>1.2.3d4</string>
            \t<key>CFBundleDevelopmentRegion</key>
            \t<string>English</string>
            \t<key>CFBundleName</key>
            \t<string>CF Test</string>
            </dict>
            </plist>
            """

        @Test func theMainBundleIsTheApplication() throws {
            let dir = launchTestApp([TestResource(type: "plst", id: 0, data: Array(Self.infoPlist.utf8))])
            giveStack()
            let main = call("CFBundleGetMainBundle")
            #expect(main != 0 && call("CFBundleGetMainBundle") == main)
            #expect(text(call("CFBundleGetIdentifier", [main])) == "com.example.cftest")
            #expect(call("CFBundleGetVersionNumber", [main]) == 0x0123_2004)
            #expect(text(call("CFBundleGetDevelopmentRegion", [main])) == "English")
            #expect(text(call("CFBundleGetValueForInfoDictionaryKey", [main, str("CFBundleName")])) == "CF Test")
            #expect(call("CFDictionaryGetCount", [call("CFBundleGetInfoDictionary", [main])]) == 4)
            #expect(call("CFBundleGetBundleWithIdentifier", [str("com.example.cftest")]) == main)
            #expect(call("CFBundleGetBundleWithIdentifier", [str("com.example.none")]) == 0)
            #expect(call("CFArrayGetCount", [call("CFBundleGetAllBundles")]) >= 1)
            let exe = call("CFBundleCopyExecutableURL", [main])
            #expect(text(call("CFURLCopyLastPathComponent", [exe])) == "Test App")
            #expect(bool("CFBundleIsExecutableLoaded", [main]))
            let type = buffer(8)
            call("CFBundleGetPackageInfo", [main, type, type + 4])
            #expect(rd32(type) != 0)
            #expect(int("CFBundleOpenBundleResourceMap", [main]) == Int32(Int16(bitPattern: rd16(UInt32(LM_CurApRefNum)))))

            // Its resources: files beside it, and its localizations.
            try Data("readme".utf8).write(to: dir.appendingPathComponent("Readme.txt"))
            let lproj = dir.appendingPathComponent("English.lproj")
            try FileManager.default.createDirectory(at: lproj, withIntermediateDirectories: true)
            try Data("/* comment */\n\"Hello\" = \"Hi there\";\n\"Bye\" = \"So long\";\n".utf8)
                .write(to: lproj.appendingPathComponent("Localizable.strings"))
            try Data("\"CFBundleName\" = \"Localized Name\";".utf8).write(to: lproj.appendingPathComponent("InfoPlist.strings"))
            let readme = call("CFBundleCopyResourceURL", [main, str("Readme"), str("txt"), 0])
            #expect(text(call("CFURLCopyLastPathComponent", [readme])) == "Readme.txt")
            #expect(call("CFBundleCopyResourceURL", [main, str("Missing"), str("txt"), 0]) == 0)
            let strings = call("CFBundleCopyResourceURL", [main, str("Localizable"), str("strings"), 0])
            #expect(text(call("CFURLCopyFileSystemPath", [strings, 0]))?.hasSuffix("/English.lproj/Localizable.strings") == true)
            let txts = call("CFBundleCopyResourceURLsOfType", [main, str("txt"), 0])
            #expect(call("CFArrayGetCount", [txts]) == 1)
            #expect(text(call("CFBundleCopyLocalizedString", [main, str("Hello"), str("default"), 0])) == "Hi there")
            #expect(text(call("CFBundleCopyLocalizedString", [main, str("Other"), str("default"), 0])) == "default")
            #expect(text(call("CFBundleCopyLocalizedString", [main, str("Other"), 0, 0])) == "Other")
            #expect(text(call("CFBundleGetValueForInfoDictionaryKey", [main, str("CFBundleName")])) == "Localized Name" ||
                    text(call("CFBundleGetValueForInfoDictionaryKey", [main, str("CFBundleName")])) == "CF Test")
            let locs = call("CFBundleCopyBundleLocalizations", [main])
            #expect(call("CFArrayGetCount", [locs]) == 1 && text(call("CFArrayGetValueAtIndex", [locs, 0])) == "English")
            let langs = call("CFArrayCreateMutable", [0, 0, ppc_data_export("kCFTypeArrayCallBacks")])
            call("CFArrayAppendValue", [langs, str("French")])
            call("CFArrayAppendValue", [langs, str("English")])
            let pref = call("CFBundleCopyPreferredLocalizationsFromArray", [langs])
            #expect(text(call("CFArrayGetValueAtIndex", [pref, 0])) == "English")
            #expect(call("CFBundleCopyResourceURLForLocalization", [main, str("Localizable"), str("strings"), 0, str("English")]) != 0)
            #expect(call("CFBundleCopyResourceURLForLocalization", [main, str("Localizable"), str("strings"), 0, str("German")]) == 0)
            #expect(call("CFArrayGetCount", [call("CFBundleCopyResourceURLsOfTypeForLocalization", [main, str("strings"), 0, str("English")])]) == 2)
            #expect(call("CFBundleCopyResourcesDirectoryURL", [main]) != 0)
        }

        @Test func folderBundlesAndPlugIns() throws {
            let dir = start()
            let pkg = dir.appendingPathComponent("Thing.bundle")
            let contents = pkg.appendingPathComponent("Contents")
            try FileManager.default.createDirectory(at: contents.appendingPathComponent("Resources/English.lproj"),
                                                    withIntermediateDirectories: true)
            let info = """
                <?xml version="1.0" encoding="UTF-8"?>
                <plist version="1.0"><dict>
                <key>CFBundleIdentifier</key><string>com.example.thing</string>
                <key>CFBundleExecutable</key><string>Thing</string>
                <key>CFPlugInFactories</key><dict>
                <key>11111111-2222-3333-4444-555555555555</key><string>MakeThing</string></dict>
                <key>CFPlugInTypes</key><dict>
                <key>AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE</key>
                <array><string>11111111-2222-3333-4444-555555555555</string></array></dict>
                </dict></plist>
                """
            try Data(info.utf8).write(to: contents.appendingPathComponent("Info.plist"))
            try Data("BNDLthng".utf8).write(to: contents.appendingPathComponent("PkgInfo"))
            try Data("icon".utf8).write(to: contents.appendingPathComponent("Resources/Thing.icns"))
            let folder = call("CFBundleCopyBundleURL", [call("CFBundleGetMainBundle")])
            let url = call("CFURLCreateCopyAppendingPathComponent", [0, folder, str("Thing.bundle"), 1])
            let b = call("CFBundleCreate", [0, url])
            #expect(b != 0 && call("CFBundleCreate", [0, url]) == b && retainCount(b) == 2)
            #expect(text(call("CFBundleGetIdentifier", [b])) == "com.example.thing")
            #expect(call("CFBundleGetBundleWithIdentifier", [str("com.example.thing")]) == b)
            let type = buffer(8)
            call("CFBundleGetPackageInfo", [b, type, type + 4])
            #expect(rd32(type) == fourCC("BNDL") && rd32(type + 4) == fourCC("thng"))
            #expect(bool("CFBundleGetPackageInfoInDirectory", [url, type, type + 4]) && rd32(type + 4) == fourCC("thng"))
            let icns = call("CFBundleCopyResourceURL", [b, str("Thing"), str("icns"), 0])
            #expect(text(call("CFURLCopyLastPathComponent", [icns])) == "Thing.icns")
            #expect(call("CFBundleCopyResourceURLInDirectory", [url, str("Thing"), str("icns"), 0]) != 0)
            #expect(call("CFArrayGetCount", [call("CFBundleCopyResourceURLsOfTypeInDirectory", [url, str("icns"), 0])]) == 1)
            let res = call("CFBundleCopyResourcesDirectoryURL", [b])
            #expect(text(call("CFURLGetString", [res])) == "Contents/Resources/" && bool("CFEqual", [call("CFURLGetBaseURL", [res]), url]))
            #expect(text(call("CFURLGetString", [call("CFBundleCopyBuiltInPlugInsURL", [b])])) == "Contents/PlugIns/")
            #expect(text(call("CFURLGetString", [call("CFBundleCopyPrivateFrameworksURL", [b])])) == "Contents/Frameworks/")
            #expect(text(call("CFURLGetString", [call("CFBundleCopySharedFrameworksURL", [b])])) == "Contents/SharedFrameworks/")
            #expect(text(call("CFURLGetString", [call("CFBundleCopySharedSupportURL", [b])])) == "Contents/SharedSupport/")
            #expect(text(call("CFURLGetString", [call("CFBundleCopySupportFilesDirectoryURL", [b])])) == "Contents/")
            let copied = call("CFBundleCopyInfoDictionaryInDirectory", [url])
            #expect(text(call("CFDictionaryGetValue", [copied, str("CFBundleExecutable")])) == "Thing")
            #expect(call("CFBundleCopyExecutableURL", [b]) == 0 && !bool("CFBundleLoadExecutable", [b]))
            #expect(call("CFBundleGetFunctionPointerForName", [b, str("MakeThing")]) == 0)
            #expect(call("CFBundleCopyAuxiliaryExecutableURL", [b, str("Tool")]) == 0)
            #expect(int("CFBundleOpenBundleResourceMap", [b]) == -1)
            let refs = buffer(4)
            #expect(int("CFBundleOpenBundleResourceFiles", [b, refs, refs + 2]) == -43)
            let found = call("CFBundleCreateBundlesFromDirectory", [0, folder, str("bundle")])
            #expect(call("CFArrayGetCount", [found]) == 1 && call("CFArrayGetValueAtIndex", [found, 0]) == b)

            // As a plug-in: its factory and type registered from its info.
            let plugIn = call("CFPlugInCreate", [0, url])
            #expect(plugIn == b && call("CFBundleGetPlugIn", [b]) == b && call("CFPlugInGetBundle", [plugIn]) == b)
            let typeID = call("CFUUIDCreateFromString", [0, str("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE")])
            let factoryID = call("CFUUIDCreateFromString", [0, str("11111111-2222-3333-4444-555555555555")])
            let factories = call("CFPlugInFindFactoriesForPlugInType", [typeID])
            #expect(call("CFArrayGetCount", [factories]) == 1 && call("CFArrayGetValueAtIndex", [factories, 0]) == factoryID)
            #expect(call("CFArrayGetCount", [call("CFPlugInFindFactoriesForPlugInTypeInPlugIn", [typeID, b])]) == 1)
            #expect(call("CFPlugInInstanceCreate", [0, factoryID, typeID]) == 0) // no code to make one with
            #expect(bool("CFPlugInIsLoadOnDemand", [b]))
            call("CFPlugInSetLoadOnDemand", [b, 0])
            #expect(!bool("CFPlugInIsLoadOnDemand", [b]))

            // A factory function of the program's own: (allocator, typeUUID) -> 0x1234.
            let other = call("CFUUIDCreateFromString", [0, str("99999999-2222-3333-4444-555555555555")])
            let make = routine([0x3860_1234, 0x4E80_0020])
            #expect(bool("CFPlugInRegisterFactoryFunction", [other, make]))
            #expect(!bool("CFPlugInRegisterFactoryFunction", [other, make]))
            #expect(call("CFPlugInInstanceCreate", [0, other, typeID]) == 0) // not of that type yet
            #expect(bool("CFPlugInRegisterPlugInType", [other, typeID]))
            #expect(call("CFPlugInInstanceCreate", [0, other, typeID]) == 0x1234)
            #expect(call("CFArrayGetCount", [call("CFPlugInFindFactoriesForPlugInType", [typeID])]) == 2)
            call("CFPlugInAddInstanceForFactory", [other])
            call("CFPlugInRemoveInstanceForFactory", [other])
            #expect(bool("CFPlugInUnregisterPlugInType", [other, typeID]) && !bool("CFPlugInUnregisterPlugInType", [other, typeID]))
            #expect(bool("CFPlugInUnregisterFactory", [other]) && !bool("CFPlugInUnregisterFactory", [other]))
            #expect(bool("CFPlugInRegisterFactoryFunctionByName", [other, b, str("MakeOther")]))

            // An instance: its data after it; its interface and deallocation the program's.
            let counter = buffer(4)
            let dealloc = routine([0x3C80_0000 | counter >> 16, 0x6084_0000 | counter & 0xFFFF, 0x9064_0000, 0x4E80_0020])
            let getInterface = routine([0x9065_0000, 0x3860_0001, 0x4E80_0020]) // *ftbl = instance; true
            let inst = call("CFPlugInInstanceCreateWithInstanceDataSize", [0, 16, dealloc, str("Factory"), getInterface])
            #expect(call("CFGetTypeID", [inst]) == call("CFPlugInInstanceGetTypeID"))
            let instData = call("CFPlugInInstanceGetInstanceData", [inst])
            #expect(instData == inst + 16 && rd32(instData) == 0)
            #expect(text(call("CFPlugInInstanceGetFactoryName", [inst])) == "Factory")
            let ftbl = buffer(4)
            #expect(bool("CFPlugInInstanceGetInterfaceFunctionTable", [inst, str("IFoo"), ftbl]) && rd32(ftbl) == inst)
            call("CFRelease", [inst])
            #expect(rd32(counter) == instData)
        }

        // MARK: Property lists

        @Test func propertyListsAsXML() {
            _ = start()
            let tcb = ppc_data_export("kCFTypeDictionaryKeyCallBacks"), vcb = ppc_data_export("kCFTypeDictionaryValueCallBacks")
            let dict = call("CFDictionaryCreateMutable", [0, 0, tcb, vcb])
            let array = call("CFArrayCreateMutable", [0, 0, ppc_data_export("kCFTypeArrayCallBacks")])
            call("CFArrayAppendValue", [array, str("a & <b>")])
            call("CFArrayAppendValue", [array, call("CFNumberCreate", [0, 3, bytes([0, 0, 0, 42])])])
            call("CFArrayAppendValue", [array, call("CFNumberCreate", [0, 6, bytes(words(0.5).flatMap { be32($0) })])])
            call("CFDictionarySetValue", [dict, str("list"), array])
            call("CFDictionarySetValue", [dict, str("yes"), data("kCFBooleanTrue")])
            call("CFDictionarySetValue", [dict, str("when"), call("CFDateCreate", [0], fp: [86400 + 3661])])
            call("CFDictionarySetValue", [dict, str("blob"), call("CFDataCreate", [0, bytes([0, 1, 2, 0xFF]), 4])])
            call("CFDictionarySetValue", [dict, str("empty"), call("CFArrayCreate", [0, 0, 0, 0])])
            let xml = call("CFPropertyListCreateXMLData", [0, dict])
            let x = String(decoding: read(call("CFDataGetBytePtr", [xml]), Int(call("CFDataGetLength", [xml]))), as: UTF8.self)
            let want = """
                <?xml version="1.0" encoding="UTF-8"?>
                <!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
                <plist version="1.0">
                <dict>
                \t<key>blob</key>
                \t<data>
                \tAAEC/w==
                \t</data>
                \t<key>empty</key>
                \t<array/>
                \t<key>list</key>
                \t<array>
                \t\t<string>a &amp; &lt;b&gt;</string>
                \t\t<integer>42</integer>
                \t\t<real>0.5</real>
                \t</array>
                \t<key>when</key>
                \t<date>2001-01-02T01:01:01Z</date>
                \t<key>yes</key>
                \t<true/>
                </dict>
                </plist>

                """
            #expect(x == want)
            let err = buffer(4)
            let back = call("CFPropertyListCreateFromXMLData", [0, xml, 0, err])
            #expect(back != 0 && rd32(err) == 0 && bool("CFEqual", [back, dict]))
            // Immutable as asked: the array can't grow... and mutable when asked.
            let mutableBack = call("CFPropertyListCreateFromXMLData", [0, xml, 2, err])
            let list = call("CFDictionaryGetValue", [mutableBack, str("list")])
            let s = call("CFArrayGetValueAtIndex", [list, 0])
            call("CFStringAppend", [s, str("!")])
            #expect(text(s) == "a & <b>!")
            let deep = call("CFPropertyListCreateDeepCopy", [0, dict, 0])
            #expect(deep != dict && bool("CFEqual", [deep, dict]))
            #expect(call("CFDictionaryGetValue", [deep, str("list")]) != array)

            // The old ASCII form, and errors.
            let ascii = call("CFDataCreate", [0, cstr("{ name = \"Blue Box\"; list = (one, \"t w o\"); hex = <0aff>; }"), 59])
            let old = call("CFPropertyListCreateFromXMLData", [0, ascii, 0, err])
            #expect(text(call("CFDictionaryGetValue", [old, str("name")])) == "Blue Box")
            #expect(call("CFArrayGetCount", [call("CFDictionaryGetValue", [old, str("list")])]) == 2)
            #expect(call("CFDataGetLength", [call("CFDictionaryGetValue", [old, str("hex")])]) == 2)
            let bad = call("CFDataCreate", [0, cstr("<plist><dict><key>a</key></plist>"), 33])
            #expect(call("CFPropertyListCreateFromXMLData", [0, bad, 0, err]) == 0 && rd32(err) != 0)
            #expect(text(rd32(err)) != nil)
            // Not a property list.
            #expect(call("CFPropertyListCreateXMLData", [0, call("CFUUIDCreate", [0])]) == 0)
        }

        @Test func preferencesInThePreferencesFolder() throws {
            _ = launchTestApp([TestResource(type: "plst", id: 0, data: Array(Self.infoPlist.utf8))])
            giveStack()
            let app = data("kCFPreferencesCurrentApplication")
            let prefs = URL(fileURLWithPath: testSystemFolder).appendingPathComponent("Preferences")
            let file = prefs.appendingPathComponent("com.example.cftest.plist")
            try? FileManager.default.removeItem(at: file)
            #expect(call("CFPreferencesCopyAppValue", [str("Score"), app]) == 0)
            let score = call("CFNumberCreate", [0, 3, bytes([0, 0, 1, 0])])
            call("CFPreferencesSetAppValue", [str("Score"), score, app])
            call("CFPreferencesSetAppValue", [str("Name"), str("Player"), app])
            call("CFPreferencesSetAppValue", [str("Sound"), str("YES"), app])
            #expect(bool("CFPreferencesAppSynchronize", [app]))
            let written = try String(contentsOf: file, encoding: .utf8)
            #expect(written.contains("<key>Score</key>\n\t<integer>256</integer>"))
            let got = call("CFPreferencesCopyAppValue", [str("Score"), app])
            #expect(bool("CFEqual", [got, score]))
            let valid = buffer(1)
            #expect(int("CFPreferencesGetAppIntegerValue", [str("Score"), app, valid]) == 256 && rd8(valid) == 1)
            #expect(bool("CFPreferencesGetAppBooleanValue", [str("Sound"), app, valid]) && rd8(valid) == 1)
            #expect(int("CFPreferencesGetAppIntegerValue", [str("Name"), app, valid]) == 0 && rd8(valid) == 0)
            #expect(!bool("CFPreferencesGetAppBooleanValue", [str("Missing"), app, valid]) && rd8(valid) == 0)

            // Another launch reads them from the file.
            carbon_cf_reset()
            let app2 = data("kCFPreferencesCurrentApplication")
            #expect(text(call("CFPreferencesCopyAppValue", [str("Name"), app2])) == "Player")
            call("CFPreferencesSetAppValue", [str("Name"), 0, app2]) // removed
            #expect(call("CFPreferencesCopyAppValue", [str("Name"), app2]) == 0)

            // The primitive calls, on exact domains.
            let anyUser = data("kCFPreferencesAnyUser"), user = data("kCFPreferencesCurrentUser")
            let anyHost = data("kCFPreferencesAnyHost"), host = data("kCFPreferencesCurrentHost")
            let id = str("com.example.other")
            call("CFPreferencesSetValue", [str("k"), str("v"), id, user, anyHost])
            call("CFPreferencesSetValue", [str("h"), str("here"), id, user, host])
            #expect(text(call("CFPreferencesCopyValue", [str("k"), id, user, anyHost])) == "v")
            #expect(call("CFPreferencesCopyValue", [str("h"), id, user, anyHost]) == 0)
            #expect(text(call("CFPreferencesCopyValue", [str("k"), id, anyUser, anyHost])) == "v") // one user
            #expect(FileManager.default.fileExists(atPath: prefs.appendingPathComponent("ByHost/com.example.other.plist").path))
            let keys = call("CFPreferencesCopyKeyList", [id, user, anyHost])
            #expect(call("CFArrayGetCount", [keys]) == 1)
            #expect(call("CFPreferencesCopyKeyList", [str("com.example.nothing"), user, anyHost]) == 0)
            let toSet = call("CFDictionaryCreateMutable", [0, 0, ppc_data_export("kCFTypeDictionaryKeyCallBacks"),
                                                         ppc_data_export("kCFTypeDictionaryValueCallBacks")])
            call("CFDictionarySetValue", [toSet, str("a"), str("1")])
            call("CFDictionarySetValue", [toSet, str("b"), str("2")])
            let toRemove = call("CFArrayCreateMutable", [0, 0, ppc_data_export("kCFTypeArrayCallBacks")])
            call("CFArrayAppendValue", [toRemove, str("k")])
            call("CFPreferencesSetMultiple", [toSet, toRemove, id, user, anyHost])
            let fetch = call("CFArrayCreateMutable", [0, 0, ppc_data_export("kCFTypeArrayCallBacks")])
            call("CFArrayAppendValue", [fetch, str("a")])
            call("CFArrayAppendValue", [fetch, str("k")])
            #expect(call("CFDictionaryGetCount", [call("CFPreferencesCopyMultiple", [fetch, id, user, anyHost])]) == 1)
            #expect(call("CFDictionaryGetCount", [call("CFPreferencesCopyMultiple", [0, id, user, anyHost])]) == 2)
            #expect(bool("CFPreferencesSynchronize", [id, user, anyHost]))
            let apps = call("CFPreferencesCopyApplicationList", [user, anyHost])
            let names = (0..<call("CFArrayGetCount", [apps])).map { text(call("CFArrayGetValueAtIndex", [apps, $0])) }
            #expect(names.contains("com.example.other") && names.contains("com.example.cftest"))

            // Suites, then any application's values.
            call("CFPreferencesSetValue", [str("shared"), str("from suite"), str("com.example.suite"), user, anyHost])
            #expect(call("CFPreferencesCopyAppValue", [str("shared"), app2]) == 0)
            call("CFPreferencesAddSuitePreferencesToApp", [app2, str("com.example.suite")])
            #expect(text(call("CFPreferencesCopyAppValue", [str("shared"), app2])) == "from suite")
            call("CFPreferencesRemoveSuitePreferencesFromApp", [app2, str("com.example.suite")])
            #expect(call("CFPreferencesCopyAppValue", [str("shared"), app2]) == 0)
            call("CFPreferencesSetValue", [str("global"), str("everyone"), data("kCFPreferencesAnyApplication"), user, anyHost])
            #expect(text(call("CFPreferencesCopyAppValue", [str("global"), app2])) == "everyone")
            #expect(FileManager.default.fileExists(atPath: prefs.appendingPathComponent(".GlobalPreferences.plist").path))
            // An ID longer than an HFS name keeps to 31 characters.
            let long = str("com.example.a.very.long.application.identifier")
            call("CFPreferencesSetAppValue", [str("x"), str("y"), long])
            let files = try FileManager.default.contentsOfDirectory(atPath: prefs.path)
            #expect(files.contains { $0.hasPrefix("com.example.a.very.l~") && $0.count <= 31 })
            #expect(text(call("CFPreferencesCopyAppValue", [str("x"), long])) == "y")
        }

        // MARK: XML

        static let xmlDocument = """
            <?xml version="1.0"?>
            <!DOCTYPE note SYSTEM "note.dtd">
            <note id="n1" lang='en'>
              <!-- a comment -->
              <to>Tove &amp; co</to>
              <body><![CDATA[<raw>]]></body>
              <empty/>
            </note>
            """

        @Test func xmlTreesAndNodes() {
            _ = start()
            let bytesOf = Array(Self.xmlDocument.utf8)
            let xml = call("CFDataCreate", [0, bytes(bytesOf), UInt32(bytesOf.count)])
            let tree = call("CFXMLTreeCreateFromData", [0, xml, 0, 4 | 8, 1]) // replace entities, skip whitespace
            #expect(tree != 0)
            let doc = call("CFXMLTreeGetNode", [tree])
            #expect(call("CFXMLNodeGetTypeCode", [doc]) == 1 && call("CFXMLNodeGetVersion", [doc]) == 1)
            #expect(call("CFTreeGetChildCount", [tree]) == 3) // <?xml?>, DOCTYPE, note
            let pi = call("CFXMLTreeGetNode", [call("CFTreeGetChildAtIndex", [tree, 0])])
            #expect(call("CFXMLNodeGetTypeCode", [pi]) == 4 && text(call("CFXMLNodeGetString", [pi])) == "xml")
            #expect(text(rd32(call("CFXMLNodeGetInfoPtr", [pi]))) == "version=\"1.0\"")
            let doctype = call("CFXMLTreeGetNode", [call("CFTreeGetChildAtIndex", [tree, 1])])
            #expect(call("CFXMLNodeGetTypeCode", [doctype]) == 11 && text(call("CFXMLNodeGetString", [doctype])) == "note")
            #expect(text(call("CFURLGetString", [rd32(call("CFXMLNodeGetInfoPtr", [doctype]))])) == "note.dtd")
            let note = call("CFTreeGetChildAtIndex", [tree, 2])
            let element = call("CFXMLTreeGetNode", [note])
            #expect(call("CFXMLNodeGetTypeCode", [element]) == 2 && text(call("CFXMLNodeGetString", [element])) == "note")
            let info = call("CFXMLNodeGetInfoPtr", [element])
            #expect(text(call("CFDictionaryGetValue", [rd32(info), str("lang")])) == "en" && rd8(info + 8) == 0)
            #expect(text(call("CFArrayGetValueAtIndex", [rd32(info + 4), 0])) == "id")
            #expect(call("CFTreeGetChildCount", [note]) == 4) // comment, to, body, empty
            let to = call("CFTreeGetChildAtIndex", [note, 1])
            #expect(text(call("CFXMLNodeGetString", [call("CFXMLTreeGetNode", [call("CFTreeGetFirstChild", [to])])])) == "Tove & co")
            let cdata = call("CFXMLTreeGetNode", [call("CFTreeGetFirstChild", [call("CFTreeGetChildAtIndex", [note, 2])])])
            #expect(call("CFXMLNodeGetTypeCode", [cdata]) == 7 && text(call("CFXMLNodeGetString", [cdata])) == "<raw>")
            let empty = call("CFXMLTreeGetNode", [call("CFTreeGetChildAtIndex", [note, 3])])
            #expect(rd8(call("CFXMLNodeGetInfoPtr", [empty]) + 8) == 1)

            // Written back.
            let out = call("CFXMLTreeCreateXMLData", [0, tree])
            let s = String(decoding: read(call("CFDataGetBytePtr", [out]), Int(call("CFDataGetLength", [out]))), as: UTF8.self)
            #expect(s == "<?xml version=\"1.0\"?><!DOCTYPE note SYSTEM \"note.dtd\"><note id=\"n1\" lang=\"en\"><!-- a comment -->" +
                         "<to>Tove &amp; co</to><body><![CDATA[<raw>]]></body><empty/></note>")
            // Entities kept as their own nodes, whitespace kept, metadata skipped.
            let tree2 = call("CFXMLTreeCreateFromData", [0, xml, 0, 2, 1])
            #expect(call("CFTreeGetChildCount", [tree2]) == 1)
            let note2 = call("CFTreeGetChildAtIndex", [tree2, 0])
            let to2 = call("CFTreeGetChildAtIndex", [note2, 2]) // whitespace twice (the comment skipped between)
            #expect(call("CFXMLNodeGetTypeCode", [call("CFXMLTreeGetNode", [call("CFTreeGetChildAtIndex", [note2, 0])])]) == 12)
            #expect(call("CFTreeGetChildCount", [to2]) == 3, "\(call("CFTreeGetChildCount", [to2]))")
            let amp = call("CFXMLTreeGetNode", [call("CFTreeGetChildAtIndex", [to2, 1])])
            #expect(call("CFXMLNodeGetTypeCode", [amp]) == 10 && text(call("CFXMLNodeGetString", [amp])) == "amp")
            #expect(call("CFXMLTreeCreateFromData", [0, call("CFDataCreate", [0, cstr("<a><b></a>"), 10]), 0, 0, 1]) == 0)

            // Nodes made by hand.
            let einfo = buffer(12)
            wr32(einfo, call("CFDictionaryCreate", [0, 0, 0, 0, 0, 0]))
            wr32(einfo + 4, call("CFArrayCreate", [0, 0, 0, 0]))
            wr8(einfo + 8, 1)
            let made = call("CFXMLNodeCreate", [0, 2, str("tag"), einfo, 1])
            #expect(call("CFXMLNodeGetInfoPtr", [made]) != einfo && rd8(call("CFXMLNodeGetInfoPtr", [made]) + 8) == 1)
            let copy = call("CFXMLNodeCreateCopy", [0, made])
            #expect(bool("CFEqual", [copy, made]) && text(call("CFXMLNodeGetString", [copy])) == "tag")
            let t = call("CFXMLTreeCreateWithNode", [0, made])
            #expect(call("CFXMLTreeGetNode", [t]) == made && retainCount(made) == 2)
            let textNode = call("CFXMLNodeCreate", [0, 6, str("words"), 0, 1])
            #expect(call("CFXMLNodeGetInfoPtr", [textNode]) == 0)
        }

        @Test func xmlParserCallsTheProgram() {
            _ = start()
            let bytesOf = Array(Self.xmlDocument.utf8)
            let xml = call("CFDataCreate", [0, bytes(bytesOf), UInt32(bytesOf.count)])
            // create counts and returns the node; addChild and end count.
            let counts = buffer(12)
            let create = routine([0x80C5_0000, 0x38C6_0001, 0x90C5_0000, 0x7C83_2378, 0x4E80_0020])
            let addChild = routine([0x80E6_0004, 0x38E7_0001, 0x90E6_0004, 0x4E80_0020])
            let end = routine([0x80C5_0008, 0x38C6_0001, 0x90C5_0008, 0x4E80_0020])
            let cbs = buffer(24)
            wr32(cbs + 4, create)
            wr32(cbs + 8, addChild)
            wr32(cbs + 12, end)
            let ctx = buffer(20)
            wr32(ctx + 4, counts)
            let parser = call("CFXMLParserCreate", [0, xml, 0, 8, 1, cbs, ctx])
            #expect(int("CFXMLParserGetStatusCode", [parser]) == -2)
            #expect(bool("CFXMLParserParse", [parser]))
            #expect(int("CFXMLParserGetStatusCode", [parser]) == 0)
            // document, <?xml?>, DOCTYPE, note, comment, to, its text, &amp;, its text, body, CDATA, empty
            #expect(rd32(counts) == 12 && rd32(counts + 4) == 11 && rd32(counts + 8) == 12)
            #expect(call("CFXMLParserGetDocument", [parser]) != 0) // what create gave for the document
            let got = buffer(24)
            call("CFXMLParserGetCallBacks", [parser, got])
            #expect(rd32(got + 4) == create)
            call("CFXMLParserGetContext", [parser, got])
            #expect(rd32(got + 4) == counts)
            #expect(call("CFXMLParserGetSourceURL", [parser]) == 0)
            #expect(!bool("CFXMLParserParse", [parser])) // once

            // A malformed document: the error, its line, the handler told.
            let badBytes = Array("<a>\n<b>\n</a>".utf8)
            let bad = call("CFDataCreate", [0, bytes(badBytes), UInt32(badBytes.count)])
            let errors = buffer(12) // the counting callbacks above still count in it
            let handler = routine([0x9085_0000, 0x3860_0000, 0x4E80_0020]) // *info = error; false
            wr32(cbs + 20, handler)
            wr32(ctx + 4, errors)
            let p2 = call("CFXMLParserCreate", [0, bad, 0, 0, 1, cbs, ctx])
            #expect(!bool("CFXMLParserParse", [p2]))
            #expect(int("CFXMLParserGetStatusCode", [p2]) == 8 && rd32(errors) == 8) // kCFXMLErrorMalformedCloseTag
            #expect(call("CFXMLParserGetLineNumber", [p2]) == 3 && call("CFXMLParserGetLocation", [p2]) > 0)
            #expect(text(call("CFXMLParserCopyErrorDescription", [p2])) == "Malformed close tag")

            // Aborted by the program from a callback.
            let abortParser = buffer(4)
            let p3 = call("CFXMLParserCreate", [0, xml, 0, 0, 1, cbs, ctx])
            call("CFXMLParserAbort", [p3, 10, str("stop")])
            #expect(!bool("CFXMLParserParse", [p3]) || int("CFXMLParserGetStatusCode", [p3]) == 10)
            _ = abortParser
            // From a file: its URL kept.
            let folder = call("CFBundleCopyBundleURL", [call("CFBundleGetMainBundle")])
            let file = call("CFURLCreateCopyAppendingPathComponent", [0, folder, str("doc.xml"), 0])
            let err = buffer(4)
            #expect(bool("CFURLWriteDataAndPropertiesToResource", [file, xml, 0, err]))
            let p4 = call("CFXMLParserCreateWithDataFromURL", [0, file, 0, 1, cbs, ctx])
            #expect(call("CFXMLParserGetSourceURL", [p4]) == file && bool("CFXMLParserParse", [p4]))
            #expect(call("CFXMLTreeCreateWithDataFromURL", [0, file, 0, 1]) != 0)
        }
    }
}
