# MintOS26 26.5.010.0 / MintDE 2.0.0 worklog

Previous worklog: `docs/WORKLOG-26.5.009.0.md`.

## Goal

Make MintDE look and behave as close to macOS as possible, using only original artwork and openly licensed resources.

## Design decisions

- **Rendering moved to ring 3.** The old desktop issued one syscall per rectangle or text run and supported a single window. MintDE 2 draws into a kernel-owned canvas mapped into its address space (`MINT_SYS_DISPLAY_MAP_CANVAS`) and presents only damaged rectangles. The pointer is a kernel overlay composited at present and move time, so moving the mouse costs no repaint.
- **Borrowed mappings, not owned frames.** The per-process page limit was 256 pages (1 MiB). Screen buffers (8–38 MB each) are therefore mapped as a SHARED region that teardown never frees. The owned-page limit was raised to 2048 (8 MiB) for code, fonts and icons only.
- **Glass without feedback.** Frosted panels (menu bar, Dock, menus, Spotlight, Control Center, alerts) blur the freshly composed scene. The render pass first grows the damage area to include each panel's 2×radius sampling margin, so old glass never blurs into itself. The blur writes back only inside the rounded shape and the clip.
- **Rounded windows with square app content.** The four corner squares behind a window are saved before it is drawn and blended back afterwards using rounded-rectangle coverage.
- **Fonts and assets.** Inter 4.1 (SIL OFL) is pre-rendered at 11–44 px into 8-bit coverage tables (`tools/make_fonts.py`), with gamma 0.82 for 1× displays. Wallpapers, icons and pointers are drawn procedurally by committed scripts. The previous photographic wallpaper, which resembled Apple's High Sierra image, was removed.
- **Logic kept, presentation rewritten.** Storage diagnostics, the Terminal command set, Files navigation, Notes and the Disk Utility safety checks were extracted unchanged from MintDE 1.0.8 into `mintde/apps.c`. Every view was redrawn.

## Problems found during the work

- **UEFI loader rejected the kernel.** It read at most 8 MiB of `KERNEL.ELF`, and the kernel with two embedded 1080p wallpapers is 14.3 MB, so the read was truncated. The cap was raised to 24 MiB; the 32 MiB load-span limit is unchanged.
- **Stale object file.** After the page limit changed, `process.obj` was not rebuilt because the Makefile lacked header dependencies. The two struct layouts disagreed and wiped `MINT_PROCESS.pid`, so ring-3 smoke tests failed. Dependencies were added.
- **Latent Linux-host test break since 26.5.009.0.** The syscall test harnesses did not stub the ACPI power functions; macOS's dynamic lookup hid this. Stubs were added for all new kernel services, and a strict link now leaves only the intentionally weak wallpaper symbols unresolved.

## Verification

- **Host (macOS, `tools/macos-host`):** all `make check` commands pass except `test_context_switch.py` (ELF assembly, Linux host only). New: `tools/test_mintde2.py` (37 checks), address-space shared-mapping cases, and REALTIME clock checks.
- **QEMU 11.1 q35/OVMF (TCG):** native MintDE 2 boots, `[display] shared canvas mapped`, and the RTC clock shows in the menu bar. Exercised:
  - The Dock, its tooltips and the Files window: volumes, folder navigation and file preview.
  - Terminal (`power`, `drives`), and Settings: Appearance → Dark switches the theme and wallpaper.
  - The leaf menu with hover and System Settings…, Spotlight ("ter" → Terminal), Launchpad and Control Center.
  - The About window, Calculator (12×3 = 36), Notes typing and ⌘W, Disk Utility, and Storage & Logs.
  - Window drag, minimize with ⌘M, and ⌘Tab.
- **Not validated:** VirtualBox; the physical OptiPlex 7040 / GTX 1050 Ti; modes above 1080p in a live guest; real-hardware performance (QEMU here is software-emulated).

## Next

- Phase 2: polled xHCI + USB HID (keyboard, mouse, tablet) so native MintDE works with USB-only input.
- PS/2 IntelliMouse wheel → scrolling in lists (`app_scroll` is already wired).
- Desktop icons for mounted volumes; window animations (minimize "genie" is out of scope without timers).
