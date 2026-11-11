/* Minimal <string.h> for vendored code built freestanding inside MintDE
 * (memcpy/memset/memmove/memcmp come from kernel/lib/mem.c). */
#ifndef MINT_WEB_SHIM_STRING_H
#define MINT_WEB_SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
#endif
