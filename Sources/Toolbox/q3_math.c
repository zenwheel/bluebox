/*
 * QuickDraw 3D's maths (QD3DMath.h), its colour routines (QD3DShader.h)
 * and bounding boxes and spheres: points, vectors, matrices and
 * quaternions in guest memory, computed in host floats.
 *
 * Conventions as QuickDraw 3D had them: matrices are row-major and
 * points are row vectors (p * M), so a matrix product A * B applies A
 * first; quaternions are w, x, y, z, and Q3Quaternion_Multiply(q1, q2)
 * is likewise the rotation q1 followed by q2.  Every result is computed
 * before any of it is stored, so a result may be one of the inputs, and
 * the routines return their result pointer as the originals did.
 */
#include <math.h>

#include "q3_internal.h"

#define ARG(n) q3_slot(c, n)
#define F(n) q3_farg(c, n)

static void ld(gaddr a, float *v, int n)
{
    for (int i = 0; i < n; i++)
        v[i] = rdf(a + 4 * (uint32_t)i);
}
static void st(gaddr a, const float *v, int n)
{
    for (int i = 0; i < n; i++)
        wrf(a + 4 * (uint32_t)i, v[i]);
}

/* ---------------------------------------------------------------------- */
/* Setting */

static void set_n(ppc_cpu *c, int n)
{
    float v[5];
    for (int i = 0; i < n; i++)
        v[i] = F(i);
    st(ARG(0), v, n);
    Q3RETURN(ARG(0));
}
static void Set2(ppc_cpu *c) { set_n(c, 2); }
static void Set3(ppc_cpu *c) { set_n(c, 3); }
static void Set4(ppc_cpu *c) { set_n(c, 4); }

/* ---------------------------------------------------------------------- */
/* Dimension changes */

static void Q3Point2D_To3D(ppc_cpu *c)
{
    float v[3];
    ld(ARG(0), v, 2);
    v[2] = 1;
    st(ARG(1), v, 3);
    Q3RETURN(ARG(1));
}
/* A homogeneous point down a dimension: divided by its last coordinate. */
static void down(ppc_cpu *c, int n)
{
    float v[4];
    ld(ARG(0), v, n + 1);
    float inv = v[n] != 0 ? 1.0f / v[n] : 1.0f;
    for (int i = 0; i < n; i++)
        v[i] *= inv;
    st(ARG(1), v, n);
    Q3RETURN(ARG(1));
}
static void Down2(ppc_cpu *c) { down(c, 2); }
static void Down3(ppc_cpu *c) { down(c, 3); }
static void Q3Point3D_To4D(ppc_cpu *c)
{
    float v[4];
    ld(ARG(0), v, 3);
    v[3] = 1;
    st(ARG(1), v, 4);
    Q3RETURN(ARG(1));
}

/* ---------------------------------------------------------------------- */
/* Arithmetic */

/* result = a op b, n coordinates. */
static void binop(ppc_cpu *c, int n, float sign)
{
    float a[4], b[4];
    ld(ARG(0), a, n);
    ld(ARG(1), b, n);
    for (int i = 0; i < n; i++)
        a[i] += sign * b[i];
    st(ARG(2), a, n);
    Q3RETURN(ARG(2));
}
static void Add2(ppc_cpu *c) { binop(c, 2, 1); }
static void Add3(ppc_cpu *c) { binop(c, 3, 1); }
static void Sub2(ppc_cpu *c) { binop(c, 2, -1); }
static void Sub3(ppc_cpu *c) { binop(c, 3, -1); }

/* Distances: n coordinates, divided by coordinate n first if rational. */
static float dist2(ppc_cpu *c, int n, bool rational)
{
    float a[4], b[4];
    ld(ARG(0), a, n + rational);
    ld(ARG(1), b, n + rational);
    float s = 0;
    for (int i = 0; i < n; i++) {
        float x = rational ? (a[n] != 0 ? a[i] / a[n] : a[i]) : a[i];
        float y = rational ? (b[n] != 0 ? b[i] / b[n] : b[i]) : b[i];
        s += (x - y) * (x - y);
    }
    return s;
}
static void Dist2(ppc_cpu *c) { q3_return_float(c, sqrtf(dist2(c, 2, false))); }
static void DistSq2(ppc_cpu *c) { q3_return_float(c, dist2(c, 2, false)); }
static void Dist3(ppc_cpu *c) { q3_return_float(c, sqrtf(dist2(c, 3, false))); }
static void DistSq3(ppc_cpu *c) { q3_return_float(c, dist2(c, 3, false)); }
static void DistR3(ppc_cpu *c) { q3_return_float(c, sqrtf(dist2(c, 2, true))); }
static void DistSqR3(ppc_cpu *c) { q3_return_float(c, dist2(c, 2, true)); }
static void DistR4(ppc_cpu *c) { q3_return_float(c, sqrtf(dist2(c, 3, true))); }
static void DistSqR4(ppc_cpu *c) { q3_return_float(c, dist2(c, 3, true)); }

/* The point dividing p1 to p2 in the ratio r1 : r2 (as QuickDraw 3D
 * computed it: p1 + r2 / (r1 + r2) of the way). */
static void rratio(ppc_cpu *c, int n)
{
    float a[4], b[4], r1 = F(0), r2 = F(1);
    ld(ARG(0), a, n);
    ld(ARG(1), b, n);
    float t = r1 + r2 != 0 ? r2 / (r1 + r2) : 0;
    for (int i = 0; i < n; i++)
        a[i] += t * (b[i] - a[i]);
    st(ARG(4), a, n);
    Q3RETURN(ARG(4));
}
static void RRatio2(ppc_cpu *c) { rratio(c, 2); }
static void RRatio3(ppc_cpu *c) { rratio(c, 3); }
static void RRatio4(ppc_cpu *c) { rratio(c, 4); }

static void scale_n(ppc_cpu *c, int n)
{
    float a[3], s = F(0);
    ld(ARG(0), a, n);
    for (int i = 0; i < n; i++)
        a[i] *= s;
    st(ARG(2), a, n);
    Q3RETURN(ARG(2));
}
static void Scale2(ppc_cpu *c) { scale_n(c, 2); }
static void Scale3(ppc_cpu *c) { scale_n(c, 3); }

static float len(gaddr p, int n)
{
    float a[3];
    ld(p, a, n);
    float s = 0;
    for (int i = 0; i < n; i++)
        s += a[i] * a[i];
    return sqrtf(s);
}
static void Len2(ppc_cpu *c) { q3_return_float(c, len(ARG(0), 2)); }
static void Len3(ppc_cpu *c) { q3_return_float(c, len(ARG(0), 3)); }

/* A zero vector stays zero. */
static void normalize_n(ppc_cpu *c, int n)
{
    float a[3];
    ld(ARG(0), a, n);
    float l = len(ARG(0), n), inv = l > 0 ? 1.0f / l : 0;
    for (int i = 0; i < n; i++)
        a[i] *= inv;
    st(ARG(1), a, n);
    Q3RETURN(ARG(1));
}
static void Norm2(ppc_cpu *c) { normalize_n(c, 2); }
static void Norm3(ppc_cpu *c) { normalize_n(c, 3); }

static void negate_n(ppc_cpu *c, int n)
{
    float a[3];
    ld(ARG(0), a, n);
    for (int i = 0; i < n; i++)
        a[i] = -a[i];
    st(ARG(1), a, n);
    Q3RETURN(ARG(1));
}
static void Neg2(ppc_cpu *c) { negate_n(c, 2); }
static void Neg3(ppc_cpu *c) { negate_n(c, 3); }

static void Q3Vector2D_Cross(ppc_cpu *c)
{
    float a[2], b[2];
    ld(ARG(0), a, 2);
    ld(ARG(1), b, 2);
    q3_return_float(c, a[0] * b[1] - a[1] * b[0]);
}

static void cross(const float *a, const float *b, float *r)
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    r[0] = x, r[1] = y, r[2] = z;
}

static void Q3Vector3D_Cross(ppc_cpu *c)
{
    float a[3], b[3];
    ld(ARG(0), a, 3);
    ld(ARG(1), b, 3);
    cross(a, b, a);
    st(ARG(2), a, 3);
    Q3RETURN(ARG(2));
}

static void Q3Point3D_CrossProductTri(ppc_cpu *c)
{
    float p1[3], p2[3], p3[3], u[3], v[3];
    ld(ARG(0), p1, 3);
    ld(ARG(1), p2, 3);
    ld(ARG(2), p3, 3);
    for (int i = 0; i < 3; i++) {
        u[i] = p2[i] - p1[i];
        v[i] = p3[i] - p2[i];
    }
    cross(u, v, u);
    st(ARG(3), u, 3);
    Q3RETURN(ARG(3));
}

static void dot_n(ppc_cpu *c, int n)
{
    float a[4], b[4], s = 0;
    ld(ARG(0), a, n);
    ld(ARG(1), b, n);
    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    q3_return_float(c, s);
}
static void Dot2(ppc_cpu *c) { dot_n(c, 2); }
static void Dot3(ppc_cpu *c) { dot_n(c, 3); }

/* ---------------------------------------------------------------------- */
/* Transforms */

/* v (n coordinates, then w) times the (n+1)-square matrix m. */
static void xform(const float *v, const float *m, int n, float *out)
{
    float r[4];
    for (int j = 0; j <= n; j++) {
        r[j] = 0;
        for (int i = 0; i <= n; i++)
            r[j] += v[i] * m[i * (n + 1) + j];
    }
    for (int j = 0; j <= n; j++)
        out[j] = r[j];
}

/* A vector (no translation), a point (divided by w) or a rational point
 * (as it is), through an n+1 square matrix. */
static void transform(ppc_cpu *c, int n, int kind)
{
    float v[4], m[16];
    ld(ARG(0), v, kind == 2 ? n + 1 : n);
    if (kind != 2)
        v[n] = kind == 0 ? 0 : 1;
    ld(ARG(1), m, (n + 1) * (n + 1));
    xform(v, m, n, v);
    if (kind == 1 && v[n] != 0 && v[n] != 1)
        for (int i = 0; i < n; i++)
            v[i] /= v[n];
    st(ARG(2), v, kind == 2 ? n + 1 : n);
    Q3RETURN(ARG(2));
}
static void XfVec2(ppc_cpu *c) { transform(c, 2, 0); }
static void XfVec3(ppc_cpu *c) { transform(c, 3, 0); }
static void XfPt2(ppc_cpu *c) { transform(c, 2, 1); }
static void XfPt3(ppc_cpu *c) { transform(c, 3, 1); }
static void XfR4(ppc_cpu *c) { transform(c, 3, 2); }

/* An array of points through a matrix, each struct of its own size:
 * in_n coordinates in (4 rational), out_n out (3 divided by w). */
static void transform_array(ppc_cpu *c, int in_n, int out_n)
{
    gaddr in = ARG(0), out = ARG(2);
    int32_t n = (int32_t)ARG(3);
    uint32_t in_size = ARG(4), out_size = ARG(5);
    float m[16];
    if (!in || !out || !ARG(1) || n < 0) {
        Q3RETURN(0);
        return;
    }
    ld(ARG(1), m, 16);
    for (int32_t k = 0; k < n; k++) {
        float v[4];
        ld(in + (uint32_t)k * in_size, v, in_n);
        if (in_n == 3)
            v[3] = 1;
        xform(v, m, 3, v);
        if (out_n == 3 && v[3] != 0 && v[3] != 1)
            for (int i = 0; i < 3; i++)
                v[i] /= v[3];
        st(out + (uint32_t)k * out_size, v, out_n);
    }
    Q3RETURN(1);
}
static void Q3Point3D_To3DTransformArray(ppc_cpu *c) { transform_array(c, 3, 3); }
static void Q3Point3D_To4DTransformArray(ppc_cpu *c) { transform_array(c, 3, 4); }
static void Q3RationalPoint4D_To4DTransformArray(ppc_cpu *c) { transform_array(c, 4, 4); }

/* ---------------------------------------------------------------------- */
/* Polar and spherical points */

static void Q3Point2D_ToPolar(ppc_cpu *c)
{
    float p[2];
    ld(ARG(0), p, 2);
    float r[2] = { sqrtf(p[0] * p[0] + p[1] * p[1]), atan2f(p[1], p[0]) };
    st(ARG(1), r, 2);
    Q3RETURN(ARG(1));
}

static void Q3PolarPoint_ToPoint2D(ppc_cpu *c)
{
    float p[2];
    ld(ARG(0), p, 2);
    float r[2] = { p[0] * cosf(p[1]), p[0] * sinf(p[1]) };
    st(ARG(1), r, 2);
    Q3RETURN(ARG(1));
}

static void Q3Point3D_ToSpherical(ppc_cpu *c)
{
    float p[3];
    ld(ARG(0), p, 3);
    float rho = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    float r[3] = { rho, atan2f(p[1], p[0]), rho > 0 ? acosf(fmaxf(-1, fminf(1, p[2] / rho))) : 0 };
    st(ARG(1), r, 3);
    Q3RETURN(ARG(1));
}

static void Q3SphericalPoint_ToPoint3D(ppc_cpu *c)
{
    float s[3];
    ld(ARG(0), s, 3);
    float r[3] = { s[0] * sinf(s[2]) * cosf(s[1]), s[0] * sinf(s[2]) * sinf(s[1]), s[0] * cosf(s[2]) };
    st(ARG(1), r, 3);
    Q3RETURN(ARG(1));
}

/* ---------------------------------------------------------------------- */
/* Affine combinations: the weighted sum, over the weights' total. */

static void affine(ppc_cpu *c, int n)
{
    gaddr pts = ARG(0), w = ARG(1), out = ARG(3);
    uint32_t count = ARG(2);
    float sum[4] = { 0 }, total = 0;
    for (uint32_t k = 0; k < count; k++) {
        float p[4], wk = rdf(w + 4 * k);
        ld(pts + k * 4 * (uint32_t)n, p, n);
        for (int i = 0; i < n; i++)
            sum[i] += p[i] * wk;
        total += wk;
    }
    if (total != 0 && total != 1)
        for (int i = 0; i < n; i++)
            sum[i] /= total;
    st(out, sum, n);
    Q3RETURN(out);
}
static void Affine2(ppc_cpu *c) { affine(c, 2); }
static void Affine3(ppc_cpu *c) { affine(c, 3); }
static void Affine4(ppc_cpu *c) { affine(c, 4); }

/* ---------------------------------------------------------------------- */
/* Matrices */

static void identity(float *m, int n)
{
    for (int i = 0; i < n * n; i++)
        m[i] = i % (n + 1) == 0 ? 1.0f : 0.0f;
}

static void mul(const float *a, const float *b, float *r, int n)
{
    float t[16];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            float s = 0;
            for (int k = 0; k < n; k++)
                s += a[i * n + k] * b[k * n + j];
            t[i * n + j] = s;
        }
    for (int i = 0; i < n * n; i++)
        r[i] = t[i];
}

static void copy_n(ppc_cpu *c, int words)
{
    float m[16];
    ld(ARG(0), m, words);
    st(ARG(1), m, words);
    Q3RETURN(ARG(1));
}
static void Copy4(ppc_cpu *c) { copy_n(c, 4); }
static void Copy9(ppc_cpu *c) { copy_n(c, 9); }
static void Copy16(ppc_cpu *c) { copy_n(c, 16); }

static void Q3Matrix3x3_SetIdentity(ppc_cpu *c)
{
    float m[9];
    identity(m, 3);
    st(ARG(0), m, 9);
    Q3RETURN(ARG(0));
}
static void Q3Matrix4x4_SetIdentity(ppc_cpu *c)
{
    float m[16];
    identity(m, 4);
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}

static void transpose(ppc_cpu *c, int n)
{
    float m[16], t[16];
    ld(ARG(0), m, n * n);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            t[j * n + i] = m[i * n + j];
    st(ARG(1), t, n * n);
    Q3RETURN(ARG(1));
}
static void Transpose3(ppc_cpu *c) { transpose(c, 3); }
static void Transpose4(ppc_cpu *c) { transpose(c, 4); }

/* Gauss-Jordan with partial pivoting, in doubles; false if singular. */
static bool invert(const float *m, float *r, int n)
{
    double a[4][8];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            a[i][j] = m[i * n + j];
            a[i][j + n] = i == j;
        }
    for (int col = 0; col < n; col++) {
        int piv = col;
        for (int i = col + 1; i < n; i++)
            if (fabs(a[i][col]) > fabs(a[piv][col]))
                piv = i;
        if (fabs(a[piv][col]) < 1e-12)
            return false;
        if (piv != col)
            for (int j = 0; j < 2 * n; j++) {
                double t = a[col][j];
                a[col][j] = a[piv][j];
                a[piv][j] = t;
            }
        double inv = 1.0 / a[col][col];
        for (int j = 0; j < 2 * n; j++)
            a[col][j] *= inv;
        for (int i = 0; i < n; i++)
            if (i != col && a[i][col] != 0) {
                double f = a[i][col];
                for (int j = 0; j < 2 * n; j++)
                    a[i][j] -= f * a[col][j];
            }
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            r[i * n + j] = (float)a[i][j + n];
    return true;
}

/* A singular matrix leaves the result as it was (QuickDraw 3D posted
 * kQ3ErrorNonInvertibleMatrix) and still returns it. */
static void invert_n(ppc_cpu *c, int n)
{
    float m[16], r[16];
    ld(ARG(0), m, n * n);
    if (invert(m, r, n))
        st(ARG(1), r, n * n);
    Q3RETURN(ARG(1));
}
static void Invert3(ppc_cpu *c) { invert_n(c, 3); }
static void Invert4(ppc_cpu *c) { invert_n(c, 4); }

static float det3(const float *m)
{
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

static float det4(const float *m)
{
    float d = 0;
    for (int col = 0; col < 4; col++) {
        float minor[9];
        int k = 0;
        for (int i = 1; i < 4; i++)
            for (int j = 0; j < 4; j++)
                if (j != col)
                    minor[k++] = m[i * 4 + j];
        d += (col & 1 ? -1.0f : 1.0f) * m[col] * det3(minor);
    }
    return d;
}

static void Q3Matrix3x3_Determinant(ppc_cpu *c)
{
    float m[9];
    ld(ARG(0), m, 9);
    q3_return_float(c, det3(m));
}
static void Q3Matrix4x4_Determinant(ppc_cpu *c)
{
    float m[16];
    ld(ARG(0), m, 16);
    q3_return_float(c, det4(m));
}

/* The classical adjoint: the transposed matrix of cofactors. */
static void Q3Matrix3x3_Adjoint(ppc_cpu *c)
{
    float m[9], r[9];
    ld(ARG(0), m, 9);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            r[j * 3 + i] = m[i1 * 3 + j1] * m[i2 * 3 + j2] - m[i1 * 3 + j2] * m[i2 * 3 + j1];
        }
    st(ARG(1), r, 9);
    Q3RETURN(ARG(1));
}

static void multiply(ppc_cpu *c, int n)
{
    float a[16], b[16];
    ld(ARG(0), a, n * n);
    ld(ARG(1), b, n * n);
    mul(a, b, a, n);
    st(ARG(2), a, n * n);
    Q3RETURN(ARG(2));
}
static void Mul3(ppc_cpu *c) { multiply(c, 3); }
static void Mul4(ppc_cpu *c) { multiply(c, 4); }

static void Q3Matrix3x3_SetTranslate(ppc_cpu *c)
{
    float m[9];
    identity(m, 3);
    m[6] = F(0);
    m[7] = F(1);
    st(ARG(0), m, 9);
    Q3RETURN(ARG(0));
}
static void Q3Matrix3x3_SetScale(ppc_cpu *c)
{
    float m[9];
    identity(m, 3);
    m[0] = F(0);
    m[4] = F(1);
    st(ARG(0), m, 9);
    Q3RETURN(ARG(0));
}
static void Q3Matrix3x3_SetRotateAboutPoint(ppc_cpu *c)
{
    float o[2], a = F(0), m[9], t[9];
    ld(ARG(1), o, 2);
    float cs = cosf(a), sn = sinf(a);
    identity(m, 3);
    m[6] = -o[0], m[7] = -o[1];
    identity(t, 3);
    t[0] = cs, t[1] = sn, t[3] = -sn, t[4] = cs;
    mul(m, t, m, 3);
    identity(t, 3);
    t[6] = o[0], t[7] = o[1];
    mul(m, t, m, 3);
    st(ARG(0), m, 9);
    Q3RETURN(ARG(0));
}

static void Q3Matrix4x4_SetTranslate(ppc_cpu *c)
{
    float m[16];
    identity(m, 4);
    m[12] = F(0), m[13] = F(1), m[14] = F(2);
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}
static void Q3Matrix4x4_SetScale(ppc_cpu *c)
{
    float m[16];
    identity(m, 4);
    m[0] = F(0), m[5] = F(1), m[10] = F(2);
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}

static void rot_axis(float *m, int axis, float a)
{
    float cs = cosf(a), sn = sinf(a);
    identity(m, 4);
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    m[i * 4 + i] = cs;
    m[i * 4 + j] = sn;
    m[j * 4 + i] = -sn;
    m[j * 4 + j] = cs;
}

/* X, then Y, then Z. */
static void rot_xyz(float *m, float x, float y, float z)
{
    float t[16];
    rot_axis(m, 0, x);
    rot_axis(t, 1, y);
    mul(m, t, m, 4);
    rot_axis(t, 2, z);
    mul(m, t, m, 4);
}

static void Q3Matrix4x4_SetRotate_X(ppc_cpu *c)
{
    float m[16];
    rot_axis(m, 0, F(0));
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}
static void Q3Matrix4x4_SetRotate_Y(ppc_cpu *c)
{
    float m[16];
    rot_axis(m, 1, F(0));
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}
static void Q3Matrix4x4_SetRotate_Z(ppc_cpu *c)
{
    float m[16];
    rot_axis(m, 2, F(0));
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}
static void Q3Matrix4x4_SetRotate_XYZ(ppc_cpu *c)
{
    float m[16];
    rot_xyz(m, F(0), F(1), F(2));
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}

/* m = T(-o) * r * T(o) */
static void about_point(float *m, const float *o)
{
    float t[16], r[16];
    for (int i = 0; i < 16; i++)
        r[i] = m[i];
    identity(t, 4);
    t[12] = -o[0], t[13] = -o[1], t[14] = -o[2];
    mul(t, r, m, 4);
    identity(t, 4);
    t[12] = o[0], t[13] = o[1], t[14] = o[2];
    mul(m, t, m, 4);
}

static void Q3Matrix4x4_SetRotateAboutPoint(ppc_cpu *c)
{
    float o[3], m[16];
    ld(ARG(1), o, 3);
    rot_xyz(m, F(0), F(1), F(2));
    about_point(m, o);
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}

/* A rotation by angle about the unit axis a, for row vectors. */
static void rot_about(float *m, const float *a, float angle)
{
    float cs = cosf(angle), sn = sinf(angle), t = 1 - cs, x = a[0], y = a[1], z = a[2];
    identity(m, 4);
    m[0] = t * x * x + cs, m[1] = t * x * y + sn * z, m[2] = t * x * z - sn * y;
    m[4] = t * x * y - sn * z, m[5] = t * y * y + cs, m[6] = t * y * z + sn * x;
    m[8] = t * x * z + sn * y, m[9] = t * y * z - sn * x, m[10] = t * z * z + cs;
}

static void unit(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0)
        v[0] /= l, v[1] /= l, v[2] /= l;
}

static void Q3Matrix4x4_SetRotateAboutAxis(ppc_cpu *c)
{
    float o[3], a[3], m[16];
    ld(ARG(1), o, 3);
    ld(ARG(2), a, 3);
    unit(a);
    rot_about(m, a, F(0));
    about_point(m, o);
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}

/* The axis and angle turning v1 to v2: any axis perpendicular when they
 * are opposite. */
static float vec_to_vec(const float *v1, const float *v2, float *axis)
{
    float a[3] = { v1[0], v1[1], v1[2] }, b[3] = { v2[0], v2[1], v2[2] };
    unit(a);
    unit(b);
    float d = fmaxf(-1, fminf(1, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
    cross(a, b, axis);
    float l = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (l < 1e-6f) {
        if (d > 0) {
            axis[0] = 1, axis[1] = axis[2] = 0;
            return 0;
        }
        /* Opposite: about whichever axis is most perpendicular. */
        float p[3] = { 0, 0, 0 };
        int k = fabsf(a[0]) < fabsf(a[1]) ? (fabsf(a[0]) < fabsf(a[2]) ? 0 : 2) : (fabsf(a[1]) < fabsf(a[2]) ? 1 : 2);
        p[k] = 1;
        cross(a, p, axis);
        unit(axis);
        return (float)M_PI;
    }
    axis[0] /= l, axis[1] /= l, axis[2] /= l;
    return acosf(d);
}

static void Q3Matrix4x4_SetRotateVectorToVector(ppc_cpu *c)
{
    float v1[3], v2[3], axis[3], m[16];
    ld(ARG(1), v1, 3);
    ld(ARG(2), v2, 3);
    rot_about(m, axis, vec_to_vec(v1, v2, axis));
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}

/* q (w x y z) as a matrix for row vectors. */
static void quat_matrix(const float *q, float *m)
{
    float w = q[0], x = q[1], y = q[2], z = q[3];
    identity(m, 4);
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + w * z), m[2] = 2 * (x * z - w * y);
    m[4] = 2 * (x * y - w * z), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + w * x);
    m[8] = 2 * (x * z + w * y), m[9] = 2 * (y * z - w * x), m[10] = 1 - 2 * (x * x + y * y);
}

static void Q3Matrix4x4_SetQuaternion(ppc_cpu *c)
{
    float q[4], m[16];
    ld(ARG(1), q, 4);
    quat_matrix(q, m);
    st(ARG(0), m, 16);
    Q3RETURN(ARG(0));
}

/* ---------------------------------------------------------------------- */
/* Quaternions */

/* Hamilton's product a b. */
static void hamilton(const float *a, const float *b, float *r)
{
    float w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    float x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    float y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    float z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    r[0] = w, r[1] = x, r[2] = y, r[3] = z;
}

/* q1 followed by q2. */
static void qmul(const float *q1, const float *q2, float *r) { hamilton(q2, q1, r); }

static void qaxis(float *q, const float *axis, float angle)
{
    float s = sinf(angle / 2);
    q[0] = cosf(angle / 2);
    q[1] = axis[0] * s, q[2] = axis[1] * s, q[3] = axis[2] * s;
}

static void qret(ppc_cpu *c, gaddr out, const float *q)
{
    st(out, q, 4);
    Q3RETURN(out);
}

static void Q3Quaternion_SetIdentity(ppc_cpu *c)
{
    float q[4] = { 1, 0, 0, 0 };
    qret(c, ARG(0), q);
}

static void Q3Quaternion_IsIdentity(ppc_cpu *c)
{
    float q[4];
    ld(ARG(0), q, 4);
    Q3RETURN(q[0] == 1 && q[1] == 0 && q[2] == 0 && q[3] == 0);
}

static void Q3Quaternion_Invert(ppc_cpu *c)
{
    float q[4];
    ld(ARG(0), q, 4);
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], inv = n > 0 ? 1.0f / n : 0;
    float r[4] = { q[0] * inv, -q[1] * inv, -q[2] * inv, -q[3] * inv };
    qret(c, ARG(1), r);
}

static void qnormalize(float *q)
{
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0)
        for (int i = 0; i < 4; i++)
            q[i] /= n;
}

static void Q3Quaternion_Normalize(ppc_cpu *c)
{
    float q[4];
    ld(ARG(0), q, 4);
    qnormalize(q);
    qret(c, ARG(1), q);
}

static void Q3Quaternion_Dot(ppc_cpu *c) { dot_n(c, 4); }

static void Q3Quaternion_Multiply(ppc_cpu *c)
{
    float a[4], b[4];
    ld(ARG(0), a, 4);
    ld(ARG(1), b, 4);
    qmul(a, b, a);
    qret(c, ARG(2), a);
}

static void Q3Quaternion_SetRotateAboutAxis(ppc_cpu *c)
{
    float axis[3], q[4];
    ld(ARG(1), axis, 3);
    unit(axis);
    qaxis(q, axis, F(0));
    qret(c, ARG(0), q);
}

static void quat_axis_n(ppc_cpu *c, int n)
{
    float axis[3] = { 0, 0, 0 }, q[4];
    axis[n] = 1;
    qaxis(q, axis, F(0));
    qret(c, ARG(0), q);
}
static void Q3Quaternion_SetRotate_X(ppc_cpu *c) { quat_axis_n(c, 0); }
static void Q3Quaternion_SetRotate_Y(ppc_cpu *c) { quat_axis_n(c, 1); }
static void Q3Quaternion_SetRotate_Z(ppc_cpu *c) { quat_axis_n(c, 2); }

static void Q3Quaternion_SetRotate_XYZ(ppc_cpu *c)
{
    float ax[3] = { 1, 0, 0 }, ay[3] = { 0, 1, 0 }, az[3] = { 0, 0, 1 }, qx[4], qy[4], qz[4];
    qaxis(qx, ax, F(0));
    qaxis(qy, ay, F(1));
    qaxis(qz, az, F(2));
    qmul(qx, qy, qx);
    qmul(qx, qz, qx);
    qret(c, ARG(0), qx);
}

/* From the rotation part of a matrix (as quat_matrix makes them). */
static void Q3Quaternion_SetMatrix(ppc_cpu *c)
{
    float m[16], q[4];
    ld(ARG(1), m, 16);
    float tr = m[0] + m[5] + m[10];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[0] = s / 4;
        q[1] = (m[6] - m[9]) / s;
        q[2] = (m[8] - m[2]) / s;
        q[3] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = sqrtf(1 + m[0] - m[5] - m[10]) * 2;
        q[0] = (m[6] - m[9]) / s;
        q[1] = s / 4;
        q[2] = (m[1] + m[4]) / s;
        q[3] = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        float s = sqrtf(1 + m[5] - m[0] - m[10]) * 2;
        q[0] = (m[8] - m[2]) / s;
        q[1] = (m[1] + m[4]) / s;
        q[2] = s / 4;
        q[3] = (m[6] + m[9]) / s;
    } else {
        float s = sqrtf(1 + m[10] - m[0] - m[5]) * 2;
        q[0] = (m[1] - m[4]) / s;
        q[1] = (m[8] + m[2]) / s;
        q[2] = (m[6] + m[9]) / s;
        q[3] = s / 4;
    }
    qret(c, ARG(0), q);
}

static void Q3Quaternion_SetRotateVectorToVector(ppc_cpu *c)
{
    float v1[3], v2[3], axis[3], q[4];
    ld(ARG(1), v1, 3);
    ld(ARG(2), v2, 3);
    qaxis(q, axis, vec_to_vec(v1, v2, axis));
    qret(c, ARG(0), q);
}

/* q1, or its negation (the same rotation) if that is nearer q2. */
static void Q3Quaternion_MatchReflection(ppc_cpu *c)
{
    float a[4], b[4];
    ld(ARG(0), a, 4);
    ld(ARG(1), b, 4);
    if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0)
        for (int i = 0; i < 4; i++)
            a[i] = -a[i];
    qret(c, ARG(2), a);
}

/* Linear interpolation between them, normalized. */
static void Q3Quaternion_InterpolateFast(ppc_cpu *c)
{
    float a[4], b[4], t = F(0);
    ld(ARG(0), a, 4);
    ld(ARG(1), b, 4);
    float s = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0f : 1.0f;
    for (int i = 0; i < 4; i++)
        a[i] += t * (s * b[i] - a[i]);
    qnormalize(a);
    qret(c, ARG(3), a);
}

/* Spherical linear interpolation. */
static void Q3Quaternion_InterpolateLinear(ppc_cpu *c)
{
    float a[4], b[4], t = F(0);
    ld(ARG(0), a, 4);
    ld(ARG(1), b, 4);
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {
        d = -d;
        for (int i = 0; i < 4; i++)
            b[i] = -b[i];
    }
    float wa = 1 - t, wb = t;
    if (d < 0.9999f) {
        float th = acosf(d), s = sinf(th);
        wa = sinf((1 - t) * th) / s;
        wb = sinf(t * th) / s;
    }
    for (int i = 0; i < 4; i++)
        a[i] = wa * a[i] + wb * b[i];
    qret(c, ARG(3), a);
}

static void transform_quat(ppc_cpu *c)
{
    float v[4], q[4], m[16];
    ld(ARG(0), v, 3);
    v[3] = 0;
    ld(ARG(1), q, 4);
    quat_matrix(q, m);
    xform(v, m, 3, v);
    st(ARG(2), v, 3);
    Q3RETURN(ARG(2));
}

/* ---------------------------------------------------------------------- */
/* Bounding boxes and spheres: min, max, isEmpty; origin, radius, isEmpty. */

typedef struct {
    float min[3], max[3];
    uint32_t empty;
} box;

static box box_ld(gaddr a)
{
    box b;
    ld(a, b.min, 3);
    ld(a + 12, b.max, 3);
    b.empty = rd32(a + 24);
    return b;
}
static void box_st(gaddr a, const box *b)
{
    st(a, b->min, 3);
    st(a + 12, b->max, 3);
    wr32(a + 24, b->empty);
}
static void box_add(box *b, const float *p)
{
    if (b->empty) {
        for (int i = 0; i < 3; i++)
            b->min[i] = b->max[i] = p[i];
        b->empty = 0;
        return;
    }
    for (int i = 0; i < 3; i++) {
        b->min[i] = fminf(b->min[i], p[i]);
        b->max[i] = fmaxf(b->max[i], p[i]);
    }
}

static void Q3BoundingBox_Copy(ppc_cpu *c) { copy_n(c, 7); }

static void Q3BoundingBox_Union(ppc_cpu *c)
{
    box a = box_ld(ARG(0)), b = box_ld(ARG(1));
    if (!b.empty) {
        box_add(&a, b.min);
        box_add(&a, b.max);
    }
    box_st(ARG(2), &a);
    Q3RETURN(ARG(2));
}

static void Q3BoundingBox_Set(ppc_cpu *c)
{
    box b;
    ld(ARG(1), b.min, 3);
    ld(ARG(2), b.max, 3);
    b.empty = ARG(3);
    box_st(ARG(0), &b);
    Q3RETURN(ARG(0));
}

static void point_of(gaddr a, bool rational, float *p)
{
    float v[4];
    ld(a, v, rational ? 4 : 3);
    if (rational && v[3] != 0)
        for (int i = 0; i < 3; i++)
            v[i] /= v[3];
    p[0] = v[0], p[1] = v[1], p[2] = v[2];
}

static void box_union_point(ppc_cpu *c, bool rational)
{
    box b = box_ld(ARG(0));
    float p[3];
    point_of(ARG(1), rational, p);
    box_add(&b, p);
    box_st(ARG(2), &b);
    Q3RETURN(ARG(2));
}
static void Q3BoundingBox_UnionPoint3D(ppc_cpu *c) { box_union_point(c, false); }
static void Q3BoundingBox_UnionRationalPoint4D(ppc_cpu *c) { box_union_point(c, true); }

static void box_from_points(ppc_cpu *c, bool rational)
{
    box b = { { 0 }, { 0 }, 1 };
    uint32_t n = ARG(2), size = ARG(3);
    for (uint32_t k = 0; k < n; k++) {
        float p[3];
        point_of(ARG(1) + k * size, rational, p);
        box_add(&b, p);
    }
    box_st(ARG(0), &b);
    Q3RETURN(ARG(0));
}
static void Q3BoundingBox_SetFromPoints3D(ppc_cpu *c) { box_from_points(c, false); }
static void Q3BoundingBox_SetFromRationalPoints4D(ppc_cpu *c) { box_from_points(c, true); }

typedef struct {
    float o[3], r;
    uint32_t empty;
} sphere;

static sphere sph_ld(gaddr a)
{
    sphere s;
    ld(a, s.o, 3);
    s.r = rdf(a + 12);
    s.empty = rd32(a + 16);
    return s;
}
static void sph_st(gaddr a, const sphere *s)
{
    st(a, s->o, 3);
    wrf(a + 12, s->r);
    wr32(a + 16, s->empty);
}

/* The smallest sphere holding both. */
static void sph_merge(sphere *a, const sphere *b)
{
    if (b->empty)
        return;
    if (a->empty) {
        *a = *b;
        return;
    }
    float d[3] = { b->o[0] - a->o[0], b->o[1] - a->o[1], b->o[2] - a->o[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist + b->r <= a->r)
        return;
    if (dist + a->r <= b->r) {
        *a = *b;
        return;
    }
    float r = (dist + a->r + b->r) / 2, t = dist > 0 ? (r - a->r) / dist : 0;
    for (int i = 0; i < 3; i++)
        a->o[i] += d[i] * t;
    a->r = r;
}

static void Q3BoundingSphere_Copy(ppc_cpu *c) { copy_n(c, 5); }

static void Q3BoundingSphere_Union(ppc_cpu *c)
{
    sphere a = sph_ld(ARG(0)), b = sph_ld(ARG(1));
    sph_merge(&a, &b);
    sph_st(ARG(2), &a);
    Q3RETURN(ARG(2));
}

static void Q3BoundingSphere_Set(ppc_cpu *c)
{
    sphere s;
    ld(ARG(1), s.o, 3);
    s.r = F(0);
    s.empty = ARG(3);
    sph_st(ARG(0), &s);
    Q3RETURN(ARG(0));
}

static void sph_union_point(ppc_cpu *c, bool rational)
{
    sphere s = sph_ld(ARG(0)), p = { { 0 }, 0, 0 };
    point_of(ARG(1), rational, p.o);
    sph_merge(&s, &p);
    sph_st(ARG(2), &s);
    Q3RETURN(ARG(2));
}
static void Q3BoundingSphere_UnionPoint3D(ppc_cpu *c) { sph_union_point(c, false); }
static void Q3BoundingSphere_UnionRationalPoint4D(ppc_cpu *c) { sph_union_point(c, true); }

/* About the centre of the points' box, as far as the farthest. */
static void sph_from_points(ppc_cpu *c, bool rational)
{
    box b = { { 0 }, { 0 }, 1 };
    uint32_t n = ARG(2), size = ARG(3);
    for (uint32_t k = 0; k < n; k++) {
        float p[3];
        point_of(ARG(1) + k * size, rational, p);
        box_add(&b, p);
    }
    sphere s = { { 0 }, 0, 1 };
    if (!b.empty) {
        s.empty = 0;
        for (int i = 0; i < 3; i++)
            s.o[i] = (b.min[i] + b.max[i]) / 2;
        float r2 = 0;
        for (uint32_t k = 0; k < n; k++) {
            float p[3];
            point_of(ARG(1) + k * size, rational, p);
            float d = (p[0] - s.o[0]) * (p[0] - s.o[0]) + (p[1] - s.o[1]) * (p[1] - s.o[1]) +
                      (p[2] - s.o[2]) * (p[2] - s.o[2]);
            r2 = fmaxf(r2, d);
        }
        s.r = sqrtf(r2);
    }
    sph_st(ARG(0), &s);
    Q3RETURN(ARG(0));
}
static void Q3BoundingSphere_SetFromPoints3D(ppc_cpu *c) { sph_from_points(c, false); }
static void Q3BoundingSphere_SetFromRationalPoints4D(ppc_cpu *c) { sph_from_points(c, true); }

/* ---------------------------------------------------------------------- */
/* Colours (QD3DShader.h) */

static void Q3ColorRGB_Scale(ppc_cpu *c) { scale_n(c, 3); }

static void Q3ColorRGB_Clamp(ppc_cpu *c)
{
    float v[3];
    ld(ARG(0), v, 3);
    for (int i = 0; i < 3; i++)
        v[i] = fminf(1, fmaxf(0, v[i]));
    st(ARG(1), v, 3);
    Q3RETURN(ARG(1));
}

static void Q3ColorRGB_Lerp(ppc_cpu *c)
{
    float a[3], b[3], t = F(0);
    ld(ARG(0), a, 3);
    ld(ARG(1), b, 3);
    for (int i = 0; i < 3; i++)
        a[i] += t * (b[i] - a[i]);
    st(ARG(3), a, 3);
    Q3RETURN(ARG(3));
}

static void Q3ColorRGB_Accumulate(ppc_cpu *c)
{
    float a[3], b[3];
    ld(ARG(0), a, 3);
    ld(ARG(1), b, 3);
    for (int i = 0; i < 3; i++)
        b[i] += a[i];
    st(ARG(1), b, 3);
    Q3RETURN(ARG(1));
}

static void Q3ColorRGB_Luminance(ppc_cpu *c)
{
    float v[3];
    ld(ARG(0), v, 3);
    wrf(ARG(1), v[0] * 0.30078125f + v[1] * 0.58984375f + v[2] * 0.109375f);
    Q3RETURN(ARG(1));
}

const ppc_export q3_math_exports[] = {
    { "Q3Point2D_Set", Set2 },
    { "Q3Param2D_Set", Set2 },
    { "Q3Point3D_Set", Set3 },
    { "Q3RationalPoint3D_Set", Set3 },
    { "Q3RationalPoint4D_Set", Set4 },
    { "Q3Vector2D_Set", Set2 },
    { "Q3Vector3D_Set", Set3 },
    { "Q3PolarPoint_Set", Set2 },
    { "Q3SphericalPoint_Set", Set3 },
    { "Q3Point2D_To3D", Q3Point2D_To3D },
    { "Q3RationalPoint3D_To2D", Down2 },
    { "Q3Point3D_To4D", Q3Point3D_To4D },
    { "Q3RationalPoint4D_To3D", Down3 },
    { "Q3Vector2D_To3D", Q3Point2D_To3D },
    { "Q3Vector3D_To2D", Down2 },
    { "Q3Point2D_Subtract", Sub2 },
    { "Q3Param2D_Subtract", Sub2 },
    { "Q3Point3D_Subtract", Sub3 },
    { "Q3Point2D_Distance", Dist2 },
    { "Q3Point2D_DistanceSquared", DistSq2 },
    { "Q3Param2D_Distance", Dist2 },
    { "Q3Param2D_DistanceSquared", DistSq2 },
    { "Q3RationalPoint3D_Distance", DistR3 },
    { "Q3RationalPoint3D_DistanceSquared", DistSqR3 },
    { "Q3Point3D_Distance", Dist3 },
    { "Q3Point3D_DistanceSquared", DistSq3 },
    { "Q3RationalPoint4D_Distance", DistR4 },
    { "Q3RationalPoint4D_DistanceSquared", DistSqR4 },
    { "Q3Point2D_RRatio", RRatio2 },
    { "Q3Param2D_RRatio", RRatio2 },
    { "Q3Point3D_RRatio", RRatio3 },
    { "Q3RationalPoint4D_RRatio", RRatio4 },
    { "Q3Point2D_Vector2D_Add", Add2 },
    { "Q3Param2D_Vector2D_Add", Add2 },
    { "Q3Point3D_Vector3D_Add", Add3 },
    { "Q3Point2D_Vector2D_Subtract", Sub2 },
    { "Q3Param2D_Vector2D_Subtract", Sub2 },
    { "Q3Point3D_Vector3D_Subtract", Sub3 },
    { "Q3Vector2D_Scale", Scale2 },
    { "Q3Vector3D_Scale", Scale3 },
    { "Q3Vector2D_Length", Len2 },
    { "Q3Vector3D_Length", Len3 },
    { "Q3Vector2D_Normalize", Norm2 },
    { "Q3Vector3D_Normalize", Norm3 },
    { "Q3Vector2D_Add", Add2 },
    { "Q3Vector3D_Add", Add3 },
    { "Q3Vector2D_Subtract", Sub2 },
    { "Q3Vector3D_Subtract", Sub3 },
    { "Q3Vector2D_Cross", Q3Vector2D_Cross },
    { "Q3Vector3D_Cross", Q3Vector3D_Cross },
    { "Q3Point3D_CrossProductTri", Q3Point3D_CrossProductTri },
    { "Q3Vector2D_Dot", Dot2 },
    { "Q3Vector3D_Dot", Dot3 },
    { "Q3Vector2D_Transform", XfVec2 },
    { "Q3Vector3D_Transform", XfVec3 },
    { "Q3Point2D_Transform", XfPt2 },
    { "Q3Param2D_Transform", XfPt2 },
    { "Q3Point3D_Transform", XfPt3 },
    { "Q3RationalPoint4D_Transform", XfR4 },
    { "Q3Point3D_To3DTransformArray", Q3Point3D_To3DTransformArray },
    { "Q3Point3D_To4DTransformArray", Q3Point3D_To4DTransformArray },
    { "Q3RationalPoint4D_To4DTransformArray", Q3RationalPoint4D_To4DTransformArray },
    { "Q3Vector2D_Negate", Neg2 },
    { "Q3Vector3D_Negate", Neg3 },
    { "Q3Point2D_ToPolar", Q3Point2D_ToPolar },
    { "Q3PolarPoint_ToPoint2D", Q3PolarPoint_ToPoint2D },
    { "Q3Point3D_ToSpherical", Q3Point3D_ToSpherical },
    { "Q3SphericalPoint_ToPoint3D", Q3SphericalPoint_ToPoint3D },
    { "Q3Point2D_AffineComb", Affine2 },
    { "Q3Param2D_AffineComb", Affine2 },
    { "Q3RationalPoint3D_AffineComb", Affine3 },
    { "Q3Point3D_AffineComb", Affine3 },
    { "Q3RationalPoint4D_AffineComb", Affine4 },
    { "Q3Matrix3x3_Copy", Copy9 },
    { "Q3Matrix4x4_Copy", Copy16 },
    { "Q3Matrix3x3_SetIdentity", Q3Matrix3x3_SetIdentity },
    { "Q3Matrix4x4_SetIdentity", Q3Matrix4x4_SetIdentity },
    { "Q3Matrix3x3_Transpose", Transpose3 },
    { "Q3Matrix4x4_Transpose", Transpose4 },
    { "Q3Matrix3x3_Invert", Invert3 },
    { "Q3Matrix4x4_Invert", Invert4 },
    { "Q3Matrix3x3_Adjoint", Q3Matrix3x3_Adjoint },
    { "Q3Matrix3x3_Multiply", Mul3 },
    { "Q3Matrix4x4_Multiply", Mul4 },
    { "Q3Matrix3x3_SetTranslate", Q3Matrix3x3_SetTranslate },
    { "Q3Matrix3x3_SetScale", Q3Matrix3x3_SetScale },
    { "Q3Matrix3x3_SetRotateAboutPoint", Q3Matrix3x3_SetRotateAboutPoint },
    { "Q3Matrix4x4_SetTranslate", Q3Matrix4x4_SetTranslate },
    { "Q3Matrix4x4_SetScale", Q3Matrix4x4_SetScale },
    { "Q3Matrix4x4_SetRotateAboutPoint", Q3Matrix4x4_SetRotateAboutPoint },
    { "Q3Matrix4x4_SetRotateAboutAxis", Q3Matrix4x4_SetRotateAboutAxis },
    { "Q3Matrix4x4_SetRotate_X", Q3Matrix4x4_SetRotate_X },
    { "Q3Matrix4x4_SetRotate_Y", Q3Matrix4x4_SetRotate_Y },
    { "Q3Matrix4x4_SetRotate_Z", Q3Matrix4x4_SetRotate_Z },
    { "Q3Matrix4x4_SetRotate_XYZ", Q3Matrix4x4_SetRotate_XYZ },
    { "Q3Matrix4x4_SetRotateVectorToVector", Q3Matrix4x4_SetRotateVectorToVector },
    { "Q3Matrix4x4_SetQuaternion", Q3Matrix4x4_SetQuaternion },
    { "Q3Matrix3x3_Determinant", Q3Matrix3x3_Determinant },
    { "Q3Matrix4x4_Determinant", Q3Matrix4x4_Determinant },
    { "Q3Quaternion_Set", Set4 },
    { "Q3Quaternion_SetIdentity", Q3Quaternion_SetIdentity },
    { "Q3Quaternion_Copy", Copy4 },
    { "Q3Quaternion_IsIdentity", Q3Quaternion_IsIdentity },
    { "Q3Quaternion_Invert", Q3Quaternion_Invert },
    { "Q3Quaternion_Normalize", Q3Quaternion_Normalize },
    { "Q3Quaternion_Dot", Q3Quaternion_Dot },
    { "Q3Quaternion_Multiply", Q3Quaternion_Multiply },
    { "Q3Quaternion_SetRotateAboutAxis", Q3Quaternion_SetRotateAboutAxis },
    { "Q3Quaternion_SetRotate_XYZ", Q3Quaternion_SetRotate_XYZ },
    { "Q3Quaternion_SetRotate_X", Q3Quaternion_SetRotate_X },
    { "Q3Quaternion_SetRotate_Y", Q3Quaternion_SetRotate_Y },
    { "Q3Quaternion_SetRotate_Z", Q3Quaternion_SetRotate_Z },
    { "Q3Quaternion_SetMatrix", Q3Quaternion_SetMatrix },
    { "Q3Quaternion_SetRotateVectorToVector", Q3Quaternion_SetRotateVectorToVector },
    { "Q3Quaternion_MatchReflection", Q3Quaternion_MatchReflection },
    { "Q3Quaternion_InterpolateFast", Q3Quaternion_InterpolateFast },
    { "Q3Quaternion_InterpolateLinear", Q3Quaternion_InterpolateLinear },
    { "Q3Vector3D_TransformQuaternion", transform_quat },
    { "Q3Point3D_TransformQuaternion", transform_quat },
    { "Q3BoundingBox_Copy", Q3BoundingBox_Copy },
    { "Q3BoundingBox_Union", Q3BoundingBox_Union },
    { "Q3BoundingBox_Set", Q3BoundingBox_Set },
    { "Q3BoundingBox_UnionPoint3D", Q3BoundingBox_UnionPoint3D },
    { "Q3BoundingBox_UnionRationalPoint4D", Q3BoundingBox_UnionRationalPoint4D },
    { "Q3BoundingBox_SetFromPoints3D", Q3BoundingBox_SetFromPoints3D },
    { "Q3BoundingBox_SetFromRationalPoints4D", Q3BoundingBox_SetFromRationalPoints4D },
    { "Q3BoundingSphere_Copy", Q3BoundingSphere_Copy },
    { "Q3BoundingSphere_Union", Q3BoundingSphere_Union },
    { "Q3BoundingSphere_Set", Q3BoundingSphere_Set },
    { "Q3BoundingSphere_UnionPoint3D", Q3BoundingSphere_UnionPoint3D },
    { "Q3BoundingSphere_UnionRationalPoint4D", Q3BoundingSphere_UnionRationalPoint4D },
    { "Q3BoundingSphere_SetFromPoints3D", Q3BoundingSphere_SetFromPoints3D },
    { "Q3BoundingSphere_SetFromRationalPoints4D", Q3BoundingSphere_SetFromRationalPoints4D },
    { "Q3ColorRGB_Set", Set3 },
    { "Q3ColorARGB_Set", Set4 },
    { "Q3ColorRGB_Add", Add3 },
    { "Q3ColorRGB_Subtract", Sub3 },
    { "Q3ColorRGB_Scale", Q3ColorRGB_Scale },
    { "Q3ColorRGB_Clamp", Q3ColorRGB_Clamp },
    { "Q3ColorRGB_Lerp", Q3ColorRGB_Lerp },
    { "Q3ColorRGB_Accumulate", Q3ColorRGB_Accumulate },
    { "Q3ColorRGB_Luminance", Q3ColorRGB_Luminance },
};
const unsigned q3_math_export_count = sizeof q3_math_exports / sizeof q3_math_exports[0];
