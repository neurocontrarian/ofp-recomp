/*
 * ifc22.c - IFC22.dll, Immersion's force-feedback mouse classes, as a mouse
 * that is not there. The game imports the classes' methods directly (MSVC
 * __thiscall: `this` in ecx, the callee pops its stack arguments, counted
 * from each mangled name). Constructors return `this`; every Initialize
 * fails, so the game runs without force feedback; the rest do nothing.
 */
#include <stdint.h>
#include "win32hle.h"
#include "ofp.h"

static void ctor0(void) { RET(g_ecx, 0); }
static void ctor1(void) { RET(g_ecx, 1); }
static void void0(void) { RETV(0); }
static void zero0(void) { RET(0, 0); }
static void zero1(void) { RET(0, 1); }
static void zero2(void) { RET(0, 2); }
static void zero3(void) { RET(0, 3); }
static void zero4(void) { RET(0, 4); }
static void zero8(void) { RET(0, 8); }
static void zero9(void) { RET(0, 9); }
static void zero10(void) { RET(0, 10); }
static void guid_by_value(void) {                   /* (hidden return pointer): a zero GUID */
    uint32_t p = A32(0);
    for (int i = 0; i < 16; i += 4) MEM32(p + (uint32_t)i) = 0;
    RET(p, 1);
}

const win32hle_shim ofp_ifc22_shims[] = {
    { "?ChangePeriod@CImmPeriodic@@QAEHK@Z", zero1 },
    { "?ChangeParameters@CImmPeriodic@@QAEHKKKJJJKPAUFEELIT_ENVELOPE@@@Z", zero8 },
    { "??1CImmPeriodic@@UAE@XZ", void0 },
    { "??1CImmMouse@@UAE@XZ", void0 },
    { "??0CImmMouse@@QAE@XZ", ctor0 },
    { "?Initialize@CImmMouse@@QAEHPAX0KH@Z", zero4 },
    { "??0CImmPeriodic@@QAE@ABU_GUID@@@Z", ctor1 },
    { "?GetIsCompatibleGUID@CImmPeriodic@@UAEHAAU_GUID@@@Z", zero1 },
    { "?GetEffectType@CImmPeriodic@@UAEKXZ", zero0 },
    { "?Initialize@CImmPeriodic@@UAEHPAVCImmDevice@@ABUFEELIT_EFFECT@@K@Z", zero3 },
    { "?InitializeFromProject@CImmEffect@@UAEHAAVCImmProject@@PBDPAVCImmDevice@@K@Z", zero4 },
    { "?Start@CImmEffect@@UAEHKKH@Z", zero3 },
    { "?Stop@CImmEffect@@UAEHXZ", zero0 },
    { "?Unload@CImmEffect@@UAEJXZ", zero0 },
    { "?Reload@CImmEffect@@UAEXXZ", void0 },
    { "?buffer_ifr_data@CImmPeriodic@@MAEHPAD@Z", zero1 },
    { "?get_ffe_data@CImmPeriodic@@MAEHPAUDIEFFECT@@@Z", zero1 },
    { "?Initialize@CImmPeriodic@@UAEHPAVCImmDevice@@KKKJJJKPAUFEELIT_ENVELOPE@@K@Z", zero10 },
    { "?InitializePolar@CImmPeriodic@@UAEHPAVCImmDevice@@KKKJJKPAUFEELIT_ENVELOPE@@K@Z", zero9 },
    { "?GetAPI@CImmMouse@@UAEPAUIFeelit@@XZ", zero0 },
    { "?GetDevice@CImmMouse@@UAEPAUIFeelitDevice@@XZ", zero0 },
    { "?GetProductType@CImmMouse@@UAEKXZ", zero0 },
    { "?GetDriverVersion@CImmMouse@@UAEHAAK00@Z", zero3 },
    { "?GetProductName@CImmMouse@@UAEHPADH@Z", zero2 },
    { "?GetProductGUIDString@CImmMouse@@UAEHPADH@Z", zero2 },
    { "?GetProductGUID@CImmMouse@@UAE?AU_GUID@@XZ", guid_by_value },
    { "?GetCurrentPosition@CImmMouse@@UAEHAAJ0@Z", zero2 },
    { "?ChangeScreenResolution@CImmMouse@@UAEHHKK@Z", zero3 },
    { "?SwitchToAbsoluteMode@CImmMouse@@UAEHH@Z", zero1 },
    { "?prepare_device@CImmMouse@@MAEHXZ", zero0 },
    { "?reset@CImmMouse@@MAEXXZ", void0 },
    { "?ChangeMagnitude@CImmPeriodic@@QAEHK@Z", zero1 },
    { 0, 0 }
};
