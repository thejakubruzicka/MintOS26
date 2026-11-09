/* MintDE 2 software renderer (see gfx.h). Integer-only, clip-aware. */
#include "gfx.h"

gfx_surface gfx_screen, gfx_wall, gfx_scratch;
static gfx_rect clip = {0, 0, 0, 0};

gfx_rect gfx_rect_xywh(int32_t x, int32_t y, int32_t w, int32_t h) {
    gfx_rect r = {x, y, x + (w > 0 ? w : 0), y + (h > 0 ? h : 0)};
    return r;
}
gfx_rect gfx_rect_intersect(gfx_rect a, gfx_rect b) {
    gfx_rect r = {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0,
                  a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1};
    if (r.x1 < r.x0) r.x1 = r.x0;
    if (r.y1 < r.y0) r.y1 = r.y0;
    return r;
}
int gfx_rect_empty(gfx_rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
gfx_rect gfx_rect_union(gfx_rect a, gfx_rect b) {
    if (gfx_rect_empty(a)) return b;
    if (gfx_rect_empty(b)) return a;
    gfx_rect r = {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
                  a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
    return r;
}
int gfx_rect_contains(gfx_rect r, int32_t x, int32_t y) {
    return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}
int gfx_rect_overlaps(gfx_rect a, gfx_rect b) { return !gfx_rect_empty(gfx_rect_intersect(a, b)); }

void gfx_clip_set(gfx_rect r) {
    gfx_rect screen = {0, 0, gfx_screen.w, gfx_screen.h};
    clip = gfx_rect_intersect(r, screen);
}
gfx_rect gfx_clip_get(void) { return clip; }

static gfx_rect clip_for(const gfx_surface *s) {
    if (s == &gfx_screen) return clip;
    gfx_rect r = {0, 0, s->w, s->h};
    return gfx_rect_intersect(clip, r);
}

/* dst*(255-a) + src*a, /255 rounded, two channels at a time. */
uint32_t gfx_mix(uint32_t d, uint32_t c, uint32_t a) {
    if (a >= 255u) return c & 0xFFFFFFu;
    if (!a) return d;
    uint32_t ia = 255u - a;
    uint32_t rb = (c & 0xFF00FFu) * a + (d & 0xFF00FFu) * ia + 0x800080u;
    uint32_t g = (c & 0x00FF00u) * a + (d & 0x00FF00u) * ia + 0x008000u;
    rb = ((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
    g = ((g + ((g >> 8) & 0x00FF00u)) >> 8) & 0x00FF00u;
    return rb | g;
}
uint32_t gfx_lerp_rgb(uint32_t a, uint32_t b, uint32_t t) { return gfx_mix(a & 0xFFFFFFu, b & 0xFFFFFFu, t); }

static inline void plot(gfx_surface *s, int32_t x, int32_t y, uint32_t argb, uint32_t cov) {
    uint32_t a = ((argb >> 24) * cov + 127u) / 255u;
    if (!a) return;
    uint32_t *p = s->px + (int64_t)y * s->stride + x;
    *p = gfx_mix(*p, argb, a);
}

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

void gfx_fill(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t argb) {
    gfx_rect r = gfx_rect_intersect(gfx_rect_xywh(x, y, w, h), clip_for(s));
    uint32_t a = argb >> 24;
    if (gfx_rect_empty(r) || !a) return;
    for (int32_t yy = r.y0; yy < r.y1; ++yy) {
        uint32_t *row = s->px + (int64_t)yy * s->stride;
        if (a == 255u) { uint32_t c = argb & 0xFFFFFFu; for (int32_t xx = r.x0; xx < r.x1; ++xx) row[xx] = c; }
        else for (int32_t xx = r.x0; xx < r.x1; ++xx) row[xx] = gfx_mix(row[xx], argb, a);
    }
}
void gfx_hline(gfx_surface *s, int32_t x, int32_t y, int32_t w, uint32_t argb) { gfx_fill(s, x, y, w, 1, argb); }
void gfx_vline(gfx_surface *s, int32_t x, int32_t y, int32_t h, uint32_t argb) { gfx_fill(s, x, y, 1, h, argb); }

/* Coverage (0..255) of pixel (px,py) for a rounded rect; corner circles are
   evaluated at 1/16-pixel precision with a 1 px anti-aliasing ramp. */
static uint32_t rrect_cov(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r) {
    int32_t cx, cy;
    if (px < x + r) cx = x + r; else if (px >= x + w - r) cx = x + w - r; else return 255u;
    if (py < y + r) cy = y + r; else if (py >= y + h - r) cy = y + h - r; else return 255u;
    int32_t dx = (px * 16 + 8) - cx * 16, dy = (py * 16 + 8) - cy * 16;
    int32_t d = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
    int32_t c = (r * 16 + 8 - d) * 16;
    return c <= 0 ? 0u : c >= 255 ? 255u : (uint32_t)c;
}

static void rrect_span(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                       uint32_t top, uint32_t bottom, int gradient) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    gfx_rect cr = gfx_rect_intersect(gfx_rect_xywh(x, y, w, h), clip_for(s));
    if (gfx_rect_empty(cr)) return;
    for (int32_t yy = cr.y0; yy < cr.y1; ++yy) {
        uint32_t argb = top;
        if (gradient) {
            uint32_t t = h > 1 ? (uint32_t)((yy - y) * 255 / (h - 1)) : 0u;
            uint32_t a = ((top >> 24) * (255u - t) + (bottom >> 24) * t) / 255u;
            argb = (a << 24) | gfx_lerp_rgb(top, bottom, t);
        }
        uint32_t a = argb >> 24;
        if (!a) continue;
        uint32_t *row = s->px + (int64_t)yy * s->stride;
        int corner_row = yy < y + r || yy >= y + h - r;
        for (int32_t xx = cr.x0; xx < cr.x1; ++xx) {
            uint32_t cov = corner_row && (xx < x + r || xx >= x + w - r) ? rrect_cov(xx, yy, x, y, w, h, r) : 255u;
            if (!cov) continue;
            uint32_t aa = cov == 255u ? a : (a * cov + 127u) / 255u;
            row[xx] = aa == 255u ? (argb & 0xFFFFFFu) : gfx_mix(row[xx], argb, aa);
        }
    }
}

void gfx_round_rect(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t argb) {
    rrect_span(s, x, y, w, h, r, argb, argb, 0);
}
void gfx_round_rect_grad(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                         uint32_t top, uint32_t bottom) {
    rrect_span(s, x, y, w, h, r, top, bottom, 1);
}

/* 1 px hairline inside the rounded rect: outer coverage minus inner coverage. */
void gfx_round_rect_stroke(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t argb) {
    if (w <= 2 || h <= 2) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_rect cr = gfx_rect_intersect(gfx_rect_xywh(x, y, w, h), clip_for(s));
    for (int32_t yy = cr.y0; yy < cr.y1; ++yy) {
        int edge_row = yy < y + r + 1 || yy >= y + h - r - 1;
        for (int32_t xx = cr.x0; xx < cr.x1; ++xx) {
            if (!edge_row && xx > x && xx < x + w - 1) { xx = x + w - 2; continue; }
            uint32_t outer = rrect_cov(xx, yy, x, y, w, h, r);
            uint32_t inner = (xx > x && xx < x + w - 1 && yy > y && yy < y + h - 1)
                             ? rrect_cov(xx, yy, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0) : 0u;
            if (outer > inner) plot(s, xx, yy, argb, outer - inner);
        }
    }
}

void gfx_circle(gfx_surface *s, int32_t cx, int32_t cy, int32_t radius, uint32_t argb) {
    gfx_round_rect(s, cx - radius, cy - radius, radius * 2, radius * 2, radius, argb);
}
void gfx_circle_stroke(gfx_surface *s, int32_t cx, int32_t cy, int32_t radius, uint32_t argb) {
    gfx_round_rect_stroke(s, cx - radius, cy - radius, radius * 2, radius * 2, radius, argb);
}

/* Soft drop shadow: alpha falls off quadratically over `blur` pixels outside a
   rounded rect offset by dy. The area the window itself will cover is skipped. */
void gfx_shadow(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                int32_t blur, int32_t dy, uint32_t argb) {
    if (blur < 1) blur = 1;
    gfx_rect area = gfx_rect_intersect(gfx_rect_xywh(x - blur, y + dy - blur, w + 2 * blur, h + 2 * blur), clip_for(s));
    int32_t cx2 = 2 * x + w, cy2 = 2 * (y + dy) + h;      /* doubled centre */
    int32_t hw = w - 2 * r, hh = h - 2 * r;                /* doubled inner half-extent */
    uint32_t a = argb >> 24;
    for (int32_t yy = area.y0; yy < area.y1; ++yy) {
        uint32_t *row = s->px + (int64_t)yy * s->stride;
        int32_t qy = (2 * yy + 1) - cy2; if (qy < 0) qy = -qy; qy -= hh; if (qy < 0) qy = 0;
        int inside_window_rows = yy >= y + r && yy < y + h - r;
        for (int32_t xx = area.x0; xx < area.x1; ++xx) {
            if (inside_window_rows && xx >= x + 1 && xx < x + w - 1) { xx = x + w - 2; continue; }
            int32_t qx = (2 * xx + 1) - cx2; if (qx < 0) qx = -qx; qx -= hw; if (qx < 0) qx = 0;
            int32_t d2 = (int32_t)isqrt32((uint32_t)(qx * qx + qy * qy)) - 2 * r; /* doubled distance */
            uint32_t cov;
            if (d2 <= 0) cov = 255u;
            else if (d2 >= 2 * blur) continue;
            else { uint32_t t = (uint32_t)(2 * blur - d2) * 255u / (uint32_t)(2 * blur); cov = t * t / 255u; }
            uint32_t aa = (a * cov + 127u) / 255u;
            if (aa) row[xx] = gfx_mix(row[xx], argb, aa);
        }
    }
}

void gfx_line(gfx_surface *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb) {
    /* Xiaolin Wu style anti-aliased line in 8.8 fixed point. */
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    gfx_rect c = clip_for(s);
    if (adx >= ady) {
        if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dy = -dy; }
        int32_t grad = adx ? (dy * 256) / adx : 0, fy = y0 * 256 + 128;
        for (int32_t x = x0; x <= x1; ++x, fy += grad) {
            int32_t yi = (fy >> 8), f = fy & 255;
            if (x >= c.x0 && x < c.x1) {
                if (yi >= c.y0 && yi < c.y1) plot(s, x, yi, argb, (uint32_t)(255 - f));
                if (yi + 1 >= c.y0 && yi + 1 < c.y1) plot(s, x, yi + 1, argb, (uint32_t)f);
            }
        }
    } else {
        if (y1 < y0) { int32_t t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dx = -dx; }
        int32_t grad = ady ? (dx * 256) / ady : 0, fx = x0 * 256 + 128;
        for (int32_t y = y0; y <= y1; ++y, fx += grad) {
            int32_t xi = (fx >> 8), f = fx & 255;
            if (y >= c.y0 && y < c.y1) {
                if (xi >= c.x0 && xi < c.x1) plot(s, xi, y, argb, (uint32_t)(255 - f));
                if (xi + 1 >= c.x0 && xi + 1 < c.x1) plot(s, xi + 1, y, argb, (uint32_t)f);
            }
        }
    }
}

void gfx_copy(gfx_surface *dst, const gfx_surface *src, gfx_rect r) {
    r = gfx_rect_intersect(r, clip_for(dst));
    gfx_rect sr = {0, 0, src->w, src->h};
    r = gfx_rect_intersect(r, sr);
    for (int32_t y = r.y0; y < r.y1; ++y) {
        uint32_t *d = dst->px + (int64_t)y * dst->stride;
        const uint32_t *s = src->px + (int64_t)y * src->stride;
        for (int32_t x = r.x0; x < r.x1; ++x) d[x] = s[x];
    }
}

/* Blur of rect r (two box passes per axis ~ Gaussian). The region plus a
   sampling margin is copied to gfx_scratch, blurred there in place with a
   static line buffer, and only r is written back, so nothing outside r on the
   target surface is ever modified. */
#define GFX_LINE_MAX 4608
static uint32_t gfx_line_buf[GFX_LINE_MAX];
void gfx_blur_rounded(gfx_surface *s, gfx_rect r, int32_t radius, int32_t corner);

static void box_line(uint32_t *p, int32_t n, int32_t step, int32_t radius) {
    if (n <= 0 || n > GFX_LINE_MAX) return;
    for (int32_t i = 0; i < n; ++i) gfx_line_buf[i] = p[(int64_t)i * step];
    const uint32_t win = (uint32_t)(2 * radius + 1), mul = 65536u / win;
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int32_t k = -radius; k <= radius; ++k) {
        int32_t i = k < 0 ? 0 : (k >= n ? n - 1 : k);
        uint32_t c = gfx_line_buf[i]; sr += (c >> 16) & 255u; sg += (c >> 8) & 255u; sb += c & 255u;
    }
    for (int32_t i = 0; i < n; ++i) {
        p[(int64_t)i * step] = (((sr * mul) >> 16) << 16) | (((sg * mul) >> 16) << 8) | ((sb * mul) >> 16);
        int32_t ia = i + radius + 1, ir = i - radius;
        if (ia >= n) ia = n - 1;
        if (ir < 0) ir = 0;
        uint32_t a = gfx_line_buf[ia], b = gfx_line_buf[ir];
        sr += ((a >> 16) & 255u) - ((b >> 16) & 255u);
        sg += ((a >> 8) & 255u) - ((b >> 8) & 255u);
        sb += (a & 255u) - (b & 255u);
    }
}

void gfx_blur(gfx_surface *s, gfx_rect r, int32_t radius) { gfx_blur_rounded(s, r, radius, 0); }

/* Blur r; only pixels inside the rounded rect (r, corner) and inside the
   current clip are written back, edge pixels blended by coverage. Callers
   must make sure the sampling margin (2 * radius) was freshly composed. */
void gfx_blur_rounded(gfx_surface *s, gfx_rect r, int32_t radius, int32_t corner) {
    if (radius < 1 || !gfx_scratch.px || s == &gfx_scratch) return;
    gfx_rect shape = r;
    gfx_rect full = {0, 0, s->w, s->h};
    r = gfx_rect_intersect(r, full);
    if (gfx_rect_empty(r)) return;
    gfx_rect m = gfx_rect_intersect((gfx_rect){r.x0 - 2 * radius, r.y0 - 2 * radius,
                                               r.x1 + 2 * radius, r.y1 + 2 * radius}, full);
    int32_t ts = gfx_scratch.stride;
    for (int32_t y = m.y0; y < m.y1; ++y) {
        const uint32_t *src = s->px + (int64_t)y * s->stride;
        uint32_t *dst = gfx_scratch.px + (int64_t)y * ts;
        for (int32_t x = m.x0; x < m.x1; ++x) dst[x] = src[x];
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (int32_t y = m.y0; y < m.y1; ++y)
            box_line(gfx_scratch.px + (int64_t)y * ts + m.x0, m.x1 - m.x0, 1, radius);
        for (int32_t x = m.x0; x < m.x1; ++x)
            box_line(gfx_scratch.px + (int64_t)m.y0 * ts + x, m.y1 - m.y0, ts, radius);
    }
    gfx_rect wr = gfx_rect_intersect(r, clip_for(s));
    int32_t sw = shape.x1 - shape.x0, sh = shape.y1 - shape.y0;
    if (corner * 2 > sw) corner = sw / 2;
    if (corner * 2 > sh) corner = sh / 2;
    for (int32_t y = wr.y0; y < wr.y1; ++y) {
        const uint32_t *src = gfx_scratch.px + (int64_t)y * ts;
        uint32_t *dst = s->px + (int64_t)y * s->stride;
        int corner_row = corner && (y < shape.y0 + corner || y >= shape.y1 - corner);
        for (int32_t x = wr.x0; x < wr.x1; ++x) {
            if (corner_row && (x < shape.x0 + corner || x >= shape.x1 - corner)) {
                uint32_t cov = rrect_cov(x, y, shape.x0, shape.y0, sw, sh, corner);
                if (cov) dst[x] = gfx_mix(dst[x], src[x], cov);
            } else dst[x] = src[x];
        }
    }
}

/* Save / restore the four r x r corner squares of a rounded window so the
   content drawn square-cornered can be re-rounded against what was behind. */
void gfx_corners_save(const gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t *buf) {
    gfx_rect c = clip_for(s);
    for (int k = 0; k < 4; ++k) {
        int32_t ox = (k & 1) ? x + w - r : x, oy = (k & 2) ? y + h - r : y;
        for (int32_t j = 0; j < r; ++j) for (int32_t i = 0; i < r; ++i) {
            int32_t px = ox + i, py = oy + j;
            if (gfx_rect_contains(c, px, py)) buf[(k * r + j) * r + i] = s->px[(int64_t)py * s->stride + px];
        }
    }
}
void gfx_corners_restore(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const uint32_t *buf) {
    gfx_rect c = clip_for(s);
    for (int k = 0; k < 4; ++k) {
        int32_t ox = (k & 1) ? x + w - r : x, oy = (k & 2) ? y + h - r : y;
        for (int32_t j = 0; j < r; ++j) for (int32_t i = 0; i < r; ++i) {
            int32_t px = ox + i, py = oy + j;
            if (!gfx_rect_contains(c, px, py)) continue;
            uint32_t cov = rrect_cov(px, py, x, y, w, h, r);
            uint32_t *d = s->px + (int64_t)py * s->stride + px;
            if (cov < 255u) *d = gfx_mix(buf[(k * r + j) * r + i], *d, cov);
        }
    }
}

/* Bilinear-scaled straight-alpha RGBA blit with a global alpha multiplier. */
void gfx_image(gfx_surface *s, int32_t x, int32_t y, int32_t dw, int32_t dh,
               const uint8_t *rgba, int32_t sw, int32_t sh, uint32_t alpha) {
    if (!rgba || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    gfx_rect r = gfx_rect_intersect(gfx_rect_xywh(x, y, dw, dh), clip_for(s));
    for (int32_t yy = r.y0; yy < r.y1; ++yy) {
        int32_t fy = (int32_t)(((int64_t)(2 * (yy - y) + 1) * sh * 128) / dh) - 128; /* 8.8 */
        if (fy < 0) fy = 0;
        int32_t y0 = fy >> 8, wy = fy & 255, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        if (y0 >= sh) { y0 = sh - 1; y1 = y0; }
        uint32_t *row = s->px + (int64_t)yy * s->stride;
        for (int32_t xx = r.x0; xx < r.x1; ++xx) {
            int32_t fx = (int32_t)(((int64_t)(2 * (xx - x) + 1) * sw * 128) / dw) - 128;
            if (fx < 0) fx = 0;
            int32_t x0 = fx >> 8, wx = fx & 255, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            if (x0 >= sw) { x0 = sw - 1; x1 = x0; }
            const uint8_t *p00 = rgba + ((int64_t)y0 * sw + x0) * 4, *p01 = rgba + ((int64_t)y0 * sw + x1) * 4;
            const uint8_t *p10 = rgba + ((int64_t)y1 * sw + x0) * 4, *p11 = rgba + ((int64_t)y1 * sw + x1) * 4;
            uint32_t c[4];
            for (int k = 0; k < 4; ++k) {
                /* premultiply colour by alpha before filtering to avoid dark fringes */
                uint32_t a00 = p00[3], a01 = p01[3], a10 = p10[3], a11 = p11[3];
                uint32_t v00 = k == 3 ? a00 * 255u : p00[k] * a00, v01 = k == 3 ? a01 * 255u : p01[k] * a01;
                uint32_t v10 = k == 3 ? a10 * 255u : p10[k] * a10, v11 = k == 3 ? a11 * 255u : p11[k] * a11;
                uint32_t top = (v00 * (uint32_t)(256 - wx) + v01 * (uint32_t)wx) >> 8;
                uint32_t bot = (v10 * (uint32_t)(256 - wx) + v11 * (uint32_t)wx) >> 8;
                c[k] = (top * (uint32_t)(256 - wy) + bot * (uint32_t)wy) >> 8;
            }
            uint32_t a = c[3] / 255u;
            if (!a) continue;
            a = (a * alpha + 127u) / 255u;
            uint32_t rr = c[0] / (c[3] / 255u ? c[3] / 255u : 1u), gg = c[1] / (c[3] / 255u ? c[3] / 255u : 1u),
                     bb = c[2] / (c[3] / 255u ? c[3] / 255u : 1u);
            if (rr > 255u) rr = 255u;
            if (gg > 255u) gg = 255u;
            if (bb > 255u) bb = 255u;
            row[xx] = gfx_mix(row[xx], (rr << 16) | (gg << 8) | bb, a);
        }
    }
}

void gfx_mask(gfx_surface *s, int32_t x, int32_t y, int32_t dw, int32_t dh,
              const uint8_t *a8, int32_t sw, int32_t sh, uint32_t argb) {
    if (!a8 || dw <= 0 || dh <= 0) return;
    gfx_rect r = gfx_rect_intersect(gfx_rect_xywh(x, y, dw, dh), clip_for(s));
    for (int32_t yy = r.y0; yy < r.y1; ++yy) {
        int32_t sy = (int32_t)((int64_t)(yy - y) * sh / dh);
        for (int32_t xx = r.x0; xx < r.x1; ++xx) {
            /* box-filter the source footprint for clean downscaling */
            int32_t sx0 = (int32_t)((int64_t)(xx - x) * sw / dw), sx1 = (int32_t)((int64_t)(xx - x + 1) * sw / dw);
            int32_t sy1 = (int32_t)((int64_t)(yy - y + 1) * sh / dh);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy) sy1 = sy + 1;
            uint32_t sum = 0, n = 0;
            for (int32_t a = sy; a < sy1 && a < sh; ++a)
                for (int32_t b = sx0; b < sx1 && b < sw; ++b) { sum += a8[(int64_t)a * sw + b]; ++n; }
            if (n && sum) plot(s, xx, yy, argb, sum / n);
        }
    }
}

/* ---------------------------------------------------------------- text */
static uint32_t utf8_next(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    if (c < 0x80u) { *p += 1; return c; }
    if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) { *p += 2; return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu); }
    if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *p += 3; return ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    }
    *p += 1; return '?';
}

static const MINT_GLYPH *glyph_for(const MINT_FONT *f, uint32_t cp) {
    if (cp >= 32u && cp < 127u && f->glyph_count > cp - 32u && f->glyphs[cp - 32u].codepoint == cp)
        return &f->glyphs[cp - 32u];
    int32_t lo = 0, hi = (int32_t)f->glyph_count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (f->glyphs[mid].codepoint == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].codepoint < cp) lo = mid + 1; else hi = mid - 1;
    }
    return cp == '?' ? 0 : glyph_for(f, '?');
}

int32_t gfx_font_ascent(int font) { return font >= 0 && font < MINT_FONT_COUNT ? mint_fonts[font].ascent : 10; }
int32_t gfx_font_line(int font) { return font >= 0 && font < MINT_FONT_COUNT ? mint_fonts[font].line : 14; }

static void draw_glyph(gfx_surface *s, const MINT_FONT *f, const MINT_GLYPH *g, int32_t x, int32_t y,
                       uint32_t argb, gfx_rect c) {
    int32_t gx = x + g->xoff, gy = y + g->yoff;
    const uint8_t *bits = f->bits + g->offset;
    uint32_t a = argb >> 24;
    for (int32_t r = 0; r < g->h; ++r) {
        int32_t yy = gy + r;
        if (yy < c.y0 || yy >= c.y1) continue;
        uint32_t *row = s->px + (int64_t)yy * s->stride;
        for (int32_t col = 0; col < g->w; ++col) {
            int32_t xx = gx + col;
            if (xx < c.x0 || xx >= c.x1) continue;
            uint32_t cov = bits[r * g->w + col];
            if (!cov) continue;
            uint32_t aa = (a * cov + 127u) / 255u;
            row[xx] = gfx_mix(row[xx], argb, aa);
        }
    }
}

int32_t gfx_text(gfx_surface *s, int font, int32_t x, int32_t y, const char *t, uint32_t argb) {
    if (!t || font < 0 || font >= MINT_FONT_COUNT) return 0;
    const MINT_FONT *f = &mint_fonts[font];
    gfx_rect c = clip_for(s);
    int32_t pen = x * 64;
    while (*t) {
        uint32_t cp = utf8_next(&t);
        const MINT_GLYPH *g = glyph_for(f, cp);
        if (!g) continue;
        if (g->w && s) draw_glyph(s, f, g, (pen + 32) >> 6, y, argb, c);
        pen += g->advance64;
    }
    return ((pen + 32) >> 6) - x;
}

int32_t gfx_text_width(int font, const char *t) {
    if (!t || font < 0 || font >= MINT_FONT_COUNT) return 0;
    const MINT_FONT *f = &mint_fonts[font];
    int32_t pen = 0;
    while (*t) { const MINT_GLYPH *g = glyph_for(f, utf8_next(&t)); if (g) pen += g->advance64; }
    return (pen + 32) >> 6;
}

int32_t gfx_text_fit(gfx_surface *s, int font, int32_t x, int32_t y, const char *t, int32_t max_w, uint32_t argb) {
    if (!t) return 0;
    if (gfx_text_width(font, t) <= max_w) return gfx_text(s, font, x, y, t, argb);
    char buf[160];
    int32_t ell = gfx_text_width(font, "\xE2\x80\xA6");
    size_t n = 0;
    const char *p = t;
    int32_t width = 0;
    while (*p && n + 4u < sizeof(buf)) {
        const char *start = p;
        uint32_t cp = utf8_next(&p);
        const MINT_GLYPH *g = glyph_for(&mint_fonts[font], cp);
        int32_t adv = g ? (g->advance64 + 32) >> 6 : 0;
        if (width + adv + ell > max_w) break;
        width += adv;
        while (start < p && n + 4u < sizeof(buf)) buf[n++] = *start++;
    }
    buf[n++] = (char)0xE2; buf[n++] = (char)0x80; buf[n++] = (char)0xA6; buf[n] = '\0';
    return gfx_text(s, font, x, y, buf, argb);
}

int32_t gfx_text_cells(gfx_surface *s, int font, int32_t x, int32_t y, const char *t, int32_t cell, uint32_t argb) {
    if (!t || font < 0 || font >= MINT_FONT_COUNT) return 0;
    const MINT_FONT *f = &mint_fonts[font];
    gfx_rect c = clip_for(s);
    int32_t cx = x;
    while (*t) {
        const MINT_GLYPH *g = glyph_for(f, utf8_next(&t));
        if (g && g->w) draw_glyph(s, f, g, cx + (cell - ((g->advance64 + 32) >> 6)) / 2, y, argb, c);
        cx += cell;
    }
    return cx - x;
}
