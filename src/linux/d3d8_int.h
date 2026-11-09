/*
 * d3d8_int.h - Direct3D 8 for the lifted Operation Flashpoint: what the
 * front end (d3d8.c: the COM objects, the state, locks, state blocks) and
 * the renderers (d3d8_gl.c: OpenGL, d3d8_vk.c: Vulkan) share.
 *
 * The front end keeps every resource's bytes in host memory and every state
 * the game sets; a renderer draws from them. Which renderer: OFP_RENDERER
 * (opengl, the default, or vulkan), chosen before the window is made.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "ofp.h"

#define D3D_OK                 0u
#define S_FALSE                1u
#define E_NOINTERFACE          0x80004002u
#define E_OUTOFMEMORY          0x8007000Eu
#define D3DERR_NOTAVAILABLE    0x8876086Au
#define D3DERR_INVALIDCALL     0x8876086Cu
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu

int d3d8_trace(void);                                  /* OFP_D3DTRACE=1 prints every call */
#define T(...) do { if (d3d8_trace()) fprintf(stderr, "[d3d8] " __VA_ARGS__); } while (0)

static inline float fl(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t uf(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
#define HOST(o) hle_ptr(MEM32((o) + 8))
/* the host structs behind COM objects and state blocks live in guest memory,
 * so their address fits the object's 32-bit slot on a 64-bit host too */
#define GNEW(T) ((T *)hle_alloc(sizeof(T)))

/* ------------------------------------------------------------------ formats */
enum {
    FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_A8 = 28, FMT_X4R4G4B4 = 30, FMT_P8 = 41,
    FMT_L8 = 50, FMT_A8L8 = 51, FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75,
    FMT_D24X8 = 77, FMT_D24X4S4 = 79, FMT_D16 = 80, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')
/* bytes a pixel, or (compressed) a 4x4 block; 0 if the format is not offered */
typedef struct { int bpp, block; } fmt_t;
int fmt_info(uint32_t f, fmt_t *g);
static inline int is_depth(uint32_t f) { return f >= 70 && f <= 80; }
uint32_t level_pitch(uint32_t f, int w);
uint32_t level_size(uint32_t f, int w, int h);

/* ------------------------------------------------------------------ resources */
/* the game's copy of a locked range (a 64-bit host: guest memory is apart) */
typedef struct { uint8_t *g; uint32_t off, size; int keep, readonly; } stage_t;

enum { SURF_TEXLEVEL, SURF_BACK, SURF_DEPTH, SURF_IMAGE, SURF_RT };
typedef struct tex_s tex_t;
typedef struct {
    uint32_t self;
    int kind, w, h, level;
    uint32_t fmt, usage, pool;
    tex_t *parent;
    uint8_t *mem;                      /* the pixels */
    stage_t st;                        /* the game's copy while locked */
    uint32_t pitch, size;
    uint32_t rb;                       /* OpenGL: a depth surface's renderbuffer */
    uint32_t gltex;                    /* OpenGL: a render target's texture */
    void *vk;                          /* Vulkan: its image */
} surf_t;
#define MAXLVL 14
struct tex_s {
    uint32_t self;
    int w, h, levels, cube;
    uint32_t fmt, usage, pool, lod;
    surf_t *lvl[MAXLVL];
    uint32_t dirty;                    /* a bit a level: changed since the renderer's copy */
    int failed;                        /* a level could not be made (out of memory) */
    /* OpenGL */
    uint32_t gl;
    int allocated;                     /* storage made */
    int wrap_s, wrap_t, minf, magf;    /* sampler state last applied */
    float aniso;
    void *vk;                          /* Vulkan: its image */
};
/* a vertex or an index buffer. The game's bytes are in mem; a renderer draws
 * from its own copy, brought up to date before a draw from the bytes changed
 * since (the locks' ranges: dlo..dhi). */
typedef struct {
    uint32_t self, size, usage;
    union { uint32_t fvf, fmt; };      /* a vertex buffer's FVF, an index buffer's format */
    uint32_t pool;
    uint8_t *mem;
    stage_t st;
    uint32_t lk_off, lk_size, lk_flags; int locked;
    uint32_t dlo, dhi;                 /* bytes changed since the renderer's copy */
    uint32_t flo, fhi;                 /* the same, for OpenGL's fog coordinates */
    int discard;                       /* locked with DISCARD since: the copy may be replaced */
    int sync;                          /* locked with neither DISCARD nor NOOVERWRITE since */
    /* OpenGL */
    uint32_t gl, fog;                  /* the copy, and a float fog coordinate a vertex (vertex fog) */
    int fog_stride, fog_off;
    void *vk;                          /* Vulkan */
} buf_t;
typedef buf_t vb_t;
typedef buf_t ib_t;
void buf_dirty(buf_t *b, uint32_t lo, uint32_t hi);
/* the renderer's copy has caught up with the locks */
static inline void buf_synced(buf_t *b) { b->dlo = b->dhi = 0, b->discard = 0, b->sync = 0; }

/* ------------------------------------------------------------------ device state */
#define NRS 256
#define NTS 8
#define NTSS 32
#define NLIGHT 32
typedef struct {
    uint32_t type;
    float diffuse[4], specular[4], ambient[4], pos[3], dir[3], range, falloff, att0, att1, att2, theta, phi;
} light_t;
typedef struct {
    uint32_t rs[NRS];
    uint32_t tss[NTS][NTSS];
    float world[16], view[16], proj[16], texm[NTS][16];
    uint32_t vp[4]; float vpz[2];
    float mat[17];
    light_t light[NLIGHT];
    uint8_t light_on[NLIGHT];
    uint32_t tex[NTS];                 /* texture objects (guest VAs) */
    uint32_t stream_vb, stream_stride, ib, base_vertex, vshader, pshader;
} state_t;
/* which parts of a state a state block holds */
typedef struct {
    uint8_t rs[NRS], tss[NTS][NTSS], tex[NTS], light[NLIGHT], light_on[NLIGHT];
    uint8_t world, view, proj, texm[NTS], vp, mat, stream, ib, vshader, pshader;
} mask_t;
typedef struct { state_t s; mask_t m; } block_t;

typedef struct {
    uint32_t self;
    int bw, bh;                        /* the back buffer */
    uint32_t bfmt, dfmt;
    int autodepth, windowed, interval;
    int msaa;                          /* samples of the back buffer, 0: none */
    surf_t *back, *depth;              /* the back buffer and the auto depth surface */
    surf_t *rt, *zs;                   /* current targets (rt NULL: the back buffer) */
    state_t s;
    block_t *rec;                      /* recording a state block */
    uint32_t hwnd, behavior;
    /* changed since the renderer last applied them */
    int d_xf, d_light, d_mat, d_rs, d_tss;
    uint32_t frames, draws;
    /* OpenGL */
    void *ctx;
    uint32_t fbo, back_tex, back_depth, ms_color, ms_depth, rfbo, white;
    int units;                         /* texture units on, as last set */
    uint32_t comb[4][10];              /* each unit's combiner as last set (comb_ok: valid) */
    int comb_ok;
    float *tmp; size_t tmp_n;          /* converted vertices */
    float *fog; size_t fog_n;
    void *vk;                          /* Vulkan */
} d3ddev_t;
extern d3ddev_t *g_dev;
static inline int rt_w(d3ddev_t *d) { return d->rt ? d->rt->w : d->bw; }
static inline int rt_h(d3ddev_t *d) { return d->rt ? d->rt->h : d->bh; }
static inline void color4(uint32_t c, float *f) {     /* D3DCOLOR (ARGB) to r, g, b, a */
    f[0] = (float)(c >> 16 & 255) / 255.0f, f[1] = (float)(c >> 8 & 255) / 255.0f;
    f[2] = (float)(c & 255) / 255.0f, f[3] = (float)(c >> 24) / 255.0f;
}

/* ------------------------------------------------------------------ vertices */
typedef struct {
    int stride, pos_n, rhw, blend, normal, psize, diffuse, specular, ntex;
    int o_normal, o_psize, o_diffuse, o_specular, o_tex[8], tex_n[8];
} fvf_t;
void fvf_decode(uint32_t fvf, fvf_t *f);
/* D3DPRIMITIVETYPE: 1 points, 2 lines, 3 line strip, 4 triangles, 5 strip, 6 fan */
int prim_count_verts(uint32_t type, uint32_t n);

/* One draw, wherever its vertices are. base is always the vertices in host
 * memory (vertex 0); with vb, they are also vb's from vertex vertex0 on. */
typedef struct {
    uint32_t prim;                     /* D3DPRIMITIVETYPE */
    uint32_t fvf;
    int stride;
    const uint8_t *base;
    buf_t *vb; uint32_t vertex0;
    int first, nv;                     /* non-indexed: count vertices from first; indexed: first 0, nv covers rmax */
    const void *idx;                   /* indices in host memory, or NULL: not indexed */
    int is32;
    buf_t *ib; uint32_t ioff;          /* the indices' buffer (idx is ib->mem + ioff), or NULL */
    int count;                         /* vertices or indices */
    int rmin, rmax;                    /* indexed: the vertices they use */
} draw_t;

/* ------------------------------------------------------------------ renderers */
typedef struct {
    const char *name;
    int  (*create)(d3ddev_t *d);       /* the context and the back buffer (d->bw x d->bh, d->msaa); 0: failed */
    void (*resize)(d3ddev_t *d);       /* the back buffer's size or antialiasing changed */
    void (*present)(d3ddev_t *d);      /* the frame to the window (and the overlay over it) */
    void (*clear)(d3ddev_t *d, uint32_t n, const int32_t *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
    void (*targets)(d3ddev_t *d);      /* d->rt or d->zs changed */
    void (*draw)(d3ddev_t *d, const draw_t *dr);
    int  (*tex_init)(tex_t *t);        /* a new texture (levels made); 0: out of memory */
    void (*tex_free)(tex_t *t);
    int  (*rt_init)(surf_t *s);        /* a render target surface */
    int  (*depth_init)(surf_t *s);     /* a depth surface */
    void (*surf_free)(surf_t *s);
    void (*buf_free)(buf_t *b);
    void (*read_back)(d3ddev_t *d, surf_t *s);   /* the back buffer into s->mem (its format, rows top first) */
    void (*vsync)(int on);             /* on 1, off 0, adaptive -1 */
} renderer_t;
extern const renderer_t d3d8_gl_renderer, d3d8_vk_renderer;
extern const renderer_t *g_r;

int  msaa_wanted(void);                /* the samples asked for (OFP_MSAA, the options) */
const char *d3d8_shot_due(d3ddev_t *d); /* OFP_SHOT: this frame's file, or NULL */
void d3d8_shot_write(const char *path, int w, int h, const uint8_t *rgb);   /* rows top first */
void stages_dirty(void);               /* a texture bound or gone outside a draw */
