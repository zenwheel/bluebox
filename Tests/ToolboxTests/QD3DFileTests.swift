import CCore
import CPUPPC
import Foundation
import Testing
import Toolbox

extension BlueBox {
    /// QuickDraw 3D's storage and 3DMF files: metafiles built here, in
    /// binary and text, read through memory storage, drawn, written back
    /// and read again.
    @Suite struct QD3DFile {
        typealias Q = Q3Call

        static func type(_ s: String) -> UInt32 { s.utf8.reduce(0) { $0 << 8 | UInt32($1) } }

        // Binary 3DMF pieces, big-endian.
        static func u32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8(v >> 16 & 0xFF), UInt8(v >> 8 & 0xFF), UInt8(v & 0xFF)] }
        static func f32(_ v: Float) -> [UInt8] { u32(v.bitPattern) }
        static func floats(_ v: [Float]) -> [UInt8] { v.flatMap { f32($0) } }
        static func obj(_ t: String, _ data: [UInt8]) -> [UInt8] {
            var d = data
            while d.count % 4 != 0 { d.append(0) }
            return u32(type(t)) + u32(UInt32(d.count)) + d
        }
        static func container(_ parts: [[UInt8]]) -> [UInt8] { obj("cntr", parts.flatMap { $0 }) }
        static func file(_ objects: [[UInt8]]) -> [UInt8] {
            obj("3DMF", u32(0x0001_0006) + u32(0) + u32(0) + u32(0)) + objects.flatMap { $0 }
        }

        /// A red trimesh triangle, as a container with its attribute set.
        static var redTriMesh: [UInt8] {
            container([
                obj("tmsh", u32(1) + u32(0) + u32(0) + u32(0) + u32(3) + u32(0) + [0, 1, 2] +
                    floats([-1, -1, 0, 1, -1, 0, 0, 1, 0]) + floats([-1, -1, 0, 1, 1, 0]) + u32(0)),
                container([obj("attr", []), obj("kdif", floats([1, 0, 0]))]),
            ])
        }

        func storage(_ bytes: [UInt8]) -> UInt32 {
            let p = tb_NewPtr(Int32(bytes.count), false, false)
            for (i, b) in bytes.enumerated() { wr8(p + UInt32(i), b) }
            let s = Q.call("Q3MemoryStorage_New", [p, UInt32(bytes.count)])
            tb_DisposePtr(p)
            return s
        }

        /// Every object of a metafile in these bytes.
        func readAll(_ bytes: [UInt8], mode: UInt32? = nil) -> [UInt32] {
            let st = storage(bytes)
            let f = Q.call("Q3File_New")
            #expect(Q.call("Q3File_SetStorage", [f, st]) == 1)
            let m = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3File_OpenRead", [f, m]) == 1)
            if let mode { #expect(rd32(m) == mode) }
            var objs: [UInt32] = []
            while Q.call("Q3File_IsEndOfFile", [f]) == 0 {
                let o = Q.call("Q3File_ReadObject", [f])
                if o != 0 { objs.append(o) } else { break }
            }
            #expect(Q.call("Q3File_Close", [f]) == 1)
            Q.call("Q3Object_Dispose", [f])
            Q.call("Q3Object_Dispose", [st])
            return objs
        }

        func leaf(_ o: UInt32) -> UInt32 { Q.call("Q3Object_GetLeafType", [o]) }

        @Test func readsBinaryTriMesh() {
            launchTestApp()
            #expect(Q.call("Q3Initialize") == 1)
            let objs = readAll(Self.file([Self.redTriMesh]), mode: 0)
            #expect(objs.count == 1)
            guard let tm = objs.first else { return }
            #expect(leaf(tm) == Self.type("tmsh"))
            let d = tb_NewPtr(80, false, true)
            #expect(Q.call("Q3TriMesh_GetData", [tm, d]) == 1)
            #expect(rd32(d + 4) == 1 && rd32(d + 36) == 3)
            #expect(rd32(rd32(d + 8) + 8) == 2)                  // the triangle's third index
            #expect(Q.float(rd32(d + 40) + 28) == 1)             // the apex's y
            let color = tb_NewPtr(12, false, true)
            #expect(Q.call("Q3AttributeSet_Get", [rd32(d), 5, color]) == 1 && Q.float(color) == 1)
            Q.call("Q3TriMesh_EmptyData", [d])
            Q.call("Q3Object_Dispose", [tm])
        }

        static let text = """
        3DMetafile ( 1 6 Normal tableofcontents0> )
        # a group with a moved box, and a polygon
        BeginGroup ( DisplayGroup ( ) )
            Translate ( 1 2 3 )
            Container (
                Box ( 0 1 0  0 0 1  1 0 0  0 0 0 )
                Container ( AttributeSet ( ) DiffuseColor ( 0 1 0 ) )
            )
        EndGroup ( )
        Container (
            Polygon ( 4  0 0 0  1 0 0  1 1 0  0 1 0 )
            Container (
                VertexAttributeSetList ( 4 Include 1 2 )
                Container ( AttributeSet ( ) DiffuseColor ( 0 0 1 ) )
            )
        )
        """

        @Test func readsText() {
            launchTestApp()
            #expect(Q.call("Q3Initialize") == 1)
            let objs = readAll(Array(Self.text.utf8), mode: 4)
            #expect(objs.count == 2)
            guard objs.count == 2 else { return }
            let group = objs[0]
            #expect(leaf(group) == Self.type("dspg"))
            let n = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Group_CountObjects", [group, n]) == 1 && rd32(n) == 2)
            let pos = tb_NewPtr(4, false, true), o = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Group_GetFirstPosition", [group, pos]) == 1)
            #expect(Q.call("Q3Group_GetPositionObject", [group, rd32(pos), o]) == 1)
            #expect(leaf(rd32(o)) == Self.type("trns"))
            let v = tb_NewPtr(12, false, true)
            #expect(Q.call("Q3TranslateTransform_Get", [rd32(o), v]) == 1 && Q.float(v + 8) == 3)
            Q.call("Q3Object_Dispose", [rd32(o)])
            #expect(Q.call("Q3Group_GetNextPosition", [group, pos]) == 1)
            #expect(Q.call("Q3Group_GetPositionObject", [group, rd32(pos), o]) == 1)
            #expect(leaf(rd32(o)) == Self.type("box "))
            #expect(Q.call("Q3Box_GetMinorAxis", [rd32(o), v]) == 1 && Q.float(v) == 1)
            let set = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Geometry_GetAttributeSet", [rd32(o), set]) == 1 && rd32(set) != 0)
            Q.call("Q3Object_Dispose", [rd32(set)])
            Q.call("Q3Object_Dispose", [rd32(o)])
            // The polygon: its vertex 2's own set, none on vertex 0.
            let poly = objs[1]
            #expect(leaf(poly) == Self.type("plyg"))
            #expect(Q.call("Q3Polygon_GetVertexAttributeSet", [poly, 2, set]) == 1 && rd32(set) != 0)
            let c = tb_NewPtr(12, false, true)
            #expect(Q.call("Q3AttributeSet_Get", [rd32(set), 5, c]) == 1 && Q.float(c + 8) == 1)
            Q.call("Q3Object_Dispose", [rd32(set)])
            #expect(Q.call("Q3Polygon_GetVertexAttributeSet", [poly, 0, set]) == 1 && rd32(set) == 0)
            for o in objs { Q.call("Q3Object_Dispose", [o]) }
        }

        /// What was read, drawn: the trimesh red in the middle.
        @Test func drawsWhatItReads() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let group = readAll(Self.file([Self.redTriMesh])).first ?? 0
            #expect(group != 0)
            let image = tb_NewPtr(64 * 64 * 4, false, true)
            func data(_ v: [Any]) -> UInt32 {
                Q.words(v.map { x -> UInt32 in
                    if let f = x as? Float { return f.bitPattern }
                    if let i = x as? Int { return UInt32(truncatingIfNeeded: i) }
                    return x as! UInt32
                })
            }
            let dc = Q.call("Q3PixmapDrawContext_New", [data([1, Float(1), Float(0), Float(0), Float(1),
                                                              Float(0), Float(0), Float(0), Float(0), 0,
                                                              0, 0, 0, 0, 0, 0, 1, image, 64, 64, 256, 32, 0, 0, 0])])
            let cam = Q.call("Q3ViewAngleAspectCamera_New", [data([Float(0), Float(0), Float(5), Float(0), Float(0), Float(0),
                                                                   Float(0), Float(1), Float(0), Float(1), Float(10),
                                                                   Float(-1), Float(1), Float(2), Float(2), Float(1), Float(1)])])
            let view = Q.call("Q3View_New")
            #expect(Q.call("Q3View_SetCamera", [view, cam]) == 1)
            #expect(Q.call("Q3View_SetDrawContext", [view, dc]) == 1)
            let null = Q.call("Q3NULLIllumination_New")
            #expect(Q.call("Q3View_StartRendering", [view]) == 1)
            #expect(Q.call("Q3Object_Submit", [null, view]) == 1)
            #expect(Q.call("Q3Object_Submit", [group, view]) == 1)
            #expect(Q.call("Q3View_EndRendering", [view]) == 0)
            #expect(rd32(image + UInt32((34 * 64 + 32) * 4)) & 0xFFFFFF == 0xFF0000)
            #expect(rd32(image + UInt32((2 * 64 + 2) * 4)) & 0xFFFFFF == 0x0000FF)
            for o in [group, dc, cam, view, null] { Q.call("Q3Object_Dispose", [o]) }
        }

        /// Objects written through a view's write loop, binary and text,
        /// and read back the same.
        @Test func writesAndReadsBack() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            let objs = readAll(Array(Self.text.utf8)) + readAll(Self.file([Self.redTriMesh]))
            #expect(objs.count == 3)
            for mode: UInt32 in [0, 4] {
                let st = Q.call("Q3MemoryStorage_New", [0, 0])
                let f = Q.call("Q3File_New")
                #expect(Q.call("Q3File_SetStorage", [f, st]) == 1)
                #expect(Q.call("Q3File_OpenWrite", [f, mode]) == 1)
                let view = Q.call("Q3View_New")
                #expect(Q.call("Q3View_StartWriting", [view, f]) == 1)
                for o in objs { #expect(Q.call("Q3Object_Submit", [o, view]) == 1) }
                #expect(Q.call("Q3View_EndWriting", [view]) == 0)
                #expect(Q.call("Q3File_Close", [f]) == 1)
                let buf = tb_NewPtr(12, false, true)
                #expect(Q.call("Q3MemoryStorage_GetBuffer", [st, buf, buf + 4, buf + 8]) == 1)
                let bytes = (0..<rd32(buf + 4)).map { rd8(rd32(buf) + $0) }
                let again = readAll(bytes, mode: mode)
                #expect(again.map { leaf($0) } == objs.map { leaf($0) })
                if again.count == 3 {
                    let d = tb_NewPtr(80, false, true)
                    #expect(Q.call("Q3TriMesh_GetData", [again[2], d]) == 1)
                    #expect(rd32(d + 36) == 3 && Q.float(rd32(d + 40) + 12) == 1)
                    let color = tb_NewPtr(12, false, true)
                    #expect(Q.call("Q3AttributeSet_Get", [rd32(d), 5, color]) == 1 && Q.float(color) == 1)
                    Q.call("Q3TriMesh_EmptyData", [d])
                    let n = tb_NewPtr(4, false, true)
                    #expect(Q.call("Q3Group_CountObjects", [again[0], n]) == 1 && rd32(n) == 2)
                }
                for o in again + [f, st, view] { Q.call("Q3Object_Dispose", [o]) }
            }
            for o in objs { Q.call("Q3Object_Dispose", [o]) }
        }

        /// Immediate mode in a writing loop: what is submitted from its
        /// data is written as the object it would make (a transform, a
        /// style, a push and pop, a triangle), in both formats.
        @Test func writesImmediateMode() {
            launchTestApp()
            _ = cfm_ppc_cpu()
            #expect(Q.call("Q3Initialize") == 1)
            func words(_ v: [Float]) -> UInt32 { Q.floats(v) }
            for mode: UInt32 in [0, 4] {
                let st = Q.call("Q3MemoryStorage_New", [0, 0])
                let f = Q.call("Q3File_New")
                #expect(Q.call("Q3File_SetStorage", [f, st]) == 1)
                #expect(Q.call("Q3File_OpenWrite", [f, mode]) == 1)
                let view = Q.call("Q3View_New")
                #expect(Q.call("Q3View_StartWriting", [view, f]) == 1)
                #expect(Q.call("Q3Push_Submit", [view]) == 1)
                #expect(Q.call("Q3TranslateTransform_Submit", [words([1, 2, 3]), view]) == 1)
                #expect(Q.call("Q3BackfacingStyle_Submit", [1, view]) == 1)
                let tri = Q.words([0, 0, 0, 0, 0x3F80_0000, 0, 0, 0, 0, 0x3F80_0000, 0, 0, 0])
                #expect(Q.call("Q3Triangle_Submit", [tri, view]) == 1)
                #expect(Q.call("Q3Pop_Submit", [view]) == 1)
                #expect(Q.call("Q3View_EndWriting", [view]) == 0)
                #expect(Q.call("Q3File_Close", [f]) == 1)
                let buf = tb_NewPtr(12, false, true)
                #expect(Q.call("Q3MemoryStorage_GetBuffer", [st, buf, buf + 4, buf + 8]) == 1)
                let bytes = (0..<rd32(buf + 4)).map { rd8(rd32(buf) + $0) }
                let again = readAll(bytes, mode: mode)
                #expect(again.map { leaf($0) } == ["push", "trns", "bckf", "trng", "pop "].map { Self.type($0) })
                if again.count == 5 {
                    let d = tb_NewPtr(64, false, true)
                    #expect(Q.call("Q3Triangle_GetData", [again[3], d]) == 1)
                    #expect(Q.float(d + 16) == 1 && Q.float(d + 36) == 1)
                    Q.call("Q3Triangle_EmptyData", [d])
                }
                for o in again + [f, st, view] { Q.call("Q3Object_Dispose", [o]) }
            }
        }

        /// The primitives, written then read, in both formats.
        @Test func primitivesRoundTrip() {
            launchTestApp()
            #expect(Q.call("Q3Initialize") == 1)
            for mode: UInt32 in [0, 4] {
                let st = Q.call("Q3MemoryStorage_New", [0, 0])
                let f = Q.call("Q3File_New")
                #expect(Q.call("Q3File_SetStorage", [f, st]) == 1)
                #expect(Q.call("Q3File_OpenWrite", [f, mode]) == 1)
                #expect(Q.call("Q3Uns32_Write", [0x1234_5678, f]) == 1)
                #expect(Q.call("Q3Float32_Write", [0, f], fp: [1.5]) == 1)
                let s = tb_NewPtr(8, false, true)
                wr8(s, 0x68); wr8(s + 1, 0x69)
                #expect(Q.call("Q3String_Write", [s, f]) == 1)
                #expect(Q.call("Q3Int16_Write", [UInt32(bitPattern: -3), f]) == 1)
                let pt = Q.floats([1, 2, 3])
                #expect(Q.call("Q3Point3D_Write", [pt, f]) == 1)
                #expect(Q.call("Q3File_Close", [f]) == 1)
                let m = tb_NewPtr(4, false, true)
                #expect(Q.call("Q3File_OpenRead", [f, m]) == 1)
                let out = tb_NewPtr(64, false, true)
                #expect(Q.call("Q3Uns32_Read", [out, f]) == 1 && rd32(out) == 0x1234_5678)
                #expect(Q.call("Q3Float32_Read", [out, f]) == 1 && Q.float(out) == 1.5)
                let len = tb_NewPtr(4, false, true)
                #expect(Q.call("Q3String_Read", [out, len, f]) == 1 && rd32(len) == 2 && rd8(out + 1) == 0x69)
                #expect(Q.call("Q3Int16_Read", [out, f]) == 1 && Int16(bitPattern: rd16(out)) == -3)
                #expect(Q.call("Q3Point3D_Read", [out, f]) == 1 && Q.float(out + 8) == 3)
                #expect(Q.call("Q3Size_Pad", [5]) == 8)
                #expect(Q.call("Q3File_Close", [f]) == 1)
                Q.call("Q3Object_Dispose", [f])
                Q.call("Q3Object_Dispose", [st])
            }
        }

        /// Storage: memory grows as written, handles too.
        @Test func storageObjects() {
            launchTestApp()
            #expect(Q.call("Q3Initialize") == 1)
            let st = Q.call("Q3MemoryStorage_New", [0, 0])
            #expect(Q.call("Q3Storage_GetType", [st]) == Self.type("mems"))
            let data = tb_NewPtr(3000, false, true)
            wr8(data + 2999, 7)
            let n = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Storage_SetData", [st, 10, 3000, data, n]) == 1 && rd32(n) == 3000)
            #expect(Q.call("Q3Storage_GetSize", [st, n]) == 1 && rd32(n) == 3010)
            let back = tb_NewPtr(4, false, true)
            #expect(Q.call("Q3Storage_GetData", [st, 3009, 4, back, n]) == 1 && rd32(n) == 1 && rd8(back) == 7)
            let h = tb_NewHandle(0, false, true)
            let hs = Q.call("Q3HandleStorage_New", [h, 0])
            #expect(Q.call("Q3MemoryStorage_GetType", [hs]) == Self.type("hndl"))
            #expect(Q.call("Q3Storage_SetData", [hs, 0, 3000, data, n]) == 1)
            #expect(tb_GetHandleSize(h) >= 3000 && rd8(rd32(h) + 2999) == 7)
            for o in [st, hs] { Q.call("Q3Object_Dispose", [o]) }
        }
    }
}
