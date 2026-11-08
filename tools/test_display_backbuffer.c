/* Host test for the production native display surface (kernel/ui/display_surface.c):
 * GOP modes with padded strides and resolutions above 1080p must initialize,
 * frames must fail closed until kernel-owned backbuffers are attached, and the
 * damage-rectangle present must copy exactly the drawn pixels to scanout. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/desktop_abi.h"

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); return 1; } \
                         printf("PASS: %s\n", m); } while (0)

static int run_mode(uint32_t w, uint32_t h, uint32_t stride, const char *label) {
    uint64_t pixels = (uint64_t)stride * h;
    uint32_t *scanout = calloc(pixels, 4);
    uint32_t *back = calloc(pixels, 4), *bg = calloc(pixels, 4);
    if (!scanout || !back || !bg) return 1;
    for (uint64_t i = 0; i < pixels; ++i) scanout[i] = 0x00102030u;
    MINT_FRAMEBUFFER_INFO fb = { (uint64_t)(uintptr_t)scanout, pixels * 4, w, h, stride, 1 };
    static MINT_DISPLAY_SURFACE surface;
    char msg[160];
    snprintf(msg, sizeof msg, "%s: surface initializes (%ux%u stride %u)", label, w, h, stride);
    CHECK(mint_display_surface_init(&surface, &fb), msg);
    CHECK(mint_display_backbuffer_pixels_required() == pixels, "required pixels = stride x height");
    CHECK(!mint_display_frame_begin(), "frame begin fails closed before backbuffers are attached");
    CHECK(!mint_display_attach_backbuffers(back, bg, pixels - 1), "undersized backbuffer is rejected");
    CHECK(!mint_display_attach_backbuffers(back, back, pixels), "aliased backbuffer/backdrop is rejected");
    CHECK(mint_display_attach_backbuffers(back, bg, pixels), "kernel-owned backbuffers attach");
    CHECK(back[(uint64_t)(h - 1) * stride + (w - 1)] == 0x00102030u, "attach seeds the retained frame from scanout");
    CHECK(mint_display_frame_begin(), "frame begins after attach");
    CHECK(mint_display_current_fill_rect((int32_t)w - 10, (int32_t)h - 6, 10, 6, 0xAABBCCu), "fill in bottom-right corner");
    CHECK(scanout[(uint64_t)(h - 1) * stride + (w - 1)] == 0x00102030u, "drawing stays in the backbuffer until present");
    CHECK(mint_display_frame_end(), "frame presents");
    CHECK(scanout[(uint64_t)(h - 1) * stride + (w - 1)] == 0x00AABBCCu &&
          scanout[(uint64_t)(h - 6) * stride + (w - 10)] == 0x00AABBCCu, "damaged rectangle reached scanout");
    CHECK(scanout[(uint64_t)(h - 7) * stride + (w - 10)] == 0x00102030u &&
          scanout[(uint64_t)(h - 1) * stride + (w - 11)] == 0x00102030u, "pixels outside the damage rectangle untouched");
    if (stride > w)
        CHECK(scanout[(uint64_t)(h - 1) * stride + w] == 0x00102030u, "stride padding is never written");
    free(scanout); free(back); free(bg);
    return 0;
}

int main(void) {
    if (run_mode(1920, 1080, 1920, "1080p")) return 1;
    if (run_mode(1920, 1080, 2048, "1080p padded stride")) return 1;   /* halted in 26.5.008.0 */
    if (run_mode(2560, 1440, 2560, "1440p")) return 1;                 /* halted in 26.5.008.0 */
    if (run_mode(3840, 2160, 3840, "4K UHD")) return 1;
    if (run_mode(1366, 768, 1376, "odd-width laptop panel")) return 1;
    static MINT_DISPLAY_SURFACE big;
    uint32_t dummy = 0;
    MINT_FRAMEBUFFER_INFO huge = { (uint64_t)(uintptr_t)&dummy, 5120ull * 2880ull * 4ull, 5120, 2880, 5120, 1 };
    CHECK(!mint_display_surface_init(&big, &huge), "5K mode beyond MINT_DISPLAY_MAX_PIXELS is refused");
    return 0;
}
