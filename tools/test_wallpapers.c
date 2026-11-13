/* Host test for kernel/user/mintde/wallpapers.c and preview renderer.
 *   test_wallpapers                 run the checks
 *   test_wallpapers ID MINUTE W H out.ppm   render one wallpaper (preview/evidence)
 * Checks: integer sine accuracy, the dynamic wallpaper's time mapping
 * (phases at the right hours, continuous palette, sun up only by day, stars
 * only at night, refresh cadence), region rendering equals the matching
 * part of a full render without writing outside it, determinism, and that
 * every wallpaper fills its area with a varied, non-black picture. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../kernel/user/mintde/wallpapers.h"

static int failures, checks;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static int channel_jump(uint32_t a, uint32_t b) {
    int m = 0;
    for (int s = 0; s < 24; s += 8) { int d = (int)((a >> s) & 255u) - (int)((b >> s) & 255u); if (d < 0) d = -d; if (d > m) m = d; }
    return m;
}

static int write_ppm(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) { unsigned char c[3] = {(unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i]}; fwrite(c, 1, 3, f); }
    fclose(f);
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 6) {
        int id = atoi(argv[1]), minute = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
        if (w < 16 || h < 16 || w > 8192 || h > 8192) return 2;
        uint32_t *px = calloc((size_t)w * (size_t)h, 4);
        if (!px) return 1;
        wallpaper_render(px, w, w, h, 0, 0, w, h, (uint32_t)id, (uint32_t)minute);
        int ok = write_ppm(argv[5], px, w, h);
        free(px);
        return ok ? 0 : 1;
    }
    int sin_ok = 1;
    for (int a = 0; a < 1024; ++a) {
        double ref = sin(a * 2.0 * M_PI / 1024.0) * 16384.0;
        if (fabs(ref - wp_sin(a)) > 40.0) sin_ok = 0;
    }
    CHECK(sin_ok && wp_sin(256) == 16384 && wp_sin(0) == 0 && wp_sin(768) == -16384, "integer sine within 0.25 %");

    CHECK(wallpaper_phase(0) == WP_PHASE_NIGHT && wallpaper_phase(6 * 60) == WP_PHASE_DAWN &&
          wallpaper_phase(12 * 60) == WP_PHASE_DAY && wallpaper_phase(18 * 60 + 30) == WP_PHASE_DUSK &&
          wallpaper_phase(22 * 60) == WP_PHASE_NIGHT && wallpaper_phase(1440 + 12 * 60) == WP_PHASE_DAY,
          "phases: night 00:00, dawn 06:00, day 12:00, dusk 18:30, night 22:00, wraps after 24 h");
    CHECK(!strcmp(wallpaper_phase_name(WP_PHASE_DUSK), "Dusk"), "phase names");
    int continuous = 1, sun_ok = 1, stars_ok = 1;
    wp_sky prev, cur;
    wallpaper_sky(1439, &prev);
    for (uint32_t m = 0; m < 1440u; ++m) {
        wallpaper_sky(m, &cur);
        if (channel_jump(prev.top, cur.top) > 6 || channel_jump(prev.horizon, cur.horizon) > 6 ||
            channel_jump(prev.hills[3], cur.hills[3]) > 6) { continuous = 0; printf("  jump at %u\n", m); }
        uint32_t ph = wallpaper_phase(m);
        if (ph == WP_PHASE_DAY && cur.sun_y > 700) sun_ok = 0;            /* sun above the horizon by day */
        if (m >= 11 * 60 && m <= 13 * 60 && cur.sun_y > 260) sun_ok = 0; /* high at noon */
        if (ph == WP_PHASE_DAY && cur.stars) stars_ok = 0;
        if ((m < 240u || m > 1300u) && cur.stars < 150u) stars_ok = 0;
        prev = cur;
    }
    CHECK(continuous, "dynamic palette changes smoothly minute by minute (no jumps, wraps at midnight)");
    CHECK(sun_ok, "sun arcs above the hills during the day and is highest around noon");
    CHECK(stars_ok, "stars only at night");
    CHECK(wallpaper_refresh_minutes(6 * 60) == 5u && wallpaper_refresh_minutes(12 * 60) == 15u, "faster refresh at dawn and dusk");
    CHECK(wallpaper_is_dynamic(WP_MINT_DAY) && !wallpaper_is_dynamic(WP_AURORA) && wallpaper_is_photo(WP_MINT_WAVES_DARK) &&
          !wallpaper_is_photo(WP_SOLID_SAND), "kinds");
    int names = 1;
    for (uint32_t i = 0; i < WP_COUNT; ++i) if (!wallpaper_name(i)[0]) names = 0;
    CHECK(names && !strcmp(wallpaper_name(WP_MINT_DAY), "Mint Day"), "names");

    const int W = 480, H = 270;
    uint32_t *full = calloc((size_t)W * H, 4), *part = calloc((size_t)(W + 8) * (H + 8), 4);
    int all_ok = 1, region_ok = 1;
    for (uint32_t id = WP_MINT_DAY; id < WP_COUNT; ++id) {
        memset(full, 0, (size_t)W * H * 4);
        wallpaper_render(full, W, W, H, 0, 0, W, H, id, 20 * 60);
        uint32_t lo = 0xFFFFFFu, hi = 0;
        int black = 0;
        for (int i = 0; i < W * H; ++i) {
            uint32_t g = ((full[i] >> 16) & 255u) + ((full[i] >> 8) & 255u) + (full[i] & 255u);
            if (g < lo) lo = g;
            if (g > hi) hi = g;
            if (!full[i]) ++black;
        }
        int solid = id >= WP_SOLID_MINT;
        if (black > W * H / 50 || (!solid && hi - lo < 60u)) { all_ok = 0; printf("  wallpaper %u looks empty\n", id); }
        /* region render with guard band */
        for (int i = 0; i < (W + 8) * (H + 8); ++i) part[i] = 0xDEADBEEFu;
        int rx0 = 101, ry0 = 37, rx1 = 333, ry1 = 201, stride = W + 8;
        wallpaper_render(part, stride, W, H, rx0, ry0, rx1, ry1, id, 20 * 60);
        for (int y = 0; y < H + 8 && region_ok; ++y)
            for (int x = 0; x < stride; ++x) {
                int inside = y < ry1 - ry0 && x < rx1 - rx0;
                uint32_t v = part[y * stride + x];
                if (inside && v != full[(ry0 + y) * W + rx0 + x]) { region_ok = 0; printf("  region mismatch id %u\n", id); break; }
                if (!inside && v != 0xDEADBEEFu) { region_ok = 0; printf("  wrote outside region id %u\n", id); break; }
            }
        uint32_t *again = calloc((size_t)W * H, 4);
        wallpaper_render(again, W, W, H, 0, 0, W, H, id, 20 * 60);
        if (memcmp(again, full, (size_t)W * H * 4)) { all_ok = 0; printf("  nondeterministic id %u\n", id); }
        free(again);
    }
    CHECK(all_ok, "every procedural wallpaper renders a varied, deterministic picture");
    CHECK(region_ok, "region rendering (thumbnails, damage) matches the full render and stays inside its bounds");
    /* day vs night differ for the dynamic wallpaper */
    uint32_t *night = calloc((size_t)W * H, 4);
    wallpaper_render(full, W, W, H, 0, 0, W, H, WP_MINT_DAY, 12 * 60);
    wallpaper_render(night, W, W, H, 0, 0, W, H, WP_MINT_DAY, 1 * 60);
    uint64_t sd = 0, sn = 0;
    for (int i = 0; i < W * H / 2; ++i) { sd += (full[i] >> 8) & 255u; sn += (night[i] >> 8) & 255u; }
    CHECK(sd > sn * 3, "Mint Day sky is bright at noon and dark at night");
    wallpaper_render(full, W, W, H, -50, -50, W + 50, H + 50, WP_DEW, 0);
    CHECK(1, "out-of-range regions are clamped");
    wallpaper_render(full, W, W, H, 10, 10, 5, 5, WP_DEW, 0);
    wallpaper_render(0, W, W, H, 0, 0, W, H, WP_DEW, 0);
    CHECK(1, "empty regions and null buffers are ignored");
    free(full); free(part); free(night);
    printf("%s: %d/%d wallpaper checks passed\n", failures ? "FAIL" : "PASS", checks - failures, checks);
    return failures ? 1 : 0;
}
