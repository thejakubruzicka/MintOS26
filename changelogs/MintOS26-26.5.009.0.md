# MintOS26 26.5.009.0 — Real-hardware display, input, power and SATA fixes

- **Display modes:** the native kernel no longer halts when the GOP mode is larger than 1920x1080 or uses a padded stride. The retained backbuffer and glass backdrop are now sized from the real mode (stride x height, up to 4096x2400) and claimed from the top of physical memory, keeping low memory free for 32-bit DMA. The kernel image's data/BSS footprint dropped from 17.7 MB to 1.1 MB.
- **Write-combining framebuffer:** the PAT is programmed as a complete table (PA4 = WC) after a CPUID check and cache flushes. The framebuffer range is then remapped write-combining at exact 4 KiB edges, so neighbouring MMIO stays uncached. Presentation copies aligned 64-bit pairs and is fenced with `sfence`. This mainly helps real PCIe GPUs such as a GeForce GTX 1050 Ti GOP framebuffer.
- **PS/2 mouse:** movement deltas are now decoded as 9-bit two's complement using the byte-0 sign bits. Before, any motion beyond ±127 counts per packet reversed direction, which made the pointer jump or vanish on fast moves. Overflow bits are handled like Linux psmouse. A fixed-point acceleration curve replaces the constant 4x: 2 px/count when slow, 4 px/count when normal (unchanged VirtualBox feel), up to 7 px/count when fast, with sub-pixel carry.
- **ACPI power management:** new `kernel/power/acpi.c` validates RSDP/XSDT/RSDT/FADT/DSDT/SSDT (signature, bounded length, checksum) and reads the `\_S5_` package.
  - Shut down enables ACPI mode through SMI_CMD with a bounded wait, clears WAK_STS and writes SLP_TYP|SLP_EN.
  - Restart tries the FADT reset register, then 0xCF9, then the 8042 pulse, then a triple fault. Known VM power-off ports are only tried under a hypervisor.
  - Two new syscalls: `MINT_SYS_POWER_INFO` (0x4D25) and `MINT_SYS_POWER_CONTROL` (0x4D26, requires a confirmation token).
  - There is no AML interpreter, so `_PTS` is not run.
- **Intel RAID-mode SATA:** Intel class 01:04:00 controllers, such as Dell's factory "RAID On" setting, are accepted when their ABAR validates as AHCI (version register and ports-implemented) before bus mastering is enabled. Member disks are read individually; RAID volumes are not assembled, and storage stays read-only.
- **Tooling:**
  - `tools/run_qemu.sh` boots the GPT image under QEMU/OVMF on Linux or macOS (Homebrew), with AHCI disks, an e1000 NIC and COM1 written to a file.
  - `tools/macos-host/` holds a clang wrapper so host tests run on Apple Silicon through Rosetta.
- **Validation:** host tests pass (see TESTING.md). On QEMU 11.1 q35/OVMF, native MintDE boots; ACPI S5 shut down and restart both worked; the pointer kept its direction on fast moves. Not yet run on a physical OptiPlex 7040 or in VirtualBox.
