#include <stdint.h>
#include <stddef.h>
#include "../include/heap.h"

#define HEAP_MAGIC 0x4D494E5448454150ULL /* MINTHEAP */
#define HEAP_ALIGNMENT 16ULL

typedef struct heap_block {
    uint64_t magic;
    uint64_t size;
    uint32_t free;
    uint32_t reserved;
    struct heap_block *next;
} heap_block_t;

static uint8_t *heap_start;
static uint8_t *heap_end;
static heap_block_t *first_block;
static uint64_t heap_total;
static uint64_t heap_used_bytes;

static uint64_t align_size(uint64_t size) {
    if (size > UINT64_MAX - (HEAP_ALIGNMENT - 1u)) return 0;
    return (size + HEAP_ALIGNMENT - 1u) & ~(HEAP_ALIGNMENT - 1u);
}

int mint_heap_init(void *base, uint64_t bytes) {
    uintptr_t b = (uintptr_t)base;
    if (!base || (b & (HEAP_ALIGNMENT - 1u)) ||
        bytes < sizeof(heap_block_t) + HEAP_ALIGNMENT ||
        bytes > UINTPTR_MAX - b) return 0;
    heap_start = (uint8_t *)base;
    heap_end = heap_start + bytes;
    heap_total = bytes;
    heap_used_bytes = 0;
    first_block = (heap_block_t *)heap_start;
    first_block->magic = HEAP_MAGIC;
    first_block->size = bytes - sizeof(heap_block_t);
    first_block->free = 1;
    first_block->reserved = 0;
    first_block->next = 0;
    return 1;
}

void *mint_kmalloc(uint64_t bytes) {
    if (!first_block || !bytes) return 0;
    uint64_t need = align_size(bytes);
    if (!need) return 0;
    for (heap_block_t *b = first_block; b; b = b->next) {
        if (b->magic != HEAP_MAGIC) return 0; /* detect metadata corruption */
        if (!b->free || b->size < need) continue;
        uint64_t remainder = b->size - need;
        if (remainder >= sizeof(heap_block_t) + HEAP_ALIGNMENT) {
            uint8_t *new_addr = (uint8_t *)(b + 1) + need;
            heap_block_t *split = (heap_block_t *)new_addr;
            split->magic = HEAP_MAGIC;
            split->size = remainder - sizeof(heap_block_t);
            split->free = 1;
            split->reserved = 0;
            split->next = b->next;
            b->next = split;
            b->size = need;
        }
        b->free = 0;
        heap_used_bytes += b->size;
        return (void *)(b + 1);
    }
    return 0;
}

static int block_is_inside_heap(const heap_block_t *b) {
    uintptr_t p = (uintptr_t)b;
    return heap_start && p >= (uintptr_t)heap_start &&
           p <= (uintptr_t)heap_end - sizeof(heap_block_t) &&
           (p & (HEAP_ALIGNMENT - 1u)) == 0;
}

int mint_kfree(void *ptr) {
    if (!ptr || !heap_start) return 0;
    uintptr_t p = (uintptr_t)ptr;
    if (p < (uintptr_t)heap_start + sizeof(heap_block_t) || p >= (uintptr_t)heap_end) return 0;
    heap_block_t *b = ((heap_block_t *)ptr) - 1;
    if (!block_is_inside_heap(b) || b->magic != HEAP_MAGIC || b->free ||
        b->size > (uint64_t)((uint8_t *)heap_end - (uint8_t *)(b + 1))) return 0;
    b->free = 1;
    if (heap_used_bytes >= b->size) heap_used_bytes -= b->size;
    else heap_used_bytes = 0;
    /* Coalesce adjacent free blocks to avoid unnecessary fragmentation. */
    for (heap_block_t *cur = first_block; cur && cur->next;) {
        if (cur->free && cur->next->free && cur->next->magic == HEAP_MAGIC) {
            cur->size += sizeof(heap_block_t) + cur->next->size;
            cur->next = cur->next->next;
        } else cur = cur->next;
    }
    return 1;
}

uint64_t mint_heap_capacity(void) { return heap_total; }
uint64_t mint_heap_used(void) { return heap_used_bytes; }
uint64_t mint_heap_free_bytes(void) {
    return heap_total > heap_used_bytes ? heap_total - heap_used_bytes : 0;
}
