# MintOS26 — next big steps (agreed 9 November 2026)

This file covers the goals set after 26.5.012.0. The decisions:
- **Disk writes:** writes are allowed only to disks that MintOS formatted itself.
- **Licensing:** GPL code may be used (NetSurf, Linux drivers). The combined OS is then distributed under the GPL, while MintOS's own files keep their MIT notice.
- **Software support:** it grows in stages, from POSIX ports to Linux binaries to a native SDK, with macOS compatibility researched last.

Done so far: **macOS-style sidebar vibrancy (goal 4)** and **Phase A (goal 5)** in 26.5.013.0. Window sidebars blur and tint whatever lies behind the window. Finder-style rows use a gray selection pill and accent line symbols, while System Settings keeps its coloured icons and accent pill.

## Phase A — a real read/write filesystem (goal 5) — done in 26.5.013.0

Delivered:
- **MintFS v1:** copy-on-write, CRC32C-checksummed and crash-safe through alternating superblocks, with fsck. A two-level inode map and pointer-based files were chosen over B-trees; see `WORKLOG.md`.
- **MintOS disks:** GPT plus a MintFS partition, created only on verified blank disks after typed confirmation. Writes go through a block-layer grant limited to that partition, and every other disk stays read-only.
- **AHCI:** 4 KiB transfers and FLUSH CACHE. Thirteen file syscalls, and an atomic save in the runtime.
- **Layout:** `/Applications`, `/Library`, `/System`, `/Volumes` and `/Users/Shared`, plus `/Users/mintos/{Desktop, Documents, Downloads, Movies, Music, Pictures}` and Trash.
- **MintDE:** Files works like Finder (Favorites, Locations, New Folder, Rename, Duplicate, Trash, dates). Notes saves real files and opens text files. Disk Utility can initialize a MintOS disk, and Terminal has file commands.

Left for later:
- **Files:** Recents, copying folders, drag and drop, and multiple selection.
- **Persistence:** Settings saved to `/Library/Preferences`.
- **Boot:** booting from a MintOS disk (MintOS on its own disk, with an installer).
- **Disks and access:** more than one MintOS disk at a time; users and permissions.
- **API:** POSIX names (`open`/`read`/…) come with the C library in Phase B.

## Phase B — platform for real software — mostly done in 26.5.014.0

Delivered:
- **FPU/SSE/AVX:** in ring 3, with eager per-task XSAVE.
- **Scheduling and memory:** timer preemption of programs, a frame pool, demand paging and `mmap`/`brk`.
- **Processes:** spawn, wait, kill and pipes.
- **C library and programs:** an explicit Linux-x86-64 syscall subset and the musl 1.2.5 SDK; Lua 5.4 and small tools run from Terminal.

Left for later:
- **Separate GUI app processes:** the window server, designed in `docs/WINDOW-SERVER.md`.
- **More ports:** SQLite, QuickJS, TinyCC and Doom.
- **Threads and SMP.**

## Phase C — full web page rendering (goal 1) — own engine in 26.5.014.0

Delivered (the user chose "own engine first"):
- **Engine:** an HTML/CSS engine with images (stb_image, with no floating point), gzip, cookies, forms, downloads and favorites.
- **TLS:** TLS 1.2, and P-256 for both TLS versions.
- **What renders:** Wikipedia, DuckDuckGo Lite and Hacker News render well. BBC News renders only partly.

Next:
- **Missing web features:** SVG/WebP images, CSS grid and positioned layouts, background images and web fonts.
- **NetSurf:** porting it (GPL-2) on the new musl SDK is still an option for full CSS 2.1.
- **Honest limit:** JavaScript-heavy sites need a JS engine and DOM (QuickJS bindings), a much later step.

## Personality (added November 2026) — done in 26.5.014.0
- **Sound:** an Intel HD Audio driver with original MintOS sounds.
- **Look and desktop:** the mint-leaf boot animation, procedural wallpapers with a dynamic Mint Day, and Notification Center with widgets and banners.
- **Your data:** a Setup Assistant and accounts, and desktop items.
- **Icons:** a refreshed set is planned for 26.6/26.7.

## Next release: MintOS 26.6 (planned by the user, 13 November 2026)

26.6 is the next big release. It focuses on four areas:
- **More file support:**
  - **Formats:** a Preview app for images (PNG/JPEG/GIF/BMP through the engine's decoders, and PDF later), a TextEdit-style editor, Quick Look in Files, and "Open With" file associations.
  - **Archives:** ZIP.
  - **Disks:** FAT32/exFAT and USB mass storage, read first, then writing on disks MintOS formatted.
- **Apps:**
  - **Separate GUI app processes:** the window server in `docs/WINDOW-SERVER.md`, so apps run and crash independently.
  - **SDK and ports:** the MintKit SDK for graphical apps, and more ports on the musl SDK (SQLite, QuickJS, TinyCC, Doom).
- **Graphics** (Phase D below): `virtio-gpu` and VMware SVGA II / VirtualBox VMSVGA for real modes and a hardware cursor, Intel HD 530 modesetting on the OptiPlex, and an SSE/AVX compositor.
- **Display scaling:** HiDPI rendering at 1×, 1.5× and 2×, with scaled fonts, icons and layouts, plus a Settings › Displays resolution and scale picker.

The icon refresh agreed for 26.6/26.7 fits here too.

**MintOS 27** will be a redesign of the whole OS. It is kept for the future and is not planned yet.

## Phase D — graphics (goal 2)

- **Virtual machines:**
  - `virtio-gpu` (QEMU; 2D, live resolution changes and a hardware cursor; virgl 3D later).
  - VMware SVGA II, which VirtualBox's VMSVGA and VMware both use.
- **OptiPlex 7040, Intel HD 530** (integrated graphics; use the motherboard's video ports): modesetting based on Intel's open documentation and the Linux i915 driver. That gives native resolution and a hardware cursor.
- **OptiPlex 7040, GTX 1050 Ti:** stays on the UEFI framebuffer. Even Linux's open nouveau driver cannot run Pascal cards at full speed (no reclocking, firmware signed by NVIDIA), so porting it would bring little.
- **Fastest real speed-up on any machine:** an SSE/AVX2 compositor spread across all cores, once Phase B allows it.

## Phase E — software compatibility in stages (goal 3)

1. POSIX ports (Phase B).
2. **Linux binaries:** run static Linux x86-64 command-line programs by translating Linux system calls.
3. **Native SDK ("MintKit"):** a documented toolkit for macOS-style apps as separate programs.
4. **macOS compatibility research:** a Darling-style Mach-O loader with libSystem shims, command-line tools first.
   - Graphical macOS apps would need an AppKit reimplementation. GNUstep (LGPL) covers AppKit at the *source* level, not for binaries.
   - Apple's own frameworks cannot be shipped with MintOS.

## Order

Phase A, then B, then C (alongside the first B ports), then D, then E2–E4. Each phase ships as a release with tests, a changelog and QEMU evidence. VirtualBox and the OptiPlex should be validated between phases.
