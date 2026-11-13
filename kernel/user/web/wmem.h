#ifndef MINT_WEB_WMEM_H
#define MINT_WEB_WMEM_H
/* Bounded heap for the Web engine (MintDE has no malloc).
 *
 * wmem_*: a boundary-tag first-fit allocator with coalescing over one static
 * arena; every allocation is 16-byte aligned and failure returns NULL (the
 * engine degrades: truncated page, missing image) instead of crashing.
 * warena_*: bump allocator for per-document data (DOM, styles, display
 * list text) built from heap chunks and released all at once.
 * Portable C: the same file runs in the host tests. */
#include <stddef.h>
#include <stdint.h>

void wmem_init(void *arena, size_t size);
void *wmem_alloc(size_t n);
void *wmem_calloc(size_t n);
void *wmem_realloc(void *p, size_t n);
void wmem_free(void *p);
size_t wmem_capacity(void);
size_t wmem_used(void);          /* bytes in live blocks (incl. headers) */
size_t wmem_peak(void);
size_t wmem_largest_free(void);
/* Consistency walk (tests): 1 if every block and the free list are sane. */
int wmem_check(void);

typedef struct warena_chunk warena_chunk;
typedef struct warena {
    warena_chunk *head;
    size_t total;                /* bytes obtained from the heap */
    int failed;                  /* an allocation failed since init */
} warena;
void warena_init(warena *a);
void *warena_alloc(warena *a, size_t n);       /* 8-byte aligned, zeroed */
char *warena_strndup(warena *a, const char *s, size_t n);
void warena_free(warena *a);

/* Small helpers shared by the engine (no libc). */
void wb_copy(void *d, const void *s, size_t n);
void wb_set(void *d, int v, size_t n);
size_t wstrlen(const char *s);
#endif
