#ifndef MINTOS26_ELF_LOADER_H
#define MINTOS26_ELF_LOADER_H
#include <stddef.h>
#include <stdint.h>
#include "address_space.h"

#define MINT_ELF64_MAX_SEGMENTS 16u
#define MINT_USER_DEFAULT_STACK_BYTES (4u * 4096u)

typedef struct mint_user_image {
    uint64_t entry;
    uint64_t image_base;
    uint64_t image_end;
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t stack_size;
    uint32_t segment_count;
    uint32_t loaded;
} MINT_USER_IMAGE;

/* Load a static ELF64 ET_EXEC image into a freshly-created, inactive address
 * space. Supports bounded PT_LOAD segments only, rejects dynamic linking and
 * W+X segments, zeroes BSS, and maps a private NX stack. If loading fails after
 * allocation begins, the whole address space is destroyed to guarantee rollback.
 */
int mint_elf64_load_user(MINT_ADDRESS_SPACE *space, const void *image,
                         size_t image_size, size_t stack_bytes,
                         MINT_USER_IMAGE *out);

#endif
