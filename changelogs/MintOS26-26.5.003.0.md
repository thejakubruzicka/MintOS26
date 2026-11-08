# MintOS26 26.5.003.0

- Added a versioned VFS mount-table ABI: volatile `/session` and separate read-only `/Volumes/sda`, `/Volumes/sdb`, etc.
- Added generic block-I/O telemetry for read successes/failures and denied write calls.
- Added ATA READ/WRITE DMA EXT and FLUSH CACHE EXT command selection for LBA48-capable devices, while preserving the block registry's read-only policy.
- Added bounded, pointer-validated PNG/RGBA bitmap drawing for ring-3 desktop assets.
- Added terminal commands: `pci`, `drivers`, `mounts`, `io`, `logs`, `cpu`, `use sda`, `pwd`, `cd`, and volume-aware `ls`/`cat`.
- Fixed Settings content spill and added mouse-selectable dropdowns with keyboard selection.
- Improved the software pointer shape and damage redraw region; added per-app PNG assets and subtle translucent backdrop sampling.
- Corrected read-only mount counts in Storage & Logs diagnostics.
- Generic file writes and persistence remain disabled.
