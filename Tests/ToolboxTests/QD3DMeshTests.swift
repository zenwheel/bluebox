import CCore
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// QuickDraw 3D's mesh: made a vertex and a face at a time, walked
    /// with iterators, and drawn with its holes.
    @Suite struct QD3DMesh {
        typealias Q = Q3Call
        typealias D = QuickDraw3D

        func vertex(_ mesh: UInt32, _ x: Float, _ y: Float, set: UInt32 = 0) -> UInt32 {
            let v = Q.call("Q3Mesh_VertexNew", [mesh, D.data([x, y, Float(0), set])])
            #expect(v != 0)
            return v
        }

        func face(_ mesh: UInt32, _ vs: [UInt32], set: UInt32 = 0) -> UInt32 {
            let f = Q.call("Q3Mesh_FaceNew", [mesh, UInt32(vs.count), Q.words(vs), set])
            #expect(f != 0)
            return f
        }

        func colour(_ r: Float, _ g: Float, _ b: Float) -> UInt32 {
            let set = Q.call("Q3AttributeSet_New")
            #expect(Q.call("Q3AttributeSet_Add", [set, 5, D.data([r, g, b])]) == 1)
            return set
        }

        func number(_ name: String, _ args: [UInt32]) -> UInt32 {
            let out = tb_NewPtr(4, false, true)
            #expect(Q.call(name, args + [out]) == 1)
            return rd32(out)
        }

        /// Everything a walk gives.
        func walk(_ first: String, _ next: String, _ of: UInt32) -> [UInt32] {
            let it = tb_NewPtr(20, false, true)
            var out: [UInt32] = []
            var h = Q.call(first, [of, it])
            while h != 0 && out.count < 100 {
                out.append(h)
                h = Q.call(next, [it])
            }
            return out
        }

        /// A square face with a square hole, and a second face beside it
        /// sharing an edge.
        func build() -> (mesh: UInt32, a: UInt32, b: UInt32, v: [UInt32]) {
            let mesh = Q.call("Q3Mesh_New")
            #expect(mesh != 0)
            let v = [vertex(mesh, -1.5, -1), vertex(mesh, 0.5, -1), vertex(mesh, 0.5, 1), vertex(mesh, -1.5, 1),
                     vertex(mesh, 1.5, -1), vertex(mesh, 1.5, 1)]
            let red = colour(1, 0, 0), green = colour(0, 1, 0)
            let a = face(mesh, [v[0], v[1], v[2], v[3]], set: red)
            let b = face(mesh, [v[1], v[4], v[5], v[2]], set: green)
            let h = [vertex(mesh, -0.9, -0.4), vertex(mesh, -0.9, 0.4), vertex(mesh, -0.1, 0.4), vertex(mesh, -0.1, -0.4)]
            let hole = face(mesh, h)
            let contour = Q.call("Q3Mesh_FaceToContour", [mesh, a, hole])
            #expect(contour != 0)
            Q.call("Q3Object_Dispose", [red])
            Q.call("Q3Object_Dispose", [green])
            return (mesh, a, b, v + h)
        }

        @Test func topologyAndWalks() {
            launchTestApp()
            #expect(Q.call("Q3Initialize") == 1)
            let (mesh, a, b, v) = build()
            #expect(number("Q3Mesh_GetNumVertices", [mesh]) == 10)
            #expect(number("Q3Mesh_GetNumFaces", [mesh]) == 2)
            #expect(number("Q3Mesh_GetNumEdges", [mesh]) == 11)
            #expect(number("Q3Mesh_GetNumComponents", [mesh]) == 1)
            #expect(number("Q3Mesh_GetOrientable", [mesh]) == 1)
            #expect(number("Q3Mesh_GetFaceNumContours", [mesh, a]) == 2)
            #expect(number("Q3Mesh_GetFaceNumVertices", [mesh, a]) == 8)
            #expect(number("Q3Mesh_GetFaceIndex", [mesh, b]) == 1)
            #expect(number("Q3Mesh_GetVertexIndex", [mesh, v[4]]) == 4)
            #expect(walk("Q3Mesh_FirstMeshFace", "Q3Mesh_NextMeshFace", mesh) == [a, b])
            #expect(walk("Q3Mesh_FirstMeshVertex", "Q3Mesh_NextMeshVertex", mesh).count == 10)
            #expect(walk("Q3Mesh_FirstFaceContour", "Q3Mesh_NextFaceContour", a).count == 2)
            #expect(walk("Q3Mesh_FirstFaceFace", "Q3Mesh_NextFaceFace", a) == [b])
            #expect(walk("Q3Mesh_FirstVertexFace", "Q3Mesh_NextVertexFace", v[1]) == [a, b])
            #expect(walk("Q3Mesh_FirstVertexVertex", "Q3Mesh_NextVertexVertex", v[1]).count == 3)
            let comps = walk("Q3Mesh_FirstMeshComponent", "Q3Mesh_NextMeshComponent", mesh)
            #expect(comps.count == 1)
            #expect(number("Q3Mesh_GetComponentNumVertices", [mesh, comps[0]]) == 10)
            // The shared edge: two faces, not on the boundary; v[0] is on it.
            let edges = walk("Q3Mesh_FirstFaceEdge", "Q3Mesh_NextFaceEdge", b)
            #expect(edges.count == 4)
            let f1 = tb_NewPtr(4, false, true), f2 = tb_NewPtr(4, false, true)
            var shared: UInt32 = 0
            for e in edges {
                #expect(Q.call("Q3Mesh_GetEdgeFaces", [mesh, e, f1, f2]) == 1)
                if rd32(f2) != 0 { shared = e }
            }
            #expect(shared != 0)
            #expect(number("Q3Mesh_GetEdgeOnBoundary", [mesh, shared]) == 0)
            #expect(number("Q3Mesh_GetVertexOnBoundary", [mesh, v[0]]) == 1)
            // Edge, face and corner attribute sets.
            let blue = colour(0, 0, 1)
            #expect(Q.call("Q3Mesh_SetEdgeAttributeSet", [mesh, shared, blue]) == 1)
            #expect(number("Q3Mesh_GetEdgeAttributeSet", [mesh, shared]) == blue)
            Q.call("Q3Object_Dispose", [blue])
            #expect(Q.call("Q3Mesh_SetCornerAttributeSet", [mesh, v[4], b, blue]) == 1)
            #expect(number("Q3Mesh_GetNumCorners", [mesh]) == 1)
            #expect(Q.call("Q3Mesh_SetCornerAttributeSet", [mesh, v[0], b, blue]) == 0) // not b's vertex
            Q.call("Q3Object_Dispose", [blue])
            let plane = tb_NewPtr(16, false, true)
            #expect(Q.call("Q3Mesh_GetFacePlaneEquation", [mesh, a, plane]) == 1)
            #expect(Q.float(plane + 8) == 1 && Q.float(plane + 12) == 0)
            // A copy is its own; deleting a vertex takes its faces.
            let copy = Q.call("Q3Object_Duplicate", [mesh])
            #expect(number("Q3Mesh_GetNumEdges", [copy]) == 11)
            #expect(Q.call("Q3Mesh_VertexDelete", [mesh, v[4]]) == 1)
            #expect(number("Q3Mesh_GetNumFaces", [mesh]) == 1)
            #expect(number("Q3Mesh_GetNumFaces", [copy]) == 2)
            // A hole made a face again.
            let conts = walk("Q3Mesh_FirstFaceContour", "Q3Mesh_NextFaceContour", a)
            let made = Q.call("Q3Mesh_ContourToFace", [mesh, conts[1]])
            #expect(made != 0 && made != a)
            #expect(number("Q3Mesh_GetFaceNumContours", [mesh, a]) == 1)
            #expect(number("Q3Mesh_GetNumFaces", [mesh]) == 2)
            Q.call("Q3Object_Dispose", [copy])
            Q.call("Q3Object_Dispose", [mesh])
        }

        /// Drawn: the faces in their sets' colours, the hole showing the
        /// clear colour.
        @Test func drawsWithItsHole() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let qd = D()
            let (dc, image) = qd.pixmapContext(64, 64)
            let view = qd.view(dc)
            let (mesh, _, _, _) = build()
            #expect(Q.call("Q3View_StartRendering", [view]) == 1)
            #expect(Q.call("Q3Object_Submit", [mesh, view]) == 1)
            #expect(Q.call("Q3View_EndRendering", [view]) == 0)
            #expect(qd.px(image, 64, 18, 32) == 0xFF0000) // face a
            #expect(qd.px(image, 64, 26, 32) == 0x0000FF) // the hole
            #expect(qd.px(image, 64, 44, 32) == 0x00FF00) // face b
            #expect(qd.px(image, 64, 2, 2) == 0x0000FF)
            for o in [mesh, view, dc] { Q.call("Q3Object_Dispose", [o]) }
        }
    }
}
