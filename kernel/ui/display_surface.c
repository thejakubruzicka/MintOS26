#include <stdint.h>
#include <stddef.h>
#include "../include/desktop_abi.h"

/* Reuse the same DejaVu-derived, proportional anti-aliased glyph masks as the
 * polished UEFI desktop. The font data and license notice ship with the source. */
typedef uint8_t u8;
#include "font_aa.h"

static MINT_DISPLAY_SURFACE *current_surface;
static volatile uint32_t *scanout_pixels;
/* Only the native kernel needs the large software backbuffer. Keep the UEFI
 * application compact; its own renderer already has a separate backbuffer.
 * 26.5.009.0: the retained backbuffer and the glass-backdrop snapshot are no
 * longer fixed 1920x1080 .bss arrays. The kernel sizes them from the real GOP
 * mode (stride x height, any stride padding, up to MINT_DISPLAY_MAX_PIXELS)
 * and attaches them after the physical allocator is online. Until then frame
 * calls fail closed and drawing goes straight to scanout, as before. */
#if !defined(_WIN32)
static uint32_t *display_backbuffer;
static uint32_t *display_background;
static uint64_t display_buffer_pixels;
static int display_background_valid;
static uint32_t *display_scratch;
static int display_canvas_mode;
static int32_t overlay_x, overlay_y;
static uint32_t overlay_shape;
static int display_backbuffer_initialized;
static uint32_t damage_x0, damage_y0, damage_x1, damage_y1;
static int display_damage_valid;
static int display_frame_active;
#else
static int display_frame_active;
#endif
#define MINT_CURSOR_W 12u
#define MINT_CURSOR_H 18u
static int cursor_visible;
static int cursor_saved_valid;
static int32_t cursor_x, cursor_y;
static uint32_t cursor_saved[MINT_CURSOR_H][MINT_CURSOR_W];
static uint32_t cursor_saved_width, cursor_saved_height;
static void cursor_hide(void);
static void cursor_paint(void);
static void mark_damage(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
static uint32_t encode_rgb(uint32_t rgb, uint32_t format);

static void zero_bytes(void *ptr, size_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < size; ++i) p[i] = 0;
}

int mint_display_surface_init(MINT_DISPLAY_SURFACE *surface,
                              const MINT_FRAMEBUFFER_INFO *f) {
    if (!surface) return 0;
    if (current_surface == surface) current_surface = 0;
    zero_bytes(surface, sizeof(*surface));
    if (!f || !f->base || !f->width || !f->height || f->width > 16384u ||
        f->height > 16384u || f->pixels_per_scanline < f->width ||
        (f->pixel_format != 0u && f->pixel_format != 1u)) return 0;
    uint64_t stride = f->pixels_per_scanline;
    uint64_t height = f->height;
    if (height && stride > UINT64_MAX / height) return 0;
    uint64_t pixels = stride * height;
    if (pixels > UINT64_MAX / sizeof(uint32_t)) return 0;
    uint64_t required = pixels * sizeof(uint32_t);
    if (f->size < required || f->base > UINTPTR_MAX - required) return 0;
    surface->pixels = (volatile uint32_t *)(uintptr_t)f->base;
    scanout_pixels = surface->pixels;
    display_frame_active = 0;
    surface->byte_size = f->size;
    surface->width = f->width;
    surface->height = f->height;
    surface->stride_pixels = f->pixels_per_scanline;
    surface->pixel_format = f->pixel_format;
    surface->initialized = 1u;
    current_surface = surface;
#if !defined(_WIN32)
    if (pixels > MINT_DISPLAY_MAX_PIXELS) {
        surface->initialized = 0;
        current_surface = 0;
        return 0;
    }
    /* A new surface invalidates any previously attached buffers. */
    display_backbuffer = 0;
    display_background = 0;
    display_scratch = 0;
    display_canvas_mode = 0;
    overlay_shape = 0;
    display_buffer_pixels = 0;
    display_backbuffer_initialized = 0;
    display_background_valid = 0;
    display_damage_valid = 0;
#endif
    return 1;
}

uint64_t mint_display_backbuffer_pixels_required(void) {
    if (!current_surface || !current_surface->initialized) return 0;
    return (uint64_t)current_surface->stride_pixels * current_surface->height;
}

int mint_display_attach_backbuffers(uint32_t *backbuffer, uint32_t *background,
                                    uint64_t capacity_pixels) {
#if defined(_WIN32)
    (void)backbuffer; (void)background; (void)capacity_pixels;
    return 0;
#else
    uint64_t required = mint_display_backbuffer_pixels_required();
    if (!required || !backbuffer || !background || backbuffer == background ||
        capacity_pixels < required || display_frame_active) return 0;
    uint64_t bytes = capacity_pixels * sizeof(uint32_t);
    uintptr_t a = (uintptr_t)backbuffer, b = (uintptr_t)background;
    if ((a < b && a + bytes > b) || (b < a && b + bytes > a)) return 0;
    display_backbuffer = backbuffer;
    display_background = background;
    display_buffer_pixels = capacity_pixels;
    /* Seed the retained frame from what is currently on screen. This is the
     * one deliberate bulk read of scanout memory. */
    for (uint32_t y = 0; y < current_surface->height; ++y) {
        uint64_t row = (uint64_t)y * current_surface->stride_pixels;
        for (uint32_t x = 0; x < current_surface->stride_pixels; ++x)
            display_backbuffer[row + x] = scanout_pixels[row + x];
    }
    display_backbuffer_initialized = 1;
    display_background_valid = 0;
    display_damage_valid = 0;
    return 1;
#endif
}

/* Scanout may be mapped write-combining. Drain the WC buffers so presented
 * pixels reach the framebuffer and later uncached reads (cursor save) see them. */
static void display_flush_scanout(void) {
#if defined(__x86_64__) && !defined(_WIN32)
    __asm__ volatile("sfence" ::: "memory");
#endif
}

int mint_display_frame_begin(void) {
#if defined(_WIN32)
    return 0;
#else
    if (!current_surface || !current_surface->initialized || !scanout_pixels || display_frame_active)
        return 0;
    uint64_t count = (uint64_t)current_surface->stride_pixels * current_surface->height;
    /* Once MintDE owns the shared canvas, the legacy per-primitive frame API
       fails closed so the two models never write the same buffer. */
    if (display_canvas_mode || !display_backbuffer || !display_backbuffer_initialized ||
        count > display_buffer_pixels) return 0;
    /* Restore the scanout beneath the hardware cursor before beginning a frame.
       The retained backbuffer never includes the cursor overlay. */
    cursor_hide();
    current_surface->pixels = display_backbuffer;
    display_damage_valid = 0;
    damage_x0 = damage_y0 = damage_x1 = damage_y1 = 0;
    display_frame_active = 1;
    return 1;
#endif
}

int mint_display_frame_end(void) {
#if defined(_WIN32)
    return 0;
#else
    if (!display_frame_active || !current_surface || !current_surface->initialized || !scanout_pixels)
        return 0;
    if (display_damage_valid) {
        uint32_t x0 = damage_x0, y0 = damage_y0, x1 = damage_x1, y1 = damage_y1;
        if (x1 > current_surface->width) x1 = current_surface->width;
        if (y1 > current_surface->height) y1 = current_surface->height;
        for (uint32_t y = y0; y < y1; ++y) {
            uint64_t row = (uint64_t)y * current_surface->stride_pixels;
            uint32_t x = x0;
            /* Present in aligned 64-bit pairs where possible: half the bus
             * transactions to UC memory and full lines for WC combining. */
            if ((x & 1u) && x < x1) { scanout_pixels[row + x] = display_backbuffer[row + x]; ++x; }
            if (!(((uintptr_t)(scanout_pixels + row + x) | (uintptr_t)(display_backbuffer + row + x)) & 7u)) {
                volatile uint64_t *dst = (volatile uint64_t *)(scanout_pixels + row + x);
                const uint64_t *src = (const uint64_t *)(display_backbuffer + row + x);
                uint32_t pairs = (x1 - x) / 2u;
                for (uint32_t i = 0; i < pairs; ++i) dst[i] = src[i];
                x += pairs * 2u;
            }
            for (; x < x1; ++x) scanout_pixels[row + x] = display_backbuffer[row + x];
        }
        display_flush_scanout();
    }
    current_surface->pixels = scanout_pixels;
    display_frame_active = 0;
    display_damage_valid = 0;
    cursor_paint();
    return 1;
#endif
}

int mint_display_current_capture_background(void) {
#if defined(_WIN32)
    return 0;
#else
    if (!display_frame_active || !current_surface || !current_surface->initialized ||
        current_surface->pixels != display_backbuffer) return 0;
    uint64_t count = (uint64_t)current_surface->stride_pixels * current_surface->height;
    if (!display_background || count > display_buffer_pixels) return 0;
    for (uint32_t y = 0; y < current_surface->height; ++y) {
        uint64_t row = (uint64_t)y * current_surface->stride_pixels;
        for (uint32_t x = 0; x < current_surface->stride_pixels; ++x)
            display_background[row + x] = display_backbuffer[row + x];
    }
    display_background_valid = 1;
    return 1;
#endif
}

int mint_display_current_restore_rect(int32_t x, int32_t y, int32_t width, int32_t height) {
#if defined(_WIN32)
    (void)x; (void)y; (void)width; (void)height;
    return 0;
#else
    if (!display_frame_active || !display_background_valid || !display_background ||
        !current_surface || !current_surface->initialized || width <= 0 || height <= 0) return 0;
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + width, y1 = (int64_t)y + height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > current_surface->width) x1 = current_surface->width;
    if (y1 > current_surface->height) y1 = current_surface->height;
    if (x1 <= x0 || y1 <= y0) return 1;
    for (int64_t yy = y0; yy < y1; ++yy) {
        uint64_t row = (uint64_t)yy * current_surface->stride_pixels;
        for (int64_t xx = x0; xx < x1; ++xx)
            display_backbuffer[row + (uint64_t)xx] = display_background[row + (uint64_t)xx];
    }
    mark_damage((uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
    return 1;
#endif
}

static void mark_damage(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
#if !defined(_WIN32)
    if (!display_frame_active || !current_surface || x1 <= x0 || y1 <= y0) return;
    if (x1 > current_surface->width) x1 = current_surface->width;
    if (y1 > current_surface->height) y1 = current_surface->height;
    if (!display_damage_valid) {
        damage_x0 = x0; damage_y0 = y0; damage_x1 = x1; damage_y1 = y1;
        display_damage_valid = 1;
    } else {
        if (x0 < damage_x0) damage_x0 = x0;
        if (y0 < damage_y0) damage_y0 = y0;
        if (x1 > damage_x1) damage_x1 = x1;
        if (y1 > damage_y1) damage_y1 = y1;
    }
#else
    (void)x0; (void)y0; (void)x1; (void)y1;
#endif
}

int mint_display_current_info(MINT_DISPLAY_INFO *out) {
    if (!out || !current_surface || !current_surface->initialized ||
        !current_surface->width || !current_surface->height ||
        !current_surface->stride_pixels) return 0;
    out->width = current_surface->width;
    out->height = current_surface->height;
    out->stride_pixels = current_surface->stride_pixels;
    out->pixel_format = current_surface->pixel_format;
    out->byte_size = current_surface->byte_size;
    return 1;
}

int mint_display_current_fill_rect(int32_t x, int32_t y, int32_t width,
                                   int32_t height, uint32_t rgb) {
    if (!current_surface || !current_surface->initialized || !current_surface->pixels)
        return 0;
    cursor_hide();
    mint_display_fill_rect(current_surface, x, y, width, height, rgb);
    cursor_paint();
    display_flush_scanout();
    return 1;
}

/* The native kernel embeds a 1920x1080 RGB wallpaper resource. UEFI and host
 * test builds omit the blob, so they can retain their own wallpaper paths. */
#if !defined(_WIN32)
extern const uint8_t mint_native_wallpaper_rgb[] __attribute__((weak));
extern const uint8_t mint_native_wallpaper_rgb_end[] __attribute__((weak));
#endif

static uint32_t decode_rgb(uint32_t pixel, uint32_t format) {
    if (format == 0u)
        return ((pixel & 0x000000FFu) << 16) | (pixel & 0x0000FF00u) |
               ((pixel >> 16) & 0x000000FFu);
    return pixel & 0x00FFFFFFu;
}

int mint_display_current_draw_rgba_scaled_row(int32_t x, int32_t y,
                                              uint32_t target_width,
                                              uint32_t source_width,
                                              const uint8_t *rgba) {
    if (!current_surface || !current_surface->initialized || !current_surface->pixels ||
        !rgba || !target_width || target_width > 128u || !source_width || source_width > 96u ||
        y < 0 || (uint32_t)y >= current_surface->height) return 0;
    int64_t x0 = x, x1 = (int64_t)x + target_width;
    if (x0 < 0) x0 = 0;
    if (x1 > current_surface->width) x1 = current_surface->width;
    if (x1 <= x0) return 1;
    cursor_hide();
    uint64_t pixel_capacity = current_surface->byte_size / sizeof(uint32_t);
    for (int64_t dx = x0; dx < x1; ++dx) {
        uint32_t target_x = (uint32_t)(dx - (int64_t)x);
        uint32_t source_x = (uint32_t)(((uint64_t)target_x * source_width) / target_width);
        if (source_x >= source_width) source_x = source_width - 1u;
        const uint8_t *src = rgba + (size_t)source_x * 4u;
        uint32_t alpha = src[3];
        if (!alpha) continue;
        uint64_t index = (uint64_t)(uint32_t)y * current_surface->stride_pixels + (uint64_t)dx;
        if (index >= pixel_capacity) continue;
        if (alpha == 255u) {
            current_surface->pixels[index] = encode_rgb(((uint32_t)src[0] << 16) |
                ((uint32_t)src[1] << 8) | src[2], current_surface->pixel_format);
        } else {
            uint32_t dst = decode_rgb(current_surface->pixels[index], current_surface->pixel_format);
            uint32_t inv = 255u - alpha;
            uint32_t rr = ((uint32_t)src[0] * alpha + ((dst >> 16) & 255u) * inv + 127u) / 255u;
            uint32_t gg = ((uint32_t)src[1] * alpha + ((dst >> 8) & 255u) * inv + 127u) / 255u;
            uint32_t bb = ((uint32_t)src[2] * alpha + (dst & 255u) * inv + 127u) / 255u;
            current_surface->pixels[index] = encode_rgb((rr << 16) | (gg << 8) | bb,
                                                        current_surface->pixel_format);
        }
    }
    if (display_frame_active)
        mark_damage((uint32_t)x0, (uint32_t)y, (uint32_t)x1, (uint32_t)y + 1u);
    cursor_paint();
    return 1;
}

int mint_display_current_blend_round_rect(int32_t x, int32_t y, int32_t width,
                                          int32_t height, uint32_t rgb,
                                          uint32_t alpha, uint32_t radius) {
    if (!current_surface || !current_surface->initialized || !current_surface->pixels ||
        width <= 0 || height <= 0 || rgb > 0xFFFFFFu || alpha > 255u || radius > 31u)
        return 0;
    int64_t x0=x, y0=y, x1=(int64_t)x+width, y1=(int64_t)y+height;
    if (x0<0) x0=0; if (y0<0) y0=0;
    if (x1>current_surface->width) x1=current_surface->width;
    if (y1>current_surface->height) y1=current_surface->height;
    if (x1<=x0 || y1<=y0) return 1;
    uint32_t r=radius;
    if (r>(uint32_t)(width/2)) r=(uint32_t)(width/2);
    if (r>(uint32_t)(height/2)) r=(uint32_t)(height/2);
    uint32_t inv=255u-alpha;
    for (int64_t yy=y0; yy<y1; ++yy) {
        for (int64_t xx=x0; xx<x1; ++xx) {
            if (r) {
                int64_t dx=0,dy=0;
                if (xx < (int64_t)x+r) dx=(int64_t)x+r-1-xx;
                else if (xx >= (int64_t)x+width-r) dx=xx-((int64_t)x+width-r);
                if (yy < (int64_t)y+r) dy=(int64_t)y+r-1-yy;
                else if (yy >= (int64_t)y+height-r) dy=yy-((int64_t)y+height-r);
                if (dx && dy && (uint64_t)(dx*dx+dy*dy)>(uint64_t)r*r) continue;
            }
            uint64_t index=(uint64_t)yy*current_surface->stride_pixels+(uint64_t)xx;
            if (index>=current_surface->byte_size/sizeof(uint32_t)) continue;
            uint32_t dst=decode_rgb(current_surface->pixels[index],current_surface->pixel_format);
#if !defined(_WIN32)
            if (display_frame_active && display_background_valid && alpha < 250u) {
                /* Cross-shaped five-tap backdrop blur. Captured before any window is drawn,
                   it avoids sampling the cursor or feeding the glass result back into itself. */
                int32_t sx=(int32_t)xx, sy=(int32_t)yy;
                int32_t bx[5]={sx,sx-3,sx+3,sx,sx};
                int32_t by[5]={sy,sy,sy,sy-3,sy+3};
                uint32_t sr=0,sg=0,sb=0,samples=0;
                for(uint32_t tap=0;tap<5u;tap++){
                    if(bx[tap]<0||by[tap]<0||(uint32_t)bx[tap]>=current_surface->width||
                       (uint32_t)by[tap]>=current_surface->height)continue;
                    uint64_t bi=(uint64_t)(uint32_t)by[tap]*current_surface->stride_pixels+(uint32_t)bx[tap];
                    if(bi>=display_buffer_pixels)continue;
                    uint32_t bp=decode_rgb(display_background[bi],current_surface->pixel_format);
                    sr+=(bp>>16)&255u;sg+=(bp>>8)&255u;sb+=bp&255u;samples++;
                }
                if(samples){
                    uint32_t br=sr/samples,bg=sg/samples,bb0=sb/samples;
                    uint32_t dr=(dst>>16)&255u,dg=(dst>>8)&255u,db=dst&255u;
                    dst=(((dr*4u+br)/5u)<<16)|(((dg*4u+bg)/5u)<<8)|((db*4u+bb0)/5u);
                }
            }
#endif
            uint32_t rr=(((rgb>>16)&255u)*alpha+((dst>>16)&255u)*inv+127u)/255u;
            uint32_t gg=(((rgb>>8)&255u)*alpha+((dst>>8)&255u)*inv+127u)/255u;
            uint32_t bb=((rgb&255u)*alpha+(dst&255u)*inv+127u)/255u;
            current_surface->pixels[index]=encode_rgb((rr<<16)|(gg<<8)|bb,current_surface->pixel_format);
        }
    }
    if (display_frame_active) mark_damage((uint32_t)x0,(uint32_t)y0,(uint32_t)x1,(uint32_t)y1);
    return 1;
}

int mint_display_current_draw_wallpaper(uint32_t variant) {
#if defined(_WIN32)
    (void)variant;
    return 0;
#else
    if (variant > 2u || !current_surface || !current_surface->initialized ||
        !current_surface->pixels || !display_frame_active ||
        !mint_native_wallpaper_rgb || !mint_native_wallpaper_rgb_end) return 0;
    const uint64_t required = 1920ull * 1080ull * 3ull;
    if ((uint64_t)(mint_native_wallpaper_rgb_end - mint_native_wallpaper_rgb) < required)
        return 0;
    for (uint32_t y = 0; y < current_surface->height; ++y) {
        uint32_t sy = (uint32_t)((uint64_t)y * 1080u / current_surface->height);
        uint64_t src_row = (uint64_t)sy * 1920u * 3u;
        uint64_t dst_row = (uint64_t)y * current_surface->stride_pixels;
        for (uint32_t x = 0; x < current_surface->width; ++x) {
            uint32_t sx = (uint32_t)((uint64_t)x * 1920u / current_surface->width);
            const uint8_t *src = mint_native_wallpaper_rgb + src_row + (uint64_t)sx * 3u;
            uint32_t r = src[0], g = src[1], b = src[2];
            /* Alternate presets keep the source image but apply restrained,
               deterministic color grades until a richer wallpaper service lands. */
            if (variant == 1u) { r = r * 82u / 100u; g = g * 94u / 100u; }
            else if (variant == 2u) { g = g * 88u / 100u; b = b * 78u / 100u; }
            uint32_t color = encode_rgb((r << 16) | (g << 8) | b, current_surface->pixel_format);
            uint64_t index = dst_row + x;
            if (index < current_surface->byte_size / sizeof(uint32_t))
                current_surface->pixels[index] = color;
        }
    }
    mark_damage(0u, 0u, current_surface->width, current_surface->height);
    return 1;
#endif
}

/* Proportional anti-aliased 16x14 glyph rasterizer shared visually with the
 * firmware desktop. Coverage is packed as two 4-bit samples per byte. */
static uint8_t native_font_coverage(unsigned glyph, unsigned sample) {
    uint8_t packed = font_aa[glyph * 112u + sample / 2u];
    uint8_t value = (sample & 1u) ? (packed & 15u) : (packed >> 4);
    return (uint8_t)(value * 17u);
}

static uint8_t native_font_cov_xy(unsigned glyph, int x, int y) {
    if (x < 0 || x > 15 || y < 0 || y > 13) return 0;
    return native_font_coverage(glyph, (unsigned)y * 16u + (unsigned)x);
}

static uint8_t native_font_cov_bilinear(unsigned glyph, int xfp, int yfp,
                                        int left, int glyph_width) {
    if (glyph_width <= 0) return 0;
    if (xfp < 0) xfp = 0;
    if (yfp < 0) yfp = 0;
    if (xfp > ((glyph_width - 1) << 8)) xfp = (glyph_width - 1) << 8;
    if (yfp > (13 << 8)) yfp = 13 << 8;
    int x0 = xfp >> 8, y0 = yfp >> 8;
    int x1 = x0 < glyph_width - 1 ? x0 + 1 : x0;
    int y1 = y0 < 13 ? y0 + 1 : y0;
    int fx = xfp & 255, fy = yfp & 255;
    int a = native_font_cov_xy(glyph, left + x0, y0);
    int b = native_font_cov_xy(glyph, left + x1, y0);
    int c = native_font_cov_xy(glyph, left + x0, y1);
    int d = native_font_cov_xy(glyph, left + x1, y1);
    int top = (a * (256 - fx) + b * fx) >> 8;
    int bottom = (c * (256 - fx) + d * fx) >> 8;
    return (uint8_t)((top * (256 - fy) + bottom * fy) >> 8);
}

static uint32_t native_blend_coverage(uint32_t dst, uint32_t src, uint8_t alpha) {
    uint32_t inv = 255u - alpha;
    uint32_t r = ((((src >> 16) & 255u) * alpha) + (((dst >> 16) & 255u) * inv)) / 255u;
    uint32_t g = ((((src >> 8) & 255u) * alpha) + (((dst >> 8) & 255u) * inv)) / 255u;
    uint32_t b = (((src & 255u) * alpha) + ((dst & 255u) * inv)) / 255u;
    return (r << 16) | (g << 8) | b;
}

int mint_display_current_draw_text(const char *text, int32_t x, int32_t y,
                                   uint32_t rgb, uint32_t scale) {
    if (!text || !current_surface || !current_surface->initialized ||
        !current_surface->pixels || scale < 1u || scale > 4u) return 0;
    cursor_hide();
    /* Scale is a typography preset, not a raw integer multiplier. The 16x14
       antialiased masks use gentle steps so Settings can scale text without
       making every control several times larger than its hit target. */
    static const uint16_t scale_q8[5] = {0u, 256u, 320u, 384u, 512u};
    const uint32_t scale_factor = scale_q8[scale];
    const int text_height = (14 * (int)scale_factor + 255) / 256;
    const int64_t start_x = x;
    int64_t cursor_x = x;
    int64_t rightmost = x;
    int touched = 0;
    uint32_t encoded_rgb = encode_rgb(rgb, current_surface->pixel_format);
    const uint64_t pixel_capacity = current_surface->byte_size / sizeof(uint32_t);

    for (size_t i = 0; text[i] && i < 128u; ++i) {
        unsigned ch = (unsigned char)text[i];
        if (ch < 32u || ch > 126u) {
            cursor_x += ((int64_t)6u * scale_factor + 255) / 256;
            continue;
        }
        unsigned glyph = ch - 32u;
        int glyph_width = font_aa_width[glyph];
        int left = font_aa_left[glyph];
        int out_width = (glyph_width * (int)scale_factor + 255) / 256;
        int out_height = text_height;
        if (out_width > 0 && cursor_x < INT64_MAX - out_width) {
            if (cursor_x + out_width > rightmost) rightmost = cursor_x + out_width;
            for (int sy = 0; sy < out_height; ++sy) {
                int64_t py64 = (int64_t)y + sy;
                if (py64 < 0 || py64 >= current_surface->height) continue;
                for (int sx = 0; sx < out_width; ++sx) {
                    int64_t px64 = cursor_x + sx;
                    if (px64 < 0 || px64 >= current_surface->width) continue;
                    uint8_t coverage;
                    if (scale > 1u) {
                        int xf = ((2 * sx + 1) * glyph_width * 128 / out_width) - 128;
                        int yf = ((2 * sy + 1) * 14 * 128 / out_height) - 128;
                        coverage = native_font_cov_bilinear(glyph, xf, yf, left, glyph_width);
                    } else {
                        int src_x = left + sx * glyph_width / (out_width ? out_width : 1);
                        int src_y = sy * 14 / (out_height ? out_height : 1);
                        if (src_x > 15) src_x = 15;
                        coverage = native_font_cov_xy(glyph, src_x, src_y);
                    }
                    if (coverage < 10u) continue;
                    uint64_t index = (uint64_t)py64 * current_surface->stride_pixels + (uint64_t)px64;
                    if (index >= pixel_capacity) continue;
                    current_surface->pixels[index] = native_blend_coverage(
                        current_surface->pixels[index], encoded_rgb, coverage);
                    touched = 1;
                }
            }
        }
        uint32_t advance = font_advance[glyph];
        if (!advance) advance = 4u;
        int64_t scaled_advance = ((int64_t)advance * scale_factor + 255) / 256;
        if (cursor_x > INT64_MAX - scaled_advance) break;
        cursor_x += scaled_advance;
    }
    if (touched && display_frame_active) {
        int64_t x0 = start_x < 0 ? 0 : start_x;
        int64_t y0 = y < 0 ? 0 : y;
        int64_t x1 = rightmost > current_surface->width ? current_surface->width : rightmost;
        int64_t y1 = (int64_t)y + (int64_t)text_height;
        if (y1 > current_surface->height) y1 = current_surface->height;
        if (x1 > x0 && y1 > y0)
            mark_damage((uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
    }
    cursor_paint();
    return 1;
}

static uint32_t encode_rgb(uint32_t rgb, uint32_t format) {
    /* GOP PixelRedGreenBlueReserved8BitPerColor (0) stores RGB bytes in
       little-endian memory, requiring the 32-bit channel-order swap here. */
    if (format == 0u)
        return ((rgb & 0x000000FFu) << 16) | (rgb & 0x0000FF00u) |
               ((rgb >> 16) & 0x000000FFu);
    return rgb;
}

static const uint16_t cursor_rows[MINT_CURSOR_H] = {
    0x800u, 0xC00u, 0xE00u, 0xF00u, 0xF80u, 0xFC0u,
    0xFE0u, 0xFF0u, 0xFF8u, 0xFFCu, 0xFF8u, 0x7F0u,
    0x3E0u, 0x1C0u, 0x0E0u, 0x060u, 0x020u, 0x000u
};

static void cursor_hide(void) {
#if !defined(_WIN32)
    if (display_frame_active) return;
#endif
    if (!cursor_visible || !current_surface || !current_surface->initialized ||
        !current_surface->pixels || !cursor_saved_valid) return;
    for (uint32_t y = 0; y < cursor_saved_height; ++y) {
        int32_t py = cursor_y + (int32_t)y;
        if (py < 0 || (uint32_t)py >= current_surface->height) continue;
        for (uint32_t x = 0; x < cursor_saved_width; ++x) {
            int32_t px = cursor_x + (int32_t)x;
            if (px < 0 || (uint32_t)px >= current_surface->width) continue;
            uint64_t index = (uint64_t)(uint32_t)py * current_surface->stride_pixels + (uint32_t)px;
            if (index < current_surface->byte_size / sizeof(uint32_t))
                current_surface->pixels[index] = cursor_saved[y][x];
        }
    }
    cursor_saved_valid = 0;
    display_flush_scanout();
}

static void cursor_paint(void) {
#if !defined(_WIN32)
    if (display_frame_active) return;
#endif
    if (!cursor_visible || !current_surface || !current_surface->initialized ||
        !current_surface->pixels) return;
    if (current_surface->width < MINT_CURSOR_W || current_surface->height < MINT_CURSOR_H) return;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if ((uint32_t)cursor_x > current_surface->width - MINT_CURSOR_W)
        cursor_x = (int32_t)(current_surface->width - MINT_CURSOR_W);
    if ((uint32_t)cursor_y > current_surface->height - MINT_CURSOR_H)
        cursor_y = (int32_t)(current_surface->height - MINT_CURSOR_H);
    cursor_saved_width = MINT_CURSOR_W; cursor_saved_height = MINT_CURSOR_H;
    for (uint32_t y = 0; y < MINT_CURSOR_H; ++y) {
        uint64_t row = (uint64_t)(uint32_t)(cursor_y + (int32_t)y) * current_surface->stride_pixels;
        for (uint32_t x = 0; x < MINT_CURSOR_W; ++x) {
            uint64_t index = row + (uint32_t)cursor_x + x;
            cursor_saved[y][x] = current_surface->pixels[index];
        }
    }
    cursor_saved_valid = 1;
    const uint32_t black = encode_rgb(0x09131Fu, current_surface->pixel_format);
    const uint32_t white = encode_rgb(0xFFFFFFu, current_surface->pixel_format);
    for (uint32_t y = 0; y < MINT_CURSOR_H; ++y) {
        uint16_t bits = cursor_rows[y];
        if (!bits) continue;
        uint32_t left = 0, right = MINT_CURSOR_W - 1u;
        while (left < MINT_CURSOR_W && !(bits & (1u << (11u - left)))) ++left;
        while (right > left && !(bits & (1u << (11u - right)))) --right;
        int32_t py = cursor_y + (int32_t)y;
        uint64_t row = (uint64_t)(uint32_t)py * current_surface->stride_pixels;
        for (uint32_t x = left; x <= right; ++x) {
            uint64_t index = row + (uint32_t)cursor_x + x;
            if (index < current_surface->byte_size / sizeof(uint32_t)) {
                uint16_t bit = (uint16_t)(1u << (11u - x));
                int top_edge = (y == 0u) || !(cursor_rows[y - 1u] & bit);
                int bottom_edge = (y + 1u >= MINT_CURSOR_H) || !(cursor_rows[y + 1u] & bit);
                current_surface->pixels[index] = (x == left || x == right || top_edge || bottom_edge)
                                                   ? black : white;
            }
        }
    }
    display_flush_scanout();
}

void mint_display_cursor_move(int32_t x, int32_t y) {
    cursor_hide();
    cursor_x = x; cursor_y = y; cursor_visible = 1;
    cursor_paint();
}

void mint_display_put_pixel(MINT_DISPLAY_SURFACE *s, int32_t x, int32_t y, uint32_t rgb) {
    if (!s || !s->initialized || !s->pixels || x < 0 || y < 0 ||
        (uint32_t)x >= s->width || (uint32_t)y >= s->height) return;
    uint64_t index = (uint64_t)(uint32_t)y * s->stride_pixels + (uint32_t)x;
    if (index >= s->byte_size / sizeof(uint32_t)) return;
    s->pixels[index] = encode_rgb(rgb, s->pixel_format);
    if (s == current_surface && display_frame_active)
        mark_damage((uint32_t)x, (uint32_t)y, (uint32_t)x + 1u, (uint32_t)y + 1u);
}

void mint_display_fill_rect(MINT_DISPLAY_SURFACE *s, int32_t x, int32_t y,
                            int32_t width, int32_t height, uint32_t rgb) {
    if (!s || !s->initialized || !s->pixels || width <= 0 || height <= 0) return;
    int64_t x0 = x, y0 = y, x1 = (int64_t)x + width, y1 = (int64_t)y + height;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t encoded = encode_rgb(rgb, s->pixel_format);
    for (int64_t yy = y0; yy < y1; ++yy) {
        uint64_t index = (uint64_t)yy * s->stride_pixels + (uint64_t)x0;
        uint64_t count = (uint64_t)(x1 - x0);
        if (index > s->byte_size / sizeof(uint32_t) ||
            count > s->byte_size / sizeof(uint32_t) - index) return;
        for (uint64_t xx = 0; xx < count; ++xx) s->pixels[index + xx] = encoded;
    }
    if (s == current_surface && display_frame_active)
        mark_damage((uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1);
}

#if !defined(_WIN32)
#include "cursor_sprites.h"
extern const uint8_t mint_native_wallpaper_dark_rgb[] __attribute__((weak));
extern const uint8_t mint_native_wallpaper_dark_rgb_end[] __attribute__((weak));

int mint_display_attach_scratch(uint32_t *scratch, uint64_t capacity_pixels) {
    uint64_t required = mint_display_backbuffer_pixels_required();
    if (!required || !scratch || capacity_pixels < required || !display_backbuffer) return 0;
    display_scratch = scratch;
    return 1;
}

int mint_display_canvas_buffers(uint64_t *canvas_pa, uint64_t *wallpaper_pa,
                                uint64_t *scratch_pa, uint64_t *bytes) {
    if (!current_surface || !current_surface->initialized || !display_backbuffer ||
        !display_background || !display_scratch || !canvas_pa || !wallpaper_pa ||
        !scratch_pa || !bytes) return 0;
    *canvas_pa = (uint64_t)(uintptr_t)display_backbuffer;   /* identity-mapped */
    *wallpaper_pa = (uint64_t)(uintptr_t)display_background;
    *scratch_pa = (uint64_t)(uintptr_t)display_scratch;
    *bytes = display_buffer_pixels * sizeof(uint32_t);
    return 1;
}

void mint_display_enter_canvas_mode(void) {
    if (!current_surface || !current_surface->initialized) return;
    display_canvas_mode = 1;
    display_frame_active = 0;
    current_surface->pixels = scanout_pixels;
    cursor_visible = 0;           /* retire the legacy save/restore cursor */
    cursor_saved_valid = 0;
}

static inline uint32_t canvas_to_scanout(uint32_t rgb) {
    return current_surface->pixel_format == 0u ? encode_rgb(rgb, 0u) : (rgb & 0x00FFFFFFu);
}

/* Copy canvas [x0,x1)x[y0,y1) to scanout, converting channel order if the
   GOP mode is RGB. Rows are copied as aligned 64-bit pairs where possible. */
static void canvas_copy_rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t stride = current_surface->stride_pixels;
    const int swap = current_surface->pixel_format == 0u;
    for (uint32_t y = y0; y < y1; ++y) {
        uint64_t row = (uint64_t)y * stride;
        uint32_t x = x0;
        if (!swap) {
            if ((x & 1u) && x < x1) { scanout_pixels[row + x] = display_backbuffer[row + x] & 0x00FFFFFFu; ++x; }
            if (!(((uintptr_t)(scanout_pixels + row + x) | (uintptr_t)(display_backbuffer + row + x)) & 7u)) {
                volatile uint64_t *dst = (volatile uint64_t *)(scanout_pixels + row + x);
                const uint64_t *src = (const uint64_t *)(display_backbuffer + row + x);
                uint32_t pairs = (x1 - x) / 2u;
                for (uint32_t i = 0; i < pairs; ++i) dst[i] = src[i] & 0x00FFFFFF00FFFFFFull;
                x += pairs * 2u;
            }
        }
        for (; x < x1; ++x) scanout_pixels[row + x] = canvas_to_scanout(display_backbuffer[row + x]);
    }
}

static int clip_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                     uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1) {
    if (w <= 0 || h <= 0) return 0;
    int64_t a = x, b = y, c = (int64_t)x + w, d = (int64_t)y + h;
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (c > current_surface->width) c = current_surface->width;
    if (d > current_surface->height) d = current_surface->height;
    if (c <= a || d <= b) return 0;
    *x0 = (uint32_t)a; *y0 = (uint32_t)b; *x1 = (uint32_t)c; *y1 = (uint32_t)d;
    return 1;
}

/* Composite the overlay pointer over canvas pixels directly into scanout. */
static void overlay_paint(void) {
    if (!overlay_shape || overlay_shape >= MINT_CURSOR_SHAPE_COUNT) return;
    const MINT_CURSOR_SPRITE *sp = &mint_cursor_sprites[overlay_shape];
    int32_t ox = overlay_x - sp->hot_x, oy = overlay_y - sp->hot_y;
    const uint32_t stride = current_surface->stride_pixels;
    for (uint32_t sy = 0; sy < MINT_CURSOR_SPRITE_SIZE; ++sy) {
        int32_t py = oy + (int32_t)sy;
        if (py < 0 || (uint32_t)py >= current_surface->height) continue;
        for (uint32_t sx = 0; sx < MINT_CURSOR_SPRITE_SIZE; ++sx) {
            int32_t px = ox + (int32_t)sx;
            if (px < 0 || (uint32_t)px >= current_surface->width) continue;
            const uint8_t *c = sp->rgba + ((uint64_t)sy * MINT_CURSOR_SPRITE_SIZE + sx) * 4u;
            uint32_t a = c[3];
            if (!a) continue;
            uint64_t i = (uint64_t)(uint32_t)py * stride + (uint32_t)px;
            uint32_t d = display_backbuffer[i], inv = 255u - a;
            uint32_t r = (c[0] * a + ((d >> 16) & 255u) * inv + 127u) / 255u;
            uint32_t g = (c[1] * a + ((d >> 8) & 255u) * inv + 127u) / 255u;
            uint32_t b = (c[2] * a + (d & 255u) * inv + 127u) / 255u;
            scanout_pixels[i] = canvas_to_scanout((r << 16) | (g << 8) | b);
        }
    }
}

static void overlay_bounds(uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1, int *valid) {
    *valid = 0;
    if (!overlay_shape || overlay_shape >= MINT_CURSOR_SHAPE_COUNT) return;
    const MINT_CURSOR_SPRITE *sp = &mint_cursor_sprites[overlay_shape];
    *valid = clip_rect(overlay_x - sp->hot_x, overlay_y - sp->hot_y,
                       (int32_t)MINT_CURSOR_SPRITE_SIZE, (int32_t)MINT_CURSOR_SPRITE_SIZE, x0, y0, x1, y1);
}

int mint_display_present_canvas(int32_t x, int32_t y, int32_t width, int32_t height) {
    if (!display_canvas_mode || !current_surface || !display_backbuffer) return 0;
    uint32_t x0, y0, x1, y1;
    if (!clip_rect(x, y, width, height, &x0, &y0, &x1, &y1)) return 1;
    canvas_copy_rect(x0, y0, x1, y1);
    uint32_t cx0, cy0, cx1, cy1; int cv;
    overlay_bounds(&cx0, &cy0, &cx1, &cy1, &cv);
    if (cv && cx0 < x1 && x0 < cx1 && cy0 < y1 && y0 < cy1) overlay_paint();
    display_flush_scanout();
    return 1;
}

int mint_display_set_overlay_cursor(int32_t x, int32_t y, uint32_t shape) {
    if (!display_canvas_mode || !current_surface || !display_backbuffer ||
        shape >= MINT_CURSOR_SHAPE_COUNT) return 0;
    uint32_t x0, y0, x1, y1; int valid;
    overlay_bounds(&x0, &y0, &x1, &y1, &valid);
    if (valid) canvas_copy_rect(x0, y0, x1, y1);   /* erase from the canvas */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((uint32_t)x >= current_surface->width) x = (int32_t)current_surface->width - 1;
    if ((uint32_t)y >= current_surface->height) y = (int32_t)current_surface->height - 1;
    overlay_x = x; overlay_y = y; overlay_shape = shape;
    overlay_paint();
    display_flush_scanout();
    return 1;
}

/* Render an embedded 1920x1080 RGB wallpaper into the wallpaper cache buffer,
   bilinearly scaled to the screen. Variant 0 = light, 1 = dark. */
int mint_display_render_wallpaper(uint32_t variant) {
    if (!current_surface || !display_background || variant > MINT_WALLPAPER_DARK) return 0;
    const uint8_t *src = variant ? mint_native_wallpaper_dark_rgb : mint_native_wallpaper_rgb;
    const uint8_t *end = variant ? mint_native_wallpaper_dark_rgb_end : mint_native_wallpaper_rgb_end;
    const uint32_t sw = 1920u, sh = 1080u;
    if (!src || !end || (uint64_t)(end - src) < (uint64_t)sw * sh * 3u) return 0;
    const uint32_t w = current_surface->width, h = current_surface->height;
    const uint32_t stride = current_surface->stride_pixels;
    for (uint32_t y = 0; y < h; ++y) {
        /* 16.16 fixed-point source coordinates (pixel-centre aligned). */
        int64_t fy = ((int64_t)(2u * y + 1u) * sh * 65536) / (2 * (int64_t)h) - 32768;
        if (fy < 0) fy = 0;
        uint32_t y0 = (uint32_t)(fy >> 16), wy = (uint32_t)((fy >> 8) & 255);
        uint32_t y1 = y0 + 1u < sh ? y0 + 1u : y0;
        const uint8_t *r0 = src + (uint64_t)y0 * sw * 3u, *r1 = src + (uint64_t)y1 * sw * 3u;
        uint32_t *dst = display_background + (uint64_t)y * stride;
        for (uint32_t x = 0; x < w; ++x) {
            int64_t fx = ((int64_t)(2u * x + 1u) * sw * 65536) / (2 * (int64_t)w) - 32768;
            if (fx < 0) fx = 0;
            uint32_t x0 = (uint32_t)(fx >> 16), wx = (uint32_t)((fx >> 8) & 255);
            uint32_t x1 = x0 + 1u < sw ? x0 + 1u : x0;
            uint32_t out = 0;
            for (uint32_t ch = 0; ch < 3u; ++ch) {
                uint32_t a = r0[x0 * 3u + ch], b = r0[x1 * 3u + ch];
                uint32_t c = r1[x0 * 3u + ch], d = r1[x1 * 3u + ch];
                uint32_t top = a * (256u - wx) + b * wx, bot = c * (256u - wx) + d * wx;
                out = (out << 8) | ((top * (256u - wy) + bot * wy) >> 16);
            }
            dst[x] = out;
        }
    }
    return 1;
}
#endif
