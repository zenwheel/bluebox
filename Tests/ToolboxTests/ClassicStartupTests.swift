import CCore
import CPU68K
import Foundation
import ForkStore
import Testing
import Toolbox

/// A file with a resource fork and nothing else, as AppleDouble.
func writeResourceFile(_ resources: [TestResource], name: String, in dir: URL) -> String {
    var fork = buildFork(resources)
    var f = mac_file()
    var empty: [UInt8] = []
    var err = [CChar](repeating: 0, count: 256)
    let path = dir.appendingPathComponent(name).path
    empty.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = 0 }
    fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
    #expect(appledouble_write(path, &f, &err, err.count))
    return path
}

/// Words of 68k code as the bytes of a resource.
func code68k(_ words: [UInt16]) -> [UInt8] { words.flatMap { be16(Int($0)) } }

extension BlueBox {
    /// System 6's startup pieces: INITs, Chain and a cdev in a window.
    @Suite struct ClassicStartup {
        init() { startup_reset() }

        /// A file's INIT resources run in order of ID, in the system heap,
        /// before the program does.
        @Test func initsRunInOrderOfID() {
            launchTestApp()
            let log = tb_NewPtr(16, false, true)
            func body(_ tag: UInt16) -> [UInt8] {
                // MOVEA.L #log,A0; MOVE.W (A0),D0; LEA 2(A0),A1; ADDA.W D0,A1; ADDA.W D0,A1;
                // MOVE.W #tag,(A1); ADDQ.W #1,(A0); RTS
                code68k([0x207C, UInt16(log >> 16), UInt16(log & 0xFFFF), 0x3010, 0x43E8, 0x0002, 0xD2C0, 0xD2C0,
                         0x32BC, tag, 0x5250, 0x4E75])
            }
            let dir = tempDir()
            let path = writeResourceFile([
                TestResource(type: "INIT", id: 20, data: body(20)),
                TestResource(type: "INIT", id: 3, data: body(3)),
                TestResource(type: "INIT", id: 11, data: body(11)),
            ], name: "Two INITs", in: dir)
            var err = [CChar](repeating: 0, count: 256)
            let before = tb_CurResFile()
            #expect(startup_run_init_file(path, &err, err.count) == 3)
            #expect(rd16(log) == 3)
            #expect([2, 4, 6].map { rd16(log + UInt32($0)) } == [3, 11, 20])
            // and the resource file that was current is again
            #expect(tb_CurResFile() == before)
        }

        /// A file with no INIT runs none.
        @Test func aFileWithoutInitsRunsNone() {
            launchTestApp()
            let path = writeResourceFile([TestResource(type: "STR ", id: 1, data: [1, 65])], name: "Plain", in: tempDir())
            var err = [CChar](repeating: 0, count: 256)
            #expect(startup_run_init_file(path, &err, err.count) == 0)
        }

        nonisolated(unsafe) static var launched: [String] = []

        /// _Chain takes Launch's record and replaces the program.
        @Test func chainLaunchesAnotherProgram() {
            let dir = launchTestApp()
            let other = dir.appendingPathComponent("Other App")
            FileManager.default.createFile(atPath: other.path, contents: Data([0]))
            Self.launched = []
            machine_set_launch_handler({ p, _, _, _ in
                ClassicStartup.launched.append(String(cString: p!))
                return true
            }, nil)
            defer { machine_set_launch_handler(nil, nil) }
            let name = tb_NewPtr(16, false, true)
            guestPString("Other App", at: name)
            let rec = tb_NewPtr(8, false, true)
            wr32(rec, name)
            let reason = run68k([0x207C, UInt16(rec >> 16), UInt16(rec & 0xFFFF), 0xA9F3, 0xA9F4])
            #expect(Self.launched.count == 1 && Self.launched[0].hasSuffix("/Other App"))
            #expect(reason.hasPrefix("ExitToShell"))
        }

        /// A cdev gets the Control Panel's messages: macDev, initDev and
        /// activDev first, nulDev while it idles, and a click on its button
        /// is hitDev with the item.
        @Test func cdevReceivesTheMessages() {
            bootMachine()
            let log = tb_NewPtr(256, false, true)
            // pascal long main(message, item, numItems, id, event, value, dialog)
            let main: [UInt16] = [
                0x302F, 0x0016,                                    // MOVE.W 22(SP),D0   the message
                0x207C, UInt16(log >> 16), UInt16(log & 0xFFFF),   // MOVEA.L #log,A0
                0x3210,                                            // MOVE.W (A0),D1     the count
                0x43E8, 0x0002,                                    // LEA 2(A0),A1
                0xD2C1, 0xD2C1,                                    // ADDA.W D1,A1 twice
                0x3280,                                            // MOVE.W D0,(A1)
                0x5250,                                            // ADDQ.W #1,(A0)
                0x2F6F, 0x0008, 0x0018,                            // MOVE.L 8(SP),24(SP) the value is the result
                0x225F, 0xDEFC, 0x0014, 0x4ED1,                    // MOVEA.L (SP)+,A1; ADDA #20,SP; JMP (A1)
            ]
            // DITL: one button "OK" at 10,10,30,80
            let ditl = cat(be16(0), be32(0), be16(10), be16(10), be16(30), be16(80), [4, 2], Array("OK".utf8))
            let path = writeResourceFile([
                TestResource(type: "cdev", id: -4064, data: code68k(main)),
                TestResource(type: "DITL", id: -4064, data: ditl),
            ], name: "Test cdev", in: tempDir())
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_launch(path, &err, err.count))
            machine_set_instruction_limit(g_cpu!.pointee.icount + 300_000) // the host loop idles to it
            defer { machine_set_instruction_limit(0) }
            _ = machine_run(400_000)
            let n = Int(rd16(log))
            let seen = (0..<n).map { rd16(log + 2 + UInt32($0 * 2)) }
            #expect(Array(seen.prefix(3)) == [8, 0, 5]) // macDev, initDev, activDev
            #expect(seen.dropFirst(3).contains(3))      // nulDev
        }
    }
}

extension BlueBox {
    /// A key disk: a floppy image the program's own copy protection reads
    /// through the Sony driver.
    @Suite struct KeyDisk {
        init() { sony_reset() }

        /// 400K of sectors, each filled with its own number, in a file.
        func sectorImage(sectors: Int = 800, name: String = "key.img") -> String {
            var d = [UInt8]()
            for s in 0..<sectors { d += [UInt8](repeating: UInt8(s & 0xFF), count: 512) }
            let url = tempDir().appendingPathComponent(name)
            try! Data(d).write(to: url)
            return url.path
        }

        func readBlocks(drive: Int16, offset: UInt32, count: UInt32, into buf: UInt32) -> Int16 {
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -5))
            wr16(pb + UInt32(IOParam_ioVRefNum_off), UInt16(bitPattern: drive))
            wr32(pb + UInt32(IOParam_ioBuffer_off), buf)
            wr32(pb + UInt32(IOParam_ioReqCount_off), count)
            wr16(pb + UInt32(IOParam_ioPosMode_off), 1) // fsFromStart
            wr32(pb + UInt32(IOParam_ioPosOffset_off), offset)
            return tb_PBRead(pb, false)
        }

        @Test func sectorsReadThroughTheSonyDriver() {
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_set_floppy(sectorImage(), &err, err.count))
            launchTestApp()
            defer { sony_reset() }
            let buf = tb_NewPtr(2048, false, true)
            #expect(readBlocks(drive: 1, offset: 5 * 512, count: 1024, into: buf) == 0)
            #expect(rd8(buf) == 5 && rd8(buf + 511) == 5 && rd8(buf + 512) == 6 && rd8(buf + 1023) == 6)
            // the drive of its own in the queue reads the same
            var found: Int16 = 0
            var e = rd32(UInt32(LM_DrvQHdr) + 2)
            while e != 0 {
                if rd16(e + UInt32(DrvQEl_dQDrive_off)) > 1 && rd16(e + UInt32(DrvQEl_dQRefNum_off)) == 0xFFFB {
                    found = Int16(bitPattern: rd16(e + UInt32(DrvQEl_dQDrive_off)))
                }
                e = rd32(e)
            }
            #expect(found > 1)
            #expect(readBlocks(drive: found, offset: 799 * 512, count: 512, into: buf) == 0 && rd8(buf) == UInt8(799 & 0xFF))
            // past the end, and not whole sectors: paramErr
            #expect(readBlocks(drive: 1, offset: 800 * 512, count: 512, into: buf) == -50)
            #expect(readBlocks(drive: 1, offset: 100, count: 512, into: buf) == -50)
        }

        @Test func noDiskMeansTheDriveIsOffline() {
            launchTestApp()
            let buf = tb_NewPtr(512, false, true)
            #expect(readBlocks(drive: 1, offset: 0, count: 512, into: buf) == -65)
        }

        /// A Disk Copy 4.2 image has the tag bytes the protection looks at.
        @Test func tagBytesGoToTheTagBuffer() {
            var d = [UInt8](repeating: 0, count: 84)
            d[0] = 4; d.replaceSubrange(1..<5, with: Array("Test".utf8))
            d.replaceSubrange(64..<68, with: be32(409600)) // data size
            d.replaceSubrange(68..<72, with: be32(800 * 12)) // tag size
            d[80] = 0; d[81] = 2; d[82] = 1; d[83] = 0
            for s in 0..<800 { d += [UInt8](repeating: UInt8(s & 0xFF), count: 512) }
            for s in 0..<800 { d += (0..<12).map { UInt8((s + $0) & 0xFF) } }
            let url = tempDir().appendingPathComponent("key.dc42")
            try! Data(d).write(to: url)
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_set_floppy(url.path, &err, err.count))
            launchTestApp()
            defer { sony_reset() }
            #expect(sony_disk_size() == 409600)
            let tags = tb_NewPtr(16, false, true)
            let pb = tb_NewPtr(Int32(CntrlParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -5))
            wr16(pb + UInt32(IOParam_ioVRefNum_off), 1)
            wr16(pb + UInt32(CntrlParam_csCode_off), 8) // SetTagBuffer
            wr32(pb + UInt32(CntrlParam_csParam_off), tags)
            #expect(tb_PBControl(pb, false) == 0)
            let buf = tb_NewPtr(512, false, true)
            #expect(readBlocks(drive: 1, offset: 7 * 512, count: 512, into: buf) == 0)
            #expect((0..<12).map { rd8(tags + UInt32($0)) } == (0..<12).map { UInt8(7 + $0) })
            // the drive's status: a disk in place, installed, two sides
            wr16(pb + UInt32(CntrlParam_csCode_off), 8)
            #expect(tb_PBStatus(pb, false) == 0)
            let st = pb + UInt32(CntrlParam_csParam_off)
            #expect(rd8(st + UInt32(DrvSts_diskInPlace_off)) == 1 && rd8(st + UInt32(DrvSts_installed_off)) == 1)
            // ejected: offline
            wr16(pb + UInt32(CntrlParam_csCode_off), 7)
            #expect(tb_PBControl(pb, false) == 0)
            #expect(readBlocks(drive: 1, offset: 0, count: 512, into: buf) == -65)
        }
    }
}

extension BlueBox {
    /// The pre-Sound Manager Sound Driver and System 6's SysEnvirons.
    @Suite struct SystemSixInterfaces {
        nonisolated(unsafe) static var chunks: [(frames: Int32, rate: Double, volume: Float, pcm: [Int16])] = []

        func capture() {
            Self.chunks = []
            machine_set_sound_handler({ _, pcm, frames, _, rate, volume, _ in
                if let pcm { SystemSixInterfaces.chunks.append((frames, rate, volume, Array(UnsafeBufferPointer(start: pcm, count: Int(frames))))) }
            }, nil)
        }

        func write(_ rec: UInt32, length: UInt32, async: Bool = true) -> (pb: UInt32, err: Int16) {
            let pb = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(pb + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -4))
            wr32(pb + UInt32(IOParam_ioBuffer_off), rec)
            wr32(pb + UInt32(IOParam_ioReqCount_off), length)
            return (pb, tb_PBWrite(pb, async))
        }

        func crossings(_ pcm: [Int16]) -> Int {
            var n = 0
            for i in 1..<pcm.count where (pcm[i] > 0) != (pcm[i - 1] > 0) { n += 1 }
            return n
        }

        /// The square-wave synthesizer: count 1000 is 783.36 Hz for 6 ticks.
        @Test func squareWaveSynthesizer() {
            launchTestApp()
            capture()
            defer { machine_set_sound_handler(nil, nil) }
            let rec = tb_NewPtr(2 + 6 * 2, false, true)
            wr16(rec, 0xFFFF) // swMode
            wr16(rec + 2, 1000); wr16(rec + 4, 255); wr16(rec + 6, 6)
            wr16(rec + 8, 0) // count 0 ends the list
            let (pb, err) = write(rec, length: 14)
            #expect(err == 1) // in progress
            #expect(rd8(UInt32(LM_SoundActive)) != 0 && rd32(UInt32(LM_SoundPtr)) == rec)
            #expect(Self.chunks.count == 1)
            let c = Self.chunks[0]
            #expect(abs(c.rate - 22254.5454) < 1 && c.frames == Int32(6.0 / 60.0 * 22254.5454))
            let hz = Double(crossings(c.pcm)) / 2 / (Double(c.frames) / c.rate)
            #expect(abs(hz - 783.36) < 25)
            machine_skip_time(1_000_000_000)
            sound_pump()
            time_pump()
            #expect(rd8(UInt32(LM_SoundActive)) == 0 && rd32(UInt32(LM_SoundPtr)) == 0)
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioResult_off))) == 0)
        }

        /// SdVolume (SetSoundVol) is the speaker's level: 0 is silence.
        @Test func soundVolumeScalesTheSynthesizers() {
            launchTestApp()
            capture()
            defer { machine_set_sound_handler(nil, nil); wr8(UInt32(LM_SdVolume), 7) }
            let rec = tb_NewPtr(2 + 6 * 2, false, true)
            wr16(rec, 0xFFFF); wr16(rec + 2, 1000); wr16(rec + 4, 255); wr16(rec + 6, 6); wr16(rec + 8, 0)
            wr8(UInt32(LM_SdVolume), 7)
            _ = write(rec, length: 14)
            let loud = Self.chunks.last!.volume
            machine_skip_time(1_000_000_000); sound_pump(); time_pump()
            wr8(UInt32(LM_SdVolume), 3)
            _ = write(rec, length: 14)
            let soft = Self.chunks.last!.volume
            machine_skip_time(1_000_000_000); sound_pump(); time_pump()
            wr8(UInt32(LM_SdVolume), 0)
            _ = write(rec, length: 14)
            let off = Self.chunks.last!.volume
            #expect(loud > 0 && abs(soft - loud * 3 / 7) < 0.01 && off == 0)
        }

        /// The four-tone synthesizer runs its voices through their wave tables
        /// and counts the duration down in the record the program can change.
        @Test func fourToneSynthesizer() {
            launchTestApp()
            capture()
            defer { machine_set_sound_handler(nil, nil) }
            let wave = tb_NewPtr(256, false, true)
            for i in 0..<256 { wr8(wave + UInt32(i), i < 128 ? 255 : 0) } // a square wave in the table
            let ft = tb_NewPtr(50, false, true)
            wr16(ft, 3)                                 // 3 ticks
            wr32(ft + 2, 0x0004_0000)                   // voice 1: four table entries a sample
            wr32(ft + 34, wave)
            let rec = tb_NewPtr(6, false, true)
            wr16(rec, 1)                                // ftMode
            wr32(rec + 2, ft)
            _ = write(rec, length: 6)
            machine_skip_time(300_000_000)
            sound_pump()
            #expect(Self.chunks.count >= 2)
            #expect(Self.chunks.allSatisfy { $0.frames == Int32(22254.5454 / 60.0) })
            #expect(Self.chunks.contains { crossings($0.pcm) > 10 })
            #expect(rd32(ft + 6) != 0)                  // the phase moved
            #expect(Int16(bitPattern: rd16(ft)) <= 0)   // the duration ran out
            machine_skip_time(1_000_000_000); sound_pump(); time_pump()
        }

        /// StopSound is KillIO on the driver: the write ends with abortErr.
        @Test func killStopsTheSound() {
            launchTestApp()
            capture()
            defer { machine_set_sound_handler(nil, nil) }
            let rec = tb_NewPtr(14, false, true)
            wr16(rec, 0xFFFF); wr16(rec + 2, 1000); wr16(rec + 4, 255); wr16(rec + 6, 600); wr16(rec + 8, 0)
            let (pb, _) = write(rec, length: 14)
            let kill = tb_NewPtr(Int32(IOParam_SIZEOF), false, true)
            wr16(kill + UInt32(IOParam_ioRefNum_off), UInt16(bitPattern: -4))
            #expect(tb_PBKillIO(kill, false) == 0)
            #expect(Int16(bitPattern: rd16(pb + UInt32(IOParam_ioResult_off))) == -27)
            #expect(rd8(UInt32(LM_SoundActive)) == 0)
        }

        func sysEnv(_ vers: Int16) -> (err: Int16, rec: UInt32) {
            let rec = tb_NewPtr(16, false, true)
            gmemset(rec, 0x55, 16)
            return (tb_SysEnvirons(vers, rec), rec)
        }

        /// Each version of SysEnvirons: 1 fills through the keyboard, 2 adds
        /// the AppleTalk driver and the System Folder's volume, anything above
        /// is answered as 2 with envVersTooBig, below 1 envBadVers.
        @Test func sysEnvironsVersions() {
            launchTestApp()
            let v1 = sysEnv(1)
            #expect(v1.err == 0 && rd16(v1.rec) == 1)
            #expect(rd16(v1.rec + 12) == 0x5555 && rd16(v1.rec + 14) == 0x5555) // version 2's fields untouched
            let v2 = sysEnv(2)
            #expect(v2.err == 0 && rd16(v2.rec) == 2 && rd16(v2.rec + 12) == 0 && rd16(v2.rec + 14) != 0x5555)
            let v3 = sysEnv(3)
            #expect(v3.err == -5502 && rd16(v3.rec) == 2)
            #expect(sysEnv(0).err == -5501 && sysEnv(-1).err == -5501)
        }

        /// SysEnvirons as a Mac Plus answered it, and a Mac II.
        @Test func sysEnvironsOnTheMachinesPresented() {
            withMachine("plus") {
                launchTestApp()
                let (err, r) = sysEnv(2)
                #expect(err == 0)
                #expect(rd16(r + 2) == 2)                // machineType: envMacPlus
                #expect(rd16(r + 4) == 0x0608)           // System 6.0.8
                #expect(rd16(r + 6) == 1)                // env68000
                #expect(rd8(r + 8) == 0 && rd8(r + 9) == 0) // no FPU, no Color QuickDraw
                #expect(rd16(r + 10) == 3)               // envMacPlusKbd
            }
            withMachine("macii") {
                launchTestApp()
                let (_, r) = sysEnv(2)
                #expect(rd16(r + 2) == 4 && rd16(r + 6) == 3 && rd8(r + 8) != 0 && rd8(r + 9) != 0) // envMacII, 68020, FPU, Color QD
            }
            withMachine("se") {
                launchTestApp()
                #expect(rd16(UInt32(LM_ROM85)) == 0x7FFF && rd16(UInt32(LM_SCSIFlags)) == 0xC400)
                #expect(rd16(UInt32(GA_ROM) + 8) == 0x0276)
            }
        }

        /// Trap numbers the old way: below $50 and $54, $57 are OS traps,
        /// the rest Toolbox ones; a whole trap word names its own table.
        @Test func oldTrapAddressSemantics() {
            launchTestApp()
            let a: UInt32 = 0xF000_1234, b: UInt32 = 0xF000_5678
            let os = tb_GetTrapAddress(0x1E, false, false), tool = tb_GetTrapAddress(0x6E, false, false)
            tb_SetTrapAddress(a, 0x1E, false, false)        // old: an OS trap by its number
            tb_SetTrapAddress(b, 0x6E, false, false)        // old: a Toolbox one
            #expect(tb_GetTrapAddress(0xA01E, false, false) == a) // as a whole trap word, the same slot
            #expect(tb_GetTrapAddress(0xA86E, false, false) == b)
            #expect(tb_GetTrapAddress(0x1E, true, false) == a)    // new: OS table
            #expect(tb_GetTrapAddress(0x6E, true, true) == b)     // new: Toolbox table
            #expect(tb_GetTrapAddress(0x6E, true, false) != b)    // the new way keeps the tables apart
            tb_SetTrapAddress(os, 0x1E, false, false)
            tb_SetTrapAddress(tool, 0x6E, false, false)
            // $54 and $57 are OS traps though above $4F
            tb_SetTrapAddress(a, 0x54, false, false)
            #expect(tb_GetTrapAddress(0x54, true, false) == a)
            #expect(tb_GetTrapAddress(0xA054, false, false) == a)
        }
    }
}
