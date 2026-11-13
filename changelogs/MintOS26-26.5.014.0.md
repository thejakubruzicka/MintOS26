# MintOS26 26.5.014.0 — Programs, saved settings, a real web engine and sound

This release was built in four parallel topics and merged together. Full details are in `docs/notes-26.5.014.0-{data-saving,app-platform,web-engine,personality}.md`.

- **Programs (Phase B):**
  - **FPU, SSE and AVX in ring 3:** per-task XSAVE state, with a two-process isolation self-test at boot.
  - **Preemption:** ring-3 code is preempted by the timer; the kernel itself stays non-preemptive.
  - **Memory:** a frame pool replaces the old limit of about 14 MiB for all user memory together. Processes now get demand-zero pages, an 8 MiB stack, `mmap`/`munmap`/`mprotect`/`brk`, and 64 MiB by default.
  - **Processes:** spawned from MintOS HD with argv, envp and auxv; exit statuses, wait, kill, pipes and descriptors.
  - **Linux-compatible subset:** a separate, explicit syscall entry (`syscall` instruction) for the subset that musl needs. It is not general Linux compatibility.
  - **SDK:** musl 1.2.5 as the C library (`sdk/`, `docs/SDK.md`).
  - **Bundled programs:** Lua 5.4 and the tools `hello`, `wc`, `grep`, `sysinfo` and `spin`, installed into `/System/Programs` at mount. Terminal runs them with live output, typed input, Ctrl+C and background jobs.
- **Data saving:**
  - **Preferences store:** `~/Library/Preferences/com.mintos.<domain>.prefs` and `/Library/Preferences`, saved atomically and debounced.
  - **Accounts:** `/Library/Preferences/com.mintos.accounts` holds the account. The kernel takes the home folder from it at mount (`MINT_SYS_MFS_SET_HOME`, 0x4D50).
  - **Safe power-off:** Shut Down and Restart commit MintFS and flush the disk cache first.
- **Web engine:**
  - **Engine:** a new HTML/CSS engine (`kernel/user/web/`) with a bounded heap, gzip/deflate, HTTP/1.1 (chunked, keep-alive), cookies, an HTML5-style tree builder, a CSS cascade, and block, inline, float, table and flex-row layout.
  - **Images:** PNG, JPEG, GIF and BMP through stb_image, with every floating-point path off.
  - **TLS:** TLS 1.2 with ECDHE (X25519/P-256) and AEAD suites, and P-256 for TLS 1.3 too.
- **Sound and boot:**
  - **Audio driver:** Intel HD Audio with a widget-graph walk to the output and a mixer driven by the timer interrupt. It starts automatically in VMs; on real hardware (the OptiPlex's ALC3234) it is experimental.
  - **Original MintOS sounds** generated at build time, about 1.1 MiB.
  - **A mint-leaf boot animation** in the kernel, which crossfades into the desktop.
- **Syscalls:**
  - 0x4D50 home folder;
  - 0x4D58–0x4D5E audio, boot splash and system status;
  - 0x4D60–0x4D7F processes and memory.
  - Log components 12 PREFS, 13 PROC and 14 AUDIO.
- **Tests:**
  - New: `test_data_saving.c` (49 checks), `test_app_platform.py`, `test_web_engine.py` (140), `test_tls.py` (39 local checks, now with TLS 1.2), plus the HDA, mixer, sounds, splash, wallpapers and notification tests, and contracts for every topic.
  - macOS host: every `make check` command passes except the Linux-only `test_context_switch.py`.
- **Validation:** a combined pass in QEMU 11.1 q35/OVMF with ich9-intel-hda:
  - setup and data: Setup Assistant on a blank disk, account `alexmint`, Dark with the purple accent, Notes;
  - programs and web: Lua and the tools in Terminal, Wikipedia over HTTPS with images;
  - after a restart: the Aurora wallpaper, the note in Notification Center and the account all came back. The startup chime was captured on both boots, and the host fsck reports the disk clean.
  - VirtualBox and the physical OptiPlex 7040 have not been validated yet.
