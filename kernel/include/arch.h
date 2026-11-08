#ifndef MINTOS26_ARCH_H
#define MINTOS26_ARCH_H
#include <stdint.h>
#include "user_transition.h"

void mint_arch_init(void);
uint64_t mint_arch_read_cr2(void);
void mint_arch_update_rsp0(uint64_t stack_top);
uint64_t mint_arch_current_rsp0(void);
/* Per-task record selected immediately before every scheduler context switch. */
extern MINT_USER_TRANSITION *volatile mint_arch_user_transition_current;
void mint_arch_set_user_transition(MINT_USER_TRANSITION *transition);
/* Enters CPL3 and returns only when this task's user image invokes SYS_EXIT. */
void mint_arch_enter_user(uint64_t entry_rip, uint64_t user_stack_top);
int mint_arch_nx_supported(void);
int mint_arch_prepare_4level_paging(void);
void mint_arch_enable_nxe(void);
void mint_arch_enable_write_protect(void);
/* CPUID PAT support, and reprogramming the unused PAT entry PA4 to WC.
 * After success, a 4 KiB PTE with bit 7 (or a 2 MiB PDE with bit 12) set and
 * PCD=PWT=0 maps write-combining memory. */
int mint_arch_pat_supported(void);
int mint_arch_enable_pat_write_combining(void);
void mint_arch_load_cr3(uint64_t physical_root);
void mint_timer_init(void);
void mint_arch_enable_input_irqs(int keyboard_enabled, int mouse_enabled);
uint64_t mint_timer_ticks(void);
void mint_serial_init(void);
void mint_serial_write(const char *s);
void mint_serial_putc(char c);
void mint_serial_hex64(uint64_t value);

struct mint_exception_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error_code, rip, cs, rflags;
};
void mint_exception_display(uint64_t vector, uint64_t rip, uint64_t error);
void mint_irq_dispatch(const struct mint_exception_frame *frame);
__attribute__((noreturn)) void mint_exception_dispatch(const struct mint_exception_frame *frame);
/* Restore the scheduler saved kernel context after a contained CPL3 fault. */
__attribute__((noreturn)) void mint_arch_return_from_user_fault(void);

#endif
