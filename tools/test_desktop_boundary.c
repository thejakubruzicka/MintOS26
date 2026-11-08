#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "desktop_abi.h"

#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } puts("PASS: " msg); } while (0)

int main(void) {
    MINT_DESKTOP_EVENT_QUEUE q;
    MINT_DESKTOP_EVENT e, out;
    mint_desktop_event_queue_init(&q);
    int ok = 1;
    for (uint32_t i = 0; i < MINT_DESKTOP_EVENT_CAPACITY; ++i) {
        memset(&e, 0, sizeof(e)); e.type = MINT_DESKTOP_EVENT_KEY;
        e.timestamp_ms = i; e.data.key.codepoint = 0x41u + i;
        if (!mint_desktop_event_push(&q, &e)) { ok = 0; break; }
    }
    CHECK(ok, "queue accepts exactly its configured capacity");
    CHECK(!mint_desktop_event_push(&q, &e) && mint_desktop_event_dropped(&q) == 1,
          "queue overflow is explicit and counted");
    ok = 1;
    for (uint32_t i = 0; i < MINT_DESKTOP_EVENT_CAPACITY / 2; ++i) {
        if (!mint_desktop_event_pop(&q, &out) || out.timestamp_ms != i ||
            out.data.key.codepoint != 0x41u + i) { ok = 0; break; }
    }
    CHECK(ok, "events preserve FIFO order");
    ok = 1;
    for (uint32_t i = 0; i < MINT_DESKTOP_EVENT_CAPACITY / 2; ++i) {
        memset(&e, 0, sizeof(e)); e.type = MINT_DESKTOP_EVENT_POINTER_MOVE;
        e.data.pointer.x = (int32_t)i; e.data.pointer.y = -(int32_t)i;
        if (!mint_desktop_event_push(&q, &e)) { ok = 0; break; }
    }
    CHECK(ok && mint_desktop_event_count(&q) == MINT_DESKTOP_EVENT_CAPACITY,
          "queue wrap-around preserves bounded count");
    ok = 1;
    for (uint32_t i = MINT_DESKTOP_EVENT_CAPACITY / 2; i < MINT_DESKTOP_EVENT_CAPACITY; ++i) {
        if (!mint_desktop_event_pop(&q, &out) || out.type != MINT_DESKTOP_EVENT_KEY ||
            out.data.key.codepoint != 0x41u + i) { ok = 0; break; }
    }
    for (uint32_t i = 0; ok && i < MINT_DESKTOP_EVENT_CAPACITY / 2; ++i) {
        if (!mint_desktop_event_pop(&q, &out) || out.type != MINT_DESKTOP_EVENT_POINTER_MOVE ||
            out.data.pointer.x != (int32_t)i || out.data.pointer.y != -(int32_t)i) { ok = 0; break; }
    }
    CHECK(ok && mint_desktop_event_count(&q) == 0, "wrapped events drain in correct FIFO order");
    CHECK(mint_desktop_event_selftest(), "production event-queue self-test passes");

    uint32_t pixels[6u * 4u];
    MINT_FRAMEBUFFER_INFO fb = { (uint64_t)(uintptr_t)pixels, sizeof(pixels), 4u, 4u, 6u, 1u };
    MINT_DISPLAY_SURFACE surface;
    memset(pixels, 0, sizeof(pixels));
    CHECK(mint_display_surface_init(&surface, &fb), "surface accepts valid GOP descriptor with padded stride");
    CHECK(mint_display_frame_begin(), "software frame begins using bounded backbuffer");
    CHECK(mint_display_current_fill_rect(0, 0, 1, 1, 0x00ABCDEFu), "frame draw targets the backbuffer");
    CHECK(pixels[0] == 0, "scanout is unchanged before frame commit");
    CHECK(mint_display_frame_end() && pixels[0] == 0x00ABCDEFu, "frame commit publishes completed pixels to scanout");
    memset(pixels, 0, sizeof(pixels));
    mint_display_put_pixel(&surface, 2, 1, 0x00112233u);
    CHECK(pixels[1u * 6u + 2u] == 0x00112233u, "surface uses stride rather than visible width");
    mint_display_put_pixel(&surface, -1, 0, 0x00FFFFFFu);
    mint_display_put_pixel(&surface, 4, 0, 0x00FFFFFFu);
    CHECK(pixels[0] == 0 && pixels[4] == 0, "surface clips out-of-bounds pixels");
    mint_display_fill_rect(&surface, -2, -1, 4, 3, 0x00ABCDEFu);
    CHECK(pixels[0] == 0x00ABCDEFu && pixels[1] == 0x00ABCDEFu && pixels[6] == 0x00ABCDEFu,
          "rectangle fill clips safely at display edges");
    CHECK(pixels[2] == 0 && pixels[4] == 0, "rectangle fill respects the visible width");

    fb.pixel_format = 0u;
    CHECK(mint_display_surface_init(&surface, &fb), "surface accepts GOP RGB-byte pixel format");
    mint_display_put_pixel(&surface, 0, 0, 0x00112233u);
    CHECK(pixels[0] == 0x00332211u, "pixel-channel order is handled explicitly");
    /* The native renderer must produce blended edge pixels and preserve case,
       rather than the chunky all-caps 5x7 bitmap used by the first shell. */
    uint32_t text_pixels[128u * 24u];
    for (size_t i = 0; i < sizeof(text_pixels)/sizeof(text_pixels[0]); ++i)
        text_pixels[i] = 0x00304050u;
    MINT_FRAMEBUFFER_INFO text_fb = { (uint64_t)(uintptr_t)text_pixels,
        sizeof(text_pixels), 128u, 24u, 128u, 1u };
    MINT_DISPLAY_SURFACE text_surface;
    CHECK(mint_display_surface_init(&text_surface, &text_fb), "anti-aliased font accepts a native text surface");
    CHECK(mint_display_current_blend_round_rect(40, 5, 16, 12, 0x00C06020u, 128u, 5u),
          "native display service draws an alpha-blended rounded glass panel");
    CHECK(text_pixels[10u * 128u + 48u] == 0x00785038u,
          "glass panel performs correct per-channel alpha blending on the backbuffer");
    CHECK(text_pixels[5u * 128u + 40u] == 0x00304050u,
          "rounded glass panel clips its transparent corner instead of painting a square corner");
    CHECK(mint_display_current_draw_text("Mint OS", 2, 3, 0x00FFFFFFu, 1u),
          "native text service renders mixed-case proportional glyphs");
    size_t changed = 0, antialiased = 0;
    for (size_t i = 0; i < sizeof(text_pixels)/sizeof(text_pixels[0]); ++i) {
        if (text_pixels[i] != 0x00304050u) ++changed;
        if (text_pixels[i] != 0x00304050u && text_pixels[i] != 0x00FFFFFFu) ++antialiased;
    }
    CHECK(changed > 0, "native text service changes pixels for visible glyphs");
    CHECK(antialiased > 0, "native text service blends anti-aliased glyph edges");

    uint32_t scaled_pixels[128u * 120u];
    for (size_t i = 0; i < sizeof(scaled_pixels)/sizeof(scaled_pixels[0]); ++i)
        scaled_pixels[i] = 0x00304050u;
    MINT_FRAMEBUFFER_INFO scaled_fb = { (uint64_t)(uintptr_t)scaled_pixels,
        sizeof(scaled_pixels), 128u, 120u, 128u, 1u };
    MINT_DISPLAY_SURFACE scaled_surface;
    CHECK(mint_display_surface_init(&scaled_surface, &scaled_fb),
          "initialize surface for bounded typography presets");
    const int scale_y[4] = {2, 24, 47, 73};
    int ink_height[4] = {0,0,0,0};
    int scale_ok = 1;
    for (uint32_t preset = 1; preset <= 4; ++preset) {
        if (!mint_display_current_draw_text("Mint", 2, scale_y[preset-1],
                                            0x00FFFFFFu, preset)) {
            scale_ok = 0; break;
        }
        int bottom = scale_y[preset-1];
        for (int yy = scale_y[preset-1]; yy < scale_y[preset-1] + 34; ++yy) {
            for (int xx = 2; xx < 110; ++xx) {
                if (scaled_pixels[(size_t)yy * 128u + (size_t)xx] != 0x00304050u && yy > bottom)
                    bottom = yy;
            }
        }
        ink_height[preset-1] = bottom - scale_y[preset-1] + 1;
    }
    CHECK(scale_ok && ink_height[0] > 0 && ink_height[0] < ink_height[1] &&
          ink_height[1] < ink_height[2] && ink_height[2] < ink_height[3],
          "typography presets increase smoothly without integer-multiplier jumps");

    fb.size = 12u;
    CHECK(!mint_display_surface_init(&surface, &fb), "surface rejects undersized framebuffer mapping");
    fb.size = sizeof(pixels); fb.pixel_format = 2u;
    CHECK(!mint_display_surface_init(&surface, &fb), "surface rejects unsupported GOP pixel format");
    CHECK(!mint_display_surface_init(&surface, 0), "surface rejects missing framebuffer descriptor");
    puts("PASS: shared desktop ABI queue and framebuffer boundary tests complete");
    return 0;
}
