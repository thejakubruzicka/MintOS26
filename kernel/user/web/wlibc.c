/* The few C library functions vendored code (stb_image) needs inside MintDE,
 * which has no libc. Built only for MintOS; host tests use the system libc. */
#include <stddef.h>

int abs(int v) { return v < 0 ? -v : v; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
long strtol(const char *s, char **end, int base) {
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if (!base) base = 10;
    for (;;) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'z') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'Z') ? *s - 'A' + 10 : 99;
        if (d >= base) break;
        if (v < 0x7FFFFFFFFFFFL / 36) v = v * base + d;
        ++s;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
