# MintOS26 26.5.003.0 / MintDE 1.0.2 worklog

## Implemented in this increment

- Fixed Settings content geometry so the session/status panel cannot extend below its window, added mouse-selectable dropdowns and keyboard option selection, and kept a bounded lower content margin.
- Added a software translucent surface sampler with restrained five-tap backdrop blur and specular edges. This is a software approximation; it is not GPU-accelerated macOS Liquid Glass.
- Added twelve individual PNG app icon files plus 96x96 straight-alpha RGBA payloads used by the native shell. The PNGs are shipped in both the EFI volume and developer data volume.
- Added a bounds-checked `MINT_SYS_DISPLAY_DRAW_BITMAP` ring-3 syscall. User memory is validated and copied row-by-row; pixel blend/scaling code clips to the active framebuffer.
- Refined cursor row masks and repaints the union of previous/current pointer rectangles to reduce stale cursor pixels at the dock.
- Changed About changelog navigation to `Changelog -> version -> release notes`; per-version history is retained in `changelogs/` and copied onto boot/developer volumes.
- Added a versioned VFS mount-table snapshot; `/session` is labelled volatile and each read-only FAT16 volume receives a separate `/Volumes/<device>` path tied to its actual block index.
- Added per-device generic block read counters and blocked-write counters, an userspace-readable syscall, and terminal `io`/`mounts`/`pci`/`drivers`/`logs` commands.
- Added LBA48 `READ DMA EXT`, `WRITE DMA EXT`, and `FLUSH CACHE EXT` selection when IDENTIFY advertises support. Generic block writes remain denied; the narrow blank-disk formatter remains the only explicit write path.
- Added a filesystem roadmap that keeps ext4 as a future target and describes the checks required before enabling journaling or general writes.

## Host validation

- Clean build exit status: 0.
- Full `make check` exit status: 0; 669 `PASS:` lines and 0 `FAIL:` lines.
- EFI volume and developer volume both contain the 12 separate PNG files and current filesystem/driver/changelog documents.
- VDI payloads match the raw developer image; the separate blank VDI has all 512 blocks unallocated.

## Runtime limits

No VirtualBox/VBoxManage or QEMU/OVMF executable is installed in the build environment. The latest guest log supplied by the user for 26.5.002.0 shows two FAT16 candidate/mount-success records with writes disabled; this is not a live 26.5.003.0 test. The new LBA48 path, pixel-perfect MintDE visuals, dropdown behavior and current ISO boot must be verified in a disposable VirtualBox snapshot.
