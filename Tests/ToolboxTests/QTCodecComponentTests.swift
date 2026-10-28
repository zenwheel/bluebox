import AudioToolbox
import BlueBoxHost
import CoreMedia
import CoreVideo
import ImageIO
import Testing
import Toolbox
import UniformTypeIdentifiers
import VideoToolbox

/// The codecs as components (qt_codec_comp.c), a program's own codec
/// decoding what ours can't, and the host's decoders (hostvideo.h).
extension BlueBox {
    @Suite struct QTCodecComponents {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(v)) }
            return p
        }

        /// An ImageDescription handle: codec, size, depth, then extension atoms.
        func description(_ type: UInt32, _ w: Int, _ h: Int, depth: Int = 32, dataSize: Int = 0,
                         extensions: [(String, [UInt8])] = []) -> UInt32 {
            var size = 86
            for e in extensions { size += 8 + e.1.count }
            let hd = tb_NewHandle(Int32(size), false, true)
            let p = rd32(hd)
            for i in 0..<size { wr8(p + UInt32(i), 0) }
            wr32(p, UInt32(size))
            wr32(p + 4, type)
            wr16(p + 32, UInt16(w))
            wr16(p + 34, UInt16(h))
            wr32(p + 36, 72 << 16)
            wr32(p + 40, 72 << 16)
            wr32(p + 44, UInt32(dataSize))
            wr16(p + 48, 1)
            wr16(p + 82, UInt16(depth))
            wr16(p + 84, 0xFFFF)
            var at = p + 86
            for e in extensions {
                wr32(at, UInt32(8 + e.1.count))
                wr32(at + 4, fourCC(e.0))
                for (i, b) in e.1.enumerated() { wr8(at + 8 + UInt32(i), b) }
                at += UInt32(8 + e.1.count)
            }
            return hd
        }

        func gworld(_ w: Int = 20, _ h: Int = 20) -> UInt32 {
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, h, w), 0, 0, 0) == 0)
            tb_SetGWorld(rd32(gw), 0)
            return rd32(gw)
        }

        func pixel(_ h: Int, _ v: Int) -> (Int, Int, Int) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            return (Int(rd16(c) >> 8), Int(rd16(c + 2) >> 8), Int(rd16(c + 4) >> 8))
        }

        func near(_ a: (Int, Int, Int), _ b: (Int, Int, Int), _ d: Int) -> Bool {
            abs(a.0 - b.0) <= d && abs(a.1 - b.1) <= d && abs(a.2 - b.2) <= d
        }

        /// A 2x2 'raw ' image: red, green / blue, white.
        func rawImage() -> (desc: UInt32, data: UInt32) {
            let data = tb_NewPtr(16, false, true)
            for (i, v) in ([0xFFFF_0000, 0xFF00_FF00, 0xFF00_00FF, 0xFFFF_FFFF] as [UInt32]).enumerated() {
                wr32(data + UInt32(i * 4), v)
            }
            return (description(fourCC("raw "), 2, 2, dataSize: 16), data)
        }

        /// Our codecs are there to find, with their information.
        @Test func codecsAreComponents() {
            qtMachine()
            for t in ["cvid", "rle ", "smc ", "rpza", "raw ", "jpeg"] {
                #expect(components_find_next(0, fourCC("imdc"), fourCC(t), 0) != 0, "\(t)")
            }
            for t in ["raw ", "rle ", "jpeg"] {
                #expect(components_find_next(0, fourCC("imco"), fourCC(t), 0) != 0, "\(t)")
            }
            var err: Int16 = 0
            let ci = components_open_component(components_find_next(0, fourCC("imdc"), fourCC("cvid"), 0), &err)
            #expect(ci != 0)
            let info = tb_NewPtr(72, false, true)
            #expect(qtCall("ImageCodecGetCodecInfo", [ci, info]) == 0)
            #expect(rd8(info) == 7 && rd32(info + 36) == fourCC("appl") && rd32(info + 40) != 0)
            components_close(ci)
            // the ICM asks the components
            #expect(tb_GetCodecInfo(info, Int32(bitPattern: fourCC("smc ")), 0) == 0 && rd8(info) == 8)
            let found = tb_NewPtr(8, false, true)
            #expect(tb_FindCodec(Int32(bitPattern: fourCC("cvid")), 0, found, found + 4) == 0)
            #expect(rd32(found) == 0 && rd32(found + 4) != 0) // Cinepak: decoded, not encoded
        }

        /// PreDecompress and BandDecompress into the PixMap in the parameters.
        @Test func bandDecompressFillsThePixMap() {
            qtMachine()
            let port = gworld()
            let img = rawImage()
            var err: Int16 = 0
            let ci = components_open_component(components_find_next(0, fourCC("imdc"), fourCC("raw "), 0), &err)
            let p = tb_NewPtr(256, false, true)
            let cap = tb_NewPtr(24, false, true)
            wr32(p + 4, img.desc)
            wr32(p + 8, img.data)
            wr32(p + 12, 16)
            wr32(p + 34, cap)
            wr32(p + 62, port)
            let pm = rd32(rd32(port + 2)) // the GWorld's PixMap
            for i in 0..<50 { wr8(p + 66 + UInt32(i), rd8(pm + UInt32(i))) }
            for (i, v) in [4, 4, 8, 8].enumerated() { wr16(p + 152 + UInt32(i * 2), UInt16(v)) } // dstRect
            #expect(qtCall("ImageCodecPreDecompress", [ci, p]) == 0)
            #expect(rd16(cap + 4) == 32)
            #expect(qtCall("ImageCodecBandDecompress", [ci, p]) == 0)
            #expect(pixel(4, 4) == (255, 0, 0))
            #expect(pixel(7, 7) == (255, 255, 255))
            #expect(pixel(5, 7) == (0, 0, 255))
            components_close(ci)
            tb_DisposeGWorld(port)
        }

        /// The base codec's calls: Initialize, Preflight, BeginBand, DrawBand
        /// into 32-bit ARGB rows.
        @Test func subcodecDrawBand() {
            qtMachine()
            let img = rawImage()
            var err: Int16 = 0
            let ci = components_open_component(components_find_next(0, fourCC("imdc"), fourCC("raw "), 0), &err)
            let caps = tb_NewPtr(32, false, true)
            #expect(qtCall("ImageCodecInitialize", [ci, caps]) == 0)
            let p = tb_NewPtr(256, false, true)
            let cap = tb_NewPtr(24, false, true)
            wr32(p + 4, img.desc)
            wr32(p + 12, 16)
            wr32(p + 34, cap)
            #expect(qtCall("ImageCodecPreflight", [ci, p]) == 0)
            let drp = tb_NewPtr(64, false, true)
            let rows = tb_NewPtr(2 * 16, false, true)
            wr32(drp, rows)
            wr32(drp + 4, 16)
            wr32(drp + 8, img.data)
            #expect(qtCall("ImageCodecBeginBand", [ci, p, drp, 0]) == 0)
            #expect(qtCall("ImageCodecDrawBand", [ci, drp]) == 0)
            #expect(rd32(rows) == 0xFFFF_0000 && rd32(rows + 4) == 0xFF00_FF00 && rd32(rows + 16) == 0xFF00_00FF)
            #expect(qtCall("ImageCodecEndBand", [ci, drp, 0, 0]) == 0)
            components_close(ci)
        }

        /// A compressor's BandCompress: the source PixMap into data, the
        /// description filled in.
        @Test func bandCompress() {
            qtMachine()
            let port = gworld(4, 4)
            let c = tb_NewPtr(6, false, true)
            wr16(c, 0xFFFF); wr16(c + 2, 0); wr16(c + 4, 0)
            tb_RGBForeColor(c)
            tb_PaintRect(rect(0, 0, 4, 4))
            var err: Int16 = 0
            let ci = components_open_component(components_find_next(0, fourCC("imco"), fourCC("raw "), 0), &err)
            let size = tb_NewPtr(4, false, true)
            let pmh = rd32(port + 2)
            #expect(qtCall("ImageCodecGetMaxCompressionSize", [ci, pmh, rect(0, 0, 4, 4), 32, 0x200, size]) == 0)
            #expect(rd32(size) >= 64)
            let p = tb_NewPtr(256, false, true)
            let desc = description(fourCC("raw "), 4, 4)
            let data = tb_NewPtr(Int32(rd32(size)), false, true)
            let cap = tb_NewPtr(24, false, true)
            wr32(p + 4, desc)
            wr32(p + 8, data)
            wr32(p + 34, cap)
            let pm = rd32(pmh)
            for i in 0..<50 { wr8(p + 62 + UInt32(i), rd8(pm + UInt32(i))) }
            wr32(p + 162, 0x200)
            #expect(qtCall("ImageCodecPreCompress", [ci, p]) == 0)
            #expect(qtCall("ImageCodecBandCompress", [ci, p]) == 0)
            #expect(rd32(rd32(desc) + 4) == fourCC("raw ") && rd32(rd32(desc) + 44) > 0)
            #expect(rd32(data) & 0xFF_FFFF == 0xFF_0000)
            components_close(ci)
            tb_DisposeGWorld(port)
        }

        /// A codec the program registered decodes a type we have no decoder for.
        @Test func aProgramsCodecDecodes() {
            qtMachine()
            let fn: component_c_fn = { _, params, _ in
                let what = Int16(bitPattern: rd16(params + 2))
                if what == -1 || what == -2 { return 0 }
                if what == 5 { return 0 } // PreDecompress
                if what == 6 { // BandDecompress: paint the PixMap green
                    let p = rd32(params + 4)
                    let base = rd32(p + 66), rb = UInt32(rd16(p + 70) & 0x3FFF)
                    for y in 0..<2 { for x in 0..<2 { wr32(base + UInt32(y) * rb + UInt32(x * 4), 0x0000_FF00) } }
                    return 0
                }
                return Int32(bitPattern: 0x8000_8002)
            }
            let comp = components_register_c(fourCC("imdc"), fourCC("zzzz"), fourCC("test"), 0, "Test", "Test", fn, nil)
            #expect(comp != 0)
            let port = gworld()
            let desc = description(fourCC("zzzz"), 2, 2, dataSize: 4)
            let data = tb_NewPtr(4, false, true)
            #expect(tb_DecompressImage(Int32(bitPattern: data), Int32(bitPattern: desc), 0, 0, rect(0, 0, 2, 2), 0, 0) == 0)
            #expect(pixel(1, 1) == (0, 255, 0))
            tb_DisposeGWorld(port)
        }

        // ---- The host's decoders ----

        /// A solid 64x64 colour as a CGImage.
        func solid(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ w: Int = 64, _ h: Int = 64) -> CGImage {
            let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                space: CGColorSpaceCreateDeviceRGB(),
                                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            ctx.setFillColor(red: r, green: g, blue: b, alpha: 1)
            ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
            return ctx.makeImage()!
        }

        func encoded(_ img: CGImage, _ type: UTType) -> [UInt8] {
            let out = NSMutableData()
            let dst = CGImageDestinationCreateWithData(out, type.identifier as CFString, 1, nil)!
            CGImageDestinationAddImage(dst, img, nil)
            CGImageDestinationFinalize(dst)
            return [UInt8](out as Data)
        }

        func decompress(_ desc: UInt32, _ bytes: [UInt8], _ w: Int, _ h: Int) -> Int16 {
            let data = tb_NewPtr(Int32(bytes.count), false, true)
            for (i, b) in bytes.enumerated() { wr8(data + UInt32(i), b) }
            return tb_DecompressImage(Int32(bitPattern: data), Int32(bitPattern: desc), 0, 0, rect(0, 0, h, w), 0, 0)
        }

        @Test func hostDecodesPNG() {
            qtMachine()
            #expect(hostvideo_supported(fourCC("png ")))
            let port = gworld(64, 64)
            let png = encoded(solid(0, 0, 1), .png)
            #expect(decompress(description(fourCC("png "), 64, 64, dataSize: png.count), png, 64, 64) == 0)
            #expect(pixel(10, 10) == (0, 0, 255))
            tb_DisposeGWorld(port)
        }

        /// Motion JPEG A: two JPEG fields, interleaved.
        @Test func hostDecodesMotionJPEGFields() {
            qtMachine()
            let port = gworld(64, 64)
            let top = encoded(solid(1, 0, 0, 64, 32), .jpeg), bottom = encoded(solid(0, 0, 1, 64, 32), .jpeg)
            let both = top + bottom
            #expect(decompress(description(fourCC("mjpa"), 64, 64, dataSize: both.count), both, 64, 64) == 0)
            #expect(near(pixel(10, 10), (255, 0, 0), 24)) // even rows: the first field
            #expect(near(pixel(10, 11), (0, 0, 255), 24)) // odd: the second
            tb_DisposeGWorld(port)
        }

        /// An H.264 frame from VideoToolbox's encoder, its 'avcC' carried in
        /// the description, decodes through the host.
        @Test func hostDecodesH264() throws {
            qtMachine()
            var session: VTCompressionSession?
            #expect(VTCompressionSessionCreate(allocator: nil, width: 64, height: 64, codecType: kCMVideoCodecType_H264,
                                               encoderSpecification: nil, imageBufferAttributes: nil,
                                               compressedDataAllocator: nil, outputCallback: nil, refcon: nil,
                                               compressionSessionOut: &session) == noErr)
            let s = try #require(session)
            var pb: CVPixelBuffer?
            CVPixelBufferCreate(nil, 64, 64, kCVPixelFormatType_32BGRA, nil, &pb)
            let buf = try #require(pb)
            CVPixelBufferLockBaseAddress(buf, [])
            let base = CVPixelBufferGetBaseAddress(buf)!.assumingMemoryBound(to: UInt8.self)
            let rb = CVPixelBufferGetBytesPerRow(buf)
            for y in 0..<64 { for x in 0..<64 { let o = y * rb + x * 4; base[o] = 0; base[o + 1] = 200; base[o + 2] = 0; base[o + 3] = 255 } }
            CVPixelBufferUnlockBaseAddress(buf, [])
            var out: CMSampleBuffer?
            #expect(VTCompressionSessionEncodeFrame(s, imageBuffer: buf, presentationTimeStamp: .zero,
                                                    duration: .invalid, frameProperties: nil, infoFlagsOut: nil,
                                                    outputHandler: { _, _, sb in out = sb }) == noErr)
            VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
            let sample = try #require(out)
            let fd = try #require(CMSampleBufferGetFormatDescription(sample))
            let atoms = CMFormatDescriptionGetExtension(fd, extensionKey: kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms) as? [String: Any]
            let avcC = try #require(atoms?["avcC"] as? Data)
            let block = try #require(CMSampleBufferGetDataBuffer(sample))
            var bytes = [UInt8](repeating: 0, count: CMBlockBufferGetDataLength(block))
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: bytes.count, destination: &bytes)
            let port = gworld(64, 64)
            let desc = description(fourCC("avc1"), 64, 64, depth: 24, dataSize: bytes.count, extensions: [("avcC", [UInt8](avcC))])
            #expect(decompress(desc, bytes, 64, 64) == 0)
            let c = pixel(32, 32) // green, give or take the encoder's colour range
            #expect(c.1 > 160 && c.0 < 60 && c.2 < 60, "\(c)")
            tb_DisposeGWorld(port)
        }

        /// AAC from AudioToolbox's encoder decodes, packet by packet, with its cookie.
        @Test func hostDecodesAAC() throws {
            var inFmt = AudioStreamBasicDescription(mSampleRate: 44100, mFormatID: kAudioFormatLinearPCM,
                                                    mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
                                                    mBytesPerPacket: 2, mFramesPerPacket: 1, mBytesPerFrame: 2,
                                                    mChannelsPerFrame: 1, mBitsPerChannel: 16, mReserved: 0)
            var aac = AudioStreamBasicDescription(mSampleRate: 44100, mFormatID: kAudioFormatMPEG4AAC, mFormatFlags: 0,
                                                  mBytesPerPacket: 0, mFramesPerPacket: 1024, mBytesPerFrame: 0,
                                                  mChannelsPerFrame: 1, mBitsPerChannel: 0, mReserved: 0)
            var conv: AudioConverterRef?
            #expect(AudioConverterNew(&inFmt, &aac, &conv) == noErr)
            let c = try #require(conv)
            let frames = 44100 / 2
            var pcm = (0..<frames).map { Int16(10000 * sin(Double($0) * 2 * .pi * 440 / 44100)) }
            final class Feed { var data: UnsafeMutablePointer<Int16>; var left: Int; init(_ d: UnsafeMutablePointer<Int16>, _ n: Int) { data = d; left = n } }
            var packets: [[UInt8]] = []
            pcm.withUnsafeMutableBufferPointer { p in
                let feed = Feed(p.baseAddress!, frames)
                let out = UnsafeMutablePointer<UInt8>.allocate(capacity: 8192)
                defer { out.deallocate() }
                while true {
                    var n: UInt32 = 1
                    var list = AudioBufferList(mNumberBuffers: 1, mBuffers: AudioBuffer(mNumberChannels: 1, mDataByteSize: 8192, mData: out))
                    var desc = AudioStreamPacketDescription()
                    let st = AudioConverterFillComplexBuffer(c, { _, count, io, _, ref in
                        let f = Unmanaged<Feed>.fromOpaque(ref!).takeUnretainedValue()
                        if f.left == 0 { count.pointee = 0; return noErr }
                        let k = min(Int(count.pointee), f.left, 4096)
                        io.pointee.mBuffers.mData = UnsafeMutableRawPointer(f.data)
                        io.pointee.mBuffers.mDataByteSize = UInt32(k * 2)
                        io.pointee.mBuffers.mNumberChannels = 1
                        f.data += k; f.left -= k
                        count.pointee = UInt32(k)
                        return noErr
                    }, Unmanaged.passUnretained(feed).toOpaque(), &n, &list, &desc)
                    if st != noErr || n == 0 { break }
                    packets.append([UInt8](UnsafeBufferPointer(start: out, count: Int(desc.mDataByteSize))))
                }
            }
            var cookieSize: UInt32 = 0
            AudioConverterGetPropertyInfo(c, kAudioConverterCompressionMagicCookie, &cookieSize, nil)
            var cookie = [UInt8](repeating: 0, count: Int(cookieSize))
            AudioConverterGetProperty(c, kAudioConverterCompressionMagicCookie, &cookieSize, &cookie)
            AudioConverterDispose(c)
            #expect(packets.count > 10)
            #expect(hostaudio_supported(fourCC("mp4a")))
            let dec = try #require(hostaudio_open(fourCC("mp4a"), 1, 44100, cookie, cookie.count))
            let all = packets.flatMap { $0 }
            let sizes = packets.map { UInt32($0.count) }
            var out: UnsafeMutablePointer<Int16>?
            let got = hostaudio_decode(dec, all, all.count, sizes, Int32(sizes.count), &out)
            #expect(got >= UInt32(frames) - 2048)
            var peak: Int16 = 0
            for i in 0..<Int(got) { peak = max(peak, out![i]) }
            #expect(peak > 5000)
            free(out)
            hostaudio_close(dec)
        }

        /// QuickTime 4's plain sound formats: G.711, 24 and 32-bit integers, floats either way round.
        @Test func plainSoundFormats() {
            func decode(_ f: String, _ bytes: [UInt8], little: Bool = false) -> [Int16] {
                var fmt = qt_sound_format()
                fmt.format = fourCC(f); fmt.channels = 1; fmt.bits = 16; fmt.rate = 8000; fmt.little = little
                #expect(qt_adec_supported(&fmt), "\(f)")
                var out: UnsafeMutablePointer<Int16>?
                let n = qt_adec_decode(&fmt, bytes, bytes.count, &out)
                defer { free(out) }
                return (0..<Int(n)).map { out![$0] }
            }
            #expect(decode("ulaw", [0xFF, 0x80, 0x00]) == [0, 32124, -32124])
            #expect(decode("alaw", [0xD5, 0xAA]) == [8, 32256])
            #expect(decode("in24", [0x40, 0x00, 0x00, 0xC0, 0x00, 0x00]) == [0x4000, -0x4000])
            #expect(decode("fl32", [0x3F, 0x00, 0x00, 0x00]) == [16383])
            #expect(decode("fl32", [0x00, 0x00, 0x00, 0xBF], little: true) == [-16383])
            #expect(decode("fl64", [0x3F, 0xF0, 0, 0, 0, 0, 0, 0]) == [32767])
            // a version 1 description's packets and a 'wave' atom's 'enda'
            var e = [UInt8](repeating: 0, count: 52)
            e[17] = 1
            e[39] = 1; e[47] = 4
            e += [0, 0, 0, 26] + Array("wave".utf8) + [0, 0, 0, 10] + Array("enda".utf8) + [0, 1] + [0, 0, 0, 8] + Array("term".utf8)
            e[3] = UInt8(e.count)
            var fmt = qt_sound_format()
            qt_sound_desc_extras(e, UInt32(e.count), &fmt)
            #expect(fmt.frames_per_packet == 1 && fmt.bytes_per_packet == 4 && fmt.little)
        }

        // ---- Graphics exporters ----

        func exporter(_ type: String) -> UInt32 {
            var err: Int16 = 0
            let c = components_find_next(0, fourCC("grex"), fourCC(type), 0)
            #expect(c != 0, "\(type)")
            return components_open_component(c, &err)
        }

        func handleBytes(_ h: UInt32) -> [UInt8] {
            (0..<Int(tb_GetHandleSize(h))).map { rd8(rd32(h) + UInt32($0)) }
        }

        /// A GWorld's pixels out as PNG through the host, read back the same.
        @Test func exportsPNGFromAGWorld() {
            qtMachine()
            ClassicImages.install()
            let port = gworld(8, 6)
            let c = tb_NewPtr(6, false, true)
            wr16(c, 0); wr16(c + 2, 0xFFFF); wr16(c + 4, 0)
            tb_RGBForeColor(c)
            tb_PaintRect(rect(0, 0, 6, 8))
            let ex = exporter("PNGf")
            let out = tb_NewHandle(0, false, false)
            #expect(qtCall("GraphicsExportSetInputGWorld", [ex, port]) == 0)
            #expect(qtCall("GraphicsExportSetOutputHandle", [ex, out]) == 0)
            let dims = tb_NewPtr(8, false, true)
            #expect(qtCall("GraphicsExportGetInputImageDimensions", [ex, dims]) == 0 && rd16(dims + 6) == 8 && rd16(dims + 4) == 6)
            let size = tb_NewPtr(4, false, true)
            #expect(qtCall("GraphicsExportDoExport", [ex, size]) == 0)
            let bytes = handleBytes(out)
            #expect(rd32(size) == UInt32(bytes.count) && bytes.starts(with: [0x89, 0x50, 0x4E, 0x47]))
            var rgba: UnsafeMutablePointer<UInt8>?
            var w: Int32 = 0, h: Int32 = 0
            #expect(qt_decode_image(bytes, bytes.count, &rgba, &w, &h) && w == 8 && h == 6)
            #expect(rgba![0] == 0 && rgba![1] == 255 && rgba![2] == 0)
            free(rgba)
            let ext = tb_NewPtr(4, false, true)
            #expect(qtCall("GraphicsExportGetDefaultFileNameExtension", [ex, ext]) == 0 && rd32(ext) == fourCC("png\0"))
            components_close(ex)
            tb_DisposeGWorld(port)
        }

        /// A graphics importer's image as a PICT file and a QuickTime Image.
        @Test func exportsAnImportersImage() {
            qtMachine()
            ClassicImages.install()
            let png = encoded(solid(1, 0, 0, 4, 4), .png)
            let data = tb_NewHandle(Int32(png.count), false, false)
            for (i, b) in png.enumerated() { wr8(rd32(data) + UInt32(i), b) }
            let ref = tb_NewHandle(4, false, false)
            wr32(rd32(ref), data)
            let gi = tb_NewPtr(4, false, true)
            #expect(tb_GetGraphicsImporterForDataRef(Int32(bitPattern: ref), Int32(bitPattern: fourCC("hndl")), gi) == 0)
            for type in ["PICT", "qtif", "JPEG", "TIFF", "BMPf"] {
                let ex = exporter(type)
                let out = tb_NewHandle(0, false, false)
                #expect(qtCall("GraphicsExportSetInputGraphicsImporter", [ex, rd32(gi)]) == 0)
                #expect(qtCall("GraphicsExportSetOutputHandle", [ex, out]) == 0)
                #expect(qtCall("GraphicsExportDoExport", [ex, 0]) == 0, "\(type)")
                let b = handleBytes(out)
                switch type {
                case "PICT": #expect(b.count > 512 + 10 && b[512 + 8] == 0 && b[512 + 9] == 4) // frame 4 wide
                case "qtif": #expect(b.count > 100 && Array(b[4..<8]) == Array("idsc".utf8))
                default:
                    var rgba: UnsafeMutablePointer<UInt8>?
                    var w: Int32 = 0, h: Int32 = 0
                    #expect(qt_decode_image(b, b.count, &rgba, &w, &h) && w == 4 && h == 4, "\(type)")
                    #expect(rgba != nil && rgba![0] > 200 && rgba![1] < 50, "\(type)")
                    free(rgba)
                }
                components_close(ex)
            }
        }
    }
}
