/*
 * ijl15.c - the Intel JPEG Library 1.5 calls the game makes (ijlInit,
 * ijlRead, ijlFree), on stb_image. The game's TextureSourceJPEG reads a
 * JPEG's header (READPARAMS) for its size, then the whole image into its own
 * buffer (READWHOLEIMAGE), from a file or from memory.
 *
 * JPEG_CORE_PROPERTIES, the fields used (ijl.h 1.5):
 *   +4 DIBBytes  +8 DIBWidth  +12 DIBHeight (negative: top-down)  +16 DIBPadBytes
 *   +20 DIBChannels  +24 DIBColor  +32 JPGFile  +36 JPGBytes  +40 JPGSizeBytes
 *   +44 JPGWidth  +48 JPGHeight  +52 JPGChannels  +56 JPGColor
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#if __has_include("../third_party/stb_image.h")
#include "../third_party/stb_image.h"
#else
#include <stb/stb_image.h>                 /* libstb-dev */
#endif
#include "win32hle.h"
#include "ofp.h"

#define IJL_OK 0
#define IJL_INVALID_JPEG_PROPERTIES (-2)
#define IJL_BAD_JPEG_FILE (-11)            /* "IJL_ERR_FILECLOSE"-ish: any failure the game reports */

/* the whole JPEG, from the guest's buffer (an odd iotype: IJL_JBUFF_*) or
 * file (even: IJL_JFILE_*); malloc'd, *n its size */
static uint8_t *jpeg_bytes(uint32_t p, uint32_t io, int *n) {
    uint32_t file = MEM32(p + 32), bytes = MEM32(p + 36), size = MEM32(p + 40);
    if (io & 1) {
        if (!bytes || !size) return NULL;
        uint8_t *b = (uint8_t *)malloc(size);
        if (!b) return NULL;
        memcpy(b, hle_ptr(bytes), size);
        *n = (int)size;
        return b;
    }
    if (!file) return NULL;
    char host[4096];
    if (!hle_host_path((const char *)hle_ptr(file), host, sizeof host)) return NULL;
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(len > 0 ? (size_t)len : 1);
    *n = (int)fread(b, 1, len > 0 ? (size_t)len : 0, f);
    fclose(f);
    return b;
}

static void ijlInit(void) {                         /* (props) */
    uint32_t p = A32(0);
    memset(hle_ptr(p), 0, 84);
    MEM32(p + 20) = 3, MEM32(p + 24) = 2;              /* DIBChannels 3, DIBColor BGR: the library's defaults */
    MEM32(p + 52) = 3, MEM32(p + 56) = 3;              /* JPGChannels 3, YCbCr */
    RET(IJL_OK, 1);
}
static void ijlFree(void) { RET(IJL_OK, 1); }
static void ijlRead(void) {                         /* (props, iotype) */
    uint32_t p = A32(0), io = A32(1);
    int n = 0;
    uint8_t *jpg = jpeg_bytes(p, io, &n);
    if (!jpg) RET(IJL_BAD_JPEG_FILE, 2);
    int w, h, ch;
    if (!stbi_info_from_memory(jpg, n, &w, &h, &ch)) { free(jpg); RET(IJL_BAD_JPEG_FILE, 2); }
    MEM32(p + 44) = (uint32_t)w, MEM32(p + 48) = (uint32_t)h, MEM32(p + 52) = (uint32_t)ch;
    MEM32(p + 56) = ch == 1 ? 4u : 3u;
    if (io == 0 || io == 1 || io == 4 || io == 5) { free(jpg); RET(IJL_OK, 2); }   /* the header only */
    uint32_t dib = MEM32(p + 4), dw = MEM32(p + 8), pad = MEM32(p + 16), dch = MEM32(p + 20), dcol = MEM32(p + 24);
    int dh = (int)MEM32(p + 12), top_down = dh < 0;
    if (dh < 0) dh = -dh;
    if (!dib || !dw || !dh || !dch || dch > 4) { free(jpg); RET(IJL_INVALID_JPEG_PROPERTIES, 2); }
    int sc = dch == 1 ? 1 : dch == 4 ? 4 : 3, got;     /* the channels stb hands back */
    uint8_t *px = stbi_load_from_memory(jpg, n, &w, &h, &got, sc);
    free(jpg);
    if (!px) RET(IJL_BAD_JPEG_FILE, 2);
    uint32_t pitch = dw * dch + pad;
    for (int y = 0; y < dh && y < h; y++) {
        uint8_t *row = (uint8_t *)hle_ptr(dib + pitch * (uint32_t)(top_down ? y : dh - 1 - y));
        const uint8_t *s = px + (size_t)y * (size_t)w * (size_t)sc;
        for (int x = 0; x < (int)dw && x < w; x++) {
            uint8_t r = s[x * sc], g = sc >= 3 ? s[x * sc + 1] : r, b = sc >= 3 ? s[x * sc + 2] : r;
            uint8_t *d = row + (size_t)x * dch;
            if (dch == 1) d[0] = r;
            else if (dcol == 1) { d[0] = r, d[1] = g, d[2] = b; if (dch == 4) d[3] = 255; }   /* IJL_RGB */
            else { d[0] = b, d[1] = g, d[2] = r; if (dch == 4) d[3] = 255; }                  /* IJL_BGR */
        }
    }
    stbi_image_free(px);
    RET(IJL_OK, 2);
}

const win32hle_shim ofp_ijl15_shims[] = {
    { "ijlInit", ijlInit }, { "ijlFree", ijlFree }, { "ijlRead", ijlRead },
    { 0, 0 }
};
