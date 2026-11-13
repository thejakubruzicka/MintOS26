/* 26.5.014.0 app platform host tests: frame pool, Linux-compat syscall table
 * and translation helpers, pipes/OFDs/descriptor tables, FPU clean state.
 * Links the production sources (see tools/test_app_platform.py). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/frame_pool.h"
#include "../kernel/include/linux_compat.h"
#include "../kernel/include/proc_files.h"
#include "../kernel/include/fpu.h"
#include "../kernel/include/mintos_fs.h"
#include "../kernel/include/uapi/mintos/syscall.h"
#include "../kernel/include/uapi/mintos/process.h"

static int failures;
#define CHECK(c, msg) do { if (c) printf("PASS: %s\n", msg); else { ++failures; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* ---- fake physmem for the pool: 64 MiB of "physical" pages */
#define FAKE_PAGES 16384u
static uint64_t fake_free = FAKE_PAGES, fake_next = 0x40000000ULL, claims, releases;
static uint64_t f_claim(void *c, uint64_t pages) { (void)c; if (pages > fake_free) return 0; fake_free -= pages; uint64_t b = fake_next; fake_next += pages * 4096u; ++claims; return b; }
static int f_release(void *c, uint64_t base, uint64_t pages) { (void)c; (void)base; fake_free += pages; ++releases; return 1; }
static uint64_t f_free(void *c) { (void)c; return fake_free; }

static void test_frame_pool(void) {
    MINT_FRAME_POOL_BACKEND b = {0, f_claim, f_release, f_free, 0};
    CHECK(mint_frame_pool_init(&b, 4096u), "pool initializes with a 16 MiB kernel reserve");
    static uint64_t pages[5000];
    int ok = 1;
    for (int i = 0; i < 5000; ++i) { pages[i] = mint_frame_pool_alloc(); ok &= pages[i] != 0 && !(pages[i] & 4095u); }
    CHECK(ok, "5000 single pages (more than physmem's 4096 records) come from a few chunks");
    MINT_FRAME_POOL_STATS st; mint_frame_pool_get_stats(&st);
    CHECK(claims == 5 && st.chunks == 5 && st.used_pages == 5000 && st.pool_pages == 5120, "4 MiB chunks, one physmem claim each");
    int unique = 1;
    for (int i = 1; i < 5000 && unique; ++i) unique = pages[i] != pages[i - 1];
    CHECK(unique && mint_frame_pool_is_allocated(pages[1234]) && mint_frame_pool_overlaps(pages[10], 1), "pages are distinct and tracked");
    CHECK(mint_frame_pool_free(pages[7]) && !mint_frame_pool_free(pages[7]) && !mint_frame_pool_is_allocated(pages[7]), "double free is refused");
    pages[7] = mint_frame_pool_alloc();
    CHECK(pages[7] != 0, "a freed page is handed out again");
    for (int i = 0; i < 5000; ++i) (void)mint_frame_pool_free(pages[i]);
    mint_frame_pool_get_stats(&st);
    CHECK(st.used_pages == 0 && st.chunks == 1 && releases == 4, "empty chunks return to physmem, one spare chunk is kept");
    /* The reserve: with 16384 fake pages and a 4096-page reserve at most 12288 pool pages exist. */
    uint64_t n = 0;
    while (mint_frame_pool_alloc()) ++n;
    mint_frame_pool_get_stats(&st);
    CHECK(fake_free >= 4096u && n == st.pool_pages && st.failed_allocations == 1, "the pool never grows into the kernel reserve");
}

static void test_linux_table(void) {
    static const struct { unsigned nr; const char *name; } linux_x86_64[] = {
        {0, "read"}, {1, "write"}, {2, "open"}, {3, "close"}, {4, "stat"}, {5, "fstat"}, {8, "lseek"},
        {9, "mmap"}, {10, "mprotect"}, {11, "munmap"}, {12, "brk"}, {16, "ioctl"}, {20, "writev"},
        {39, "getpid"}, {60, "exit"}, {61, "wait4"}, {62, "kill"}, {63, "uname"}, {72, "fcntl"},
        {79, "getcwd"}, {80, "chdir"}, {82, "rename"}, {83, "mkdir"}, {87, "unlink"}, {158, "arch_prctl"},
        {217, "getdents64"}, {218, "set_tid_address"}, {228, "clock_gettime"}, {231, "exit_group"},
        {257, "openat"}, {262, "newfstatat"}, {302, "prlimit64"}, {318, "getrandom"},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof(linux_x86_64) / sizeof(linux_x86_64[0]); ++i) {
        const MINT_LINUX_SYSCALL_INFO *e = mint_linux_syscall_info(linux_x86_64[i].nr);
        if (!e || strcmp(e->name, linux_x86_64[i].name)) { ok = 0; printf("  mismatch at %u\n", linux_x86_64[i].nr); }
    }
    CHECK(ok, "translated numbers match the Linux x86-64 syscall table");
    int sorted = 1;
    for (uint32_t i = 1; i < mint_linux_syscall_table_count; ++i)
        sorted &= mint_linux_syscall_table[i - 1].number < mint_linux_syscall_table[i].number;
    CHECK(sorted, "syscall table is strictly sorted (binary search)");
    CHECK(!mint_linux_syscall_info(56) && !mint_linux_syscall_info(57) && !mint_linux_syscall_info(59) &&
          !mint_linux_syscall_info(41), "clone, fork, execve and socket stay outside the subset (ENOSYS)");
    CHECK(mint_linux_open_flags(0) == MINT_O_READ && mint_linux_open_flags(01 | 0100 | 01000) == (MINT_O_WRITE | MINT_O_CREATE | MINT_O_TRUNC) &&
          mint_linux_open_flags(02 | 02000) == (MINT_O_READ | MINT_O_WRITE | MINT_O_APPEND) &&
          mint_linux_open_flags(010000000) == -MINT_EINVAL && mint_linux_open_flags(0 | 01000) == MINT_O_READ,
          "open(2) flags translate to MintFS flags; O_PATH refused; O_TRUNC needs write access");
    char out[64];
    CHECK(mint_linux_path_resolve("/Users/mintos", "Documents/../Desktop/./a.lua", out, sizeof out) > 0 &&
          !strcmp(out, "/Users/mintos/Desktop/a.lua"), "relative paths resolve against the cwd");
    CHECK(mint_linux_path_resolve("/a", "/../../x//y/", out, sizeof out) > 0 && !strcmp(out, "/x/y") &&
          mint_linux_path_resolve("/", "..", out, sizeof out) == 1 && !strcmp(out, "/"), ".. stops at the root, slashes collapse");
    CHECK(mint_linux_path_resolve("/", "", out, sizeof out) == -MINT_ENOENT &&
          mint_linux_path_resolve("/", "abcdefghijabcdefghijabcdefghij", out, 16) == -MINT_ENAMETOOLONG, "empty and overlong paths are errors");
    MINT_LINUX_STAT st;
    mint_linux_stat_fill(&st, MINT_LINUX_S_IFREG | 0755u, 9, 5000, 1700000000, 1700000001);
    CHECK(sizeof(MINT_LINUX_STAT) == 144 && __builtin_offsetof(MINT_LINUX_STAT, st_size) == 48 &&
          __builtin_offsetof(MINT_LINUX_STAT, st_mtime_sec) == 88 && st.st_blocks == 10 && st.st_ino == 9,
          "struct stat matches the x86-64 kernel layout");
    CHECK(mint_linux_wait_status(MINT_PROC_EXIT_NORMAL, 3) == 0x300 && mint_linux_wait_status(MINT_PROC_EXIT_KILLED, 2) == 2 &&
          mint_linux_wait_status(MINT_PROC_EXIT_FAULT, 14) == 11 && mint_linux_wait_status(MINT_PROC_EXIT_FAULT, 0) == 8,
          "wait4 status words: exit codes, signals, faults as SIGSEGV/SIGFPE");
}

static void test_pipes_and_fds(void) {
    mint_proc_files_init();
    static MINT_PROCESS p;
    memset(&p, 0, sizeof p);
    mint_fd_table_init(&p);
    int32_t pipe = mint_pipe_create();
    int32_t r = mint_ofd_create(MINT_OFD_PIPE_READ, MINT_OFD_READ, pipe, 1);
    int32_t w = mint_ofd_create(MINT_OFD_PIPE_WRITE, MINT_OFD_WRITE | MINT_OFD_TTY, pipe, 1);
    CHECK(pipe >= 0 && r >= 0 && w >= 0 && mint_pipe_readers(pipe) == 1 && mint_pipe_writers(pipe) == 1, "pipe ends are counted per open file description");
    static uint8_t big[MINT_PIPE_BYTES + 100], back[MINT_PIPE_BYTES + 100];
    for (size_t i = 0; i < sizeof big; ++i) big[i] = (uint8_t)(i * 7u);
    CHECK(mint_pipe_put(pipe, big, sizeof big) == MINT_PIPE_BYTES && mint_pipe_buffered(pipe) == MINT_PIPE_BYTES, "a full pipe accepts no more bytes");
    CHECK(mint_pipe_get(pipe, back, 100) == 100 && mint_pipe_put(pipe, big + MINT_PIPE_BYTES, 100) == 100 &&
          mint_pipe_get(pipe, back + 100, sizeof back) == MINT_PIPE_BYTES && !memcmp(back, big, sizeof big), "ring buffer keeps byte order across wrap-around");
    CHECK(mint_fd_install_at(&p, r, 0, 0) == 0 && mint_fd_install_at(&p, w, 1, 0) == 1, "descriptors 0 and 1 installed");
    mint_ofd_ref(w);
    CHECK(mint_fd_install(&p, w, 0, MINT_FD_FLAG_CLOEXEC) == 2 && mint_fd_is_tty(&p, 2) && !mint_fd_is_tty(&p, 0), "dup shares the description (tty flag) at the lowest free fd");
    CHECK(mint_fd_close(&p, 1) == 0 && mint_pipe_writers(pipe) == 1 && mint_fd_close(&p, 2) == 0 && mint_pipe_writers(pipe) == 0,
          "the write end closes only with its last descriptor (EOF for readers)");
    CHECK(mint_fd_close(&p, 1) == -MINT_EBADF && mint_fd_ofd(&p, 5) == -1, "closed or unused descriptors are EBADF");
    mint_fd_close_all(&p);
    CHECK(mint_pipe_readers(pipe) == 0 && mint_pipe_create() == pipe, "closing everything frees the pipe slot");
    int32_t dir = mint_dir_handle_open("/Users/mintos");
    int32_t dofd = mint_ofd_create(MINT_OFD_DIR, MINT_OFD_READ, dir, 1);
    CHECK(dir >= 0 && dofd >= 0 && !strcmp(mint_dir_handle_path(dir), "/Users/mintos") && mint_ofd_dir_pos(dofd) && *mint_ofd_dir_pos(dofd) == 0,
          "directory handles keep their path and getdents position");
    mint_ofd_unref(dofd);
    CHECK(mint_dir_handle_path(dir) == 0, "the directory handle closes with its description");
}

static void test_fpu_state(void) {
    MINT_FPU_STATE s;
    memset(&s, 0xAB, sizeof s);
    mint_fpu_state_make_clean(&s);
    uint16_t fcw; uint32_t mxcsr; uint64_t xstate_bv;
    memcpy(&fcw, s.bytes + MINT_FPU_FCW_OFFSET, 2); memcpy(&mxcsr, s.bytes + MINT_FPU_MXCSR_OFFSET, 4);
    memcpy(&xstate_bv, s.bytes + MINT_FPU_XSTATE_BV_OFFSET, 8);
    CHECK(fcw == 0x037F && mxcsr == 0x1F80 && xstate_bv == 0 && s.bytes[4] == 0 && s.bytes[160] == 0,
          "new tasks start with FCW 0x37F, MXCSR 0x1F80 (all exceptions masked), empty x87 stack, zero XMM/YMM");
    CHECK(_Alignof(MINT_FPU_STATE) == 64 && sizeof(MINT_FPU_STATE) >= 832, "save area is XSAVE-aligned and holds x87+SSE+AVX");
}

int main(void) {
    test_frame_pool();
    test_linux_table();
    test_pipes_and_fds();
    test_fpu_state();
    if (failures) { printf("%d app-platform host test(s) FAILED\n", failures); return 1; }
    puts("PASS: app-platform host tests complete");
    return 0;
}
