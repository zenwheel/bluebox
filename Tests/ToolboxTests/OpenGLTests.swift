import CCore
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// Apple's OpenGL for Mac OS 9 (OpenGLLibrary's gl and agl calls) on
    /// the host's: the exports called as a PowerPC caller would, integers
    /// and pointers in r3 on (past r10 on the stack), floating point in f1 on.
    @Suite struct OpenGL {
        static let GL_COLOR_BUFFER_BIT: UInt32 = 0x4000, GL_TRIANGLES: UInt32 = 4, GL_QUADS: UInt32 = 7
        static let GL_FLOAT: UInt32 = 0x1406, GL_UNSIGNED_BYTE: UInt32 = 0x1401, GL_RGBA: UInt32 = 0x1908
        static let GL_RGB: UInt32 = 0x1907, GL_TEXTURE_2D: UInt32 = 0x0DE1, GL_VERTEX_ARRAY: UInt32 = 0x8074

        /// Call an export: `args` are the general-register slots in order
        /// (a float takes one, a double two), `fp` the floating-point ones.
        @discardableResult
        func gl(_ name: String, _ args: [UInt32] = [], fp: [Double] = []) -> UInt32 {
            let stack = tb_NewPtr(256, false, true)
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
            tb_DisposePtr(stack)
            return g_ppc.gpr.3
        }

        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        func words(_ values: [UInt32]) -> UInt32 {
            let p = tb_NewPtr(Int32(values.count * 4 + 4), false, true)
            for (i, v) in values.enumerated() { wr32(p + UInt32(4 * i), v) }
            return p
        }

        func floats(_ values: [Float]) -> UInt32 { words(values.map { $0.bitPattern }) }

        /// A current context: RGBA, double-buffered, with a depth buffer.
        func context() -> UInt32 {
            // AGL_RGBA, AGL_DOUBLEBUFFER, AGL_DEPTH_SIZE 16, AGL_NONE
            let pix = gl("aglChoosePixelFormat", [0, 0, words([4, 5, 12, 16, 0])])
            #expect(pix != 0)
            let ctx = gl("aglCreateContext", [pix, 0])
            #expect(ctx != 0)
            #expect(gl("aglDestroyPixelFormat", [pix]) == 0)
            #expect(gl("aglSetCurrentContext", [ctx]) == 1)
            return ctx
        }

        func pixel(_ h: Int, _ v: Int) -> (UInt16, UInt16, UInt16) {
            let c = tb_NewPtr(6, false, true)
            tb_GetCPixel(Int16(h), Int16(v), c)
            defer { tb_DisposePtr(c) }
            return (rd16(c), rd16(c + 2), rd16(c + 4))
        }

        /// sixtyforce's way in: the version and the one renderer, then a
        /// window cleared blue with a red triangle, shown by
        /// aglSwapBuffers and not before.
        @Test func drawsIntoAWindow() {
            launchTestApp()
            let ver = tb_NewPtr(8, false, true)
            gl("aglGetVersion", [ver, ver + 4])
            #expect(rd32(ver) == 2 && rd32(ver + 4) == 0)
            let rend = gl("aglQueryRendererInfo", [0, 0])
            #expect(rend != 0)
            #expect(gl("aglDescribeRenderer", [rend, 73 /* AGL_ACCELERATED */, ver]) == 1 && rd32(ver) == 1)
            #expect(gl("aglNextRendererInfo", [rend]) == 0)
            gl("aglDestroyRendererInfo", [rend])
            #expect(gl("aglGetError") == 0)

            _ = initToolbox()
            let w = tb_NewCWindow(0, rect(100, 100, 148, 164), 0, 1, 0, 0xFFFF_FFFF, 0, 0)
            tb_SetPort(w)
            let ctx = context()
            #expect(gl("aglSetDrawable", [ctx, w]) == 1)
            let vp = tb_NewPtr(16, false, true)
            gl("glGetIntegerv", [0x0BA2 /* GL_VIEWPORT */, vp])
            #expect([rd32(vp), rd32(vp + 4), rd32(vp + 8), rd32(vp + 12)] == [0, 0, 64, 48])

            gl("glClearColor", fp: [0, 0, 1, 1])
            gl("glClear", [Self.GL_COLOR_BUFFER_BIT])
            gl("glColor3f", fp: [1, 0, 0])
            gl("glBegin", [Self.GL_TRIANGLES])
            gl("glVertex2f", fp: [-1, -1])
            gl("glVertex2f", fp: [1, -1])
            gl("glVertex2f", fp: [-1, 1])
            gl("glEnd")
            #expect(pixel(2, 45) == (0xFFFF, 0xFFFF, 0xFFFF)) // not yet swapped
            #expect(gl("aglSwapBuffers", [ctx]) == 0)
            tb_SetPort(w)
            #expect(pixel(2, 45) == (0xFFFF, 0, 0))   // bottom left: the triangle
            #expect(pixel(61, 2) == (0, 0, 0xFFFF))   // top right: the clear colour
            #expect(gl("glGetError") == 0)

            #expect(gl("aglSetCurrentContext", [0]) == 1)
            #expect(gl("aglDestroyContext", [ctx]) == 1)
            #expect(gl("aglDestroyContext", [ctx]) == 0)
            #expect(gl("aglGetError") == 10004) // AGL_BAD_CONTEXT
            #expect(gl("aglGetError") == 0)
            tb_DisposeWindow(w)
        }

        /// agl_macro.h's way in (Cro-Mag Rally): gl through the context's
        /// own table, the renderer word first, each entry a transition
        /// vector; clear_color is the thirteenth, read_pixels's eighth
        /// argument comes in r10.
        @Test func macroDispatchThroughTheContext() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            let ctx = context()
            let mem = tb_NewPtr(8 * 8 * 4, false, true)
            #expect(gl("aglSetOffScreen", [ctx, 8, 8, 32, mem]) == 1)
            let rend = rd32(ctx)
            func entry(_ i: UInt32) -> UInt32 { rd32(ctx + 4 + 4 * i) }
            #expect(entry(0) != 0 && entry(335) != 0 && rd32(entry(12) + 4) == 12)
            for (i, f) in [0.0, 1.0, 0.0, 1.0].enumerated() { ppc_set_fpr(&g_ppc, Int32(1 + i), f) }
            _ = ppc_call(entry(12), [rend], 1)                                   // glClearColor
            _ = ppc_call(entry(10), [rend, Self.GL_COLOR_BUFFER_BIT], 2)         // glClear
            let px = tb_NewPtr(4, false, true)
            _ = ppc_call(entry(237), [rend, 0, 0, 1, 1, Self.GL_RGBA, Self.GL_UNSIGNED_BYTE, px], 8) // glReadPixels
            #expect([rd8(px), rd8(px + 1), rd8(px + 2), rd8(px + 3)] == [0, 255, 0, 255])
            #expect(ppc_call(entry(102), [rend], 1) == 0)                         // glGetError
            #expect(gl("aglDestroyContext", [ctx]) == 1)
        }

        /// Big-endian data both ways: a 16-bit texture (the host swapping
        /// its pixels, the ninth argument on the stack), a float vertex
        /// array, a matrix read back, and glReadPixels; drawn off screen.
        @Test func guestByteOrder() {
            launchTestApp()
            let ctx = context()
            let mem = tb_NewPtr(8 * 8 * 4, false, true)
            #expect(gl("aglSetOffScreen", [ctx, 8, 8, 32, mem]) == 1)
            gl("glClearColor", fp: [0, 0, 0, 1])
            gl("glClear", [Self.GL_COLOR_BUFFER_BIT])

            gl("glTranslatef", fp: [0.5, 0.25, 0])
            let m = tb_NewPtr(64, false, true)
            gl("glGetFloatv", [0x0BA6 /* GL_MODELVIEW_MATRIX */, m])
            #expect(Float(bitPattern: rd32(m + 48)) == 0.5 && Float(bitPattern: rd32(m + 52)) == 0.25)
            #expect(Float(bitPattern: rd32(m)) == 1)
            gl("glLoadIdentity")

            // A 1x1 green texture as GL_UNSIGNED_SHORT_5_6_5, big-endian.
            let tex = tb_NewPtr(4, false, true), name = tb_NewPtr(4, false, true)
            wr16(tex, 0x07E0)
            gl("glGenTextures", [1, name])
            #expect(rd32(name) != 0)
            gl("glBindTexture", [Self.GL_TEXTURE_2D, rd32(name)])
            gl("glTexParameteri", [Self.GL_TEXTURE_2D, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */])
            gl("glTexImage2D", [Self.GL_TEXTURE_2D, 0, Self.GL_RGB, 1, 1, 0, Self.GL_RGB, 0x8363 /* 5_6_5 */, tex])
            gl("glEnable", [Self.GL_TEXTURE_2D])
            // The left half by a vertex array of big-endian floats.
            let verts = floats([-1, -1, 0, -1, 0, 1, -1, 1])
            gl("glVertexPointer", [2, Self.GL_FLOAT, 0, verts])
            gl("glEnableClientState", [Self.GL_VERTEX_ARRAY])
            gl("glDrawArrays", [Self.GL_QUADS, 0, 4])
            gl("glDisableClientState", [Self.GL_VERTEX_ARRAY])
            gl("glDisable", [Self.GL_TEXTURE_2D])
            #expect(gl("glGetError") == 0)

            // glReadPixels: bottom row, RGBA bytes.
            let px = tb_NewPtr(32, false, true)
            gl("glReadPixels", [0, 0, 8, 1, Self.GL_RGBA, Self.GL_UNSIGNED_BYTE, px])
            #expect(rd32(px) == 0x00FF_00FF && rd32(px + 28) == 0x0000_00FF)
            // 32-bit unsigned ints come back big-endian.
            gl("glReadPixels", [0, 0, 1, 1, Self.GL_RGBA, 0x1405 /* GL_UNSIGNED_INT */, px])
            #expect(rd32(px) == 0 && rd32(px + 4) == 0xFFFF_FFFF)

            // Off screen, the frame lands in memory as xRGB, top row first.
            gl("glFlush")
            #expect(rd32(mem) == 0) // nothing yet: double-buffered
            gl("aglSwapBuffers", [ctx])
            #expect(rd32(mem) & 0x00FF_FFFF == 0x0000_FF00)
            #expect(rd32(mem + 7 * 32 + 28) & 0x00FF_FFFF == 0)
            gl("aglDestroyContext", [ctx])
        }

        /// aglUseFont: QuickDraw's characters as bitmap display lists,
        /// drawn by glCallLists from a string of bytes.
        @Test func useFont() {
            launchTestApp()
            _ = initToolbox()
            let ctx = context()
            let mem = tb_NewPtr(64 * 32 * 4, false, true)
            #expect(gl("aglSetOffScreen", [ctx, 64, 32, 256, mem]) == 1)
            #expect(gl("aglUseFont", [ctx, 0, 0, 12, 0, 128, 1000]) == 1)
            gl("glClearColor", fp: [0, 0, 0, 1])
            gl("glClear", [Self.GL_COLOR_BUFFER_BIT])
            gl("glMatrixMode", [0x1701]); gl("glLoadIdentity")
            gl("glOrtho", fp: [0, 64, 0, 32, -1, 1])
            gl("glColor3f", fp: [1, 1, 1])
            gl("glRasterPos2i", [4, 10])
            gl("glListBase", [1000])
            let text = tb_NewPtr(4, false, true)
            for (i, b) in Array("HI".utf8).enumerated() { wr8(text + UInt32(i), b) }
            gl("glCallLists", [2, Self.GL_UNSIGNED_BYTE, text])
            #expect(gl("glGetError") == 0)
            gl("aglSwapBuffers", [ctx])
            let lit = (0..<(64 * 32)).filter { rd32(mem + UInt32(4 * $0)) & 0xFFFFFF == 0xFFFFFF }.count
            #expect(lit > 20 && lit < 400)
            gl("aglDestroyContext", [ctx])
        }

        /// glGetString's strings are the program's to read, and gl calls
        /// with no current context do nothing.
        @Test func stringsAndNoContext() {
            launchTestApp()
            gl("glClear", [Self.GL_COLOR_BUFFER_BIT])
            #expect(gl("glGetString", [0x1F02]) == 0)
            let ctx = context()
            let s = gl("glGetString", [0x1F02 /* GL_VERSION */])
            #expect(s != 0 && rd8(s) == UInt8(ascii: "1") && rd8(s + 1) == UInt8(ascii: "."))
            let e = gl("glGetString", [0x1F03 /* GL_EXTENSIONS */])
            #expect(e != 0)
            gl("aglDestroyContext", [ctx])
        }
    }
}
