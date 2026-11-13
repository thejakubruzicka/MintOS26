#ifndef MINTOS26_LINUX_COMPAT_H
#define MINTOS26_LINUX_COMPAT_H
/* 26.5.014.0: Linux x86-64 system-call compatibility SUBSET.
 *
 * MintOS's own ABI is int 0x80 with MintOS numbers. Separately from that, the
 * `syscall` instruction (EFER.SCE/STAR/LSTAR/SFMASK) is routed here and the
 * Linux x86-64 numbers below are translated to MintOS services. This is
 * enough for static musl programs (Lua, the SDK tools) and is the start of
 * running static Linux binaries (roadmap Phase E2). It is NOT Linux
 * compatibility: no fork/exec/clone/threads/signals delivery/sockets/ttys,
 * and every unlisted call returns -ENOSYS. See docs/SDK.md for the list. */
#include <stdint.h>
#include <stddef.h>

enum {
    MINT_LINUX_FULL = 1,     /* implemented with Linux semantics for MintOS objects */
    MINT_LINUX_PARTIAL = 2,  /* implemented; some flags/cases return an error */
    MINT_LINUX_STUB = 3      /* accepted and succeeds without effect (documented) */
};
typedef struct mint_linux_syscall_info {
    uint16_t number;
    uint8_t support;
    uint8_t reserved;
    const char *name;
} MINT_LINUX_SYSCALL_INFO;

extern const MINT_LINUX_SYSCALL_INFO mint_linux_syscall_table[];
extern const uint32_t mint_linux_syscall_table_count;
/* 0 if the number is not part of the subset (-> -ENOSYS). */
const MINT_LINUX_SYSCALL_INFO *mint_linux_syscall_info(uint64_t number);

/* ---- pure translation helpers (host-tested) ---- */
#define MINT_LINUX_O_ACCMODE   03u
#define MINT_LINUX_O_WRONLY    01u
#define MINT_LINUX_O_RDWR      02u
#define MINT_LINUX_O_CREAT     0100u
#define MINT_LINUX_O_EXCL      0200u
#define MINT_LINUX_O_NOCTTY    0400u
#define MINT_LINUX_O_TRUNC     01000u
#define MINT_LINUX_O_APPEND    02000u
#define MINT_LINUX_O_NONBLOCK  04000u
#define MINT_LINUX_O_LARGEFILE 0100000u
#define MINT_LINUX_O_DIRECTORY 0200000u
#define MINT_LINUX_O_NOFOLLOW  0400000u
#define MINT_LINUX_O_CLOEXEC   02000000u
#define MINT_LINUX_O_PATH      010000000u
/* MintFS open flags for a Linux open(2) flag word, or a negative errno. */
int64_t mint_linux_open_flags(uint64_t linux_flags);

/* Join cwd and path and normalize ".", ".." and repeated '/' lexically.
 * Returns the length, or a negative errno (-ENAMETOOLONG, -ENOENT for ""). */
int64_t mint_linux_path_resolve(const char *cwd, const char *path, char *out, size_t capacity);

/* struct stat as the x86-64 Linux kernel lays it out (144 bytes). */
typedef struct mint_linux_stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    int64_t st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    int64_t unused[3];
} MINT_LINUX_STAT;
#define MINT_LINUX_S_IFIFO 0010000u
#define MINT_LINUX_S_IFCHR 0020000u
#define MINT_LINUX_S_IFDIR 0040000u
#define MINT_LINUX_S_IFREG 0100000u
void mint_linux_stat_fill(MINT_LINUX_STAT *out, uint32_t mode, uint64_t ino, uint64_t size,
                          int64_t mtime, int64_t ctime);
/* wait4 status word for a MintOS exit reason/code (see uapi/mintos/process.h). */
int32_t mint_linux_wait_status(uint32_t reason, int32_t code);
/* Linux signal for a CPU exception vector that terminated a process. */
int32_t mint_linux_fault_signal(uint32_t vector);

#ifndef MINT_LINUX_COMPAT_HOST_TEST
struct mint_syscall_frame;
/* Programs EFER.SCE/STAR/LSTAR/SFMASK. Returns 1 when the entry is live. */
int mint_linux_compat_init(void);
int mint_linux_compat_enabled(void);
uint64_t mint_linux_compat_calls(void);
uint64_t mint_linux_compat_unsupported(void);
/* C side of the `syscall` entry (kernel/arch/x86_64/linux_entry.S). */
int64_t mint_linux_syscall_dispatch(struct mint_syscall_frame *frame);
#endif
#endif
