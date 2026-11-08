#ifndef MINTOS26_SYSCALL_H
#define MINTOS26_SYSCALL_H
#include <stdint.h>
#include "address_space.h"

/* Register layout made by mint_syscall_stub after crossing CPL3 -> CPL0.
 * The trailing user_rsp/user_ss words exist because privilege changed. */
typedef struct mint_syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error_code, rip, cs, rflags, user_rsp, user_ss;
} MINT_SYSCALL_FRAME;

#define MINT_SYSCALL_EXIT_SENTINEL 0x4D494E5445584954ULL /* "MINTEXIT" */

int64_t mint_syscall_dispatch(uint64_t number, uint64_t arg0, uint64_t arg1,
                              uint64_t arg2, uint64_t arg3, uint64_t arg4,
                              uint64_t arg5);
int64_t mint_syscall_dispatch_user(uint64_t number, uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                   uint64_t arg5);
int64_t mint_syscall_trap_dispatch(MINT_SYSCALL_FRAME *frame);
void mint_syscall_user_exit_reset(void);
int mint_syscall_user_exit_seen(void);
uint64_t mint_syscall_user_exit_code(void);
#endif
