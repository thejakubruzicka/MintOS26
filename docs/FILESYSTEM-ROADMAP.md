# MintOS26 filesystem roadmap

## Current contract in 26.5.013.0

- **MintOS HD:** a MintFS volume on a GPT disk that MintOS initialized itself, mounted read/write as `/`. MintFS is copy-on-write and checksummed, with alternating superblocks, so a power cut leaves the last commit. It is described in `kernel/include/mintfs.h` and `WORKLOG.md`.
- **Write safety:**
  - Writes reach the disk only through a block-layer grant that covers the MintOS partition.
  - Initialization needs a blank disk, a matching serial and size, and a typed confirmation.
  - Every other disk, FAT16 included, stays read-only.
- **Tests:**
  - On the host: `tools/test_mintfs.c` (a torture test with power cuts), `tools/test_mintos_disk.c` (block grants and the file API), `tools/test_gpt.py` and `tools/test_mintfs_contracts.py`.
  - In QEMU: `tools/run_qemu.sh --mintos-disk` attaches a persistent scratch disk. `tools/mintfs_tool check <image>` checks it from the host.

## Previous contract (26.5.008.0)

- `/session` is volatile RAMFS.
- FAT16 volumes are registered separately in the versioned VFS mount table as `/Volumes/<device>` and remain read-only.
- The AHCI block registry rejects generic media writes. Disk Utility's confirmed blank-media formatter remains a narrow, separate path.
- Block reads are single-sector and bounded; block statistics count generic requests. The formatter's private writes are reflected in the storage log, not the generic-write counters.

## Planned sequence

1. Add a common filesystem operations contract for path lookup, directory iteration, file read, file write, flush and unmount; keep volume identity independent of UI rows.
2. Add read-only ext2 metadata/directory support as a testbed for a second filesystem implementation; do not advertise ext4 support until extents, checksums, 64-bit feature flags and journaling requirements are handled.
3. ~~Add a write-capable block-device policy that requires explicit per-device opt-in, bounded range checks, flush ordering and host-side fault injection.~~ Done in 26.5.013.0: block-layer write grants plus MintFS. Boot and system disks stay read-only.
4. Add FAT16 file writes only after transaction boundaries, allocation metadata consistency, reboot verification and fault-injection tests exist.
5. Consider ext4 read-only support first. Writable ext4 requires journal replay/recovery, barriers, checksums, inode/extents, feature negotiation and crash testing; it is not a drop-in FAT16 replacement.

## Acceptance gates

- All mounts have stable device identifiers and independent error state.
- Directory iteration on one volume cannot change another volume's selection.
- Any write failure leaves a detectable error and does not mark the mount persistent.
- Reboot tests verify contents survive only after read-after-write, flush, and reset checks in a real VM.
