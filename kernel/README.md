# MintOS26 Native Kernel — 26.5.003.0 / MintDE 1.0.2

MintOS26 is the operating system/kernel release (`26.5.003.0`). MintDE - Mint Desktop Environment is the distinct native desktop release (`1.0.0`). The UEFI Desktop remains the default and recovery environment. A startup selector offers an opt-in MintDE handoff; the three-second timeout selects UEFI Desktop. `BOOTMINTDE` and `BOOTKERNEL` are equivalent UEFI Terminal commands. Kernel handoff calls `ExitBootServices` and is effectively one-way until the VM is reset.

## Kernel services

The kernel sets up a private GDT/TSS, IDT and exception stacks, paging with NX/W^X, guard pages, physical page allocator, kernel heap/vmalloc, isolated user roots, ELF64 ring-3 loading, validated syscall ABI, cooperative round-robin scheduler, user runtime, retained-frame display/input services, PS/2 input, PCI inventory, structured COM1 logging, bounded AHCI diagnostics, read-only FAT16 mount/file reads, and a tightly scoped blank-disk initializer available through Disk Utility.

## Storage

A user-provided 26.4.004.4 guest log records a blank-disk initialization request and then `FAT16 volume verified and mounted read-only after disk operation`. This is evidence of a successful read-only mount in that specific run. It is not proof that general file writes, Notes/Settings persistence or crash-safe transactions work.

Generic file writes remain disabled. The Disk Utility formatter is destructive and should only be used on a positively identified blank disposable VDI in a VM snapshot after confirming model, serial and capacity. The formatted filesystem is validated and mounted read-only.

## Driver boundaries

MintOS26 uses freestanding native drivers rather than macOS `.kext` binaries. PCI enumeration or a detected device ID does not mean functional support. VMware SVGA acceleration, E1000 packet I/O/TCP/IP, USB mass storage, native NVMe, native audio, ACPI power transitions and hotplug remain future work. Live AHCI/DMA, selector, cursor and window interaction tests require VirtualBox runtime validation; host tests alone cannot prove them.
