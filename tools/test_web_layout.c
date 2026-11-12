/* Host harness: HTML -> DOM -> CSS -> layout with the MintOS Web engine.
 *   test-web-layout FILE.html WIDTH [CSS-FILE...]
 * Linked <link rel=stylesheet> sheets are filled from the CSS files in order.
 * Images named "...WxH..." (e.g. photo-220x160.jpg) report that natural size.
 * Prints document info, every display item and stats; exits non-zero on OOM
 * or heap corruption. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/user/web/wdoc.h"
#include "../kernel/user/web/wlayout.h"
#include "../kernel/user/web/whttp.h"

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f); b[n] = 0; *len = (size_t)n;
    return b;
}

static wdoc doc;
static int img_size(void *ctx, int32_t img, int32_t *w, int32_t *h) {
    (void)ctx;
    if (img < 0 || (uint32_t)img >= doc.img_count) return 0;
    const char *src = wnode_attr(doc.imgs[img], "src");
    if (!src) return 0;
    for (const char *p = src; *p; ++p) {
        int a, b;
        if (*p >= '0' && *p <= '9' && sscanf(p, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) { *w = a; *h = b; return 1; }
    }
    return 0;
}
static int img_state(void *ctx, int32_t img) { int32_t w, h; return img_size(ctx, img, &w, &h) ? 1 : -1; }

int main(int argc, char **argv) {
    static uint8_t arena[48u << 20];
    wmem_init(arena, sizeof(arena));
    if (argc < 3) { fprintf(stderr, "usage: test-web-layout FILE WIDTH [CSS...]\n"); return 2; }
    size_t len;
    char *html = slurp(argv[1], &len);
    if (!html) return 2;
    char cs[32];
    whtml_sniff_charset((const uint8_t *)html, len, cs, sizeof(cs));
    size_t ul;
    char *utf = wcharset_to_utf8((const uint8_t *)html, len, cs, &ul);
    if (whtml_parse(&doc, utf, ul, 0) != 0) { printf("ERR parse\n"); return 1; }
    int css_arg = 3;
    for (uint32_t i = 0; i < doc.sheet_count; ++i) {
        if (doc.sheets[i].kind == WSHEET_LINK && css_arg < argc) {
            size_t cl; char *css = slurp(argv[css_arg++], &cl);
            if (css) wcss_set_sheet_text(&doc, i, css, cl);
        }
    }
    int32_t width = atoi(argv[2]);
    wcss *c = wcss_create(&doc, width, 0);
    if (!c || wcss_apply(c, &doc) != 0) { printf("ERR css\n"); return 1; }
    wcss_stats st; wcss_get_stats(c, &st);
    wlayout_env env = {width, 800, img_size, img_state, 0};
    wlayout_out out; memset(&out, 0, sizeof(out));
    if (wlayout_run(&doc, &env, &out) != 0) { printf("ERR layout\n"); return 1; }
    printf("TITLE %s\n", doc.title);
    printf("DOC nodes=%u elements=%u links=%u images=%u controls=%u sheets=%u truncated=%d\n", doc.node_count, doc.elem_count,
           doc.link_count, doc.img_count, doc.ctl_count, doc.sheet_count, doc.truncated);
    printf("CSS rules=%u decls=%u distinct_styles=%u media_skipped=%u\n", st.rules, st.decls, st.distinct_styles, st.media_skipped);
    printf("PAGE width=%d height=%d bg=%08x items=%u\n", out.width, out.height, out.page_bg, out.count);
    static const char *const kinds[] = {"-", "RECT", "TEXT", "IMAGE", "CTL", "BULLET", "BORDER", "ALT"};
    for (uint32_t i = 0; i < out.count; ++i) {
        witem *it = &out.items[i];
        if (!it->kind) continue;
        printf("%s x=%d y=%d w=%d h=%d font=%d flags=%d color=%08x link=%d", kinds[it->kind], it->x, it->y, it->w, it->h, it->font,
               it->flags, it->color, it->link);
        if (it->kind == WI_TEXT) printf(" text=\"%s\"", out.pool + it->a);
        if (it->kind == WI_IMAGE || it->kind == WI_CTL) printf(" index=%u", it->a);
        printf("\n");
    }
    for (uint32_t i = 0; i < doc.ctl_count; ++i) {
        wctl *k = &doc.ctls[i];
        printf("CONTROL %u type=%d name=%s value=\"%s\" x=%d y=%d w=%d h=%d form=%d opts=%d sel=%d\n", i, k->type, k->name ? k->name : "",
               k->value ? k->value : "", k->x, k->y, k->w, k->h, k->form ? (int)k->form->order : -1, k->nopts, k->selected);
    }
    for (uint32_t i = 0; i < doc.link_count; ++i) printf("LINK %u %s\n", i, doc.links[i]);
    wlayout_free(&out);
    wcss_free(c);
    wdoc_free(&doc);
    wmem_free(utf);
    printf("HEAP used=%zu peak=%zu ok=%d\n", wmem_used(), wmem_peak(), wmem_check());
    return wmem_check() && wmem_used() == 0 ? 0 : 1;
}
