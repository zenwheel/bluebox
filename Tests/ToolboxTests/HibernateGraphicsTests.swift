import CCore
import CPU68K
import CPUPPC
import ForkStore
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

        // ---- OpenGL ----

        typealias G = OpenGL
        let g = OpenGL()

        func checksum(_ mem: UInt32, _ n: Int) -> UInt64 {
            var h: UInt64 = 1469598103934665603
            for i in 0..<n { h = (h ^ UInt64(rd32(mem + UInt32(4 * i)) & 0xFFFFFF)) &* 1099511628211 }
            return h
        }

        /// Values glGet answers for a few of each kind of state.
        func dump() -> [UInt32] {
            let out = tb_NewPtr(64 * 4, false, true)
            defer { tb_DisposePtr(out) }
            var all: [UInt32] = []
            for (pname, n) in [(0x0BA6, 16), (0x0BA7, 16), (0x0BA3, 1), (0x0C22, 4), (0x0B66, 4), (0x0BA2, 4),
                               (0x8069, 1), (0x0B63, 1), (0x0B64, 1), (0x0B74, 1), (0x0B53, 4), (0x0B00, 4),
                               (0x0B65, 1), (0x0B22, 1), (0x0B21, 1), (0x0BA0, 1), (0x0B11, 1)] {
                g.gl("glGetFloatv", [UInt32(pname), out])
                all += (0..<n).map { rd32(out + UInt32(4 * $0)) }
            }
            g.gl("glGetLightfv", [0x4000, 0x1203 /* GL_POSITION */, out])
            all += (0..<4).map { rd32(out + UInt32(4 * $0)) }
            for cap: UInt32 in [0x0B71, 0x0B50, 0x4000, 0x0B60, 0x0DE1, 0x0B57] { all.append(g.gl("glIsEnabled", [cap])) }
            return all
        }

        /// A context drawing off screen with much of its state set: depth
        /// tested, lit, fogged and textured (its pixels gone from the
        /// program's memory), a model-view stack two deep, and display
        /// lists of immediate calls, a vertex array and a GLU sphere.  The
        /// one that goes on after hibernating and the resumed one show the
        /// frame as it was (the framebuffer carried), have the same state,
        /// and draw the same frame again.
        @Test func openGLContextComesBack() {
            let dir = launchTestApp()
            startLoop()
            _ = initToolbox()
            let ctx = g.context()
            let w = 32, mem = tb_NewPtr(Int32(w * w * 4), false, true)
            #expect(g.gl("aglSetOffScreen", [ctx, UInt32(w), UInt32(w), UInt32(w * 4), mem]) == 1)
            g.gl("glClearColor", fp: [0.1, 0.2, 0.3, 1])
            g.gl("glEnable", [0x0B71]); g.gl("glDepthFunc", [0x0203]) // DEPTH_TEST, LEQUAL
            g.gl("glMatrixMode", [0x1701]); g.gl("glLoadIdentity")
            g.gl("glFrustum", fp: [-1, 1, -1, 1, 1, 10])
            g.gl("glMatrixMode", [0x1700]); g.gl("glLoadIdentity")
            g.gl("glTranslatef", fp: [0, 0, -3])
            g.gl("glLightfv", [0x4000, 0x1203, g.floats([1, 1, 1, 0])])  // under the translation: eye coordinates
            g.gl("glPushMatrix")
            g.gl("glRotatef", fp: [30, 0, 1, 0])
            g.gl("glEnable", [0x0B50]); g.gl("glEnable", [0x4000])          // LIGHTING, LIGHT0
            g.gl("glLightfv", [0x4000, 0x1201, g.floats([1, 0.5, 0.25, 1])])
            g.gl("glMaterialfv", [0x0404, 0x1602, g.floats([0.8, 0.8, 0.8, 1])]) // FRONT, AMBIENT_AND_DIFFUSE
            g.gl("glEnable", [0x0B60]); g.gl("glFogi", [0x0B65, 0x2601])    // FOG, LINEAR
            g.gl("glFogf", [0x0B63], fp: [2]); g.gl("glFogf", [0x0B64], fp: [5])
            g.gl("glFogfv", [0x0B66, g.floats([0, 1, 0, 1])])
            let names = tb_NewPtr(8, false, true)
            g.gl("glGenTextures", [1, names])
            g.gl("glBindTexture", [G.GL_TEXTURE_2D, rd32(names)])
            g.gl("glTexParameteri", [G.GL_TEXTURE_2D, 0x2801, 0x2600])
            g.gl("glTexParameteri", [G.GL_TEXTURE_2D, 0x2800, 0x2600])
            let texels = g.words([0xFF00_00FF, 0x00FF_00FF, 0x0000_FFFF, 0xFFFF_FFFF]) // RGBA bytes
            g.gl("glTexImage2D", [G.GL_TEXTURE_2D, 0, G.GL_RGBA, 2, 2, 0, G.GL_RGBA, G.GL_UNSIGNED_BYTE, texels])
            for i in 0..<4 { wr32(texels + UInt32(4 * i), 0) }
            g.gl("glEnable", [G.GL_TEXTURE_2D])
            let list = g.gl("glGenLists", [3])
            #expect(list != 0)
            g.gl("glNewList", [list, 0x1300]) // GL_COMPILE
            g.gl("glBegin", [G.GL_QUADS])
            for (x, y, s, t) in [(-1.0, -1.0, 0.0, 0.0), (1.0, -1.0, 1.0, 0.0), (1.0, 1.0, 1.0, 1.0), (-1.0, 1.0, 0.0, 1.0)] {
                g.gl("glNormal3f", fp: [0, 0, 1]); g.gl("glTexCoord2f", fp: [s, t]); g.gl("glVertex3f", fp: [x, y, 0])
            }
            g.gl("glEnd")
            g.gl("glEndList")
            #expect(g.gl("glIsList", [list + 2]) == 1)
            g.gl("glNewList", [list + 1, 0x1300])
            g.gl("glDisable", [G.GL_TEXTURE_2D])
            let quadric = g.gl("gluNewQuadric")
            g.gl("gluQuadricDrawStyle", [quadric, 100012 /* GLU_LINE */])
            g.gl("gluSphere", [quadric, 0, 0, 8, 6], fp: [0.5])
            let verts = g.floats([0.6, 0.6, 0.5, 0.9, 0.6, 0.5, 0.9, 0.9, 0.5])
            g.gl("glVertexPointer", [3, G.GL_FLOAT, 0, verts])
            g.gl("glEnableClientState", [G.GL_VERTEX_ARRAY])
            g.gl("glDrawArrays", [G.GL_TRIANGLES, 0, 3])
            g.gl("glDisableClientState", [G.GL_VERTEX_ARRAY])
            g.gl("glEnable", [G.GL_TEXTURE_2D])
            g.gl("glEndList")
            for i in 0..<9 { wr32(verts + UInt32(4 * i), 0) }
            g.gl("gluQuadricDrawStyle", [quadric, 100011 /* GLU_POINT: the list keeps GLU_LINE */])
            #expect(hostgl_display_lists() == 3)
            func frame() -> UInt64 {
                g.gl("glClear", [0x4100])
                g.gl("glCallList", [list])
                g.gl("glCallList", [list + 1])
                g.gl("aglSwapBuffers", [ctx])
                return checksum(mem, w * w)
            }
            #expect(g.gl("glGetError") == 0)
            let before = frame()
            let state = dump()
            #expect(g.gl("glGetError") == 0)
            #expect(rd32(mem + UInt32(4 * (16 * w + 16))) & 0xFFFFFF != 0x1A334C) // something drawn mid-way

            let snap = hibernate(dir, "gl.bbhib")
            #expect(hostgl_saved_bytes() == 0)
            #expect(dump() == state)              // the process that hibernated, as it was
            #expect(g.gl("glGetError") == 0)
            #expect(frame() == before)
            g.gl("glPopMatrix")
            g.gl("glClearColor", fp: [1, 1, 1, 1])
            _ = frame()

            resume(snap)
            #expect(hostgl_saved_bytes() > 32 * 32 * 8)
            for i in 0..<(w * w) { wr32(mem + UInt32(4 * i), 0) }
            g.gl("aglSwapBuffers", [ctx])         // the frame as it was, before anything is drawn
            #expect(checksum(mem, w * w) == before)
            #expect(hostgl_saved_bytes() == 0)
            #expect(hostgl_display_lists() == 3)
            #expect(dump() == state)
            #expect(frame() == before)
            #expect(g.gl("glGetError") == 0)
            #expect(g.gl("glIsList", [list + 2]) == 1)
            g.gl("aglDestroyContext", [ctx])
        }

        /// Texture names after resuming are the ones the host would have
        /// given: one made and never bound is still taken, the next made
        /// is the one after; an error the host had is the program's still.
        @Test func openGLNamesAndErrors() {
            let dir = launchTestApp()
            startLoop()
            let ctx = g.context()
            let mem = tb_NewPtr(8 * 8 * 4, false, true)
            #expect(g.gl("aglSetOffScreen", [ctx, 8, 8, 32, mem]) == 1)
            let names = tb_NewPtr(16, false, true)
            g.gl("glGenTextures", [3, names])
            let t = (0..<3).map { rd32(names + UInt32(4 * $0)) }
            g.gl("glBindTexture", [G.GL_TEXTURE_2D, t[0]])
            g.gl("glBindTexture", [G.GL_TEXTURE_2D, t[2]])
            g.gl("glBindTexture", [G.GL_TEXTURE_2D, 0])
            g.gl("glEnable", [0x1234])  // GL_INVALID_ENUM, not yet asked for
            let snap = hibernate(dir, "names.bbhib")
            g.gl("glGenTextures", [1, names])
            let next = rd32(names)
            #expect(!t.contains(next))
            #expect(g.gl("glGetError") == 0x0500)
            resume(snap)
            #expect(g.gl("glGetError") == 0x0500)
            #expect(g.gl("glGetError") == 0)
            #expect(g.gl("glIsTexture", [t[0]]) == 1 && g.gl("glIsTexture", [t[1]]) == 0)
            g.gl("glGenTextures", [1, names])
            #expect(rd32(names) == next)
            g.gl("aglDestroyContext", [ctx])
        }

        /// A program between glBegin and glEnd, or compiling a display
        /// list, at its event loop: refused, saying so.
        @Test func openGLMidwayIsRefused() {
            launchTestApp()
            startLoop()
            let ctx = g.context()
            g.gl("glBegin", [G.GL_TRIANGLES])
            machine_drain_begin(true)
            _ = machine_run(10_000_000)
            #expect(!machine_hibernatable())
            #expect(String(cString: machine_drain_reason()).contains("glBegin"))
            machine_drain_end()
            g.gl("glEnd")
            g.gl("glNewList", [g.gl("glGenLists", [1]), 0x1300])
            machine_drain_begin(true)
            _ = machine_run(10_000_000)
            #expect(String(cString: machine_drain_reason()).contains("display list"))
            machine_drain_end()
            g.gl("glEndList")
            machine_drain_begin(true)
            #expect(machine_run(10_000_000) == RUN_DRAINED)
            #expect(machine_hibernatable())
            machine_drain_end()
            g.gl("aglDestroyContext", [ctx])
        }

        // ---- QuickTime ----

        func atom(_ type: String, _ body: [UInt8]) -> [UInt8] { cat(be32(UInt32(8 + body.count)), Array(type.utf8), body) }

        /// A movie VLC would play (Sorenson Video, which our decoders do
        /// not have) with its description in the resource fork: played from
        /// a flattened copy in the temporary folder.
        func sorensonMovie(_ dir: URL) -> String {
            func full(_ type: String, _ body: [UInt8]) -> [UInt8] { atom(type, cat([0, 0, 0, 0], body)) }
            let stsd = cat(be32(86), Array("SVQ1".utf8), [0, 0, 0, 0, 0, 0], be16(1), [UInt8](repeating: 0, count: 16),
                           be16(2), be16(2), be32(72 << 16), be32(72 << 16), be32(0), be16(1),
                           [UInt8](repeating: 0, count: 32), be16(24), be16(0xFFFF))
            let stbl = atom("stbl", cat(full("stsd", cat(be32(1), stsd)), full("stts", cat(be32(1), be32(2), be32(600))),
                                        full("stsc", cat(be32(1), be32(1), be32(1), be32(1))),
                                        full("stsz", cat(be32(16), be32(2))), full("stco", cat(be32(2), be32(0), be32(16)))))
            let mdhd = full("mdhd", cat(be32(0), be32(0), be32(600), be32(1200), be32(0)))
            let hdlr = full("hdlr", cat(Array("mhlr".utf8), Array("vide".utf8), [UInt8](repeating: 0, count: 13)))
            let tkhd = cat([0, 0, 0, 1], be32(0), be32(0), be32(1), be32(0), be32(1200), [UInt8](repeating: 0, count: 52),
                           be32(2 << 16), be32(2 << 16))
            let trak = atom("trak", cat(atom("tkhd", tkhd), atom("mdia", cat(mdhd, hdlr, atom("minf", stbl)))))
            let mvhd = full("mvhd", cat(be32(0), be32(0), be32(600), be32(1200), [UInt8](repeating: 0, count: 80)))
            let moov = atom("moov", cat(mvhd, trak))
            var fork = buildFork([TestResource(type: "moov", id: 128, data: moov)])
            var data = [UInt8](repeating: 0x55, count: 32)
            let path = dir.appendingPathComponent("Sorenson").path
            var f = mac_file()
            data.withUnsafeMutableBufferPointer { f.data = $0.baseAddress; f.data_len = $0.count }
            fork.withUnsafeMutableBufferPointer { f.rsrc = $0.baseAddress; f.rsrc_len = $0.count }
            var err = [CChar](repeating: 0, count: 256)
            #expect(appledouble_write(path, &f, &err, err.count))
            return path
        }

        func tempCopies() -> Set<String> {
            let tmp = ProcessInfo.processInfo.environment["TMPDIR"] ?? "/tmp"
            let mine = "bluebox-movie-\(getpid())-"
            let names = (try? FileManager.default.contentsOfDirectory(atPath: tmp)) ?? []
            return Set(names.filter { $0.hasPrefix(mine) }.map { (tmp as NSString).appendingPathComponent($0) })
        }

        /// A movie played from a flattened copy (VLC's) hibernates: resumed
        /// after the process that made the copy has ended (and deleted it),
        /// the copy is made again, and the movie's time goes on.
        @Test func movieFlattenedCopyIsMadeAgain() {
            let dir = launchTestApp()
            startLoop()
            #expect(tb_EnterMovies() == 0)
            let path = sorensonMovie(dir)
            let before = tempCopies()
            let spec = tb_NewPtr(Int32(FSSpec_SIZEOF), false, true)
            fm_spec_for_path(path, spec)
            let rn = tb_NewPtr(2, false, true)
            #expect(tb_OpenMovieFile(spec, rn, 1) == 0)
            let mp = tb_NewPtr(4, false, true)
            #expect(tb_NewMovieFromFile(mp, Int16(bitPattern: rd16(rn)), 0, 0, 0, 0) == 0)
            _ = tb_CloseMovieFile(Int16(bitPattern: rd16(rn)))
            let movie = rd32(mp)
            #expect(movie != 0)
            let copies = tempCopies().subtracting(before)
            #expect(copies.count == 1)
            tb_StartMovie(movie)
            machine_skip_time(500_000_000)
            let t0 = tb_GetMovieTime(Int32(bitPattern: movie), 0)

            let snap = hibernate(dir, "movie.bbhib")
            for c in copies { unlink(c) } // the process that made it, ended
            resume(snap)
            let made = tempCopies().subtracting(before)
            #expect(made.count == 1)
            #expect(made.allSatisfy { FileManager.default.fileExists(atPath: $0) })
            let t1 = tb_GetMovieTime(Int32(bitPattern: movie), 0)
            #expect(t1 >= t0 && t1 <= t0 + 60)
            machine_skip_time(500_000_000)
            tb_MoviesTask(movie, 0)
            #expect(tb_GetMovieTime(Int32(bitPattern: movie), 0) > t1 + 200)
            tb_DisposeMovie(movie)
            #expect(tempCopies().subtracting(before).isEmpty)
        }
    }
}
