# MintOS26 Kernel Roadmap — 26.5.008.0 / MintDE 1.0.7

## Current status

- **Boot:** UEFI Desktop remains the default and recovery path. An explicit selector enables the experimental MintDE path; `ExitBootServices` makes it one-way until VM reset.
- **Kernel core:** own GDT/TSS and IDT/exception handling; paging/NX, guarded stacks, physical page allocation, heap/vmalloc, isolated address spaces, ring-3 ELF loader, validated syscalls, cooperative scheduler, C runtime, framebuffer/event services and PS/2 keyboard/mouse.
- **MintDE:** menu dropdowns, rounded alpha-blended UI panels, retained dock and Launchpad, custom software cursor, Settings categories, separate About pages with optional changelogs, Files volume selection, diagnostic Terminal, System Monitor, Storage & Logs, Disk Utility and Hardware & Drivers.
- **Storage:** logs from 26.5.001.0 confirm one MBR/FAT16 developer volume mounted read-only alongside a blank second SATA disk. The current kernel can register multiple read-only FAT16 volumes and browse generic FAT16 without seed-file dependencies. General writes and Notes/Settings persistence remain disabled.
- **Drivers:** PCI resource inventory plus an experimental opt-in Intel 82540EM/E1000 polling driver with bounded TX/RX rings and an internal loopback diagnostic. The actual VirtualBox loopback still needs live validation; no TCP/IP stack is present. VMware SVGA acceleration, USB mass storage, native NVMe, native audio, ACPI power transitions and hotplug remain incomplete.

## Next milestones

1. **MintDE 1.0.3 stabilization:** validate cursor stability, menu dismissal, alpha blend appearance, resize hit targets, Launchpad and multiple-volume Files behavior in VirtualBox at several screen resolutions.
2. **Prepare general file I/O:** define a narrow write API with explicit device selection, bounds and safe allocation/directory updates. Design flush ordering, error rollback and reboot-survival tests before enabling general writes. Keep Disk Utility isolated and destructive.
3. **Console and process services:** improve terminal output/keyboard editing, blocking/wakeup, child process cleanup and handle/resource accounting.
4. **Resizable windows and UI runtime:** formalize resize constraints, content re-layout, focus and multi-window state; avoid adding decoration without hit-test parity.
5. **E1000 validation:** use the new `net status`, `net start`, `net test`, `net status` workflow in a disposable VM, preserve COM1 stage/register snapshots, and fix the observed TX/RX completion failure before adding packet-buffer queues or TCP/IP.
6. **More VirtualBox drivers:** proceed only after repeated DMA reliability; likely next targets are 16550 serial RX, ACPI table diagnostics, PS/2 edge cases, and USB UHCI/OHCI/EHCI discovery.
7. **MacBook support:** only after repeatable VM stability; stage ACPI, USB/HID and NVMe development. Do not write to the MacBook internal NVMe or unknown USB media.

## Release/test policy

Preserve the `.003.2` baseline and `.004.1` reference. Package source, UEFI ISO, raw and VDI images, reports, test logs and checksums. Never present host tests as live VM evidence. VirtualBox/VBoxManage and QEMU/OVMF are not present in this build environment, so MintDE runtime and live AHCI behavior still require guest validation.
