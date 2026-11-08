# MintDE 1.0.7 — VirtualBox E1000 loopback path

- Terminal uses the visible `root#mintos >` prompt and keeps commands distinct from output.
- `net test` selects the VirtualBox emulated 82540EM TCVR loopback path; this test is emulator-specific and does not claim physical NIC compatibility.
- E1000 diagnostics now select the submitted TX descriptor and capture RX descriptor status, length, errors and buffer DMA address.
- The driver remains opt-in and idle at boot. General networking, TCP/IP and external packet transmission are not implemented.
