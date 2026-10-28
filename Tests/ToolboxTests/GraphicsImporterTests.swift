import BlueBoxHost
import CCore
import CPUPPC
import Foundation
import ImageIO
import Testing
import Toolbox
import UniformTypeIdentifiers

extension BlueBox {
    /// QuickTime's graphics importers: an image's size, description and
    /// drawing, through QuickTimeLib's GraphicsImport calls as a PowerPC
    /// program makes them (Super Snake's GIFs).
    @Suite struct GraphicsImporter {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        @discardableResult
        func call(_ name: String, _ args: [UInt32]) -> UInt32 {
            withUnsafeMutableBytes(of: &g_ppc.gpr) { gpr in
                for (i, a) in args.enumerated() { gpr.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            }
            guard let fn = cfm_export_fn(name) else {
                Issue.record("no export \(name)")
                return 0
            }
            fn(&g_ppc)
            return withUnsafeBytes(of: g_ppc.gpr) { $0.load(fromByteOffset: 12, as: UInt32.self) }
        }

        /// A 4x2 PNG: the left half red, the right half blue.
        func png() -> [UInt8] {
            var px = [UInt8]()
            for _ in 0..<2 { px += [255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255] }
            let provider = CGDataProvider(data: Data(px) as CFData)!
            let image = CGImage(width: 4, height: 2, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: 16,
                                space: CGColorSpaceCreateDeviceRGB(),
                                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
            let out = NSMutableData()
            let dest = CGImageDestinationCreateWithData(out as CFMutableData, UTType.png.identifier as CFString, 1, nil)!
            CGImageDestinationAddImage(dest, image, nil)
            CGImageDestinationFinalize(dest)
            return [UInt8](out as Data)
        }

        @Test func importsAndDrawsAnImage() {
            launchTestApp()
            _ = initToolbox()
            _ = cfm_ppc_cpu()
            ClassicImages.install()
            let bytes = png()
            let h = tb_NewHandle(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(rd32(h) + UInt32(i), b) }
            let ref = tb_NewHandle(4, false, false)
            wr32(rd32(ref), h)
            let gi = tb_NewPtr(4, false, true)
            #expect(tb_GetGraphicsImporterForDataRef(Int32(bitPattern: ref), Int32(bitPattern: fourCC("hndl")), gi) == 0)
            let ci = rd32(gi)
            #expect(ci != 0)
            let r = rect(0, 0, 0, 0)
            #expect(call("GraphicsImportGetNaturalBounds", [ci, r]) == 0)
            #expect(rd16(r + 4) == 2 && rd16(r + 6) == 4)
            let desc = tb_NewPtr(4, false, true)
            #expect(call("GraphicsImportGetImageDescription", [ci, desc]) == 0)
            let d = rd32(rd32(desc))
            #expect(rd32(d + 4) == fourCC("png ") && rd16(d + 32) == 4 && rd16(d + 34) == 2)

            // Twice the size, into a GWorld of its own.
            let gw = tb_NewPtr(4, false, true)
            #expect(tb_NewGWorld(gw, 32, rect(0, 0, 4, 8), 0, 0, 0) == 0)
            #expect(call("GraphicsImportSetGWorld", [ci, rd32(gw), 0]) == 0)
            #expect(call("GraphicsImportSetBoundsRect", [ci, rect(0, 0, 4, 8)]) == 0)
            #expect(call("GraphicsImportDraw", [ci]) == 0)
            let pmh = tb_GetGWorldPixMap(rd32(gw))
            let base = tb_GetPixBaseAddr(pmh), row = UInt32(rd16(rd32(pmh) + 4) & 0x3FFF)
            #expect(rd32(base + row * 3 + 4) & 0x00FF_FFFF == 0x00FF_0000)    // red at (2, 3)
            #expect(rd32(base + row * 3 + 28) & 0x00FF_FFFF == 0x0000_00FF)   // blue at (7, 3)
            let back = rect(0, 0, 0, 0)
            #expect(call("GraphicsImportGetBoundsRect", [ci, back]) == 0 && rd16(back + 6) == 8)
            // no exporter for no type (the exporters' own tests: QTCodecComponentTests)
            #expect(Int16(truncatingIfNeeded: call("GraphicsImportExportImageFile", [ci, 0, 0, 0, 0])) == -2003)
            components_close(ci)

            // Not an image: no importer.
            let junk = tb_NewHandle(16, false, true)
            wr32(rd32(ref), junk)
            #expect(tb_GetGraphicsImporterForDataRef(Int32(bitPattern: ref), Int32(bitPattern: fourCC("hndl")), gi) == -2003)
            #expect(rd32(gi) == 0)
        }
    }
}
