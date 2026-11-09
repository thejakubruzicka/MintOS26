#ifndef MINTOS26_DESKTOP_ABI_H
#define MINTOS26_DESKTOP_ABI_H

#include <stdint.h>
#include <stddef.h>
#include "boot_abi.h"

/* Kernel/desktop boundary. This header deliberately has no UEFI types so the
 * future native compositor, firmware fallback, and host tests can share it. */
#define MINT_DESKTOP_ABI_VERSION 1u
#define MINT_DESKTOP_EVENT_CAPACITY 64u
#define MINT_DESKTOP_EVENT_MASK (MINT_DESKTOP_EVENT_CAPACITY - 1u)

typedef enum mint_desktop_event_type {
    MINT_DESKTOP_EVENT_NONE = 0,
    MINT_DESKTOP_EVENT_KEY = 1,
    MINT_DESKTOP_EVENT_POINTER_MOVE = 2,
    MINT_DESKTOP_EVENT_POINTER_BUTTON = 3,
    MINT_DESKTOP_EVENT_RESIZE = 4,
    MINT_DESKTOP_EVENT_DAMAGE = 5,
    MINT_DESKTOP_EVENT_TIMER = 6,
    MINT_DESKTOP_EVENT_APP_REQUEST = 7,
    MINT_DESKTOP_EVENT_QUIT = 8
} MINT_DESKTOP_EVENT_TYPE;

/* Key-event modifier bits. GUI is the Windows/Super key, used as Command. */
#define MINT_KEYMOD_SHIFT 0x01u
#define MINT_KEYMOD_CTRL  0x02u
#define MINT_KEYMOD_ALT   0x04u
#define MINT_KEYMOD_CAPS  0x08u
#define MINT_KEYMOD_GUI   0x10u
typedef struct mint_desktop_key_event {
    uint32_t codepoint;
    uint16_t scancode;
    uint8_t modifiers;
    uint8_t pressed;
} MINT_DESKTOP_KEY_EVENT;

typedef struct mint_desktop_pointer_event {
    int32_t x, y;
    int32_t dx, dy;
    uint32_t buttons;
    uint32_t changed_buttons;
} MINT_DESKTOP_POINTER_EVENT;

typedef struct mint_desktop_resize_event {
    uint32_t width, height;
} MINT_DESKTOP_RESIZE_EVENT;

typedef struct mint_desktop_event {
    uint32_t type;
    uint32_t flags;
    uint64_t timestamp_ms;
    union {
        MINT_DESKTOP_KEY_EVENT key;
        MINT_DESKTOP_POINTER_EVENT pointer;
        MINT_DESKTOP_RESIZE_EVENT resize;
        struct { uint32_t app_id; uint32_t argument; } app;
        struct { int32_t x, y, width, height; } damage;
        uint64_t opaque[2];
    } data;
} MINT_DESKTOP_EVENT;

/* A bounded single-producer/single-consumer queue. Device IRQ handlers should
 * feed a single input-dispatch producer; desktop tasks consume from the other
 * side. If it is full, push drops the new event and increments dropped_events.
 * Head/tail are monotonically increasing unsigned counters so wrap is defined. */
typedef struct mint_desktop_event_queue {
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint64_t dropped_events;
    MINT_DESKTOP_EVENT events[MINT_DESKTOP_EVENT_CAPACITY];
} MINT_DESKTOP_EVENT_QUEUE;

void mint_desktop_event_queue_init(MINT_DESKTOP_EVENT_QUEUE *queue);
int mint_desktop_event_push(MINT_DESKTOP_EVENT_QUEUE *queue, const MINT_DESKTOP_EVENT *event);
int mint_desktop_event_pop(MINT_DESKTOP_EVENT_QUEUE *queue, MINT_DESKTOP_EVENT *event_out);
uint32_t mint_desktop_event_count(const MINT_DESKTOP_EVENT_QUEUE *queue);
uint64_t mint_desktop_event_dropped(const MINT_DESKTOP_EVENT_QUEUE *queue);
int mint_desktop_event_selftest(void);
int mint_desktop_core_init(void);
int mint_desktop_event_post(const MINT_DESKTOP_EVENT *event);
int mint_desktop_event_next(MINT_DESKTOP_EVENT *event_out);
uint64_t mint_desktop_event_overflow_count(void);

/* Stable 32-bit GOP framebuffer surface, without EFI-specific declarations. */
typedef struct mint_display_surface {
    volatile uint32_t *pixels;
    uint64_t byte_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
    uint32_t pixel_format;
    uint32_t initialized;
    uint32_t reserved;
} MINT_DISPLAY_SURFACE;

/* Safe userspace query result. It intentionally exposes no kernel framebuffer
 * pointer or writable framebuffer mapping. */
typedef struct mint_display_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
    uint32_t pixel_format;
    uint64_t byte_size;
} MINT_DISPLAY_INFO;

/* Hardware input readiness and bounded event counters reported through a
 * pointer-safe syscall. Counters make real-device input testable from Settings. */
typedef struct mint_input_info {
    uint32_t keyboard_ready;
    uint32_t mouse_ready;
    uint64_t keyboard_events;
    uint64_t pointer_motion_events;
    uint64_t pointer_button_events;
    uint64_t dropped_events;
} MINT_INPUT_INFO;

/* Versioned session settings contract. The current implementation stores
 * settings in kernel-owned RAM; the future block/VFS backend will make them
 * survive reboot without changing this ABI. */
#define MINT_DESKTOP_SETTINGS_VERSION 1u
enum { MINT_THEME_LIGHT = 0u, MINT_THEME_DARK = 1u, MINT_THEME_WARM = 2u };
enum { MINT_ACCENT_MINT = 0u, MINT_ACCENT_BLUE = 1u, MINT_ACCENT_VIOLET = 2u,
       MINT_ACCENT_ORANGE = 3u, MINT_ACCENT_ROSE = 4u };
#define MINT_DESKTOP_SETTING_REDUCE_MOTION 0x01u
typedef struct mint_desktop_settings {
    uint32_t version;
    uint32_t theme;
    uint32_t accent;
    uint32_t ui_scale; /* 1..3 */
    uint32_t wallpaper;
    uint32_t flags;
    uint32_t generation; /* incremented by kernel on each accepted update */
    uint32_t reserved;
} MINT_DESKTOP_SETTINGS;

#define MINT_FS_NAME_MAX 24u
typedef struct mint_fs_dirent {
    char name[MINT_FS_NAME_MAX];
    uint32_t size;
    uint32_t flags;
} MINT_FS_DIRENT;

int mint_display_surface_init(MINT_DISPLAY_SURFACE *surface,
                              const MINT_FRAMEBUFFER_INFO *framebuffer);
/* 26.5.010.0 shared-canvas display model. MintDE renders into a kernel-owned
 * retained canvas mapped into its address space, then asks the kernel to
 * present damaged rectangles. Canvas pixels are always 0x00RRGGBB; the kernel
 * converts to the GOP pixel format while presenting. The pointer is a
 * kernel overlay composited at present/move time, so moving it never
 * requires MintDE to redraw. */
#define MINT_DISPLAY_CANVAS_VERSION 1u
#define MINT_DISPLAY_CANVAS_USER_VA 0x0000004000000000ull /* 256 GiB */
enum { MINT_CURSOR_SHAPE_HIDDEN = 0u, MINT_CURSOR_SHAPE_ARROW = 1u, MINT_CURSOR_SHAPE_IBEAM = 2u,
       MINT_CURSOR_SHAPE_RESIZE_EW = 3u, MINT_CURSOR_SHAPE_RESIZE_NS = 4u,
       MINT_CURSOR_SHAPE_RESIZE_NWSE = 5u, MINT_CURSOR_SHAPE_RESIZE_NESW = 6u,
       MINT_CURSOR_SHAPE_COUNT = 7u };
enum { MINT_WALLPAPER_LIGHT = 0u, MINT_WALLPAPER_DARK = 1u };
typedef struct mint_display_canvas {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;      /* row pitch of every mapped buffer */
    uint64_t canvas;             /* presented retained frame (user VA) */
    uint64_t wallpaper;          /* desktop background cache (user VA) */
    uint64_t scratch;            /* free scratch buffer, same size (user VA) */
    uint64_t buffer_bytes;       /* bytes in each buffer */
    uint32_t scanout_format;     /* informational: GOP pixel format */
    uint32_t flags;
} MINT_DISPLAY_CANVAS;

/* Kernel-side canvas services (native kernel only). */
int mint_display_attach_scratch(uint32_t *scratch, uint64_t capacity_pixels);
int mint_display_canvas_buffers(uint64_t *canvas_pa, uint64_t *wallpaper_pa,
                                uint64_t *scratch_pa, uint64_t *bytes);
void mint_display_enter_canvas_mode(void);
int mint_display_present_canvas(int32_t x, int32_t y, int32_t width, int32_t height);
int mint_display_set_overlay_cursor(int32_t x, int32_t y, uint32_t shape);
int mint_display_render_wallpaper(uint32_t variant);

/* Largest scanout (stride x height) the native kernel will drive: 4096x2400
 * covers 3840x2160 and 2560x1600 GOP modes including stride padding. */
#define MINT_DISPLAY_MAX_PIXELS (4096ull * 2400ull)
/* Pixels each attached buffer must hold for the current surface (0 if none). */
uint64_t mint_display_backbuffer_pixels_required(void);
/* Attach kernel-owned retained-frame and glass-backdrop buffers. Each must
 * hold capacity_pixels >= stride*height uint32_t values and must not overlap. */
int mint_display_attach_backbuffers(uint32_t *backbuffer, uint32_t *background,
                                    uint64_t capacity_pixels);
void mint_display_put_pixel(MINT_DISPLAY_SURFACE *surface, int32_t x, int32_t y, uint32_t rgb);
void mint_display_fill_rect(MINT_DISPLAY_SURFACE *surface, int32_t x, int32_t y,
                            int32_t width, int32_t height, uint32_t rgb);
int mint_display_current_info(MINT_DISPLAY_INFO *info_out);
/* Native kernel-owned photo wallpaper service; variants apply bounded color grades. */
int mint_display_current_draw_wallpaper(uint32_t variant);
/* Kernel-mediated draw services for userspace. They never expose framebuffer
 * memory or a physical framebuffer address to ring 3. Coordinates are clipped
 * by the registered current surface. */
int mint_display_current_fill_rect(int32_t x, int32_t y, int32_t width,
                                   int32_t height, uint32_t rgb);
int mint_display_current_blend_round_rect(int32_t x, int32_t y, int32_t width,
                                          int32_t height, uint32_t rgb,
                                          uint32_t alpha, uint32_t radius);
/* A row of straight-alpha RGBA pixels, horizontally scaled with nearest-neighbor.
 * The caller copies and validates user memory before crossing into this helper. */
int mint_display_current_draw_rgba_scaled_row(int32_t x, int32_t y,
                                              uint32_t target_width,
                                              uint32_t source_width,
                                              const uint8_t *rgba);
int mint_display_frame_begin(void);
int mint_display_frame_end(void);
int mint_display_current_capture_background(void);
int mint_display_current_restore_rect(int32_t x, int32_t y, int32_t width, int32_t height);
int mint_display_current_draw_text(const char *text, int32_t x, int32_t y,
                                   uint32_t rgb, uint32_t scale);
void mint_display_cursor_move(int32_t x, int32_t y);

#endif
