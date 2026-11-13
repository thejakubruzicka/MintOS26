# 26.5.014.0 — app platform (Phase B): release notes for the coordinator

MintOS can now run real, ported programs stored on MintOS HD. Terminal starts
`lua`, `lua script.lua`, `./prog` and the bundled tools as separate ring-3
processes with live output, typed input, Ctrl+C, exit statuses and background
jobs. Several programs run at once, preempted by the timer.

## What changed

### FPU / SSE / AVX in ring 3 (`kernel/arch/x86_64/fpu.c`, `kernel/include/fpu.h`)
- **CPU setup:**
  - CR0: MP and NE set, EM and TS cleared.
  - CR4: OSFXSR and OSXMMEXCPT, plus OSXSAVE when the CPU has XSAVE.
  - XCR0 = x87 | SSE | AVX when available; AVX-512 is deliberately left off, so the save area stays ≤ 1024 bytes.
- **Per-task state:** each task has a 64-byte-aligned XSAVE area (FXSAVE as fallback). Switching is eager:
  - the scheduler saves the outgoing user task's registers after every switch back to the scheduler (yield, sleep, exit, preemption);
  - it restores the next user task's registers before running it.
- **New programs:**
  - every program starts with the architectural clean state (FCW 0x37F, MXCSR 0x1F80, empty x87 stack, zero XMM/YMM);
  - the FS base (musl's thread pointer, set through `arch_prctl`) is saved per task too.
- **Faults:**
  - `#XM` (with MXCSR), `#MF` and `#NM` raised in ring 3 terminate only that process, with a named reason on COM1;
  - `#NM` cannot happen in practice, because TS is never set.
- **Kernel stays integer-only:** the kernel C code, including `fpu.c`, is still `-mgeneral-regs-only`. MintDE also stays integer-only this release.

### Timer preemption of ring-3 code (`kernel/task/scheduler.c`, `arch.c`)
- **What:** when IRQ0 arrives while CPL3 code runs, and the task has used its 2-tick (20 ms) slice while another task is ready, the IRQ handler switches to the scheduler. The kernel itself stays non-preemptive.
- **Why this is safe:**
  - Preemption happens only when the interrupted context was ring 3, so no kernel structure can be half-updated.
  - The interrupt frame sits on the process's private RSP0 stack, so switching away is the same as a yielding syscall.
- **Why it is needed:** without it, a CPU-bound program (`spin`, a Lua loop) would freeze MintDE, and Ctrl+C could never be delivered.
- **Effect:** MintDE is preempted too, which only makes it smoother.

### Memory for programs (`frame_pool.c`, `address_space.c`, `elf_loader.c`, `proc_memory.c`)
- **Frame pool:**
  - Problem: `physmem.c` has only 4096 claim records, and each user page was one record, so all processes together were capped at about 14 MiB.
  - Fix: user pages and user page tables now come from a bitmap pool that takes 4 MiB chunks from physmem on demand.
  - Limits: the pool never grows into a reserve of 1/8 of RAM (at least 64 MiB), and it returns empty chunks while keeping one spare.
- **Ownership tracking:**
  - Mapped owned pages are tracked in a PTE software bit (bit 10), and borrowed pages in bit 9.
  - The 24-bytes-per-page `frames[]` array is gone, which matters because at 16384 pages it would have been 384 KiB per address space.
  - Allocated-but-unmapped frames sit in a small pending list.
- **Default limit:** `MINT_ASPACE_MAX_USER_FRAMES` = **16384** pages (64 MiB), matching web-engine. A per-process limit of up to 1 GiB can be set at spawn (`frame_limit`).
- **Demand-zero paging:**
  - ANONYMOUS and STACK regions get a zeroed page on first touch, either through a user `#PF` or a kernel copy to or from user memory.
  - The exception stub can now return to user mode for these faults.
  - Hitting the limit stops the process with an "out of memory" message.
- **Stacks:** programs get 8 MiB of stack address space above a 64 KiB GUARD region. A fault there is reported as a stack overflow. MintDE keeps its 64 KiB stack.
- **Memory syscalls:** `mmap`/`munmap`/`mprotect`/`brk` (anonymous memory only). Regions merge and split, the table grows from 32 to 256 regions, and W^X is enforced.
- **Teardown:** the page tables are walked, every owned page is freed and the pending list is released. Peak pages are logged per process. The boot address-space self-test also checks that the pool is back to zero used pages.

### Processes, pipes and descriptors (`process.c`, `proc_files.c`, `proc_syscalls.c`)
- **Spawn:** `MINT_SYS_PROC_SPAWN` starts an ELF64 program from a MintOS HD path with argv, envp and a working directory.
  - Before returning a pid, it checks synchronously that the file exists, is a file and starts with the ELF magic.
  - A runner task then loads the image straight from MintFS through a reader-based ELF loader.
  - The initial stack follows the SysV layout: argc, argv, envp and auxv (`AT_PHDR`, `AT_RANDOM`, `AT_EXECFN`…).
  - The loader also accepts static-PIE images.
- **Process model:**
  - wait (blocking or `WNOHANG`), exit statuses (normal, killed, fault, load failure), zombies until the parent collects them, and re-parenting when the parent exits;
  - kill sets a pending flag and wakes the task. The process leaves at its next return toward ring 3 (syscall exit or preempting IRQ), or right away if it is blocked in a pipe read or write, a wait or a sleep;
  - `MINT_TASK_MAX` goes from 12 to **32**, and process slots from 4 to **24**.
- **Descriptors:** a POSIX-style table of open file descriptions shared by `dup` and spawned stdio. Each process has 32 descriptors.
  - Kinds: console, `/dev/null`, pipe ends, MintFS files and directories.
  - Pipes: 48 pipes of 8 KiB each, with blocking or non-blocking I/O, EOF and EPIPE.
- **Terminal I/O:** a program started from Terminal gets a stdin pipe and one shared stdout/stderr pipe, both marked *tty* so `isatty()` is true and output is line-buffered.
- **Process list:** `MINT_SYS_PROC_LIST` (pid, parent, state, owned and peak pages, CPU ticks), used by `ps` and `sysinfo`.
- **Syscall numbers:** 0x4D60–0x4D6D, defined in the new header `kernel/include/uapi/mintos/process.h`. Log component 13 is PROC.

### Linux x86-64 syscall-compatibility subset (`linux_compat.c`, `linux_table.c`, `linux_entry.S`)
- **Entry:** the `syscall` instruction, enabled through EFER.SCE, STAR, LSTAR and SFMASK.
  - SFMASK clears IF, TF, DF, AC and NT.
  - The entry switches to the task's RSP0 through a shadow copy, then builds the same frame as `int 0x80`.
  - It returns with IRETQ; SYSRET is never used.
- **Coverage:** 73 Linux calls are translated to MintOS services (listed in `docs/SDK.md`); everything else returns `-ENOSYS` and is logged once.
- **Why this route, not a musl port to `int 0x80`:**
  - musl stays an unmodified upstream build.
  - The same entry is the first step of Phase E2 (static Linux binaries).
  - The MintOS ABI stays separate. It is documented everywhere as a subset, **not** Linux compatibility.

### C library, SDK and programs (`vendor/`, `sdk/`, `docs/SDK.md`)
- **Vendored sources** (unmodified):
  - musl 1.2.5, SHA-256 a9a118bb…c75e4;
  - Lua 5.4.9 (latest 5.4), SHA-256 2335b6c5…fb8e6;
  - both are listed in `vendor/README.md` with their licences.
- **musl build:** out of tree with a hand-written `sdk/musl-config.mak`, in about 10 s.
- **Programs:** static ET_EXEC images at 64 GiB, built with `-fPIE` and linked with `ld.lld`.
  - `lua` (346 KB), `hello`, `wc`, `grep` (musl regex), `sysinfo` (uses both ABIs) and `spin`;
  - Lua samples in `sdk/examples`.
- **Delivery:** `tools/mkprograms.py` bundles them into the kernel image (about 560 KB). The `programs` kernel task installs them into `/System/Programs` and `/Library/Examples/Lua`:
  - whenever a MintOS disk is mounted and `/System/Programs/.bundle-id` differs;
  - only through the MintFS file API.
- **Terminal** (`kernel/user/mintde/procterm.c`):
  - PATH lookup (`/System/Programs`, `~/Programs`) and explicit paths, with quoting;
  - the program's live output and its prompt shown with the user's typing after it;
  - Enter sends the line to stdin, Ctrl+C sends SIGINT, Ctrl+D ends input;
  - `&` starts a background job; `ps`, `kill`, `which` and `install-programs`;
  - exit statuses are reported.

### Window server (`docs/WINDOW-SERVER.md`)
The design for GUI apps as separate processes: shared reference-counted surfaces, message-framed pipes, MintDE proxy windows and a MintKit client library.

## Tests

- **`tools/test_app_platform.py`** (new host test; `make check` runs it through `check-app-platform`):
  - frame pool: chunking, more than 4096 pages, double free, chunk return, reserve;
  - Linux table: numbers match the x86-64 table, sorted; clone, fork, execve and socket stay `ENOSYS`;
  - translation: open flags, path normalization, `struct stat` layout (144 bytes), `wait4` status words;
  - pipes: ring wrap-around, OFD reference counts and EOF, `dup` with the tty flag, directory handles;
  - FPU clean-state layout.
- **`tools/test_app_platform_contracts.py`** (new, 20 static contracts):
  - integer-only kernel and MintDE;
  - CR4/XCR0 setup and save/restore placement;
  - preemption only from CPL3;
  - kill path, syscall range, RSP0 switch before the first push, SFMASK;
  - loader rejects interpreters and W+X; W^X in `mmap`; 64 MiB default;
  - task and process limits; installs only through MintFS;
  - SDK flags, licences, docs and evidence.
- **Updated tests:**
  - `tools/test_address_space.c`: lazy stack with guard region, demand population through kernel copies, carve/split, PROT_NONE, per-space limit;
  - `tools/verify_address_space.py`: PTE-tracked ownership and demand regions;
  - `tools/verify_scheduler.py`: `MINT_TASK_MAX 32u`;
  - `tools/test_user_syscalls.c` and `tools/test_syscall_dispatch.c`: stubs for the new calls `syscall.c` makes. The macOS `dynamic_lookup` wrapper had turned the missing symbols into a segfault.
- **Boot self-test:** two copies of `kernel/user/tests/fputest.c` (built with SSE) do the following, then compare:
  - fill XMM0-15 and the YMM0-7 upper halves with different patterns;
  - yield 40 times and spin between yields so the timer preempts them;
  - compute a double series and check MXCSR.
  - COM1: `[fpu] TWO-PROCESS FPU STATE ISOLATION SELF-TEST PASSED preemptions=…`.
- **Results on macOS:** all `make check` steps pass when run one by one. The exception is `tools/test_context_switch.py`, which is Linux-only and fails as before.

## QEMU evidence (q35, OVMF, `-cpu max`, 1 GiB)

`tools/qemu_app_platform.py SCRIPT` drives a headless run: HMP 45463, QMP 45473, a typed-text map and PNG screenshots. `tools/mkmintos_disk.c` creates a pre-initialized MintOS scratch disk with the real kernel initializer, so runs do not need Disk Utility clicks. The disk is always a new file.

| File | What it shows |
|---|---|
| `test-evidence/qemu-26.5.014.0-app-platform-boot-check.png` | MintDE boots normally with all changes; FPU self-test passed with AVX, 368 preemptions. |
| `…-terminal-hello.png` | `hello one "two words"` (argv, HOME, floats, malloc) and `sysinfo` (AVX, XSAVE 832 B, preemptions, pool usage, 0 unsupported Linux calls). |
| `…-lua-repl.png` | Lua REPL: float arithmetic, `//`, `math.floor`, `0.1+0.2`, `string.format`, `upper`, `gsub`, tables; Ctrl+D exits. |
| `…-lua-script.png` | `calc.lua` written with `echo` onto MintOS HD and run (sorting floats, sum/avg, `%08.3f`); `/Library/Examples/Lua/hello.lua`; `primes.lua 200000` (17984 primes in 0.21 s). |
| `…-two-programs.png` | `spin 12 &` keeps printing while `hello fail` exits with **status 3** and `sysinfo` runs; three processes alive. |
| `…-ctrlc-tools.png` | Ctrl+C interrupts a CPU-bound `spin` (killed with SIGINT, exit reported); `wc` on two files; `grep -n`; `files.lua` writes and reads `~/Documents`; `ps`. |
| `…-com1.txt` | COM1 excerpt: FPU init, self-test, Linux entry, pool, program install, process starts and exits with peak pages, kill. |

## Known limits

- **No fork/exec/threads/signal handlers.** Programs are started by Terminal (the parent is MintDE). A program cannot start another program yet; `posix_spawn` and `os.execute` fail.
- **Fixed terminal model:**
  - Terminal reports `isatty`, 80x24 and cooked line input; editing is local to Terminal (Backspace, Up-arrow recall);
  - full-screen programs and raw mode are not supported;
  - one foreground program at a time, plus up to 4 background jobs.
- **Kill permissions:** a process may kill only itself and its descendants; there are no users yet.
- **10 ms granularity:** blocking calls (pipe read and write, wait, sleep) poll in 1-tick sleeps, not wait queues, so latency is about 10 ms.
- **No file-backed memory:** `mmap` with a file and `mremap` are not supported; `mprotect(PROT_NONE)` discards page contents.
- **Weak `getrandom`:** it is not cryptographic. MintDE's TLS keeps using its own RDSEED/RDRAND path.
- **Static-PIE untested:** the loader path for static-PIE (`ET_DYN`) exists, but no test covers it. Linux binaries linked at 0x400000 cannot load, because the user window starts at 64 GiB.
- **Bundle size:** the bundle adds about 0.6 MB to the kernel image, which is now 15.6 MB of the 32 MiB load span.
- **Not validated** on VirtualBox or the OptiPlex 7040.

## Notes for merging

- **One-line hooks in shared files:**
  - `Makefile` gets one `include mk/app-platform.mk` line; every rule, object, MintDE object and test lives in `mk/app-platform.mk` and `sdk/sdk.mk`;
  - `syscall.c` gets the include, the 0x4D60 range and the kill check;
  - `kernel.c` gets includes, the pool backend, FPU/Linux init, self-test tasks and the programs task;
  - `apps.c` gets the include, the fallback command, key routing, prompt painting, the background pump and one help line;
  - `arch.S`, `arch.c`, `log.h` (`MINT_LOG_COMPONENT_PROC = 13`), `log.c` and `mintos_disk.c` (new `mint_mfs_fstat` at the end).
- **`address_space.h`:** the `MINT_ASPACE_MAX_USER_FRAMES` lines use exactly the text sent to web-engine (`app-platform-shared-edits.md` in the shared notes folder).
- **Struct change:** `MINT_ADDRESS_SPACE` no longer has `frames[]`. Any other branch that reads `space->frames` needs the pending-list or PTE-bit equivalent; none did at the fork point.
- **For web-engine:** if it raises MintDE's memory, it benefits from the frame pool after the merge. Without the pool, physmem's 4096 records cap all user memory together at about 14 MiB.
