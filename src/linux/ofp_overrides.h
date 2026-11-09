/*
 * ofp_overrides.h - force-included into the generated files that define a
 * lifted function replaced by a hand-written one (CMakeLists.txt, with
 * OFP_GEN_FILE = 1 followed by the file's number). --wrap only reaches calls
 * from other files; these functions are also called from their own. Here the
 * lifted definition is made weak, so the strong one in ofp_native.c wins for
 * every call, the dispatch table included, and the lifted body stays
 * reachable as lifted_sub_XXXXXXXX (an alias of the same code).
 */

#include <stdint.h>

#if OFP_GEN_FILE == 10051
_Pragma("weak sub_00671950")
void lifted_sub_00671950(void) __attribute__((alias("sub_00671950")));
#endif

#if OFP_GEN_FILE == 10054
_Pragma("weak sub_006A2660")
void lifted_sub_006A2660(void) __attribute__((alias("sub_006A2660")));
#endif

/* the hot regions of lifted functions run natively when this says so
 * (ofp_native.c; run_lift.py's region_hook, src/linux/ofp_region_*.c) */
int ofp_region_on(uint32_t start);
