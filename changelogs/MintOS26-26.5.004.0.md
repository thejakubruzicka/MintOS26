# MintOS26 26.5.004.0 — Driver foundation and user-fault containment

- Fixed terminal `use sda` / `use sdb` page fault caused by an approximately 9 KiB automatic storage snapshot on the four-page userspace stack. Scratch snapshots are now in static userspace memory.
- Added a process-contained CPL3 exception path. A fault logs vector/RIP/error/CR2, switches to the kernel page table, marks that task's process failed, and returns to the saved scheduler continuation. Ring-0 faults still panic as before.
- Added versioned PCI inventory of configured BAR and interrupt metadata, with pure decoders for I/O, 32-bit memory, 20-bit memory, and 64-bit memory BARs. No sizing writes are performed.
- Added the `resources` terminal command and expanded the driver status catalogue. The VMware SVGA, E1000, audio and USB entries remain explicitly “device found” until the functional driver exists.
- General media writes and persistent Notes/Settings remain disabled.
