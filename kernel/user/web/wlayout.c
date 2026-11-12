/* Web engine layout — see wlayout.h. */
#include "wlayout.h"
#include "whttp.h"
#include "../mintde/font.h"

#define MAX_DEPTH 64
#define MAX_FLOATS 256u
#define MAX_COLS 100u
#define MAX_DEFER 8

/* ------------------------------------------------------------------ fonts */
static const MINT_GLYPH *glyph(const MINT_FONT *f, uint32_t cp) {
    if (cp >= 32u && cp < 127u && f->glyph_count > cp - 32u && f->glyphs[cp - 32u].codepoint == cp) return &f->glyphs[cp - 32u];
    int32_t lo = 0, hi = (int32_t)f->glyph_count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (f->glyphs[mid].codepoint == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].codepoint < cp) lo = mid + 1; else hi = mid - 1;
    }
    return cp == '?' ? 0 : glyph(f, '?');
}

int32_t wfont_ascent(int font) { return font >= 0 && font < MINT_FONT_COUNT ? mint_fonts[font].ascent : 10; }
int32_t wfont_line(int font) { return font >= 0 && font < MINT_FONT_COUNT ? mint_fonts[font].line : 14; }
int32_t wfont_mono_cell(int font) { return font == FONT_SMALL ? 7 : font == FONT_CAPTION ? 7 : font == FONT_BODY ? 8 : (wfont_line(font) * 9 + 8) / 16; }

int32_t wtext_width(int font, const char *s, size_t n) {
    if (font < 0 || font >= MINT_FONT_COUNT) return 0;
    const MINT_FONT *f = &mint_fonts[font];
    const char *p = s, *end = s + n;
    int32_t pen = 0;
    while (p < end) {
        uint32_t cp = wutf8_next(&p, end);
        if (cp == 0xA0u) cp = ' ';
        const MINT_GLYPH *g = glyph(f, cp);
        if (g) pen += g->advance64;
    }
    return (pen + 32) >> 6;
}

int wfont_pick(int px, int weight, int italic, int mono, int nonascii) {
    (void)italic;
    if (mono) return px <= 14 ? FONT_SMALL : FONT_BODY;
    if (weight >= 6) {
        if (px <= 11) return FONT_CAPTION_BOLD;
        if (px <= 14) return FONT_BODY_BOLD;
        if (px <= 18) return FONT_HEADLINE;
        if (px <= 26) return FONT_TITLE;
        return nonascii ? FONT_TITLE : FONT_HUGE;
    }
    if (weight == 5 && px >= 12 && px <= 16) return FONT_BODY_MEDIUM;
    if (px <= 11) return FONT_CAPTION;
    if (px == 12) return FONT_SMALL;
    if (px <= 18) return FONT_BODY;
    if (px <= 26) return nonascii ? FONT_HEADLINE : FONT_LARGE;
    return nonascii ? FONT_TITLE : FONT_HUGE;
}

/* ------------------------------------------------------------------ state */
enum { F_TEXT = 1, F_SPACE, F_ATOM, F_GAP, F_MARKER };
typedef struct frag {
    uint8_t kind, font, flags, valign;
    const wstyle *st;
    const char *text; uint32_t len;
    int32_t x, w, asc, desc, dy;
    int32_t link;
    uint32_t first, count;         /* atom items */
    int32_t ah;                    /* atom height */
    uint32_t color, bg;
} frag;

typedef struct flt { int32_t x0, y0, x1, y1; uint8_t right; } flt;

typedef struct ifc {
    int32_t x0, x1, y;
    const wstyle *st;
    uint32_t base;
    int32_t lx, ll, lr;
    int open, first, space;
    const wstyle *space_st;
    int32_t space_link;
    int32_t pm, pmn;               /* pending collapsed margin: positive max, negative min */
    uint8_t align, flex, any_content;
    int32_t gap;
    wnode *deferred[MAX_DEFER]; int ndef;
} ifc;

typedef struct lctx {
    wdoc *doc; const wlayout_env *env; wlayout_out *o;
    frag *frags; uint32_t nfrags, fcap;
    flt floats[MAX_FLOATS]; uint32_t nfloats, bfc;
    ifc ifcs[MAX_DEPTH + 4]; int nifc;
    int depth;
    int32_t link;
    uint8_t deco, invisible;
    struct { int on; const wstyle *st; int32_t right; char text[24]; uint8_t bullet; } marker;
    uint32_t gen;
    warena scratch;
    uint32_t row_bg;
} lctx;
static lctx *L;

/* ------------------------------------------------------------------ output */
static uint32_t item_new(uint8_t kind) {
    wlayout_out *o = L->o;
    if (o->count >= o->cap) {
        uint32_t cap = o->cap ? o->cap * 2u : 4096u;
        witem *n = (witem *)wmem_realloc(o->items, (size_t)cap * sizeof(witem));
        if (!n) { o->oom = 1; return UINT32_MAX; }
        o->items = n; o->cap = cap;
    }
    witem *it = &o->items[o->count];
    wb_set(it, 0, sizeof(*it));
    it->kind = L->invisible ? 0 : kind;
    it->link = L->link;
    return o->count++;
}
static witem *IT(uint32_t i) { static witem dummy; return i < L->o->count ? &L->o->items[i] : &dummy; }

static uint32_t pool_put(const char *s, size_t n) {
    wlayout_out *o = L->o;
    if (o->pool_len + n + 1u > o->pool_cap) {
        size_t cap = o->pool_cap ? o->pool_cap * 2u : 65536u;
        while (cap < o->pool_len + n + 1u) cap *= 2u;
        char *p = (char *)wmem_realloc(o->pool, cap);
        if (!p) { o->oom = 1; return 0; }
        o->pool = p; o->pool_cap = cap;
    }
    uint32_t at = (uint32_t)o->pool_len;
    wb_copy(o->pool + at, s, n);
    o->pool[at + n] = '\0';
    o->pool_len += n + 1u;
    return at;
}

static void translate(uint32_t first, uint32_t end, int32_t dx, int32_t dy) {
    for (uint32_t i = first; i < end && i < L->o->count; ++i) { L->o->items[i].x += dx; L->o->items[i].y += dy; }
}

/* ------------------------------------------------------------------ lengths and boxes */
static int32_t len_px(wlen l, int32_t ref, int32_t dflt) {
    if (l.u == WU_PX) return l.v / 64;
    if (l.u == WU_PCT) return (int32_t)((int64_t)ref * l.v / 6400);
    return dflt;
}
typedef struct box { int32_t ml, mr, mt, mb, bl, br, bt, bb, pl, pr, pt, pb; uint8_t auto_l, auto_r; } box;
static void box_of(const wstyle *s, int32_t cw, box *b) {
    if (cw < 0) cw = 0;
    b->mt = len_px(s->margin[0], cw, 0); b->mr = len_px(s->margin[1], cw, 0);
    b->mb = len_px(s->margin[2], cw, 0); b->ml = len_px(s->margin[3], cw, 0);
    b->auto_l = s->margin[3].u == WU_AUTO; b->auto_r = s->margin[1].u == WU_AUTO;
    b->pt = len_px(s->padding[0], cw, 0); b->pr = len_px(s->padding[1], cw, 0);
    b->pb = len_px(s->padding[2], cw, 0); b->pl = len_px(s->padding[3], cw, 0);
    b->bt = s->border[0]; b->br = s->border[1]; b->bb = s->border[2]; b->bl = s->border[3];
    if (b->mt < -4000) b->mt = -4000;
    if (b->mb < -4000) b->mb = -4000;
    if (b->ml < -4000) b->ml = 0;
    if (b->mr < -4000) b->mr = 0;
}

static int is_elem(const wnode *n) { return n && n->tag >= WT_UNKNOWN && n->style; }
static int is_blockish(const wstyle *s) {
    switch (s->display) {
    case WD_BLOCK: case WD_LIST_ITEM: case WD_TABLE: case WD_FLEX: case WD_TABLE_ROW: case WD_TABLE_ROW_GROUP:
    case WD_TABLE_CELL: case WD_TABLE_CAPTION: return 1;
    default: return 0;
    }
}
static int is_control(const wnode *n) { return n->ctl >= 0 && (n->tag == WT_INPUT || n->tag == WT_SELECT || n->tag == WT_TEXTAREA || n->tag == WT_BUTTON); }
static int establishes_bfc(const wnode *n, const wstyle *s) {
    if (s->float_ != WF_NONE || s->position == WP_ABSOLUTE || s->position == WP_FIXED || s->overflow_hidden) return 1;
    if (s->display == WD_INLINE_BLOCK || s->display == WD_TABLE_CELL || s->display == WD_TABLE_CAPTION || s->display == WD_FLEX ||
        s->display == WD_INLINE_FLEX || s->display == WD_TABLE || s->display == WD_INLINE_TABLE) return 1;
    if (n->parent && n->parent->style && (n->parent->style->display == WD_FLEX || n->parent->style->display == WD_INLINE_FLEX)) return 1;
    return n->tag == WT_BODY || n->tag == WT_HTML;
}

/* ------------------------------------------------------------------ floats */
static void float_edges(int32_t y, int32_t h, int32_t x0, int32_t x1, int32_t *l, int32_t *r) {
    *l = x0; *r = x1;
    if (h < 1) h = 1;
    for (uint32_t i = L->bfc; i < L->nfloats; ++i) {
        const flt *f = &L->floats[i];
        if (f->y1 <= y || f->y0 >= y + h) continue;
        if (f->right) { if (f->x0 < *r) *r = f->x0; }
        else if (f->x1 > *l) *l = f->x1;
    }
    if (*l < x0) *l = x0;
    if (*r > x1) *r = x1;
}
/* Lowest float bottom below y that overlaps [y, y+h), or -1. */
static int32_t next_float_bottom(int32_t y, int32_t h) {
    int32_t best = -1;
    for (uint32_t i = L->bfc; i < L->nfloats; ++i) {
        const flt *f = &L->floats[i];
        if (f->y1 <= y || f->y0 >= y + (h < 1 ? 1 : h)) continue;
        if (best < 0 || f->y1 < best) best = f->y1;
    }
    return best;
}
static int32_t clear_y(uint8_t clear) {
    int32_t y = 0;
    for (uint32_t i = L->bfc; i < L->nfloats; ++i) {
        const flt *f = &L->floats[i];
        if ((clear == WC_BOTH) || (clear == WC_LEFT && !f->right) || (clear == WC_RIGHT && f->right)) if (f->y1 > y) y = f->y1;
    }
    return y;
}
static int32_t floats_bottom(void) { return clear_y(WC_BOTH); }

/* ------------------------------------------------------------------ text metrics */
static int has_nonascii(const char *s, size_t n) { for (size_t i = 0; i < n; ++i) if ((uint8_t)s[i] >= 0x80u) return 1; return 0; }
static int font_for(const wstyle *st, const char *s, size_t n) {
    return wfont_pick(st->font_px, st->weight, st->italic, st->mono, has_nonascii(s, n));
}
static int32_t measure(int font, const wstyle *st, const char *s, size_t n) {
    if (st->mono) {
        int32_t chars = 0;
        for (size_t i = 0; i < n; ++i) if (((uint8_t)s[i] & 0xC0u) != 0x80u) ++chars;
        return chars * wfont_mono_cell(font);
    }
    return wtext_width(font, s, n);
}
static void line_metrics(int font, const wstyle *st, int32_t *asc, int32_t *desc) {
    int32_t fa = wfont_ascent(font), fl = wfont_line(font);
    if (st->line_px > 0 && st->font_px > 0) {
        int32_t lh = (int32_t)((int64_t)st->line_px * mint_fonts[font].px / st->font_px);
        if (lh < fl) lh = fl;
        if (lh > fl * 3) lh = fl * 3;
        int32_t half = (lh - fl) / 2;
        *asc = fa + half; *desc = lh - *asc;
    } else { *asc = fa + 2; *desc = fl - fa + 2; }
}
static int32_t strut_height(const wstyle *st) {
    int font = wfont_pick(st->font_px ? st->font_px : 16, st->weight, st->italic, st->mono, 0);
    int32_t a, d; line_metrics(font, st, &a, &d);
    return a + d;
}

/* ------------------------------------------------------------------ frags */
static frag *frag_new(uint8_t kind) {
    if (L->nfrags >= L->fcap) {
        uint32_t cap = L->fcap ? L->fcap * 2u : 1024u;
        frag *n = (frag *)wmem_realloc(L->frags, (size_t)cap * sizeof(frag));
        if (!n) { L->o->oom = 1; return 0; }
        L->frags = n; L->fcap = cap;
    }
    frag *f = &L->frags[L->nfrags++];
    wb_set(f, 0, sizeof(*f));
    f->kind = kind; f->link = L->link;
    return f;
}

/* ------------------------------------------------------------------ inline formatting */
static void place_float(ifc *I, wnode *n);

static void ifc_apply_margin(ifc *I) {
    I->y += I->pm + I->pmn;
    I->pm = I->pmn = 0;
}
static void collapse_margin(ifc *I, int32_t m) {
    if (m >= 0) { if (m > I->pm) I->pm = m; }
    else if (m < I->pmn) I->pmn = m;
}

static void line_begin(ifc *I) {
    if (I->open) return;
    ifc_apply_margin(I);
    int32_t h = strut_height(I->st);
    for (uint32_t guard = 0; guard < MAX_FLOATS; ++guard) {
        float_edges(I->y, h, I->x0, I->x1, &I->ll, &I->lr);
        if (I->lr - I->ll >= 40 || I->lr - I->ll >= I->x1 - I->x0) break;
        int32_t nb = next_float_bottom(I->y, h);
        if (nb < 0) break;
        I->y = nb;
    }
    I->lx = 0;
    if (I->first && I->st->text_indent) I->lx = I->st->text_indent;
    if (I->lx < 0 && I->ll + I->lx < I->x0) I->lx = 0;
    I->open = 1;
    I->base = L->nfrags;
    I->space = 0;
}

/* Finishes the current line: computes its box, aligns and emits the items. */
static void line_end(ifc *I, int last) {
    if (!I->open) return;
    I->open = 0;
    uint32_t b = I->base, e = L->nfrags;
    /* drop trailing spaces */
    while (e > b && L->frags[e - 1].kind == F_SPACE) --e;
    int32_t asc = 0, desc = 0;
    {
        int font = wfont_pick(I->st->font_px ? I->st->font_px : 16, I->st->weight, 0, I->st->mono, 0);
        line_metrics(font, I->st, &asc, &desc);                     /* strut */
    }
    int content = 0;
    for (uint32_t i = b; i < e; ++i) {
        frag *f = &L->frags[i];
        if (f->kind == F_TEXT || f->kind == F_ATOM || f->kind == F_MARKER) content = 1;
        int32_t a = f->asc - f->dy, d = f->desc + f->dy;
        if (f->kind == F_ATOM) {
            if (f->valign == WV_MIDDLE || f->valign == WV_TOP || f->valign == WV_BOTTOM) {
                int32_t mid = asc - 4;
                a = mid + f->ah / 2; d = f->ah - a;
                if (d < 0) d = 0;
            } else { a = f->ah; d = 0; }
        }
        if (a > asc) asc = a;
        if (d > desc) desc = d;
    }
    if (!content && !(e > b) && !last) {
        /* empty line (e.g. <br><br>): strut height */
    }
    int32_t width = 0;
    for (uint32_t i = b; i < e; ++i) { int32_t r = L->frags[i].x + L->frags[i].w; if (r > width) width = r; }
    int32_t avail = I->lr - I->ll;
    int32_t shift = 0, extra_per = 0, between = 0;
    uint32_t spaces = 0, atoms = 0;
    for (uint32_t i = b; i < e; ++i) { if (L->frags[i].kind == F_SPACE) ++spaces; if (L->frags[i].kind == F_ATOM) ++atoms; }
    if (width < avail) {
        int32_t slack = avail - width;
        if (I->flex && I->align == 3 && atoms > 1) between = slack / (int32_t)(atoms - 1);
        else if (I->flex && I->align == 4 && atoms) { between = slack / (int32_t)atoms; shift = between / 2; }
        else if (I->align == WA_RIGHT) shift = slack;
        else if (I->align == WA_CENTER) shift = slack / 2;
        else if (I->align == WA_JUSTIFY && !last && spaces) extra_per = slack / (int32_t)spaces;
    }
    int32_t top = I->y;
    int32_t base_y = top + asc;
    int32_t x0 = I->ll;
    /* list marker of the first line in a list item */
    if (L->marker.on && (content || last)) {
        const wstyle *ms = L->marker.st;
        if (L->marker.text[0]) {
            int font = font_for(ms, L->marker.text, wstrlen(L->marker.text));
            int32_t w = wtext_width(font, L->marker.text, wstrlen(L->marker.text));
            uint32_t k = item_new(WI_TEXT);
            witem *it = IT(k);
            it->font = (uint8_t)font; it->x = L->marker.right - w; it->y = base_y - wfont_ascent(font); it->w = w; it->h = wfont_line(font);
            it->color = ms->color; it->a = pool_put(L->marker.text, wstrlen(L->marker.text)); it->b = (uint32_t)wstrlen(L->marker.text);
            it->link = -1;
        } else {
            int font = font_for(ms, "x", 1);
            int32_t sz = ms->font_px >= 20 ? 7 : 5;
            uint32_t k = item_new(WI_BULLET);
            witem *it = IT(k);
            it->x = L->marker.right - sz - 2; it->y = base_y - wfont_ascent(font) / 2 - sz / 2 - 1; it->w = sz; it->h = sz;
            it->extra = L->marker.bullet; it->color = ms->color; it->link = -1;
        }
        L->marker.on = 0;
    }
    /* emit text runs, atoms and inline backgrounds */
    int32_t acc = 0, abetween = 0;
    uint32_t i = b;
    while (i < e) {
        frag *f = &L->frags[i];
        int32_t fx = x0 + shift + f->x + acc + abetween;
        if (f->kind == F_SPACE) { acc += extra_per; ++i; continue; }
        if (f->kind == F_ATOM) {
            int32_t ay;
            if (f->valign == WV_MIDDLE || f->valign == WV_TOP || f->valign == WV_BOTTOM) {
                ay = f->valign == WV_TOP ? top : f->valign == WV_BOTTOM ? top + asc + desc - f->ah : base_y - (asc - 4) - (f->ah - (f->ah / 2)) + (f->ah - f->ah / 2) - f->ah / 2 ;
                if (f->valign == WV_MIDDLE) ay = base_y - 4 - f->ah / 2;
            } else ay = base_y - f->ah;
            translate(f->first, f->first + f->count, fx, ay);
            abetween += between;
            ++i;
            continue;
        }
        if (f->kind != F_TEXT && f->kind != F_MARKER) { ++i; continue; }
        /* merge following words of the same run */
        uint32_t j = i + 1;
        while (j + 1 < e && L->frags[j].kind == F_SPACE && L->frags[j + 1].kind == F_TEXT && !extra_per &&
               L->frags[j + 1].st == f->st && L->frags[j + 1].font == f->font && L->frags[j + 1].link == f->link &&
               L->frags[j + 1].flags == f->flags && L->frags[j + 1].dy == f->dy && L->frags[j].st == f->st) j += 2;
        /* text of the run */
        static char run[4096];
        size_t rl = 0;
        int32_t rw = 0;
        for (uint32_t k = i; k < j; ++k) {
            frag *g = &L->frags[k];
            if (g->kind == F_SPACE) { if (rl + 1 < sizeof(run)) run[rl++] = ' '; }
            else for (uint32_t q = 0; q < g->len && rl + 1 < sizeof(run); ++q) run[rl++] = g->text[q];
            rw = g->x + g->w - f->x;
        }
        if (f->bg >> 24) {
            uint32_t k = item_new(WI_RECT);
            witem *it = IT(k);
            it->x = fx - 1; it->y = base_y - f->asc + 1 + f->dy; it->w = rw + 2; it->h = f->asc + f->desc - 2; it->color = f->bg;
            it->link = f->link;
        }
        uint32_t k = item_new(WI_TEXT);
        witem *it = IT(k);
        it->font = f->font; it->flags = f->flags;
        it->x = fx; it->y = base_y + f->dy - wfont_ascent(f->font); it->w = rw; it->h = wfont_line(f->font);
        it->color = f->color; it->link = f->link;
        it->a = pool_put(run, rl); it->b = (uint32_t)rl;
        if (f->flags & WIF_MONO) it->extra = (uint8_t)wfont_mono_cell(f->font);
        i = j;
    }
    L->nfrags = I->base;
    I->y = top + asc + desc;
    I->first = 0;
    I->space = 0;
    if (content) I->any_content = 1;
    /* floats that arrived in the middle of this line */
    int nd = I->ndef;
    I->ndef = 0;
    for (int k = 0; k < nd; ++k) place_float(I, I->deferred[k]);
}

static void forced_break(ifc *I) {
    line_begin(I);
    line_end(I, 1);
}

static int fits(ifc *I, int32_t w) { return I->ll + I->lx + w <= I->lr; }

/* Moves an empty line down past floats until w fits (or no floats remain). */
static void make_room(ifc *I, int32_t w) {
    for (uint32_t guard = 0; guard < MAX_FLOATS && !fits(I, w) && L->nfrags == I->base; ++guard) {
        int32_t nb = next_float_bottom(I->y, strut_height(I->st));
        if (nb < 0 || nb <= I->y) break;
        I->y = nb;
        float_edges(I->y, strut_height(I->st), I->x0, I->x1, &I->ll, &I->lr);
    }
}

static void push_space_if_needed(ifc *I) {
    if (!I->space) return;
    I->space = 0;
    if (L->nfrags == I->base) return;                 /* no leading spaces */
    const wstyle *st = I->space_st;
    int font = font_for(st, " ", 1);
    frag *f = frag_new(F_SPACE);
    if (!f) return;
    f->st = st; f->font = (uint8_t)font; f->x = I->lx; f->w = measure(font, st, " ", 1); f->link = I->space_link;
    line_metrics(font, st, &f->asc, &f->desc);
    I->lx += f->w;
}

static void add_word(ifc *I, const char *s, size_t n, const wstyle *st, uint8_t extra_flags, int wrap) {
    if (!n) return;
    int font = font_for(st, s, n);
    int32_t w = measure(font, st, s, n);
    line_begin(I);
    int32_t sw = 0;
    if (I->space && L->nfrags > I->base) sw = measure(font_for(I->space_st, " ", 1), I->space_st, " ", 1);
    if (wrap && L->nfrags > I->base && !fits(I, sw + w)) {
        line_end(I, 0);
        line_begin(I);
    }
    if (wrap && L->nfrags == I->base) make_room(I, w);
    push_space_if_needed(I);
    if (wrap && w > I->lr - I->ll - I->lx && L->nfrags == I->base && n > 1) {
        /* a word longer than the line (URLs): break it between characters */
        const char *p = s, *end = s + n;
        while (p < end) {
            const char *start = p;
            int32_t cw = 0;
            while (p < end) {
                const char *q = p;
                (void)wutf8_next(&q, end);
                int32_t gw = measure(font, st, p, (size_t)(q - p));
                if (cw + gw > I->lr - I->ll - I->lx && p > start) break;
                cw += gw; p = q;
            }
            frag *f = frag_new(F_TEXT);
            if (!f) return;
            f->st = st; f->font = (uint8_t)font; f->text = start; f->len = (uint32_t)(p - start); f->x = I->lx; f->w = cw;
            line_metrics(font, st, &f->asc, &f->desc);
            f->flags = extra_flags; f->color = st->color; f->bg = st->bg;
            I->lx += cw;
            if (p < end) { line_end(I, 0); line_begin(I); }
        }
        return;
    }
    frag *f = frag_new(F_TEXT);
    if (!f) return;
    f->st = st; f->font = (uint8_t)font; f->text = s; f->len = (uint32_t)n; f->x = I->lx; f->w = w;
    line_metrics(font, st, &f->asc, &f->desc);
    f->flags = extra_flags; f->color = st->color; f->bg = st->bg;
    if (st->valign == WV_SUPER) f->dy = -(wfont_ascent(font) / 3);
    else if (st->valign == WV_SUB) f->dy = wfont_ascent(font) / 4;
    I->lx += w;
}

static uint8_t text_flags(const wstyle *st) {
    uint8_t d = (uint8_t)(L->deco | st->decoration);
    uint8_t f = 0;
    if (st->italic) f |= WIF_ITALIC;
    if (d & WDEC_UNDERLINE) f |= WIF_UNDERLINE;
    if (d & WDEC_STRIKE) f |= WIF_STRIKE;
    if (d & WDEC_OVERLINE) f |= WIF_OVERLINE;
    if (st->mono) f |= WIF_MONO;
    return f;
}

static const char *transform(const char *s, size_t n, uint8_t tt, int cap_first) {
    char *o = (char *)warena_alloc(&L->scratch, n + 1);
    if (!o) return s;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (tt == WTT_UPPER || (tt == WTT_CAPITALIZE && i == 0 && cap_first)) { if (c >= 'a' && c <= 'z') c = (char)(c - 32); }
        else if (tt == WTT_LOWER) { if (c >= 'A' && c <= 'Z') c = (char)(c + 32); }
        o[i] = c;
    }
    return o;
}

static int is_space_byte(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* Adds the text of a text node with style st. */
static void add_text(ifc *I, const char *t, size_t n, const wstyle *st, uint8_t extra_flags) {
    if (st->hide_text || !n) return;
    uint8_t flags = (uint8_t)(text_flags(st) | extra_flags);
    uint8_t ws = st->white_space;
    int wrap = ws != WS_NOWRAP && ws != WS_PRE;
    if (I->flex == 2) wrap = 0;
    size_t i = 0;
    if (ws == WS_PRE || ws == WS_PRE_WRAP) {
        while (i < n) {
            if (t[i] == '\n') { forced_break(I); ++i; continue; }
            if (t[i] == '\r') { ++i; continue; }
            size_t st0 = i;
            /* a run of non-newline characters; pre-wrap breaks at spaces */
            while (i < n && t[i] != '\n' && t[i] != '\r' && !(ws == WS_PRE_WRAP && t[i] == ' ' && i > st0)) ++i;
            if (ws == WS_PRE_WRAP && i < n && t[i] == ' ') ++i;          /* keep one space with the word */
            /* expand tabs */
            size_t len = i - st0;
            const char *seg = t + st0;
            int has_tab = 0;
            for (size_t k = 0; k < len; ++k) if (seg[k] == '\t') has_tab = 1;
            if (has_tab) {
                char *o = (char *)warena_alloc(&L->scratch, len * 8 + 1);
                if (o) {
                    size_t ol = 0, col = 0;
                    for (size_t k = 0; k < len; ++k) {
                        if (seg[k] == '\t') { do { o[ol++] = ' '; ++col; } while (col % 8); }
                        else { o[ol++] = seg[k]; if (((uint8_t)seg[k] & 0xC0u) != 0x80u) ++col; }
                    }
                    seg = o; len = ol;
                }
            }
            add_word(I, seg, len, st, flags, ws == WS_PRE_WRAP);
        }
        return;
    }
    while (i < n) {
        if (is_space_byte(t[i])) {
            if (ws == WS_PRE_LINE && t[i] == '\n') { forced_break(I); ++i; continue; }
            if (L->nfrags > I->base || (I->open && L->nfrags > I->base)) { I->space = 1; I->space_st = st; I->space_link = L->link; }
            else if (I->open) { I->space = 1; I->space_st = st; I->space_link = L->link; }
            ++i;
            continue;
        }
        size_t s0 = i;
        while (i < n && !is_space_byte(t[i])) {
            if ((uint8_t)t[i] == 0xE2 && i + 2 < n && (uint8_t)t[i + 1] == 0x80 && (uint8_t)t[i + 2] == 0x8B) break;   /* ZWSP */
            ++i;
        }
        size_t len = i - s0;
        const char *w = t + s0;
        if (len && st->text_transform != WTT_NONE) w = transform(w, len, st->text_transform, 1);
        /* strip soft hyphens */
        int shy = 0;
        for (size_t k = 0; k + 1 < len; ++k) if ((uint8_t)w[k] == 0xC2 && (uint8_t)w[k + 1] == 0xAD) shy = 1;
        if (shy) {
            char *o = (char *)warena_alloc(&L->scratch, len + 1);
            if (o) { size_t ol = 0; for (size_t k = 0; k < len; ++k) { if (k + 1 < len && (uint8_t)w[k] == 0xC2 && (uint8_t)w[k + 1] == 0xAD) { ++k; continue; } o[ol++] = w[k]; } w = o; len = ol; }
        }
        if (len) add_word(I, w, len, st, flags, wrap);
        if (i + 2 < n && (uint8_t)t[i] == 0xE2 && (uint8_t)t[i + 1] == 0x80 && (uint8_t)t[i + 2] == 0x8B) i += 3;
    }
}

static void add_gap(ifc *I, int32_t w) {
    if (w <= 0) return;
    line_begin(I);
    push_space_if_needed(I);
    frag *f = frag_new(F_GAP);
    if (!f) return;
    f->x = I->lx; f->w = w;
    I->lx += w;
}

static void add_atom(ifc *I, uint32_t first, uint32_t count, int32_t w, int32_t h, uint8_t valign, int wrap) {
    line_begin(I);
    int32_t sw = 0;
    if (I->space && L->nfrags > I->base) sw = measure(font_for(I->space_st, " ", 1), I->space_st, " ", 1);
    int32_t g = (I->flex && L->nfrags > I->base) ? I->gap : 0;
    if (wrap && L->nfrags > I->base && !fits(I, sw + g + w)) { line_end(I, 0); line_begin(I); g = 0; }
    if (L->nfrags == I->base) make_room(I, w);
    push_space_if_needed(I);
    if (g) I->lx += g;
    frag *f = frag_new(F_ATOM);
    if (!f) return;
    f->x = I->lx; f->w = w; f->ah = h; f->first = first; f->count = count; f->valign = valign;
    I->lx += w;
}

/* ------------------------------------------------------------------ intrinsic widths */
static void intrinsic(wnode *n, int32_t *mn, int32_t *mx);
static void image_box(wnode *n, const wstyle *s, int32_t avail, int32_t *w, int32_t *h, int *known);
static void control_box(wnode *n, const wstyle *s, int32_t avail, int32_t *w, int32_t *h);
static void table_columns(wnode *t, int32_t avail, int32_t *cols, uint32_t *ncols, int32_t *tmin, int32_t *tmax, int compute_widths);

static void intrinsic_flow(wnode *n, int32_t *mn, int32_t *line, int32_t *mx, int nowrap, int depth) {
    for (wnode *c = n->first; c; c = c->next) {
        if (c->tag == WT_TEXT) {
            const wstyle *st = n->style;
            if (!st || st->hide_text) continue;
            int nw = nowrap || st->white_space == WS_NOWRAP || st->white_space == WS_PRE;
            size_t i = 0;
            const char *t = c->text;
            int32_t spacew = 0;
            while (i < c->text_len) {
                if (is_space_byte(t[i])) {
                    if (st->white_space == WS_PRE && t[i] == '\n') { if (*line > *mx) *mx = *line; if (*line > *mn && nw) *mn = *line; *line = 0; }
                    if (!spacew) spacew = measure(font_for(st, " ", 1), st, " ", 1);
                    if (*line) *line += spacew;
                    ++i;
                    continue;
                }
                size_t s0 = i;
                while (i < c->text_len && !is_space_byte(t[i])) ++i;
                int32_t w = measure(font_for(st, t + s0, i - s0), st, t + s0, i - s0);
                *line += w;
                if (w > *mn) *mn = w;
                if (nw && *line > *mn) *mn = *line;
            }
            continue;
        }
        if (!is_elem(c)) continue;
        const wstyle *s = c->style;
        if (s->display == WD_NONE) continue;
        if (c->tag == WT_BR) { if (*line > *mx) *mx = *line; *line = 0; continue; }
        if (s->display == WD_INLINE && !is_control(c) && c->tag != WT_IMG && depth < MAX_DEPTH) {
            box b; box_of(s, 0, &b);
            *line += b.ml + b.pl + b.bl;
            intrinsic_flow(c, mn, line, mx, nowrap || s->white_space == WS_NOWRAP, depth + 1);
            *line += b.mr + b.pr + b.br;
            continue;
        }
        if (s->display == WD_CONTENTS) { intrinsic_flow(c, mn, line, mx, nowrap, depth + 1); continue; }
        int32_t cmn = 0, cmx = 0;
        if (depth < MAX_DEPTH) intrinsic(c, &cmn, &cmx);
        box b; box_of(s, 0, &b);
        int32_t mrg = (b.auto_l ? 0 : b.ml) + (b.auto_r ? 0 : b.mr);
        if (cmn + mrg > *mn) *mn = cmn + mrg;
        int inline_level = s->display == WD_INLINE || s->display == WD_INLINE_BLOCK || s->display == WD_INLINE_TABLE ||
                           s->display == WD_INLINE_FLEX || c->tag == WT_IMG || is_control(c);
        int flex_parent = n->style && (n->style->display == WD_FLEX || n->style->display == WD_INLINE_FLEX) && !n->style->flex_column;
        if ((inline_level || flex_parent) && s->float_ == WF_NONE) *line += cmx + mrg + (flex_parent && *line ? n->style->gap : 0);
        else {
            if (*line > *mx) *mx = *line;
            *line = 0;
            if (cmx + mrg > *mx) *mx = cmx + mrg;
        }
    }
}

static void intrinsic(wnode *n, int32_t *mn, int32_t *mx) {
    *mn = *mx = 0;
    if (!is_elem(n)) return;
    if (n->iw_gen == L->gen) { *mn = n->min_w; *mx = n->max_w; return; }
    const wstyle *s = n->style;
    if (s->display == WD_NONE) return;
    box b; box_of(s, 0, &b);
    int32_t extra = b.bl + b.br + b.pl + b.pr;
    int32_t a = 0, z = 0;
    if (s->width.u == WU_PX && n->tag != WT_TABLE) {
        a = z = s->width.v / 64 + (s->border_box ? 0 : extra);
        if (a < extra) a = z = extra;
    } else if (n->tag == WT_IMG) {
        int32_t w, h; int known;
        image_box(n, s, 1 << 20, &w, &h, &known);
        z = w + extra; a = s->width.u == WU_PCT ? 0 : z;
    } else if (is_control(n)) {
        int32_t w, h; control_box(n, s, 1 << 20, &w, &h);
        a = z = w;
    } else if (s->display == WD_TABLE || s->display == WD_INLINE_TABLE) {
        int32_t tmin = 0, tmax = 0; uint32_t nc = 0;
        table_columns(n, 1 << 20, 0, &nc, &tmin, &tmax, 0);
        a = tmin + extra; z = tmax + extra;
        if (s->width.u == WU_PX && s->width.v / 64 > a) a = z = s->width.v / 64;
    } else {
        int32_t line = 0;
        int nowrap = s->white_space == WS_NOWRAP || s->white_space == WS_PRE;
        intrinsic_flow(n, &a, &line, &z, nowrap, 0);
        if (line > z) z = line;
        if (a > z) z = a;
        a += extra; z += extra;
    }
    if (s->max_w.u == WU_PX) { int32_t m = s->max_w.v / 64 + (s->border_box ? 0 : extra); if (z > m) z = m; if (a > z) a = z; }
    if (s->min_w.u == WU_PX) { int32_t m = s->min_w.v / 64 + (s->border_box ? 0 : extra); if (a < m) a = m; if (z < m) z = m; }
    n->min_w = a; n->max_w = z; n->iw_gen = L->gen;
    *mn = a; *mx = z;
}

/* ------------------------------------------------------------------ replaced elements */
static int32_t attr_int(const wnode *n, const char *name, int32_t dflt) {
    const char *v = wnode_attr(n, name);
    if (!v) return dflt;
    int32_t r = 0; int any = 0;
    while (*v == ' ') ++v;
    while (*v >= '0' && *v <= '9' && r < 100000) { r = r * 10 + (*v++ - '0'); any = 1; }
    return any ? r : dflt;
}

static void image_box(wnode *n, const wstyle *s, int32_t avail, int32_t *w, int32_t *h, int *known) {
    int32_t nw = 0, nh = 0;
    int have = n->img >= 0 && L->env->image_size && L->env->image_size(L->env->ctx, n->img, &nw, &nh) && nw > 0 && nh > 0;
    *known = have;
    int32_t sw = s->width.u == WU_PX ? s->width.v / 64 : s->width.u == WU_PCT ? (int32_t)((int64_t)avail * s->width.v / 6400) : -1;
    int32_t sh = s->height.u == WU_PX ? s->height.v / 64 : -1;
    if (sw >= 0 && s->border_box) { sw -= s->border[1] + s->border[3]; if (sw < 0) sw = 0; }
    if (sw >= 0 && sh >= 0) { *w = sw; *h = sh; }
    else if (sw >= 0) { *w = sw; *h = have ? (int32_t)((int64_t)sw * nh / nw) : (sw ? sw * 2 / 3 : 0); }
    else if (sh >= 0) { *h = sh; *w = have ? (int32_t)((int64_t)sh * nw / nh) : sh; }
    else if (have) { *w = nw; *h = nh; }
    else { *w = 0; *h = 0; }
    int32_t maxw = s->max_w.u == WU_PX ? s->max_w.v / 64 : s->max_w.u == WU_PCT ? (int32_t)((int64_t)avail * s->max_w.v / 6400) : avail;
    if (maxw > avail) maxw = avail;
    if (maxw >= 0 && *w > maxw && *w > 0) {
        *h = (int32_t)((int64_t)*h * maxw / *w);
        *w = maxw;
    }
    if (s->max_h.u == WU_PX && *h > s->max_h.v / 64 && *h > 0) { int32_t mh = s->max_h.v / 64; *w = (int32_t)((int64_t)*w * mh / *h); *h = mh; }
    if (*w > 20000) *w = 20000;
    if (*h > 20000) *h = 20000;
}

static void control_box(wnode *n, const wstyle *s, int32_t avail, int32_t *w, int32_t *h) {
    const wctl *c = &L->doc->ctls[n->ctl];
    int32_t cw = -1;
    if (s->width.u == WU_PX) cw = s->width.v / 64;
    else if (s->width.u == WU_PCT) cw = (int32_t)((int64_t)avail * s->width.v / 6400);
    *h = 24;
    switch (c->type) {
    case WCT_CHECKBOX: case WCT_RADIO: *w = 18; *h = 18; return;
    case WCT_SUBMIT: case WCT_RESET: case WCT_BUTTON: case WCT_IMAGE: case WCT_BUTTON_EL: {
        const char *lab = c->label ? c->label : "";
        *w = wtext_width(FONT_BODY, lab, wstrlen(lab)) + 24;
        if (*w < 32) *w = 32;
        break;
    }
    case WCT_SELECT: {
        int32_t m = 40;
        for (int32_t i = 0; i < c->nopts && i < 200; ++i) {
            char t[200]; size_t l = wnode_text(c->opts[i], t, sizeof(t));
            int32_t tw = wtext_width(FONT_BODY, t, l) + 36;
            if (tw > m) m = tw;
        }
        *w = m > 360 ? 360 : m;
        break;
    }
    case WCT_TEXTAREA: *w = (c->size ? c->size : 30) * 7 + 14; *h = (c->rows ? c->rows : 3) * 17 + 10; break;
    case WCT_FILE: *w = 230; break;
    case WCT_HIDDEN: *w = 0; *h = 0; return;
    default: *w = (c->size ? c->size : 24) * 7 + 14; break;
    }
    if (cw > 0) *w = cw;
    if (s->height.u == WU_PX && s->height.v / 64 > 10 && c->type != WCT_CHECKBOX && c->type != WCT_RADIO) *h = s->height.v / 64;
    if (*w > avail && avail > 40) *w = avail;
    if (*h > 2000) *h = 2000;
}

/* ------------------------------------------------------------------ block layout */
typedef struct boxres { int32_t h; uint32_t bg, border, content_first, content_end; } boxres;
static void layout_box(wnode *n, int32_t x, int32_t y, int32_t w, boxres *res);
static void flow_children(wnode *n, int32_t x0, int32_t x1, int32_t *y, int nested_bfc_ok);
static void flow(ifc *I, wnode *n);

static int32_t used_width(wnode *n, const wstyle *s, const box *b, int32_t cw, int shrink) {
    int32_t pb = b->pl + b->pr + b->bl + b->br;
    int32_t w;
    if (s->width.u == WU_PX || s->width.u == WU_PCT) {
        w = len_px(s->width, cw, 0);
        if (!s->border_box) w += pb;
    } else if (shrink) {
        int32_t mn, mx;
        intrinsic(n, &mn, &mx);
        int32_t avail = cw - (b->auto_l ? 0 : b->ml) - (b->auto_r ? 0 : b->mr);
        w = mx < avail ? mx : avail;
        if (w < mn) w = mn;
    } else w = cw - b->ml - b->mr;
    if (s->max_w.u == WU_PX || s->max_w.u == WU_PCT) { int32_t m = len_px(s->max_w, cw, w) + (s->border_box ? 0 : pb); if (w > m) w = m; }
    if (s->min_w.u == WU_PX || s->min_w.u == WU_PCT) { int32_t m = len_px(s->min_w, cw, 0) + (s->border_box ? 0 : pb); if (w < m) w = m; }
    if (w < pb) w = pb;
    if (w > 30000) w = 30000;
    return w;
}

static void list_marker(wnode *n, const wstyle *s, int32_t content_x) {
    L->marker.on = 0;
    if (s->list_style == WL_NONE) return;
    L->marker.st = s;
    L->marker.right = content_x - 6;
    L->marker.text[0] = '\0';
    if (s->list_style <= WL_SQUARE) {
        L->marker.bullet = s->list_style == WL_CIRCLE ? WBUL_CIRCLE : s->list_style == WL_SQUARE ? WBUL_SQUARE : WBUL_DISC;
    } else {
        /* ordinal: <ol start>, <li value>, previous items */
        int32_t num = 1;
        const wnode *p = n->parent;
        int32_t start = p && p->tag == WT_OL ? attr_int(p, "start", 1) : 1;
        num = start;
        for (const wnode *q = n->prev; q; q = q->prev) {
            if (is_elem(q) && q->style->display == WD_LIST_ITEM) {
                const char *v = wnode_attr(q, "value");
                if (v) { num += attr_int(q, "value", 0); break; }
                ++num;
            }
        }
        if (wnode_attr(n, "value")) num = attr_int(n, "value", num);
        char t[24]; size_t o = 0;
        if (s->list_style == WL_LOWER_ALPHA || s->list_style == WL_UPPER_ALPHA) {
            char buf[8]; size_t k = 0;
            int32_t v = num > 0 ? num : 1;
            while (v > 0 && k < sizeof(buf)) { --v; buf[k++] = (char)((s->list_style == WL_LOWER_ALPHA ? 'a' : 'A') + v % 26); v /= 26; }
            while (k) t[o++] = buf[--k];
        } else if (s->list_style == WL_LOWER_ROMAN || s->list_style == WL_UPPER_ROMAN) {
            static const int vals[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
            static const char *const syms[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
            int32_t v = num > 0 && num < 4000 ? num : 1;
            for (int k = 0; k < 13; ++k) while (v >= vals[k] && o + 3 < sizeof(t)) { for (const char *q = syms[k]; *q; ++q) t[o++] = s->list_style == WL_UPPER_ROMAN ? (char)(*q - 32) : *q; v -= vals[k]; }
        } else {
            char d[16]; wstr_u64(d, sizeof(d), (uint64_t)(num < 0 ? 0 : num));
            for (size_t k = 0; d[k] && o + 2 < sizeof(t); ++k) t[o++] = d[k];
        }
        t[o++] = '.'; t[o] = '\0';
        wstr_copy(L->marker.text, sizeof(L->marker.text), t);
    }
    if (s->list_inside) L->marker.right = content_x + (L->marker.text[0] ? wtext_width(FONT_BODY, L->marker.text, wstrlen(L->marker.text)) + 4 : 12);
    L->marker.on = 1;
}

/* Emits a background rectangle placeholder (height set later). */
static uint32_t reserve_bg(const wstyle *s, wnode *n) {
    uint32_t bg = s->bg;
    if (!(bg >> 24) && L->row_bg && (n->style->display == WD_TABLE_CELL)) bg = L->row_bg;
    if (!(bg >> 24) || n->tag == WT_HTML || n->tag == WT_BODY) return UINT32_MAX;
    uint32_t k = item_new(WI_RECT);
    IT(k)->color = bg;
    IT(k)->a = (uint32_t)(s->radius > 0 ? s->radius : 0);
    IT(k)->link = -1;
    return k;
}

static void layout_table(wnode *t, int32_t x, int32_t y, int32_t w, int32_t *h_out);

/* Lays out element n as a block box with border-box top-left (x, y) and width w. */
static void layout_box(wnode *n, int32_t x, int32_t y, int32_t w, boxres *res) {
    const wstyle *s = n->style;
    box b; box_of(s, w, &b);
    n->y = y;
    res->bg = reserve_bg(s, n);
    res->content_first = L->o->count;
    uint8_t saved_inv = L->invisible;
    if (s->visibility_hidden) L->invisible = 1;
    int32_t h;
    if (s->display == WD_TABLE || s->display == WD_INLINE_TABLE) {
        layout_table(n, x, y, w, &h);
    } else if (n->tag == WT_IMG || is_control(n)) {
        h = 0;
    } else {
        int bfc = establishes_bfc(n, s);
        uint32_t saved_bfc = L->bfc, saved_nf = L->nfloats;
        if (bfc) L->bfc = L->nfloats;
        int32_t cx0 = x + b.bl + b.pl, cx1 = x + w - b.br - b.pr;
        if (cx1 < cx0) cx1 = cx0;
        int32_t cy = y + b.bt + b.pt;
        if (s->display == WD_LIST_ITEM) list_marker(n, s, cx0);
        int32_t end = cy;
        uint8_t saved_deco = L->deco;
        L->deco |= s->decoration;
        if (L->depth < MAX_DEPTH) { ++L->depth; flow_children(n, cx0, cx1, &end, 1); --L->depth; }
        L->deco = saved_deco;
        if (L->marker.on && L->marker.st == s) L->marker.on = 0;     /* empty list item */
        if (bfc) {
            int32_t fb = floats_bottom();
            if (fb > end) end = fb;
            L->bfc = saved_bfc; L->nfloats = saved_nf;
        }
        h = end - y + b.pb + b.bb;
    }
    int32_t min_h = b.bt + b.bb + b.pt + b.pb;
    if (s->height.u == WU_PX) {
        int32_t sh = s->height.v / 64 + (s->border_box ? 0 : min_h);
        if (s->overflow_hidden || s->display == WD_TABLE_CELL || n->tag == WT_HR) {
            if (s->display != WD_TABLE_CELL || sh > h) h = sh;
        } else if (sh > h) h = sh;
    }
    if (s->min_h.u == WU_PX) { int32_t m = s->min_h.v / 64 + (s->border_box ? 0 : min_h); if (h < m) h = m; }
    if (s->max_h.u == WU_PX && s->overflow_hidden) { int32_t m = s->max_h.v / 64 + (s->border_box ? 0 : min_h); if (h > m) h = m; }
    if (h < min_h) h = min_h;
    if (s->overflow_hidden && (s->height.u == WU_PX || s->max_h.u == WU_PX)) {
        /* clip: drop items that start below the box */
        for (uint32_t i = res->content_first; i < L->o->count; ++i) if (L->o->items[i].y >= y + h) L->o->items[i].kind = 0;
    }
    res->content_end = L->o->count;
    if (res->bg != UINT32_MAX) { witem *it = IT(res->bg); it->x = x; it->y = y; it->w = w; it->h = h; }
    res->border = UINT32_MAX;
    if ((b.bt || b.br || b.bb || b.bl) && n->tag != WT_HTML && n->tag != WT_BODY) {
        uint32_t k = item_new(WI_BORDER);
        witem *it = IT(k);
        it->x = x; it->y = y; it->w = w; it->h = h; it->link = -1;
        it->a = (uint32_t)(b.bt & 255) | ((uint32_t)(b.br & 255) << 8) | ((uint32_t)(b.bb & 255) << 16) | ((uint32_t)(b.bl & 255) << 24);
        it->color = s->border_color[0];
        it->c = s->border_color[2];
        it->b = s->border_color[3];
        it->extra = s->border_style[0] == WB_DOTTED || s->border_style[2] == WB_DOTTED ? 1 : s->border_style[0] == WB_DASHED || s->border_style[2] == WB_DASHED ? 2 : 0;
        it->font = (uint8_t)(s->radius > 0 ? (s->radius > 60 ? 60 : s->radius) : 0);
        res->border = k;
    }
    L->invisible = saved_inv;
    res->h = h;
}

/* A block-level child inside an inline formatting context. */
static void flow_block(ifc *I, wnode *n) {
    const wstyle *s = n->style;
    line_end(I, 1);
    if (s->float_ != WF_NONE) { place_float(I, n); return; }
    box b; box_of(s, I->x1 - I->x0, &b);
    if (s->clear != WC_NONE) {
        int32_t cy = clear_y(s->clear);
        ifc_apply_margin(I);
        if (cy > I->y) I->y = cy;
    }
    collapse_margin(I, b.mt);
    ifc_apply_margin(I);
    int32_t cx0 = I->x0, cx1 = I->x1;
    int shrink = s->display == WD_TABLE || s->position == WP_ABSOLUTE || s->position == WP_FIXED;
    if (establishes_bfc(n, s) && L->nfloats > L->bfc) {
        /* boxes that start a formatting context sit beside floats */
        int32_t l, r;
        float_edges(I->y, 20, I->x0, I->x1, &l, &r);
        int32_t mn, mx;
        intrinsic(n, &mn, &mx);
        if (r - l < mn && r - l < I->x1 - I->x0) {
            int32_t fb = next_float_bottom(I->y, 20);
            for (uint32_t guard = 0; guard < MAX_FLOATS && fb >= 0 && r - l < mn; ++guard) {
                I->y = fb; float_edges(I->y, 20, I->x0, I->x1, &l, &r); fb = next_float_bottom(I->y, 20);
            }
        }
        cx0 = l; cx1 = r;
    }
    int32_t cw = cx1 - cx0;
    int32_t w = used_width(n, s, &b, cw, shrink);
    int32_t x = cx0 + b.ml;
    if (b.auto_l && b.auto_r) x = cx0 + (cw - w) / 2;
    else if (b.auto_l) x = cx1 - b.mr - w;
    else if (n->tag == WT_TABLE && I->st->text_align == WA_CENTER && n->parent && n->parent->tag == WT_CENTER) x = cx0 + (cw - w) / 2;
    if (x < cx0 && (b.auto_l || b.auto_r)) x = cx0;
    boxres r;
    layout_box(n, x, I->y, w, &r);
    I->y += r.h;
    I->pm = I->pmn = 0;
    collapse_margin(I, b.mb);
    I->first = 1;
}

static void place_float(ifc *I, wnode *n) {
    if (I->open && L->nfrags > I->base) {                    /* wait for the end of this line */
        if (I->ndef < MAX_DEFER) { I->deferred[I->ndef++] = n; return; }
    }
    const wstyle *s = n->style;
    int32_t cw = I->x1 - I->x0;
    box b; box_of(s, cw, &b);
    int32_t w;
    int32_t ih = 0;
    uint32_t start = L->o->count;
    if (n->tag == WT_IMG) {
        int32_t iw; int known;
        image_box(n, s, cw, &iw, &ih, &known);
        w = iw + b.bl + b.br + b.pl + b.pr;
        uint32_t k = item_new(WI_IMAGE);
        witem *it = IT(k);
        it->x = b.ml + b.bl + b.pl; it->y = b.mt + b.bt + b.pt; it->w = iw; it->h = ih; it->a = (uint32_t)n->img;
        if (!known) it->flags = WIF_PLACEHOLDER;
        ih += b.bt + b.bb + b.pt + b.pb;
    } else {
        w = used_width(n, s, &b, cw, 1);
        if (w > cw) w = cw;
        boxres r;
        uint32_t saved_bfc = L->bfc, saved_nf = L->nfloats;
        layout_box(n, b.ml, b.mt, w, &r);
        L->bfc = saved_bfc; L->nfloats = saved_nf;
        ih = r.h;
    }
    int32_t mw = w + b.ml + b.mr, mh = ih + b.mt + b.mb;
    int32_t y = I->open ? I->y : I->y + I->pm + I->pmn;
    if (s->clear != WC_NONE) { int32_t c = clear_y(s->clear); if (c > y) y = c; }
    /* below earlier floats on the same side that started lower (CSS float rule 5) */
    for (uint32_t i = L->bfc; i < L->nfloats; ++i) if (L->floats[i].y0 > y) y = L->floats[i].y0;
    int32_t l = I->x0, r = I->x1;
    for (uint32_t guard = 0; guard < MAX_FLOATS; ++guard) {
        float_edges(y, mh, I->x0, I->x1, &l, &r);
        if (r - l >= mw || (l == I->x0 && r == I->x1)) break;
        int32_t nb = next_float_bottom(y, mh);
        if (nb < 0) break;
        y = nb;
    }
    int32_t fx = s->float_ == WF_RIGHT ? r - mw : l;
    if (fx < I->x0 && s->float_ == WF_RIGHT) fx = I->x0;
    translate(start, L->o->count, fx, y);
    n->y = y;
    if (L->nfloats < MAX_FLOATS) {
        flt *f = &L->floats[L->nfloats++];
        f->x0 = fx; f->x1 = fx + mw; f->y0 = y; f->y1 = y + mh; f->right = s->float_ == WF_RIGHT;
    }
    if (I->open && L->nfrags == I->base) {                   /* the empty line now starts beside the float */
        float_edges(I->y, strut_height(I->st), I->x0, I->x1, &I->ll, &I->lr);
    }
}

/* Inline-level atomic boxes: images, controls, inline-blocks, flex items. */
static void flow_atom(ifc *I, wnode *n, int as_flex_item) {
    const wstyle *s = n->style;
    int32_t cw = I->x1 - I->x0;
    box b; box_of(s, cw, &b);
    if (b.auto_l) b.ml = 0;
    if (b.auto_r) b.mr = 0;
    uint32_t start = L->o->count;
    int32_t w, h;
    if (n->tag == WT_IMG) {
        int32_t iw, ih; int known;
        int st = n->img >= 0 && L->env->image_state ? L->env->image_state(L->env->ctx, n->img) : -1;
        image_box(n, s, cw - b.ml - b.mr - b.bl - b.br - b.pl - b.pr, &iw, &ih, &known);
        const char *alt = wnode_attr(n, "alt");
        if (!known && st < 0 && (!iw || !ih)) {
            if (alt && alt[0]) add_text(I, alt, wstrlen(alt), s, WIF_ITALIC);
            return;
        }
        if (!iw && !ih && !known) return;                     /* still loading, size unknown */
        boxres r; wb_set(&r, 0, sizeof(r));
        r.bg = reserve_bg(s, n);
        uint32_t k = item_new(WI_IMAGE);
        witem *it = IT(k);
        it->x = b.ml + b.bl + b.pl; it->y = b.mt + b.bt + b.pt; it->w = iw; it->h = ih; it->a = (uint32_t)n->img;
        if (!known) it->flags = WIF_PLACEHOLDER;
        w = iw + b.bl + b.br + b.pl + b.pr; h = ih + b.bt + b.bb + b.pt + b.pb;
        if (r.bg != UINT32_MAX) { witem *bg = IT(r.bg); bg->x = b.ml; bg->y = b.mt; bg->w = w; bg->h = h; }
        if (b.bt || b.bl) {
            uint32_t bk = item_new(WI_BORDER);
            witem *bi = IT(bk);
            bi->x = b.ml; bi->y = b.mt; bi->w = w; bi->h = h;
            bi->a = (uint32_t)(b.bt & 255) | ((uint32_t)(b.br & 255) << 8) | ((uint32_t)(b.bb & 255) << 16) | ((uint32_t)(b.bl & 255) << 24);
            bi->color = bi->b = bi->c = s->border_color[0];
        }
    } else if (is_control(n)) {
        control_box(n, s, cw, &w, &h);
        if (!w && !h) return;
        uint32_t k = item_new(WI_CTL);
        witem *it = IT(k);
        it->x = b.ml; it->y = b.mt; it->w = w; it->h = h; it->a = (uint32_t)n->ctl;
        it->color = s->color; it->link = -1;
        n->y = 0;
    } else {
        w = used_width(n, s, &b, cw, 1);
        if (as_flex_item && I->flex && s->width.u == WU_AUTO) {
            int32_t mn, mx; intrinsic(n, &mn, &mx);
            if (w > cw - b.ml - b.mr) w = cw - b.ml - b.mr;
        }
        if (w > cw - b.ml - b.mr && cw - b.ml - b.mr > 0) w = cw - b.ml - b.mr;
        boxres r;
        uint32_t saved_bfc = L->bfc, saved_nf = L->nfloats;
        int saved_marker = L->marker.on;
        L->marker.on = 0;
        layout_box(n, b.ml, b.mt, w, &r);
        L->marker.on = saved_marker;
        L->bfc = saved_bfc; L->nfloats = saved_nf;
        h = r.h;
    }
    uint8_t va = s->valign;
    if (as_flex_item) va = WV_MIDDLE;
    add_atom(I, start, L->o->count - start, w + b.ml + b.mr, h + b.mt + b.mb, va, !(I->flex && !I->st->flex_wrap && 0));
}

static void flow_inline_children(ifc *I, wnode *n) { for (wnode *c = n->first; c; c = c->next) flow(I, c); }

static void flow(ifc *I, wnode *n) {
    if (n->tag == WT_TEXT) {
        const wnode *p = n->parent;
        const wstyle *st = p ? p->style : 0;
        if (!st) return;
        if (I->flex && p->style == I->st) {
            /* loose text in a flex row behaves like an anonymous item */
            int any = 0;
            for (uint32_t k = 0; k < n->text_len; ++k) if (!is_space_byte(n->text[k])) { any = 1; break; }
            if (!any) return;
        }
        add_text(I, n->text, n->text_len, st, 0);
        return;
    }
    if (!is_elem(n)) return;
    const wstyle *s = n->style;
    if (s->display == WD_NONE) return;
    if (L->depth >= MAX_DEPTH) {                       /* too deep: keep the words, drop the boxes */
        for (wnode *c = n->first; c; c = c->next) if (c->tag == WT_TEXT) add_text(I, c->text, c->text_len, s, 0);
        return;
    }
    int32_t saved_link = L->link;
    if (n->link >= 0) L->link = n->link;
    uint8_t saved_inv = L->invisible;
    if (s->visibility_hidden) L->invisible = 1;
    ++L->depth;
    int flex_item = I->flex && n->parent && n->parent->style == I->st;
    if (s->display == WD_CONTENTS) flow_inline_children(I, n);
    else if (n->tag == WT_BR) {
        if (!I->open || L->nfrags == I->base) { line_begin(I); }
        line_end(I, 1);
    } else if (n->tag == WT_WBR) {
    } else if (flex_item && s->float_ == WF_NONE && s->position != WP_ABSOLUTE && s->position != WP_FIXED) {
        flow_atom(I, n, 1);
    } else if (s->float_ != WF_NONE && (s->position == WP_STATIC || s->position == WP_RELATIVE || s->position == WP_STICKY)) {
        place_float(I, n);
    } else if (n->tag == WT_IMG || is_control(n) || s->display == WD_INLINE_BLOCK || s->display == WD_INLINE_TABLE || s->display == WD_INLINE_FLEX) {
        flow_atom(I, n, 0);
    } else if (is_blockish(s) || s->position == WP_ABSOLUTE || s->position == WP_FIXED) {
        flow_block(I, n);
    } else {
        /* inline element: margins/padding/borders on the sides, decorations and background on the text */
        box b; box_of(s, I->x1 - I->x0, &b);
        int32_t left = (b.auto_l ? 0 : b.ml) + b.bl + b.pl, right = (b.auto_r ? 0 : b.mr) + b.br + b.pr;
        uint8_t saved_deco = L->deco;
        L->deco |= s->decoration;
        if (left > 0) add_gap(I, left);
        flow_inline_children(I, n);
        if (right > 0) add_gap(I, right);
        L->deco = saved_deco;
    }
    --L->depth;
    L->link = saved_link;
    L->invisible = saved_inv;
}

static void flow_children(wnode *n, int32_t x0, int32_t x1, int32_t *y, int nested) {
    (void)nested;
    if (L->nifc >= MAX_DEPTH + 3) return;
    ifc *I = &L->ifcs[L->nifc++];
    wb_set(I, 0, sizeof(*I));
    I->x0 = x0; I->x1 = x1; I->y = *y; I->st = n->style; I->first = 1;
    I->align = n->style->text_align;
    I->base = L->nfrags;
    if ((n->style->display == WD_FLEX || n->style->display == WD_INLINE_FLEX) && !n->style->flex_column) {
        I->flex = 1; I->gap = n->style->gap;
        switch (n->style->justify) {
        case WJ_CENTER: I->align = WA_CENTER; break;
        case WJ_END: I->align = WA_RIGHT; break;
        case WJ_BETWEEN: I->align = 3; break;
        case WJ_AROUND: I->align = 4; break;
        default: I->align = n->style->text_align == WA_CENTER ? WA_CENTER : WA_LEFT; break;
        }
    }
    for (wnode *c = n->first; c; c = c->next) flow(I, c);
    line_end(I, 1);
    ifc_apply_margin(I);
    *y = I->y;
    --L->nifc;
}

/* ------------------------------------------------------------------ tables */
typedef struct tcell { wnode *n; uint16_t row, col, rs, cs; int32_t mn, mx, pct; boxres r; int32_t y; } tcell;

static int is_row(const wnode *n) { return is_elem(n) && (n->style->display == WD_TABLE_ROW || n->tag == WT_TR); }
static int is_group(const wnode *n) { return is_elem(n) && (n->style->display == WD_TABLE_ROW_GROUP || n->tag == WT_TBODY || n->tag == WT_THEAD || n->tag == WT_TFOOT); }
static int is_cell(const wnode *n) { return is_elem(n) && n->style->display != WD_NONE && (n->style->display == WD_TABLE_CELL || n->tag == WT_TD || n->tag == WT_TH); }

/* Collects the rows of table t (through row groups and stray wrappers like <form>). */
static uint32_t table_rows(wnode *t, wnode **rows, uint32_t max) {
    uint32_t n = 0;
    /* thead first, then bodies and loose rows in order, tfoot last */
    for (int pass = 0; pass < 3; ++pass) {
        for (wnode *c = t->first; c && n < max; c = c->next) {
            if (!is_elem(c) || c->style->display == WD_NONE) continue;
            int head = c->tag == WT_THEAD, foot = c->tag == WT_TFOOT;
            if ((pass == 0 && !head) || (pass == 2 && !foot) || (pass == 1 && (head || foot))) continue;
            if (is_row(c)) rows[n++] = c;
            else if (is_group(c)) { for (wnode *r = c->first; r && n < max; r = r->next) if (is_row(r) && r->style->display != WD_NONE) rows[n++] = r; }
            else if (c->style->display != WD_TABLE_CAPTION && c->style->display != WD_TABLE_COLUMN && c->tag != WT_TABLE) {
                for (wnode *r = c->first; r && n < max; r = r->next) {
                    if (is_row(r)) rows[n++] = r;
                    else if (is_group(r)) for (wnode *q = r->first; q && n < max; q = q->next) if (is_row(q)) rows[n++] = q;
                }
            }
        }
    }
    return n;
}

typedef struct tgrid { wnode **rows; uint32_t nrows; tcell *cells; uint32_t ncells; uint32_t ncols; } tgrid;

static int build_grid(wnode *t, tgrid *g) {
    uint32_t maxrows = 4096;
    g->rows = (wnode **)warena_alloc(&L->scratch, maxrows * sizeof(wnode *));
    if (!g->rows) return 0;
    g->nrows = table_rows(t, g->rows, maxrows);
    uint32_t count = 0;
    for (uint32_t r = 0; r < g->nrows; ++r) for (wnode *c = g->rows[r]->first; c; c = c->next) if (is_cell(c)) ++count;
    g->cells = count ? (tcell *)warena_alloc(&L->scratch, count * sizeof(tcell)) : 0;
    if (count && !g->cells) return 0;
    uint8_t *occ = g->nrows ? (uint8_t *)warena_alloc(&L->scratch, (size_t)g->nrows * MAX_COLS) : 0;
    if (g->nrows && !occ) return 0;
    g->ncells = 0; g->ncols = 0;
    for (uint32_t r = 0; r < g->nrows; ++r) {
        uint32_t col = 0;
        for (wnode *c = g->rows[r]->first; c; c = c->next) {
            if (!is_cell(c)) continue;
            while (col < MAX_COLS && occ[(size_t)r * MAX_COLS + col]) ++col;
            if (col >= MAX_COLS) break;
            int32_t cs = attr_int(c, "colspan", 1), rs = attr_int(c, "rowspan", 1);
            if (cs < 1) cs = 1;
            if (cs > (int32_t)(MAX_COLS - col)) cs = (int32_t)(MAX_COLS - col);
            if (rs < 1) rs = 1;
            if (rs > (int32_t)(g->nrows - r)) rs = (int32_t)(g->nrows - r);
            tcell *tc = &g->cells[g->ncells++];
            wb_set(tc, 0, sizeof(*tc));
            tc->n = c; tc->row = (uint16_t)r; tc->col = (uint16_t)col; tc->cs = (uint16_t)cs; tc->rs = (uint16_t)rs;
            for (int32_t rr = 0; rr < rs; ++rr) for (int32_t cc = 0; cc < cs; ++cc) occ[(size_t)(r + (uint32_t)rr) * MAX_COLS + col + (uint32_t)cc] = 1;
            col += (uint32_t)cs;
            if (col > g->ncols) g->ncols = col;
        }
    }
    return 1;
}

/* Column widths for table t inside avail; returns min/max table widths (content + spacing). */
static void table_columns(wnode *t, int32_t avail, int32_t *cols_out, uint32_t *ncols_out, int32_t *tmin, int32_t *tmax, int compute) {
    tgrid g;
    *tmin = *tmax = 0; *ncols_out = 0;
    if (!build_grid(t, &g) || !g.ncols) return;
    const wstyle *ts = t->style;
    int32_t sp = ts->collapse ? 0 : ts->border_spacing;
    int32_t cmin[MAX_COLS], cmax[MAX_COLS], cpct[MAX_COLS];
    for (uint32_t c = 0; c < g.ncols; ++c) { cmin[c] = 0; cmax[c] = 0; cpct[c] = 0; }
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < g.ncells; ++i) {
            tcell *tc = &g.cells[i];
            if ((pass == 0) != (tc->cs == 1)) continue;
            int32_t mn, mx;
            intrinsic(tc->n, &mn, &mx);
            const wstyle *cs = tc->n->style;
            if (cs->width.u == WU_PCT && tc->cs == 1) { if (cs->width.v > cpct[tc->col]) cpct[tc->col] = cs->width.v; }
            if (cs->white_space == WS_NOWRAP) mn = mx;
            if (tc->cs == 1) {
                if (mn > cmin[tc->col]) cmin[tc->col] = mn;
                if (mx > cmax[tc->col]) cmax[tc->col] = mx;
            } else {
                int32_t smin = sp * (tc->cs - 1), smax = sp * (tc->cs - 1);
                for (uint32_t c = tc->col; c < (uint32_t)tc->col + tc->cs; ++c) { smin += cmin[c]; smax += cmax[c]; }
                if (mn > smin) { int32_t d = (mn - smin) / tc->cs + 1; for (uint32_t c = tc->col; c < (uint32_t)tc->col + tc->cs; ++c) cmin[c] += d; }
                if (mx > smax) { int32_t d = (mx - smax) / tc->cs + 1; for (uint32_t c = tc->col; c < (uint32_t)tc->col + tc->cs; ++c) cmax[c] += d; }
            }
        }
    }
    int32_t smin = sp * ((int32_t)g.ncols + 1), smax = smin;
    for (uint32_t c = 0; c < g.ncols; ++c) { if (cmax[c] < cmin[c]) cmax[c] = cmin[c]; smin += cmin[c]; smax += cmax[c]; }
    *tmin = smin; *tmax = smax; *ncols_out = g.ncols;
    if (!compute || !cols_out) return;
    int32_t A = avail - sp * ((int32_t)g.ncols + 1);
    int32_t sum_min = smin - sp * ((int32_t)g.ncols + 1), sum_max = smax - sp * ((int32_t)g.ncols + 1);
    if (A < sum_min) A = sum_min;
    /* percentage columns first */
    int32_t fixed = 0; uint32_t free_cols = 0;
    for (uint32_t c = 0; c < g.ncols; ++c) {
        if (cpct[c]) {
            int32_t w = (int32_t)((int64_t)A * cpct[c] / 6400);
            if (w < cmin[c]) w = cmin[c];
            cols_out[c] = w; fixed += w;
        } else { cols_out[c] = -1; ++free_cols; }
    }
    int32_t rest = A - fixed;
    int32_t fmin = 0, fmax = 0;
    for (uint32_t c = 0; c < g.ncols; ++c) if (cols_out[c] < 0) { fmin += cmin[c]; fmax += cmax[c]; }
    for (uint32_t c = 0; c < g.ncols; ++c) {
        if (cols_out[c] >= 0) continue;
        int32_t w;
        if (rest >= fmax) {
            int32_t extra = rest - fmax;
            w = cmax[c] + (fmax ? (int32_t)((int64_t)extra * cmax[c] / fmax) : extra / (int32_t)free_cols);
        } else if (rest <= fmin) w = cmin[c];
        else w = cmin[c] + (int32_t)((int64_t)(cmax[c] - cmin[c]) * (rest - fmin) / (fmax - fmin ? fmax - fmin : 1));
        cols_out[c] = w;
    }
    (void)sum_max;
}

static void layout_table(wnode *t, int32_t x, int32_t y, int32_t w, int32_t *h_out) {
    const wstyle *ts = t->style;
    box b; box_of(ts, w, &b);
    int32_t sp = ts->collapse ? 0 : ts->border_spacing;
    int32_t ix = x + b.bl + b.pl, iw = w - b.bl - b.br - b.pl - b.pr;
    int32_t cy = y + b.bt + b.pt;
    /* captions */
    for (wnode *c = t->first; c; c = c->next) {
        if (!is_elem(c) || c->style->display != WD_TABLE_CAPTION) continue;
        boxres r; layout_box(c, ix, cy, iw, &r);
        cy += r.h;
    }
    int32_t *cols = (int32_t *)warena_alloc(&L->scratch, MAX_COLS * sizeof(int32_t));
    int32_t tmin, tmax; uint32_t ncols;
    if (!cols) { *h_out = cy - y; return; }
    table_columns(t, iw, cols, &ncols, &tmin, &tmax, 1);
    tgrid g;
    if (!ncols || !build_grid(t, &g)) { *h_out = cy - y + b.pb + b.bb; return; }
    int32_t colx[MAX_COLS + 1];
    colx[0] = ix + sp;
    for (uint32_t c = 0; c < ncols; ++c) colx[c + 1] = colx[c] + cols[c] + sp;
    int32_t *rowy = (int32_t *)warena_alloc(&L->scratch, ((size_t)g.nrows + 1) * sizeof(int32_t));
    if (!rowy) { *h_out = cy - y; return; }
    int32_t ry = cy + sp;
    uint32_t ci = 0;
    for (uint32_t r = 0; r < g.nrows; ++r) {
        rowy[r] = ry;
        int32_t rh = 0;
        const wstyle *rs = g.rows[r]->style;
        uint32_t saved_row_bg = L->row_bg;
        L->row_bg = (rs->bg >> 24) ? rs->bg : (g.rows[r]->parent && g.rows[r]->parent->style && (g.rows[r]->parent->style->bg >> 24) && g.rows[r]->parent != t) ? g.rows[r]->parent->style->bg : 0;
        uint32_t first_cell = ci;
        while (ci < g.ncells && g.cells[ci].row == r) {
            tcell *tc = &g.cells[ci];
            int32_t cw = colx[tc->col + tc->cs] - colx[tc->col] - sp;
            layout_box(tc->n, colx[tc->col], ry, cw, &tc->r);
            tc->y = ry;
            if (tc->rs == 1 && tc->r.h > rh) rh = tc->r.h;
            ++ci;
        }
        L->row_bg = saved_row_bg;
        if (rs->height.u == WU_PX && rs->height.v / 64 > rh) rh = rs->height.v / 64;
        /* rowspans ending here */
        for (uint32_t k = 0; k < ci; ++k) {
            tcell *tc = &g.cells[k];
            if (tc->rs > 1 && tc->row + tc->rs - 1 == r) {
                int32_t have = ry + rh - tc->y;
                if (tc->r.h > have) rh += tc->r.h - have;
            }
        }
        /* stretch and align the cells that end in this row */
        for (uint32_t k = 0; k < ci; ++k) {
            tcell *tc = &g.cells[k];
            if ((uint32_t)(tc->row + tc->rs - 1) != r) continue;
            int32_t full = ry + rh - tc->y;
            int32_t off = 0;
            uint8_t va = tc->n->style->valign;
            if (va == WV_BASELINE && g.rows[r]->style->valign != WV_BASELINE) va = g.rows[r]->style->valign;
            if (va == WV_MIDDLE || va == WV_BASELINE) off = (full - tc->r.h) / 2;
            else if (va == WV_BOTTOM) off = full - tc->r.h;
            if (va == WV_BASELINE) off = 0;
            if (off > 0) translate(tc->r.content_first, tc->r.content_end, 0, off);
            if (tc->r.bg != UINT32_MAX) IT(tc->r.bg)->h = full;
            if (tc->r.border != UINT32_MAX) { IT(tc->r.border)->h = full; IT(tc->r.border)->y = tc->y; }
        }
        (void)first_cell;
        ry += rh + sp;
    }
    rowy[g.nrows] = ry;
    *h_out = ry - y + b.pb + b.bb;
}

/* ------------------------------------------------------------------ entry */
static int img_state_none(void *c, int32_t i) { (void)c; (void)i; return -1; }

int wlayout_run(wdoc *doc, const wlayout_env *env, wlayout_out *o) {
    static lctx ctx;
    static uint32_t generation;
    wlayout_free(o);
    wb_set(&ctx, 0, sizeof(ctx));
    L = &ctx;
    ctx.doc = doc; ctx.env = env; ctx.o = o; ctx.link = -1;
    ctx.gen = ++generation;
    warena_init(&ctx.scratch);
    wlayout_env fallback = *env;
    if (!fallback.image_state) fallback.image_state = img_state_none;
    ctx.env = &fallback;
    o->page_bg = 0xFFFFFFFFu;
    o->page_color = 0xFF000000u;
    wnode *html = doc->html, *body = doc->body;
    if (html && html->style && (html->style->bg >> 24)) o->page_bg = html->style->bg | 0xFF000000u;
    else if (body && body->style && (body->style->bg >> 24)) o->page_bg = body->style->bg | 0xFF000000u;
    if (body && body->style) o->page_color = body->style->color;
    int32_t vw = env->viewport_w > 100 ? env->viewport_w : 100;
    int32_t y = 0;
    if (html && html->style && html->style->display != WD_NONE) {
        boxres r;
        box b; box_of(html->style, vw, &b);
        layout_box(html, b.ml, b.mt, vw - b.ml - b.mr, &r);
        y = r.h + b.mt + b.mb;
    }
    int32_t fb = floats_bottom();
    if (fb > y) y = fb;
    o->height = y;
    o->width = vw;
    /* final positions of form controls (for hit testing and focus rings) */
    for (uint32_t i = 0; i < o->count; ++i) {
        witem *it = &o->items[i];
        if (it->kind == WI_CTL && it->a < doc->ctl_count) {
            wctl *c = &doc->ctls[it->a];
            c->x = it->x; c->y = it->y; c->w = it->w; c->h = it->h;
        }
        if (it->x + it->w > o->width && it->kind) o->width = it->x + it->w;
    }
    wmem_free(ctx.frags);
    warena_free(&ctx.scratch);
    L = 0;
    return o->oom ? -1 : 0;
}

void wlayout_free(wlayout_out *o) {
    wmem_free(o->items); wmem_free(o->pool);
    wb_set(o, 0, sizeof(*o));
}
