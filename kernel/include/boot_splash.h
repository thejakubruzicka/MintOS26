#ifndef MINTOS26_BOOT_SPLASH_H
#define MINTOS26_BOOT_SPLASH_H
#include <stdint.h>

/* MintOS boot animation (26.5.014.0): a mint leaf over a deep-green glow
 * with a thin progress bar. kernel/ui/splash_art.c is a pure integer
 * renderer shared by the kernel splash (kernel/ui/boot_splash.c), MintDE's
 * crossfade into the desktop and the host preview tool, so the hand-off is
 * pixel-identical. Colours are 0x00RRGGBB. */
typedef struct mint_splash_geometry {
    int32_t width, height;
    int32_t cx, cy;                /* leaf centre */
    int32_t leaf;                  /* leaf half-length in pixels */
    int32_t box_x0, box_y0, box_x1, box_y1;   /* leaf bounding box */
    int32_t bar_x, bar_y, bar_w, bar_h;
} MINT_SPLASH_GEOMETRY;

void mint_splash_geometry(MINT_SPLASH_GEOMETRY *g, int32_t width, int32_t height);
uint32_t mint_splash_background(const MINT_SPLASH_GEOMETRY *g, int32_t x, int32_t y);
/* Render [x0,x1) x [y0,y1); pixel (x,y) goes to px[(y-y0)*stride + (x-x0)]. leaf_alpha 0..255,
 * progress 0..1000 (bar hidden when > 1000), shimmer = animation phase. */
void mint_splash_render(uint32_t *px, int32_t stride, const MINT_SPLASH_GEOMETRY *g,
                        int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                        uint32_t leaf_alpha, uint32_t progress, uint32_t shimmer);

#ifdef MINT_KERNEL_SPLASH
#include "desktop_abi.h"
void mint_boot_splash_begin(MINT_DISPLAY_SURFACE *surface);
void mint_boot_splash_progress(uint32_t permille);
void mint_boot_splash_task(void *argument);
/* Diagnostic hand-over: stop animating so the kernel status panel can draw. */
void mint_boot_splash_abort(void);
#endif
/* Called by MintDE (MINT_SYS_BOOT_SPLASH) right before it maps the canvas. */
void mint_boot_splash_finish(void);
int mint_boot_splash_active(void);
#endif
