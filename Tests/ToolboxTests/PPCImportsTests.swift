import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Library calls PowerPC programs import that have no 68k trap of their
    /// own: the Slot Manager's answers, the Sound Converter, StdCLib's
    /// descriptors, Core Foundation's strings and odds and ends.
    @Suite struct PowerPCImports {
        /// Call an export with its arguments in r3 on; r3 after.
        @discardableResult
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        func result16(_ name: String, _ args: [UInt32]) -> Int16 { Int16(truncatingIfNeeded: call(name, args)) }

        func bytes(_ b: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(max(b.count, 1)), false, true)
            for (i, x) in b.enumerated() { wr8(p + UInt32(i), x) }
            return p
        }

        func cstr(_ s: String) -> UInt32 { bytes(Array(s.utf8) + [0]) }

        func read(_ p: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { rd8(p + UInt32($0)) } }

        /// A PowerPC program's asynchronous PB call sets ioResult and runs
        /// its completion routine later, at interrupt time, as a 68k one's
        /// does (Star Wars DroidWorks waits in YieldToAnyThread for it).
        @Test func asyncCallsRunTheirCompletion() {
            bootMachine()
            let stack = tb_NewPtr(65536, false, true)
            g_cpu!.pointee.a.7 = stack + 65536 - 16
            let flag = tb_NewPtr(4, false, true)
            // MOVE.W D0,flag; ADDQ.W #1,flag+2; RTS
            let words: [UInt16] = [0x33C0, UInt16(flag >> 16), UInt16(flag & 0xFFFF),
                                   0x5279, UInt16((flag + 2) >> 16), UInt16((flag + 2) & 0xFFFF), 0x4E75]
            let code = bytes(words.flatMap { [UInt8($0 >> 8), UInt8($0 & 0xFF)] })
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr32(pb + UInt32(IOParam_ioCompletion_off), code)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -1000)) // no such file
            wr16(pb + UInt32(IOParam_ioResult_off), 1)
            #expect(result16("PBReadAsync", [pb]) == -51) // rfNumErr
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioResult_off))) == -51)
            #expect(rd16(flag + 2) == 0) // not from inside the call
            time_pump()
            #expect(rd16(flag + 2) == 1 && Int16(bitPattern: rd16(flag)) == -51)
            wr16(pb + UInt32(IOParam_ioResult_off), 1)
            #expect(result16("PBReadSync", [pb]) == -51)
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioResult_off))) == -51)
            time_pump()
            #expect(rd16(flag + 2) == 1) // a synchronous call has no completion
        }

        // MARK: Code Fragment Manager and Mixed Mode

        /// A PEF container in guest memory: one 8-byte data section whose
        /// first word is relocated by one import (of class cls) from
        /// InterfaceLib, and main at the data section's start.
        func importContainer(_ symbol: String, cls: UInt8, library: String = "InterfaceLib", weak: Bool = false) -> UInt32 {
            func be32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8((v >> 16) & 0xFF), UInt8((v >> 8) & 0xFF), UInt8(v & 0xFF)] }
            func be16(_ v: UInt16) -> [UInt8] { [UInt8(v >> 8), UInt8(v & 0xFF)] }
            let strings = Array(library.utf8) + [0] + Array(symbol.utf8) + [0]
            var loader = be32(0) + be32(0) + be32(UInt32(bitPattern: -1)) + be32(0) + be32(UInt32(bitPattern: -1)) + be32(0)
            let stringsOff = UInt32(56 + 24 + 4 + 12 + 4)
            let hashOff = (stringsOff + UInt32(strings.count) + 3) & ~3
            loader += be32(1) + be32(1) + be32(1) + be32(56 + 24 + 4 + 12) + be32(stringsOff) + be32(hashOff) + be32(0) + be32(0)
            loader += be32(0) + be32(0) + be32(0) + be32(1) + be32(0) + [weak ? 0x40 : 0, 0] + be16(0) // the library
            loader += be32(UInt32(cls) << 24 | UInt32(library.utf8.count + 1))             // the symbol
            loader += be16(0) + be16(0) + be32(1) + be32(0)                                 // relocations for section 0
            loader += be16(0x6000) + be16(0)                                                // RelocSmByImport 0
            loader += strings
            while loader.count < Int(hashOff) { loader.append(0) }
            loader += be32(0)                                                               // one empty hash slot
            let dataOff = UInt32(40 + 2 * 28), loaderOff = dataOff + 8
            var pef = Array("Joy!peffpwpc".utf8) + be32(1) + be32(0) + be32(0) + be32(0) + be32(0) + be16(2) + be16(1) + be32(0)
            pef += be32(UInt32(bitPattern: -1)) + be32(0) + be32(8) + be32(8) + be32(8) + be32(dataOff) + [1, 1, 0, 0]
            pef += be32(UInt32(bitPattern: -1)) + be32(0) + be32(0) + be32(0) + be32(UInt32(loader.count)) + be32(loaderOff) + [4, 1, 0, 0]
            pef += [UInt8](repeating: 0, count: 8) + loader
            return bytes(pef)
        }

        /// CFM binds an import by its name: a routine a program imports as
        /// data (Hardwood Solitaire II's GetCurrentProcess) is still the
        /// routine's transition vector, not a block of zeros to call.
        @Test func routineImportedAsDataIsItsTransitionVector() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let main = cfm_prepare_container(importContainer("GetCurrentProcess", cls: 1))
            #expect(main != 0)
            let tv = rd32(main)
            #expect(tv != 0 && rd32(tv) != 0)
            #expect(tv == rd32(cfm_prepare_container(importContainer("GetCurrentProcess", cls: 2))))
        }

        /// A call through a missing weak import (a transition vector of 0)
        /// stops naming the import, found from the cross-TOC glue's slot.
        @Test func nilCallNamesTheWeakImport() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let toc = cfm_prepare_container(importContainer("Q3IsInitialized", cls: 2, library: "NoSuchLib", weak: true))
            #expect(toc != 0 && rd32(toc) == 0)
            // lwz r12,0(r2); stw r2,20(r1); lwz r0,0(r12); lwz r2,4(r12); mtctr r0; bctr
            let words: [UInt32] = [0x8182_0000, 0x9041_0014, 0x800C_0000, 0x804C_0004, 0x7C09_03A6, 0x4E80_0420]
            let glue = bytes(words.flatMap { [UInt8($0 >> 24), UInt8(($0 >> 16) & 0xFF), UInt8(($0 >> 8) & 0xFF), UInt8($0 & 0xFF)] })
            let stack = tb_NewPtr(4096, false, true)
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                gpr.storeBytes(of: stack + 2048, toByteOffset: 4, as: UInt32.self)
                gpr.storeBytes(of: toc, toByteOffset: 8, as: UInt32.self)
            }
            g_ppc.pc = glue
            g_ppc.halted = false
            _ = ppc_run(&g_ppc, 20)
            let reason = withUnsafeBytes(of: g_ppc.halt_reason) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
            #expect(reason.hasPrefix("illegal PowerPC instruction 00000000 at 00000000"))
            #expect(reason.hasSuffix(": weak Q3IsInitialized from NoSuchLib, not there"))
        }

        /// Mixed Mode calls a PowerPC routine with r12 at its transition
        /// vector, as a call through a pointer has it: Hardwood Solitaire
        /// II's trap patches read where to go on from their vectors.
        @Test func mixedModeCallsWithTheVectorInR12() {
            bootMachine()
            _ = cfm_ppc_cpu()
            // or r3,r12,r12; blr
            let code = bytes([0x7D, 0x83, 0x63, 0x78, 0x4E, 0x80, 0x00, 0x20])
            let tv = bytes([0, 0, 0, 0, 0, 0, 0, 0])
            wr32(tv, code)
            #expect(ppc_call(tv, nil, 0) == tv)
        }

        /// CallUniversalProc to a PowerPC routine moves every argument down
        /// two registers, however many the procInfo counts: Amadeus II
        /// passes its plug-ins a sixth (where to keep their data) under a
        /// procInfo for five, as the Mixed Mode it was written on allowed.
        @Test func callUniversalProcPassesEveryArgument() {
            bootMachine()
            _ = cfm_ppc_cpu()
            let tv = bytes([0, 0, 0, 0, 0, 0, 0, 0])
            wr32(tv, bytes([0x4E, 0x80, 0x00, 0x20])) // blr
            // A descriptor of one PowerPC routine taking six longs (Pascal).
            let rd = bytes([0xAA, 0xFE, 7, 0] + [UInt8](repeating: 0, count: 8 + 20))
            wr32(rd + 12, 0x3FFC0)
            wr8(rd + 17, 1) // kPowerPCISA
            wr32(rd + 20, tv)
            call("CallUniversalProc", [rd, 0xFFC0, 1, 2, 3, 4, 5, 6])
            #expect(g_ppc.pc == rd32(tv) && g_ppc.gpr.12 == tv)
            #expect([g_ppc.gpr.3, g_ppc.gpr.4, g_ppc.gpr.5, g_ppc.gpr.6, g_ppc.gpr.7, g_ppc.gpr.8] == [1, 2, 3, 4, 5, 6])
        }

        // MARK: Slot Manager

        /// SpBlock: spResult 0, spsPointer 4, spParamData 24, spRefNum 38,
        /// spCategory 40, spCType 42, spDrvrSW 44, spDrvrHW 46, spTBMask 48,
        /// spSlot 49, spID 50, spExtDev 51.
        /// GetHandleSize and GetPtrSize answer 0 for a bad handle or pointer,
        /// MemError the reason (the 68k trap's D0 has the result code).
        @Test func sizesOfBadHandlesAreZero() {
            bootMachine()
            #expect(call("GetHandleSize", [0]) == 0 && result16("MemError", []) == -109) // nilHandleErr
            let h = tb_NewHandle(24, false, false)
            #expect(call("GetHandleSize", [h]) == 24 && result16("MemError", []) == 0)
            tb_EmptyHandle(h)
            #expect(call("GetHandleSize", [h]) == 0)
            #expect(call("GetPtrSize", [0]) == 0 && call("GetPtrSize", [tb_NewPtr(10, false, false)]) == 10)
        }

        @Test func slotManagerFindsTheBuiltInVideo() {
            bootMachine()
            let spb = tb_NewPtr(56, false, true)
            wr16(spb + 40, 3) // catDisplay
            wr16(spb + 42, 1) // typeVideo
            wr16(spb + 44, 1) // drSwApple
            wr8(spb + 48, 1)  // any DrvrHW
            #expect(result16("SGetTypeSRsrc", [spb]) == 0)
            #expect(rd8(spb + 49) == 0 && rd8(spb + 50) == 0x80)
            let gd = rd32(rd32(UInt32(LM_MainDevice)))
            #expect(rd16(spb + 38) == rd16(gd + UInt32(GDevice_gdRefNum_off)))
            let video = rd32(spb + 4)
            #expect(result16("SGetTypeSRsrc", [spb]) == -344) // smNoMoresRsrcs: the only one

            // The name, and the 8-bit mode's parameters at the screen's size.
            wr32(spb + 4, video)
            wr8(spb + 50, 2) // sRsrcName
            #expect(result16("SGetCString", [spb]) == 0)
            let name = rd32(spb)
            #expect(String(decoding: read(name, 27), as: UTF8.self) == "Display_Video_Apple_BlueBox")
            wr8(spb + 50, 0x83)
            #expect(result16("SFindStruct", [spb]) == 0)
            let mode = rd32(spb + 4)
            wr8(spb + 50, 1) // mVidParams
            #expect(result16("SGetBlock", [spb]) == 0)
            let vp = rd32(spb)
            #expect(rd16(vp + 32) == 8)                     // vpPixelSize
            #expect(Int(rd16(vp + 12)) == Int(g_screen_width))  // vpBounds.right
            #expect(Int(rd16(vp + 10)) == Int(g_screen_height)) // vpBounds.bottom
            wr32(spb + 4, mode)
            wr8(spb + 50, 4) // mDevType: clutType
            #expect(result16("SReadWord", [spb]) == 0 && rd32(spb) == 0)
            wr8(spb + 50, 0x42)
            #expect(result16("SFindStruct", [spb]) == -330) // smBadRefId

            // Every sResource in turn (fnext): the board, the video, no more.
            let all = tb_NewPtr(56, false, true)
            wr32(all + 24, 4)
            var found = [UInt8]()
            while result16("SGetSRsrc", [all]) == 0 { found.append(rd8(all + 50)) }
            #expect(found == [1, 0x80])
            #expect(slot_manager(0x16, all) == 0) // SRsrcInfo on the last one found
        }

        // MARK: Sound Converter

        /// SoundComponentData: flags, format, numChannels, sampleSize, sampleRate, ...
        func soundFormat(_ format: String, channels: UInt16, bits: UInt16, rate: UInt32, flags: UInt32 = 0) -> UInt32 {
            let p = tb_NewPtr(28, false, true)
            wr32(p, flags)
            wr32(p + 4, fourCC(format))
            wr16(p + 8, channels)
            wr16(p + 10, bits)
            wr32(p + 12, rate << 16)
            return p
        }

        func open(_ input: UInt32, _ output: UInt32) -> (Int16, UInt32) {
            let scp = tb_NewPtr(4, false, true)
            let err = result16("SoundConverterOpen", [input, output, scp])
            return (err, rd32(scp))
        }

        @Test func soundConverterResamplesAndWidens() {
            bootMachine()
            let (err, sc) = open(soundFormat("raw ", channels: 1, bits: 8, rate: 11025),
                                 soundFormat("twos", channels: 2, bits: 16, rate: 22050))
            #expect(err == 0 && sc != 0)
            let sizes = tb_NewPtr(12, false, true)
            #expect(result16("SoundConverterGetBufferSizes", [sc, 100, sizes, sizes + 4, sizes + 8]) == 0)
            #expect(rd32(sizes) == 100 && rd32(sizes + 4) == 100)
            #expect(rd32(sizes + 8) >= 200 * 4)
            #expect(result16("SoundConverterBeginConversion", [sc]) == 0)

            // A ramp: offset binary 128, 129, ... is 0, 256, 512 ... as 16 bits.
            let input = bytes((0..<100).map { UInt8(128 + $0) })
            let out = tb_NewPtr(Int32(rd32(sizes + 8)), false, true)
            let counts = tb_NewPtr(8, false, true)
            #expect(result16("SoundConverterConvertBuffer", [sc, input, 100, out, counts, counts + 4]) == 0)
            let frames = Int(rd32(counts))
            #expect(frames >= 197 && frames <= 200)
            #expect(Int(rd32(counts + 4)) == frames * 4)
            func sample(_ i: Int, _ ch: Int) -> Int16 { Int16(bitPattern: rd16(out + UInt32(i * 4 + ch * 2))) }
            #expect(sample(0, 0) == 0 && sample(0, 1) == 0)
            #expect(sample(1, 0) == 128 && sample(1, 1) == 128) // halfway between 0 and 256
            #expect(sample(2, 0) == 256)
            // The next buffer carries on from the last sample of this one.
            #expect(result16("SoundConverterConvertBuffer", [sc, input, 100, out, counts, counts + 4]) == 0)
            #expect(Int(rd32(counts)) + frames >= 396 && Int(rd32(counts)) + frames <= 400)
            #expect(result16("SoundConverterEndConversion", [sc, out, counts, counts + 4]) == 0)
            #expect(rd32(counts) == 0)
            #expect(result16("SoundConverterClose", [sc]) == 0)
            #expect(result16("SoundConverterClose", [sc]) == -50)
        }

        @Test func soundConverterSwapsAndDecodes() {
            bootMachine()
            // Little-endian to big-endian at the same rate: the bytes swapped.
            var (err, sc) = open(soundFormat("sowt", channels: 1, bits: 16, rate: 22050),
                                 soundFormat("twos", channels: 1, bits: 16, rate: 22050))
            #expect(err == 0)
            let out = tb_NewPtr(256, false, true)
            let counts = tb_NewPtr(8, false, true)
            #expect(result16("SoundConverterConvertBuffer", [sc, bytes([0x34, 0x12, 0xFF, 0x7F]), 2, out, counts, counts + 4]) == 0)
            #expect(rd32(counts) == 2 && read(out, 4) == [0x12, 0x34, 0x7F, 0xFF])

            // IMA 4:1: a frame is a 34-byte packet of 64 samples; silence stays silent.
            (err, sc) = open(soundFormat("ima4", channels: 1, bits: 16, rate: 22050),
                             soundFormat("twos", channels: 1, bits: 16, rate: 22050))
            #expect(err == 0)
            let sizes = tb_NewPtr(12, false, true)
            #expect(result16("SoundConverterGetBufferSizes", [sc, 100, sizes, sizes + 4, sizes + 8]) == 0)
            #expect(rd32(sizes) == 2 && rd32(sizes + 4) == 68 && rd32(sizes + 8) == 256)
            #expect(result16("SoundConverterConvertBuffer", [sc, bytes([UInt8](repeating: 0, count: 34)), 1, out, counts, counts + 4]) == 0)
            #expect(rd32(counts) == 64 && read(out, 128).allSatisfy { $0 == 0 })

            // Compressed output isn't made.
            (err, sc) = open(soundFormat("twos", channels: 1, bits: 16, rate: 22050),
                             soundFormat("ima4", channels: 1, bits: 16, rate: 22050))
            #expect(err == -206 && sc == 0) // badFormat
        }

        @Test func parseSndHeaderDescribesTheSound() {
            bootMachine()
            let snd = SoundManager.snd(samples: 10)
            let h = tb_NewHandle(Int32(snd.count), false, true)
            for (i, b) in snd.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            let info = tb_NewPtr(28, false, true), out = tb_NewPtr(8, false, true)
            #expect(result16("ParseSndHeader", [h, info, out, out + 4]) == 0)
            #expect(rd32(info + 4) == fourCC("raw ") && rd16(info + 8) == 1 && rd16(info + 10) == 8)
            #expect(rd32(info + 12) == 0x56EE_8BA3)
            #expect(rd32(out) == 10 && rd32(out + 4) == 20 + 22)
        }

        // MARK: StdCLib

        @Test func stdCLibReadsAndWritesFiles() throws {
            let dir = launchTestApp()
            let errno = ppc_data_export("errno")
            let fd = call("open", [cstr("Notes"), 0x102]) // O_CREAT | O_RDWR
            #expect(Int32(bitPattern: fd) >= 3)
            let text = cstr("hello world")
            #expect(call("write", [fd, text, 11]) == 11)
            #expect(call("lseek", [fd, 6, 0]) == 6)
            let buf = tb_NewPtr(16, false, true)
            #expect(call("read", [fd, buf, 16]) == 5)
            #expect(String(decoding: read(buf, 5), as: UTF8.self) == "world")
            #expect(call("lseek", [fd, 0, 2]) == 11)
            #expect(call("read", [fd, buf, 16]) == 0) // at the end
            #expect(call("close", [fd]) == 0)
            #expect(Int32(bitPattern: call("close", [fd])) == -1 && rd32(errno) == 9) // EBADF
            let host = try String(contentsOf: dir.appendingPathComponent("Notes"), encoding: .utf8)
            #expect(host == "hello world")

            // Appending, and a file that isn't there.
            let fd2 = call("open", [cstr("Notes"), 0x9]) // O_WRONLY | O_APPEND
            #expect(call("write", [fd2, cstr("!"), 1]) == 1)
            #expect(call("close", [fd2]) == 0)
            #expect(try String(contentsOf: dir.appendingPathComponent("Notes"), encoding: .utf8) == "hello world!")
            #expect(Int32(bitPattern: call("open", [cstr("Missing"), 0])) == -1 && rd32(errno) == 2) // ENOENT
            // The console takes what's written and has nothing to read.
            #expect(call("write", [1, text, 5]) == 5 && call("read", [0, buf, 4]) == 0)
            #expect(Int32(bitPattern: call("lseek", [1, 0, 0])) == -1 && rd32(errno) == 22) // EINVAL, as MPW's console
        }

        // MARK: Core Foundation

        @Test func cfStringsFromBytes() {
            bootMachine()
            func chars(_ s: UInt32) -> [UInt16] {
                let n = call("CFStringGetLength", [s])
                let buf = tb_NewPtr(Int32(n * 2 + 2), false, true)
                call("CFStringGetCharacters", [s, 0, n, buf])
                return (0..<Int(n)).map { rd16(buf + UInt32($0 * 2)) }
            }
            let cafe = call("CFStringCreateWithCString", [0, bytes([0x43, 0x61, 0x66, 0x8E, 0]), 0]) // MacRoman é
            #expect(chars(cafe) == [0x43, 0x61, 0x66, 0xE9])
            let euro = call("CFStringCreateWithBytes", [0, bytes([0xE2, 0x82, 0xAC]), 3, 0x0800_0100, 0]) // UTF-8
            #expect(chars(euro) == [0x20AC])
            #expect(call("CFStringCreateWithBytes", [0, bytes([0xC3]), 1, 0x0800_0100, 0]) == 0) // cut short
            let units = bytes([0, 0x41, 0, 0x42])
            let nocopy = call("CFStringCreateWithCharactersNoCopy", [0, units, 2, 0])
            #expect(chars(nocopy) == [0x41, 0x42])
            // A range of the characters.
            let buf = tb_NewPtr(8, false, true)
            call("CFStringGetCharacters", [cafe, 1, 2, buf])
            #expect(rd16(buf) == 0x61 && rd16(buf + 2) == 0x66)

            // CFSTR: one string a literal, kept through CFRelease.
            let literal = cstr("Snake")
            let k = call("__CFStringMakeConstantString", [literal])
            #expect(k != 0 && call("__CFStringMakeConstantString", [literal]) == k)
            call("CFRelease", [k])
            #expect(chars(k).count == 5)
            let url = call("CFURLCreateWithFileSystemPath", [0, k, 1, 0])
            #expect(url != 0)
            #expect(call("CFBundleCreate", [0, url]) == 0)
            call("CFRelease", [url])
            call("CFRelease", [cafe])
        }

        // MARK: Odds and ends

        @Test func validDateAndFormatOrder() {
            bootMachine()
            // LongDateRec: era, year, month, day, hour, minute, second ...
            let d = tb_NewPtr(28, false, true)
            for (i, v) in [0, 2024, 2, 30, 12, 0, 0].enumerated() { wr16(d + UInt32(i * 2), UInt16(v)) }
            let secs = tb_NewPtr(8, false, true)
            #expect(result16("ValidDate", [d, 0x7F, secs]) == 3) // dayField
            wr16(d + 6, 29)
            #expect(result16("ValidDate", [d, 0x7F, secs]) == -1)
            #expect(rd32(secs + 4) != 0)
            wr16(d + 2, 2050)
            #expect(result16("ValidDate", [d, 0x8000_007F, secs]) == 1) // smallDateBit: past 2040

            // Left-to-right runs keep their order.
            let order = tb_NewPtr(8, false, true)
            call("GetFormatOrder", [order, 1, 4, 0, 0, 0])
            #expect((0..<4).map { rd16(order + UInt32($0 * 2)) } == [1, 2, 3, 4])
        }

        @Test func cursorDevicesAreTheProgramsOwn() {
            bootMachine()
            let p = tb_NewPtr(4, false, true)
            #expect(result16("CrsrDevNextDevice", [p]) == 0 && rd32(p) == 0)
            #expect(result16("CrsrDevNewDevice", [p]) == 0)
            let dev = rd32(p)
            #expect(dev != 0 && rd8(dev + 25) == 1) // one button
            #expect(result16("CrsrDevUnitsPerInch", [dev, 200 << 16]) == 0 && rd32(dev + 20) == 200 << 16)
            #expect(result16("CrsrDevSetButtons", [dev, 3]) == 0 && rd8(dev + 25) == 3)
            #expect(result16("CrsrDevButtons", [dev, 1]) == 0 && rd8(dev + 27) == 1)
            wr32(p, 0)
            #expect(result16("CrsrDevNextDevice", [p]) == 0 && rd32(p) == dev)
            #expect(result16("CrsrDevNextDevice", [p]) == 0 && rd32(p) == 0)
            #expect(result16("CrsrDevDisposeDevice", [dev]) == 0)
            #expect(result16("CrsrDevNextDevice", [p]) == 0 && rd32(p) == 0)
        }

        /// LMGetEventQueue and the other queue headers' getters take no
        /// argument and answer the header's address; copying the header to
        /// wherever r3 pointed overwrote Director 5's score in Squark.
        @Test func queueHeaderGettersAnswerTheirAddress() {
            bootMachine()
            let p = bytes([UInt8](repeating: 0xA5, count: 16))
            #expect(call("LMGetEventQueue", [p]) == 0x14A)
            #expect(call("LMGetVBLQueue", [p]) == 0x160)
            #expect(call("LMGetVCBQHdr", [p]) == 0x356)
            #expect(read(p, 16) == [UInt8](repeating: 0xA5, count: 16))
            // A Point still comes back through the pointer.
            wr32(0x828, 0x0012_0034)
            call("LMGetMTemp", [p])
            #expect(rd32(p) == 0x0012_0034)
        }

        /// Microseconds follows the machine's clock, as Ticks does: in a
        /// headless run that is the instructions executed (Director 5 paces
        /// its frames by Microseconds and Lingo's timer by Ticks).
        @Test func microsecondsFollowTheMachinesClock() {
            bootMachine()
            let p = tb_NewPtr(8, false, true)
            func micros() -> UInt64 {
                call("Microseconds", [p])
                return UInt64(rd32(p)) << 32 | UInt64(rd32(p + 4))
            }
            let before = micros()
            #expect(before <= machine_time_ns() / 1000 && machine_time_ns() / 1000 - before < 1000)
            g_cpu!.pointee.icount += 10_000_000 // a second of instructions
            let after = micros()
            #expect(after - before >= 1_000_000 && after - before < 1_010_000)
        }
    }
}
