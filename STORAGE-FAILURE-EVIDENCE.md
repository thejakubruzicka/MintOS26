# Storage Failure Evidence Preserved for 26.4.004.2.1

The original uploaded COM1 logs and `.004.1` build-check log are copied verbatim under `docs/evidence/` in the source package. The originals under `/mnt/data` were not modified. These excerpts describe the observed outcomes, not a root-cause claim.

## `.004.1` run from `docs/evidence/mint.log`

The log begins with a firmware boot-entry failure:

```text
BdsDxe: failed to load Boot0002 "UEFI VBOX HARDDISK VBe6506ec5-76be4052 " from PciRoot(0x0)/Pci(0x1F,0x1)/Ata(Primary,Slave,0x0): Not Found
```

Later the native kernel reports:

```text
MintOS26 26.4.004.1 native kernel entry
[block] registered AHCI port=0 sectors=52428800 writable=yes
[storage] AHCI devices present; no recognized MintOS26 FAT16 volume found
[settings] no persistent volume; using session defaults
[vfs] RAMFS overlay online files=3 max-files=8 bytes-per-file=4096; volatile across reboot
```

`52,428,800 * 512 = 26,843,545,600` bytes = **25 GiB**. This is not the intended 512 MiB developer disk. The firmware `Boot0002` message is preserved as boot context; by itself it does not establish the cause of the later AHCI enumeration mismatch.

## `.004.1` run from `docs/evidence/mint_285_29.log`

```text
MintOS26 26.4.004.1 native kernel entry
[block] registered AHCI port=0 sectors=1050112 writable=yes
[storage] AHCI devices present; no recognized MintOS26 FAT16 volume found
[settings] no persistent volume; using session defaults
[vfs] RAMFS overlay online files=3 max-files=8 bytes-per-file=4096; volatile across reboot
```

Expected sectors for an exact 512 MiB image: `536,870,912 / 512 = 1,048,576`. Observed `1,050,112` is **1,536 sectors (786,432 bytes) larger**. It is not a correct capacity report for the intended image.

## Earlier branches

- `docs/evidence/mint_287_29.log` is from `.003.2`; it shows the baseline native desktop and session RAMFS behavior, not a persistent native filesystem mount.
- `docs/evidence/mint_283_29.log` is an older `.3.005.4` native kernel run and is preserved as earlier diagnostic context.
- `docs/evidence/MintOS26-26.4.004.1-build-check.log` is the supplied source/build check log; static checks alone do not prove AHCI/DMA execution.

## How `.004.2` changes the evidence collected

The diagnostics release logs each discovered controller BDF/BAR, implemented port bitmap, inactive port status (SSTS/signature), IDENTIFY result, raw LBA28 and LBA48 capacity fields, the selected count/source, sector-zero result, all primary MBR entries, the reason for each supported partition's FAT16 validation result, and truthful `read-only`, `NOT MOUNTED`, and persistence-unavailable states. No media writes are attempted. This should distinguish wrong disk/controller/port selection from an IDENTIFY capacity parsing issue or a later MBR/BPB validation failure, but a fresh VirtualBox run remains necessary.


## New runtime evidence supplied with 26.4.004.2.1

The user supplied `mint(1).log` and three Storage & Logs screenshots; verbatim copies are preserved in `docs/evidence/`. The `.004.2` runtime log records:

```text
[ahci] controller vendor=0x0000000000008086 device=0x0000000000002829 BAR5=0x00000000E1624000 PI=0x0000000000000000
[log][INFO][AHCI][#3] SATA link online with ATA device signature; starting bounded IDENTIFY probe
[log][WARN][PARTITION][#4] failed at MBR/boot signature validation (expected 0x55AA)
[block] sda controller-index=0 port=0 sectors=1048576 bytes=536870912 capacity-source=LBA48 read-only=yes
[identify] sda lba28_raw=1048576 lba48_supported=yes lba48_raw=1048576 selected_sectors=1048576
[device] sda model=VBOX HARDDISK serial=VBafb808d0-79406223 ...
  read: OK: IDENTIFY and LBA 0 read
  table: bad MBR signature
  filesystem: failed at MBR/boot signature validation (expected 0x55AA)
  mount: NOT MOUNTED - diagnostics-only release; persistence is unavailable
  write: DISABLED - read-only diagnostics; no media writes attempted
```

The export view later reports the controller's PI register as `0x1`; the initial `[ahci] ... PI=0` text was emitted before the register was read and was stale diagnostic output. `.004.2.1` logs PI only after the MMIO read. The authoritative validation failure is **LBA 0 MBR signature**, before partition-entry parsing or any FAT16 BPB checks. The UI's `read: OK` proves the command completed according to the driver status, not that the returned bytes were correct.

The generated `.004.2` raw image has `0x55AA` at bytes 510–511, one MBR partition (type `0x06`, start LBA 2048, length 1,046,528 sectors), and a valid MintOS26 FAT16 boot sector; the `.004.2` VDI payload was verified byte-identical to that raw image. The VM-reported serial `VBafb808d0-79406223` differs from the default UUID-derived serial of the generated `.004.2` VDI (`af026d82-e442-4432-a87b-daad568731f6` would normally be presented as `VBaf026d82-f6318756`). VirtualBox does support custom serial overrides, so check for an override; absent one, the mismatch suggests the VM was attached to a different/old disk or restored a snapshot with different media. It is not conclusive evidence by itself that DMA is faulty.

The `.004.2.1` raw image carries disk identity marker `0x26300421` in the standard MBR disk-signature field at bytes 440–443. The kernel now logs LBA 0 signature bytes, CRC32, repeat-read stability, and this marker. If the correctly attached image produces two differing LBA 0 reads, investigate AHCI/DMA/buffer integrity. If it produces stable `00 00` bytes and the marker is absent, verify the actual attached image and snapshot before altering filesystem validation. Media writes and mounting remain disabled.

## 26.4.004.3 follow-up: attached VDI was blank

The user then uploaded `MintOS 26.vdi`. Direct inspection of that exact file showed:

- Physical file length: **2,097,152 bytes** (header plus dynamic block map; no payload blocks).
- Virtual capacity: **536,870,912 bytes** (512 MiB), sector size 512 bytes.
- VDI version/type: VirtualBox VDI 1.1, dynamic image.
- Block size/count: 1 MiB / 512 blocks; block map starts at 1 MiB and data offset is 2 MiB.
- All 512 block map entries were `0xFFFFFFFF` (unallocated), and the file ended exactly at the declared data offset.
- SHA-256: `cacca1b92e3249bfaab4be29b35e14c4123cf9e3c2ad4cb12037a503015b43b2`. The copied template in the working tree had the same digest; neither was edited by the seeding tool.

The next user COM1 log confirmed stable all-zero data at LBA 0 (`signature=0x0000`, `disk-id=0x00000000`, CRC32 `0xB2AA7578`, `repeat=MATCH`). A blank dynamic VDI naturally reads as zero-filled sectors until blocks are allocated. This resolves why that particular attached medium had no MBR; it does not prove all AHCI/DMA implementations are correct.

### 26.4.004.3 response

- A new output VDI is created from a copy of the blank image's VDI format metadata. It does not overwrite `/mnt/data/MintOS 26.vdi`.
- The new VDI allocates all 512 block-map entries and stores the generated FAT16 image after the VDI data offset. It has a different creation UUID.
- A host verifier confirms VDI dynamic type, contiguous map entries, 512 MiB virtual capacity, valid LBA 0 MBR signature `55 AA`, disk ID `0x26300421`, and SHA-256 equality between the VDI payload and raw `.img`.
- A separate host test calls the production block/FAT16 code on the generated raw image. It verifies a read-only mount at partition LBA 2048, reads both Notes and Settings seed files, rejects a write through the read-only block API, and checks the raw source image remains unchanged.

The production kernel now performs the same read-only mount/file-read verification after the guest AHCI path registers a disk. **This is still only host-validated.** No VirtualBox/QEMU runtime is available in this build environment. The remaining runtime test is to attach the *new populated dynamic VDI* as a separate SATA data disk, run `BOOTKERNEL`, then preserve the guest COM1 output and check for a `0x55AA` signature and read-only mount confirmation. Do not enable persistence or media writes until that log is correct.

## 26.4.004.3 follow-up: two blank SATA disks from the user's VM

The newly supplied `docs/evidence/mint_0043-two-blank-vdi-runtime.log` and VirtualBox settings screenshot are preserved with the release source. The guest enumerates two ports (`PI=0x3`): `sda` has `1,048,576` sectors (512 MiB) and `sdb` has `1,048,719` sectors (about 512.07 MiB). Both reads of each disk's LBA 0 are stable, both return signature `00 00`, both have CRC32 `0xB2AA7578`, and both report disk ID zero / unknown developer marker. Thus the observed media contents are blank at sector 0 on both devices. The second disk's 143 extra sectors should be retained as an exact IDENTIFY capacity difference, not rounded away in the driver report.

The current `.004.4` response adds **Disk Utility** for deliberately confirmed initialization of a blank disposable disk. The formatter does not require pre-existing FAT16; it writes one MBR/FAT16 partition on the selected blank volume and validates the resulting volume through the production read-only parser. Before any write, it requires stable zero LBA 0, scans sectors 1–33 for conventional primary metadata and scans the last 33 sectors for backup metadata (including standard GPT headers/entry arrays), then repeats those tests just before the first write. A GPT-like fixture with a zero LBA 0 and `EFI PART` at either LBA 1 or the final LBA is refused before any media writes in host tests.

The disk preparation operation is destructive and capped at 2 GiB. It remains explicitly user-confirmed and is not a general write API; formatted media is mounted read-only afterwards. Live AHCI `WRITE DMA`/flush/read-back in VirtualBox is still **not validated** in this environment. A successful host fake-device format does not establish real guest DMA write correctness. If formatting fails in the VM, preserve COM1 and the exact error, do not bypass guard checks, and do not test on internal NVMe or unknown USB media.

## 26.4.004.4 interpretation and new safeguards

The latest supplied `.004.3` guest log enumerates two SATA devices (`PI=0x3`): `sda` at 1,048,576 sectors and `sdb` at 1,048,719 sectors. Both IDENTIFY probes complete. Both two-pass LBA 0 checks return the same all-zero sector and CRC32 `0xB2AA7578`, with zero MBR disk ID. Because the uploaded and VM-created test VDIs were blank dynamic disks, this result is consistent with blank media; it does not itself prove read DMA is faulty. A valid blank disk is expected to be unpartitioned/unformatted.

The appropriate next experiment is to format only a new disposable blank data VDI with the `.004.4` Disk Utility, or alternatively to attach the pre-populated `.004.4` developer VDI for a read-only mount test. The two tests answer different questions: the populated image exercises guest reads and FAT16 mount; the blank image exercises the guest write/flush/read-back path. Keep these images distinct and never format the pre-populated image.

### `.004.4` device-index safety fix

Storage diagnostic entries now carry the actual block registry index and registration state. Initial read-only mount and the formatter resolve the selected device through that explicit mapping, rather than assuming the snapshot row index equals the block-device index. This guards multi-port scenarios in which an earlier device fails IDENTIFY/registration. Host source checks and the full suite pass, but this mapping still needs live VM validation.

The `.004.4` Disk Utility checks LBA 0 and repeated-read stability, requires blank metadata in both front and end regions, rechecks immediately before writing, writes an MBR/FAT16 volume and commits the MBR last, then attempts read-back and read-only validation. General writes remain disabled. If the VM write path reports any error, stop, preserve the COM1 log, and do not retry on a disk containing anything valuable.

## Follow-up evidence supplied by the user after 26.4.004.4

The full guest COM1 log `test-evidence/mint-26.4.004.4-user-com1-disk-utility-mount-success.log` includes the earlier probe of two blank disks (all-zero LBA 0, repeat reads MATCH), followed later by:

- `[log][WARN][PARTITION][#8] explicitly confirmed blank developer disk initialization requested`
- `[log][INFO][PARTITION][#9] FAT16 volume verified and mounted read-only after disk operation`
- `[log][INFO][PARTITION][#10] developer disk initialized; FAT16 read-only validation completed`

This is direct guest evidence that the Disk Utility formatting path proceeded to a verified **read-only FAT16 mount** in that run. It does not prove arbitrary file writes, Notes/Settings persistence, failure-safe filesystem updates, or crash consistency. The user reports no visible runtime issues in this run. The next release keeps the generic media/file write path disabled and adds a safe-default boot selector for an opt-in MintDE 1.0 test; the UEFI Desktop remains the recovery path.

## User VM evidence received after MintOS26 26.5.001.0

Source file retained verbatim at `docs/evidence/mint_26.5.001.0-user-vm-com1.log`.

- `sda` reports 1,048,576 sectors (512 MiB), LBA0 signature bytes `55 AA`, repeated reads `MATCH`, developer disk ID `0x26300421`, marker `MATCH`, and FAT16 BPB/cluster geometry validation passed. The log then says the read-only mount is verified and the Notes/Settings seed files are readable.
- `sdb` reports 1,048,719 sectors and repeatable zero-filled LBA0 (`CRC32=0xB2AA7578`), so it is correctly not recognized as a partitioned MintOS/FAT16 volume before formatting.
- The same log contains a Disk Utility formatter `ERROR` followed by a read-only FAT16 mount message and then a later separately confirmed initialization/mount success. These are separate events. The generic recheck did not identify which target had just been formatted, so the mount message after the error was ambiguous and must not be treated as proof that the failed formatting operation succeeded.

In 26.5.002.0, the recheck messages now name the block index for each read-only revalidation, and the format syscall logs the requested target block index on both failure and success. A valid FAT16 filesystem can be mounted for browsing even if MintOS-specific Notes/Settings seed files are absent. All general filesystem writes and persistent settings/notes writes remain disabled.

## Later user-provided evidence: MintOS26 26.5.002.0

The attached `test-evidence/mint-26.5.002.0-user-com1-two-volume-readonly-mount.log` records two detected SATA devices. Both report the MBR signature bytes `55 AA`, the developer disk marker, successful FAT16 BPB/cluster-geometry validation and read-only mount success messages for block indices 0 and 1. The final diagnostic line still says `readonly-mounted=1`; in that version this was a boolean active flag rather than an actual count. MintOS26 26.5.004.0 adds `readonly_mount_count` and a versioned VFS mount-table snapshot so UI/terminal output can distinguish mount count from a boolean state.

The same log confirms `writes-disabled=yes`; it does not demonstrate general file writes, persistence across reboot, or current-release runtime behavior. Separate historical error and later success records are preserved as separate events.
