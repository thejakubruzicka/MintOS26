/* 26.5.014.0: MintOS-native process, pipe and memory services
 * (int 0x80, numbers 0x4D60-0x4D7F; see uapi/mintos/process.h). */
#include <stdint.h>
#include <stddef.h>
#include "../include/proc_syscalls.h"
#include "../include/process.h"
#include "../include/proc_files.h"
#include "../include/proc_memory.h"
#include "../include/address_space.h"
#include "../include/task.h"
#include "../include/fpu.h"
#include "../include/frame_pool.h"
#include "../include/programs.h"
#include "../include/linux_compat.h"
#include "../include/uapi/mintos/syscall.h"
#include "../include/uapi/mintos/process.h"

static int copy_cstring(const MINT_ADDRESS_SPACE *space, uint64_t address, char *out, size_t capacity) {
    if (!address || !capacity) return -MINT_EFAULT;
    for (size_t i = 0; i < capacity; ++i) {
        if (!mint_address_space_copy_from_user(space, &out[i], address + i, 1u)) return -MINT_EFAULT;
        if (!out[i]) return 0;
    }
    out[capacity - 1u] = '\0';
    return -MINT_ENAMETOOLONG;
}

static int32_t make_pipe_ofds(uint32_t tty, int32_t *read_ofd, int32_t *write_ofd) {
    int32_t pipe = mint_pipe_create();
    if (pipe < 0) return pipe;
    uint32_t extra = tty ? MINT_OFD_TTY : 0u;
    *read_ofd = mint_ofd_create(MINT_OFD_PIPE_READ, MINT_OFD_READ | extra, pipe, 0);
    if (*read_ofd < 0) return *read_ofd;
    *write_ofd = mint_ofd_create(MINT_OFD_PIPE_WRITE, MINT_OFD_WRITE | extra, pipe, 0);
    if (*write_ofd < 0) { mint_ofd_unref(*read_ofd); return *write_ofd; }
    return 0;
}

static int64_t sys_spawn(MINT_PROCESS *self, const MINT_ADDRESS_SPACE *space, uint64_t request_va) {
    static MINT_SPAWN_REQUEST req;
    static char path[MINT_PROC_PATH_MAX], cwd[MINT_PROC_PATH_MAX];
    static uint8_t args[MINT_PROC_ARGS_MAX];
    if (!mint_address_space_copy_from_user(space, &req, request_va, sizeof(req))) return -MINT_EFAULT;
    if (req.version != MINT_SPAWN_VERSION || (req.flags & ~(MINT_SPAWN_PIPE_STDIO | MINT_SPAWN_TTY)) ||
        req.args_bytes > MINT_PROC_ARGS_MAX) return -MINT_EINVAL;
    int rc = copy_cstring(space, req.path, path, sizeof(path));
    if (rc) return rc;
    cwd[0] = '/'; cwd[1] = '\0';
    if (req.cwd && (rc = copy_cstring(space, req.cwd, cwd, sizeof(cwd)))) return rc;
    if (req.args_bytes && !mint_address_space_copy_from_user(space, args, req.args, req.args_bytes))
        return -MINT_EFAULT;
    int32_t child[3] = {-1, -1, -1};
    int32_t parent_in = -1, parent_out = -1;
    if (req.flags & MINT_SPAWN_PIPE_STDIO) {
        uint32_t tty = (req.flags & MINT_SPAWN_TTY) != 0;
        int32_t in_r = -1, in_w = -1, out_r = -1, out_w = -1;
        if ((rc = make_pipe_ofds(tty, &in_r, &in_w)) < 0) return rc;
        if ((rc = make_pipe_ofds(tty, &out_r, &out_w)) < 0) { mint_ofd_unref(in_r); mint_ofd_unref(in_w); return rc; }
        child[0] = in_r; child[1] = out_w; child[2] = out_w;
        mint_ofd_ref(out_w);                 /* stdout and stderr share one description */
        parent_in = in_w; parent_out = out_r;
    } else {
        for (int fd = 0; fd < 3; ++fd) {
            child[fd] = mint_fd_ofd(self, fd);
            if (child[fd] >= 0) mint_ofd_ref(child[fd]);
        }
    }
    int64_t pid = mint_process_spawn(self->pid, path, cwd, args, req.args_bytes, req.argc, req.envc,
                                     child, req.flags, req.frame_limit, req.stack_bytes);
    if (pid < 0) {
        for (int fd = 0; fd < 3; ++fd) if (child[fd] >= 0) mint_ofd_unref(child[fd]);
        if (parent_in >= 0) mint_ofd_unref(parent_in);
        if (parent_out >= 0) mint_ofd_unref(parent_out);
        return pid;
    }
    req.stdin_fd = -1; req.stdout_fd = -1;
    if (parent_in >= 0) {
        int64_t fd = mint_fd_install(self, parent_in, 3, MINT_FD_FLAG_CLOEXEC);
        if (fd < 0) mint_ofd_unref(parent_in); else req.stdin_fd = (int32_t)fd;
    }
    if (parent_out >= 0) {
        int64_t fd = mint_fd_install(self, parent_out, 3, MINT_FD_FLAG_CLOEXEC);
        if (fd < 0) mint_ofd_unref(parent_out); else req.stdout_fd = (int32_t)fd;
    }
    (void)mint_address_space_copy_to_user(space, request_va, &req, sizeof(req));
    return pid;
}

void mint_platform_info_fill(MINT_PLATFORM_INFO *out) {
    MINT_FPU_INFO fpu;
    MINT_FRAME_POOL_STATS pool;
    mint_fpu_get_info(&fpu);
    mint_frame_pool_get_stats(&pool);
    for (size_t i = 0; i < sizeof(*out); ++i) ((uint8_t *)out)[i] = 0;
    out->version = MINT_PLATFORM_INFO_VERSION;
    out->fpu_flags = fpu.flags;
    out->fpu_area_bytes = fpu.area_bytes;
    out->linux_compat = mint_linux_compat_enabled() ? 1u : 0u;
    out->xcr0 = fpu.xcr0;
    out->fpu_saves = fpu.saves;
    out->fpu_restores = fpu.restores;
    out->preemptions = mint_task_total_preemptions();
    out->pool_pages = pool.pool_pages;
    out->pool_used_pages = pool.used_pages;
    out->pool_peak_pages = pool.peak_used_pages;
    out->processes_live = mint_process_live_count();
    out->tasks_live = mint_task_live_count();
    out->linux_syscalls = mint_linux_compat_calls();
    out->linux_unsupported = mint_linux_compat_unsupported();
}

int64_t mint_proc_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const MINT_ADDRESS_SPACE *space = mint_address_space_current();
    MINT_PROCESS *self = mint_process_current();
    if (!space || !self) return -MINT_EPERM;
    switch (number) {
    case MINT_SYS_PROC_SPAWN:
        return sys_spawn(self, space, a0);
    case MINT_SYS_PROC_WAIT: {
        MINT_PROC_STATUS status = {0, 0, 0, 0};
        if (a1 && !mint_address_space_validate_user_range(space, a1, sizeof(status), 1)) return -MINT_EFAULT;
        int64_t rc = mint_process_wait(self->pid, (int64_t)a0, (uint32_t)a2, &status);
        if (rc > 0 && a1 && !mint_address_space_copy_to_user(space, a1, &status, sizeof(status))) return -MINT_EFAULT;
        return rc;
    }
    case MINT_SYS_PROC_KILL:
        return mint_process_kill(self->pid, a0, (uint32_t)a1);
    case MINT_SYS_PROC_LIST: {
        static MINT_PROC_INFO list[MINT_PROCESS_MAX];
        uint32_t cap = a1 > MINT_PROCESS_MAX ? MINT_PROCESS_MAX : (uint32_t)a1;
        uint32_t n = mint_process_list(list, cap);
        if (n && !mint_address_space_copy_to_user(space, a0, list, n * sizeof(list[0]))) return -MINT_EFAULT;
        return n;
    }
    case MINT_SYS_FD_READ:
        return mint_fd_read_user(self, (int64_t)a0, a1, a2, (a3 & MINT_IO_NONBLOCK) != 0);
    case MINT_SYS_FD_WRITE:
        return mint_fd_write_user(self, (int64_t)a0, a1, a2, (a3 & MINT_IO_NONBLOCK) != 0);
    case MINT_SYS_FD_CLOSE:
        return mint_fd_close(self, (int64_t)a0);
    case MINT_SYS_PIPE: {
        int32_t r = -1, w = -1, fds[2];
        if (!mint_address_space_validate_user_range(space, a0, sizeof(fds), 1)) return -MINT_EFAULT;
        int32_t rc = make_pipe_ofds(0, &r, &w);
        if (rc < 0) return rc;
        int64_t fr = mint_fd_install(self, r, 0, 0);
        int64_t fw = fr >= 0 ? mint_fd_install(self, w, 0, 0) : -MINT_EMFILE;
        if (fr < 0 || fw < 0) {
            if (fr >= 0) (void)mint_fd_close(self, fr); else mint_ofd_unref(r);
            mint_ofd_unref(w);
            return -MINT_EMFILE;
        }
        fds[0] = (int32_t)fr; fds[1] = (int32_t)fw;
        return mint_address_space_copy_to_user(space, a0, fds, sizeof(fds)) ? 0 : -MINT_EFAULT;
    }
    case MINT_SYS_MEM_MAP:
        return mint_proc_mmap(self, a0, a1, (uint32_t)a2, 0);
    case MINT_SYS_MEM_UNMAP:
        return mint_proc_munmap(self, a0, a1);
    case MINT_SYS_MEM_PROTECT:
        return mint_proc_mprotect(self, a0, a1, (uint32_t)a2);
    case MINT_SYS_MEM_BRK:
        return (int64_t)mint_proc_brk(self, a0);
    case MINT_SYS_PLATFORM_INFO: {
        MINT_PLATFORM_INFO info;
        mint_platform_info_fill(&info);
        MINT_ASPACE_USAGE usage;
        mint_address_space_get_usage(&self->address_space, &usage);
        info.demand_faults = usage.demand_faults;
        return mint_address_space_copy_to_user(space, a0, &info, sizeof(info)) ? 0 : -MINT_EFAULT;
    }
    case MINT_SYS_PROGRAMS_INSTALL:
        return mint_programs_install(1);
    default:
        return -MINT_ENOSYS;
    }
}
