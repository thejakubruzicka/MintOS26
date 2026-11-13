#ifndef MINTOS26_USER_PROC_H
#define MINTOS26_USER_PROC_H
/* 26.5.014.0: MintOS-native process, pipe and memory calls for ring-3 code
 * built on the int 0x80 runtime (MintDE). Programs built with the SDK use
 * musl and the Linux-compat subset instead. */
#include <stddef.h>
#include <stdint.h>
#include <uapi/mintos/syscall.h>
#include <uapi/mintos/process.h>

int64_t mint_proc_spawn(MINT_SPAWN_REQUEST *request);
int64_t mint_proc_wait(int64_t pid, MINT_PROC_STATUS *status, uint32_t options);
int64_t mint_proc_kill(uint64_t pid, uint32_t signal);
int64_t mint_proc_list(MINT_PROC_INFO *out, uint32_t capacity);
int64_t mint_fd_read(int64_t fd, void *buffer, size_t length, uint32_t flags);
int64_t mint_fd_write(int64_t fd, const void *buffer, size_t length, uint32_t flags);
int64_t mint_fd_close(int64_t fd);
int64_t mint_platform_info(MINT_PLATFORM_INFO *out);
int64_t mint_programs_install(void);
#endif
