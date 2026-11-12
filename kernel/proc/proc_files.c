/* 26.5.014.0: descriptors, open file descriptions, pipes and directory
 * handles for user processes (see proc_files.h). */
#include <stdint.h>
#include <stddef.h>
#include "../include/proc_files.h"
#include "../include/uapi/mintos/syscall.h"
#ifndef MINT_PROC_FILES_HOST_TEST
#include "../include/task.h"
#include "../include/serial.h"
#include "../include/mintos_fs.h"
#include "../include/address_space.h"
#endif

typedef struct mint_ofd {
    uint16_t kind;
    uint16_t flags;
    int32_t object;
    uint32_t refs;
    uint32_t dir_pos;
    uint64_t owner_pid;
} MINT_OFD;

typedef struct mint_pipe {
    uint32_t used;
    uint32_t readers, writers;
    uint32_t head, count;
    uint8_t buffer[MINT_PIPE_BYTES];
} MINT_PIPE;

static MINT_OFD ofds[MINT_OFD_MAX];
static MINT_PIPE pipes[MINT_PIPE_MAX];
static struct { uint32_t used; char path[MINT_PROC_PATH_MAX]; } dir_handles[MINT_DIR_HANDLE_MAX];

void mint_proc_files_init(void) {
    for (uint32_t i = 0; i < MINT_OFD_MAX; ++i) { ofds[i].kind = MINT_OFD_NONE; ofds[i].refs = 0; }
    for (uint32_t i = 0; i < MINT_PIPE_MAX; ++i) pipes[i].used = 0;
    for (uint32_t i = 0; i < MINT_DIR_HANDLE_MAX; ++i) dir_handles[i].used = 0;
}

static int valid_ofd(int32_t ofd) { return ofd >= 0 && (uint32_t)ofd < MINT_OFD_MAX && ofds[ofd].refs; }
static int valid_pipe(int32_t p) { return p >= 0 && (uint32_t)p < MINT_PIPE_MAX && pipes[p].used; }

/* ------------------------------------------------------------------ pipes */
int32_t mint_pipe_create(void) {
    for (uint32_t i = 0; i < MINT_PIPE_MAX; ++i) {
        if (pipes[i].used) continue;
        pipes[i].used = 1; pipes[i].readers = 0; pipes[i].writers = 0;
        pipes[i].head = 0; pipes[i].count = 0;
        return (int32_t)i;
    }
    return -MINT_EMFILE;
}
uint32_t mint_pipe_readers(int32_t p) { return valid_pipe(p) ? pipes[p].readers : 0; }
uint32_t mint_pipe_writers(int32_t p) { return valid_pipe(p) ? pipes[p].writers : 0; }
uint32_t mint_pipe_buffered(int32_t p) { return valid_pipe(p) ? pipes[p].count : 0; }

uint32_t mint_pipe_put(int32_t p, const void *data, uint32_t length) {
    if (!valid_pipe(p) || !data) return 0;
    MINT_PIPE *pp = &pipes[p];
    const uint8_t *in = (const uint8_t *)data;
    uint32_t n = 0;
    while (n < length && pp->count < MINT_PIPE_BYTES) {
        pp->buffer[(pp->head + pp->count) % MINT_PIPE_BYTES] = in[n++];
        ++pp->count;
    }
    return n;
}

uint32_t mint_pipe_get(int32_t p, void *out, uint32_t length) {
    if (!valid_pipe(p) || !out) return 0;
    MINT_PIPE *pp = &pipes[p];
    uint8_t *o = (uint8_t *)out;
    uint32_t n = 0;
    while (n < length && pp->count) {
        o[n++] = pp->buffer[pp->head];
        pp->head = (pp->head + 1u) % MINT_PIPE_BYTES;
        --pp->count;
    }
    if (!pp->count) pp->head = 0;
    return n;
}

static void pipe_release_if_unused(int32_t p) {
    if (valid_pipe(p) && !pipes[p].readers && !pipes[p].writers) pipes[p].used = 0;
}

/* ------------------------------------------------------- directory handles */
int32_t mint_dir_handle_open(const char *path) {
    if (!path) return -MINT_EINVAL;
    for (uint32_t i = 0; i < MINT_DIR_HANDLE_MAX; ++i) {
        if (dir_handles[i].used) continue;
        uint32_t n = 0;
        while (path[n] && n + 1u < MINT_PROC_PATH_MAX) { dir_handles[i].path[n] = path[n]; ++n; }
        if (path[n]) return -MINT_ENAMETOOLONG;
        dir_handles[i].path[n] = '\0';
        dir_handles[i].used = 1;
        return (int32_t)i;
    }
    return -MINT_EMFILE;
}
const char *mint_dir_handle_path(int32_t h) {
    return h >= 0 && (uint32_t)h < MINT_DIR_HANDLE_MAX && dir_handles[h].used ? dir_handles[h].path : 0;
}
void mint_dir_handle_close(int32_t h) {
    if (h >= 0 && (uint32_t)h < MINT_DIR_HANDLE_MAX) dir_handles[h].used = 0;
}

/* ------------------------------------------------------------------- OFDs */
int32_t mint_ofd_create(uint32_t kind, uint32_t flags, int32_t object, uint64_t owner_pid) {
    if (kind == MINT_OFD_NONE || kind > MINT_OFD_NULL) return -MINT_EINVAL;
    if ((kind == MINT_OFD_PIPE_READ || kind == MINT_OFD_PIPE_WRITE) && !valid_pipe(object)) return -MINT_EINVAL;
    for (uint32_t i = 0; i < MINT_OFD_MAX; ++i) {
        if (ofds[i].refs) continue;
        ofds[i].kind = (uint16_t)kind; ofds[i].flags = (uint16_t)flags; ofds[i].object = object;
        ofds[i].refs = 1; ofds[i].dir_pos = 0; ofds[i].owner_pid = owner_pid;
        if (kind == MINT_OFD_PIPE_READ) ++pipes[object].readers;
        if (kind == MINT_OFD_PIPE_WRITE) ++pipes[object].writers;
        return (int32_t)i;
    }
    return -MINT_EMFILE;
}

void mint_ofd_ref(int32_t ofd) { if (valid_ofd(ofd)) ++ofds[ofd].refs; }

void mint_ofd_unref(int32_t ofd) {
    if (!valid_ofd(ofd)) return;
    MINT_OFD *o = &ofds[ofd];
    if (--o->refs) return;
    switch (o->kind) {
    case MINT_OFD_PIPE_READ:
        if (valid_pipe(o->object) && pipes[o->object].readers) --pipes[o->object].readers;
        pipe_release_if_unused(o->object);
        break;
    case MINT_OFD_PIPE_WRITE:
        if (valid_pipe(o->object) && pipes[o->object].writers) --pipes[o->object].writers;
        pipe_release_if_unused(o->object);
        break;
#ifndef MINT_PROC_FILES_HOST_TEST
    case MINT_OFD_FILE:
        (void)mint_mfs_close(o->object, o->owner_pid);
        break;
#endif
    case MINT_OFD_DIR:
        mint_dir_handle_close(o->object);
        break;
    default:
        break;
    }
    o->kind = MINT_OFD_NONE;
}

uint32_t mint_ofd_kind(int32_t ofd) { return valid_ofd(ofd) ? ofds[ofd].kind : MINT_OFD_NONE; }
uint32_t mint_ofd_flags(int32_t ofd) { return valid_ofd(ofd) ? ofds[ofd].flags : 0; }
void mint_ofd_set_flags(int32_t ofd, uint32_t flags) { if (valid_ofd(ofd)) ofds[ofd].flags = (uint16_t)flags; }
int32_t mint_ofd_object(int32_t ofd) { return valid_ofd(ofd) ? ofds[ofd].object : -1; }
uint32_t *mint_ofd_dir_pos(int32_t ofd) { return valid_ofd(ofd) ? &ofds[ofd].dir_pos : 0; }

/* ------------------------------------------------------- descriptor tables */
void mint_fd_table_init(MINT_PROCESS *p) {
    if (!p) return;
    for (uint32_t i = 0; i < MINT_PROC_FD_MAX; ++i) { p->fds[i].ofd = -1; p->fds[i].flags = 0; }
}

int64_t mint_fd_install(MINT_PROCESS *p, int32_t ofd, int64_t min_fd, uint32_t fd_flags) {
    if (!p || !valid_ofd(ofd) || min_fd < 0) return -MINT_EINVAL;
    for (int64_t fd = min_fd; fd < (int64_t)MINT_PROC_FD_MAX; ++fd) {
        if (p->fds[fd].ofd >= 0) continue;
        p->fds[fd].ofd = ofd; p->fds[fd].flags = fd_flags;
        return fd;
    }
    return -MINT_EMFILE;
}

int64_t mint_fd_install_at(MINT_PROCESS *p, int32_t ofd, int64_t fd, uint32_t fd_flags) {
    if (!p || !valid_ofd(ofd) || fd < 0 || fd >= (int64_t)MINT_PROC_FD_MAX) return -MINT_EBADF;
    if (p->fds[fd].ofd >= 0) mint_ofd_unref(p->fds[fd].ofd);
    p->fds[fd].ofd = ofd; p->fds[fd].flags = fd_flags;
    return fd;
}

int32_t mint_fd_ofd(const MINT_PROCESS *p, int64_t fd) {
    if (!p || fd < 0 || fd >= (int64_t)MINT_PROC_FD_MAX) return -1;
    return valid_ofd(p->fds[fd].ofd) ? p->fds[fd].ofd : -1;
}

int64_t mint_fd_close(MINT_PROCESS *p, int64_t fd) {
    int32_t ofd = mint_fd_ofd(p, fd);
    if (ofd < 0) return -MINT_EBADF;
    p->fds[fd].ofd = -1; p->fds[fd].flags = 0;
    mint_ofd_unref(ofd);
    return 0;
}

void mint_fd_close_all(MINT_PROCESS *p) {
    if (!p) return;
    for (int64_t fd = 0; fd < (int64_t)MINT_PROC_FD_MAX; ++fd)
        if (p->fds[fd].ofd >= 0) (void)mint_fd_close(p, fd);
}

int mint_fd_is_tty(const MINT_PROCESS *p, int64_t fd) {
    int32_t ofd = mint_fd_ofd(p, fd);
    if (ofd < 0) return 0;
    return ofds[ofd].kind == MINT_OFD_CONSOLE || (ofds[ofd].flags & MINT_OFD_TTY) != 0;
}

#ifndef MINT_PROC_FILES_HOST_TEST
/* Bounce buffer: syscalls never interleave except where they sleep, and no
 * path below sleeps while holding data in it. */
static uint8_t io_bounce[4096];

static int user_killed(void) { return mint_process_current_killed(); }

int64_t mint_fd_read_user(MINT_PROCESS *p, int64_t fd, uint64_t buffer, uint64_t length, int nonblock) {
    int32_t ofd = mint_fd_ofd(p, fd);
    if (ofd < 0) return -MINT_EBADF;
    const MINT_ADDRESS_SPACE *space = mint_address_space_current();
    MINT_OFD *o = &ofds[ofd];
    if (!length) return 0;
    if (length > 0x7FFFF000ULL) length = 0x7FFFF000ULL;
    if (!mint_address_space_validate_user_range(space, buffer, (size_t)(length < 4096u ? length : 4096u), 1))
        return -MINT_EFAULT;
    nonblock = nonblock || (o->flags & MINT_OFD_NONBLOCK);
    switch (o->kind) {
    case MINT_OFD_CONSOLE:
    case MINT_OFD_NULL:
        return 0;
    case MINT_OFD_PIPE_WRITE:
    case MINT_OFD_DIR:
        return o->kind == MINT_OFD_DIR ? -MINT_EISDIR : -MINT_EBADF;
    case MINT_OFD_PIPE_READ: {
        int32_t pipe = o->object;
        while (!mint_pipe_buffered(pipe)) {
            if (!mint_pipe_writers(pipe)) return 0;               /* end of file */
            if (nonblock) return -MINT_EAGAIN;
            if (user_killed()) return -MINT_EINTR;
            mint_task_sleep_ticks(1);
            if (!valid_ofd(ofd)) return -MINT_EBADF;
        }
        uint64_t total = 0;
        while (total < length && mint_pipe_buffered(pipe)) {
            uint32_t chunk = (uint32_t)(length - total < sizeof(io_bounce) ? length - total : sizeof(io_bounce));
            uint32_t got = mint_pipe_get(pipe, io_bounce, chunk);
            if (!got) break;
            if (!mint_address_space_copy_to_user(space, buffer + total, io_bounce, got))
                return total ? (int64_t)total : -MINT_EFAULT;
            total += got;
        }
        return (int64_t)total;
    }
    case MINT_OFD_FILE: {
        uint64_t total = 0;
        while (total < length) {
            uint32_t chunk = (uint32_t)(length - total < sizeof(io_bounce) ? length - total : sizeof(io_bounce));
            int64_t got = mint_mfs_read(o->object, io_bounce, chunk, o->owner_pid);
            if (got < 0) return total ? (int64_t)total : got;
            if (got == 0) break;
            if (!mint_address_space_copy_to_user(space, buffer + total, io_bounce, (size_t)got))
                return total ? (int64_t)total : -MINT_EFAULT;
            total += (uint64_t)got;
            if ((uint64_t)got < chunk) break;
        }
        return (int64_t)total;
    }
    default:
        return -MINT_EBADF;
    }
}

int64_t mint_fd_pread_user(MINT_PROCESS *p, int64_t fd, uint64_t buffer, uint64_t length, uint64_t offset) {
    int32_t ofd = mint_fd_ofd(p, fd);
    if (ofd < 0) return -MINT_EBADF;
    if (ofds[ofd].kind != MINT_OFD_FILE) return -MINT_ESPIPE;
    int64_t saved = mint_mfs_seek(ofds[ofd].object, 0, MINT_SEEK_CUR, ofds[ofd].owner_pid);
    if (saved < 0) return saved;
    int64_t rc = mint_mfs_seek(ofds[ofd].object, (int64_t)offset, MINT_SEEK_SET, ofds[ofd].owner_pid);
    if (rc >= 0) rc = mint_fd_read_user(p, fd, buffer, length, 0);
    (void)mint_mfs_seek(ofds[ofd].object, saved, MINT_SEEK_SET, ofds[ofd].owner_pid);
    return rc;
}

static void console_write(const uint8_t *data, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) mint_serial_putc((char)data[i]);
}

int64_t mint_fd_write_user(MINT_PROCESS *p, int64_t fd, uint64_t buffer, uint64_t length, int nonblock) {
    int32_t ofd = mint_fd_ofd(p, fd);
    if (ofd < 0) return -MINT_EBADF;
    const MINT_ADDRESS_SPACE *space = mint_address_space_current();
    MINT_OFD *o = &ofds[ofd];
    if (!length) return 0;
    if (length > 0x7FFFF000ULL) length = 0x7FFFF000ULL;
    nonblock = nonblock || (o->flags & MINT_OFD_NONBLOCK);
    uint64_t total = 0;
    switch (o->kind) {
    case MINT_OFD_NULL:
        return mint_address_space_validate_user_range(space, buffer, (size_t)(length < 4096u ? length : 4096u), 0)
               ? (int64_t)length : -MINT_EFAULT;
    case MINT_OFD_CONSOLE:
        while (total < length) {
            uint32_t chunk = (uint32_t)(length - total < sizeof(io_bounce) ? length - total : sizeof(io_bounce));
            if (!mint_address_space_copy_from_user(space, io_bounce, buffer + total, chunk))
                return total ? (int64_t)total : -MINT_EFAULT;
            console_write(io_bounce, chunk);
            total += chunk;
        }
        return (int64_t)total;
    case MINT_OFD_PIPE_WRITE: {
        int32_t pipe = o->object;
        while (total < length) {
            if (!mint_pipe_readers(pipe)) return total ? (int64_t)total : -MINT_EPIPE;
            uint32_t space_left = MINT_PIPE_BYTES - mint_pipe_buffered(pipe);
            if (!space_left) {
                if (nonblock) return total ? (int64_t)total : -MINT_EAGAIN;
                if (user_killed()) return total ? (int64_t)total : -MINT_EINTR;
                mint_task_sleep_ticks(1);
                if (!valid_ofd(ofd)) return total ? (int64_t)total : -MINT_EBADF;
                continue;
            }
            uint32_t chunk = (uint32_t)(length - total);
            if (chunk > space_left) chunk = space_left;
            if (chunk > sizeof(io_bounce)) chunk = sizeof(io_bounce);
            if (!mint_address_space_copy_from_user(space, io_bounce, buffer + total, chunk))
                return total ? (int64_t)total : -MINT_EFAULT;
            total += mint_pipe_put(pipe, io_bounce, chunk);
        }
        return (int64_t)total;
    }
    case MINT_OFD_FILE:
        while (total < length) {
            uint32_t chunk = (uint32_t)(length - total < sizeof(io_bounce) ? length - total : sizeof(io_bounce));
            if (!mint_address_space_copy_from_user(space, io_bounce, buffer + total, chunk))
                return total ? (int64_t)total : -MINT_EFAULT;
            int64_t w = mint_mfs_write(o->object, io_bounce, chunk, o->owner_pid);
            if (w < 0) return total ? (int64_t)total : w;
            total += (uint64_t)w;
            if ((uint64_t)w < chunk) break;
        }
        return (int64_t)total;
    case MINT_OFD_DIR:
        return -MINT_EISDIR;
    default:
        return -MINT_EBADF;
    }
}
#endif
