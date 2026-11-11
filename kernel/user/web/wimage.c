/* Web engine images — see wimage.h. */
#include "wimage.h"
#include "wmem.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 16384
#define STBI_ASSERT(x) ((void)0)
#define STBI_MALLOC(sz) wmem_alloc(sz)
#define STBI_REALLOC(p, newsz) wmem_realloc(p, newsz)
#define STBI_FREE(p) wmem_free(p)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wcast-qual"
#include "../../../vendor/stb/stb_image.h"
#pragma clang diagnostic pop

static const char *failure = "";

int wimage_sniff(const uint8_t *d, size_t n) {
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return WIMG_PNG;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return WIMG_JPEG;
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') return WIMG_GIF;
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return WIMG_BMP;
    if (n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' && d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P') return WIMG_WEBP;
    if (n >= 12 && d[4] == 'f' && d[5] == 't' && d[6] == 'y' && d[7] == 'p' && d[8] == 'a' && d[9] == 'v' && d[10] == 'i') return WIMG_AVIF;
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0) return WIMG_ICO;
    for (size_t i = 0; i < n && i < 512u; ++i) {                    /* "<svg" or "<?xml" near the start */
        if (d[i] == '<' && i + 4 < n && (d[i + 1] == 's' || d[i + 1] == 'S') && (d[i + 2] == 'v' || d[i + 2] == 'V') && (d[i + 3] == 'g' || d[i + 3] == 'G')) return WIMG_SVG;
    }
    return WIMG_UNKNOWN;
}

const char *wimage_format_name(int f) {
    switch (f) {
    case WIMG_PNG: return "PNG"; case WIMG_JPEG: return "JPEG"; case WIMG_GIF: return "GIF"; case WIMG_BMP: return "BMP";
    case WIMG_SVG: return "SVG"; case WIMG_WEBP: return "WebP"; case WIMG_ICO: return "ICO"; case WIMG_AVIF: return "AVIF";
    default: return "unknown";
    }
}

int wimage_info(const uint8_t *data, size_t len, int *w, int *h) {
    int comp = 0;
    if (len > 0x7FFFFFFFu) return 0;
    return stbi_info_from_memory(data, (int)len, w, h, &comp) ? 1 : 0;
}

uint8_t *wimage_decode(const uint8_t *data, size_t len, int *w, int *h, size_t max_pixels) {
    int fmt = wimage_sniff(data, len);
    *w = *h = 0;
    if (fmt != WIMG_PNG && fmt != WIMG_JPEG && fmt != WIMG_GIF && fmt != WIMG_BMP) {
        failure = fmt == WIMG_UNKNOWN ? "not an image" : "image format not supported";
        return 0;
    }
    int iw = 0, ih = 0;
    if (!wimage_info(data, len, &iw, &ih) || iw <= 0 || ih <= 0) { failure = "corrupt image header"; return 0; }
    if ((size_t)iw * (size_t)ih > max_pixels) { failure = "image too large"; return 0; }
    int comp = 0;
    uint8_t *px = stbi_load_from_memory(data, (int)len, &iw, &ih, &comp, 4);
    if (!px) { failure = stbi_failure_reason() ? stbi_failure_reason() : "decode failed"; return 0; }
    *w = iw; *h = ih;
    failure = "";
    return px;
}

const char *wimage_failure(void) { return failure; }

/* ------------------------------------------------------------------ resampling */
/* One source row (straight RGBA) -> dw premultiplied channels x256 precision. */
static void hpass(const uint8_t *row, int sw, int dw, uint32_t *out) {
    if (dw <= sw) {
        for (int d = 0; d < dw; ++d) {
            uint64_t start = (uint64_t)d * (uint64_t)sw * 65536u / (uint64_t)dw;
            uint64_t end = (uint64_t)(d + 1) * (uint64_t)sw * 65536u / (uint64_t)dw;
            uint64_t acc[4] = {0, 0, 0, 0}, wsum = end - start;
            for (uint64_t i = start >> 16; i < ((end + 65535u) >> 16) && i < (uint64_t)sw; ++i) {
                uint64_t lo = start > (i << 16) ? start : (i << 16);
                uint64_t hi = end < ((i + 1) << 16) ? end : ((i + 1) << 16);
                if (hi <= lo) continue;
                uint64_t wgt = hi - lo;
                const uint8_t *p = row + i * 4u;
                uint32_t a = p[3];
                acc[0] += wgt * ((p[0] * a + 127u) / 255u);
                acc[1] += wgt * ((p[1] * a + 127u) / 255u);
                acc[2] += wgt * ((p[2] * a + 127u) / 255u);
                acc[3] += wgt * a;
            }
            if (!wsum) wsum = 1;
            for (int k = 0; k < 4; ++k) out[d * 4 + k] = (uint32_t)(acc[k] * 256u / wsum);
        }
        return;
    }
    for (int d = 0; d < dw; ++d) {
        int64_t pos = (int64_t)(((uint64_t)(2 * d + 1) * (uint64_t)sw * 65536u) / (uint64_t)(2 * dw)) - 32768;
        if (pos < 0) pos = 0;
        int i0 = (int)(pos >> 16), i1 = i0 + 1 < sw ? i0 + 1 : i0;
        if (i0 >= sw) i0 = i1 = sw - 1;
        uint32_t f = (uint32_t)(pos & 0xFFFF);
        const uint8_t *p0 = row + i0 * 4, *p1 = row + i1 * 4;
        uint32_t a0 = p0[3], a1 = p1[3];
        uint32_t c0[4] = {(p0[0] * a0 + 127u) / 255u, (p0[1] * a0 + 127u) / 255u, (p0[2] * a0 + 127u) / 255u, a0};
        uint32_t c1[4] = {(p1[0] * a1 + 127u) / 255u, (p1[1] * a1 + 127u) / 255u, (p1[2] * a1 + 127u) / 255u, a1};
        for (int k = 0; k < 4; ++k) out[d * 4 + k] = (uint32_t)(((uint64_t)c0[k] * (65536u - f) + (uint64_t)c1[k] * f) >> 8);
    }
}

uint32_t *wimage_scale(const uint8_t *rgba, int sw, int sh, int dw, int dh) {
    if (!rgba || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || (size_t)dw * (size_t)dh > (size_t)1 << 26) return 0;
    uint32_t *out = (uint32_t *)wmem_alloc((size_t)dw * (size_t)dh * 4u);
    uint32_t *rows = (uint32_t *)wmem_alloc((size_t)dw * 64u);          /* two cached rows (2 x dw x 4 x u32) + accumulator (dw x 4 x u64) */
    if (!out || !rows) { wmem_free(out); wmem_free(rows); return 0; }
    uint32_t *cache[2] = {rows, rows + dw * 4};
    int cached[2] = {-1, -1}, next = 0;
    uint64_t *acc = (uint64_t *)(rows + dw * 8);                         /* dw * 4 uint64 = dw * 8 uint32 */
    #define ROW(i) (cached[0] == (i) ? cache[0] : cached[1] == (i) ? cache[1] : \
                    (hpass(rgba + (size_t)(i) * (size_t)sw * 4u, sw, dw, cache[next]), cached[next] = (i), next ^= 1, cache[next ^ 1]))
    for (int y = 0; y < dh; ++y) {
        for (int k = 0; k < dw * 4; ++k) acc[k] = 0;
        uint64_t wsum = 0;
        if (dh <= sh) {
            uint64_t start = (uint64_t)y * (uint64_t)sh * 65536u / (uint64_t)dh;
            uint64_t end = (uint64_t)(y + 1) * (uint64_t)sh * 65536u / (uint64_t)dh;
            for (uint64_t i = start >> 16; i < ((end + 65535u) >> 16) && i < (uint64_t)sh; ++i) {
                uint64_t lo = start > (i << 16) ? start : (i << 16);
                uint64_t hi = end < ((i + 1) << 16) ? end : ((i + 1) << 16);
                if (hi <= lo) continue;
                uint64_t wgt = hi - lo;
                const uint32_t *r = ROW((int)i);
                for (int k = 0; k < dw * 4; ++k) acc[k] += wgt * r[k];
                wsum += wgt;
            }
        } else {
            int64_t pos = (int64_t)(((uint64_t)(2 * y + 1) * (uint64_t)sh * 65536u) / (uint64_t)(2 * dh)) - 32768;
            if (pos < 0) pos = 0;
            int i0 = (int)(pos >> 16), i1 = i0 + 1 < sh ? i0 + 1 : i0;
            if (i0 >= sh) i0 = i1 = sh - 1;
            uint64_t f = (uint64_t)(pos & 0xFFFF);
            const uint32_t *r0 = ROW(i0);
            for (int k = 0; k < dw * 4; ++k) acc[k] += (65536u - f) * r0[k];
            const uint32_t *r1 = ROW(i1);
            for (int k = 0; k < dw * 4; ++k) acc[k] += f * r1[k];
            wsum = 65536u;
        }
        if (!wsum) wsum = 1;
        uint32_t *o = out + (size_t)y * (size_t)dw;
        for (int x = 0; x < dw; ++x) {
            uint32_t c[4];
            for (int k = 0; k < 4; ++k) {
                uint64_t v = (acc[x * 4 + k] / wsum + 128u) >> 8;
                c[k] = v > 255u ? 255u : (uint32_t)v;
            }
            for (int k = 0; k < 3; ++k) if (c[k] > c[3]) c[k] = c[3];
            o[x] = (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
        }
    }
    #undef ROW
    wmem_free(rows);
    return out;
}
