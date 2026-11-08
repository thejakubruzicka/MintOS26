#ifndef MINTOS26_USER_TRANSITION_H
#define MINTOS26_USER_TRANSITION_H
#include <stdint.h>

/* Per-task return state for a task that is currently executing a CPL3 image.
 * The first two fields have stable offsets consumed by arch.S: saved RSP at
 * 0, saved RFLAGS at 8. Exit status is per task, never process-global. */
typedef struct mint_user_transition {
    uint64_t saved_kernel_rsp;       /* offset 0: asm ABI */
    uint64_t saved_kernel_rflags;    /* offset 8: asm ABI */
    volatile uint32_t exit_seen;     /* offset 16 */
    uint32_t reserved;
    uint64_t exit_code;              /* offset 24 */
} MINT_USER_TRANSITION;

#endif
