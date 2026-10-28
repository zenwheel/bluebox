import CCore
import CPUPPC
import Testing
import Toolbox

extension BlueBox {
    /// QuickDraw 3D's maths (QD3DMath.h, the colour routines, bounding
    /// boxes and spheres): row vectors times row-major matrices, results
    /// that may be their inputs.
    @Suite struct QD3DMath {
        typealias Q = Q3Call

        func get(_ p: UInt32, _ n: Int) -> [Float] { (0..<n).map { Q.float(p + UInt32(4 * $0)) } }
        func close(_ a: [Float], _ b: [Float], _ eps: Float = 1e-4) -> Bool {
            a.count == b.count && zip(a, b).allSatisfy { abs($0 - $1) <= eps }
        }

        @Test func vectorsAndPoints() {
            launchTestApp()
            let v = Q.floats([0, 0, 0]), w = Q.floats([0, 0, 0]), r = Q.floats([0, 0, 0])
            #expect(Q.call("Q3Vector3D_Set", [v, 0, 0, 0], fp: [3, 0, 4]) == v)
            #expect(get(v, 3) == [3, 0, 4])
            Q.call("Q3Vector3D_Length", [v])
            #expect(Q.f1 == 5)
            #expect(Q.call("Q3Vector3D_Normalize", [v, v]) == v) // in place
            #expect(close(get(v, 3), [0.6, 0, 0.8]))
            Q.call("Q3Vector3D_Set", [v, 0, 0, 0], fp: [1, 0, 0])
            Q.call("Q3Vector3D_Set", [w, 0, 0, 0], fp: [0, 1, 0])
            Q.call("Q3Vector3D_Cross", [v, w, r])
            #expect(get(r, 3) == [0, 0, 1])
            Q.call("Q3Vector3D_Dot", [v, w])
            #expect(Q.f1 == 0)
            Q.call("Q3Vector3D_Set", [r, 0, 0, 0], fp: [0, 0, 0])
            Q.call("Q3Vector3D_Normalize", [r, r])
            #expect(get(r, 3) == [0, 0, 0]) // zero stays zero
            let p = Q.floats([1, 2, 3]), q = Q.floats([4, 6, 3])
            Q.call("Q3Point3D_Distance", [p, q])
            #expect(Q.f1 == 5)
            Q.call("Q3Point3D_Subtract", [q, p, r])
            #expect(get(r, 3) == [3, 4, 0])
            Q.call("Q3Point3D_RRatio", [p, q, 0, 0, r], fp: [1, 1])
            #expect(get(r, 3) == [2.5, 4, 3])
            // A pointer after floats takes the slot past them.
            let s = Q.floats([0, 0, 0, 0, 0])
            Q.call("Q3BoundingSphere_Set", [s, p, 0, 1], fp: [2])
            #expect(get(s, 4) == [1, 2, 3, 2] && rd32(s + 16) == 1)
            let pts2 = Q.floats([0, 0, 0, 4, 4, 4]), w2 = Q.floats([0.75, 0.25])
            Q.call("Q3Point3D_AffineComb", [pts2, w2, 2, r])
            #expect(get(r, 3) == [1, 1, 1])
            let sp = Q.floats([0, 0, 0])
            Q.call("Q3Point3D_ToSpherical", [Q.floats([0, 0, 2]), sp])
            #expect(close(get(sp, 3), [2, 0, 0]))
            Q.call("Q3SphericalPoint_ToPoint3D", [Q.floats([2, 0, Float.pi / 2]), r])
            #expect(close(get(r, 3), [2, 0, 0]))
        }

        @Test func matrices() {
            launchTestApp()
            let m = Q.floats([Float](repeating: 0, count: 16)), n = Q.floats([Float](repeating: 0, count: 16))
            let r = Q.floats([Float](repeating: 0, count: 16))
            Q.call("Q3Matrix4x4_SetTranslate", [m, 0, 0, 0], fp: [1, 2, 3])
            let p = Q.floats([1, 1, 1]), out = Q.floats([0, 0, 0])
            Q.call("Q3Point3D_Transform", [p, m, out])
            #expect(get(out, 3) == [2, 3, 4])
            Q.call("Q3Vector3D_Transform", [p, m, out])
            #expect(get(out, 3) == [1, 1, 1]) // vectors don't translate
            // Rotate a quarter turn about Z: x goes to y.
            Q.call("Q3Matrix4x4_SetRotate_Z", [n, 0], fp: [Double.pi / 2])
            Q.call("Q3Point3D_Transform", [Q.floats([1, 0, 0]), n, out])
            #expect(close(get(out, 3), [0, 1, 0]))
            // Translate then rotate (A * B applies A first).
            Q.call("Q3Matrix4x4_Multiply", [m, n, r])
            Q.call("Q3Point3D_Transform", [Q.floats([0, 0, 0]), r, out])
            #expect(close(get(out, 3), [-2, 1, 3]))
            Q.call("Q3Matrix4x4_Invert", [r, r]) // in place
            Q.call("Q3Point3D_Transform", [Q.floats([-2, 1, 3]), r, out])
            #expect(close(get(out, 3), [0, 0, 0]))
            Q.call("Q3Matrix4x4_SetScale", [m, 0, 0, 0], fp: [2, 3, 4])
            Q.call("Q3Matrix4x4_Determinant", [m])
            #expect(Q.f1 == 24)
            // A singular matrix leaves the result alone.
            Q.call("Q3Matrix4x4_SetScale", [n, 0, 0, 0], fp: [0, 1, 1])
            Q.call("Q3Matrix4x4_SetIdentity", [r])
            #expect(Q.call("Q3Matrix4x4_Invert", [n, r]) == r)
            #expect(get(r, 16)[0] == 1)
            // X then Y then Z.
            Q.call("Q3Matrix4x4_SetRotate_XYZ", [m, 0, 0, 0], fp: [Double.pi / 2, 0, Double.pi / 2])
            Q.call("Q3Point3D_Transform", [Q.floats([0, 1, 0]), m, out])
            #expect(close(get(out, 3), [0, 0, 1])) // y -> z by X, z stays under Z
            Q.call("Q3Matrix4x4_SetRotateAboutAxis", [m, Q.floats([1, 0, 0]), Q.floats([0, 0, 1]), 0], fp: [Double.pi])
            Q.call("Q3Point3D_Transform", [Q.floats([2, 0, 0]), m, out])
            #expect(close(get(out, 3), [0, 0, 0]))
            Q.call("Q3Matrix4x4_SetRotateVectorToVector", [m, Q.floats([1, 0, 0]), Q.floats([0, 0, 1])])
            Q.call("Q3Vector3D_Transform", [Q.floats([1, 0, 0]), m, out])
            #expect(close(get(out, 3), [0, 0, 1]))
            // Arrays with their own struct sizes.
            let src = Q.floats([1, 0, 0, 99, 0, 1, 0, 99]), dst = Q.floats([Float](repeating: 0, count: 8))
            Q.call("Q3Matrix4x4_SetTranslate", [n, 0, 0, 0], fp: [10, 0, 0])
            #expect(Q.call("Q3Point3D_To4DTransformArray", [src, n, dst, 2, 16, 16]) == 1)
            #expect(get(dst, 8) == [11, 0, 0, 1, 10, 1, 0, 1])
            let m3 = Q.floats([Float](repeating: 0, count: 9))
            Q.call("Q3Matrix3x3_SetTranslate", [m3, 0, 0], fp: [5, 6])
            let p2 = Q.floats([1, 1])
            Q.call("Q3Point2D_Transform", [p2, m3, p2])
            #expect(get(p2, 2) == [6, 7])
            Q.call("Q3Matrix3x3_Adjoint", [Q.floats([2, 0, 0, 0, 3, 0, 0, 0, 4]), m3])
            #expect(get(m3, 9) == [12, 0, 0, 0, 8, 0, 0, 0, 6])
        }

        @Test func quaternions() {
            launchTestApp()
            let q = Q.floats([0, 0, 0, 0]), r = Q.floats([0, 0, 0, 0]), out = Q.floats([0, 0, 0])
            Q.call("Q3Quaternion_SetRotate_Z", [q, 0], fp: [Double.pi / 2])
            Q.call("Q3Vector3D_TransformQuaternion", [Q.floats([1, 0, 0]), q, out])
            #expect(close(get(out, 3), [0, 1, 0]))
            // Its matrix agrees with the matrix routines'.
            let m = Q.floats([Float](repeating: 0, count: 16)), z = Q.floats([Float](repeating: 0, count: 16))
            Q.call("Q3Matrix4x4_SetQuaternion", [m, q])
            Q.call("Q3Matrix4x4_SetRotate_Z", [z, 0], fp: [Double.pi / 2])
            #expect(close(get(m, 16), get(z, 16)))
            // And back.
            Q.call("Q3Quaternion_SetMatrix", [r, m])
            #expect(close(get(r, 4), get(q, 4)))
            // XYZ composes in the matrices' order.
            Q.call("Q3Quaternion_SetRotate_XYZ", [q, 0, 0, 0], fp: [0.3, 0.5, 0.7])
            Q.call("Q3Matrix4x4_SetQuaternion", [m, q])
            Q.call("Q3Matrix4x4_SetRotate_XYZ", [z, 0, 0, 0], fp: [0.3, 0.5, 0.7])
            #expect(close(get(m, 16), get(z, 16)))
            // q times its inverse is the identity.
            Q.call("Q3Quaternion_Invert", [q, r])
            Q.call("Q3Quaternion_Multiply", [q, r, r])
            #expect(close(get(r, 4), [1, 0, 0, 0]))
            Q.call("Q3Quaternion_SetIdentity", [r])
            #expect(Q.call("Q3Quaternion_IsIdentity", [r]) == 1)
            // Halfway between identity and a third of a turn about Z.
            Q.call("Q3Quaternion_SetRotate_Z", [q, 0], fp: [2 * Double.pi / 3])
            Q.call("Q3Quaternion_InterpolateLinear", [r, q, 0, r], fp: [0.5])
            Q.call("Q3Vector3D_TransformQuaternion", [Q.floats([1, 0, 0]), r, out])
            #expect(close(get(out, 3), [0.5, Float(3.0).squareRoot() / 2, 0]))
        }

        @Test func boundsAndColours() {
            launchTestApp()
            let b = Q.floats([0, 0, 0, 0, 0, 0, 0])
            let pts = Q.floats([1, 5, -1, 99, -2, 0, 3, 99])
            Q.call("Q3BoundingBox_SetFromPoints3D", [b, pts, 2, 16])
            #expect(get(b, 6) == [-2, 0, -1, 1, 5, 3] && rd32(b + 24) == 0)
            let e = Q.floats([0, 0, 0, 0, 0, 0])
            wr32(e + 24, 1)
            let u = Q.floats([0, 0, 0, 0, 0, 0, 0])
            Q.call("Q3BoundingBox_Union", [e, b, u])
            #expect(get(u, 6) == get(b, 6) && rd32(u + 24) == 0)
            Q.call("Q3BoundingBox_UnionPoint3D", [b, Q.floats([10, 0, 0]), b])
            #expect(get(b, 6)[3] == 10)
            let s = Q.floats([0, 0, 0, 0, 0])
            Q.call("Q3BoundingSphere_SetFromPoints3D", [s, Q.floats([-1, 0, 0, 1, 0, 0]), 2, 12])
            #expect(get(s, 4) == [0, 0, 0, 1])
            let col = Q.floats([0, 0, 0]), lum = Q.floats([0])
            Q.call("Q3ColorRGB_Set", [col, 0, 0, 0], fp: [2, -1, 0.5])
            Q.call("Q3ColorRGB_Clamp", [col, col])
            #expect(get(col, 3) == [1, 0, 0.5])
            Q.call("Q3ColorRGB_Set", [col, 0, 0, 0], fp: [1, 1, 1])
            #expect(Q.call("Q3ColorRGB_Luminance", [col, lum]) == lum)
            #expect(abs(Q.float(lum) - 1) < 1e-6)
        }
    }
}
