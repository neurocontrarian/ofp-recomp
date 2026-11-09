/*
 * overlay.c - the recomp's own options, over the game: Pause (or Ctrl+O)
 * opens them. Antialiasing, vertical sync, an images-a-second counter.
 * Up/Down choose, Left/Right/Enter change, Escape or Pause closes. A change
 * applies at once and is written to Options.txt (env OFP_OPTIONS).
 *
 * Drawn by the renderer's present into the game's finished picture (so it
 * scales with it): SDL_ttf renders the text into a picture; OpenGL shows it
 * as a texture (overlay_draw), Vulkan takes the pictures (overlay_layers).
 */
#define GL_GLEXT_PROTOTYPES 1
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "win32hle.h"
#include "ofp.h"

enum { OPT_MSAA, OPT_VSYNC, OPT_FPS, OPT_RENDER, OPT_N };
static const int k_msaa[] = { 0, 2, 4, 8 };
static int g_open, g_sel, g_msaa, g_vsync = 1, g_showfps, g_vulkan, g_vulkan_now;
static int g_hold;                                    /* after closing: input stays off until every key is up */
#define NMSAA (int)(sizeof k_msaa / sizeof k_msaa[0])
static TTF_Font *g_font;
static int g_font_px;
/* a text picture, remade only when its text changes (ver counts the changes);
 * OpenGL's texture of it, remade when tex_ver is behind */
typedef struct { SDL_Surface *pic; unsigned ver; GLuint tex; unsigned tex_ver; int w, h; char key[512]; } text_t;
static text_t g_menu_tx, g_fps_tx;

/* Options.txt, read by the program itself (not only by play.sh): OFP_OPTIONS,
 * else the one next to the program. Each NAME=value fills its environment
 * variable unless it was set by hand. Called first in main. The second name
 * is the one older packages wrote; it is still read, and replaced on save. */
static char g_opt_path[4096];
static const struct { const char *name, *old, *env; } k_opts[] = {
    { "ANTIALIASING", NULL, "OFP_MSAA" }, { "VSYNC", NULL, "OFP_VSYNC" },
    { "SHOW_FPS", "AFFICHER_FPS", "OFP_SHOWFPS" }, { "MASTER_SERVER", NULL, "OFP_MASTER" },
    { "INTERNET_SERVERS", "SERVEURS_INTERNET", "OFP_INTERNET_LIST" },
    { "GAME_MEMORY_MB", "MEMOIRE_JEU_MO", "OFP_HEAP_MB" }, { "VULKAN", "RENDU_VULKAN", "OFP_RENDERER" } };
#define NOPTS (int)(sizeof k_opts / sizeof k_opts[0])

/* a line "NAME = value": where the value starts, or NULL for another line */
static char *opt_match(char *line, const char *name) {
    char *p = line + strspn(line, " \t");
    size_t nl = strlen(name);
    if (strncmp(p, name, nl)) return NULL;
    p += nl;
    p += strspn(p, " \t");
    if (*p++ != '=') return NULL;
    return p + strspn(p, " \t");
}

void options_load(void) {
    const char *e = getenv("OFP_OPTIONS");
    if (e && *e) {                                     /* absolute: main enters the game folder later */
        char cwd[2048];
        if (*e != '/' && getcwd(cwd, sizeof cwd)) snprintf(g_opt_path, sizeof g_opt_path, "%s/%s", cwd, e);
        else snprintf(g_opt_path, sizeof g_opt_path, "%s", e);
    } else {
        char exe[4000];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) return;
        exe[n] = 0;
        char *s = strrchr(exe, '/');
        if (s) *s = 0;
        snprintf(g_opt_path, sizeof g_opt_path, "%s/Options.txt", exe);
    }
    FILE *f = fopen(g_opt_path, "r");
    if (!f) return;
    char line[1024], val[NOPTS][256] = { { 0 } };
    while (fgets(line, sizeof line, f))                /* the last line of a name wins */
        for (int i = 0; i < NOPTS; i++) {
            char *v = opt_match(line, k_opts[i].name);
            if (!v && k_opts[i].old) v = opt_match(line, k_opts[i].old);
            if (v) v[strcspn(v, " \t\r\n#")] = 0, snprintf(val[i], sizeof val[i], "%s", v);
        }
    fclose(f);
    for (int i = 0; i < NOPTS; i++) {
        const char *cur = getenv(k_opts[i].env);
        if ((cur && *cur) || !val[i][0]) continue;     /* set by hand: that wins */
        if (!strcmp(k_opts[i].env, "OFP_RENDERER")) { if (!strcmp(val[i], "1")) setenv("OFP_RENDERER", "vulkan", 1); }
        else setenv(k_opts[i].env, val[i], 1);
    }
}

void overlay_init(void) {
    perf_note("options file: %s%s", g_opt_path[0] ? g_opt_path : "(none)", g_opt_path[0] && access(g_opt_path, R_OK) ? " (absent)" : "");
    g_msaa = ofp_env_int("OFP_MSAA", 0);
    g_vsync = ofp_env_int("OFP_VSYNC", 1);            /* -1: adaptive */
    g_showfps = ofp_env_int("OFP_SHOWFPS", 0) != 0;
    const char *r = getenv("OFP_RENDERER");
    g_vulkan = r && !strcasecmp(r, "vulkan");
}
void overlay_renderer(int vulkan) { g_vulkan_now = vulkan; }
void overlay_open(void) { g_open = 1; }       /* the game's Options screen asked (ofp_native.c) */
int overlay_vsync(void) { return g_vsync; }
int overlay_msaa(void) { return g_msaa; }

/* the game's input is off while the options are open, and until every key and
 * button is up after: the Escape that closed them is not the game's */
int overlay_blocks_input(void) {
    if (g_open) return 1;
    if (!g_hold) return 0;
    int n = 0;
    const Uint8 *k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; i++) if (k[i]) return 1;
    if (SDL_GetMouseState(NULL, NULL)) return 1;
    g_hold = 0;
    return 0;
}

/* Options.txt: the line NAME=... replaced, or added */
static void save_option(const char *name, int value) {
    const char *path = g_opt_path;
    if (!*path) { perf_note("options: no Options.txt to save %s into", name); return; }
    FILE *f = fopen(path, "r");
    char *text = NULL;
    size_t len = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        text = (char *)malloc(n > 0 ? (size_t)n + 1 : 1);
        len = fread(text, 1, n > 0 ? (size_t)n : 0, f);
        text[len] = 0;
        fclose(f);
    }
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *o = fopen(tmp, "w");
    int direct = !o && (errno == EACCES || errno == EROFS || errno == EPERM);   /* folder not writable: the file itself */
    if (direct) o = fopen(path, "w");
    if (!o) { perf_note("options: cannot write %s: %s", path, strerror(errno)); free(text); return; }
    const char *old = NULL;
    for (int i = 0; i < NOPTS; i++)
        if (!strcmp(k_opts[i].name, name)) old = k_opts[i].old;
    int done = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (opt_match(line, name) || (old && opt_match(line, old))) {
            if (!done) fprintf(o, "%s=%d\n", name, value), done = 1;
        } else fprintf(o, "%s\n", line);
    }
    if (!done) fprintf(o, "%s=%d\n", name, value);
    int bad = ferror(o) ? (errno ? errno : EIO) : 0;
    if (fclose(o) && !bad) bad = errno;
    if (bad) {
        perf_note("options: writing %s failed: %s", direct ? path : tmp, strerror(bad));
        if (!direct) remove(tmp);
    } else if (!direct && rename(tmp, path)) perf_note("options: cannot replace %s: %s", path, strerror(errno)), remove(tmp);
    free(text);
}

static void change(int dir) {
    switch (g_sel) {
    case OPT_MSAA: {
        int i = 0;
        while (i < NMSAA - 1 && k_msaa[i] != g_msaa) i++;
        i = (i + dir + NMSAA) % NMSAA;
        g_msaa = k_msaa[i];
        d3d8_set_msaa();
        save_option("ANTIALIASING", g_msaa);
        break;
    }
    case OPT_VSYNC:
        g_vsync = !g_vsync;                            /* adaptive (-1) counts as on: off next */
        d3d8_set_vsync(g_vsync);
        save_option("VSYNC", g_vsync);
        break;
    case OPT_FPS:
        g_showfps = !g_showfps;
        save_option("SHOW_FPS", g_showfps);
        break;
    case OPT_RENDER:                                   /* at the next start */
        g_vulkan = !g_vulkan;
        save_option("VULKAN", g_vulkan);
        break;
    }
    perf_note("options: antialiasing %d, vsync %d, fps counter %d, Vulkan %d", g_msaa, g_vsync, g_showfps, g_vulkan);
}

static int on_key(int sc, int down) {
    int ctrl = (SDL_GetModState() & KMOD_CTRL) != 0;
    int toggle = sc == SDL_SCANCODE_PAUSE || (ctrl && sc == SDL_SCANCODE_O);
    if (toggle) {
        if (down) { g_open = !g_open; if (!g_open) g_hold = 1; }
        return 1;
    }
    if (!g_open) return 0;
    if (!down) return 1;
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: g_open = 0, g_hold = 1; break;
    case SDL_SCANCODE_UP: g_sel = (g_sel + OPT_N - 1) % OPT_N; break;
    case SDL_SCANCODE_DOWN: g_sel = (g_sel + 1) % OPT_N; break;
    case SDL_SCANCODE_LEFT: change(-1); break;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: change(1); break;
    default: break;
    }
    return 1;
}
static int (*g_next_key_hook)(int, int);
static int key_hook(int sc, int down) { return on_key(sc, down) || (g_next_key_hook && g_next_key_hook(sc, down)); }
void overlay_register(void) {
    g_next_key_hook = hle_screen_key_hook;
    hle_screen_key_hook = key_hook;
}

/* ---- drawing ---- */
static int font_for(int wh) {
    int px = wh / 36 < 14 ? 14 : wh / 36;
    if (g_font && px == g_font_px) return 1;
    if (TTF_WasInit() == 0 && TTF_Init() != 0) return 0;
    const char *path = hle_font_file("Arial", 1, 0);
    if (!path) return 0;
    if (g_font) TTF_CloseFont(g_font);
    g_font = TTF_OpenFont(path, px);
    g_font_px = px;
    return g_font != NULL;
}
/* lines of text into tx's RGBA picture, unless they are the ones it shows */
static int text_picture(text_t *tx, const char *const *lines, const int *lit, int n) {
    char key[sizeof tx->key];
    size_t o = (size_t)snprintf(key, sizeof key, "%d|", g_font_px);
    for (int i = 0; i < n && o < sizeof key; i++) o += (size_t)snprintf(key + o, sizeof key - o, "%d%s\n", lit ? lit[i] : 0, lines[i]);
    if (tx->pic && !strcmp(key, tx->key)) return 1;
    snprintf(tx->key, sizeof tx->key, "%s", key);
    SDL_Surface *s[8] = { 0 };
    int w = 0, h = 0, gap = g_font_px / 3;
    for (int i = 0; i < n && i < 8; i++) {
        SDL_Color c = lit && lit[i] ? (SDL_Color){ 255, 210, 60, 255 } : (SDL_Color){ 235, 235, 235, 255 };
        s[i] = TTF_RenderUTF8_Blended(g_font, lines[i][0] ? lines[i] : " ", c);
        if (!s[i]) continue;
        if (s[i]->w > w) w = s[i]->w;
        h += s[i]->h + gap;
    }
    int pad = g_font_px / 2;
    w += 2 * pad, h += 2 * pad;
    SDL_Surface *all = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!all) return 0;
    SDL_FillRect(all, NULL, SDL_MapRGBA(all->format, 10, 14, 10, 205));
    for (int i = 0, y = pad; i < n && i < 8; i++) {
        if (!s[i]) continue;
        SDL_Rect r = { pad, y, s[i]->w, s[i]->h };
        SDL_SetSurfaceBlendMode(s[i], SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(s[i], NULL, all, &r);
        y += s[i]->h + gap;
        SDL_FreeSurface(s[i]);
    }
    if (tx->pic) SDL_FreeSurface(tx->pic);
    tx->pic = all;
    tx->ver++;
    tx->w = w, tx->h = h;
    return 1;
}
/* tx's picture as a GL texture, bound */
static void text_bind(text_t *tx) {
    if (!tx->tex) glGenTextures(1, &tx->tex);
    glBindTexture(GL_TEXTURE_2D, tx->tex);
    if (tx->tex_ver == tx->ver) return;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, tx->pic->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tx->w, tx->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tx->pic->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    tx->tex_ver = tx->ver;
}
/* a rectangle in the back buffer, whose row 0 is the picture's top */
static void quad(int ww, int wh, int x, int y, int w, int h) {
    float x0 = 2.0f * x / ww - 1, x1 = 2.0f * (x + w) / ww - 1;
    float y0 = 2.0f * y / wh - 1, y1 = 2.0f * (y + h) / wh - 1;
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0), glVertex2f(x0, y0);
    glTexCoord2f(1, 0), glVertex2f(x1, y0);
    glTexCoord2f(1, 1), glVertex2f(x1, y1);
    glTexCoord2f(0, 1), glVertex2f(x0, y1);
    glEnd();
}

/* the frame counter: frames in the last second */
static uint32_t g_frames, g_fps, g_t0;

/* this frame's pictures: the options (centred), the counter (top left);
 * a bit each in the result */
static int build(int wh) {
    uint32_t now = SDL_GetTicks();
    g_frames++;
    if (now - g_t0 >= 1000) g_fps = g_frames * 1000u / (now - g_t0 ? now - g_t0 : 1), g_frames = 0, g_t0 = now;
    if (!g_open && !g_showfps) return 0;
    if (!font_for(wh)) return 0;
    int got = 0;
    if (g_open) {
        char l[OPT_N + 3][96];
        snprintf(l[0], sizeof l[0], "Options of the recompiled version");
        snprintf(l[1], sizeof l[1], "%s", "");
        snprintf(l[2], sizeof l[2], "Antialiasing (smoother edges):  %s", g_msaa ? (g_msaa == 2 ? "2x" : g_msaa == 4 ? "4x" : "8x") : "off");
        snprintf(l[3], sizeof l[3], "Vertical sync:  %s", g_vsync ? "on (at most the screen's refresh rate)" : "off (more frames per second)");
        snprintf(l[4], sizeof l[4], "Frames per second counter:  %s", g_showfps ? "shown" : "hidden");
        snprintf(l[5], sizeof l[5], "Renderer:  %s%s", g_vulkan ? "Vulkan" : "OpenGL",
                 g_vulkan != g_vulkan_now ? "  (from the next start)" : "");
        const char *lines[8] = { l[0], l[1], l[2], l[3], l[4], l[5], "", "Up/Down: choose   Left/Right: change   Esc: close" };
        int lit[8] = { 0 };
        lit[2 + g_sel] = 1;                            /* the options are lines 2.. */
        if (text_picture(&g_menu_tx, lines, lit, 8)) got |= 1;
    }
    if (g_showfps) {
        char f[32];
        snprintf(f, sizeof f, "%u fps", g_fps);
        const char *lines[1] = { f };
        if (text_picture(&g_fps_tx, lines, NULL, 1)) got |= 2;
    }
    return got;
}
int overlay_layers(int ww, int wh, overlay_layer_t *out) {
    int got = build(wh), n = 0;
    text_t *t[2] = { &g_menu_tx, &g_fps_tx };
    for (int i = 0; i < 2; i++) {
        if (!(got & (1 << i))) continue;
        overlay_layer_t *o = &out[n++];
        o->rgba = t[i]->pic->pixels, o->pitch = t[i]->pic->pitch, o->w = t[i]->w, o->h = t[i]->h, o->ver = t[i]->ver;
        o->id = i;
        o->x = i ? 8 : (ww - t[i]->w) / 2, o->y = i ? 8 : (wh - t[i]->h) / 2;
    }
    return n;
}

void overlay_draw(int ww, int wh) {
    int got = build(wh);
    if (!got) return;

    /* the game's GL state, kept (the attribute stack holds the viewport,
     * texture units and bindings; the program is not in it) */
    GLint prog = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glUseProgram(0);
    for (int m = 0; m < 3; m++) {
        glMatrixMode(m == 0 ? GL_PROJECTION : m == 1 ? GL_MODELVIEW : GL_TEXTURE);
        glPushMatrix();
        glLoadIdentity();
    }
    glViewport(0, 0, ww, wh);
    glDisable(GL_DEPTH_TEST), glDisable(GL_CULL_FACE), glDisable(GL_LIGHTING), glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST), glDisable(GL_SCISSOR_TEST), glDisable(GL_STENCIL_TEST);
    for (int u = 1; u < 4; u++) { glActiveTexture(GL_TEXTURE0 + u); glDisable(GL_TEXTURE_2D); }
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(1, 1, 1, 1);

    if (got & 1) text_bind(&g_menu_tx), quad(ww, wh, (ww - g_menu_tx.w) / 2, (wh - g_menu_tx.h) / 2, g_menu_tx.w, g_menu_tx.h);
    if (got & 2) text_bind(&g_fps_tx), quad(ww, wh, 8, 8, g_fps_tx.w, g_fps_tx.h);

    for (int m = 2; m >= 0; m--) {
        glMatrixMode(m == 0 ? GL_PROJECTION : m == 1 ? GL_MODELVIEW : GL_TEXTURE);
        glPopMatrix();
    }
    glPopClientAttrib();
    glPopAttrib();
    glUseProgram((GLuint)prog);
}
