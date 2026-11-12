# MintOS SDK (26.5.014.0)

The SDK builds **command-line programs** that are stored on MintOS HD and run
as their own ring-3 processes. Programs are written in C against **musl 1.2.5**
(a static libc) and compiled with clang/lld on the build host.

## How programs talk to MintOS

MintOS has two system-call entries:

| Entry | Numbers | Used by |
|---|---|---|
| `int 0x80` | MintOS's own ABI (`kernel/include/uapi/mintos/*.h`) | MintDE, the boot self-tests, and any program that wants MintOS-specific services |
| `syscall` instruction | **Linux x86-64 numbers, translated** (`kernel/abi/linux_compat.c`) | musl, and so every SDK program |

The second entry is a **compatibility subset**, not Linux compatibility. MintOS
translates 73 Linux calls to its own services. Every other number returns
`-ENOSYS`, and the first use of each is logged on COM1 (`[linux] unsupported syscall N`).
`sysinfo` reports how many calls were translated and how many were unsupported.

### What is supported

- **Files** on MintOS HD: `open`, `openat`, `close`, `read`, `write`, `readv`, `writev`, `pread64`, `lseek`, `stat`, `fstat`, `lstat`, `newfstatat`, `access`, `faccessat`, `getdents64`, `mkdir`, `rmdir`, `unlink`, `rename`, `getcwd`, `chdir` and `fsync`. Paths are relative to the process's working directory, and `.` and `..` are resolved. `/dev/null`, `/dev/tty` and `/dev/std{in,out,err}` exist.
- **Descriptors:** `dup`, `dup2`, `dup3`, `fcntl` (`F_DUPFD`, `F_GETFD/SETFD`, `F_GETFL/SETFL` with `O_NONBLOCK`), `pipe`, `pipe2` and `poll`. On Terminal's pipes, `ioctl(TIOCGWINSZ, TCGETS)` makes `isatty()` true, so stdout is line-buffered.
- **Memory:** anonymous `mmap`, `munmap`, `mprotect`, `brk` and `madvise` (accepted, no effect). `mremap` is `ENOSYS`; musl falls back to copying.
- **Time:** `clock_gettime` (realtime, monotonic and process CPU time), `clock_getres`, `gettimeofday`, `time`, `nanosleep`, `clock_nanosleep` and `times`. The clock has 10 ms resolution (100 Hz PIT).
- **Process:** `exit`, `exit_group`, `getpid`, `getppid`, `gettid`, `set_tid_address`, `wait4`, `kill`, `tkill`, `tgkill`, `uname`, `getrlimit`, `prlimit64`, `getrusage`, `sysinfo`, `arch_prctl(ARCH_SET_FS/GET_FS)` (the FS base is saved per task), and uid/gid queries (always 0).
- **Accepted, no effect:** `rt_sigaction`, `rt_sigprocmask`, `sigaltstack`, `set_robust_list` and `umask`.
- **Partial:** `getrandom` mixes the TSC and is **not** cryptographic.

### What is not supported (yet)

- `fork`, `vfork`, `clone`, `execve`, `posix_spawn` and threads. Programs are started by MintOS (Terminal), not by other programs.
- Signal delivery. Handlers are accepted but never called; Ctrl+C and `kill` terminate the process.
- Sockets, file-backed `mmap`, symbolic links, permissions/users, and real terminals (no raw mode).
- Dynamic linking. Programs are static.

## Memory and limits

- Each process gets a private address space from 64 GiB to 512 GiB. Programs are linked at 64 GiB (`0x1000000000`).
- `mmap`/`brk` memory and the stack are **demand-zero**: a page is allocated on its first touch.
- A process may own **16384 pages (64 MiB)** by default. When it hits the limit, the program is stopped with an "out of memory" message on COM1.
- The stack has 8 MiB of address space above a 64 KiB guard region. Running into the guard region is reported as a stack overflow.
- Writable+executable memory is refused (W^X), for image segments and `mmap` alike.
- Floating point, SSE and AVX work. Registers are saved and restored per task, and the timer preempts ring-3 code every 20 ms when another task is waiting.

## Building

`make all` builds the SDK as part of the image:

1. **musl:** `sdk/musl-config.mak` replaces `./configure`. musl is built out of tree in `build/musl-obj` and installed to `build/sdk/sysroot/{include,lib}`.
2. **Programs:** `sdk/programs/<name>.c` becomes `build/sdk/bin/<name>`. `vendor/lua/src` is built with `LUA_USE_POSIX` into `build/sdk/bin/lua`.
3. **Bundle:** `tools/mkprograms.py` bundles the programs and `sdk/examples/*.lua` into the kernel image.

To add a tool, put `sdk/programs/foo.c` in the tree and add `foo` to `SDK_TOOLS` in `sdk/sdk.mk`.

The rules, for building by hand:

```sh
clang -target x86_64-unknown-linux-musl -nostdinc -isystem build/sdk/sysroot/include \
      -fPIE -O2 -c foo.c -o foo.o
ld.lld -static -nostdlib --image-base=0x1000000000 --defsym=_DYNAMIC=__ehdr_start \
       -z max-page-size=0x1000 --gc-sections -s -o foo \
       build/sdk/sysroot/lib/crt1.o build/sdk/sysroot/lib/crti.o foo.o \
       build/sdk/sysroot/lib/libc.a build/sdk/sysroot/lib/crtn.o
```

- `-fPIE` makes all code RIP-relative, so it runs above 4 GiB.
- `crt1.o` names the weak `_DYNAMIC` RIP-relatively. Static programs never use it, so it is pinned to the image start.
- The loader also accepts **static-PIE** images (`ET_DYN` without `PT_INTERP`) and places them at 64 GiB. This path is not covered by tests yet.

## How programs reach MintOS HD

The bundle is installed into `/System/Programs` (programs) and `/Library/Examples/Lua` (Lua samples):

- **When:** at boot, whenever a MintOS disk is mounted and `/System/Programs/.bundle-id` differs from the bundle's id. Terminal's `install-programs` command forces a fresh install.
- **How:** only through the MintFS file API, so only the MintOS disk is ever written.

## Running programs in Terminal

| You type | What happens |
|---|---|
| `name args…` | `name` is looked up in `PATH` (`/System/Programs`, then `~/Programs`). |
| `./prog`, `dir/prog`, `/abs/path` | Runs that file. |
| `"two words"`, `'…'`, `\x` | Quoting and escapes group arguments. |
| `prog &` | Runs in the background. Its output is prefixed with `[prog]`, and stdin is closed. |
| Typed line + Enter | Goes to the program's stdin and is echoed after the program's prompt. |
| Ctrl+C | Interrupts the program (SIGINT). |
| Ctrl+D | Ends the program's input. |
| `ps`, `kill <pid>`, `which <name>` | List processes, terminate one, show where a name resolves. |

Environment: `PATH=/System/Programs`, `HOME`, `PWD`, `TERM=dumb`, `USER=mintos`, and `LUA_PATH=/Library/Lua/?.lua;./?.lua`.

## Bundled programs

| Program | What it does |
|---|---|
| `lua` | Lua 5.4.9. With no arguments it starts the REPL; `lua script.lua args…` runs a script. |
| `hello` | Prints argv, environment and a floating-point result. `hello fail` exits with status 3. |
| `wc` | `wc [-lwc] [file…]` |
| `grep` | `grep [-ivncEF] pattern [file…]`, using musl's POSIX regex. |
| `sysinfo` | System, CPU, memory, FPU, scheduler, page pool, Linux-call counters and the process list. Uses both syscall ABIs. |
| `spin` | A CPU-bound loop for testing preemption and Ctrl+C. `spin [seconds]` |

Lua samples:

| Sample | What it shows |
|---|---|
| `hello.lua` | Floats, strings and `os.date`. |
| `primes.lua [n]` | A sieve timed with `os.clock`. |
| `files.lua` | Writes and reads `~/Documents/lua-test.txt`. |
