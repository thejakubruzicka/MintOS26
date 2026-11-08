#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../kernel/include/heap.h"

int main(void) {
    const size_t arena_bytes = 64u * 1024u;
    void *arena = aligned_alloc(4096, arena_bytes);
    assert(arena);
    assert(mint_heap_init(arena, arena_bytes));
    assert(mint_heap_capacity() == arena_bytes);
    assert(mint_kmalloc(0) == 0);
    uint8_t *a = mint_kmalloc(128);
    uint8_t *b = mint_kmalloc(512);
    uint8_t *c = mint_kmalloc(4096);
    assert(a && b && c && a < b && b < c);
    assert(((uintptr_t)a & 15u) == 0 && ((uintptr_t)b & 15u) == 0 && ((uintptr_t)c & 15u) == 0);
    for (uint32_t i = 0; i < 128; ++i) a[i] = (uint8_t)i;
    for (uint32_t i = 0; i < 512; ++i) b[i] = (uint8_t)(i ^ 0x5a);
    assert(mint_kfree(b));
    uint8_t *d = mint_kmalloc(256);
    assert(d == b); /* first-fit reuses and splits the freed middle block */
    assert(!mint_kfree((void *)(uintptr_t)0x1234));
    assert(mint_kfree(a));
    assert(mint_kfree(c));
    assert(mint_kfree(d));
    assert(!mint_kfree(d)); /* no double free */
    assert(mint_heap_used() == 0);
    void *large = mint_kmalloc(60u * 1024u);
    assert(large); /* adjacent free blocks coalesced */
    assert(mint_kfree(large));
    assert(mint_heap_used() == 0);
    free(arena);
    puts("PASS: kernel heap aligns allocations, splits/reuses blocks, rejects invalid/double frees, coalesces, and returns used bytes to zero");
    return 0;
}
