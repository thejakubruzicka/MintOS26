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

## Phase B — platform for real software (prerequisite for goals 1 and 3)

- **FPU/SSE/AVX** in user space: XSAVE state switching in the scheduler. Most real software needs floating point.
- **Separate app processes:** each app runs as its own process that draws into a shared surface MintDE composites. This needs IPC and a small window-server protocol, so one crashing app cannot take down the desktop.
- **C library:** a port of musl (MIT) on top of POSIX system calls (files via the VFS, time, memory mapping, then threads).
- **First ports:** Lua, SQLite, QuickJS, a C compiler (TinyCC) and a game (Doom).
- **Later:** SMP, so all four cores of the i5-6700 are used.

## Phase C — full web page rendering (goal 1)

- **NetSurf:** port it (GPL-2), with HTML 4/5 parsing, CSS 2.1 layout and PNG/JPEG/GIF/WebP images. It fetches through MintOS's own HTTP and TLS stack and draws into a MintDE window. Its framebuffer front end is the starting point.
- **Before the port is ready:** images in the current Web app via stb_image (public domain), gzip, and basic CSS (`display:none`, colours, font sizes).
- **Honest limit:** document-style sites (Wikipedia, news, blogs, docs) will render properly. JavaScript-heavy web apps (YouTube, Gmail, Figma) need a modern JavaScript engine and DOM, a much later step (QuickJS bindings).
- **Also needed:** TLS 1.2 and P-256 key exchange for the remaining HTTPS servers.

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
