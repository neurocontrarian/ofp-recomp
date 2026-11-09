/*
 * d3d8.c - Direct3D 8 for the lifted Operation Flashpoint on runtime/win32hle:
 * the front end. The COM objects, the device state, resources in host memory,
 * locks and state blocks; a renderer (d3d8_int.h: OpenGL or Vulkan) draws.
 *
 * The game loads d3d8.dll itself and asks GetProcAddress for Direct3DCreate8:
 * win32hle answers by name from the shim registry, so registering the shim
 * is all it takes. Every interface is a COM object built by hle_com_vtable
 * (each method a stdcall shim, arg 0 `this`); the host state of an object is
 * a struct in guest memory whose address sits at +8.
 *
 * OFP_D3DTRACE=1 prints every call.
 */
#include <math.h>
#include <SDL2/SDL.h>
#include <time.h>
#include <errno.h>
#include "d3d8_int.h"

SDL_Window *hle_screen_sdl_window(void);                                   /* screen.c */

static int g_trace = -1;
int d3d8_trace(void) { if (g_trace < 0) g_trace = getenv("OFP_D3DTRACE") != NULL; return g_trace; }

d3ddev_t *g_dev;
const renderer_t *g_r = &d3d8_gl_renderer;
/* the renderer: OFP_RENDERER=vulkan (main.c asks before the window is made) */
void d3d8_use_vulkan(int on) { g_r = on ? &d3d8_vk_renderer : &d3d8_gl_renderer; }
void stages_dirty(void) { if (g_dev) g_dev->d_tss = 1, g_dev->comb_ok = 0; }

/* ------------------------------------------------------------------ formats */
int fmt_info(uint32_t f, fmt_t *g) {
    fmt_t t = { 0, 0 };
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: t.bpp = 4; break;
    case FMT_R8G8B8: t.bpp = 3; break;
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_X4R4G4B4: case FMT_A8L8: t.bpp = 2; break;
    case FMT_A8: case FMT_L8: t.bpp = 1; break;
    case FMT_DXT1: t.block = 8; break;
    case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5: t.block = 16; break;
    case FMT_D16: case FMT_D16_LOCKABLE: case FMT_D15S1: t.bpp = 2; break;
    case FMT_D24S8: case FMT_D24X8: case FMT_D24X4S4: case FMT_D32: t.bpp = 4; break;
    default: return 0;
    }
    if (g) *g = t;
    return 1;
}
uint32_t level_pitch(uint32_t f, int w) {
    fmt_t g;
    if (!fmt_info(f, &g)) return (uint32_t)w * 4;
    if (g.block) return (uint32_t)((w + 3) / 4) * (uint32_t)g.block;
    return (uint32_t)w * (uint32_t)g.bpp;
}
uint32_t level_size(uint32_t f, int w, int h) {
    fmt_t g = { 4, 0 };
    fmt_info(f, &g);
    return level_pitch(f, w) * (uint32_t)(g.block ? (h + 3) / 4 : h);
}

/* ------------------------------------------------------------------ resource memory
 * On a 64-bit host the guest has 2 GB of its own (as on Windows); copies of
 * every texture and buffer there leave the game too little at long view
 * distances. So they live in host memory, and a lock hands the game a guest
 * copy of the locked range that Unlock writes back. On a 32-bit host the
 * two are one memory and a lock points straight in. */
static void *res_alloc(uint32_t n) {
#ifdef HLE_FLAT
    return hle_alloc(n ? n : 4);
#else
    return calloc(1, n ? n : 4);
#endif
}
static void res_free(void *p) {
#ifdef HLE_FLAT
    hle_free(p);
#else
    free(p);
#endif
}
/* the game's copy back into mem (not after a read-only lock); a kept copy stays */
static void stage_unlock(stage_t *st, uint8_t *mem) {
#ifndef HLE_FLAT
    if (!st->g || !st->size) return;
    if (!st->readonly) memcpy(mem + st->off, st->g + (st->keep ? st->off : 0), st->size);
    st->size = 0;
    if (!st->keep) hle_free(st->g), st->g = NULL;
#else
    (void)st, (void)mem;
#endif
}
static void stage_free(stage_t *st, uint8_t *mem) {
    stage_unlock(st, mem);
#ifndef HLE_FLAT
    if (st->g) hle_free(st->g), st->g = NULL;
#endif
}
/* The guest address of mem[off, off + size) (mem is total bytes). A copy made
 * for the lock and dropped by Unlock; or, keep (a buffer locked every frame:
 * D3DUSAGE_DYNAMIC), one copy of all of mem kept for good, which always
 * matches mem after an Unlock, so nothing is copied in again. */
static uint32_t stage_lock(stage_t *st, uint8_t *mem, uint32_t total, uint32_t off, uint32_t size, uint32_t flags, int keep) {
#ifdef HLE_FLAT
    (void)st, (void)total, (void)size, (void)flags, (void)keep;
    return hle_va(mem) + off;
#else
    stage_unlock(st, mem);                             /* locked again: the last lock first */
    st->readonly = (flags & 0x10u) != 0;               /* D3DLOCK_READONLY: nothing comes back */
    st->off = off, st->size = size, st->keep = keep;
    if (keep) {
        if (!st->g) {
            if (!(st->g = (uint8_t *)hle_alloc(total ? total : 4))) return 0;
            memcpy(st->g, mem, total);
        }
        return hle_va(st->g) + off;
    }
    if (!(st->g = (uint8_t *)hle_alloc(size ? size : 4))) return 0;
    if (!(flags & 0x2000u)) memcpy(st->g, mem + off, size);   /* D3DLOCK_DISCARD: nothing to keep */
    return hle_va(st->g);
#endif
}

/* the renderer's copy is behind for bytes lo..hi */
void buf_dirty(buf_t *b, uint32_t lo, uint32_t hi) {
    if (hi > b->size) hi = b->size;
    if (lo >= hi) return;
    if (b->dhi <= b->dlo) b->dlo = lo, b->dhi = hi;
    else { if (lo < b->dlo) b->dlo = lo; if (hi > b->dhi) b->dhi = hi; }
    if (b->fhi <= b->flo) b->flo = lo, b->fhi = hi;
    else { if (lo < b->flo) b->flo = lo; if (hi > b->fhi) b->fhi = hi; }
}

static uint32_t g_vt_d3d, g_vt_dev, g_vt_tex, g_vt_surf, g_vt_vb, g_vt_ib;
static uint32_t g_d3d;

/* ------------------------------------------------------------------ device state */
static void identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

static void state_defaults(state_t *s) {
    memset(s, 0, sizeof *s);
    uint32_t *rs = s->rs;
    rs[7] = 1;      /* ZENABLE */
    rs[8] = 3;      /* FILLMODE solid */
    rs[9] = 2;      /* SHADEMODE gouraud */
    rs[14] = 1;     /* ZWRITEENABLE */
    rs[16] = 1;     /* LASTPIXEL */
    rs[19] = 2;     /* SRCBLEND one */
    rs[20] = 1;     /* DESTBLEND zero */
    rs[22] = 3;     /* CULLMODE ccw */
    rs[23] = 4;     /* ZFUNC lessequal */
    rs[25] = 8;     /* ALPHAFUNC always */
    rs[37] = uf(1.0f);   /* FOGEND */
    rs[38] = uf(1.0f);   /* FOGDENSITY */
    rs[56] = 8;     /* STENCILFUNC always */
    rs[53] = rs[54] = rs[55] = 1;   /* stencil ops keep */
    rs[58] = rs[59] = 0xFFFFFFFFu;  /* stencil masks */
    rs[60] = 0xFFFFFFFFu;           /* TEXTUREFACTOR */
    rs[136] = 1;    /* CLIPPING */
    rs[137] = 1;    /* LIGHTING */
    rs[141] = 1;    /* COLORVERTEX */
    rs[142] = 1;    /* LOCALVIEWER */
    rs[145] = 1;    /* DIFFUSEMATERIALSOURCE color1 */
    rs[146] = 2;    /* SPECULARMATERIALSOURCE color2 */
    rs[154] = uf(1.0f);   /* POINTSIZE */
    rs[155] = uf(1.0f);   /* POINTSIZE_MIN */
    rs[158] = uf(1.0f);   /* POINTSCALE_A */
    rs[162] = 0xFFFFFFFFu;          /* MULTISAMPLEMASK */
    rs[166] = uf(64.0f);  /* POINTSIZE_MAX */
    rs[168] = 0xF;  /* COLORWRITEENABLE */
    rs[171] = 1;    /* BLENDOP add */
    for (int i = 0; i < NTS; i++) {
        uint32_t *t = s->tss[i];
        t[1] = i ? 1 : 4;          /* COLOROP: stage 0 modulate, the rest disable */
        t[2] = 2; t[3] = 1;        /* COLORARG1 texture, COLORARG2 current */
        t[4] = i ? 1 : 2;          /* ALPHAOP: stage 0 selectarg1 */
        t[5] = 2; t[6] = 1;
        t[11] = (uint32_t)i;       /* TEXCOORDINDEX */
        t[13] = t[14] = t[25] = 1; /* ADDRESS wrap */
        t[16] = t[17] = 1;         /* MAG/MINFILTER point */
        t[20] = 0; t[21] = 1;      /* MAXMIPLEVEL, MAXANISOTROPY */
        identity(s->texm[i]);
    }
    identity(s->world); identity(s->view); identity(s->proj);
    s->mat[0] = s->mat[1] = s->mat[2] = s->mat[3] = 1.0f;   /* diffuse white */
    s->vpz[1] = 1.0f;
}
/* ------------------------------------------------------------------ objects: constructors */
static uint32_t new_obj(uint32_t vt, void *host) {
    uint32_t o = hle_com_new(vt, 12);
    MEM32(o + 8) = hle_va(host);
    return o;
}
static surf_t *new_surface(int kind, int w, int h, uint32_t fmt, uint32_t usage, uint32_t pool, int alloc_mem) {
    surf_t *s = GNEW(surf_t);
    if (!s) return NULL;                               /* out of guest memory: the caller says so */
    s->kind = kind, s->w = w, s->h = h, s->fmt = fmt, s->usage = usage, s->pool = pool;
    s->pitch = level_pitch(fmt, w);
    s->size = level_size(fmt, w, h);
    if (alloc_mem && !is_depth(fmt)) s->mem = (uint8_t *)res_alloc(s->size);
    s->self = new_obj(g_vt_surf, s);
    return s;
}

/* ------------------------------------------------------------------ vertices */
void fvf_decode(uint32_t fvf, fvf_t *f) {
    memset(f, 0, sizeof *f);
    int o = 0;
    switch (fvf & 0xE) {
    case 0x2: o = 12; f->pos_n = 3; break;
    case 0x4: o = 16; f->pos_n = 4; f->rhw = 1; break;
    case 0x6: o = 16; f->pos_n = 3; f->blend = 1; break;
    case 0x8: o = 20; f->pos_n = 3; f->blend = 2; break;
    case 0xA: o = 24; f->pos_n = 3; f->blend = 3; break;
    case 0xC: o = 28; f->pos_n = 3; f->blend = 4; break;
    case 0xE: o = 32; f->pos_n = 3; f->blend = 5; break;
    }
    if (fvf & 0x10) f->normal = 1, f->o_normal = o, o += 12;
    if (fvf & 0x20) f->psize = 1, f->o_psize = o, o += 4;
    if (fvf & 0x40) f->diffuse = 1, f->o_diffuse = o, o += 4;
    if (fvf & 0x80) f->specular = 1, f->o_specular = o, o += 4;
    f->ntex = (int)(fvf >> 8 & 0xF);
    for (int i = 0; i < f->ntex && i < 8; i++) {
        static const int sz[4] = { 2, 3, 4, 1 };
        f->tex_n[i] = sz[fvf >> (16 + 2 * i) & 3];
        f->o_tex[i] = o;
        o += 4 * f->tex_n[i];
    }
    f->stride = o;
}
int prim_count_verts(uint32_t type, uint32_t n) {
    switch (type) {
    case 1: return (int)n;
    case 2: return (int)n * 2;
    case 3: return (int)n + 1;
    case 4: return (int)n * 3;
    case 5: case 6: return (int)n + 2;
    }
    return 0;
}

/* ------------------------------------------------------------------ IDirect3D8 */
static uint32_t ok_n(int n) { (void)n; return D3D_OK; }
static void ok0(void) { RET(D3D_OK, 1); }
static void ok1(void) { RET(D3D_OK, 2); }
static void ok2(void) { RET(D3D_OK, 3); }
static void ok3(void) { RET(D3D_OK, 4); }
static void ok4(void) { RET(D3D_OK, 5); }
static void ok5(void) { RET(D3D_OK, 6); }

static int g_modes[64][2], g_nmodes;
static void modes_init(void) {
    if (g_nmodes) return;
    static const int common[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 },
                                     { 1280, 960 }, { 1280, 1024 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 },
                                     { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1080 },
                                     { 2560, 1440 }, { 2560, 1600 }, { 3440, 1440 }, { 3840, 2160 } };
    int dw = 0, dh = 0;
    SDL_DisplayMode m;
    if (SDL_GetDesktopDisplayMode(0, &m) == 0) dw = m.w, dh = m.h;
    for (size_t i = 0; i < sizeof common / sizeof common[0]; i++)
        if (!dw || (common[i][0] <= dw && common[i][1] <= dh)) g_modes[g_nmodes][0] = common[i][0], g_modes[g_nmodes++][1] = common[i][1];
    int have = 0;
    for (int i = 0; i < g_nmodes; i++) have |= g_modes[i][0] == dw && g_modes[i][1] == dh;
    if (dw && !have && g_nmodes < 64) g_modes[g_nmodes][0] = dw, g_modes[g_nmodes++][1] = dh;
    /* smallest first */
    for (int i = 1; i < g_nmodes; i++)
        for (int j = i; j > 0 && (long)g_modes[j][0] * g_modes[j][1] < (long)g_modes[j - 1][0] * g_modes[j - 1][1]; j--) {
            int t0 = g_modes[j][0], t1 = g_modes[j][1];
            g_modes[j][0] = g_modes[j - 1][0], g_modes[j][1] = g_modes[j - 1][1];
            g_modes[j - 1][0] = t0, g_modes[j - 1][1] = t1;
        }
}
/* modes are offered in two formats: 32-bit, then 16-bit */
static void put_mode(uint32_t p, int i) {
    modes_init();
    int k = i % g_nmodes;
    MEM32(p) = (uint32_t)g_modes[k][0], MEM32(p + 4) = (uint32_t)g_modes[k][1];
    MEM32(p + 8) = 60, MEM32(p + 12) = i < g_nmodes ? FMT_X8R8G8B8 : FMT_R5G6B5;
}

static void d3_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = 0; RET(E_NOINTERFACE, 3); }
static void d3_Release(void) { uint32_t n = MEM32(A32(0) + 4); if (n) MEM32(A32(0) + 4) = --n; RET(n, 1); }
static void d3_GetAdapterCount(void) { RET(1, 1); }
static void d3_GetAdapterIdentifier(void) {          /* (this, adapter, flags, id) */
    uint32_t p = A32(3);
    memset(hle_ptr(p), 0, 1068);
    strcpy((char *)hle_ptr(p), "opengl");
    snprintf((char *)hle_ptr(p + 512), 512, "%s (pcrecomp)", g_r->name);
    MEM32(p + 1024) = 0x0001, MEM32(p + 1028) = 0x0006000E;   /* driver version 6.14.1 */
    MEM32(p + 1032) = 0x10DE, MEM32(p + 1036) = 0x0200;        /* a GeForce 3: what the game knows */
    MEM32(p + 1064) = 1;
    RET(D3D_OK, 4);
}
static void d3_GetAdapterModeCount(void) { modes_init(); RET((uint32_t)g_nmodes * 2, 2); }
static void d3_EnumAdapterModes(void) {              /* (this, adapter, mode, D3DDISPLAYMODE*) */
    modes_init();
    if (A32(2) >= (uint32_t)g_nmodes * 2) RET(D3DERR_INVALIDCALL, 4);
    put_mode(A32(3), (int)A32(2));
    RET(D3D_OK, 4);
}
static void d3_GetAdapterDisplayMode(void) {
    uint32_t p = A32(2);
    SDL_DisplayMode m;
    if (SDL_GetDesktopDisplayMode(0, &m) != 0) m.w = 1920, m.h = 1080, m.refresh_rate = 60;
    MEM32(p) = (uint32_t)m.w, MEM32(p + 4) = (uint32_t)m.h, MEM32(p + 8) = (uint32_t)(m.refresh_rate ? m.refresh_rate : 60);
    MEM32(p + 12) = FMT_X8R8G8B8;
    RET(D3D_OK, 2);
}
static void d3_CheckDeviceType(void) {               /* (this, adapter, type, display, back, windowed) */
    uint32_t df = A32(3);
    RET(df == FMT_X8R8G8B8 || df == FMT_R5G6B5 || df == FMT_X1R5G5B5 || df == FMT_A8R8G8B8 ? D3D_OK : D3DERR_NOTAVAILABLE, 6);
}
static void d3_CheckDeviceFormat(void) {             /* (this, adapter, type, adapterfmt, usage, rtype, fmt) */
    uint32_t usage = A32(4), rtype = A32(5), f = A32(6);
    int ok = fmt_info(f, NULL);
    if (usage & 2) ok = is_depth(f);                   /* D3DUSAGE_DEPTHSTENCIL */
    else if (is_depth(f)) ok = 0;
    if ((usage & 1) && !(f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8 || f == FMT_R5G6B5)) ok = 0;   /* RENDERTARGET */
    if (rtype == 4 && f >= FMT_DXT1) ok = ok;          /* volumes: as textures */
    T("CheckDeviceFormat usage=%X type=%u fmt=%u -> %s\n", usage, rtype, f, ok ? "ok" : "no");
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}
static void d3_CheckDeviceMultiSampleType(void) { RET(A32(5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE, 6); }
static void d3_CheckDepthStencilMatch(void) { RET(D3D_OK, 6); }
static void put_caps(uint32_t p) {
    uint32_t c[53] = { 0 };
    c[0] = 1;                          /* HAL */
    c[2] = 0x20000;
    c[3] = 0x20000 | 0x80000 | 0x20000000;
    c[4] = 0x20;
    c[5] = 0x80000000u | 1;
    c[7] = 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x10000 | 0x80000;
    c[8] = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x800;
    c[9] = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 | 0x20000 | 0x100000 | 0x200000 | 0x400000;
    c[10] = 0xFF, c[11] = 0x1FFF, c[12] = 0x1FFF, c[13] = 0xFF;
    c[14] = 0x84208;
    c[15] = 0x1 | 0x4 | 0x40 | 0x400 | 0x4000;
    c[16] = 0x07030700, c[17] = 0x07030700, c[18] = 0x03030300;
    c[19] = 0x3F, c[20] = 0x3F, c[21] = 0x1F;
    c[22] = 4096, c[23] = 4096, c[24] = 256, c[25] = 8192, c[26] = 8192, c[27] = 16;
    c[28] = uf(1e10f);
    c[29] = uf(-32768.0f), c[30] = uf(-32768.0f), c[31] = uf(32768.0f), c[32] = uf(32768.0f);
    c[34] = 0xFF;
    c[35] = 0x00080008;
    c[36] = 0x03FFFFFF;
    c[37] = 4, c[38] = 4;
    c[39] = 0x3B;
    c[40] = 8, c[41] = 0;
    c[44] = uf(64.0f);
    c[45] = 0xFFFFF, c[46] = 0xFFFFFF, c[47] = 1, c[48] = 255;
    c[49] = 0, c[50] = 0, c[51] = 0;
    memcpy(hle_ptr(p), c, sizeof c);
}
static void d3_GetDeviceCaps(void) { put_caps(A32(3)); RET(D3D_OK, 4); }
static void d3_GetAdapterMonitor(void) { RET(1, 2); }

/* the device's own surfaces at the back buffer's size (no readback of the old one) */
static void back_resize(d3ddev_t *d) {
    surf_t *b = d->back;
    if (b) {
        b->w = d->bw, b->h = d->bh;
        b->pitch = level_pitch(b->fmt, b->w), b->size = level_size(b->fmt, b->w, b->h);
        stage_free(&b->st, b->mem);
        res_free(b->mem), b->mem = NULL;
    }
    if (d->depth) d->depth->w = d->bw, d->depth->h = d->bh;
}
static void read_pp(d3ddev_t *d, uint32_t pp) {
    d->bw = (int)MEM32(pp), d->bh = (int)MEM32(pp + 4);
    d->bfmt = MEM32(pp + 8);
    d->windowed = (int)MEM32(pp + 28);
    d->autodepth = (int)MEM32(pp + 32);
    d->dfmt = MEM32(pp + 36);
    d->interval = (int)MEM32(pp + 48);
    if (d->bw <= 0 || d->bh <= 0) d->bw = 1024, d->bh = 768;
    if (!d->bfmt) d->bfmt = FMT_X8R8G8B8;
    MEM32(pp) = (uint32_t)d->bw, MEM32(pp + 4) = (uint32_t)d->bh;
    if (!MEM32(pp + 12)) MEM32(pp + 12) = 1;
}
static void apply_window(d3ddev_t *d) {
    SDL_Window *w = hle_screen_sdl_window();
    if (!w) return;
    if (d->windowed) {
        SDL_SetWindowFullscreen(w, 0);
        int dw = 0, dh = 0;
        SDL_DisplayMode m;
        if (SDL_GetDesktopDisplayMode(0, &m) == 0) dw = m.w, dh = m.h;
        if (!dw || (d->bw < dw && d->bh < dh)) SDL_SetWindowSize(w, d->bw, d->bh);
    } else SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
    /* an interval of IMMEDIATE (0x80000000) means no vsync: frames as fast as they come */
    /* the player's choice (OFP_VSYNC, the options) wins; else the game's:
     * IMMEDIATE (0x80000000) means no vsync, frames as fast as they come */
    const char *vs = getenv("OFP_VSYNC");
    int want = vs && *vs ? overlay_vsync() : d->interval == (int)0x80000000 ? 0 : 1;
    d3d8_set_vsync(want);
    perf_note("window: %s, vsync asked %d", d->windowed ? "windowed" : "fullscreen", want);
}
static void d3_CreateDevice(void) {                  /* (this, adapter, type, hwnd, behavior, pp, out) */
    uint32_t pp = A32(5), out = A32(6);
    SDL_Window *w = hle_screen_sdl_window();
    if (!w) { fprintf(stderr, "[d3d8] no window (headless)\n"); RET(D3DERR_NOTAVAILABLE, 7); }
    if (g_dev) { fprintf(stderr, "[d3d8] a second device: the first is reused\n"); }
    d3ddev_t *d = g_dev ? g_dev : GNEW(d3ddev_t);
    if (!d) RET(D3DERR_NOTAVAILABLE, 7);
    read_pp(d, pp);
    d->hwnd = A32(3), d->behavior = A32(4);
    static int made;
    if (!made) {
        d->back = new_surface(SURF_BACK, d->bw, d->bh, d->bfmt, 1, 0, 0);
        d->depth = new_surface(SURF_DEPTH, d->bw, d->bh, d->dfmt ? d->dfmt : FMT_D24S8, 2, 0, 0);
        if (!d->back || !d->depth) RET(D3DERR_NOTAVAILABLE, 7);
        MEM32(d->back->self + 4) = 0x10000, MEM32(d->depth->self + 4) = 0x10000;   /* the device's own */
    }
    state_defaults(&d->s);
    d->s.vp[2] = (uint32_t)d->bw, d->s.vp[3] = (uint32_t)d->bh;
    d->rt = NULL;
    d->zs = d->autodepth ? d->depth : NULL;
    d->msaa = msaa_wanted();
    back_resize(d);
    if (!made) {
        g_dev = d;
        if (!g_r->create(d)) { fprintf(stderr, "[d3d8] %s: no device\n", g_r->name); g_dev = NULL; RET(D3DERR_NOTAVAILABLE, 7); }
        made = 1;
    } else g_r->resize(d);
    apply_window(d);                                   /* (vsync wants the context) */
    d->d_rs = d->d_tss = d->d_xf = d->d_light = d->d_mat = 1;
    if (!d->self) d->self = new_obj(g_vt_dev, d);
    g_dev = d;
    MEM32(out) = d->self;
    fprintf(stderr, "[d3d8] device %dx%d fmt %u %s, depth %s fmt %u, interval 0x%X\n", d->bw, d->bh, d->bfmt,
            d->windowed ? "windowed" : "fullscreen", d->autodepth ? "auto" : "none", d->dfmt, (unsigned)d->interval);
    perf_note("Direct3D device (%s): %dx%d, format %u, %s, depth %s format %u, present interval 0x%X", g_r->name, d->bw, d->bh, d->bfmt,
              d->windowed ? "windowed" : "fullscreen", d->autodepth ? "auto" : "none", d->dfmt, (unsigned)d->interval);
    RET(D3D_OK, 7);
}

/* ------------------------------------------------------------------ IDirect3DDevice8 */
#define DEV_SELF ((d3ddev_t *)HOST(A32(0)))
static void dv_QueryInterface(void) { if (A32(2)) MEM32(A32(2)) = 0; RET(E_NOINTERFACE, 3); }
static void dv_Release(void) { uint32_t n = MEM32(A32(0) + 4); if (n) MEM32(A32(0) + 4) = --n; RET(n, 1); }
static void dv_TestCooperativeLevel(void) { RET(D3D_OK, 1); }
static void dv_GetAvailableTextureMem(void) { RET(512u << 20, 1); }
static void dv_GetDirect3D(void) { MEM32(g_d3d + 4)++; MEM32(A32(1)) = g_d3d; RET(D3D_OK, 2); }
static void dv_GetDeviceCaps(void) { put_caps(A32(1)); RET(D3D_OK, 2); }
static void dv_GetDisplayMode(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t p = A32(1);
    MEM32(p) = (uint32_t)d->bw, MEM32(p + 4) = (uint32_t)d->bh, MEM32(p + 8) = 60, MEM32(p + 12) = d->bfmt;
    RET(D3D_OK, 2);
}
static void dv_GetCreationParameters(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t p = A32(1);
    MEM32(p) = 0, MEM32(p + 4) = 1, MEM32(p + 8) = d->hwnd, MEM32(p + 12) = d->behavior;
    RET(D3D_OK, 2);
}
static void dv_ShowCursor(void) { RET(0, 2); }
static void dv_SetCursorPosition(void) { RETV(4); }
static void dv_Reset(void) {
    d3ddev_t *d = DEV_SELF;
    read_pp(d, A32(1));
    apply_window(d);
    d->msaa = msaa_wanted();
    back_resize(d);
    g_r->resize(d);
    d->rt = NULL;
    d->zs = d->autodepth ? d->depth : NULL;
    g_r->targets(d);
    state_defaults(&d->s);
    d->s.vp[2] = (uint32_t)d->bw, d->s.vp[3] = (uint32_t)d->bh;
    d->d_rs = d->d_tss = d->d_xf = d->d_light = d->d_mat = 1;
    fprintf(stderr, "[d3d8] reset %dx%d %s\n", d->bw, d->bh, d->windowed ? "windowed" : "fullscreen");
    perf_note("Direct3D reset: %dx%d, %s, present interval 0x%X", d->bw, d->bh, d->windowed ? "windowed" : "fullscreen", (unsigned)d->interval);
    RET(D3D_OK, 2);
}
/* ------------------------------------------------------------------ present */
static int g_msaa_pending;                             /* the options changed antialiasing (d3d8_set_msaa) */
/* env OFP_FPS_CAP=N: at most N frames a second, on a fixed schedule (the
 * bench: the game's work a frame depends on the time between frames, so a
 * free frame rate makes it vary from run to run; capped below what the
 * machine reaches, every run draws the same frames). A frame late by more
 * than a period restarts the schedule. */
static void fps_cap(void) {
    static int64_t period = -1;
    static uint64_t next;
    if (period < 0) { int n = ofp_env_int("OFP_FPS_CAP", 0); period = n > 0 ? 1000000000 / n : 0; }
    if (!period) return;
    uint64_t t = hle_mono_ns();
    if (!next || t > next + (uint64_t)period) next = t;
    else if (t < next) {
        struct timespec ts = { (time_t)(next / 1000000000ull), (long)(next % 1000000000ull) };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    next += (uint64_t)period;
}
static void present(d3ddev_t *d) {
    g_r->present(d);
    fps_cap();
    if (g_msaa_pending) {                              /* between frames, on the thread that draws */
        g_msaa_pending = 0;
        d->msaa = msaa_wanted();
        g_r->resize(d);
        d->rt = NULL;
        d->zs = d->autodepth ? d->depth : NULL;
        g_r->targets(d);
    }
    d->d_rs = 1;
    d->frames++;
    perf_frame(d->draws);
    if (d3d8_trace() && d->frames % 100 == 0) T("frame %u, %u draws since the last\n", d->frames, d->draws);
    d->draws = 0;
}
/* the options (overlay.c): antialiasing and vertical sync, applied at once;
 * antialiasing, asked from the options' key (the message pump, maybe
 * mid-scene), by the next Present */
void d3d8_set_msaa(void) { g_msaa_pending = 1; }       /* overlay_msaa() has the count */
void d3d8_set_vsync(int on) { g_r->vsync(on); }
int msaa_wanted(void) { int n = overlay_msaa(); return n < 2 ? 0 : n; }
/* env OFP_SHOT=DIR: every 100th frame (OFP_SHOT_EVERY=N: every Nth), the back buffer as DIR/frame_N.ppm
 * (what the game drew, before the window: for tests on a virtual screen) */
const char *d3d8_shot_due(d3ddev_t *d) {
    static const char *dir; static int on = -1, every = 100;
    static char path[4096];
    if (on < 0) {
        on = (dir = getenv("OFP_SHOT")) != NULL;
        every = ofp_env_int("OFP_SHOT_EVERY", 100) > 0 ? ofp_env_int("OFP_SHOT_EVERY", 100) : 100;
    }
    if (!on || (d->frames + 1) % (unsigned)every) return NULL;
    snprintf(path, sizeof path, "%s/frame_%05u.ppm", dir, d->frames + 1);
    return path;
}
void d3d8_shot_write(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 3, (size_t)w * h, f); fclose(f); }
}
static void dv_Present(void) { present(DEV_SELF); RET(D3D_OK, 5); }
static void dv_GetBackBuffer(void) {                 /* (this, i, type, out) */
    d3ddev_t *d = DEV_SELF;
    MEM32(d->back->self + 4)++;
    MEM32(A32(3)) = d->back->self;
    RET(D3D_OK, 4);
}
static void dv_GetRasterStatus(void) { MEM32(A32(1)) = 1; MEM32(A32(1) + 4) = 0; RET(D3D_OK, 2); }
static void dv_SetGammaRamp(void) { RETV(3); }
static void dv_GetGammaRamp(void) {
    uint16_t *r = (uint16_t *)APTR(1);
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) r[c * 256 + i] = (uint16_t)(i * 257);
    RETV(2);
}

static tex_t *new_texture(int w, int h, int levels, uint32_t usage, uint32_t fmt, uint32_t pool) {
    tex_t *t = GNEW(tex_t);
    if (!t) return NULL;
    int max = 1;
    for (int a = w, b = h; a > 1 || b > 1; a = a > 1 ? a / 2 : 1, b = b > 1 ? b / 2 : 1) max++;
    if (!levels || levels > max) levels = max;
    if (levels > MAXLVL) levels = MAXLVL;
    t->w = w, t->h = h, t->levels = levels, t->fmt = fmt, t->usage = usage, t->pool = pool;
    for (int l = 0, lw = w, lh = h; l < levels; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) {
        surf_t *s = new_surface((usage & 1) ? SURF_RT : SURF_TEXLEVEL, lw, lh, fmt, usage, pool, 1);
        if (!s) { t->levels = l, t->failed = 1; break; }
        s->parent = t, s->level = l;
        MEM32(s->self + 4) = 0;                            /* held by the texture */
        t->lvl[l] = s;
    }
    if ((usage & 1) && !t->failed) {                      /* a render target: one level, drawn into */
        for (int l = 1; l < t->levels; l++) {
            res_free(t->lvl[l]->mem);
            hle_com_free(t->lvl[l]->self);
            hle_free(t->lvl[l]);
            t->lvl[l] = NULL;
        }
        t->levels = 1;
    }
    if (!t->failed && !g_r->tex_init(t)) t->failed = 1;
    t->self = new_obj(g_vt_tex, t);
    return t;
}
/* a texture, its levels and the renderer's copy, gone */
static void tex_destroy(tex_t *t) {
    g_r->tex_free(t);
    stages_dirty();
    for (int l = 0; l < t->levels; l++) {
        stage_free(&t->lvl[l]->st, t->lvl[l]->mem);
        res_free(t->lvl[l]->mem);
        hle_com_free(t->lvl[l]->self);
        hle_free(t->lvl[l]);
    }
    hle_com_free(t->self);
    hle_free(t);
}
static void dv_CreateTexture(void) {                 /* (this, w, h, levels, usage, fmt, pool, out) */
    uint32_t fmt = A32(5);
    if (!fmt_info(fmt, NULL) || is_depth(fmt)) { T("CreateTexture fmt %u: not available\n", fmt); RET(D3DERR_INVALIDCALL, 8); }
    tex_t *t = new_texture((int)A32(1), (int)A32(2), (int)A32(3), A32(4), fmt, A32(6));
    if (!t) { perf_note("CreateTexture %ux%u: out of memory", A32(1), A32(2)); MEM32(A32(7)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 8); }
    int lost = t->failed;
    for (int l = 0; l < t->levels; l++) lost |= !t->lvl[l]->mem;
    if (lost) {                                        /* out of memory: as D3D says it */
        perf_note("CreateTexture %ux%u fmt %u: out of memory", A32(1), A32(2), fmt);
        tex_destroy(t);
        MEM32(A32(7)) = 0;
        RET(D3DERR_OUTOFVIDEOMEMORY, 8);
    }
    T("CreateTexture %ux%u levels %d fmt %u usage %X -> %08X\n", A32(1), A32(2), t->levels, fmt, A32(4), t->self);
    MEM32(A32(7)) = t->self;
    RET(D3D_OK, 8);
}
static void dv_CreateVolumeTexture(void) { MEM32(A32(8)) = 0; RET(D3DERR_NOTAVAILABLE, 9); }
static void dv_CreateCubeTexture(void) { MEM32(A32(6)) = 0; RET(D3DERR_NOTAVAILABLE, 7); }
static void dv_CreateVertexBuffer(void) {            /* (this, length, usage, fvf, pool, out) */
    vb_t *b = GNEW(vb_t);
    if (!b) { MEM32(A32(5)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 6); }
    b->size = A32(1), b->usage = A32(2), b->fvf = A32(3), b->pool = A32(4);
    b->mem = (uint8_t *)res_alloc(b->size);
    if (!b->mem) {
        perf_note("CreateVertexBuffer %u bytes: out of memory", b->size);
        hle_free(b), MEM32(A32(5)) = 0;
        RET(D3DERR_OUTOFVIDEOMEMORY, 6);
    }
    b->self = new_obj(g_vt_vb, b);
    MEM32(A32(5)) = b->self;
    T("CreateVertexBuffer %u fvf %X -> %08X\n", b->size, b->fvf, b->self);
    RET(D3D_OK, 6);
}
static void dv_CreateIndexBuffer(void) {             /* (this, length, usage, fmt, pool, out) */
    ib_t *b = GNEW(ib_t);
    if (!b) { MEM32(A32(5)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 6); }
    b->size = A32(1), b->usage = A32(2), b->fmt = A32(3), b->pool = A32(4);
    b->mem = (uint8_t *)res_alloc(b->size);
    if (!b->mem) {
        perf_note("CreateIndexBuffer %u bytes: out of memory", b->size);
        hle_free(b), MEM32(A32(5)) = 0;
        RET(D3DERR_OUTOFVIDEOMEMORY, 6);
    }
    b->self = new_obj(g_vt_ib, b);
    MEM32(A32(5)) = b->self;
    RET(D3D_OK, 6);
}
static void dv_CreateRenderTarget(void) {            /* (this, w, h, fmt, ms, lockable, out) */
    surf_t *s = new_surface(SURF_RT, (int)A32(1), (int)A32(2), A32(3), 1, 0, 1);
    if (!s || !s->mem) { if (s) hle_com_free(s->self), hle_free(s); MEM32(A32(6)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 7); }
    if (!g_r->rt_init(s)) { res_free(s->mem), hle_com_free(s->self), hle_free(s); MEM32(A32(6)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 7); }
    MEM32(A32(6)) = s->self;
    RET(D3D_OK, 7);
}
static void dv_CreateDepthStencilSurface(void) {     /* (this, w, h, fmt, ms, out) */
    surf_t *s = new_surface(SURF_DEPTH, (int)A32(1), (int)A32(2), A32(3), 2, 0, 0);
    if (!s) { MEM32(A32(5)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 6); }
    if (!g_r->depth_init(s)) { hle_com_free(s->self), hle_free(s); MEM32(A32(5)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 6); }
    MEM32(A32(5)) = s->self;
    RET(D3D_OK, 6);
}
static void dv_CreateImageSurface(void) {            /* (this, w, h, fmt, out) */
    surf_t *s = new_surface(SURF_IMAGE, (int)A32(1), (int)A32(2), A32(3), 0, 2, 1);
    if (!s || !s->mem) { if (s) hle_com_free(s->self), hle_free(s); MEM32(A32(4)) = 0; RET(D3DERR_OUTOFVIDEOMEMORY, 5); }
    MEM32(A32(4)) = s->self;
    RET(D3D_OK, 5);
}
static void mark_dirty(surf_t *s) { if (s->parent) s->parent->dirty |= 1u << s->level; }
static void copy_rect(surf_t *src, int sx, int sy, int w, int h, surf_t *dst, int dx, int dy) {
    if (!src->mem || !dst->mem || src->fmt != dst->fmt) {
        T("CopyRects between formats %u -> %u (or a GPU surface): skipped\n", src->fmt, dst->fmt);
        return;
    }
    fmt_t g;
    fmt_info(src->fmt, &g);
    int bx = g.block ? 4 : 1;
    uint32_t bytes = g.block ? (uint32_t)g.block : (uint32_t)g.bpp;
    for (int y = 0; y < h; y += bx) {
        memcpy(dst->mem + (size_t)((dy + y) / bx) * dst->pitch + (size_t)(dx / bx) * bytes,
               src->mem + (size_t)((sy + y) / bx) * src->pitch + (size_t)(sx / bx) * bytes,
               (size_t)((w + bx - 1) / bx) * bytes);
    }
    mark_dirty(dst);
}
static void dv_CopyRects(void) {                     /* (this, src, rects, n, dst, points) */
    surf_t *src = (surf_t *)HOST(A32(1)), *dst = (surf_t *)HOST(A32(4));
    uint32_t rects = A32(2), n = A32(3), pts = A32(5);
    if (!rects || !n) copy_rect(src, 0, 0, src->w, src->h, dst, 0, 0);
    else for (uint32_t i = 0; i < n; i++) {
        int l = (int)MEM32(rects + 16 * i), t = (int)MEM32(rects + 16 * i + 4);
        int r = (int)MEM32(rects + 16 * i + 8), b = (int)MEM32(rects + 16 * i + 12);
        int dx = pts ? (int)MEM32(pts + 8 * i) : l, dy = pts ? (int)MEM32(pts + 8 * i + 4) : t;
        copy_rect(src, l, t, r - l, b - t, dst, dx, dy);
    }
    RET(D3D_OK, 6);
}
static void dv_UpdateTexture(void) {                 /* (this, src, dst) */
    tex_t *s = (tex_t *)HOST(A32(1)), *t = (tex_t *)HOST(A32(2));
    for (int l = 0; l < s->levels && l < t->levels; l++)
        if (s->lvl[l]->size == t->lvl[l]->size) memcpy(t->lvl[l]->mem, s->lvl[l]->mem, s->lvl[l]->size);
    t->dirty = ~0u;
    RET(D3D_OK, 3);
}
static void dv_GetFrontBuffer(void) { RET(D3D_OK, 2); }
static void dv_SetRenderTarget(void) {               /* (this, rt, zs) */
    d3ddev_t *d = DEV_SELF;
    surf_t *rt = A32(1) ? (surf_t *)HOST(A32(1)) : NULL, *zs = A32(2) ? (surf_t *)HOST(A32(2)) : NULL;
    if (rt) d->rt = rt->kind == SURF_BACK ? NULL : rt;
    d->zs = zs;
    g_r->targets(d);
    d->s.vp[0] = d->s.vp[1] = 0, d->s.vp[2] = (uint32_t)rt_w(d), d->s.vp[3] = (uint32_t)rt_h(d);
    d->s.vpz[0] = 0, d->s.vpz[1] = 1;
    d->d_xf = 1;
    RET(D3D_OK, 3);
}
static void dv_GetRenderTarget(void) {
    d3ddev_t *d = DEV_SELF;
    surf_t *s = d->rt ? d->rt : d->back;
    MEM32(s->self + 4)++;
    MEM32(A32(1)) = s->self;
    RET(D3D_OK, 2);
}
static void dv_GetDepthStencilSurface(void) {
    d3ddev_t *d = DEV_SELF;
    if (!d->zs) { MEM32(A32(1)) = 0; RET(D3DERR_NOTAVAILABLE, 2); }
    MEM32(d->zs->self + 4)++;
    MEM32(A32(1)) = d->zs->self;
    RET(D3D_OK, 2);
}
static void dv_BeginScene(void) { RET(D3D_OK, 1); }
static void dv_EndScene(void) { RET(D3D_OK, 1); }
static void dv_Clear(void) {                         /* (this, count, rects, flags, color, z, stencil) */
    d3ddev_t *d = DEV_SELF;
    uint32_t n = A32(1), rects = A32(2), flags = A32(3), color = A32(4), stencil = A32(6);
    int32_t r[4 * 16];
    if (rects && n > 16) n = 16;
    for (uint32_t i = 0; rects && i < n * 4; i++) r[i] = (int32_t)MEM32(rects + 4 * i);
    g_r->clear(d, rects ? n : 0, r, flags, color, fl(A32(5)), stencil);
    d->d_rs = 1;
    RET(D3D_OK, 7);
}
static float *xf_slot(d3ddev_t *d, state_t *s, uint32_t t) {
    (void)d;
    if (t == 2) return s->view;
    if (t == 3) return s->proj;
    if (t >= 16 && t < 16 + NTS) return s->texm[t - 16];
    if (t == 256) return s->world;
    return NULL;                                         /* world 1..255: vertex blending, not offered */
}
static void rec_xf(d3ddev_t *d, uint32_t t) {
    if (!d->rec) return;
    mask_t *m = &d->rec->m;
    if (t == 2) m->view = 1; else if (t == 3) m->proj = 1; else if (t == 256) m->world = 1;
    else if (t >= 16 && t < 16 + NTS) m->texm[t - 16] = 1;
}
static void dv_SetTransform(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t t = A32(1);
    state_t *s = d->rec ? &d->rec->s : &d->s;
    float *m = xf_slot(d, s, t);
    if (m) memcpy(m, APTR(2), 64);
    rec_xf(d, t);
    if (!d->rec) { d->d_xf = 1; if (t == 2) d->d_light = 1; }
    RET(D3D_OK, 3);
}
static void dv_GetTransform(void) {
    d3ddev_t *d = DEV_SELF;
    float *m = xf_slot(d, &d->s, A32(1));
    if (m) memcpy(APTR(2), m, 64); else { float id[16]; identity(id); memcpy(APTR(2), id, 64); }
    RET(D3D_OK, 3);
}
static void mat_mul(float *r, const float *a, const float *b) {     /* r = a * b, D3D row vectors */
    float t[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
        t[i * 4 + j] = s;
    }
    memcpy(r, t, 64);
}
static void dv_MultiplyTransform(void) {
    d3ddev_t *d = DEV_SELF;
    float *m = xf_slot(d, &d->s, A32(1));
    if (m) mat_mul(m, m, (const float *)APTR(2));
    d->d_xf = 1;
    if (A32(1) == 2) d->d_light = 1;
    RET(D3D_OK, 3);
}
static void dv_SetViewport(void) {
    d3ddev_t *d = DEV_SELF;
    state_t *s = d->rec ? &d->rec->s : &d->s;
    uint32_t p = A32(1);
    s->vp[0] = MEM32(p), s->vp[1] = MEM32(p + 4), s->vp[2] = MEM32(p + 8), s->vp[3] = MEM32(p + 12);
    s->vpz[0] = fl(MEM32(p + 16)), s->vpz[1] = fl(MEM32(p + 20));
    if (d->rec) d->rec->m.vp = 1; else d->d_xf = 1;
    RET(D3D_OK, 2);
}
static void dv_GetViewport(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t p = A32(1);
    MEM32(p) = d->s.vp[0], MEM32(p + 4) = d->s.vp[1], MEM32(p + 8) = d->s.vp[2], MEM32(p + 12) = d->s.vp[3];
    MEM32(p + 16) = uf(d->s.vpz[0]), MEM32(p + 20) = uf(d->s.vpz[1]);
    RET(D3D_OK, 2);
}
static void dv_SetMaterial(void) {
    d3ddev_t *d = DEV_SELF;
    state_t *s = d->rec ? &d->rec->s : &d->s;
    memcpy(s->mat, APTR(1), 68);
    if (d->rec) d->rec->m.mat = 1; else d->d_mat = 1;
    RET(D3D_OK, 2);
}
static void dv_GetMaterial(void) { memcpy(APTR(1), DEV_SELF->s.mat, 68); RET(D3D_OK, 2); }
static void dv_SetLight(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t i = A32(1);
    if (i >= NLIGHT) RET(D3D_OK, 3);
    state_t *s = d->rec ? &d->rec->s : &d->s;
    light_t *l = &s->light[i];
    const uint32_t *p = (const uint32_t *)APTR(2);
    l->type = p[0];
    memcpy(l->diffuse, p + 1, 16), memcpy(l->specular, p + 5, 16), memcpy(l->ambient, p + 9, 16);
    memcpy(l->pos, p + 13, 12), memcpy(l->dir, p + 16, 12);
    memcpy(&l->range, p + 19, 4), memcpy(&l->falloff, p + 20, 4);
    memcpy(&l->att0, p + 21, 4), memcpy(&l->att1, p + 22, 4), memcpy(&l->att2, p + 23, 4);
    memcpy(&l->theta, p + 24, 4), memcpy(&l->phi, p + 25, 4);
    if (d->rec) d->rec->m.light[i] = 1; else d->d_light = 1;
    RET(D3D_OK, 3);
}
static void dv_GetLight(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t i = A32(1);
    if (i >= NLIGHT) RET(D3DERR_INVALIDCALL, 3);
    const light_t *l = &d->s.light[i];
    uint32_t *p = (uint32_t *)APTR(2);
    p[0] = l->type;
    memcpy(p + 1, l->diffuse, 16), memcpy(p + 5, l->specular, 16), memcpy(p + 9, l->ambient, 16);
    memcpy(p + 13, l->pos, 12), memcpy(p + 16, l->dir, 12);
    memcpy(p + 19, &l->range, 4), memcpy(p + 20, &l->falloff, 4);
    memcpy(p + 21, &l->att0, 4), memcpy(p + 22, &l->att1, 4), memcpy(p + 23, &l->att2, 4);
    memcpy(p + 24, &l->theta, 4), memcpy(p + 25, &l->phi, 4);
    RET(D3D_OK, 3);
}
static void dv_LightEnable(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t i = A32(1);
    if (i < NLIGHT) {
        state_t *s = d->rec ? &d->rec->s : &d->s;
        s->light_on[i] = A32(2) != 0;
        if (d->rec) d->rec->m.light_on[i] = 1; else d->d_light = 1;
    }
    RET(D3D_OK, 3);
}
static void dv_GetLightEnable(void) {
    uint32_t i = A32(1);
    MEM32(A32(2)) = i < NLIGHT ? DEV_SELF->s.light_on[i] : 0;
    RET(D3D_OK, 3);
}
static void dv_SetRenderState(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t r = A32(1), v = A32(2);
    if (r < NRS) {
        if (d->rec) d->rec->s.rs[r] = v, d->rec->m.rs[r] = 1;
        else if (d->s.rs[r] != v) {
            d->s.rs[r] = v;
            d->d_rs = 1;
        }
    }
    RET(D3D_OK, 3);
}
static void dv_GetRenderState(void) {
    uint32_t r = A32(1);
    MEM32(A32(2)) = r < NRS ? DEV_SELF->s.rs[r] : 0;
    RET(D3D_OK, 3);
}

/* state blocks: a token is a block_t pointer */
static void capture(d3ddev_t *d, block_t *b) {
    state_t *c = &d->s, *s = &b->s;
    mask_t *m = &b->m;
    for (int i = 0; i < NRS; i++) if (m->rs[i]) s->rs[i] = c->rs[i];
    for (int i = 0; i < NTS; i++) {
        for (int j = 0; j < NTSS; j++) if (m->tss[i][j]) s->tss[i][j] = c->tss[i][j];
        if (m->tex[i]) s->tex[i] = c->tex[i];
        if (m->texm[i]) memcpy(s->texm[i], c->texm[i], 64);
    }
    for (int i = 0; i < NLIGHT; i++) {
        if (m->light[i]) s->light[i] = c->light[i];
        if (m->light_on[i]) s->light_on[i] = c->light_on[i];
    }
    if (m->world) memcpy(s->world, c->world, 64);
    if (m->view) memcpy(s->view, c->view, 64);
    if (m->proj) memcpy(s->proj, c->proj, 64);
    if (m->vp) memcpy(s->vp, c->vp, 16), memcpy(s->vpz, c->vpz, 8);
    if (m->mat) memcpy(s->mat, c->mat, 68);
    if (m->stream) s->stream_vb = c->stream_vb, s->stream_stride = c->stream_stride;
    if (m->ib) s->ib = c->ib, s->base_vertex = c->base_vertex;
    if (m->vshader) s->vshader = c->vshader;
    if (m->pshader) s->pshader = c->pshader;
}
static void apply_block(d3ddev_t *d, const block_t *b) {
    state_t *c = &d->s;
    const state_t *s = &b->s;
    const mask_t *m = &b->m;
    for (int i = 0; i < NRS; i++) if (m->rs[i]) c->rs[i] = s->rs[i];
    for (int i = 0; i < NTS; i++) {
        for (int j = 0; j < NTSS; j++) if (m->tss[i][j]) c->tss[i][j] = s->tss[i][j];
        if (m->tex[i]) c->tex[i] = s->tex[i];
        if (m->texm[i]) memcpy(c->texm[i], s->texm[i], 64);
    }
    for (int i = 0; i < NLIGHT; i++) {
        if (m->light[i]) c->light[i] = s->light[i];
        if (m->light_on[i]) c->light_on[i] = s->light_on[i];
    }
    if (m->world) memcpy(c->world, s->world, 64);
    if (m->view) memcpy(c->view, s->view, 64);
    if (m->proj) memcpy(c->proj, s->proj, 64);
    if (m->vp) memcpy(c->vp, s->vp, 16), memcpy(c->vpz, s->vpz, 8);
    if (m->mat) memcpy(c->mat, s->mat, 68);
    if (m->stream) c->stream_vb = s->stream_vb, c->stream_stride = s->stream_stride;
    if (m->ib) c->ib = s->ib, c->base_vertex = s->base_vertex;
    if (m->vshader) c->vshader = s->vshader;
    if (m->pshader) c->pshader = s->pshader;
    d->d_rs = d->d_tss = d->d_xf = d->d_light = d->d_mat = 1;
}
static void dv_BeginStateBlock(void) {
    d3ddev_t *d = DEV_SELF;
    d->rec = GNEW(block_t);
    if (!d->rec) RET(0x8007000Eu, 1);                 /* E_OUTOFMEMORY */
    d->rec->s = d->s;
    RET(D3D_OK, 1);
}
static void dv_EndStateBlock(void) {
    d3ddev_t *d = DEV_SELF;
    MEM32(A32(1)) = hle_va(d->rec);
    d->rec = NULL;
    RET(D3D_OK, 2);
}
static void dv_ApplyStateBlock(void) { if (A32(1)) apply_block(DEV_SELF, (block_t *)hle_ptr(A32(1))); RET(D3D_OK, 2); }
static void dv_CaptureStateBlock(void) { if (A32(1)) capture(DEV_SELF, (block_t *)hle_ptr(A32(1))); RET(D3D_OK, 2); }
static void dv_DeleteStateBlock(void) { hle_free(hle_ptr(A32(1))); RET(D3D_OK, 2); }
static void dv_CreateStateBlock(void) {              /* (this, type, out): ALL 1, PIXEL 2, VERTEX 3 */
    d3ddev_t *d = DEV_SELF;
    block_t *b = GNEW(block_t);
    if (!b) { MEM32(A32(2)) = 0; RET(0x8007000Eu, 3); }
    memset(&b->m, 1, sizeof b->m);                     /* near enough for PIXEL and VERTEX too */
    b->s = d->s;
    MEM32(A32(2)) = hle_va(b);
    RET(D3D_OK, 3);
}
static void dv_GetTexture(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t i = A32(1), t = i < NTS ? d->s.tex[i] : 0;
    if (t) MEM32(t + 4)++;
    MEM32(A32(2)) = t;
    RET(D3D_OK, 3);
}
static void dv_SetTexture(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t i = A32(1);
    if (i < NTS) {
        if (d->rec) d->rec->s.tex[i] = A32(2), d->rec->m.tex[i] = 1;
        else d->s.tex[i] = A32(2), d->d_tss = 1;
    }
    RET(D3D_OK, 3);
}
static void dv_GetTextureStageState(void) {
    uint32_t i = A32(1), t = A32(2);
    MEM32(A32(3)) = i < NTS && t < NTSS ? DEV_SELF->s.tss[i][t] : 0;
    RET(D3D_OK, 4);
}
static void dv_SetTextureStageState(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t i = A32(1), t = A32(2), v = A32(3);
    if (i < NTS && t < NTSS) {
        if (d->rec) d->rec->s.tss[i][t] = v, d->rec->m.tss[i][t] = 1;
        else d->s.tss[i][t] = v, d->d_tss = 1;
    }
    RET(D3D_OK, 4);
}
static void dv_ValidateDevice(void) { if (A32(1)) MEM32(A32(1)) = 1; RET(D3D_OK, 2); }
static void dv_GetInfo(void) { RET(S_FALSE, 4); }
static void dv_GetCurrentTexturePalette(void) { MEM32(A32(1)) = 0; RET(D3D_OK, 2); }

/* a draw to the renderer, unless it needs what is not offered */
static void submit(d3ddev_t *d, draw_t *dr) {
    if (dr->count <= 0 || !dr->base) return;
    dr->fvf = d->s.vshader;
    if (dr->fvf & 1) { T("draw with a programmable vertex shader 0x%X: skipped\n", dr->fvf); return; }
    if (!dr->stride) { fvf_t f; fvf_decode(dr->fvf, &f); dr->stride = f.stride; }
    d->draws++;
    uint64_t t0 = perf_file() ? hle_ticks() : 0;      /* timed only when the log is on */
    g_r->draw(d, dr);
    if (t0) perf_rc[RC_NS] += hle_ticks_ns(hle_ticks() - t0);
}
static int stream_stride(d3ddev_t *d) {
    fvf_t f;
    fvf_decode(d->s.vshader, &f);
    return d->s.stream_stride ? (int)d->s.stream_stride : f.stride;
}
static void dv_DrawPrimitive(void) {                 /* (this, type, start, count) */
    d3ddev_t *d = DEV_SELF;
    int n = prim_count_verts(A32(1), A32(3));
    vb_t *b = d->s.stream_vb ? (vb_t *)HOST(d->s.stream_vb) : NULL;
    if (b && n && b->mem) {
        draw_t dr = { .prim = A32(1), .stride = stream_stride(d), .base = b->mem, .vb = b, .vertex0 = 0,
                      .first = (int)A32(2), .nv = n, .count = n };
        submit(d, &dr);
    }
    RET(D3D_OK, 4);
}
static void dv_DrawIndexedPrimitive(void) {          /* (this, type, minindex, numvertices, startindex, count) */
    d3ddev_t *d = DEV_SELF;
    int n = prim_count_verts(A32(1), A32(5));
    vb_t *b = d->s.stream_vb ? (vb_t *)HOST(d->s.stream_vb) : NULL;
    ib_t *ib = d->s.ib ? (ib_t *)HOST(d->s.ib) : NULL;
    int minv = (int)A32(2), nv = (int)A32(3);
    if (b && ib && n && b->mem && ib->mem && nv > 0) {
        int stride = stream_stride(d), is32 = ib->fmt == FMT_INDEX32;
        uint32_t ioff = A32(4) * (is32 ? 4u : 2u);
        if (ioff + (uint32_t)n * (is32 ? 4u : 2u) > ib->size) { T("DrawIndexedPrimitive past its index buffer: skipped\n"); RET(D3D_OK, 6); }
        draw_t dr = { .prim = A32(1), .stride = stride, .base = b->mem + (size_t)d->s.base_vertex * (size_t)stride,
                      .vb = b, .vertex0 = d->s.base_vertex, .first = 0, .nv = minv + nv,
                      .idx = ib->mem + ioff, .is32 = is32, .ib = ib, .ioff = ioff, .count = n,
                      .rmin = minv, .rmax = minv + nv - 1 };
        submit(d, &dr);
    }
    RET(D3D_OK, 6);
}
static void dv_DrawPrimitiveUP(void) {               /* (this, type, count, data, stride) */
    d3ddev_t *d = DEV_SELF;
    int n = prim_count_verts(A32(1), A32(2));
    if (n) {
        draw_t dr = { .prim = A32(1), .stride = (int)A32(4), .base = (const uint8_t *)APTR(3), .first = 0, .nv = n, .count = n };
        submit(d, &dr);
    }
    d->s.stream_vb = 0;                                /* as D3D: the stream is unset after a UP draw */
    RET(D3D_OK, 5);
}
static void dv_DrawIndexedPrimitiveUP(void) {        /* (this, type, minindex, numverts, count, idx, idxfmt, data, stride) */
    d3ddev_t *d = DEV_SELF;
    int n = prim_count_verts(A32(1), A32(4));
    int minv = (int)A32(2), nv = (int)A32(3);
    if (n && nv > 0) {
        draw_t dr = { .prim = A32(1), .stride = (int)A32(8), .base = (const uint8_t *)APTR(7), .first = 0, .nv = minv + nv,
                      .idx = APTR(5), .is32 = A32(6) == FMT_INDEX32, .count = n, .rmin = minv, .rmax = minv + nv - 1 };
        submit(d, &dr);
    }
    d->s.stream_vb = 0, d->s.ib = 0;
    RET(D3D_OK, 9);
}
static void dv_ProcessVertices(void) { T("ProcessVertices: not done\n"); RET(D3D_OK, 6); }
static uint32_t g_next_shader = 0x10001;
static void dv_CreateVertexShader(void) {            /* (this, decl, function, out, usage) */
    T("CreateVertexShader (function %s): a handle, not run\n", A32(2) ? "given" : "none");
    MEM32(A32(3)) = g_next_shader, g_next_shader += 2;
    RET(D3D_OK, 5);
}
static void dv_SetVertexShader(void) {
    d3ddev_t *d = DEV_SELF;
    if (d->rec) d->rec->s.vshader = A32(1), d->rec->m.vshader = 1;
    else d->s.vshader = A32(1);
    RET(D3D_OK, 2);
}
static void dv_GetVertexShader(void) { MEM32(A32(1)) = DEV_SELF->s.vshader; RET(D3D_OK, 2); }
static void dv_SetStreamSource(void) {               /* (this, stream, vb, stride) */
    d3ddev_t *d = DEV_SELF;
    if (A32(1) == 0) {
        state_t *s = d->rec ? &d->rec->s : &d->s;
        s->stream_vb = A32(2), s->stream_stride = A32(3);
        if (d->rec) d->rec->m.stream = 1;
    }
    RET(D3D_OK, 4);
}
static void dv_GetStreamSource(void) {
    d3ddev_t *d = DEV_SELF;
    uint32_t vb = A32(1) == 0 ? d->s.stream_vb : 0;
    if (vb) MEM32(vb + 4)++;
    MEM32(A32(2)) = vb;
    MEM32(A32(3)) = A32(1) == 0 ? d->s.stream_stride : 0;
    RET(D3D_OK, 4);
}
static void dv_SetIndices(void) {
    d3ddev_t *d = DEV_SELF;
    state_t *s = d->rec ? &d->rec->s : &d->s;
    s->ib = A32(1), s->base_vertex = A32(2);
    if (d->rec) d->rec->m.ib = 1;
    RET(D3D_OK, 3);
}
static void dv_GetIndices(void) {
    d3ddev_t *d = DEV_SELF;
    if (d->s.ib) MEM32(d->s.ib + 4)++;
    MEM32(A32(1)) = d->s.ib, MEM32(A32(2)) = d->s.base_vertex;
    RET(D3D_OK, 3);
}
static void dv_CreatePixelShader(void) { T("CreatePixelShader: a handle, not run\n"); MEM32(A32(2)) = g_next_shader, g_next_shader += 2; RET(D3D_OK, 3); }
static void dv_SetPixelShader(void) { DEV_SELF->s.pshader = A32(1); RET(D3D_OK, 2); }
static void dv_GetPixelShader(void) { MEM32(A32(1)) = DEV_SELF->s.pshader; RET(D3D_OK, 2); }
static void dv_notavail1(void) { RET(D3DERR_NOTAVAILABLE, 2); }
static void dv_notavail2(void) { RET(D3DERR_NOTAVAILABLE, 3); }
static void dv_notavail3(void) { RET(D3DERR_NOTAVAILABLE, 4); }
static void dv_CreateAdditionalSwapChain(void) { MEM32(A32(2)) = 0; RET(D3DERR_NOTAVAILABLE, 3); }

/* ------------------------------------------------------------------ resources */
static void res_GetDevice(void) { if (g_dev) MEM32(g_dev->self + 4)++; MEM32(A32(1)) = g_dev ? g_dev->self : 0; RET(D3D_OK, 2); }
static void res_SetPrivateData(void) { RET(D3D_OK, 5); }
static void res_GetPrivateData(void) { RET(D3DERR_NOTAVAILABLE, 4); }
static void res_FreePrivateData(void) { RET(D3D_OK, 2); }
static void res_SetPriority(void) { RET(0, 2); }
static void res_GetPriority(void) { RET(0, 1); }
static void res_PreLoad(void) { RETV(1); }
static void tex_GetType(void) { RET(3, 1); }                 /* D3DRTYPE_TEXTURE */
static void vb_GetType(void) { RET(6, 1); }
static void ib_GetType(void) { RET(7, 1); }

static void tx_Release(void) {
    uint32_t o = A32(0), n = MEM32(o + 4);
    if (n) MEM32(o + 4) = --n;
    if (!n) {
        tex_t *t = (tex_t *)HOST(o);
        if (g_dev) for (int i = 0; i < NTS; i++) if (g_dev->s.tex[i] == o) g_dev->s.tex[i] = 0, g_dev->d_tss = 1;
        tex_destroy(t);
    }
    RET(n, 1);
}
static void tx_SetLOD(void) { tex_t *t = (tex_t *)HOST(A32(0)); uint32_t o = t->lod; t->lod = A32(1); RET(o, 2); }
static void tx_GetLOD(void) { RET(((tex_t *)HOST(A32(0)))->lod, 1); }
static void tx_GetLevelCount(void) { RET((uint32_t)((tex_t *)HOST(A32(0)))->levels, 1); }
static void put_desc(uint32_t p, const surf_t *s, uint32_t type) {
    MEM32(p) = s->fmt, MEM32(p + 4) = type, MEM32(p + 8) = s->usage, MEM32(p + 12) = s->pool;
    MEM32(p + 16) = s->size, MEM32(p + 20) = 0, MEM32(p + 24) = (uint32_t)s->w, MEM32(p + 28) = (uint32_t)s->h;
}
static void tx_GetLevelDesc(void) {
    tex_t *t = (tex_t *)HOST(A32(0));
    uint32_t l = A32(1);
    if (l >= (uint32_t)t->levels) RET(D3DERR_INVALIDCALL, 3);
    put_desc(A32(2), t->lvl[l], 1);
    RET(D3D_OK, 3);
}
static void tx_GetSurfaceLevel(void) {
    tex_t *t = (tex_t *)HOST(A32(0));
    uint32_t l = A32(1);
    if (l >= (uint32_t)t->levels) RET(D3DERR_INVALIDCALL, 3);
    MEM32(t->lvl[l]->self + 4)++;
    MEM32(t->self + 4)++;                                 /* the level holds its texture, as D3D */
    MEM32(A32(2)) = t->lvl[l]->self;
    RET(D3D_OK, 3);
}
/* the pitch and the address of the rect (all of it without one) */
static void lock_out(surf_t *s, uint32_t out, uint32_t rect, uint32_t flags) {
    uint32_t off = 0, lo = 0, hi = s->size;
    if (rect) {
        fmt_t g;
        fmt_info(s->fmt, &g);
        int x = (int)MEM32(rect), y = (int)MEM32(rect + 4), y2 = (int)MEM32(rect + 12);
        int row = g.block ? 4 : 1;                     /* a compressed row is 4 pixels high */
        if (g.block) off = (uint32_t)(y / 4) * s->pitch + (uint32_t)(x / 4) * (uint32_t)g.block;
        else off = (uint32_t)y * s->pitch + (uint32_t)x * (uint32_t)g.bpp;
        lo = (uint32_t)(y / row) * s->pitch;           /* only the rect's rows are copied */
        uint32_t end = (uint32_t)((y2 + row - 1) / row) * s->pitch;
        if (lo > s->size) lo = s->size;
        hi = end > lo && end <= s->size ? end : s->size;
    }
    MEM32(out) = s->pitch;
    uint32_t va = s->mem ? stage_lock(&s->st, s->mem, s->size, lo, hi - lo, flags, 0) : 0;
    MEM32(out + 4) = va ? va + (off - lo) : 0;
}
static void tx_LockRect(void) {                      /* (this, level, out, rect, flags) */
    tex_t *t = (tex_t *)HOST(A32(0));
    uint32_t l = A32(1);
    if (l >= (uint32_t)t->levels || !t->lvl[l]->mem) RET(D3DERR_INVALIDCALL, 5);
    lock_out(t->lvl[l], A32(2), A32(3), A32(4));
    RET(D3D_OK, 5);
}
static void tx_UnlockRect(void) {
    tex_t *t = (tex_t *)HOST(A32(0));
    uint32_t l = A32(1);
    if (l < (uint32_t)t->levels) stage_unlock(&t->lvl[l]->st, t->lvl[l]->mem), t->dirty |= 1u << l;
    RET(D3D_OK, 2);
}
static void tx_AddDirtyRect(void) { ((tex_t *)HOST(A32(0)))->dirty = ~0u; RET(D3D_OK, 2); }

static void sf_Release(void) {
    uint32_t o = A32(0), n = MEM32(o + 4);
    surf_t *s = (surf_t *)HOST(o);
    if (n) MEM32(o + 4) = --n;
    if (s->parent) {                                      /* a texture level: it releases its texture */
        uint32_t tn = MEM32(s->parent->self + 4);
        if (tn) MEM32(s->parent->self + 4) = tn - 1;
        RET(n, 1);
    }
    if (!n && s->kind != SURF_BACK && s->kind != SURF_DEPTH) {
        stage_free(&s->st, s->mem);
        if (s->mem) res_free(s->mem);
        g_r->surf_free(s);
        hle_free(s);
        hle_com_free(o);
    } else if (!n && s->kind == SURF_DEPTH && g_dev && s != g_dev->depth) {
        if (g_dev->zs == s) g_dev->zs = NULL;
        g_r->surf_free(s);
        hle_free(s);
        hle_com_free(o);
    }
    RET(n, 1);
}
static void sf_GetContainer(void) {                  /* (this, iid, out) */
    surf_t *s = (surf_t *)HOST(A32(0));
    if (s->parent) { MEM32(s->parent->self + 4)++; MEM32(A32(2)) = s->parent->self; RET(D3D_OK, 3); }
    if (g_dev) { MEM32(g_dev->self + 4)++; MEM32(A32(2)) = g_dev->self; RET(D3D_OK, 3); }
    RET(E_NOINTERFACE, 3);
}
static void sf_GetDesc(void) { surf_t *s = (surf_t *)HOST(A32(0)); put_desc(A32(1), s, 1); RET(D3D_OK, 2); }
static void sf_LockRect(void) {                      /* (this, out, rect, flags) */
    surf_t *s = (surf_t *)HOST(A32(0));
    if (s->kind == SURF_BACK) {                        /* the back buffer: read back for the game, each lock */
        if (!s->mem) s->mem = (uint8_t *)res_alloc(s->size);
        if (!s->mem) RET(D3DERR_OUTOFVIDEOMEMORY, 4);
        if (g_dev) g_r->read_back(g_dev, s);
    }
    if (!s->mem) RET(D3DERR_INVALIDCALL, 4);
    lock_out(s, A32(1), A32(2), A32(3));
    RET(D3D_OK, 4);
}
static void sf_UnlockRect(void) {
    surf_t *s = (surf_t *)HOST(A32(0));
    stage_unlock(&s->st, s->mem);
    mark_dirty(s);
    RET(D3D_OK, 1);
}

static void bf_Release(void) {
    uint32_t o = A32(0), n = MEM32(o + 4);
    if (n) MEM32(o + 4) = --n;
    if (!n) {
        vb_t *b = (vb_t *)HOST(o);
        if (g_dev && g_dev->s.stream_vb == o) g_dev->s.stream_vb = 0;
        if (g_dev && g_dev->s.ib == o) g_dev->s.ib = 0;
        stage_free(&b->st, b->mem);
        g_r->buf_free(b);
        res_free(b->mem);
        hle_free(b);
        hle_com_free(o);
    }
    RET(n, 1);
}
/* vb_t and ib_t share their layout up to st: one Lock and Unlock for both */
static void bf_Lock(void) {                          /* (this, offset, size, out, flags) */
    vb_t *b = (vb_t *)HOST(A32(0));
    uint32_t off = A32(1), size = A32(2);
    if (off > b->size) off = b->size;
    if (!size || size > b->size - off) size = b->size - off;     /* 0: to the end */
    if (b->locked && !(b->lk_flags & 0x10u)) buf_dirty(b, b->lk_off, b->lk_off + b->lk_size);   /* locked again */
    uint32_t va = stage_lock(&b->st, b->mem, b->size, off, size, A32(4), (b->usage & 0x200u) != 0);   /* D3DUSAGE_DYNAMIC: kept */
    b->lk_off = off, b->lk_size = size, b->lk_flags = A32(4), b->locked = va != 0;
    if (A32(4) & 0x2000u) b->discard = 1;              /* D3DLOCK_DISCARD */
    else if (!(A32(4) & 0x1010u)) b->sync = 1;         /* not NOOVERWRITE (nor READONLY): may touch what is drawn */
    MEM32(A32(3)) = va;
    RET(va ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY, 5);
}
static void bf_Unlock(void) {
    vb_t *b = (vb_t *)HOST(A32(0));
    stage_unlock(&b->st, b->mem);
    if (b->locked && !(b->lk_flags & 0x10u)) buf_dirty(b, b->lk_off, b->lk_off + b->lk_size);   /* not READONLY: the GL copy is behind */
    b->locked = 0;
    RET(D3D_OK, 1);
}
static void vb_GetDesc(void) {
    vb_t *b = (vb_t *)HOST(A32(0));
    uint32_t p = A32(1);
    MEM32(p) = 100, MEM32(p + 4) = 6, MEM32(p + 8) = b->usage, MEM32(p + 12) = b->pool, MEM32(p + 16) = b->size, MEM32(p + 20) = b->fvf;
    RET(D3D_OK, 2);
}
static void ib_GetDesc(void) {
    ib_t *b = (ib_t *)HOST(A32(0));
    uint32_t p = A32(1);
    MEM32(p) = b->fmt, MEM32(p + 4) = 7, MEM32(p + 8) = b->usage, MEM32(p + 12) = b->pool, MEM32(p + 16) = b->size;
    RET(D3D_OK, 2);
}

/* ------------------------------------------------------------------ vtables and the entry point */
static void vtables(void) {
    if (g_vt_d3d) return;
    static const win32hle_shim d3[] = {
        { "IDirect3D8::QueryInterface", d3_QueryInterface }, { "IDirect3D8::AddRef", hle_com_AddRef },
        { "IDirect3D8::Release", d3_Release }, { "IDirect3D8::RegisterSoftwareDevice", ok1 },
        { "IDirect3D8::GetAdapterCount", d3_GetAdapterCount }, { "IDirect3D8::GetAdapterIdentifier", d3_GetAdapterIdentifier },
        { "IDirect3D8::GetAdapterModeCount", d3_GetAdapterModeCount }, { "IDirect3D8::EnumAdapterModes", d3_EnumAdapterModes },
        { "IDirect3D8::GetAdapterDisplayMode", d3_GetAdapterDisplayMode }, { "IDirect3D8::CheckDeviceType", d3_CheckDeviceType },
        { "IDirect3D8::CheckDeviceFormat", d3_CheckDeviceFormat },
        { "IDirect3D8::CheckDeviceMultiSampleType", d3_CheckDeviceMultiSampleType },
        { "IDirect3D8::CheckDepthStencilMatch", d3_CheckDepthStencilMatch }, { "IDirect3D8::GetDeviceCaps", d3_GetDeviceCaps },
        { "IDirect3D8::GetAdapterMonitor", d3_GetAdapterMonitor }, { "IDirect3D8::CreateDevice", d3_CreateDevice }, { 0, 0 } };
    static const win32hle_shim dv[] = {
        { "IDirect3DDevice8::QueryInterface", dv_QueryInterface }, { "IDirect3DDevice8::AddRef", hle_com_AddRef },
        { "IDirect3DDevice8::Release", dv_Release }, { "IDirect3DDevice8::TestCooperativeLevel", dv_TestCooperativeLevel },
        { "IDirect3DDevice8::GetAvailableTextureMem", dv_GetAvailableTextureMem },
        { "IDirect3DDevice8::ResourceManagerDiscardBytes", ok1 }, { "IDirect3DDevice8::GetDirect3D", dv_GetDirect3D },
        { "IDirect3DDevice8::GetDeviceCaps", dv_GetDeviceCaps }, { "IDirect3DDevice8::GetDisplayMode", dv_GetDisplayMode },
        { "IDirect3DDevice8::GetCreationParameters", dv_GetCreationParameters },
        { "IDirect3DDevice8::SetCursorProperties", ok3 }, { "IDirect3DDevice8::SetCursorPosition", dv_SetCursorPosition },
        { "IDirect3DDevice8::ShowCursor", dv_ShowCursor }, { "IDirect3DDevice8::CreateAdditionalSwapChain", dv_CreateAdditionalSwapChain },
        { "IDirect3DDevice8::Reset", dv_Reset }, { "IDirect3DDevice8::Present", dv_Present },
        { "IDirect3DDevice8::GetBackBuffer", dv_GetBackBuffer }, { "IDirect3DDevice8::GetRasterStatus", dv_GetRasterStatus },
        { "IDirect3DDevice8::SetGammaRamp", dv_SetGammaRamp }, { "IDirect3DDevice8::GetGammaRamp", dv_GetGammaRamp },
        { "IDirect3DDevice8::CreateTexture", dv_CreateTexture }, { "IDirect3DDevice8::CreateVolumeTexture", dv_CreateVolumeTexture },
        { "IDirect3DDevice8::CreateCubeTexture", dv_CreateCubeTexture }, { "IDirect3DDevice8::CreateVertexBuffer", dv_CreateVertexBuffer },
        { "IDirect3DDevice8::CreateIndexBuffer", dv_CreateIndexBuffer }, { "IDirect3DDevice8::CreateRenderTarget", dv_CreateRenderTarget },
        { "IDirect3DDevice8::CreateDepthStencilSurface", dv_CreateDepthStencilSurface },
        { "IDirect3DDevice8::CreateImageSurface", dv_CreateImageSurface }, { "IDirect3DDevice8::CopyRects", dv_CopyRects },
        { "IDirect3DDevice8::UpdateTexture", dv_UpdateTexture }, { "IDirect3DDevice8::GetFrontBuffer", dv_GetFrontBuffer },
        { "IDirect3DDevice8::SetRenderTarget", dv_SetRenderTarget }, { "IDirect3DDevice8::GetRenderTarget", dv_GetRenderTarget },
        { "IDirect3DDevice8::GetDepthStencilSurface", dv_GetDepthStencilSurface },
        { "IDirect3DDevice8::BeginScene", dv_BeginScene }, { "IDirect3DDevice8::EndScene", dv_EndScene },
        { "IDirect3DDevice8::Clear", dv_Clear }, { "IDirect3DDevice8::SetTransform", dv_SetTransform },
        { "IDirect3DDevice8::GetTransform", dv_GetTransform }, { "IDirect3DDevice8::MultiplyTransform", dv_MultiplyTransform },
        { "IDirect3DDevice8::SetViewport", dv_SetViewport }, { "IDirect3DDevice8::GetViewport", dv_GetViewport },
        { "IDirect3DDevice8::SetMaterial", dv_SetMaterial }, { "IDirect3DDevice8::GetMaterial", dv_GetMaterial },
        { "IDirect3DDevice8::SetLight", dv_SetLight }, { "IDirect3DDevice8::GetLight", dv_GetLight },
        { "IDirect3DDevice8::LightEnable", dv_LightEnable }, { "IDirect3DDevice8::GetLightEnable", dv_GetLightEnable },
        { "IDirect3DDevice8::SetClipPlane", ok2 }, { "IDirect3DDevice8::GetClipPlane", ok2 },
        { "IDirect3DDevice8::SetRenderState", dv_SetRenderState }, { "IDirect3DDevice8::GetRenderState", dv_GetRenderState },
        { "IDirect3DDevice8::BeginStateBlock", dv_BeginStateBlock }, { "IDirect3DDevice8::EndStateBlock", dv_EndStateBlock },
        { "IDirect3DDevice8::ApplyStateBlock", dv_ApplyStateBlock }, { "IDirect3DDevice8::CaptureStateBlock", dv_CaptureStateBlock },
        { "IDirect3DDevice8::DeleteStateBlock", dv_DeleteStateBlock }, { "IDirect3DDevice8::CreateStateBlock", dv_CreateStateBlock },
        { "IDirect3DDevice8::SetClipStatus", ok1 }, { "IDirect3DDevice8::GetClipStatus", ok1 },
        { "IDirect3DDevice8::GetTexture", dv_GetTexture }, { "IDirect3DDevice8::SetTexture", dv_SetTexture },
        { "IDirect3DDevice8::GetTextureStageState", dv_GetTextureStageState },
        { "IDirect3DDevice8::SetTextureStageState", dv_SetTextureStageState },
        { "IDirect3DDevice8::ValidateDevice", dv_ValidateDevice }, { "IDirect3DDevice8::GetInfo", dv_GetInfo },
        { "IDirect3DDevice8::SetPaletteEntries", ok2 }, { "IDirect3DDevice8::GetPaletteEntries", ok2 },
        { "IDirect3DDevice8::SetCurrentTexturePalette", ok1 },
        { "IDirect3DDevice8::GetCurrentTexturePalette", dv_GetCurrentTexturePalette },
        { "IDirect3DDevice8::DrawPrimitive", dv_DrawPrimitive }, { "IDirect3DDevice8::DrawIndexedPrimitive", dv_DrawIndexedPrimitive },
        { "IDirect3DDevice8::DrawPrimitiveUP", dv_DrawPrimitiveUP },
        { "IDirect3DDevice8::DrawIndexedPrimitiveUP", dv_DrawIndexedPrimitiveUP },
        { "IDirect3DDevice8::ProcessVertices", dv_ProcessVertices }, { "IDirect3DDevice8::CreateVertexShader", dv_CreateVertexShader },
        { "IDirect3DDevice8::SetVertexShader", dv_SetVertexShader }, { "IDirect3DDevice8::GetVertexShader", dv_GetVertexShader },
        { "IDirect3DDevice8::DeleteVertexShader", ok1 }, { "IDirect3DDevice8::SetVertexShaderConstant", ok3 },
        { "IDirect3DDevice8::GetVertexShaderConstant", ok3 }, { "IDirect3DDevice8::GetVertexShaderDeclaration", dv_notavail3 },
        { "IDirect3DDevice8::GetVertexShaderFunction", dv_notavail3 }, { "IDirect3DDevice8::SetStreamSource", dv_SetStreamSource },
        { "IDirect3DDevice8::GetStreamSource", dv_GetStreamSource }, { "IDirect3DDevice8::SetIndices", dv_SetIndices },
        { "IDirect3DDevice8::GetIndices", dv_GetIndices }, { "IDirect3DDevice8::CreatePixelShader", dv_CreatePixelShader },
        { "IDirect3DDevice8::SetPixelShader", dv_SetPixelShader }, { "IDirect3DDevice8::GetPixelShader", dv_GetPixelShader },
        { "IDirect3DDevice8::DeletePixelShader", ok1 }, { "IDirect3DDevice8::SetPixelShaderConstant", ok3 },
        { "IDirect3DDevice8::GetPixelShaderConstant", ok3 }, { "IDirect3DDevice8::GetPixelShaderFunction", dv_notavail3 },
        { "IDirect3DDevice8::DrawRectPatch", dv_notavail3 }, { "IDirect3DDevice8::DrawTriPatch", dv_notavail3 },
        { "IDirect3DDevice8::DeletePatch", dv_notavail1 }, { 0, 0 } };
    static const win32hle_shim tx[] = {
        { "IDirect3DTexture8::QueryInterface", hle_com_QueryInterface }, { "IDirect3DTexture8::AddRef", hle_com_AddRef },
        { "IDirect3DTexture8::Release", tx_Release }, { "IDirect3DTexture8::GetDevice", res_GetDevice },
        { "IDirect3DTexture8::SetPrivateData", res_SetPrivateData }, { "IDirect3DTexture8::GetPrivateData", res_GetPrivateData },
        { "IDirect3DTexture8::FreePrivateData", res_FreePrivateData }, { "IDirect3DTexture8::SetPriority", res_SetPriority },
        { "IDirect3DTexture8::GetPriority", res_GetPriority }, { "IDirect3DTexture8::PreLoad", res_PreLoad },
        { "IDirect3DTexture8::GetType", tex_GetType }, { "IDirect3DTexture8::SetLOD", tx_SetLOD },
        { "IDirect3DTexture8::GetLOD", tx_GetLOD }, { "IDirect3DTexture8::GetLevelCount", tx_GetLevelCount },
        { "IDirect3DTexture8::GetLevelDesc", tx_GetLevelDesc }, { "IDirect3DTexture8::GetSurfaceLevel", tx_GetSurfaceLevel },
        { "IDirect3DTexture8::LockRect", tx_LockRect }, { "IDirect3DTexture8::UnlockRect", tx_UnlockRect },
        { "IDirect3DTexture8::AddDirtyRect", tx_AddDirtyRect }, { 0, 0 } };
    static const win32hle_shim sf[] = {
        { "IDirect3DSurface8::QueryInterface", hle_com_QueryInterface }, { "IDirect3DSurface8::AddRef", hle_com_AddRef },
        { "IDirect3DSurface8::Release", sf_Release }, { "IDirect3DSurface8::GetDevice", res_GetDevice },
        { "IDirect3DSurface8::SetPrivateData", res_SetPrivateData }, { "IDirect3DSurface8::GetPrivateData", res_GetPrivateData },
        { "IDirect3DSurface8::FreePrivateData", res_FreePrivateData }, { "IDirect3DSurface8::GetContainer", sf_GetContainer },
        { "IDirect3DSurface8::GetDesc", sf_GetDesc }, { "IDirect3DSurface8::LockRect", sf_LockRect },
        { "IDirect3DSurface8::UnlockRect", sf_UnlockRect }, { 0, 0 } };
    static const win32hle_shim vb[] = {
        { "IDirect3DVertexBuffer8::QueryInterface", hle_com_QueryInterface }, { "IDirect3DVertexBuffer8::AddRef", hle_com_AddRef },
        { "IDirect3DVertexBuffer8::Release", bf_Release }, { "IDirect3DVertexBuffer8::GetDevice", res_GetDevice },
        { "IDirect3DVertexBuffer8::SetPrivateData", res_SetPrivateData }, { "IDirect3DVertexBuffer8::GetPrivateData", res_GetPrivateData },
        { "IDirect3DVertexBuffer8::FreePrivateData", res_FreePrivateData }, { "IDirect3DVertexBuffer8::SetPriority", res_SetPriority },
        { "IDirect3DVertexBuffer8::GetPriority", res_GetPriority }, { "IDirect3DVertexBuffer8::PreLoad", res_PreLoad },
        { "IDirect3DVertexBuffer8::GetType", vb_GetType }, { "IDirect3DVertexBuffer8::Lock", bf_Lock },
        { "IDirect3DVertexBuffer8::Unlock", bf_Unlock }, { "IDirect3DVertexBuffer8::GetDesc", vb_GetDesc }, { 0, 0 } };
    static const win32hle_shim ib[] = {
        { "IDirect3DIndexBuffer8::QueryInterface", hle_com_QueryInterface }, { "IDirect3DIndexBuffer8::AddRef", hle_com_AddRef },
        { "IDirect3DIndexBuffer8::Release", bf_Release }, { "IDirect3DIndexBuffer8::GetDevice", res_GetDevice },
        { "IDirect3DIndexBuffer8::SetPrivateData", res_SetPrivateData }, { "IDirect3DIndexBuffer8::GetPrivateData", res_GetPrivateData },
        { "IDirect3DIndexBuffer8::FreePrivateData", res_FreePrivateData }, { "IDirect3DIndexBuffer8::SetPriority", res_SetPriority },
        { "IDirect3DIndexBuffer8::GetPriority", res_GetPriority }, { "IDirect3DIndexBuffer8::PreLoad", res_PreLoad },
        { "IDirect3DIndexBuffer8::GetType", ib_GetType }, { "IDirect3DIndexBuffer8::Lock", bf_Lock },
        { "IDirect3DIndexBuffer8::Unlock", bf_Unlock }, { "IDirect3DIndexBuffer8::GetDesc", ib_GetDesc }, { 0, 0 } };
    g_vt_d3d = hle_com_vtable(d3);
    g_vt_dev = hle_com_vtable(dv);
    g_vt_tex = hle_com_vtable(tx);
    g_vt_surf = hle_com_vtable(sf);
    g_vt_vb = hle_com_vtable(vb);
    g_vt_ib = hle_com_vtable(ib);
    (void)ok0, (void)ok4, (void)ok5, (void)ok_n, (void)dv_notavail2;
}

static void Direct3DCreate8(void) {                  /* (sdk version) */
    vtables();
    if (!g_d3d) g_d3d = hle_com_new(g_vt_d3d, 12);
    else MEM32(g_d3d + 4)++;
    fprintf(stderr, "[d3d8] Direct3DCreate8(%u)\n", A32(0));
    RET(g_d3d, 1);
}

const win32hle_shim ofp_d3d8_shims[] = {
    { "Direct3DCreate8", Direct3DCreate8 },
    { 0, 0 }
};

