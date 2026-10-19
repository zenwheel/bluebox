import Testing
import Toolbox

/// QuickTime's matrix and fixed-point utilities ($ABC2).
extension BlueBox {
    @Suite struct Matrix {
        func rect(_ t: Int, _ l: Int, _ b: Int, _ r: Int) -> UInt32 {
            let p = tb_NewPtr(8, false, true)
            for (i, v) in [t, l, b, r].enumerated() { wr16(p + UInt32(i * 2), UInt16(bitPattern: Int16(v))) }
            return p
        }

        @Test func mappingsCompose() {
            launchTestApp()
            _ = initToolbox()
            let m = tb_NewPtr(36, false, true)
            tb_SetIdentityMatrix(m)
            #expect(tb_GetMatrixType(m) == 0)
            tb_ScaleMatrix(m, 2 << 16, 2 << 16, 0, 0)
            tb_TranslateMatrix(m, 10 << 16, 20 << 16)
            #expect(tb_GetMatrixType(m) == 3)                         // scale and translate
            let r = rect(0, 0, 10, 10)
            #expect(tb_TransformRect(m, r, 0) != 0)
            #expect(rd16(r) == 20 && rd16(r + 2) == 10 && rd16(r + 4) == 40 && rd16(r + 6) == 30)
            let inv = tb_NewPtr(36, false, true)
            #expect(tb_InverseMatrix(m, inv) != 0)
            tb_ConcatMatrix(m, inv)                                    // m then its inverse
            #expect(tb_GetMatrixType(inv) == 0)
            tb_RectMatrix(m, rect(0, 0, 10, 10), rect(5, 5, 25, 45))
            let pt = tb_NewPtr(4, false, true)
            wr16(pt, 10); wr16(pt + 2, 10)                             // v, h
            #expect(tb_TransformPoints(m, pt, 1) == 0)
            #expect(rd16(pt) == 25 && rd16(pt + 2) == 45)
            tb_SetIdentityMatrix(m)
            tb_RotateMatrix(m, 90 << 16, 0, 0)
            #expect(tb_GetMatrixType(m) == 4)                          // linear
        }

        @Test func fixedArithmetic() {
            #expect(tb_FixMulDiv(3 << 16, 4 << 16, 6 << 16) == 2 << 16)
            let w = tb_NewPtr(8, false, true)
            tb_CompMul(0x4000_0000, 8, w)                              // 2^33
            #expect(rd32(w) == 2 && rd32(w + 4) == 0)
            let rem = tb_NewPtr(4, false, true)
            #expect(tb_CompSquareRoot(w) == 92681)
            tb_CompMul(1_000_000, 1_000_007, w)
            #expect(tb_CompDiv(w, 1_000_000, rem) == 1_000_007 && rd32(rem) == 0)
            let c = tb_NewPtr(4, false, true)
            let s = tb_FracSinCos(30 << 16, c)                         // sin 30 = 0.5 (Fract)
            #expect(abs(s - 0x2000_0000) < 4)
        }
    }
}
