/* Operation Flashpoint host: the game-specific shim modules. */
#pragma once
#include "win32hle.h"

/* Registers the DLLs Windows would have found next to the game or in
 * System32 and win32hle does not answer: d3d8, dinput8, IFC22 (force
 * feedback), ijl15 (JPEG). ofp_shims.c */
void ofp_register_shims(void);

extern int hle_screen_gl;                              /* screen.c: an OpenGL window (d3d8_gl.c draws) */
extern const win32hle_shim ofp_d3d8_shims[];           /* d3d8_gl.c */
extern const win32hle_shim ofp_dinput8_shims[];         /* dinput8.c */
extern const win32hle_shim ofp_ifc22_shims[];           /* ifc22.c */
extern const win32hle_shim ofp_ijl15_shims[];           /* ijl15.c */

/* perf.c: the performance log */
#include <stdio.h>
void perf_init(const char *game_dir, int argc, char **argv);
void ofp_file_opened(const char *name);                 /* each file the game opens (ofp_native.c) */
void perf_register_shims(void);                         /* before win32hle's own */
void perf_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void perf_frame(unsigned draws);
void perf_texture(unsigned bytes);
/* the renderer's counters (perf.c writes them per second): time in draws, bytes streamed,
 * buffer copies (bytes, new ones), descriptor pushes, uniform blocks, pipeline binds */
enum { RC_NS, RC_STREAM, RC_COPY, RC_NEWBUF, RC_PUSH, RC_UBO, RC_PIPE, RC_NEWPIPE, RC_NEWPIPE_NS, RC_N };
extern volatile uint64_t perf_rc[RC_N];
FILE *perf_file(void);                                  /* NULL when off */
void ofp_net_register(void);                            /* ofp_net.c: master server */

/* overlay.c: the recomp's options over the game (Pause or Ctrl+O) */
void options_load(void);                         /* Options.txt into the environment (first in main) */
void overlay_init(void);
void overlay_register(void);
void overlay_open(void);                         /* the Options screen's "Linux options" button */
void overlay_draw(int w, int h);                 /* d3d8_gl.c's present, into the back buffer */
/* the overlay's pictures this frame (Vulkan's present): RGBA rows at x, y of a w x h back buffer */
typedef struct { const void *rgba; int pitch, w, h, x, y, id; unsigned ver; } overlay_layer_t;
int  overlay_layers(int w, int h, overlay_layer_t *out);   /* out[2]; how many */
void overlay_renderer(int vulkan);               /* the renderer running (the options show it) */
int  overlay_blocks_input(void);                 /* dinput8.c: the options have the keys */
int  overlay_vsync(void);
int  overlay_msaa(void);
void d3d8_set_msaa(void);                        /* d3d8_gl.c: applied at once (overlay_msaa) */
void d3d8_set_vsync(int on);
void d3d8_use_vulkan(int on);                    /* d3d8.c: the Vulkan renderer (before the device) */
int  d3d8_vk_available(void);                    /* d3d8_vk.c: a Vulkan 1.3 device to draw with */

/* ofp_shims.c: small shared helpers */
int ofp_env_int(const char *name, int dflt);     /* an integer environment variable; dflt if unset or empty */
recomp_func_t ofp_find_shim(const win32hle_shim *table, const char *name);   /* a runtime shim, to wrap */
