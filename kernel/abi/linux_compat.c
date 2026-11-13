/* 26.5.014.0: Linux x86-64 syscall-compatibility subset (see linux_compat.h).
 * Every handler translates to MintOS services: MintFS files, MintOS pipes,
 * demand-zero memory and the MintOS process manager. User memory is only
 * touched through the validated copy helpers. */
#include <stdint.h>
#include <stddef.h>
#include "../include/linux_compat.h"
#include "../include/syscall.h"
#include "../include/process.h"
#include "../include/proc_files.h"
#include "../include/proc_memory.h"
#include "../include/address_space.h"
#include "../include/task.h"
#include "../include/arch.h"
#include "../include/serial.h"
#include "../include/rtc.h"
#include "../include/physmem.h"
#include "../include/mintos_fs.h"
#include "../include/uapi/mintos/syscall.h"
#include "../include/uapi/mintos/process.h"

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u

#define AT_FDCWD (-100)
#define AT_EMPTY_PATH 0x1000u
#define AT_REMOVEDIR 0x200u

extern void mint_linux_syscall_entry(void);

static int compat_ready;
static uint64_t calls, unsupported;
static uint8_t logged_unsupported[64];   /* one bit per syscall number < 512 */

static void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}
static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

int mint_linux_compat_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0u));
    if (!(d & (1u << 11))) {
        mint_serial_write("[linux] CPU lacks SYSCALL/SYSRET; Linux-compat entry disabled\n");
        return 0;
    }
    /* STAR[47:32]: kernel CS 0x08 (SS = 0x10). SYSRET is never executed (the
     * return path is IRETQ), so STAR[63:48] is left zero. */
    wrmsr(MSR_STAR, (uint64_t)0x08u << 32);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)mint_linux_syscall_entry);
    /* Clear IF, TF, DF, AC and NT on entry: the entry runs on the user stack
     * until it switches to RSP0 and must not be interrupted there. */
    wrmsr(MSR_SFMASK, (1u << 9) | (1u << 8) | (1u << 10) | (1u << 18) | (1u << 14));
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);   /* EFER.SCE */
    compat_ready = 1;
    mint_serial_write("[linux] syscall-instruction entry online: Linux x86-64 numbers -> MintOS services (subset of ");
    mint_serial_dec64(mint_linux_syscall_table_count);
    mint_serial_write(" calls; not a Linux kernel)\n");
    return 1;
}
int mint_linux_compat_enabled(void) { return compat_ready; }
uint64_t mint_linux_compat_calls(void) { return calls; }
uint64_t mint_linux_compat_unsupported(void) { return unsupported; }

/* ------------------------------------------------------------- helpers */
static const MINT_ADDRESS_SPACE *cur_space(void) { return mint_address_space_current(); }

static int64_t get_user_path(uint64_t address, char *out, size_t capacity) {
    const MINT_ADDRESS_SPACE *space = cur_space();
    if (!address) return -MINT_EFAULT;
    for (size_t i = 0; i < capacity; ++i) {
        if (!mint_address_space_copy_from_user(space, &out[i], address + i, 1u)) return -MINT_EFAULT;
        if (!out[i]) return (int64_t)i;
    }
    return -MINT_ENAMETOOLONG;
}

static int put_user(uint64_t address, const void *data, size_t length) {
    return mint_address_space_copy_to_user(cur_space(), address, data, length);
}
static int get_user(void *out, uint64_t address, size_t length) {
    return mint_address_space_copy_from_user(cur_space(), out, address, length);
}

/* Resolve (dirfd, path) to an absolute, normalized MintOS HD path. */
static int64_t resolve_at(MINT_PROCESS *p, int64_t dirfd, uint64_t user_path, char *out, size_t capacity) {
    static char raw[MINT_PROC_PATH_MAX];
    int64_t n = get_user_path(user_path, raw, sizeof(raw));
    if (n < 0) return n;
    const char *base = p->cwd;
    if (raw[0] != '/' && dirfd != AT_FDCWD) {
        int32_t ofd = mint_fd_ofd(p, dirfd);
        if (ofd < 0) return -MINT_EBADF;
        if (mint_ofd_kind(ofd) != MINT_OFD_DIR) return -MINT_ENOTDIR;
        base = mint_dir_handle_path(mint_ofd_object(ofd));
        if (!base) return -MINT_EBADF;
    }
    return mint_linux_path_resolve(base, raw, out, capacity);
}

static int64_t stat_path(const char *path, MINT_LINUX_STAT *st) {
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/') {
        mint_linux_stat_fill(st, MINT_LINUX_S_IFCHR | 0666u, 3u, 0, 0, 0);
        return 0;
    }
    MINT_MFS_STAT m;
    int64_t rc = mint_mfs_stat(path, &m);
    if (rc < 0) return rc;
    uint32_t mode = m.type == MINT_MFS_DIR ? (MINT_LINUX_S_IFDIR | 0755u) : (MINT_LINUX_S_IFREG | 0755u);
    mint_linux_stat_fill(st, mode, m.ino, m.size, m.mtime, m.ctime);
    return 0;
}

static int64_t stat_fd(MINT_PROCESS *p, int64_t fd, MINT_LINUX_STAT *st) {
    int32_t ofd = mint_fd_ofd(p, fd);
    if (ofd < 0) return -MINT_EBADF;
    switch (mint_ofd_kind(ofd)) {
    case MINT_OFD_FILE: {
        MINT_MFS_STAT m;
        int64_t rc = mint_mfs_fstat(mint_ofd_object(ofd), p->pid, &m);
        if (rc < 0) return rc;
        mint_linux_stat_fill(st, MINT_LINUX_S_IFREG | 0755u, m.ino, m.size, m.mtime, m.ctime);
        return 0;
    }
    case MINT_OFD_DIR: {
        const char *path = mint_dir_handle_path(mint_ofd_object(ofd));
        return path ? stat_path(path, st) : -MINT_EBADF;
    }
    case MINT_OFD_PIPE_READ: case MINT_OFD_PIPE_WRITE:
        mint_linux_stat_fill(st, ((mint_ofd_flags(ofd) & MINT_OFD_TTY) ? MINT_LINUX_S_IFCHR : MINT_LINUX_S_IFIFO) | 0620u,
                             (uint64_t)mint_ofd_object(ofd) + 1000u, 0, 0, 0);
        return 0;
    default:
        mint_linux_stat_fill(st, MINT_LINUX_S_IFCHR | 0620u, 2u, 0, 0, 0);
        return 0;
    }
}

static int64_t do_open(MINT_PROCESS *p, int64_t dirfd, uint64_t user_path, uint64_t flags) {
    static char path[MINT_PROC_PATH_MAX];
    int64_t rc = resolve_at(p, dirfd, user_path, path, sizeof(path));
    if (rc < 0) return rc;
    int64_t mflags = mint_linux_open_flags(flags);
    if (mflags < 0) return mflags;
    uint32_t fdflags = (flags & MINT_LINUX_O_CLOEXEC) ? MINT_FD_FLAG_CLOEXEC : 0u;
    uint32_t oflags = ((mflags & MINT_O_READ) ? MINT_OFD_READ : 0u) | ((mflags & MINT_O_WRITE) ? MINT_OFD_WRITE : 0u) |
                      ((flags & MINT_LINUX_O_NONBLOCK) ? MINT_OFD_NONBLOCK : 0u) |
                      ((flags & MINT_LINUX_O_APPEND) ? MINT_OFD_APPEND : 0u);
    int32_t ofd = -1;
    /* A few device names. */
    const char *dev = path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/' ? path + 5 : 0;
    if (dev) {
        int64_t same = -1;
        if (dev[0] == 'n' && dev[1] == 'u' && dev[2] == 'l' && dev[3] == 'l' && !dev[4])
            ofd = mint_ofd_create(MINT_OFD_NULL, oflags, -1, p->pid);
        else if (dev[0] == 't' && dev[1] == 't' && dev[2] == 'y' && !dev[3]) same = mint_fd_is_tty(p, 1) ? 1 : 0;
        else if (dev[0] == 's' && dev[1] == 't' && dev[2] == 'd') {
            if (dev[3] == 'i') same = 0; else if (dev[3] == 'o') same = 1; else if (dev[3] == 'e') same = 2;
        }
        if (same >= 0) {
            ofd = mint_fd_ofd(p, same);
            if (ofd < 0) return -MINT_EBADF;
            mint_ofd_ref(ofd);
        }
        if (ofd < 0) return -MINT_ENOENT;
    } else {
        MINT_MFS_STAT st;
        int64_t s = mint_mfs_stat(path, &st);
        if (s == 0 && st.type == MINT_MFS_DIR) {
            if (mflags & MINT_O_WRITE) return -MINT_EISDIR;
            int32_t h = mint_dir_handle_open(path);
            if (h < 0) return h;
            ofd = mint_ofd_create(MINT_OFD_DIR, MINT_OFD_READ, h, p->pid);
            if (ofd < 0) { mint_dir_handle_close(h); return ofd; }
        } else {
            if (flags & MINT_LINUX_O_DIRECTORY) return s < 0 ? s : -MINT_ENOTDIR;
            int64_t h = mint_mfs_open(path, (uint32_t)mflags, p->pid);
            if (h < 0) return h;
            ofd = mint_ofd_create(MINT_OFD_FILE, oflags, (int32_t)h, p->pid);
            if (ofd < 0) { (void)mint_mfs_close(h, p->pid); return ofd; }
        }
    }
    int64_t fd = mint_fd_install(p, ofd, 0, fdflags);
    if (fd < 0) mint_ofd_unref(ofd);
    return fd;
}

static int64_t do_iov(MINT_PROCESS *p, int64_t fd, uint64_t iov, uint64_t count, int write) {
    if (count > 1024u) return -MINT_EINVAL;
    int64_t total = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t v[2];
        if (!get_user(v, iov + i * 16u, sizeof(v))) return total ? total : -MINT_EFAULT;
        if (!v[1]) continue;
        int64_t n = write ? mint_fd_write_user(p, fd, v[0], v[1], 0) : mint_fd_read_user(p, fd, v[0], v[1], 0);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < v[1]) break;
    }
    return total;
}

static uint64_t realtime_seconds(void) {
    return mint_rtc_boot_epoch() + mint_timer_ticks() / 100u;
}

static int64_t clock_now(int64_t clock, int64_t *sec, int64_t *nsec) {
    uint64_t ticks = mint_timer_ticks();
    MINT_PROCESS *p = mint_process_current();
    switch (clock) {
    case 0: case 5:              /* REALTIME, REALTIME_COARSE */
        *sec = (int64_t)(mint_rtc_boot_epoch() + ticks / 100u);
        *nsec = (int64_t)(ticks % 100u) * 10000000;
        return 0;
    case 1: case 4: case 6: case 7:   /* MONOTONIC, _RAW, _COARSE, BOOTTIME */
        *sec = (int64_t)(ticks / 100u);
        *nsec = (int64_t)(ticks % 100u) * 10000000;
        return 0;
    case 2: case 3: {            /* PROCESS/THREAD CPU time */
        uint64_t used = p ? mint_task_user_ticks(p->pid) : 0;
        *sec = (int64_t)(used / 100u);
        *nsec = (int64_t)(used % 100u) * 10000000;
        return 0;
    }
    default:
        return -MINT_EINVAL;
    }
}

static int64_t sleep_ticks_interruptible(uint64_t ticks) {
    uint64_t end = mint_timer_ticks() + ticks;
    while (mint_timer_ticks() < end) {
        if (mint_process_current_killed()) return -MINT_EINTR;
        uint64_t left = end - mint_timer_ticks();
        mint_task_sleep_ticks(left > 10u ? 10u : left);
    }
    return 0;
}

static int64_t do_nanosleep(uint64_t req) {
    int64_t ts[2];
    if (!get_user(ts, req, sizeof(ts))) return -MINT_EFAULT;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000) return -MINT_EINVAL;
    uint64_t ticks = (uint64_t)ts[0] * 100u + ((uint64_t)ts[1] + 9999999u) / 10000000u;
    if (!ticks) { mint_task_yield(); return 0; }
    return sleep_ticks_interruptible(ticks);
}

static uint64_t rng_state;
static uint64_t rng_next(void) {
    if (!rng_state) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        rng_state = (((uint64_t)hi << 32) | lo) ^ 0x6A09E667F3BCC909ULL;
    }
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return rng_state ^ ((uint64_t)lo << 32);
}

static int64_t do_getdents(MINT_PROCESS *p, int64_t fd, uint64_t buffer, uint64_t count) {
    static MINT_MFS_DIRENT list[MINT_MFS_LIST_MAX];
    static uint8_t out[4096];
    int32_t ofd = mint_fd_ofd(p, fd);
    if (ofd < 0) return -MINT_EBADF;
    if (mint_ofd_kind(ofd) != MINT_OFD_DIR) return -MINT_ENOTDIR;
    const char *path = mint_dir_handle_path(mint_ofd_object(ofd));
    uint32_t *pos = mint_ofd_dir_pos(ofd);
    if (!path || !pos) return -MINT_EBADF;
    if (count > sizeof(out)) count = sizeof(out);
    uint64_t used = 0;
    for (;;) {
        /* Positions 0 and 1 are "." and ".."; 2.. are MintFS entries. */
        const char *name = 0;
        uint8_t type = 4;            /* DT_DIR */
        if (*pos == 0) name = ".";
        else if (*pos == 1) name = "..";
        else {
            int64_t n = mint_mfs_list(path, *pos - 2u, list, 1u);
            if (n < 0) return used ? (int64_t)used : n;
            if (n == 0) break;
            name = list[0].name;
            type = list[0].type == MINT_MFS_DIR ? 4u : 8u;   /* DT_REG */
        }
        size_t len = 0;
        while (name[len]) ++len;
        uint64_t reclen = (19u + len + 1u + 7u) & ~7ULL;
        if (used + reclen > count) {
            if (!used) return -MINT_EINVAL;
            break;
        }
        uint8_t *rec = out + used;
        for (uint64_t i = 0; i < reclen; ++i) rec[i] = 0;
        uint64_t ino = (uint64_t)*pos + 1u, off = (uint64_t)*pos + 1u;
        for (int i = 0; i < 8; ++i) { rec[i] = (uint8_t)(ino >> (8 * i)); rec[8 + i] = (uint8_t)(off >> (8 * i)); }
        rec[16] = (uint8_t)reclen; rec[17] = (uint8_t)(reclen >> 8); rec[18] = type;
        for (size_t i = 0; i < len; ++i) rec[19 + i] = (uint8_t)name[i];
        used += reclen;
        ++*pos;
    }
    if (used && !put_user(buffer, out, (size_t)used)) return -MINT_EFAULT;
    return (int64_t)used;
}

static int64_t do_poll(MINT_PROCESS *p, uint64_t fds, uint64_t nfds, int64_t timeout_ms) {
    if (nfds > MINT_PROC_FD_MAX) return -MINT_EINVAL;
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : mint_timer_ticks() + ((uint64_t)timeout_ms + 9u) / 10u;
    for (;;) {
        int64_t ready = 0;
        for (uint64_t i = 0; i < nfds; ++i) {
            int32_t e[2];   /* fd, events|revents<<16 */
            if (!get_user(e, fds + i * 8u, sizeof(e))) return -MINT_EFAULT;
            int16_t events = (int16_t)(e[1] & 0xFFFF), revents = 0;
            int32_t ofd = mint_fd_ofd(p, e[0]);
            if (e[0] < 0) revents = 0;
            else if (ofd < 0) revents = 0x20;                          /* POLLNVAL */
            else {
                uint32_t kind = mint_ofd_kind(ofd);
                int32_t pipe = mint_ofd_object(ofd);
                if (kind == MINT_OFD_PIPE_READ) {
                    if (mint_pipe_buffered(pipe)) revents |= events & 0x1;            /* POLLIN */
                    if (!mint_pipe_writers(pipe)) revents |= 0x10;                    /* POLLHUP */
                } else if (kind == MINT_OFD_PIPE_WRITE) {
                    if (!mint_pipe_readers(pipe)) revents |= 0x8;                      /* POLLERR */
                    else if (mint_pipe_buffered(pipe) < MINT_PIPE_BYTES) revents |= events & 0x4;
                } else revents |= events & 0x5;
            }
            e[1] = (e[1] & 0xFFFF) | ((int32_t)(uint16_t)revents << 16);
            if (!put_user(fds + i * 8u, e, sizeof(e))) return -MINT_EFAULT;
            if (revents) ++ready;
        }
        if (ready || mint_timer_ticks() >= deadline) return ready;
        if (mint_process_current_killed()) return -MINT_EINTR;
        mint_task_sleep_ticks(1);
    }
}

static uint32_t linux_prot(uint64_t prot) {
    return (uint32_t)(((prot & 1u) ? MINT_MEM_READ : 0u) | ((prot & 2u) ? MINT_MEM_WRITE : 0u) |
                      ((prot & 4u) ? MINT_MEM_EXEC : 0u));
}

static int64_t do_kill_self_or_child(MINT_PROCESS *p, int64_t pid, uint64_t sig) {
    if (pid <= 0) return -MINT_EPERM;          /* process groups are not supported */
    return mint_process_kill(p->pid, (uint64_t)pid, (uint32_t)sig);
}

static void log_unsupported(MINT_PROCESS *p, uint64_t nr) {
    ++unsupported;
    if (nr < 512u) {
        if (logged_unsupported[nr / 8u] & (1u << (nr % 8u))) return;
        logged_unsupported[nr / 8u] |= (uint8_t)(1u << (nr % 8u));
    }
    mint_serial_write("[linux] unsupported syscall "); mint_serial_dec64(nr);
    mint_serial_write(" from pid "); mint_serial_dec64(p ? p->pid : 0);
    mint_serial_write(" -> ENOSYS\n");
}

static int64_t exit_process(uint64_t code) {
    if (!mint_address_space_activate_kernel()) return -MINT_EFAULT;
    mint_task_user_exit_record(code & 0xFFu);
    return (int64_t)MINT_SYSCALL_EXIT_SENTINEL;
}

/* -------------------------------------------------------------- dispatcher */
static int64_t linux_dispatch(MINT_PROCESS *p, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    static char path[MINT_PROC_PATH_MAX], path2[MINT_PROC_PATH_MAX];
    MINT_LINUX_STAT st;
    int64_t rc;
    (void)a5;
    switch (nr) {
    case 0: return mint_fd_read_user(p, (int64_t)a0, a1, a2, 0);
    case 1: return mint_fd_write_user(p, (int64_t)a0, a1, a2, 0);
    case 2: return do_open(p, AT_FDCWD, a0, a1);
    case 3: return mint_fd_close(p, (int64_t)a0);
    case 4: case 6:
        if ((rc = resolve_at(p, AT_FDCWD, a0, path, sizeof(path))) < 0) return rc;
        if ((rc = stat_path(path, &st)) < 0) return rc;
        return put_user(a1, &st, sizeof(st)) ? 0 : -MINT_EFAULT;
    case 5:
        if ((rc = stat_fd(p, (int64_t)a0, &st)) < 0) return rc;
        return put_user(a1, &st, sizeof(st)) ? 0 : -MINT_EFAULT;
    case 7: return do_poll(p, a0, a1, (int64_t)(int32_t)a2);
    case 8: {
        int32_t ofd = mint_fd_ofd(p, (int64_t)a0);
        if (ofd < 0) return -MINT_EBADF;
        if (mint_ofd_kind(ofd) == MINT_OFD_DIR) {
            if (a1 == 0 && a2 == 0) { *mint_ofd_dir_pos(ofd) = 0; return 0; }
            return -MINT_EINVAL;
        }
        if (mint_ofd_kind(ofd) != MINT_OFD_FILE) return -MINT_ESPIPE;
        if (a2 > 2u) return -MINT_EINVAL;
        return mint_mfs_seek(mint_ofd_object(ofd), (int64_t)a1, (uint32_t)a2, p->pid);
    }
    case 9: {
        /* Anonymous private/shared mappings only; file mappings are refused. */
        const uint64_t MAP_SHARED = 1u, MAP_PRIVATE = 2u, MAP_FIXED = 0x10u, MAP_ANONYMOUS = 0x20u,
                       MAP_FIXED_NOREPLACE = 0x100000u;
        if (!(a3 & MAP_ANONYMOUS)) return -MINT_ENODEV;
        if (!(a3 & (MAP_SHARED | MAP_PRIVATE))) return -MINT_EINVAL;
        int fixed = (a3 & MAP_FIXED) != 0;
        if ((a3 & MAP_FIXED_NOREPLACE) && a0) {
            MINT_ADDRESS_REGION r;
            if (mint_address_space_region_query(&p->address_space, a0, &r)) return -MINT_EEXIST;
            fixed = 1;
        }
        (void)a4;
        return mint_proc_mmap(p, a0, a1, linux_prot(a2), fixed);
    }
    case 10: return mint_proc_mprotect(p, a0, a1, linux_prot(a2));
    case 11: return mint_proc_munmap(p, a0, a1);
    case 12: return (int64_t)mint_proc_brk(p, a0);
    case 13: {
        /* Handlers are recorded nowhere: MintOS does not deliver signals.
         * Ctrl+C / kill terminate the process. Report "default" as old action. */
        if (a2) { uint8_t zero[32] = {0}; if (!put_user(a2, zero, sizeof(zero))) return -MINT_EFAULT; }
        return a0 == 0 || a0 > 64u || a0 == 9u || a0 == 19u ? -MINT_EINVAL : 0;
    }
    case 14:
        if (a2) { uint64_t zero = 0; if (!put_user(a2, &zero, sizeof(zero))) return -MINT_EFAULT; }
        return 0;
    case 16: {
        const uint64_t TCGETS = 0x5401u, TIOCGWINSZ = 0x5413u, TIOCGPGRP = 0x540Fu;
        int32_t ofd = mint_fd_ofd(p, (int64_t)a0);
        if (ofd < 0) return -MINT_EBADF;
        if (!mint_fd_is_tty(p, (int64_t)a0)) return -MINT_ENOTTY;
        if (a1 == TIOCGWINSZ) {
            uint16_t ws[4] = {24u, 80u, 0, 0};
            return put_user(a2, ws, sizeof(ws)) ? 0 : -MINT_EFAULT;
        }
        if (a1 == TCGETS) {
            uint8_t termios[36] = {0};   /* kernel struct termios */
            /* c_lflag: ICANON|ECHO so readline-style programs treat input as cooked lines. */
            termios[12] = 0x0Au;
            return put_user(a2, termios, sizeof(termios)) ? 0 : -MINT_EFAULT;
        }
        if (a1 == TIOCGPGRP) { int32_t pg = (int32_t)p->pid; return put_user(a2, &pg, sizeof(pg)) ? 0 : -MINT_EFAULT; }
        return -MINT_ENOTTY;
    }
    case 17: return mint_fd_pread_user(p, (int64_t)a0, a1, a2, a3);
    case 19: return do_iov(p, (int64_t)a0, a1, a2, 0);
    case 20: return do_iov(p, (int64_t)a0, a1, a2, 1);
    case 21: case 269: case 439: {
        int64_t dirfd = nr == 21 ? AT_FDCWD : (int64_t)(int32_t)a0;
        uint64_t up = nr == 21 ? a0 : a1;
        if ((rc = resolve_at(p, dirfd, up, path, sizeof(path))) < 0) return rc;
        return stat_path(path, &st);
    }
    case 22: case 293: {
        int32_t pipe = mint_pipe_create();
        if (pipe < 0) return pipe;
        uint32_t nb = (nr == 293 && (a1 & MINT_LINUX_O_NONBLOCK)) ? MINT_OFD_NONBLOCK : 0u;
        uint32_t cloexec = (nr == 293 && (a1 & MINT_LINUX_O_CLOEXEC)) ? MINT_FD_FLAG_CLOEXEC : 0u;
        int32_t r = mint_ofd_create(MINT_OFD_PIPE_READ, MINT_OFD_READ | nb, pipe, p->pid);
        int32_t w = r >= 0 ? mint_ofd_create(MINT_OFD_PIPE_WRITE, MINT_OFD_WRITE | nb, pipe, p->pid) : -1;
        int64_t fr = r >= 0 ? mint_fd_install(p, r, 0, cloexec) : -MINT_EMFILE;
        int64_t fw = (w >= 0 && fr >= 0) ? mint_fd_install(p, w, 0, cloexec) : -MINT_EMFILE;
        if (fr < 0 || fw < 0) {
            if (fr >= 0) (void)mint_fd_close(p, fr); else if (r >= 0) mint_ofd_unref(r);
            if (w >= 0) mint_ofd_unref(w);
            return -MINT_EMFILE;
        }
        int32_t fds[2] = {(int32_t)fr, (int32_t)fw};
        return put_user(a0, fds, sizeof(fds)) ? 0 : -MINT_EFAULT;
    }
    case 24: mint_task_yield(); return 0;
    case 28: return 0;
    case 32: {
        int32_t ofd = mint_fd_ofd(p, (int64_t)a0);
        if (ofd < 0) return -MINT_EBADF;
        mint_ofd_ref(ofd);
        rc = mint_fd_install(p, ofd, 0, 0);
        if (rc < 0) mint_ofd_unref(ofd);
        return rc;
    }
    case 33: case 292: {
        int32_t ofd = mint_fd_ofd(p, (int64_t)a0);
        if (ofd < 0 || a1 >= MINT_PROC_FD_MAX) return -MINT_EBADF;
        if (a0 == a1) return nr == 292 ? -MINT_EINVAL : (int64_t)a1;
        mint_ofd_ref(ofd);
        return mint_fd_install_at(p, ofd, (int64_t)a1,
                                  (nr == 292 && (a2 & MINT_LINUX_O_CLOEXEC)) ? MINT_FD_FLAG_CLOEXEC : 0u);
    }
    case 35: return do_nanosleep(a0);
    case 39: case 186: case 218: return (int64_t)p->pid;
    case 60: case 231: return exit_process(a0);
    case 61: {
        MINT_PROC_STATUS status = {0, 0, 0, 0};
        int64_t pid = (int64_t)(int32_t)a0;
        if (pid < -1 || pid == 0) return -MINT_ECHILD;
        rc = mint_process_wait(p->pid, pid == -1 ? 0 : pid, (a2 & 1u) ? MINT_WAIT_NOHANG : 0u, &status);
        if (rc > 0 && a1) {
            int32_t ws = mint_linux_wait_status(status.reason, status.code);
            if (!put_user(a1, &ws, sizeof(ws))) return -MINT_EFAULT;
        }
        return rc;
    }
    case 62: return do_kill_self_or_child(p, (int64_t)(int32_t)a0, a1);
    case 200: return do_kill_self_or_child(p, (int64_t)(int32_t)a0, a1);
    case 234: return do_kill_self_or_child(p, (int64_t)(int32_t)a1, a2);
    case 63: {
        static char uts[6 * 65];
        const char *fields[6] = {"MintOS", "mintos", "26.5", "MintOS26 Linux-compat subset", "x86_64", "(none)"};
        for (int f = 0; f < 6; ++f) {
            size_t i = 0;
            for (; fields[f][i] && i < 64u; ++i) uts[f * 65 + (int)i] = fields[f][i];
            for (; i < 65u; ++i) uts[f * 65 + (int)i] = 0;
        }
        return put_user(a0, uts, sizeof(uts)) ? 0 : -MINT_EFAULT;
    }
    case 72: {
        const uint64_t F_DUPFD = 0, F_GETFD = 1, F_SETFD = 2, F_GETFL = 3, F_SETFL = 4, F_DUPFD_CLOEXEC = 1030;
        int32_t ofd = mint_fd_ofd(p, (int64_t)a0);
        if (ofd < 0) return -MINT_EBADF;
        if (a1 == F_DUPFD || a1 == F_DUPFD_CLOEXEC) {
            mint_ofd_ref(ofd);
            rc = mint_fd_install(p, ofd, (int64_t)a2, a1 == F_DUPFD_CLOEXEC ? MINT_FD_FLAG_CLOEXEC : 0u);
            if (rc < 0) mint_ofd_unref(ofd);
            return rc;
        }
        if (a1 == F_GETFD) return (p->fds[a0].flags & MINT_FD_FLAG_CLOEXEC) ? 1 : 0;
        if (a1 == F_SETFD) { p->fds[a0].flags = (a2 & 1u) ? MINT_FD_FLAG_CLOEXEC : 0u; return 0; }
        uint32_t of = mint_ofd_flags(ofd);
        if (a1 == F_GETFL) {
            int64_t fl = (of & MINT_OFD_READ) && (of & MINT_OFD_WRITE) ? 2 : (of & MINT_OFD_WRITE) ? 1 : 0;
            if (of & MINT_OFD_NONBLOCK) fl |= MINT_LINUX_O_NONBLOCK;
            if (of & MINT_OFD_APPEND) fl |= MINT_LINUX_O_APPEND;
            return fl;
        }
        if (a1 == F_SETFL) {
            of &= ~MINT_OFD_NONBLOCK;
            if (a2 & MINT_LINUX_O_NONBLOCK) of |= MINT_OFD_NONBLOCK;
            mint_ofd_set_flags(ofd, of);
            return 0;
        }
        return -MINT_EINVAL;
    }
    case 74: case 75:
        if (mint_fd_ofd(p, (int64_t)a0) < 0) return -MINT_EBADF;
        (void)mint_mfs_sync();
        return 0;
    case 79: {
        size_t len = 0;
        while (p->cwd[len]) ++len;
        if (a1 < len + 1u) return -MINT_ERANGE;
        return put_user(a0, p->cwd, len + 1u) ? (int64_t)(len + 1u) : -MINT_EFAULT;
    }
    case 80: {
        if ((rc = resolve_at(p, AT_FDCWD, a0, path, sizeof(path))) < 0) return rc;
        MINT_MFS_STAT m;
        if ((rc = mint_mfs_stat(path, &m)) < 0) return rc;
        if (m.type != MINT_MFS_DIR) return -MINT_ENOTDIR;
        for (size_t i = 0; i < sizeof(p->cwd); ++i) { p->cwd[i] = path[i]; if (!path[i]) break; }
        return 0;
    }
    case 82:
        if ((rc = resolve_at(p, AT_FDCWD, a0, path, sizeof(path))) < 0) return rc;
        if ((rc = resolve_at(p, AT_FDCWD, a1, path2, sizeof(path2))) < 0) return rc;
        return mint_mfs_rename(path, path2);
    case 83:
        if ((rc = resolve_at(p, AT_FDCWD, a0, path, sizeof(path))) < 0) return rc;
        return mint_mfs_mkdir(path);
    case 84: case 87: {
        if ((rc = resolve_at(p, AT_FDCWD, a0, path, sizeof(path))) < 0) return rc;
        MINT_MFS_STAT m;
        if ((rc = mint_mfs_stat(path, &m)) < 0) return rc;
        if (nr == 84 && m.type != MINT_MFS_DIR) return -MINT_ENOTDIR;
        if (nr == 87 && m.type == MINT_MFS_DIR) return -MINT_EISDIR;
        return mint_mfs_remove(path);
    }
    case 89:
        if ((rc = resolve_at(p, AT_FDCWD, a0, path, sizeof(path))) < 0) return rc;
        return stat_path(path, &st) < 0 ? -MINT_ENOENT : -MINT_EINVAL;   /* no symbolic links */
    case 95: return 022;
    case 96: {
        if (a0) {
            int64_t sec = 0, nsec = 0;
            (void)clock_now(0, &sec, &nsec);
            int64_t tv[2] = {sec, nsec / 1000};
            if (!put_user(a0, tv, sizeof(tv))) return -MINT_EFAULT;
        }
        if (a1) { int32_t tz[2] = {0, 0}; if (!put_user(a1, tz, sizeof(tz))) return -MINT_EFAULT; }
        return 0;
    }
    case 97: case 302: {
        uint64_t resource = nr == 97 ? a0 : a1, out = nr == 97 ? a1 : a3;
        if (nr == 302 && a0 && a0 != p->pid) return -MINT_ESRCH;
        uint64_t lim[2] = {~0ULL, ~0ULL};
        if (resource == 3u) lim[0] = lim[1] = p->stack_bytes ? p->stack_bytes : MINT_USER_PROGRAM_STACK_BYTES;
        else if (resource == 7u) lim[0] = lim[1] = MINT_PROC_FD_MAX;
        else if (resource == 9u || resource == 2u) {
            MINT_ASPACE_USAGE u; mint_address_space_get_usage(&p->address_space, &u);
            lim[0] = lim[1] = u.frame_limit * 4096u;
        } else if (resource >= 16u) return -MINT_EINVAL;
        if (out && !put_user(out, lim, sizeof(lim))) return -MINT_EFAULT;
        return 0;
    }
    case 98: {
        int64_t ru[18] = {0};
        uint64_t t = mint_task_user_ticks(p->pid);
        ru[0] = (int64_t)(t / 100u); ru[1] = (int64_t)(t % 100u) * 10000;
        MINT_ASPACE_USAGE u; mint_address_space_get_usage(&p->address_space, &u);
        ru[4] = (int64_t)(u.peak_owned_pages * 4u);   /* ru_maxrss in KiB */
        return put_user(a1, ru, sizeof(ru)) ? 0 : -MINT_EFAULT;
    }
    case 99: {
        uint64_t info[14] = {0};
        info[0] = mint_timer_ticks() / 100u;
        info[4] = mint_physmem_total_pages() * 4096u;
        info[5] = mint_physmem_free_pages() * 4096u;
        info[10] = mint_process_live_count();
        info[13] = 1u;    /* mem_unit (in the last word with padding) */
        return put_user(a0, info, sizeof(info)) ? 0 : -MINT_EFAULT;
    }
    case 100: {
        int64_t t[4] = {(int64_t)mint_task_user_ticks(p->pid), 0, 0, 0};
        if (a0 && !put_user(a0, t, sizeof(t))) return -MINT_EFAULT;
        return (int64_t)mint_timer_ticks();
    }
    case 102: case 104: case 107: case 108: return 0;     /* the only user is root (uid 0) */
    case 110: return (int64_t)p->parent_pid;
    case 131:
        if (a1) { uint8_t zero[24] = {0}; zero[8] = 2; /* SS_DISABLE */ if (!put_user(a1, zero, sizeof(zero))) return -MINT_EFAULT; }
        return 0;
    case 158: {
        const uint64_t ARCH_SET_FS = 0x1002u, ARCH_GET_FS = 0x1003u;
        if (a0 == ARCH_SET_FS) {
            if (a1 && !mint_address_space_is_user_address(a1)) return -MINT_EPERM;
            return mint_task_set_fs_base(a1) ? 0 : -MINT_EINVAL;
        }
        if (a0 == ARCH_GET_FS) {
            uint64_t fs = rdmsr(0xC0000100u);
            return put_user(a1, &fs, sizeof(fs)) ? 0 : -MINT_EFAULT;
        }
        return -MINT_EINVAL;
    }
    case 201: {
        uint64_t t = realtime_seconds();
        if (a0 && !put_user(a0, &t, sizeof(t))) return -MINT_EFAULT;
        return (int64_t)t;
    }
    case 217: return do_getdents(p, (int64_t)a0, a1, a2);
    case 228: {
        int64_t ts[2];
        if ((rc = clock_now((int64_t)a0, &ts[0], &ts[1])) < 0) return rc;
        return put_user(a1, ts, sizeof(ts)) ? 0 : -MINT_EFAULT;
    }
    case 229: {
        if ((int64_t)a0 < 0 || a0 > 7u) return -MINT_EINVAL;
        int64_t ts[2] = {0, 10000000};
        if (a1 && !put_user(a1, ts, sizeof(ts))) return -MINT_EFAULT;
        return 0;
    }
    case 230: {
        if (a1 & 1u) {   /* TIMER_ABSTIME */
            int64_t ts[2], now_s = 0, now_ns = 0;
            if (!get_user(ts, a2, sizeof(ts))) return -MINT_EFAULT;
            if ((rc = clock_now((int64_t)a0, &now_s, &now_ns)) < 0) return rc;
            int64_t delta_ns = (ts[0] - now_s) * 1000000000 + (ts[1] - now_ns);
            if (delta_ns <= 0) return 0;
            return sleep_ticks_interruptible(((uint64_t)delta_ns + 9999999u) / 10000000u);
        }
        return do_nanosleep(a2);
    }
    case 257: return do_open(p, (int64_t)(int32_t)a0, a1, a2);
    case 262: {
        if ((a3 & AT_EMPTY_PATH)) {
            char first = 0;
            if (!get_user(&first, a1, 1u)) return -MINT_EFAULT;
            if (!first) {
                if ((rc = stat_fd(p, (int64_t)(int32_t)a0, &st)) < 0) return rc;
                return put_user(a2, &st, sizeof(st)) ? 0 : -MINT_EFAULT;
            }
        }
        if ((rc = resolve_at(p, (int64_t)(int32_t)a0, a1, path, sizeof(path))) < 0) return rc;
        if ((rc = stat_path(path, &st)) < 0) return rc;
        return put_user(a2, &st, sizeof(st)) ? 0 : -MINT_EFAULT;
    }
    case 273: return 0;
    case 318: {
        static uint8_t buf[256];
        uint64_t len = a1 > sizeof(buf) ? sizeof(buf) : a1;
        for (uint64_t i = 0; i < len; i += 8u) {
            uint64_t r = rng_next();
            for (uint64_t k = 0; k < 8u && i + k < len; ++k) buf[i + k] = (uint8_t)(r >> (8u * k));
        }
        return put_user(a0, buf, (size_t)len) ? (int64_t)len : -MINT_EFAULT;
    }
    default:
        return -MINT_ENOSYS;
    }
}

int64_t mint_linux_syscall_dispatch(struct mint_syscall_frame *frame) {
    MINT_SYSCALL_FRAME *f = (MINT_SYSCALL_FRAME *)frame;
    MINT_PROCESS *p = mint_process_current();
    if (!f || (f->cs & 3u) != 3u || !p || !mint_address_space_current()) return -MINT_EPERM;
    ++calls;
    uint64_t nr = f->rax;
    if (!mint_linux_syscall_info(nr)) {
        log_unsupported(p, nr);
        return -MINT_ENOSYS;
    }
    int64_t rc = linux_dispatch(p, nr, f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9);
    if (rc != (int64_t)MINT_SYSCALL_EXIT_SENTINEL && mint_process_current_killed()) {
        if (mint_address_space_activate_kernel()) {
            mint_task_user_exit_record(MINT_EXIT_KILLED_BIT | p->kill_signal);
            return (int64_t)MINT_SYSCALL_EXIT_SENTINEL;
        }
    }
    return rc;
}
