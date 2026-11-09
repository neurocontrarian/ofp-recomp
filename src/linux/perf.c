/*
 * perf.c - the performance log: logs/perf-YYYYMMDD-HHMMSS.log
 *
 * - a header: build, machine (CPU, memory, distribution, desktop), the game's
 *   settings files, then the OpenGL driver and the Direct3D device as made;
 * - one line a second: frames, frame times, draws, textures uploaded, files
 *   opened and read, CPU and memory, where the time went (sampled every 5 ms:
 *   the game's own code by lifted function, or the import running);
 * - freezes (no frame for FREEZE_MS): when one starts, while it lasts, and
 *   on its end how long it was, where it was spent and the files it read;
 * - every file the game opens (name, time to open), slow reads;
 * - at exit: totals, and the settings files again.
 * Off with OFP_PERF=0. OFP_LOG_DIR picks the folder (default: logs/ beside
 * the game folder).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <x86intrin.h>
#include <linux/perf_event.h>

#include "win32hle.h"
#include "ofp.h"

#ifndef OFP_GIT
#define OFP_GIT "?"
#endif
#ifndef OFP_GEN_OPT
#define OFP_GEN_OPT "?"
#endif

#define FREEZE_MS    500           /* no frame for this long: a freeze */
#define HITCH_MS     50            /* one frame this long: logged, with what it did */
#define MAX_HITCH_LINES 3000       /* then only the count a second */
#define SAMPLE_US    5000
#define MAX_FILE_LINES 4000        /* then only slow opens are logged */

static FILE *g_log;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_t0;
static time_t g_wall0;                 /* the start, wall clock: a newer .rpt is this run's */
static char g_game[4096];

#define now_ns hle_mono_ns
static double secs(void) { return (double)(now_ns() - g_t0) / 1e9; }

void perf_note(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap;
    pthread_mutex_lock(&g_mu);
    if (!g_log) { pthread_mutex_unlock(&g_mu); return; }   /* closed meanwhile (perf_exit) */
    fprintf(g_log, "[%8.2f] ", secs());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    pthread_mutex_unlock(&g_mu);                     /* flushed once a second (sampler), on a fault and at exit */
}
FILE *perf_file(void) { return g_log; }

/* ---- counters, written by the game's thread, read by the sampler ---------- */
static volatile uint64_t c_frames, c_draws, c_tex_n, c_tex_bytes, c_open_n, c_read_bytes, c_read_ns, c_read_n;
static volatile uint64_t g_last_frame_ns, g_frame_max_ns;
static int g_first_frame_done;

static volatile uint64_t c_hitch;            /* slow frames (HITCH_MS), counted a second */
/* a slow frame: what it did (the game's own code, the imports that took the
 * most, textures uploaded, files opened and read), against the frame before */
static void slow_frame(uint64_t dt, unsigned draws) {
    static uint64_t *imp0, tex0, texb0, open0, read0, out0, np0, npns0;
    static unsigned lines;
    int n = hle_shim_total();
    if (!imp0) imp0 = calloc(4096, sizeof *imp0);
    uint64_t out = hle_shim_ns_outer;
    if (dt) {
        c_hitch++;
        if (lines++ < MAX_HITCH_LINES) {
            int a = -1, b = -1;
            for (int i = 0; i < n; i++) {
                uint64_t d = hle_shim_ns_main[i] - imp0[i];
                if (a < 0 || d > hle_shim_ns_main[a] - imp0[a]) b = a, a = i;
                else if (b < 0 || d > hle_shim_ns_main[b] - imp0[b]) b = i;
            }
            uint64_t g = dt > out - out0 ? dt - (out - out0) : 0;
            perf_note("slow frame %.0f ms (%u draws): game code %.0f ms; %s %.0f ms, %s %.0f ms; %llu textures (%.1f MB), %llu files opened, %.1f MB read, %llu new pipelines (%.0f ms)",
                      (double)dt / 1e6, draws, (double)g / 1e6,
                      a >= 0 ? hle_shim_name_at(a) : "-", a >= 0 ? (double)(hle_shim_ns_main[a] - imp0[a]) / 1e6 : 0.0,
                      b >= 0 ? hle_shim_name_at(b) : "-", b >= 0 ? (double)(hle_shim_ns_main[b] - imp0[b]) / 1e6 : 0.0,
                      (unsigned long long)(c_tex_n - tex0), (double)(c_tex_bytes - texb0) / 1048576.0,
                      (unsigned long long)(c_open_n - open0), (double)(c_read_bytes - read0) / 1048576.0,
                      (unsigned long long)(perf_rc[RC_NEWPIPE] - np0), (double)(perf_rc[RC_NEWPIPE_NS] - npns0) / 1e6);
        }
    }
    memcpy(imp0, hle_shim_ns_main, sizeof(uint64_t) * (size_t)n);
    tex0 = c_tex_n, texb0 = c_tex_bytes, open0 = c_open_n, read0 = c_read_bytes, out0 = out;
    np0 = perf_rc[RC_NEWPIPE], npns0 = perf_rc[RC_NEWPIPE_NS];
}

void perf_frame(unsigned draws) {
    if (!g_log) return;
    uint64_t t = now_ns(), last = g_last_frame_ns, dt = 0;
    if (!g_first_frame_done) { g_first_frame_done = 1; perf_note("first frame after %.2f s", secs()); }
    else if (last) {
        dt = t - last;
        if (dt > g_frame_max_ns) g_frame_max_ns = dt;
    }
    slow_frame(dt >= HITCH_MS * 1000000ull && dt < FREEZE_MS * 1000000ull ? dt : 0, draws);
    g_last_frame_ns = t;
    c_frames++;
    c_draws += draws;
}
void perf_texture(uint32_t bytes) { c_tex_n++; c_tex_bytes += bytes; }
volatile uint64_t perf_rc[RC_N];

/* ---- the main thread's CPU cycles (user mode): the bench's figure, which
 * a moving CPU clock does not change as it does the time. The kernel may
 * refuse (perf_event_paranoid 3 or more): then no figure. On a hybrid CPU
 * the performance cores' counter (the bench runs there). ------------------ */
static int g_cyc_fd = -1;
static volatile struct perf_event_mmap_page *g_pc;
/* the count now, by rdpmc (some 30 cycles); 0 when it cannot be read so (the
 * thread is on a core whose counter this is not) */
static uint64_t cycles_rdpmc(void) {
    uint32_t seq, idx;
    uint64_t cnt;
    do {
        seq = g_pc->lock;
        __asm__ volatile("" ::: "memory");
        idx = g_pc->index;
        if (!idx) return 0;
        int64_t pmc = (int64_t)__rdpmc((int)idx - 1);
        int w = g_pc->pmc_width;
        pmc = (int64_t)((uint64_t)pmc << (64 - w)) >> (64 - w);
        cnt = (uint64_t)((int64_t)g_pc->offset + pmc);
        __asm__ volatile("" ::: "memory");
    } while (g_pc->lock != seq);
    return cnt;
}
static void cycles_open(void) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a, a.type = PERF_TYPE_HARDWARE, a.config = PERF_COUNT_HW_CPU_CYCLES;
    a.exclude_kernel = 1, a.exclude_hv = 1;
    FILE *f = fopen("/sys/bus/event_source/devices/cpu_core/type", "r");
    unsigned core = 0;
    if (f) { if (fscanf(f, "%u", &core) == 1 && core) a.config |= (uint64_t)core << 32; fclose(f); }
    g_cyc_fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (g_cyc_fd < 0) return;
    /* the counter's page: read it in user mode (rdpmc) around each import */
    void *pg = mmap(NULL, (size_t)sysconf(_SC_PAGESIZE), PROT_READ, MAP_SHARED, g_cyc_fd, 0);
    if (pg != MAP_FAILED && ((struct perf_event_mmap_page *)pg)->cap_user_rdpmc) {
        g_pc = pg;
        hle_cycles_fn = cycles_rdpmc;
    }
}
static uint64_t cycles_now(void) {
    uint64_t v = 0;
    if (g_cyc_fd < 0 || read(g_cyc_fd, &v, sizeof v) != sizeof v) return 0;
    return v;
}

/* ---- the sampler: hot lifted functions and imports ------------------------ */
#define HBITS 12
typedef struct { uint32_t key, n; } slot_t;
typedef struct { slot_t fn[1 << HBITS]; uint32_t samples, in_game; } hist_t;
static void hist_add(hist_t *h, uint32_t key) {
    uint32_t i = (key * 2654435761u) >> (32 - HBITS);
    for (int k = 0; k < 32; k++, i = (i + 1) & ((1 << HBITS) - 1)) {   /* a short probe: a full table drops a sample */
        if (h->fn[i].key == key) { h->fn[i].n++; return; }
        if (!h->fn[i].n) { h->fn[i].key = key; h->fn[i].n = 1; return; }
    }
}
/* the top n of a histogram, as "sub_XXXXXXXX 12%, ReadFile 5%, ..." (keys
 * with the top bit set are imports, by registration index) */
static void hist_top(const hist_t *h, int n, char *out, size_t sz) {
    slot_t best[8] = { { 0, 0 } };
    if (n > 8) n = 8;
    for (int i = 0; i < (1 << HBITS); i++) {
        slot_t s = h->fn[i];
        for (int k = 0; k < n; k++)
            if (s.n > best[k].n) { memmove(&best[k + 1], &best[k], sizeof best[0] * (size_t)(n - 1 - k)); best[k] = s; break; }
    }
    size_t o = 0; out[0] = 0;
    for (int k = 0; k < n && best[k].n && o < sz; k++)
    {
        unsigned pc = h->samples ? best[k].n * 100u / h->samples : 0u;
        if (best[k].key & 0x80000000u) {
            const char *nm = hle_shim_name_at((int)(best[k].key & 0x7FFFFFFFu));
            o += (size_t)snprintf(out + o, sz - o, "%s%s %u%%", k ? ", " : "", nm ? nm : "?", pc);
        } else o += (size_t)snprintf(out + o, sz - o, "%ssub_%08X %u%%", k ? ", " : "", best[k].key, pc);
    }
}

/* the imports that took the most time between two snapshots of hle_shim_ns */
static void imports_top(const uint64_t *before, int n, char *out, size_t sz, double min_ms) {
    int tot = hle_shim_total(), best[6] = { -1, -1, -1, -1, -1, -1 };
    uint64_t bv[6] = { 0 };
    if (n > 6) n = 6;
    for (int i = 0; i < tot; i++) {
        uint64_t d = hle_shim_ns_main[i] - before[i];
        for (int k = 0; k < n; k++)
            if (d > bv[k]) {
                memmove(&bv[k + 1], &bv[k], sizeof bv[0] * (size_t)(n - 1 - k));
                memmove(&best[k + 1], &best[k], sizeof best[0] * (size_t)(n - 1 - k));
                bv[k] = d, best[k] = i;
                break;
            }
    }
    size_t o = 0; out[0] = 0;
    for (int k = 0; k < n && best[k] >= 0 && o < sz; k++) {
        if ((double)bv[k] / 1e6 < min_ms) break;
        o += (size_t)snprintf(out + o, sz - o, "%s%s %.0fms", k ? ", " : "", hle_shim_name_at(best[k]), (double)bv[k] / 1e6);
    }
}

/* the address space in use (VmSize: statm's first field) */
static double vm_mb(void) {
    unsigned long sz = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    if (fscanf(f, "%lu", &sz) != 1) sz = 0;
    fclose(f);
    return (double)sz * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
}
static volatile unsigned c_alloc_fail;
static void on_alloc_fail(const char *what, uint32_t n) {
    if (c_alloc_fail++ < 50)
        perf_note("OUT OF MEMORY: %s of %u bytes refused (address space used %.0f MB of 4096)", what, n, vm_mb());
}

static void proc_cpu_mem(double *cpu_s, double *rss_mb) {
    *cpu_s = 0, *rss_mb = 0;
    FILE *f = fopen("/proc/self/stat", "r");
    if (f) {
        char b[1024];
        if (fgets(b, sizeof b, f)) {
            char *p = strrchr(b, ')');
            unsigned long ut = 0, st = 0;
            if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2)
                *cpu_s = (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
        }
        fclose(f);
    }
    f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long sz, rss;
        if (fscanf(f, "%lu %lu", &sz, &rss) == 2) *rss_mb = (double)rss * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
        fclose(f);
    }
}

/* the freeze being tracked */
static struct {
    int on;
    uint64_t start;
    hist_t h;
    uint64_t *imp0;
    uint64_t open0, read0, readns0;
    char files[6][160];
    int nfiles;
} g_fz;
static char g_last_open[512];

static int tsc_on;
/* The landscape's segment cache (sub_00670AE0, sized by sub_00670CD0): its
 * limit, (view distance x 0.02 / 8)^2 x 8 segments of 8 x 8 cells, clamped to
 * 16..4096, and the view distance the game uses now (0x7C5B20: it lowers it
 * when the frame rate falls under the player's "frame rate" setting). Logged
 * at most every 30 s, when it changed. */
static void terrain_note(void) {
    static float g0; static uint32_t c0; static int wait;
    if (wait > 0) { wait--; return; }
    uint32_t land = MEM32(0x0079C72Cu);
    if (!land) return;
    float g = MEMF(0x007C5B20u);
    uint32_t cap = MEM32(land + 0x205CCu + 0x14u);
    if (g == g0 && cap == c0) return;
    g0 = g, c0 = cap, wait = 29;
    perf_note("terrain: view distance now %.0f m, segment cache limit %u", (double)g, cap);
}
static void *sampler(void *arg) {
    (void)arg;
    uint64_t *imp_all = calloc(4096, sizeof *imp_all);
    g_fz.imp0 = calloc(4096, sizeof *g_fz.imp0);
    static hist_t sec, all;
    uint64_t next = now_ns() + 1000000000ull, f0 = 0, d0 = 0, t0n = 0, t0b = 0, o0 = 0, r0 = 0, rn0 = 0;
    double cpu0, rss;
    proc_cpu_mem(&cpu0, &rss);
    for (;;) {
        usleep(SAMPLE_US);
        /* where the main thread is: in an import, or in lifted code (the
         * function running guest code, which is the main thread's unless
         * it is waiting in an import) */
        uint32_t fn = g_cur_func;
        int sh = hle_in_shim_main;
        uint32_t key = sh >= 0 ? 0x80000000u | (uint32_t)sh : fn;
        sec.samples++, all.samples++;
        if (sh < 0) sec.in_game++, all.in_game++;
        hist_add(&sec, key), hist_add(&all, key);
        if (g_fz.on) { g_fz.h.samples++; if (sh < 0) g_fz.h.in_game++; hist_add(&g_fz.h, key); }

        uint64_t t = now_ns();
        /* a freeze starts: no frame for FREEZE_MS once frames have come */
        if (!g_fz.on && g_first_frame_done && t - g_last_frame_ns > FREEZE_MS * 1000000ull) {
            memset(&g_fz.h, 0, sizeof g_fz.h);
            g_fz.on = 1, g_fz.start = g_last_frame_ns, g_fz.nfiles = 0;
            memcpy(g_fz.imp0, hle_shim_ns_main, sizeof(uint64_t) * (size_t)hle_shim_total());
            g_fz.open0 = c_open_n, g_fz.read0 = c_read_bytes, g_fz.readns0 = c_read_ns;
            const char *imp = sh >= 0 ? hle_shim_name_at(sh) : NULL;
            perf_note("FREEZE starts: no frame for %d ms; in sub_%08X%s%s, last file opened: %s", FREEZE_MS, fn,
                      imp ? ", inside " : "", imp ? imp : "", g_last_open[0] ? g_last_open : "-");
        }
        /* and ends with the next frame */
        if (g_fz.on && g_last_frame_ns > g_fz.start) {
            char top[512], imp[512];
            hist_top(&g_fz.h, 6, top, sizeof top);
            imports_top(g_fz.imp0, 5, imp, sizeof imp, 1.0);
            perf_note("FREEZE ends: %.2f s without a frame; main thread: game code %u%%, where [%s]; import time [%s]; %llu files opened, %.1f MB read in %.0f ms",
                      (double)(g_last_frame_ns - g_fz.start) / 1e9, g_fz.h.samples ? g_fz.h.in_game * 100u / g_fz.h.samples : 0u, top, imp,
                      (unsigned long long)(c_open_n - g_fz.open0), (double)(c_read_bytes - g_fz.read0) / 1048576.0,
                      (double)(c_read_ns - g_fz.readns0) / 1e6);
            for (int i = 0; i < g_fz.nfiles; i++) perf_note("  file during freeze: %s", g_fz.files[i]);
            g_fz.on = 0;
        }

        if (t < next) continue;
        /* the line of the second */
        double dt = (double)(t - next + 1000000000ull) / 1e9, cpu;
        next = t + 1000000000ull;
        if (tsc_on) hle_tsc_calibrate();        /* refines the scale */
        if (g_first_frame_done) terrain_note(); /* the game is mapped by then */
        proc_cpu_mem(&cpu, &rss);
        uint64_t fr = c_frames - f0, dr = c_draws - d0;
        static uint64_t h0;
        /* the main thread's time in the game's own code this second, exact:
         * the wall time outside imports (the bench figure, tools/bench.sh) */
        static uint64_t gt0, go0, cy0, ci0;
        /* the main thread's cycles in the game's own code: all of them less
         * those in imports (or all of them when the imports' are not read) */
        uint64_t cy = cycles_now(), ci = hle_shim_cyc_outer;
        uint64_t dcy = cy0 && cy > cy0 ? cy - cy0 : 0, dci = ci - ci0;
        uint64_t acy = dcy;                 /* all of them: what the import timing costs shows here */
        dcy = dcy > dci ? dcy - dci : 0;
        cy0 = cy, ci0 = ci;
        uint64_t go = hle_shim_ns_outer, gns = gt0 && t - gt0 > go - go0 ? (t - gt0) - (go - go0) : 0;
        gt0 = t, go0 = go;
        char top[512];
        hist_top(&sec, 4, top, sizeof top);
        perf_note("fps %3.0f | frame max %4.0f ms | draws/frame %4.0f | tex %llu (%.1f MB) | files %llu, read %.1f MB in %.0f ms (%llu reads) | cpu %3.0f%% | ram %.0f MB, space %.0f MB%s | game %.2f ms/frame (%.0f ms), %.2f Mcycles/frame, all %.2f Mcycles/frame | slow frames %llu | main thread: game code %u%%, where [%s]%s",
                  (double)fr / dt, (double)g_frame_max_ns / 1e6, fr ? (double)dr / (double)fr : 0.0,
                  (unsigned long long)(c_tex_n - t0n), (double)(c_tex_bytes - t0b) / 1048576.0,
                  (unsigned long long)(c_open_n - o0), (double)(c_read_bytes - r0) / 1048576.0, (double)(c_read_ns - rn0) / 1e6,
                  (unsigned long long)c_read_n, (cpu - cpu0) * 100.0 / dt, rss, vm_mb(), c_alloc_fail ? " (allocations refused!)" : "",
                  fr ? (double)gns / 1e6 / (double)fr : 0.0, (double)gns / 1e6, fr ? (double)dcy / 1e6 / (double)fr : 0.0, fr ? (double)acy / 1e6 / (double)fr : 0.0, (unsigned long long)(c_hitch - h0), sec.samples ? sec.in_game * 100u / sec.samples : 0u, top, g_fz.on ? " | FROZEN" : "");
        static uint64_t rc0[RC_N];
        uint64_t rc[RC_N];
        for (int i = 0; i < RC_N; i++) rc[i] = perf_rc[i] - rc0[i], rc0[i] = perf_rc[i];
        double pf = fr ? (double)fr : 1.0;
        perf_note("  render %.0f%% (%.1f us a draw), stream %.1f MB, copies %.1f MB (%llu new), per frame: %.0f pushes, %.0f uniforms, %.0f pipelines; %llu new pipelines (%.0f ms)",
                  (double)rc[RC_NS] / 1e7 / dt, dr ? (double)rc[RC_NS] / 1e3 / (double)dr : 0.0, (double)rc[RC_STREAM] / 1048576.0,
                  (double)rc[RC_COPY] / 1048576.0, (unsigned long long)rc[RC_NEWBUF], rc[RC_PUSH] / pf, rc[RC_UBO] / pf, rc[RC_PIPE] / pf,
                  (unsigned long long)rc[RC_NEWPIPE], (double)rc[RC_NEWPIPE_NS] / 1e6);
        pthread_mutex_lock(&g_mu);
        if (g_log) fflush(g_log);
        pthread_mutex_unlock(&g_mu);
        h0 = c_hitch;
        f0 = c_frames, d0 = c_draws, t0n = c_tex_n, t0b = c_tex_bytes, o0 = c_open_n, r0 = c_read_bytes, rn0 = c_read_ns;
        c_read_n = 0;
        g_frame_max_ns = 0;
        cpu0 = cpu;
        memset(&sec, 0, sizeof sec);
        /* every minute: the whole session so far */
        static int minute;
        extern uint64_t ofp_seg_n, ofp_seg_ns;
        static uint64_t sn0, sns0;
        if (minute % 10 == 9 && ofp_seg_n != sn0) {
            perf_note("terrain: %llu segments built in %.0f ms (%.2f ms each); since start %llu in %.0f ms (%.2f ms each)",
                      (unsigned long long)(ofp_seg_n - sn0), (double)(ofp_seg_ns - sns0) / 1e6,
                      (double)(ofp_seg_ns - sns0) / 1e6 / (double)(ofp_seg_n - sn0),
                      (unsigned long long)ofp_seg_n, (double)ofp_seg_ns / 1e6, (double)ofp_seg_ns / 1e6 / (double)ofp_seg_n);
            sn0 = ofp_seg_n, sns0 = ofp_seg_ns;
        }
        if (++minute % 60 == 0) {
            char imp[512];
            hist_top(&all, 8, top, sizeof top);
            imports_top(imp_all, 6, imp, sizeof imp, 1.0);
            perf_note("== since start, main thread: game code %u%%, where [%s]; import time [%s]", all.samples ? all.in_game * 100u / all.samples : 0u, top, imp);
        }
    }
    return NULL;
}

/* ---- files: CreateFileA and ReadFile, timed, around win32hle's own -------- */
static recomp_func_t o_CreateFileA, o_ReadFile, o_VirtualAlloc;
static unsigned g_file_lines;
static void p_CreateFileA(void) {
    char name[400];
    const char *p = ASTR(0);
    snprintf(name, sizeof name, "%s", p ? p : "(null)");
    uint32_t access = A32(1);
    uint64_t t = now_ns();
    o_CreateFileA();
    double ms = (double)(now_ns() - t) / 1e6;
    int ok = g_eax != 0xFFFFFFFFu;
    if (ok) ofp_file_opened(name);
    c_open_n++;
    snprintf(g_last_open, sizeof g_last_open, "%s", name);
    if (g_fz.on && g_fz.nfiles < 6) snprintf(g_fz.files[g_fz.nfiles++], sizeof g_fz.files[0], "%s", name);
    if (g_file_lines < MAX_FILE_LINES || ms >= 20.0) {
        g_file_lines++;
        perf_note("open %s %s%s (%.1f ms)", access & 0x40000000u ? "W" : "R", name, ok ? "" : " -> not found", ms);
    }
}
static void p_ReadFile(void) {                        /* (h, buf, n, &read, ovl) */
    uint32_t want = A32(2), pread = A32(3);
    uint64_t t = now_ns();
    o_ReadFile();
    uint64_t dt = now_ns() - t;
    uint32_t got = pread ? MEM32(pread) : want;
    c_read_ns += dt, c_read_bytes += got, c_read_n++;
    if (dt >= 50000000ull) perf_note("slow read: %u bytes in %.0f ms (last file opened: %s)", got, (double)dt / 1e6, g_last_open);
}
/* big reservations: the game's own memory pools */
static void p_VirtualAlloc(void) {                    /* (addr, size, type, protect) */
    uint32_t size = A32(1), type = A32(2);
    o_VirtualAlloc();
    if (size >= (16u << 20) || !g_eax)
        perf_note("VirtualAlloc %.0f MB (%s%s) -> %s, address space used %.0f MB", size / 1048576.0,
                  type & 0x2000 ? "reserve" : "", type & 0x1000 ? " commit" : "", g_eax ? "ok" : "REFUSED", vm_mb());
}
const win32hle_shim ofp_perf_shims[] = {
    { "VirtualAlloc", p_VirtualAlloc },
    { "CreateFileA", p_CreateFileA },
    { "ReadFile", p_ReadFile },
    { 0, 0 }
};

/* ---- the header ----------------------------------------------------------- */
static void dump_file(const char *path, const char *label) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    perf_note("--- %s (%s)", label, path);
    char line[512];
    pthread_mutex_lock(&g_mu);
    while (g_log && fgets(line, sizeof line, f)) { fputs("      ", g_log); fputs(line, g_log); if (!strchr(line, '\n')) fputc('\n', g_log); }
    if (g_log) fflush(g_log);
    pthread_mutex_unlock(&g_mu);
    fclose(f);
}
/* every *.cfg of a folder */
static void dump_cfgs(const char *dir, const char *label) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".cfg")) {
            char p[4400];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            dump_file(p, label);
        }
    }
    closedir(d);
}
/* the game's settings, and each player's (Users/NAME) */
static void dump_settings(void) {
    char p[4400];
    dump_cfgs(g_game, "settings");
    snprintf(p, sizeof p, "%s/Users", g_game);
    DIR *d = opendir(p);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char q[4400];
        snprintf(q, sizeof q, "%s/Users/%s", g_game, e->d_name);
        dump_cfgs(q, "player settings");
    }
    closedir(d);
}
static void first_line_of(const char *path, const char *key, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl)) {
            char *v = line + kl;
            while (*v == ' ' || *v == '\t' || *v == ':' || *v == '=' || *v == '"') v++;
            v[strcspn(v, "\"\n")] = 0;
            snprintf(out, n, "%s", v);
            break;
        }
    fclose(f);
}

/* a build made for profile-guided optimisation (-fprofile-generate) writes its
 * counts here: the game leaves with _exit, which skips gcc's own dump */
extern void __gcov_dump(void) __attribute__((weak));

static void perf_exit(int code) {
    if (__gcov_dump) __gcov_dump();
    if (!g_log) return;
    perf_note("exit, code %d, after %.1f s, %llu frames (%.1f fps on average)", code, secs(), (unsigned long long)c_frames,
              c_frames / (secs() > 0 ? secs() : 1));
    dump_settings();
    /* the game's own error report, when it stopped on one */
    char rpt[4400];
    snprintf(rpt, sizeof rpt, "%s/ColdWarAssault.rpt", g_game);
    struct stat st;
    if (stat(rpt, &st) == 0 && st.st_mtime >= (time_t)(g_wall0)) dump_file(rpt, "the game's error report");
    pthread_mutex_lock(&g_mu);                         /* the sampler and the network thread may be writing */
    fclose(g_log);
    g_log = NULL;
    pthread_mutex_unlock(&g_mu);
}

void perf_init(const char *game_dir, int argc, char **argv) {
    if (!ofp_env_int("OFP_PERF", 1)) return;
    snprintf(g_game, sizeof g_game, "%s", game_dir);
    char dir[4200];
    const char *ld = getenv("OFP_LOG_DIR");
    if (ld && *ld) snprintf(dir, sizeof dir, "%s", ld);
    else {
        snprintf(dir, sizeof dir, "%s", game_dir);
        char *s = strrchr(dir, '/');
        if (s) strcpy(s, "/logs"); else strcpy(dir, "logs");
    }
    mkdir(dir, 0755);
    time_t tt = time(NULL);
    struct tm tm;
    localtime_r(&tt, &tm);
    char path[4400];
    snprintf(path, sizeof path, "%s/perf-%04d%02d%02d-%02d%02d%02d.log", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    g_log = fopen(path, "w");
    if (!g_log) { fprintf(stderr, "[perf] cannot write %s\n", path); return; }
    g_t0 = now_ns();
    g_wall0 = time(NULL);
    fprintf(stderr, "[perf] log: %s\n", path);

    char b[512];
    perf_note("Operation Flashpoint recomp, build %s, generated code %s, started %04d-%02d-%02d %02d:%02d:%02d", OFP_GIT, OFP_GEN_OPT,
              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    {
        char cl[1024] = "";
        size_t o = 0;
        for (int i = 0; i < argc && o < sizeof cl; i++) o += (size_t)snprintf(cl + o, sizeof cl - o, "%s%s", i ? " " : "", argv[i]);
        perf_note("command line: %s", cl);
    }
    struct utsname u;
    if (!uname(&u)) perf_note("kernel: %s %s %s", u.sysname, u.release, u.machine);
    first_line_of("/etc/os-release", "PRETTY_NAME", b, sizeof b);
    perf_note("distribution: %s", b[0] ? b : "?");
    first_line_of("/proc/cpuinfo", "model name", b, sizeof b);
    perf_note("cpu: %s, %ld threads", b[0] ? b : "?", sysconf(_SC_NPROCESSORS_ONLN));
    char mt[64], ma[64];
    first_line_of("/proc/meminfo", "MemTotal", mt, sizeof mt);
    first_line_of("/proc/meminfo", "MemAvailable", ma, sizeof ma);
    perf_note("memory: total %s, available %s", mt, ma);
    first_line_of("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "", b, sizeof b);
    if (b[0]) perf_note("cpu governor: %s", b);
    static const char *env[] = { "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "DISPLAY", "WAYLAND_DISPLAY", "SDL_VIDEODRIVER",
                                 "LIBGL_ALWAYS_SOFTWARE", "LIBGL_DRIVERS_PATH", "MESA_LOADER_DRIVER_OVERRIDE", "__GLX_VENDOR_LIBRARY_NAME",
                                 "vblank_mode", "__GL_SYNC_TO_VBLANK", "OFP_VSYNC", "OFP_MSAA", "OFP_MASTER", "OFP_RENDERER", "OFP_HEAP_MB", "OFP_TSC", "OFP_SHOT", "OFP_D3DTRACE", 0 };
    for (int i = 0; env[i]; i++) if (getenv(env[i])) perf_note("env %s=%s", env[i], getenv(env[i]));
    dump_settings();
    hle_on_exit = perf_exit;
    hle_on_alloc_fail = on_alloc_fail;
    hle_mark_main_thread();             /* the game's main loop runs on this thread */
    cycles_open();                      /* counts this thread only */
    /* the cheap clock of the import times (20 ms); OFP_TSC=0: the system clock, for comparison */
    tsc_on = ofp_env_int("OFP_TSC", 1);
    if (tsc_on) hle_tsc_calibrate();
    hle_shim_timing = 1;                /* time spent in each import (host_lite.c) */
    pthread_t th;
    pthread_create(&th, NULL, sampler, NULL);
    pthread_detach(th);
}

/* without the log, CreateFileA is still followed (ofp_file_opened) */
static void w_CreateFileA(void) {
    char name[400];
    const char *p = ASTR(0);
    snprintf(name, sizeof name, "%s", p ? p : "(null)");
    o_CreateFileA();
    if (g_eax != 0xFFFFFFFFu) ofp_file_opened(name);
}
static const win32hle_shim ofp_open_shims[] = { { "CreateFileA", w_CreateFileA }, { 0, 0 } };

/* the file hooks wrap win32hle's own CreateFileA/ReadFile */
void perf_register_shims(void) {
    o_CreateFileA = ofp_find_shim(win32hle_kernel32_ext, "CreateFileA");
    if (!g_log) {                                     /* perf_init ran first: off with OFP_PERF=0 */
        if (o_CreateFileA) win32hle_register(ofp_open_shims);
        return;
    }
    o_ReadFile = ofp_find_shim(win32hle_kernel32_ext, "ReadFile");
    o_VirtualAlloc = ofp_find_shim(win32hle_kernel32, "VirtualAlloc");
    if (o_CreateFileA && o_ReadFile && o_VirtualAlloc) win32hle_register(ofp_perf_shims);
}
