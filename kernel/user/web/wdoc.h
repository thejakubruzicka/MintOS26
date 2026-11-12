#ifndef MINT_WEB_WDOC_H
#define MINT_WEB_WDOC_H
/* Document model shared by the MintOS Web engine stages:
 *   whtml.c   HTML bytes -> DOM tree (HTML5-style recovery, no scripts)
 *   wcss.c    style sheets + style attributes -> computed style per element
 *   wlayout.c DOM + styles -> display list (block/inline/float/table/list layout)
 * Everything lives in the document's arena and is bounded. Integer only. */
#include <stddef.h>
#include <stdint.h>
#include "wmem.h"

/* ------------------------------------------------------------------ tags */
#define WTAG_LIST(X) \
    X(A,"a") X(ABBR,"abbr") X(ACRONYM,"acronym") X(ADDRESS,"address") X(AREA,"area") X(ARTICLE,"article") X(ASIDE,"aside") \
    X(AUDIO,"audio") X(B,"b") X(BASE,"base") X(BDI,"bdi") X(BDO,"bdo") X(BIG,"big") X(BLOCKQUOTE,"blockquote") X(BODY,"body") \
    X(BR,"br") X(BUTTON,"button") X(CANVAS,"canvas") X(CAPTION,"caption") X(CENTER,"center") X(CITE,"cite") X(CODE,"code") \
    X(COL,"col") X(COLGROUP,"colgroup") X(DATA,"data") X(DATALIST,"datalist") X(DD,"dd") X(DEL,"del") X(DETAILS,"details") \
    X(DFN,"dfn") X(DIALOG,"dialog") X(DIR,"dir") X(DIV,"div") X(DL,"dl") X(DT,"dt") X(EM,"em") X(EMBED,"embed") \
    X(FIELDSET,"fieldset") X(FIGCAPTION,"figcaption") X(FIGURE,"figure") X(FONT,"font") X(FOOTER,"footer") X(FORM,"form") \
    X(FRAME,"frame") X(FRAMESET,"frameset") X(H1,"h1") X(H2,"h2") X(H3,"h3") X(H4,"h4") X(H5,"h5") X(H6,"h6") X(HEAD,"head") \
    X(HEADER,"header") X(HGROUP,"hgroup") X(HR,"hr") X(HTML,"html") X(I,"i") X(IFRAME,"iframe") X(IMG,"img") X(INPUT,"input") \
    X(INS,"ins") X(KBD,"kbd") X(LABEL,"label") X(LEGEND,"legend") X(LI,"li") X(LINK,"link") X(LISTING,"listing") X(MAIN,"main") \
    X(MAP,"map") X(MARK,"mark") X(MATH,"math") X(MENU,"menu") X(META,"meta") X(METER,"meter") X(NAV,"nav") X(NOBR,"nobr") \
    X(NOEMBED,"noembed") X(NOFRAMES,"noframes") X(NOSCRIPT,"noscript") X(OBJECT,"object") X(OL,"ol") X(OPTGROUP,"optgroup") \
    X(OPTION,"option") X(OUTPUT,"output") X(P,"p") X(PARAM,"param") X(PICTURE,"picture") X(PLAINTEXT,"plaintext") X(PRE,"pre") \
    X(PROGRESS,"progress") X(Q,"q") X(RP,"rp") X(RT,"rt") X(RUBY,"ruby") X(S,"s") X(SAMP,"samp") X(SCRIPT,"script") \
    X(SEARCH,"search") X(SECTION,"section") X(SELECT,"select") X(SLOT,"slot") X(SMALL,"small") X(SOURCE,"source") X(SPAN,"span") \
    X(STRIKE,"strike") X(STRONG,"strong") X(STYLE,"style") X(SUB,"sub") X(SUMMARY,"summary") X(SUP,"sup") X(SVG,"svg") \
    X(TABLE,"table") X(TBODY,"tbody") X(TD,"td") X(TEMPLATE,"template") X(TEXTAREA,"textarea") X(TFOOT,"tfoot") X(TH,"th") \
    X(THEAD,"thead") X(TIME,"time") X(TITLE,"title") X(TR,"tr") X(TRACK,"track") X(TT,"tt") X(U,"u") X(UL,"ul") X(VAR,"var") \
    X(VIDEO,"video") X(WBR,"wbr") X(XMP,"xmp")

enum {
    WT_NONE = 0, WT_TEXT, WT_DOCUMENT, WT_UNKNOWN,
#define WT_ENUM(id, s) WT_##id,
    WTAG_LIST(WT_ENUM)
#undef WT_ENUM
    WT_COUNT
};
const char *wtag_name(uint32_t tag);
uint32_t wtag_lookup(const char *name, size_t len);       /* lower-case name -> WT_*, WT_UNKNOWN */

/* ------------------------------------------------------------------ styles */
enum { WU_PX = 0, WU_PCT, WU_AUTO, WU_NONE };              /* lengths: v is 1/64 px or 1/64 percent */
typedef struct wlen { int32_t v; uint8_t u; } wlen;

enum { WD_NONE = 0, WD_INLINE, WD_BLOCK, WD_INLINE_BLOCK, WD_LIST_ITEM, WD_TABLE, WD_INLINE_TABLE, WD_TABLE_ROW,
       WD_TABLE_CELL, WD_TABLE_ROW_GROUP, WD_TABLE_CAPTION, WD_TABLE_COLUMN, WD_FLEX, WD_INLINE_FLEX, WD_CONTENTS };
enum { WP_STATIC = 0, WP_RELATIVE, WP_ABSOLUTE, WP_FIXED, WP_STICKY };
enum { WF_NONE = 0, WF_LEFT, WF_RIGHT };
enum { WC_NONE = 0, WC_LEFT, WC_RIGHT, WC_BOTH };
enum { WS_NORMAL = 0, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum { WA_LEFT = 0, WA_RIGHT, WA_CENTER, WA_JUSTIFY };
enum { WTT_NONE = 0, WTT_UPPER, WTT_LOWER, WTT_CAPITALIZE };
enum { WL_DISC = 0, WL_CIRCLE, WL_SQUARE, WL_DECIMAL, WL_LOWER_ALPHA, WL_UPPER_ALPHA, WL_LOWER_ROMAN, WL_UPPER_ROMAN, WL_NONE };
enum { WV_BASELINE = 0, WV_TOP, WV_MIDDLE, WV_BOTTOM, WV_SUB, WV_SUPER, WV_TEXT_TOP, WV_TEXT_BOTTOM };
enum { WDEC_UNDERLINE = 1, WDEC_STRIKE = 2, WDEC_OVERLINE = 4 };
enum { WB_NONE = 0, WB_SOLID, WB_DOTTED, WB_DASHED, WB_DOUBLE, WB_INSET, WB_OUTSET, WB_GROOVE, WB_RIDGE, WB_HIDDEN };
enum { WJ_START = 0, WJ_CENTER, WJ_END, WJ_BETWEEN, WJ_AROUND };

typedef struct wstyle {
    uint8_t display, position, float_, clear, white_space, text_align, text_transform, list_style;
    uint8_t weight;                /* 1..9 (400 = 4, 700 = 7) */
    uint8_t italic, mono, decoration, visibility_hidden, valign, overflow_hidden, border_box;
    uint8_t flex_column, flex_wrap, justify, hide_text, list_inside, collapse, table_fixed, centered_auto, grow, pad0[3];
    int16_t font_px, line_px;      /* line_px 0 = normal */
    int16_t text_indent, border_spacing, radius, gap;
    uint32_t color, bg;            /* ARGB; bg alpha 0 = transparent */
    wlen width, height, min_w, max_w, min_h, max_h;
    wlen margin[4], padding[4];    /* top right bottom left */
    int16_t border[4];             /* px */
    uint32_t border_color[4];
    uint8_t border_style[4];
    const char *bg_image;          /* url(...) of background-image, NULL if none */
    uint32_t hash;
} wstyle;

/* ------------------------------------------------------------------ DOM */
typedef struct wattr { const char *name, *value; } wattr;

typedef struct wnode wnode;
struct wnode {
    uint16_t tag, nattr;
    uint32_t order;                /* document order */
    wnode *parent, *first, *last, *next, *prev;
    const char *name;              /* lower-case tag name (WT_UNKNOWN elements) */
    wattr *attrs;
    const char *text; uint32_t text_len;   /* WT_TEXT: decoded UTF-8, whitespace preserved */
    const char *id, *cls;
    const wstyle *style;           /* computed (elements only) */
    int32_t ctl, img, link;        /* indexes into the document tables, -1 = none */
    int32_t y;                     /* layout: top of the box (fragment scrolling) */
    int32_t min_w, max_w;          /* intrinsic width cache */
    uint32_t iw_gen;
};

/* Form controls */
enum { WCT_TEXT = 1, WCT_PASSWORD, WCT_SEARCH, WCT_HIDDEN, WCT_CHECKBOX, WCT_RADIO, WCT_SUBMIT, WCT_RESET, WCT_BUTTON,
       WCT_IMAGE, WCT_TEXTAREA, WCT_SELECT, WCT_BUTTON_EL, WCT_FILE, WCT_OTHER };
typedef struct wctl {
    wnode *node, *form;
    uint8_t type, checked, disabled, readonly, focus_ok;
    const char *name, *placeholder, *label;   /* label: button text */
    char *value; uint32_t len, cap;           /* editable value (UTF-8) */
    uint32_t caret;
    int32_t selected, nopts, size, rows;
    wnode **opts;
    int32_t x, y, w, h;                       /* last layout position (document coordinates) */
} wctl;

enum { WSHEET_INLINE = 1, WSHEET_LINK = 2 };
typedef struct wsheet_src {
    uint8_t kind;
    const char *text; size_t len;  /* inline <style> text, or fetched text */
    const char *href;              /* <link rel=stylesheet href> (raw) */
    const char *media;
} wsheet_src;

#define WDOC_MAX_SHEETS 48u
typedef struct wdoc {
    warena arena;
    wnode *root, *html, *head, *body;
    uint32_t node_count, elem_count;
    char title[200];
    const char *base_href;
    const char *refresh;                       /* <meta http-equiv=refresh content> */
    wsheet_src sheets[WDOC_MAX_SHEETS]; uint32_t sheet_count;
    const char **links; uint32_t link_count, link_cap;
    wnode **imgs; uint32_t img_count, img_cap;
    wctl *ctls; uint32_t ctl_count, ctl_cap;
    int truncated;                             /* ran out of nodes or memory while parsing */
    int is_plain_text;
    int has_doctype;                           /* no <!DOCTYPE>: quirks mode */
} wdoc;

/* ------------------------------------------------------------------ HTML */
/* Parses UTF-8 HTML (or plain text when as_text) into a new document.
 * Returns 0 on success (doc->truncated set if limits were hit), -1 on OOM. */
int whtml_parse(wdoc *doc, const char *html, size_t len, int as_text);
void wdoc_free(wdoc *doc);
const char *wnode_attr(const wnode *n, const char *name);
int wnode_has_class(const wnode *n, const char *cls, size_t len);
/* Text content of an element (whitespace collapsed) into out. */
size_t wnode_text(const wnode *n, char *out, size_t cap);
wnode *wdoc_find_id(const wdoc *doc, const char *id);
/* Converts a legacy charset (windows-1252, iso-8859-1/2, windows-1250) to
 * UTF-8 and repairs invalid UTF-8; returns a new wmem buffer. */
char *wcharset_to_utf8(const uint8_t *in, size_t len, const char *charset, size_t *out_len);
/* Charset from a BOM or <meta charset> / http-equiv in the first 1 KiB ("" if none). */
void whtml_sniff_charset(const uint8_t *in, size_t len, char *out, size_t cap);
/* Decodes one entity at s (after '&'): returns bytes consumed or 0. */
size_t whtml_entity(const char *s, size_t n, uint32_t *cp, uint32_t *cp2);
size_t wutf8_put(char *out, uint32_t cp);    /* writes 1-4 bytes */
uint32_t wutf8_next(const char **p, const char *end);

/* ------------------------------------------------------------------ CSS */
typedef struct wcss wcss;
/* Builds a cascade context for a document: the user-agent sheet plus every
 * author sheet in doc->sheets (in order). viewport_w drives @media. */
wcss *wcss_create(wdoc *doc, int32_t viewport_w, int dark);
/* Adds (or replaces) the text of sheet i after it was fetched. */
void wcss_set_sheet_text(wdoc *doc, uint32_t i, const char *text, size_t len);
/* Computes styles for every element; returns 0 or -1 on OOM. */
int wcss_apply(wcss *c, wdoc *doc);
void wcss_free(wcss *c);
uint32_t wcss_rule_count(const wcss *c);
/* Parses "#rgb", "rgb()", names... into ARGB; 1 on success. */
int wcss_parse_color(const char *s, size_t n, uint32_t *argb);
/* Stats for tests/diagnostics. */
typedef struct wcss_stats { uint32_t rules, decls, sheets, distinct_styles, media_skipped, unknown_props; } wcss_stats;
void wcss_get_stats(const wcss *c, wcss_stats *out);
#endif
