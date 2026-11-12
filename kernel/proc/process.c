#include <stdint.h>
#include <stddef.h>
#include "../include/process.h"
#include "../include/task.h"
#include "../include/arch.h"
#include "../include/serial.h"
#include "../include/vmalloc.h"
#include "../include/syscall.h"
#include "../include/ramfs.h"
#include "../include/mintos_fs.h"
#include "../include/proc_files.h"
#include "../include/log.h"
#include "../include/uapi/mintos/syscall.h"

/* 26.5.014.0 process model
 * -----------------------
 * A process is owned by one scheduler task (its pid is the task's pid). The
 * task enters ring 3 through mint_process_run_current() and comes back when
 * the program exits, faults or is killed. Programs started from MintOS HD get
 * a dedicated "runner" task (mint_process_spawn) whose slot is reserved at
 * spawn time, so the parent can wait for it immediately. After exit the slot
 * stays a zombie until the parent collects the status. */

static MINT_PROCESS processes[MINT_PROCESS_MAX];

static void zero_bytes(void *ptr, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < n; ++i) p[i] = 0;
}
static size_t text_copy(char *out, size_t capacity, const char *in) {
    size_t n = 0;
    if (!out || !capacity) return 0;
    while (in && in[n] && n + 1u < capacity) { out[n] = in[n]; ++n; }
    out[n] = '\0';
    return n;
}

void mint_process_system_init(void) {
    zero_bytes(processes, sizeof(processes));
    mint_proc_files_init();
    mint_serial_write("[proc] process table initialized; bounded ELF64 loader enabled\n");
}

static int slot_free(const MINT_PROCESS *p) {
    return !p->allocated ||
           ((p->state == MINT_PROCESS_EXITED || p->state == MINT_PROCESS_FAILED) && !p->zombie);
}

static MINT_PROCESS *find_process_slot(void) {
    for (unsigned i = 0; i < MINT_PROCESS_MAX; ++i)
        if (slot_free(&processes[i])) return &processes[i];
    return 0;
}

MINT_PROCESS *mint_process_find(uint64_t pid) {
    if (!pid) return 0;
    for (unsigned i = 0; i < MINT_PROCESS_MAX; ++i)
        if (processes[i].allocated && processes[i].pid == pid && !slot_free(&processes[i]))
            return &processes[i];
    for (unsigned i = 0; i < MINT_PROCESS_MAX; ++i)
        if (processes[i].allocated && processes[i].pid == pid) return &processes[i];
    return 0;
}

MINT_PROCESS *mint_process_current(void) {
    return (MINT_PROCESS *)mint_task_current_process();
}

int mint_process_current_killed(void) {
    MINT_PROCESS *p = mint_process_current();
    return p && p->kill_pending;
}

static void rollback_create(MINT_PROCESS *p, int attached) {
    if (!p) return;
    if (attached) {
        (void)mint_task_set_privilege_stack_top(p->prior_privilege_stack_top);
        (void)mint_task_detach_process(p);
    }
    if (p->address_space.initialized && !p->address_space.active)
        (void)mint_address_space_destroy(&p->address_space);
    if (p->kernel_entry_stack) (void)mint_vmalloc_free(p->kernel_entry_stack);
    p->kernel_entry_stack = 0;
}

/* Allocates the privilege-transition stack and attaches p to the current task. */
static int attach_to_current_task(MINT_PROCESS *p) {
    p->kernel_entry_stack = mint_vmalloc_alloc(MINT_PROCESS_KERNEL_STACK_BYTES, MINT_PAGE_MAP_WRITE);
    if (!p->kernel_entry_stack) {
        mint_serial_write("[proc] unable to allocate process privilege-transition stack\n");
        return 0;
    }
    p->prior_privilege_stack_top = mint_task_privilege_stack_top();
    if (!p->prior_privilege_stack_top ||
        !mint_task_attach_process(p) ||
        !mint_task_set_privilege_stack_top((uint64_t)(uintptr_t)p->kernel_entry_stack + MINT_PROCESS_KERNEL_STACK_BYTES)) {
        mint_serial_write("[proc] failed to attach process to scheduler task\n");
        return -1;
    }
    return 1;
}

/* ------------------------------------------------------- initial user stack */
#define AT_NULL 0u
#define AT_PHDR 3u
#define AT_PHENT 4u
#define AT_PHNUM 5u
#define AT_PAGESZ 6u
#define AT_BASE 7u
#define AT_FLAGS 8u
#define AT_ENTRY 9u
#define AT_UID 11u
#define AT_EUID 12u
#define AT_GID 13u
#define AT_EGID 14u
#define AT_HWCAP 16u
#define AT_CLKTCK 17u
#define AT_SECURE 23u
#define AT_RANDOM 25u
#define AT_EXECFN 31u
#define STACK_BUILD_MAX (MINT_PROC_ARGS_MAX + MINT_PROC_PATH_MAX + 8192u)
static uint8_t stack_build[STACK_BUILD_MAX] __attribute__((aligned(16)));

static uint64_t entropy64(uint64_t salt) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = (((uint64_t)hi << 32) | lo) ^ (salt * 0x9E3779B97F4A7C15ULL) ^ mint_timer_ticks();
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDULL; x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ULL; x ^= x >> 33;
    return x;
}

/* SysV x86-64 process entry layout: argc, argv[], NULL, envp[], NULL, auxv,
 * then the strings and AT_RANDOM bytes near the top. RSP is 16-byte aligned. */
static int build_initial_stack(MINT_PROCESS *p, uint64_t *rsp_out) {
    const uint32_t argc = p->argc, envc = p->envc;
    const char *exec_path = p->path[0] ? p->path : p->name;
    size_t exec_len = 0;
    while (exec_path[exec_len]) ++exec_len;
    uint64_t strings = 16u + exec_len + 1u + p->args_bytes;
    uint64_t words = 1u + argc + 1u + envc + 1u + 2u * 17u;
    uint64_t total = ((strings + 15u) & ~15ULL) + words * 8u;
    total = (total + 15u) & ~15ULL;
    if (total > STACK_BUILD_MAX || total + MINT_PAGING_PAGE_SIZE > p->image.stack_size) return 0;
    uint64_t top = p->image.stack_top;
    uint64_t rsp = top - total;
    if (rsp & 15u) return 0;
    zero_bytes(stack_build, (size_t)total);
    /* Strings sit at the end of the block. */
    uint64_t str_off = total - strings;
    uint64_t random_va = rsp + str_off;
    uint64_t r0 = entropy64(p->pid), r1 = entropy64(r0);
    for (int i = 0; i < 8; ++i) { stack_build[str_off + i] = (uint8_t)(r0 >> (8 * i)); stack_build[str_off + 8 + i] = (uint8_t)(r1 >> (8 * i)); }
    uint64_t cursor = str_off + 16u;
    uint64_t execfn_va = rsp + cursor;
    for (size_t i = 0; i <= exec_len; ++i) stack_build[cursor++] = (uint8_t)exec_path[i];
    uint64_t args_va = rsp + cursor;
    for (uint32_t i = 0; i < p->args_bytes; ++i) stack_build[cursor + i] = p->args ? p->args[i] : 0;
    uint64_t *w = (uint64_t *)(void *)stack_build;
    uint64_t wi = 0;
    w[wi++] = argc;
    uint64_t at = 0;
    for (uint32_t i = 0; i < argc + envc; ++i) {
        if (i == argc) w[wi++] = 0;
        w[wi++] = args_va + at;
        while (at < p->args_bytes && p->args[at]) ++at;
        ++at;
    }
    if (argc == argc + envc) w[wi++] = 0;   /* argv terminator when envc == 0 */
    w[wi++] = 0;                            /* envp terminator */
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    const uint64_t aux[][2] = {
        {AT_PHDR, p->image.phdr}, {AT_PHENT, 56u}, {AT_PHNUM, p->image.phnum},
        {AT_PAGESZ, 4096u}, {AT_BASE, 0}, {AT_FLAGS, 0}, {AT_ENTRY, p->image.entry},
        {AT_UID, 0}, {AT_EUID, 0}, {AT_GID, 0}, {AT_EGID, 0}, {AT_HWCAP, d},
        {AT_CLKTCK, 100u}, {AT_SECURE, 0}, {AT_RANDOM, random_va}, {AT_EXECFN, execfn_va},
        {AT_NULL, 0}
    };
    for (unsigned i = 0; i < sizeof(aux) / sizeof(aux[0]); ++i) {
        if (aux[i][0] == AT_PHDR && !aux[i][1]) continue;
        w[wi++] = aux[i][0]; w[wi++] = aux[i][1];
    }
    if (wi * 8u > str_off) return 0;
    /* Map every stack page the block touches (the loader mapped only the top one). */
    for (uint64_t page = rsp & ~(uint64_t)(MINT_PAGING_PAGE_SIZE - 1u); page < top - MINT_PAGING_PAGE_SIZE;
         page += MINT_PAGING_PAGE_SIZE) {
        uint64_t pa = 0;
        if (mint_address_space_lookup(&p->address_space, page, &pa, 0)) continue;
        pa = mint_address_space_alloc_frame(&p->address_space);
        if (!pa) return 0;
        if (!mint_address_space_map(&p->address_space, page, pa, MINT_PAGE_MAP_WRITE)) {
            (void)mint_address_space_release_frame(&p->address_space, pa);
            return 0;
        }
    }
    if (!mint_address_space_initialize_mapped_bytes(&p->address_space, rsp, stack_build, (size_t)total)) return 0;
    *rsp_out = rsp;
    return 1;
}

/* Fresh address space + image load + initial stack, for a process slot. */
static int prepare_image(MINT_PROCESS *p, mint_elf_reader_t reader, void *ctx, size_t size, size_t stack_bytes) {
    if (!mint_address_space_create(&p->address_space)) {
        mint_serial_write("[proc] unable to create process address space\n");
        return 0;
    }
    if (p->frame_limit) (void)mint_address_space_set_frame_limit(&p->address_space, p->frame_limit);
    if (!mint_elf64_load_user_reader(&p->address_space, reader, ctx, size, stack_bytes, &p->image)) {
        mint_serial_write("[proc] static ELF64 validation/load failed\n");
        return 0;
    }
    uint64_t rsp = 0;
    if (!build_initial_stack(p, &rsp)) {
        mint_serial_write("[proc] could not build the initial argv/envp/auxv stack\n");
        if (p->address_space.initialized && !p->address_space.active)
            (void)mint_address_space_destroy(&p->address_space);
        return 0;
    }
    p->image.stack_top = rsp;
    /* brk starts on the page after the image, 1 GiB of address space at most. */
    p->brk_base = (p->image.image_end + 0x1FFFFFULL) & ~0x1FFFFFULL;
    p->brk_current = p->brk_base;
    return 1;
}

static int memory_reader(void *context, uint64_t offset, void *out, size_t length) {
    const uint8_t *blob = (const uint8_t *)context;
    uint8_t *o = (uint8_t *)out;
    for (size_t i = 0; i < length; ++i) o[i] = blob[offset + i];
    return 1;
}

MINT_PROCESS *mint_process_create_current_from_elf(const void *image, size_t image_size) {
    if (!image || image_size == 0 || mint_task_current_pid() == 0 ||
        mint_task_current_process() != 0) return 0;
    MINT_PROCESS *p = find_process_slot();
    if (!p) {
        mint_serial_write("[proc] process table exhausted\n");
        return 0;
    }
    zero_bytes(p, sizeof(*p));
    p->allocated = 1u;
    p->state = MINT_PROCESS_UNUSED;
    p->pid = mint_task_current_pid(); /* one process per owning task */
    p->parent_pid = 0;
    p->start_tick = mint_timer_ticks();
    text_copy(p->name, sizeof(p->name), "builtin");
    text_copy(p->cwd, sizeof(p->cwd), "/");
    mint_fd_table_init(p);
    if (!prepare_image(p, memory_reader, (void *)image, image_size, MINT_USER_DEFAULT_STACK_BYTES)) {
        rollback_create(p, 0);
        zero_bytes(p, sizeof(*p));
        return 0;
    }
    int attached = attach_to_current_task(p);
    if (attached != 1) {
        rollback_create(p, attached < 0 && mint_task_current_process() == p);
        zero_bytes(p, sizeof(*p));
        return 0;
    }
    /* Built-in images log to COM1 through fds 1 and 2. */
    for (int fd = 0; fd < 3; ++fd) {
        int32_t ofd = mint_ofd_create(fd == 0 ? MINT_OFD_NULL : MINT_OFD_CONSOLE,
                                      fd == 0 ? MINT_OFD_READ : MINT_OFD_WRITE, -1, p->pid);
        if (ofd >= 0 && mint_fd_install_at(p, ofd, fd, 0) < 0) mint_ofd_unref(ofd);
    }
    p->state = MINT_PROCESS_READY;
    mint_serial_write("[proc] loaded ELF64 process pid=");
    mint_serial_dec64(p->pid);
    mint_serial_write(" entry=");
    mint_serial_hex64(p->image.entry);
    mint_serial_write(" segments=");
    mint_serial_dec64(p->image.segment_count);
    mint_serial_write(" stack-pages=");
    mint_serial_dec64(p->image.stack_size / 4096u);
    mint_serial_write("\n");
    return p;
}

void mint_process_decode_status(uint64_t code, MINT_PROC_STATUS *out) {
    if (!out) return;
    out->detail = 0;
    if (code & MINT_EXIT_FAULT_BIT) {
        out->reason = MINT_PROC_EXIT_FAULT;
        out->code = (int32_t)((code >> 32) & 0xFFu);
        out->detail = code & 0xFFFFFFFFu;
    } else if (code & MINT_EXIT_KILLED_BIT) {
        out->reason = MINT_PROC_EXIT_KILLED;
        out->code = (int32_t)(code & 0xFFu);
    } else if (code == (MINT_EXIT_KILLED_BIT >> 1 | 127u)) {
        out->reason = MINT_PROC_EXIT_LOAD_FAILED;
        out->code = 127;
    } else {
        out->reason = MINT_PROC_EXIT_NORMAL;
        out->code = (int32_t)(code & 0xFFu);
    }
}
#define MINT_EXIT_LOAD_FAILED ((MINT_EXIT_KILLED_BIT >> 1) | 127u)

static void reparent_children(uint64_t pid) {
    for (unsigned i = 0; i < MINT_PROCESS_MAX; ++i) {
        MINT_PROCESS *c = &processes[i];
        if (!c->allocated || c->parent_pid != pid) continue;
        c->parent_pid = 0;
        c->waitable = 0;
        if (c->zombie) c->zombie = 0;   /* nobody will collect it: free the slot */
    }
}

static void finish_exit(MINT_PROCESS *p) {
    MINT_ASPACE_USAGE usage;
    mint_address_space_get_usage(&p->address_space, &usage);
    if (usage.peak_owned_pages > p->peak_pages) p->peak_pages = usage.peak_owned_pages;
    p->user_ticks = mint_task_user_ticks(p->pid);
    p->end_tick = mint_timer_ticks();
    mint_fd_close_all(p);
    if (p->args) { (void)mint_vmalloc_free(p->args); p->args = 0; }
    reparent_children(p->pid);
    p->zombie = p->waitable && p->parent_pid && mint_process_find(p->parent_pid) != 0;
}

int mint_process_run_current(MINT_PROCESS *p) {
    if (!p || !p->allocated || p->state != MINT_PROCESS_READY ||
        mint_task_current_process() != p || !p->image.loaded || !p->kernel_entry_stack) return 0;
    p->state = MINT_PROCESS_RUNNING;
    mint_syscall_user_exit_reset();
    mint_serial_write("[proc] entering ring 3 at ELF entry\n");
    if (!mint_address_space_activate(&p->address_space)) {
        p->state = MINT_PROCESS_FAILED;
        mint_serial_write("[proc] CR3 activation failed before user entry\n");
        goto cleanup;
    }
    if (p->kill_pending) {
        (void)mint_address_space_activate_kernel();
        mint_task_user_exit_record(MINT_EXIT_KILLED_BIT | p->kill_signal);
    } else {
        /* Clean x87/SSE/AVX registers and FS base for the new program. */
        mint_task_user_state_begin();
        mint_arch_enter_user(p->image.entry, p->image.stack_top);
        mint_task_user_state_end();
    }
    /* SYS_EXIT switches to the kernel root and restores this scheduler task's
       stack. Re-enable its ordinary RSP0 before reaping the process. */
    if (p->address_space.active) (void)mint_address_space_activate_kernel();
    (void)mint_task_set_privilege_stack_top(p->prior_privilege_stack_top);
    if (mint_syscall_user_exit_seen()) {
        p->exit_code = mint_syscall_user_exit_code();
        p->state = (p->exit_code & (MINT_EXIT_FAULT_BIT | MINT_EXIT_KILLED_BIT)) ? MINT_PROCESS_FAILED
                                                                             : MINT_PROCESS_EXITED;
    } else {
        p->state = MINT_PROCESS_FAILED;
        mint_serial_write("[proc] user image returned without SYS_EXIT\n");
    }
cleanup:
    /* File handles are process-owned resources. Reap them on every process
       exit/failure path so the bounded handle table cannot leak across runs. */
    mint_fd_close_all(p);
    mint_ramfs_close_owner(p->pid);
    mint_mfs_close_all(p->pid);
    /* Also restore this task's ordinary transition stack if activation failed
       before we entered ring 3. This keeps every error path symmetric. */
    if (p->prior_privilege_stack_top &&
        !mint_task_set_privilege_stack_top(p->prior_privilege_stack_top)) {
        mint_serial_write("[proc] prior privilege-stack restoration failed\n");
        p->state = MINT_PROCESS_FAILED;
    }
    {
        MINT_ASPACE_USAGE usage;
        mint_address_space_get_usage(&p->address_space, &usage);
        p->peak_pages = usage.peak_owned_pages;
    }
    if (p->address_space.initialized && !p->address_space.active) {
        if (!mint_address_space_destroy(&p->address_space)) {
            mint_serial_write("[proc] address-space teardown failed\n");
            p->state = MINT_PROCESS_FAILED;
        }
    }
    if (p->address_space.initialized) {
        mint_serial_write("[proc] address-space resources remain after teardown\n");
        p->state = MINT_PROCESS_FAILED;
    }
    if (p->kernel_entry_stack) {
        if (!mint_vmalloc_free(p->kernel_entry_stack)) {
            mint_serial_write("[proc] transition-stack release failed\n");
            p->state = MINT_PROCESS_FAILED;
        } else {
            p->kernel_entry_stack = 0;
        }
    }
    if (p->kernel_entry_stack) p->state = MINT_PROCESS_FAILED;
    if (!mint_task_detach_process(p)) {
        mint_serial_write("[proc] scheduler/process ownership detach failed\n");
        p->state = MINT_PROCESS_FAILED;
    }
    mint_serial_write("[proc] process pid="); mint_serial_dec64(p->pid);
    mint_serial_write(" exited status="); mint_serial_dec64(p->exit_code);
    mint_serial_write(" peak-pages="); mint_serial_dec64(p->peak_pages);
    mint_serial_write(p->state == MINT_PROCESS_EXITED ? " (clean)\n" : " (failed)\n");
    if (p->state == MINT_PROCESS_EXITED)
        mint_serial_write("[proc] address space and transition stack reaped cleanly\n");
    finish_exit(p);
    return p->state == MINT_PROCESS_EXITED;
}

uint64_t mint_process_pid(const MINT_PROCESS *p) { return p && p->allocated ? p->pid : 0; }
uint64_t mint_process_exit_code(const MINT_PROCESS *p) { return p && p->allocated ? p->exit_code : UINT64_MAX; }
MINT_PROCESS_STATE mint_process_state(const MINT_PROCESS *p) {
    return p && p->allocated ? (MINT_PROCESS_STATE)p->state : MINT_PROCESS_UNUSED;
}

/* ----------------------------------------------------- kill / trap returns */
void mint_process_trap_return_check(void) {
    MINT_PROCESS *p = mint_process_current();
    if (!p || !p->kill_pending || !mint_task_current_user_transition()) return;
    if (!mint_address_space_activate_kernel()) return;
    mint_task_user_exit_record(MINT_EXIT_KILLED_BIT | p->kill_signal);
    mint_arch_return_from_user_fault();
}

static int is_descendant(uint64_t pid, uint64_t ancestor) {
    for (unsigned depth = 0; depth < MINT_PROCESS_MAX && pid; ++depth) {
        MINT_PROCESS *p = mint_process_find(pid);
        if (!p) return 0;
        if (p->parent_pid == ancestor) return 1;
        pid = p->parent_pid;
    }
    return 0;
}

int64_t mint_process_kill(uint64_t caller_pid, uint64_t pid, uint32_t signal) {
    MINT_PROCESS *p = mint_process_find(pid);
    if (!p || p->zombie || slot_free(p)) return -MINT_ESRCH;
    if (signal > 64u) return -MINT_EINVAL;
    /* No users or permissions yet: a process may signal itself and its
     * descendants (Terminal's programs are children of MintDE). */
    if (caller_pid && pid != caller_pid && !is_descendant(pid, caller_pid)) return -MINT_EPERM;
    if (signal == 0) return 0;
    if (!p->kill_pending) {
        p->kill_signal = signal;
        p->kill_pending = 1u;
        mint_task_wake(pid);
        mint_serial_write("[proc] kill pid="); mint_serial_dec64(pid);
        mint_serial_write(" signal="); mint_serial_dec64(signal); mint_serial_write("\n");
    }
    return 0;
}

/* ------------------------------------------------------------ spawn / wait */
typedef struct mfs_reader_ctx { int64_t handle; uint64_t pid; } MFS_READER_CTX;

static int mfs_reader(void *context, uint64_t offset, void *out, size_t length) {
    MFS_READER_CTX *c = (MFS_READER_CTX *)context;
    if (mint_mfs_seek(c->handle, (int64_t)offset, MINT_SEEK_SET, c->pid) != (int64_t)offset) return 0;
    uint8_t *o = (uint8_t *)out;
    size_t done = 0;
    while (done < length) {
        uint32_t chunk = (uint32_t)(length - done > MINT_MFS_IO_MAX ? MINT_MFS_IO_MAX : length - done);
        int64_t got = mint_mfs_read(c->handle, o + done, chunk, c->pid);
        if (got <= 0) return 0;
        done += (size_t)got;
    }
    return 1;
}

static void program_runner(void *argument) {
    MINT_PROCESS *p = (MINT_PROCESS *)argument;
    if (!p || !p->allocated || p->state != MINT_PROCESS_STARTING || p->pid != mint_task_current_pid()) {
        mint_serial_write("[proc] runner started with an invalid process slot\n");
        return;
    }
    MINT_MFS_STAT st;
    MFS_READER_CTX ctx = {-1, p->pid};
    int ok = mint_mfs_stat(p->path, &st) == 0 && st.type == MINT_MFS_FILE && st.size >= 64u;
    if (ok) {
        ctx.handle = mint_mfs_open(p->path, MINT_O_READ, p->pid);
        ok = ctx.handle >= 0;
    }
    if (ok) ok = prepare_image(p, mfs_reader, &ctx, (size_t)st.size,
                               p->stack_bytes ? p->stack_bytes : MINT_USER_PROGRAM_STACK_BYTES);
    if (ctx.handle >= 0) (void)mint_mfs_close(ctx.handle, p->pid);
    if (ok) {
        int attached = attach_to_current_task(p);
        if (attached != 1) {
            rollback_create(p, attached < 0 && mint_task_current_process() == p);
            ok = 0;
        }
    } else if (p->address_space.initialized && !p->address_space.active) {
        (void)mint_address_space_destroy(&p->address_space);
    }
    if (!ok) {
        mint_serial_write("[proc] could not load program ");
        mint_serial_write(p->path);
        mint_serial_write("\n");
        p->exit_code = MINT_EXIT_LOAD_FAILED;
        p->state = MINT_PROCESS_FAILED;
        mint_fd_close_all(p);
        mint_mfs_close_all(p->pid);
        finish_exit(p);
        return;
    }
    mint_serial_write("[proc] started "); mint_serial_write(p->path);
    mint_serial_write(" pid="); mint_serial_dec64(p->pid);
    mint_serial_write(" argc="); mint_serial_dec64(p->argc);
    mint_serial_write(" entry="); mint_serial_hex64(p->image.entry);
    mint_serial_write("\n");
    p->state = MINT_PROCESS_READY;
    (void)mint_process_run_current(p);
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; ++c) if (*c == '/' && c[1]) b = c + 1;
    return b;
}

int64_t mint_process_spawn(uint64_t parent_pid, const char *path, const char *cwd,
                           const uint8_t *args, uint32_t args_bytes, uint32_t argc, uint32_t envc,
                           const int32_t child_ofds[3], uint32_t flags, uint32_t frame_limit,
                           uint32_t stack_bytes) {
    (void)flags;
    if (!path || path[0] != '/' || argc > MINT_PROC_ARGC_MAX || envc > MINT_PROC_ARGC_MAX ||
        args_bytes > MINT_PROC_ARGS_MAX || (args_bytes && !args) ||
        frame_limit > MINT_ASPACE_FRAME_LIMIT_MAX ||
        (stack_bytes && (stack_bytes < 16384u || stack_bytes > MINT_USER_MAX_STACK_BYTES || (stack_bytes & 4095u))))
        return -MINT_EINVAL;
    /* argv/envp must be exactly argc + envc NUL-terminated strings. */
    uint32_t strings = 0;
    for (uint32_t i = 0; i < args_bytes; ++i) if (!args[i]) ++strings;
    if (strings != argc + envc || (args_bytes && args[args_bytes - 1u])) return -MINT_EINVAL;
    /* Validate the file up front so "not found" and "not a program" are
     * reported synchronously, like execve. */
    MINT_MFS_STAT st;
    int64_t rc = mint_mfs_stat(path, &st);
    if (rc < 0) return rc;
    if (st.type != MINT_MFS_FILE) return -MINT_EACCES;
    {
        uint64_t check_pid = parent_pid ? parent_pid : mint_task_current_pid();
        int64_t h = mint_mfs_open(path, MINT_O_READ, check_pid);
        if (h < 0) return h;
        uint8_t magic[4] = {0, 0, 0, 0};
        int64_t got = mint_mfs_read(h, magic, 4u, check_pid);
        (void)mint_mfs_close(h, check_pid);
        if (got != 4 || magic[0] != 0x7Fu || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F')
            return -MINT_ENOEXEC;
    }
    MINT_PROCESS *p = find_process_slot();
    if (!p) return -MINT_EAGAIN;
    zero_bytes(p, sizeof(*p));
    p->allocated = 1u;
    p->state = MINT_PROCESS_STARTING;
    p->parent_pid = parent_pid;
    p->waitable = parent_pid != 0;
    p->frame_limit = frame_limit;
    p->stack_bytes = stack_bytes;
    p->spawn_flags = flags;
    p->argc = argc; p->envc = envc; p->args_bytes = args_bytes;
    p->start_tick = mint_timer_ticks();
    text_copy(p->path, sizeof(p->path), path);
    text_copy(p->name, sizeof(p->name), base_name(path));
    text_copy(p->cwd, sizeof(p->cwd), cwd && cwd[0] == '/' ? cwd : "/");
    mint_fd_table_init(p);
    if (args_bytes) {
        p->args = (uint8_t *)mint_vmalloc_alloc(args_bytes, MINT_PAGE_MAP_WRITE);
        if (!p->args) { zero_bytes(p, sizeof(*p)); return -MINT_ENOMEM; }
        for (uint32_t i = 0; i < args_bytes; ++i) p->args[i] = args[i];
    }
    int64_t pid = mint_task_create(p->name, program_runner, p);
    if (pid <= 0) {
        if (p->args) (void)mint_vmalloc_free(p->args);
        zero_bytes(p, sizeof(*p));
        return pid < 0 ? pid : -MINT_EAGAIN;
    }
    p->pid = (uint64_t)pid;
    /* From here on the child owns the descriptor references. */
    for (int fd = 0; fd < 3; ++fd)
        if (child_ofds && child_ofds[fd] >= 0) (void)mint_fd_install_at(p, child_ofds[fd], fd, 0);
    return pid;
}

int64_t mint_process_wait(uint64_t parent_pid, int64_t pid, uint32_t options, MINT_PROC_STATUS *out) {
    for (;;) {
        int have_child = 0;
        for (unsigned i = 0; i < MINT_PROCESS_MAX; ++i) {
            MINT_PROCESS *c = &processes[i];
            if (!c->allocated || !c->waitable || c->parent_pid != parent_pid) continue;
            if (pid > 0 && c->pid != (uint64_t)pid) continue;
            if (!c->zombie && slot_free(c)) continue;
            have_child = 1;
            if (!c->zombie) continue;
            if (out) {
                mint_process_decode_status(c->exit_code, out);
                out->pid = c->pid;
            }
            uint64_t reaped = c->pid;
            c->zombie = 0;
            c->waitable = 0;
            return (int64_t)reaped;
        }
        if (!have_child) return -MINT_ECHILD;
        if (options & MINT_WAIT_NOHANG) return 0;
        if (mint_process_current_killed()) return -MINT_EINTR;
        mint_task_sleep_ticks(1);
    }
}

uint32_t mint_process_list(MINT_PROC_INFO *out, uint32_t capacity) {
    uint32_t n = 0;
    for (unsigned i = 0; i < MINT_PROCESS_MAX && n < capacity; ++i) {
        MINT_PROCESS *p = &processes[i];
        if (!p->allocated || slot_free(p)) continue;
        MINT_PROC_INFO *info = &out[n++];
        zero_bytes(info, sizeof(*info));
        info->pid = p->pid;
        info->parent_pid = p->parent_pid;
        info->state = p->zombie ? MINT_PROC_INFO_ZOMBIE :
                      p->state == MINT_PROCESS_STARTING ? MINT_PROC_INFO_STARTING : MINT_PROC_INFO_RUNNING;
        MINT_ASPACE_USAGE usage;
        mint_address_space_get_usage(&p->address_space, &usage);
        info->owned_pages = usage.owned_pages;
        info->peak_pages = usage.peak_owned_pages > p->peak_pages ? usage.peak_owned_pages : p->peak_pages;
        info->frame_limit = usage.frame_limit ? usage.frame_limit :
                            (p->frame_limit ? p->frame_limit : MINT_ASPACE_MAX_USER_FRAMES);
        info->cpu_ticks = p->zombie ? p->user_ticks : mint_task_user_ticks(p->pid);
        info->start_tick = p->start_tick;
        text_copy(info->name, sizeof(info->name), p->name);
    }
    return n;
}

uint64_t mint_process_live_count(void) {
    uint64_t n = 0;
    for (unsigned i = 0; i < MINT_PROCESS_MAX; ++i)
        if (processes[i].allocated && !slot_free(&processes[i]) && !processes[i].zombie) ++n;
    return n;
}
