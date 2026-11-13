#include <stdint.h>
#include "../include/task.h"
#include "../include/arch.h"
#include "../include/serial.h"
#include "../include/address_space.h"
#include "../include/process.h"
#include "../include/fpu.h"

#define MINT_TASK_NAME_LEN 16u
#define MINT_EINVAL 22
#define MINT_EAGAIN 11
#define MINT_ENOMEM 12

typedef struct mint_task {
    /* 26.5.014.0: user FPU/SSE/AVX registers and FS base live here while the
     * task is switched out (eager save/restore; see fpu.h). */
    MINT_FPU_STATE fpu;
    uint64_t fs_base;
    uint64_t user_ticks;
    uint32_t slice_ticks;
    uint8_t fpu_user;
    uint64_t rsp;
    mint_task_entry_t entry;
    void *argument;
    uint64_t wake_tick;
    uint64_t switches;
    uint64_t pid;
    uint64_t privilege_stack_top;
    void *process;
    MINT_USER_TRANSITION user_transition;
    mint_task_state_t state;
    uint8_t logged_user_root;
    uint8_t logged_user_yield;
    char name[MINT_TASK_NAME_LEN];
    uint8_t stack[MINT_TASK_STACK_SIZE] __attribute__((aligned(16)));
} mint_task_t;

static mint_task_t tasks[MINT_TASK_MAX] __attribute__((aligned(64)));
static uint32_t task_count;
static int32_t current_task = -1;
static uint32_t next_candidate;
static uint64_t scheduler_rsp;
static uint64_t next_pid = 1;
static uint64_t total_created;
static uint64_t total_exited;
static uint64_t total_preemptions;
#define MINT_MSR_FS_BASE 0xC0000100u
/* Ring-3 time slice: a user task that has run for this many PIT ticks without
 * entering the kernel is preempted when another task is ready. */
#define MINT_USER_SLICE_TICKS 2u

static void write_fs_base(uint64_t value) {
    __asm__ volatile("wrmsr" :: "c"(MINT_MSR_FS_BASE), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)) : "memory");
}

extern void mint_context_switch(uint64_t *old_rsp, uint64_t new_rsp);

static uint32_t bounded_strlen(const char *s, uint32_t max) {
    uint32_t n = 0;
    if (!s) return 0;
    while (n < max && s[n]) ++n;
    return n;
}

void mint_task_init(void) {
    task_count = 0;
    current_task = -1;
    next_candidate = 0;
    scheduler_rsp = 0;
    next_pid = 1;
    total_created = 0;
    total_exited = 0;
    for (uint32_t i = 0; i < MINT_TASK_MAX; ++i) {
        tasks[i].rsp = 0;
        tasks[i].entry = 0;
        tasks[i].argument = 0;
        tasks[i].wake_tick = 0;
        tasks[i].switches = 0;
        tasks[i].pid = 0;
        tasks[i].privilege_stack_top = 0;
        tasks[i].process = 0;
        tasks[i].user_transition = (MINT_USER_TRANSITION){0};
        tasks[i].state = MINT_TASK_UNUSED;
        tasks[i].logged_user_root = 0;
        tasks[i].logged_user_yield = 0;
        tasks[i].fs_base = 0;
        tasks[i].user_ticks = 0;
        tasks[i].slice_ticks = 0;
        tasks[i].fpu_user = 0;
        tasks[i].name[0] = 0;
    }
    total_preemptions = 0;
    mint_serial_write("[sched] cooperative round-robin scheduler initialized\n");
}

static void task_bootstrap(void) __attribute__((noreturn));
static void task_bootstrap(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) {
        mint_serial_write("[sched] invalid first task context; halted\n");
        for (;;) __asm__ volatile("cli; hlt" ::: "memory");
    }
    mint_task_t *t = &tasks[(uint32_t)current_task];
    mint_serial_write("[sched] task start pid=");
    mint_serial_dec64(t->pid);
    mint_serial_write(" name=");
    mint_serial_write(t->name);
    mint_serial_write("\n");
    t->entry(t->argument);
    mint_task_exit();
}

int64_t mint_task_create(const char *name, mint_task_entry_t entry, void *argument) {
    if (!entry) return -MINT_EINVAL;
    uint32_t i = MINT_TASK_MAX;
    /* Reuse a dead slot before consuming a new slot. This makes the limit a
     * concurrent-task limit rather than a lifetime task-creation limit. */
    for (uint32_t j = 0; j < task_count; ++j) {
        if (tasks[j].state == MINT_TASK_UNUSED || tasks[j].state == MINT_TASK_DEAD) {
            i = j;
            break;
        }
    }
    if (i == MINT_TASK_MAX) {
        if (task_count >= MINT_TASK_MAX) return -MINT_EAGAIN;
        i = task_count++;
    }
    mint_task_t *t = &tasks[i];
    t->entry = entry;
    t->argument = argument;
    t->wake_tick = 0;
    t->switches = 0;
    t->pid = next_pid++;
    t->privilege_stack_top = (uint64_t)(uintptr_t)(t->stack + sizeof(t->stack));
    t->privilege_stack_top &= ~15ULL;
    t->process = 0;
    t->user_transition = (MINT_USER_TRANSITION){0};
    t->state = MINT_TASK_READY;
    t->logged_user_root = 0;
    t->logged_user_yield = 0;
    t->fs_base = 0;
    t->user_ticks = 0;
    t->slice_ticks = 0;
    t->fpu_user = 0;
    total_created++;
    uint32_t n = bounded_strlen(name, MINT_TASK_NAME_LEN - 1u);
    for (uint32_t j = 0; j < n; ++j) t->name[j] = name[j];
    t->name[n] = 0;
    /* Context frame: r15,r14,r13,r12,rbx,rbp,RFLAGS,return address.
     * Seed IF=1 so timer-backed sleep works on the first task dispatch. */
    uintptr_t top = (uintptr_t)(t->stack + sizeof(t->stack));
    top &= ~(uintptr_t)0xFu;
    top -= sizeof(uint64_t); /* after ret, function entry RSP is 8 mod 16 */
    uint64_t *sp = (uint64_t *)(top - 8u * sizeof(uint64_t));
    for (uint32_t j = 0; j < 6; ++j) sp[j] = 0;
    sp[6] = 0x202u; /* reserved bit + IF */
    sp[7] = (uint64_t)(uintptr_t)task_bootstrap;
    t->rsp = (uint64_t)(uintptr_t)sp;
    mint_serial_write("[sched] created pid="); mint_serial_dec64(t->pid);
    mint_serial_write(" name="); mint_serial_write(t->name); mint_serial_write("\n");
    return (int64_t)t->pid;
}

uint64_t mint_task_current_pid(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    return tasks[(uint32_t)current_task].pid;
}

uint64_t mint_task_live_count(void) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < task_count; ++i)
        if (tasks[i].state == MINT_TASK_READY || tasks[i].state == MINT_TASK_RUNNING ||
            tasks[i].state == MINT_TASK_SLEEPING) ++n;
    return n;
}
uint64_t mint_task_total_created(void) { return total_created; }
uint64_t mint_task_total_exited(void) { return total_exited; }

int mint_task_attach_process(void *process) {
    if (!process || current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    mint_task_t *t = &tasks[(uint32_t)current_task];
    if (t->process) return 0;
    t->process = process;
    return 1;
}

void *mint_task_current_process(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    return tasks[(uint32_t)current_task].process;
}

MINT_USER_TRANSITION *mint_task_current_user_transition(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    return &tasks[(uint32_t)current_task].user_transition;
}

void mint_task_user_exit_reset(void) {
    MINT_USER_TRANSITION *state = mint_task_current_user_transition();
    if (!state) return;
    state->exit_seen = 0;
    state->exit_code = 0;
}

void mint_task_user_exit_record(uint64_t exit_code) {
    MINT_USER_TRANSITION *state = mint_task_current_user_transition();
    if (!state) return;
    state->exit_code = exit_code;
    state->exit_seen = 1;
}

int mint_task_user_exit_seen(void) {
    MINT_USER_TRANSITION *state = mint_task_current_user_transition();
    return state && state->exit_seen != 0;
}

uint64_t mint_task_user_exit_code(void) {
    MINT_USER_TRANSITION *state = mint_task_current_user_transition();
    return state ? state->exit_code : UINT64_MAX;
}

int mint_task_detach_process(void *process) {
    if (!process || current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    mint_task_t *t = &tasks[(uint32_t)current_task];
    if (t->process != process) return 0;
    t->process = 0;
    return 1;
}

uint64_t mint_task_privilege_stack_top(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    return tasks[(uint32_t)current_task].privilege_stack_top;
}

int mint_task_set_privilege_stack_top(uint64_t stack_top) {
    if (!stack_top || (stack_top & 15ULL) || current_task < 0 ||
        (uint32_t)current_task >= task_count) return 0;
    tasks[(uint32_t)current_task].privilege_stack_top = stack_top;
    mint_arch_update_rsp0(stack_top);
    return 1;
}

/* Called by the process manager immediately before a task first enters
 * ring 3: the task gets a clean x87/SSE/AVX state and FS base 0, and from now
 * on the scheduler saves/restores them whenever the task switches. */
void mint_task_user_state_begin(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return;
    mint_task_t *t = &tasks[(uint32_t)current_task];
    mint_fpu_state_make_clean(&t->fpu);
    t->fs_base = 0;
    t->slice_ticks = 0;
    t->fpu_user = mint_fpu_ready() ? 1u : 0u;
    if (t->fpu_user) mint_fpu_restore(&t->fpu);
    write_fs_base(0);
}

void mint_task_user_state_end(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return;
    mint_task_t *t = &tasks[(uint32_t)current_task];
    t->fpu_user = 0;
    t->fs_base = 0;
    write_fs_base(0);
}

int mint_task_set_fs_base(uint64_t value) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    tasks[(uint32_t)current_task].fs_base = value;
    write_fs_base(value);
    return 1;
}

uint64_t mint_task_total_preemptions(void) { return total_preemptions; }

/* Make a sleeping task runnable now (used by kill so a sleeping program
 * notices the request immediately). */
void mint_task_wake(uint64_t pid) {
    for (uint32_t i = 0; i < task_count; ++i)
        if (tasks[i].pid == pid && tasks[i].state == MINT_TASK_SLEEPING) tasks[i].wake_tick = 0;
}

uint64_t mint_task_user_ticks(uint64_t pid) {
    for (uint32_t i = 0; i < task_count; ++i)
        if (tasks[i].pid == pid && tasks[i].state != MINT_TASK_UNUSED) return tasks[i].user_ticks;
    return 0;
}

static int another_task_ready(uint32_t self) {
    uint64_t now = mint_timer_ticks();
    for (uint32_t i = 0; i < task_count; ++i) {
        if (i == self) continue;
        if (tasks[i].state == MINT_TASK_READY) return 1;
        if (tasks[i].state == MINT_TASK_SLEEPING && now >= tasks[i].wake_tick) return 1;
    }
    return 0;
}

/* PIT IRQ0 arrived while ring-3 code was running. The kernel itself stays
 * non-preemptive: this is reached only when the interrupted context was CPL3,
 * so no kernel data structure can be half-updated. The IRQ frame lives on
 * this task's private RSP0 stack, so switching away here is the same as a
 * yielding syscall. Returns 1 if the task was switched out and resumed. */
int mint_task_timer_tick_from_user(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return 0;
    uint32_t self = (uint32_t)current_task;
    mint_task_t *t = &tasks[self];
    if (!t->process) return 0;
    ++t->user_ticks;
    if (++t->slice_ticks < MINT_USER_SLICE_TICKS) return 0;
    t->slice_ticks = 0;
    if (!another_task_ready(self)) return 0;
    ++total_preemptions;
    if (t->state == MINT_TASK_RUNNING) t->state = MINT_TASK_READY;
    mint_context_switch(&t->rsp, scheduler_rsp);
    return 1;
}

void mint_task_yield(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return;
    mint_task_t *t = &tasks[(uint32_t)current_task];
    if (t->state == MINT_TASK_RUNNING) t->state = MINT_TASK_READY;
    if (t->process && !t->logged_user_yield) {
        mint_serial_write("[sched] user task yield enabled pid=");
        mint_serial_dec64(t->pid);
        mint_serial_write("\n");
        t->logged_user_yield = 1u;
    }
    mint_context_switch(&t->rsp, scheduler_rsp);
}

void mint_task_sleep_ticks(uint64_t ticks) {
    if (current_task < 0 || (uint32_t)current_task >= task_count) return;
    if (ticks == 0) { mint_task_yield(); return; }
    mint_task_t *t = &tasks[(uint32_t)current_task];
    uint64_t now = mint_timer_ticks();
    t->wake_tick = (UINT64_MAX - now < ticks) ? UINT64_MAX : now + ticks;
    t->state = MINT_TASK_SLEEPING;
    mint_context_switch(&t->rsp, scheduler_rsp);
}

__attribute__((noreturn)) void mint_task_exit(void) {
    if (current_task < 0 || (uint32_t)current_task >= task_count)
        for (;;) __asm__ volatile("cli; hlt" ::: "memory");
    mint_task_t *t = &tasks[(uint32_t)current_task];
    t->state = MINT_TASK_DEAD;
    total_exited++;
    mint_serial_write("[sched] task exited pid=");
    mint_serial_dec64(t->pid);
    mint_serial_write(" name=");
    mint_serial_write(t->name);
    mint_serial_write(" total-exited=");
    mint_serial_dec64(total_exited);
    mint_serial_write("\n");
    mint_context_switch(&t->rsp, scheduler_rsp);
    for (;;) __asm__ volatile("cli; hlt" ::: "memory");
}

static void scheduler_fatal(const char *reason) __attribute__((noreturn));
static void scheduler_fatal(const char *reason) {
    mint_serial_write("[sched] FATAL: ");
    mint_serial_write(reason);
    mint_serial_write("; halted\n");
    for (;;) __asm__ volatile("cli; hlt" ::: "memory");
}

static void activate_selected_task(mint_task_t *task) {
    if (!task) scheduler_fatal("null task selected");
    if (task->process) {
        MINT_PROCESS *process = (MINT_PROCESS *)task->process;
        if (!process->allocated || !process->address_space.initialized ||
            (process->state != MINT_PROCESS_READY && process->state != MINT_PROCESS_RUNNING))
            scheduler_fatal("task has invalid process/address-space ownership");
        if (!mint_address_space_activate(&process->address_space))
            scheduler_fatal("failed to activate selected user address space");
        /* Log the address-space activation once per task, not on every wake.
         * A resident shell sleeps/polls many times per second; dumping CR3 for
         * each activation used to turn COM1 logs into megabytes of noise. */
        if (!task->logged_user_root) {
            mint_serial_write("[sched] first activation user root pid=");
            mint_serial_dec64(task->pid);
            mint_serial_write(" cr3=");
            mint_serial_hex64(mint_address_space_root(&process->address_space));
            mint_serial_write("\n");
            task->logged_user_root = 1u;
        }
    } else {
        if (!mint_address_space_activate_kernel())
            scheduler_fatal("failed to activate kernel address space");
    }
    /* This pointer is consumed by the syscall-exit assembly. It must follow
     * the selected task, not whichever user process entered CPL3 most recently. */
    mint_arch_set_user_transition(&task->user_transition);
    /* TSS.RSP0 must agree with the selected task before any CPL3 return. */
    mint_arch_update_rsp0(task->privilege_stack_top);
    /* User register state that the kernel never touches: FPU/SSE/AVX and the
     * FS base (musl's thread pointer). */
    if (task->fpu_user) {
        mint_fpu_restore(&task->fpu);
        write_fs_base(task->fs_base);
    }
    task->slice_ticks = 0;
}

__attribute__((noreturn)) void mint_scheduler_run(void) {
    mint_serial_write("[sched] per-task CR3, transition-state and RFLAGS switching online\n");
    for (;;) {
        /* A yielding user syscall returns to this shared scheduler stack. Move
         * to the kernel root before dispatching any unrelated task. */
        if (!mint_address_space_activate_kernel())
            scheduler_fatal("failed to restore kernel address space in scheduler");
        mint_arch_set_user_transition(0);
        current_task = -1;
        uint64_t now = mint_timer_ticks();
        for (uint32_t i = 0; i < task_count; ++i) {
            if (tasks[i].state == MINT_TASK_SLEEPING && now >= tasks[i].wake_tick)
                tasks[i].state = MINT_TASK_READY;
        }
        int32_t selected = -1;
        for (uint32_t scanned = 0; scanned < task_count; ++scanned) {
            uint32_t i = (next_candidate + scanned) % (task_count ? task_count : 1u);
            if (i < task_count && tasks[i].state == MINT_TASK_READY) {
                selected = (int32_t)i;
                next_candidate = (i + 1u) % task_count;
                break;
            }
        }
        if (selected < 0) {
            /* All tasks are blocked/asleep. The next PIT interrupt wakes the
             * CPU; the scheduler context restores IF independently of task IF. */
            __asm__ volatile("sti; hlt" ::: "memory");
            continue;
        }
        current_task = selected;
        mint_task_t *task = &tasks[(uint32_t)selected];
        task->state = MINT_TASK_RUNNING;
        task->switches++;
        activate_selected_task(task);
        mint_context_switch(&scheduler_rsp, task->rsp);
        /* Reached whenever the selected task yields, sleeps, exits or is
         * preempted. Its FPU registers are still live: save them before any
         * other user task can run. */
        if (task->fpu_user) mint_fpu_save(&task->fpu);
        /* The scheduler root and transition pointer are restored at loop top. */
        if (task->state == MINT_TASK_RUNNING) task->state = MINT_TASK_READY;
    }
}
