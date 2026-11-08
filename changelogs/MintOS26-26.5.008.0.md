# MintOS26 26.5.008.0 — VirtualBox E1000 RX loopback diagnosis

- Updated the E1000 self-test to use the VirtualBox emulated 82540EM's TCVR loopback mode instead of MAC loopback routed through the virtual network backend.
- Corrected TX descriptor diagnostics to report the descriptor actually submitted, not the next ring slot after the head advances.
- Added RX descriptor index, length, error bits and DMA buffer address to failure evidence.
- Kept the E1000 driver opt-in; boot does read-only PCI discovery only, and no TCP/IP or external packet transmission is provided.
- Preserved earlier release changelogs.
