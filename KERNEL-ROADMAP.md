# MintOS26 Kernel Roadmap — 26.5.011.0 / MintDE 2.1.0

## Current status

- **Boot:** UEFI Desktop remains the default and recovery path. An explicit selector enables the experimental MintDE path; `ExitBootServices` makes it one-way until VM reset.
- **Kernel core:** own GDT/TSS and IDT/exception handling; paging/NX, guarded stacks, physical page allocation, heap/vmalloc, isolated address spaces, ring-3 ELF loader, validated syscalls, cooperative scheduler, C runtime, framebuffer/event services and PS/2 keyboard/mouse.
- **MintDE:** menu dropdowns, rounded alpha-blended UI panels, retained dock and Launchpad, custom software cursor, Settings categories, separate About pages with optional changelogs, Files volume selection, diagnostic Terminal, System Monitor, Storage & Logs, Disk Utility and Hardware & Drivers.
- **Storage:** logs from 26.5.001.0 confirm one MBR/FAT16 developer volume mounted read-only alongside a blank second SATA disk. The current kernel can register multiple read-only FAT16 volumes and browse generic FAT16 without seed-file dependencies. General writes and Notes/Settings persistence remain disabled.
- **Drivers:** PCI resource inventory, xHCI USB (HID only, no hubs) and an Intel E1000-family polling driver carrying the IPv4 stack. The VirtualBox loopback diagnostic still needs live validation. VMware SVGA acceleration, USB mass storage, native NVMe, native audio, ACPI power transitions and hotplug remain incomplete.

- **26.5.009.0 additions:** GOP modes up to 4096x2400 with padded strides (per-mode backbuffers from high memory), write-combined framebuffer via PAT, PS/2 9-bit delta fix and acceleration, ACPI S5 shutdown and FADT/0xCF9/8042 restart, Intel RAID-mode AHCI acceptance. Validated live on QEMU q35/OVMF; physical hardware and VirtualBox still pending.

- **26.5.011.0 additions:**
  - Polled xHCI driver with HID keyboards, mice and tablets, and device-neutral input injection.
  - Uncached MMIO window for 64-bit BARs above the identity map.
  - Intel E1000-family driver for 82540EM, 82545EM, 82543GC and 82574L, plus experimental I217/I218/I219.
  - IPv4 stack (ARP, ICMP, UDP, DHCP, DNS, TCP client) in a `net` polling task, with non-blocking network and TCP syscalls.
  - `MINT_TASK_MAX` raised to 12.

- **26.5.010.0 additions:** shared display canvas (map/present/overlay pointer/wallpaper syscalls), borrowed SHARED address-space mappings, 8 MiB per-process page limit, CMOS RTC wall clock, GUI/Command key modifier, FAT16 prefix reads. MintDE 2 (macOS-style shell) runs entirely in ring 3 on top of these.

## Next milestones

0. **TLS 1.3 client** (X25519, AES-GCM or ChaCha20-Poly1305, SHA-256, certificate parsing) so the Web app and `curl` can reach HTTPS sites; **USB hubs** and **USB mass storage (read-only)**; live validation of USB input and the experimental I219 on the OptiPlex 7040.

1. **MintDE 1.0.3 stabilization:** validate cursor stability, menu dismissal, alpha blend appearance, resize hit targets, Launchpad and multiple-volume Files behavior in VirtualBox at several screen resolutions.
2. **Prepare general file I/O:** define a narrow write API with explicit device selection, bounds and safe allocation/directory updates. Design flush ordering, error rollback and reboot-survival tests before enabling general writes. Keep Disk Utility isolated and destructive.
3. **Console and process services:** improve terminal output/keyboard editing, blocking/wakeup, child process cleanup and handle/resource accounting.
4. **Resizable windows and UI runtime:** formalize resize constraints, content re-layout, focus and multi-window state; avoid adding decoration without hit-test parity.
5. **E1000 validation:** use the new `net status`, `net start`, `net test`, `net status` workflow in a disposable VM, preserve COM1 stage/register snapshots, and fix the observed TX/RX completion failure before adding packet-buffer queues or TCP/IP.
6. **More VirtualBox drivers:** proceed only after repeated DMA reliability; likely next targets are 16550 serial RX, ACPI table diagnostics, PS/2 edge cases, and USB UHCI/OHCI/EHCI discovery.
7. **MacBook support:** only after repeatable VM stability; stage ACPI, USB/HID and NVMe development. Do not write to the MacBook internal NVMe or unknown USB media.

## Release/test policy

Preserve the `.003.2` baseline and `.004.1` reference. Package source, UEFI ISO, raw and VDI images, reports, test logs and checksums. Never present host tests as live VM evidence. VirtualBox/VBoxManage and QEMU/OVMF are not present in this build environment, so MintDE runtime and live AHCI behavior still require guest validation.
