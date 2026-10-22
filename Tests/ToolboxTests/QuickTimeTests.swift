import CCore
import Foundation
import Testing
import Toolbox

/// An atom: size, type, contents.
private func atom(_ type: String, _ body: [UInt8]) -> [UInt8] {
    cat(be32(UInt32(8 + body.count)), Array(type.utf8), body)
}

/// A movie description: 2 s at a time scale of 600, one 64x48 video track.
private func testMoov() -> [UInt8] {
    let mvhd = atom("mvhd", cat([0, 0, 0, 0], be32(0), be32(0), be32(600), be32(1200),
                                [UInt8](repeating: 0, count: 80)))
    var tkhd = cat([0, 0, 0, 0], be32(0), be32(0), be32(1), be32(0), be32(1200),
                   [UInt8](repeating: 0, count: 52))
    tkhd += cat(be32(64 << 16), be32(48 << 16)) // width, height (Fixed)
    let hdlr = atom("hdlr", cat([0, 0, 0, 0], Array("mhlr".utf8), Array("vide".utf8),
                                [UInt8](repeating: 0, count: 13)))
    let trak = atom("trak", cat(atom("tkhd", tkhd), atom("mdia", hdlr)))
    return atom("moov", cat(mvhd, trak))
}

/// A flattened movie of 2x2 'raw ' frames, one red second then one blue,
/// at a time scale of 600, with the given creation and modification dates.
private func rawMovie(created: UInt32 = 0, modified: UInt32 = 0) -> [UInt8] {
    func full(_ type: String, _ body: [UInt8]) -> [UInt8] { atom(type, cat([0, 0, 0, 0], body)) }
    let red: [UInt8] = [255, 255, 0, 0], blue: [UInt8] = [255, 0, 0, 255]
    let mdat = atom("mdat", Array(repeating: red, count: 4).flatMap { $0 } + Array(repeating: blue, count: 4).flatMap { $0 })
    let stsd = cat(be32(86), Array("raw ".utf8), [0, 0, 0, 0, 0, 0], be16(1), [UInt8](repeating: 0, count: 16),
                   be16(2), be16(2), be32(72 << 16), be32(72 << 16), be32(0), be16(1),
                   [UInt8](repeating: 0, count: 32), be16(32), be16(0xFFFF))
    let stbl = atom("stbl", cat(full("stsd", cat(be32(1), stsd)),
                                full("stts", cat(be32(1), be32(2), be32(600))),
                                full("stsc", cat(be32(1), be32(1), be32(1), be32(1))),
                                full("stsz", cat(be32(16), be32(2))),
                                full("stco", cat(be32(2), be32(8), be32(24)))))
    let mdhd = full("mdhd", cat(be32(0), be32(0), be32(600), be32(1200), be32(0)))
    let hdlr = full("hdlr", cat(Array("mhlr".utf8), Array("vide".utf8), [UInt8](repeating: 0, count: 13)))
    let tkhd = cat([0, 0, 0, 1], be32(0), be32(0), be32(1), be32(0), be32(1200), [UInt8](repeating: 0, count: 52),
                   be32(2 << 16), be32(2 << 16))
    let trak = atom("trak", cat(atom("tkhd", tkhd), atom("mdia", cat(mdhd, hdlr, atom("minf", stbl)))))
    let mvhd = full("mvhd", cat(be32(created), be32(modified), be32(600), be32(1200),
                                [UInt8](repeating: 0, count: 80)))
    return cat(mdat, atom("moov", cat(mvhd, trak)))
}

/// A movie from a file of these bytes in the test application's folder.
private func openMovie(_ bytes: [UInt8], named name: String, in dir: URL) throws -> UInt32 {
    let path = dir.appendingPathComponent(name).path
    try Data(bytes).write(to: URL(fileURLWithPath: path))
    let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
    fm_spec_for_path(path, spec)
    let rn = tb_NewPtr(2, false, true)
    #expect(tb_OpenMovieFile(spec, rn, 1) == 0)
    let mp = tb_NewPtr(4, false, true)
    #expect(tb_NewMovieFromFile(mp, Int16(bitPattern: rd16(rn)), 0, 0, 0, 0) == 0)
    _ = tb_CloseMovieFile(Int16(bitPattern: rd16(rn)))
    return rd32(mp)
}

extension BlueBox {
    @Suite struct QuickTime {
        /// A flattened movie file: its description, tracks and timing (the
        /// clock follows instructions headless, and there is no decoding).
        @Test func flattenedMovieKeepsTime() throws {
            let dir = launchTestApp()
            let path = dir.appendingPathComponent("Movie").path
            try Data(testMoov()).write(to: URL(fileURLWithPath: path))
            #expect(tb_EnterMovies() == 0)
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            let rn = tb_NewPtr(2, false, true)
            #expect(tb_OpenMovieFile(spec, rn, 1) == 0)
            let mp = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromFile(mp, Int16(bitPattern: rd16(rn)), 0, 0, 0, 0) == 0)
            let movie = rd32(mp)
            #expect(movie != 0)
            #expect(tb_GetMovieTimeScale(Int32(bitPattern: movie)) == 600)
            #expect(tb_GetMovieDuration(Int32(bitPattern: movie)) == 1200)
            let box = tb_NewPtr(8, false, true)
            tb_GetMovieNaturalBoundsRect(Int32(bitPattern: movie), box)
            #expect(rd16(box + 4) == 48 && rd16(box + 6) == 64) // bottom, right
            #expect(tb_GetMovieTrackCount(Int32(bitPattern: movie)) == 1)
            let track = tb_GetMovieIndTrack(Int32(bitPattern: movie), 1)
            let type = tb_NewPtr(4, false, true)
            tb_GetMediaHandlerDescription(tb_GetTrackMedia(track), type, 0, 0)
            #expect(rd32(type) == fourCC("vide"))

            tb_StartMovie(movie)
            #expect(tb_IsMovieDone(movie) == 0)
            machine_skip_time(1_000_000_000)
            let t = tb_GetMovieTime(Int32(bitPattern: movie), 0)
            #expect(t >= 590 && t <= 610)
            machine_skip_time(1_500_000_000)
            #expect(tb_IsMovieDone(movie) != 0)
            tb_DisposeMovie(movie)
            #expect(tb_CloseMovieFile(Int16(bitPattern: rd16(rn))) == 0)
        }

        /// With VLC installed (and ffmpeg to make a clip), a movie decodes and
        /// its frames are drawn into the port.  Skipped otherwise.
        @Test func vlcDrawsFrames() throws {
            let ffmpeg = ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"]
                .first { FileManager.default.isExecutableFile(atPath: $0) }
            guard let ffmpeg, qt_vlc_available() else { return }
            let dir = launchTestApp()
            let path = dir.appendingPathComponent("Red").path
            let p = Process()
            p.executableURL = URL(fileURLWithPath: ffmpeg)
            p.arguments = ["-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=c=red:s=64x48:d=3:r=10",
                           "-c:v", "mpeg4", "-f", "mov", path]
            try p.run()
            p.waitUntilExit()
            #expect(p.terminationStatus == 0)
            _ = initToolbox()
            setenv("BLUEBOX_QUICKTIME_HEADLESS", "1", 1)
            defer { unsetenv("BLUEBOX_QUICKTIME_HEADLESS") }
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            let rn = tb_NewPtr(2, false, true)
            #expect(tb_OpenMovieFile(spec, rn, 1) == 0)
            let mp = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromFile(mp, Int16(bitPattern: rd16(rn)), 0, 0, 0, 0) == 0)
            let movie = rd32(mp)
            tb_StartMovie(movie)
            var drawn = false
            for _ in 0..<60 where !drawn {
                Thread.sleep(forTimeInterval: 0.05)
                tb_MoviesTask(movie, 0)
                let c = tb_NewPtr(6, false, true)
                tb_GetCPixel(32, 24, c)
                drawn = rd16(c) > 0xC000 && rd16(c + 2) < 0x4000 && rd16(c + 4) < 0x4000
            }
            #expect(drawn)
            tb_DisposeMovie(movie)
            _ = tb_CloseMovieFile(Int16(bitPattern: rd16(rn)))
        }

        /// A movie slaved to another's time base follows its time and rate,
        /// less slaveZero, and is back on its own clock when that one goes.
        @Test func slavedMovieFollowsItsMaster() throws {
            let dir = launchTestApp()
            #expect(tb_EnterMovies() == 0)
            let a = try openMovie(testMoov(), named: "A", in: dir), b = try openMovie(testMoov(), named: "B", in: dir)
            let A = Int32(bitPattern: a), B = Int32(bitPattern: b)
            let tbA = tb_GetMovieTimeBase(A), tbB = tb_GetMovieTimeBase(B)
            #expect(tbA != 0 && tbA != A && tbB != tbA)
            tb_SetMovieMasterTimeBase(B, tbA, 0)
            #expect(tb_GetTimeBaseMasterTimeBase(tbB) == tbA)
            tb_StartMovie(b)                                        // its rate, but the master stands still
            machine_skip_time(500_000_000)
            #expect(tb_GetMovieTime(B, 0) == 0)
            #expect(tb_GetTimeBaseEffectiveRate(tbB) == 0)
            tb_StartMovie(a)
            machine_skip_time(1_000_000_000)
            let ta = tb_GetMovieTime(A, 0), tb = tb_GetMovieTime(B, 0)
            #expect(ta >= 590 && ta <= 610 && abs(ta - tb) <= 1)
            #expect(tb_GetTimeBaseEffectiveRate(tbB) == 0x10000)
            tb_SetMovieRate(A, 0)                                   // stopped there
            tb_SetMovieTimeValue(A, 300)                            // and back: the slave goes too
            machine_skip_time(500_000_000)
            #expect(tb_GetMovieTime(B, 0) == 300)
            let zero = tb_NewPtr(16, false, true)
            wr32(zero + 4, 120); wr32(zero + 8, 600)                // the master's 0.2 s is the slave's 0
            tb_SetMovieMasterTimeBase(B, tbA, zero)
            #expect(tb_GetMovieTime(B, 0) == 180)
            tb_SetMovieRate(A, 0x20000)                             // twice as fast
            #expect(tb_GetTimeBaseEffectiveRate(tbB) == 0x20000)
            machine_skip_time(250_000_000)
            #expect(abs(tb_GetMovieTime(B, 0) - 480) <= 1)
            tb_DisposeMovie(a)
            #expect(tb_GetTimeBaseMasterTimeBase(tbB) == 0)
            #expect(abs(tb_GetMovieTime(B, 0) - 480) <= 1)          // where it was, on its own clock now
            tb_DisposeMovie(b)
        }

        /// Time bases of the program's own: exact while stopped, moving at
        /// their rate times their master's, held at the stop time.
        @Test func timeBasesKeepTimeAndChain() {
            launchTestApp()
            let tb = tb_NewTimeBase(), slave = tb_NewTimeBase()
            #expect(tb != 0 && slave != 0 && tb != slave)
            tb_SetTimeBaseValue(tb, 100, 600)
            #expect(tb_GetTimeBaseTime(tb, 600, 0) == 100)
            #expect(tb_GetTimeBaseTime(tb, 60, 0) == 10)
            tb_SetTimeBaseRate(tb, 0x10000)
            #expect(tb_GetTimeBaseRate(tb) == 0x10000)
            machine_skip_time(1_000_000_000)
            #expect(tb_GetTimeBaseTime(tb, 600, 0) == 700)
            tb_SetTimeBaseMasterTimeBase(slave, tb, 0)              // no offset: it reads as its master
            #expect(tb_GetTimeBaseMasterTimeBase(slave) == tb)
            #expect(tb_GetTimeBaseTime(slave, 600, 0) == 700)
            tb_SetTimeBaseRate(slave, 0x8000)
            #expect(tb_GetTimeBaseEffectiveRate(slave) == 0x8000)
            machine_skip_time(1_000_000_000)
            #expect(tb_GetTimeBaseTime(tb, 600, 0) == 1300)
            #expect(tb_GetTimeBaseTime(slave, 600, 0) == 1000)
            let tr = tb_NewPtr(16, false, true)
            wr32(tr + 4, 1500); wr32(tr + 8, 600)
            tb_SetTimeBaseStopTime(tb, tr)
            #expect(tb_GetTimeBaseStopTime(tb, 600, 0) == 1500)
            machine_skip_time(1_000_000_000)
            #expect(tb_GetTimeBaseTime(tb, 600, tr) == 1500 && rd32(tr + 12) == UInt32(bitPattern: tb))
            let status = tb_GetTimeBaseStatus(tb, tr)
            #expect(status == 2 && rd32(tr + 4) == 1900, "status \(status), unpinned \(rd32(tr + 4))") // after its stop time
            // A movie slaved to it: the time base's 0.5 s is the movie's 0.
            let movie = tb_NewMovie(0)
            wr32(tr, 0); wr32(tr + 4, 300); wr32(tr + 8, 600)
            tb_SetMovieMasterTimeBase(movie, tb, tr)
            tb_SetMovieTimeValue(movie, 0)
            #expect(tb_GetTimeBaseMasterTimeBase(tb_GetMovieTimeBase(movie)) == tb)
            // ConvertTime: the time base's 1500 (where it is held) is the slave's 1100.
            wr32(tr, 0); wr32(tr + 4, 1500); wr32(tr + 8, 600); wr32(tr + 12, UInt32(bitPattern: tb))
            tb_ConvertTime(tr, slave)
            #expect(rd32(tr + 12) == UInt32(bitPattern: slave) && abs(Int32(bitPattern: rd32(tr + 4)) - 1100) <= 1)
            tb_DisposeTimeBase(tb)
            #expect(tb_GetTimeBaseMasterTimeBase(slave) == 0)
            #expect(tb_GetTimeBaseTime(slave, 600, 0) == 1100)      // where it was
            tb_DisposeTimeBase(slave)
            tb_DisposeMovie(UInt32(bitPattern: movie))
        }

        /// The 'mvhd' dates, and the movie's clip and display clip narrowing
        /// where its frames are drawn.
        @Test func datesAndClipRegions() throws {
            let dir = launchTestApp()
            _ = initToolbox()                                       // a port to draw in
            #expect(tb_EnterMovies() == 0)
            let m = try openMovie(rawMovie(created: 0xB000_0001, modified: 0xB000_0002), named: "Raw", in: dir)
            let M = Int32(bitPattern: m)
            #expect(UInt32(bitPattern: tb_GetMovieCreationTime(M)) == 0xB000_0001)
            #expect(UInt32(bitPattern: tb_GetMovieModificationTime(M)) == 0xB000_0002)
            let box = tb_NewPtr(8, false, true)
            wr16(box + 4, 40); wr16(box + 6, 40)                     // 0,0,40,40
            tb_SetMovieBox(m, box)
            #expect(tb_GetMovieDisplayClipRgn(M) == 0)
            let left = tb_NewRgn()
            tb_SetRectRgn(left, 0, 0, 20, 40)
            tb_SetMovieDisplayClipRgn(M, Int32(bitPattern: left))
            let copy = UInt32(bitPattern: tb_GetMovieDisplayClipRgn(M))
            #expect(copy != 0 && copy != left && tb_EqualRgn(copy, left) != 0)
            let white = tb_NewPtr(8, false, true)
            wr16(white + 4, 60); wr16(white + 6, 60)
            tb_EraseRect(white)
            tb_StartMovie(m)
            _ = tb_UpdateMovie(m)
            let c = tb_NewPtr(6, false, true)
            func red(_ h: Int16, _ v: Int16) -> Bool {
                tb_GetCPixel(h, v, c)
                return rd16(c) > 0xC000 && rd16(c + 2) < 0x4000 && rd16(c + 4) < 0x4000
            }
            #expect(red(10, 10) && !red(30, 10))                     // the left half only
            // The movie's own clip, in its 2x2 coordinates: the top row.
            tb_SetMovieDisplayClipRgn(M, 0)
            #expect(tb_GetMovieDisplayClipRgn(M) == 0)
            let top = tb_NewRgn()
            tb_SetRectRgn(top, 0, 0, 2, 1)
            tb_SetMovieClipRgn(M, Int32(bitPattern: top))
            tb_EraseRect(white)
            _ = tb_UpdateMovie(m)
            #expect(red(30, 10) && !red(10, 30))                     // the top half only
            #expect(tb_GetMovieClipRgn(M) != 0)
            tb_DisposeMovie(m)
            #expect(tb_GetMovieCreationTime(tb_NewMovie(0)) != 0)
        }

        /// TimeRecords change scale and add up across scales (Director's
        /// QuickTime sprites time themselves this way).
        @Test func timeRecordArithmetic() {
            bootMachine()
            let a = tb_NewPtr(16, false, true), b = tb_NewPtr(16, false, true)
            wr32(a, 1); wr32(a + 4, 0); wr32(a + 8, 600)       // 2^32 at 600
            tb_ConvertTimeScale(a, 60)
            #expect(rd32(a) == 0 && rd32(a + 4) == 0x1999_9999 && rd32(a + 8) == 60)
            wr32(a, 0); wr32(a + 4, 120); wr32(a + 8, 60)      // 2 s
            wr32(b, 0); wr32(b + 4, 300); wr32(b + 8, 600)     // 0.5 s
            tb_AddTime(a, b)
            #expect(rd32(a + 4) == 150 && rd32(a + 8) == 60)
            tb_SubtractTime(a, b)
            tb_SubtractTime(a, b)
            #expect(rd32(a + 4) == 90)
        }
    }
}
