/* Freestanding memory primitives. Clang may emit calls to these for struct
 * copies even with -ffreestanding; string instructions keep the compiler
 * from turning the implementations back into calls to themselves. */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    void *d = dst; const void *s = src;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}
void *memset(void *dst, int value, size_t n) {
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(value) : "memory");
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    if ((uintptr_t)dst <= (uintptr_t)src || (uintptr_t)dst >= (uintptr_t)src + n) return memcpy(dst, src, n);
    unsigned char *d = (unsigned char *)dst + n - 1; const unsigned char *s = (const unsigned char *)src + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const volatile unsigned char *x = (const volatile unsigned char *)a, *y = (const volatile unsigned char *)b;
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
