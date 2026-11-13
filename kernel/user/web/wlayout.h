#ifndef MINT_WEB_WLAYOUT_H
#define MINT_WEB_WLAYOUT_H
/* Layout for the MintOS Web engine: turns a styled DOM into a display list
 * in document coordinates (0,0 = top-left of the page). Supports block and
 * inline formatting with line breaking and text-align, collapsing margins
 * between siblings, floats with line wrapping, clear, lists with markers,
 * tables (colspan/rowspan, auto and fixed column widths), inline-block,
 * flex rows approximated as wrapping lines of boxes, images with intrinsic
 * or specified sizes, and form controls. The fonts are the MintDE Inter faces;
 * CSS sizes map to the nearest face. Integer only, bounded recursion. */
#include "wdoc.h"

enum { WI_RECT = 1, WI_TEXT, WI_IMAGE, WI_CTL, WI_BULLET, WI_BORDER, WI_ALT };
enum { WIF_ITALIC = 1, WIF_UNDERLINE = 2, WIF_STRIKE = 4, WIF_MONO = 8, WIF_OVERLINE = 16, WIF_PLACEHOLDER = 32 };
enum { WBUL_DISC = 0, WBUL_CIRCLE, WBUL_SQUARE };

typedef struct witem {
    uint8_t kind, font, flags, extra;  /* extra: bullet shape, border style, mono cell width */
    int32_t x, y, w, h;                /* TEXT: y is the top of the text, baseline = y + ascent(font) */
    uint32_t color;                    /* ARGB; BORDER: top colour */
    int32_t link;                      /* index into doc->links or -1 */
    uint32_t a, b;                     /* TEXT: pool offset/length; IMAGE: image index; CTL: control;
                                          RECT: corner radius; BORDER: widths (t,r,b,l bytes) */
    uint32_t c;                        /* BORDER: other colours packed? (unused = 0) */
} witem;

typedef struct wlayout_out {
    witem *items; uint32_t count, cap;
    char *pool; size_t pool_len, pool_cap;
    int32_t width, height;             /* document extent */
    uint32_t page_bg;                  /* canvas colour (html/body background) */
    uint32_t page_color;
    int oom, truncated;
} wlayout_out;

typedef struct wlayout_env {
    int32_t viewport_w, viewport_h;
    /* intrinsic size of doc image img: 1 if known */
    int (*image_size)(void *ctx, int32_t img, int32_t *w, int32_t *h);
    /* 0 loading, 1 decoded, -1 failed / unsupported */
    int (*image_state)(void *ctx, int32_t img);
    void *ctx;
} wlayout_env;

int wlayout_run(wdoc *doc, const wlayout_env *env, wlayout_out *out);
void wlayout_free(wlayout_out *out);

/* Fonts shared with the renderer. */
int wfont_pick(int css_px, int weight, int italic, int mono, int nonascii);
int32_t wtext_width(int font, const char *s, size_t n);
int32_t wfont_ascent(int font);
int32_t wfont_line(int font);
int32_t wfont_mono_cell(int font);
#endif
