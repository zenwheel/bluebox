import CCore
import CPUPPC
import Testing
import Toolbox

/// Helpers for QuickDraw 3D's and RAVE's exports, called as a PowerPC
/// caller would: `args` the general-register slots (a float takes one),
/// `fp` the floating-point registers.
struct Q3Call {
    @discardableResult
    static func call(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> UInt32 {
        // r1 well inside its block: a callee pushes its frames below it, the
        // parameter area is above
        let block = tb_NewPtr(4096, false, true), stack = block + 3072
        withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: stack, toByteOffset: 4, as: UInt32.self) }
        for (i, a) in args.enumerated() {
            if i < 8 {
                withUnsafeMutableBytes(of: &g_ppc.gpr) { $0.storeBytes(of: a, toByteOffset: (3 + i) * 4, as: UInt32.self) }
            } else {
                wr32(stack + 24 + UInt32(4 * i), a)
            }
        }
        for (i, f) in fp.enumerated() { ppc_set_fpr(&g_ppc, Int32(1 + i), f) }
        guard let fn = cfm_export_fn(name) else {
            Issue.record("\(name) is not exported")
            return 0xFFFF_FFFF
        }
        fn(&g_ppc)
        tb_DisposePtr(block)
        return g_ppc.gpr.3
    }

    /// The float a call returned (in f1).
    static var f1: Double { ppc_fpr(&g_ppc, 1) }

    static func words(_ values: [UInt32]) -> UInt32 {
        let p = tb_NewPtr(Int32(values.count * 4 + 4), false, true)
        for (i, v) in values.enumerated() { wr32(p + UInt32(4 * i), v) }
        return p
    }

    static func floats(_ values: [Float]) -> UInt32 { words(values.map { $0.bitPattern }) }

    static func float(_ a: UInt32) -> Float { Float(bitPattern: rd32(a)) }
}

extension BlueBox {
    /// RAVE (QuickDraw™ 3D Accelerator) on the host's renderer: the one
    /// engine, a draw context on a memory device, its method table.
    @Suite struct RAVE {
        typealias Q = Q3Call

        /// A 32-bit memory device of w by h and a draw context covering it.
        func context(_ w: Int, _ h: Int, flags: UInt32 = 0) -> (dc: UInt32, pixels: UInt32) {
            let pixels = tb_NewPtr(Int32(w * h * 4), false, true)
            // TQADevice: memory, rowBytes, kQAPixel_RGB32, width, height, baseAddr
            let dev = Q.words([0, UInt32(w * 4), 3, UInt32(w), UInt32(h), pixels])
            let engine = Q.call("QADeviceGetFirstEngine", [dev])
            #expect(engine != 0)
            let rect = Q.words([0, UInt32(w), 0, UInt32(h)]) // left, right, top, bottom
            let out = Q.words([0])
            #expect(Q.call("QADrawContextNew", [dev, rect, 0, engine, flags, out]) == 0)
            let dc = rd32(out)
            #expect(dc != 0)
            return (dc, pixels)
        }

        /// A method of the draw context (TQADrawMethodTag order).
        func method(_ dc: UInt32, _ tag: UInt32) -> UInt32 { rd32(dc + 8 + 4 * tag) }

        func px(_ pixels: UInt32, _ w: Int, _ x: Int, _ y: Int) -> UInt32 { rd32(pixels + UInt32((y * w + x) * 4)) & 0xFFFFFF }

        @Test func engineGestalt() {
            launchTestApp()
            let dev = Q.words([0, 64, 3, 16, 16, tb_NewPtr(1024, false, true)])
            let engine = Q.call("QADeviceGetFirstEngine", [dev])
            #expect(engine != 0)
            // Apple's software engine after it, fast at nothing.
            let sw = Q.call("QADeviceGetNextEngine", [dev, engine])
            #expect(sw != 0 && sw != engine)
            #expect(Q.call("QADeviceGetNextEngine", [dev, sw]) == 0)
            let fast = Q.words([0])
            #expect(Q.call("QAEngineGestalt", [sw, 1 /* FastFeatures */, fast]) == 0 && rd32(fast) == 0)
            #expect(Q.call("QAEngineGestalt", [engine, 1, fast]) == 0 && rd32(fast) & 4 != 0)
            #expect(Q.call("QADeviceGetFirstEngine", [0]) == engine) // any device
            #expect(Q.call("QAEngineCheckDevice", [engine, dev]) == 0)
            let r = Q.words([0, 0])
            #expect(Q.call("QAEngineGestalt", [engine, 0 /* OptionalFeatures */, r]) == 0)
            #expect(rd32(r) & 2 != 0) // kQAOptional_Texture
            #expect(Q.call("QAEngineGestalt", [engine, 5 /* ASCIINameLength */, r]) == 0)
            #expect(rd32(r) > 0)
            #expect(Q.call("QAEngineGestalt", [engine, 999, r]) == 6) // kQAGestaltUnknown
            // A CL8 device isn't one we can draw.
            let cl8 = Q.words([0, 16, 6, 16, 16, tb_NewPtr(256, false, true)])
            #expect(Q.call("QADeviceGetFirstEngine", [cl8]) == 0)
        }

        /// A cleared frame with a Gouraud triangle, depth tested against
        /// a nearer one, appears in the device only at QARenderEnd.
        @Test func drawsTrianglesIntoMemory() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let (dc, pixels) = context(32, 32)
            #expect(rd32(dc + 4) == 4) // kQAVersion_1_6
            // Background blue.
            for (tag, v) in [(1, 1.0), (2, 0.0), (3, 0.0), (4, 1.0)] {
                ppc_set_fpr(&g_ppc, 1, v)
                _ = ppc_call(method(dc, 0), [dc, UInt32(tag)], 2) // setFloat
            }
            _ = ppc_call(method(dc, 13), [dc, 0, 0], 3) // renderStart
            // A red triangle over the left half at z 0.5, then a green one
            // behind it (z 0.9) over everything: it shows only on the right.
            let red = Q.floats([0, 0, 0.5, 1, 1, 0, 0, 1,  32, 0, 0.5, 1, 1, 0, 0, 1,  0, 32, 0.5, 1, 1, 0, 0, 1])
            _ = ppc_call(method(dc, 8), [dc, red, red + 32, red + 64, 0], 5) // drawTriGouraud
            let green = Q.floats([-40, -40, 0.9, 1, 0, 1, 0, 1,  100, -40, 0.9, 1, 0, 1, 0, 1,  -40, 100, 0.9, 1, 0, 1, 0, 1])
            _ = ppc_call(method(dc, 8), [dc, green, green + 32, green + 64, 0], 5)
            #expect(px(pixels, 32, 2, 2) == 0) // nothing yet
            #expect(ppc_call(method(dc, 14), [dc, 0], 2) == 0) // renderEnd
            #expect(px(pixels, 32, 2, 2) == 0xFF0000)
            #expect(px(pixels, 32, 20, 20) == 0x00FF00)
            // Depth off: the green one drawn over everything.
            _ = ppc_call(method(dc, 1), [dc, 0, 7], 3) // setInt ZFunction True
            #expect(ppc_call(method(dc, 4), [dc, 0], 2) == 7) // getInt
            _ = ppc_call(method(dc, 13), [dc, 0, 0], 3)
            _ = ppc_call(method(dc, 8), [dc, red, red + 32, red + 64, 0], 5)
            _ = ppc_call(method(dc, 8), [dc, green, green + 32, green + 64, 0], 5)
            _ = ppc_call(method(dc, 14), [dc, 0], 2)
            #expect(px(pixels, 32, 2, 2) == 0x00FF00)
            Q.call("QADrawContextDelete", [dc])
        }

        /// A 2 by 2 texture over the whole context, nearest-filtered:
        /// each quarter one texel, modulated by kd.
        @Test func texturedTriangles() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let (dc, pixels) = context(16, 16)
            let engine = Q.call("QADeviceGetFirstEngine", [Q.words([0, 64, 3, 16, 16, pixels])])
            let image = Q.words([0xFFFF_0000, 0xFF00_FF00, 0xFF00_00FF, 0xFFFF_FFFF]) // ARGB32, top row first
            let images = Q.words([2, 2, 8, image])
            let out = Q.words([0])
            #expect(Q.call("QATextureNew", [engine, 0, 4 /* ARGB32 */, images, out]) == 0)
            let tex = rd32(out)
            #expect(tex != 0)
            _ = ppc_call(method(dc, 2), [dc, 13, tex], 3) // setPtr kQATag_Texture
            _ = ppc_call(method(dc, 1), [dc, 12, 1], 3)  // TextureOp modulate
            func v(_ x: Float, _ y: Float, _ u: Float, _ vv: Float) -> [Float] {
                [x, y, 0.5, 1, 1, 1, 1, 1, u, vv, 1, 1, 1, 0, 0, 0]
            }
            let quad = Q.floats(v(0, 0, 0, 0) + v(16, 0, 1, 0) + v(0, 16, 0, 1) + v(16, 0, 1, 0) + v(16, 16, 1, 1) + v(0, 16, 0, 1))
            _ = ppc_call(method(dc, 13), [dc, 0, 0], 3)
            _ = ppc_call(method(dc, 11), [dc, 6, 3 /* Tri */, quad, 0], 5) // drawVTexture
            _ = ppc_call(method(dc, 14), [dc, 0], 2)
            #expect(px(pixels, 16, 3, 3) == 0xFF0000)
            #expect(px(pixels, 16, 12, 3) == 0x00FF00)
            #expect(px(pixels, 16, 3, 12) == 0x0000FF)
            #expect(px(pixels, 16, 12, 12) == 0xFFFFFF)
            Q.call("QATextureDelete", [engine, tex])
            Q.call("QADrawContextDelete", [dc])
        }

        /// A CL4 texture drawn through the colour table bound to it, then
        /// through another, index 0 clear where the table says so.
        @Test func colourTableTextures() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let (dc, pixels) = context(16, 16)
            let engine = Q.call("QADeviceGetFirstEngine", [Q.words([0, 64, 3, 16, 16, pixels])])
            let r = Q.words([0])
            #expect(Q.call("QAEngineGestalt", [engine, 0, r]) == 0 && rd32(r) & (1 << 13) != 0) // kQAOptional_CL4
            // 2 by 2 texels: indices 1 2 / 3 0, high nibble first.
            let image = Q.words([0x1200_0000, 0x3000_0000])
            let out = Q.words([0])
            #expect(Q.call("QATextureNew", [engine, 0, 5 /* CL4 */, Q.words([2, 2, 4, image]), out]) == 0)
            let tex = rd32(out)
            var entries = [UInt32](repeating: 0, count: 16)
            entries[0] = 0xFFFFFF; entries[1] = 0xFF0000; entries[2] = 0x00FF00; entries[3] = 0x0000FF
            #expect(Q.call("QAColorTableNew", [engine, 1 /* CL4_RGB32 */, Q.words(entries), 0, out]) == 0)
            let table = rd32(out)
            #expect(Q.call("QATextureBindColorTable", [engine, tex, table]) == 0)
            _ = ppc_call(method(dc, 2), [dc, 13, tex], 3)
            _ = ppc_call(method(dc, 1), [dc, 12, 1], 3)
            func v(_ x: Float, _ y: Float, _ u: Float, _ vv: Float) -> [Float] {
                [x, y, 0.5, 1, 1, 1, 1, 1, u, vv, 1, 1, 1, 0, 0, 0]
            }
            let quad = Q.floats(v(0, 0, 0, 0) + v(16, 0, 1, 0) + v(0, 16, 0, 1) + v(16, 0, 1, 0) + v(16, 16, 1, 1) + v(0, 16, 0, 1))
            func draw() {
                _ = ppc_call(method(dc, 13), [dc, 0, 0], 3)
                _ = ppc_call(method(dc, 11), [dc, 6, 3, quad, 0], 5)
                _ = ppc_call(method(dc, 14), [dc, 0], 2)
            }
            draw()
            #expect(px(pixels, 16, 3, 3) == 0xFF0000 && px(pixels, 16, 12, 3) == 0x00FF00)
            #expect(px(pixels, 16, 3, 12) == 0x0000FF && px(pixels, 16, 12, 12) == 0xFFFFFF)
            // Another table, with index 0 clear: the background shows there.
            entries[1] = 0xFFFF00
            #expect(Q.call("QAColorTableNew", [engine, 1, Q.words(entries), 1, out]) == 0)
            #expect(Q.call("QATextureBindColorTable", [engine, tex, rd32(out)]) == 0)
            draw()
            #expect(px(pixels, 16, 3, 3) == 0xFFFF00)
            #expect(px(pixels, 16, 12, 12) == 0x000000) // cleared to the default background
            Q.call("QAColorTableDelete", [engine, table])
            Q.call("QAColorTableDelete", [engine, rd32(out)])
            Q.call("QATextureDelete", [engine, tex])
            Q.call("QADrawContextDelete", [dc])
        }

        /// The draw buffer and z buffer read mid-frame, changed and put
        /// back; a texture made from what the context drew.
        @Test func bufferAccess() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let (dc, pixels) = context(8, 8)
            _ = ppc_call(method(dc, 13), [dc, 0, 0], 3) // renderStart
            let red = Q.floats([0, 0, 0.5, 1, 1, 0, 0, 1,  8, 0, 0.5, 1, 1, 0, 0, 1,  0, 8, 0.5, 1, 1, 0, 0, 1])
            _ = ppc_call(method(dc, 8), [dc, red, red + 32, red + 64, 0], 5)
            let buf = Q.words([0, 0, 0, 0, 0, 0])
            #expect(ppc_call(method(dc, 25), [dc, buf], 2) == 0) // accessDrawBuffer
            #expect(rd32(buf) == 32 && rd32(buf + 4) == 3 && rd32(buf + 8) == 8)
            let base = rd32(buf + 16)
            #expect(rd32(base) & 0xFFFFFF == 0xFF0000)        // drawn
            #expect(rd32(base + 4 * 63) & 0xFFFFFF == 0)      // not drawn
            wr32(base + 4 * 63, 0x00FF00)                      // the corner made green
            #expect(ppc_call(method(dc, 26), [dc, 0], 2) == 0) // accessDrawBufferEnd
            #expect(ppc_call(method(dc, 27), [dc, buf], 2) == 0) // accessZBuffer
            #expect(rd32(buf) == 8 && rd32(buf + 16) == 32 && rd32(buf + 20) == 1)
            let z = rd32(buf + 12)
            #expect(rd32(z) < 0x8100_0000 && rd32(z) > 0x7F00_0000) // 0.5
            #expect(rd32(z + 4 * 63) == 0xFFFF_FFFF)                // cleared: far
            wr32(z + 4 * 63, 0)                                     // nearest: nothing passes there
            #expect(ppc_call(method(dc, 28), [dc, 0], 2) == 0)
            let blue = Q.floats([-8, -8, 0.1, 1, 0, 0, 1, 1,  24, -8, 0.1, 1, 0, 0, 1, 1,  -8, 24, 0.1, 1, 0, 0, 1, 1])
            _ = ppc_call(method(dc, 8), [dc, blue, blue + 32, blue + 64, 0], 5)
            _ = ppc_call(method(dc, 14), [dc, 0], 2) // renderEnd
            #expect(px(pixels, 8, 1, 1) == 0x0000FF)
            #expect(px(pixels, 8, 7, 7) == 0x00FF00) // kept by its depth
            let out = Q.words([0])
            #expect(ppc_call(method(dc, 31), [dc, 0, out], 3) == 0) // textureFromContext
            #expect(rd32(out) != 0)
            let engine = Q.call("QADeviceGetFirstEngine", [0])
            Q.call("QATextureDelete", [engine, rd32(out)])
            Q.call("QADrawContextDelete", [dc])
        }
    }
}
