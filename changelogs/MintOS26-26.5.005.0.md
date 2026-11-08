# MintOS26 26.5.005.0 — VirtualBox E1000 driver foundation

- Added read-only boot detection and an opt-in Intel 82540EM (8086:100E) PCI driver with checked BAR0 discovery, an uncached MMIO window and a bounded DMA allocation below 4 GiB. Boot does not enable bus mastering; the user must run Terminal `net start`.
- Added fixed-size RX/TX descriptor rings, serialized polling, frame-size checks, transfer counters and an opt-in internal MAC-loopback diagnostic.
- Added `net` / `net start` / `net status` / `net test` terminal diagnostics and a user-pointer-validated status/test syscall contract.
- Added physical allocator API for contiguous allocations constrained below a maximum address.
- Added tests for ring wrapping, DMA-window overflow, explicit startup syscall and E1000 initialization/timeout contracts.
- Fixed and retained the terminal `use sda` / `use sdb` stack-overflow fix and per-process ring-3 fault containment.

Not included: TCP/IP, DHCP, DNS, sockets, external packet transmission, interrupt-driven networking, persistent file writes, ext4, or a live VM validation claim.
