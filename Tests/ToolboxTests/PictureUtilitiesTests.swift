import CCore
import Testing
import Toolbox

extension BlueBox {
    /// Picture Utilities (Pack15): a picture's or pixel map's depth, counts,
    /// fonts, comments and colours.
    @Suite struct PictureUtilities {
        static let returnColorTable: Int16 = 1, recordComments: Int16 = 4, recordFontInfo: Int16 = 8

        func handle(_ bytes: [UInt8]) -> UInt32 {
            let h = tb_NewHandle(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            return h
        }

        /// A version 2 picture, frame 0,0,20,20: a 2-by-2 16-bit
        /// DirectBitsRect (red, green, blue, red), a paintRect, the text
        /// "Hi" in font 3, and three comments of two kinds.
        func picture() -> UInt32 {
            let header = cat(be16(0), be16(0), be16(0), be16(20), be16(20), be16(0x0011), be16(0x02FF),
                             be16(0x0C00), be16(-1 & 0xFFFF), be16(0xFFFF), [UInt8](repeating: 0, count: 20))
            let bits = cat(be16(0x009A), be32(0xFF), be16(0x8004), be16(0), be16(0), be16(2), be16(2),
                           be16(0), be16(1), be32(0), be32(72 << 16), be32(72 << 16),
                           be16(16), be16(16), be16(3), be16(5), be32(0), be32(0), be32(0),
                           be16(0), be16(0), be16(2), be16(2), be16(0), be16(0), be16(2), be16(2), be16(0),
                           be16(0x7C00), be16(0x03E0), be16(0x001F), be16(0x7C00))
            let shapes = cat(be16(0x0031), be16(4), be16(4), be16(8), be16(8),
                             be16(0x0003), be16(3), be16(0x0028), be16(12), be16(4), [2, 0x48, 0x69], [0],
                             be16(0x00A0), be16(100), be16(0x00A0), be16(101), be16(0x00A1), be16(100), be16(0),
                             be16(0x00FF))
            return handle(cat(header, bits, shapes))
        }

        func long(_ info: UInt32, _ off: UInt32) -> UInt32 { rd32(info + off) }

        @Test func getPictInfoSurveysThePicture() {
            launchTestApp()
            _ = initToolbox()
            let pic = picture()
            let info = tb_NewPtr(Int32(PictInfo_SIZEOF), false, true)
            let verb = Self.returnColorTable | Self.recordComments | Self.recordFontInfo
            #expect(tb_GetPictInfo(pic, info, verb, 2, 1, 0) == 0)
            #expect(rd16(info + PictInfo_version_off) == 2)
            #expect(rd16(info + PictInfo_depth_off) == 16)  // the deepest pixel map
            #expect(long(info, PictInfo_hRes_off) == 72 << 16)
            #expect(long(info, PictInfo_pixMapCount_off) == 1 && long(info, PictInfo_bitMapCount_off) == 0)
            #expect(long(info, PictInfo_rectCount_off) == 1 && long(info, PictInfo_textCount_off) == 1)
            #expect(long(info, PictInfo_commentCount_off) == 3 && long(info, PictInfo_uniqueComments_off) == 2)
            let comments = rd32(long(info, PictInfo_commentHandle_off))
            #expect(rd16(comments) == 2 && rd16(comments + 2) == 100)  // count, then ID
            #expect(long(info, PictInfo_uniqueFonts_off) == 1)
            let fonts = rd32(long(info, PictInfo_fontHandle_off))
            #expect(rd16(fonts + UInt32(FontSpec_pictFontID_off)) == 3)
            // Red, green and blue; the two most popular asked for, red first.
            #expect(long(info, PictInfo_uniqueColors_off) == 3)
            let ct = rd32(long(info, PictInfo_theColorTable_off))
            #expect(rd16(ct + 6) == 1)
            #expect(rd16(ct + 10) > 0xF000 && rd16(ct + 12) == 0 && rd16(ct + 14) == 0)
            // The picture's frame, and the caller's port untouched.
            #expect(rd16(info + PictInfo_sourceRect_off + 4) == 20)
        }

        @Test func getPictInfoChecksItsArguments() {
            launchTestApp()
            _ = initToolbox()
            let pic = picture()
            let info = tb_NewPtr(Int32(PictInfo_SIZEOF), false, true)
            #expect(tb_GetPictInfo(pic, info, 0, 0, 0, 1) == -11000)                       // pictInfoVersionErr
            #expect(tb_GetPictInfo(pic, info, Self.returnColorTable, 0, 0, 0) == -11004)   // colorsRequestedErr
            #expect(tb_GetPictInfo(pic, info, 0, 0, 7, 0) == -11003)                       // cantLoadPickMethodErr
            #expect(tb_GetPictInfo(0, info, 0, 0, 0, 0) == -11005)                         // pictureDataErr
            // Without the verbs, no handles.
            #expect(tb_GetPictInfo(pic, info, 0, 0, 0, 0) == 0)
            #expect(long(info, PictInfo_commentHandle_off) == 0 && long(info, PictInfo_theColorTable_off) == 0)
        }

        @Test func pixMapInfoAndRecordedSurveys() {
            launchTestApp()
            _ = initToolbox()
            let gw = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true)
            wr16(r + 4, 8)
            wr16(r + 6, 8)
            #expect(tb_NewGWorld(gw, 8, r, 0, 0, 0) == 0)
            let pm = tb_GetGWorldPixMap(rd32(gw))
            let info = tb_NewPtr(Int32(PictInfo_SIZEOF), false, true)
            #expect(tb_GetPixMapInfo(pm, info, Self.returnColorTable, 4, 2, 0) == 0)
            #expect(rd16(info + PictInfo_depth_off) == 8 && long(info, PictInfo_pixMapCount_off) == 1)
            #expect(long(info, PictInfo_uniqueColors_off) >= 1)

            // Two pictures and the pixel map surveyed together.
            let id = tb_NewPtr(4, false, true)
            #expect(tb_NewPictInfo(id, Self.recordComments, 0, 0, 0) == 0)
            let pic = picture()
            #expect(tb_RecordPictInfo(Int32(bitPattern: rd32(id)), pic) == 0)
            #expect(tb_RecordPictInfo(Int32(bitPattern: rd32(id)), pic) == 0)
            #expect(tb_RecordPixMapInfo(Int32(bitPattern: rd32(id)), pm) == 0)
            #expect(tb_RetrievePictInfo(Int32(bitPattern: rd32(id)), info, 0) == 0)
            #expect(long(info, PictInfo_rectCount_off) == 2 && long(info, PictInfo_pixMapCount_off) == 3)
            #expect(long(info, PictInfo_commentCount_off) == 6 && long(info, PictInfo_uniqueComments_off) == 2)
            #expect(tb_DisposePictInfo(Int32(bitPattern: rd32(id))) == 0)
            #expect(tb_RetrievePictInfo(Int32(bitPattern: rd32(id)), info, 0) == -11001)  // pictInfoIDErr
            tb_DisposeGWorld(rd32(gw))
        }
    }
}
