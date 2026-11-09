# Running MintOS26 on real hardware — Dell OptiPlex 7040 guide

Target machine: Dell OptiPlex 7040 (Intel Skylake, Q170 PCH), Core i5/i7-6xxx, NVIDIA GeForce GTX 1050 Ti, UEFI firmware.
Status: **26.5.012.0 (MintDE 2.2.0) is not yet validated on this machine.** This guide lists what is expected to work, the firmware settings to use, and how to capture evidence.

## What to expect

| Area | UEFI Desktop (default) | Native MintDE (opt-in, key 2/M) |
|---|---|---|
| Display (GTX 1050 Ti GOP) | Firmware GOP, 1920x1080 preferred | Same GOP framebuffer, write-combined via PAT; any mode up to 4096x2400 |
| USB keyboard/mouse | Firmware USB drivers | Native xHCI driver: USB keyboards, mice and tablets on the computer's own ports. Devices behind a USB hub (including keyboards with a built-in hub) are not supported yet |
| PS/2 keyboard/mouse | Firmware | Works (IRQ1/IRQ12) |
| SATA disks | Firmware file-system access | Read-only AHCI; also in Dell "RAID On" mode (member disks individually) |
| M.2 NVMe SSD | Firmware | Not supported (no NVMe driver) |
| Network (Intel I219-LM) | Firmware HTTP only, if offered | **Experimental** driver: starts only when you click Connect (Network app or Settings) or run `net up`; see below |
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
7. **System Configuration → Integrated NIC: Enabled w/PXE**, and **Enable UEFI Network Stack** if offered. This lets the firmware power up and configure the I219-LM PHY, which MintOS's experimental driver relies on.

## Writing the boot USB stick

**This erases the USB stick.** Check the device name carefully. Never write to an internal disk.

```sh
make clean all
# macOS: find the stick with `diskutil list` (e.g. /dev/disk4), then:
diskutil unmountDisk /dev/diskN
sudo dd if=build/mintos26-26.5.012.0-uefi.img of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```

balenaEtcher can write the same `.img` file. Boot the stick with **F12** (one-time boot menu) and choose the UEFI USB entry.

## Native MintDE on the OptiPlex today

- MintDE 2 uses three screen-sized buffers (about 24 MB at 1080p, 44 MB at 1440p) and a write-combined framebuffer; the pointer is a kernel overlay. The Windows key acts as Command (⌘Space Spotlight, ⌘Tab, ⌘W, ⌘Q).
- The menu bar clock reads the PC's CMOS clock as-is. Windows usually keeps local time there, Linux usually UTC; MintOS applies no timezone.
- **Input:** plug the USB keyboard and mouse directly into the computer's ports, not into a hub or monitor hub. The xHCI driver takes ownership from the firmware, so firmware legacy emulation is no longer needed. COM1 shows `[usb] port N slot M: HID keyboard ready`. If nothing responds, Terminal `usb` (reachable with a PS/2 keyboard) and the COM1 log show what was found.
- **Network:** the onboard Intel I219-LM uses an experimental driver. It is started without a full reset and relies on the PHY state the firmware leaves behind.
  - In the firmware, **enable the UEFI network stack / "Integrated NIC: Enabled w/PXE"** so the PHY is powered and configured before MintOS starts.
  - Then click **Connect** in the Network app. The `[e1000]` and `[net]` lines on COM1 report the link and DHCP progress.
  - If it never gets an address, a USB-to-Ethernet adapter is not an alternative yet (no USB network driver).
- **Clock and HTTPS:** HTTPS certificate checks use the PC's CMOS clock, read as UTC. If Windows on the same machine keeps local time in the CMOS clock, the error is only a few hours, which is harmless for certificates. A clock that is years off makes sites look "not valid yet" or "expired".
- Run `power` in Terminal (when input works) to see what the firmware's ACPI tables report, including whether an 8042 controller is advertised.
- The internal SATA disks are mounted read-only only if they contain FAT16. Nothing is ever written to them. The Disk Utility formatter must never be used on this machine.
- Return to UEFI Desktop by restarting (leaf menu → Restart…) or with the reset button.

## Evidence to collect for each hardware run

1. MintOS26 and MintDE versions, plus the BIOS version (F2 → Overview).
2. A photo of the boot selector and of the native desktop.
3. COM1 log if you have a serial cable, especially the `[display]`, `[acpi]`, `[ahci]`, `[input]`, `[usb]`, `[e1000]` and `[net]` lines.
4. Whether Shut Down powered the machine off, and whether Restart rebooted it.
