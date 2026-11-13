# MintOS26 changelog

## 26.5.014.0 / MintDE 2.4.0 — Programs, saved settings, real web pages and sound

- **Programs:** FPU/SSE/AVX in ring 3, timer preemption of programs, a frame pool with demand paging, processes with pipes, a Linux-compatible syscall subset, the musl 1.2.5 SDK, and Lua 5.4 plus tools running from Terminal.
- **Data saving:** a preferences store on MintOS HD, accounts with a first-boot Setup Assistant, Notes with many notes, Terminal history, and saving before Shut Down/Restart.
- **Web:** a new HTML/CSS engine with images, forms, cookies, downloads and favorites, plus TLS 1.2 and P-256.
- **Personality:** an Intel HD Audio driver with original MintOS sounds, a mint-leaf boot animation, procedural wallpapers including the dynamic Mint Day, Notification Center with widgets and banners, and desktop items.
- **Validation:** every `make check` command passes on macOS except the Linux-only test. In a combined QEMU pass, settings, the account and notes survived a restart, the startup chime was captured, and fsck reported the disk clean.

## 26.5.013.0 / MintDE 2.3.0 — MintFS, a read/write filesystem

- **MintFS:** MintOS's own copy-on-write, checksummed filesystem. A power cut leaves the last commit intact, and there is a consistency checker.
- **MintOS disks:** Disk Utility turns a blank disk into a GPT disk with one MintFS volume, "MintOS HD", laid out like macOS (`/Applications`, `/Users/mintos/Documents`, …). It mounts at boot.
- **Write safety:** writes go only through a grant that covers the MintOS partition. Every other disk stays read-only.
- **AHCI** moves up to 4 KiB per command.
- **File syscalls:** 13 new calls, plus an atomic save in the runtime.
- **MintDE 2.3.0:**
  - Files works like Finder: Favorites, New Folder, Rename, Duplicate, Trash, and dates.
  - Notes saves to `~/Documents`.
  - Terminal gains file commands (`ls`, `cd`, `mkdir`, `rm`, `mv`, `cp`, `echo >`, `df`).
  - Settings and Disk Utility show the volume.
  - Sidebars use macOS vibrancy.
- **Tests:** a MintFS torture test with 300 power cuts per seed, GPT checks against an independent parser, safety contracts, and syscall harness cases. In QEMU, data survived hard power-offs and the host fsck reported the disk clean.

## 26.5.012.0 / MintDE 2.2.0 — TLS 1.3 and HTTPS

- **Crypto library:** MintOS's own, in user space: SHA-2, HMAC/HKDF, ChaCha20-Poly1305, AES-GCM, X25519, and RSA (PKCS#1, PSS) and ECDSA P-256/P-384 verification.
- **Certificates:** X.509 chain validation against Mozilla's root list (curl.se bundle, MPL 2.0, fingerprint-checked at generation).
- **TLS 1.3 client:** X25519 key exchange and the three standard cipher suites, verified against real sites (Google, Wikipedia, GitHub, Cloudflare and others).
- **MintDE 2.2.0:**
  - Web opens HTTPS pages, with a padlock, certificate-error pages that have no bypass, DuckDuckGo search, and pages opening at their main content.
  - `curl` speaks HTTPS.
  - Relative links and redirects work again.
- **Processes:** the default user stack is 64 KiB.
- **Not supported yet:** TLS 1.2, P-256 key exchange, revocation checking and images.

## 26.5.011.0 / MintDE 2.1.0 — USB input, Intel Ethernet, IPv4/TCP and a web browser

- **USB:** a polled xHCI driver (firmware handoff, rings, device enumeration on root ports, hot-plug) with HID keyboards, mice and tablets. USB devices drive the same input path as PS/2, including Command shortcuts and the new mouse wheel. Hubs and mass storage are not supported yet.
- **Device registers above 64 GiB** are now mapped through an uncached MMIO window (`mint_mmio_map`). Firmware often places 64-bit BARs this high; QEMU puts the xHCI at 768 GiB.
- **Intel Ethernet:**
  - The 82540EM, 82545EM, 82543GC and 82574L are supported. The PCH-integrated I217, I218 and I219 (for example the OptiPlex 7040's I219-LM) are supported experimentally.
  - Larger rings (32 RX / 16 TX) and non-blocking transmit.
- **IPv4 stack:** ARP, IPv4, ICMP echo, UDP, a DHCP client with lease renewal, a DNS resolver and a TCP client (flow control, retransmission, graceful close). It is driven by the new `net` kernel task, with non-blocking `NET_*` and `TCP` syscalls.
- **MintDE 2.1.0:**
  - A new **Web** browser for HTTP sites (HTTPS needs TLS, which is not available yet) and a redesigned **Network** app.
  - Terminal adds `net up`, `ifconfig`, `ping`, `nslookup`, `curl` and `usb`.
  - Hardware & Drivers gains a **USB** page.
  - Settings adds a Network pane with automatic connection, and the Input pane now shows USB devices.
  - Accented European letters display in the interface.
- **Validation:** QEMU q35/OVMF with `qemu-xhci` USB devices and an e1000 NIC, including without a PS/2 controller. Real web pages loaded over the stack with no retransmissions. VirtualBox and physical hardware are not yet validated.

## 26.5.010.0 / MintDE 2.0.0 — macOS-style desktop

- MintDE 2 is a new desktop in a macOS style: multiple windows with traffic lights, shadows, drag, resize, zoom and minimize; a translucent menu bar with menus, Control Center, Spotlight and a real clock; a glass Dock; Launchpad; ⌘Tab; and Light, Dark and Auto appearance with accent colours.
- Every app was redesigned (Files, Settings, Terminal, Notes, System Monitor, Storage & Logs, Hardware & Drivers, Network, Disk Utility, About), and Calculator is new.
- Original "Mint Waves" wallpapers (light and dark), app icons and pointer shapes, generated by scripts in `tools/`. The previous photographic wallpaper was removed.
- The UI font is Inter 4.1 (SIL OFL), pre-rendered at 11–44 px by `tools/make_fonts.py`.
- Kernel: shared display canvas with present, overlay-pointer and wallpaper syscalls; borrowed (shared) address-space mappings; 8 MiB per-process page limit; CMOS RTC wall clock; Command (Windows) key modifier; FAT16 prefix reads for previews.
- Validated in QEMU q35/OVMF. Not yet run in VirtualBox or on physical hardware.

## 26.5.009.0 / MintDE 1.0.8 — Real-hardware display, input, power and SATA

- The native kernel now boots with GOP modes above 1920x1080 or with padded strides (up to 4096x2400). Display backbuffers are sized from the real mode and claimed from high memory.
- The framebuffer is mapped write-combining through a fully programmed PAT (PA4 = WC); edge pages are split so neighbouring MMIO stays uncached.
- Fixed PS/2 mouse decoding to use 9-bit signed deltas. Fast motion no longer reverses direction. Added a fixed-point acceleration curve.
- Added ACPI table parsing, S5 shut down, and FADT/0xCF9/8042 restart, with new versioned syscalls `MINT_SYS_POWER_INFO` and `MINT_SYS_POWER_CONTROL`.
- MintDE adds Restart… and Shut Down… with a confirmation alert, plus Terminal `power`, `shutdown` and `restart` commands.
- Intel RAID-mode (class 01:04:00, Dell "RAID On") SATA controllers are accepted after AHCI register validation. Storage remains read-only.
- Added a QEMU/OVMF runner for Linux and macOS, and a macOS (Apple Silicon) host-test compiler wrapper.
- Validated live on QEMU 11.1 q35/OVMF: native boot, ACPI power-off and restart. VirtualBox and physical hardware are still to be run.

## 26.5.008.0 / MintDE 1.0.7 — VirtualBox E1000 RX loopback diagnosis

- Switched the internal E1000 self-test to RCTL.LBM_TCVR, the loopback-buffer path used by VirtualBox's emulated 82540EM. The earlier MAC-loopback mode could complete TX while sending the frame toward the virtual network backend rather than the guest RX ring.
- Corrected diagnostics to snapshot the TX descriptor actually submitted, not the next ring slot after `tx_head` advances.
- Added RX descriptor index, length, error flags and DMA buffer address to failure records.
- Fixed About changelog click targets to match the rendered compact rows so all eight visible OS and MintDE versions can be selected.
- E1000 stays opt-in; no TCP/IP or external packet transmission is included. General filesystem writes remain disabled.

## 26.5.007.0 / MintDE 1.0.6 — E1000 link-reset and TX diagnostics

- Changed loopback to exercise MAC loopback alone and explicitly clear CTRL.LRST before TX is queued.
- Added a bounded check that refuses to queue a diagnostic frame if the link-reset bit remains asserted, and stopped forcing the PHY into a second loopback mode.
- Expanded the failure snapshot with TX descriptor command, length, buffer DMA address, TX/RX ring base and lengths, and PCI command register.
- Terminal `net status` and `net test` now expose descriptor command/status details using the versioned E1000 status ABI.
- Preserved the `root#mintos >` command prompt and its distinct command transcript.
- E1000 remains opt-in; no TCP/IP or external packet transmission is added.
- General storage writes remain disabled.

# MintOS26 Release Changelog

## 26.5.006.0 — E1000 Loopback Diagnostics & Terminal Prompt
- Added bounded MDIC read/write routines and PHY loopback setup for the Intel 82540EM.
- The opt-in loopback test records whether PHY setup, TX completion, RX completion, frame comparison, or PHY restoration failed.
- COM1 exports capture STATUS, CTRL, RCTL, TCTL, head/tail registers, descriptor status, MDIC and PHY registers before cleanup.
- Terminal transcript visibly separates commands with the `root#mintos >` prompt and styles command input separately from output.
- E1000 remains idle at boot; `net start` is still explicit and no external packet transmission/TCP/IP service is implemented.

# Changelog

## MintOS26 26.5.005.0 / MintDE 1.0.4

See `changelogs/MintOS26-26.5.005.0.md` and `changelogs/MintDE-1.0.4.md`.


## Added and improved in 26.5.003.0

- **PNG application icons:** twelve separately authored PNG assets are shipped on the EFI and developer FAT volumes and their bounded 96x96 RGBA payloads are rendered by MintDE through a validated ring-3 bitmap syscall.
- **Settings and liquid glass:** repaired the overflowing Appearance/session panel geometry, added real dropdown popovers and keyboard selection, and added backdrop sampling plus specular edges to the rounded alpha-blended surface service.
- **Pointer redraw:** refined the software arrow and redraws the union of old/new pointer bounds so movement across the dock restores the whole damaged area.
- **Versioned changelog browser:** About MintOS26 and About MintDE present a version list before showing release notes; older entries are kept in `changelogs/`.
- **VFS mount table:** publishes `/session` as volatile RAMFS and every validated read-only FAT16 volume as its own `/Volumes/<device>` mount, retaining its actual block index.
- **Hardware I/O groundwork:** added per-device generic block read/failed-read/blocked-write counters and ATA READ/WRITE DMA EXT plus FLUSH CACHE EXT selection for LBA48-capable devices. Generic writes remain blocked.
- **Terminal diagnostics:** added `pci`, `drivers`, `mounts`, `io`, `logs`, `cpu`, `use sda`, `pwd`, `cd`, and volume-aware `ls`/`cat` commands, all through bounded kernel services rather than arbitrary user-mode port I/O.
- **Read-only storage status:** Storage & Logs and Settings now expose the count of mounted read-only volumes.

General filesystem writes and persistent Notes/Settings remain disabled. An ext2/ext4 direction is documented in `docs/FILESYSTEM-ROADMAP.md`; ext4 is not claimed as implemented.

## Previous release: MintOS26 26.5.002.0 / MintDE 1.0.1

## Added and improved

- **MintDE multi-volume Files:** enumerate up to eight read-only FAT16 volumes through an explicit kernel registry. Each mounted volume has a separate selection mapped to its real block-device index, avoiding assumptions that a UI row equals `sda`/`sdb` or a block index.
- **Standard FAT16 compatibility:** a geometrically valid FAT16 volume can be mounted and browsed even when it does not contain MintOS26-specific `MINTOS26/DOCUMENTS/NOTES.TXT` and `SETTINGS.DAT` seed paths. Seed files remain optional integration features, not a filesystem-format requirement.
- **Translucent UI surface primitive:** added a bounded rounded-rectangle alpha-blend syscall; the host test checks the exact blended RGB value and confirms rounded corners leave pixels outside the mask unchanged.
- **Pointer cleanup:** removed the duplicate kernel cursor overlay from the PS/2 path; MintDE owns one software cursor. Adjusted the cursor mask to keep the arrow and lower stem contiguous.
- **Cleaner launch:** MintDE starts with an empty desktop rather than opening the unused Welcome window. Launchpad remains accessible from the dock.
- **Menus, Settings and windows:** rounded glass panels, menu hover state and dropdown hit-testing, category tabs for General, Appearance, Displays, Input, Storage and Privacy, and a mouse-driven resize grip.
- **About pages:** separate About MintOS26 and About MintDE pages, with a changelog button that reveals release details only when requested.
- **Bounded Terminal:** `help`, `clear`, `ver`, `uptime`, `status`, `drives`, `volumes`, `ls`, and bounded `cat` reads. This is a diagnostic shell, not a POSIX shell; arbitrary device I/O and general writes are not supported.
- **Regression coverage:** added two-volume enumeration/selection, generic FAT16 root browsing, alpha blending and cursor-contract checks.

## Storage and safety status

The supplied COM1 log for MintOS26 26.5.001.0 shows one valid 512 MiB FAT16 developer volume mounted read-only while another detected SATA disk is blank and rejected. That log also contains a formatter `ERROR` record followed later by successful read-only mount records; those distinct events are preserved rather than being treated as one universally successful formatting attempt.

Disk Utility's narrow initializer is still the only production media-write path and must only be used on a verified blank disposable disk in a VM snapshot. General filesystem writes, allocation, journaling, crash-safe transactions and persistent Notes/Settings remain disabled. A read-only mount does not prove write durability.

## Previous release: MintOS26 26.5.001.0 / MintDE 1.0.0

- Introduced the UEFI startup selector, with UEFI Desktop remaining the timeout/default recovery path and MintDE as an opt-in one-way handoff.
- Added separate About MintOS26 and About MintDE pages.
- Added Files locations/navigation, Disk Utility, Storage & Logs, and the Hardware & Drivers catalogue.
- Preserved the dock and removed desktop shortcut clutter.

## Known limitations

- This environment does not have VirtualBox/VBoxManage or QEMU/OVMF, so this release's live pointer, menu, resize and storage behavior still needs a VM test.
- PCI IDs and device inventory are not equivalent to functional drivers. VMware SVGA acceleration, E1000 packet I/O/TCP-IP, USB mass storage, NVMe I/O, ACPI power management, native audio and macOS `.kext` compatibility are not implemented.


## MintOS26 26.5.004.0 / MintDE 1.0.3

- Fixed a ring-3 stack exhaustion bug in `use sda` / `use sdb` by moving large syscall result buffers out of the tiny user stack.
- Added containment for CPU exceptions originating in ring 3: log the fault, switch back to the kernel page table, mark the process failed, and resume the scheduler context rather than panicking the whole OS.
- Expanded PCI inventory to decode already-programmed I/O, 32-bit memory and 64-bit memory BARs, plus interrupt line/pin data. BAR sizing writes are intentionally never issued.
- Added the `resources` terminal command and an expanded driver catalog for VirtualBox E1000, VMware SVGA, ICH AC97, legacy IDE, COM1 UART and PCI resources. Device detection is explicitly distinguished from a functional driver.
- Kept AHCI/FAT16 general writes disabled; driver resource discovery and host tests are not live VM validation.

## Driver safety and diagnostics follow-up
- E1000 loopback diagnostics are captured before restoring normal MAC/PHY settings so the register snapshot reflects the actual failing state.
- E1000 partial initialization now clears and verifies PCI bus mastering before releasing the DMA pool; if ownership cannot be revoked, DMA pages are retained and reinitialization is refused.
- `net status` and `net test` show loopback stage, TX/RX ring head/tail, descriptor status, and PHY/MDIC registers. The visible command prompt is `root#mintos >`.
- Live E1000 TX/RX loopback is still experimental; host tests do not establish successful DMA in a VM.
