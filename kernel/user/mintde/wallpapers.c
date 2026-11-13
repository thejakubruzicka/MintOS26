/* MintOS wallpaper collection (26.5.014.0) — see wallpapers.h.
 * Original procedural designs in the mint palette; integer math only (the
 * ring-3 ABI forbids SSE) and mostly row/column separable so a 1920x1080
 * wallpaper renders quickly even under emulation. */
#include <stdint.h>
#include "wallpapers.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t) {        /* t 0..256 */
    if (t >= 256u) return b;
    uint32_t ra = (a >> 16) & 255u, ga = (a >> 8) & 255u, ba = a & 255u;
    uint32_t rb = (b >> 16) & 255u, gb = (b >> 8) & 255u, bb = b & 255u;
    return RGB((ra * (256u - t) + rb * t) >> 8, (ga * (256u - t) + gb * t) >> 8, (ba * (256u - t) + bb * t) >> 8);
}
static uint32_t add_light(uint32_t c, uint32_t light, uint32_t t) {   /* screen-like additive glow */
    uint32_t r = ((c >> 16) & 255u) + (((light >> 16) & 255u) * t >> 8);
    uint32_t g = ((c >> 8) & 255u) + (((light >> 8) & 255u) * t >> 8);
    uint32_t b = (c & 255u) + ((light & 255u) * t >> 8);
    return RGB(r > 255u ? 255u : r, g > 255u ? 255u : g, b > 255u ? 255u : b);
}
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

/* Bhaskara I approximation, one turn = 1024; result scaled by 16384. */
int32_t wp_sin(int32_t angle) {
    angle &= 1023;
    int32_t sign = angle >= 512 ? -1 : 1;
    int64_t x = angle & 511;                       /* 0..511 = 0..pi */
    int64_t p = x * (512 - x);
    int64_t v = 16 * p * 16384 / (5 * 512 * 512 - 4 * p);
    return sign * (int32_t)v;
}

const char *wallpaper_name(uint32_t id) {
    static const char *const names[WP_COUNT] = {
        "Mint Waves", "Mint Waves Light", "Mint Waves Dark", "Mint Day", "Leaf Light", "Aurora",
        "Fresh Dew", "Mint Dunes", "Mint", "Teal", "Graphite", "Midnight", "Sand"
    };
    return id < WP_COUNT ? names[id] : "Mint Waves";
}
int wallpaper_is_photo(uint32_t id) { return id <= WP_MINT_WAVES_DARK; }
int wallpaper_is_dynamic(uint32_t id) { return id == WP_MINT_DAY; }

/* ------------------------------------------------------------ dynamic palette */
typedef struct { uint32_t minute; wp_sky sky; } keyframe;
static const keyframe keys[] = {
    {0,    {RGB(6, 12, 30),    RGB(22, 38, 70),   {RGB(20, 38, 58), RGB(14, 32, 46), RGB(10, 26, 36), RGB(6, 18, 26)},   RGB(24, 40, 70),   RGB(214, 226, 240), RGB(90, 120, 170), 640, 220, 255}},
    {300,  {RGB(8, 16, 38),    RGB(30, 44, 82),   {RGB(22, 42, 64), RGB(16, 36, 52), RGB(12, 28, 40), RGB(8, 20, 30)},   RGB(30, 46, 80),   RGB(214, 226, 240), RGB(90, 120, 170), 860, 520, 220}},
    {375,  {RGB(52, 70, 128),  RGB(250, 170, 140), {RGB(126, 112, 150), RGB(84, 104, 120), RGB(46, 92, 92), RGB(26, 70, 66)}, RGB(240, 170, 160), RGB(255, 214, 160), RGB(255, 160, 120), 180, 860, 0}},
    {480,  {RGB(96, 170, 236), RGB(210, 232, 246), {RGB(150, 200, 214), RGB(98, 186, 170), RGB(52, 160, 130), RGB(28, 126, 100)}, RGB(196, 226, 240), RGB(255, 246, 220), RGB(255, 240, 200), 280, 420, 0}},
    {720,  {RGB(70, 150, 236), RGB(196, 228, 250), {RGB(150, 206, 222), RGB(96, 196, 176), RGB(46, 170, 132), RGB(22, 134, 102)}, RGB(190, 226, 244), RGB(255, 252, 236), RGB(255, 250, 220), 520, 200, 0}},
    {1020, {RGB(84, 156, 230), RGB(214, 230, 240), {RGB(156, 200, 212), RGB(104, 190, 168), RGB(54, 164, 126), RGB(28, 128, 98)}, RGB(200, 226, 236), RGB(255, 244, 214), RGB(255, 230, 180), 740, 420, 0}},
    {1110, {RGB(70, 76, 150),  RGB(255, 150, 104), {RGB(150, 104, 140), RGB(112, 86, 120), RGB(64, 72, 96), RGB(34, 54, 70)},  RGB(250, 150, 120), RGB(255, 196, 130), RGB(255, 130, 90), 840, 840, 0}},
    {1230, {RGB(14, 20, 50),   RGB(56, 52, 104),  {RGB(40, 46, 80), RGB(26, 38, 62), RGB(16, 30, 46), RGB(8, 20, 32)},    RGB(54, 56, 100),  RGB(214, 226, 240), RGB(90, 120, 170), 200, 600, 200}},
    {1440, {RGB(6, 12, 30),    RGB(22, 38, 70),   {RGB(20, 38, 58), RGB(14, 32, 46), RGB(10, 26, 36), RGB(6, 18, 26)},   RGB(24, 40, 70),   RGB(214, 226, 240), RGB(90, 120, 170), 640, 220, 255}},
};
#define KEY_COUNT (sizeof(keys) / sizeof(keys[0]))

uint32_t wallpaper_phase(uint32_t minute) {
    minute %= 1440u;
    if (minute >= 330u && minute < 450u) return WP_PHASE_DAWN;
    if (minute >= 450u && minute < 1065u) return WP_PHASE_DAY;
    if (minute >= 1065u && minute < 1200u) return WP_PHASE_DUSK;
    return WP_PHASE_NIGHT;
}
const char *wallpaper_phase_name(uint32_t phase) {
    static const char *const n[4] = {"Night", "Dawn", "Day", "Dusk"};
    return phase < 4u ? n[phase] : "Day";
}

void wallpaper_sky(uint32_t minute, wp_sky *out) {
    minute %= 1440u;
    uint32_t k = 0;
    while (k + 2u < KEY_COUNT && keys[k + 1u].minute <= minute) ++k;
    const wp_sky *a = &keys[k].sky, *b = &keys[k + 1u].sky;
    uint32_t span = keys[k + 1u].minute - keys[k].minute;
    uint32_t t = span ? (minute - keys[k].minute) * 256u / span : 0u;
    out->top = mix(a->top, b->top, t);
    out->horizon = mix(a->horizon, b->horizon, t);
    for (int i = 0; i < 4; ++i) out->hills[i] = mix(a->hills[i], b->hills[i], t);
    out->haze = mix(a->haze, b->haze, t);
    out->sun = mix(a->sun, b->sun, t);
    out->glow = mix(a->glow, b->glow, t);
    out->stars = (a->stars * (256u - t) + b->stars * t) >> 8;
    /* The sun travels an arc from 06:00 to 19:00; the moon the rest of the night. */
    int32_t day0 = 360, day1 = 1140;
    int32_t m = (int32_t)minute;
    if (m >= day0 && m <= day1) {
        int32_t p = (m - day0) * 512 / (day1 - day0);            /* 0..512 half turn */
        out->sun_x = 120 + (m - day0) * 760 / (day1 - day0);
        out->sun_y = 700 - wp_sin(p) * 560 / 16384;              /* 700 at the horizon, 140 at noon */
    } else {
        int32_t n = m > day1 ? m - day1 : m + 1440 - day1;       /* minutes since sunset */
        int32_t len = 1440 - (day1 - day0);
        int32_t p = n * 512 / len;
        out->sun_x = 140 + n * 720 / len;
        out->sun_y = 720 - wp_sin(p) * 460 / 16384;
    }
}

uint32_t wallpaper_refresh_minutes(uint32_t minute) {
    uint32_t phase = wallpaper_phase(minute);
    return phase == WP_PHASE_DAWN || phase == WP_PHASE_DUSK ? 5u : 15u;
}

/* ------------------------------------------------------------ renderers */
typedef struct {
    uint32_t *px; int32_t stride, w, h, x0, y0, x1, y1;
} target;

static void put(const target *t, int32_t x, int32_t y, uint32_t c) { t->px[(int64_t)(y - t->y0) * t->stride + (x - t->x0)] = c; }

/* Rolling hills: height of layer `k` at column x in 1/16 pixels. */
static int32_t hill16(int32_t w, int32_t h, int32_t x, int32_t k, int32_t style) {
    static const int32_t base[2][4] = {{560, 640, 735, 840}, {520, 610, 700, 800}};    /* permille of height */
    static const int32_t amp[2][4] = {{70, 62, 58, 54}, {40, 48, 56, 62}};
    static const int32_t f1[4] = {2, 3, 2, 3}, f2[4] = {5, 7, 6, 4}, ph[4] = {100, 420, 760, 260};
    int32_t ax = (int32_t)((int64_t)x * 1024 / (w > 0 ? w : 1));
    int64_t s = (int64_t)wp_sin(ax * f1[k] + ph[k]) * 3 + (int64_t)wp_sin(ax * f2[k] + ph[k] * 3) +
                (style ? (int64_t)wp_sin(ax * (f2[k] + 5) + ph[k]) / 2 : 0);
    int64_t y = (int64_t)h * base[style][k] / 1000 + s * h * amp[style][k] / (1000 * 4 * 16384);
    return (int32_t)(y * 16);
}

static void render_landscape(const target *t, const wp_sky *sky, int32_t style) {
    int32_t w = t->w, h = t->h;
    int32_t sx = w * sky->sun_x / 1000, sy = h * sky->sun_y / 1000;
    int32_t sr = h / 26 + 1, gr = h / 4 + 1;
    for (int32_t x = t->x0; x < t->x1; ++x) {
        int32_t hy[4];
        for (int32_t k = 0; k < 4; ++k) hy[k] = hill16(w, h, x, k, style);
        for (int32_t y = t->y0; y < t->y1; ++y) {
            /* sky */
            uint32_t ty = (uint32_t)(y * 256 / (h * 7 / 10 + 1));
            uint32_t c = mix(sky->top, sky->horizon, ty > 256u ? 256u : ty);
            if (sky->stars && y < h * 6 / 10) {
                uint32_t hsh = hash32((uint32_t)x * 7919u + (uint32_t)y * 104729u);
                if ((hsh & 1023u) == 0u) c = add_light(c, RGB(255, 255, 255), (sky->stars * ((hsh >> 10) & 255u)) >> 8);
            }
            int64_t dx = x - sx, dy = y - sy, d2 = dx * dx + dy * dy;
            if (d2 < (int64_t)gr * gr) {
                uint32_t g = (uint32_t)(256 - d2 * 256 / ((int64_t)gr * gr));
                c = add_light(c, sky->glow, (g * g) >> 9);
            }
            if (d2 < (int64_t)(sr + 1) * (sr + 1)) {
                int64_t edge = (int64_t)sr * sr - d2;
                uint32_t a = edge >= 2 * sr ? 256u : edge <= 0 ? 0u : (uint32_t)(edge * 128 / sr);
                c = mix(c, sky->sun, a);
            }
            /* hills, far to near, with an anti-aliased crest */
            int32_t y16 = y * 16 + 8;
            for (int32_t k = 0; k < 4; ++k) {
                int32_t below = y16 - hy[k];
                if (below <= -16) continue;
                uint32_t cover = below >= 16 ? 256u : (uint32_t)(below + 16) * 8u;
                uint32_t depth = (uint32_t)((y16 - hy[k]) / 16);
                uint32_t hill = mix(sky->hills[k], RGB(0, 0, 0), depth * 60u / (uint32_t)(h / 3 + 1) > 90u ? 90u : depth * 60u / (uint32_t)(h / 3 + 1));
                if (k == 0) hill = mix(hill, sky->haze, 90u);
                else if (k == 1) hill = mix(hill, sky->haze, 40u);
                c = mix(c, hill, cover);
            }
            put(t, x, y, c);
        }
    }
}

/* Lens ("leaf") coverage 0..256 with a soft edge; u along the leaf. */
static uint32_t lens(int64_t px, int64_t py, int64_t cx, int64_t cy, int32_t angle, int64_t len, int64_t width, int64_t soft) {
    int64_t dx = px - cx, dy = py - cy;
    int64_t c = wp_sin(angle + 256), s = wp_sin(angle);
    int64_t u = (dx * c + dy * s) / 16384, v = (-dx * s + dy * c) / 16384;
    if (u < -len - soft || u > len + soft || v < -width - soft || v > width + soft) return 0;
    /* circle radius R through the tips (+-len, 0) and the sides (0, +-width) */
    int64_t R = (len * len + width * width) / (2 * width);
    int64_t D = R - width;
    int64_t d1 = u * u + (v - D) * (v - D), d2 = u * u + (v + D) * (v + D);
    int64_t m = d1 > d2 ? d1 : d2;                    /* the farther circle decides */
    int64_t edge = (R * R - m) / (2 * R);             /* ~distance inside the boundary */
    if (edge <= 0) return 0;
    if (edge >= soft) return 256;
    return (uint32_t)(edge * 256 / soft);
}

static void render_leaves(const target *t) {
    int32_t w = t->w, h = t->h;
    struct { int32_t x, y, angle, len, width; uint32_t color, alpha; } L[5] = {
        {780, 380, 860, 300, 120, RGB(120, 226, 196), 120}, {300, 700, 120, 360, 140, RGB(60, 196, 160), 110},
        {1500, 760, 680, 420, 160, RGB(40, 170, 150), 120}, {1180, 240, 30, 260, 96, RGB(170, 236, 214), 130},
        {120, 220, 940, 240, 90, RGB(150, 230, 204), 100}};
    for (int32_t y = t->y0; y < t->y1; ++y) {
        for (int32_t x = t->x0; x < t->x1; ++x) {
            uint32_t g = (uint32_t)(((int64_t)x * 3 + (int64_t)y * 5) * 256 / ((int64_t)w * 3 + (int64_t)h * 5 + 1));
            uint32_t c = mix(RGB(236, 250, 244), RGB(170, 226, 208), g);
            for (int i = 0; i < 5; ++i) {
                int64_t px = (int64_t)x * 1920 / w, py = (int64_t)y * 1080 / h;
                uint32_t a = lens(px, py, L[i].x, L[i].y, L[i].angle, L[i].len, L[i].width, 6 * 1920 / (w > 0 ? w : 1) + 3);
                if (!a) continue;
                uint32_t col = L[i].color;
                /* midrib highlight */
                int64_t dx = px - L[i].x, dy = py - L[i].y;
                int64_t v = (-dx * wp_sin(L[i].angle) + dy * wp_sin(L[i].angle + 256)) / 16384;
                if (v > -4 && v < 4) col = mix(col, RGB(255, 255, 255), 90u);
                c = mix(c, col, a * L[i].alpha >> 8);
            }
            put(t, x, y, c);
        }
    }
}

static void render_aurora(const target *t) {
    int32_t w = t->w, h = t->h;
    static const uint32_t col[3] = {RGB(60, 240, 180), RGB(40, 200, 220), RGB(150, 110, 240)};
    for (int32_t x = t->x0; x < t->x1; ++x) {
        int32_t ax = (int32_t)((int64_t)x * 1024 / (w > 0 ? w : 1));
        int32_t cy[3], thick[3]; uint32_t curtain[3];
        for (int i = 0; i < 3; ++i) {
            cy[i] = h * (300 + i * 110) / 1000 + (int32_t)((int64_t)h * (wp_sin(ax * (2 + i) + i * 300) + wp_sin(ax * 5 + i * 170) / 3) / (16384 * 9));
            thick[i] = h * (90 - i * 15) / 1000 + 1;
            int32_t s = wp_sin(ax * 23 + i * 97) + wp_sin(ax * 41 + i * 13) / 2;
            curtain[i] = (uint32_t)(150 + s / 300);
        }
        for (int32_t y = t->y0; y < t->y1; ++y) {
            uint32_t c = mix(RGB(4, 10, 26), RGB(10, 34, 52), (uint32_t)(y * 256 / (h + 1)));
            uint32_t hsh = hash32((uint32_t)x * 2654435761u ^ (uint32_t)y * 40503u);
            if ((hsh & 2047u) == 0u) c = add_light(c, RGB(255, 255, 255), (hsh >> 11) & 255u);
            for (int i = 0; i < 3; ++i) {
                int32_t d = y - cy[i];
                if (d < -thick[i] * 3 || d > thick[i]) continue;
                /* sharp lower edge, long fade upwards like light curtains */
                uint32_t a = d >= 0 ? (uint32_t)((thick[i] - d) * 256 / thick[i]) : (uint32_t)((thick[i] * 3 + d) * 256 / (thick[i] * 3));
                a = (a * a) >> 8;
                c = add_light(c, col[i], (a * curtain[i]) >> 9);
            }
            put(t, x, y, c);
        }
    }
    /* distant ridge */
    for (int32_t x = t->x0; x < t->x1; ++x) {
        int32_t ridge = hill16(w, h, x, 3, 1) / 16 + h / 12;
        for (int32_t y = ridge > t->y0 ? ridge : t->y0; y < t->y1; ++y) put(t, x, y, RGB(4, 14, 20));
    }
}

static void render_dew(const target *t) {
    int32_t w = t->w, h = t->h;
    for (int32_t y = t->y0; y < t->y1; ++y)
        for (int32_t x = t->x0; x < t->x1; ++x) {
            uint32_t g = (uint32_t)(((int64_t)x + (int64_t)y * 2) * 256 / ((int64_t)w + (int64_t)h * 2 + 1));
            put(t, x, y, mix(RGB(18, 120, 120), RGB(10, 60, 80), g));
        }
    for (uint32_t i = 0; i < 28u; ++i) {
        uint32_t r1 = hash32(i * 977u + 13u), r2 = hash32(i * 31337u + 7u);
        int32_t cx = (int32_t)(r1 % 1920u) * w / 1920, cy = (int32_t)(r2 % 1080u) * h / 1080;
        int32_t r = (int32_t)(30u + (r1 >> 20) % 110u) * h / 1080 + 1;
        uint32_t tone = (r2 >> 16) & 3u;
        uint32_t color = tone == 0u ? RGB(160, 255, 220) : tone == 1u ? RGB(120, 230, 240) : tone == 2u ? RGB(220, 255, 240) : RGB(90, 210, 170);
        uint32_t alpha = 30u + (r2 >> 24) % 50u;
        int32_t bx0 = cx - r - 2, bx1 = cx + r + 2, by0 = cy - r - 2, by1 = cy + r + 2;
        if (bx0 < t->x0) bx0 = t->x0;
        if (by0 < t->y0) by0 = t->y0;
        if (bx1 > t->x1) bx1 = t->x1;
        if (by1 > t->y1) by1 = t->y1;
        for (int32_t y = by0; y < by1; ++y)
            for (int32_t x = bx0; x < bx1; ++x) {
                int64_t dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy, r2s = (int64_t)r * r;
                if (d2 >= r2s) continue;
                int64_t edge = (r2s - d2) / (2 * r + 1);
                uint32_t a = edge >= 2 ? 256u : (uint32_t)(edge * 128);
                uint32_t rim = d2 * 256 / r2s > 200 ? 60u : 0u;             /* brighter rim, like a lens */
                uint32_t *p = &t->px[(int64_t)(y - t->y0) * t->stride + (x - t->x0)];
                *p = add_light(*p, color, ((alpha + rim) * a) >> 8);
            }
    }
}

static void render_solid(const target *t, uint32_t id) {
    static const uint32_t top[5] = {RGB(98, 214, 176), RGB(26, 128, 136), RGB(70, 74, 80), RGB(16, 22, 44), RGB(232, 216, 186)};
    static const uint32_t bot[5] = {RGB(88, 204, 166), RGB(22, 118, 128), RGB(60, 64, 70), RGB(12, 18, 38), RGB(222, 204, 172)};
    uint32_t i = id - WP_SOLID_MINT;
    for (int32_t y = t->y0; y < t->y1; ++y) {
        uint32_t c = mix(top[i], bot[i], (uint32_t)(y * 256 / (t->h + 1)));     /* a whisper of depth */
        for (int32_t x = t->x0; x < t->x1; ++x) put(t, x, y, c);
    }
}

void wallpaper_render(uint32_t *px, int32_t stride, int32_t w, int32_t h,
                      int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t id, uint32_t minute) {
    if (!px || w <= 0 || h <= 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0 || stride < x1 - x0) return;
    target t = {px, stride, w, h, x0, y0, x1, y1};
    wp_sky sky;
    switch (id) {
    case WP_MINT_DAY:
        wallpaper_sky(minute, &sky);
        render_landscape(&t, &sky, 0);
        break;
    case WP_DUNES:
        wallpaper_sky(1095u, &sky);                   /* golden hour, fixed */
        sky.hills[0] = RGB(236, 170, 140); sky.hills[1] = RGB(200, 150, 136);
        sky.hills[2] = RGB(110, 170, 150); sky.hills[3] = RGB(40, 130, 112);
        sky.sun_x = 300; sky.sun_y = 520;
        render_landscape(&t, &sky, 1);
        break;
    case WP_LEAF_LIGHT: render_leaves(&t); break;
    case WP_AURORA: render_aurora(&t); break;
    case WP_DEW: render_dew(&t); break;
    case WP_SOLID_MINT: case WP_SOLID_TEAL: case WP_SOLID_GRAPHITE: case WP_SOLID_MIDNIGHT: case WP_SOLID_SAND:
        render_solid(&t, id);
        break;
    default:                                          /* photos: a stylised stand-in */
        wallpaper_sky(id == WP_MINT_WAVES_DARK ? 1300u : 720u, &sky);
        render_landscape(&t, &sky, 1);
        break;
    }
}
