# MintOS26 26.5.009.0 / MintDE 1.0.8 worklog

Previous worklog: `docs/WORKLOG-26.5.008.0.md`.

## Baseline reproduction (26.5.008.0, unmodified)

- The source archive matched the handoff SHA-256 (`f48b5d27…2983`). It is imported verbatim as git tag `v26.5.008.0` on `main`; this work is on branch `dev/26.5.009.0`.
- Host: macOS 27 on Apple Silicon, Apple clang 21, Homebrew lld/qemu 11.1.3/binutils. `make clean all` exited 0 with no warnings.
- `make check` run per command with the `tools/macos-host` wrapper: 41/43 commands passed (714 PASS lines). `test_context_switch.py` and `test_paging.py` cannot run on a Mach-O host (ELF assembly, undefined linker-script symbols); they are not failures of MintOS code.
- **First live VM run of 26.5.008.0** (QEMU q35 + OVMF, TCG): UEFI Desktop and native MintDE both boot; ring-3 smoke tests pass. Evidence: `test-evidence/qemu-26.5.008.0-*`.
- E1000 on QEMU: TX descriptor completes; with the corrected diagnostics the submitted frame length is 0x3C. RX times out under RCTL.LBM_TCVR. QEMU's e1000 loops back only through PHY BMCR loopback, so this neither confirms nor refutes the VirtualBox TCVR hypothesis.

## Defects found and fixed

1. **PS/2 pointer direction inversion.** Deltas were read as `int8_t`, ignoring the 9-bit sign in byte 0. Motion above 127 counts per packet reversed direction. Reproduced in QEMU, where host moves are coalesced; real mice do the same on fast flicks. Fixed, with host tests for ±200-count packets.
2. **Kernel halt on large or padded GOP modes.** The backbuffers were static 1920x1080 arrays, and `stride*height` above that failed surface init ("invalid/unsupported framebuffer surface; halted"). They are now allocated per mode from high memory, up to 4096x2400.
3. **Uncached framebuffer.** It is now WC via PAT PA4, with exact-range remapping; large-page splits keep the PAT bit (otherwise a WC leaf would silently become WB).
4. **No power-off/restart path** on the native kernel. ACPI S5 and reset were added.
5. **Dell "RAID On" SATA invisible** (class 01:04). Intel RAID-mode HBAs are now accepted after AHCI register validation.

## Verification for 26.5.009.0

- Host (macOS, `tools/macos-host` wrapper): 44/45 `make check` commands pass with 820 PASS lines; only `test_context_switch.py` (ELF assembly, Linux-host only) cannot run. `test_paging.py` now names linker-script symbols portably, so it runs on macOS, and gained a PAT-split regression check. Summary: `test-evidence/host-check-macos-26.5.009.0-summary.txt`.
- Host: new `tools/test_acpi.py` (synthetic RSDP/XSDT/RSDT/FADT/DSDT/SSDT: Dell-style SLP_TYP 7, SSDT fallback, multi-byte PkgLength, bad checksums, MethodOp decoy, oversized lengths) and `tools/test_hardware_compat.py` (display surface with 1080p, padded 2048-stride 1080p, 1440p, 4K, 1366x768/1376 stride, 5K refusal; PAT/WC/RAID/PS2 source contracts). Updated `test_ps2_input.c` to spec-correct packets plus fast-motion cases.
- QEMU 11.1 q35/OVMF, native MintDE (COM1: `test-evidence/qemu-26.5.009.0-native-power-shutdown-com1.log`): `[display] framebuffer remapped write-combining via PAT`; 1920x1080 backbuffers attached at the top of RAM; `[acpi] … PM1a_CNT=0x604 S5=0/0 reset-reg 8042`. Menu → Shut Down… → confirm made QEMU exit (ACPI S5). Menu → Restart… → confirm rebooted to UEFI Desktop. Fast pointer moves kept their direction.
- **Not validated:** VirtualBox, the physical OptiPlex 7040 / GTX 1050 Ti, the Intel RAID-mode path (QEMU cannot present a class 01:04 Intel HBA), modes above 1080p in a live guest, and WC timing (TCG ignores memory types).

## Next

- **Phase 2 — USB input:** a polled xHCI driver (BIOS handoff via USBLEGSUP, bounded command/event rings) with HID boot keyboard/mouse and report-protocol tablet. This lets native MintDE run on USB-only machines (OptiPlex 7040) and with VirtualBox's default USB tablet. Test in QEMU with `-device qemu-xhci -device usb-kbd -device usb-tablet`.
- **Phase 3 — MintDE macOS-style rework:** multi-window z-order, traffic-light chrome, translucent menu bar with clock and status icons, dock indicators, Spotlight-style launcher, original wallpaper.
