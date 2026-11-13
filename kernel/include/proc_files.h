#ifndef MINTOS26_PROC_FILES_H
#define MINTOS26_PROC_FILES_H
/* 26.5.014.0: per-process descriptors, shared open file descriptions (OFDs),
 * pipes and directory handles. Single core; blocking calls sleep one tick at
 * a time and give up with -MINT_EINTR when the caller is being killed. */
#include <stdint.h>
#include "process.h"

#define MINT_OFD_MAX 128u
#define MINT_PIPE_MAX 48u
#define MINT_PIPE_BYTES 8192u
#define MINT_DIR_HANDLE_MAX 16u

enum {
    MINT_OFD_NONE = 0,
    MINT_OFD_CONSOLE,     /* COM1: writes are logged, reads see end-of-file */
    MINT_OFD_PIPE_READ,
    MINT_OFD_PIPE_WRITE,
    MINT_OFD_FILE,        /* MintOS HD file: object = MintFS handle */
    MINT_OFD_DIR,         /* MintOS HD directory: object = directory handle */
    MINT_OFD_NULL         /* /dev/null */
};
#define MINT_OFD_NONBLOCK 0x1u
#define MINT_OFD_READ     0x2u
#define MINT_OFD_WRITE    0x4u
#define MINT_OFD_APPEND   0x8u
#define MINT_OFD_TTY      0x10u   /* behaves as a terminal (isatty, TIOCGWINSZ) */

void mint_proc_files_init(void);
/* New OFD with one reference; -MINT_ENFILE-style negative value on failure.
 * For pipe ends `object` is the pipe id and the pipe end gains a reference. */
int32_t mint_ofd_create(uint32_t kind, uint32_t flags, int32_t object, uint64_t owner_pid);
void mint_ofd_ref(int32_t ofd);
void mint_ofd_unref(int32_t ofd);
uint32_t mint_ofd_kind(int32_t ofd);
uint32_t mint_ofd_flags(int32_t ofd);
void mint_ofd_set_flags(int32_t ofd, uint32_t flags);
int32_t mint_ofd_object(int32_t ofd);
/* Directory read position (getdents). */
uint32_t *mint_ofd_dir_pos(int32_t ofd);

/* A pipe has no references until OFDs are created for its ends. */
int32_t mint_pipe_create(void);
uint32_t mint_pipe_readers(int32_t pipe);
uint32_t mint_pipe_writers(int32_t pipe);
uint32_t mint_pipe_buffered(int32_t pipe);
/* Host-testable ring buffer primitives (no blocking). */
uint32_t mint_pipe_put(int32_t pipe, const void *data, uint32_t length);
uint32_t mint_pipe_get(int32_t pipe, void *out, uint32_t length);

int32_t mint_dir_handle_open(const char *path);
const char *mint_dir_handle_path(int32_t handle);
void mint_dir_handle_close(int32_t handle);

/* Descriptor table of one process. */
void mint_fd_table_init(MINT_PROCESS *p);
int64_t mint_fd_install(MINT_PROCESS *p, int32_t ofd, int64_t min_fd, uint32_t fd_flags);
int64_t mint_fd_install_at(MINT_PROCESS *p, int32_t ofd, int64_t fd, uint32_t fd_flags);
int32_t mint_fd_ofd(const MINT_PROCESS *p, int64_t fd);
int64_t mint_fd_close(MINT_PROCESS *p, int64_t fd);
void mint_fd_close_all(MINT_PROCESS *p);
/* I/O on user buffers of the CURRENT (active) process. */
int64_t mint_fd_read_user(MINT_PROCESS *p, int64_t fd, uint64_t buffer, uint64_t length, int nonblock);
int64_t mint_fd_write_user(MINT_PROCESS *p, int64_t fd, uint64_t buffer, uint64_t length, int nonblock);
int64_t mint_fd_pread_user(MINT_PROCESS *p, int64_t fd, uint64_t buffer, uint64_t length, uint64_t offset);
int mint_fd_is_tty(const MINT_PROCESS *p, int64_t fd);
/* MintFS handle owner for files: the process pid. */
#endif
