#ifndef MINTDE_FONT_H
#define MINTDE_FONT_H
#include <stdint.h>

/* Pre-rasterized Inter faces (tools/make_fonts.py). Coverage is 8-bit and
 * cropped per glyph; offsets are relative to the pen position on the baseline. */
typedef struct mint_glyph {
    uint16_t codepoint;
    uint16_t advance64;   /* 26.6 fixed point */
    int8_t xoff, yoff;    /* top-left of bitmap relative to (pen x, baseline) */
    uint8_t w, h;
    uint32_t offset;      /* into the face's coverage array */
} MINT_GLYPH;

typedef struct mint_font {
    uint8_t px, ascent, descent, line;
    uint16_t glyph_count, reserved;
    const MINT_GLYPH *glyphs;
    const uint8_t *bits;
} MINT_FONT;

enum {
    FONT_CAPTION = 0,     /* 11 Regular */
    FONT_CAPTION_BOLD,    /* 11 SemiBold */
    FONT_SMALL,           /* 12 Regular */
    FONT_BODY,            /* 13 Regular  (system font size) */
    FONT_BODY_MEDIUM,     /* 13 Medium */
    FONT_BODY_BOLD,       /* 13 Bold     (menu bar app name) */
    FONT_HEADLINE,        /* 15 SemiBold */
    FONT_TITLE,           /* 20 Bold */
    FONT_LARGE,           /* 22 Regular  (Spotlight field) */
    FONT_HUGE,            /* 30 Bold */
    FONT_DISPLAY,         /* 44 Medium   (calculator display) */
    MINT_FONT_COUNT
};

extern const MINT_FONT mint_fonts[MINT_FONT_COUNT];
#endif
