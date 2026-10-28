import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

/// A 2x2 'raw ' image description at 32 bits.
private func rawDescription() -> UInt32 {
    let d = cat(be32(86), Array("raw ".utf8), [0, 0, 0, 0, 0, 0], be16(0), [UInt8](repeating: 0, count: 16),
                be16(2), be16(2), be32(72 << 16), be32(72 << 16), be32(0), be16(1),
                [UInt8](repeating: 0, count: 32), be16(32), be16(0xFFFF))
    let h = tb_NewHandle(Int32(d.count), false, false)
    gput(rd32(h), d, UInt32(d.count))
    return h
}

/// Four pixels of one colour (xRGB).
private func pixels(_ r: UInt8, _ g: UInt8, _ b: UInt8) -> UInt32 {
    let h = tb_NewHandle(16, false, false)
    var bytes: [UInt8] = []
    for _ in 0..<4 { bytes += [255, r, g, b] }
    gput(rd32(h), bytes, 16)
    return h
}

/// A movie made with the Movie Toolbox: one video track of a red second
/// then a blue one, its samples where the media's data reference says.
private func buildMovie(_ movie: UInt32) -> (track: Int32, media: Int32) {
    let M = Int32(bitPattern: movie)
    let track = tb_NewMovieTrack(M, 2 << 16, 2 << 16, 0)
    #expect(track != 0)
    let media = tb_NewTrackMedia(track, Int32(bitPattern: fourCC("vide")), 600, 0, 0)
    #expect(media != 0)
    #expect(tb_BeginMediaEdits(media) == 0)
    let desc = rawDescription()
    let time = tb_NewPtr(4, false, true)
    #expect(tb_AddMediaSample(media, Int32(bitPattern: pixels(255, 0, 0)), 0, 16, 600,
                              Int32(bitPattern: desc), 1, 0, time) == 0)
    #expect(rd32(time) == 0)
    #expect(tb_AddMediaSample(media, Int32(bitPattern: pixels(0, 0, 255)), 0, 16, 600,
                              Int32(bitPattern: desc), 1, 0, time) == 0)
    #expect(rd32(time) == 600)
    #expect(tb_EndMediaEdits(media) == 0)
    #expect(tb_InsertMediaIntoTrack(track, 0, 0, tb_GetMediaDuration(media), 0x10000) == 0)
    return (track, media)
}

extension BlueBox {
    @Suite struct QTMovieToolbox {
        /// A movie built in memory: its media's samples, edits and durations,
        /// and its frames drawn by our decoders from the media's handle.
        @Test func moviesBuiltInMemoryPlay() {
            qtMachine()
            let movie = UInt32(bitPattern: tb_NewMovie(1))
            let M = Int32(bitPattern: movie)
            let (track, media) = buildMovie(movie)
            #expect(tb_GetMovieDuration(M) == 1200)
            #expect(tb_GetTrackDuration(track) == 1200)
            #expect(tb_GetMediaSampleCount(media) == 2)
            #expect(tb_GetMediaSampleDescriptionCount(media) == 1)
            let n = tb_NewPtr(12, false, true)
            tb_MediaTimeToSampleNum(media, 700, n, n + 4, n + 8)
            #expect(rd32(n) == 2 && rd32(n + 4) == 600 && rd32(n + 8) == 600)
            tb_SampleNumToMediaTime(media, 1, n, n + 4)
            #expect(rd32(n) == 0 && rd32(n + 4) == 600)
            let data = tb_NewHandle(0, false, false), size = tb_NewPtr(4, false, true)
            #expect(tb_GetMediaSample(media, Int32(bitPattern: data), 0, size, 650, 0, 0, 0, 0, 1, 0, 0) == 0)
            #expect(rd32(size) == 16 && rd32(rd32(data)) == 0xFF00_00FF)
            #expect(tb_GetMovieTrack(M, tb_GetTrackID(track)) == track)
            #expect(tb_GetTrackMedia(track) == media && tb_GetMediaTrack(media) == track)
            let nat = tb_NewPtr(8, false, true)
            tb_GetMovieNaturalBoundsRect(M, nat)
            #expect(rd16(nat + 4) == 2 && rd16(nat + 6) == 2)
            // drawn into the port: red at the start, blue a second in
            let box = tb_NewPtr(8, false, true)
            wr16(box + 4, 20); wr16(box + 6, 20)
            tb_SetMovieBox(movie, box)
            let c = tb_NewPtr(6, false, true)
            func colour(_ h: Int16, _ v: Int16) -> (UInt16, UInt16) {
                tb_GetCPixel(h, v, c)
                return (rd16(c), rd16(c + 4))
            }
            _ = tb_UpdateMovie(movie)
            #expect(colour(5, 5).0 > 0xC000 && colour(5, 5).1 < 0x4000)
            tb_SetMovieTimeValue(M, 900)
            _ = tb_UpdateMovie(movie)
            #expect(colour(5, 5).0 < 0x4000 && colour(5, 5).1 > 0xC000)
            // the next interesting times: sample boundaries
            let it = tb_NewPtr(8, false, true)
            tb_GetMovieNextInterestingTime(M, 1, 0, 0, 0, 0x10000, it, it + 4)
            #expect(rd32(it) == 600)
            tb_DisposeMovie(movie)
        }

        /// CreateMovieFile, samples written into its data fork, the movie
        /// saved as its 'moov' resource and opened again.
        @Test func movieFilesKeepWhatWasAdded() throws {
            let dir = launchTestApp()
            _ = initToolbox()
            _ = tb_EnterMovies()
            let path = dir.appendingPathComponent("Made").path
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            let out = tb_NewPtr(8, false, true)
            #expect(tb_CreateMovieFile(spec, Int32(bitPattern: fourCC("TVOD")), 0, Int32(bitPattern: 0x8000_0000),
                                       out, out + 4) == 0)
            let rn = Int16(bitPattern: rd16(out)), movie = rd32(out + 4)
            #expect(rn > 0 && movie != 0)
            let (_, media) = buildMovie(movie)
            let ud = tb_GetMovieUserData(Int32(bitPattern: movie))
            let text = tb_NewHandle(4, false, false)
            gput(rd32(text), Array("Made".utf8), 4)
            #expect(tb_AddUserDataText(ud, Int32(bitPattern: text), Int32(bitPattern: 0xA96E_616D), 1, 0) == 0)
            let id = tb_NewPtr(2, false, true)
            #expect(tb_AddMovieResource(Int32(bitPattern: movie), rn, id, 0) == 0)
            #expect(tb_CloseMovieFile(rn) == 0)
            #expect(tb_GetMediaDataSize(media, 0, 1200) == 32)
            tb_DisposeMovie(movie)
            // the data fork holds the samples
            let bytes = try Data(contentsOf: URL(fileURLWithPath: path))
            #expect(bytes.count == 32 && bytes[1] == 255 && bytes[31] == 255)
            let rn2 = tb_NewPtr(2, false, true)
            #expect(tb_OpenMovieFile(spec, rn2, 1) == 0)
            let mp = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromFile(mp, Int16(bitPattern: rd16(rn2)), 0, 0, 0, 0) == 0)
            _ = tb_CloseMovieFile(Int16(bitPattern: rd16(rn2)))
            let again = Int32(bitPattern: rd32(mp))
            #expect(tb_GetMovieDuration(again) == 1200)
            let t = tb_GetMovieIndTrack(again, 1)
            let m2 = tb_GetTrackMedia(t)
            #expect(tb_GetMediaSampleCount(m2) == 2)
            let data = tb_NewHandle(0, false, false)
            #expect(tb_GetMediaSample(m2, Int32(bitPattern: data), 0, 0, 0, 0, 0, 0, 0, 1, 0, 0) == 0)
            #expect(rd32(rd32(data)) == 0xFFFF_0000)
            let got = tb_NewHandle(0, false, false)
            #expect(tb_GetUserDataText(tb_GetMovieUserData(again), Int32(bitPattern: got),
                                       Int32(bitPattern: 0xA96E_616D), 1, 0) == 0)
            #expect(tb_GetHandleSize(got) == 4)
            tb_DisposeMovie(UInt32(bitPattern: again))
        }

        /// FlattenMovieData: a movie whose samples are in memory copied into
        /// one file with its description, which opens as a movie of its own.
        @Test func flatteningMakesOneFile() throws {
            let dir = launchTestApp()
            _ = initToolbox()
            _ = tb_EnterMovies()
            let movie = UInt32(bitPattern: tb_NewMovie(0))
            _ = buildMovie(movie)
            let path = dir.appendingPathComponent("Flat").path
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            let flat = tb_FlattenMovieData(Int32(bitPattern: movie), 1, spec, Int32(bitPattern: fourCC("TVOD")), 0,
                                           Int32(bitPattern: 0x8000_0000))
            #expect(flat != 0)
            #expect(tb_GetMovieDuration(flat) == 1200)
            let media = tb_GetTrackMedia(tb_GetMovieIndTrack(flat, 1))
            let data = tb_NewHandle(0, false, false)
            #expect(tb_GetMediaSample(media, Int32(bitPattern: data), 0, 0, 700, 0, 0, 0, 0, 1, 0, 0) == 0)
            #expect(rd32(rd32(data)) == 0xFF00_00FF)
            let bytes = try Data(contentsOf: URL(fileURLWithPath: path))
            #expect(bytes.count > 48 && String(decoding: bytes[4..<8], as: UTF8.self) == "mdat")
            // and the file alone opens as a movie
            let rn = tb_NewPtr(2, false, true)
            #expect(tb_OpenMovieFile(spec, rn, 1) == 0)
            let mp = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromFile(mp, Int16(bitPattern: rd16(rn)), 0, 0, 0, 0) == 0)
            #expect(tb_GetMovieDuration(Int32(bitPattern: rd32(mp))) == 1200)
            tb_DisposeMovie(rd32(mp))
            tb_DisposeMovie(UInt32(bitPattern: flat))
            tb_DisposeMovie(movie)
        }

        /// A movie into a handle and back, settings and track references
        /// with it; edit states put edits back.
        @Test func handlesReferencesAndEditStates() {
            qtMachine()
            let movie = UInt32(bitPattern: tb_NewMovie(0))
            let M = Int32(bitPattern: movie)
            let (track, _) = buildMovie(movie)
            let other = tb_NewMovieTrack(M, 0, 0, 0x100)
            _ = tb_NewTrackMedia(other, Int32(bitPattern: fourCC("soun")), 22050, 0, 0)
            let idx = tb_NewPtr(4, false, true)
            #expect(tb_AddTrackReference(track, other, Int32(bitPattern: fourCC("sync")), idx) == 0 && rd32(idx) == 1)
            #expect(tb_GetTrackReference(track, Int32(bitPattern: fourCC("sync")), 1) == other)
            #expect(tb_GetNextTrackReferenceType(track, 0) == Int32(bitPattern: fourCC("sync")))
            tb_SetTrackLayer(track, -1)
            tb_SetMoviePosterTime(M, 300)
            let state = tb_NewMovieEditState(M)
            #expect(tb_DeleteTrackSegment(track, 0, 600) == 0)
            #expect(tb_GetTrackDuration(track) == 600)
            #expect(tb_UseMovieEditState(M, state) == 0)
            #expect(tb_GetTrackDuration(track) == 1200)
            #expect(tb_DisposeMovieEditState(state) == 0)
            #expect(tb_SetTrackOffset(track, 300) == ())
            #expect(tb_GetTrackOffset(track) == 300 && tb_GetTrackDuration(track) == 1500)
            let h = tb_NewHandle(0, false, false)
            #expect(tb_PutMovieIntoHandle(M, Int32(bitPattern: h)) == 0)
            let mp = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromHandle(mp, Int32(bitPattern: h), 0, 0) == 0)
            let copy = Int32(bitPattern: rd32(mp))
            #expect(tb_GetMovieTrackCount(copy) == 2)
            #expect(tb_GetMoviePosterTime(copy) == 300)
            let t1 = tb_GetMovieIndTrack(copy, 1), t2 = tb_GetMovieIndTrack(copy, 2)
            #expect(tb_GetTrackLayer(t1) == -1)
            #expect(tb_GetTrackReference(t1, Int32(bitPattern: fourCC("sync")), 1) == t2)
            // the copy reads its samples from the original's handle
            let data = tb_NewHandle(0, false, false)
            #expect(tb_GetMediaSample(tb_GetTrackMedia(t1), Int32(bitPattern: data), 0, 0, 0, 0, 0, 0, 0, 1, 0, 0) == 0)
            #expect(rd32(rd32(data)) == 0xFFFF_0000)
            #expect(tb_GetMovieIndTrackType(copy, 1, Int32(bitPattern: fourCC("ears")), 2) == t2)
            tb_DisposeMovie(rd32(mp))
            tb_DisposeMovie(movie)
        }

        nonisolated(unsafe) static var called: [UInt32] = []

        /// QTCallBack(cb, refCon), pascal: records the refCon.
        static let callback: native_fn = { c, _ in
            guard let c else { return }
            let ret = cpu_pop32(c)
            QTMovieToolbox.called.append(cpu_pop32(c))
            _ = cpu_pop32(c)
            cpu_push32(c, ret)
        }

        /// A time base's callbacks go off when its time passes the one asked
        /// for, and when its rate changes; each once until armed again.
        @Test func callbacksGoOffOnce() {
            qtMachine()
            Self.called = []
            let proc = native_register(Self.callback, "test callback", 0)
            let tb = tb_NewTimeBase()
            tb_SetTimeBaseValue(tb, 0, 600)
            let atTime = tb_NewCallBack(tb, 1)
            #expect(tb_CallMeWhen(atTime, Int32(bitPattern: proc), 7, 1, 300, 600) == 0)
            let atRate = tb_NewCallBack(tb, 2)
            #expect(tb_CallMeWhen(atRate, Int32(bitPattern: proc), 9, 0, 0, 0) == 0)
            tb_SetTimeBaseRate(tb, 0x10000)
            tb_MoviesTask(0, 0)
            #expect(Self.called == [9])
            machine_skip_time(600_000_000)
            tb_MoviesTask(0, 0)
            #expect(Self.called == [9, 7])
            machine_skip_time(600_000_000)
            tb_MoviesTask(0, 0)
            #expect(Self.called == [9, 7])
            #expect(tb_GetFirstCallBack(tb) == atTime && tb_GetNextCallBack(atTime) == atRate)
            tb_DisposeCallBack(atTime)
            tb_DisposeCallBack(atRate)
            tb_DisposeTimeBase(tb)
        }
    }
}
