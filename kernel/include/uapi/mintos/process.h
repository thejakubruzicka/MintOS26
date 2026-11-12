#ifndef MINTOS26_UAPI_PROCESS_H
#define MINTOS26_UAPI_PROCESS_H
/* 26.5.014.0: MintOS process, pipe and memory services (native int 0x80 ABI,
 * reserved range 0x4D60-0x4D7F). These are MintOS calls, not Linux ones; the
 * separate Linux compatibility subset lives behind the `syscall` instruction
 * (see docs/SDK.md). All calls return a negative MINT_E* value on error. */
#include <stdint.h>

#define MINT_SYS_PROC_SPAWN        0x4D60u  /* (MINT_SPAWN_REQUEST *) -> pid */
#define MINT_SYS_PROC_WAIT         0x4D61u  /* (pid or 0, MINT_PROC_STATUS *, options) -> pid / 0 */
#define MINT_SYS_PROC_KILL         0x4D62u  /* (pid, signal) */
#define MINT_SYS_PROC_LIST         0x4D63u  /* (MINT_PROC_INFO *, capacity) -> count */
#define MINT_SYS_FD_READ           0x4D64u  /* (fd, buf, len, MINT_IO_NONBLOCK?) -> bytes */
#define MINT_SYS_FD_WRITE          0x4D65u  /* (fd, buf, len, MINT_IO_NONBLOCK?) -> bytes */
#define MINT_SYS_FD_CLOSE          0x4D66u  /* (fd) */
#define MINT_SYS_PIPE              0x4D67u  /* (int32_t fds[2]) */
#define MINT_SYS_MEM_MAP           0x4D68u  /* (hint, length, MINT_MEM_* prot) -> address */
#define MINT_SYS_MEM_UNMAP         0x4D69u  /* (address, length) */
#define MINT_SYS_MEM_PROTECT       0x4D6Au  /* (address, length, prot) */
#define MINT_SYS_MEM_BRK           0x4D6Bu  /* (new end or 0) -> current end */
#define MINT_SYS_PLATFORM_INFO     0x4D6Cu  /* (MINT_PLATFORM_INFO *) */
#define MINT_SYS_PROGRAMS_INSTALL  0x4D6Du  /* () -> files written; refresh bundled programs on MintOS HD */

/* errno values used by these services that syscall.h does not list. */
#ifndef MINT_ESRCH
#define MINT_ESRCH 3
#define MINT_E2BIG 7
#define MINT_ENOEXEC 8
#define MINT_ECHILD 10
#define MINT_EACCES 13
#define MINT_ENFILE 23
#define MINT_ENOTTY 25
#define MINT_ESPIPE 29
#define MINT_ERANGE 34
#define MINT_ELOOP 40
#endif

#define MINT_PROC_NAME_MAX 32u
#define MINT_PROC_PATH_MAX 512u
#define MINT_PROC_ARGS_MAX 16384u    /* argv + envp bytes */
#define MINT_PROC_ARGC_MAX 256u

#define MINT_SPAWN_VERSION 1u
#define MINT_SPAWN_PIPE_STDIO 0x1u   /* stdin pipe + one stdout/stderr pipe; parent receives fds */
#define MINT_SPAWN_TTY        0x2u   /* those pipes report isatty() and an 80x24 window */
typedef struct mint_spawn_request {
    uint32_t version;
    uint32_t flags;
    uint64_t path;         /* absolute MintOS HD path, user pointer */
    uint64_t cwd;          /* working directory or 0 for "/" */
    uint64_t args;         /* argc argv strings then envc env strings, NUL separated */
    uint32_t args_bytes;
    uint32_t argc;
    uint32_t envc;
    uint32_t frame_limit;  /* owned pages; 0 = default (16384 = 64 MiB) */
    uint32_t stack_bytes;  /* 0 = 8 MiB of demand-zero stack */
    int32_t stdin_fd;      /* out: parent's write end of the child's stdin */
    int32_t stdout_fd;     /* out: parent's read end of the child's stdout+stderr */
    uint32_t reserved;
} MINT_SPAWN_REQUEST;

#define MINT_WAIT_NOHANG 0x1u
enum {
    MINT_PROC_EXIT_NORMAL = 1,      /* code = exit status 0..255 */
    MINT_PROC_EXIT_KILLED = 2,      /* code = signal number */
    MINT_PROC_EXIT_FAULT = 3,       /* code = CPU exception vector, detail = error code */
    MINT_PROC_EXIT_LOAD_FAILED = 4  /* code = 127 */
};
typedef struct mint_proc_status {
    uint64_t pid;
    int32_t code;
    uint32_t reason;
    uint64_t detail;
} MINT_PROC_STATUS;

#define MINT_SIGINT 2u
#define MINT_SIGKILL 9u
#define MINT_SIGTERM 15u

enum { MINT_PROC_INFO_STARTING = 0, MINT_PROC_INFO_RUNNING = 1, MINT_PROC_INFO_ZOMBIE = 2 };
typedef struct mint_proc_info {
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t state;
    uint32_t reserved;
    uint64_t owned_pages;
    uint64_t peak_pages;
    uint64_t frame_limit;
    uint64_t cpu_ticks;    /* 100 Hz ticks spent in ring 3 */
    uint64_t start_tick;
    char name[MINT_PROC_NAME_MAX];
} MINT_PROC_INFO;

#define MINT_IO_NONBLOCK 0x1u

#define MINT_MEM_READ  0x1u
#define MINT_MEM_WRITE 0x2u
#define MINT_MEM_EXEC  0x4u

#define MINT_PLATFORM_INFO_VERSION 1u
typedef struct mint_platform_info {
    uint32_t version;
    uint32_t fpu_flags;        /* MINT_FPU_FLAG_* from fpu.h: 1 SSE, 2 FXSR, 4 XSAVE, 8 AVX, 16 ready */
    uint32_t fpu_area_bytes;
    uint32_t linux_compat;     /* 1 if the `syscall` instruction entry is enabled */
    uint64_t xcr0;
    uint64_t fpu_saves;
    uint64_t fpu_restores;
    uint64_t preemptions;
    uint64_t pool_pages;       /* user-page pool held from physical memory */
    uint64_t pool_used_pages;
    uint64_t pool_peak_pages;
    uint64_t processes_live;
    uint64_t tasks_live;
    uint64_t linux_syscalls;
    uint64_t linux_unsupported;
    uint64_t demand_faults;
} MINT_PLATFORM_INFO;

#endif
