/*
 * Operation Flashpoint / Arma: Cold War Assault 1.99 - the native Linux host.
 *
 * The lifted C (src/recomp/gen) on pcrecomp's runtime/win32hle: every Win32
 * import the game makes is answered by win32hle's own implementation, so no
 * Windows, no Wine, no eON. What is here is what is specific to this game:
 * where the image goes, the command line, the hooks the lift calls, the
 * fault report. Modelled on tiberiansun-recomp's src/linux/main.c.
 *
 * Linked non-PIE at 0x08048000, so the guest's 0x00400000..0x007CA000 is free.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "ofp.h"

extern const uint32_t ofp_entry_va;   /* recomp_dispatch.c */

#define OFP_IMAGE_BASE 0x00400000u
#define OFP_EXE_STAMP  0x58BD672Du    /* ColdWarAssault.exe 1.99 (run_lift.py) */

static int g_headless, g_fullscreen;
static unsigned g_watchdog_s;

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[host] not lifted: sub_%08X (called from sub_%08X)\n", va, g_cur_func);
    hle_exit(5);
}

/* ---- game-specific answers ------------------------------------------------ */
static void ofp_MessageBoxA(void) {
    uint32_t buttons = A32(3) & 0xF;
    uint32_t answer = buttons == 4 || buttons == 3 ? (g_headless ? 7u : 6u) : 1u;
    fprintf(stderr, "[messagebox] %s: %s -> %u\n", ASTR(2) ? ASTR(2) : "", ASTR(1) ? ASTR(1) : "", answer);
    RET(answer, 4);
}

/* The eON build of ColdWarAssault.exe has six KERNEL32 import names rotated
 * among themselves (the hints are intact, and the call sites agree with the
 * hints: e.g. the CRT's _ioinit calls the slot named GetFileTime with one
 * handle). Rebind those IAT slots to the function the code really calls. */
static void fix_swapped_imports(void) {
    static const struct { uint32_t slot; const char *named, *real; } fix[] = {
        { 0x007001C8, "DeleteFileA", "CloseHandle" },   /* hint 27 */
        { 0x00700258, "CloseHandle", "CreateFileA" },   /* hint 52 */
        { 0x00700254, "CreateFileA", "DeleteFileA" },   /* hint 87 */
        { 0x007000E8, "SetFileTime", "GetFileSize" },   /* hint 274 */
        { 0x00700244, "GetFileType", "GetFileTime" },   /* hint 276 */
        { 0x00700268, "GetFileTime", "GetFileType" },   /* hint 277 */
    };
    for (unsigned i = 0; i < sizeof fix / sizeof fix[0]; i++) {
        if (MEM32(fix[i].slot) != hle_resolve(fix[i].named)) {
            fprintf(stderr, "import slot 0x%08X is not %s: not rebinding\n", fix[i].slot, fix[i].named);
            continue;
        }
        MEM32(fix[i].slot) = hle_resolve_or_stub(fix[i].real);
    }
}

/* SetClassLongA(hwnd, GCL_HICON, icon): the window icon; nothing to keep. */
static void ofp_SetClassLongA(void) { RET(0u, 3); }
/* The game's profiler/crash sampler suspends a thread and reads its context;
 * a lifted thread has no Win32 context to give, so it reports failure. */
static void ofp_SuspendThread(void) { RET(0u, 1); }
static void ofp_GetThreadContext(void) { RET(0u, 2); }

static const win32hle_shim g_ofp_shims[] = {
    { "MessageBoxA", ofp_MessageBoxA },
    { "SetClassLongA", ofp_SetClassLongA },
    { "SuspendThread", ofp_SuspendThread },
    { "GetThreadContext", ofp_GetThreadContext },
    { 0, 0 }
};

/* ---- faults and the watchdog ---------------------------------------------- */
static void report_to(FILE *out, const char *why) {
    fprintf(out, "\n=== %s ===\n  in lifted sub_%08X, last import %s\n", why, g_cur_func,
            g_cur_import ? g_cur_import : "(none)");
    fprintf(out, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    fprintf(out, "last indirect calls (newest first):\n");
    for (int i = 1; i <= 30 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char *nm = hle_name(g_icall_trace[k]);
        fprintf(out, "  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    fprintf(out, "guest stack at %08X:", g_esp);
    for (int k = 0; k < 32; k++) fprintf(out, "%s%08X", k % 8 ? " " : "\n  ", MEM32(g_esp + 4 * k));
    fprintf(out, "\n");
}

static void report(const char *why) {
    report_to(stderr, why);
    if (perf_file()) { report_to(perf_file(), why); fflush(perf_file()); }
}
static void on_fault(int sig, siginfo_t *si, void *uc) {
    static volatile int once;
    if (__sync_lock_test_and_set(&once, 1)) _exit(3);
    char why[256];
#ifdef __x86_64__
    unsigned long pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#else
    unsigned long pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext.gregs[REG_EIP];
#endif
    const char *in = hle_in_shim_main >= 0 ? hle_shim_name_at(hle_in_shim_main) : NULL;
    snprintf(why, sizeof why, "fault: signal %d, address %p, host pc 0x%lx (%s)", sig, si->si_addr, pc,
             in ? in : "in lifted code");
    report(why);
    _exit(3);
}

static void *watchdog(void *arg) {
    (void)arg;
    sleep(g_watchdog_s);
    fprintf(stderr, "\n[watchdog] %u s: in sub_%08X, last import %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import ? g_cur_import : "(none)", g_icall_count);
    hle_exit(4);
    return NULL;
}

static uint32_t exe_stamp(const char *path) {
    uint8_t head[4096] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    uint32_t nt = n >= 0x40 ? *(const uint32_t *)(head + 0x3C) : 0;
    return nt && nt < sizeof head - 12 ? *(const uint32_t *)(head + nt + 8) : 0;
}

/* Without ColdWarAssault.cfg the game starts its Windows settings tool
 * (ColdWarAssaultPreferences.exe) and quits: write the settings that tool
 * would, for a 1024x768 window. The game then keeps them up to date. */
static void write_default_cfg(const char *game_full) {
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/ColdWarAssault.cfg", game_full);
    if (access(path, F_OK) == 0) return;
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fputs("3D_Performance=\"28800\";\nAdapter=0;\nAnimated_Textures=64;\nCockpit_Textures=4096;\n"
          "CPU_Benchmark=24000;\nEON_AspectRatio=\"4:3\";\nEON_MonitorIndex=0;\nFile_Heap=\"16\";\n"
          "Frame_Rate_Pref=500;\nHW_Type=\"Direct3D HW T&L\";\nLandscape_Textures=4096;\n"
          "Language=\"English\";\nLight_Explo=1;\nLight_Missile=1;\nLight_Static=1;\n"
          "Limit_LOD=0.020000;\nLOD=7.500000;\nMaxLights=32;\nMaxObjects=256;\n"
          "Object_Textures=4096;\nProduct=\"ColdWarAssault\";\nQuality_Pref=500;\nrefresh=60;\n"
          "Resolution_Bpp=\"32\";\nResolution_H=\"768\";\nResolution_W=\"1024\";\n"
          "Shadows_LOD=0.050000;\nTexture_Heap=\"16\";\nTextures_Drop_Down=4;\n"
          "Total_Memory=\"1024 MB\";\n", f);
    fclose(f);
    printf("  wrote default settings: %s\n", path);
}

int main(int argc, char **argv) {
    const char *game = "game";
    int run = 0;
    char gargs[2048] = "";                   /* after "--": the game's own command line (-connect=..., -nosplash) */
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) g_fullscreen = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else if (!strcmp(argv[i], "--")) {
            for (int k = i + 1; k < argc; k++) {        /* quoted when it has a space, as Windows splits them */
                size_t o = strlen(gargs);
                snprintf(gargs + o, sizeof gargs - o, strchr(argv[k], ' ') ? " \"%s\"" : " %s", argv[k]);
            }
            break;
        }
        else {
            printf("usage: ofp [--run] [--headless | --fullscreen] [--game DIR] [--watchdog S] [--trace] [-- game arguments]\n");
            return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 1;
        }
    }
    options_load();                          /* Options.txt, even without play.sh */
    char game_full[PATH_MAX], exe_host[PATH_MAX + 32];
    if (!realpath(game, game_full)) { fprintf(stderr, "no game folder at %s\n", game); return 1; }
    hle_set_drive('C', game_full);
    perf_init(game_full, argc, argv);
    if (!hle_host_path("C:\\ColdWarAssault.exe", exe_host, sizeof exe_host)) {
        fprintf(stderr, "no ColdWarAssault.exe in %s\n", game_full);
        return 1;
    }
    uint32_t stamp = exe_stamp(exe_host);
    if (stamp != OFP_EXE_STAMP) {
        fprintf(stderr, "%s: PE timestamp 0x%08X, this host was lifted from 0x%08X\n", exe_host, stamp, OFP_EXE_STAMP);
        return 1;
    }
    write_default_cfg(game_full);

    signal(SIGPIPE, SIG_IGN);
    {
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
    }

    win32hle_register(g_ofp_shims);    /* first: the game's own answers win */
    perf_register_shims();             /* timed CreateFileA/ReadFile around win32hle's */
    ofp_net_register();                /* GameSpy's master server: the community's */
    overlay_init();                    /* the recomp's options (Pause / Ctrl+O) */
    overlay_register();
    ofp_register_shims();              /* d3d8, dinput8, IFC22, ijl15 (ofp_*.c) */
    recomp_host_init();
    printf("Operation Flashpoint recomp host (native Linux)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    pe_image img;
    if (recomp_pe_map(exe_host, &img) != 0 || img.base != OFP_IMAGE_BASE) {
        fprintf(stderr, "cannot map %s at 0x%08X\n", exe_host, OFP_IMAGE_BASE);
        return 1;
    }
    hle_module_add("C:\\ColdWarAssault.exe", img.base, img.resource_rva, img.resource_size, 1);
    int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
#ifndef HLE_FLAT
    /* A 64-bit host gives the game the 4 GB a large-address-aware program
     * gets on a 64-bit Windows (OFP_LARGE_ADDRESS=0: 2 GB, as it was built). */
    if (ofp_env_int("OFP_LARGE_ADDRESS", 1)) hle_vm_any_hi = 0xFFF00000u;
    /* The game's own memory pool: the smaller of the memory GlobalMemoryStatus
     * reports and the cap at 0x756E20, 1 GB (in KB); a long view distance or
     * a big mission runs out of it ("Out of reserved memory"). OFP_HEAP_MB
     * (Options.txt MEMOIRE_JEU_MO) raises the cap: 1536 by default with the
     * 4 GB, as there is then room for it. */
    {
        int mb = ofp_env_int("OFP_HEAP_MB", hle_vm_any_hi > 0x80000000u ? 1536 : 1024);
        if (mb < 1024) mb = 1024;
        if (mb > 2000) mb = 2000;
        if (MEM32(0x00756E20u) == 0x100000u) MEM32(0x00756E20u) = (uint32_t)mb * 1024u;
    }
#endif
    fix_swapped_imports();
    printf("  mapped %s: 0x%08X-0x%08X, %d imports not implemented yet\n", exe_host, img.base, img.base + img.span, unresolved);
    static char cmdline[2200];
    snprintf(cmdline, sizeof cmdline, "\"C:\\ColdWarAssault.exe\"%s", gargs);
    hle_set_command_line(cmdline);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", ofp_entry_va);
        return 0;
    }
    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    /* the renderer: OpenGL (d3d8_gl.c), or Vulkan (d3d8_vk.c) when asked
     * (OFP_RENDERER=vulkan) and the machine has it */
    hle_screen_gl = 1;
    {
        const char *r = getenv("OFP_RENDERER");
        if (r && !strcasecmp(r, "vulkan")) {
            if (d3d8_vk_available()) hle_screen_gl = 2, d3d8_use_vulkan(1);
            else perf_note("Vulkan asked, not available here: OpenGL");
        }
        overlay_renderer(hle_screen_gl == 2);
    }
    if (hle_screen_open("Operation Flashpoint", g_fullscreen, g_headless) != 0) return 1;
    if (g_watchdog_s) {
        pthread_t t;
        pthread_create(&t, NULL, watchdog, NULL);
        pthread_detach(t);
    }
    printf("  entering 0x%08X\n\n", ofp_entry_va);
    fflush(stdout);
    uint32_t rc = hle_call_guest(ofp_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", rc);
    hle_exit((int)rc);
    return 0;
}
