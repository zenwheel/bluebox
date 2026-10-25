import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// The Carbon Printing Manager on the Printing Manager's port and the
    /// host's printing.
    @Suite struct CarbonPrinting {
        nonisolated(unsafe) static var pages = 0
        nonisolated(unsafe) static var page = mprect()
        nonisolated(unsafe) static var res = (0, 0)
        nonisolated(unsafe) static var ops: [Int] = []

        @Test func sessionPrintsPages() {
            launchTestApp()
            _ = initToolbox()
            machine_set_print_handlers({ job, _ in
                guard let job else { return false }
                CarbonPrinting.pages = Int(job.pointee.npages)
                CarbonPrinting.page = job.pointee.page
                CarbonPrinting.res = (Int(job.pointee.hres), Int(job.pointee.vres))
                CarbonPrinting.ops = (0..<Int(job.pointee.npages)).map { Int(job.pointee.pages[$0].nops) }
                return true
            }, nil, nil)
            defer { machine_set_print_handlers(nil, nil, nil) }
            let v = svcPtr(64)
            #expect(svcCall("PMCreateSession", v) == 0)
            let session = rd32(v)
            #expect(svcCall("PMCreatePageFormat", v) == 0)
            let format = rd32(v)
            #expect(svcCall("PMCreatePrintSettings", v) == 0)
            let settings = rd32(v)
            #expect(svcCall("PMSessionDefaultPageFormat", session, format) == 0)
            #expect(svcCall("PMSessionDefaultPrintSettings", session, settings) == 0)
            #expect(svcCall("PMSessionValidatePageFormat", session, format, v) == 0 && rd8(v) == 0)

            // US Letter less quarter-inch margins, in points.
            #expect(svcCall("PMGetAdjustedPageRect", format, v) == 0)
            #expect(svcDouble(v + 16) == 756 && svcDouble(v + 24) == 576)
            #expect(svcCall("PMGetAdjustedPaperRect", format, v) == 0)
            #expect(svcDouble(v) == -18 && svcDouble(v + 24) == 594)
            #expect(svcCall("PMGetPhysicalPaperSize", format, v) == 0 && svcDouble(v + 16) == 792 && svcDouble(v + 24) == 612)
            #expect(svcCall("PMGetOrientation", format, v) == 0 && rd16(v) == 1)

            // At 50%, the page is twice as big in the document's coordinates.
            #expect(svcCall("PMSetScale", format, f1: 50) == 0)
            #expect(svcCall("PMGetScale", format, v) == 0 && svcDouble(v) == 50)
            #expect(svcCall("PMGetAdjustedPageRect", format, v) == 0 && svcDouble(v + 24) == 1152)
            #expect(svcCall("PMSetScale", format, f1: 100) == 0)

            // Settings.
            #expect(svcCall("PMSetCopies", settings, 2, 0) == 0)
            #expect(svcCall("PMGetCopies", settings, v) == 0 && rd32(v) == 2)
            #expect(svcCall("PMSetPageRange", settings, 1, 3) == 0)
            #expect(svcCall("PMGetFirstPage", settings, v) == 0 && rd32(v) == 1)
            #expect(svcCall("PMGetLastPage", settings, v) == 0 && rd32(v) == 3)
            #expect(svcCall("PMSetJobNameCFString", settings, svcCFString("Report")) == 0)
            #expect(svcCall("PMGetJobNameCFString", settings, v) == 0 && svcString(rd32(v)) == "Report")

            // The print dialog says yes (the host's panel comes at the end).
            #expect(svcCall("PMSessionPrintDialog", session, settings, format, v) == 0 && rd8(v) == 1)

            // Two pages drawn.
            #expect(svcCall("PMSessionBeginDocument", session, settings, format) == 0)
            #expect(svcStatus(svcCall("PMSessionBeginDocument", session, settings, format)) == -30871)
            for _ in 0..<2 {
                #expect(svcCall("PMSessionBeginPage", session, format, 0) == 0)
                #expect(svcCall("PMSessionGetGraphicsContext", session, 0, v) == 0)
                let port = rd32(v)
                #expect(port != 0 && port == qd_port())
                tb_PaintRect(svcRect(10, 10, 50, 50))
                #expect(svcCall("PMSessionEndPage", session) == 0)
            }
            #expect(svcCall("PMSessionEndDocument", session) == 0)
            #expect(Self.pages == 2 && Self.ops.allSatisfy { $0 > 0 })
            #expect(Self.page.bottom == 756 && Self.page.right == 576 && Self.res == (72, 72))
            #expect(svcCall("PMSessionError", session) == 0)
            #expect(svcStatus(svcCall("PMSessionEndDocument", session)) == -30871)
            #expect(svcCall("PMRelease", session) == 0)
        }

        @Test func formatsFlattenAndConvert() {
            launchTestApp()
            let v = svcPtr(64), h = svcPtr(4)
            svcCall("PMCreatePageFormat", v)
            let format = rd32(v)
            svcCall("PMCreatePrintSettings", v)
            let settings = rd32(v)
            #expect(svcCall("PMSetOrientation", format, 2, 0) == 0)
            #expect(svcCall("PMGetOrientation", format, v) == 0 && rd16(v) == 2)
            #expect(svcCall("PMGetPhysicalPaperSize", format, v) == 0 && svcDouble(v + 24) == 792)
            let res = svcPtr(16)
            wr32(res, 0x4072_C000); wr32(res + 4, 0) // 300.0
            wr32(res + 8, 0x4072_C000); wr32(res + 12, 0)
            #expect(svcCall("PMSetResolution", format, res) == 0)
            #expect(svcCall("PMGetResolution", format, v) == 0 && svcDouble(v) == 300 && svcDouble(v + 8) == 300)
            #expect(svcCall("PMSetJobName", settings, svcPStr("Job")) == 0)
            let ext = svcPStr("extra")
            #expect(svcCall("PMSetPageFormatExtendedData", format, fourCC("mine"), 6, ext) == 0)

            // Flattened and back.
            #expect(svcCall("PMFlattenPageFormat", format, h) == 0)
            #expect(svcCall("PMUnflattenPageFormat", rd32(h), v) == 0)
            let copy = rd32(v)
            #expect(svcCall("PMGetOrientation", copy, v) == 0 && rd16(v) == 2)
            let size = svcPtr(4), out = svcPtr(16)
            wr32(size, 16)
            #expect(svcCall("PMGetPageFormatExtendedData", copy, fourCC("mine"), size, out) == 0)
            #expect(rd32(size) == 6 && pstring(at: out) == "extra")
            #expect(svcStatus(svcCall("PMGetPageFormatExtendedData", copy, fourCC("none"), size, out)) == -30874)
            #expect(svcCall("PMFlattenPrintSettings", settings, h) == 0)
            #expect(svcCall("PMUnflattenPrintSettings", rd32(h), v) == 0)
            let name = svcPtr(256)
            #expect(svcCall("PMGetJobName", rd32(v), name) == 0 && pstring(at: name) == "Job")
            #expect(svcStatus(svcCall("PMUnflattenPageFormat", rd32(h), v)) == -30876) // settings aren't a format

            // An old print record, both ways.
            #expect(svcCall("PMMakeOldPrintRecord", settings, format, h) == 0)
            let record = rd32(h)
            #expect(rd16(rd32(record) + 4) == 300 && rd16(rd32(record) + 6) == 300) // iVRes, iHRes
            let ps = svcPtr(4), pf = svcPtr(4)
            #expect(svcCall("PMConvertOldPrintRecord", record, ps, pf) == 0)
            #expect(svcCall("PMGetResolution", rd32(pf), v) == 0 && svcDouble(v) == 300)

            // The printer: not PostScript, LaserWriter resolutions.
            #expect(svcCall("PMIsPostScriptDriver", v) == 0 && rd8(v) == 0)
            #expect(svcCall("PMGetPrinterResolutionCount", v) == 0 && rd32(v) == 4)
            #expect(svcCall("PMGetIndexedPrinterResolution", 4, v) == 0 && svcDouble(v) == 600)
            #expect(svcStatus(svcCall("PMPostScriptBegin")) == -30873)
            #expect(svcCall("PMRelease", copy) == 0 && svcCall("PMRelease", format) == 0)
            #expect(svcStatus(svcCall("PMRelease", format)) == -50)
        }

        /// The dialog Init calls give the classic TPrDlg, whose fields the
        /// PMDialog accessors reach.
        @Test func dialogInitAndAccessors() {
            launchTestApp()
            _ = initToolbox()
            _ = tb_InitDialogs(0)
            let v = svcPtr(16)
            svcCall("PMCreateSession", v)
            let session = rd32(v)
            svcCall("PMCreatePrintSettings", v)
            let settings = rd32(v)
            svcCall("PMCreatePageFormat", v)
            let format = rd32(v)
            #expect(svcCall("PMSessionPrintDialogInit", session, settings, format, v) == 0)
            let dialog = rd32(v)
            #expect(dialog != 0)
            #expect(svcCall("PMGetDialogPtr", dialog, v) == 0 && rd32(v) == dialog)
            #expect(svcCall("PMGetItemProc", dialog, v) == 0 && rd16(rd32(v)) == 0xAAFE) // a UPP for our 68k code
            #expect(svcCall("PMSetDialogAccepted", dialog, 1) == 0)
            #expect(svcCall("PMGetDialogAccepted", dialog, v) == 0 && rd8(v) == 1)
            #expect(svcCall("PMSetDialogDone", dialog, 1) == 0)
            #expect(svcCall("PMGetDialogDone", dialog, v) == 0 && rd8(v) == 1)
            #expect(svcCall("PMSetModalFilterProc", dialog, 0x1234) == 0)
            #expect(svcCall("PMGetModalFilterProc", dialog, v) == 0 && rd32(v) == 0x1234)
            #expect(svcCall("PMSessionPageSetupDialogInit", session, format, v) == 0 && rd32(v) != 0)
            #expect(svcStatus(svcCall("PMSessionUseSheets", session, 0, 0)) == -30873)
        }

        /// The CarbonLib 1.0 calls, without a session.
        @Test func sessionlessDocument() {
            launchTestApp()
            _ = initToolbox()
            machine_set_print_handlers({ job, _ in
                CarbonPrinting.pages = Int(job?.pointee.npages ?? 0)
                return true
            }, nil, nil)
            defer { machine_set_print_handlers(nil, nil, nil) }
            let v = svcPtr(16)
            #expect(svcCall("PMBegin") == 0)
            svcCall("PMNewPageFormat", v)
            let format = rd32(v)
            svcCall("PMDefaultPageFormat", format)
            svcCall("PMNewPrintSettings", v)
            let settings = rd32(v)
            svcCall("PMDefaultPrintSettings", settings)
            #expect(svcCall("PMPrintDialog", settings, format, v) == 0 && rd8(v) == 1)
            #expect(svcCall("PMBeginDocument", settings, format, v) == 0)
            let context = rd32(v)
            #expect(svcCall("PMBeginPage", context, 0) == 0)
            #expect(svcCall("PMGetGrafPtr", context, v) == 0 && rd32(v) != 0)
            #expect(svcCall("PMEndPage", context) == 0)
            Self.pages = 0
            #expect(svcCall("PMEndDocument", context) == 0)
            #expect(Self.pages == 1)
            #expect(svcCall("PMError") == 0)
            svcCall("PMDisposePageFormat", format)
            svcCall("PMDisposePrintSettings", settings)
            #expect(svcCall("PMEnd") == 0)
        }
    }
}
