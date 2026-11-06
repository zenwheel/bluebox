import CCore
import CPU68K
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// Hibernating with 3D graphics (hibernate milestone M6): a snapshot
    /// taken while a program has RAVE draw contexts and textures, QuickDraw
    /// 3D views, or OpenGL contexts, put back as a resumed process would.
    /// What the host held (its frames, textures, contexts) is read back into
    /// the machine for the snapshot, and made again from it the first time
    /// it is used after resuming; the resumed program then draws as the one
    /// that hibernated does.
    @Suite(.serialized) struct HibernateGraphics {
        typealias Q = Q3Call

        /// The program's event loop (_SystemTask; BRA.S), with a stack.
        func startLoop() {
            let code = tb_NewPtr(4, false, false)
            wr16(code, 0xA9B4)
            wr16(code + 2, 0x60FC)
            let cpu = g_cpu!
            cpu.pointee.a.7 = tb_NewPtr(65536, false, true) + 65000
            cpu.pointee.sr = 0x2000
            cpu.pointee.pc = code
            cpu.pointee.halted = false
        }

        /// Stopped at the loop's SystemTask and saved; the machine then goes on.
        func hibernate(_ dir: URL, _ name: String) -> String {
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable(), Comment(rawValue: String(cString: machine_drain_reason())))
            let snap = dir.appendingPathComponent(name).path
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_save(snap, "{}", nil, &err, err.count), Comment(rawValue: String(cString: err)))
            machine_drain_end()
            return snap
        }

        func resume(_ snap: String) {
            var err = [CChar](repeating: 0, count: 256)
            #expect(machine_hibernate_restore(snap, &err, err.count), Comment(rawValue: String(cString: err)))
        }

        // ---- RAVE ----

        func rave(_ w: Int, _ h: Int) -> (dc: UInt32, pixels: UInt32, engine: UInt32) {
            let pixels = tb_NewPtr(Int32(w * h * 4), false, true)
            let dev = Q.words([0, UInt32(w * 4), 3, UInt32(w), UInt32(h), pixels])
            let engine = Q.call("QADeviceGetFirstEngine", [dev])
            let out = Q.words([0])
            #expect(Q.call("QADrawContextNew", [dev, Q.words([0, UInt32(w), 0, UInt32(h)]), 0, engine, 0, out]) == 0)
            return (rd32(out), pixels, engine)
        }

        func method(_ dc: UInt32, _ tag: UInt32) -> UInt32 { rd32(dc + 8 + 4 * tag) }
        func px(_ pixels: UInt32, _ w: Int, _ x: Int, _ y: Int) -> UInt32 { rd32(pixels + UInt32((y * w + x) * 4)) & 0xFFFFFF }

        func texturedQuad(_ w: Float) -> UInt32 {
            func v(_ x: Float, _ y: Float, _ u: Float, _ vv: Float) -> [Float] {
                [x, y, 0.5, 1, 1, 1, 1, 1, u, vv, 1, 1, 1, 0, 0, 0]
            }
            return Q.floats(v(0, 0, 0, 0) + v(w, 0, 1, 0) + v(0, w, 0, 1) + v(w, 0, 1, 0) + v(w, w, 1, 1) + v(0, w, 0, 1))
        }

        /// A RAVE texture made before hibernating draws after resuming: the
        /// snapshot carries its pixels (the program's own are gone), and the
        /// host's is made again from them at the first draw.
        @Test func raveTextureComesBack() {
            let dir = launchTestApp()
            startLoop()
            _ = cfm_ppc_cpu()
            let (dc, pixels, engine) = rave(16, 16)
            let image = Q.words([0xFFFF_0000, 0xFF00_FF00, 0xFF00_00FF, 0xFFFF_FFFF])
            let out = Q.words([0])
            #expect(Q.call("QATextureNew", [engine, 0, 4, Q.words([2, 2, 8, image]), out]) == 0)
            let tex = rd32(out)
            #expect(Q.call("QATextureDetach", [engine, tex]) == 0)
            for i in 0..<4 { wr32(image + UInt32(4 * i), 0) } // the program's copy, gone
            _ = ppc_call(method(dc, 2), [dc, 13, tex], 3) // kQATag_Texture
            _ = ppc_call(method(dc, 1), [dc, 12, 1], 3)
            let quad = texturedQuad(16)

            let snap = hibernate(dir, "rave-tex.bbhib")
            #expect(r3d_saved_bytes() == 0) // let go of again in the process that goes on
            resume(snap)
            #expect(r3d_saved_bytes() == 16) // the texture's 2 by 2 pixels
            _ = ppc_call(method(dc, 13), [dc, 0, 0], 3)
            _ = ppc_call(method(dc, 11), [dc, 6, 3, quad, 0], 5)
            _ = ppc_call(method(dc, 14), [dc, 0], 2)
            #expect(px(pixels, 16, 3, 3) == 0xFF0000)
            #expect(px(pixels, 16, 12, 3) == 0x00FF00)
            #expect(px(pixels, 16, 3, 12) == 0x0000FF)
            #expect(px(pixels, 16, 12, 12) == 0xFFFFFF)
            #expect(r3d_saved_bytes() == 0)
            Q.call("QATextureDelete", [engine, tex])
            Q.call("QADrawContextDelete", [dc])
        }

        /// Hibernated between QARenderStart and QARenderEnd: what was drawn
        /// so far, its depths too, goes on in the resumed frame.  A red
        /// triangle near, drawn before; a green one far over everything,
        /// after: green shows only where red is not.
        @Test func raveFrameMidwayGoesOn() {
            let dir = launchTestApp()
            startLoop()
            _ = cfm_ppc_cpu()
            let (dc, pixels, _) = rave(32, 32)
            for (tag, v) in [(1, 1.0), (2, 0.0), (3, 0.0), (4, 1.0)] {
                ppc_set_fpr(&g_ppc, 1, v)
                _ = ppc_call(method(dc, 0), [dc, UInt32(tag)], 2)
            }
            _ = ppc_call(method(dc, 13), [dc, 0, 0], 3) // renderStart
            let red = Q.floats([0, 0, 0.5, 1, 1, 0, 0, 1, 32, 0, 0.5, 1, 1, 0, 0, 1, 0, 32, 0.5, 1, 1, 0, 0, 1])
            _ = ppc_call(method(dc, 8), [dc, red, red + 32, red + 64, 0], 5)

            let snap = hibernate(dir, "rave-mid.bbhib")
            resume(snap)
            #expect(r3d_saved_bytes() == 32 * 32 * 8)
            let green = Q.floats([-40, -40, 0.9, 1, 0, 1, 0, 1, 100, -40, 0.9, 1, 0, 1, 0, 1, -40, 100, 0.9, 1, 0, 1, 0, 1])
            _ = ppc_call(method(dc, 8), [dc, green, green + 32, green + 64, 0], 5)
            #expect(ppc_call(method(dc, 14), [dc, 0], 2) == 0) // renderEnd
            #expect(px(pixels, 32, 2, 2) == 0xFF0000)
            #expect(px(pixels, 32, 20, 20) == 0x00FF00)
            Q.call("QADrawContextDelete", [dc])
        }

        // ---- QuickDraw 3D ----

        /// A QuickDraw 3D view with a textured box renders after resuming as
        /// before: its frame is made again, and its texture from the texture
        /// object (the snapshot does not carry the renderer's copy).
        @Test func quickDraw3DViewRendersAgain() {
            let dir = launchTestApp()
            startLoop()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let q = QuickDraw3D()
            let (dc, image) = q.pixmapContext(64, 64, clear: (0, 0, 0))
            let v = q.view(dc)
            var desc = [UInt8](repeating: 0, count: 86)
            desc[3] = 86
            desc[4...7] = [0x72, 0x61, 0x77, 0x20] // 'raw '
            desc[33] = 2; desc[35] = 2; desc[83] = 32
            let d = tb_NewPtr(86, false, true)
            for (i, b) in desc.enumerated() { wr8(d + UInt32(i), b) }
            let texels = Q.words([0xFF00_FF00, 0xFF00_FF00, 0xFF00_FF00, 0xFF00_FF00])
            let descStorage = Q.call("Q3MemoryStorage_New", [d, 86])
            let dataStorage = Q.call("Q3MemoryStorage_New", [texels, 16])
            let tex = Q.call("Q3CompressedPixmapTexture_New", [Q.words([dataStorage, 0, descStorage, 0, 2, 2, 32, 0])])
            let shader = Q.call("Q3TextureShader_New", [tex])
            let null = Q.call("Q3NULLIllumination_New")
            let box = Q.call("Q3Box_New", [QuickDraw3D.data([Float(-1), Float(-1), Float(0), Float(0), Float(2), Float(0),
                                                             Float(0), Float(0), Float(1), Float(2), Float(0), Float(0), 0, 0])])
            func render() {
                for i in 0..<(64 * 64) { wr32(image + UInt32(4 * i), 0x123456) }
                #expect(Q.call("Q3View_StartRendering", [v]) == 1)
                #expect(Q.call("Q3Object_Submit", [null, v]) == 1)
                #expect(Q.call("Q3Object_Submit", [shader, v]) == 1)
                #expect(Q.call("Q3Object_Submit", [box, v]) == 1)
                #expect(Q.call("Q3View_EndRendering", [v]) == 0)
            }
            render()
            #expect(px(image, 64, 32, 32) == 0x00FF00)

            let snap = hibernate(dir, "q3.bbhib")
            resume(snap)
            #expect(r3d_saved_bytes() == 0) // nothing the guest cannot make again
            render()
            #expect(px(image, 64, 32, 32) == 0x00FF00)
            #expect(px(image, 64, 1, 1) == 0x000000)
            for obj in [box, null, shader, tex, descStorage, dataStorage, v, dc] { Q.call("Q3Object_Dispose", [obj]) }
        }
    }
}
