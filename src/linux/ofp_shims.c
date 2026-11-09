/* The DLLs this game needs beyond win32hle: registered ahead of win32hle's own. */
#include <stdlib.h>
#include <string.h>
#include "ofp.h"

void ofp_register_shims(void) {
    win32hle_register(ofp_d3d8_shims);
    win32hle_register(ofp_dinput8_shims);
    win32hle_register(ofp_ifc22_shims);
    win32hle_register(ofp_ijl15_shims);
}

int ofp_env_int(const char *name, int dflt) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : dflt;
}
recomp_func_t ofp_find_shim(const win32hle_shim *t, const char *name) {
    for (; t->name; t++) if (!strcmp(t->name, name)) return t->fn;
    return NULL;
}
