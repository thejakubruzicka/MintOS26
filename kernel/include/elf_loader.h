#ifndef MINTOS26_ELF_LOADER_H
#define MINTOS26_ELF_LOADER_H
#include <stddef.h>
#include <stdint.h>
#include "address_space.h"

#define MINT_ELF64_MAX_SEGMENTS 16u
/* 26.5.012.0: 64 KiB (the loader maximum); TLS certificate checks and the
 * Web layout need more than the previous 16 KiB. */
#define MINT_USER_DEFAULT_STACK_BYTES (16u * 4096u)
/* 26.5.014.0: stacks are demand-zero, so programs get 8 MiB of stack address
 * space (pages are allocated only when touched) above a 64 KiB guard region. */
#define MINT_USER_MAX_STACK_BYTES (8u * 1024u * 1024u)
#define MINT_USER_PROGRAM_STACK_BYTES MINT_USER_MAX_STACK_BYTES
#define MINT_USER_STACK_GUARD_BYTES (64u * 1024u)
/* Static-PIE (ET_DYN without an interpreter) images are placed here. */
#define MINT_USER_PIE_BASE 0x0000001000000000ULL

typedef struct mint_user_image {
    uint64_t entry;
    uint64_t image_base;
    uint64_t image_end;
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t stack_size;
    uint32_t segment_count;
    uint32_t loaded;
    uint64_t load_bias;  /* 0 for ET_EXEC */
    uint64_t phdr;       /* user address of the program headers, 0 if not loaded */
    uint64_t phnum;
} MINT_USER_IMAGE;

/* Load a static ELF64 ET_EXEC image into a freshly-created, inactive address
 * space. Supports bounded PT_LOAD segments only, rejects dynamic linking and
 * W+X segments, zeroes BSS, and maps a private NX stack. If loading fails after
 * allocation begins, the whole address space is destroyed to guarantee rollback.
 */
int mint_elf64_load_user(MINT_ADDRESS_SPACE *space, const void *image,
                         size_t image_size, size_t stack_bytes,
                         MINT_USER_IMAGE *out);

/* Same loader over a byte reader (a MintOS HD file): reader copies `length`
 * bytes at `offset` of the image into `out` and returns 1 on success. */
typedef int (*mint_elf_reader_t)(void *context, uint64_t offset, void *out, size_t length);
int mint_elf64_load_user_reader(MINT_ADDRESS_SPACE *space, mint_elf_reader_t reader, void *context,
                                size_t image_size, size_t stack_bytes, MINT_USER_IMAGE *out);

#endif
