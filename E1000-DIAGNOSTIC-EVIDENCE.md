# E1000 Diagnostic Evidence — MintOS26 26.5.007.0

## Observed in the user's 26.5.006.0 VM log

The Intel 82540EM at PCI 00:03.0 was found by read-only PCI scan. `net start` initialized bounded RX/TX DMA rings below 4 GiB and left interrupts masked. `net test` then failed at stage 3, before receiving a frame.

The decisive saved snapshot was:

- `STATUS=0x81`; the link-up status bit was not asserted in this snapshot.
- `CTRL=0x1A49`; bit `CTRL.LRST` (`0x08`) was set.
- `TDH/TDT=0/1`, `TXD/RXD=0/0`.
- The log reported `TX descriptor completion timed out`.
- `net status`, `net start`, and `net test` ran without a kernel panic; `use sda` and `use sdb` also remained usable.

The TX descriptor's source command already requested EOP, IFCS and RS status write-back, so absence of RS was not supported as the cause. The old runtime log did not include descriptor command, length, buffer address, PCI command, or ring DMA base registers.

## Changes in 26.5.007.0 / MintDE 1.0.6

- Controller initialization clears `CTRL.LRST` after reset and verifies that the bit remains clear.
- The opt-in test now selects MAC loopback through `RCTL.LBM_MAC` only. It no longer simultaneously switches the integrated PHY into MII loopback or forces the PHY to a separate speed/duplex configuration.
- Loopback saves the original MAC CTRL/RCTL state and restores it after the probe.
- Failure diagnostics now capture TX descriptor command/status, length, DMA buffer address, TDBAL/TDBAH/TDLEN, RDBAL/RDBAH/RDLEN and PCI command register, in addition to status/control, ring head/tail, MDIC and PHY snapshots. Capture occurs before restoring normal MAC settings.
- DMA memory is not returned to the allocator on a failed start unless PCI bus mastering is confirmed disabled.

## What is verified

The clean host build and full host test suite passed for this source snapshot. The host tests inspect the descriptor, register, and cleanup contracts, plus verify the release images. They do not run an E1000 model or emulate live DMA.

**Live E1000 TX completion is not yet confirmed.** The new ISO must be tested in the disposable VirtualBox VM with COM1 set to Raw File. If `net test` still times out, its new snapshot should identify `TXCMD/LEN/BUF`, ring base/length, PCI command and the current CTRL/STATUS values so the next change can target the actual failing stage.


## Follow-up from 26.5.007.0 runtime log

The guest log showed the RX test timing out, but TX had completed: TDH/TDT was 1/1, TX descriptor DD was set, while RDH/RDT remained 0/7 and RX DD was clear. The emitted TX buffer/length fields described descriptor 1 (the next ring slot) rather than submitted descriptor 0 because diagnostics read the current head after it had advanced.

The 26.5.008.0 change selects `RCTL.LBM_TCVR` (`0xC0` in the LBM field) for the VirtualBox-emulated 82540EM internal loopback path and snapshots the actual submitted TX descriptor. This is emulator-specific. Host-side tests do not prove that VirtualBox DMA/RX now succeeds; keep the driver opt-in and attach the next complete COM1 log.
