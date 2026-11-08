#ifndef MINTOS26_HEAP_H
#define MINTOS26_HEAP_H
#include <stdint.h>
#include <stddef.h>

/* Single-core, cooperative-only heap for kernel allocations. Not interrupt-safe
 * and not callable from the IRQ path; add locking once preemption exists. */
int mint_heap_init(void *base, uint64_t bytes);
void *mint_kmalloc(uint64_t bytes);
int mint_kfree(void *ptr);
uint64_t mint_heap_capacity(void);
uint64_t mint_heap_used(void);
uint64_t mint_heap_free_bytes(void);

#endif
