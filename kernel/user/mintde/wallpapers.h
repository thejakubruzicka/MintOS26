#ifndef MINTDE_WALLPAPERS_H
#define MINTDE_WALLPAPERS_H
/* MintOS wallpaper collection (26.5.014.0). Ids 0-2 are the embedded
 * "Mint Waves" photos rendered by the kernel; everything else is generated
 * procedurally with integer math at any size, so the collection adds no
 * image data to the kernel. Pure code: also built on the host by
 * tools/test_wallpapers.c and tools/render_wallpaper_preview.c. */
#include <stdint.h>

enum {
    WP_MINT_WAVES_AUTO = 0, WP_MINT_WAVES_LIGHT = 1, WP_MINT_WAVES_DARK = 2,
    WP_MINT_DAY = 3,            /* dynamic: dawn, day, dusk, night from the clock */
    WP_LEAF_LIGHT = 4, WP_AURORA = 5, WP_DEW = 6, WP_DUNES = 7,
    WP_SOLID_MINT = 8, WP_SOLID_TEAL = 9, WP_SOLID_GRAPHITE = 10, WP_SOLID_MIDNIGHT = 11, WP_SOLID_SAND = 12,
    WP_COUNT = 13
};
enum { WP_PHASE_NIGHT = 0, WP_PHASE_DAWN = 1, WP_PHASE_DAY = 2, WP_PHASE_DUSK = 3 };

typedef struct wp_sky {
    uint32_t top, horizon;          /* sky gradient */
    uint32_t hills[4];              /* far -> near */
    uint32_t haze;                  /* blended into far hills */
    uint32_t sun;                   /* disc colour (sun or moon) */
    uint32_t glow;                  /* glow colour around it */
    int32_t sun_x, sun_y;           /* permille of width / height (y may be > 1000: below the horizon) */
    uint32_t stars;                 /* 0..255 star visibility */
} wp_sky;

const char *wallpaper_name(uint32_t id);
int wallpaper_is_photo(uint32_t id);       /* rendered by the kernel (Mint Waves) */
int wallpaper_is_dynamic(uint32_t id);
/* Dynamic wallpaper: minute of the day (0..1439) -> phase and palette. */
uint32_t wallpaper_phase(uint32_t minute);
const char *wallpaper_phase_name(uint32_t phase);
void wallpaper_sky(uint32_t minute, wp_sky *out);
/* Minutes until the dynamic wallpaper should be redrawn (palette moved enough). */
uint32_t wallpaper_refresh_minutes(uint32_t minute);
/* Render a w x h image of wallpaper `id`; only [x0,x1) x [y0,y1) is written,
 * pixel (x,y) to px[(y-y0)*stride + (x-x0)] as 0x00RRGGBB. */
void wallpaper_render(uint32_t *px, int32_t stride, int32_t w, int32_t h,
                      int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t id, uint32_t minute);
/* Integer sine: angle 0..1023 = one turn; returns -16384..16384. */
int32_t wp_sin(int32_t angle);
#endif
