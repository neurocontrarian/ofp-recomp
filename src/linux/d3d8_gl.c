/*
 * d3d8_gl.c - the OpenGL renderer of d3d8.c (compatibility profile, fixed
 * function).
 *
 *   - the device renders into an FBO the size of the back buffer (any size:
 *     the game's resolution is the screen's), Present scales it into the
 *     window with its aspect kept, as screen.c would place a DirectDraw frame;
 *   - fixed-function T&L maps onto GL's: D3D's row-vector matrices are GL's
 *     column-major ones as they lie in memory; the projection is followed by
 *     a fix-up (y flipped so the FBO's row 0 is the picture's top as in D3D,
 *     z from [0,w] to [-w,w], D3D's integer pixel centres);
 *   - pre-transformed vertices (XYZRHW) are drawn through an orthographic
 *     projection with w = 1/rhw restored, so texturing stays perspective-correct;
 *   - texture stages map onto GL_COMBINE; formats include DXT1-5 (S3TC);
 *   - vertex and index buffers are drawn from GL buffer copies, brought up to
 *     date before a draw; a texture level is uploaded when next drawn with.
 */
#define GL_GLEXT_PROTOTYPES 1
#include <math.h>
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include "d3d8_int.h"

SDL_Window *hle_screen_sdl_window(void);                                   /* screen.c */
void hle_screen_gl_picture(int src_w, int src_h, int x, int y, int w, int h);

typedef struct { GLenum internal, format, type; int bpp, block; } glfmt_t;   /* block: bytes per 4x4, 0 if not compressed */
static int gl_fmt(uint32_t f, glfmt_t *g) {
    glfmt_t t = { 0, 0, 0, 0, 0 };
    switch (f) {

    case FMT_A8R8G8B8: t = (glfmt_t){ GL_RGBA8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 4, 0 }; break;
    case FMT_X8R8G8B8: t = (glfmt_t){ GL_RGB8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, 4, 0 }; break;
    case FMT_R8G8B8:   t = (glfmt_t){ GL_RGB8, GL_BGR, GL_UNSIGNED_BYTE, 3, 0 }; break;
    case FMT_R5G6B5:   t = (glfmt_t){ GL_RGB5, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, 0 }; break;
    case FMT_X1R5G5B5: t = (glfmt_t){ GL_RGB5, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, 0 }; break;
    case FMT_A1R5G5B5: t = (glfmt_t){ GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, 0 }; break;
    case FMT_A4R4G4B4: t = (glfmt_t){ GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2, 0 }; break;
    case FMT_X4R4G4B4: t = (glfmt_t){ GL_RGB4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2, 0 }; break;
    case FMT_A8:       t = (glfmt_t){ GL_ALPHA8, GL_ALPHA, GL_UNSIGNED_BYTE, 1, 0 }; break;
    case FMT_L8:       t = (glfmt_t){ GL_LUMINANCE8, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1, 0 }; break;
    case FMT_A8L8:     t = (glfmt_t){ GL_LUMINANCE8_ALPHA8, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2, 0 }; break;
    case FMT_DXT1:     t = (glfmt_t){ GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 0, 8 }; break;
    case FMT_DXT2: case FMT_DXT3: t = (glfmt_t){ GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 0, 16 }; break;
    case FMT_DXT4: case FMT_DXT5: t = (glfmt_t){ GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 0, 16 }; break;
    case FMT_D16: case FMT_D16_LOCKABLE: case FMT_D15S1: t = (glfmt_t){ GL_DEPTH24_STENCIL8, 0, 0, 2, 0 }; break;
    case FMT_D24S8: case FMT_D24X8: case FMT_D24X4S4: case FMT_D32: t = (glfmt_t){ GL_DEPTH24_STENCIL8, 0, 0, 4, 0 }; break;
    default: return 0;
    }
    if (g) *g = t;
    return 1;
}

/* ------------------------------------------------------------------ GL setup */
/* Antialiasing (MSAA): env OFP_MSAA = 2, 4, 8... samples. The game draws its
 * back buffer into multisampled renderbuffers; Present (and a read of the
 * back buffer) resolves them into back_tex, which the rest reads as before.
 * Render-to-texture targets stay single-sampled. */
static int msaa_samples(void) {
    static GLint max = -1;
    if (max < 0) glGetIntegerv(GL_MAX_SAMPLES, &max);
    int n = msaa_wanted();                             /* OFP_MSAA, or the options' choice */
    if (n > max) n = max;
    if (n < 2) n = 0;
    perf_note("antialiasing: %s%d samples (the driver allows %d)", n ? "" : "off, ", n, (int)max);
    return n;
}
static void make_backbuffer(d3ddev_t *d) {
    if (d->fbo) {
        glDeleteFramebuffers(1, &d->fbo);
        glDeleteTextures(1, &d->back_tex);
        glDeleteRenderbuffers(1, &d->back_depth);
        if (d->msaa) {
            glDeleteRenderbuffers(1, &d->ms_color), glDeleteRenderbuffers(1, &d->ms_depth);
            glDeleteFramebuffers(1, &d->rfbo);
        }
    }
    d->msaa = msaa_samples();
    if (d->msaa) {
        glGenRenderbuffers(1, &d->ms_color);
        glBindRenderbuffer(GL_RENDERBUFFER, d->ms_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, d->msaa, GL_RGBA8, d->bw, d->bh);
        glGenRenderbuffers(1, &d->ms_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, d->ms_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, d->msaa, GL_DEPTH24_STENCIL8, d->bw, d->bh);
        glGenFramebuffers(1, &d->rfbo);
    }
    glGenFramebuffers(1, &d->fbo);
    glGenTextures(1, &d->back_tex);
    glBindTexture(GL_TEXTURE_2D, d->back_tex);
    d->d_tss = 1, d->comb_ok = 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, d->bw, d->bh, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &d->back_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, d->back_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, d->bw, d->bh);
    if (d->back) d->back->gltex = d->back_tex;
    if (d->depth) d->depth->rb = d->back_depth;
    if (d->msaa) {
        glBindFramebuffer(GL_FRAMEBUFFER, d->rfbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->back_tex, 0);
    }
}
/* the multisampled back buffer into back_tex; leaves rfbo bound for reading */
static void msaa_resolve(d3ddev_t *d) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, d->fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, d->ms_color);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d->rfbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, d->bw, d->bh, 0, 0, d->bw, d->bh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, d->rfbo);
}
static void bind_targets(d3ddev_t *d) {
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    GLuint zb = d->zs ? d->zs->rb : 0;
    if (d->msaa && !d->rt) {                           /* the back buffer, multisampled */
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, d->ms_color);
        /* a multisampled colour takes a multisampled depth: the device's own,
         * also for a depth surface the game made (single-sampled, it would
         * leave the framebuffer incomplete) */
        if (d->zs) zb = d->ms_depth;
    } else {
        if (d->msaa) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
        GLuint ct = d->rt ? d->rt->gltex : d->back_tex;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ct, 0);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, zb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[d3d8] framebuffer incomplete 0x%X\n", st);
}
/* ------------------------------------------------------------------ textures: upload and sampling */
static void tex_upload(tex_t *t) {
    glfmt_t g;
    if (!gl_fmt(t->fmt, &g)) return;
    glBindTexture(GL_TEXTURE_2D, t->gl);
    if (!t->allocated) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
        t->wrap_s = t->wrap_t = t->minf = t->magf = -1;
        t->aniso = -1;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int l = 0; l < t->levels; l++) {
        if (!t->allocated || (t->dirty & (1u << l))) {
            surf_t *s = t->lvl[l];
            perf_texture(s->size);
            if (g.block) glCompressedTexImage2D(GL_TEXTURE_2D, l, g.internal, s->w, s->h, 0, (GLsizei)s->size, s->mem);
            else {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(s->pitch / (uint32_t)g.bpp));
                glTexImage2D(GL_TEXTURE_2D, l, (GLint)g.internal, s->w, s->h, 0, g.format, g.type, s->mem);
            }
        }
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    t->allocated = 1;
    t->dirty = 0;
}
static GLint gl_wrap(uint32_t a) {
    switch (a) { case 2: return GL_MIRRORED_REPEAT; case 3: return GL_CLAMP_TO_EDGE; case 4: return GL_CLAMP_TO_BORDER;
                 case 5: return GL_MIRRORED_REPEAT; default: return GL_REPEAT; }
}
static void tex_sampler(tex_t *t, const uint32_t *ts) {
    GLint ws = gl_wrap(ts[13]), wt = gl_wrap(ts[14]);
    uint32_t mag = ts[16], min = ts[17], mip = ts[18];
    GLint magf = mag == 1 ? GL_NEAREST : GL_LINEAR, minf;
    int lin = min != 1;
    if (t->levels <= 1 || mip == 0) minf = lin ? GL_LINEAR : GL_NEAREST;
    else if (mip == 1) minf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else minf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    float an = (min == 3 || mag == 3) ? (float)(ts[21] ? ts[21] : 1) : 1.0f;
    if (ws != t->wrap_s) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws), t->wrap_s = ws;
    if (wt != t->wrap_t) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt), t->wrap_t = wt;
    if (magf != t->magf) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf), t->magf = magf;
    if (minf != t->minf) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf), t->minf = minf;
    if (an != t->aniso) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, an), t->aniso = an;
}

/* ------------------------------------------------------------------ state application */
static GLenum gl_cmp(uint32_t c) {
    static const GLenum m[] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return c < 9 ? m[c] : GL_ALWAYS;
}
static GLenum gl_blend(uint32_t b) {
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA; case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE; case 12: return GL_SRC_ALPHA; case 13: return GL_ONE_MINUS_SRC_ALPHA;
    default: return GL_ONE;
    }
}
static GLenum gl_stencil_op(uint32_t o) {
    switch (o) { case 2: return GL_ZERO; case 3: return GL_REPLACE; case 4: return GL_INCR; case 5: return GL_DECR;
                 case 6: return GL_INVERT; case 7: return GL_INCR_WRAP; case 8: return GL_DECR_WRAP; default: return GL_KEEP; }
}

static void apply_rs(d3ddev_t *d) {
    const uint32_t *rs = d->s.rs;
    if (rs[7]) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(gl_cmp(rs[23]));
    glDepthMask(rs[14] ? GL_TRUE : GL_FALSE);
    glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
    glShadeModel(rs[9] == 1 ? GL_FLAT : GL_SMOOTH);
    if (rs[15]) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
    glAlphaFunc(gl_cmp(rs[25]), (float)(rs[24] & 255) / 255.0f);
    if (rs[27]) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    {
        uint32_t sb = rs[19], db = rs[20];
        if (sb == 12) sb = 5, db = 6;                    /* BOTHSRCALPHA */
        if (sb == 13) sb = 6, db = 5;                    /* BOTHINVSRCALPHA */
        glBlendFunc(gl_blend(sb), gl_blend(db));
        GLenum op = GL_FUNC_ADD;
        switch (rs[171]) { case 2: op = GL_FUNC_SUBTRACT; break; case 3: op = GL_FUNC_REVERSE_SUBTRACT; break;
                           case 4: op = GL_MIN; break; case 5: op = GL_MAX; break; }
        glBlendEquation(op);
    }
    /* culling: see the file's header, the y flip turns D3D's screen winding into GL's opposite */
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(rs[22] == 3 ? GL_BACK : GL_FRONT); }
    uint32_t cw = rs[168];
    glColorMask(cw & 1 ? 1 : 0, cw & 2 ? 1 : 0, cw & 4 ? 1 : 0, cw & 8 ? 1 : 0);
    if (rs[52]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOp(gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
        glStencilMask(rs[59]);
    } else glDisable(GL_STENCIL_TEST);
    if (rs[47]) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -(float)rs[47]); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    float fc[4];
    color4(rs[34], fc);
    glFogfv(GL_FOG_COLOR, fc);
    glPointSize(fl(rs[154]) > 0 ? fl(rs[154]) : 1.0f);
    float amb[4];
    color4(rs[139], amb);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, rs[142] ? 1 : 0);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
    if (rs[143]) glEnable(GL_NORMALIZE); else glDisable(GL_NORMALIZE);
    d->d_rs = 0;
}

/* GL_COMBINE sources for a D3DTA argument */
static void ta_src(uint32_t arg, int alpha, int stage, GLenum *src, GLenum *op) {
    uint32_t a = arg & 0xF;
    switch (a) {
    case 0: *src = GL_PRIMARY_COLOR; break;                /* DIFFUSE */
    case 1: *src = stage ? GL_PREVIOUS : GL_PRIMARY_COLOR; break;   /* CURRENT */
    case 2: *src = GL_TEXTURE; break;
    case 3: *src = GL_CONSTANT; break;                     /* TFACTOR */
    case 4: *src = GL_PRIMARY_COLOR; break;                /* SPECULAR: not a combiner source in GL */
    default: *src = GL_PREVIOUS; break;
    }
    int comp = (arg & 0x10) != 0, rep = (arg & 0x20) != 0;
    if (alpha || rep) *op = comp ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    else *op = comp ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
}
static void setup_combiner(int stage, uint32_t top, uint32_t a1, uint32_t a2, uint32_t a0, int alpha) {
    GLenum mode = GL_MODULATE, s0, o0, s1, o1, s2 = GL_PRIMARY_COLOR, o2 = alpha ? GL_SRC_ALPHA : GL_SRC_ALPHA;
    float scale = 1.0f;
    ta_src(a1, alpha, stage, &s0, &o0);
    ta_src(a2, alpha, stage, &s1, &o1);
    switch (top) {
    case 2: mode = GL_REPLACE; break;
    case 3: mode = GL_REPLACE; s0 = s1, o0 = o1; break;
    case 4: mode = GL_MODULATE; break;
    case 5: mode = GL_MODULATE; scale = 2; break;
    case 6: mode = GL_MODULATE; scale = 4; break;
    case 7: case 11: mode = GL_ADD; break;
    case 8: mode = GL_ADD_SIGNED; break;
    case 9: mode = GL_ADD_SIGNED; scale = 2; break;
    case 10: mode = GL_SUBTRACT; break;
    case 12: mode = GL_INTERPOLATE; s2 = GL_PRIMARY_COLOR; o2 = GL_SRC_ALPHA; break;
    case 13: case 15: mode = GL_INTERPOLATE; s2 = GL_TEXTURE; o2 = GL_SRC_ALPHA; break;
    case 14: mode = GL_INTERPOLATE; s2 = GL_CONSTANT; o2 = GL_SRC_ALPHA; break;
    case 16: mode = GL_INTERPOLATE; s2 = stage ? GL_PREVIOUS : GL_PRIMARY_COLOR; o2 = GL_SRC_ALPHA; break;
    case 24: mode = alpha ? GL_MODULATE : GL_DOT3_RGBA; break;
    case 26: {                                            /* LERP: arg0 selects */
        GLenum sa, oa;
        ta_src(a0, alpha, stage, &sa, &oa);
        mode = GL_INTERPOLATE; s2 = sa; o2 = oa;
        break;
    }
    default: mode = GL_MODULATE; break;                    /* the rarer ops: near enough */
    }
    if (alpha) {
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, (GLint)mode);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, (GLint)s0); glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, (GLint)o0);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA, (GLint)s1); glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, (GLint)o1);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_ALPHA, (GLint)s2); glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_ALPHA, GL_SRC_ALPHA);
        glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, scale);
    } else {
        glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, (GLint)mode);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, (GLint)s0); glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, (GLint)o0);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, (GLint)s1); glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, (GLint)o1);
        glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, (GLint)s2); glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, (GLint)o2);
        glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, scale);
    }
}
static int arg_uses_tex(uint32_t a) { return (a & 0xF) == 2; }

/* Texture units for this draw; returns how many are enabled. */
static int apply_stages(d3ddev_t *d) {
    int n = 0;
    float tf[4];
    color4(d->s.rs[60], tf);
    for (int i = 0; i < NTS; i++) {
        const uint32_t *ts = d->s.tss[i];
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        if (n < 0 || ts[1] == 1 || i >= 4) {               /* COLOROP disable: this stage and the rest off */
            glDisable(GL_TEXTURE_2D);
            n = n < 0 ? n : -n - 1;
            continue;
        }
        tex_t *t = d->s.tex[i] ? (tex_t *)HOST(d->s.tex[i]) : NULL;
        if (t && !t->cube) {
            if (t->dirty || !t->allocated) tex_upload(t);
            glBindTexture(GL_TEXTURE_2D, t->gl);
            tex_sampler(t, ts);
        } else glBindTexture(GL_TEXTURE_2D, d->white);
        glEnable(GL_TEXTURE_2D);
        uint32_t aop = ts[4] == 1 ? 2 : ts[4];             /* alpha disabled under a live colour: pass current */
        uint32_t c[10] = { 1, d->s.rs[60], ts[1], ts[2], ts[3], ts[26], aop, ts[4] == 1 ? 1 : ts[5], ts[6], ts[27] };
        if (!d->comb_ok || memcmp(c, d->comb[i], sizeof c)) {   /* the combiner as it was: nothing to set */
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
            setup_combiner(i, c[2], c[3], c[4], c[5], 0);
            setup_combiner(i, c[6], c[7], c[8], c[9], 1);
            memcpy(d->comb[i], c, sizeof c);
        }
        (void)arg_uses_tex;
        n++;
    }
    glActiveTexture(GL_TEXTURE0);
    d->d_tss = 0;
    d->comb_ok = 1;
    return d->units = n < 0 ? -n - 1 : n;
}

static void apply_lights(d3ddev_t *d) {
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(d->s.view);                              /* D3D's lights are in world space */
    int gl_i = 0;
    for (int i = 0; i < NLIGHT && gl_i < 8; i++) {
        if (!d->s.light_on[i]) continue;
        const light_t *l = &d->s.light[i];
        GLenum L = GL_LIGHT0 + (GLenum)gl_i++;
        glEnable(L);
        glLightfv(L, GL_DIFFUSE, l->diffuse);
        glLightfv(L, GL_SPECULAR, l->specular);
        glLightfv(L, GL_AMBIENT, l->ambient);
        if (l->type == 3) {                                /* directional: toward the light */
            float p[4] = { -l->dir[0], -l->dir[1], -l->dir[2], 0 };
            glLightfv(L, GL_POSITION, p);
            glLightf(L, GL_SPOT_CUTOFF, 180.0f);
            glLightf(L, GL_CONSTANT_ATTENUATION, 1), glLightf(L, GL_LINEAR_ATTENUATION, 0), glLightf(L, GL_QUADRATIC_ATTENUATION, 0);
        } else {
            float p[4] = { l->pos[0], l->pos[1], l->pos[2], 1 };
            glLightfv(L, GL_POSITION, p);
            glLightf(L, GL_CONSTANT_ATTENUATION, l->att0);
            glLightf(L, GL_LINEAR_ATTENUATION, l->att1);
            glLightf(L, GL_QUADRATIC_ATTENUATION, l->att2);
            if (l->type == 2) {                            /* spot */
                glLightfv(L, GL_SPOT_DIRECTION, l->dir);
                float c = l->phi * 0.5f * 57.29578f;
                glLightf(L, GL_SPOT_CUTOFF, c > 90 ? 90 : c);
                glLightf(L, GL_SPOT_EXPONENT, l->falloff > 0 ? l->falloff : 1.0f);
            } else glLightf(L, GL_SPOT_CUTOFF, 180.0f);
        }
    }
    for (; gl_i < 8; gl_i++) glDisable(GL_LIGHT0 + (GLenum)gl_i);
    d->d_light = 0;
    d->d_xf = 1;
}
static void apply_material(d3ddev_t *d) {
    const float *m = d->s.mat;
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, m);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, m + 4);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, m + 8);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, m + 12);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, m[16] < 0 ? 0 : m[16] > 128 ? 128 : m[16]);
    d->d_mat = 0;
}

/* the projection fix-up after D3D's projection (GL convention, column-major) */
static void fixup(float *m, int vw, int vh) {
    memset(m, 0, 64);
    m[0] = 1;
    m[5] = -1;
    m[10] = 2;
    m[12] = 1.0f / (float)vw;
    m[13] = 1.0f / (float)vh;
    m[14] = -1;
    m[15] = 1;
}
/* ------------------------------------------------------------------ vertices */
/* ------------------------------------------------------------------ GL buffers */
static GLuint g_ab, g_eab;                             /* the buffers bound, as far as we know (~0u: unknown) */
static void bind_ab(GLuint b) { if (g_ab != b) glBindBuffer(GL_ARRAY_BUFFER, b), g_ab = b; }
static void bind_eab(GLuint b) { if (g_eab != b) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b), g_eab = b; }
/* the GL copy up to date and bound */
static void buf_sync(buf_t *b, int index) {
    GLenum target = index ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    GLenum use = (b->usage & 0x200u) ? GL_STREAM_DRAW : GL_STATIC_DRAW;   /* D3DUSAGE_DYNAMIC */
    if (!b->gl) {
        glGenBuffers(1, &b->gl);
        if (index) bind_eab(b->gl); else bind_ab(b->gl);
        glBufferData(target, b->size, b->mem, use);
        buf_synced(b);
        return;
    }
    if (index) bind_eab(b->gl); else bind_ab(b->gl);
    if (b->dhi > b->dlo) {
        /* all of it new, or the game discarded the old: fresh storage, so no
         * wait for the draws still reading the old */
        if (b->discard || (b->dlo == 0 && b->dhi == b->size)) glBufferData(target, b->size, NULL, use);
        glBufferSubData(target, b->dlo, b->dhi - b->dlo, b->mem + b->dlo);
    }
    buf_synced(b);
}
/* vertex fog from the specular alpha: GL takes the fog coordinate only as a
 * float, so a vertex buffer drawn with it has a second GL buffer of them
 * (fog coordinate 1 - alpha: linear fog from 0 to 1 gives alpha back) */
static GLuint buf_fog(buf_t *b, int stride, int off) {
    uint32_t n = b->size / (uint32_t)stride, lo = b->flo, hi = b->fhi;
    int whole = !b->fog || b->fog_stride != stride || b->fog_off != off;
    if (!b->fog) glGenBuffers(1, &b->fog);
    bind_ab(b->fog);
    if (whole) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n ? n : 1) * 4, NULL, (b->usage & 0x200u) ? GL_STREAM_DRAW : GL_STATIC_DRAW);
        b->fog_stride = stride, b->fog_off = off, lo = 0, hi = b->size;
    }
    if (hi > lo) {
        uint32_t v0 = lo / (uint32_t)stride, v1 = (hi + (uint32_t)stride - 1) / (uint32_t)stride;
        if (v1 > n) v1 = n;
        if (v1 > v0) {
            static float *tmp; static uint32_t tn;
            if (tn < v1 - v0) tmp = (float *)realloc(tmp, (size_t)(v1 - v0) * 4), tn = v1 - v0;
            const uint8_t *p = b->mem + (size_t)v0 * (uint32_t)stride + (uint32_t)off + 3;
            for (uint32_t i = 0; i < v1 - v0; i++, p += stride) tmp[i] = 1.0f - (float)*p * (1.0f / 255.0f);
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)v0 * 4, (GLsizeiptr)(v1 - v0) * 4, tmp);
        }
    }
    b->flo = b->fhi = 0;
    return b->fog;
}
static void buf_gl_free(buf_t *b) {
    if (b->gl) { if (g_ab == b->gl) g_ab = 0; if (g_eab == b->gl) g_eab = 0; glDeleteBuffers(1, &b->gl); }
    if (b->fog) { if (g_ab == b->fog) g_ab = 0; glDeleteBuffers(1, &b->fog); }
    b->gl = b->fog = 0;
}

/* Where a draw's vertices (and indices) are in GL buffers: vb's vertex
 * `vertex0` is the draw's vertex 0; indices from byte ioff of ib, all within
 * [rmin, rmax]. */
typedef struct { buf_t *vb, *ib; uint32_t vertex0, ioff; int rmin, rmax; } gsrc_t;

/* One draw: vertex `first..first+nv` of base (stride bytes each); indices
 * if idx (type GL_UNSIGNED_SHORT/INT, `count` of them), else `count` vertices
 * from vertex 0 of base. */
static void draw(d3ddev_t *d, GLenum mode, const uint8_t *base, int stride, int first, int nv,
                 const void *idx, GLenum itype, int count, const gsrc_t *gs) {
    fvf_t f;
    fvf_decode(d->s.vshader, &f);
    if (d->d_rs) apply_rs(d);
    if (d->d_mat) apply_material(d);
    int units = d->units;
    if (!d->d_tss)                                     /* the units as they were, unless a texture changed */
        for (int i = 0; i < units; i++) {
            tex_t *t = d->s.tex[i] ? (tex_t *)HOST(d->s.tex[i]) : NULL;
            if (t && !t->cube && (t->dirty || !t->allocated)) { d->d_tss = 1; break; }
        }
    if (d->d_tss) units = apply_stages(d);
    /* from GL buffers, unless the positions are converted here (RHW) */
    int gpu = gs && gs->vb && !f.rhw;
    if (gpu) buf_sync(gs->vb, 0);
    else bind_ab(0);
    const uint32_t *rs = d->s.rs;
    int vw = rt_w(d), vh = rt_h(d);

    /* fog: table (pixel) or vertex; vertex fog with neither mode is the
     * specular alpha, the application's own */
    int fog = rs[28] != 0, fog_from_spec = 0, fog_from_w = 0;
    uint32_t fmode = rs[35] ? rs[35] : rs[140];
    if (fog) {
        glEnable(GL_FOG);
        if (!fmode) {
            fog_from_spec = f.specular;
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, 0.0f), glFogf(GL_FOG_END, 1.0f);
            if (!fog_from_spec) glDisable(GL_FOG), fog = 0;
        } else {
            glFogi(GL_FOG_MODE, fmode == 1 ? GL_EXP : fmode == 2 ? GL_EXP2 : GL_LINEAR);
            glFogf(GL_FOG_START, fl(rs[36])), glFogf(GL_FOG_END, fl(rs[37]));
            glFogf(GL_FOG_DENSITY, fl(rs[38]));
            fog_from_w = f.rhw;
        }
        glFogi(GL_FOG_COORD_SRC, (fog_from_spec || fog_from_w) ? GL_FOG_COORD : GL_FRAGMENT_DEPTH);
    } else glDisable(GL_FOG);

    const uint8_t *v0 = gpu ? (const uint8_t *)(uintptr_t)(((size_t)gs->vertex0 + (size_t)first) * (size_t)stride)
                           : base + (size_t)first * (size_t)stride;   /* GL buffer: an offset */
    /* positions */
    if (f.rhw) {
        size_t need = (size_t)nv * 4;
        if (d->tmp_n < need) d->tmp = (float *)realloc(d->tmp, need * 4), d->tmp_n = need;
        for (int i = 0; i < nv; i++) {
            const float *p = (const float *)(v0 + (size_t)i * (size_t)stride);
            float rhw = p[3], w = rhw != 0 && isfinite(rhw) ? 1.0f / rhw : 1.0f;
            if (w < 0) w = -w;
            d->tmp[4 * i] = p[0] * w, d->tmp[4 * i + 1] = p[1] * w, d->tmp[4 * i + 2] = p[2] * w, d->tmp[4 * i + 3] = w;
        }
        glMatrixMode(GL_PROJECTION);
        float o[16] = { 0 };                               /* screen pixels -> NDC, integer pixel centres */
        o[0] = 2.0f / (float)vw, o[5] = 2.0f / (float)vh, o[10] = 2.0f, o[15] = 1;
        o[12] = -1.0f + 1.0f / (float)vw, o[13] = -1.0f + 1.0f / (float)vh, o[14] = -1;
        glLoadMatrixf(o);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glViewport(0, 0, vw, vh);
        glDepthRange(0, 1);
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)d->s.vp[0], (GLint)d->s.vp[1], (GLsizei)d->s.vp[2], (GLsizei)d->s.vp[3]);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(4, GL_FLOAT, 0, d->tmp);
        glDisable(GL_LIGHTING);
        d->d_xf = 1;
    } else {
        if (d->d_light && rs[137]) apply_lights(d);
        if (d->d_xf) {
            float fx[16];
            glViewport((GLint)d->s.vp[0], (GLint)d->s.vp[1], (GLsizei)d->s.vp[2], (GLsizei)d->s.vp[3]);
            glDepthRange(d->s.vpz[0], d->s.vpz[1]);
            glDisable(GL_SCISSOR_TEST);
            glMatrixMode(GL_PROJECTION);
            fixup(fx, (int)d->s.vp[2] ? (int)d->s.vp[2] : vw, (int)d->s.vp[3] ? (int)d->s.vp[3] : vh);
            glLoadMatrixf(fx);
            glMultMatrixf(d->s.proj);
            glMatrixMode(GL_MODELVIEW);
            glLoadMatrixf(d->s.view);
            glMultMatrixf(d->s.world);
            d->d_xf = 0;
        }
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, stride, v0);
        if (rs[137]) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
    }
    if (f.normal && !f.rhw) { glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, stride, v0 + f.o_normal); }
    else { glDisableClientState(GL_NORMAL_ARRAY); glNormal3f(0, 0, 0); }
    /* colours: lit, the vertex colour stands in for the material where D3D's sources say */
    int lit = !f.rhw && rs[137], cmat = 0;
    if (f.diffuse) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, v0 + f.o_diffuse);
        if (lit && rs[141] && (rs[145] == 1 || rs[147] == 1)) {
            glColorMaterial(GL_FRONT_AND_BACK, rs[145] == 1 && rs[147] == 1 ? GL_AMBIENT_AND_DIFFUSE
                                               : rs[145] == 1 ? GL_DIFFUSE : GL_AMBIENT);
            glEnable(GL_COLOR_MATERIAL);
            cmat = 1;
        } else glDisable(GL_COLOR_MATERIAL);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glDisable(GL_COLOR_MATERIAL);
        glColor4f(1, 1, 1, 1);
    }
    if (cmat) d->d_mat = 1;                                /* color material rewrites the material */
    int spec = rs[29] && f.specular && !lit;
    if (spec) {
        glEnableClientState(GL_SECONDARY_COLOR_ARRAY);
        glSecondaryColorPointer(GL_BGRA, GL_UNSIGNED_BYTE, stride, v0 + f.o_specular);
        glEnable(GL_COLOR_SUM);
    } else { glDisableClientState(GL_SECONDARY_COLOR_ARRAY); glDisable(GL_COLOR_SUM); }
    if (gpu && fog_from_spec) {
        GLuint fb = buf_fog(gs->vb, stride, f.o_specular);
        glEnableClientState(GL_FOG_COORD_ARRAY);
        glFogCoordPointer(GL_FLOAT, 0, (const void *)(uintptr_t)(((size_t)gs->vertex0 + (size_t)first) * 4));
        (void)fb;
        bind_ab(gs->vb->gl);
    } else if (fog_from_spec || fog_from_w) {
        if (d->fog_n < (size_t)nv) d->fog = (float *)realloc(d->fog, (size_t)nv * 4), d->fog_n = (size_t)nv;
        for (int i = 0; i < nv; i++) {
            const uint8_t *v = v0 + (size_t)i * (size_t)stride;
            d->fog[i] = fog_from_spec ? 1.0f - (float)v[f.o_specular + 3] / 255.0f : d->tmp[4 * i + 3];
        }
        glEnableClientState(GL_FOG_COORD_ARRAY);
        glFogCoordPointer(GL_FLOAT, 0, d->fog);
    } else glDisableClientState(GL_FOG_COORD_ARRAY);
    /* texture coordinates, by each stage's TEXCOORDINDEX */
    for (int i = 0; i < 4; i++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)i);
        uint32_t ci = d->s.tss[i][11] & 0xFFFF;
        if (i < units && (int)ci < f.ntex) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(f.tex_n[ci], GL_FLOAT, stride, v0 + f.o_tex[ci]);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glMultiTexCoord4f(GL_TEXTURE0 + (GLenum)i, 0, 0, 0, 1);
        }
    }
    glClientActiveTexture(GL_TEXTURE0);

    if (idx && gpu && gs->ib) {
        buf_sync(gs->ib, 1);
        glDrawRangeElements(mode, (GLuint)gs->rmin, (GLuint)gs->rmax, count, itype, (const void *)(uintptr_t)gs->ioff);
    } else if (idx) {                                  /* client indices: their range spares GL a pass over them */
        bind_eab(0);
        int lo = gs ? gs->rmin : 0, hi = gs ? gs->rmax : nv - 1;
        glDrawRangeElements(mode, (GLuint)lo, (GLuint)(hi > lo ? hi : lo), count, itype, idx);
    } else glDrawArrays(mode, 0, count);
    if (f.rhw) glDisable(GL_SCISSOR_TEST);
}

static GLenum gl_prim(uint32_t type) {
    switch (type) {
    case 2: return GL_LINES; case 3: return GL_LINE_STRIP; case 4: return GL_TRIANGLES;
    case 5: return GL_TRIANGLE_STRIP; case 6: return GL_TRIANGLE_FAN; default: return GL_POINTS;
    }
}
/* A draw (d3d8_int.h). From GL buffer copies unless positions or fog are
 * converted here (RHW, client memory); indices from client memory whose
 * values count from vertex 0 of base, while converted arrays start at vertex
 * rmin: those are rebased. */
static void gl_draw(d3ddev_t *d, const draw_t *dr) {
    fvf_t f;
    fvf_decode(dr->fvf, &f);
    GLenum mode = gl_prim(dr->prim), itype = dr->is32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    if (!dr->idx) {
        gsrc_t g = { dr->vb, NULL, dr->vertex0, 0, 0, 0 };
        draw(d, mode, dr->base, dr->stride, dr->first, dr->nv, NULL, 0, dr->count, dr->vb ? &g : NULL);
        return;
    }
    if (!f.rhw && (dr->vb || !(d->s.rs[28] && f.specular))) {   /* no conversion: arrays from vertex 0 */
        gsrc_t g = { dr->vb, dr->ib, dr->vertex0, dr->ioff, dr->rmin, dr->rmax };
        draw(d, mode, dr->base, dr->stride, 0, dr->nv, dr->idx, itype, dr->count, &g);
        return;
    }
    static uint32_t *ri; static size_t rn;
    if (rn < (size_t)dr->count) ri = (uint32_t *)realloc(ri, (size_t)dr->count * 4), rn = (size_t)dr->count;
    uint32_t minv = (uint32_t)dr->rmin;
    for (int i = 0; i < dr->count; i++) {
        uint32_t v = dr->is32 ? ((const uint32_t *)dr->idx)[i] : ((const uint16_t *)dr->idx)[i];
        ri[i] = v >= minv ? v - minv : 0;
    }
    draw(d, mode, dr->base, dr->stride, dr->rmin, dr->rmax - dr->rmin + 1, ri, GL_UNSIGNED_INT, dr->count, NULL);
}

/* ------------------------------------------------------------------ present */
static void shot(d3ddev_t *d) {
    const char *path = d3d8_shot_due(d);
    if (!path) return;
    uint8_t *px = (uint8_t *)malloc((size_t)d->bw * d->bh * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, d->bw, d->bh, GL_RGB, GL_UNSIGNED_BYTE, px);   /* row 0 is the top */
    d3d8_shot_write(path, d->bw, d->bh, px);
    free(px);
}
static void gl_present(d3ddev_t *d) {
    SDL_Window *w = hle_screen_sdl_window();
    if (!w) return;
    int ww, wh;
    SDL_GL_GetDrawableSize(w, &ww, &wh);
    int dw = ww, dh = wh;
    if ((long long)ww * d->bh > (long long)wh * d->bw) dw = (int)((long long)wh * d->bw / d->bh);
    else dh = (int)((long long)ww * d->bh / d->bw);
    int dx = (ww - dw) / 2, dy = (wh - dh) / 2;
    if (d->msaa) msaa_resolve(d);
    else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, d->fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->back_tex, 0);
    }
    /* the recomp's options (overlay.c), into the game's picture: scaled with it */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d->msaa ? d->rfbo : d->fbo);
    overlay_draw(d->bw, d->bh);
    d->d_tss = 1, d->comb_ok = 0;                      /* the overlay drew with its own state */
    g_ab = g_eab = ~0u;
    shot(d);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* the FBO's row 0 is the picture's top: flip into the window */
    glBlitFramebuffer(0, 0, d->bw, d->bh, dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT,
                      dw == d->bw && dh == d->bh ? GL_NEAREST : GL_LINEAR);
    SDL_GL_SwapWindow(w);
    hle_screen_gl_picture(d->bw, d->bh, dx, wh - dy - dh, dw, dh);
    bind_targets(d);
}
/* on: 1, off: 0, adaptive (late frames not held back): -1, else 1 */
static void gl_vsync(int on) {
    if (on < 0 && SDL_GL_SetSwapInterval(-1) == 0) return;
    SDL_GL_SetSwapInterval(on ? 1 : 0);
}

/* ------------------------------------------------------------------ the renderer */
static int gl_create(d3ddev_t *d) {
    SDL_Window *w = hle_screen_sdl_window();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    d->ctx = SDL_GL_CreateContext(w);
    if (!d->ctx) { fprintf(stderr, "[d3d8] GL context: %s\n", SDL_GetError()); return 0; }
    SDL_GL_MakeCurrent(w, (SDL_GLContext)d->ctx);
    const char *ren = (const char *)glGetString(GL_RENDERER);
    fprintf(stderr, "[d3d8] OpenGL %s, %s\n", (const char *)glGetString(GL_VERSION), ren);
    SDL_DisplayMode dm;
    int disp = SDL_GetWindowDisplayIndex(w);
    perf_note("OpenGL: %s | %s | %s | SDL video %s", (const char *)glGetString(GL_VENDOR), ren,
              (const char *)glGetString(GL_VERSION), SDL_GetCurrentVideoDriver());
    if (SDL_GetDesktopDisplayMode(disp < 0 ? 0 : disp, &dm) == 0)
        perf_note("screen %d: %dx%d at %d Hz (%d screens)", disp, dm.w, dm.h, dm.refresh_rate, SDL_GetNumVideoDisplays());
    if (ren && (strstr(ren, "llvmpipe") || strstr(ren, "softpipe") || strstr(ren, "Software")))
        perf_note("WARNING: software rendering (%s): the graphics card is not used", ren);
    perf_note("vsync: %d", SDL_GL_GetSwapInterval());
    glGenTextures(1, &d->white);
    glBindTexture(GL_TEXTURE_2D, d->white);
    uint32_t px = 0xFFFFFFFFu;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, &px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    make_backbuffer(d);
    bind_targets(d);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    return 1;
}
static void gl_resize(d3ddev_t *d) { make_backbuffer(d); bind_targets(d); }
static void gl_clear(d3ddev_t *d, uint32_t n, const int32_t *r, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    float c[4];
    color4(color, c);
    GLbitfield mask = 0;
    if (flags & 1) mask |= GL_COLOR_BUFFER_BIT;
    if ((flags & 2) && d->zs) mask |= GL_DEPTH_BUFFER_BIT;
    if ((flags & 4) && d->zs) mask |= GL_STENCIL_BUFFER_BIT;
    glClearColor(c[0], c[1], c[2], c[3]);
    glClearDepth(z);
    glClearStencil((GLint)stencil);
    glColorMask(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFFFFFFFu);
    glEnable(GL_SCISSOR_TEST);
    if (!n) {
        glScissor((GLint)d->s.vp[0], (GLint)d->s.vp[1], (GLsizei)d->s.vp[2], (GLsizei)d->s.vp[3]);
        glClear(mask);
    } else for (uint32_t i = 0; i < n; i++) {
        glScissor(r[4 * i], r[4 * i + 1], r[4 * i + 2] - r[4 * i], r[4 * i + 3] - r[4 * i + 1]);
        glClear(mask);
    }
    glDisable(GL_SCISSOR_TEST);
}
static int gl_tex_init(tex_t *t) {
    glGenTextures(1, &t->gl);
    if (t->usage & 1) {                                /* a render target: storage now, drawn into by the FBO */
        glBindTexture(GL_TEXTURE_2D, t->gl);
        stages_dirty();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->w, t->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        t->allocated = 1;
        t->lvl[0]->gltex = t->gl;
        t->wrap_s = t->wrap_t = t->minf = t->magf = -1, t->aniso = -1;
    }
    return 1;
}
static void gl_tex_free(tex_t *t) { glDeleteTextures(1, &t->gl); }
static int gl_rt_init(surf_t *s) {
    glGenTextures(1, &s->gltex);
    glBindTexture(GL_TEXTURE_2D, s->gltex);
    stages_dirty();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    return 1;
}
static int gl_depth_init(surf_t *s) {
    glGenRenderbuffers(1, &s->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, s->rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s->w, s->h);
    return 1;
}
static void gl_surf_free(surf_t *s) {
    if (s->gltex) glDeleteTextures(1, &s->gltex), s->gltex = 0, stages_dirty();
    if (s->rb) glDeleteRenderbuffers(1, &s->rb), s->rb = 0;
}
static void gl_read_back(d3ddev_t *d, surf_t *s) {
    if (d->msaa) msaa_resolve(d);
    else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, d->fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->rt ? d->rt->gltex : d->back_tex, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s->w, s->h, GL_BGRA, GL_UNSIGNED_BYTE, s->mem);
    bind_targets(d);
}
const renderer_t d3d8_gl_renderer = {
    "OpenGL", gl_create, gl_resize, gl_present, gl_clear, bind_targets, gl_draw,
    gl_tex_init, gl_tex_free, gl_rt_init, gl_depth_init, gl_surf_free, buf_gl_free, gl_read_back, gl_vsync,
};
