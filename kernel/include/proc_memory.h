#ifndef MINTOS26_PROC_MEMORY_H
#define MINTOS26_PROC_MEMORY_H
/* 26.5.014.0: anonymous memory for user processes (mmap/munmap/mprotect/brk).
 * Mappings are demand-zero regions in the process address space; pages are
 * allocated on first touch and count against the process's owned-page limit
 * (default 16384 pages = 64 MiB). Writable+executable mappings are refused. */
#include <stdint.h>
#include "process.h"

#define MINT_MMAP_LOW  0x0000002000000000ULL   /* 128 GiB */
#define MINT_MMAP_MAX_LENGTH (4ULL * 1024ULL * 1024ULL * 1024ULL)
#define MINT_BRK_MAX (1024ULL * 1024ULL * 1024ULL)

/* prot uses MINT_MEM_READ/WRITE/EXEC; 0 = no access. Returns the address or
 * a negative MINT_E* value. `fixed` maps exactly at hint, replacing older
 * anonymous mappings there. */
int64_t mint_proc_mmap(MINT_PROCESS *p, uint64_t hint, uint64_t length, uint32_t prot, int fixed);
int64_t mint_proc_munmap(MINT_PROCESS *p, uint64_t address, uint64_t length);
int64_t mint_proc_mprotect(MINT_PROCESS *p, uint64_t address, uint64_t length, uint32_t prot);
/* Returns the (new) program break; on failure the unchanged break. */
uint64_t mint_proc_brk(MINT_PROCESS *p, uint64_t new_end);
#endif
