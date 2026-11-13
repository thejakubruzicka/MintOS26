# MintOS26 26.5.013.0 / MintDE 2.3.0 worklog

Previous worklog: `docs/WORKLOG-26.5.012.0.md`.

## Goal

Phase A of `docs/ROADMAP-NEXT.md`: a real read/write filesystem with a proper structure. The agreed rule is that MintOS writes only to disks it formatted itself, and the work is tested in VMs first.

## Design decisions

- **Copy-on-write rather than a journal.**
  - MintFS never overwrites a block that the committed state still references. A commit writes new blocks, relocates the dirty bitmap blocks, flushes, writes the next of two alternating superblocks, and flushes again.
  - Mount picks the newest superblock whose checksum is valid, so there is no replay step that could itself be interrupted.
  - Blocks freed in an open transaction stay reserved until it commits.
- **Checksums with context.** Every metadata block header carries a type tag, a CRC32C, its own block address, its owner and its generation. A misdirected or stale write is detected as reliably as bit rot.
- **Simple structures.**
  - A two-level radix inode map, inodes with inline data or direct, single-indirect and double-indirect pointers, and folders as variable-length record lists.
  - Mount builds the free-space bitmap into RAM.
  - The roadmap had mentioned B-trees. A flat layout was chosen instead because it is easier to verify exhaustively, and folders of a few thousand entries are fast enough at this scale.
- **Write safety at the block layer, not only in the filesystem.**
  - Every disk stays READONLY to `mint_block_write`.
  - MintFS gets a write grant for exactly its partition's LBA range, under its own owner tag. A bug in MintFS cannot write the GPT or another disk.
  - Initialization temporarily grants the whole disk to write the GPT, then revokes the grant before formatting.
- **Initialization only on blank disks.** It requires:
  - zeroed sectors 0, 1, 2 and 33, and n−33 and n−1;
  - at least 64 MiB;
  - the device's serial and size as Disk Utility displayed them;
  - the typed `FORMAT <device>` confirmation (the existing FAT16 formatter gate, now shared).
- **One MintOS disk at a time,** mounted as `/` ("MintOS HD"). The home folder is `/Users/mintos`.
- **Kernel-side descriptors, user-side conveniences.**
  - The kernel offers a small handle API: 32 handles, owned by a process and closed on exit.
  - The runtime builds whole-file load and an **atomic save** on top of it: it writes `.~name` next to the original, then renames over it. Notes, and anything else using `mint_file_save`, can never leave a half-written document.
  - Rename replaces its target in one MintFS commit.
- **Recursive delete lives in user space** (Empty Trash, `rm -r`). The kernel removes only empty folders, which keeps every kernel operation small and bounded.
- **Trash is a folder,** `~/.Trash`, as on macOS. The Dock Trash opens it.

## Problems found during the work

- **Deleting an inline file corrupted the disk** (found by the host torture test). Small files keep their bytes in the inode, in the same space the block-pointer table would use. `free_from()` read those bytes as pointers and freed live blocks. It now returns early for inline files. The fsck-after-every-commit run caught this within a few hundred operations.
- **The first crash simulation was too weak.** Cutting power only between whole commits never hit the interesting window. The RAM disk now has a volatile write cache, and a power cut persists a random subset of pending writes, some torn. A `crash_at` hook also freezes the disk at a chosen write inside a commit.
- **A per-index folder listing would be O(n²),** because each call reloaded the folder. `mintfs_list()` returns a batch from a single load. The torture test cross-checks it against `mintfs_readdir`.
- **The new syscall names collided with the session RAMFS calls** (`MINT_SYS_FS_*`, `MINT_FS_DIRENT`). The MintFS ABI uses the `MFS` prefix, and the kernel functions are `mint_mfs_*`.
- **macOS host tests hid a missing symbol.** The Rosetta wrapper links with `dynamic_lookup`, so the syscall harnesses linked without stubs for the new calls. They now have stubs, and `test_user_syscalls.c` exercises the new calls, so Linux CI keeps linking.
- **UI bugs found in QEMU:**
  - Rename appended typed text after the extension. Rename now has a real caret and selects the name without its extension, as Finder does.
  - Files kept a stale selection index after a refresh. It now reselects by name.
  - Disk Utility still offered Erase for the new MintOS disk. The kernel would refuse it anyway because the disk is not blank, but the button is now disabled.

## Validation

- **Host tests:**
  - `test_mintfs.c`: 32 checks. Seeds 1–23 all pass, about 10,000 random operations each, with fsck after every commit, 300 power cuts with torn writes, and crashes inside commits. The default seed takes about 20 s and is also clean under ASan and UBSan.
  - `test_mintos_disk.c`: 48 checks. It runs the real `block.c`, `mintos_disk.c` and `mintfs.c` on read-only-registered RAM disks and covers the blank-disk rules, the grant boundaries, untouched other disks and gaps, the file API, remount and fsck. It is clean under ASan and UBSan.
  - `test_gpt.py`: 48 checks against an independent struct/zlib parser, including the 63 GiB cap and corruption refusal.
  - `test_mintfs_contracts.py`: 26 checks.
  - `test_user_syscalls.py`: 15 new checks.
- **QEMU 11.1 q35/OVMF** with a blank, persistent 1 GiB SATA disk:
  - Disk Utility initialized it (GPT plus MintFS) and it mounted as MintOS HD.
  - In Files: Finder sidebar, New Folder, rename, Move to Trash with ⌘⌫, Empty Trash with ⇧⌘⌫ and its confirmation, and the Dock Trash.
  - Notes saved to `~/Documents/Notes.txt`, and double-clicking a text file opened it in Notes.
  - In Terminal: `ls -l`, `cd`, `mkdir`, `echo >`, `>>`, `cat`, `cp`, `mv`, `rm -r` and `df`.
  - Settings › Storage showed the volume.
  - **Hard power-offs** (QEMU killed): data written before the cut survived. In one test I typed, waited two seconds, typed more and cut power at once. The file held the committed text and not the last keystrokes, there was no leftover temporary file, and `tools/mintfs_tool check` on the host reported the volume clean (generation 25 after the session).
- **After the QEMU evidence run,** a one-line defensive guard was added: `mint_mfs_write` stops if MintFS ever reports zero progress. The host tests were rerun afterwards, but the QEMU screenshots predate it.
- **Not validated:** VirtualBox and the physical OptiPlex 7040. Do not initialize a disk on the OptiPlex unless it is a spare, blank disk.
