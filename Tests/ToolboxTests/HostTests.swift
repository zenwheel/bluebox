import BlueBoxHost
import Foundation
import Testing
#if canImport(AppKit)
import AppKit
import Carbon
#endif

@Suite struct FinderIcons {
    /// An application bundle: BNDL maps local 0 to ICN# 128; FREF 128 is APPL, 129 is TEXT.
    static func fork() -> [UInt8] {
        let bndl = cat(Array("TEST".utf8), be16(0), be16(1),
                       Array("ICN#".utf8), be16(0), be16(0), be16(128),
                       Array("FREF".utf8), be16(1), be16(0), be16(128), be16(1), be16(129))
        let frefApp = cat(Array("APPL".utf8), be16(0), [0])
        let frefDoc = cat(Array("TEXT".utf8), be16(1), [0])
        // ICN#: black square outline icon; mask covers the top half only.
        var icon = [UInt8](repeating: 0, count: 128)
        for i in 0..<128 { icon[i] = 0xFF }
        var mask = [UInt8](repeating: 0, count: 128)
        for i in 0..<64 { mask[i] = 0xFF }
        // icl8: every pixel colour 5 (a light blue in the system palette).
        let icl8 = [UInt8](repeating: 5, count: 1024)
        return buildFork([
            TestResource(type: "BNDL", id: 128, data: bndl),
            TestResource(type: "FREF", id: 128, data: frefApp),
            TestResource(type: "FREF", id: 129, data: frefDoc),
            TestResource(type: "ICN#", id: 128, data: icon + mask),
            TestResource(type: "icl8", id: 128, data: icl8),
        ])
    }

    @Test func bundleGivesColourIconAndDocumentTypes() throws {
        let bytes = Self.fork()
        let fork = try #require(bytes.withUnsafeBufferPointer { ResourceFork(fork: $0.baseAddress, length: $0.count) })
        let bundle = finderBundle(fork: fork, creator: fourCC("TEST"))
        #expect(bundle.documentTypes == [fourCC("TEXT")])
        let icon = try #require(bundle.appIcons.first)
        #expect(icon.size == 32)
        let top = Array(icon.rgba[0..<4]), bottom = Array(icon.rgba[(31 * 32) * 4..<(31 * 32) * 4 + 4])
        let (r, g, b) = macPalette8(5)
        #expect(top == [r, g, b, 255])       // inside the mask: icl8 colour
        #expect(bottom[3] == 0)              // outside the mask: transparent
    }

    @Test func standardPalettes() {
        #expect(macPalette8(0) == (255, 255, 255))
        #expect(macPalette8(255) == (0, 0, 0))
        #expect(macPalette8(215) == (0xEE, 0, 0))
        #expect(macPalette8(254) == (0x11, 0x11, 0x11))
        #expect(macPalette4[15] == (0, 0, 0))
    }

    @Test func scalingIsNearestNeighbour() {
        var img = IconImage(size: 2, rgba: [UInt8](repeating: 0, count: 16))
        img.rgba[0..<4] = [255, 0, 0, 255]
        let big = scaled(img, to: 8)
        #expect(Array(big.rgba[0..<4]) == [255, 0, 0, 255])
        #expect(Array(big.rgba[(3 * 8 + 3) * 4..<(3 * 8 + 3) * 4 + 4]) == [255, 0, 0, 255])
        #expect(big.rgba[(4 * 8 + 4) * 4 + 3] == 0)
    }

    #if canImport(AppKit)
    @Test func icnsHoldsEveryFinderSize() throws {
        let bytes = Self.fork()
        let fork = try #require(bytes.withUnsafeBufferPointer { ResourceFork(fork: $0.baseAddress, length: $0.count) })
        let data = try #require(icnsData(finderBundle(fork: fork, creator: fourCC("TEST")).appIcons))
        let b = [UInt8](data)
        #expect(Array(b[0..<4]) == Array("icns".utf8))
        #expect(Int(UInt32(b[4]) << 24 | UInt32(b[5]) << 16 | UInt32(b[6]) << 8 | UInt32(b[7])) == b.count)
        var types: [String] = []
        var o = 8
        while o + 8 <= b.count {
            types.append(String(decoding: b[o..<o + 4], as: UTF8.self))
            o += Int(UInt32(b[o + 4]) << 24 | UInt32(b[o + 5]) << 16 | UInt32(b[o + 6]) << 8 | UInt32(b[o + 7]))
        }
        #expect(o == b.count)
        for t in ["ic07", "ic08", "ic09", "ic10", "ic11", "ic12", "ic13", "ic14"] { #expect(types.contains(t)) }
    }
    #endif
}

#if canImport(AppKit)
/// Dead keys compose through the keyboard layout (needs the US or ABC layout).
@MainActor @Suite struct Keyboard {
    nonisolated static var usLayout: Bool {
        guard let src = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
              let ptr = TISGetInputSourceProperty(src, kTISPropertyInputSourceID) else { return false }
        let id = Unmanaged<CFString>.fromOpaque(ptr).takeUnretainedValue() as String
        return id.hasSuffix(".US") || id.hasSuffix(".ABC")
    }

    func key(_ code: UInt16, _ flags: NSEvent.ModifierFlags = []) -> NSEvent {
        NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0, windowNumber: 0,
                         context: nil, characters: "", charactersIgnoringModifiers: "", isARepeat: false,
                         keyCode: code)!
    }

    @Test(.enabled(if: usLayout)) func optionEThenEMakesEAcute() {
        let t = KeyTranslator()
        #expect(t.translate(key(14, .option)) == nil)       // Option-E waits
        #expect(t.translate(key(14)) == "é")
        #expect(t.translate(key(32, .option)) == nil)       // Option-U
        #expect(t.translate(key(34)) == "ï")                // I
        #expect(t.translate(key(14)) == "e")                // plain again
    }

    @Test(.enabled(if: usLayout)) func deadKeyThenSpaceGivesTheAccent() {
        let t = KeyTranslator()
        #expect(t.translate(key(14, .option)) == nil)
        #expect(t.translate(key(49)) == "´")
    }
}
#endif

#if canImport(AppKit)
import PDFKit
import Toolbox

/// The Printing Manager records a page; the host renders it as PDF.
extension BlueBox {
    @Suite struct Printing {
        nonisolated(unsafe) static var ops: [(kind: Int32, verb: Int32, text: String, font: Int16, rect: mprect)] = []
        nonisolated(unsafe) static var pdf = Data()
        nonisolated(unsafe) static var pages = 0

        func printPage(_ draw: () -> Void) {
            Self.ops = []
            Self.pdf = Data()
            Self.pages = 0
            machine_set_print_handlers({ job, _ in
                guard let job else { return false }
                Printing.pages = Int(job.pointee.npages)
                let page = job.pointee.pages[0]
                for i in 0..<Int(page.nops) {
                    let op = page.ops[i]
                    let text = op.len > 0 ? String(decoding: UnsafeBufferPointer(start: op.text, count: Int(op.len)), as: UTF8.self) : ""
                    Printing.ops.append((op.kind, op.verb, text, op.font_id, op.r))
                }
                Printing.pdf = ClassicPrint.pdf(job)
                return true
            }, nil, nil)
            defer { machine_set_print_handlers(nil, nil, nil) }
            let h = tb_NewHandle(120, false, true)
            tb_PrOpen()
            tb_PrintDefault(h)
            let port = tb_PrOpenDoc(h, 0, 0)
            #expect(port != 0)
            tb_SetPort(port)
            tb_PrOpenPage(port, 0)
            draw()
            tb_PrClosePage(port)
            tb_PrCloseDoc(port)
            #expect(tb_PrError() == 0)
        }

        @Test func pagesAreRecordedAsShapesAndText() throws {
            launchTestApp()
            _ = initToolbox()
            let r = tb_NewPtr(8, false, true)
            printPage {
                tb_TextFont(3)
                tb_TextSize(12)
                tb_MoveTo(72, 100)
                tb_DrawString(guestString("Hello, printer"))
                qrectPut(r, 120, 72, 200, 300)
                tb_FrameRect(r)
                tb_MoveTo(72, 220)
                tb_LineTo(300, 220)
            }
            #expect(Self.pages == 1)
            let text = try #require(Self.ops.first { $0.kind == Int32(MACHINE_PRINT_TEXT) })
            #expect(text.text == "Hello, printer")
            #expect(text.font == 3)
            let rect = try #require(Self.ops.first { $0.kind == Int32(MACHINE_PRINT_RECT) })
            #expect(rect.verb == Int32(MACHINE_VERB_FRAME))
            #expect(rect.rect.top == 120 && rect.rect.left == 72 && rect.rect.bottom == 200 && rect.rect.right == 300)
            #expect(Self.ops.contains { $0.kind == Int32(MACHINE_PRINT_LINE) })

            let doc = try #require(PDFDocument(data: Self.pdf))
            #expect(doc.pageCount == 1)
            #expect(doc.page(at: 0)?.string?.contains("Hello, printer") == true) // real text, not pixels
        }

        @Test func pageSetupChangesThePrintRecord() {
            launchTestApp()
            machine_set_print_handlers(nil, { ps, _ in
                ps?.pointee = machine_page_setup(paper_width: 792, paper_height: 612, left: 18, top: 18,
                                                 right: 774, bottom: 594, landscape: true)
                return true
            }, nil)
            defer { machine_set_print_handlers(nil, nil, nil) }
            let h = tb_NewHandle(120, false, true)
            tb_PrintDefault(h)
            #expect(tb_PrStlDialog(h) != 0)
            let p = rd32(h)
            let page = (top: Int16(bitPattern: rd16(p + 8)), left: Int16(bitPattern: rd16(p + 10)),
                        bottom: Int16(bitPattern: rd16(p + 12)), right: Int16(bitPattern: rd16(p + 14)))
            #expect(page.right - page.left == 756)                // landscape printable width
            #expect(page.bottom - page.top == 576)
            #expect(Int16(bitPattern: rd16(p + 16)) == -18)      // rPaper starts at the margin
        }
    }
}

private func guestString(_ s: String) -> UInt32 {
    let p = tb_NewPtr(256, false, true)
    guestPString(s, at: p)
    return p
}

private func qrectPut(_ r: UInt32, _ top: Int, _ left: Int, _ bottom: Int, _ right: Int) {
    for (i, v) in [top, left, bottom, right].enumerated() { wr16(r + UInt32(i * 2), UInt16(v)) }
}
#endif
