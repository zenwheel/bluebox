/*
 * QuickDraw 3D's other shapes and shared objects, as data objects
 * (q3_data.h): cameras (QD3DCamera.h), lights (QD3DLight.h), styles
 * (QD3DStyle.h), transforms (QD3DTransform.h), shaders and textures
 * (QD3DShader.h), draw contexts (QD3DDrawContext.h), renderers
 * (QD3DRenderer.h), view hints and state operators; their calls, and what
 * each does submitted to a view.
 */
#include <math.h>
#include <stdlib.h>

#include "q3_view.h"
#include "tb_internal.h"

/* q3_view.c's state for submitted shapes. */
void q3v_set_style(q3view *v, uint32_t type, const uint32_t *w);
void q3v_set_shader(q3view *v, q3obj *shader);
void q3v_reset_transform(q3view *v);
void q3v_parts(q3view *v, gaddr parts[3]);
void q3_texture_forget(gaddr tex);
bool q3_camera_matrices(gaddr camera, float w2v[16], float v2f[16]);
uint32_t *q3_storage_pixmap_argb(const uint32_t *w, gaddr storage);
uint8_t *q3_storage_bytes(gaddr storage, uint32_t *size);

#define T4(s) Q3T(s[0], s[1], s[2], s[3])
#define ORTH T4("orth")
#define VWPL T4("vwpl")
#define VANA T4("vana")
#define AMBN T4("ambn")
#define DRCT T4("drct")
#define PNTL T4("pntl")
#define SPOT T4("spot")
#define BCKF T4("bckf")
#define INTP T4("intp")
#define FIST T4("fist")
#define PKID T4("pkid")
#define RCSH T4("rcsh")
#define HIGH T4("high")
#define SBDV T4("sbdv")
#define OFDR T4("ofdr")
#define PKPT T4("pkpt")
#define ANTI T4("anti")
#define FOGG T4("fogg")
#define MTRX T4("mtrx")
#define SCAL T4("scal")
#define TRNS T4("trns")
#define ROTT T4("rott")
#define RTAP T4("rtap")
#define RTAA T4("rtaa")
#define QTRN T4("qtrn")
#define RSET T4("rset")
#define PHIL T4("phil")
#define LMIL T4("lmil")
#define NUIL T4("nuil")
#define TXSU T4("txsu")
#define TXPM T4("txpm")
#define TXMM T4("txmm")
#define TXCP T4("txcp")
#define DPXP T4("dpxp")
#define DMAC T4("dmac")
#define CTWN T4("ctwn")
#define WRFR T4("wrfr")
#define GNRR T4("gnrr")
#define VWHN T4("vwhn")
#define PUSH T4("push")
#define POP T4("pop ")

/* ---------------------------------------------------------------------- */
/* Schemas */

static const q3schema cam_orth_s = { 19, 0, NULL };
static const q3schema cam_vwpl_s = { 20, 0, NULL };
static const q3schema cam_vana_s = { 17, 0, NULL };
static const q3schema light_ambn_s = { 5, 0, NULL };
static const q3schema light_drct_s = { 9, 0, NULL };
static const q3schema light_pntl_s = { 10, 0, NULL };
static const q3schema light_spot_s = { 16, 0, NULL };
static const q3schema word1_s = { 1, 0, NULL };
static const q3field obj0_f[] = { { 0, QF_OBJ, NULL, NULL } };
static const q3schema high_s = { 1, 1, obj0_f };
static const q3schema word3_s = { 3, 0, NULL };
static const q3schema fog_s = { 9, 0, NULL };
static const q3schema mtrx_s = { 16, 0, NULL };
static const q3schema rott_s = { 2, 0, NULL };
static const q3schema rtap_s = { 5, 0, NULL };
static const q3schema rtaa_s = { 7, 0, NULL };
static const q3schema qtrn_s = { 4, 0, NULL };
static const q3schema illum_s = { 11, 0, NULL };
static const q3field txsu_f[] = { { 11, QF_OBJ, NULL, NULL } };
static const q3schema txsu_s = { 12, 1, txsu_f };
static const q3schema txpm_s = { 8, 1, obj0_f };
static const q3schema txmm_s = { 134, 1, obj0_f };
static const q3field txcp_f[] = { { 0, QF_OBJ, NULL, NULL }, { 2, QF_OBJ, NULL, NULL } };
static const q3schema txcp_s = { 8, 2, txcp_f };
static uint32_t mask_bytes(const uint32_t *s, const uint32_t *p) { return s[12] * s[13]; }
static const q3field dctx_f[] = { { 10, QF_BYTES, mask_bytes, NULL } };
static const q3schema dctx_s = { 17, 1, dctx_f };
static const q3schema dmac_s = { 21, 1, dctx_f };
static const q3schema dpxp_s = { 25, 1, dctx_f };
static const q3schema rddr_s = { 6, 0, NULL };
static uint32_t hint_mask_bytes(const uint32_t *s, const uint32_t *p) { return s[10] * s[11]; }
static const q3field vwhn_f[] = { { 0, QF_OBJ, NULL, NULL }, { 1, QF_OBJ, NULL, NULL }, { 2, QF_OBJ, NULL, NULL }, { 3, QF_OBJ, NULL, NULL },
                                  { 8, QF_BYTES, hint_mask_bytes, NULL } };
static const q3schema vwhn_s = { 18, 5, vwhn_f };

/* ---------------------------------------------------------------------- */
/* Submitting */

static void submit_nothing(q3obj *o, q3view *v) {}

static void submit_style(q3obj *o, q3view *v) { q3v_set_style(v, o->type, ((q3node *)o->data)->w); }

static void rot_axis(int axis, float a, float m[16])
{
    q3m_identity(m);
    float c = cosf(a), s = sinf(a);
    int i = (axis + 1) % 3, j = (axis + 2) % 3;
    m[i * 4 + i] = c;
    m[i * 4 + j] = s;
    m[j * 4 + i] = -s;
    m[j * 4 + j] = c;
}

static void rot_about_axis(const float *axis, float a, float m[16])
{
    float x = axis[0], y = axis[1], z = axis[2], l = sqrtf(x * x + y * y + z * z);
    if (l > 0)
        x /= l, y /= l, z /= l;
    float c = cosf(a), s = sinf(a), t = 1 - c;
    q3m_identity(m);
    /* Row vectors: the transpose of the column-vector Rodrigues matrix. */
    m[0] = t * x * x + c;
    m[1] = t * x * y + s * z;
    m[2] = t * x * z - s * y;
    m[4] = t * x * y - s * z;
    m[5] = t * y * y + c;
    m[6] = t * y * z + s * x;
    m[8] = t * x * z + s * y;
    m[9] = t * y * z - s * x;
    m[10] = t * z * z + c;
}

static void translate(float m[16], float x, float y, float z)
{
    q3m_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

/* A transform's matrix from its words. */
static void transform_matrix(uint32_t type, const uint32_t *w, float m[16])
{
    q3m_identity(m);
    switch (type) {
    case MTRX:
        for (int i = 0; i < 16; i++)
            m[i] = q3d_f(w, i);
        break;
    case SCAL:
        m[0] = q3d_f(w, 0);
        m[5] = q3d_f(w, 1);
        m[10] = q3d_f(w, 2);
        break;
    case TRNS: translate(m, q3d_f(w, 0), q3d_f(w, 1), q3d_f(w, 2)); break;
    case ROTT: rot_axis((int)(w[0] % 3), q3d_f(w, 1), m); break;
    case RTAP: {
        float a[16], r[16], b[16];
        translate(a, -q3d_f(w, 2), -q3d_f(w, 3), -q3d_f(w, 4));
        rot_axis((int)(w[0] % 3), q3d_f(w, 1), r);
        translate(b, q3d_f(w, 2), q3d_f(w, 3), q3d_f(w, 4));
        q3m_mul(a, r, m);
        q3m_mul(m, b, m);
        break;
    }
    case RTAA: {
        float a[16], r[16], b[16], axis[3] = { q3d_f(w, 3), q3d_f(w, 4), q3d_f(w, 5) };
        translate(a, -q3d_f(w, 0), -q3d_f(w, 1), -q3d_f(w, 2));
        rot_about_axis(axis, q3d_f(w, 6), r);
        translate(b, q3d_f(w, 0), q3d_f(w, 1), q3d_f(w, 2));
        q3m_mul(a, r, m);
        q3m_mul(m, b, m);
        break;
    }
    case QTRN: {
        float qw = q3d_f(w, 0), x = q3d_f(w, 1), y = q3d_f(w, 2), z = q3d_f(w, 3);
        m[0] = 1 - 2 * (y * y + z * z);
        m[1] = 2 * (x * y + qw * z);
        m[2] = 2 * (x * z - qw * y);
        m[4] = 2 * (x * y - qw * z);
        m[5] = 1 - 2 * (x * x + z * z);
        m[6] = 2 * (y * z + qw * x);
        m[8] = 2 * (x * z + qw * y);
        m[9] = 2 * (y * z - qw * x);
        m[10] = 1 - 2 * (x * x + y * y);
        break;
    }
    default: break;
    }
}

static void submit_transform(q3obj *o, q3view *v)
{
    if (o->type == RSET) {
        q3v_reset_transform(v);
        return;
    }
    float m[16];
    transform_matrix(o->type, ((q3node *)o->data)->w, m);
    q3v_transform(v, m);
}

static void submit_shader(q3obj *o, q3view *v) { q3v_set_shader(v, o); }
static void submit_push(q3obj *o, q3view *v) { q3v_push(v); }
static void submit_pop(q3obj *o, q3view *v) { q3v_pop(v); }
static void submit_attrs(q3obj *o, q3view *v) { q3v_attribute_set(v, o->g); }

static void texture_dispose(q3obj *o)
{
    q3_texture_forget(o->g);
    q3d_free(o->data, q3_schema_of(o->type));
    o->data = NULL;
}

static bool texture_duplicate(q3obj *to, const q3obj *from)
{
    to->data = q3d_dup(from->data, q3_schema_of(from->type));
    return to->data != NULL;
}

void q3_shapes_classes(void)
{
    q3_data_class(ORTH, &cam_orth_s, submit_nothing, true);
    q3_data_class(VWPL, &cam_vwpl_s, submit_nothing, true);
    q3_data_class(VANA, &cam_vana_s, submit_nothing, true);
    q3_data_class(AMBN, &light_ambn_s, submit_nothing, true);
    q3_data_class(DRCT, &light_drct_s, submit_nothing, true);
    q3_data_class(PNTL, &light_pntl_s, submit_nothing, true);
    q3_data_class(SPOT, &light_spot_s, submit_nothing, true);
    const uint32_t words1[] = { BCKF, INTP, FIST, PKID, RCSH, OFDR, PKPT };
    for (unsigned i = 0; i < sizeof words1 / sizeof words1[0]; i++)
        q3_data_class(words1[i], &word1_s, submit_style, true);
    q3_data_class(HIGH, &high_s, submit_style, true);
    q3_data_class(SBDV, &word3_s, submit_style, true);
    q3_data_class(ANTI, &word3_s, submit_style, true);
    q3_data_class(FOGG, &fog_s, submit_style, true);
    q3_data_class(MTRX, &mtrx_s, submit_transform, true);
    q3_data_class(SCAL, &word3_s, submit_transform, true);
    q3_data_class(TRNS, &word3_s, submit_transform, true);
    q3_data_class(ROTT, &rott_s, submit_transform, true);
    q3_data_class(RTAP, &rtap_s, submit_transform, true);
    q3_data_class(RTAA, &rtaa_s, submit_transform, true);
    q3_data_class(QTRN, &qtrn_s, submit_transform, true);
    q3_data_class(RSET, &word1_s, submit_transform, true);
    q3_data_class(PHIL, &illum_s, submit_shader, true);
    q3_data_class(LMIL, &illum_s, submit_shader, true);
    q3_data_class(NUIL, &illum_s, submit_shader, true);
    q3_data_class(TXSU, &txsu_s, submit_shader, true);
    q3_data_class(TXPM, &txpm_s, NULL, false);
    q3_data_class(TXMM, &txmm_s, NULL, false);
    q3_data_class(TXCP, &txcp_s, NULL, false);
    q3_class_ops(TXPM, texture_dispose, texture_duplicate, NULL, false);
    q3_class_ops(TXMM, texture_dispose, texture_duplicate, NULL, false);
    q3_class_ops(TXCP, texture_dispose, texture_duplicate, NULL, false);
    q3_data_class(DPXP, &dpxp_s, NULL, false);
    q3_data_class(DMAC, &dmac_s, NULL, false);
    q3_data_class(CTWN, &rddr_s, NULL, false);
    q3_data_class(WRFR, &rddr_s, NULL, false);
    q3_data_class(GNRR, &rddr_s, NULL, false);
    q3_data_class(VWHN, &vwhn_s, NULL, false);
    q3_data_class(PUSH, &word1_s, submit_push, true);
    q3_data_class(POP, &word1_s, submit_pop, true);
    q3_class_submit(Q3_ATTRIBUTE_SET, submit_attrs);
}

/* A new data object of type with its first n words from the program at
 * p (or the defaults). */
static gaddr new_words(uint32_t type, const uint32_t *w, uint32_t n)
{
    gaddr g = q3_data_new(type, 0);
    q3node *d = g ? q3_get(g)->data : NULL;
    if (d)
        memcpy(d->w, w, n * 4);
    return g;
}

static void Q3Shape_Submit_generic(ppc_cpu *c, uint32_t type)
{
    q3view *v = q3v_of(Q3R(1));
    q3obj *o = q3_of(Q3R(0), type);
    if (v && o)
        q3v_submit(v, o->g);
    Q3RETURN(v && o ? kQ3Success : kQ3Failure);
}

static uint32_t type_below(gaddr g, uint32_t above)
{
    q3obj *o = q3_of(g, above);
    return o ? q3_type_below(o->type, above) : 0;
}

/* ---------------------------------------------------------------------- */
/* Cameras */

static void Q3Camera_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_CAMERA)); }
Q3_SETW(Q3Camera_SetData, Q3_CAMERA, 0, 15)
Q3_GETW(Q3Camera_GetData, Q3_CAMERA, 0, 15)
Q3_SETW(Q3Camera_SetPlacement, Q3_CAMERA, 0, 9)
Q3_GETW(Q3Camera_GetPlacement, Q3_CAMERA, 0, 9)
Q3_SETW(Q3Camera_SetRange, Q3_CAMERA, 9, 2)
Q3_GETW(Q3Camera_GetRange, Q3_CAMERA, 9, 2)
Q3_SETW(Q3Camera_SetViewPort, Q3_CAMERA, 11, 4)
Q3_GETW(Q3Camera_GetViewPort, Q3_CAMERA, 11, 4)

static void camera_matrix(ppc_cpu *c, int which)
{
    float w2v[16], v2f[16], m[16];
    if (!Q3R(1) || !q3_camera_matrices(Q3R(0), w2v, v2f)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (which == 0)
        memcpy(m, w2v, sizeof m);
    else if (which == 1)
        q3m_mul(w2v, v2f, m);
    else
        memcpy(m, v2f, sizeof m);
    q3m_put(Q3R(1), m);
    Q3RETURN(kQ3Success);
}
static void Q3Camera_GetWorldToView(ppc_cpu *c) { camera_matrix(c, 0); }
static void Q3Camera_GetWorldToFrustum(ppc_cpu *c) { camera_matrix(c, 1); }
static void Q3Camera_GetViewToFrustum(ppc_cpu *c) { camera_matrix(c, 2); }

Q3_NEWDATA(Q3OrthographicCamera_New, ORTH)
Q3_GETW(Q3OrthographicCamera_GetData, ORTH, 0, 19)
Q3_SETW(Q3OrthographicCamera_SetData, ORTH, 0, 19)
Q3_SETF(Q3OrthographicCamera_SetLeft, ORTH, 15)
Q3_GETW(Q3OrthographicCamera_GetLeft, ORTH, 15, 1)
Q3_SETF(Q3OrthographicCamera_SetTop, ORTH, 16)
Q3_GETW(Q3OrthographicCamera_GetTop, ORTH, 16, 1)
Q3_SETF(Q3OrthographicCamera_SetRight, ORTH, 17)
Q3_GETW(Q3OrthographicCamera_GetRight, ORTH, 17, 1)
Q3_SETF(Q3OrthographicCamera_SetBottom, ORTH, 18)
Q3_GETW(Q3OrthographicCamera_GetBottom, ORTH, 18, 1)
Q3_NEWDATA(Q3ViewPlaneCamera_New, VWPL)
Q3_GETW(Q3ViewPlaneCamera_GetData, VWPL, 0, 20)
Q3_SETW(Q3ViewPlaneCamera_SetData, VWPL, 0, 20)
Q3_SETF(Q3ViewPlaneCamera_SetViewPlane, VWPL, 15)
Q3_GETW(Q3ViewPlaneCamera_GetViewPlane, VWPL, 15, 1)
Q3_SETF(Q3ViewPlaneCamera_SetHalfWidth, VWPL, 16)
Q3_GETW(Q3ViewPlaneCamera_GetHalfWidth, VWPL, 16, 1)
Q3_SETF(Q3ViewPlaneCamera_SetHalfHeight, VWPL, 17)
Q3_GETW(Q3ViewPlaneCamera_GetHalfHeight, VWPL, 17, 1)
Q3_SETF(Q3ViewPlaneCamera_SetCenterX, VWPL, 18)
Q3_GETW(Q3ViewPlaneCamera_GetCenterX, VWPL, 18, 1)
Q3_SETF(Q3ViewPlaneCamera_SetCenterY, VWPL, 19)
Q3_GETW(Q3ViewPlaneCamera_GetCenterY, VWPL, 19, 1)
Q3_NEWDATA(Q3ViewAngleAspectCamera_New, VANA)
Q3_SETW(Q3ViewAngleAspectCamera_SetData, VANA, 0, 17)
Q3_GETW(Q3ViewAngleAspectCamera_GetData, VANA, 0, 17)
Q3_SETF(Q3ViewAngleAspectCamera_SetFOV, VANA, 15)
Q3_GETW(Q3ViewAngleAspectCamera_GetFOV, VANA, 15, 1)
Q3_SETF(Q3ViewAngleAspectCamera_SetAspectRatio, VANA, 16)
Q3_GETW(Q3ViewAngleAspectCamera_GetAspectRatio, VANA, 16, 1)

/* ---------------------------------------------------------------------- */
/* Lights */

static void Q3Light_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_LIGHT)); }
Q3_GETW(Q3Light_GetState, Q3_LIGHT, 0, 1)
Q3_GETW(Q3Light_GetBrightness, Q3_LIGHT, 1, 1)
Q3_GETW(Q3Light_GetColor, Q3_LIGHT, 2, 3)
Q3_SETV(Q3Light_SetState, Q3_LIGHT, 0)
Q3_SETF(Q3Light_SetBrightness, Q3_LIGHT, 1)
Q3_SETW(Q3Light_SetColor, Q3_LIGHT, 2, 3)
Q3_GETW(Q3Light_GetData, Q3_LIGHT, 0, 5)
Q3_SETW(Q3Light_SetData, Q3_LIGHT, 0, 5)
Q3_NEWDATA(Q3AmbientLight_New, AMBN)
Q3_GETW(Q3AmbientLight_GetData, AMBN, 0, 5)
Q3_SETW(Q3AmbientLight_SetData, AMBN, 0, 5)
Q3_NEWDATA(Q3DirectionalLight_New, DRCT)
Q3_GETW(Q3DirectionalLight_GetCastShadowsState, DRCT, 5, 1)
Q3_GETW(Q3DirectionalLight_GetDirection, DRCT, 6, 3)
Q3_SETV(Q3DirectionalLight_SetCastShadowsState, DRCT, 5)
Q3_SETW(Q3DirectionalLight_SetDirection, DRCT, 6, 3)
Q3_GETW(Q3DirectionalLight_GetData, DRCT, 0, 9)
Q3_SETW(Q3DirectionalLight_SetData, DRCT, 0, 9)
Q3_NEWDATA(Q3PointLight_New, PNTL)
Q3_GETW(Q3PointLight_GetCastShadowsState, PNTL, 5, 1)
Q3_GETW(Q3PointLight_GetAttenuation, PNTL, 6, 1)
Q3_GETW(Q3PointLight_GetLocation, PNTL, 7, 3)
Q3_GETW(Q3PointLight_GetData, PNTL, 0, 10)
Q3_SETV(Q3PointLight_SetCastShadowsState, PNTL, 5)
Q3_SETV(Q3PointLight_SetAttenuation, PNTL, 6)
Q3_SETW(Q3PointLight_SetLocation, PNTL, 7, 3)
Q3_SETW(Q3PointLight_SetData, PNTL, 0, 10)
Q3_NEWDATA(Q3SpotLight_New, SPOT)
Q3_GETW(Q3SpotLight_GetCastShadowsState, SPOT, 5, 1)
Q3_GETW(Q3SpotLight_GetAttenuation, SPOT, 6, 1)
Q3_GETW(Q3SpotLight_GetLocation, SPOT, 7, 3)
Q3_GETW(Q3SpotLight_GetDirection, SPOT, 10, 3)
Q3_GETW(Q3SpotLight_GetHotAngle, SPOT, 13, 1)
Q3_GETW(Q3SpotLight_GetOuterAngle, SPOT, 14, 1)
Q3_GETW(Q3SpotLight_GetFallOff, SPOT, 15, 1)
Q3_GETW(Q3SpotLight_GetData, SPOT, 0, 16)
Q3_SETV(Q3SpotLight_SetCastShadowsState, SPOT, 5)
Q3_SETV(Q3SpotLight_SetAttenuation, SPOT, 6)
Q3_SETW(Q3SpotLight_SetLocation, SPOT, 7, 3)
Q3_SETW(Q3SpotLight_SetDirection, SPOT, 10, 3)
Q3_SETF(Q3SpotLight_SetHotAngle, SPOT, 13)
Q3_SETF(Q3SpotLight_SetOuterAngle, SPOT, 14)
Q3_SETV(Q3SpotLight_SetFallOff, SPOT, 15)
Q3_SETW(Q3SpotLight_SetData, SPOT, 0, 16)

/* ---------------------------------------------------------------------- */
/* Styles */

static void Q3Style_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_STYLE)); }
static void Q3Style_Submit(ppc_cpu *c) { Q3Shape_Submit_generic(c, Q3_STYLE); }

/* A one-word style: New by value, Submit by value, Get, Set. */
#define STYLE1(Name, TYPE)                                                                                           \
    static void Q3##Name##_New(ppc_cpu *c)                                                                           \
    {                                                                                                                \
        uint32_t w = Q3R(0);                                                                                         \
        Q3RETURN(new_words(TYPE, &w, 1));                                                                            \
    }                                                                                                                \
    static void Q3##Name##_Submit(ppc_cpu *c)                                                                        \
    {                                                                                                                \
        uint32_t w = Q3R(0);                                                                                         \
        q3view *v = q3v_of(Q3R(1));                                                                                  \
        if (v)                                                                                                       \
            q3v_set_style(v, TYPE, &w);                                                                              \
        Q3RETURN(v ? kQ3Success : kQ3Failure);                                                                       \
    }                                                                                                                \
    Q3_GETW(Q3##Name##_Get, TYPE, 0, 1)                                                                              \
    Q3_SETV(Q3##Name##_Set, TYPE, 0)

STYLE1(PickIDStyle, PKID)
STYLE1(PickPartsStyle, PKPT)
STYLE1(ReceiveShadowsStyle, RCSH)
STYLE1(FillStyle, FIST)
STYLE1(BackfacingStyle, BCKF)
STYLE1(InterpolationStyle, INTP)
STYLE1(OrientationStyle, OFDR)

static void Q3HighlightStyle_New(ppc_cpu *c)
{
    gaddr g = q3_data_new(HIGH, 0);
    q3node *d = g ? q3_get(g)->data : NULL;
    if (d)
        q3_assign(&d->w[0], Q3R(0));
    Q3RETURN(g);
}
static void Q3HighlightStyle_Submit(ppc_cpu *c)
{
    uint32_t w = Q3R(0);
    q3view *v = q3v_of(Q3R(1));
    if (v)
        q3v_set_style(v, HIGH, &w);
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}
Q3_GETO(Q3HighlightStyle_Get, HIGH, 0)
Q3_SETO(Q3HighlightStyle_Set, HIGH, 0)

/* Styles with a structure. */
#define STYLE_DATA(Name, TYPE, N)                                                                                    \
    Q3_NEWDATA(Q3##Name##_New, TYPE)                                                                                 \
    static void Q3##Name##_Submit(ppc_cpu *c)                                                                        \
    {                                                                                                                \
        uint32_t w[N];                                                                                               \
        q3view *v = q3v_of(Q3R(1));                                                                                  \
        if (v && Q3R(0)) {                                                                                           \
            for (int i = 0; i < (N); i++)                                                                            \
                w[i] = rd32(Q3R(0) + 4 * (uint32_t)i);                                                               \
            q3v_set_style(v, TYPE, w);                                                                               \
        }                                                                                                            \
        Q3RETURN(v && Q3R(0) ? kQ3Success : kQ3Failure);                                                             \
    }                                                                                                                \
    Q3_GETW(Q3##Name##_GetData, TYPE, 0, N)                                                                          \
    Q3_SETW(Q3##Name##_SetData, TYPE, 0, N)

STYLE_DATA(SubdivisionStyle, SBDV, 3)
STYLE_DATA(AntiAliasStyle, ANTI, 3)
STYLE_DATA(FogStyle, FOGG, 9)

/* ---------------------------------------------------------------------- */
/* Transforms */

static void Q3Transform_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_TRANSFORM)); }

static void Q3Transform_GetMatrix(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_TRANSFORM);
    if (!o || !Q3R(1)) {
        Q3RETURN(0);
        return;
    }
    float m[16];
    transform_matrix(o->type, o->data ? ((q3node *)o->data)->w : NULL, m);
    q3m_put(Q3R(1), m);
    Q3RETURN(Q3R(1));
}

static void Q3Transform_Submit(ppc_cpu *c) { Q3Shape_Submit_generic(c, Q3_TRANSFORM); }

/* An immediate transform: its words from the program at p. */
static void submit_transform_now(ppc_cpu *c, uint32_t type, gaddr p, uint32_t n, gaddr view)
{
    q3view *v = q3v_of(view);
    if (!v || (n && !p)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t w[16] = { 0 };
    for (uint32_t i = 0; i < n; i++)
        w[i] = rd32(p + 4 * i);
    float m[16];
    transform_matrix(type, w, m);
    if (v)
        q3v_transform(v, m);
    Q3RETURN(kQ3Success);
}

#define XFORM(Name, TYPE, N)                                                                                         \
    Q3_NEWDATA(Q3##Name##_New, TYPE)                                                                                 \
    static void Q3##Name##_Submit(ppc_cpu *c) { submit_transform_now(c, TYPE, Q3R(0), N, Q3R(1)); }

XFORM(MatrixTransform, MTRX, 16)
Q3_SETW(Q3MatrixTransform_Set, MTRX, 0, 16)
Q3_GETW(Q3MatrixTransform_Get, MTRX, 0, 16)
XFORM(RotateTransform, ROTT, 2)
Q3_SETW(Q3RotateTransform_SetData, ROTT, 0, 2)
Q3_GETW(Q3RotateTransform_GetData, ROTT, 0, 2)
Q3_SETV(Q3RotateTransform_SetAxis, ROTT, 0)
Q3_SETF(Q3RotateTransform_SetAngle, ROTT, 1)
Q3_GETW(Q3RotateTransform_GetAxis, ROTT, 0, 1)
Q3_GETW(Q3RotateTransform_GetAngle, ROTT, 1, 1)
XFORM(RotateAboutPointTransform, RTAP, 5)
Q3_SETW(Q3RotateAboutPointTransform_SetData, RTAP, 0, 5)
Q3_GETW(Q3RotateAboutPointTransform_GetData, RTAP, 0, 5)
Q3_SETV(Q3RotateAboutPointTransform_SetAxis, RTAP, 0)
Q3_GETW(Q3RotateAboutPointTransform_GetAxis, RTAP, 0, 1)
Q3_SETF(Q3RotateAboutPointTransform_SetAngle, RTAP, 1)
Q3_GETW(Q3RotateAboutPointTransform_GetAngle, RTAP, 1, 1)
Q3_SETW(Q3RotateAboutPointTransform_SetAboutPoint, RTAP, 2, 3)
Q3_GETW(Q3RotateAboutPointTransform_GetAboutPoint, RTAP, 2, 3)
XFORM(RotateAboutAxisTransform, RTAA, 7)
Q3_SETW(Q3RotateAboutAxisTransform_SetData, RTAA, 0, 7)
Q3_GETW(Q3RotateAboutAxisTransform_GetData, RTAA, 0, 7)
Q3_SETW(Q3RotateAboutAxisTransform_SetOrientation, RTAA, 3, 3)
Q3_GETW(Q3RotateAboutAxisTransform_GetOrientation, RTAA, 3, 3)
Q3_SETF(Q3RotateAboutAxisTransform_SetAngle, RTAA, 6)
Q3_GETW(Q3RotateAboutAxisTransform_GetAngle, RTAA, 6, 1)
Q3_SETW(Q3RotateAboutAxisTransform_SetOrigin, RTAA, 0, 3)
Q3_GETW(Q3RotateAboutAxisTransform_GetOrigin, RTAA, 0, 3)
XFORM(ScaleTransform, SCAL, 3)
Q3_SETW(Q3ScaleTransform_Set, SCAL, 0, 3)
Q3_GETW(Q3ScaleTransform_Get, SCAL, 0, 3)
XFORM(TranslateTransform, TRNS, 3)
Q3_SETW(Q3TranslateTransform_Set, TRNS, 0, 3)
Q3_GETW(Q3TranslateTransform_Get, TRNS, 0, 3)
XFORM(QuaternionTransform, QTRN, 4)
Q3_SETW(Q3QuaternionTransform_Set, QTRN, 0, 4)
Q3_GETW(Q3QuaternionTransform_Get, QTRN, 0, 4)

static void Q3ResetTransform_New(ppc_cpu *c) { Q3RETURN(q3_data_new(RSET, 0)); }
static void Q3ResetTransform_Submit(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(0));
    if (v)
        q3v_reset_transform(v);
    Q3RETURN(v ? kQ3Success : kQ3Failure);
}

/* ---------------------------------------------------------------------- */
/* Shaders and textures */

static gaddr new_shader(uint32_t type)
{
    gaddr g = q3_data_new(type, 0);
    q3node *d = g ? q3_get(g)->data : NULL;
    if (d) { /* the identity uv transform, wrapping */
        q3d_setf(d->w, 0, 1);
        q3d_setf(d->w, 4, 1);
        q3d_setf(d->w, 8, 1);
    }
    return g;
}

static void Q3Shader_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_SHADER)); }
static void Q3Shader_Submit(ppc_cpu *c) { Q3Shape_Submit_generic(c, Q3_SHADER); }
Q3_SETW(Q3Shader_SetUVTransform, Q3_SHADER, 0, 9)
Q3_GETW(Q3Shader_GetUVTransform, Q3_SHADER, 0, 9)
Q3_SETV(Q3Shader_SetUBoundary, Q3_SHADER, 9)
Q3_SETV(Q3Shader_SetVBoundary, Q3_SHADER, 10)
Q3_GETW(Q3Shader_GetUBoundary, Q3_SHADER, 9, 1)
Q3_GETW(Q3Shader_GetVBoundary, Q3_SHADER, 10, 1)
static void Q3IlluminationShader_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_ILLUMINATION)); }
static void Q3PhongIllumination_New(ppc_cpu *c) { Q3RETURN(new_shader(PHIL)); }
static void Q3LambertIllumination_New(ppc_cpu *c) { Q3RETURN(new_shader(LMIL)); }
static void Q3NULLIllumination_New(ppc_cpu *c) { Q3RETURN(new_shader(NUIL)); }
static void Q3SurfaceShader_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_SURFACE_SHADER)); }

static void Q3TextureShader_New(ppc_cpu *c)
{
    gaddr g = new_shader(TXSU);
    q3node *d = g ? q3_get(g)->data : NULL;
    if (d)
        q3_assign(&d->w[11], Q3R(0));
    Q3RETURN(g);
}
Q3_GETO(Q3TextureShader_GetTexture, TXSU, 11)
Q3_SETO(Q3TextureShader_SetTexture, TXSU, 11)

static void Q3Texture_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_TEXTURE)); }

/* The texture's pixels: a pixmap's, a mipmap's first image, or a
 * compressed pixmap's decompressed (none yet). */
uint32_t *q3_texture_argb(q3obj *t, int *w, int *h);
uint32_t *q3_texture_argb(q3obj *t, int *w, int *h)
{
    if (!t || !t->data)
        return NULL;
    const uint32_t *d = ((q3node *)t->data)->w;
    if (t->type == TXPM) {
        *w = (int)d[1];
        *h = (int)d[2];
        return q3_storage_pixmap_argb(d, d[0]);
    }
    if (t->type == TXMM) {
        /* As a storage pixmap: image, then the first level's size,
         * rowBytes, then pixel size, type and orders, from its offset. */
        uint32_t off = d[9];
        if (off) /* a first level not at the start: not seen in practice */
            return NULL;
        uint32_t sp[8] = { d[0], d[6], d[7], d[8], d[2] == 0 || d[2] == 1 ? 32u : 16u, d[2], d[3], d[4] };
        *w = (int)d[6];
        *h = (int)d[7];
        return q3_storage_pixmap_argb(sp, d[0]);
    }
    return NULL;
}

static void texture_size(ppc_cpu *c, bool height)
{
    q3obj *o = q3_of(Q3R(0), Q3_TEXTURE);
    uint32_t v = 0;
    if (o && o->data) {
        const uint32_t *d = ((q3node *)o->data)->w;
        v = o->type == TXPM ? d[height ? 2 : 1] : o->type == TXMM ? d[height ? 7 : 6] : d[height ? 5 : 4];
    }
    if (o && Q3R(1))
        wr32(Q3R(1), v);
    Q3RETURN(o && Q3R(1) ? kQ3Success : kQ3Failure);
}
static void Q3Texture_GetWidth(ppc_cpu *c) { texture_size(c, false); }
static void Q3Texture_GetHeight(ppc_cpu *c) { texture_size(c, true); }
Q3_NEWDATA(Q3PixmapTexture_New, TXPM)
Q3_GETDATA(Q3PixmapTexture_GetPixmap, TXPM)
Q3_SETDATA(Q3PixmapTexture_SetPixmap, TXPM)
Q3_NEWDATA(Q3MipmapTexture_New, TXMM)
Q3_GETDATA(Q3MipmapTexture_GetMipmap, TXMM)
Q3_SETDATA(Q3MipmapTexture_SetMipmap, TXMM)
Q3_NEWDATA(Q3CompressedPixmapTexture_New, TXCP)
Q3_GETDATA(Q3CompressedPixmapTexture_GetCompressedPixmap, TXCP)
Q3_SETDATA(Q3CompressedPixmapTexture_SetCompressedPixmap, TXCP)

static void Q3CompressedPixmapTexture_CompressImage(ppc_cpu *c)
{
    q3_error(kQ3ErrorUnimplemented);
    Q3RETURN(kQ3Failure);
}

/* ---------------------------------------------------------------------- */
/* Draw contexts */

static void Q3DrawContext_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_DRAW_CONTEXT)); }

static void Q3DrawContext_GetData(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), Q3_DRAW_CONTEXT);
    if (d && Q3R(1))
        q3d_to_guest(Q3R(1), d, &dctx_s, 0);
    Q3RETURN(d && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3DrawContext_SetData(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), Q3_DRAW_CONTEXT);
    q3node *n = d && Q3R(1) ? q3d_from_guest(Q3R(1), &dctx_s, 1, NULL) : NULL;
    if (n) {
        memcpy(d->w, n->w, 17 * 4);
        q3d_set_kid(d, &dctx_s, 0, 0, q3d_kid(n, &dctx_s, 0, 0));
        n->kids[0] = NULL;
        d->w[10] = n->w[10];
        q3d_free(n, &dctx_s);
        q3_edited(q3_get(Q3R(0)));
    }
    Q3RETURN(n ? kQ3Success : kQ3Failure);
}

Q3_SETW(Q3DrawContext_SetClearImageColor, Q3_DRAW_CONTEXT, 1, 4)
Q3_GETW(Q3DrawContext_GetClearImageColor, Q3_DRAW_CONTEXT, 1, 4)
Q3_SETW(Q3DrawContext_SetPane, Q3_DRAW_CONTEXT, 5, 4)
Q3_GETW(Q3DrawContext_GetPane, Q3_DRAW_CONTEXT, 5, 4)
Q3_SETV(Q3DrawContext_SetPaneState, Q3_DRAW_CONTEXT, 9)
Q3_GETW(Q3DrawContext_GetPaneState, Q3_DRAW_CONTEXT, 9, 1)
Q3_SETV(Q3DrawContext_SetClearImageMethod, Q3_DRAW_CONTEXT, 0)
Q3_GETW(Q3DrawContext_GetClearImageMethod, Q3_DRAW_CONTEXT, 0, 1)
Q3_SETV(Q3DrawContext_SetMaskState, Q3_DRAW_CONTEXT, 15)
Q3_GETW(Q3DrawContext_GetMaskState, Q3_DRAW_CONTEXT, 15, 1)
Q3_SETV(Q3DrawContext_SetDoubleBufferState, Q3_DRAW_CONTEXT, 16)
Q3_GETW(Q3DrawContext_GetDoubleBufferState, Q3_DRAW_CONTEXT, 16, 1)

/* A TQ3Bitmap field: its image copied in or out. */
static void bitmap_field(ppc_cpu *c, uint32_t type, const q3schema *s, int field, int off, bool set)
{
    q3node *d = q3_data_of(Q3R(0), type);
    gaddr b = Q3R(1);
    if (!d || !b) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (set) {
        for (int k = 1; k < 5; k++)
            d->w[off + k] = rd32(b + 4 * (uint32_t)k);
        uint32_t n = d->w[off + 2] * d->w[off + 3];
        q3node *img = NULL;
        if (rd32(b) && n < (64u << 20)) {
            img = calloc(1, sizeof *img);
            if (img) {
                img->bytes = malloc(n ? n : 1);
                img->nbytes = n;
                if (img->bytes && n)
                    gget(img->bytes, rd32(b), n);
            }
        }
        q3d_set_kid(d, s, 0, field, img);
        q3_edited(q3_get(Q3R(0)));
    } else {
        q3node *img = q3d_kid(d, s, 0, field);
        gaddr g = img ? q3_galloc(img->nbytes) : 0;
        if (g && img->nbytes)
            gput(g, img->bytes, img->nbytes);
        wr32(b, g);
        for (int k = 1; k < 5; k++)
            wr32(b + 4 * (uint32_t)k, d->w[off + k]);
    }
    Q3RETURN(kQ3Success);
}
static void Q3DrawContext_SetMask(ppc_cpu *c) { bitmap_field(c, Q3_DRAW_CONTEXT, &dctx_s, 0, 10, true); }
static void Q3DrawContext_GetMask(ppc_cpu *c) { bitmap_field(c, Q3_DRAW_CONTEXT, &dctx_s, 0, 10, false); }

Q3_NEWDATA(Q3PixmapDrawContext_New, DPXP)
Q3_SETW(Q3PixmapDrawContext_SetPixmap, DPXP, 17, 8)
Q3_GETW(Q3PixmapDrawContext_GetPixmap, DPXP, 17, 8)
Q3_NEWDATA(Q3MacDrawContext_New, DMAC)
Q3_SETV(Q3MacDrawContext_SetWindow, DMAC, 17)
Q3_GETW(Q3MacDrawContext_GetWindow, DMAC, 17, 1)
Q3_SETV(Q3MacDrawContext_SetGXViewPort, DMAC, 19)
Q3_GETW(Q3MacDrawContext_GetGXViewPort, DMAC, 19, 1)
Q3_SETV(Q3MacDrawContext_SetGrafPort, DMAC, 20)
Q3_GETW(Q3MacDrawContext_GetGrafPort, DMAC, 20, 1)
Q3_SETV(Q3MacDrawContext_Set2DLibrary, DMAC, 18)
Q3_GETW(Q3MacDrawContext_Get2DLibrary, DMAC, 18, 1)

/* ---------------------------------------------------------------------- */
/* Renderers: the interactive one (drawn by r3d), the wireframe one, and
 * the generic one that draws nothing. */

enum { R_CSG, R_VENDOR, R_ENGINE, R_BYPASS, R_HINTS, R_FILTER };

gaddr q3_renderer_new(uint32_t type);
gaddr q3_renderer_new(uint32_t type)
{
    if (type != CTWN && type != WRFR && type != GNRR) {
        q3_error(kQ3ErrorInvalidObjectClass);
        return 0;
    }
    gaddr g = q3_data_new(type, 0);
    q3node *d = g ? q3_get(g)->data : NULL;
    if (d) {
        d->w[R_CSG] = 0x88888888u; /* kQ3CSGEquationAandB */
        d->w[R_VENDOR] = d->w[R_ENGINE] = 0xFFFFFFFFu; /* kQAVendor_BestChoice */
        d->w[R_FILTER] = 1;                            /* kQATextureFilter_Mid */
    }
    return g;
}

static void Q3Renderer_NewFromType(ppc_cpu *c) { Q3RETURN(q3_renderer_new(Q3R(0))); }
static void Q3Renderer_GetType(ppc_cpu *c) { Q3RETURN(type_below(Q3R(0), Q3_RENDERER)); }
static void Q3Renderer_IsInteractive(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_RENDERER);
    Q3RETURN(o && o->type == CTWN);
}
static void Q3Renderer_HasModalConfigure(ppc_cpu *c) { Q3RETURN(kQ3False); }
static void Q3Renderer_ModalConfigure(ppc_cpu *c)
{
    if (Q3R(2))
        wr32(Q3R(2), kQ3True);
    Q3RETURN(kQ3Success);
}

static void Q3RendererClass_GetNickNameString(ppc_cpu *c)
{
    const char *name = Q3R(0) == CTWN ? "Interactive Renderer" : Q3R(0) == WRFR ? "WireFrame" : Q3R(0) == GNRR ? "Generic" : NULL;
    if (Q3R(1))
        gput(Q3R(1), name ? name : "", name ? (uint32_t)strlen(name) + 1 : 1);
    Q3RETURN(name ? kQ3Success : kQ3Failure);
}

/* The configuration data: the interactive renderer's preferences. */
static void Q3Renderer_GetConfigurationData(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), Q3_RENDERER);
    gaddr buf = Q3R(1);
    uint32_t size = Q3R(2);
    if (Q3R(3))
        wr32(Q3R(3), d ? 24 : 0);
    if (d && buf && size >= 24)
        for (int k = 0; k < 6; k++)
            wr32(buf + 4 * (uint32_t)k, d->w[k]);
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}

static void Q3Renderer_SetConfigurationData(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), Q3_RENDERER);
    gaddr buf = Q3R(1);
    if (d && buf && Q3R(2) >= 24)
        for (int k = 0; k < 6; k++)
            d->w[k] = rd32(buf + 4 * (uint32_t)k);
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}

Q3_SETV(Q3InteractiveRenderer_SetCSGEquation, CTWN, R_CSG)
Q3_GETW(Q3InteractiveRenderer_GetCSGEquation, CTWN, R_CSG, 1)
static void Q3InteractiveRenderer_SetPreferences(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), CTWN);
    if (d) {
        d->w[R_VENDOR] = Q3R(1);
        d->w[R_ENGINE] = Q3R(2);
    }
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}
static void Q3InteractiveRenderer_GetPreferences(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), CTWN);
    if (d && Q3R(1))
        wr32(Q3R(1), d->w[R_VENDOR]);
    if (d && Q3R(2))
        wr32(Q3R(2), d->w[R_ENGINE]);
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}
Q3_SETV(Q3InteractiveRenderer_SetDoubleBufferBypass, CTWN, R_BYPASS)
Q3_GETW(Q3InteractiveRenderer_GetDoubleBufferBypass, CTWN, R_BYPASS, 1)
Q3_SETV(Q3InteractiveRenderer_SetRAVEContextHints, CTWN, R_HINTS)
Q3_GETW(Q3InteractiveRenderer_GetRAVEContextHints, CTWN, R_HINTS, 1)
Q3_SETV(Q3InteractiveRenderer_SetRAVETextureFilter, CTWN, R_FILTER)
Q3_GETW(Q3InteractiveRenderer_GetRAVETextureFilter, CTWN, R_FILTER, 1)

/* The interactive renderer draws through the host, not RAVE draw
 * contexts a program could reach. */
static void Q3InteractiveRenderer_CountRAVEDrawContexts(ppc_cpu *c)
{
    if (Q3R(1))
        wr32(Q3R(1), 0);
    Q3RETURN(q3_of(Q3R(0), CTWN) ? kQ3Success : kQ3Failure);
}
static void Q3InteractiveRenderer_GetRAVEDrawContexts(ppc_cpu *c)
{
    if (Q3R(3))
        wr32(Q3R(3), 0);
    Q3RETURN(q3_of(Q3R(0), CTWN) ? kQ3Success : kQ3Failure);
}

static void Q3XAttributeSet_GetMask(ppc_cpu *c)
{
    q3set *s = q3_set_of(Q3R(0));
    uint32_t m = 0;
    for (int i = 0; s && i < s->n; i++)
        m |= s->e[i].type > 0 && s->e[i].type < kQ3AttributeTypeNumTypes ? 1u << (s->e[i].type - 1) : 0x80000000u;
    Q3RETURN(m);
}

/* ---------------------------------------------------------------------- */
/* View hints: what a file says about how to view its contents */

static void Q3ViewHints_New(ppc_cpu *c)
{
    gaddr g = q3_data_new(VWHN, 0);
    q3node *d = g ? q3_get(g)->data : NULL;
    q3view *v = q3v_of(Q3R(0));
    if (d && v) { /* the view's renderer, camera and lights */
        gaddr parts[3];
        q3v_parts(v, parts);
        for (int k = 0; k < 3; k++)
            q3_assign(&d->w[k], parts[k]);
    }
    Q3RETURN(g);
}
Q3_SETO(Q3ViewHints_SetRenderer, VWHN, 0)
Q3_GETO(Q3ViewHints_GetRenderer, VWHN, 0)
Q3_SETO(Q3ViewHints_SetCamera, VWHN, 1)
Q3_GETO(Q3ViewHints_GetCamera, VWHN, 1)
Q3_SETO(Q3ViewHints_SetLightGroup, VWHN, 2)
Q3_GETO(Q3ViewHints_GetLightGroup, VWHN, 2)
Q3_SETO(Q3ViewHints_SetAttributeSet, VWHN, 3)
Q3_GETO(Q3ViewHints_GetAttributeSet, VWHN, 3)
Q3_SETV(Q3ViewHints_SetDimensionsState, VWHN, 4)
Q3_GETW(Q3ViewHints_GetDimensionsState, VWHN, 4, 1)
static void Q3ViewHints_SetDimensions(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), VWHN);
    if (d) {
        d->w[5] = Q3R(1);
        d->w[6] = Q3R(2);
    }
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}
static void Q3ViewHints_GetDimensions(ppc_cpu *c)
{
    q3node *d = q3_data_of(Q3R(0), VWHN);
    if (d && Q3R(1))
        wr32(Q3R(1), d->w[5]);
    if (d && Q3R(2))
        wr32(Q3R(2), d->w[6]);
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}
Q3_SETV(Q3ViewHints_SetMaskState, VWHN, 7)
Q3_GETW(Q3ViewHints_GetMaskState, VWHN, 7, 1)
static void Q3ViewHints_SetMask(ppc_cpu *c) { bitmap_field(c, VWHN, &vwhn_s, 4, 8, true); }
static void Q3ViewHints_GetMask(ppc_cpu *c) { bitmap_field(c, VWHN, &vwhn_s, 4, 8, false); }
Q3_SETV(Q3ViewHints_SetClearImageMethod, VWHN, 13)
Q3_GETW(Q3ViewHints_GetClearImageMethod, VWHN, 13, 1)
Q3_SETW(Q3ViewHints_SetClearImageColor, VWHN, 14, 4)
Q3_GETW(Q3ViewHints_GetClearImageColor, VWHN, 14, 4)

/* ---------------------------------------------------------------------- */
/* State operators and submitting */

static void Q3Push_New(ppc_cpu *c) { Q3RETURN(q3_data_new(PUSH, 0)); }
static void Q3Pop_New(ppc_cpu *c) { Q3RETURN(q3_data_new(POP, 0)); }
static void Q3StateOperator_Submit(ppc_cpu *c) { Q3Shape_Submit_generic(c, Q3_STATE_OPERATOR); }

static void Q3Object_Submit(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(1));
    q3obj *o = q3_of(Q3R(0), 0);
    if (v && o)
        q3v_submit(v, o->g);
    Q3RETURN(v && o ? kQ3Success : kQ3Failure);
}

static void Q3AttributeSet_Submit(ppc_cpu *c) { Q3Shape_Submit_generic(c, Q3_ATTRIBUTE_SET); }

/* One attribute into the view's state. */
static void Q3Attribute_Submit(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(2));
    int32_t type = (int32_t)Q3R(0);
    if (!v || !Q3R(1) || q3_element_size(type) < 0) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3set *s = calloc(1, sizeof *s);
    q3obj *o = s ? q3_new(Q3_ATTRIBUTE_SET, s) : NULL;
    if (!o) {
        free(s);
        Q3RETURN(kQ3Failure);
        return;
    }
    q3set_add(s, type, Q3R(1));
    q3v_attribute_set(v, o->g);
    q3_release(o->g);
    Q3RETURN(kQ3Success);
}

#define E(n) { #n, n }
const ppc_export q3_shapes_exports[] = {
    E(Q3Camera_GetType), E(Q3Camera_SetData), E(Q3Camera_GetData), E(Q3Camera_SetPlacement),
    E(Q3Camera_GetPlacement), E(Q3Camera_SetRange), E(Q3Camera_GetRange), E(Q3Camera_SetViewPort),
    E(Q3Camera_GetViewPort), E(Q3Camera_GetWorldToView), E(Q3Camera_GetWorldToFrustum),
    E(Q3Camera_GetViewToFrustum), E(Q3OrthographicCamera_New), E(Q3OrthographicCamera_GetData),
    E(Q3OrthographicCamera_SetData), E(Q3OrthographicCamera_SetLeft), E(Q3OrthographicCamera_GetLeft),
    E(Q3OrthographicCamera_SetTop), E(Q3OrthographicCamera_GetTop), E(Q3OrthographicCamera_SetRight),
    E(Q3OrthographicCamera_GetRight), E(Q3OrthographicCamera_SetBottom), E(Q3OrthographicCamera_GetBottom),
    E(Q3ViewPlaneCamera_New), E(Q3ViewPlaneCamera_GetData), E(Q3ViewPlaneCamera_SetData),
    E(Q3ViewPlaneCamera_SetViewPlane), E(Q3ViewPlaneCamera_GetViewPlane), E(Q3ViewPlaneCamera_SetHalfWidth),
    E(Q3ViewPlaneCamera_GetHalfWidth), E(Q3ViewPlaneCamera_SetHalfHeight), E(Q3ViewPlaneCamera_GetHalfHeight),
    E(Q3ViewPlaneCamera_SetCenterX), E(Q3ViewPlaneCamera_GetCenterX), E(Q3ViewPlaneCamera_SetCenterY),
    E(Q3ViewPlaneCamera_GetCenterY), E(Q3ViewAngleAspectCamera_New), E(Q3ViewAngleAspectCamera_SetData),
    E(Q3ViewAngleAspectCamera_GetData), E(Q3ViewAngleAspectCamera_SetFOV), E(Q3ViewAngleAspectCamera_GetFOV),
    E(Q3ViewAngleAspectCamera_SetAspectRatio), E(Q3ViewAngleAspectCamera_GetAspectRatio),
    E(Q3Light_GetType), E(Q3Light_GetState), E(Q3Light_GetBrightness), E(Q3Light_GetColor), E(Q3Light_SetState),
    E(Q3Light_SetBrightness), E(Q3Light_SetColor), E(Q3Light_GetData), E(Q3Light_SetData), E(Q3AmbientLight_New),
    E(Q3AmbientLight_GetData), E(Q3AmbientLight_SetData), E(Q3DirectionalLight_New),
    E(Q3DirectionalLight_GetCastShadowsState), E(Q3DirectionalLight_GetDirection),
    E(Q3DirectionalLight_SetCastShadowsState), E(Q3DirectionalLight_SetDirection), E(Q3DirectionalLight_GetData),
    E(Q3DirectionalLight_SetData), E(Q3PointLight_New), E(Q3PointLight_GetCastShadowsState),
    E(Q3PointLight_GetAttenuation), E(Q3PointLight_GetLocation), E(Q3PointLight_GetData),
    E(Q3PointLight_SetCastShadowsState), E(Q3PointLight_SetAttenuation), E(Q3PointLight_SetLocation),
    E(Q3PointLight_SetData), E(Q3SpotLight_New), E(Q3SpotLight_GetCastShadowsState), E(Q3SpotLight_GetAttenuation),
    E(Q3SpotLight_GetLocation), E(Q3SpotLight_GetDirection), E(Q3SpotLight_GetHotAngle),
    E(Q3SpotLight_GetOuterAngle), E(Q3SpotLight_GetFallOff), E(Q3SpotLight_GetData),
    E(Q3SpotLight_SetCastShadowsState), E(Q3SpotLight_SetAttenuation), E(Q3SpotLight_SetLocation),
    E(Q3SpotLight_SetDirection), E(Q3SpotLight_SetHotAngle), E(Q3SpotLight_SetOuterAngle), E(Q3SpotLight_SetFallOff),
    E(Q3SpotLight_SetData),
    E(Q3Style_GetType), E(Q3Style_Submit),
    E(Q3PickIDStyle_New), E(Q3PickIDStyle_Submit), E(Q3PickIDStyle_Get), E(Q3PickIDStyle_Set),
    E(Q3PickPartsStyle_New), E(Q3PickPartsStyle_Submit), E(Q3PickPartsStyle_Get), E(Q3PickPartsStyle_Set),
    E(Q3ReceiveShadowsStyle_New), E(Q3ReceiveShadowsStyle_Submit), E(Q3ReceiveShadowsStyle_Get),
    E(Q3ReceiveShadowsStyle_Set), E(Q3FillStyle_New), E(Q3FillStyle_Submit), E(Q3FillStyle_Get), E(Q3FillStyle_Set),
    E(Q3BackfacingStyle_New), E(Q3BackfacingStyle_Submit), E(Q3BackfacingStyle_Get), E(Q3BackfacingStyle_Set),
    E(Q3InterpolationStyle_New), E(Q3InterpolationStyle_Submit), E(Q3InterpolationStyle_Get),
    E(Q3InterpolationStyle_Set), E(Q3OrientationStyle_New), E(Q3OrientationStyle_Submit), E(Q3OrientationStyle_Get),
    E(Q3OrientationStyle_Set), E(Q3HighlightStyle_New), E(Q3HighlightStyle_Submit), E(Q3HighlightStyle_Get),
    E(Q3HighlightStyle_Set), E(Q3SubdivisionStyle_New), E(Q3SubdivisionStyle_Submit), E(Q3SubdivisionStyle_GetData),
    E(Q3SubdivisionStyle_SetData), E(Q3AntiAliasStyle_New), E(Q3AntiAliasStyle_Submit), E(Q3AntiAliasStyle_GetData),
    E(Q3AntiAliasStyle_SetData), E(Q3FogStyle_New), E(Q3FogStyle_Submit), E(Q3FogStyle_GetData), E(Q3FogStyle_SetData),
    E(Q3Transform_GetType), E(Q3Transform_GetMatrix), E(Q3Transform_Submit), E(Q3MatrixTransform_New),
    E(Q3MatrixTransform_Submit), E(Q3MatrixTransform_Set), E(Q3MatrixTransform_Get), E(Q3RotateTransform_New),
    E(Q3RotateTransform_Submit), E(Q3RotateTransform_SetData), E(Q3RotateTransform_GetData),
    E(Q3RotateTransform_SetAxis), E(Q3RotateTransform_SetAngle), E(Q3RotateTransform_GetAxis),
    E(Q3RotateTransform_GetAngle), E(Q3RotateAboutPointTransform_New), E(Q3RotateAboutPointTransform_Submit),
    E(Q3RotateAboutPointTransform_SetData), E(Q3RotateAboutPointTransform_GetData),
    E(Q3RotateAboutPointTransform_SetAxis), E(Q3RotateAboutPointTransform_GetAxis),
    E(Q3RotateAboutPointTransform_SetAngle), E(Q3RotateAboutPointTransform_GetAngle),
    E(Q3RotateAboutPointTransform_SetAboutPoint), E(Q3RotateAboutPointTransform_GetAboutPoint),
    E(Q3RotateAboutAxisTransform_New), E(Q3RotateAboutAxisTransform_Submit), E(Q3RotateAboutAxisTransform_SetData),
    E(Q3RotateAboutAxisTransform_GetData), E(Q3RotateAboutAxisTransform_SetOrientation),
    E(Q3RotateAboutAxisTransform_GetOrientation), E(Q3RotateAboutAxisTransform_SetAngle),
    E(Q3RotateAboutAxisTransform_GetAngle), E(Q3RotateAboutAxisTransform_SetOrigin),
    E(Q3RotateAboutAxisTransform_GetOrigin), E(Q3ScaleTransform_New), E(Q3ScaleTransform_Submit),
    E(Q3ScaleTransform_Set), E(Q3ScaleTransform_Get), E(Q3TranslateTransform_New), E(Q3TranslateTransform_Submit),
    E(Q3TranslateTransform_Set), E(Q3TranslateTransform_Get), E(Q3QuaternionTransform_New),
    E(Q3QuaternionTransform_Submit), E(Q3QuaternionTransform_Set), E(Q3QuaternionTransform_Get),
    E(Q3ResetTransform_New), E(Q3ResetTransform_Submit),
    E(Q3Shader_GetType), E(Q3Shader_Submit), E(Q3Shader_SetUVTransform), E(Q3Shader_GetUVTransform),
    E(Q3Shader_SetUBoundary), E(Q3Shader_SetVBoundary), E(Q3Shader_GetUBoundary), E(Q3Shader_GetVBoundary),
    E(Q3IlluminationShader_GetType), E(Q3PhongIllumination_New), E(Q3LambertIllumination_New),
    E(Q3NULLIllumination_New), E(Q3SurfaceShader_GetType), E(Q3TextureShader_New), E(Q3TextureShader_GetTexture),
    E(Q3TextureShader_SetTexture), E(Q3Texture_GetType), E(Q3Texture_GetWidth), E(Q3Texture_GetHeight),
    E(Q3PixmapTexture_New), E(Q3PixmapTexture_GetPixmap), E(Q3PixmapTexture_SetPixmap), E(Q3MipmapTexture_New),
    E(Q3MipmapTexture_GetMipmap), E(Q3MipmapTexture_SetMipmap), E(Q3CompressedPixmapTexture_New),
    E(Q3CompressedPixmapTexture_GetCompressedPixmap), E(Q3CompressedPixmapTexture_SetCompressedPixmap),
    E(Q3CompressedPixmapTexture_CompressImage),
    E(Q3DrawContext_GetType), E(Q3DrawContext_SetData), E(Q3DrawContext_GetData), E(Q3DrawContext_SetClearImageColor),
    E(Q3DrawContext_GetClearImageColor), E(Q3DrawContext_SetPane), E(Q3DrawContext_GetPane),
    E(Q3DrawContext_SetPaneState), E(Q3DrawContext_GetPaneState), E(Q3DrawContext_SetClearImageMethod),
    E(Q3DrawContext_GetClearImageMethod), E(Q3DrawContext_SetMask), E(Q3DrawContext_GetMask),
    E(Q3DrawContext_SetMaskState), E(Q3DrawContext_GetMaskState), E(Q3DrawContext_SetDoubleBufferState),
    E(Q3DrawContext_GetDoubleBufferState), E(Q3PixmapDrawContext_New), E(Q3PixmapDrawContext_SetPixmap),
    E(Q3PixmapDrawContext_GetPixmap), E(Q3MacDrawContext_New), E(Q3MacDrawContext_SetWindow),
    E(Q3MacDrawContext_GetWindow), E(Q3MacDrawContext_SetGXViewPort), E(Q3MacDrawContext_GetGXViewPort),
    E(Q3MacDrawContext_SetGrafPort), E(Q3MacDrawContext_GetGrafPort), E(Q3MacDrawContext_Set2DLibrary),
    E(Q3MacDrawContext_Get2DLibrary),
    E(Q3Renderer_NewFromType), E(Q3Renderer_GetType), E(Q3Renderer_IsInteractive), E(Q3Renderer_HasModalConfigure),
    E(Q3Renderer_ModalConfigure), E(Q3RendererClass_GetNickNameString), E(Q3Renderer_GetConfigurationData),
    E(Q3Renderer_SetConfigurationData), E(Q3InteractiveRenderer_SetCSGEquation),
    E(Q3InteractiveRenderer_GetCSGEquation), E(Q3InteractiveRenderer_SetPreferences),
    E(Q3InteractiveRenderer_GetPreferences), E(Q3InteractiveRenderer_SetDoubleBufferBypass),
    E(Q3InteractiveRenderer_GetDoubleBufferBypass), E(Q3InteractiveRenderer_SetRAVEContextHints),
    E(Q3InteractiveRenderer_GetRAVEContextHints), E(Q3InteractiveRenderer_SetRAVETextureFilter),
    E(Q3InteractiveRenderer_GetRAVETextureFilter), E(Q3InteractiveRenderer_CountRAVEDrawContexts),
    E(Q3InteractiveRenderer_GetRAVEDrawContexts), E(Q3XAttributeSet_GetMask),
    E(Q3ViewHints_New), E(Q3ViewHints_SetRenderer), E(Q3ViewHints_GetRenderer), E(Q3ViewHints_SetCamera),
    E(Q3ViewHints_GetCamera), E(Q3ViewHints_SetLightGroup), E(Q3ViewHints_GetLightGroup),
    E(Q3ViewHints_SetAttributeSet), E(Q3ViewHints_GetAttributeSet), E(Q3ViewHints_SetDimensionsState),
    E(Q3ViewHints_GetDimensionsState), E(Q3ViewHints_SetDimensions), E(Q3ViewHints_GetDimensions),
    E(Q3ViewHints_SetMaskState), E(Q3ViewHints_GetMaskState), E(Q3ViewHints_SetMask), E(Q3ViewHints_GetMask),
    E(Q3ViewHints_SetClearImageMethod), E(Q3ViewHints_GetClearImageMethod), E(Q3ViewHints_SetClearImageColor),
    E(Q3ViewHints_GetClearImageColor),
    E(Q3Push_New), E(Q3Pop_New), E(Q3StateOperator_Submit), E(Q3Object_Submit), E(Q3AttributeSet_Submit),
    E(Q3Attribute_Submit),
};
const unsigned q3_shapes_export_count = sizeof q3_shapes_exports / sizeof q3_shapes_exports[0];
