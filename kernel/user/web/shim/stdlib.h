/* Minimal <stdlib.h> for vendored code built freestanding inside MintDE
 * (kernel/user/web/wlibc.c). Only what stb_image references. */
#ifndef MINT_WEB_SHIM_STDLIB_H
#define MINT_WEB_SHIM_STDLIB_H
#include <stddef.h>
int abs(int v);
long strtol(const char *s, char **end, int base);
#endif
