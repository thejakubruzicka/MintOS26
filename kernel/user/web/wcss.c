/* CSS for the MintOS Web engine — see wdoc.h.
 *
 * Supported: type/class/id/universal/attribute selectors, descendant, child
 * and sibling combinators, :first-child/:last-child/:only-child/:nth-child/
 * :first-of-type/:last-of-type/:not()/:is()/:where()/:root/:link/:empty/
 * :checked/:disabled; specificity and source order; !important; @media
 * (width, prefers-color-scheme, print), @supports, @layer; custom properties
 * through var() (document-wide tokens); the properties in wstyle with their
 * shorthands. Unknown properties, values and at-rules are skipped. Selectors
 * with dynamic or generated-content pseudo classes (:hover, ::before) never
 * match. Rules are bucketed by their rightmost id/class/tag so each element
 * only tests the rules that can apply. Integer arithmetic throughout. */
#include "wdoc.h"
#include "whttp.h"

/* ------------------------------------------------------------------ properties */
enum {
    P_NONE = 0, P_DISPLAY, P_POSITION, P_FLOAT, P_CLEAR, P_WHITE_SPACE, P_TEXT_ALIGN, P_TEXT_TRANSFORM, P_LIST_STYLE_TYPE,
    P_LIST_STYLE_POSITION, P_FONT_WEIGHT, P_FONT_STYLE, P_FONT_FAMILY, P_FONT_SIZE, P_LINE_HEIGHT, P_TEXT_DECORATION_LINE,
    P_VISIBILITY, P_VERTICAL_ALIGN, P_OVERFLOW, P_BOX_SIZING, P_COLOR, P_BACKGROUND_COLOR, P_BACKGROUND_IMAGE, P_WIDTH,
    P_HEIGHT, P_MIN_WIDTH, P_MAX_WIDTH, P_MIN_HEIGHT, P_MAX_HEIGHT,
    P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM, P_MARGIN_LEFT, P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_BORDER_TOP_WIDTH, P_BORDER_RIGHT_WIDTH, P_BORDER_BOTTOM_WIDTH, P_BORDER_LEFT_WIDTH,
    P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE, P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE,
    P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR, P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR,
    P_TEXT_INDENT, P_BORDER_SPACING, P_BORDER_COLLAPSE, P_BORDER_RADIUS, P_FLEX_DIRECTION, P_FLEX_WRAP, P_JUSTIFY_CONTENT,
    P_GAP, P_LEFT, P_TOP, P_CLIP, P_TABLE_LAYOUT, P_OPACITY, P_FONT_SHORT_SIZE_ONLY,
    /* shorthands (expanded while parsing) */
    S_MARGIN, S_PADDING, S_BORDER, S_BORDER_TOP, S_BORDER_RIGHT, S_BORDER_BOTTOM, S_BORDER_LEFT, S_BORDER_WIDTH,
    S_BORDER_STYLE, S_BORDER_COLOR, S_BACKGROUND, S_FONT, S_LIST_STYLE, S_TEXT_DECORATION, S_FLEX_FLOW, S_INSET, S_OVERFLOW_X,
    S_MARGIN_INLINE, S_MARGIN_BLOCK, S_PADDING_INLINE, S_PADDING_BLOCK, S_CLIP_PATH, S_INSET_BLOCK_START, S_INSET_INLINE_START,
    P_COUNT
};
static const struct { const char *name; uint16_t id; } PROPS[] = {
    {"display", P_DISPLAY}, {"position", P_POSITION}, {"float", P_FLOAT}, {"clear", P_CLEAR}, {"white-space", P_WHITE_SPACE},
    {"text-align", P_TEXT_ALIGN}, {"text-transform", P_TEXT_TRANSFORM}, {"list-style-type", P_LIST_STYLE_TYPE},
    {"list-style-position", P_LIST_STYLE_POSITION}, {"font-weight", P_FONT_WEIGHT}, {"font-style", P_FONT_STYLE},
    {"font-family", P_FONT_FAMILY}, {"font-size", P_FONT_SIZE}, {"line-height", P_LINE_HEIGHT},
    {"text-decoration-line", P_TEXT_DECORATION_LINE}, {"visibility", P_VISIBILITY}, {"vertical-align", P_VERTICAL_ALIGN},
    {"overflow", P_OVERFLOW}, {"overflow-y", P_OVERFLOW}, {"box-sizing", P_BOX_SIZING}, {"color", P_COLOR},
    {"background-color", P_BACKGROUND_COLOR}, {"background-image", P_BACKGROUND_IMAGE}, {"width", P_WIDTH}, {"height", P_HEIGHT},
    {"min-width", P_MIN_WIDTH}, {"max-width", P_MAX_WIDTH}, {"min-height", P_MIN_HEIGHT}, {"max-height", P_MAX_HEIGHT},
    {"inline-size", P_WIDTH}, {"block-size", P_HEIGHT}, {"max-inline-size", P_MAX_WIDTH}, {"min-inline-size", P_MIN_WIDTH},
    {"margin-top", P_MARGIN_TOP}, {"margin-right", P_MARGIN_RIGHT}, {"margin-bottom", P_MARGIN_BOTTOM}, {"margin-left", P_MARGIN_LEFT},
    {"margin-block-start", P_MARGIN_TOP}, {"margin-block-end", P_MARGIN_BOTTOM}, {"margin-inline-start", P_MARGIN_LEFT},
    {"margin-inline-end", P_MARGIN_RIGHT},
    {"padding-top", P_PADDING_TOP}, {"padding-right", P_PADDING_RIGHT}, {"padding-bottom", P_PADDING_BOTTOM},
    {"padding-left", P_PADDING_LEFT}, {"padding-block-start", P_PADDING_TOP}, {"padding-block-end", P_PADDING_BOTTOM},
    {"padding-inline-start", P_PADDING_LEFT}, {"padding-inline-end", P_PADDING_RIGHT},
    {"border-top-width", P_BORDER_TOP_WIDTH}, {"border-right-width", P_BORDER_RIGHT_WIDTH},
    {"border-bottom-width", P_BORDER_BOTTOM_WIDTH}, {"border-left-width", P_BORDER_LEFT_WIDTH},
    {"border-top-style", P_BORDER_TOP_STYLE}, {"border-right-style", P_BORDER_RIGHT_STYLE},
    {"border-bottom-style", P_BORDER_BOTTOM_STYLE}, {"border-left-style", P_BORDER_LEFT_STYLE},
    {"border-top-color", P_BORDER_TOP_COLOR}, {"border-right-color", P_BORDER_RIGHT_COLOR},
    {"border-bottom-color", P_BORDER_BOTTOM_COLOR}, {"border-left-color", P_BORDER_LEFT_COLOR},
    {"text-indent", P_TEXT_INDENT}, {"border-spacing", P_BORDER_SPACING}, {"border-collapse", P_BORDER_COLLAPSE},
    {"border-radius", P_BORDER_RADIUS}, {"flex-direction", P_FLEX_DIRECTION}, {"flex-wrap", P_FLEX_WRAP},
    {"justify-content", P_JUSTIFY_CONTENT}, {"gap", P_GAP}, {"column-gap", P_GAP}, {"grid-gap", P_GAP}, {"left", P_LEFT},
    {"top", P_TOP}, {"clip", P_CLIP}, {"table-layout", P_TABLE_LAYOUT}, {"opacity", P_OPACITY},
    {"margin", S_MARGIN}, {"padding", S_PADDING}, {"border", S_BORDER}, {"border-top", S_BORDER_TOP},
    {"border-right", S_BORDER_RIGHT}, {"border-bottom", S_BORDER_BOTTOM}, {"border-left", S_BORDER_LEFT},
    {"border-block-start", S_BORDER_TOP}, {"border-block-end", S_BORDER_BOTTOM}, {"border-inline-start", S_BORDER_LEFT},
    {"border-inline-end", S_BORDER_RIGHT},
    {"border-width", S_BORDER_WIDTH}, {"border-style", S_BORDER_STYLE}, {"border-color", S_BORDER_COLOR},
    {"background", S_BACKGROUND}, {"font", S_FONT}, {"list-style", S_LIST_STYLE}, {"text-decoration", S_TEXT_DECORATION},
    {"flex-flow", S_FLEX_FLOW}, {"inset", S_INSET}, {"overflow-x", S_OVERFLOW_X}, {"margin-inline", S_MARGIN_INLINE},
    {"margin-block", S_MARGIN_BLOCK}, {"padding-inline", S_PADDING_INLINE}, {"padding-block", S_PADDING_BLOCK},
    {"clip-path", S_CLIP_PATH}, {"inset-block-start", S_INSET_BLOCK_START}, {"inset-inline-start", S_INSET_INLINE_START},
};

/* value kinds */
enum { V_NONE = 0, V_LEN, V_COLOR, V_KEY, V_NUM, V_URL, V_INHERIT, V_INITIAL, V_UNSET, V_VAR, V_FAMILY };
/* length units before computing */
enum { U_PX = 0, U_PCT, U_EM, U_REM, U_EX, U_VW, U_VH, U_AUTO, U_CH, U_NORMAL };

typedef struct wdecl {
    uint16_t prop;
    uint8_t kind, unit, important, pad[3];
    int32_t n;                     /* lengths and numbers x64, colours ARGB, keywords K_* */
    const char *s;                 /* url, font family, raw text for var() */
} wdecl;

/* keywords */
enum {
    K_NONE = 1, K_INLINE, K_BLOCK, K_INLINE_BLOCK, K_LIST_ITEM, K_TABLE, K_INLINE_TABLE, K_TABLE_ROW, K_TABLE_CELL,
    K_TABLE_ROW_GROUP, K_TABLE_HEADER_GROUP, K_TABLE_FOOTER_GROUP, K_TABLE_CAPTION, K_TABLE_COLUMN, K_TABLE_COLUMN_GROUP,
    K_FLEX, K_INLINE_FLEX, K_GRID, K_INLINE_GRID, K_CONTENTS, K_FLOW_ROOT,
    K_STATIC, K_RELATIVE, K_ABSOLUTE, K_FIXED, K_STICKY, K_LEFT, K_RIGHT, K_BOTH, K_CENTER, K_JUSTIFY, K_START, K_END,
    K_NORMAL, K_NOWRAP, K_PRE, K_PRE_WRAP, K_PRE_LINE, K_BREAK_SPACES, K_UPPERCASE, K_LOWERCASE, K_CAPITALIZE,
    K_DISC, K_CIRCLE, K_SQUARE, K_DECIMAL, K_DECIMAL_LZ, K_LOWER_ALPHA, K_UPPER_ALPHA, K_LOWER_LATIN, K_UPPER_LATIN,
    K_LOWER_ROMAN, K_UPPER_ROMAN, K_INSIDE, K_OUTSIDE, K_BOLD, K_BOLDER, K_LIGHTER, K_ITALIC, K_OBLIQUE, K_UNDERLINE,
    K_LINE_THROUGH, K_OVERLINE, K_HIDDEN, K_VISIBLE, K_COLLAPSE, K_BASELINE, K_TOP, K_MIDDLE, K_BOTTOM, K_SUB, K_SUPER,
    K_TEXT_TOP, K_TEXT_BOTTOM, K_SCROLL, K_AUTO, K_CLIP, K_BORDER_BOX, K_CONTENT_BOX, K_SOLID, K_DOTTED, K_DASHED,
    K_DOUBLE, K_INSET, K_OUTSET, K_GROOVE, K_RIDGE, K_ROW, K_ROW_REVERSE, K_COLUMN, K_COLUMN_REVERSE, K_WRAP,
    K_FLEX_START, K_FLEX_END, K_SPACE_BETWEEN, K_SPACE_AROUND, K_SPACE_EVENLY, K_SEPARATE, K_FIXED_LAYOUT,
    K_THIN, K_MEDIUM, K_THICK, K_XX_SMALL, K_X_SMALL, K_SMALL, K_LARGE, K_X_LARGE, K_XX_LARGE, K_SMALLER, K_LARGER,
    K_RUBY, K_TRANSPARENT, K_CURRENTCOLOR, K_HANGING, K_COUNT
};
static const char *const KEYWORDS[K_COUNT] = {
    0, "none", "inline", "block", "inline-block", "list-item", "table", "inline-table", "table-row", "table-cell",
    "table-row-group", "table-header-group", "table-footer-group", "table-caption", "table-column", "table-column-group",
    "flex", "inline-flex", "grid", "inline-grid", "contents", "flow-root",
    "static", "relative", "absolute", "fixed", "sticky", "left", "right", "both", "center", "justify", "start", "end",
    "normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces", "uppercase", "lowercase", "capitalize",
    "disc", "circle", "square", "decimal", "decimal-leading-zero", "lower-alpha", "upper-alpha", "lower-latin", "upper-latin",
    "lower-roman", "upper-roman", "inside", "outside", "bold", "bolder", "lighter", "italic", "oblique", "underline",
    "line-through", "overline", "hidden", "visible", "collapse", "baseline", "top", "middle", "bottom", "sub", "super",
    "text-top", "text-bottom", "scroll", "auto", "clip", "border-box", "content-box", "solid", "dotted", "dashed",
    "double", "inset", "outset", "groove", "ridge", "row", "row-reverse", "column", "column-reverse", "wrap",
    "flex-start", "flex-end", "space-between", "space-around", "space-evenly", "separate", "fixed",
    "thin", "medium", "thick", "xx-small", "x-small", "small", "large", "x-large", "xx-large", "smaller", "larger",
    "ruby", "transparent", "currentcolor", "hanging",
};

/* ------------------------------------------------------------------ selectors */
enum { PS_FIRST_CHILD = 1, PS_LAST_CHILD, PS_ONLY_CHILD, PS_FIRST_OF_TYPE, PS_LAST_OF_TYPE, PS_NTH_CHILD, PS_NTH_LAST_CHILD,
       PS_NTH_OF_TYPE, PS_NOT, PS_IS, PS_ROOT, PS_LINK, PS_EMPTY, PS_CHECKED, PS_DISABLED, PS_ENABLED, PS_NEVER };
enum { AO_EXISTS = 0, AO_EQ, AO_WORD, AO_DASH, AO_PREFIX, AO_SUFFIX, AO_SUBSTR };

typedef struct wcomp wcomp;
typedef struct wattrsel { const char *name, *val; uint8_t op, icase; } wattrsel;
typedef struct wpseudo { uint8_t kind; int16_t a, b; wcomp *inner; uint8_t ninner; } wpseudo;
struct wcomp {
    uint16_t tag;                  /* 0 = any */
    uint8_t comb;                  /* combinator to the compound on the left: ' ', '>', '+', '~' (0 for the first) */
    uint8_t ncls, nattr, nps;
    const char *name, *id;
    const char **cls; uint8_t *cls_len;
    wattrsel *attrs;
    wpseudo *ps;
};
typedef struct wrule {
    wcomp *comp; uint8_t ncomp, origin;
    uint16_t media;
    uint32_t spec, order;
    uint32_t decl, ndecl;
    int32_t next;                  /* bucket chain */
} wrule;

#define NBUCKET 2048u
#define MAX_MEDIA 512u
typedef struct wmedia { const char *text; uint16_t len; uint8_t ok; } wmedia;
typedef struct wvar { const char *name; uint16_t nlen; const char *val; uint32_t vlen; } wvar;

struct wcss {
    warena arena;
    wrule *rules; uint32_t nrules, cap_rules;
    wdecl *decls; uint32_t ndecls, cap_decls;
    wmedia media[MAX_MEDIA]; uint32_t nmedia;
    int32_t b_id[NBUCKET], b_cls[NBUCKET], b_tag[NBUCKET], b_any;
    wvar *vars; uint32_t nvars, cap_vars;
    int32_t vw, vh;
    int dark;
    uint32_t order;
    uint32_t unknown_props, media_skipped, sheets;
    /* style sharing */
    const wstyle **shared; uint32_t shared_cap, shared_count;
    int oom;
};

/* ------------------------------------------------------------------ helpers */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || (uint8_t)c >= 0x80; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int ieq_n(const char *a, size_t n, const char *b) {
    size_t i = 0;
    for (; i < n && b[i]; ++i) if (lc(a[i]) != lc(b[i])) return 0;
    return i == n && !b[i];
}
static uint32_t hash_cs(const char *s, size_t n) {              /* case-sensitive (classes, ids) */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static int keyword(const char *s, size_t n) {
    for (int k = 1; k < K_COUNT; ++k) if (ieq_n(s, n, KEYWORDS[k])) return k;
    return 0;
}
static uint16_t prop_id(const char *s, size_t n) {
    if (n > 8 && s[0] == '-' && s[1] != '-') {                 /* vendor prefixes: -webkit-box-sizing -> box-sizing */
        size_t i = 1;
        while (i < n && s[i] != '-') ++i;
        if (i < n) { s += i + 1; n -= i + 1; }
    }
    for (size_t i = 0; i < sizeof(PROPS) / sizeof(PROPS[0]); ++i) if (ieq_n(s, n, PROPS[i].name)) return PROPS[i].id;
    return P_NONE;
}

/* ------------------------------------------------------------------ colours */
static const struct { const char *name; uint32_t rgb; } COLORS[] = {
    {"aliceblue",0xF0F8FF},{"antiquewhite",0xFAEBD7},{"aqua",0x00FFFF},{"aquamarine",0x7FFFD4},{"azure",0xF0FFFF},
    {"beige",0xF5F5DC},{"bisque",0xFFE4C4},{"black",0x000000},{"blanchedalmond",0xFFEBCD},{"blue",0x0000FF},
    {"blueviolet",0x8A2BE2},{"brown",0xA52A2A},{"burlywood",0xDEB887},{"cadetblue",0x5F9EA0},{"chartreuse",0x7FFF00},
    {"chocolate",0xD2691E},{"coral",0xFF7F50},{"cornflowerblue",0x6495ED},{"cornsilk",0xFFF8DC},{"crimson",0xDC143C},
    {"cyan",0x00FFFF},{"darkblue",0x00008B},{"darkcyan",0x008B8B},{"darkgoldenrod",0xB8860B},{"darkgray",0xA9A9A9},
    {"darkgreen",0x006400},{"darkgrey",0xA9A9A9},{"darkkhaki",0xBDB76B},{"darkmagenta",0x8B008B},{"darkolivegreen",0x556B2F},
    {"darkorange",0xFF8C00},{"darkorchid",0x9932CC},{"darkred",0x8B0000},{"darksalmon",0xE9967A},{"darkseagreen",0x8FBC8F},
    {"darkslateblue",0x483D8B},{"darkslategray",0x2F4F4F},{"darkslategrey",0x2F4F4F},{"darkturquoise",0x00CED1},
    {"darkviolet",0x9400D3},{"deeppink",0xFF1493},{"deepskyblue",0x00BFFF},{"dimgray",0x696969},{"dimgrey",0x696969},
    {"dodgerblue",0x1E90FF},{"firebrick",0xB22222},{"floralwhite",0xFFFAF0},{"forestgreen",0x228B22},{"fuchsia",0xFF00FF},
    {"gainsboro",0xDCDCDC},{"ghostwhite",0xF8F8FF},{"gold",0xFFD700},{"goldenrod",0xDAA520},{"gray",0x808080},
    {"green",0x008000},{"greenyellow",0xADFF2F},{"grey",0x808080},{"honeydew",0xF0FFF0},{"hotpink",0xFF69B4},
    {"indianred",0xCD5C5C},{"indigo",0x4B0082},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},{"lavender",0xE6E6FA},
    {"lavenderblush",0xFFF0F5},{"lawngreen",0x7CFC00},{"lemonchiffon",0xFFFACD},{"lightblue",0xADD8E6},{"lightcoral",0xF08080},
    {"lightcyan",0xE0FFFF},{"lightgoldenrodyellow",0xFAFAD2},{"lightgray",0xD3D3D3},{"lightgreen",0x90EE90},
    {"lightgrey",0xD3D3D3},{"lightpink",0xFFB6C1},{"lightsalmon",0xFFA07A},{"lightseagreen",0x20B2AA},
    {"lightskyblue",0x87CEFA},{"lightslategray",0x778899},{"lightslategrey",0x778899},{"lightsteelblue",0xB0C4DE},
    {"lightyellow",0xFFFFE0},{"lime",0x00FF00},{"limegreen",0x32CD32},{"linen",0xFAF0E6},{"magenta",0xFF00FF},
    {"maroon",0x800000},{"mediumaquamarine",0x66CDAA},{"mediumblue",0x0000CD},{"mediumorchid",0xBA55D3},
    {"mediumpurple",0x9370DB},{"mediumseagreen",0x3CB371},{"mediumslateblue",0x7B68EE},{"mediumspringgreen",0x00FA9A},
    {"mediumturquoise",0x48D1CC},{"mediumvioletred",0xC71585},{"midnightblue",0x191970},{"mintcream",0xF5FFFA},
    {"mistyrose",0xFFE4E1},{"moccasin",0xFFE4B5},{"navajowhite",0xFFDEAD},{"navy",0x000080},{"oldlace",0xFDF5E6},
    {"olive",0x808000},{"olivedrab",0x6B8E23},{"orange",0xFFA500},{"orangered",0xFF4500},{"orchid",0xDA70D6},
    {"palegoldenrod",0xEEE8AA},{"palegreen",0x98FB98},{"paleturquoise",0xAFEEEE},{"palevioletred",0xDB7093},
    {"papayawhip",0xFFEFD5},{"peachpuff",0xFFDAB9},{"peru",0xCD853F},{"pink",0xFFC0CB},{"plum",0xDDA0DD},
    {"powderblue",0xB0E0E6},{"purple",0x800080},{"rebeccapurple",0x663399},{"red",0xFF0000},{"rosybrown",0xBC8F8F},
    {"royalblue",0x4169E1},{"saddlebrown",0x8B4513},{"salmon",0xFA8072},{"sandybrown",0xF4A460},{"seagreen",0x2E8B57},
    {"seashell",0xFFF5EE},{"sienna",0xA0522D},{"silver",0xC0C0C0},{"skyblue",0x87CEEB},{"slateblue",0x6A5ACD},
    {"slategray",0x708090},{"slategrey",0x708090},{"snow",0xFFFAFA},{"springgreen",0x00FF7F},{"steelblue",0x4682B4},
    {"tan",0xD2B48C},{"teal",0x008080},{"thistle",0xD8BFD8},{"tomato",0xFF6347},{"turquoise",0x40E0D0},{"violet",0xEE82EE},
    {"wheat",0xF5DEB3},{"white",0xFFFFFF},{"whitesmoke",0xF5F5F5},{"yellow",0xFFFF00},{"yellowgreen",0x9ACD32},
    /* CSS system colours, light appearance */
    {"canvas",0xFFFFFF},{"canvastext",0x000000},{"linktext",0x0066CC},{"visitedtext",0x551A8B},{"buttonface",0xEFEFEF},
    {"buttontext",0x000000},{"field",0xFFFFFF},{"fieldtext",0x000000},{"graytext",0x808080},{"highlight",0xB3D7FF},
    {"highlighttext",0x000000},{"window",0xFFFFFF},{"windowtext",0x000000},{"buttonborder",0x767676},
};

static int hexd(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}
/* Parses a CSS number into x1000 fixed point; returns chars used (0 = none). */
static size_t parse_num(const char *s, size_t n, int64_t *out) {
    size_t i = 0;
    int neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
    int64_t ip = 0, fp = 0, fd = 1;
    size_t digits = 0;
    while (i < n && is_digit(s[i])) { if (ip < 100000000) ip = ip * 10 + (s[i] - '0'); ++i; ++digits; }
    if (i < n && s[i] == '.' && i + 1 < n && is_digit(s[i + 1])) {
        ++i;
        while (i < n && is_digit(s[i])) { if (fd < 1000) { fp = fp * 10 + (s[i] - '0'); fd *= 10; } ++i; ++digits; }
    }
    if (!digits) return 0;
    if (i + 1 < n && (s[i] == 'e' || s[i] == 'E') && (is_digit(s[i + 1]) || ((s[i + 1] == '-' || s[i + 1] == '+') && i + 2 < n && is_digit(s[i + 2])))) {
        size_t j = i + 1; int eneg = 0; int e = 0;
        if (s[j] == '-' || s[j] == '+') { eneg = s[j] == '-'; ++j; }
        while (j < n && is_digit(s[j])) { if (e < 20) e = e * 10 + (s[j] - '0'); ++j; }
        int64_t v = ip * 1000 + fp * 1000 / fd;
        while (e-- > 0) { if (eneg) v /= 10; else if (v < ((int64_t)1 << 40)) v *= 10; }
        *out = neg ? -v : v;
        return j;
    }
    int64_t v = ip * 1000 + fp * 1000 / fd;
    *out = neg ? -v : v;
    return i;
}

static uint32_t hsl_to_rgb(int64_t h, int64_t s1000, int64_t l1000) {
    h %= 360000; if (h < 0) h += 360000;                     /* x1000 degrees */
    int64_t c = (1000 - (2 * l1000 - 1000 < 0 ? -(2 * l1000 - 1000) : 2 * l1000 - 1000)) * s1000 / 1000;
    int64_t hp = h / 60;                                     /* x1000 sectors */
    int64_t m2 = hp % 2000 - 1000; if (m2 < 0) m2 = -m2;
    int64_t x = c * (1000 - m2) / 1000;
    int64_t r = 0, g = 0, b = 0;
    if (hp < 1000) { r = c; g = x; } else if (hp < 2000) { r = x; g = c; } else if (hp < 3000) { g = c; b = x; }
    else if (hp < 4000) { g = x; b = c; } else if (hp < 5000) { r = x; b = c; } else { r = c; b = x; }
    int64_t m = l1000 - c / 2;
    r = (r + m) * 255 / 1000; g = (g + m) * 255 / 1000; b = (b + m) * 255 / 1000;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint32_t)((r << 16) | (g << 8) | b);
}

int wcss_parse_color(const char *s, size_t n, uint32_t *out) {
    while (n && is_ws(*s)) { ++s; --n; }
    while (n && is_ws(s[n - 1])) --n;
    if (!n) return 0;
    if (s[0] == '#') {
        int v[8]; size_t k = n - 1;
        if (k != 3 && k != 4 && k != 6 && k != 8) return 0;
        for (size_t i = 0; i < k; ++i) { v[i] = hexd(s[1 + i]); if (v[i] < 0) return 0; }
        uint32_t r, g, b, a = 255;
        if (k <= 4) { r = (uint32_t)v[0] * 17u; g = (uint32_t)v[1] * 17u; b = (uint32_t)v[2] * 17u; if (k == 4) a = (uint32_t)v[3] * 17u; }
        else { r = (uint32_t)(v[0] * 16 + v[1]); g = (uint32_t)(v[2] * 16 + v[3]); b = (uint32_t)(v[4] * 16 + v[5]); if (k == 8) a = (uint32_t)(v[6] * 16 + v[7]); }
        *out = (a << 24) | (r << 16) | (g << 8) | b;
        return 1;
    }
    if ((n > 4 && ieq_n(s, 4, "rgb(")) || (n > 5 && ieq_n(s, 5, "rgba(")) || (n > 4 && ieq_n(s, 4, "hsl(")) || (n > 5 && ieq_n(s, 5, "hsla("))) {
        int hsl = lc(s[0]) == 'h';
        size_t i = 0;
        while (i < n && s[i] != '(') ++i;
        ++i;
        int64_t v[4] = {0, 0, 0, 1000}; int pct[4] = {0, 0, 0, 0};
        int count = 0;
        while (i < n && count < 4) {
            while (i < n && (is_ws(s[i]) || s[i] == ',' || s[i] == '/')) ++i;
            if (i >= n || s[i] == ')') break;
            if (ieq_n(s + i, 4 <= n - i ? 4 : n - i, "none")) { v[count++] = 0; i += 4; continue; }
            size_t used = parse_num(s + i, n - i, &v[count]);
            if (!used) return 0;
            i += used;
            if (i < n && s[i] == '%') { pct[count] = 1; ++i; }
            else if (i + 3 <= n && ieq_n(s + i, 3, "deg")) i += 3;
            ++count;
        }
        if (count < 3) return 0;
        uint32_t rgb;
        if (hsl) rgb = hsl_to_rgb(v[0], pct[1] ? v[1] / 100 : v[1], pct[2] ? v[2] / 100 : v[2]);
        else {
            int64_t c[3];
            for (int k = 0; k < 3; ++k) {
                c[k] = pct[k] ? v[k] * 255 / 100000 : v[k] / 1000;
                if (c[k] < 0) c[k] = 0;
                if (c[k] > 255) c[k] = 255;
            }
            rgb = (uint32_t)((c[0] << 16) | (c[1] << 8) | c[2]);
        }
        int64_t a = pct[3] ? v[3] / 100 : v[3];               /* x1000 */
        if (count < 4) a = 1000;
        if (a < 0) a = 0;
        if (a > 1000) a = 1000;
        *out = ((uint32_t)(a * 255 / 1000) << 24) | rgb;
        return 1;
    }
    if (ieq_n(s, n, "transparent")) { *out = 0; return 1; }
    for (size_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]); ++i) if (ieq_n(s, n, COLORS[i].name)) { *out = 0xFF000000u | COLORS[i].rgb; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ value parsing */
typedef struct tok { const char *s; size_t n; } tok;
/* Splits a value into space-separated tokens (keeping function calls whole). */
static uint32_t split_tokens(const char *s, size_t n, tok *out, uint32_t max, char sep) {
    uint32_t count = 0;
    size_t i = 0;
    int ws_split = sep != ',';                      /* comma lists keep their inner spaces */
    while (i < n && count < max) {
        while (i < n && (is_ws(s[i]) || s[i] == sep)) ++i;
        if (i >= n) break;
        size_t st = i;
        int depth = 0;
        char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) { if (c == q) q = 0; ++i; continue; }
            if (c == '"' || c == '\'') { q = c; ++i; continue; }
            if (c == '(' || c == '[') ++depth;
            else if (c == ')' || c == ']') { if (depth) --depth; }
            else if (!depth && ((ws_split && is_ws(c)) || c == sep)) break;
            ++i;
        }
        size_t e = i;
        while (e > st && is_ws(s[e - 1])) --e;
        out[count].s = s + st; out[count].n = e - st;
        ++count;
    }
    return count;
}

/* Evaluates a calc()/min()/max()/clamp() expression; returns 1 with a px or % result. */
static int parse_len(const char *s, size_t n, int32_t *v64, uint8_t *unit);
static int eval_calc(const char *s, size_t n, int32_t *v64, uint8_t *unit) {
    /* inner of calc(...): terms separated by + / -, factors by * and / with plain numbers */
    int64_t px = 0, pct = 0, em = 0, rem = 0, vw = 0;
    int have_pct = 0;
    size_t i = 0;
    int sign = 1;
    while (i < n) {
        while (i < n && is_ws(s[i])) ++i;
        if (i >= n) break;
        if (s[i] == '+') { sign = 1; ++i; continue; }
        if (s[i] == '-' && i + 1 < n && is_ws(s[i + 1])) { sign = -1; ++i; continue; }
        size_t st = i;
        int depth = 0;
        while (i < n && (depth || !(is_ws(s[i]) && i + 1 < n && (s[i + 1] == '+' || s[i + 1] == '-') && i + 2 < n && is_ws(s[i + 2])))) {
            if (s[i] == '(') ++depth; else if (s[i] == ')' && depth) --depth;
            ++i;
        }
        /* term s[st..i): value [* num] [/ num] */
        tok parts[5]; uint32_t np = split_tokens(s + st, i - st, parts, 5, 0);
        if (!np) return 0;
        int32_t tv; uint8_t tu;
        int64_t mul = 1000, div = 1000;
        size_t vi = 0;
        int64_t num;
        if (np >= 3 && parts[1].n == 1 && parts[1].s[0] == '*' && parse_num(parts[0].s, parts[0].n, &num) == parts[0].n) { mul = num; vi = 2; }
        if (!parse_len(parts[vi].s, parts[vi].n, &tv, &tu)) {
            if (parts[vi].n > 1 && parts[vi].s[0] == '(') {
                if (!eval_calc(parts[vi].s + 1, parts[vi].n - 2, &tv, &tu)) return 0;
            } else return 0;
        }
        for (uint32_t k = vi + 1; k + 1 < np; k += 2) {
            if (parse_num(parts[k + 1].s, parts[k + 1].n, &num) != parts[k + 1].n) return 0;
            if (parts[k].n == 1 && parts[k].s[0] == '*') mul = mul * num / 1000;
            else if (parts[k].n == 1 && parts[k].s[0] == '/' && num) div = div * num / 1000;
        }
        int64_t val = (int64_t)tv * mul / div * sign;
        switch (tu) {
        case U_PX: px += val; break;
        case U_PCT: pct += val; have_pct = 1; break;
        case U_EM: em += val; break;
        case U_REM: rem += val; break;
        case U_VW: vw += val; break;
        default: break;
        }
        sign = 1;
    }
    if (have_pct) { *v64 = (int32_t)pct; *unit = U_PCT; return 1; }
    /* em/rem/vw are folded at 16px / 1280px since calc() results are rarely critical */
    *v64 = (int32_t)(px + em * 16 + rem * 16 + vw * 12);
    *unit = U_PX;
    return 1;
}

static int parse_len(const char *s, size_t n, int32_t *v64, uint8_t *unit) {
    while (n && is_ws(*s)) { ++s; --n; }
    while (n && is_ws(s[n - 1])) --n;
    if (!n) return 0;
    if (ieq_n(s, n, "auto")) { *v64 = 0; *unit = U_AUTO; return 1; }
    if (n > 5 && (ieq_n(s, 5, "calc(") || ieq_n(s, 4, "min(") || ieq_n(s, 4, "max(") || ieq_n(s, 6, "clamp(")) && s[n - 1] == ')') {
        size_t open = 0;
        while (s[open] != '(') ++open;
        const char *in = s + open + 1; size_t il = n - open - 2;
        if (lc(s[0]) == 'c' && lc(s[1]) == 'a') return eval_calc(in, il, v64, unit);
        /* min()/max()/clamp(): use the middle (clamp) or first argument */
        tok args[3]; uint32_t na = split_tokens(in, il, args, 3, ',');
        if (!na) return 0;
        tok a = na == 3 ? args[1] : args[0];
        if (lc(s[1]) == 'a' && na >= 2) a = args[na - 1];             /* max(): last is usually the floor in px... */
        if (parse_len(a.s, a.n, v64, unit)) return 1;
        return eval_calc(a.s, a.n, v64, unit);
    }
    int64_t num;
    size_t used = parse_num(s, n, &num);
    if (!used) return 0;
    const char *u = s + used; size_t ul = n - used;
    *v64 = (int32_t)(num * 64 / 1000);
    if (!ul) { if (num != 0) { *unit = U_PX; return 2; } *unit = U_PX; return 1; }      /* 2 = unitless */
    if (ieq_n(u, ul, "px")) *unit = U_PX;
    else if (ul == 1 && u[0] == '%') *unit = U_PCT;
    else if (ieq_n(u, ul, "em")) *unit = U_EM;
    else if (ieq_n(u, ul, "rem")) *unit = U_REM;
    else if (ieq_n(u, ul, "ex") || ieq_n(u, ul, "ch")) { *unit = U_EM; *v64 /= 2; }
    else if (ieq_n(u, ul, "vw") || ieq_n(u, ul, "vi") || ieq_n(u, ul, "vmin") || ieq_n(u, ul, "dvw") || ieq_n(u, ul, "svw")) *unit = U_VW;
    else if (ieq_n(u, ul, "vh") || ieq_n(u, ul, "dvh") || ieq_n(u, ul, "svh") || ieq_n(u, ul, "lvh") || ieq_n(u, ul, "vb") || ieq_n(u, ul, "vmax")) *unit = U_VH;
    else if (ieq_n(u, ul, "pt")) { *unit = U_PX; *v64 = (int32_t)(num * 64 * 4 / 3000); }
    else if (ieq_n(u, ul, "pc")) { *unit = U_PX; *v64 = (int32_t)(num * 64 * 16 / 1000); }
    else if (ieq_n(u, ul, "in")) { *unit = U_PX; *v64 = (int32_t)(num * 64 * 96 / 1000); }
    else if (ieq_n(u, ul, "cm")) { *unit = U_PX; *v64 = (int32_t)(num * 64 * 378 / 10000); }
    else if (ieq_n(u, ul, "mm")) { *unit = U_PX; *v64 = (int32_t)(num * 64 * 378 / 100000); }
    else if (ieq_n(u, ul, "q")) { *unit = U_PX; *v64 = (int32_t)(num * 64 * 378 / 400000); }
    else if (ieq_n(u, ul, "fr")) return 0;
    else return 0;
    return 1;
}

typedef struct dbuf { wdecl *d; uint32_t n, cap; } dbuf;
static wcss *C;

static void emit(dbuf *b, uint16_t prop, uint8_t kind, int32_t n, uint8_t unit, const char *s, int imp) {
    if (b->n >= b->cap) {
        uint32_t cap = b->cap ? b->cap * 2u : 256u;
        wdecl *nd = (wdecl *)wmem_realloc(b->d, cap * sizeof(wdecl));
        if (!nd) { if (C) C->oom = 1; return; }
        b->d = nd; b->cap = cap;
    }
    wdecl *d = &b->d[b->n++];
    wb_set(d, 0, sizeof(*d));
    d->prop = prop; d->kind = kind; d->n = n; d->unit = unit; d->s = s; d->important = (uint8_t)imp;
}

static int width_kw(const char *s, size_t n, int32_t *v) {
    if (ieq_n(s, n, "thin")) { *v = 1 * 64; return 1; }
    if (ieq_n(s, n, "medium")) { *v = 3 * 64; return 1; }
    if (ieq_n(s, n, "thick")) { *v = 5 * 64; return 1; }
    return 0;
}
static int border_style_kw(int k) {
    return k == K_NONE || k == K_HIDDEN || k == K_SOLID || k == K_DOTTED || k == K_DASHED || k == K_DOUBLE || k == K_INSET ||
           k == K_OUTSET || k == K_GROOVE || k == K_RIDGE;
}

/* Parses one longhand value into a declaration; returns 0 if invalid. */
static int parse_longhand(dbuf *b, uint16_t prop, const char *s, size_t n, int imp) {
    while (n && is_ws(*s)) { ++s; --n; }
    while (n && is_ws(s[n - 1])) --n;
    if (!n) return 0;
    if (ieq_n(s, n, "inherit")) { emit(b, prop, V_INHERIT, 0, 0, 0, imp); return 1; }
    if (ieq_n(s, n, "initial") || ieq_n(s, n, "revert") || ieq_n(s, n, "revert-layer")) { emit(b, prop, V_INITIAL, 0, 0, 0, imp); return 1; }
    if (ieq_n(s, n, "unset")) { emit(b, prop, V_UNSET, 0, 0, 0, imp); return 1; }
    int32_t v; uint8_t u;
    int k = keyword(s, n);
    switch (prop) {
    case P_COLOR: case P_BACKGROUND_COLOR: case P_BORDER_TOP_COLOR: case P_BORDER_RIGHT_COLOR: case P_BORDER_BOTTOM_COLOR: case P_BORDER_LEFT_COLOR: {
        uint32_t c;
        if (k == K_CURRENTCOLOR) { emit(b, prop, V_KEY, K_CURRENTCOLOR, 0, 0, imp); return 1; }
        if (!wcss_parse_color(s, n, &c)) return 0;
        emit(b, prop, V_COLOR, (int32_t)c, 0, 0, imp);
        return 1;
    }
    case P_BACKGROUND_IMAGE: {
        if (k == K_NONE) { emit(b, prop, V_KEY, K_NONE, 0, 0, imp); return 1; }
        for (size_t i = 0; i + 4 < n; ++i) {
            if (ieq_n(s + i, 4, "url(")) {
                size_t j = i + 4, e;
                while (j < n && is_ws(s[j])) ++j;
                char q = (s[j] == '"' || s[j] == '\'') ? s[j++] : 0;
                e = j;
                while (e < n && (q ? s[e] != q : s[e] != ')')) ++e;
                size_t l = e - j;
                while (!q && l && is_ws(s[j + l - 1])) --l;
                if (!l || (l > 5 && ieq_n(s + j, 5, "data:") && !(l > 15 && ieq_n(s + j, 11, "data:image/") && !ieq_n(s + j + 11, 3, "svg")))) return 0;
                const char *url = C ? warena_strndup(&C->arena, s + j, l) : 0;
                if (!url) return 0;
                emit(b, prop, V_URL, 0, 0, url, imp);
                return 1;
            }
        }
        return 0;
    }
    case P_DISPLAY: case P_POSITION: case P_FLOAT: case P_CLEAR: case P_WHITE_SPACE: case P_TEXT_ALIGN: case P_TEXT_TRANSFORM:
    case P_LIST_STYLE_TYPE: case P_LIST_STYLE_POSITION: case P_FONT_STYLE: case P_VISIBILITY: case P_OVERFLOW: case P_BOX_SIZING:
    case P_BORDER_TOP_STYLE: case P_BORDER_RIGHT_STYLE: case P_BORDER_BOTTOM_STYLE: case P_BORDER_LEFT_STYLE:
    case P_BORDER_COLLAPSE: case P_FLEX_DIRECTION: case P_FLEX_WRAP: case P_JUSTIFY_CONTENT: case P_TABLE_LAYOUT: {
        if (!k) {
            /* "display: block flow", "flex flow-root"... take the first word; "-webkit-box" counts as flex */
            tok t[3]; uint32_t nt = split_tokens(s, n, t, 3, 0);
            if (prop == P_DISPLAY && ieq_n(s, n, "-webkit-box")) k = K_FLEX;
            else if (nt >= 1) k = keyword(t[0].s, t[0].n);
            if (prop == P_DISPLAY && nt == 2 && k == K_INLINE) { int k2 = keyword(t[1].s, t[1].n); if (k2 == K_FLEX) k = K_INLINE_FLEX; else if (k2 == K_GRID) k = K_INLINE_GRID; else if (k2 == K_FLOW_ROOT) k = K_INLINE_BLOCK; }
            if (prop == P_TEXT_ALIGN && ieq_n(s, n, "-webkit-center")) k = K_CENTER;
            if (prop == P_TEXT_ALIGN && (ieq_n(s, n, "-webkit-left") || ieq_n(s, n, "-moz-left"))) k = K_LEFT;
            if (prop == P_LIST_STYLE_TYPE && n >= 2 && (s[0] == '"' || s[0] == '\'')) k = K_NONE;    /* string markers */
            if (!k) return 0;
        }
        emit(b, prop, V_KEY, k, 0, 0, imp);
        return 1;
    }
    case P_FONT_WEIGHT: {
        if (k == K_NORMAL) { emit(b, prop, V_NUM, 4, 0, 0, imp); return 1; }
        if (k == K_BOLD) { emit(b, prop, V_NUM, 7, 0, 0, imp); return 1; }
        if (k == K_BOLDER || k == K_LIGHTER) { emit(b, prop, V_KEY, k, 0, 0, imp); return 1; }
        int64_t num;
        if (parse_num(s, n, &num) == n) { int w = (int)(num / 100000); if (w < 1) w = 1; if (w > 9) w = 9; emit(b, prop, V_NUM, w, 0, 0, imp); return 1; }
        return 0;
    }
    case P_FONT_FAMILY: {
        int mono = 0;
        for (size_t i = 0; i + 4 <= n; ++i)
            if (ieq_n(s + i, 4, "mono") || (i + 7 <= n && ieq_n(s + i, 7, "courier")) || (i + 8 <= n && ieq_n(s + i, 8, "consolas")) ||
                (i + 5 <= n && ieq_n(s + i, 5, "menlo")) || (i + 6 <= n && ieq_n(s + i, 6, "monaco"))) { mono = 1; break; }
        emit(b, prop, V_FAMILY, mono, 0, 0, imp);
        return 1;
    }
    case P_FONT_SIZE: {
        if (k >= K_XX_SMALL && k <= K_LARGER) { emit(b, prop, V_KEY, k, 0, 0, imp); return 1; }
        if (k == K_SMALL || k == K_MEDIUM) { emit(b, prop, V_KEY, k, 0, 0, imp); return 1; }
        int r = parse_len(s, n, &v, &u);
        if (!r || u == U_AUTO || (r == 2 && v)) return 0;
        emit(b, prop, V_LEN, v, u, 0, imp);
        return 1;
    }
    case P_LINE_HEIGHT: {
        if (k == K_NORMAL) { emit(b, prop, V_KEY, K_NORMAL, 0, 0, imp); return 1; }
        int64_t num;
        if (parse_num(s, n, &num) == n) { emit(b, prop, V_NUM, (int32_t)(num * 64 / 1000), 0, 0, imp); return 1; }
        if (!parse_len(s, n, &v, &u) || u == U_AUTO) return 0;
        emit(b, prop, V_LEN, v, u, 0, imp);
        return 1;
    }
    case P_TEXT_DECORATION_LINE: {
        tok t[4]; uint32_t nt = split_tokens(s, n, t, 4, 0);
        int32_t bits = 0;
        for (uint32_t i = 0; i < nt; ++i) {
            int kk = keyword(t[i].s, t[i].n);
            if (kk == K_UNDERLINE) bits |= WDEC_UNDERLINE;
            else if (kk == K_LINE_THROUGH) bits |= WDEC_STRIKE;
            else if (kk == K_OVERLINE) bits |= WDEC_OVERLINE;
            else if (kk == K_NONE) bits = 0;
            else return 0;
        }
        emit(b, prop, V_NUM, bits, 0, 0, imp);
        return 1;
    }
    case P_VERTICAL_ALIGN: {
        if (k) { emit(b, prop, V_KEY, k, 0, 0, imp); return 1; }
        if (parse_len(s, n, &v, &u)) { emit(b, prop, V_KEY, v > 0 ? K_SUPER : v < 0 ? K_SUB : K_BASELINE, 0, 0, imp); return 1; }
        return 0;
    }
    case P_OPACITY: {
        int64_t num;
        size_t used = parse_num(s, n, &num);
        if (!used) return 0;
        if (used < n && s[used] == '%') num /= 100;
        emit(b, prop, V_NUM, (int32_t)num, 0, 0, imp);
        return 1;
    }
    case P_CLIP: emit(b, prop, V_NUM, k == K_AUTO ? 0 : 1, 0, 0, imp); return 1;
    case P_BORDER_TOP_WIDTH: case P_BORDER_RIGHT_WIDTH: case P_BORDER_BOTTOM_WIDTH: case P_BORDER_LEFT_WIDTH:
        if (width_kw(s, n, &v)) { emit(b, prop, V_LEN, v, U_PX, 0, imp); return 1; }
        if (!parse_len(s, n, &v, &u) || u == U_AUTO || u == U_PCT) return 0;
        emit(b, prop, V_LEN, v, u, 0, imp);
        return 1;
    case P_BORDER_SPACING: {
        tok t[2]; uint32_t nt = split_tokens(s, n, t, 2, 0);
        if (!nt || !parse_len(t[0].s, t[0].n, &v, &u)) return 0;
        emit(b, prop, V_LEN, v, u, 0, imp);
        return 1;
    }
    case P_BORDER_RADIUS: {
        tok t[4]; uint32_t nt = split_tokens(s, n, t, 4, '/');
        if (!nt || !parse_len(t[0].s, t[0].n, &v, &u)) return 0;
        emit(b, prop, V_LEN, v, u, 0, imp);
        return 1;
    }
    case P_GAP: {
        tok t[2]; uint32_t nt = split_tokens(s, n, t, 2, 0);
        if (!nt) return 0;
        tok g = nt == 2 ? t[1] : t[0];
        if (keyword(g.s, g.n) == K_NORMAL) { emit(b, prop, V_LEN, 0, U_PX, 0, imp); return 1; }
        if (!parse_len(g.s, g.n, &v, &u)) return 0;
        emit(b, prop, V_LEN, v, u, 0, imp);
        return 1;
    }
    default: {
        /* lengths: width/height/margins/paddings/text-indent/left/top */
        if (k == K_NONE && (prop == P_MAX_WIDTH || prop == P_MAX_HEIGHT)) { emit(b, prop, V_LEN, 0, U_AUTO, 0, imp); return 1; }
        if ((k == K_NORMAL || k == K_AUTO) && (prop == P_MIN_WIDTH || prop == P_MIN_HEIGHT)) { emit(b, prop, V_LEN, 0, U_PX, 0, imp); return 1; }
        if (ieq_n(s, n, "max-content") || ieq_n(s, n, "fit-content") || ieq_n(s, n, "min-content") || ieq_n(s, n, "-webkit-fill-available") ||
            ieq_n(s, n, "-moz-available") || ieq_n(s, n, "stretch") || ieq_n(s, n, "-webkit-max-content")) {
            emit(b, prop, V_LEN, 0, U_AUTO, 0, imp);
            return 1;
        }
        int r = parse_len(s, n, &v, &u);
        if (!r) return 0;
        if (r == 2 && prop != P_LEFT && prop != P_TOP) return 0;          /* unitless non-zero */
        if (u == U_AUTO && (prop == P_PADDING_TOP || prop == P_PADDING_RIGHT || prop == P_PADDING_BOTTOM || prop == P_PADDING_LEFT)) return 0;
        if ((prop >= P_PADDING_TOP && prop <= P_PADDING_LEFT) && v < 0) return 0;
        emit(b, prop, V_LEN, v, u, 0, imp);
        return 1;
    }
    }
}

static void four_sides(dbuf *b, uint16_t first, const char *s, size_t n, int imp) {
    tok t[4]; uint32_t nt = split_tokens(s, n, t, 4, 0);
    if (!nt) return;
    tok side[4];
    side[0] = t[0];
    side[1] = nt > 1 ? t[1] : t[0];
    side[2] = nt > 2 ? t[2] : t[0];
    side[3] = nt > 3 ? t[3] : side[1];
    dbuf tmp = {0, 0, 0};
    int ok = 1;
    for (int i = 0; i < 4 && ok; ++i) ok = parse_longhand(&tmp, (uint16_t)(first + i), side[i].s, side[i].n, imp);
    if (ok && tmp.n == 4) for (uint32_t i = 0; i < 4; ++i) emit(b, tmp.d[i].prop, tmp.d[i].kind, tmp.d[i].n, tmp.d[i].unit, tmp.d[i].s, imp);
    wmem_free(tmp.d);
}

static void border_side(dbuf *b, int side, const char *s, size_t n, int imp) {
    tok t[4]; uint32_t nt = split_tokens(s, n, t, 4, 0);
    int32_t w = 3 * 64; int st = K_NONE; uint32_t col = 0; int have_col = 0, cur = 0;
    if (nt == 1 && (keyword(t[0].s, t[0].n) == K_NONE || (t[0].n == 1 && t[0].s[0] == '0'))) { w = 0; }
    for (uint32_t i = 0; i < nt; ++i) {
        int k = keyword(t[i].s, t[i].n);
        int32_t v; uint8_t u;
        if (border_style_kw(k)) st = k;
        else if (width_kw(t[i].s, t[i].n, &v)) w = v;
        else if (k == K_CURRENTCOLOR) cur = 1;
        else if (wcss_parse_color(t[i].s, t[i].n, &col)) have_col = 1;
        else if (parse_len(t[i].s, t[i].n, &v, &u) && u != U_PCT && u != U_AUTO) { w = v; if (u == U_EM) w = v * 16; }
        else return;
    }
    emit(b, (uint16_t)(P_BORDER_TOP_WIDTH + side), V_LEN, w, U_PX, 0, imp);
    emit(b, (uint16_t)(P_BORDER_TOP_STYLE + side), V_KEY, st, 0, 0, imp);
    if (have_col) emit(b, (uint16_t)(P_BORDER_TOP_COLOR + side), V_COLOR, (int32_t)col, 0, 0, imp);
    else emit(b, (uint16_t)(P_BORDER_TOP_COLOR + side), V_KEY, K_CURRENTCOLOR, 0, 0, imp);
    (void)cur;
}

static void parse_font_shorthand(dbuf *b, const char *s, size_t n, int imp) {
    tok t[10]; uint32_t nt = split_tokens(s, n, t, 10, 0);
    if (nt == 1) {                                     /* system fonts: caption, menu... */
        emit(b, P_FONT_STYLE, V_KEY, K_NORMAL, 0, 0, imp);
        emit(b, P_FONT_WEIGHT, V_NUM, 4, 0, 0, imp);
        return;
    }
    int style = K_NORMAL, weight = 4;
    uint32_t i = 0;
    for (; i < nt; ++i) {
        int k = keyword(t[i].s, t[i].n);
        int64_t num;
        if (k == K_ITALIC || k == K_OBLIQUE) style = K_ITALIC;
        else if (k == K_BOLD) weight = 7;
        else if (k == K_BOLDER) weight = 7;
        else if (k == K_NORMAL || ieq_n(t[i].s, t[i].n, "small-caps")) {}
        else if (parse_num(t[i].s, t[i].n, &num) == t[i].n && num >= 100000) weight = (int)(num / 100000);
        else break;
    }
    if (i >= nt) return;
    /* size[/line-height] */
    const char *sz = t[i].s; size_t szl = t[i].n;
    const char *lh = 0; size_t lhl = 0;
    for (size_t k = 0; k < szl; ++k) if (sz[k] == '/') { lh = sz + k + 1; lhl = szl - k - 1; szl = k; break; }
    if (!lh && i + 1 < nt && t[i + 1].s[0] == '/') {
        if (t[i + 1].n > 1) { lh = t[i + 1].s + 1; lhl = t[i + 1].n - 1; ++i; }
        else if (i + 2 < nt) { lh = t[i + 2].s; lhl = t[i + 2].n; i += 2; }
    }
    if (!parse_longhand(b, P_FONT_SIZE, sz, szl, imp)) return;
    emit(b, P_FONT_STYLE, V_KEY, style, 0, 0, imp);
    emit(b, P_FONT_WEIGHT, V_NUM, weight, 0, 0, imp);
    if (lh) (void)parse_longhand(b, P_LINE_HEIGHT, lh, lhl, imp);
    else emit(b, P_LINE_HEIGHT, V_KEY, K_NORMAL, 0, 0, imp);
    if (i + 1 < nt) {
        const char *fam = t[i + 1].s;
        (void)parse_longhand(b, P_FONT_FAMILY, fam, (size_t)(s + n - fam), imp);
    }
}

/* Parses "prop: value" (value without "!important") into longhands. */
static void parse_property(dbuf *b, uint16_t prop, const char *v, size_t vn, int imp) {
    while (vn && is_ws(*v)) { ++v; --vn; }
    while (vn && is_ws(v[vn - 1])) --vn;
    if (!vn) return;
    /* CSS-wide keywords on shorthands apply to every longhand */
    int wide = ieq_n(v, vn, "inherit") || ieq_n(v, vn, "initial") || ieq_n(v, vn, "unset") || ieq_n(v, vn, "revert");
    switch (prop) {
    case S_MARGIN: four_sides(b, P_MARGIN_TOP, v, vn, imp); return;
    case S_PADDING: four_sides(b, P_PADDING_TOP, v, vn, imp); return;
    case S_BORDER_WIDTH: {
        tok t[4]; uint32_t nt = split_tokens(v, vn, t, 4, 0);
        for (uint32_t i = 0; i < 4 && nt; ++i) {
            tok x = i < nt ? t[i] : (i == 3 && nt > 1 ? t[1] : t[0]);
            if (nt == 3 && i == 3) x = t[1];
            (void)parse_longhand(b, (uint16_t)(P_BORDER_TOP_WIDTH + i), x.s, x.n, imp);
        }
        return;
    }
    case S_BORDER_STYLE: case S_BORDER_COLOR: {
        uint16_t first = prop == S_BORDER_STYLE ? P_BORDER_TOP_STYLE : P_BORDER_TOP_COLOR;
        tok t[4]; uint32_t nt = split_tokens(v, vn, t, 4, 0);
        for (uint32_t i = 0; i < 4 && nt; ++i) {
            tok x = i < nt ? t[i] : (i == 3 && nt > 1 ? t[1] : t[0]);
            if (nt == 3 && i == 3) x = t[1];
            (void)parse_longhand(b, (uint16_t)(first + i), x.s, x.n, imp);
        }
        return;
    }
    case S_BORDER: for (int i = 0; i < 4; ++i) border_side(b, i, v, vn, imp); return;
    case S_BORDER_TOP: border_side(b, 0, v, vn, imp); return;
    case S_BORDER_RIGHT: border_side(b, 1, v, vn, imp); return;
    case S_BORDER_BOTTOM: border_side(b, 2, v, vn, imp); return;
    case S_BORDER_LEFT: border_side(b, 3, v, vn, imp); return;
    case S_BACKGROUND: {
        if (wide) { (void)parse_longhand(b, P_BACKGROUND_COLOR, v, vn, imp); return; }
        /* the colour is the token that parses as one; url() becomes background-image */
        tok t[12]; uint32_t nt = split_tokens(v, vn, t, 12, 0);
        uint32_t col = 0; int have = 0, img = 0;
        for (uint32_t i = 0; i < nt; ++i) {
            if (t[i].n > 4 && ieq_n(t[i].s, 4, "url(")) { if (parse_longhand(b, P_BACKGROUND_IMAGE, t[i].s, t[i].n, imp)) img = 1; continue; }
            if (t[i].n > 3 && (t[i].s[t[i].n - 1] == ')') && !ieq_n(t[i].s, 4, "rgb(") && !ieq_n(t[i].s, 5, "rgba(") &&
                !ieq_n(t[i].s, 4, "hsl(") && !ieq_n(t[i].s, 5, "hsla(")) { img = 2; continue; }     /* gradients */
            if (wcss_parse_color(t[i].s, t[i].n, &col)) have = 1;
        }
        emit(b, P_BACKGROUND_COLOR, V_COLOR, have ? (int32_t)col : 0, 0, 0, imp);
        if (!img) emit(b, P_BACKGROUND_IMAGE, V_KEY, K_NONE, 0, 0, imp);
        return;
    }
    case S_FONT: if (wide) { (void)parse_longhand(b, P_FONT_SIZE, v, vn, imp); return; } parse_font_shorthand(b, v, vn, imp); return;
    case S_LIST_STYLE: {
        tok t[3]; uint32_t nt = split_tokens(v, vn, t, 3, 0);
        int set = 0;
        for (uint32_t i = 0; i < nt; ++i) {
            int k = keyword(t[i].s, t[i].n);
            if (k == K_INSIDE || k == K_OUTSIDE) emit(b, P_LIST_STYLE_POSITION, V_KEY, k, 0, 0, imp);
            else if (k && !set) { emit(b, P_LIST_STYLE_TYPE, V_KEY, k, 0, 0, imp); set = 1; }
        }
        return;
    }
    case S_TEXT_DECORATION: {
        tok t[5]; uint32_t nt = split_tokens(v, vn, t, 5, 0);
        int32_t bits = 0;
        for (uint32_t i = 0; i < nt; ++i) {
            int k = keyword(t[i].s, t[i].n);
            if (k == K_UNDERLINE) bits |= WDEC_UNDERLINE;
            else if (k == K_LINE_THROUGH) bits |= WDEC_STRIKE;
            else if (k == K_OVERLINE) bits |= WDEC_OVERLINE;
        }
        if (wide) { (void)parse_longhand(b, P_TEXT_DECORATION_LINE, v, vn, imp); return; }
        emit(b, P_TEXT_DECORATION_LINE, V_NUM, bits, 0, 0, imp);
        return;
    }
    case S_FLEX_FLOW: {
        tok t[2]; uint32_t nt = split_tokens(v, vn, t, 2, 0);
        for (uint32_t i = 0; i < nt; ++i) {
            int k = keyword(t[i].s, t[i].n);
            if (k == K_ROW || k == K_COLUMN || k == K_ROW_REVERSE || k == K_COLUMN_REVERSE) emit(b, P_FLEX_DIRECTION, V_KEY, k, 0, 0, imp);
            else if (k == K_WRAP || k == K_NOWRAP) emit(b, P_FLEX_WRAP, V_KEY, k, 0, 0, imp);
        }
        return;
    }
    case S_INSET: {
        tok t[4]; uint32_t nt = split_tokens(v, vn, t, 4, 0);
        if (nt >= 1) (void)parse_longhand(b, P_TOP, t[0].s, t[0].n, imp);
        if (nt >= 1) (void)parse_longhand(b, P_LEFT, t[nt >= 4 ? 3 : nt >= 2 ? 1 : 0].s, t[nt >= 4 ? 3 : nt >= 2 ? 1 : 0].n, imp);
        return;
    }
    case S_INSET_BLOCK_START: (void)parse_longhand(b, P_TOP, v, vn, imp); return;
    case S_INSET_INLINE_START: (void)parse_longhand(b, P_LEFT, v, vn, imp); return;
    case S_OVERFLOW_X: return;                                 /* horizontal overflow never hides content here */
    case S_MARGIN_INLINE: case S_MARGIN_BLOCK: case S_PADDING_INLINE: case S_PADDING_BLOCK: {
        tok t[2]; uint32_t nt = split_tokens(v, vn, t, 2, 0);
        if (!nt) return;
        uint16_t a = prop == S_MARGIN_INLINE ? P_MARGIN_LEFT : prop == S_MARGIN_BLOCK ? P_MARGIN_TOP : prop == S_PADDING_INLINE ? P_PADDING_LEFT : P_PADDING_TOP;
        uint16_t z = prop == S_MARGIN_INLINE ? P_MARGIN_RIGHT : prop == S_MARGIN_BLOCK ? P_MARGIN_BOTTOM : prop == S_PADDING_INLINE ? P_PADDING_RIGHT : P_PADDING_BOTTOM;
        (void)parse_longhand(b, a, t[0].s, t[0].n, imp);
        (void)parse_longhand(b, z, t[nt - 1].s, t[nt - 1].n, imp);
        return;
    }
    case S_CLIP_PATH:
        if (vn >= 6 && ieq_n(v, 6, "inset(") && (ieq_n(v, vn, "inset(50%)") || ieq_n(v, vn, "inset(100%)"))) emit(b, P_CLIP, V_NUM, 1, 0, 0, imp);
        return;
    default: (void)parse_longhand(b, prop, v, vn, imp); return;
    }
}

static void add_var(const char *name, size_t nl, const char *val, size_t vl);

/* Parses a declaration block "a: b; c: d !important" into b. */
static void parse_decls(dbuf *b, const char *s, size_t n, int collect_vars) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (is_ws(s[i]) || s[i] == ';')) ++i;
        size_t ns = i;
        while (i < n && s[i] != ':' && s[i] != ';' && s[i] != '{' && s[i] != '}') ++i;
        if (i >= n || s[i] != ':') {
            if (i < n && s[i] == '{') {                             /* nested rule: skip its block */
                int depth = 0;
                while (i < n) { if (s[i] == '{') ++depth; else if (s[i] == '}' && --depth == 0) { ++i; break; } ++i; }
            } else ++i;
            continue;
        }
        size_t ne = i;
        while (ne > ns && is_ws(s[ne - 1])) --ne;
        ++i;
        size_t vs = i;
        int depth = 0; char q = 0;
        while (i < n) {
            char c = s[i];
            if (q) { if (c == '\\') ++i; else if (c == q) q = 0; ++i; continue; }
            if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') ++depth;
            else if ((c == ')' || c == ']') && depth) --depth;
            else if (!depth && (c == ';' || c == '}')) break;
            ++i;
        }
        size_t ve = i;
        while (ve > vs && is_ws(s[ve - 1])) --ve;
        int imp = 0;
        if (ve > vs + 9) {
            size_t k = ve;
            while (k > vs && s[k - 1] != '!') --k;
            if (k > vs && ieq_n(s + k, ve - k, "important")) { imp = 1; ve = k - 1; while (ve > vs && is_ws(s[ve - 1])) --ve; }
        }
        if (ne - ns > 2 && s[ns] == '-' && s[ns + 1] == '-') {         /* custom property */
            if (collect_vars) add_var(s + ns, ne - ns, s + vs, ve - vs);
            continue;
        }
        uint16_t prop = prop_id(s + ns, ne - ns);
        if (!prop) { if (C) ++C->unknown_props; continue; }
        int has_var = 0;
        for (size_t k = vs; k + 4 <= ve; ++k) if (ieq_n(s + k, 4, "var(")) { has_var = 1; break; }
        if (has_var) {
            const char *raw = C ? warena_strndup(&C->arena, s + vs, ve - vs) : 0;
            if (raw) emit(b, prop, V_VAR, 0, 0, raw, imp);
            continue;
        }
        parse_property(b, prop, s + vs, ve - vs, imp);
    }
}

/* ------------------------------------------------------------------ custom properties */
static int collecting_vars;
static void add_var(const char *name, size_t nl, const char *val, size_t vl) {
    if (!C || !collecting_vars) return;
    for (uint32_t i = 0; i < C->nvars; ++i) {
        if (C->vars[i].nlen == nl && !__builtin_memcmp(C->vars[i].name, name, nl)) {
            C->vars[i].val = warena_strndup(&C->arena, val, vl); C->vars[i].vlen = C->vars[i].val ? (uint32_t)vl : 0;
            return;
        }
    }
    if (C->nvars >= C->cap_vars) {
        uint32_t cap = C->cap_vars ? C->cap_vars * 2u : 128u;
        wvar *nv = (wvar *)wmem_realloc(C->vars, cap * sizeof(wvar));
        if (!nv) return;
        C->vars = nv; C->cap_vars = cap;
    }
    wvar *v = &C->vars[C->nvars];
    v->name = warena_strndup(&C->arena, name, nl);
    v->val = warena_strndup(&C->arena, val, vl);
    if (!v->name || !v->val) return;
    v->nlen = (uint16_t)nl; v->vlen = (uint32_t)vl;
    ++C->nvars;
}
static const wvar *find_var(const char *name, size_t nl) {
    for (uint32_t i = C->nvars; i-- > 0;) if (C->vars[i].nlen == nl && !__builtin_memcmp(C->vars[i].name, name, nl)) return &C->vars[i];
    return 0;
}
/* Substitutes var(--x[, fallback]) in s into out; returns length or -1. */
static int64_t substitute(const char *s, size_t n, char *out, size_t cap, int depth) {
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (i + 4 <= n && ieq_n(s + i, 4, "var(")) {
            size_t j = i + 4, d = 1;
            size_t start = j;
            while (j < n && d) { if (s[j] == '(') ++d; else if (s[j] == ')') --d; if (d) ++j; }
            if (j >= n) return -1;
            const char *in = s + start; size_t il = j - start;
            size_t comma = 0; int dd = 0;
            for (; comma < il; ++comma) { if (in[comma] == '(') ++dd; else if (in[comma] == ')') --dd; else if (in[comma] == ',' && !dd) break; }
            size_t nl = comma;
            while (nl && is_ws(in[nl - 1])) --nl;
            size_t ns = 0;
            while (ns < nl && is_ws(in[ns])) ++ns;
            const wvar *v = find_var(in + ns, nl - ns);
            const char *rep; size_t rl;
            if (v) { rep = v->val; rl = v->vlen; }
            else if (comma < il) { rep = in + comma + 1; rl = il - comma - 1; }
            else return -1;
            if (depth < 6) {
                int64_t sub = substitute(rep, rl, out + o, cap - o, depth + 1);
                if (sub < 0) return -1;
                o += (size_t)sub;
            } else return -1;
            i = j + 1;
            continue;
        }
        if (o + 1 >= cap) return -1;
        out[o++] = s[i++];
    }
    return (int64_t)o;
}

/* ------------------------------------------------------------------ selector parsing */
static int parse_compound(const char *s, size_t n, size_t *pos, wcomp *c);

static int parse_nth(const char *s, size_t n, int16_t *a, int16_t *b) {
    while (n && is_ws(*s)) { ++s; --n; }
    while (n && is_ws(s[n - 1])) --n;
    if (ieq_n(s, n, "odd")) { *a = 2; *b = 1; return 1; }
    if (ieq_n(s, n, "even")) { *a = 2; *b = 0; return 1; }
    size_t i = 0; int neg = 0; int64_t av = 0, bv = 0; int has_n = 0;
    for (size_t k = 0; k < n; ++k) if (lc(s[k]) == 'n') has_n = 1;
    if (!has_n) {
        int64_t v; if (parse_num(s, n, &v) != n) return 0;
        *a = 0; *b = (int16_t)(v / 1000); return 1;
    }
    if (s[i] == '-') { neg = 1; ++i; } else if (s[i] == '+') ++i;
    if (lc(s[i]) == 'n') av = 1;
    else { while (i < n && is_digit(s[i])) { av = av * 10 + (s[i] - '0'); ++i; } }
    if (neg) av = -av;
    while (i < n && lc(s[i]) != 'n') ++i;
    ++i;
    while (i < n && is_ws(s[i])) ++i;
    if (i < n) {
        int bneg = s[i] == '-';
        if (s[i] == '+' || s[i] == '-') ++i;
        while (i < n && is_ws(s[i])) ++i;
        while (i < n && is_digit(s[i])) { bv = bv * 10 + (s[i] - '0'); ++i; }
        if (bneg) bv = -bv;
    }
    *a = (int16_t)av; *b = (int16_t)bv;
    return 1;
}

static const char *dup(const char *s, size_t n) { return warena_strndup(&C->arena, s, n); }

/* Parses one compound selector at *pos; returns 0 if it uses unsupported features. */
static int parse_compound(const char *s, size_t n, size_t *pos, wcomp *c) {
    size_t i = *pos;
    const char *cls[8]; uint8_t clen[8]; uint32_t ncls = 0;
    wattrsel at[4]; uint32_t nat = 0;
    wpseudo ps[4]; uint32_t nps = 0;
    int ok = 1, any = 0;
    if (i < n && (s[i] == '*' || is_ident(s[i]))) {
        size_t st = i;
        if (s[i] == '*') ++i; else while (i < n && is_ident(s[i])) ++i;
        if (i < n && s[i] == '|') { ++i; st = i; while (i < n && (is_ident(s[i]) || s[i] == '*')) ++i; }   /* ns|tag */
        if (s[st] != '*') {
            char name[40]; size_t l = 0;
            for (size_t k = st; k < i && l + 1 < sizeof(name); ++k) name[l++] = lc(s[k]);
            name[l] = '\0';
            c->tag = (uint16_t)wtag_lookup(name, l);
            if (c->tag == WT_UNKNOWN) c->name = dup(name, l);
        }
        any = 1;
    }
    while (i < n) {
        char ch = s[i];
        if (ch == '#' || ch == '.') {
            size_t st = ++i;
            while (i < n && (is_ident(s[i]) || s[i] == '\\')) { if (s[i] == '\\' && i + 1 < n) ++i; ++i; }
            if (i == st) return 0;
            /* unescape "\:" etc. */
            char buf[128]; size_t l = 0;
            for (size_t k = st; k < i && l + 1 < sizeof(buf); ++k) { if (s[k] == '\\' && k + 1 < i) ++k; buf[l++] = s[k]; }
            if (ch == '#') c->id = dup(buf, l);
            else if (ncls < 8) { cls[ncls] = dup(buf, l); clen[ncls] = (uint8_t)(l > 255 ? 255 : l); ++ncls; }
            any = 1;
            continue;
        }
        if (ch == '[') {
            size_t st = ++i;
            while (i < n && s[i] != ']') { if (s[i] == '"' || s[i] == '\'') { char q = s[i++]; while (i < n && s[i] != q) ++i; } ++i; }
            if (i >= n) return 0;
            const char *in = s + st; size_t il = i - st;
            ++i;
            if (nat >= 4) { ok = 0; continue; }
            size_t k = 0;
            while (k < il && is_ws(in[k])) ++k;
            size_t ns = k;
            while (k < il && (is_ident(in[k]) || in[k] == ':')) ++k;
            if (k == ns) return 0;
            wattrsel *a = &at[nat];
            wb_set(a, 0, sizeof(*a));
            char nb[64]; size_t nl = 0;
            for (size_t q = ns; q < k && nl + 1 < sizeof(nb); ++q) nb[nl++] = lc(in[q]);
            a->name = dup(nb, nl);
            while (k < il && is_ws(in[k])) ++k;
            if (k < il) {
                char op = in[k];
                if (op == '=') { a->op = AO_EQ; ++k; }
                else if (k + 1 < il && in[k + 1] == '=') {
                    a->op = op == '~' ? AO_WORD : op == '|' ? AO_DASH : op == '^' ? AO_PREFIX : op == '$' ? AO_SUFFIX : op == '*' ? AO_SUBSTR : 255;
                    if (a->op == 255) return 0;
                    k += 2;
                } else return 0;
                while (k < il && is_ws(in[k])) ++k;
                size_t vs, ve;
                if (k < il && (in[k] == '"' || in[k] == '\'')) { char q = in[k++]; vs = k; while (k < il && in[k] != q) ++k; ve = k; ++k; }
                else { vs = k; while (k < il && !is_ws(in[k])) ++k; ve = k; }
                a->val = dup(in + vs, ve - vs);
                while (k < il && is_ws(in[k])) ++k;
                if (k < il && lc(in[k]) == 'i') a->icase = 1;
            }
            ++nat; any = 1;
            continue;
        }
        if (ch == ':') {
            ++i;
            if (i < n && s[i] == ':') return 0;                 /* pseudo-elements generate content we do not render */
            size_t st = i;
            while (i < n && is_ident(s[i])) ++i;
            const char *p = s + st; size_t pl = i - st;
            const char *arg = 0; size_t al = 0;
            if (i < n && s[i] == '(') {
                size_t d = 1, as = ++i;
                while (i < n && d) { if (s[i] == '(') ++d; else if (s[i] == ')') --d; if (d) ++i; }
                arg = s + as; al = i - as;
                ++i;
            }
            if (nps >= 4) return 0;
            wpseudo *q = &ps[nps];
            wb_set(q, 0, sizeof(*q));
            if (ieq_n(p, pl, "before") || ieq_n(p, pl, "after") || ieq_n(p, pl, "first-line") || ieq_n(p, pl, "first-letter")) return 0;
            if (ieq_n(p, pl, "first-child")) q->kind = PS_FIRST_CHILD;
            else if (ieq_n(p, pl, "last-child")) q->kind = PS_LAST_CHILD;
            else if (ieq_n(p, pl, "only-child")) q->kind = PS_ONLY_CHILD;
            else if (ieq_n(p, pl, "first-of-type")) q->kind = PS_FIRST_OF_TYPE;
            else if (ieq_n(p, pl, "last-of-type")) q->kind = PS_LAST_OF_TYPE;
            else if (ieq_n(p, pl, "root")) q->kind = PS_ROOT;
            else if (ieq_n(p, pl, "link") || ieq_n(p, pl, "any-link") || ieq_n(p, pl, "-webkit-any-link")) q->kind = PS_LINK;
            else if (ieq_n(p, pl, "empty")) q->kind = PS_EMPTY;
            else if (ieq_n(p, pl, "checked")) q->kind = PS_CHECKED;
            else if (ieq_n(p, pl, "disabled")) q->kind = PS_DISABLED;
            else if (ieq_n(p, pl, "enabled")) q->kind = PS_ENABLED;
            else if ((ieq_n(p, pl, "nth-child") || ieq_n(p, pl, "nth-last-child") || ieq_n(p, pl, "nth-of-type")) && arg) {
                q->kind = ieq_n(p, pl, "nth-child") ? PS_NTH_CHILD : ieq_n(p, pl, "nth-of-type") ? PS_NTH_OF_TYPE : PS_NTH_LAST_CHILD;
                if (!parse_nth(arg, al, &q->a, &q->b)) return 0;
            } else if ((ieq_n(p, pl, "not") || ieq_n(p, pl, "is") || ieq_n(p, pl, "where") || ieq_n(p, pl, "matches") ||
                        ieq_n(p, pl, "-webkit-any")) && arg) {
                q->kind = ieq_n(p, pl, "not") ? PS_NOT : PS_IS;
                tok parts[8]; uint32_t np = split_tokens(arg, al, parts, 8, ',');
                q->inner = (wcomp *)warena_alloc(&C->arena, np * sizeof(wcomp) + 1);
                if (!q->inner) return 0;
                uint32_t good = 0;
                for (uint32_t k = 0; k < np; ++k) {
                    size_t pp = 0;
                    while (pp < parts[k].n && is_ws(parts[k].s[pp])) ++pp;
                    wcomp tmp; wb_set(&tmp, 0, sizeof(tmp));
                    if (parse_compound(parts[k].s, parts[k].n, &pp, &tmp)) {
                        while (pp < parts[k].n && is_ws(parts[k].s[pp])) ++pp;
                        if (pp == parts[k].n) q->inner[good++] = tmp;       /* simple compound only */
                        else if (q->kind == PS_NOT) return 0;
                    } else if (q->kind == PS_NOT) return 0;
                }
                if (!good && q->kind == PS_IS) return 0;
                q->ninner = (uint8_t)good;
            } else if (ieq_n(p, pl, "hover") || ieq_n(p, pl, "focus") || ieq_n(p, pl, "active") || ieq_n(p, pl, "visited") ||
                       ieq_n(p, pl, "focus-within") || ieq_n(p, pl, "focus-visible") || ieq_n(p, pl, "target") ||
                       ieq_n(p, pl, "placeholder-shown") || ieq_n(p, pl, "invalid") || ieq_n(p, pl, "has") || ieq_n(p, pl, "host") ||
                       ieq_n(p, pl, "defined") || ieq_n(p, pl, "indeterminate") || ieq_n(p, pl, "fullscreen") || ieq_n(p, pl, "modal") ||
                       ieq_n(p, pl, "popover-open") || ieq_n(p, pl, "open")) {
                return 0;                                       /* dynamic states never match */
            } else if (ieq_n(p, pl, "lang") || ieq_n(p, pl, "dir") || ieq_n(p, pl, "required") || ieq_n(p, pl, "optional") ||
                       ieq_n(p, pl, "valid") || ieq_n(p, pl, "read-only") || ieq_n(p, pl, "read-write") || ieq_n(p, pl, "scope")) {
                continue;                                       /* treated as always true */
            } else return 0;
            ++nps; any = 1;
            continue;
        }
        break;
    }
    if (!any || !ok) return 0;
    if (ncls) {
        c->cls = (const char **)warena_alloc(&C->arena, ncls * sizeof(char *));
        c->cls_len = (uint8_t *)warena_alloc(&C->arena, ncls);
        if (!c->cls || !c->cls_len) return 0;
        for (uint32_t k = 0; k < ncls; ++k) { c->cls[k] = cls[k]; c->cls_len[k] = clen[k]; }
        c->ncls = (uint8_t)ncls;
    }
    if (nat) { c->attrs = (wattrsel *)warena_alloc(&C->arena, nat * sizeof(wattrsel)); if (!c->attrs) return 0; for (uint32_t k = 0; k < nat; ++k) c->attrs[k] = at[k]; c->nattr = (uint8_t)nat; }
    if (nps) { c->ps = (wpseudo *)warena_alloc(&C->arena, nps * sizeof(wpseudo)); if (!c->ps) return 0; for (uint32_t k = 0; k < nps; ++k) c->ps[k] = ps[k]; c->nps = (uint8_t)nps; }
    *pos = i;
    return 1;
}

static uint32_t specificity(const wcomp *c, uint32_t n) {
    uint32_t a = 0, b = 0, d = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (c[i].id) ++a;
        b += c[i].ncls + c[i].nattr;
        for (uint32_t k = 0; k < c[i].nps; ++k) {
            if (c[i].ps[k].kind == PS_NOT || c[i].ps[k].kind == PS_IS) {
                /* :is()/:not() take their argument's specificity (first argument as an approximation) */
                if (c[i].ps[k].ninner) { uint32_t s = specificity(c[i].ps[k].inner, 1); a += s >> 20; b += (s >> 10) & 1023; d += s & 1023; }
            } else ++b;
        }
        if (c[i].tag || c[i].name) ++d;
    }
    if (a > 1023) a = 1023;
    if (b > 1023) b = 1023;
    if (d > 1023) d = 1023;
    return (a << 20) | (b << 10) | d;
}

/* Adds a rule for each selector in the list (sel) with the given declarations. */
static void add_rules(const char *sel, size_t sn, uint32_t decl_first, uint32_t ndecl, uint16_t media, uint8_t origin) {
    tok parts[64]; uint32_t np = split_tokens(sel, sn, parts, 64, ',');
    for (uint32_t p = 0; p < np; ++p) {
        wcomp comps[16]; uint32_t nc = 0;
        const char *s = parts[p].s; size_t n = parts[p].n, i = 0;
        uint8_t comb = 0;
        int ok = 1;
        while (i < n && ok) {
            int saw_ws = 0;
            while (i < n && is_ws(s[i])) { ++i; saw_ws = 1; }
            if (i >= n) break;
            if (s[i] == '>' || s[i] == '+' || s[i] == '~') { comb = (uint8_t)s[i]; ++i; while (i < n && is_ws(s[i])) ++i; }
            else if (saw_ws && nc) comb = ' ';
            if (nc >= 16) { ok = 0; break; }
            wcomp *c = &comps[nc];
            wb_set(c, 0, sizeof(*c));
            if (!parse_compound(s, n, &i, c)) { ok = 0; break; }
            c->comb = nc ? (comb ? comb : ' ') : 0;
            comb = 0;
            ++nc;
        }
        if (!ok || !nc) continue;
        if (C->nrules >= C->cap_rules) {
            uint32_t cap = C->cap_rules ? C->cap_rules * 2u : 1024u;
            wrule *nr = (wrule *)wmem_realloc(C->rules, cap * sizeof(wrule));
            if (!nr) { C->oom = 1; return; }
            C->rules = nr; C->cap_rules = cap;
        }
        wrule *r = &C->rules[C->nrules];
        r->comp = (wcomp *)warena_alloc(&C->arena, nc * sizeof(wcomp));
        if (!r->comp) { C->oom = 1; return; }
        for (uint32_t k = 0; k < nc; ++k) r->comp[k] = comps[k];
        r->ncomp = (uint8_t)nc; r->origin = origin; r->media = media;
        r->spec = specificity(comps, nc);
        r->order = C->order++;
        r->decl = decl_first; r->ndecl = ndecl;
        /* bucket by the rightmost compound */
        const wcomp *last = &comps[nc - 1];
        int32_t idx = (int32_t)C->nrules;
        if (last->id) { uint32_t h = hash_cs(last->id, wstrlen(last->id)) % NBUCKET; r->next = C->b_id[h]; C->b_id[h] = idx; }
        else if (last->ncls) { uint32_t h = hash_cs(last->cls[0], last->cls_len[0]) % NBUCKET; r->next = C->b_cls[h]; C->b_cls[h] = idx; }
        else if (last->tag && last->tag != WT_UNKNOWN) { uint32_t h = last->tag % NBUCKET; r->next = C->b_tag[h]; C->b_tag[h] = idx; }
        else { r->next = C->b_any; C->b_any = idx; }
        ++C->nrules;
    }
}

/* ------------------------------------------------------------------ media queries */
static int media_feature(const char *s, size_t n) {
    /* "(min-width: 768px)" etc.; s without the parentheses */
    size_t colon = 0;
    while (colon < n && s[colon] != ':') ++colon;
    const char *name = s; size_t nl = colon;
    while (nl && is_ws(name[nl - 1])) --nl;
    while (nl && is_ws(*name)) { ++name; --nl; }
    const char *val = colon < n ? s + colon + 1 : 0; size_t vl = colon < n ? n - colon - 1 : 0;
    int32_t v = 0; uint8_t u = U_PX;
    if (val) {
        if (!parse_len(val, vl, &v, &u)) {
            if (ieq_n(name, nl, "prefers-color-scheme")) { while (vl && is_ws(*val)) { ++val; --vl; } while (vl && is_ws(val[vl - 1])) --vl; return C->dark ? ieq_n(val, vl, "dark") : ieq_n(val, vl, "light"); }
            if (ieq_n(name, nl, "orientation")) { while (vl && is_ws(*val)) { ++val; --vl; } return ieq_n(val, 9 <= vl ? 9 : vl, "landscape"); }
            if (ieq_n(name, nl, "prefers-reduced-motion") || ieq_n(name, nl, "prefers-contrast")) return 0;
            if (ieq_n(name, nl, "hover") || ieq_n(name, nl, "any-hover")) return 1;
            if (ieq_n(name, nl, "pointer") || ieq_n(name, nl, "any-pointer")) return 1;
            return 0;
        }
        if (u == U_EM || u == U_REM) v *= 16;
        if (u != U_PX && u != U_EM && u != U_REM) { if (!(ieq_n(name, nl, "min-resolution") || ieq_n(name, nl, "max-resolution"))) return 0; }
    }
    int32_t w = C->vw * 64, h = C->vh * 64;
    if (ieq_n(name, nl, "min-width")) return w >= v;
    if (ieq_n(name, nl, "max-width")) return w <= v;
    if (ieq_n(name, nl, "width")) return w == v;
    if (ieq_n(name, nl, "min-height")) return h >= v;
    if (ieq_n(name, nl, "max-height")) return h <= v;
    if (ieq_n(name, nl, "min-device-width")) return w >= v;
    if (ieq_n(name, nl, "max-device-width")) return w <= v;
    if (ieq_n(name, nl, "-webkit-min-device-pixel-ratio") || ieq_n(name, nl, "min-resolution") || ieq_n(name, nl, "min--moz-device-pixel-ratio"))
        return v <= 64 || (u == U_PX && v <= 96 * 64 && v < 2 * 64);
    if (ieq_n(name, nl, "-webkit-max-device-pixel-ratio") || ieq_n(name, nl, "max-resolution")) return 1;
    if (ieq_n(name, nl, "color")) return 1;
    if (ieq_n(name, nl, "hover") || ieq_n(name, nl, "any-hover")) return 1;
    if (ieq_n(name, nl, "grid")) return 0;
    if (ieq_n(name, nl, "monochrome")) return 0;
    return 0;
}

/* Evaluates a media query list ("screen and (min-width: 700px), print"). */
static int media_match(const char *s, size_t n) {
    while (n && is_ws(*s)) { ++s; --n; }
    if (!n) return 1;
    tok qs[16]; uint32_t nq = split_tokens(s, n, qs, 16, ',');
    for (uint32_t q = 0; q < nq; ++q) {
        const char *x = qs[q].s; size_t xn = qs[q].n;
        tok w[24]; uint32_t nw = split_tokens(x, xn, w, 24, 0);
        int neg = 0, ok = 1;
        uint32_t i = 0;
        if (i < nw && ieq_n(w[i].s, w[i].n, "not")) { neg = 1; ++i; }
        if (i < nw && ieq_n(w[i].s, w[i].n, "only")) ++i;
        int is_or = 0;
        for (; i < nw; ++i) {
            if (ieq_n(w[i].s, w[i].n, "and")) continue;
            if (ieq_n(w[i].s, w[i].n, "or")) { is_or = 1; continue; }
            int r;
            if (w[i].s[0] == '(') {
                const char *in = w[i].s + 1; size_t il = w[i].n >= 2 ? w[i].n - 2 : 0;
                /* range syntax (width >= 600px) */
                size_t k = 0;
                while (k < il && in[k] != '<' && in[k] != '>' && in[k] != ':') ++k;
                if (k < il && (in[k] == '<' || in[k] == '>') && !(il > 4 && ieq_n(in, 4, "not "))) {
                    char op = in[k]; int eq = k + 1 < il && in[k + 1] == '=';
                    const char *nm = in; size_t nml = k;
                    while (nml && is_ws(nm[nml - 1])) --nml;
                    while (nml && is_ws(*nm)) { ++nm; --nml; }
                    const char *vv = in + k + 1 + (size_t)eq; size_t vvl = il - k - 1 - (size_t)eq;
                    int32_t v; uint8_t u;
                    int name_first = ieq_n(nm, nml, "width") || ieq_n(nm, nml, "height");
                    if (!name_first) {                          /* 600px <= width */
                        if (!parse_len(nm, nml, &v, &u)) { r = 0; goto done; }
                        op = op == '<' ? '>' : '<';
                        nm = vv; nml = vvl;
                        while (nml && is_ws(*nm)) { ++nm; --nml; }
                        while (nml && is_ws(nm[nml - 1])) --nml;
                    } else if (!parse_len(vv, vvl, &v, &u)) { r = 0; goto done; }
                    if (u == U_EM || u == U_REM) v *= 16;
                    int32_t have = ieq_n(nm, nml, "height") ? C->vh * 64 : C->vw * 64;
                    r = op == '>' ? (eq ? have >= v : have > v) : (eq ? have <= v : have < v);
                } else if (il > 4 && ieq_n(in, 4, "not ")) {
                    r = !media_feature(in + 4, il - 4);
                } else r = media_feature(in, il);
            } else {
                r = ieq_n(w[i].s, w[i].n, "screen") || ieq_n(w[i].s, w[i].n, "all");
            }
        done:
            if (is_or) { ok = ok || r; is_or = 0; } else ok = ok && r;
        }
        if (neg) ok = !ok;
        if (ok) return 1;
    }
    return 0;
}

static uint16_t add_media(const char *s, size_t n) {
    if (C->nmedia >= MAX_MEDIA) return 1;                       /* index 1 is reserved "never" below */
    wmedia *m = &C->media[C->nmedia];
    m->text = dup(s, n); m->len = (uint16_t)(n > 65535 ? 65535 : n);
    m->ok = 0;
    return (uint16_t)C->nmedia++;
}

/* ------------------------------------------------------------------ sheet parsing */
static void parse_sheet(const char *src, size_t n, uint16_t media, uint8_t origin, int depth);

static size_t skip_block(const char *s, size_t n, size_t i) {
    int d = 0; char q = 0;
    while (i < n) {
        char c = s[i];
        if (q) { if (c == '\\') ++i; else if (c == q) q = 0; ++i; continue; }
        if (c == '"' || c == '\'') q = c;
        else if (c == '{') ++d;
        else if (c == '}') { if (--d <= 0) return i + 1; }
        ++i;
    }
    return n;
}

/* Media index whose condition is the conjunction of the parent and child queries. */
static uint16_t nest_media(uint16_t parent, const char *s, size_t n) {
    if (!parent) return add_media(s, n);
    wmedia *p = &C->media[parent];
    char buf[600];
    size_t l = 0;
    /* "parent-query" and "child-query": store both, ANDed with a separator */
    for (size_t i = 0; i < p->len && l + 1 < sizeof(buf); ++i) buf[l++] = p->text[i];
    if (l + 2 < sizeof(buf)) { buf[l++] = '\x01'; }
    for (size_t i = 0; i < n && l + 1 < sizeof(buf); ++i) buf[l++] = s[i];
    return add_media(buf, l);
}

static void parse_sheet(const char *s, size_t n, uint16_t media, uint8_t origin, int depth) {
    size_t i = 0;
    while (i < n && !C->oom) {
        while (i < n && (is_ws(s[i]) || s[i] == ';')) ++i;
        if (i >= n) break;
        if (s[i] == '}') { ++i; continue; }
        if (s[i] == '<' && i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') { i += 4; continue; }   /* <!-- --> */
        if (s[i] == '-' && i + 2 < n && s[i + 1] == '-' && s[i + 2] == '>') { i += 3; continue; }
        if (s[i] == '@') {
            size_t st = ++i;
            while (i < n && is_ident(s[i])) ++i;
            const char *kw = s + st; size_t kl = i - st;
            size_t ps = i;
            while (i < n && s[i] != '{' && s[i] != ';') { if (s[i] == '"' || s[i] == '\'') { char q = s[i++]; while (i < n && s[i] != q) ++i; } ++i; }
            const char *prelude = s + ps; size_t pl = i - ps;
            if (i >= n) break;
            if (s[i] == ';') { ++i; continue; }                     /* @import, @charset, @namespace, @layer a, b; */
            size_t body_start = i + 1, end = skip_block(s, n, i);
            size_t body_end = end > body_start ? end - 1 : body_start;
            if (depth < 8 && ieq_n(kw, kl, "media")) {
                uint16_t m = nest_media(media, prelude, pl);
                parse_sheet(s + body_start, body_end - body_start, m, origin, depth + 1);
            } else if (depth < 8 && (ieq_n(kw, kl, "supports"))) {
                size_t k = 0;
                while (k < pl && is_ws(prelude[k])) ++k;
                int negated = pl - k >= 3 && ieq_n(prelude + k, 3, "not");
                if (!negated) parse_sheet(s + body_start, body_end - body_start, media, origin, depth + 1);
            } else if (depth < 8 && (ieq_n(kw, kl, "layer") || ieq_n(kw, kl, "container") || ieq_n(kw, kl, "scope") ||
                                     ieq_n(kw, kl, "document") || ieq_n(kw, kl, "-moz-document"))) {
                parse_sheet(s + body_start, body_end - body_start, media, origin, depth + 1);
            }
            i = end;
            continue;
        }
        /* selector list up to '{' */
        size_t ss = i;
        while (i < n && s[i] != '{' && s[i] != '}') { if (s[i] == '"' || s[i] == '\'') { char q = s[i++]; while (i < n && s[i] != q) ++i; } ++i; }
        if (i >= n) break;
        if (s[i] == '}') { ++i; continue; }
        size_t se = i;
        size_t end = skip_block(s, n, i);
        const char *body = s + i + 1; size_t bl = end > i + 1 ? end - i - 2 : 0;
        if (end > i + 1 && s[end - 1] != '}') bl = end - i - 1;
        /* custom properties only count from root-level selectors */
        const char *sel = s + ss; size_t sl = se - ss;
        while (sl && is_ws(sel[sl - 1])) --sl;
        int rootish = 0;
        {
            tok parts[16]; uint32_t np = split_tokens(sel, sl, parts, 16, ',');
            for (uint32_t k = 0; k < np; ++k) {
                if (ieq_n(parts[k].s, parts[k].n, ":root") || ieq_n(parts[k].s, parts[k].n, "html") || ieq_n(parts[k].s, parts[k].n, "body") ||
                    ieq_n(parts[k].s, parts[k].n, "*") || ieq_n(parts[k].s, parts[k].n, ":host") || ieq_n(parts[k].s, parts[k].n, ":root,:host"))
                    rootish = 1;
            }
        }
        collecting_vars = rootish && (media == 0 || media_match(C->media[media].text, C->media[media].len));
        dbuf b = {0, 0, 0};
        parse_decls(&b, body, bl, 1);
        collecting_vars = 0;
        if (b.n) {
            /* move into the global declaration array */
            if (C->ndecls + b.n > C->cap_decls) {
                uint32_t cap = C->cap_decls ? C->cap_decls : 4096u;
                while (cap < C->ndecls + b.n) cap *= 2u;
                wdecl *nd = (wdecl *)wmem_realloc(C->decls, cap * sizeof(wdecl));
                if (!nd) { C->oom = 1; wmem_free(b.d); return; }
                C->decls = nd; C->cap_decls = cap;
            }
            uint32_t first = C->ndecls;
            for (uint32_t k = 0; k < b.n; ++k) C->decls[C->ndecls++] = b.d[k];
            add_rules(sel, sl, first, b.n, media, origin);
        }
        wmem_free(b.d);
        i = end;
    }
}

/* Removes comments into a new arena copy. */
static const char *strip_comments(const char *s, size_t n, size_t *out_n) {
    char *o = (char *)warena_alloc(&C->arena, n + 1);
    if (!o) { *out_n = 0; return 0; }
    size_t k = 0;
    char q = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (q) { o[k++] = c; if (c == '\\' && i + 1 < n) o[k++] = s[++i]; else if (c == q || c == '\n') q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; o[k++] = c; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
            ++i;
            o[k++] = ' ';
            continue;
        }
        o[k++] = c;
    }
    o[k] = '\0';
    *out_n = k;
    return o;
}

static const char UA_SHEET[] =
    "html,address,blockquote,body,center,dialog,div,figure,figcaption,footer,form,header,hr,legend,listing,main,p,plaintext,"
    "pre,xmp,section,article,aside,nav,hgroup,details,summary,fieldset,dl,dt,dd,menu,ol,ul,dir,search,optgroup{display:block}"
    "head,script,style,title,meta,link,base,template,noembed,noframes,param,area,datalist,svg,math,audio,video,canvas,iframe,"
    "object,embed,frame,frameset,source,track,rp,map,dialog,input[type=hidden],[hidden],slot,option{display:none}"
    "dialog[open]{display:block}"
    "li{display:list-item}table{display:table;border-spacing:2px;border-collapse:separate;box-sizing:border-box}"
    "tr{display:table-row;vertical-align:inherit}td,th{display:table-cell;padding:1px;vertical-align:middle}"
    "thead,tbody,tfoot{display:table-row-group;vertical-align:middle}caption{display:table-caption;text-align:center}"
    "col,colgroup{display:table-column}"
    "body{margin:8px}p,dl,multicol{margin:1em 0}blockquote,figure{margin:1em 40px}dd{margin-left:40px}"
    "h1{display:block;font-size:2em;margin:.67em 0;font-weight:bold}h2{display:block;font-size:1.5em;margin:.83em 0;font-weight:bold}"
    "h3{display:block;font-size:1.17em;margin:1em 0;font-weight:bold}h4{display:block;margin:1.33em 0;font-weight:bold}"
    "h5{display:block;font-size:.83em;margin:1.67em 0;font-weight:bold}h6{display:block;font-size:.67em;margin:2.33em 0;font-weight:bold}"
    "ul,menu,dir{list-style-type:disc;margin:1em 0;padding-left:40px}ol{list-style-type:decimal;margin:1em 0;padding-left:40px}"
    "ul ul,ol ul,ul menu{list-style-type:circle}ul ul ul,ol ul ul,ul ol ul{list-style-type:square}"
    "ul ul,ul ol,ol ul,ol ol{margin-top:0;margin-bottom:0}"
    "pre,code,kbd,samp,tt,xmp,listing,plaintext{font-family:monospace}pre,xmp,listing,plaintext{white-space:pre;margin:1em 0}"
    "b,strong,th{font-weight:bold}i,em,cite,var,dfn,address{font-style:italic}u,ins{text-decoration:underline}"
    "s,strike,del{text-decoration:line-through}a:link{color:#0066cc;text-decoration:underline}"
    "small{font-size:smaller}big{font-size:larger}sub{vertical-align:sub;font-size:smaller}sup{vertical-align:super;font-size:smaller}"
    "center{text-align:center}th{text-align:center}"
    "hr{border-style:inset;border-width:1px;color:gray;margin:.5em auto;border-color:#c8c8cc}"
    "mark{background-color:yellow;color:black}abbr[title]{text-decoration:underline}"
    "button,input,select,textarea{display:inline-block;font-size:13.33px}"
    "fieldset{margin:0 2px;padding:.35em .75em .625em;border:2px groove #c0c0c0}legend{padding:0 2px}"
    "nobr{white-space:nowrap}wbr{display:inline}q{font-style:normal}"
    "rt{font-size:50%}ruby{display:inline}img{display:inline}"
    "noscript{display:inline}summary{display:block;font-weight:bold}details>*{display:none}details>summary{display:block}";

wcss *wcss_create(wdoc *doc, int32_t viewport_w, int dark) {
    wcss *c = (wcss *)wmem_calloc(sizeof(wcss));
    if (!c) return 0;
    C = c;
    warena_init(&c->arena);
    for (uint32_t i = 0; i < NBUCKET; ++i) c->b_id[i] = c->b_cls[i] = c->b_tag[i] = -1;
    c->b_any = -1;
    c->vw = viewport_w > 0 ? viewport_w : 1024;
    c->vh = c->vw * 10 / 16;
    c->dark = dark;
    c->nmedia = 1;                                     /* media 0 = always */
    c->media[0].ok = 1;
    size_t n;
    const char *ua = strip_comments(UA_SHEET, sizeof(UA_SHEET) - 1u, &n);
    if (ua) parse_sheet(ua, n, 0, 0, 0);
    for (uint32_t i = 0; i < doc->sheet_count; ++i) {
        wsheet_src *sh = &doc->sheets[i];
        if (!sh->text || !sh->len) continue;
        uint16_t media = 0;
        if (sh->media && sh->media[0]) media = add_media(sh->media, wstrlen(sh->media));
        const char *t = strip_comments(sh->text, sh->len, &n);
        if (t) parse_sheet(t, n, media, 1, 0);
        ++c->sheets;
    }
    C = 0;
    return c;
}

void wcss_set_sheet_text(wdoc *doc, uint32_t i, const char *text, size_t len) {
    if (i >= doc->sheet_count) return;
    doc->sheets[i].text = text; doc->sheets[i].len = len;
}

void wcss_free(wcss *c) {
    if (!c) return;
    wmem_free(c->rules); wmem_free(c->decls); wmem_free(c->vars); wmem_free((void *)c->shared);
    warena_free(&c->arena);
    wmem_free(c);
}
uint32_t wcss_rule_count(const wcss *c) { return c ? c->nrules : 0; }

/* ------------------------------------------------------------------ matching */
static int attr_match(const wnode *n, const wattrsel *a) {
    const char *v = wnode_attr(n, a->name);
    if (!v) return 0;
    if (a->op == AO_EXISTS) return 1;
    size_t vl = wstrlen(v), al = wstrlen(a->val);
    #define CEQ(x, y) (a->icase ? lc(x) == lc(y) : (x) == (y))
    switch (a->op) {
    case AO_EQ: if (vl != al) return 0; for (size_t i = 0; i < al; ++i) if (!CEQ(v[i], a->val[i])) return 0; return 1;
    case AO_PREFIX: if (!al || vl < al) return 0; for (size_t i = 0; i < al; ++i) if (!CEQ(v[i], a->val[i])) return 0; return 1;
    case AO_SUFFIX: if (!al || vl < al) return 0; for (size_t i = 0; i < al; ++i) if (!CEQ(v[vl - al + i], a->val[i])) return 0; return 1;
    case AO_SUBSTR:
        if (!al) return 0;
        for (size_t i = 0; i + al <= vl; ++i) { size_t k = 0; while (k < al && CEQ(v[i + k], a->val[k])) ++k; if (k == al) return 1; }
        return 0;
    case AO_DASH:
        if (vl < al) return 0;
        for (size_t i = 0; i < al; ++i) if (!CEQ(v[i], a->val[i])) return 0;
        return vl == al || v[al] == '-';
    case AO_WORD: {
        if (!al) return 0;
        size_t i = 0;
        while (i < vl) {
            while (i < vl && is_ws(v[i])) ++i;
            size_t st = i;
            while (i < vl && !is_ws(v[i])) ++i;
            if (i - st == al) { size_t k = 0; while (k < al && CEQ(v[st + k], a->val[k])) ++k; if (k == al) return 1; }
        }
        return 0;
    }
    default: return 0;
    }
    #undef CEQ
}

static int is_element(const wnode *n) { return n && n->tag >= WT_UNKNOWN; }
static const wnode *prev_elem(const wnode *n) { for (n = n->prev; n; n = n->prev) if (is_element(n)) return n; return 0; }
static const wnode *next_elem(const wnode *n) { for (n = n->next; n; n = n->next) if (is_element(n)) return n; return 0; }
static int same_type(const wnode *a, const wnode *b) { return a->tag == b->tag && (a->tag != WT_UNKNOWN || (a->name && b->name && wstr_ieq(a->name, b->name))); }

static int nth_ok(int a, int b, int idx) {         /* idx is 1-based */
    if (!a) return idx == b;
    int d = idx - b;
    if (d % a) return 0;
    return d / a >= 0;
}

static int match_comp(const wnode *n, const wcomp *c) {
    if (c->tag) {
        if (c->tag != n->tag) return 0;
        if (c->tag == WT_UNKNOWN && (!n->name || !c->name || !wstr_ieq(n->name, c->name))) return 0;
    } else if (c->name) {
        if (n->tag != WT_UNKNOWN || !n->name || !wstr_ieq(n->name, c->name)) return 0;
    }
    if (c->id && (!n->id || !(wstrlen(n->id) == wstrlen(c->id)) || __builtin_memcmp(n->id, c->id, wstrlen(c->id)))) return 0;
    for (uint32_t i = 0; i < c->ncls; ++i) if (!wnode_has_class(n, c->cls[i], c->cls_len[i])) return 0;
    for (uint32_t i = 0; i < c->nattr; ++i) if (!attr_match(n, &c->attrs[i])) return 0;
    for (uint32_t i = 0; i < c->nps; ++i) {
        const wpseudo *p = &c->ps[i];
        switch (p->kind) {
        case PS_FIRST_CHILD: if (prev_elem(n)) return 0; break;
        case PS_LAST_CHILD: if (next_elem(n)) return 0; break;
        case PS_ONLY_CHILD: if (prev_elem(n) || next_elem(n)) return 0; break;
        case PS_FIRST_OF_TYPE: for (const wnode *s = prev_elem(n); s; s = prev_elem(s)) if (same_type(s, n)) return 0; break;
        case PS_LAST_OF_TYPE: for (const wnode *s = next_elem(n); s; s = next_elem(s)) if (same_type(s, n)) return 0; break;
        case PS_NTH_CHILD: case PS_NTH_LAST_CHILD: case PS_NTH_OF_TYPE: {
            int idx = 1;
            if (p->kind == PS_NTH_LAST_CHILD) { for (const wnode *s = next_elem(n); s; s = next_elem(s)) ++idx; }
            else for (const wnode *s = prev_elem(n); s; s = prev_elem(s)) if (p->kind == PS_NTH_CHILD || same_type(s, n)) ++idx;
            if (!nth_ok(p->a, p->b, idx)) return 0;
            break;
        }
        case PS_NOT: for (uint32_t k = 0; k < p->ninner; ++k) if (match_comp(n, &p->inner[k])) return 0; break;
        case PS_IS: { int any = 0; for (uint32_t k = 0; k < p->ninner && !any; ++k) any = match_comp(n, &p->inner[k]); if (!any) return 0; break; }
        case PS_ROOT: if (n->tag != WT_HTML) return 0; break;
        case PS_LINK: if (!(n->tag == WT_A || n->tag == WT_AREA) || !wnode_attr(n, "href")) return 0; break;
        case PS_EMPTY: for (const wnode *k = n->first; k; k = k->next) if (is_element(k) || (k->tag == WT_TEXT && k->text_len)) return 0; break;
        case PS_CHECKED: if (!wnode_attr(n, "checked") && !wnode_attr(n, "selected")) return 0; break;
        case PS_DISABLED: if (!wnode_attr(n, "disabled")) return 0; break;
        case PS_ENABLED: if (wnode_attr(n, "disabled")) return 0; break;
        default: return 0;
        }
    }
    return 1;
}

/* Right-to-left matching of compounds [0..last] ending at n. */
static int match_from(const wnode *n, const wcomp *c, int last) {
    if (!match_comp(n, &c[last])) return 0;
    if (last == 0) return 1;
    uint8_t comb = c[last].comb;
    if (comb == '>') { const wnode *p = n->parent; return is_element(p) && match_from(p, c, last - 1); }
    if (comb == '+') { const wnode *p = prev_elem(n); return p && match_from(p, c, last - 1); }
    if (comb == '~') { for (const wnode *p = prev_elem(n); p; p = prev_elem(p)) if (match_from(p, c, last - 1)) return 1; return 0; }
    for (const wnode *p = n->parent; is_element(p); p = p->parent) if (match_from(p, c, last - 1)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ computing */
typedef struct extras { int32_t left, top; uint8_t left_set, top_set, clip, opacity0; int32_t opacity; } extras;

static int32_t px_of(const wdecl *d, int32_t font_px, int32_t root_px, int32_t vw, int32_t vh) {
    int64_t v = d->n;
    switch (d->unit) {
    case U_EM: return (int32_t)(v * font_px);                 /* v is x64 -> result x64 */
    case U_REM: return (int32_t)(v * root_px);
    case U_VW: return (int32_t)(v * vw / 100);
    case U_VH: return (int32_t)(v * vh / 100);
    default: return (int32_t)v;
    }
}
static wlen to_wlen(const wdecl *d, int32_t font_px, int32_t root_px, int32_t vw, int32_t vh) {
    wlen l;
    if (d->unit == U_AUTO) { l.v = 0; l.u = WU_AUTO; return l; }
    if (d->unit == U_PCT) { l.v = d->n; l.u = WU_PCT; return l; }
    l.v = px_of(d, font_px, root_px, vw, vh); l.u = WU_PX;
    return l;
}

static void initial_noninherited(wstyle *s) {
    s->display = WD_INLINE; s->position = WP_STATIC; s->float_ = WF_NONE; s->clear = WC_NONE;
    s->decoration = 0; s->valign = WV_BASELINE; s->overflow_hidden = 0; s->border_box = 0;
    s->flex_column = 0; s->flex_wrap = 0; s->justify = WJ_START; s->table_fixed = 0; s->centered_auto = 0;
    s->bg = 0; s->bg_image = 0; s->radius = 0; s->gap = 0;
    s->width.u = s->height.u = WU_AUTO; s->width.v = s->height.v = 0;
    s->min_w.u = s->min_h.u = WU_PX; s->min_w.v = s->min_h.v = 0;
    s->max_w.u = s->max_h.u = WU_AUTO; s->max_w.v = s->max_h.v = 0;
    for (int i = 0; i < 4; ++i) {
        s->margin[i].u = WU_PX; s->margin[i].v = 0; s->padding[i].u = WU_PX; s->padding[i].v = 0;
        s->border[i] = 0; s->border_style[i] = WB_NONE; s->border_color[i] = s->color;
    }
}

static int font_kw_px(int k, int parent) {
    switch (k) {
    case K_XX_SMALL: return 9; case K_X_SMALL: return 10; case K_SMALL: return 13; case K_MEDIUM: return 16;
    case K_LARGE: return 18; case K_X_LARGE: return 24; case K_XX_LARGE: return 32;
    case K_SMALLER: return parent * 5 / 6; case K_LARGER: return parent * 6 / 5;
    default: return parent;
    }
}

typedef struct ctx { const wstyle *parent; int32_t root_px; extras *ex; uint32_t border_color_set; } ctx;

static void apply_decl(wstyle *s, const wdecl *d, ctx *x, int font_pass);

static void apply_var(wstyle *s, const wdecl *d, ctx *x, int font_pass) {
    static char buf[1024];
    int64_t l = substitute(d->s, wstrlen(d->s), buf, sizeof(buf), 0);
    if (l < 0) return;
    dbuf b = {0, 0, 0};
    parse_property(&b, d->prop, buf, (size_t)l, d->important);
    for (uint32_t i = 0; i < b.n; ++i) apply_decl(s, &b.d[i], x, font_pass);
    wmem_free(b.d);
}

static void apply_decl(wstyle *s, const wdecl *d, ctx *x, int font_pass) {
    const wstyle *p = x->parent;
    if (d->kind == V_VAR) { apply_var(s, d, x, font_pass); return; }
    int is_font = d->prop == P_FONT_SIZE;
    if (font_pass != is_font) return;
    int32_t fpx = s->font_px, vw = C->vw, vh = C->vh;
    if (d->kind == V_INHERIT || d->kind == V_INITIAL || d->kind == V_UNSET) {
        wstyle init;
        int inherited = d->prop == P_COLOR || d->prop == P_FONT_SIZE || d->prop == P_FONT_WEIGHT || d->prop == P_FONT_STYLE ||
                        d->prop == P_FONT_FAMILY || d->prop == P_LINE_HEIGHT || d->prop == P_TEXT_ALIGN || d->prop == P_WHITE_SPACE ||
                        d->prop == P_TEXT_TRANSFORM || d->prop == P_LIST_STYLE_TYPE || d->prop == P_VISIBILITY || d->prop == P_TEXT_INDENT ||
                        d->prop == P_BORDER_SPACING || d->prop == P_BORDER_COLLAPSE || d->prop == P_LIST_STYLE_POSITION;
        const wstyle *src = p;
        if (d->kind == V_INITIAL || (d->kind == V_UNSET && !inherited)) {
            wb_set(&init, 0, sizeof(init));
            init.color = 0xFF000000u; init.font_px = 16; init.weight = 4;
            initial_noninherited(&init);
            src = &init;
        }
        if (!src) return;
        switch (d->prop) {
        case P_COLOR: s->color = src->color; break;
        case P_BACKGROUND_COLOR: s->bg = src->bg; break;
        case P_FONT_SIZE: s->font_px = src->font_px; break;
        case P_FONT_WEIGHT: s->weight = src->weight; break;
        case P_FONT_STYLE: s->italic = src->italic; break;
        case P_TEXT_ALIGN: s->text_align = src->text_align; break;
        case P_DISPLAY: s->display = src->display; break;
        case P_WIDTH: s->width = src->width; break;
        case P_HEIGHT: s->height = src->height; break;
        case P_MAX_WIDTH: s->max_w = src->max_w; break;
        case P_TEXT_DECORATION_LINE: s->decoration = src->decoration; break;
        case P_WHITE_SPACE: s->white_space = src->white_space; break;
        case P_LINE_HEIGHT: s->line_px = src->line_px; break;
        default:
            if (d->prop >= P_MARGIN_TOP && d->prop <= P_MARGIN_LEFT) s->margin[d->prop - P_MARGIN_TOP] = src->margin[d->prop - P_MARGIN_TOP];
            else if (d->prop >= P_PADDING_TOP && d->prop <= P_PADDING_LEFT) s->padding[d->prop - P_PADDING_TOP] = src->padding[d->prop - P_PADDING_TOP];
            else if (d->prop >= P_BORDER_TOP_WIDTH && d->prop <= P_BORDER_LEFT_WIDTH) s->border[d->prop - P_BORDER_TOP_WIDTH] = src->border[d->prop - P_BORDER_TOP_WIDTH];
            else if (d->prop >= P_BORDER_TOP_STYLE && d->prop <= P_BORDER_LEFT_STYLE) s->border_style[d->prop - P_BORDER_TOP_STYLE] = src->border_style[d->prop - P_BORDER_TOP_STYLE];
            else if (d->prop >= P_BORDER_TOP_COLOR && d->prop <= P_BORDER_LEFT_COLOR) s->border_color[d->prop - P_BORDER_TOP_COLOR] = src->border_color[d->prop - P_BORDER_TOP_COLOR];
            break;
        }
        return;
    }
    switch (d->prop) {
    case P_FONT_SIZE: {
        int32_t parent_px = p ? p->font_px : 16;
        int32_t v;
        if (d->kind == V_KEY) v = font_kw_px(d->n, parent_px);
        else if (d->unit == U_PCT) v = (int32_t)((int64_t)d->n * parent_px / 6400);
        else if (d->unit == U_EM) v = (int32_t)((int64_t)d->n * parent_px / 64);
        else if (d->unit == U_REM) v = (int32_t)((int64_t)d->n * x->root_px / 64);
        else if (d->unit == U_VW) v = (int32_t)((int64_t)d->n * vw / 6400);
        else if (d->unit == U_VH) v = (int32_t)((int64_t)d->n * vh / 6400);
        else v = (d->n + 32) / 64;
        if (v < 0) v = 0;
        if (v > 200) v = 200;
        s->font_px = (int16_t)v;
        break;
    }
    case P_DISPLAY:
        switch (d->n) {
        case K_NONE: s->display = WD_NONE; break;
        case K_INLINE: s->display = WD_INLINE; break;
        case K_BLOCK: case K_FLOW_ROOT: case K_GRID: s->display = WD_BLOCK; break;
        case K_INLINE_BLOCK: case K_INLINE_GRID: s->display = WD_INLINE_BLOCK; break;
        case K_LIST_ITEM: s->display = WD_LIST_ITEM; break;
        case K_TABLE: s->display = WD_TABLE; break;
        case K_INLINE_TABLE: s->display = WD_INLINE_TABLE; break;
        case K_TABLE_ROW: s->display = WD_TABLE_ROW; break;
        case K_TABLE_CELL: s->display = WD_TABLE_CELL; break;
        case K_TABLE_ROW_GROUP: case K_TABLE_HEADER_GROUP: case K_TABLE_FOOTER_GROUP: s->display = WD_TABLE_ROW_GROUP; break;
        case K_TABLE_CAPTION: s->display = WD_TABLE_CAPTION; break;
        case K_TABLE_COLUMN: case K_TABLE_COLUMN_GROUP: s->display = WD_TABLE_COLUMN; break;
        case K_FLEX: s->display = WD_FLEX; break;
        case K_INLINE_FLEX: s->display = WD_INLINE_FLEX; break;
        case K_CONTENTS: s->display = WD_CONTENTS; break;
        case K_RUBY: s->display = WD_INLINE; break;
        default: break;
        }
        break;
    case P_POSITION: s->position = d->n == K_RELATIVE ? WP_RELATIVE : d->n == K_ABSOLUTE ? WP_ABSOLUTE : d->n == K_FIXED ? WP_FIXED : d->n == K_STICKY ? WP_STICKY : WP_STATIC; break;
    case P_FLOAT: s->float_ = d->n == K_LEFT || d->n == K_START ? WF_LEFT : d->n == K_RIGHT || d->n == K_END ? WF_RIGHT : WF_NONE; break;
    case P_CLEAR: s->clear = d->n == K_LEFT ? WC_LEFT : d->n == K_RIGHT ? WC_RIGHT : d->n == K_BOTH ? WC_BOTH : WC_NONE; break;
    case P_WHITE_SPACE: s->white_space = d->n == K_NOWRAP ? WS_NOWRAP : d->n == K_PRE ? WS_PRE : (d->n == K_PRE_WRAP || d->n == K_BREAK_SPACES) ? WS_PRE_WRAP : d->n == K_PRE_LINE ? WS_PRE_LINE : WS_NORMAL; break;
    case P_TEXT_ALIGN: s->text_align = d->n == K_RIGHT || d->n == K_END ? WA_RIGHT : d->n == K_CENTER ? WA_CENTER : d->n == K_JUSTIFY ? WA_JUSTIFY : WA_LEFT; break;
    case P_TEXT_TRANSFORM: s->text_transform = d->n == K_UPPERCASE ? WTT_UPPER : d->n == K_LOWERCASE ? WTT_LOWER : d->n == K_CAPITALIZE ? WTT_CAPITALIZE : WTT_NONE; break;
    case P_LIST_STYLE_TYPE:
        switch (d->n) {
        case K_DISC: s->list_style = WL_DISC; break; case K_CIRCLE: s->list_style = WL_CIRCLE; break;
        case K_SQUARE: s->list_style = WL_SQUARE; break; case K_DECIMAL: case K_DECIMAL_LZ: s->list_style = WL_DECIMAL; break;
        case K_LOWER_ALPHA: case K_LOWER_LATIN: s->list_style = WL_LOWER_ALPHA; break;
        case K_UPPER_ALPHA: case K_UPPER_LATIN: s->list_style = WL_UPPER_ALPHA; break;
        case K_LOWER_ROMAN: s->list_style = WL_LOWER_ROMAN; break; case K_UPPER_ROMAN: s->list_style = WL_UPPER_ROMAN; break;
        case K_NONE: s->list_style = WL_NONE; break;
        default: s->list_style = WL_DISC; break;
        }
        break;
    case P_LIST_STYLE_POSITION: s->list_inside = d->n == K_INSIDE; break;
    case P_FONT_WEIGHT:
        if (d->kind == V_KEY) {
            int pw = p ? p->weight : 4;
            s->weight = (uint8_t)(d->n == K_BOLDER ? (pw < 4 ? 4 : pw < 6 ? 7 : 9) : (pw > 7 ? 7 : pw > 5 ? 4 : 1));
        } else s->weight = (uint8_t)d->n;
        break;
    case P_FONT_STYLE: s->italic = d->n == K_ITALIC || d->n == K_OBLIQUE; break;
    case P_FONT_FAMILY: s->mono = (uint8_t)d->n; break;
    case P_LINE_HEIGHT:
        if (d->kind == V_KEY) s->line_px = 0;
        else if (d->kind == V_NUM) s->line_px = (int16_t)((int64_t)d->n * fpx / 64);
        else if (d->unit == U_PCT) s->line_px = (int16_t)((int64_t)d->n * fpx / 6400);
        else s->line_px = (int16_t)(px_of(d, fpx, x->root_px, vw, vh) / 64);
        if (s->line_px < 0) s->line_px = 0;
        if (s->line_px > 400) s->line_px = 400;
        break;
    case P_TEXT_DECORATION_LINE: s->decoration = (uint8_t)d->n; break;
    case P_VISIBILITY: s->visibility_hidden = d->n == K_HIDDEN || d->n == K_COLLAPSE; break;
    case P_VERTICAL_ALIGN:
        s->valign = d->n == K_TOP ? WV_TOP : d->n == K_MIDDLE ? WV_MIDDLE : d->n == K_BOTTOM ? WV_BOTTOM : d->n == K_SUB ? WV_SUB :
                    d->n == K_SUPER ? WV_SUPER : d->n == K_TEXT_TOP ? WV_TEXT_TOP : d->n == K_TEXT_BOTTOM ? WV_TEXT_BOTTOM : WV_BASELINE;
        break;
    case P_OVERFLOW: s->overflow_hidden = d->n == K_HIDDEN || d->n == K_CLIP || d->n == K_AUTO || d->n == K_SCROLL; break;
    case P_BOX_SIZING: s->border_box = d->n == K_BORDER_BOX; break;
    case P_COLOR:
        if (d->kind == V_COLOR) s->color = (uint32_t)d->n;
        else if (d->kind == V_KEY && p) s->color = p->color;
        if ((s->color >> 24) < 0x40u && p) s->color = p->color;     /* nearly transparent text: keep it readable */
        break;
    case P_BACKGROUND_COLOR: s->bg = d->kind == V_KEY ? s->color : (uint32_t)d->n; break;
    case P_BACKGROUND_IMAGE: s->bg_image = d->kind == V_URL ? d->s : 0; break;
    case P_WIDTH: s->width = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_HEIGHT: s->height = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_MIN_WIDTH: s->min_w = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_MAX_WIDTH: s->max_w = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_MIN_HEIGHT: s->min_h = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_MAX_HEIGHT: s->max_h = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_MARGIN_TOP: case P_MARGIN_RIGHT: case P_MARGIN_BOTTOM: case P_MARGIN_LEFT:
        s->margin[d->prop - P_MARGIN_TOP] = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_PADDING_TOP: case P_PADDING_RIGHT: case P_PADDING_BOTTOM: case P_PADDING_LEFT:
        s->padding[d->prop - P_PADDING_TOP] = to_wlen(d, fpx, x->root_px, vw, vh); break;
    case P_BORDER_TOP_WIDTH: case P_BORDER_RIGHT_WIDTH: case P_BORDER_BOTTOM_WIDTH: case P_BORDER_LEFT_WIDTH: {
        int32_t v = (px_of(d, fpx, x->root_px, vw, vh) + 32) / 64;
        if (d->n > 0 && v == 0) v = 1;                         /* hairlines stay visible */
        s->border[d->prop - P_BORDER_TOP_WIDTH] = (int16_t)(v < 0 ? 0 : v > 50 ? 50 : v);
        break;
    }
    case P_BORDER_TOP_STYLE: case P_BORDER_RIGHT_STYLE: case P_BORDER_BOTTOM_STYLE: case P_BORDER_LEFT_STYLE: {
        uint8_t st = WB_NONE;
        switch (d->n) {
        case K_SOLID: st = WB_SOLID; break; case K_DOTTED: st = WB_DOTTED; break; case K_DASHED: st = WB_DASHED; break;
        case K_DOUBLE: st = WB_DOUBLE; break; case K_INSET: st = WB_INSET; break; case K_OUTSET: st = WB_OUTSET; break;
        case K_GROOVE: st = WB_GROOVE; break; case K_RIDGE: st = WB_RIDGE; break; case K_HIDDEN: st = WB_HIDDEN; break;
        default: st = WB_NONE; break;
        }
        s->border_style[d->prop - P_BORDER_TOP_STYLE] = st;
        break;
    }
    case P_BORDER_TOP_COLOR: case P_BORDER_RIGHT_COLOR: case P_BORDER_BOTTOM_COLOR: case P_BORDER_LEFT_COLOR: {
        int side = d->prop - P_BORDER_TOP_COLOR;
        if (d->kind == V_KEY) { x->border_color_set &= ~(1u << side); }
        else { s->border_color[side] = (uint32_t)d->n; x->border_color_set |= 1u << side; }
        break;
    }
    case P_TEXT_INDENT: s->text_indent = (int16_t)(d->unit == U_PCT ? 0 : px_of(d, fpx, x->root_px, vw, vh) / 64); break;
    case P_BORDER_SPACING: s->border_spacing = (int16_t)(px_of(d, fpx, x->root_px, vw, vh) / 64); break;
    case P_BORDER_COLLAPSE: s->collapse = d->n == K_COLLAPSE; break;
    case P_BORDER_RADIUS: s->radius = (int16_t)(d->unit == U_PCT ? (d->n >= 50 * 64 ? 999 : 0) : px_of(d, fpx, x->root_px, vw, vh) / 64); break;
    case P_FLEX_DIRECTION: s->flex_column = d->n == K_COLUMN || d->n == K_COLUMN_REVERSE; break;
    case P_FLEX_WRAP: s->flex_wrap = d->n == K_WRAP; break;
    case P_JUSTIFY_CONTENT:
        s->justify = d->n == K_CENTER ? WJ_CENTER : (d->n == K_FLEX_END || d->n == K_END || d->n == K_RIGHT) ? WJ_END :
                     d->n == K_SPACE_BETWEEN ? WJ_BETWEEN : (d->n == K_SPACE_AROUND || d->n == K_SPACE_EVENLY) ? WJ_AROUND : WJ_START;
        break;
    case P_GAP: s->gap = (int16_t)(d->unit == U_PCT ? 0 : px_of(d, fpx, x->root_px, vw, vh) / 64); break;
    case P_TABLE_LAYOUT: s->table_fixed = d->n == K_FIXED; break;
    case P_LEFT: x->ex->left = d->unit == U_PCT ? (d->n < -64 * 50 ? -100000 : 0) : px_of(d, fpx, x->root_px, vw, vh) / 64; x->ex->left_set = d->unit != U_AUTO; break;
    case P_TOP: x->ex->top = d->unit == U_PCT ? 0 : px_of(d, fpx, x->root_px, vw, vh) / 64; x->ex->top_set = d->unit != U_AUTO; break;
    case P_CLIP: x->ex->clip = (uint8_t)d->n; break;
    case P_OPACITY: x->ex->opacity0 = d->n <= 0; break;
    default: break;
    }
}

/* HTML presentational attributes, applied as author rules of specificity 0. */
static void presentational(const wnode *n, dbuf *b) {
    const char *v;
    #define EMIT_LEN_ATTR(attr, prop) if ((v = wnode_attr(n, attr)) != 0) { int32_t lv; uint8_t lu; int r = parse_len(v, wstrlen(v), &lv, &lu); if (r) { if (r == 2) lu = U_PX; emit(b, prop, V_LEN, lv, lu, 0, 0); } }
    switch (n->tag) {
    case WT_BODY: case WT_TABLE: case WT_TD: case WT_TH: case WT_TR:
        if ((v = wnode_attr(n, "bgcolor")) != 0) { uint32_t c; if (wcss_parse_color(v, wstrlen(v), &c) || (v[0] != '#' && wcss_parse_color(v, 0, &c))) emit(b, P_BACKGROUND_COLOR, V_COLOR, (int32_t)c, 0, 0, 0);
            else { char h[10] = "#"; size_t l = wstrlen(v); if (l == 6 || l == 3) { wstr_append(h, sizeof(h), v); if (wcss_parse_color(h, l + 1, &c)) emit(b, P_BACKGROUND_COLOR, V_COLOR, (int32_t)c, 0, 0, 0); } } }
        if (n->tag == WT_BODY) {
            if ((v = wnode_attr(n, "text")) != 0) { uint32_t c; if (wcss_parse_color(v, wstrlen(v), &c)) emit(b, P_COLOR, V_COLOR, (int32_t)c, 0, 0, 0); }
            break;
        }
        EMIT_LEN_ATTR("width", P_WIDTH);
        EMIT_LEN_ATTR("height", P_HEIGHT);
        if ((v = wnode_attr(n, "align")) != 0) {
            if (n->tag == WT_TABLE) {
                if (wstr_ieq(v, "center")) { emit(b, P_MARGIN_LEFT, V_LEN, 0, U_AUTO, 0, 0); emit(b, P_MARGIN_RIGHT, V_LEN, 0, U_AUTO, 0, 0); }
                else if (wstr_ieq(v, "right")) emit(b, P_FLOAT, V_KEY, K_RIGHT, 0, 0, 0);
                else if (wstr_ieq(v, "left")) emit(b, P_FLOAT, V_KEY, K_LEFT, 0, 0, 0);
            } else (void)parse_longhand(b, P_TEXT_ALIGN, v, wstrlen(v), 0);
        }
        if ((v = wnode_attr(n, "valign")) != 0) (void)parse_longhand(b, P_VERTICAL_ALIGN, v, wstrlen(v), 0);
        if (wnode_attr(n, "nowrap")) emit(b, P_WHITE_SPACE, V_KEY, K_NOWRAP, 0, 0, 0);
        if (n->tag == WT_TABLE) {
            if ((v = wnode_attr(n, "cellspacing")) != 0) { int32_t lv; uint8_t lu; if (parse_len(v, wstrlen(v), &lv, &lu)) emit(b, P_BORDER_SPACING, V_LEN, lv, U_PX, 0, 0); }
            if ((v = wnode_attr(n, "border")) != 0) {
                int32_t lv = 64; uint8_t lu;
                if (v[0]) (void)parse_len(v, wstrlen(v), &lv, &lu);
                if (lv > 0) for (int i = 0; i < 4; ++i) {
                    emit(b, (uint16_t)(P_BORDER_TOP_WIDTH + i), V_LEN, lv, U_PX, 0, 0);
                    emit(b, (uint16_t)(P_BORDER_TOP_STYLE + i), V_KEY, K_OUTSET, 0, 0, 0);
                    emit(b, (uint16_t)(P_BORDER_TOP_COLOR + i), V_COLOR, (int32_t)0xFF808080u, 0, 0, 0);
                }
            }
        } else if (n->tag == WT_TD || n->tag == WT_TH) {
            const wnode *t = n->parent;
            while (t && t->tag != WT_TABLE) t = t->parent;
            if (t) {
                if ((v = wnode_attr(t, "cellpadding")) != 0) {
                    int32_t lv; uint8_t lu;
                    if (parse_len(v, wstrlen(v), &lv, &lu)) for (int i = 0; i < 4; ++i) emit(b, (uint16_t)(P_PADDING_TOP + i), V_LEN, lv, lu == U_PCT ? U_PCT : U_PX, 0, 0);
                }
                if ((v = wnode_attr(t, "border")) != 0 && !(v[0] == '0' && !v[1])) for (int i = 0; i < 4; ++i) {
                    emit(b, (uint16_t)(P_BORDER_TOP_WIDTH + i), V_LEN, 64, U_PX, 0, 0);
                    emit(b, (uint16_t)(P_BORDER_TOP_STYLE + i), V_KEY, K_INSET, 0, 0, 0);
                    emit(b, (uint16_t)(P_BORDER_TOP_COLOR + i), V_COLOR, (int32_t)0xFFA0A0A0u, 0, 0, 0);
                }
            }
        }
        break;
    case WT_IMG: case WT_IFRAME: case WT_OBJECT: case WT_EMBED: case WT_VIDEO: case WT_CANVAS:
        EMIT_LEN_ATTR("width", P_WIDTH);
        EMIT_LEN_ATTR("height", P_HEIGHT);
        if ((v = wnode_attr(n, "align")) != 0) {
            if (wstr_ieq(v, "left")) emit(b, P_FLOAT, V_KEY, K_LEFT, 0, 0, 0);
            else if (wstr_ieq(v, "right")) emit(b, P_FLOAT, V_KEY, K_RIGHT, 0, 0, 0);
            else if (wstr_ieq(v, "middle") || wstr_ieq(v, "absmiddle")) emit(b, P_VERTICAL_ALIGN, V_KEY, K_MIDDLE, 0, 0, 0);
        }
        if ((v = wnode_attr(n, "hspace")) != 0) { int32_t lv; uint8_t lu; if (parse_len(v, wstrlen(v), &lv, &lu)) { emit(b, P_MARGIN_LEFT, V_LEN, lv, U_PX, 0, 0); emit(b, P_MARGIN_RIGHT, V_LEN, lv, U_PX, 0, 0); } }
        if ((v = wnode_attr(n, "vspace")) != 0) { int32_t lv; uint8_t lu; if (parse_len(v, wstrlen(v), &lv, &lu)) { emit(b, P_MARGIN_TOP, V_LEN, lv, U_PX, 0, 0); emit(b, P_MARGIN_BOTTOM, V_LEN, lv, U_PX, 0, 0); } }
        if ((v = wnode_attr(n, "border")) != 0) { int32_t lv; uint8_t lu; if (parse_len(v, wstrlen(v), &lv, &lu) && lv > 0) for (int i = 0; i < 4; ++i) { emit(b, (uint16_t)(P_BORDER_TOP_WIDTH + i), V_LEN, lv, U_PX, 0, 0); emit(b, (uint16_t)(P_BORDER_TOP_STYLE + i), V_KEY, K_SOLID, 0, 0, 0); } }
        break;
    case WT_FONT:
        if ((v = wnode_attr(n, "color")) != 0) { uint32_t c; if (wcss_parse_color(v, wstrlen(v), &c)) emit(b, P_COLOR, V_COLOR, (int32_t)c, 0, 0, 0); }
        if ((v = wnode_attr(n, "face")) != 0) (void)parse_longhand(b, P_FONT_FAMILY, v, wstrlen(v), 0);
        if ((v = wnode_attr(n, "size")) != 0) {
            static const int sizes[8] = {10, 10, 13, 16, 18, 24, 32, 48};
            int rel = v[0] == '+' || v[0] == '-', sign = v[0] == '-' ? -1 : 1;
            int k = 0; const char *q = v + rel;
            while (*q >= '0' && *q <= '9') { k = k * 10 + (*q - '0'); ++q; }
            if (rel) k = 3 + sign * k;
            if (k < 1) k = 1;
            if (k > 7) k = 7;
            emit(b, P_FONT_SIZE, V_LEN, sizes[k] * 64, U_PX, 0, 0);
        }
        break;
    case WT_HR:
        if ((v = wnode_attr(n, "width")) != 0) { int32_t lv; uint8_t lu; int r = parse_len(v, wstrlen(v), &lv, &lu); if (r) emit(b, P_WIDTH, V_LEN, lv, r == 2 ? U_PX : lu, 0, 0); }
        if ((v = wnode_attr(n, "size")) != 0) { int32_t lv; uint8_t lu; if (parse_len(v, wstrlen(v), &lv, &lu)) { emit(b, P_BORDER_TOP_WIDTH, V_LEN, lv / 2, U_PX, 0, 0); emit(b, P_BORDER_BOTTOM_WIDTH, V_LEN, lv / 2, U_PX, 0, 0); } }
        if ((v = wnode_attr(n, "align")) != 0) {
            if (wstr_ieq(v, "left")) emit(b, P_MARGIN_LEFT, V_LEN, 0, U_PX, 0, 0);
            else if (wstr_ieq(v, "right")) emit(b, P_MARGIN_RIGHT, V_LEN, 0, U_PX, 0, 0);
        }
        break;
    case WT_DIV: case WT_P: case WT_H1: case WT_H2: case WT_H3: case WT_H4: case WT_H5: case WT_H6: case WT_CAPTION: case WT_THEAD: case WT_TBODY: case WT_TFOOT:
        if ((v = wnode_attr(n, "align")) != 0) (void)parse_longhand(b, P_TEXT_ALIGN, v, wstrlen(v), 0);
        if ((v = wnode_attr(n, "valign")) != 0) (void)parse_longhand(b, P_VERTICAL_ALIGN, v, wstrlen(v), 0);
        break;
    case WT_UL: case WT_OL: case WT_LI:
        if ((v = wnode_attr(n, "type")) != 0) {
            int k = wstr_ieq(v, "disc") ? K_DISC : wstr_ieq(v, "circle") ? K_CIRCLE : wstr_ieq(v, "square") ? K_SQUARE :
                    !wstr_ieq(v, "1") ? (v[0] == 'a' ? K_LOWER_ALPHA : v[0] == 'A' ? K_UPPER_ALPHA : v[0] == 'i' ? K_LOWER_ROMAN : v[0] == 'I' ? K_UPPER_ROMAN : 0) : K_DECIMAL;
            if (k) emit(b, P_LIST_STYLE_TYPE, V_KEY, k, 0, 0, 0);
        }
        break;
    case WT_PRE:
        if (wnode_attr(n, "wrap")) emit(b, P_WHITE_SPACE, V_KEY, K_PRE_WRAP, 0, 0, 0);
        break;
    default: break;
    }
    if (wnode_attr(n, "hidden")) emit(b, P_DISPLAY, V_KEY, K_NONE, 0, 0, 0);
    #undef EMIT_LEN_ATTR
}

/* ------------------------------------------------------------------ style sharing */
static uint32_t style_hash(const wstyle *s) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < __builtin_offsetof(wstyle, hash); ++i) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1;
}
static int style_eq(const wstyle *a, const wstyle *b) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (size_t i = 0; i < __builtin_offsetof(wstyle, hash); ++i) if (x[i] != y[i]) return 0;
    return 1;
}
static const wstyle *intern(wdoc *doc, wstyle *s) {
    s->hash = style_hash(s);
    if (C->shared_count * 2u >= C->shared_cap) {
        uint32_t cap = C->shared_cap ? C->shared_cap * 2u : 1024u;
        const wstyle **t = (const wstyle **)wmem_calloc(cap * sizeof(wstyle *));
        if (!t) { C->oom = 1; goto alloc; }
        for (uint32_t i = 0; i < C->shared_cap; ++i) if (C->shared[i]) {
            uint32_t k = C->shared[i]->hash & (cap - 1u);
            while (t[k]) k = (k + 1u) & (cap - 1u);
            t[k] = C->shared[i];
        }
        wmem_free((void *)C->shared);
        C->shared = t; C->shared_cap = cap;
    }
    {
        uint32_t k = s->hash & (C->shared_cap - 1u);
        while (C->shared[k]) {
            if (C->shared[k]->hash == s->hash && style_eq(C->shared[k], s)) return C->shared[k];
            k = (k + 1u) & (C->shared_cap - 1u);
        }
        wstyle *n = (wstyle *)warena_alloc(&doc->arena, sizeof(wstyle));
        if (!n) { C->oom = 1; return 0; }
        *n = *s;
        C->shared[k] = n;
        ++C->shared_count;
        return n;
    }
alloc: {
        wstyle *n = (wstyle *)warena_alloc(&doc->arena, sizeof(wstyle));
        if (n) *n = *s;
        return n;
    }
}

/* ------------------------------------------------------------------ cascade */
typedef struct match { uint32_t key_hi, key_lo; uint32_t rule; } match;

static int match_less(const match *a, const match *b) {
    return a->key_hi < b->key_hi || (a->key_hi == b->key_hi && a->key_lo < b->key_lo);
}

static void consider_chain(const wnode *n, int32_t head, match *out, uint32_t *count, uint32_t cap, int kind, const char *key, size_t kl) {
    for (int32_t r = head; r >= 0; r = C->rules[r].next) {
        const wrule *rule = &C->rules[r];
        const wcomp *last = &rule->comp[rule->ncomp - 1];
        if (kind == 1 && (!last->id || wstrlen(last->id) != kl || __builtin_memcmp(last->id, key, kl))) continue;
        if (kind == 2 && (last->cls_len[0] != kl || __builtin_memcmp(last->cls[0], key, kl))) continue;
        if (kind == 3 && last->tag != n->tag) continue;
        if (!C->media[rule->media].ok) continue;
        if (!match_from(n, rule->comp, rule->ncomp - 1)) continue;
        if (*count < cap) {
            match *m = &out[(*count)++];
            m->key_hi = ((uint32_t)rule->origin << 30) | (rule->spec >> 2);
            m->key_lo = ((rule->spec & 3u) << 30) | (rule->order & 0x3FFFFFFFu);
            m->rule = (uint32_t)r;
        }
    }
}

int wcss_apply(wcss *c, wdoc *doc) {
    if (!c) return -1;
    C = c;
    /* evaluate media queries for this viewport */
    for (uint32_t i = 1; i < c->nmedia; ++i) {
        wmedia *m = &c->media[i];
        int ok = 1;
        size_t st = 0;
        for (size_t k = 0; k <= m->len; ++k) {
            if (k == m->len || m->text[k] == '\x01') { ok = ok && media_match(m->text + st, k - st); st = k + 1; }
        }
        m->ok = (uint8_t)ok;
        if (!ok) ++c->media_skipped;
    }
    static match matched[1024];
    wstyle root_parent;
    wb_set(&root_parent, 0, sizeof(root_parent));
    root_parent.color = 0xFF000000u; root_parent.font_px = 16; root_parent.weight = 4;
    initial_noninherited(&root_parent);
    int32_t root_px = 16;
    wnode *cur = doc->root ? doc->root->first : 0;
    while (cur) {
        if (cur->tag >= WT_UNKNOWN) {
            const wstyle *parent = cur->parent && cur->parent->style ? cur->parent->style : &root_parent;
            wstyle s;
            wb_set(&s, 0, sizeof(s));
            /* inherited */
            s.color = parent->color; s.font_px = parent->font_px; s.line_px = parent->line_px; s.weight = parent->weight;
            s.italic = parent->italic; s.mono = parent->mono; s.text_align = parent->text_align; s.white_space = parent->white_space;
            s.text_transform = parent->text_transform; s.list_style = parent->list_style; s.list_inside = parent->list_inside;
            s.visibility_hidden = parent->visibility_hidden; s.text_indent = parent->text_indent; s.border_spacing = parent->border_spacing;
            s.collapse = parent->collapse; s.hide_text = parent->hide_text;
            if (parent->line_px && parent->font_px) s.line_px = parent->line_px;
            initial_noninherited(&s);
            uint32_t count = 0;
            const uint32_t cap = sizeof(matched) / sizeof(matched[0]);
            if (cur->id) { size_t l = wstrlen(cur->id); consider_chain(cur, c->b_id[hash_cs(cur->id, l) % NBUCKET], matched, &count, cap, 1, cur->id, l); }
            if (cur->cls) {
                const char *p = cur->cls;
                while (*p) {
                    while (*p && is_ws(*p)) ++p;
                    const char *st = p;
                    while (*p && !is_ws(*p)) ++p;
                    if (p > st) {
                        /* skip duplicate class names in the attribute */
                        int dupl = 0;
                        for (const char *q = cur->cls; q < st && !dupl;) {
                            while (q < st && is_ws(*q)) ++q;
                            const char *qs = q;
                            while (q < st && !is_ws(*q)) ++q;
                            if (q - qs == p - st && !__builtin_memcmp(qs, st, (size_t)(p - st))) dupl = 1;
                        }
                        if (!dupl) consider_chain(cur, c->b_cls[hash_cs(st, (size_t)(p - st)) % NBUCKET], matched, &count, cap, 2, st, (size_t)(p - st));
                    }
                }
            }
            if (cur->tag != WT_UNKNOWN) consider_chain(cur, c->b_tag[cur->tag % NBUCKET], matched, &count, cap, 3, 0, 0);
            consider_chain(cur, c->b_any, matched, &count, cap, 0, 0, 0);
            for (uint32_t i = 1; i < count; ++i) {                 /* insertion sort: origin, specificity, order */
                match m = matched[i]; uint32_t j = i;
                while (j && match_less(&m, &matched[j - 1])) { matched[j] = matched[j - 1]; --j; }
                matched[j] = m;
            }
            /* presentational hints and the style attribute */
            dbuf pres = {0, 0, 0}, inl = {0, 0, 0};
            presentational(cur, &pres);
            const char *style_attr = wnode_attr(cur, "style");
            if (style_attr) parse_decls(&inl, style_attr, wstrlen(style_attr), 0);
            extras ex; wb_set(&ex, 0, sizeof(ex));
            ctx x = {parent, root_px, &ex, 0};
            for (int pass = 1; pass >= 0; --pass) {                 /* font-size first, then the rest */
                /* UA rules, presentational hints, author rules (normal), inline (normal), important */
                uint32_t i = 0;
                for (; i < count && C->rules[matched[i].rule].origin == 0; ++i) {
                    const wrule *r = &C->rules[matched[i].rule];
                    for (uint32_t k = 0; k < r->ndecl; ++k) if (!C->decls[r->decl + k].important) apply_decl(&s, &C->decls[r->decl + k], &x, pass);
                }
                for (uint32_t k = 0; k < pres.n; ++k) apply_decl(&s, &pres.d[k], &x, pass);
                for (uint32_t j = i; j < count; ++j) {
                    const wrule *r = &C->rules[matched[j].rule];
                    for (uint32_t k = 0; k < r->ndecl; ++k) if (!C->decls[r->decl + k].important) apply_decl(&s, &C->decls[r->decl + k], &x, pass);
                }
                for (uint32_t k = 0; k < inl.n; ++k) if (!inl.d[k].important) apply_decl(&s, &inl.d[k], &x, pass);
                for (uint32_t j = i; j < count; ++j) {
                    const wrule *r = &C->rules[matched[j].rule];
                    for (uint32_t k = 0; k < r->ndecl; ++k) if (C->decls[r->decl + k].important) apply_decl(&s, &C->decls[r->decl + k], &x, pass);
                }
                for (uint32_t k = 0; k < inl.n; ++k) if (inl.d[k].important) apply_decl(&s, &inl.d[k], &x, pass);
                if (pass == 1 && cur->tag == WT_HTML) { root_px = s.font_px ? s.font_px : 16; x.root_px = root_px; }
            }
            wmem_free(pres.d); wmem_free(inl.d);
            for (int side = 0; side < 4; ++side) if (!(x.border_color_set & (1u << side))) s.border_color[side] = s.color;
            for (int side = 0; side < 4; ++side) if (s.border_style[side] == WB_NONE || s.border_style[side] == WB_HIDDEN) s.border[side] = 0;
            /* hidden-content heuristics: off-screen and visually hidden elements, image replacement */
            if (s.position == WP_ABSOLUTE || s.position == WP_FIXED) {
                int tiny = (s.width.u == WU_PX && s.width.v <= 64) || (s.height.u == WU_PX && s.height.v <= 64);
                if ((ex.left_set && ex.left < -999) || (ex.top_set && ex.top < -999) || ex.clip || (tiny && s.overflow_hidden) || ex.opacity0)
                    s.display = WD_NONE;
            }
            if (ex.clip && s.overflow_hidden) s.display = WD_NONE;
            if (s.text_indent < -999) { s.hide_text = 1; s.text_indent = 0; }
            if (s.font_px == 0) s.hide_text = 1;
            if (s.overflow_hidden && ((s.height.u == WU_PX && s.height.v == 0) || (s.max_h.u == WU_PX && s.max_h.v == 0))) s.display = WD_NONE;
            if (cur->tag == WT_HTML) root_px = s.font_px ? s.font_px : 16;
            if (s.margin[1].u == WU_AUTO && s.margin[3].u == WU_AUTO) s.centered_auto = 1;
            /* floats and absolutely positioned boxes are blockified */
            if ((s.float_ != WF_NONE || s.position == WP_ABSOLUTE || s.position == WP_FIXED) &&
                (s.display == WD_INLINE || s.display == WD_INLINE_BLOCK)) s.display = WD_BLOCK;
            if (s.position == WP_ABSOLUTE || s.position == WP_FIXED) s.float_ = WF_NONE;
            cur->style = intern(doc, &s);
            if (!cur->style) { C = 0; return -1; }
            if (s.display != WD_NONE && cur->first) { cur = cur->first; continue; }
        }
        while (cur && !cur->next) cur = cur->parent;
        if (cur) cur = cur->next;
    }
    C = 0;
    return c->oom ? -1 : 0;
}

void wcss_get_stats(const wcss *c, wcss_stats *out) {
    wb_set(out, 0, sizeof(*out));
    if (!c) return;
    out->rules = c->nrules; out->decls = c->ndecls; out->sheets = c->sheets;
    out->distinct_styles = c->shared_count; out->media_skipped = c->media_skipped; out->unknown_props = c->unknown_props;
}
