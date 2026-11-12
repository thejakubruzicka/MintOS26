/* 26.5.014.0: int 0x80 wrappers for the app-platform services (proc.h). */
#include "include/mintos/proc.h"

static inline int64_t sys4(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t rax __asm__("rax") = n;
    register uint64_t rdi __asm__("rdi") = a0;
    register uint64_t rsi __asm__("rsi") = a1;
    register uint64_t rdx __asm__("rdx") = a2;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("int $0x80" : "+a"(rax) : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10)
                     : "rcx", "r11", "memory", "cc");
    return (int64_t)rax;
}

int64_t mint_proc_spawn(MINT_SPAWN_REQUEST *r) { return sys4(MINT_SYS_PROC_SPAWN, (uint64_t)(uintptr_t)r, 0, 0, 0); }
int64_t mint_proc_wait(int64_t pid, MINT_PROC_STATUS *s, uint32_t o) {
    return sys4(MINT_SYS_PROC_WAIT, (uint64_t)pid, (uint64_t)(uintptr_t)s, o, 0);
}
int64_t mint_proc_kill(uint64_t pid, uint32_t sig) { return sys4(MINT_SYS_PROC_KILL, pid, sig, 0, 0); }
int64_t mint_proc_list(MINT_PROC_INFO *out, uint32_t cap) { return sys4(MINT_SYS_PROC_LIST, (uint64_t)(uintptr_t)out, cap, 0, 0); }
int64_t mint_fd_read(int64_t fd, void *b, size_t n, uint32_t f) {
    return sys4(MINT_SYS_FD_READ, (uint64_t)fd, (uint64_t)(uintptr_t)b, n, f);
}
int64_t mint_fd_write(int64_t fd, const void *b, size_t n, uint32_t f) {
    return sys4(MINT_SYS_FD_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)b, n, f);
}
int64_t mint_fd_close(int64_t fd) { return sys4(MINT_SYS_FD_CLOSE, (uint64_t)fd, 0, 0, 0); }
int64_t mint_platform_info(MINT_PLATFORM_INFO *out) { return sys4(MINT_SYS_PLATFORM_INFO, (uint64_t)(uintptr_t)out, 0, 0, 0); }
int64_t mint_programs_install(void) { return sys4(MINT_SYS_PROGRAMS_INSTALL, 0, 0, 0, 0); }
