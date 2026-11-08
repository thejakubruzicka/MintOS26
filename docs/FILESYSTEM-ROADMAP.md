# MintOS26 filesystem roadmap

## Current contract in 26.5.008.0

- `/session` is volatile RAMFS.
- FAT16 volumes are registered separately in the versioned VFS mount table as `/Volumes/<device>` and remain read-only.
- The AHCI block registry rejects generic media writes. Disk Utility's confirmed blank-media formatter remains a narrow, separate path.
- Block reads are single-sector and bounded; block statistics count generic requests. The formatter's private writes are reflected in the storage log, not the generic-write counters.

## Planned sequence

1. Add a common filesystem operations contract for path lookup, directory iteration, file read, file write, flush and unmount; keep volume identity independent of UI rows.
2. Add read-only ext2 metadata/directory support as a testbed for a second filesystem implementation; do not advertise ext4 support until extents, checksums, 64-bit feature flags and journaling requirements are handled.
3. Add a write-capable block-device policy that requires explicit per-device opt-in, bounded range checks, flush ordering and host-side fault injection. Keep all boot/system disks read-only.
4. Add FAT16 file writes only after transaction boundaries, allocation metadata consistency, reboot verification and fault-injection tests exist.
5. Consider ext4 read-only support first. Writable ext4 requires journal replay/recovery, barriers, checksums, inode/extents, feature negotiation and crash testing; it is not a drop-in FAT16 replacement.

## Acceptance gates

- All mounts have stable device identifiers and independent error state.
- Directory iteration on one volume cannot change another volume's selection.
- Any write failure leaves a detectable error and does not mark the mount persistent.
- Reboot tests verify contents survive only after read-after-write, flush, and reset checks in a real VM.
