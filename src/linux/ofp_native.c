/*
 * ofp_native.c - hand-written native versions of lifted functions that are
 * too slow as lifted x87 code. Each replaces sub_XXXXXXXX through the
 * linker (-Wl,--wrap=sub_XXXXXXXX, CMakeLists.txt): every direct call from
 * another file and the dispatch table reach __wrap_, the lifted body stays
 * reachable as __real_. OFP_NATIVE=0 turns them off (A/B checks).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdio.h>

#include "recomp_types.h"
#include "win32hle.h"
#include "ofp.h"

void sub_00540B55(void);
void sub_0061048E(void);
void sub_00611460(void);
void __real_sub_00618794(void);

/* 1 native, 0 lifted, 2 ("check"): both, results compared in the perf log */
static int native_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("OFP_NATIVE");
        on = !e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "check") ? 2 : 1;
    }
    return on;
}

/* native_on() for one function: 0 when OFP_NATIVE_OFF lists it (as
 * "00618959,005B6637"), for an A/B of that one */
static int native_on_for(const char *id) {
    const char *off = getenv("OFP_NATIVE_OFF");
    return off && strstr(off, id) ? 0 : native_on();
}
#define NATIVE_MODE(id) ({ static int m_ = -1; if (m_ < 0) m_ = native_on_for(id); m_; })

/* OFP_TRACE_HASH=1: a running hash of what the vertex and face functions get
 * and return, logged every 50 000 calls. Two runs that build the same terrain
 * the same way log the same hashes (the loading at a mission's start builds in
 * a fixed order): a check of a translated region against the lifted code. */
static int trace_on(void) { static int on = -1; if (on < 0) on = ofp_env_int("OFP_TRACE_HASH", 0); return on; }
static uint64_t g_th = 1469598103934665603ull, g_tn;
static void th_add(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) g_th = (g_th ^ b[i]) * 1099511628211ull;
}
static void th_guest(uint32_t va, uint32_t n) { th_add(hle_ptr(va), n); }
/* OFP_TRACE_HASH=N (N > 1): the first N calls are logged one by one too */
static int trace_verbose(void) { return trace_on() > 1 && g_tn < (uint64_t)trace_on(); }
static void th_end(void) {
    ++g_tn;
    if (g_tn % 50000 == 0 || (trace_on() > 1 && g_tn <= (uint64_t)trace_on()))
        perf_note("trace %llu: %016llx", (unsigned long long)g_tn, (unsigned long long)g_th);
}

/* hot regions of lifted functions translated by tools/region_c.py: on unless
 * OFP_NATIVE=0 or OFP_NATIVE_OFF names the region's start; OFP_NATIVE=check
 * also checks esp after each call they make */
int ofp_region_check;
int ofp_region_on(uint32_t start) {
#ifndef RECOMP_X87_RING
    (void)start;
    return 0;                                   /* the regions are stubs (ofp_region.h) */
#endif
    static uint32_t va[8]; static int on[8], n;
    for (int i = 0; i < n; i++) if (va[i] == start) return on[i];
    char id[16];
    snprintf(id, sizeof id, "%08X", start);
    int m = native_on_for(id);
    ofp_region_check = native_on() == 2;
    if (n < 8) va[n] = start, on[n] = m != 0, n++;
    return m != 0;
}
void region_bad(uint32_t call_va, uint32_t esp_off) {
    static unsigned bad;
    if (bad++ < 20) perf_note("region: the call at %08X left esp %d bytes off", call_va, (int)esp_off);
}

/* a thiscall to a lifted method taking one stack argument (ret 4) */
static void call_this1(void (*fn)(void), uint32_t self, uint32_t arg) {
    uint32_t caller = g_cur_func, ecx = g_ecx, edx = g_edx;
    g_esp -= 4, MEM32(g_esp) = arg;
    g_esp -= 4, MEM32(g_esp) = RECOMP_RETADDR;
    g_ecx = self;
    fn();
    g_cur_func = caller;
    g_ecx = ecx, g_edx = edx;              /* scratch for the callee; ours again */
}

/* The game's AutoArray Resize(n) (sub_00540B55, sub_00611460, sub_0061048E:
 * the same code for three element types): within the capacity (+8) it only
 * sets the count (+0xC); the lifted call is made only when it must grow. */
static void resize(void (*fn)(void), uint32_t arr, uint32_t n) {
    if ((int32_t)n <= (int32_t)MEM32(arr + 8)) { MEM32(arr + 0xC) = n; return; }
    call_this1(fn, arr, n);
}

static inline float F(uint32_t a) { float f; uint32_t v = MEM32(a); memcpy(&f, &v, 4); return f; }

/*
 * sub_00618794: a model's "add vertex, or reuse an equal one" (thiscall, ret 0x24)
 *   (pos*, normal*, clip, u, v, group*, group_id, pos_eps, normal_eps) -> index
 * Looks, in order, for the first vertex i with the same clip flags (and the
 * same group id when a group is given), position within pos_eps, normal
 * within normal_eps and u, v within 0.005; if none, appends one. The lifted
 * search is quadratic x87 code: tens of seconds for a big model.
 * The arithmetic is the original's: differences and squares in double
 * (MSVC's 53-bit x87), dy rounded through a float as the code stores it.
 */
static unsigned g_grid_calls;                  /* searches that went through the grid (check mode reports it) */
/* the search alone: the index of the first equal vertex, or -1 */
/* The test of vertex i against the one being added: the original's, exactly. */
typedef struct {
    const uint32_t *clips; const float *P, *N, *T; const int16_t *G;
    uint32_t clip, gid; float px, py, pz, nx, ny, nz, u, v; double pe2, ne2, uv_eps;
} vq_t;
static int vertex_equal(const vq_t *q, int32_t i) {
    if (q->G && (int32_t)q->G[i] != (int32_t)q->gid) return 0;
    if (q->clips[i] != q->clip) return 0;
    const float *p = q->P + 3 * i;
    double dx = (double)p[0] - q->px, dy = (double)(float)((double)p[1] - q->py), dz = (double)p[2] - q->pz;
    if (dx * dx + dy * dy + dz * dz > q->pe2) return 0;      /* NaN: a match, as fcomp's unordered */
    const float *m = q->N + 3 * i;
    double ex = (double)m[0] - q->nx, ey = (double)(float)((double)m[1] - q->ny), ez = (double)m[2] - q->nz;
    if (ex * ex + ey * ey + ez * ez > q->ne2) return 0;
    if (fabs((double)q->u - q->T[2 * i]) > q->uv_eps) return 0;
    if (fabs((double)q->v - q->T[2 * i + 1]) > q->uv_eps) return 0;
    return 1;
}

/* A grid of the model's positions (cells at least pos_eps wide), so a search
 * looks at the 27 cells around the new vertex, not at every vertex: an equal
 * vertex is within pos_eps on each axis, so in one of them. The answer is the
 * lowest such index, as the linear search's. Vertices with a coordinate that
 * is not finite (a NaN distance matches anything) are always looked at. A few
 * models at a time (a terrain segment builds several, in turns): a model is
 * indexed again when its count went down or the cell size no longer fits, and
 * the least recently used grid goes to a model that has none. */
typedef struct {
    uint32_t self; int32_t n; double cell;
    float v0[3], vl[3];                         /* vertex 0 and the last one indexed: the same model? */
    int32_t *head, *next, *odd; uint32_t nb, cap, nodd;
    uint64_t used;
} vgrid_t;
#define VG_SLOTS 8
static vgrid_t g_vgs[VG_SLOTS], *g_vgp = &g_vgs[0];
static uint64_t g_vg_clock;
#define g_vg (*g_vgp)
/* the grid of this model, if it has one (it becomes the current one) */
static int vg_find(uint32_t self) {
    if (g_vg.self == self && g_vg.head) { g_vg.used = ++g_vg_clock; return 1; }
    for (int k = 0; k < VG_SLOTS; k++)
        if (g_vgs[k].self == self && g_vgs[k].head) { g_vgp = &g_vgs[k]; g_vg.used = ++g_vg_clock; return 1; }
    return 0;
}
/* a grid for a model that has none: the least recently used */
static void vg_take(void) {
    int v = 0;
    for (int k = 1; k < VG_SLOTS; k++) if (g_vgs[k].used < g_vgs[v].used) v = k;
    g_vgp = &g_vgs[v];
    g_vg.self = 0, g_vg.n = 0, g_vg.used = ++g_vg_clock;
}
static void cell_of(const float *p, double cell, int64_t c[3]) { for (int k = 0; k < 3; k++) c[k] = (int64_t)floor((double)p[k] / cell); }
static uint32_t cell_hash(const int64_t c[3], uint32_t nb) {
    uint64_t h = (uint64_t)c[0] * 0x9E3779B97F4A7C15ull ^ (uint64_t)c[1] * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)c[2] * 0x165667B19E3779F9ull;
    return (uint32_t)(h >> 32) & (nb - 1);
}
static void vg_add(const float *P, int32_t i) {
    const float *p = P + 3 * i;
    if (i == 0) memcpy(g_vg.v0, p, sizeof g_vg.v0);
    memcpy(g_vg.vl, p, sizeof g_vg.vl);
    if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) {
        g_vg.odd = (int32_t *)realloc(g_vg.odd, sizeof(int32_t) * (g_vg.nodd + 1));
        g_vg.odd[g_vg.nodd++] = i;
        return;
    }
    if ((uint32_t)i >= g_vg.cap) {
        g_vg.cap = g_vg.cap ? g_vg.cap * 2 : 4096;
        while ((uint32_t)i >= g_vg.cap) g_vg.cap *= 2;
        g_vg.next = (int32_t *)realloc(g_vg.next, sizeof(int32_t) * g_vg.cap);
    }
    int64_t c[3];
    cell_of(p, g_vg.cell, c);
    uint32_t b = cell_hash(c, g_vg.nb);
    g_vg.next[i] = g_vg.head[b], g_vg.head[b] = i;
}
static void vg_build(uint32_t self, const float *P, int32_t n, double cell) {
    g_vg.self = self, g_vg.cell = cell, g_vg.nodd = 0, g_vg.n = 0;
    uint32_t nb = 1u << 16;                     /* a terrain segment's models grow to ~20 000: one build, not five */
    while (nb < (uint32_t)n * 2u && nb < (1u << 24)) nb <<= 1;
    if (nb != g_vg.nb) free(g_vg.head), g_vg.head = (int32_t *)malloc(sizeof(int32_t) * nb), g_vg.nb = nb;
    memset(g_vg.head, 0xFF, sizeof(int32_t) * nb);
    for (int32_t i = 0; i < n; i++) vg_add(P, i);
    g_vg.n = n;
}

static unsigned g_grid_calls;                  /* searches that went through the grid (check mode reports it) */
/* the search alone: the index of the first equal vertex, or -1 */
static int32_t find_vertex_618794(uint32_t self, uint32_t sp) {
    uint32_t pos = MEM32(sp + 4), nrm = MEM32(sp + 8);
    vq_t q;
    q.clip = MEM32(sp + 12), q.u = F(sp + 16), q.v = F(sp + 20);
    uint32_t grp = MEM32(sp + 24);
    q.gid = MEM32(sp + 28);
    float pe = F(sp + 32), ne = F(sp + 36);
    q.pe2 = (double)pe * pe, q.ne2 = (double)ne * ne;
    q.uv_eps = MEMD(0x0071B840u);
    int32_t n = (int32_t)MEM32(self + 0x58);
    q.clips = (const uint32_t *)hle_ptr(MEM32(self + 0x20));
    q.P = (const float *)hle_ptr(MEM32(self + 0x50));
    q.N = (const float *)hle_ptr(MEM32(self + 0x60));
    q.T = (const float *)hle_ptr(MEM32(self + 0x70));
    q.G = grp ? (const int16_t *)hle_ptr(MEM32(grp + 4)) : NULL;
    q.px = F(pos), q.py = F(pos + 4), q.pz = F(pos + 8);
    q.nx = F(nrm), q.ny = F(nrm + 4), q.nz = F(nrm + 8);

    /* small models, a position that is not finite, or an odd tolerance: every vertex */
    float qp[3] = { q.px, q.py, q.pz };
    double cell = (double)pe * 1.01 + 1e-6;
    if (n < 256 || !isfinite(q.px) || !isfinite(q.py) || !isfinite(q.pz) || !(q.pe2 >= 0 && q.pe2 < 1e10)) {
        for (int32_t i = 0; i < n; i++) if (vertex_equal(&q, i)) return i;
        return -1;
    }
    if (!vg_find(self)) vg_take();
    if (g_vg.self != self || n < g_vg.n || cell > g_vg.cell || g_vg.cell > cell * 64 ||
        (g_vg.n > 0 && (memcmp(g_vg.v0, q.P, sizeof g_vg.v0) || memcmp(g_vg.vl, q.P + 3 * (g_vg.n - 1), sizeof g_vg.vl))))
        vg_build(self, q.P, n, cell);
    else {
        if ((uint32_t)n * 2u > g_vg.nb && g_vg.nb < (1u << 24)) vg_build(self, q.P, n, g_vg.cell);
        for (int32_t i = g_vg.n; i < n; i++) vg_add(q.P, i);  /* added by the game's other code */
        g_vg.n = n;
    }
    g_grid_calls++;
    int32_t best = -1;
    for (uint32_t k = 0; k < g_vg.nodd; k++)
        if ((best < 0 || g_vg.odd[k] < best) && g_vg.odd[k] < n && vertex_equal(&q, g_vg.odd[k])) best = g_vg.odd[k];
    int64_t c[3];
    cell_of(qp, g_vg.cell, c);
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                int64_t cc[3] = { c[0] + dx, c[1] + dy, c[2] + dz };
                for (int32_t i = g_vg.head[cell_hash(cc, g_vg.nb)]; i >= 0; i = g_vg.next[i])
                    if ((best < 0 || i < best) && i < n && vertex_equal(&q, i)) best = i;
            }
    return best;
}

/* the vertex the native path just added (index n): into the grid too */
static void vg_added(uint32_t self, int32_t n) {
    if (!vg_find(self) || g_vg.n != n) return;              /* not a model indexed: it is indexed when searched */
    const float *P = (const float *)hle_ptr(MEM32(self + 0x50));
    vg_add(P, n);
    g_vg.n = n + 1;
}

/* A new vertex at index n of a model: its four arrays grown to n + 1 and the
 * vertex stored (the end of sub_00618794 and sub_00618959; their second
 * Resize of +0x6C, to the same count, does nothing). uv: its u, v. */
static void append_vertex(uint32_t self, int32_t n, uint32_t pos, uint32_t nrm, uint32_t clip, uint32_t uv) {
    uint32_t m = (uint32_t)n + 1;
    resize(sub_00540B55, self + 0x4C, m);
    resize(sub_00540B55, self + 0x5C, m);
    resize(sub_00611460, self + 0x6C, m);
    resize(sub_0061048E, self + 0x1C, m);
    MEM32(MEM32(self + 0x20) + 4u * (uint32_t)n) = clip;
    uint32_t pd = MEM32(self + 0x50) + 12u * (uint32_t)n, nd = MEM32(self + 0x60) + 12u * (uint32_t)n;
    MEM32(pd) = MEM32(pos), MEM32(pd + 4) = MEM32(pos + 4), MEM32(pd + 8) = MEM32(pos + 8);
    MEM32(nd) = MEM32(nrm), MEM32(nd + 4) = MEM32(nrm + 4), MEM32(nd + 8) = MEM32(nrm + 8);
    uint32_t td = MEM32(self + 0x70) + 8u * (uint32_t)n;
    MEM32(td) = MEM32(uv), MEM32(td + 4) = MEM32(uv + 4);
}

static void w618794(void);
void __wrap_sub_00618794(void) {
    uint32_t sp = g_esp;
    w618794();
    if (trace_on()) {
        uint32_t eax = g_eax;
        th_guest(MEM32(sp + 4), 12), th_guest(MEM32(sp + 8), 12), th_guest(sp + 12, 12), th_guest(sp + 28, 12);
        th_add(&eax, 4);
        if (trace_verbose()) {
            uint32_t p = MEM32(sp + 4), q = MEM32(sp + 8);
            perf_note("  618794 pos %08X %08X %08X nrm %08X %08X %08X clip %08X uv %08X %08X g %08X %08X eps %08X %08X -> %u",
                      MEM32(p), MEM32(p + 4), MEM32(p + 8), MEM32(q), MEM32(q + 4), MEM32(q + 8), MEM32(sp + 12),
                      MEM32(sp + 16), MEM32(sp + 20), MEM32(sp + 24), MEM32(sp + 28), MEM32(sp + 32), MEM32(sp + 36), eax);
        }
        th_end();
    }
}
static void w618794(void) {
    int mode = NATIVE_MODE("00618794");
    if (!mode) { __real_sub_00618794(); return; }
    uint32_t self = g_ecx, sp = g_esp;
    if (mode == 2) {
        static unsigned calls, bad;
        int32_t want = find_vertex_618794(self, sp), n = (int32_t)MEM32(self + 0x58);
        __real_sub_00618794();
        calls++;
        if ((int32_t)g_eax != (want < 0 ? n : want) && bad++ < 20)
            perf_note("check sub_00618794: lifted %d, native %d (n %d)", (int)g_eax, (int)want, (int)n);
        if (calls % 20000 == 0) perf_note("check sub_00618794: %u calls (%u through the grid), %u differ", calls, g_grid_calls, bad);
        return;
    }
    g_cur_func = 0x00618794u;              /* for the profiler; the caller restores its own */
    int32_t i = find_vertex_618794(self, sp);
    if (i >= 0) {
        g_eax = (uint32_t)i;
        g_esp = sp + 4 + 0x24;
        return;
    }
    int32_t n = (int32_t)MEM32(self + 0x58);              /* none: a new vertex */
    append_vertex(self, n, MEM32(sp + 4), MEM32(sp + 8), MEM32(sp + 12), sp + 16);
    vg_added(self, n);
    g_eax = (uint32_t)n;
    g_esp = sp + 4 + 0x24;
}

/*
 * sub_00618959: the landscape's "add vertex, or reuse one of these"
 * (thiscall, ret 0x28), from the segment builder sub_00671950
 *   (pos*, normal*, clip, u, v, cand*, ncand, found*, ...) -> index
 * Looks through the ncand candidate indices for the first vertex with the
 * same clip flags and u, v within the same 0.005 (fsub, fabs, fcomp: double
 * arithmetic, NaN counts as close); *found = 1 and its index. If none, appends
 * the vertex as sub_00618794 does, *found = 0 and the new index. A fifth of
 * the segment builder's time as lifted x87 code (flight bench, the Mint PC's view
 * distance and terrain grid).
 */
void __real_sub_00618959(void);
static int32_t find_vertex_618959(uint32_t self, uint32_t sp) {
    uint32_t clip = MEM32(sp + 12);
    double u = F(sp + 16), v = F(sp + 20), eps = MEMD(0x0071B840u);
    int32_t nc = (int32_t)MEM32(sp + 28);
    uint32_t cand = MEM32(sp + 24);
    const uint32_t *clips = (const uint32_t *)hle_ptr(MEM32(self + 0x20));
    const float *T = (const float *)hle_ptr(MEM32(self + 0x70));
    for (int32_t k = 0; k < nc; k++) {
        uint32_t i = MEM32(cand + 4u * (uint32_t)k);
        if (clips[i] != clip) continue;
        if (fabs(u - T[2 * i]) > eps) continue;
        if (fabs(v - T[2 * i + 1]) > eps) continue;
        return (int32_t)i;
    }
    return -1;
}
static void trace_618959(uint32_t sp, uint32_t eax) {
    th_guest(MEM32(sp + 4), 12), th_guest(MEM32(sp + 8), 12), th_guest(sp + 12, 12);
    uint32_t nc = MEM32(sp + 28);
    th_add(&nc, 4);
    if ((int32_t)nc > 0) th_guest(MEM32(sp + 24), 4u * nc);
    th_add(&eax, 4), th_guest(MEM32(sp + 32), 1);
    if (trace_verbose()) {
        uint32_t p = MEM32(sp + 4), q = MEM32(sp + 8);
        perf_note("  618959 pos %08X %08X %08X nrm %08X %08X %08X clip %08X uv %08X %08X nc %u -> %u found %u",
                  MEM32(p), MEM32(p + 4), MEM32(p + 8), MEM32(q), MEM32(q + 4), MEM32(q + 8),
                  MEM32(sp + 12), MEM32(sp + 16), MEM32(sp + 20), nc, eax, MEM8(MEM32(sp + 32)));
    }
    th_end();
}
static void w618959(void);
void __wrap_sub_00618959(void) {
    uint32_t sp = g_esp;
    w618959();
    if (trace_on()) trace_618959(sp, g_eax);    /* the arguments are still there */
}
static void w618959(void) {
    int mode = NATIVE_MODE("00618959");
    if (!mode) { __real_sub_00618959(); return; }
    uint32_t self = g_ecx, sp = g_esp, found = MEM32(sp + 32);
    if (mode == 2) {
        static unsigned calls, bad;
        int32_t want = find_vertex_618959(self, sp), n = (int32_t)MEM32(self + 0x58);
        __real_sub_00618959();
        calls++;
        if (((int32_t)g_eax != (want < 0 ? n : want) || MEM8(found) != (want >= 0)) && bad++ < 20)
            perf_note("check sub_00618959: lifted %d (found %u), native %d (n %d)", (int)g_eax, MEM8(found), (int)want, (int)n);
        if (calls % 200000 == 0) perf_note("check sub_00618959: %u calls, %u differ", calls, bad);
        return;
    }
    g_cur_func = 0x00618959u;              /* for the profiler; the caller restores its own */
    int32_t i = find_vertex_618959(self, sp);
    if (i >= 0) {
        MEM8(found) = 1;
        g_eax = (uint32_t)i;
        g_esp = sp + 4 + 0x28;
        return;
    }
    int32_t n = (int32_t)MEM32(self + 0x58);
    append_vertex(self, n, MEM32(sp + 4), MEM32(sp + 8), MEM32(sp + 12), sp + 16);
    MEM8(found) = 0;
    g_eax = (uint32_t)n;
    g_esp = sp + 4 + 0x28;
}

/*
 * sub_005B6637: a model's face list, "append this face" (thiscall, ret 4)
 *   (face*) -> its byte offset in the list
 * A face is 10 bytes and then its n vertex indices (int16 n at +8, 2 bytes
 * each). The list is a byte AutoArray (data +4, capacity +8, count +0xC; Resize
 * sub_005B668E, grown by sub_005B6701) and +0x10 counts the faces.
 */
void sub_005B6701(void);
void __real_sub_005B6637(void);
static void w5B6637(void);
void __wrap_sub_005B6637(void) {
    uint32_t sp = g_esp;
    w5B6637();
    if (trace_on()) {
        uint32_t face = MEM32(sp + 4), eax = g_eax;
        th_guest(face, 10u + 2u * (uint32_t)(int32_t)(int16_t)MEM16(face + 8)), th_add(&eax, 4);
        if (trace_verbose()) {
            char hx[96]; int n = 10 + 2 * (int16_t)MEM16(face + 8), k = 0;
            for (int j = 0; j < n && j < 40; j++) k += snprintf(hx + k, sizeof hx - (size_t)k, "%02X", MEM8(face + (uint32_t)j));
            perf_note("  5B6637 face %s -> %u", hx, eax);
        }
        th_end();
    }
}
static void w5B6637(void) {
    int mode = NATIVE_MODE("005B6637");
    if (!mode) { __real_sub_005B6637(); return; }
    uint32_t self = g_ecx, sp = g_esp, face = MEM32(sp + 4);
    uint32_t off = MEM32(self + 0xC), len = 2u * (uint32_t)(int32_t)(int16_t)MEM16(face + 8) + 10u;
    if (mode == 2) {                       /* check: the lifted one does what this one would */
        static unsigned calls, bad;
        uint32_t nf = MEM32(self + 0x10);
        __real_sub_005B6637();
        calls++;
        if ((g_eax != off || MEM32(self + 0xC) != off + len || MEM32(self + 0x10) != nf + 1 ||
             memcmp(hle_ptr(MEM32(self + 4) + off), hle_ptr(face), len)) && bad++ < 20)
            perf_note("check sub_005B6637: offset %u (want %u), count %u, faces %u (want %u)", g_eax, off, MEM32(self + 0xC), MEM32(self + 0x10), nf + 1);
        if (calls % 200000 == 0) perf_note("check sub_005B6637: %u calls, %u differ", calls, bad);
        return;
    }
    uint32_t n = off + len;
    if ((int32_t)n > (int32_t)MEM32(self + 8)) {
        uint32_t cap = MEM32(self + 8), grow = (int32_t)cap < 0x20 ? 0x20u : cap;
        call_this1(sub_005B6701, self, grow + n - 1);
    }
    MEM32(self + 0xC) = n;
    memmove(hle_ptr(MEM32(self + 4) + off), hle_ptr(face), len);
    MEM32(self + 0x10) += 1;
    g_eax = off;
    g_esp = sp + 4 + 4;
}

/*
 * sub_00508542: CSessions::SetCurSel(sel, update) (thiscall, ret 8), the
 * multiplayer screen's server list (cursel at +0x98, top row at +0x9c, a
 * float). The screen rebuilds the list every frame (sub_004C2B68: the rows
 * again, the selected server found again by its key) and selects it again
 * with update = 1, which also scrolls the list back to it: a list longer
 * than the screen (the Internet servers) could not be scrolled. When that
 * refresh selects the row already selected, it is done with update = 0,
 * which keeps the scroll. env OFP_TRACE_LIST=1 logs each call.
 */
void __real_sub_00508542(void);
void __wrap_sub_00508542(void) {
    static int trace = -1;
    if (trace < 0) trace = ofp_env_int("OFP_TRACE_LIST", 0);
    uint32_t self = g_ecx, sp = g_esp;
    int sel = (int)MEM32(sp + 4), cur = (int)MEM32(self + 0x98);
    if (trace)
        perf_note("list SetCurSel(sel %d, update %u) from sub_%08X: cursel %d, top %.1f", sel, MEM32(sp + 8) & 0xFF,
                  g_cur_func, cur, F(self + 0x9c));
    if (g_cur_func == 0x004C2B68u && sel == cur) MEM8(sp + 8) = 0;   /* the refresh, nothing new: keep the scroll */
    __real_sub_00508542();
}

/*
 * DisplayOptions__virtual_22 (0x004E22E3): the Options screen's button
 * handler (thiscall, idc, ret 4). The "Linux options" button the recomp adds
 * to that screen (tools/make_resource.py, idc 107) opens the recomp's own
 * options; every other button is the game's.
 */
void __real_DisplayOptions__virtual_22(void);
void __wrap_DisplayOptions__virtual_22(void) {
    if (MEM32(g_esp + 4) == 107) {
        overlay_open();
        g_esp += 8;                            /* ret 4 */
        return;
    }
    __real_DisplayOptions__virtual_22();
}

/*
 * sub_0064DA91: the view distance the scene is drawn to (cdecl, a float,
 * clamped by the game to 500-5000), set from the player's settings when a
 * world loads and when they change. The main menu's background scene
 * (worlds\intro.wrp, no fog) at 5000 m with a fine terrain grid builds
 * more landscape than the game's memory holds: about 20 s an image,
 * forever. There it is held to MENU_VIEW_MAX; the player's setting itself
 * is untouched, and missions get it in full.
 */
#define MENU_VIEW_MAX 1500.0f
static int g_menu_world;
void ofp_file_opened(const char *name) {
    size_t n = strlen(name);
    if (n < 4 || strcasecmp(name + n - 4, ".wrp")) return;
    g_menu_world = n >= 9 && !strcasecmp(name + n - 9, "intro.wrp") && (n == 9 || name[n - 10] == '\\' || name[n - 10] == '/');
}
void __real_sub_0064DA91(void);
void __wrap_sub_0064DA91(void) {
    float *d = (float *)ADDR(g_esp + 4);
    if (g_menu_world && *d > MENU_VIEW_MAX) {
        perf_note("main menu scene: view distance %.0f m held to %.0f m", *d, MENU_VIEW_MAX);
        *d = MENU_VIEW_MAX;
    }
    __real_sub_0064DA91();
}

/* a stdcall or thiscall to any guest address (a lifted function, a vtable
 * slot, an import): the callee pops its arguments */
static uint32_t call_va(uint32_t va, uint32_t ecx, int nargs, const uint32_t *args) {
    uint32_t caller = g_cur_func;
    for (int i = nargs - 1; i >= 0; i--) g_esp -= 4, MEM32(g_esp) = args[i];
    g_esp -= 4, MEM32(g_esp) = RECOMP_RETADDR;
    g_ecx = ecx;
    recomp_resolve(va)();
    g_cur_func = caller;
    return g_eax;
}

/*
 * sub_006A2660: a D3D shape's index buffer, made from the shape's faces
 * (thiscall on a new 0x40-byte object, ret 8): (shape, mode) -> al = ok.
 * Each face (10 bytes, int16 n at +8, then n int16 vertex indices; the list at
 * shape+0xDC, its size in bytes at +0xE4) becomes a fan of n - 2 triangles;
 * each section (shape+0xF0, 0x18 bytes each, faces from byte +0x10 to +0x14,
 * shape+0xF8 of them) gets a record {first index, end index, min vertex,
 * max vertex + 1} in the object's array at +0x10 (data +0x14).
 * Native here: the object's own buffer (+0x3C clear) with the mode setting
 * +0xC (mode 2 and over). The rest (a buffer shared through 0x7729A8) runs the
 * lifted code. The loops are the original's, in the same order.
 */
void lifted_sub_006A2660(void);
/* the faces of [beg, end): how many indices their fans take */
static int32_t ib_count(const uint8_t *faces, int32_t beg, int32_t end) {
    int32_t nidx = 0;
    for (int32_t o = beg; o < end; ) {
        int32_t n = *(const int16_t *)(faces + o + 8);
        o += 2 * n + 10;
        nidx += 3 * n - 6;
    }
    return nidx;
}
/* the fans: (v0, v[k-1], v[k]) for k from 2 */
static void ib_fans(const uint8_t *faces, int32_t total, uint16_t *out) {
    for (int32_t o = 0; o < total; ) {
        const uint8_t *f = faces + o;
        int32_t n = *(const int16_t *)(f + 8);
        const uint16_t *v = (const uint16_t *)(f + 0xA);
        for (int32_t k = 2; k < n; k++) *out++ = v[0], *out++ = v[k - 1], *out++ = v[k];
        o += 2 * n + 10;
    }
}
/* a section's record: {first index, end index, min vertex, max vertex + 1} */
static void ib_section(const uint8_t *faces, int32_t beg, int32_t end, int32_t start, uint32_t rec[4]) {
    int32_t cnt = 0, mn = 0x7FFFFFFF, mx = 0;
    for (int32_t o = beg; o < end; ) {
        int32_t n = *(const int16_t *)(faces + o + 8);
        cnt += 3 * n - 6;
        const int16_t *v = (const int16_t *)(faces + o + 0xA);
        for (int32_t k = 0; k < n; k++) {
            if (v[k] < mn) mn = v[k];
            if (v[k] > mx) mx = v[k];
        }
        o += 2 * n + 10;
    }
    rec[0] = (uint32_t)start, rec[1] = (uint32_t)(start + cnt), rec[2] = (uint32_t)mn, rec[3] = (uint32_t)(mx + 1);
}
static void ib_build(uint32_t self, uint32_t shape) {
    uint32_t sp = g_esp, frame = sp - 0x34, a[5];
    g_esp = frame;                         /* callees push below our frame, as the original's */
    if (!MEM8(self + 0xD)) {
        a[0] = MEM32(shape + 0x58);
        call_va(0x006A2580u, self, 1, a);
        if (!MEM32(self + 0x28)) { g_eax &= ~0xFFu; g_esp = sp + 4 + 8; return; }
        a[0] = shape;
        call_va(0x006A2BE0u, self, 1, a);
    }
    int32_t total = (int32_t)MEM32(shape + 0xE4);
    int32_t nidx = ib_count((const uint8_t *)hle_ptr(MEM32(shape + 0xDC)), 0, total);
    if (total > 0 && nidx > 0) {
        a[0] = (uint32_t)nidx;
        call_va(0x006A2480u, self, 1, a);
        uint32_t ptr;
        for (;;) {
            uint32_t ib = MEM32(self + 0x2C);
            MEM32(frame + 0x28) = 0;
            a[0] = ib, a[1] = 0, a[2] = 2u * (uint32_t)nidx, a[3] = frame + 0x28, a[4] = 0x800;
            uint32_t hr = call_va(MEM32(MEM32(ib) + 0x2C), ib, 5, a);    /* IDirect3DIndexBuffer8::Lock */
            ptr = MEM32(frame + 0x28);
            if (hr == 0 && ptr) break;
            uint32_t mm = MEM32(0x00758ADCu);
            a[0] = 0x100000;
            if (call_va(MEM32(MEM32(mm) + 4), mm, 1, a) & 0xFF) continue;   /* memory freed: again */
            a[0] = hr;
            call_va(0x006FC700u, mm, 1, a);
            g_eax &= ~0xFFu;
            g_esp = sp + 4 + 8;
            return;
        }
        ib_fans((const uint8_t *)hle_ptr(MEM32(shape + 0xDC)), (int32_t)MEM32(shape + 0xE4), (uint16_t *)hle_ptr(ptr));
        uint32_t ib = MEM32(self + 0x2C);
        a[0] = ib;
        call_va(MEM32(MEM32(ib) + 0x30), ib, 1, a);                     /* Unlock */

        /* the section records: the array at +0x10 sized to the sections */
        uint32_t arr = self + 0x10;
        int32_t ns = (int32_t)MEM32(shape + 0xF8);
        if ((int32_t)MEM32(arr + 8) != ns) {
            uint32_t al = MEM32(0x00756E24u);
            a[0] = (uint32_t)ns << 4;
            uint32_t nd = call_va(MEM32(MEM32(al) + 4), al, 1, a);
            if (MEM32(arr + 4)) {
                if ((int32_t)MEM32(arr + 0xC) > ns) { a[0] = (uint32_t)ns; call_va(0x006A9B00u, arr, 1, a); }
                memmove(hle_ptr(nd), hle_ptr(MEM32(arr + 4)), MEM32(arr + 0xC) << 4);
                a[0] = MEM32(arr + 4);
                call_va(MEM32(MEM32(al) + 0xC), al, 1, a);
            }
            MEM32(arr + 4) = nd, MEM32(arr + 8) = (uint32_t)ns;
        }
        ns = (int32_t)MEM32(shape + 0xF8);
        int32_t cap = (int32_t)MEM32(arr + 8);
        if (ns > cap) {
            int32_t g = cap < 0x20 ? 0x20 : cap;
            a[0] = (uint32_t)ns, a[1] = (uint32_t)(g + ns - 1);
            call_va(0x006AA420u, arr, 2, a);
        }
        MEM32(arr + 0xC) = (uint32_t)ns;
        int32_t start = 0;
        for (int32_t i = 0; i < (int32_t)MEM32(shape + 0xF8); i++) {
            uint32_t sec = MEM32(shape + 0xF0) + 0x18u * (uint32_t)i, r[4];
            ib_section((const uint8_t *)hle_ptr(MEM32(shape + 0xDC)), (int32_t)MEM32(sec + 0x10), (int32_t)MEM32(sec + 0x14), start, r);
            memcpy(hle_ptr(MEM32(self + 0x14) + 16u * (uint32_t)i), r, 16);
            start = (int32_t)r[1];
        }
    }
    MEM32(self + 0x20) = 0, MEM32(self + 0x24) = 0;
    g_eax = (g_eax & ~0xFFu) | 1;
    g_esp = sp + 4 + 8;
}
/* check mode: the lifted code runs; the indices and the section records it
 * made are compared with what the native loops make from the same faces */
static void ib_check(uint32_t self, uint32_t shape) {
    static unsigned calls, bad;
    int32_t total = (int32_t)MEM32(shape + 0xE4);
    const uint8_t *faces = (const uint8_t *)hle_ptr(MEM32(shape + 0xDC));
    int32_t nidx = ib_count(faces, 0, total);
    uint16_t *want = nidx > 0 ? (uint16_t *)malloc(2u * (uint32_t)nidx) : NULL;
    if (want) ib_fans(faces, total, want);
    lifted_sub_006A2660();
    uint32_t ok = g_eax & 0xFF, eax = g_eax, sp = g_esp;
    int diff = 0;
    if (ok && want && MEM32(self + 0x2C)) {
        uint32_t ib = MEM32(self + 0x2C), slot = g_esp - 0x40, a[5];
        g_esp = slot - 4;
        MEM32(slot) = 0;
        a[0] = ib, a[1] = 0, a[2] = 2u * (uint32_t)nidx, a[3] = slot, a[4] = 0x10;   /* read only */
        if (call_va(MEM32(MEM32(ib) + 0x2C), ib, 5, a) == 0 && MEM32(slot)) {
            if (memcmp(hle_ptr(MEM32(slot)), want, 2u * (uint32_t)nidx)) diff |= 1;
            a[0] = ib; call_va(MEM32(MEM32(ib) + 0x30), ib, 1, a);
        }
        int32_t start = 0;
        for (int32_t i = 0; i < (int32_t)MEM32(shape + 0xF8); i++) {
            uint32_t sec = MEM32(shape + 0xF0) + 0x18u * (uint32_t)i, r[4];
            ib_section(faces, (int32_t)MEM32(sec + 0x10), (int32_t)MEM32(sec + 0x14), start, r);
            if (memcmp(hle_ptr(MEM32(self + 0x14) + 16u * (uint32_t)i), r, 16)) diff |= 2;
            start = (int32_t)r[1];
        }
    }
    g_esp = sp, g_eax = eax;
    free(want);
    calls++;
    if (diff && bad++ < 20) perf_note("check sub_006A2660: differs (%s%s), %d indices", diff & 1 ? "indices " : "", diff & 2 ? "sections" : "", (int)nidx);
    if (calls % 2000 == 0) perf_note("check sub_006A2660: %u calls, %u differ", calls, bad);
}
void sub_006A2660(void) {
    int mode = NATIVE_MODE("006A2660");
    uint32_t self = g_ecx, sp = g_esp, shape = MEM32(sp + 4), m = MEM32(sp + 8);
    /* only the object's own buffer: c (+0xC) set by the mode, no shared one */
    if (!mode || m < 2 || MEM32(self + 0x3C) || (int32_t)MEM32(shape + 0x58) <= 0) {
        static unsigned other;
        if (mode == 2 && other++ % 1000 == 0) perf_note("sub_006A2660: lifted path (mode %u, shared %u), %u times", m, MEM32(self + 0x3C) != 0, other);
        lifted_sub_006A2660();
        return;
    }
    if (mode == 2) { ib_check(self, shape); return; }
    g_cur_func = 0x006A2660u;
    MEM8(self + 0xC) = 1, MEM8(self + 0xD) = m != 2;
    MEM32(self + 0x20) = 0, MEM32(self + 0x24) = 0, MEM32(sp + 8) = 0;
    ib_build(self, shape);
}

/* sub_00671950, a terrain segment's build: timed, for the perf log (the
 * stutters when the view turns are these builds) */
void lifted_sub_00671950(void);
uint64_t ofp_seg_n, ofp_seg_ns;
void sub_00671950(void) {
    uint64_t t0 = hle_ticks();
    lifted_sub_00671950();
    ofp_seg_n++, ofp_seg_ns += hle_ticks_ns(hle_ticks() - t0);
}
