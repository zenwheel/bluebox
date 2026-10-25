import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// StdCLib, MPW's C library for PowerPC programs: streams on MPW's FILE
    /// structs (and the getc and putc macros programs have for them), the
    /// device switch, time, locale, signals, the rest of stdlib, the Pascal
    /// string routines, MPW's paths, the runtime's calls and the data.
    @Suite struct StdCLib {
        /// Call an export with its arguments in r3 on (and f1 when given); r3 after.
        @discardableResult
        func call(_ name: String, _ args: [UInt32], f1: Double? = nil) -> UInt32 {
            // The rest in the parameter area, as a caller leaves them.
            if args.count > 8 {
                giveStack()
                for i in 8..<args.count { wr32(g_ppc.gpr.1 + 24 + UInt32(4 * i), args[i]) }
            }
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.prefix(8).enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            if let f1 {
                withUnsafeMutableBytes(of: &g_ppc.fpr) { $0.storeBytes(of: f1.bitPattern, toByteOffset: 8, as: UInt64.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        func int(_ name: String, _ args: [UInt32]) -> Int32 { Int32(bitPattern: call(name, args)) }
        var r4: UInt32 { withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 16, as: UInt32.self) } }
        var f1: Double { Double(bitPattern: withUnsafeBytes(of: g_ppc.fpr) { $0.load(fromByteOffset: 8, as: UInt64.self) }) }

        func bytes(_ b: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }
        func cstr(_ s: String) -> UInt32 { bytes(Array(s.utf8) + [0]) }
        func pstr(_ s: String) -> UInt32 { bytes([UInt8(s.utf8.count)] + Array(s.utf8)) }
        func buffer(_ n: Int) -> UInt32 { tb_NewPtr(Int32(n), false, true) }
        func string(_ p: UInt32) -> String {
            var b: [UInt8] = []
            var a = p
            while rd8(a) != 0 { b.append(rd8(a)); a += 1 }
            return String(decoding: b, as: UTF8.self)
        }
        func pascal(_ p: UInt32) -> String { String(decoding: (0..<Int(rd8(p))).map { rd8(p + 1 + UInt32($0)) }, as: UTF8.self) }
        func double(_ p: UInt32) -> Double { Double(bitPattern: UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4))) }

        /// PowerPC code of a test's own, as a transition vector; a stack for
        /// the library to call it on.
        func routine(_ words: [UInt32]) -> UInt32 {
            let code = tb_NewPtr(Int32(words.count * 4), false, true)
            for (i, w) in words.enumerated() { wr32(code + UInt32(i * 4), w) }
            let tv = tb_NewPtr(8, false, true)
            wr32(tv, code)
            return tv
        }
        func giveStack() {
            #expect(cfm_ppc_cpu())
            g_ppc.gpr.1 -= 1024 // room for a parameter area above
        }
        /// lis r4,hi; ori r4,r4,lo
        func loadR4(_ a: UInt32) -> [UInt32] { [0x3C80_0000 | a >> 16, 0x6084_0000 | a & 0xFFFF] }

        var errno: UInt32 { ppc_data_export("errno") }

        // MARK: Streams

        @Test func streamsReadAndWriteFiles() throws {
            let dir = launchTestApp()
            let f = call("fopen", [cstr("Notes"), cstr("w")])
            #expect(f != 0)
            let iob = ppc_data_export("_iob")
            #expect(f >= iob + 3 * 24 && f < iob + 40 * 24 && (f - iob) % 24 == 0)
            #expect(int("fprintf", [f, cstr("%d apples and %s\r"), 12, cstr("pears")]) == 20)
            #expect(int("fputs", [cstr("line two\r"), f]) >= 0)
            #expect(call("fputc", [UInt32(UInt8(ascii: "!")), f]) == 0x21)
            #expect(int("ftell", [f]) == 30)
            #expect(int("fclose", [f]) == 0)
            let host = try Data(contentsOf: dir.appendingPathComponent("Notes"))
            #expect(String(decoding: host, as: UTF8.self) == "12 apples and pears\rline two\r!")

            // Reading back: fgets stops after MPW's newline (13), fscanf.
            let r = call("fopen", [cstr("Notes"), cstr("r")])
            let line = buffer(64)
            #expect(call("fgets", [line, 64, r]) == line && string(line) == "12 apples and pears\r")
            #expect(int("fseek", [r, 0, 0]) == 0)
            let n = buffer(4), word = buffer(16)
            #expect(int("fscanf", [r, cstr("%d %s"), n, word]) == 2)
            #expect(rd32(n) == 12 && string(word) == "apples")
            #expect(int("ftell", [r]) == 9)
            #expect(call("ungetc", [UInt32(UInt8(ascii: "X")), r]) == 0x58)
            #expect(call("fgetc", [r]) == 0x58 && call("fgetc", [r]) == 0x20)
            #expect(int("fseek", [r, UInt32(bitPattern: -1), 2]) == 0 && call("fgetc", [r]) == 0x21)
            #expect(int("fgetc", [r]) == -1 && call("feof", [r]) != 0)
            call("clearerr", [r])
            #expect(call("feof", [r]) == 0)
            call("rewind", [r])
            let block = buffer(64)
            #expect(call("fread", [block, 4, 3, r]) == 3 && string(block) == "12 apples an")
            #expect(int("fclose", [r]) == 0 && int("fclose", [r]) == -1)

            // Appending; updating in place ("r+"), reading then writing.
            let a = call("fopen", [cstr("Notes"), cstr("a")])
            #expect(call("fwrite", [cstr("?"), 1, 1, a]) == 1)
            #expect(int("fclose", [a]) == 0)
            let u = call("fopen", [cstr("Notes"), cstr("r+")])
            #expect(call("fgetc", [u]) == 0x31)
            #expect(int("fseek", [u, 0, 1]) == 0 && call("fputc", [0x33, u]) == 0x33)
            #expect(int("fclose", [u]) == 0)
            let text = String(decoding: try Data(contentsOf: dir.appendingPathComponent("Notes")), as: UTF8.self)
            #expect(text == "13 apples and pears\rline two\r!?")

            // Missing files, rename and remove.
            #expect(call("fopen", [cstr("Missing"), cstr("r")]) == 0 && rd32(errno) == 2)
            #expect(call("fopen", [cstr("Notes"), cstr("q")]) == 0 && rd32(errno) == 22)
            #expect(int("rename", [cstr("Notes"), cstr("Renamed")]) == 0)
            #expect(FileManager.default.fileExists(atPath: dir.appendingPathComponent("Renamed").path))
            #expect(int("remove", [cstr("Renamed")]) == 0)
            #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("Renamed").path))
            #expect(int("remove", [cstr("Renamed")]) == -1 && rd32(errno) == 2)
        }

        /// A program's getc and putc macros work on the FILE struct itself
        /// and call _filbuf and _flsbuf when its buffer runs out.
        @Test func getcAndPutcMacrosOnTheStruct() throws {
            let dir = launchTestApp()
            func putcMacro(_ ch: UInt8, _ f: UInt32) -> Int32 {
                let cnt = Int32(bitPattern: rd32(f)) - 1
                wr32(f, UInt32(bitPattern: cnt))
                if cnt >= 0 {
                    let p = rd32(f + 4)
                    wr8(p, ch)
                    wr32(f + 4, p + 1)
                    return Int32(ch)
                }
                return int("_flsbuf", [UInt32(ch), f])
            }
            func getcMacro(_ f: UInt32) -> Int32 {
                let cnt = Int32(bitPattern: rd32(f)) - 1
                wr32(f, UInt32(bitPattern: cnt))
                if cnt >= 0 {
                    let p = rd32(f + 4)
                    wr32(f + 4, p + 1)
                    return Int32(rd8(p))
                }
                return int("_filbuf", [f])
            }
            // _iob: stdin, stdout and stderr as MPW's library starts them.
            let iob = ppc_data_export("_iob")
            #expect(rd16(iob + 18) == 0x01 && rd16(iob + 20) == 0)
            #expect(rd16(iob + 24 + 18) == 0x02 && rd16(iob + 24 + 20) == 1)
            #expect(rd16(iob + 48 + 18) == 0x42 && rd16(iob + 48 + 20) == 2)
            #expect(rd32(ppc_data_export("_lastbuf")) == iob + 40 * 24)

            let f = call("fopen", [cstr("Macro"), cstr("w")])
            #expect(rd32(f) == 0 && rd32(f + 8) == 0) // no buffer until the first write
            let text = Array((0..<3000).map { UInt8(65 + $0 % 26) })
            for b in text { #expect(putcMacro(b, f) == Int32(b)) }
            // A full buffer of 1024: the first putc found none, the rest went
            // straight in until _flsbuf wrote it out.
            #expect(rd16(f + 16) == 1024 && rd32(f + 12) == rd32(f + 8) + 1024)
            #expect(rd32(f + 4) - rd32(f + 8) == UInt32(3000 - 2048) && Int32(bitPattern: rd32(f)) == 1024 - (3000 - 2048))
            #expect(int("fclose", [f]) == 0)
            #expect(try Data(contentsOf: dir.appendingPathComponent("Macro")) == Data(text))

            let r = call("fopen", [cstr("Macro"), cstr("r")])
            var back: [UInt8] = []
            while true {
                let ch = getcMacro(r)
                if ch < 0 { break }
                back.append(UInt8(ch))
                if back.count == 1 { #expect(Int32(bitPattern: rd32(r)) == 1023 && rd32(r + 4) == rd32(r + 8) + 1) }
            }
            #expect(back == text && rd16(r + 18) & 0x10 != 0 && rd32(r) == 0) // _IOEOF
            #expect(int("fclose", [r]) == 0)

            // stdout is the console: line buffered, so _cnt stays 0 and
            // each putc comes to _flsbuf, which writes the line at 13.
            let out = iob + 24
            for b in Array("hi".utf8) { #expect(putcMacro(b, out) == Int32(b)) }
            #expect(rd16(out + 18) & 0x40 != 0 && Int32(bitPattern: rd32(out)) <= 0 && rd32(out + 4) == rd32(out + 8) + 2)
            #expect(putcMacro(13, out) == 13)
            #expect(rd32(out + 4) == rd32(out + 8)) // written out
            // stdin has nothing to read.
            #expect(getcMacro(iob) == -1 && rd16(iob + 18) & 0x10 != 0)
        }

        @Test func streamBuffersAndPrintf() {
            launchTestApp()
            let f = call("fopen", [cstr("Buffered"), cstr("w+")])
            let mine = buffer(16)
            #expect(int("setvbuf", [f, mine, 0, 16]) == 0) // _IOFBF
            #expect(call("fputs", [cstr("0123456789"), f]) == 0)
            #expect(rd32(f + 8) == mine && rd32(f + 4) == mine + 10)
            #expect(int("fflush", [f]) == 0 && rd32(f + 4) == mine)
            call("rewind", [f])
            let s = buffer(32)
            #expect(call("fgets", [s, 5, f]) == s && string(s) == "0123")
            #expect(int("fclose", [f]) == 0)

            // Formatting: widths, precision, long long, %n, Pascal strings.
            let out = buffer(128), count = buffer(4)
            let n = int("sprintf", [out, cstr("[%5.2f|%-4d|%04x|%lld|%c|%.3s|%P%n]"), 0x4009_21FB, 0x5444_2D18, 7, 255,
                                    0, 5, 65, cstr("abcdef"), pstr("pas"), count])
            #expect(string(out) == "[ 3.14|7   |00ff|5|A|abc|pas]" && n == 29 && rd32(count) == 28)
            // vsprintf: the arguments through a va_list.
            let va = bytes([0, 0, 0, 42] + Array("x".utf8) + [0, 0, 0])
            wr32(va + 4, cstr("str"))
            #expect(int("vsprintf", [out, cstr("%d-%s"), va]) == 6 && string(out) == "42-str")
            // sscanf with a scanset and widths.
            let a = buffer(16), b = buffer(4), d = buffer(8)
            #expect(int("sscanf", [cstr("abc123 0x1F 2.5"), cstr("%[a-z]%2d%*d %i %lf"), a, b, b + 0, d]) == 4)
            #expect(string(a) == "abc" && rd32(b) == 31 && double(d) == 2.5)
            #expect(int("sscanf", [cstr(""), cstr("%d"), b]) == -1)
        }

        @Test func tmpfileAndTmpnam() throws {
            let dir = launchTestApp()
            let name = string(call("tmpnam", [0]))
            #expect(name.hasPrefix("tmp.") && name.count == 14)
            let f = call("tmpfile", [])
            #expect(f != 0)
            #expect(call("fputs", [cstr("scratch"), f]) == 0)
            let files = try FileManager.default.contentsOfDirectory(atPath: dir.path).filter { $0.hasPrefix("tmp.") }
            #expect(files.count == 1)
            #expect(int("fclose", [f]) == 0)
            #expect(try FileManager.default.contentsOfDirectory(atPath: dir.path).filter { $0.hasPrefix("tmp.") }.isEmpty)
            let t = cstr("dataXXXXX")
            #expect(call("mktemp", [t]) == t && string(t).hasPrefix("data") && !string(t).contains("X"))
        }

        // MARK: Descriptors and devices

        @Test func deviceSwitch() {
            launchTestApp()
            giveStack()
            let devs = ppc_data_export("_StdDevs")
            #expect(rd32(devs) == fourCC("FSYS") && rd32(devs + 24) == fourCC("CONS") && rd32(devs + 48) == fourCC("SYST"))
            let intenv = ppc_data_export("_IntEnv")
            #expect(rd16(intenv) == 0x5348 && rd32(intenv + 0x20) == devs && rd16(intenv + 0x1A) == 40 * 20)
            let out = buffer(28)
            #expect(int("_getDevHandler", [1, out, out + 4, out + 8, out + 12, out + 16, out + 20]) == 0)
            #expect(rd32(out) == fourCC("CONS") && rd32(out + 16) == rd32(devs + 24 + 16))

            // dev:null takes everything and gives nothing back.
            let null = int("open", [cstr("dev:null"), 1])
            let buf = cstr("hello")
            #expect(null >= 3 && int("write", [UInt32(null), buf, 5]) == 5 && int("read", [UInt32(null), buf, 5]) == -1)
            #expect(rd32(errno) == 9) // opened for writing only
            // dev:stdout is descriptor 1 again; the console reads nothing.
            let o = int("open", [cstr("Dev:StdOut"), 1])
            #expect(o > null && int("write", [UInt32(o), buf, 5]) == 5)
            #expect(int("open", [cstr("dev:stdin"), 1]) == -1 && rd32(errno) == 13) // stdin isn't for writing
            let dup = int("dup", [1])
            #expect(dup > 0 && int("write", [UInt32(dup), buf, 2]) == 2 && int("close", [UInt32(dup)]) == 0)
            #expect(int("ioctl", [0, 0x6602, 0]) == 0) // FIOINTERACTIVE: the console is
            let entry = buffer(4)
            wr32(entry, UInt32(bitPattern: Int32(o)))
            #expect(call("_getIOPort", [entry]) == rd32(intenv + 0x1C) + UInt32(o) * 20)

            // A device of the program's own, in a free slot: its faccess
            // takes every name, its write takes every byte (count to 0).
            let faccess = routine([0x3860_0000, 0x4E80_0020])                          // li r3,0; blr
            let write = routine([0x3800_0000, 0x9003_000C, 0x3860_0000, 0x4E80_0020]) // count = 0; return 0
            let none = routine([0x3860_0000, 0x4E80_0020])
            #expect(int("_addDevHandler", [5, fourCC("MINE"), faccess, none, none, write, none]) == 0)
            let mine = int("open", [cstr("anything"), 1])
            #expect(mine >= 3 && int("write", [UInt32(mine), buf, 5]) == 5)
            #expect(rd32(rd32(intenv + 0x1C) + UInt32(mine) * 20 + 4) == devs + 5 * 24)
            #expect(int("close", [UInt32(mine)]) == 0)
            call("_addDevHandler", [5, 0, 0, 0, 0, 0, 0])
            #expect(int("open", [cstr("anything"), 0]) == -1 && rd32(errno) == 2)
        }

        @Test func fileDescriptors() throws {
            let dir = launchTestApp()
            let fd = int("creat", [cstr("Made")])
            #expect(fd >= 3 && int("write", [UInt32(fd), cstr("abc"), 3]) == 3 && int("close", [UInt32(fd)]) == 0)
            // MPW's open makes TEXT files of the MPW Shell's.
            let spec = buffer(70)
            #expect(Int16(truncatingIfNeeded: call("FSMakeFSSpec_Long", [cstr("Made"), buffer(64), 0, 0, spec])) == 0)
            let info = buffer(16)
            #expect(tb_FSpGetFInfo(spec, info) == 0 && rd32(info) == fourCC("TEXT") && rd32(info + 4) == fourCC("MPS "))
            let r = int("FSp_open", [spec, 2])
            #expect(r >= 3)
            #expect(int("lseek", [UInt32(r), 1, 0]) == 1 && int("write", [UInt32(r), cstr("Z"), 1]) == 1)
            let ref = buffer(2)
            #expect(int("ioctl", [UInt32(r), 0x6605, ref]) == 0 && rd16(ref) != 0) // FIOREFNUM
            #expect(int("ioctl", [UInt32(r), 0x6606, 2]) == 0)                      // FIOSETEOF
            let dup = int("fcntl", [UInt32(r), 0, 10])
            #expect(dup >= 10 && int("close", [UInt32(r)]) == 0)
            #expect(int("lseek", [UInt32(dup), 0, 2]) == 2 && int("close", [UInt32(dup)]) == 0)
            #expect(String(decoding: try Data(contentsOf: dir.appendingPathComponent("Made")), as: UTF8.self) == "aZ")
            #expect(int("open", [cstr("Made"), 0x502]) == -1 && rd32(errno) == 17) // O_CREAT | O_EXCL
            #expect(Int16(truncatingIfNeeded: rd16(ppc_data_export("MacOSErr"))) == -48)
            #expect(int("access", [cstr("Made"), 0]) == 0 && int("access", [cstr("Gone"), 0]) == -1)
            #expect(int("FSp_unlink", [spec]) == 0 && int("unlink", [cstr("Made")]) == -1)
        }

        @Test func pathsInFull() {
            let dir = launchTestApp()
            try? FileManager.default.createDirectory(at: dir.appendingPathComponent("Folder"), withIntermediateDirectories: true)
            FileManager.default.createFile(atPath: dir.appendingPathComponent("Folder/Leaf").path, contents: Data("x".utf8))
            let spec = buffer(70), path = buffer(512), isFolder = buffer(4)
            #expect(Int16(truncatingIfNeeded: call("ResolveFolderAliases", [0, 0, pstr(":Folder:Leaf"), 1, spec, isFolder, isFolder + 1, isFolder + 2])) == 0)
            #expect(rd8(isFolder) == 0 && pascal(spec + 6) == "Leaf")
            // The FSSpec by value: its words in the argument registers and on.
            giveStack()
            let sp = g_ppc.gpr.1
            let words = (0..<18).map { rd32(spec + UInt32($0 * 4)) }
            for i in 8..<18 { wr32(sp + 24 + UInt32(4 * i), words[i]) }
            wr32(sp + 24 + 4 * 18, path)
            wr32(sp + 24 + 4 * 19, 512)
            #expect(call("FSSpec2Path_Long", Array(words[0..<8])) == 0)
            let full = string(path)
            #expect(full.hasSuffix(":Folder:Leaf") && !full.hasPrefix(":"))
            let resolved = buffer(512)
            #expect(Int16(truncatingIfNeeded: call("ResolvePath", [cstr(":Folder"), resolved, isFolder, isFolder + 1])) == 0)
            #expect(string(resolved) == String(full.dropLast(4)) && rd8(isFolder) == 1)
            // A path longer than a Str255, a folder at a time.
            let names = (1...9).map { "Folder number \($0) " + String(repeating: "x", count: 14) } // 31 characters
            var deep = dir
            for n in names { deep = deep.appendingPathComponent(n) }
            try? FileManager.default.createDirectory(at: deep, withIntermediateDirectories: true)
            FileManager.default.createFile(atPath: deep.appendingPathComponent("Deep").path, contents: Data("y".utf8))
            let long = ":" + names.joined(separator: ":") + ":Deep"
            #expect(long.utf8.count > 255)
            let name = buffer(64)
            #expect(Int16(truncatingIfNeeded: call("FSMakeFSSpec_Long", [cstr(long), name, 0, 0, spec])) == 0)
            #expect(pascal(spec + 6) == "Deep" && pascal(name) == "Deep")
            let longPath = buffer(1024)
            for i in 8..<18 { wr32(sp + 24 + UInt32(4 * i), rd32(spec + UInt32(i * 4))) }
            wr32(sp + 24 + 4 * 18, longPath)
            wr32(sp + 24 + 4 * 19, 1024)
            #expect(call("FSSpec2Path_Long", (0..<8).map { rd32(spec + UInt32($0 * 4)) }) == 0)
            #expect(string(longPath).hasSuffix(long))
            let f = call("FSp_fopen", [spec, cstr("r")])
            #expect(f != 0 && call("fgetc", [f]) == 0x79)
        }

        // MARK: Time

        @Test func timeIsTheMacs() {
            bootMachine()
            let t = buffer(4)
            wr32(t, 0)
            let tm = call("localtime", [t])
            #expect((0..<8).map { Int32(bitPattern: rd32(tm + UInt32($0 * 4))) } == [0, 0, 0, 1, 0, 4, 5, 0])
            #expect(string(call("asctime", [tm])) == "Fri Jan  1 00:00:00 1904\r")
            // mktime brings the fields into range.
            for (i, v) in [75, 59, 23, 31, 11, 99, 0, 0, 0].enumerated() { wr32(tm + UInt32(i * 4), UInt32(bitPattern: Int32(v))) }
            let secs = call("mktime", [tm])
            #expect((0..<8).map { Int32(bitPattern: rd32(tm + UInt32($0 * 4))) } == [15, 0, 0, 1, 0, 100, 6, 0])
            wr32(t, secs)
            #expect(string(call("ctime", [t])) == "Sat Jan  1 00:00:15 2000\r")
            #expect(secs == 3_029_529_615)
            let s = buffer(64)
            #expect(call("strftime", [s, 64, cstr("%A %d %B %Y %H:%M:%S %j %p %%"), tm]) == 42)
            #expect(string(s) == "Saturday 01 January 2000 00:00:15 001 AM %")
            #expect(call("strftime", [s, 5, cstr("%Y-%m"), tm]) == 0)
            call("difftime", [100, 40])
            #expect(f1 == 60)
            #expect(call("time", [0]) == rd32(UInt32(LM_Time)) && call("clock", []) == rd32(UInt32(LM_Ticks)))
            // gmtime: the host's offset from UTC away.
            wr32(t, 3_029_529_615)
            let g = call("gmtime", [t])
            let utc = Date(timeIntervalSince1970: 946_684_815) // 2000-01-01 00:00:15 local, as UTC
            let offset = TimeZone.current.secondsFromGMT(for: utc.addingTimeInterval(-Double(TimeZone.current.secondsFromGMT(for: utc))))
            let shifted = Int(rd32(g + 8)) * 3600 + Int(rd32(g + 4)) * 60 + Int(rd32(g))
            #expect((15 - offset + 86400 * 2) % 86400 == shifted % 86400)
        }

        // MARK: stdlib

        @Test func numbersAndStrings() {
            bootMachine()
            let end = buffer(4)
            let s = cstr("  -12.5e1xyz")
            call("strtod", [s, end])
            #expect(f1 == -125 && rd32(end) == s + 9)
            #expect(call("strtol", [cstr("99999999999"), end, 10]) == 0x7FFF_FFFF && rd32(errno) == 34)
            call("strtoll", [cstr("-9000000000"), 0, 10])
            #expect(Int64(Int32(bitPattern: g_ppc.gpr.3)) << 32 | Int64(r4) == -9_000_000_000)
            call("llabs", [0xFFFF_FFFF, 0xFFFF_FFFE])
            #expect(g_ppc.gpr.3 == 0 && r4 == 2)
            let q = buffer(16)
            call("div", [q, UInt32(bitPattern: -7), 2])
            #expect(Int32(bitPattern: rd32(q)) == -3 && Int32(bitPattern: rd32(q + 4)) == -1)
            call("lldiv", [q, 0, 100, 0, 7])
            #expect(rd32(q + 4) == 14 && rd32(q + 12) == 2)
            let t = cstr("a,b;;c")
            #expect(string(call("strtok", [t, cstr(",;")])) == "a")
            #expect(string(call("strtok", [0, cstr(",;")])) == "b")
            #expect(string(call("strtok", [0, cstr(",;")])) == "c" && call("strtok", [0, cstr(",;")]) == 0)
            #expect(string(call("strerror", [2])) == "No such file or directory" && string(call("strerror", [99])) == "Unknown error")
            let h = cstr("hello world")
            #expect(call("strpbrk", [h, cstr("ow")]) == h + 4 && call("strspn", [h, cstr("leh")]) == 4)
            #expect(call("strcspn", [h, cstr(" ")]) == 5 && call("toascii", [0xC1]) == 0x41)
            let x = buffer(8)
            #expect(call("strxfrm", [x, cstr("abc"), 8]) == 3 && string(x) == "abc")
            #expect(call("memccpy", [x, cstr("xyz"), UInt32(UInt8(ascii: "y")), 3]) == x + 2)
            let dp = buffer(4), sign = buffer(4)
            #expect(string(call("ecvt", [0, 0, 4, dp, sign], f1: -3.14159)) == "3142" && rd32(dp) == 1 && rd32(sign) == 1)
            #expect(string(call("fcvt", [0, 0, 2, dp, sign], f1: 12.3456)) == "1235" && rd32(dp) == 2)
            // The multibyte functions: bytes, and two-byte wchar_t.
            let w = buffer(8)
            #expect(int("mblen", [cstr("a"), 1]) == 1 && int("mbtowc", [w, cstr("Z"), 1]) == 1 && rd16(w) == 0x5A)
            #expect(call("mbstowcs", [w, cstr("ab"), 4]) == 2 && call("wcstombs", [x, w, 4]) == 2 && string(x) == "ab")
            // getenv finds what setenv set, and nothing else.
            #expect(call("getenv", [cstr("HOME")]) == 0)
            #expect(int("setenv", [cstr("NAME"), cstr("value")]) == 0 && string(call("getenv", [cstr("NAME")])) == "value")
            #expect(call("system", [0]) == 0)
            let v = call("vec_malloc", [100])
            #expect(v % 16 == 0 && call("vec_realloc", [v, 200]) % 16 == 0)
        }

        @Test func callbacksSortSearchAndSignals() {
            launchTestApp()
            giveStack()
            // int compare(const int *a, const int *b) { return *a - *b; }
            let compare = routine([0x8063_0000, 0x8084_0000, 0x7C64_1850, 0x4E80_0020])
            let list = bytes([0, 0, 0, 9, 0, 0, 0, 2, 0, 0, 0, 7, 0, 0, 0, 4])
            call("qsort", [list, 4, 4, compare])
            #expect((0..<4).map { rd32(list + UInt32($0 * 4)) } == [2, 4, 7, 9])
            let key = bytes([0, 0, 0, 7])
            #expect(call("bsearch", [key, list, 4, 4, compare]) == list + 8)
            wr32(key, 5)
            #expect(call("bsearch", [key, list, 4, 4, compare]) == 0)

            // A handler that keeps the signal it got.
            let got = buffer(4)
            let handler = routine(loadR4(got) + [0x9064_0000, 0x4E80_0020])
            #expect(call("signal", [2, handler]) == 1) // SIGINT's default was SIG_DFL
            #expect(int("raise", [2]) == 0 && rd32(got) == 2)
            #expect(call("signal", [2, 0]) == 1) // back to SIG_DFL after it ran
            #expect(int("raise", [2]) == 0)      // SIG_IGN now
            #expect(call("signal", [3, handler]) == 0xFFFF_FFFF && rd32(errno) == 22)
            // SIG_HOLD keeps it until SIG_RELEASE.
            wr32(got, 0)
            call("signal", [32, handler])
            call("signal", [32, 3])
            #expect(int("raise", [32]) == 0 && rd32(got) == 0)
            call("signal", [32, 5])
            #expect(rd32(got) == 32)

            // atexit procedures run last first at exit, then the streams.
            let order = buffer(16)
            func adder(_ v: UInt32) -> UInt32 {
                // order[n++] = v, n at order+4
                routine(loadR4(order) + [0x80A4_0004, 0x38C0_0000 | v, 0x7CC4_292E, 0x38A5_0004, 0x90A4_0004, 0x4E80_0020])
            }
            wr32(order + 4, 8)
            #expect(int("atexit", [adder(1)]) == 0 && int("atexit", [adder(2)]) == 0)
            let f = call("fopen", [cstr("AtExit"), cstr("w")])
            call("fputs", [cstr("kept"), f])
            call("exit", [3])
            #expect(rd32(order + 4) == 16)
            #expect(rd32(order + 8) == 2)
            #expect(rd32(order + 12) == 1)
            #expect(rd32(ppc_data_export("_exit_status")) == 3)
            #expect(g_cpu!.pointee.halted)
        }

        @Test func setjmpAndLongjmp() {
            bootMachine()
            let jb = buffer(256)
            g_ppc.lr = 0x1234_5678
            g_ppc.gpr.1 = 0x0010_0000
            g_ppc.gpr.20 = 0xABCD
            #expect(call("setjmp", [jb]) == 0)
            #expect(rd32(jb) == 0x1234_5678 && rd32(jb + 8) == 0x0010_0000 && rd32(jb + 20 + 7 * 4) == 0xABCD)
            g_ppc.gpr.20 = 0
            g_ppc.gpr.1 = 0
            #expect(call("longjmp", [jb, 0]) == 1)
            #expect(g_ppc.pc == 0x1234_5678 && g_ppc.gpr.1 == 0x0010_0000 && g_ppc.gpr.20 == 0xABCD)
        }

        @Test func localeIsC() {
            bootMachine()
            #expect(string(call("setlocale", [1, 0])) == "C" && string(call("setlocale", [5, cstr("")])) == "C")
            #expect(call("setlocale", [1, cstr("fr_FR")]) == 0 && call("setlocale", [9, cstr("C")]) == 0)
            let lc = call("localeconv", [])
            #expect(lc == ppc_data_export("__loc") && string(rd32(lc)) == "." && string(rd32(lc + 4)).isEmpty && rd8(lc + 40) == 0x7F)
            #expect(string(call("MakeTheLocaleString", [])) == "#C#C#C#C#C")
            #expect(int("ParseTheLocaleString", [cstr("#C#C#C#C#C"), buffer(8)]) == 0)
        }

        // MARK: Pascal strings and data

        @Test func pascalStrings() {
            bootMachine()
            let s = buffer(256)
            #expect(call("PLstrcpy", [s, pstr("Hello")]) == s && pascal(s) == "Hello")
            call("PLstrcat", [s, pstr(", world")])
            #expect(pascal(s) == "Hello, world" && call("PLstrlen", [s]) == 12)
            #expect(Int16(truncatingIfNeeded: call("PLstrcmp", [pstr("abc"), pstr("abd")])) < 0)
            #expect(call("PLstrncmp", [pstr("abcx"), pstr("abcy"), 3]) == 0)
            #expect(call("PLstrchr", [s, UInt32(UInt8(ascii: "o"))]) == s + 5 && call("PLstrrchr", [s, UInt32(UInt8(ascii: "o"))]) == s + 9)
            #expect(call("PLpos", [s, pstr("world")]) == 8 && call("PLpos", [s, pstr("moon")]) == 0)
            #expect(call("PLstrstr", [s, pstr("lo")]) == s + 4 && call("PLstrpbrk", [s, pstr(",w")]) == s + 6)
            #expect(call("PLstrspn", [s, pstr("Hel")]) == 4)
            call("PLstrncpy", [s, pstr("truncate"), 5])
            #expect(pascal(s) == "trunc")
            call("PLstrncat", [s, pstr("ated!"), 4])
            #expect(pascal(s) == "truncated")
            #expect(call("_c2pstrcpy", [s, cstr("C to P")]) == s && pascal(s) == "C to P")
            let c = cstr("in place")
            #expect(call("__SmartC2Pstr", [c]) == c && pascal(c) == "in place")
            let hex = buffer(8)
            call("binhex", [0xBEEF, hex, 6])
            #expect(string(hex) == "00BEEF")
        }

        @Test func dataExports() {
            bootMachine()
            func float(_ p: UInt32) -> Float { Float(bitPattern: rd32(p)) }
            #expect(float(ppc_data_export("_FLT_EPSILON")) == .ulpOfOne && float(ppc_data_export("_FLT_MAX")) == .greatestFiniteMagnitude)
            #expect(float(ppc_data_export("_FLT_MIN")) == .leastNormalMagnitude)
            #expect(double(ppc_data_export("_LDBL_EPSILON")) == .ulpOfOne && double(ppc_data_export("_LDBL_MAX") + 8) == 0)
            #expect(double(ppc_data_export("_DBL_MAX")) == .greatestFiniteMagnitude)
            #expect(double(ppc_data_export("__huge_val")) == .infinity)
            #expect(rd32(ppc_data_export("StandAlone")) == 1)
            let yd = ppc_data_export("__yd")
            #expect(rd32(yd + 4) == 31 && rd32(yd + 8) == 59 && rd32(yd + 44) == 334)
            #expect(call("__geterrno", []) == errno)
            call("_uerror", [13, UInt32(bitPattern: -54)])
            #expect(rd32(errno) == 13 && Int16(truncatingIfNeeded: rd16(ppc_data_export("MacOSErr"))) == -54)
            for name in ["MoneyData", "NumericData", "TimeData", "_PublicTimeInfo", "__SigEnv", "_CategoryLoc", "NoMoreDebugStr"] {
                #expect(ppc_data_export(name) != 0, "\(name)")
            }
            #expect(string(rd32(ppc_data_export("TimeData") + 4)) == "Sunday")
        }

        // MARK: The runtime

        @Test func runtimeCalls() {
            launchTestApp()
            let argc = buffer(4), argv = buffer(4), envp = buffer(4)
            wr32(argc, 99)
            #expect(call("_RTInit", [0, argc, argv, envp, 0]) == 0 && rd32(argc) == 0 && rd32(rd32(argv)) == 0)
            #expect(call("__GetTrapType", [0xA9F0]) == 1 && call("__GetTrapType", [0xA002]) == 0)
            #expect(call("__NumToolboxTraps", []) == 0x400)
            #expect(call("TrapAvailable", [0xA9F0]) != 0)
            let save = buffer(12), zone = tb_GetZone(), a5 = g_cpu!.pointee.a.5
            call("__RestoreInitialCFragWorld", [save])
            #expect(rd32(save) == zone && rd32(save + 4) == a5)
            tb_SetZone(rd32(UInt32(LM_SysZone)))
            g_cpu!.pointee.a.5 = 0x1000
            call("__RevertCFragWorld", [save])
            #expect(tb_GetZone() == zone && g_cpu!.pointee.a.5 == a5)
            let p = call("_SA_NewPtr", [64])
            #expect(p != 0 && call("_SA_GetPtrSize", [p]) == 64 && call("_SA_SetPtrSize", [p, 32]) == p)
            #expect(call("_SA_DisposePtr", [p]) == 0 && call("getpid", []) != 0)
            let id = cstr("....")
            call("getIDstring", [id])
            #expect(!string(id).contains("."))
        }
    }
}
