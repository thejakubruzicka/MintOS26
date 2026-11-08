# MintOS26 26.5.008.0 / MintDE 1.0.7 — Test Guide

## Host build and test

```sh
make clean all
make check
make preview
```

Host tests cover source contracts, UEFI/ISO/GPT construction, ELF permissions, memory management, scheduler/syscalls, UI/service contracts, FAT16 parser/formatter behavior, multiple mounted volumes, generic FAT16 browsing without MintOS seed files, RGBA bitmap scaling/alpha blending, VFS mount-table safety, per-device block-I/O counters and VDI/raw payload verification. They do not emulate live AHCI DMA, guest pointer rendering or live menu interaction.

## UEFI boot selector

1. Use a disposable VM copy and snapshot. Keep **Enable EFI** active.
2. Attach `MintOS26-26.5.008.0-UEFI.iso` to the optical drive.
3. Start the VM. Press **1** or **Enter** for UEFI Desktop; press **2** or **M** to request MintDE. No input selects UEFI Desktop after three seconds.
4. MintDE exits UEFI boot services and the handoff is one-way until reset. If it fails, reset the VM; do not remove the UEFI Desktop recovery route.

## MintDE UI test

1. Test the mouse cursor moving slowly and quickly over wallpaper and translucent panels; look for trails/duplicate cursors.
2. Test each menu-bar dropdown, hover highlight, clicking outside a menu, the dock and Launchpad.
3. Open About MintOS26 and About MintDE. Verify the displayed versions, then expand and hide each changelog.
4. Open Settings and visit General, Appearance, Displays, Input, Storage and Privacy. Test an adjustable row and resize a normal window using its bottom-right grip.
5. Open Files; switch between Session (RAM) and every mounted developer volume. Test Up navigation, folders and bounded text previews.
6. Open Terminal and test `help`, `ver`, `uptime`, `status`, `drives`, `volumes`, `mounts`, `pci`, `drivers`, `io`, `logs`, `cpu`, `use sda`, `pwd`, `ls`, `cd`, and `cat`. For the experimental E1000 driver, run `net status` first; boot only detects the device and leaves bus mastering disabled. Run `net start` explicitly to initialize the adapter, then `net test` for the VirtualBox-specific TCVR internal-loopback diagnostic. The test does not intentionally send packets onto the NAT network. `resources` displays read-only PCI BAR and IRQ metadata. This terminal is a bounded diagnostic shell, not an unrestricted host-style shell or arbitrary hardware-port interface.

## Storage mount-only test

Attach the populated `MintOS26-26.5.008.0-SATA-data-512MiB-VDI-dynamic.vdi` as a separate SATA data disk. Storage & Logs should show the correct 512 MiB capacity, MBR signature and FAT16 read-only mount if the guest read path succeeds. Files should list it alongside every other validated FAT16 volume. No general file writes or persistent Notes/Settings are supported.

## Disk Utility destructive test

1. Take a fresh VM snapshot and configure COM1 as **Raw File** logging.
2. Disconnect existing data disks from the disposable test VM. Keep the ISO optical drive attached. Attach **only** `MintOS26-26.5.008.0-SATA-blank-512MiB-VDI-dynamic.vdi` as SATA data.
3. Boot UEFI Desktop and launch MintDE. Open Disk Utility and verify the selected disk's model, serial and capacity.
4. Enter the exact confirmation phrase shown by the app (for example `FORMAT sda`) only after verifying that the selected device is the blank disposable disk. The operation destroys data on that disk.
5. Save the complete COM1 log and screenshot. The formatter writes an MBR FAT16 partition and checks it by read-back and a read-only mount. If any operation reports an error, preserve the first failure and later records; do not infer success from a later unrelated mount message.

## Multiple-volume test

Attach one populated developer VDI and one blank VDI. The populated volume should mount read-only while the blank one is rejected. Then, in a disposable snapshot, format the blank volume and refresh Storage & Logs and Files. Confirm both mounted volumes appear separately, and verify the volume selector switches directory listings correctly.

## Runtime boundaries

VirtualBox/VBoxManage and QEMU/OVMF are not installed in the build environment. Report VM validation separately from host tests. The E1000 driver is opt-in and has not been validated live here; it provides bounded polling rings and a VirtualBox-specific TCVR internal-loopback test, not a general packet interface. No TCP/IP, DHCP, DNS, sockets, functional GPU acceleration, USB mass storage, NVMe driver, ACPI power transitions, native audio or macOS `.kext` compatibility is claimed from PCI discovery alone.


## E1000 loopback diagnostics in 26.5.008.0
1. Use a snapshot with EFI enabled and COM1 Raw File logging.
2. In MintDE Terminal run `net status`, `net start`, `net test`, `net status`.
3. A failed loopback should identify link-reset, TX descriptor, RX descriptor or frame-mismatch stage. COM1 also records TX command/length/buffer, DMA ring bases/lengths, PCI command, MMIO and PHY/MDIC snapshot. The test uses VirtualBox-specific RCTL.LBM_TCVR internal loopback and does not configure PHY loopback. This branch is for the emulated 82540EM, not claimed to work on arbitrary physical NICs.
4. No external packets are sent; a loopback pass does not prove NAT or TCP/IP functionality.

## E1000 loopback diagnosis (26.5.008.0)

With a VM snapshot and COM1 Raw File logging enabled, boot MintDE, then type `net status`, `net start`, `net test`, and `net status` at the visible `root#mintos >` prompt. Initialization is opt-in; boot only detects the PCI function. On failure, the COM1 report should include a stage number plus STATUS/CTRL/RCTL/TCTL, TX/RX head/tail, descriptor state, MDIC and PHY control/status captured before restoration. The Terminal repeats key registers. Attach the entire COM1 log; a stage timeout is diagnostic evidence, not a successful network test. This path is internal loopback only and is not TCP/IP.
