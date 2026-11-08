#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct mint_user_transition;
struct mint_user_transition *volatile mint_arch_user_transition_current;

void mint_context_switch(uint64_t *old_rsp, uint64_t new_rsp);
struct mint_exception_frame;
void mint_irq_dispatch(const struct mint_exception_frame *frame) { (void)frame; }
void mint_exception_dispatch(const struct mint_exception_frame *frame) { (void)frame; abort(); }
int64_t mint_syscall_trap_dispatch(void *frame) { (void)frame; return 0; }

static uint64_t main_rsp, task_a_rsp, task_b_rsp;
static uint8_t stack_a[8192] __attribute__((aligned(16)));
static uint8_t stack_b[8192] __attribute__((aligned(16)));
static volatile int phase_a, phase_b;
static volatile int task_a_restored_df;

static void seed_stack(uint8_t *stack, size_t size, uint64_t *rsp, void (*entry)(void)) {
    uintptr_t top = ((uintptr_t)(stack + size)) & ~(uintptr_t)0xFu;
    top -= sizeof(uint64_t); /* post-ret entry RSP must be 8 mod 16 */
    uint64_t *frame = (uint64_t *)(top - 8u * sizeof(uint64_t));
    for (unsigned i = 0; i < 6; ++i) frame[i] = 0;
    frame[6] = 0x202u; /* fabricated IF-enabled task context */
    frame[7] = (uint64_t)(uintptr_t)entry;
    *rsp = (uint64_t)(uintptr_t)frame;
}

static void task_a(void) {
    phase_a = 1;
    /* DF is user-modifiable even in the host test process. Verify the real
     * assembly switch restores it, then clear it immediately for C ABI safety. */
    __asm__ volatile("std" ::: "cc");
    mint_context_switch(&task_a_rsp, main_rsp);
    unsigned long resumed_flags;
    __asm__ volatile("pushfq; popq %0; cld" : "=r"(resumed_flags) :: "cc", "memory");
    task_a_restored_df = (resumed_flags & (1UL << 10)) != 0;
    phase_a = 2;
    mint_context_switch(&task_a_rsp, main_rsp);
    phase_a = 3;
    mint_context_switch(&task_a_rsp, main_rsp);
    abort();
}
static void task_b(void) {
    phase_b = 1;
    mint_context_switch(&task_b_rsp, main_rsp);
    phase_b = 2;
    mint_context_switch(&task_b_rsp, main_rsp);
    phase_b = 3;
    mint_context_switch(&task_b_rsp, main_rsp);
    abort();
}
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } printf("PASS: %s\n", msg); } while (0)

int main(void) {
    seed_stack(stack_a, sizeof(stack_a), &task_a_rsp, task_a);
    seed_stack(stack_b, sizeof(stack_b), &task_b_rsp, task_b);
    mint_context_switch(&main_rsp, task_a_rsp);
    CHECK(phase_a == 1 && phase_b == 0, "first-run context enters task A and yields back to caller");
    mint_context_switch(&main_rsp, task_b_rsp);
    CHECK(phase_a == 1 && phase_b == 1, "task B has an independent stack and resumes its own entry");
    mint_context_switch(&main_rsp, task_a_rsp);
    CHECK(phase_a == 2 && phase_b == 1, "task A resumes after its previous switch call");
    CHECK(task_a_restored_df == 1, "task A resumes with its own saved RFLAGS (DF test), then clears DF");
    mint_context_switch(&main_rsp, task_b_rsp);
    CHECK(phase_a == 2 && phase_b == 2, "task B resumes after its previous switch call");
    mint_context_switch(&main_rsp, task_a_rsp);
    CHECK(phase_a == 3, "task A preserves the full call stack across repeated switches");
    mint_context_switch(&main_rsp, task_b_rsp);
    CHECK(phase_b == 3, "task B preserves the full call stack across repeated switches");
    return 0;
}
