# MintOS26 26.5.010.0 — Shared display canvas for MintDE 2

- **Shared canvas display model.** New syscalls:
  - `MINT_SYS_DISPLAY_MAP_CANVAS` (0x4D27) maps three kernel-owned, per-mode buffers into the desktop process as write-back, non-executable memory: the presented canvas, a wallpaper cache and a compositor scratch buffer.
  - `MINT_SYS_DISPLAY_PRESENT` (0x4D28) copies a damaged rectangle to scanout, converting to RGB GOP order when needed.
  - `MINT_SYS_DISPLAY_CURSOR` (0x4D29) draws the anti-aliased pointer as a kernel overlay, so pointer motion never requires a repaint.
  - `MINT_SYS_DISPLAY_WALLPAPER_RENDER` (0x4D2A) renders the light or dark wallpaper, bilinearly scaled, into the wallpaper buffer.
  - After the canvas is mapped, the legacy per-primitive frame API fails closed.
- **Borrowed mappings:** `mint_address_space_map_shared` maps a physically contiguous, kernel-owned range as a SHARED region. It is never released by `unmap` or `destroy`, refuses to alias frames the space owns, and rolls back if it fails partway.
- **Process memory:** the per-process owned-page limit rises from 256 to 2048 pages (8 MiB), so MintDE can carry its font and icon resources.
- **Wall clock:** the CMOS RTC is read once at boot (bounded update-in-progress wait, BCD/binary and 12/24-hour aware). `clock_gettime(CLOCK_REALTIME)` now returns the RTC time plus uptime, or ENODEV if the RTC was invalid. No timezone is applied.
- **Input:** the PS/2 driver tracks the Windows/GUI key as a modifier and reports its press and release, which MintDE uses as Command.
- **FAT16:** a bounded prefix read (`MINT_FAT16_READ_PREFIX` flag) for file previews. Whole-file reads still refuse files larger than the buffer.
- **Boot loader:** the UEFI loader accepts kernel images up to 24 MiB, since the kernel now embeds both 1080p wallpapers. The 32 MiB load-span limit is unchanged.
- **Build:** the Makefile now lists the address-space and process headers as dependencies of every object that embeds those structures. A stale object caused a struct-layout mismatch during development.
- **Tests:**
  - New `tools/test_mintde2.py` covers the renderer, clipping, blur bounds, corner restoration, text, canvas present/RGB conversion, overlay pointer and RTC date math.
  - The address-space test gains shared-mapping cases.
  - The syscall test harnesses gain stubs for every kernel service `syscall.c` calls, so they also link on Linux.
- **Validation:** QEMU 11.1 q35/OVMF on macOS. Not yet validated in VirtualBox or on physical hardware.
