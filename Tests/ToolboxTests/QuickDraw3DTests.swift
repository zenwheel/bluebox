import CCore
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// QuickDraw 3D's objects, views and drawing, called as a PowerPC
    /// program would, with frames rendered by the host and read back.
    @Suite struct QuickDraw3D {
        typealias Q = Q3Call

        static func f(_ x: Float) -> UInt32 { x.bitPattern }

        /// Guest words from a mix of integers and floats.
        static func data(_ values: [Any]) -> UInt32 {
            Q.words(values.map { v -> UInt32 in
                if let x = v as? Float { return x.bitPattern }
                if let x = v as? Double { return Float(x).bitPattern }
                if let x = v as? Int { return UInt32(truncatingIfNeeded: x) }
                return v as! UInt32
            })
        }

        static func type(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

        /// A 32-bit pixmap draw context of w by h cleared to (r, g, b).
        func pixmapContext(_ w: Int, _ h: Int, clear: (Float, Float, Float) = (0, 0, 1)) -> (UInt32, UInt32) {
            let image = tb_NewPtr(Int32(w * h * 4), false, true)
            let d = Self.data([1, Float(1), clear.0, clear.1, clear.2,       // clear with colour (a, r, g, b)
                               Float(0), Float(0), Float(0), Float(0), 0,    // pane, paneState
                               0, 0, 0, 0, 0, 0, 1,                          // mask, maskState, doubleBuffer
                               image, w, h, w * 4, 32, 0, 0, 0])             // pixmap: RGB32, big-endian
            let dc = Q.call("Q3PixmapDrawContext_New", [d])
            #expect(dc != 0)
            return (dc, image)
        }

        /// A view angle aspect camera at (0, 0, 5) looking at the origin.
        func camera() -> UInt32 {
            let d = Self.data([Float(0), Float(0), Float(5), Float(0), Float(0), Float(0), Float(0), Float(1), Float(0),
                               Float(1), Float(10),                            // hither, yon
                               Float(-1), Float(1), Float(2), Float(2),         // view port
                               Float(1.0), Float(1)])                          // fov, aspect
            return Q.call("Q3ViewAngleAspectCamera_New", [d])
        }

        /// A light group with a white directional light along -z.
        func lights() -> UInt32 {
            let group = Q.call("Q3LightGroup_New")
            let d = Self.data([1, Float(1), Float(1), Float(1), Float(1), 0, Float(0), Float(0), Float(-1)])
            let light = Q.call("Q3DirectionalLight_New", [d])
            #expect(light != 0)
            #expect(Q.call("Q3Group_AddObject", [group, light]) != 0)
            Q.call("Q3Object_Dispose", [light])
            return group
        }

        func view(_ dc: UInt32) -> UInt32 {
            let v = Q.call("Q3View_New")
            let cam = camera(), lg = lights()
            #expect(Q.call("Q3View_SetCamera", [v, cam]) == 1)
            #expect(Q.call("Q3View_SetLightGroup", [v, lg]) == 1)
            #expect(Q.call("Q3View_SetDrawContext", [v, dc]) == 1)
            #expect(Q.call("Q3View_SetRendererByType", [v, Self.type("ctwn")]) == 1)
            Q.call("Q3Object_Dispose", [cam])
            Q.call("Q3Object_Dispose", [lg])
            return v
        }

        func px(_ image: UInt32, _ w: Int, _ x: Int, _ y: Int) -> UInt32 { rd32(image + UInt32((y * w + x) * 4)) & 0xFFFFFF }

        /// A red trimesh triangle with its own attribute set.
        func trimesh() -> UInt32 {
            let set = Q.call("Q3AttributeSet_New")
            let red = Self.data([Float(1), Float(0), Float(0)])
            #expect(Q.call("Q3AttributeSet_Add", [set, 5 /* diffuse colour */, red]) == 1)
            let points = Self.data([Float(-1), Float(-1), Float(0), Float(1), Float(-1), Float(0), Float(0), Float(1), Float(0)])
            let tris = Q.words([0, 1, 2])
            let d = Self.data([set, 1, tris, 0, 0, 0, 0, 0, 0, 3, points, 0, 0,
                               Float(0), Float(0), Float(0), Float(0), Float(0), Float(0), 1]) // bBox empty: computed
            let tm = Q.call("Q3TriMesh_New", [d])
            Q.call("Q3Object_Dispose", [set])
            return tm
        }

        @Test func objectsAndHierarchy() {
            launchTestApp()
            #expect(Q.call("Q3IsInitialized") == 0)
            #expect(Q.call("Q3Initialize") == 1)
            #expect(Q.call("Q3IsInitialized") == 1)
            let tm = trimesh()
            #expect(tm != 0)
            #expect(Q.call("Q3Object_GetLeafType", [tm]) == Self.type("tmsh"))
            #expect(Q.call("Q3Object_GetType", [tm]) == Self.type("shrd"))
            #expect(Q.call("Q3Shared_GetType", [tm]) == Self.type("shap"))
            #expect(Q.call("Q3Shape_GetType", [tm]) == Self.type("gmtr"))
            #expect(Q.call("Q3Geometry_GetType", [tm]) == Self.type("tmsh"))
            #expect(Q.call("Q3Object_IsType", [tm, Self.type("gmtr")]) == 1)
            #expect(Q.call("Q3Object_IsDrawable", [tm]) == 1)
            // References: one more, then two disposes.
            #expect(Q.call("Q3Shared_GetReference", [tm]) == tm)
            #expect(Q.call("Q3Shared_IsReferenced", [tm]) == 1)
            #expect(Q.call("Q3Object_Dispose", [tm]) == 1)
            #expect(Q.call("Q3Shared_IsReferenced", [tm]) == 0)
            // The data comes back as given, its bounding box computed.
            let out = tb_NewPtr(80, false, true)
            #expect(Q.call("Q3TriMesh_GetData", [tm, out]) == 1)
            #expect(rd32(out + 4) == 1 && rd32(out + 36) == 3)
            #expect(Q.float(out + 52) == -1 && Q.float(out + 64) == 1 && rd32(out + 76) == 0)
            #expect(Q.float(rd32(out + 40) + 28) == 1) // the third point's y
            let set = rd32(out)
            #expect(set != 0)
            let color = tb_NewPtr(12, false, true)
            #expect(Q.call("Q3AttributeSet_Get", [set, 5, color]) == 1 && Q.float(color) == 1)
            #expect(Q.call("Q3TriMesh_EmptyData", [out]) == 1)
            #expect(rd32(out + 40) == 0)
            // Renderer subclasses and their names.
            let sub = tb_NewPtr(8, false, true)
            #expect(Q.call("Q3ObjectHierarchy_GetSubClassData", [Self.type("rddr"), sub]) == 1)
            #expect(rd32(sub) == 3)
            let name = tb_NewPtr(1024, false, true)
            #expect(Q.call("Q3RendererClass_GetNickNameString", [Self.type("ctwn"), name]) == 1)
            #expect(rd8(name) == UInt8(ascii: "I"))
            #expect(Q.call("Q3ObjectHierarchy_GetStringFromType", [Self.type("tmsh"), name]) == 1)
            #expect(rd8(name) == UInt8(ascii: "T") && rd8(name + 4) == UInt8(ascii: "e"))
            Q.call("Q3ObjectHierarchy_EmptySubClassData", [sub])
            #expect(Q.call("Q3Object_Dispose", [tm]) == 1)
            #expect(Q.call("Q3Object_IsType", [tm, Self.type("gmtr")]) == 0) // gone
            #expect(Q.call("Q3Exit") == 1)
        }

        /// A trimesh lit by a directional light into a pixmap draw
        /// context: red where it is, the clear colour elsewhere.
        @Test func rendersATriMesh() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let (dc, image) = pixmapContext(64, 64)
            let v = view(dc)
            let tm = trimesh()
            var status: UInt32 = 1
            repeat {
                #expect(Q.call("Q3View_StartRendering", [v]) == 1)
                #expect(Q.call("Q3Object_Submit", [tm, v]) == 1)
                status = Q.call("Q3View_EndRendering", [v])
            } while status == 1 // kQ3ViewStatusRetraverse
            #expect(status == 0)
            #expect(px(image, 64, 32, 34) == 0xFF0000)  // the middle: red, facing the light
            #expect(px(image, 64, 2, 2) == 0x0000FF)    // a corner: the clear colour
            #expect(px(image, 64, 32, 6) == 0x0000FF)   // above the apex
            // The camera's frustum: the apex (0, 1, 0) at 1/tan(0.5) of
            // the way up the half-height from the middle.
            let m = tb_NewPtr(64, false, true)
            #expect(Q.call("Q3View_GetFrustumToWindowMatrixState", [v, m]) == 1)
            #expect(Q.float(m) == 32 && Q.float(m + 20) == -32)
            for obj in [tm, v, dc] { Q.call("Q3Object_Dispose", [obj]) }
        }

        /// Immediate mode with the state: a transform and a style, a
        /// triangle submitted from its data, a group with a box in it.
        @Test func immediateModeAndGroups() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let (dc, image) = pixmapContext(64, 64, clear: (0, 0, 0))
            let v = view(dc)
            let green = Q.call("Q3AttributeSet_New")
            #expect(Q.call("Q3AttributeSet_Add", [green, 5, Self.data([Float(0), Float(1), Float(0)])]) == 1)
            // A triangle on the right, moved there by a translation.
            let tri = Self.data([Float(-0.5), Float(-0.5), Float(0), 0, Float(0.5), Float(-0.5), Float(0), 0,
                                 Float(0), Float(0.5), Float(0), 0, green])
            #expect(Q.call("Q3View_StartRendering", [v]) == 1)
            #expect(Q.call("Q3Push_Submit", [v]) == 1)
            #expect(Q.call("Q3TranslateTransform_Submit", [Self.data([Float(1.2), Float(0), Float(0)]), v]) == 1)
            #expect(Q.call("Q3Triangle_Submit", [tri, v]) == 1)
            #expect(Q.call("Q3Pop_Submit", [v]) == 1)
            // A unit box on the left, in a display group, unlit (null illumination).
            let group = Q.call("Q3DisplayGroup_New")
            let null = Q.call("Q3NULLIllumination_New")
            let box = Q.call("Q3Box_New", [Self.data([Float(-1.8), Float(-0.4), Float(0), Float(0), Float(0.8), Float(0),
                                                      Float(0), Float(0), Float(0.8), Float(0.8), Float(0), Float(0), 0, 0])])
            #expect(box != 0)
            #expect(Q.call("Q3Group_AddObject", [group, null]) != 0)
            #expect(Q.call("Q3Group_AddObject", [group, box]) != 0)
            var n: UInt32 = 0
            let count = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Group_CountObjects", [group, count]) == 1)
            n = rd32(count)
            #expect(n == 2)
            #expect(Q.call("Q3Object_Submit", [group, v]) == 1)
            #expect(Q.call("Q3View_EndRendering", [v]) == 0)
            #expect(px(image, 64, 46, 33) == 0x00FF00)  // the triangle, moved right
            #expect(px(image, 64, 32, 32) == 0x000000)  // nothing in the middle
            #expect(px(image, 64, 12, 30) == 0xFFFFFF)  // the box: white, unlit
            // The group's bounds, through the bounding loop.
            let bb = tb_NewPtr(28, false, true)
            #expect(Q.call("Q3View_StartBoundingBox", [v, 0]) == 1)
            #expect(Q.call("Q3Object_Submit", [group, v]) == 1)
            #expect(Q.call("Q3View_EndBoundingBox", [v, bb]) == 0)
            #expect(abs(Q.float(bb) + 1.8) < 1e-5 && abs(Q.float(bb + 12) + 1.0) < 1e-5 && rd32(bb + 24) == 0)
            for obj in [green, group, null, box, v, dc] { Q.call("Q3Object_Dispose", [obj]) }
        }

        /// A window point pick finds the trimesh under the point, not
        /// beside it.
        @Test func picksWhatIsUnderThePoint() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let (dc, _) = pixmapContext(64, 64)
            let v = view(dc)
            let tm = trimesh()
            func pick(_ x: Float, _ y: Float) -> UInt32 {
                let pk = Q.call("Q3WindowPointPick_New", [Self.data([1, 4 /* object */, 0, x, y, Float(2), Float(2)])])
                #expect(Q.call("Q3View_StartPicking", [v, pk]) == 1)
                #expect(Q.call("Q3Object_Submit", [tm, v]) == 1)
                #expect(Q.call("Q3View_EndPicking", [v]) == 0)
                let n = tb_NewPtr(4, false, true)
                #expect(Q.call("Q3Pick_GetNumHits", [pk, n]) == 1)
                let hits = rd32(n)
                if hits > 0 {
                    let obj = tb_NewPtr(4, false, true)
                    #expect(Q.call("Q3Pick_GetPickDetailData", [pk, 0, 4, obj]) == 1)
                    #expect(rd32(obj) == tm)
                    Q.call("Q3Object_Dispose", [rd32(obj)])
                }
                Q.call("Q3Object_Dispose", [pk])
                return hits
            }
            #expect(pick(32, 34) == 1)
            #expect(pick(4, 4) == 0)
            for obj in [tm, v, dc] { Q.call("Q3Object_Dispose", [obj]) }
        }

        /// A compressed pixmap texture ('raw ', 32-bit) decoded and drawn
        /// on a box: its colour, unlit, where the box is.
        @Test func compressedPixmapTexture() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let (dc, image) = pixmapContext(64, 64, clear: (0, 0, 0))
            let v = view(dc)
            var desc = [UInt8](repeating: 0, count: 86)
            desc[3] = 86
            desc[4...7] = [0x72, 0x61, 0x77, 0x20] // 'raw '
            desc[33] = 2; desc[35] = 2; desc[83] = 32
            let d = tb_NewPtr(86, false, true)
            for (i, b) in desc.enumerated() { wr8(d + UInt32(i), b) }
            let pixels = Q.words([0xFF00_FF00, 0xFF00_FF00, 0xFF00_FF00, 0xFF00_FF00])
            let descStorage = Q.call("Q3MemoryStorage_New", [d, 86])
            let dataStorage = Q.call("Q3MemoryStorage_New", [pixels, 16])
            #expect(descStorage != 0 && dataStorage != 0)
            let tex = Q.call("Q3CompressedPixmapTexture_New", [Q.words([dataStorage, 0, descStorage, 0, 2, 2, 32, 0])])
            #expect(tex != 0)
            let w = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Texture_GetWidth", [tex, w]) == 1 && rd32(w) == 2)
            let shader = Q.call("Q3TextureShader_New", [tex])
            let null = Q.call("Q3NULLIllumination_New")
            let box = Q.call("Q3Box_New", [Self.data([Float(-1), Float(-1), Float(0), Float(0), Float(2), Float(0),
                                                      Float(0), Float(0), Float(1), Float(2), Float(0), Float(0), 0, 0])])
            #expect(Q.call("Q3View_StartRendering", [v]) == 1)
            #expect(Q.call("Q3Object_Submit", [null, v]) == 1)
            #expect(Q.call("Q3Object_Submit", [shader, v]) == 1)
            #expect(Q.call("Q3Object_Submit", [box, v]) == 1)
            #expect(Q.call("Q3View_EndRendering", [v]) == 0)
            #expect(px(image, 64, 32, 32) == 0x00FF00)
            #expect(px(image, 64, 1, 1) == 0x000000)
            for obj in [box, null, shader, tex, descStorage, dataStorage, v, dc] { Q.call("Q3Object_Dispose", [obj]) }
        }
    }
}
