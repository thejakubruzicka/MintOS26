#ifndef MINTDE_GFX_H
#define MINTDE_GFX_H
#include <stdint.h>
#include <stddef.h>
#include "font.h"

/* MintDE 2 software renderer. Draws 0x00RRGGBB pixels into the kernel-shared
 * canvas (see MINT_DISPLAY_CANVAS). Colors passed to the API are 0xAARRGGBB;
 * alpha 0xFF is opaque. Integer arithmetic only (ring-3 ABI forbids SSE).
 * Every primitive is clipped to the global clip rectangle. */
typedef struct gfx_surface {
    uint32_t *px;
    int32_t w, h, stride;
} gfx_surface;

typedef struct gfx_rect { int32_t x0, y0, x1, y1; } gfx_rect;

#define ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define OPAQUE(rgb) (0xFF000000u | (uint32_t)(rgb))
#define WITH_ALPHA(rgb, a) (((uint32_t)(a) << 24) | ((uint32_t)(rgb) & 0xFFFFFFu))

extern gfx_surface gfx_screen;     /* presented canvas */
extern gfx_surface gfx_wall;       /* cached wallpaper at screen size */
extern gfx_surface gfx_scratch;    /* temporary buffer (blur, offscreen) */

gfx_rect gfx_rect_xywh(int32_t x, int32_t y, int32_t w, int32_t h);
gfx_rect gfx_rect_intersect(gfx_rect a, gfx_rect b);
gfx_rect gfx_rect_union(gfx_rect a, gfx_rect b);
int gfx_rect_empty(gfx_rect r);
int gfx_rect_contains(gfx_rect r, int32_t x, int32_t y);
int gfx_rect_overlaps(gfx_rect a, gfx_rect b);

void gfx_clip_set(gfx_rect r);    /* intersected with the screen */
gfx_rect gfx_clip_get(void);

uint32_t gfx_mix(uint32_t dst, uint32_t src_rgb, uint32_t alpha);
uint32_t gfx_lerp_rgb(uint32_t a, uint32_t b, uint32_t t255);

void gfx_fill(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t argb);
void gfx_round_rect(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t argb);
void gfx_round_rect_grad(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                         uint32_t argb_top, uint32_t argb_bottom);
void gfx_round_rect_stroke(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                           uint32_t argb);
void gfx_circle(gfx_surface *s, int32_t cx, int32_t cy, int32_t radius, uint32_t argb);
void gfx_circle_stroke(gfx_surface *s, int32_t cx, int32_t cy, int32_t radius, uint32_t argb);
void gfx_shadow(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r,
                int32_t blur, int32_t dy, uint32_t argb);
void gfx_line(gfx_surface *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb);
void gfx_hline(gfx_surface *s, int32_t x, int32_t y, int32_t w, uint32_t argb);
void gfx_vline(gfx_surface *s, int32_t x, int32_t y, int32_t h, uint32_t argb);
void gfx_copy(gfx_surface *dst, const gfx_surface *src, gfx_rect r);
void gfx_blur(gfx_surface *s, gfx_rect r, int32_t radius);
void gfx_blur_rounded(gfx_surface *s, gfx_rect r, int32_t radius, int32_t corner);
void gfx_corners_save(const gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t *buf);
void gfx_corners_restore(gfx_surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, const uint32_t *buf);
void gfx_image(gfx_surface *s, int32_t x, int32_t y, int32_t dw, int32_t dh,
               const uint8_t *rgba, int32_t sw, int32_t sh, uint32_t alpha);
void gfx_mask(gfx_surface *s, int32_t x, int32_t y, int32_t dw, int32_t dh,
              const uint8_t *a8, int32_t sw, int32_t sh, uint32_t argb);

/* Text. y is the BASELINE. Returns the advance in pixels. */
int32_t gfx_text(gfx_surface *s, int font, int32_t x, int32_t y, const char *utf8, uint32_t argb);
int32_t gfx_text_width(int font, const char *utf8);
/* Draw with "…" truncation to max_w. Returns the drawn width. */
int32_t gfx_text_fit(gfx_surface *s, int font, int32_t x, int32_t y, const char *utf8,
                     int32_t max_w, uint32_t argb);
/* Fixed-cell rendering (pseudo-monospace for Terminal). */
int32_t gfx_text_cells(gfx_surface *s, int font, int32_t x, int32_t y, const char *utf8,
                       int32_t cell_w, uint32_t argb);
int32_t gfx_font_ascent(int font);
int32_t gfx_font_line(int font);
#endif
