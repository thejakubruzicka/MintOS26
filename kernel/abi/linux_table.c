/* 26.5.014.0: the Linux x86-64 syscall subset MintOS translates, plus pure
 * translation helpers. No kernel dependencies: tools/test_linux_compat.c
 * links this file on the host. */
#include <stdint.h>
#include <stddef.h>
#include "../include/linux_compat.h"
#include "../include/mintos_fs.h"
#include "../include/uapi/mintos/syscall.h"
#include "../include/uapi/mintos/process.h"

#define F MINT_LINUX_FULL
#define P MINT_LINUX_PARTIAL
#define S MINT_LINUX_STUB
/* Sorted by number. */
const MINT_LINUX_SYSCALL_INFO mint_linux_syscall_table[] = {
    {0, F, 0, "read"},          {1, F, 0, "write"},         {2, P, 0, "open"},
    {3, F, 0, "close"},         {4, F, 0, "stat"},          {5, F, 0, "fstat"},
    {6, F, 0, "lstat"},         {7, P, 0, "poll"},          {8, P, 0, "lseek"},
    {9, P, 0, "mmap"},          {10, P, 0, "mprotect"},     {11, F, 0, "munmap"},
    {12, F, 0, "brk"},          {13, S, 0, "rt_sigaction"}, {14, S, 0, "rt_sigprocmask"},
    {16, P, 0, "ioctl"},        {17, P, 0, "pread64"},      {19, F, 0, "readv"},
    {20, F, 0, "writev"},       {21, F, 0, "access"},       {22, F, 0, "pipe"},
    {24, F, 0, "sched_yield"},  {28, S, 0, "madvise"},      {32, F, 0, "dup"},
    {33, F, 0, "dup2"},         {35, F, 0, "nanosleep"},    {39, F, 0, "getpid"},
    {60, F, 0, "exit"},         {61, P, 0, "wait4"},        {62, P, 0, "kill"},
    {63, F, 0, "uname"},        {72, P, 0, "fcntl"},        {74, F, 0, "fsync"},
    {75, F, 0, "fdatasync"},    {79, F, 0, "getcwd"},       {80, F, 0, "chdir"},
    {82, F, 0, "rename"},       {83, F, 0, "mkdir"},        {84, F, 0, "rmdir"},
    {87, F, 0, "unlink"},       {89, P, 0, "readlink"},     {95, S, 0, "umask"},
    {96, F, 0, "gettimeofday"}, {97, P, 0, "getrlimit"},    {98, P, 0, "getrusage"},
    {99, P, 0, "sysinfo"},      {100, P, 0, "times"},       {102, F, 0, "getuid"},
    {104, F, 0, "getgid"},      {107, F, 0, "geteuid"},     {108, F, 0, "getegid"},
    {110, F, 0, "getppid"},     {131, S, 0, "sigaltstack"}, {158, P, 0, "arch_prctl"},
    {186, F, 0, "gettid"},      {200, P, 0, "tkill"},       {201, F, 0, "time"},
    {217, F, 0, "getdents64"},  {218, F, 0, "set_tid_address"},
    {228, F, 0, "clock_gettime"}, {229, F, 0, "clock_getres"}, {230, P, 0, "clock_nanosleep"},
    {231, F, 0, "exit_group"},  {234, P, 0, "tgkill"},      {257, P, 0, "openat"},
    {262, P, 0, "newfstatat"},  {269, F, 0, "faccessat"},   {273, S, 0, "set_robust_list"},
    {292, F, 0, "dup3"},        {293, F, 0, "pipe2"},       {302, P, 0, "prlimit64"},
    {318, P, 0, "getrandom"},   {439, F, 0, "faccessat2"},
};
#undef F
#undef P
#undef S
const uint32_t mint_linux_syscall_table_count =
    (uint32_t)(sizeof(mint_linux_syscall_table) / sizeof(mint_linux_syscall_table[0]));

const MINT_LINUX_SYSCALL_INFO *mint_linux_syscall_info(uint64_t number) {
    uint32_t lo = 0, hi = mint_linux_syscall_table_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (mint_linux_syscall_table[mid].number == number) return &mint_linux_syscall_table[mid];
        if (mint_linux_syscall_table[mid].number < number) lo = mid + 1u; else hi = mid;
    }
    return 0;
}

int64_t mint_linux_open_flags(uint64_t f) {
    uint64_t known = MINT_LINUX_O_ACCMODE | MINT_LINUX_O_CREAT | MINT_LINUX_O_EXCL | MINT_LINUX_O_NOCTTY |
                     MINT_LINUX_O_TRUNC | MINT_LINUX_O_APPEND | MINT_LINUX_O_NONBLOCK |
                     MINT_LINUX_O_LARGEFILE | MINT_LINUX_O_DIRECTORY | MINT_LINUX_O_NOFOLLOW |
                     MINT_LINUX_O_CLOEXEC;
    if (f & ~known) return -MINT_EINVAL;     /* O_PATH, O_TMPFILE, O_SYNC... */
    uint32_t acc = (uint32_t)(f & MINT_LINUX_O_ACCMODE);
    if (acc == 3u) return -MINT_EINVAL;
    uint32_t out = acc == 0u ? MINT_O_READ : acc == MINT_LINUX_O_WRONLY ? MINT_O_WRITE : (MINT_O_READ | MINT_O_WRITE);
    if (f & MINT_LINUX_O_CREAT) out |= MINT_O_CREATE;
    if (f & MINT_LINUX_O_EXCL) out |= MINT_O_EXCL;
    if (f & MINT_LINUX_O_TRUNC) out |= MINT_O_TRUNC;
    if (f & MINT_LINUX_O_APPEND) out |= MINT_O_APPEND;
    if ((out & (MINT_O_TRUNC | MINT_O_APPEND)) && !(out & MINT_O_WRITE)) out &= ~(MINT_O_TRUNC | MINT_O_APPEND);
    return out;
}

int64_t mint_linux_path_resolve(const char *cwd, const char *path, char *out, size_t capacity) {
    if (!path || !out || capacity < 2u) return -MINT_EINVAL;
    if (!path[0]) return -MINT_ENOENT;
    size_t n = 0;
    out[n++] = '/';
    /* Process cwd (if relative) and then path, component by component. */
    for (int pass = 0; pass < 2; ++pass) {
        const char *s = pass == 0 ? (path[0] == '/' ? "" : (cwd ? cwd : "/")) : path;
        size_t i = 0;
        while (s[i]) {
            while (s[i] == '/') ++i;
            if (!s[i]) break;
            size_t start = i;
            while (s[i] && s[i] != '/') ++i;
            size_t len = i - start;
            if (len == 1u && s[start] == '.') continue;
            if (len == 2u && s[start] == '.' && s[start + 1u] == '.') {
                while (n > 1u && out[n - 1u] != '/') --n;   /* drop the last component */
                if (n > 1u) --n;                            /* and its slash */
                continue;
            }
            if (n > 1u) {
                if (n + 1u >= capacity) return -MINT_ENAMETOOLONG;
                out[n++] = '/';
            }
            if (n + len >= capacity) return -MINT_ENAMETOOLONG;
            for (size_t k = 0; k < len; ++k) out[n++] = s[start + k];
        }
    }
    out[n] = '\0';
    return (int64_t)n;
}

void mint_linux_stat_fill(MINT_LINUX_STAT *out, uint32_t mode, uint64_t ino, uint64_t size,
                          int64_t mtime, int64_t ctime) {
    uint8_t *b = (uint8_t *)out;
    for (size_t i = 0; i < sizeof(*out); ++i) b[i] = 0;
    out->st_dev = 0x4D46u;   /* "MF" */
    out->st_ino = ino ? ino : 1u;
    out->st_nlink = 1u;
    out->st_mode = mode;
    out->st_size = (int64_t)size;
    out->st_blksize = 4096;
    out->st_blocks = (int64_t)((size + 511u) / 512u);
    out->st_atime_sec = mtime;
    out->st_mtime_sec = mtime;
    out->st_ctime_sec = ctime;
}

int32_t mint_linux_fault_signal(uint32_t vector) {
    switch (vector) {
    case 0u: case 16u: case 19u: return 8;    /* SIGFPE: #DE, #MF, #XM */
    case 6u: return 4;                        /* SIGILL: #UD */
    case 3u: case 1u: return 5;               /* SIGTRAP */
    case 17u: return 7;                       /* SIGBUS: #AC */
    default: return 11;                       /* SIGSEGV: #PF, #GP, #SS, ... */
    }
}

int32_t mint_linux_wait_status(uint32_t reason, int32_t code) {
    switch (reason) {
    case MINT_PROC_EXIT_NORMAL: return (code & 0xFF) << 8;
    case MINT_PROC_EXIT_KILLED: return code & 0x7F;
    case MINT_PROC_EXIT_FAULT: return mint_linux_fault_signal((uint32_t)code) & 0x7F;
    default: return 127 << 8;
    }
}
