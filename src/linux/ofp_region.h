/*
 * ofp_region.h - for the C that tools/region_c.py writes: a hot region of a
 * lifted function with its stack slots and x87 values in host variables.
 * Registers live in locals named after them; REGS_OUT/REGS_IN move them to
 * and from the globals around calls (esp: the region's, fixed by the tool).
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "recomp_types.h"
#include "win32hle.h"

/* the regions need the x87 ring (RECOMP_X87_RING, the 64-bit build): without
 * it they compile to stubs and ofp_region_on says no */
#ifdef RECOMP_X87_RING
#define OFP_REGIONS 1
#endif

/* float bits, as recomp_types.h's SSE helpers */
#define u2f sse_f32
#define f2u sse_u32
#define u2d sse_f64

/* OFP_NATIVE=check: esp is checked after every call (a callee that did not
 * pop what the tool expected) */
extern int ofp_region_check;
#define CHECK ofp_region_check
void region_bad(uint32_t call_va, uint32_t esp_off);

#define REGS_OUT(sp) (g_eax = eax, g_ecx = ecx, g_edx = edx, g_ebx = ebx, g_esi = esi, g_edi = edi, g_ebp = ebp, g_esp = (sp))
/* after a call: the registers back, and g_cur_func the region's function's
 * again (a callee's entry set it), as RECOMP_CALL does */
#define REGS_IN() (eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, g_cur_func = cf_)
/* a loop's back edge hands the machine over now and then, as RECOMP_BACKEDGE */
#define BACKEDGE() do { if (++g_backedges >= RECOMP_YIELD_EVERY && recomp_yield_hook) { \
        g_backedges = 0; REGS_OUT(E); recomp_yield_hook(); REGS_IN(); } } while (0)
/* fnstsw ax after fcom st(0), b: C0 (0x100) below, C2 (0x400) unordered,
 * C3 (0x4000) equal; unordered sets all three */
static inline uint32_t FSW(double a, double b) {
    if (a != a || b != b) return 0x4500u;
    return (a < b ? 0x100u : 0) | (a == b ? 0x4000u : 0);
}
/* an indirect call: its target through the call site's cache, as RECOMP_ICALL;
 * one that resolves to nothing is reported, its return address taken back
 * and eax 0, as RECOMP_ICALL does */
#define RCALL(va) do { uint32_t v_ = (va); recomp_func_t f_ = RECOMP_RESOLVE_SITE(v_); \
        if (f_) f_(); \
        else { fprintf(stderr, "region ICALL: unresolved VA 0x%08X from 0x%08X\n", v_, cf_); g_esp += 4; g_eax = 0; } } while (0)
/* the guest functions that return their one float argument, done inline */
#define IDENTITY_F1(va) ((va) == 0x00616698u)
