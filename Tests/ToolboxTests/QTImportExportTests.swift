import CCore
import CPU68K
import Foundation
import Testing
import Toolbox

private func le16(_ v: Int) -> [UInt8] { [UInt8(v & 0xFF), UInt8(v >> 8 & 0xFF)] }
private func le32(_ v: UInt32) -> [UInt8] { (0..<4).map { UInt8(v >> (8 * UInt32($0)) & 0xFF) } }

/// An AIFF of 16-bit mono samples at 8000 Hz.
private func aiff(_ samples: [Int16]) -> [UInt8] {
    let rate: [UInt8] = [0x40, 0x0B, 0xFA, 0, 0, 0, 0, 0, 0, 0] // 8000 as an 80-bit extended
    let comm = Array("COMM".utf8) + be32(18) + be16(1) + be32(UInt32(samples.count)) + be16(16) + rate
    let data = samples.flatMap { be16(Int(UInt16(bitPattern: $0))) }
    let ssnd = Array("SSND".utf8) + be32(UInt32(8 + data.count)) + be32(0) + be32(0) + data
    return Array("FORM".utf8) + be32(UInt32(4 + comm.count + ssnd.count)) + Array("AIFF".utf8) + comm + ssnd
}

/// A WAVE of 24-bit mono samples at 8000 Hz (an encoding the importer converts).
private func wave24(_ samples: [Int16]) -> [UInt8] {
    let data = samples.flatMap { s -> [UInt8] in let v = Int(s) << 8; return [UInt8(v & 0xFF), UInt8(v >> 8 & 0xFF), UInt8(v >> 16 & 0xFF)] }
    let fmt = Array("fmt ".utf8) + le32(16) + le16(1) + le16(1) + le32(8000) + le32(24000) + le16(3) + le16(24)
    let d = Array("data".utf8) + le32(UInt32(data.count)) + data
    return Array("RIFF".utf8) + le32(UInt32(4 + fmt.count + d.count)) + Array("WAVE".utf8) + fmt + d
}

/// A Standard MIDI File: middle C then E, a quarter note each at 120 bpm (96 ticks a quarter).
private let midiFile: [UInt8] = {
    let events: [UInt8] = [0x00, 0xC0, 0x00, 0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0x00, 0x90, 64, 90,
                           0x60, 0x80, 64, 0, 0x00, 0xFF, 0x2F, 0x00]
    return Array("MThd".utf8) + be32(6) + be16(0) + be16(1) + be16(96) + Array("MTrk".utf8) + be32(UInt32(events.count))
        + events
}()

private func handle(_ bytes: [UInt8]) -> UInt32 {
    let h = tb_NewHandle(Int32(bytes.count), false, false)
    if !bytes.isEmpty { gput(rd32(h), bytes, UInt32(bytes.count)) }
    return h
}

private func bytes(of h: UInt32) -> [UInt8] {
    let n = Int(tb_GetHandleSize(h))
    var out = [UInt8](repeating: 0, count: n)
    if n > 0 { gget(&out, rd32(h), UInt32(n)) }
    return out
}

/// qtMachine's machine, and the test application's folder.
private func qtMachineDir() -> URL {
    let dir = launchTestApp()
    _ = initToolbox()
    let stack = tb_NewPtr(65536, false, true)
    g_cpu!.pointee.a.7 = stack + 65536 - 16
    g_cpu!.pointee.halted = false
    _ = tb_EnterMovies()
    return dir
}

/// A movie of a host file, through NewMovieFromFile (and so a movie importer).
private func movieOf(_ bytes: [UInt8], named name: String, in dir: URL) throws -> Int32 {
    let path = dir.appendingPathComponent(name).path
    try Data(bytes).write(to: URL(fileURLWithPath: path))
    let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
    fm_spec_for_path(path, spec)
    let rn = tb_NewPtr(2, false, true)
    #expect(tb_OpenMovieFile(spec, rn, 1) == 0)
    let mp = tb_NewPtr(4, false, true)
    #expect(tb_NewMovieFromFile(mp, Int16(bitPattern: rd16(rn)), 0, 0, 0, 0) == 0)
    _ = tb_CloseMovieFile(Int16(bitPattern: rd16(rn)))
    return Int32(bitPattern: rd32(mp))
}

/// A movie exported through our 'spit' component of the kind, into a handle.
private func export(_ movie: Int32, _ kind: String, settings: UInt32 = 0) -> [UInt8] {
    let ci = components_open_default(fourCC("spit"), fourCC(kind))
    #expect(ci != 0)
    if settings != 0 { #expect(qtCall("MovieExportSetSettingsFromAtomContainer", [ci, settings]) == 0) }
    let h = tb_NewHandle(0, false, false)
    let r = qtCallErr("MovieExportToHandle", [ci, h, UInt32(bitPattern: movie), 0, 0,
                                              UInt32(bitPattern: tb_GetMovieDuration(movie))])
    #expect(r == 0)
    components_close(ci)
    return bytes(of: h)
}

private func samples16(aiff b: [UInt8]) -> (channels: Int, frames: Int, bits: Int, data: [Int16]) {
    var at = 12, channels = 0, frames = 0, bits = 0
    var data: [Int16] = []
    while at + 8 <= b.count {
        let id = String(decoding: b[at..<at + 4], as: UTF8.self)
        let len = Int(b[at + 4]) << 24 | Int(b[at + 5]) << 16 | Int(b[at + 6]) << 8 | Int(b[at + 7])
        if id == "COMM" {
            channels = Int(b[at + 8]) << 8 | Int(b[at + 9])
            frames = Int(b[at + 10]) << 24 | Int(b[at + 11]) << 16 | Int(b[at + 12]) << 8 | Int(b[at + 13])
            bits = Int(b[at + 14]) << 8 | Int(b[at + 15])
        } else if id == "SSND", bits == 16 {
            var i = at + 16
            while i + 1 < at + 8 + len { data.append(Int16(bitPattern: UInt16(b[i]) << 8 | UInt16(b[i + 1]))); i += 2 }
        }
        at += 8 + len + (len & 1)
    }
    return (channels, frames, bits, data)
}

extension BlueBox {
    @Suite struct QTImportExport {
        let ramp: [Int16] = (0..<4000).map { Int16(($0 % 200) * 300 - 30000) }

        /// The importers and exporters are components programs can find,
        /// with the flags QuickTime gave them.
        @Test func componentsAreRegistered() {
            qtMachine()
            for t in ["AIFF", "WAVE", "Midi", "PICT", "TEXT", "JPEG", "sfil", "PICS", "GIFf"] {
                #expect(components_find_next(0, fourCC("eat "), fourCC(t), 0) != 0, "eat \(t)")
            }
            for t in ["MooV", "AIFF", "WAVE", "ULAW", "sfil", "PICT", "TEXT", "Midi"] {
                #expect(components_find_next(0, fourCC("spit"), fourCC(t), 0) != 0, "spit \(t)")
            }
            let ci = components_open_default(fourCC("eat "), fourCC("WAVE"))
            let out = tb_NewPtr(4, false, true)
            #expect(qtCall("MovieImportGetMIMETypeList", [ci, out]) == 0 && rd32(out) != 0)
            #expect(qtCall("MovieImportGetFileType", [ci, out]) == 0 && rd32(out) == fourCC("WAVE"))
            components_close(ci)
        }

        /// An AIFF file opens as a movie of one sound track, its samples
        /// where the file has them; exported as AIFF they come back the same.
        @Test func aiffImportsInPlaceAndExports() throws {
            let dir = qtMachineDir()
            let movie = try movieOf(aiff(ramp), named: "Ramp.aif", in: dir)
            #expect(movie != 0)
            #expect(tb_GetMovieTrackCount(movie) == 1)
            let t = tb_GetMovieIndTrack(movie, 1)
            let media = tb_GetTrackMedia(t)
            #expect(tb_GetMediaTimeScale(media) == 8000)
            #expect(tb_GetMediaDuration(media) == 4000)
            #expect(tb_GetMovieDuration(movie) * 8000 == 4000 * tb_GetMovieTimeScale(movie))
            let type = tb_NewPtr(4, false, true)
            let ref = tb_NewPtr(4, false, true)
            #expect(tb_GetMediaDataRef(media, 1, ref, type, 0) == 0 && rd32(type) == fourCC("alis"))
            let out = samples16(aiff: export(movie, "AIFF"))
            #expect(out.channels == 1 && out.bits == 16 && out.frames == 4000)
            #expect(out.data == ramp)
        }

        /// A 24-bit WAVE is converted to 16-bit samples of its own; the
        /// sound exporter's settings give 8-bit stereo.
        @Test func waveConvertsAndSettingsApply() throws {
            let dir = qtMachineDir()
            let movie = try movieOf(wave24(ramp), named: "Ramp.wav", in: dir)
            #expect(tb_GetMovieTrackCount(movie) == 1)
            let out = samples16(aiff: export(movie, "AIFF"))
            #expect(out.frames == 4000 && out.data == ramp)
            // 8-bit stereo through the settings atoms
            let c = qt_atoms_new()
            var err: Int16 = 0
            let soun = qt_atoms_insert(c, 0, fourCC("soun"), 1, 0, nil, 0, &err)
            var rate = be32(8000 << 16), size = be16(8), chans = be16(2)
            _ = qt_atoms_insert(c, soun, fourCC("ssrt"), 1, 0, &rate, 4, &err)
            _ = qt_atoms_insert(c, soun, fourCC("ssss"), 1, 0, &size, 2, &err)
            _ = qt_atoms_insert(c, soun, fourCC("sscc"), 1, 0, &chans, 2, &err)
            let wav = export(movie, "WAVE", settings: c)
            #expect(Array(wav[0..<4]) == Array("RIFF".utf8))
            #expect(wav[22] == 2 && wav[34] == 8) // channels, bits
            #expect(wav.count == 44 + 8000)
        }

        /// A MIDI file becomes a music track of its notes; exported, the
        /// notes come back as a MIDI file.
        @Test func midiImportsAsMusicAndExports() throws {
            let dir = qtMachineDir()
            let movie = try movieOf(midiFile, named: "Tune.mid", in: dir)
            #expect(tb_GetMovieTrackCount(movie) == 1)
            let t = tb_GetMovieIndTrack(movie, 1)
            let media = tb_GetTrackMedia(t)
            let mtype = tb_NewPtr(4, false, true)
            tb_GetMediaHandlerDescription(media, mtype, 0, 0)
            #expect(rd32(mtype) == fourCC("musi"))
            // two quarters at 120 bpm: a second, in 600ths
            #expect(abs(tb_GetMediaDuration(media) - 601) <= 1)
            let smf = export(movie, "Midi")
            #expect(Array(smf[0..<4]) == Array("MThd".utf8))
            var notes: [UInt8] = []
            for i in 22..<smf.count - 2 where smf[i] & 0xF0 == 0x90 && smf[i + 2] > 0 { notes.append(smf[i + 1]) }
            #expect(notes == [60, 64])
        }

        /// Text imported from a handle: a sample a line, two seconds each;
        /// QTtext's time stamps place them; the text exporter gives it back.
        @Test func textImportsAndExports() {
            qtMachine()
            let movie = tb_NewMovie(0)
            let ci = components_open_default(fourCC("eat "), fourCC("TEXT"))
            let out = tb_NewPtr(12, false, true)
            let h = handle(Array("Hello\rWorld".utf8))
            #expect(qtCall("MovieImportHandle", [ci, h, UInt32(bitPattern: movie), 0, out, 0, out + 4, 0, out + 8]) == 0)
            #expect(rd32(out) != 0)
            #expect(tb_GetMovieDuration(movie) == 2400)
            #expect(String(decoding: export(movie, "TEXT"), as: UTF8.self) == "Hello\rWorld\r")
            let timed = tb_NewMovie(0)
            let q = handle(Array("{QTtext}{timeScale:10}\r[00:00:00.0]\rOne\r[00:00:01.5]\rTwo\r[00:00:03.0]\r".utf8))
            #expect(qtCall("MovieImportHandle", [ci, q, UInt32(bitPattern: timed), 0, out, 0, out + 4, 0, out + 8]) == 0)
            let media = tb_GetTrackMedia(tb_GetMovieIndTrack(timed, 1))
            #expect(tb_GetMediaTimeScale(media) == 10)
            #expect(tb_GetMediaDuration(media) == 30)
            #expect(String(decoding: export(timed, "TEXT"), as: UTF8.self) == "One\rTwo\r")
            components_close(ci)
        }

        /// A picture pasted into a movie becomes a frame of a video track;
        /// the PICT exporter draws the same picture back.
        @Test func pictureImportsAsAFrame() {
            qtMachine()
            let gw = tb_NewPtr(4, false, true)
            let r = tb_NewPtr(8, false, true)
            wr16(r + 4, 10); wr16(r + 6, 16)
            #expect(tb_NewGWorld(gw, 32, r, 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            let pic = tb_OpenPicture(r)
            tb_EraseRect(r)
            let red = tb_NewPtr(6, false, true)
            wr16(red, 0xFFFF)
            tb_RGBForeColor(red)
            let half = tb_NewPtr(8, false, true)
            wr16(half + 4, 10); wr16(half + 6, 8)
            tb_PaintRect(half)
            tb_ClosePicture()
            let movie = tb_NewMovie(0)
            #expect(tb_PasteHandleIntoMovie(Int32(bitPattern: pic), Int32(bitPattern: fourCC("PICT")), movie, 0, 0) == 0)
            #expect(tb_GetMovieTrackCount(movie) == 1)
            let back = export(movie, "PICT")
            #expect(back.count > 10)
            let ph = handle(back)
            tb_SetGWorld(rd32(gw), 0)
            let fg = tb_NewPtr(6, false, true)
            tb_RGBForeColor(fg)
            tb_EraseRect(r)
            tb_DrawPicture(ph, r)
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(2, 5, c)
            #expect(rd16(c) == 0xFFFF && rd16(c + 2) == 0 && rd16(c + 4) == 0)
            tb_GetCPixel(12, 5, c)
            #expect(rd16(c) == 0xFFFF && rd16(c + 2) == 0xFFFF && rd16(c + 4) == 0xFFFF)
            tb_DisposeGWorld(rd32(gw))
        }

        /// A movie exported as a QuickTime movie is self-contained: the
        /// bytes open as a movie of the same length.
        @Test func movieExportIsSelfContained() throws {
            let dir = qtMachineDir()
            let movie = try movieOf(aiff(ramp), named: "Ramp.aif", in: dir)
            let flat = export(movie, "MooV")
            #expect(flat.count > 8000)
            let again = try movieOf(flat, named: "Flat.mov", in: dir)
            #expect(tb_GetMovieDuration(again) == tb_GetMovieDuration(movie))
            #expect(samples16(aiff: export(again, "AIFF")).data == ramp)
        }
    }
}

extension BlueBox {
    @Suite struct QTSpriteToolbox {
        func gworld(_ w: Int, _ h: Int) -> UInt32 {
            let gw = tb_NewPtr(4, false, true)
            let r = tb_NewPtr(8, false, true)
            wr16(r + 4, UInt16(h)); wr16(r + 6, UInt16(w))
            #expect(tb_NewGWorld(gw, 32, r, 0, 0, 0) == 0)
            return rd32(gw)
        }

        func pixel(_ port: UInt32, _ h: Int16, _ v: Int16) -> UInt32 {
            let saved = tb_NewPtr(8, false, true)
            tb_GetGWorld(saved, saved + 4)
            tb_SetGWorld(port, 0)
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(h, v, c)
            tb_SetGWorld(rd32(saved), rd32(saved + 4))
            return UInt32(rd16(c) >> 8) << 16 | UInt32(rd16(c + 2) >> 8) << 8 | UInt32(rd16(c + 4) >> 8)
        }

        /// A 4x4 red image as a raw 32-bit ImageDescription and its data.
        func redImage() -> (UInt32, UInt32) {
            let idh = tb_NewHandle(86, false, true)
            let d = rd32(idh)
            wr32(d, 86); wr32(d + 4, fourCC("raw "))
            wr16(d + 32, 4); wr16(d + 34, 4)
            wr32(d + 36, 72 << 16); wr32(d + 40, 72 << 16)
            wr32(d + 44, 64); wr16(d + 48, 1); wr16(d + 82, 32); wr16(d + 84, 0xFFFF)
            let data = tb_NewPtr(64, false, true)
            for i in 0..<16 { wr32(data + UInt32(i * 4), 0xFFFF_0000) }
            return (idh, data)
        }

        /// Sprites composited over the background colour and copied to the
        /// destination when the world is idled; moved, hidden and hit tested.
        @Test func spritesDrawMoveAndHit() {
            qtMachine()
            let dest = gworld(32, 32), layer = gworld(32, 32)
            let blue = tb_NewPtr(6, false, true)
            wr16(blue + 4, 0xFFFF)
            let out = tb_NewPtr(4, false, true)
            #expect(tb_NewSpriteWorld(out, Int32(bitPattern: dest), Int32(bitPattern: layer), blue, 0) == 0)
            let world = Int32(bitPattern: rd32(out))
            let (idh, data) = redImage()
            let m = tb_NewPtr(36, false, true)
            wr32(m, 0x10000); wr32(m + 16, 0x10000); wr32(m + 24, 8 << 16); wr32(m + 28, 10 << 16); wr32(m + 32, 0x4000_0000)
            #expect(tb_NewSprite(out, world, Int32(bitPattern: idh), Int32(bitPattern: data), m, 1, 0) == 0)
            let sprite = Int32(bitPattern: rd32(out))
            let flags = tb_NewPtr(4, false, true)
            #expect(tb_SpriteWorldIdle(world, 0, flags) == 0 && rd32(flags) & 1 == 1)
            #expect(pixel(dest, 9, 11) == 0xFF0000)
            #expect(pixel(dest, 2, 2) == 0x0000FF)
            // nothing changed: nothing drawn
            #expect(tb_SpriteWorldIdle(world, 0, flags) == 0 && rd32(flags) == 0)
            // hit tests: on the sprite, beside it
            let hit = tb_NewPtr(4, false, true)
            #expect(tb_SpriteWorldHitTest(world, 1, 11 << 16 | 9, hit) == 0 && rd32(hit) == UInt32(bitPattern: sprite))
            #expect(tb_SpriteWorldHitTest(world, 1, 2 << 16 | 2, hit) == 0 && rd32(hit) == 0)
            // moved, then hidden
            wr32(m + 24, 20 << 16)
            #expect(tb_SetSpriteProperty(sprite, 1, m) == 0)
            #expect(tb_SpriteWorldIdle(world, 0, flags) == 0)
            #expect(pixel(dest, 9, 11) == 0x0000FF && pixel(dest, 21, 11) == 0xFF0000)
            #expect(tb_SetSpriteProperty(sprite, 4, 0) == 0)
            let vis = tb_NewPtr(2, false, true)
            #expect(tb_GetSpriteProperty(sprite, 4, vis) == 0 && rd8(vis) == 0)
            #expect(tb_SpriteWorldIdle(world, 0, flags) == 0)
            #expect(pixel(dest, 21, 11) == 0x0000FF)
            #expect(tb_SetSpriteProperty(sprite, 99, 0) == -2065)
            tb_DisposeSprite(sprite)
            tb_DisposeSpriteWorld(world)
        }
    }
}
