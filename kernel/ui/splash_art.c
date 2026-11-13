/* MintOS boot splash artwork — pure integer renderer (see boot_splash.h).
 * Compiled into the kernel (boot animation), MintDE (crossfade into the
 * desktop) and tools/render_splash_preview.c. Original MintOS design: a
 * folded mint leaf on a deep-green radial glow with a thin progress bar. */
#include <stdint.h>
#include "../include/boot_splash.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) {      /* t: 0..256 */
    if (t >= 256u) return b;
    uint32_t ra = (a >> 16) & 255u, ga = (a >> 8) & 255u, ba = a & 255u;
    uint32_t rb = (b >> 16) & 255u, gb = (b >> 8) & 255u, bb = b & 255u;
    return RGB((ra * (256u - t) + rb * t) >> 8, (ga * (256u - t) + gb * t) >> 8, (ba * (256u - t) + bb * t) >> 8);
}

void mint_splash_geometry(MINT_SPLASH_GEOMETRY *g, int32_t width, int32_t height) {
    if (!g) return;
    if (width < 64) width = 64;
    if (height < 64) height = 64;
    g->width = width; g->height = height;
    g->cx = width / 2;
    g->cy = height / 2 - height / 25;
    g->leaf = height * 85 / 1000;
    if (g->leaf < 16) g->leaf = 16;
    int32_t ext = g->leaf * 112 / 100 + 2;
    g->box_x0 = g->cx - ext; g->box_x1 = g->cx + ext;
    g->box_y0 = g->cy - ext; g->box_y1 = g->cy + ext;
    g->bar_w = height * 21 / 100;
    g->bar_h = height / 216 < 4 ? 4 : height / 216;
    g->bar_x = g->cx - g->bar_w / 2;
    g->bar_y = g->cy + height * 18 / 100;
}

uint32_t mint_splash_background(const MINT_SPLASH_GEOMETRY *g, int32_t x, int32_t y) {
    uint32_t ty = (uint32_t)(y < 0 ? 0 : y >= g->height ? g->height - 1 : y) * 256u / (uint32_t)g->height;
    uint32_t c = mix(RGB(14, 44, 38), RGB(5, 17, 15), ty);
    int64_t dx = x - g->cx, dy = (int64_t)(y - g->cy) * 5 / 4;
    int64_t r = (int64_t)g->height * 46 / 100;
    int64_t d2 = dx * dx + dy * dy, r2 = r * r;
    if (d2 < r2) {
        uint32_t t = (uint32_t)(1024 - d2 * 1024 / r2);         /* 0..1024 */
        uint32_t w = (t * t) >> 10;
        c = mix(c, RGB(34, 112, 92), (w * 150u) >> 10);
    }
    return c;
}

/* 0 = outside, 1 = leaf blade, 2 = stem. Coordinates in quarter pixels from
   the leaf centre; outputs are leaf units (1024 = half-length). */
static int leaf_sample(const MINT_SPLASH_GEOMETRY *g, int32_t qx, int32_t qy, int32_t *uo, int32_t *vo) {
    int64_t s4 = (int64_t)g->leaf * 4;
    int64_t u = ((int64_t)(qx - qy) * 181 / 256) * 1024 / s4;     /* along the axis, tip up-right */
    int64_t v = ((int64_t)(qx + qy) * 181 / 256) * 1024 / s4;
    int64_t bend = 0;
    if (u > -1024 && u < 1024) bend = 90 * (1048576 - u * u) / 1048576;
    int64_t vb = v + bend;
    *uo = (int32_t)u; *vo = (int32_t)vb;
    const int64_t R = 1434, D = 1004;
    if (u * u + (vb - D) * (vb - D) <= R * R && u * u + (vb + D) * (vb + D) <= R * R) return 1;
    if (u > -1390 && u < -930) {
        int64_t centre = (u + 980) * (u + 980) / 5200;
        int64_t dv = v - centre;
        if (dv > -44 && dv < 44) return 2;
    }
    return 0;
}

static uint32_t leaf_pixel(const MINT_SPLASH_GEOMETRY *g, int32_t x, int32_t y, uint32_t bg, uint32_t alpha) {
    int32_t qx0 = (x - g->cx) * 4, qy0 = (y - g->cy) * 4;
    uint32_t blade = 0, stem = 0;
    int32_t u = 0, v = 0, cu, cv;
    for (int32_t sy = 0; sy < 4; ++sy)
        for (int32_t sx = 0; sx < 4; ++sx) {
            int k = leaf_sample(g, qx0 + sx, qy0 + sy, &cu, &cv);
            if (k == 1) ++blade; else if (k == 2) ++stem;
        }
    if (!blade && !stem) return bg;
    (void)leaf_sample(g, qx0 + 2, qy0 + 2, &u, &v);
    uint32_t t = (uint32_t)(u < -1024 ? 0 : u > 1024 ? 256 : (u + 1024) / 8);   /* base 0 .. tip 256 */
    uint32_t col;
    if (blade >= stem) {
        uint32_t light = mix(RGB(31, 196, 146), RGB(132, 246, 210), t);
        uint32_t dark = mix(RGB(14, 150, 112), RGB(70, 222, 176), t);
        col = v >= 0 ? light : dark;
        int32_t vein = 34 - (u + 1000) * 22 / 1900;
        if (u > -1000 && u < 900 && v > -vein && v < vein) col = mix(col, RGB(214, 255, 241), 130u);
        /* soft rim light along the upper edge */
        if (v > 300) col = mix(col, RGB(190, 255, 232), (uint32_t)(v - 300) / 6u > 60u ? 60u : (uint32_t)(v - 300) / 6u);
    } else {
        col = RGB(24, 160, 120);
    }
    uint32_t cover = (blade + stem) * 16u;                      /* 0..256 */
    return mix(bg, col, cover * alpha / 255u);
}

static uint32_t bar_pixel(const MINT_SPLASH_GEOMETRY *g, int32_t x, int32_t y, uint32_t bg,
                          uint32_t progress, uint32_t shimmer) {
    int32_t r4 = g->bar_h * 2;                                  /* radius in quarter pixels */
    int32_t fill_end = g->bar_x + (int32_t)((int64_t)g->bar_w * progress / 1000);
    uint32_t track = 0, filled = 0;
    for (int32_t sy = 0; sy < 4; ++sy)
        for (int32_t sx = 0; sx < 4; ++sx) {
            int32_t qx = (x - g->bar_x) * 4 + sx, qy = (y - g->bar_y) * 4 + sy;
            int32_t w4 = g->bar_w * 4, h4 = g->bar_h * 4;
            if (qy < 0 || qy >= h4 || qx < 0 || qx >= w4) continue;
            int32_t cx = qx < r4 ? r4 : qx > w4 - r4 ? w4 - r4 : qx;
            int32_t dx = qx - cx, dy = qy - r4;
            if (dx * dx + dy * dy > r4 * r4) continue;
            ++track;
            if (qx < (fill_end - g->bar_x) * 4) ++filled;
        }
    if (!track) return bg;
    uint32_t c = mix(bg, RGB(255, 255, 255), track * 16u * 44u / 256u);
    if (filled) {
        uint32_t f = RGB(62, 230, 176);
        int32_t span = g->bar_w * 2, hx = g->bar_x + (int32_t)(shimmer % (uint32_t)span) - g->bar_w / 2;
        int32_t d = x - hx; if (d < 0) d = -d;
        int32_t wide = g->bar_h * 9;
        if (d < wide) f = mix(f, RGB(222, 255, 244), (uint32_t)(wide - d) * 150u / (uint32_t)wide);
        c = mix(c, f, filled * 16u);
    }
    return c;
}

void mint_splash_render(uint32_t *px, int32_t stride, const MINT_SPLASH_GEOMETRY *g,
                        int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                        uint32_t leaf_alpha, uint32_t progress, uint32_t shimmer) {
    if (!px || !g) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->width) x1 = g->width;
    if (y1 > g->height) y1 = g->height;
    if (x1 <= x0 || y1 <= y0 || stride < x1 - x0) return;
    if (leaf_alpha > 255u) leaf_alpha = 255u;
    for (int32_t y = y0; y < y1; ++y) {
        uint32_t *row = px + (int64_t)(y - y0) * stride;
        int in_leaf_rows = y >= g->box_y0 && y < g->box_y1 && leaf_alpha;
        int in_bar_rows = progress <= 1000u && y >= g->bar_y && y < g->bar_y + g->bar_h;
        for (int32_t x = x0; x < x1; ++x) {
            uint32_t c = mint_splash_background(g, x, y);
            if (in_leaf_rows && x >= g->box_x0 && x < g->box_x1) c = leaf_pixel(g, x, y, c, leaf_alpha);
            if (in_bar_rows && x >= g->bar_x && x < g->bar_x + g->bar_w) c = bar_pixel(g, x, y, c, progress, shimmer);
            row[x - x0] = c;
        }
    }
}
