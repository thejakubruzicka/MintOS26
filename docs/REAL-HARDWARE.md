# Running MintOS26 on real hardware — Dell OptiPlex 7040 guide

Target machine: Dell OptiPlex 7040 (Intel Skylake, Q170 PCH), Core i5/i7-6xxx, NVIDIA GeForce GTX 1050 Ti, UEFI firmware.
Status: **26.5.010.0 (MintDE 2.0.0) is not yet validated on this machine.** This guide lists what is expected to work, the firmware settings to use, and how to capture evidence.

## What to expect

| Area | UEFI Desktop (default) | Native MintDE (opt-in, key 2/M) |
|---|---|---|
| Display (GTX 1050 Ti GOP) | Firmware GOP, 1920x1080 preferred | Same GOP framebuffer, write-combined via PAT; any mode up to 4096x2400 |
| USB keyboard/mouse | Firmware USB drivers | **Not yet.** Native input is PS/2 only (a USB driver is the next milestone); see below |
| PS/2 keyboard/mouse | Firmware | Works (IRQ1/IRQ12) |
| SATA disks | Firmware file-system access | Read-only AHCI; also in Dell "RAID On" mode (member disks individually) |
| M.2 NVMe SSD | Firmware | Not supported (no NVMe driver) |
| Network (Intel I219-LM) | Firmware HTTP only, if offered | Not supported (the driver covers the 82540EM only) |
| Shut Down / Restart | Firmware | ACPI S5 and FADT/0xCF9/8042 reset |
| Audio | None | None |

The GTX 1050 Ti is driven only through the UEFI GOP framebuffer the card's firmware sets up. There is no NVIDIA driver; resolution changes require a reboot through UEFI Desktop Settings.

## Firmware (BIOS) settings — press F2 at the Dell logo

1. **Boot Sequence → Boot List Option: UEFI.** Legacy/CSM boot cannot start MintOS26.
2. **Secure Boot → Disabled.** `BOOTX64.EFI` is not signed.
3. **Advanced Boot Options → Enable Legacy Option ROMs: off.** The GTX 1050 Ti then provides a UEFI GOP. If the screen stays black at the boot selector, check this first.
4. **System Configuration → SATA Operation: AHCI** (recommended). "RAID On" also works read-only in 26.5.009.0, but AHCI is the standard path. Changing this can stop an installed Windows from booting; note the current value before you change it.
5. **System Configuration → USB Configuration:** keep boot support enabled so the USB stick and the firmware's own USB keyboard work in UEFI Desktop.
6. Optional: if the machine has a **serial port**, it is COM1 at 0x3F8. Capture logs with a null-modem cable at 115200 8N1.

## Writing the boot USB stick

**This erases the USB stick.** Check the device name carefully. Never write to an internal disk.

```sh
make clean all
# macOS: find the stick with `diskutil list` (e.g. /dev/disk4), then:
diskutil unmountDisk /dev/diskN
sudo dd if=build/mintos26-26.5.010.0-uefi.img of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```

balenaEtcher can write the same `.img` file. Boot the stick with **F12** (one-time boot menu) and choose the UEFI USB entry.

## Native MintDE on the OptiPlex today

- MintDE 2 uses three screen-sized buffers (about 24 MB at 1080p, 44 MB at 1440p) and a write-combined framebuffer; the pointer is a kernel overlay. The Windows key acts as Command (⌘Space Spotlight, ⌘Tab, ⌘W, ⌘Q).
- The menu bar clock reads the PC's CMOS clock as-is. Windows usually keeps local time there, Linux usually UTC; MintOS applies no timezone.
- Native input needs a PS/2 keyboard and mouse, or firmware USB legacy emulation that survives `ExitBootServices`. On UEFI-only Skylake firmware that emulation is usually inactive. If neither works, the desktop draws but does not respond.
- Run `power` in Terminal (when input works) to see what the firmware's ACPI tables report, including whether an 8042 controller is advertised.
- The internal SATA disks are mounted read-only only if they contain FAT16. Nothing is ever written to them. The Disk Utility formatter must never be used on this machine.
- Return to UEFI Desktop by restarting (leaf menu → Restart…) or with the reset button.

## Evidence to collect for each hardware run

1. MintOS26 and MintDE versions, plus the BIOS version (F2 → Overview).
2. A photo of the boot selector and of the native desktop.
3. COM1 log if you have a serial cable, especially the `[display]`, `[acpi]`, `[ahci]` and `[input]` lines.
4. Whether Shut Down powered the machine off, and whether Restart rebooted it.
