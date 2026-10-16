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
    }
}
