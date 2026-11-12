#ifndef MINTOS26_PROCESS_H
#define MINTOS26_PROCESS_H
#include <stddef.h>
#include <stdint.h>
#include "address_space.h"
#include "elf_loader.h"
#include "uapi/mintos/process.h"

/* 26.5.014.0: 24 process slots (was 4). A slot stays reserved as a zombie
 * after exit until its parent collects the status with wait. */
#define MINT_PROCESS_MAX 24u
#define MINT_PROCESS_KERNEL_STACK_BYTES (16u * 1024u)

typedef enum mint_process_state {
    MINT_PROCESS_UNUSED = 0,
    MINT_PROCESS_READY,
    MINT_PROCESS_RUNNING,
    MINT_PROCESS_EXITED,
    MINT_PROCESS_FAILED,
    MINT_PROCESS_STARTING    /* spawned, runner task not yet loaded the image */
} MINT_PROCESS_STATE;

/* A process descriptor points at a shared open file description (OFD, see
 * proc_files.h); dup() and spawned stdio share OFDs like POSIX. */
#define MINT_FD_FLAG_CLOEXEC 0x1u
typedef struct mint_fd {
    int32_t ofd;      /* -1 = closed */
    uint32_t flags;   /* MINT_FD_FLAG_* */
} MINT_FD;
#define MINT_PROC_FD_MAX 32u

/* Kill/exit encodings recorded in exit_code (see uapi/mintos/process.h for
 * what wait reports). */
#define MINT_EXIT_FAULT_BIT  0x8000000000000000ULL
#define MINT_EXIT_KILLED_BIT 0x4000000000000000ULL

typedef struct mint_process {
    MINT_ADDRESS_SPACE address_space;
    MINT_USER_IMAGE image;
    uint64_t pid;
    uint64_t parent_pid;
    uint64_t exit_code;
    uint64_t prior_privilege_stack_top;
    void *kernel_entry_stack;
    uint32_t state;
    uint32_t allocated;
    /* 26.5.014.0 */
    uint32_t zombie;              /* exited, status not collected yet */
    uint32_t waitable;            /* has a parent that may wait */
    volatile uint32_t kill_pending;
    uint32_t kill_signal;
    uint32_t frame_limit;
    uint32_t stack_bytes;
    uint64_t start_tick;
    uint64_t end_tick;
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t mmap_bytes;
    uint64_t peak_pages;
    uint64_t user_ticks;
    char name[MINT_PROC_NAME_MAX];
    char path[MINT_PROC_PATH_MAX];
    char cwd[MINT_PROC_PATH_MAX];
    MINT_FD fds[MINT_PROC_FD_MAX];
    /* argv + envp for the runner, packed NUL-separated strings */
    uint8_t *args;
    uint32_t args_bytes;
    uint32_t argc;
    uint32_t envc;
    uint32_t spawn_flags;
} MINT_PROCESS;

void mint_process_system_init(void);
/* Create a process owned by the current scheduler task. The loader consumes a
 * fresh address space and maps a static ELF64 image plus private user stack. */
MINT_PROCESS *mint_process_create_current_from_elf(const void *image, size_t image_size);
/* Synchronously run the current task's attached process at CPL3 until SYS_EXIT. */
int mint_process_run_current(MINT_PROCESS *process);
uint64_t mint_process_pid(const MINT_PROCESS *process);
uint64_t mint_process_exit_code(const MINT_PROCESS *process);
MINT_PROCESS_STATE mint_process_state(const MINT_PROCESS *process);
/* 26.5.014.0: called on every return path to ring 3 that may follow a task
 * switch (syscall exit, preempting IRQ). If the current process was killed
 * while switched out, it never returns to user mode. */
void mint_process_trap_return_check(void);
/* 1 if the current process has a kill pending (blocking syscalls poll this
 * and return -MINT_EINTR so the trap exit can terminate the process). */
int mint_process_current_killed(void);
MINT_PROCESS *mint_process_current(void);
MINT_PROCESS *mint_process_find(uint64_t pid);

/* Spawn a program from MintOS HD. `args` holds argc argv strings followed by
 * envc environment strings, each NUL-terminated. `child_ofds` are the open
 * file descriptions for the child's fds 0..2; on success the child owns those
 * references, on failure the caller still does. Returns the pid
 * or a negative MINT_E* value. */
int64_t mint_process_spawn(uint64_t parent_pid, const char *path, const char *cwd,
                           const uint8_t *args, uint32_t args_bytes, uint32_t argc, uint32_t envc,
                           const int32_t child_ofds[3], uint32_t flags, uint32_t frame_limit,
                           uint32_t stack_bytes);
/* wait: pid > 0 for that child, 0 for any child of `parent_pid`.
 * Returns the reaped pid, 0 if WNOHANG and nothing exited, or -MINT_ECHILD. */
int64_t mint_process_wait(uint64_t parent_pid, int64_t pid, uint32_t options, MINT_PROC_STATUS *out);
int64_t mint_process_kill(uint64_t caller_pid, uint64_t pid, uint32_t signal);
uint32_t mint_process_list(MINT_PROC_INFO *out, uint32_t capacity);
/* Leak accounting snapshot: live processes and their owned pages. */
uint64_t mint_process_live_count(void);
/* Translate a raw exit_code into the wait status reported to user space. */
void mint_process_decode_status(uint64_t exit_code, MINT_PROC_STATUS *out);

#endif
