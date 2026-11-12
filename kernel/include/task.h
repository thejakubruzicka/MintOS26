#ifndef MINTOS26_TASK_H
#define MINTOS26_TASK_H
#include <stdint.h>
#include "user_transition.h"

/* 26.5.014.0: 32 concurrent tasks (was 12) so several programs can run at once. */
#define MINT_TASK_MAX 32u
#define MINT_TASK_STACK_SIZE (16u * 1024u)

typedef void (*mint_task_entry_t)(void *argument);

typedef enum {
    MINT_TASK_UNUSED = 0,
    MINT_TASK_READY,
    MINT_TASK_RUNNING,
    MINT_TASK_SLEEPING,
    MINT_TASK_DEAD
} mint_task_state_t;

void mint_task_init(void);
/* Return positive PID on success, or a negative errno-style value. */
int64_t mint_task_create(const char *name, mint_task_entry_t entry, void *argument);
uint64_t mint_task_current_pid(void);
uint64_t mint_task_live_count(void);
uint64_t mint_task_total_created(void);
uint64_t mint_task_total_exited(void);
/* Each user process is attached to its owning task. Its transition/exit state
 * remains task-local while that task is switched out in a syscall. */
int mint_task_attach_process(void *process);
void *mint_task_current_process(void);
MINT_USER_TRANSITION *mint_task_current_user_transition(void);
void mint_task_user_exit_reset(void);
void mint_task_user_exit_record(uint64_t exit_code);
int mint_task_user_exit_seen(void);
uint64_t mint_task_user_exit_code(void);
int mint_task_detach_process(void *process);
uint64_t mint_task_privilege_stack_top(void);
int mint_task_set_privilege_stack_top(uint64_t stack_top);
void mint_task_yield(void);
/* 26.5.014.0: per-task user register state (FPU/SSE/AVX + FS base) and
 * ring-3 timer preemption; see scheduler.c. */
void mint_task_user_state_begin(void);
void mint_task_user_state_end(void);
int mint_task_set_fs_base(uint64_t value);
int mint_task_timer_tick_from_user(void);
uint64_t mint_task_total_preemptions(void);
uint64_t mint_task_user_ticks(uint64_t pid);
void mint_task_wake(uint64_t pid);
void mint_task_sleep_ticks(uint64_t ticks);
__attribute__((noreturn)) void mint_task_exit(void);
__attribute__((noreturn)) void mint_scheduler_run(void);

#endif
