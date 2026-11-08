# MintOS26 26.5.007.0 worklog

## User-provided VM evidence

The 26.5.005.0 COM1 log shows:
- `sda` and `sdb` identified and read-only FAT16 mount messages for both block indexes.
- Intel 82540EM (8086:100E) detected read-only at boot; bus-master DMA remains disabled until `net start`.
- `net start` initialized the polling RX/TX rings and reported MAC `08:00:27:11:F3:4F`.
- `net test` failed/timed out at loopback. The older log did not record the exact timeout phase.
- `use sda` and `use sdb` ran after the network test without another page fault.

The source log is preserved as `test-evidence/mint-26.5.005.0-e1000-loopback.log`.

## 26.5.007.0 changes

- Terminal presents command history with a distinct `root#mintos >` prefix and accent-colored command entries separated from output.
- E1000 performs bounded MDIC PHY register access and configures both integrated-PHY and MAC loopback for the internal probe.
- Loopback failure snapshots record STATUS/CTRL/RCTL/TCTL, TX/RX head-tail registers, descriptor status, MDIC and PHY control/status **before normal mode is restored**.
- `net status` and `net test` show stage/error and the register snapshot on screen as well as in COM1.
- Partial E1000 initialization now disables and verifies PCI bus mastering before freeing the DMA pool. If bus-master disable cannot be confirmed, the pool is retained and retries are refused.
- Boot remains detect-only; `net start` explicitly activates the experimental driver. It has no TCP/IP, DHCP, DNS or external packet-send path.

## Verification boundaries

- `make clean all` completed with exit status 0; final documentation changes were then rebuilt into the ISO/data images by `make all`, exit status 0.
- Host suite runs were split because the monolithic `make check` exceeded the shell command's 300-second execution limit. The core batch printed 538 PASS / 0 FAIL. The final storage/artifact batch printed 186 PASS / 0 FAIL and repeated the UEFI verifier; this corresponds to 715 unique PASS assertions, no reported test failures.
- The E1000 core/driver, stack-usage, kernel, userspace, UEFI, storage, VDI, architecture-hardening and VMALLOC verifiers passed in the recorded batch logs.
- No VirtualBox/VBoxManage or QEMU/OVMF runtime was available in the build environment. Therefore the revised E1000 PHY/MAC loopback has **not** been proven live; the next VM log should report the exact failure stage and pre-restore registers if it still fails.


## 26.5.007.0 E1000 follow-up
- The supplied 26.5.006.0 COM1 snapshot shows TDH/TDT 0/1, TX descriptor status 0, CTRL.LRST bit set and STATUS.LU clear at TX timeout.
- The 26.5.007.0 loopback path no longer combines MII PHY loopback with RCTL MAC loopback; it clears and verifies CTRL.LRST and leaves PHY configuration untouched.
- Added TX command/length/buffer and DMA ring/PCI command diagnostics. Runtime loopback success still requires a fresh VirtualBox log.


## 26.5.008.0 / MintDE 1.0.7 follow-up

- User's 26.5.007.0 COM1 trace showed TX descriptor completion with TDH/TDT=1/1 and TX DD=1, while RX remained empty (RDH/RDT=0/7; RX DD=0).
- Updated the VirtualBox-emulated 82540EM self-test from RCTL.LBM_MAC to RCTL.LBM_TCVR to use the emulator's internal loopback-buffer route.
- Corrected failure diagnostics to retain the actual submitted TX descriptor index and capture RX descriptor index, length, error flags and DMA buffer address.
- Fixed hit testing for the compact eight-version changelog list.
- Host tests can check the bitfield selection and diagnostics contract, but they cannot establish live VirtualBox DMA behavior.
