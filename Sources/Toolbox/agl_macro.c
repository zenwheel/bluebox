/*
 * AGL's macro dispatch (agl_macro.h): a program built with it calls gl
 * through the context itself, not OpenGLLibrary's exports.  An AGLContext
 * starts with a renderer word, passed as each call's first argument, and
 * then a table of transition vectors, one per OpenGL 1.1 call in
 * alphabetical order (Cro-Mag Rally's glClearColor is the thirteenth).
 *
 * Every entry's code is one native routine; the vector's TOC word is the
 * call's index, which arrives in r2.  It drops the renderer word, moving
 * the general registers down one place and reading the stack one word
 * further on, and calls our export of that name, which works on the
 * current context.
 */
#include <stdio.h>

#include "hostgl.h"
#include "tb_internal.h"

static const char *const names[AGL_DISPATCH_COUNT] = {
    "glAccum", "glAlphaFunc", "glAreTexturesResident", "glArrayElement", "glBegin", "glBindTexture",
    "glBitmap", "glBlendFunc", "glCallList", "glCallLists", "glClear", "glClearAccum", "glClearColor",
    "glClearDepth", "glClearIndex", "glClearStencil", "glClipPlane", "glColor3b", "glColor3bv", "glColor3d",
    "glColor3dv", "glColor3f", "glColor3fv", "glColor3i", "glColor3iv", "glColor3s", "glColor3sv",
    "glColor3ub", "glColor3ubv", "glColor3ui", "glColor3uiv", "glColor3us", "glColor3usv", "glColor4b",
    "glColor4bv", "glColor4d", "glColor4dv", "glColor4f", "glColor4fv", "glColor4i", "glColor4iv",
    "glColor4s", "glColor4sv", "glColor4ub", "glColor4ubv", "glColor4ui", "glColor4uiv", "glColor4us",
    "glColor4usv", "glColorMask", "glColorMaterial", "glColorPointer", "glCopyPixels", "glCopyTexImage1D",
    "glCopyTexImage2D", "glCopyTexSubImage1D", "glCopyTexSubImage2D", "glCullFace", "glDeleteLists",
    "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDepthRange", "glDisable", "glDisableClientState",
    "glDrawArrays", "glDrawBuffer", "glDrawElements", "glDrawPixels", "glEdgeFlag", "glEdgeFlagPointer",
    "glEdgeFlagv", "glEnable", "glEnableClientState", "glEnd", "glEndList", "glEvalCoord1d", "glEvalCoord1dv",
    "glEvalCoord1f", "glEvalCoord1fv", "glEvalCoord2d", "glEvalCoord2dv", "glEvalCoord2f", "glEvalCoord2fv",
    "glEvalMesh1", "glEvalMesh2", "glEvalPoint1", "glEvalPoint2", "glFeedbackBuffer", "glFinish", "glFlush",
    "glFogf", "glFogfv", "glFogi", "glFogiv", "glFrontFace", "glFrustum", "glGenLists", "glGenTextures",
    "glGetBooleanv", "glGetClipPlane", "glGetDoublev", "glGetError", "glGetFloatv", "glGetIntegerv",
    "glGetLightfv", "glGetLightiv", "glGetMapdv", "glGetMapfv", "glGetMapiv", "glGetMaterialfv",
    "glGetMaterialiv", "glGetPixelMapfv", "glGetPixelMapuiv", "glGetPixelMapusv", "glGetPointerv",
    "glGetPolygonStipple", "glGetString", "glGetTexEnvfv", "glGetTexEnviv", "glGetTexGendv", "glGetTexGenfv",
    "glGetTexGeniv", "glGetTexImage", "glGetTexLevelParameterfv", "glGetTexLevelParameteriv",
    "glGetTexParameterfv", "glGetTexParameteriv", "glHint", "glIndexMask", "glIndexPointer", "glIndexd",
    "glIndexdv", "glIndexf", "glIndexfv", "glIndexi", "glIndexiv", "glIndexs", "glIndexsv", "glIndexub",
    "glIndexubv", "glInitNames", "glInterleavedArrays", "glIsEnabled", "glIsList", "glIsTexture",
    "glLightModelf", "glLightModelfv", "glLightModeli", "glLightModeliv", "glLightf", "glLightfv", "glLighti",
    "glLightiv", "glLineStipple", "glLineWidth", "glListBase", "glLoadIdentity", "glLoadMatrixd",
    "glLoadMatrixf", "glLoadName", "glLogicOp", "glMap1d", "glMap1f", "glMap2d", "glMap2f", "glMapGrid1d",
    "glMapGrid1f", "glMapGrid2d", "glMapGrid2f", "glMaterialf", "glMaterialfv", "glMateriali", "glMaterialiv",
    "glMatrixMode", "glMultMatrixd", "glMultMatrixf", "glNewList", "glNormal3b", "glNormal3bv", "glNormal3d",
    "glNormal3dv", "glNormal3f", "glNormal3fv", "glNormal3i", "glNormal3iv", "glNormal3s", "glNormal3sv",
    "glNormalPointer", "glOrtho", "glPassThrough", "glPixelMapfv", "glPixelMapuiv", "glPixelMapusv",
    "glPixelStoref", "glPixelStorei", "glPixelTransferf", "glPixelTransferi", "glPixelZoom", "glPointSize",
    "glPolygonMode", "glPolygonOffset", "glPolygonStipple", "glPopAttrib", "glPopClientAttrib", "glPopMatrix",
    "glPopName", "glPrioritizeTextures", "glPushAttrib", "glPushClientAttrib", "glPushMatrix", "glPushName",
    "glRasterPos2d", "glRasterPos2dv", "glRasterPos2f", "glRasterPos2fv", "glRasterPos2i", "glRasterPos2iv",
    "glRasterPos2s", "glRasterPos2sv", "glRasterPos3d", "glRasterPos3dv", "glRasterPos3f", "glRasterPos3fv",
    "glRasterPos3i", "glRasterPos3iv", "glRasterPos3s", "glRasterPos3sv", "glRasterPos4d", "glRasterPos4dv",
    "glRasterPos4f", "glRasterPos4fv", "glRasterPos4i", "glRasterPos4iv", "glRasterPos4s", "glRasterPos4sv",
    "glReadBuffer", "glReadPixels", "glRectd", "glRectdv", "glRectf", "glRectfv", "glRecti", "glRectiv",
    "glRects", "glRectsv", "glRenderMode", "glRotated", "glRotatef", "glScaled", "glScalef", "glScissor",
    "glSelectBuffer", "glShadeModel", "glStencilFunc", "glStencilMask", "glStencilOp", "glTexCoord1d",
    "glTexCoord1dv", "glTexCoord1f", "glTexCoord1fv", "glTexCoord1i", "glTexCoord1iv", "glTexCoord1s",
    "glTexCoord1sv", "glTexCoord2d", "glTexCoord2dv", "glTexCoord2f", "glTexCoord2fv", "glTexCoord2i",
    "glTexCoord2iv", "glTexCoord2s", "glTexCoord2sv", "glTexCoord3d", "glTexCoord3dv", "glTexCoord3f",
    "glTexCoord3fv", "glTexCoord3i", "glTexCoord3iv", "glTexCoord3s", "glTexCoord3sv", "glTexCoord4d",
    "glTexCoord4dv", "glTexCoord4f", "glTexCoord4fv", "glTexCoord4i", "glTexCoord4iv", "glTexCoord4s",
    "glTexCoord4sv", "glTexCoordPointer", "glTexEnvf", "glTexEnvfv", "glTexEnvi", "glTexEnviv", "glTexGend",
    "glTexGendv", "glTexGenf", "glTexGenfv", "glTexGeni", "glTexGeniv", "glTexImage1D", "glTexImage2D",
    "glTexParameterf", "glTexParameterfv", "glTexParameteri", "glTexParameteriv", "glTexSubImage1D",
    "glTexSubImage2D", "glTranslated", "glTranslatef", "glVertex2d", "glVertex2dv", "glVertex2f",
    "glVertex2fv", "glVertex2i", "glVertex2iv", "glVertex2s", "glVertex2sv", "glVertex3d", "glVertex3dv",
    "glVertex3f", "glVertex3fv", "glVertex3i", "glVertex3iv", "glVertex3s", "glVertex3sv", "glVertex4d",
    "glVertex4dv", "glVertex4f", "glVertex4fv", "glVertex4i", "glVertex4iv", "glVertex4s", "glVertex4sv",
    "glVertexPointer", "glViewport",
};

static void (*fns[AGL_DISPATCH_COUNT])(ppc_cpu *);

static void unavailable(ppc_cpu *c)
{
    c->gpr[3] = 0;
    c->pc = c->lr;
}

static void dispatch(ppc_cpu *c)
{
    uint32_t i = c->gpr[2];
    if (i >= AGL_DISPATCH_COUNT) {
        unavailable(c);
        return;
    }
    if (!fns[i]) {
        fns[i] = cfm_export_fn(names[i]);
        if (!fns[i]) {
            LOGF(LOG_UNIMPL, "AGL dispatch: %s not provided\n", names[i]);
            fns[i] = unavailable;
        }
    }
    uint32_t sp = c->gpr[1];
    for (int k = 0; k < 7; k++)
        c->gpr[3 + k] = c->gpr[4 + k];
    c->gpr[10] = rd32(sp + 24 + 32);
    c->gpr[1] = sp + 4; /* stack arguments: one word on */
    fns[i](c);
    c->gpr[1] = sp;
}

void agl_macro_fill(gaddr table, gaddr vectors)
{
    gaddr tv = cfm_native_tvector(dispatch, "AGL dispatch");
    gaddr code = tv ? rd32(tv) : 0;
    for (uint32_t i = 0; i < AGL_DISPATCH_COUNT; i++) {
        wr32(vectors + 8 * i, code);
        wr32(vectors + 8 * i + 4, i);
        wr32(table + 4 * i, code ? vectors + 8 * i : 0);
    }
}
