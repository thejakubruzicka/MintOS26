/* Host tests for MintDE 2: the userspace renderer (gfx.c + font_data.c), the
 * kernel shared-canvas present / overlay cursor (display_surface.c) and the
 * CMOS RTC date math (rtc.c). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/user/mintde/gfx.h"
#include "../kernel/include/desktop_abi.h"
#include "../kernel/include/rtc.h"

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); return 1; } printf("PASS: %s\n", m); } while (0)
#define WID 320
#define HEI 200

static uint32_t screen[WID * HEI], scratch[WID * HEI], wall[WID * HEI];
static uint32_t px(int x, int y) { return screen[y * WID + x]; }
static int ch(uint32_t c, int sh) { return (int)((c >> sh) & 255u); }

int main(void) {
    gfx_screen = (gfx_surface){screen, WID, HEI, WID};
    gfx_scratch = (gfx_surface){scratch, WID, HEI, WID};
    gfx_wall = (gfx_surface){wall, WID, HEI, WID};
    gfx_clip_set(gfx_rect_xywh(0, 0, WID, HEI));

    /* blending */
    CHECK(gfx_mix(0x000000u, 0xFFFFFFu, 255u) == 0xFFFFFFu && gfx_mix(0x123456u, 0xFFFFFFu, 0u) == 0x123456u,
          "alpha 255 replaces, alpha 0 keeps");
    uint32_t half = gfx_mix(0x000000u, 0xFFFFFFu, 128u);
    CHECK(ch(half, 16) >= 127 && ch(half, 16) <= 129 && ch(half, 8) == ch(half, 16), "50% blend is mid-grey on every channel");

    /* rounded rectangle: interior solid, corners anti-aliased, outside untouched */
    gfx_fill(&gfx_screen, 0, 0, WID, HEI, OPAQUE(0x000000u));
    gfx_round_rect(&gfx_screen, 20, 20, 100, 60, 10, OPAQUE(0xFFFFFFu));
    CHECK(px(70, 50) == 0xFFFFFFu && px(19, 50) == 0 && px(120, 50) == 0, "rounded rect fills inside its bounds only");
    CHECK(px(20, 20) == 0 && px(119, 79) == 0, "rounded rect leaves the extreme corner pixels empty");
    int edge = ch(px(22, 23), 16);
    CHECK(edge > 0 && edge < 255, "corner arc pixels are partially covered (anti-aliased)");
    CHECK(ch(px(22, 23), 16) == ch(px(117, 23), 16) && ch(px(22, 23), 16) == ch(px(22, 76), 16),
          "corner coverage is symmetric left/right and top/bottom");

    /* clipping */
    gfx_fill(&gfx_screen, 0, 0, WID, HEI, OPAQUE(0x000000u));
    gfx_clip_set(gfx_rect_xywh(50, 50, 10, 10));
    gfx_fill(&gfx_screen, 0, 0, WID, HEI, OPAQUE(0xFF0000u));
    gfx_clip_set(gfx_rect_xywh(0, 0, WID, HEI));
    CHECK(px(55, 55) == 0xFF0000u && px(49, 55) == 0 && px(60, 55) == 0, "all drawing respects the clip rectangle");

    /* blur never writes outside its target rectangle */
    for (int y = 0; y < HEI; ++y) for (int x = 0; x < WID; ++x) screen[y * WID + x] = (x / 8 + y / 8) & 1 ? 0xFFFFFFu : 0;
    uint32_t before_out = px(39, 60), before_out2 = px(161, 60), before_in = px(100, 60);
    gfx_blur_rounded(&gfx_screen, gfx_rect_xywh(40, 40, 120, 60), 12, 10);
    CHECK(px(39, 60) == before_out && px(161, 60) == before_out2, "blur leaves pixels outside the rectangle untouched");
    CHECK(px(40, 40) == ((40 / 8 + 40 / 8) & 1 ? 0xFFFFFFu : 0u), "rounded blur keeps the outer corner pixel unblurred");
    int v = ch(px(100, 60), 16);
    CHECK(v > 60 && v < 195 && px(100, 60) != before_in, "blur averages the checkerboard towards grey inside");

    /* corner save/restore re-rounds square-drawn window content */
    static uint32_t corners[4 * 10 * 10];
    gfx_fill(&gfx_screen, 0, 0, WID, HEI, OPAQUE(0x0000FFu));
    gfx_corners_save(&gfx_screen, 100, 100, 80, 60, 10, corners);
    gfx_fill(&gfx_screen, 100, 100, 80, 60, OPAQUE(0xFFFFFFu));
    gfx_corners_restore(&gfx_screen, 100, 100, 80, 60, 10, corners);
    CHECK(px(100, 100) == 0x0000FFu && px(179, 159) == 0x0000FFu && px(140, 130) == 0xFFFFFFu,
          "corner restore shows what was behind outside the rounded window shape");

    /* text */
    int32_t w1 = gfx_text_width(FONT_BODY, "Files"), w2 = gfx_text_width(FONT_BODY, "Files Files");
    CHECK(w1 > 20 && w1 < 60 && w2 > 2 * w1, "text width is proportional and additive");
    CHECK(gfx_text_width(FONT_TITLE, "Files") > w1, "larger faces measure wider");
    CHECK(gfx_text_width(FONT_BODY, "\xE2\x8C\x98") > 0 && gfx_text_width(FONT_BODY, "\xE2\x88\x92") > 0,
          "UTF-8 symbols (Command key, minus sign) have glyphs");
    gfx_fill(&gfx_screen, 0, 0, WID, HEI, OPAQUE(0xFFFFFFu));
    int32_t drawn = gfx_text_fit(&gfx_screen, FONT_BODY, 10, 40, "A very long file name that cannot fit", 80, OPAQUE(0x000000u));
    CHECK(drawn <= 80, "truncated text stays within its width");
    int dark = 0; for (int x = 10; x < 95; ++x) for (int y = 28; y < 44; ++y) if (ch(px(x, y), 16) < 128) ++dark;
    CHECK(dark > 40, "text rendering produces visible glyph coverage");

    /* kernel shared canvas: present converts channel order, cursor erases cleanly */
    static uint32_t fb[WID * HEI], back[WID * HEI], bg[WID * HEI], scr[WID * HEI];
    MINT_FRAMEBUFFER_INFO info = {(uint64_t)(uintptr_t)fb, sizeof(fb), WID, HEI, WID, 0u /* RGB */};
    static MINT_DISPLAY_SURFACE surface;
    CHECK(mint_display_surface_init(&surface, &info) &&
          mint_display_attach_backbuffers(back, bg, WID * HEI) &&
          mint_display_attach_scratch(scr, WID * HEI), "surface with canvas, wallpaper and scratch buffers");
    uint64_t cpa = 0, wpa = 0, spa = 0, bytes = 0;
    CHECK(mint_display_canvas_buffers(&cpa, &wpa, &spa, &bytes) && cpa == (uint64_t)(uintptr_t)back &&
          bytes >= WID * HEI * 4u, "canvas buffer addresses are reported for mapping");
    mint_display_enter_canvas_mode();
    CHECK(!mint_display_frame_begin(), "legacy frame API fails closed in canvas mode");
    for (int i = 0; i < WID * HEI; ++i) back[i] = 0x112233u;
    CHECK(mint_display_present_canvas(0, 0, WID, HEI), "present the whole canvas");
    CHECK(fb[5 * WID + 5] == 0x332211u, "RGB-format GOP gets red/blue swapped on present");
    CHECK(mint_display_set_overlay_cursor(50, 50, MINT_CURSOR_SHAPE_ARROW), "show the overlay pointer");
    CHECK(fb[52 * WID + 52] != 0x332211u, "pointer pixels are composited into scanout");
    CHECK(back[52 * WID + 52] == 0x112233u, "pointer is never drawn into the canvas");
    CHECK(mint_display_set_overlay_cursor(200, 150, MINT_CURSOR_SHAPE_ARROW) && fb[52 * WID + 52] == 0x332211u,
          "moving the pointer restores the old area from the canvas");
    CHECK(!mint_display_set_overlay_cursor(10, 10, MINT_CURSOR_SHAPE_COUNT), "unknown pointer shape is rejected");

    /* RTC */
    CHECK(mint_rtc_days_from_civil(1970, 1, 1) == 0 && mint_rtc_days_from_civil(2000, 3, 1) == 11017 &&
          mint_rtc_days_from_civil(2026, 11, 9) == 20766, "days_from_civil matches known dates (incl. leap years)");
    CHECK(mint_rtc_bcd_to_binary(0x59) == 59 && mint_rtc_bcd_to_binary(0x09) == 9, "BCD decoding");
    puts("PASS: MintDE 2 renderer, shared canvas and RTC host tests complete");
    return 0;
}
