/* wc: count lines, words and bytes (MintOS SDK tool). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int show_l, show_w, show_c;
static unsigned long tl, tw, tc;

static void report(unsigned long l, unsigned long w, unsigned long c, const char *name) {
    if (show_l) printf("%8lu", l);
    if (show_w) printf("%8lu", w);
    if (show_c) printf("%8lu", c);
    if (name) printf(" %s", name);
    putchar('\n');
}

static int count(FILE *f, const char *name) {
    unsigned long l = 0, w = 0, c = 0;
    int ch, in_word = 0;
    while ((ch = getc(f)) != EOF) {
        ++c;
        if (ch == '\n') ++l;
        if (isspace(ch)) in_word = 0;
        else if (!in_word) { in_word = 1; ++w; }
    }
    if (ferror(f)) { perror(name ? name : "stdin"); return 1; }
    report(l, w, c, name);
    tl += l; tw += w; tc += c;
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, status = 0, files = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
        for (const char *o = argv[i] + 1; *o; ++o) {
            if (*o == 'l') show_l = 1; else if (*o == 'w') show_w = 1; else if (*o == 'c') show_c = 1;
            else { fprintf(stderr, "usage: wc [-lwc] [file...]\n"); return 2; }
        }
    }
    if (!show_l && !show_w && !show_c) show_l = show_w = show_c = 1;
    if (i == argc) return count(stdin, 0);
    for (; i < argc; ++i, ++files) {
        FILE *f = fopen(argv[i], "r");
        if (!f) { perror(argv[i]); status = 1; continue; }
        status |= count(f, argv[i]);
        fclose(f);
    }
    if (files > 1) report(tl, tw, tc, "total");
    return status;
}
