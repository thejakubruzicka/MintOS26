#ifndef MINTOS26_PROC_SYSCALLS_H
#define MINTOS26_PROC_SYSCALLS_H
#include <stdint.h>
#include "uapi/mintos/process.h"

#define MINT_SYS_APP_PLATFORM_FIRST 0x4D60u
#define MINT_SYS_APP_PLATFORM_LAST  0x4D7Fu
/* Handles MintOS process/pipe/memory calls for the current process. */
int64_t mint_proc_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);
void mint_platform_info_fill(MINT_PLATFORM_INFO *out);
#endif
