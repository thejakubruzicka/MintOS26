# MintOS26 26.5.012.0 / MintDE 2.2.0 — Test Guide

## Host build and test

```sh
make clean all
make check
make preview
```

On macOS with Apple Silicon (`brew install lld qemu binutils`), run the host tests through the bundled wrapper. It builds host test programs for x86_64 and runs them under Rosetta 2:

```sh
PATH="$PWD/tools/macos-host:/opt/homebrew/opt/binutils/bin:$PATH" make check
```

`tools/test_context_switch.py` links ELF-format kernel assembly into a host program and only runs on a Linux host. On macOS it fails at assembly time; this is not a MintOS regression. (`tools/test_paging.py` now names linker-script symbols portably and runs on both.) 26.5.009.0 adds `tools/test_acpi.py` (ACPI parser against synthetic firmware tables) and `tools/test_hardware_compat.py` (display buffers for 1080p/padded/1440p/4K modes, PAT/WC, RAID-mode AHCI and PS/2 contracts).

## QEMU/OVMF quick run (Linux or macOS)

`make run` (or `tools/run_qemu.sh [--headless] [--blank]`) boots the GPT image on a q35 machine with ICH9 AHCI, an e1000 NIC and COM1 written to `build/com1-qemu.log`. All disks use `snapshot=on`, so the build images are never modified. Press **2** at the selector for native MintDE.

### HTTPS test (26.5.012.0)

On the host, `tools/test_crypto.py`, `tools/test_x509.py` and `tools/test_tls.py` run as part of `make check`. They need an OpenSSL 3 command line, for example `brew install openssl`, and the tests skip politely without one. Add `--live` to `test_x509.py` and `test_tls.py` to also check real sites.

In QEMU (network as below):

1. **Terminal:** `curl -I https://example.com` prints the TLS 1.3 line ("certificate verified") and `HTTP/1.1 200 OK`. `curl -s https://www.google.com/` reports about 88 KB.
2. **Web:** open `https://en.wikipedia.org/wiki/Prague_Castle`. The address bar shows a padlock and `en.wikipedia.org`, and the page opens at the article title.
3. **Search:** type a few words; DuckDuckGo results load over HTTPS.
4. **Certificate errors:** a certificate for another name, or one from an untrusted authority, shows "This connection is not private" with the reason. The local `openssl s_server` tests in `tools/test_tls.py` produce these cases on demand.
5. **TLS 1.2-only server** (for example `https://tls-v1-2.badssl.com:1012/`): shows "Secure connection failed" and says the server may only support TLS 1.2.

### USB, network and Web test (26.5.011.0)

Boot with USB devices on an xHCI controller (the e1000 NIC is already part of `run_qemu.sh`):

```sh
tools/run_qemu.sh -- -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
```

Add `-machine i8042=off` to remove the PS/2 controller, as on USB-only PCs. Use `usb-mouse` instead of `usb-tablet` to test relative motion.

1. **USB:** COM1 shows `[usb] xHCI running` and `HID keyboard ready` / `HID tablet ready`. Typing, ⌘Space, the pointer, clicks and the wheel all work; Hardware & Drivers › USB lists the devices.
2. **Network:** COM1 shows `[net] DHCP lease 10.0.2.15 …` a moment after the desktop appears. The Network app reports Connected, with IP 10.0.2.15, router 10.0.2.2 and DNS 10.0.2.3.
3. **Terminal:** `ifconfig`; `ping 10.0.2.2 3` gets three replies; `nslookup example.com` returns an address; `curl -I http://example.com` prints `HTTP/1.1 200 OK`; `curl -s http://textfiles.com/etext/FICTION/alice.txt` reports about 151 KB; `ifconfig` then shows 0 retransmits. Ctrl+C stops a running command. (QEMU user networking cannot ping hosts outside the VM.)
4. **Web:** open Web from the Dock and follow the 68k.news link (live headlines). Type `prague castle` in the address field to search wiby.me, then check that accented letters display. ⌘[ goes back. An `https://` address shows the "needs a secure connection" page.
5. **Disconnect:** click Disconnect in Network; `curl` now reports that the network is down. Connect again.

`tools/test_usb_net.c` (HID descriptors, key map, intervals, checksums, IPv4 text, DHCP, DNS, TCP options) also runs cleanly under `-fsanitize=address,undefined`. `tools/test_usb_net_contracts.py` checks the drivers' DMA, wait and parsing rules.

### MintDE 2 desktop test (26.5.010.0)

1. **Desktop:** after boot you should see the wallpaper, menu bar (leaf menu, bold "Files", menus, Control Center, Spotlight, date and time) and the Dock.
2. **Windows:** open Files from the Dock. Check the rounded window, shadow and traffic lights; × − + appear on hover. Drag the toolbar, double-click it to zoom, resize from the bottom-right corner, minimize with the yellow light, and restore from the Dock.
3. **Several apps:** open Terminal, Notes and Settings together. Clicking a window brings it to the front, and inactive windows have grey traffic lights. ⌘Tab (Windows key + Tab) switches apps; ⌘W closes; ⌘M minimizes.
4. **Menus:** hover over leaf menu items (accent highlight), choose System Settings…, and check that Edit items are greyed out.
5. **Overlays:** ⌘Space opens Spotlight (type "ter", then Enter opens Terminal). The Launchpad Dock icon shows every app. The Control Center icon toggles Dark Mode and accent colour.
6. **Settings › Appearance:** Light, Dark and Auto (Auto turns dark from 19:00 to 07:00); the accent colour applies everywhere.
7. **Files:** select the FAT16 volume, double-click into folders, select a text file (preview pane) and use the search filter.
8. **Disk Utility:** Erase… is only enabled for a blank, unmounted disk.

### Power management test (26.5.009.0)

1. In native MintDE, open Terminal and run `power`. It reports the ACPI revision, `\_S5_` SLP_TYP values, the PM1a control port and the restart method.
2. MintDE menu → **Restart…** → Restart (or Enter). The machine must reset and return to the boot selector or UEFI Desktop.
3. MintDE menu → **Shut Down…** → Shut Down. The machine must power off; QEMU exits. If the platform ignores S5, the alert says so; then use the power button.
4. Esc or Cancel must dismiss the alert without side effects.

Host tests cover source contracts, UEFI/ISO/GPT construction, ELF permissions, memory management, scheduler/syscalls, UI/service contracts, FAT16 parser/formatter behavior, multiple mounted volumes, generic FAT16 browsing without MintOS seed files, RGBA bitmap scaling/alpha blending, VFS mount-table safety, per-device block-I/O counters and VDI/raw payload verification. They do not emulate live AHCI DMA, guest pointer rendering or live menu interaction.

## UEFI boot selector

1. Use a disposable VM copy and snapshot. Keep **Enable EFI** active.
2. Attach `MintOS26-26.5.009.0-UEFI.iso` to the optical drive.
3. Start the VM. Press **1** or **Enter** for UEFI Desktop; press **2** or **M** to request MintDE. No input selects UEFI Desktop after three seconds.
4. MintDE exits UEFI boot services and the handoff is one-way until reset. If it fails, reset the VM; do not remove the UEFI Desktop recovery route.

## MintDE UI test

1. Test the mouse cursor moving slowly and quickly over wallpaper and translucent panels; look for trails/duplicate cursors.
2. Test each menu-bar dropdown, hover highlight, clicking outside a menu, the dock and Launchpad.
3. Open About MintOS26 and About MintDE. Verify the displayed versions, then expand and hide each changelog.
4. Open Settings and visit General, Appearance, Displays, Input, Storage and Privacy. Test an adjustable row and resize a normal window using its bottom-right grip.
5. Open Files; switch between Session (RAM) and every mounted developer volume. Test Up navigation, folders and bounded text previews.
6. Open Terminal and test `help`, `ver`, `uptime`, `status`, `drives`, `volumes`, `mounts`, `pci`, `drivers`, `io`, `logs`, `cpu`, `use sda`, `pwd`, `ls`, `cd`, and `cat`. For the experimental E1000 driver, run `net status` first; boot only detects the device and leaves bus mastering disabled. Run `net start` explicitly to initialize the adapter, then `net test` for the VirtualBox-specific TCVR internal-loopback diagnostic. The test does not intentionally send packets onto the NAT network. `resources` displays read-only PCI BAR and IRQ metadata. This terminal is a bounded diagnostic shell, not an unrestricted host-style shell or arbitrary hardware-port interface.

## Storage mount-only test

Attach the populated `MintOS26-26.5.009.0-SATA-data-512MiB-VDI-dynamic.vdi` as a separate SATA data disk. Storage & Logs should show the correct 512 MiB capacity, MBR signature and FAT16 read-only mount if the guest read path succeeds. Files should list it alongside every other validated FAT16 volume. No general file writes or persistent Notes/Settings are supported.

## Disk Utility destructive test

1. Take a fresh VM snapshot and configure COM1 as **Raw File** logging.
2. Disconnect existing data disks from the disposable test VM. Keep the ISO optical drive attached. Attach **only** `MintOS26-26.5.009.0-SATA-blank-512MiB-VDI-dynamic.vdi` as SATA data.
3. Boot UEFI Desktop and launch MintDE. Open Disk Utility and verify the selected disk's model, serial and capacity.
4. Enter the exact confirmation phrase shown by the app (for example `FORMAT sda`) only after verifying that the selected device is the blank disposable disk. The operation destroys data on that disk.
5. Save the complete COM1 log and screenshot. The formatter writes an MBR FAT16 partition and checks it by read-back and a read-only mount. If any operation reports an error, preserve the first failure and later records; do not infer success from a later unrelated mount message.

## Multiple-volume test

Attach one populated developer VDI and one blank VDI. The populated volume should mount read-only while the blank one is rejected. Then, in a disposable snapshot, format the blank volume and refresh Storage & Logs and Files. Confirm both mounted volumes appear separately, and verify the volume selector switches directory listings correctly.

## Runtime boundaries

QEMU/OVMF is available on the 26.5.009.0 development host; VirtualBox is not. Report VM validation separately from host tests and name the hypervisor. The E1000 driver is opt-in and has not been validated live here; it provides bounded polling rings and a VirtualBox-specific TCVR internal-loopback test, not a general packet interface. No TCP/IP, DHCP, DNS, sockets, functional GPU acceleration, USB mass storage, NVMe driver, ACPI power transitions, native audio or macOS `.kext` compatibility is claimed from PCI discovery alone.


## E1000 loopback diagnostics in 26.5.008.0
1. Use a snapshot with EFI enabled and COM1 Raw File logging.
2. In MintDE Terminal run `net status`, `net start`, `net test`, `net status`.
3. A failed loopback should identify link-reset, TX descriptor, RX descriptor or frame-mismatch stage. COM1 also records TX command/length/buffer, DMA ring bases/lengths, PCI command, MMIO and PHY/MDIC snapshot. The test uses VirtualBox-specific RCTL.LBM_TCVR internal loopback and does not configure PHY loopback. This branch is for the emulated 82540EM, not claimed to work on arbitrary physical NICs.
4. No external packets are sent; a loopback pass does not prove NAT or TCP/IP functionality.

## E1000 loopback diagnosis (26.5.008.0)

With a VM snapshot and COM1 Raw File logging enabled, boot MintDE, then type `net status`, `net start`, `net test`, and `net status` at the visible `root#mintos >` prompt. Initialization is opt-in; boot only detects the PCI function. On failure, the COM1 report should include a stage number plus STATUS/CTRL/RCTL/TCTL, TX/RX head/tail, descriptor state, MDIC and PHY control/status captured before restoration. The Terminal repeats key registers. Attach the entire COM1 log; a stage timeout is diagnostic evidence, not a successful network test. This path is internal loopback only and is not TCP/IP.
