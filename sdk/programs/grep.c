/* grep: print lines matching a POSIX regular expression (MintOS SDK tool).
 * Options: -i ignore case, -v invert, -n line numbers, -c count, -E extended
 * regex, -F fixed string. Uses musl's regcomp/regexec. */
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int opt_i, opt_v, opt_n, opt_c, opt_E, opt_F;
static regex_t re;
static const char *fixed;

static int matches(const char *line) {
    if (opt_F) {
        size_t n = strlen(fixed);
        for (const char *p = line; *p; ++p)
            if ((opt_i ? strncasecmp(p, fixed, n) : strncmp(p, fixed, n)) == 0) return 1;
        return n == 0;
    }
    return regexec(&re, line, 0, 0, 0) == 0;
}

static int grep_file(FILE *f, const char *name, int show_name) {
    static char line[8192];
    unsigned long lineno = 0, hits = 0;
    while (fgets(line, sizeof line, f)) {
        ++lineno;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[n - 1] = '\0';
        if (matches(line) == !opt_v) {
            ++hits;
            if (opt_c) continue;
            if (show_name) printf("%s:", name);
            if (opt_n) printf("%lu:", lineno);
            puts(line);
        }
    }
    if (opt_c) { if (show_name) printf("%s:", name); printf("%lu\n", hits); }
    return hits ? 0 : 1;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
        if (!strcmp(argv[i], "--")) { ++i; break; }
        for (const char *o = argv[i] + 1; *o; ++o) {
            switch (*o) {
            case 'i': opt_i = 1; break; case 'v': opt_v = 1; break; case 'n': opt_n = 1; break;
            case 'c': opt_c = 1; break; case 'E': opt_E = 1; break; case 'F': opt_F = 1; break;
            default: fprintf(stderr, "usage: grep [-ivncEF] pattern [file...]\n"); return 2;
            }
        }
    }
    if (i >= argc) { fprintf(stderr, "usage: grep [-ivncEF] pattern [file...]\n"); return 2; }
    const char *pattern = argv[i++];
    if (opt_F) fixed = pattern;
    else {
        int rc = regcomp(&re, pattern, (opt_E ? REG_EXTENDED : 0) | (opt_i ? REG_ICASE : 0) | REG_NOSUB);
        if (rc) { char msg[128]; regerror(rc, &re, msg, sizeof msg); fprintf(stderr, "grep: %s\n", msg); return 2; }
    }
    if (i == argc) return grep_file(stdin, "(stdin)", 0);
    int status = 1, many = argc - i > 1;
    for (; i < argc; ++i) {
        FILE *f = fopen(argv[i], "r");
        if (!f) { perror(argv[i]); status = 2; continue; }
        if (grep_file(f, argv[i], many) == 0 && status != 2) status = 0;
        fclose(f);
    }
    return status;
}
