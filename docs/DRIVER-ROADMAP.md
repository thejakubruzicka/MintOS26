# MintOS26 Driver Roadmap

## Current baseline — 26.5.008.0

The kernel has read-only PCI mechanism-1 enumeration and a versioned inventory of configured BARs and PCI interrupt line/pin fields. BARs are decoded read-only; the kernel does not size resources by writing all ones. AHCI currently does bounded, serialized, polled DMA reads using the advertised LBA48 `READ DMA EXT` path when supported. PS/2 keyboard/mouse input and a polled COM1 transmit logger are functional at their current scope. The UEFI framebuffer is the current display surface; VMware SVGA is detected but is not a functional GPU driver.

## Current E1000 implementation — 26.5.008.0

The Intel 82540EM (8086:100E) is detected read-only at boot; it remains idle with bus mastering disabled until Terminal `net start`. Explicit startup then claims a contiguous DMA pool below 4 GiB, marks its BAR0 MMIO range uncached, initializes fixed-size RX/TX descriptor rings, masks device interrupts and provides a bounded VirtualBox-specific TCVR internal-loopback diagnostic. This is a polling-only first device driver; it does not implement TCP/IP, DHCP, DNS, sockets, interrupt-driven networking or external packet-send commands.

## Driver sequence

1. **Resource/ownership layer:** checked PCI BDF and BAR decode, MMIO mapping attributes, DMA allocation below device limits, cache-coherency rules, command timeouts and per-device fault counters.
2. **VirtualBox serial/input/display:** add testable 16550 RX where a serial port is assigned, improve PS/2 packet/status paths, and decide whether the display adapter is VMSVGA or a simpler framebuffer interface before attempting acceleration.
3. **E1000 NIC:** validate the opt-in reset/RX/TX setup in the VM, repeat internal loopback and receive tests, then add a kernel-owned packet-buffer queue. No TCP/IP claims until that path is repeatable.
4. **USB UHCI/OHCI/EHCI:** enumerate root ports, keyboard/mouse HID and eventually USB mass storage; no unsafe controller probing on unknown devices.
5. **Audio and ACPI:** implement device-specific DMA/interrupt handling after PCI resources, IRQ routing and memory ownership are robust.
6. **MacBook support:** only after VM drivers stabilize. Native NVMe, Apple-specific input, USB-C/Thunderbolt and power management need dedicated hardware evidence; do not test by writing to the MacBook disks.

## Validation rule

A detected VID:DID or decoded BAR does not mean a driver works. Every driver needs host-decoder tests, resource bounds tests, failure-timeout tests, and a repeatable VM test with COM1 logs. Do not enable bus mastering or issue device commands until each resource and DMA buffer is explicitly owned and validated.

## 26.5.008.0 safety notes

The loopback test now snapshots failure registers before restoring the prior CTRL/RCTL configuration. A partial initialization failure clears PCI bus mastering before returning its contiguous DMA pool to the physical allocator; if bus-master ownership cannot be confirmed off, the driver retains the pages and refuses a retry. This deliberately prefers a bounded memory leak over a device writing into reused pages. `net status` displays the last loopback stage and relevant head/tail, descriptor and PHY registers. Actual loopback remains pending a live VirtualBox run.


## 26.5.008.0 E1000 diagnostic follow-up

The user's 26.5.007.0 COM1 trace showed TX completion but no RX descriptor completion. The diagnostic now uses the TCVR loopback-buffer mode specific to VirtualBox's emulated Intel 82540EM and reports the actual submitted TX descriptor plus RX descriptor state. This mode is a hypothesis grounded in the emulator's implementation, not a promise about physical E1000 cards. Validate `net start` / `net test` in a snapshot VM before enabling interrupts or external packet paths.


## 26.5.009.0 hardware-compatibility notes

- **Display:** still the firmware GOP framebuffer (no GPU driver), now write-combined through PAT entry PA4 and supported up to 4096x2400 including stride padding.
- **ACPI:** new read-only table parser (RSDP/XSDT/RSDT/FADT/DSDT/SSDT, checksummed and bounded) used for S5 soft-off and reset. There is no AML interpreter; `_PTS`/`_GTS` are not executed.
- **SATA:** Intel class 01:04:00 (RST "RAID On") controllers are accepted only after the ABAR's AHCI version and ports-implemented registers validate, and before bus mastering is enabled. RAID volumes are not assembled.
- **Input:** PS/2 deltas are decoded as 9-bit two's complement. USB HID (xHCI) is the next driver milestone. A detected xHCI controller is still inventory only.
