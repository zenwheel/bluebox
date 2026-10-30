import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

private func h32(_ v: UInt32) -> [UInt8] { be32(v) }

/// A QT atom: its header (size, type, ID, reserved, child count, reserved),
/// then its children or data.
private func qtAtom(_ type: String, _ id: UInt32, children: [[UInt8]] = [], data: [UInt8] = []) -> [UInt8] {
    let body = children.isEmpty ? data : children.flatMap { $0 }
    return cat(h32(UInt32(20 + body.count)), Array(type.utf8), h32(id), be16(0), be16(children.count), h32(0), body)
}

private func qtAtom(_ type: UInt32, _ id: UInt32, data: [UInt8]) -> [UInt8] {
    cat(h32(UInt32(20 + data.count)), h32(type), h32(id), be16(0), be16(0), h32(0), data)
}

private func handle(_ bytes: [UInt8]) -> UInt32 {
    let h = tb_NewHandle(Int32(bytes.count), false, false)
    if !bytes.isEmpty { gput(rd32(h), bytes, UInt32(bytes.count)) }
    return h
}

private func cString(_ s: String) -> UInt32 {
    let p = tb_NewPtr(Int32(s.utf8.count + 1), false, true)
    gput(p, Array(s.utf8), UInt32(s.utf8.count))
    return p
}

extension BlueBox {
    @Suite struct QTMediaHandlers {
        /// The handle and alias data handlers read and write what their
        /// references name.
        @Test func dataHandlers() throws {
            qtMachine()
            let data = handle([])
            let ref = handle(be32(data))
            let dh = tb_NewPtr(4, false, true)
            #expect(tb_OpenADataHandler(Int32(bitPattern: ref), Int32(bitPattern: fourCC("hndl")), 0, 0, 0, 1 | 8, dh) == 0)
            let h = rd32(dh)
            #expect(h != 0)
            let src = handle(Array("abcdef".utf8)), off = tb_NewPtr(4, false, true)
            #expect(qtCall("DataHPutData", [h, src, 0, off, 6]) == 0 && rd32(off) == 0)
            #expect(qtCall("DataHPutData", [h, src, 2, off, 4]) == 0 && rd32(off) == 6)
            let size = tb_NewPtr(4, false, true)
            #expect(qtCall("DataHGetFileSize", [h, size]) == 0 && rd32(size) == 10)
            let out = tb_NewHandle(4, false, true)
            #expect(qtCall("DataHGetData", [h, out, 0, 6, 4]) == 0)
            #expect(rd32(rd32(out)) == fourCC("cdef"))
            let canUse = tb_NewPtr(4, false, true)
            #expect(qtCall("DataHCanUseDataRef", [h, ref, canUse]) == 0 && rd32(canUse) & 9 == 9)
            components_close(h)
            // a file, made and written through the alias data handler
            let dir = launchTestApp()
            _ = initToolbox()
            let path = dir.appendingPathComponent("Data").path
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            let alias = tb_NewPtr(4, false, true)
            #expect(tb_NewAliasMinimal(spec, alias) == 0)
            #expect(tb_OpenADataHandler(Int32(bitPattern: rd32(alias)), Int32(bitPattern: fourCC("alis")), 0, 0, 0, 0, dh) == 0)
            let fh = rd32(dh)
            #expect(qtCall("DataHCreateFile", [fh, fourCC("TVOD"), 1]) == 0)
            #expect(qtCall("DataHOpenForWrite", [fh]) == 0)
            let bytes = tb_NewPtr(4, false, false)
            wr32(bytes, 0x1234_5678)
            let at = tb_NewPtr(8, false, true)
            #expect(qtCall("DataHWrite64", [fh, bytes, at, 4, 0, 0]) == 0)
            #expect(qtCall("DataHFlushData", [fh]) == 0)
            #expect(qtCall("DataHCloseForWrite", [fh]) == 0)
            let wide = tb_NewPtr(8, false, true)
            #expect(qtCall("DataHGetFileSize64", [fh, wide]) == 0 && rd32(wide + 4) == 4)
            let name = tb_NewPtr(256, false, true)
            #expect(qtCall("DataHGetFileName", [fh, name]) == 0 && pstring(at: name) == "Data")
            components_close(fh)
            let onDisk = try Data(contentsOf: URL(fileURLWithPath: path))
            #expect(onDisk == Data([0x12, 0x34, 0x56, 0x78]))
        }

        /// The text media handler makes text samples, finds and changes
        /// their text, and a text track it made draws.
        @Test func textMediaHandler() {
            qtMachine()
            let movie = UInt32(bitPattern: tb_NewMovie(1))
            let M = Int32(bitPattern: movie)
            let track = tb_NewMovieTrack(M, 100 << 16, 20 << 16, 0)
            let media = tb_NewTrackMedia(track, Int32(bitPattern: fourCC("text")), 600, 0, 0)
            let mh = UInt32(bitPattern: tb_GetMediaHandler(media))
            #expect(mh != 0)
            let desc = tb_NewPtr(8, false, true)
            _ = tb_GetMediaHandlerDescription(media, desc, 0, 0)
            let text = cString("Hello world")
            let time = tb_NewPtr(4, false, true)
            let black = tb_NewPtr(6, false, true), white = tb_NewPtr(6, false, true)
            wr16(white, 0xFFFF); wr16(white + 2, 0xFFFF); wr16(white + 4, 0xFFFF)
            #expect(qtCall("TextMediaAddTextSample", [mh, text, 11, 0, 12, 0, black, white, 0, 0, 0, 0, 0, 0, 0,
                                                      600, time]) == 0)
            #expect(rd32(time) == 0)
            #expect(qtCall("TextMediaAddTextSample", [mh, cString("second"), 6, 0, 12, 1, black, white, 0, 0, 0, 0,
                                                      0, 0, 0, 600, time]) == 0)
            #expect(rd32(time) == 600)
            #expect(tb_InsertMediaIntoTrack(track, 0, 0, tb_GetMediaDuration(media), 0x10000) == 0)
            #expect(tb_GetMediaSampleCount(media) == 2)
            let data = tb_NewHandle(0, false, false)
            #expect(tb_GetMediaSample(media, Int32(bitPattern: data), 0, 0, 0, 0, 0, 0, 0, 1, 0, 0) == 0)
            #expect(rd16(rd32(data)) == 11 && rd32(rd32(data) + 2) == fourCC("Hell"))
            // found, from the start, in the second sample
            let found = tb_NewPtr(12, false, true)
            #expect(qtCall("TextMediaFindNextText", [mh, cString("cond"), 4, 1, 0, found, found + 4, found + 8]) == 0)
            #expect(rd32(found) == 600 && rd32(found + 8) == 2)
            let length = tb_NewPtr(4, false, true)
            #expect(qtCall("TextMediaGetTextProperty", [mh, 0, 22, length, 4]) == 0 && rd32(length) == 11)
            // drawn: dark text on the white box
            _ = tb_UpdateMovie(movie)
            let c = tb_NewPtr(6, false, true)
            var dark = 0
            for y in 0..<20 { for x in 0..<100 {
                tb_GetCPixel(Int16(x), Int16(y), c)
                if rd16(c) < 0x8000 { dark += 1 }
            } }
            #expect(dark > 10)
            // changed as it shows: the text, and the length it reports
            #expect(qtCall("TextMediaSetTextProperty", [mh, 0, 2, cString("Hi"), 2]) == 0)
            #expect(tb_MovieSearchText(M, Int32(bitPattern: cString("world")), 5, 0, 0, 0, 0) == 0)
            tb_DisposeMovie(movie)
        }

        /// QuickTime 2's names for the media handlers' calls, which
        /// QuickTimeLib still exports (Sparkle imports AddTextSample): the
        /// text calls reach the text handler; the video and sound media's
        /// graphics mode and balance are the handlers' own.
        @Test func quickTime2MediaHandlerNames() {
            qtMachine()
            let movie = UInt32(bitPattern: tb_NewMovie(1))
            let M = Int32(bitPattern: movie)
            let track = tb_NewMovieTrack(M, 100 << 16, 20 << 16, 0)
            let media = tb_NewTrackMedia(track, Int32(bitPattern: fourCC("text")), 600, 0, 0)
            let mh = UInt32(bitPattern: tb_GetMediaHandler(media))
            let time = tb_NewPtr(4, false, true)
            let black = tb_NewPtr(6, false, true)
            #expect(qtCall("AddTextSample", [mh, cString("Caption one"), 11, 0, 12, 0, black, 0, 0, 0, 0, 0, 0, 0,
                                             0, 600, time]) == 0)
            #expect(qtCall("AddHiliteSample", [mh, 0, 7, 0, 300, time]) == 0 && rd32(time) == 600)
            #expect(tb_GetMediaSampleCount(media) == 2)
            #expect(tb_InsertMediaIntoTrack(track, 0, 0, tb_GetMediaDuration(media), 0x10000) == 0)
            let found = tb_NewPtr(12, false, true)
            #expect(qtCall("FindNextText", [mh, cString("one"), 3, 0, 0, found, found + 4, found + 8]) == 0)
            #expect(rd32(found + 8) == 8)
            #expect(qtCall("HiliteTextSample", [mh, 0, 0, 3, 0]) == 0)
            #expect(qtCall("SetTextProc", [mh, 0, 0]) == 0)
            for name in ["AddTESample", "SetTextSampleData"] { #expect(cfm_export_fn(name) != nil, "\(name)") }
            tb_DisposeMovie(movie)

            let vmovie = tb_NewMovie(0)
            let vtrack = tb_NewMovieTrack(vmovie, 0, 0, 0)
            let video = UInt32(bitPattern: tb_GetMediaHandler(tb_NewTrackMedia(vtrack, Int32(bitPattern: fourCC("vide")), 600, 0, 0)))
            let op = tb_NewPtr(6, false, true)
            wr16(op, 0x1111); wr16(op + 2, 0x2222); wr16(op + 4, 0x3333)
            #expect(qtCall("SetVideoMediaGraphicsMode", [video, 0x24, op]) == 0)
            let mode = tb_NewPtr(4, false, true), back = tb_NewPtr(6, false, true)
            #expect(qtCall("GetVideoMediaGraphicsMode", [video, mode, back]) == 0)
            #expect(rd32(mode) == 0x24 && rd16(back + 2) == 0x2222)
            let strack = tb_NewMovieTrack(vmovie, 0, 0, 0x100)
            let sound = UInt32(bitPattern: tb_GetMediaHandler(tb_NewTrackMedia(strack, Int32(bitPattern: fourCC("soun")), 22050, 0, 0)))
            #expect(qtCall("SetSoundMediaBalance", [sound, 0xFFC0]) == 0)
            let balance = tb_NewPtr(2, false, true)
            #expect(qtCall("GetSoundMediaBalance", [sound, balance]) == 0 && rd16(balance) == 0xFFC0)
            #expect(qtCall("MediaGetSoundBalance", [sound, balance]) == 0 && rd16(balance) == 0xFFC0)
            tb_DisposeMovie(UInt32(bitPattern: vmovie))
        }

        /// Timecode: a sample's frame number seen as a time, drop frame or not.
        @Test func timecodeMediaHandler() {
            qtMachine()
            let movie = UInt32(bitPattern: tb_NewMovie(0))
            let track = tb_NewMovieTrack(Int32(bitPattern: movie), 0, 0, 0)
            let media = tb_NewTrackMedia(track, Int32(bitPattern: fourCC("tmcd")), 2997, 0, 0)
            // TimeCodeDescription: drop frame, 2997 / 100, 30 frames
            let desc = handle(cat(be32(38), Array("tmcd".utf8), [0, 0, 0, 0, 0, 0], be16(1), be32(0),
                                  be32(1), be32(2997), be32(100), [30, 0], be32(0)))
            let sample = handle(be32(1800)) // a minute in
            #expect(tb_AddMediaSample(media, Int32(bitPattern: sample), 0, 4, 2997 * 60, Int32(bitPattern: desc), 1, 0, 0) == 0)
            let mh = UInt32(bitPattern: tb_GetMediaHandler(media))
            let frame = tb_NewPtr(4, false, true), def = tb_NewPtr(16, false, true), rec = tb_NewPtr(4, false, true)
            #expect(qtCall("TCGetTimeCodeAtTime", [mh, 1000, frame, def, rec, 0]) == 0)
            #expect(rd32(frame) == 1810)
            // 1810 drop-frame frames: 00:01:00;12 (frames 0 and 1 of minute 1 are skipped)
            #expect(rd8(rec) == 0 && rd8(rec + 1) == 1 && rd8(rec + 2) == 0 && rd8(rec + 3) == 12)
            let s = tb_NewPtr(32, false, true)
            #expect(qtCall("TCTimeCodeToString", [mh, def, rec, s]) == 0)
            #expect(pstring(at: s) == "00:01:00;12")
            let back = tb_NewPtr(4, false, true)
            #expect(qtCall("TCTimeCodeToFrameNumber", [mh, def, rec, back]) == 0 && rd32(back) == 1810)
            tb_DisposeMovie(movie)
        }

        /// The sprite media handler: a key frame's sprites counted, read,
        /// changed and hit as they show.
        @Test func spriteMediaHandler() {
            qtMachine()
            let movie = UInt32(bitPattern: tb_NewMovie(1))
            let M = Int32(bitPattern: movie)
            let track = tb_NewMovieTrack(M, 64 << 16, 64 << 16, 0)
            let media = tb_NewTrackMedia(track, Int32(bitPattern: fourCC("sprt")), 600, 0, 0)
            // a 2x2 red image, the sprite at (10, 20) twice its size
            let idesc = cat(be32(86), Array("raw ".utf8), [0, 0, 0, 0, 0, 0], be16(0), [UInt8](repeating: 0, count: 16),
                            be16(2), be16(2), be32(72 << 16), be32(72 << 16), be32(0), be16(1),
                            [UInt8](repeating: 0, count: 32), be16(32), be16(0xFFFF))
            var px: [UInt8] = []
            for _ in 0..<4 { px += [255, 255, 0, 0] }
            let matrix = cat(be32(2 << 16), be32(0), be32(0), be32(0), be32(2 << 16), be32(0), be32(10 << 16),
                             be32(20 << 16), be32(0x4000_0000))
            let images = qtAtom("dflt", 1, children: [qtAtom("imct", 1, children: [qtAtom("imag", 1, children: [
                qtAtom("imda", 1, data: idesc + px)])])])
            let sprite = qtAtom("sprt", 1, children: [qtAtom(1, 1, data: matrix), qtAtom(4, 1, data: be16(1)),
                                                      qtAtom(5, 1, data: be16(0)), qtAtom(100, 1, data: be16(1))])
            let sean = qtAtom("sean", 1, children: [images, sprite])
            let sample = handle([UInt8](repeating: 0, count: 12) + sean)
            let sdesc = handle(cat(be32(28), Array("sprt".utf8), [0, 0, 0, 0, 0, 0], be16(1), be32(0), be32(0), be32(0)))
            #expect(tb_AddMediaSample(media, Int32(bitPattern: sample), 0, Int32(tb_GetHandleSize(sample)), 600,
                                      Int32(bitPattern: sdesc), 1, 0, 0) == 0)
            #expect(tb_InsertMediaIntoTrack(track, 0, 0, 600, 0x10000) == 0)
            _ = tb_UpdateMovie(movie)
            let mh = UInt32(bitPattern: tb_GetMediaHandler(media))
            let n = tb_NewPtr(4, false, true)
            #expect(qtCall("SpriteMediaCountSprites", [mh, n]) == 0 && rd16(n) == 1)
            #expect(qtCall("SpriteMediaCountImages", [mh, n]) == 0 && rd16(n) == 1)
            let m = tb_NewPtr(36, false, true)
            #expect(qtCall("SpriteMediaGetSpriteProperty", [mh, 1, 1, m]) == 0)
            #expect(rd32(m) == 2 << 16 && rd32(m + 24) == 10 << 16 && rd32(m + 28) == 20 << 16)
            // hit where it is (10..14, 20..24), not elsewhere
            let hit = tb_NewPtr(4, false, true)
            #expect(qtCall("SpriteMediaHitTestAllSprites", [mh, 1, UInt32(22 << 16 | 12), hit]) == 0 && rd32(hit) == 1)
            #expect(qtCall("SpriteMediaHitTestAllSprites", [mh, 1, UInt32(40 << 16 | 40), hit]) == 0 && rd32(hit) == 0)
            // moved: drawn where it went
            wr32(m + 24, 40 << 16)
            #expect(qtCall("SpriteMediaSetSpriteProperty", [mh, 1, 1, m]) == 0)
            #expect(qtCall("SpriteMediaHitTestAllSprites", [mh, 1, UInt32(22 << 16 | 42), hit]) == 0 && rd32(hit) == 1)
            #expect(qtCall("SpriteMediaSetSpriteProperty", [mh, 1, 4, 0]) == 0) // invisible
            let vis = tb_NewPtr(2, false, true)
            #expect(qtCall("SpriteMediaGetSpriteProperty", [mh, 1, 4, vis]) == 0 && rd8(vis) == 0)
            let idx = tb_NewPtr(4, false, true)
            #expect(qtCall("SpriteMediaSpriteIDToIndex", [mh, 1, idx]) == 0 && rd16(idx) == 1)
            #expect(qtCall("SpriteMediaGetSpriteProperty", [mh, 9, 1, m]) == UInt32(bitPattern: -2069))
            tb_DisposeMovie(movie)
        }
    }
}
