# MintOS26 26.5.007.0 — E1000 TX timeout isolation

- Changed the opt-in E1000 loopback test to use the MAC loopback mode alone instead of combining MAC loopback with a separate MII PHY loopback setting.
- Cleared and verified `CTRL.LRST` after reset and before TX setup, then removed forced speed/duplex overrides during the MAC diagnostic so the PHY/autonegotiation state is not needlessly changed.
- Expanded failure snapshots: TX descriptor command, length, DMA buffer address, TX/RX ring bases and lengths, PCI command register, plus the existing CTRL/RCTL/TCTL, ring head/tail, descriptor status, MDIC and PHY state.
- Terminal `net test` keeps a distinct `root#mintos >` prompt and displays the new descriptor/resource diagnostics.
- E1000 is still opt-in (`net start`); external packet transmission, TCP/IP and successful live loopback have not yet been demonstrated. General storage writes remain disabled.
