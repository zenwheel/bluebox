#if canImport(AppKit)
import Testing
import Toolbox

/// QuickDraw GX printing: a job's page (a rectangle and a converted
/// QuickDraw picture) printed through the Printing Manager, and the
/// Collection Manager that jobs keep settings in.
extension BlueBox {
    @Suite struct GX {
        nonisolated(unsafe) static var kinds: [Int32] = []

        func be32(_ v: Int32) -> [UInt8] { (0..<4).map { UInt8(UInt32(bitPattern: v) >> (24 - 8 * UInt32($0)) & 0xFF) } }

        @Test func jobsPrintShapes() {
            launchTestApp()
            _ = initToolbox()
            machine_set_print_handlers({ job, _ in
                guard let job else { return false }
                let page = job.pointee.pages[0]
                GX.kinds = (0..<Int(page.nops)).map { page.ops[$0].kind }
                return true
            }, nil, nil)
            defer { machine_set_print_handlers(nil, nil, nil) }
            _ = tb_GXNewGraphicsClient(0, 0, 0)
            tb_GXEnterGraphics()
            #expect(tb_GXInitPrinting() == 0)
            let jp = tb_NewPtr(4, false, true)
            #expect(tb_GXNewJob(jp) == 0)
            let job = Int32(bitPattern: rd32(jp))
            // The format: US Letter less margins, in points.
            let page = tb_NewPtr(16, false, true), paper = tb_NewPtr(16, false, true)
            tb_GXGetFormatDimensions(tb_GXGetJobFormat(job, 1), page, paper)
            #expect(Int32(bitPattern: rd32(paper + 8)) >> 16 == 612 && Int32(bitPattern: rd32(paper + 12)) >> 16 == 792)
            // A rectangle (filled) and a picture of an oval, as the page's parts.
            let r = tb_NewPtr(16, false, true)
            for (i, v) in [72, 72, 216, 144].enumerated() { wr32(r + UInt32(4 * i), UInt32(v << 16)) }
            let rect = tb_GXNewRectangle(r)
            tb_GXSetShapeFill(rect, 3)
            let frame = tb_NewPtr(8, false, true)
            for (i, v) in [0, 0, 50, 80].enumerated() { wr16(frame + UInt32(2 * i), UInt16(v)) }
            let pic = tb_OpenPicture(frame)
            tb_FrameOval(frame)
            tb_ClosePicture()
            let oval = tb_GXConvertPICTToShape(pic, 0, 0, 0, 0, 0, 0)
            #expect(tb_GXGetShapeType(oval) == 13)
            tb_GXMoveShape(oval, 100 << 16, 300 << 16)
            let pageShape = tb_GXNewShape(13)
            let parts = tb_NewPtr(8, false, true)
            wr32(parts, UInt32(bitPattern: rect))
            wr32(parts + 4, UInt32(bitPattern: oval))
            tb_GXSetPictureParts(pageShape, 0, 0, 2, parts, 0, 0, 0)
            tb_GXDisposeShape(rect)                              // the picture keeps its parts
            tb_GXDisposeShape(oval)
            #expect(tb_GXJobPrintDialog(job, 0) == 1)
            tb_GXStartJob(job, 0, 1)
            tb_GXPrintPage(job, 1, tb_GXGetJobFormat(job, 1), pageShape)
            tb_GXFinishJob(job)
            #expect(GX.kinds.contains(Int32(MACHINE_PRINT_RECT)))
            #expect(GX.kinds.contains(Int32(MACHINE_PRINT_OVAL)))
            // Flattened and back.
            let h = tb_GXFlattenJobToHdl(job, 0)
            #expect(h != 0 && tb_GetHandleSize(h) >= 124)
            #expect(tb_GXUnflattenJobFromHdl(job, h) == job)
            tb_GXDisposeShape(pageShape)
            #expect(tb_GXDisposeJob(job) == 0)
        }

        @Test func collectionsKeepItems() {
            launchTestApp()
            _ = initToolbox()
            let c = tb_NewCollection()
            let data = tb_NewPtr(8, false, true)
            wr32(data, 0x1234_5678)
            #expect(tb_AddCollectionItem(c, 0x7465_7374, 1, 4, data) == 0)
            #expect(tb_CountCollectionItems(c) == 1)
            let size = tb_NewPtr(4, false, true), out = tb_NewPtr(8, false, true)
            wr32(size, 8)
            #expect(tb_GetCollectionItem(c, 0x7465_7374, 1, size, out) == 0)
            #expect(rd32(size) == 4 && rd32(out) == 0x1234_5678)
            #expect(tb_GetCollectionItem(c, 0x7465_7374, 2, size, out) == -5752)  // not found
            wr32(data, 7)
            #expect(tb_ReplaceIndexedCollectionItem(c, 1, 4, data) == 0)
            wr32(size, 4)
            #expect(tb_GetIndexedCollectionItem(c, 1, size, out) == 0 && rd32(out) == 7)
            #expect(tb_RemoveCollectionItem(c, 0x7465_7374, 1) == 0 && tb_CountCollectionItems(c) == 0)
            tb_DisposeCollection(c)
        }
    }
}
#endif
