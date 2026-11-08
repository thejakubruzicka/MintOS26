# MintOS26 26.5.008.0 — MintDE 1.0.7

**MintOS26** is the operating system and kernel. **MintDE — Mint Desktop Environment** is its native ring-3 desktop environment, independently versioned as **MintDE 1.0.7**. The firmware-mediated desktop is referred to as **UEFI Desktop**.

The startup selector defaults to UEFI Desktop (Enter/1 or a three-second timeout). Press 2/M to request the experimental MintDE native handoff. MintDE calls `ExitBootServices`; this handoff is one-way until reset, so keep EFI enabled and retain UEFI Desktop as the recovery path.

## Release highlights

- MintDE begins on a clean workspace; Launchpad remains in the dock. The About MintOS26 and About MintDE screens are separate, and each has a changelog button hidden until requested.
- Rounded alpha-blended surfaces with backdrop sampling/specular edges, per-app PNG icon assets, functional mouse-driven Settings dropdowns and menu dropdowns, a software arrow cursor without the duplicate kernel overlay, and an initial resize-grip implementation.
- Settings includes General, Appearance, Displays, Input, Storage and Privacy categories.
- Files shows Session (RAM) separately from every registered read-only FAT16 volume, with explicit volume selection, directory navigation and bounded text preview.
- Standard FAT16 volumes can be mounted without MintOS-specific seed files. A versioned VFS mount table publishes `/session` and independent `/Volumes/<device>` mount paths; up to eight mounted volumes can be registered for Files. The block layer now tracks per-device generic read results and denied write attempts.
- System Monitor, Storage & Logs, Hardware & Drivers and a bounded diagnostic Terminal provide honest service/hardware status. Terminal exposes `pci`, `resources`, `drivers`, `mounts`, `io`, `logs`, `cpu`, `use sda/sdb`, and volume-aware `ls`/`cat`. The experimental Intel 82540EM path is read-only-detected at boot; `net start` explicitly enables its bounded DMA driver, and `net test` now uses the TCVR internal loopback-buffer path required by VirtualBox's emulated 82540EM and captures the actual submitted TX descriptor plus RX descriptor state. Loopback completion remains pending live VM validation; no IP stack is included.
- AHCI uses READ/WRITE DMA EXT and FLUSH CACHE EXT when a device advertises LBA48. Disk Utility only initializes an explicitly confirmed blank disposable disk; generic block writes remain denied.

## Storage truth

A COM1 log from the 26.5.001.0 VM showed one valid 512 MiB FAT16 developer disk mounted read-only, with the developer ID and `55 AA` MBR signature, while a second detected disk returned repeatable zero-filled LBA 0 and was correctly rejected as blank. The same log contains a formatter error for one operation followed later by successful read-only mount entries. Treat these as separate events and preserve the full log when testing.

This release still has **read-only filesystem access and no general persistent file-write support**. Notes and Settings remain RAM/session-backed. The blank-disk initializer is deliberately narrow and destructive; use only a disposable data disk in a snapshot, after confirming model, serial and capacity. Mount success is not proof of durable writes.

## Build and host checks

Requirements: clang, `lld-link`, `ld.lld`, GNU make, Python 3, and Pillow for concept previews.

```sh
make clean all
make check
make preview
```

The build produces the EFI application, native kernel ELF, ring-3 programs, ESP, GPT UEFI disk, raw developer image, populated dynamic/fixed VDIs, blank dynamic VDI and UEFI ISO. Host tests include source checks, production parser/formatter tests, a two-volume/generic-FAT16 test, pixel-level alpha blending and artifact validation.

Host tests do not emulate live AHCI/E1000 DMA or prove MintDE runtime behavior. VirtualBox/VBoxManage and QEMU/OVMF are unavailable in the build environment, so the next guest test is still required.

## Next development work

1. Validate MintDE 1.0.7 on the VM at multiple display sizes, especially cursor stability, menu dismissal, resize behavior and both formatted and blank disks attached simultaneously.
2. Design bounded general file writes and persistence separately: allocation, directory updates, flush ordering, rollback and reboot verification must be addressed before enabling them.
3. Improve terminal input/console I/O, process blocking/wakeup and resource cleanup.
4. Expand VM hardware support incrementally, beginning with ACPI/PCI enumeration and carefully bounded VirtualBox drivers. A detected PCI function is not a driver.
5. Validate the opt-in E1000 init/loopback path on VirtualBox with COM1 logs before adding a packet-buffer service. Start MacBook-specific hardware work only after repeatable VM stability. Do not write to the MacBook's internal NVMe or unknown USB media.

## References

See `CHANGELOG.md`, `changelogs/`, `KERNEL-ROADMAP.md`, `docs/FILESYSTEM-ROADMAP.md`, `docs/DRIVER-ROADMAP.md`, `TESTING.md`, `STORAGE-FAILURE-EVIDENCE.md`, and `test-evidence/` for current status and prior logs. Keep the `.003.2` baseline and `.004.1` reference branch intact.


### E1000 loopback troubleshooting

Boot performs read-only E1000 detection only. In a disposable VirtualBox snapshot, run `net status`, `net start`, `net test`, then `net status`. If loopback fails, the new COM1 record includes the failing phase and a bounded snapshot of the E1000 MAC/PHY registers and descriptor completion bits. It still sends no external packets.

Terminal command history is displayed with `root#mintos >` and colored separately from command output.

### 26.5.008.0 E1000 test sequence

MintDE Terminal displays each entered line using the distinct `root#mintos >` prompt. For the opt-in Intel 82540EM diagnostic, use a disposable VirtualBox snapshot and run:

```text
root#mintos > net status
root#mintos > net start
root#mintos > net test
root#mintos > net status
```

Boot only performs read-only PCI detection. `net start` enables the experimental bounded-polling RX/TX rings and DMA; `net test` sends only an internal loopback probe. If it fails, preserve the COM1 `loopback FAIL stage=...` register snapshot and the Terminal `Loopback stage/error` output. Do not interpret host tests as proof of guest DMA. No TCP/IP stack or external packet-send service is included.
