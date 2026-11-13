/* MintOS boot animation on the native kernel framebuffer (26.5.014.0).
 *
 * kernel_entry draws the first frame right after the GOP surface is
 * validated and reports progress at each boot stage. Once the scheduler
 * runs, the "splash" task fades the leaf in, eases the progress bar towards
 * the latest stage and sweeps a soft highlight along it (~25 fps, only the
 * leaf box and the bar are redrawn). MintDE calls MINT_SYS_BOOT_SPLASH
 * FINISH before mapping its canvas: the final frame is drawn synchronously
 * and MintDE crossfades from an identical rendering into the desktop.
 * If the desktop cannot start, kernel.c aborts the splash and shows the
 * diagnostic panel instead. Serial logging is unchanged. */
#include <stdint.h>
#define MINT_KERNEL_SPLASH 1
#include "../include/boot_splash.h"
#include "../include/desktop_abi.h"
#include "../include/task.h"
#include "../include/arch.h"
#include "../include/serial.h"

static MINT_DISPLAY_SURFACE *surface;
static MINT_SPLASH_GEOMETRY geom;
static volatile uint32_t active, task_running;
static uint32_t target, shown, alpha, frame;
static uint32_t row[4096];

static uint32_t encode(uint32_t rgb) {
    if (surface->pixel_format == 0u)
        return ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu);
    return rgb & 0xFFFFFFu;
}

static void draw(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t shimmer) {
    if (!surface || !surface->initialized || !surface->pixels) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > geom.width) x1 = geom.width;
    if (y1 > geom.height) y1 = geom.height;
    if (x1 <= x0 || y1 <= y0 || x1 - x0 > 4096) return;
    for (int32_t y = y0; y < y1; ++y) {
        mint_splash_render(row, 4096, &geom, x0, y, x1, y + 1, alpha, shown, shimmer);
        volatile uint32_t *dst = surface->pixels + (uint64_t)y * surface->stride_pixels;
        for (int32_t x = x0; x < x1; ++x) dst[x] = encode(row[x - x0]);
    }
    __asm__ volatile("sfence" ::: "memory");
}

static void draw_bar(uint32_t shimmer) {
    draw(geom.bar_x - 2, geom.bar_y - 1, geom.bar_x + geom.bar_w + 2, geom.bar_y + geom.bar_h + 1, shimmer);
}

void mint_boot_splash_begin(MINT_DISPLAY_SURFACE *s) {
    if (!s || !s->initialized || s->width < 320u || s->height < 240u || s->width > 4096u) return;
    surface = s;
    mint_splash_geometry(&geom, (int32_t)s->width, (int32_t)s->height);
    alpha = 0; shown = 0; target = 20; frame = 0;
    active = 1;
    draw(0, 0, geom.width, geom.height, 0);
    mint_serial_write("[splash] MintOS boot animation started\n");
}

int mint_boot_splash_active(void) { return active != 0; }

void mint_boot_splash_progress(uint32_t permille) {
    if (!active) return;
    if (permille > 1000u) permille = 1000u;
    if (permille > target) target = permille;
    if (!task_running) {                       /* early boot: no scheduler, draw now */
        shown = target;
        draw_bar(0);
    }
}

void mint_boot_splash_task(void *argument) {
    (void)argument;
    if (!active) return;
    task_running = 1;
    uint32_t idle_creep = 0;
    while (active) {
        ++frame;
        if (alpha < 255u) {
            alpha = alpha + 20u > 255u ? 255u : alpha + 20u;
            draw(geom.box_x0, geom.box_y0, geom.box_x1, geom.box_y1, 0);
        }
        if (shown < target) {
            uint32_t step = (target - shown) / 5u;
            shown += step < 3u ? (target - shown < 3u ? target - shown : 3u) : step;
        } else if (target < 900u && ++idle_creep % 3u == 0u) {
            ++target;                          /* keep moving slowly between stages */
        }
        draw_bar(frame * (uint32_t)(geom.bar_w / 24 + 1));
        mint_task_sleep_ticks(4);
    }
    task_running = 0;
}

void mint_boot_splash_finish(void) {
    if (!active) return;
    alpha = 255u;
    shown = 1000u;
    target = 1000u;
    draw(geom.box_x0, geom.box_y0, geom.box_x1, geom.box_y1, 0);
    draw_bar(0);
    active = 0;
    mint_serial_write("[splash] boot animation finished; MintDE takes over the display\n");
}

void mint_boot_splash_abort(void) {
    if (!active) return;
    active = 0;
    mint_serial_write("[splash] boot animation stopped; diagnostic panel shown\n");
}
