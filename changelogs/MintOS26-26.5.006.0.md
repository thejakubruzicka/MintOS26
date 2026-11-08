# MintOS26 26.5.006.0 — E1000 Loopback Diagnostics

- Terminal commands now appear with a distinct `root#mintos >` prompt in both the on-screen transcript and COM1 log.
- Fixed release labels and retained every prior versioned changelog.
- Added bounded MDIC transactions for the Intel 82540EM integrated PHY.
- `net test` now configures/restores PHY and MAC loopback modes and differentiates PHY, TX timeout, RX timeout, invalid descriptor and frame mismatch failures.
- COM1 diagnostics capture device status/control, queue heads/tails, descriptor completion state and PHY/MDIC registers before restoring normal operation.
- Driver remains opt-in: boot discovers E1000 without enabling bus-master DMA; `net start` is explicit.
- This release does not add TCP/IP or external packet transmission.
- E1000 partial initialization clears PCI bus mastering before releasing DMA memory; if hardware ownership cannot be confirmed disabled, pages are retained and retry is refused.
- `net status` and `net test` show the stage code and relevant register/descriptor snapshot on screen.
- A successful `net start` only proves initialization; internal loopback is still experimental and must be verified in VirtualBox.
