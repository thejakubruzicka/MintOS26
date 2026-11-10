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

static MINT_PROCESS processes[MINT_PROCESS_MAX];

static void zero_bytes(void *ptr, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < n; ++i) p[i] = 0;
}

void mint_process_system_init(void) {
    zero_bytes(processes, sizeof(processes));
    mint_serial_write("[proc] process table initialized; bounded ELF64 loader enabled\n");
}

static MINT_PROCESS *find_process_slot(void) {
    for (unsigned i = 0; i < MINT_PROCESS_MAX; ++i) {
        if (!processes[i].allocated || processes[i].state == MINT_PROCESS_EXITED ||
            processes[i].state == MINT_PROCESS_FAILED) return &processes[i];
    }
    return 0;
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
    zero_bytes(p, sizeof(*p));
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
    p->pid = mint_task_current_pid(); /* initial model: one process per owning task */
    p->parent_pid = 0;
    if (!mint_address_space_create(&p->address_space)) {
        mint_serial_write("[proc] unable to create process address space\n");
        rollback_create(p, 0);
        return 0;
    }
    if (!mint_elf64_load_user(&p->address_space, image, image_size,
                              MINT_USER_DEFAULT_STACK_BYTES, &p->image)) {
        mint_serial_write("[proc] static ELF64 validation/load failed\n");
        rollback_create(p, 0);
        return 0;
    }
    p->kernel_entry_stack = mint_vmalloc_alloc(MINT_PROCESS_KERNEL_STACK_BYTES, MINT_PAGE_MAP_WRITE);
    if (!p->kernel_entry_stack) {
        mint_serial_write("[proc] unable to allocate process privilege-transition stack\n");
        rollback_create(p, 0);
        return 0;
    }
    p->prior_privilege_stack_top = mint_task_privilege_stack_top();
    if (!p->prior_privilege_stack_top ||
        !mint_task_attach_process(p) ||
        !mint_task_set_privilege_stack_top((uint64_t)(uintptr_t)p->kernel_entry_stack + MINT_PROCESS_KERNEL_STACK_BYTES)) {
        mint_serial_write("[proc] failed to attach process to scheduler task\n");
        rollback_create(p, mint_task_current_process() == p);
        return 0;
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
    mint_arch_enter_user(p->image.entry, p->image.stack_top);
    /* SYS_EXIT switches to the kernel root and restores this scheduler task's
       stack. Re-enable its ordinary RSP0 before reaping the process. */
    if (p->address_space.active) (void)mint_address_space_activate_kernel();
    (void)mint_task_set_privilege_stack_top(p->prior_privilege_stack_top);
    if (mint_syscall_user_exit_seen()) {
        p->exit_code = mint_syscall_user_exit_code();
        p->state = p->exit_code == 0 ? MINT_PROCESS_EXITED : MINT_PROCESS_FAILED;
    } else {
        p->state = MINT_PROCESS_FAILED;
        mint_serial_write("[proc] user image returned without SYS_EXIT\n");
    }
cleanup:
    /* File handles are process-owned resources. Reap them on every process
       exit/failure path so the bounded handle table cannot leak across runs. */
    mint_ramfs_close_owner(p->pid);
    mint_mfs_close_all(p->pid);
    /* Also restore this task's ordinary transition stack if activation failed
       before we entered ring 3. This keeps every error path symmetric. */
    if (p->prior_privilege_stack_top &&
        !mint_task_set_privilege_stack_top(p->prior_privilege_stack_top)) {
        mint_serial_write("[proc] prior privilege-stack restoration failed\n");
        p->state = MINT_PROCESS_FAILED;
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
    mint_serial_write(p->state == MINT_PROCESS_EXITED ? " (clean)\n" : " (failed)\n");
    if (p->state == MINT_PROCESS_EXITED)
        mint_serial_write("[proc] address space and transition stack reaped cleanly\n");
    return p->state == MINT_PROCESS_EXITED;
}

uint64_t mint_process_pid(const MINT_PROCESS *p) { return p && p->allocated ? p->pid : 0; }
uint64_t mint_process_exit_code(const MINT_PROCESS *p) { return p && p->allocated ? p->exit_code : UINT64_MAX; }
MINT_PROCESS_STATE mint_process_state(const MINT_PROCESS *p) {
    return p && p->allocated ? (MINT_PROCESS_STATE)p->state : MINT_PROCESS_UNUSED;
}
