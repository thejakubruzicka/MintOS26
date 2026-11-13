# MintOS26 26.5.013.0 / MintDE 2.3.0 — Test Guide

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

### App platform test (26.5.014.0)

Host: `make check` runs `tools/test_app_platform.py` (frame pool, Linux-compat table and translation, pipes/OFDs, FPU clean state) and `tools/test_app_platform_contracts.py` first, through `check-app-platform`. On macOS, run the steps after `tools/test_context_switch.py` one by one (`make -n check`).

QEMU, automated:

```sh
cc -std=c11 -O2 -o build/mkmintos-disk tools/mkmintos_disk.c kernel/block/block.c kernel/fs/mintos_disk.c kernel/fs/mintfs.c
rm -f build/qemu-mintos-disk-1g.img && build/mkmintos-disk build/qemu-mintos-disk-1g.img 1024
python3 tools/qemu_app_platform.py SCRIPT
```

On Apple Silicon, put `tools/macos-host` on `PATH` for the `cc` step. The QEMU driver uses HMP 45463 and QMP 45473; see the docstring for script commands.

By hand: boot with `--mintos-disk`, press 2, open Terminal and check:

- COM1 shows `[fpu] TWO-PROCESS FPU STATE ISOLATION SELF-TEST PASSED` and `[programs] installed … bundled files`.
- `hello a "b c"` prints its argv. `hello fail` reports `exited with status 3`.
- `lua` starts the REPL: try `print(7/2, 2^0.5, 0.1+0.2)`; Ctrl+D quits.
- `lua /Library/Examples/Lua/primes.lua 200000` runs a script; `echo print(1.5*2) > t.lua` then `lua t.lua` runs your own.
- `spin 10 &`, then `ps` and `sysinfo`, shows two programs alive at once. `spin` in the foreground, then Ctrl+C, reports `interrupted (Ctrl+C)`.
- `wc` and `grep -n print` work on `/Library/Examples/Lua/*.lua`.

### Personality test (26.5.014.0): sound, boot animation, wallpapers, notifications

`make check` runs `personality-check`:
- `test-hda`: the HDA core against a register-level fake controller with QEMU and Realtek-like codecs.
- `test-audio-mixer`: the kernel mixer.
- `test_sounds.py`, `test_splash.py`, `test-wallpapers`, `test-notify` and `test_personality_contracts.py`.

In QEMU:

1. Add the HDA controller and codec to the usual command:
   `tools/run_qemu.sh --mintos-disk -- -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 -audiodev coreaudio,id=snd0 -device ich9-intel-hda -device hda-output,audiodev=snd0`
   (Linux: `-audiodev pa,id=snd0` or `alsa`).
2. Press 2. The mint-leaf boot animation runs, crossfades into the desktop and plays the startup chime. COM1 shows `[splash]` and `[audio] HDA running: QEMU HDA output`.
3. **Settings › Sound:** change the alert sound (it previews), the volume and the switches. **Control Center:** drag the Sound slider; Do Not Disturb.
4. **Settings › Wallpaper:** pick Mint Day (dynamic), Aurora, Leaf Light and the others.
5. **Notifications:**
   - Settings › Notifications › Send Test Notification shows a banner at the top right that dismisses itself after 5 s.
   - Click the date and time in the menu bar to open Notification Center.
   - Terminal `notify hello` also posts one.
6. **Desktop items:** after Disk Utility has made a MintOS HD, in Terminal run `cd Desktop`, `mkdir Projects`, `echo hi > Note.txt`. The icons appear on the desktop. Double-click to open them; Return renames through Files.
7. **⌘⇧3** saves a screenshot on the Desktop, with a sound and a banner. **Volume keys:** HMP `sendkey volumeup`, `volumedown` and `audiomute` show the volume HUD.
8. **Headless audio capture:** `python3 tools/qemu_personality.py SCRIPT --wav chime.wav` (it uses `-audiodev wav,...,out.frequency=48000`), then `python3 tools/analyze_wav.py chime.wav build/sounds/startup.pcm --channels 2 --volume 70`. Expect a bit-exact match: correlation 1.00000, worst error 0.

### MintOS disk test (26.5.013.0)

On the host, `make check` runs these MintFS tests:
- `test-mintfs`: random operations against a model, fsck after every commit, 300 power cuts with torn writes, a full disk and checksum damage. Set `MINTFS_SEED=n` for other seeds.
- `test-mintos-disk`: the real block layer, MintOS-disk module and MintFS on RAM disks.
- `test_gpt.py` and `test_mintfs_contracts.py`.

In QEMU:

1. `tools/run_qemu.sh --mintos-disk -- -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0`. This attaches `build/qemu-mintos-disk-1g.img`, a blank 1 GiB disk that is **persistent**, unlike the other disks.
2. **Disk Utility:** select the 1 GB disk, then **Erase…**, keep **MintOS (MintFS)**, type `FORMAT sdX` as shown, and press Erase. COM1 shows `[mintfs] initialized a MintOS disk`.
3. **Files** opens at `mintos` (home). Try New Folder (⇧⌘N), Rename (Return), Duplicate (⌘D), Move to Trash (⌘⌫), the Dock Trash and Empty Trash (⇧⌘⌫).
4. **Notes:** type, wait a second, and `Notes.txt` appears in Documents. Double-click a `.txt` file in Files to open it in Notes.
5. **Terminal:** `ls -l`, `cd Documents`, `echo hi > a.txt`, `cat a.txt`, `mkdir x`, `mv a.txt x`, `rm -r x`, `df`.
6. **Persistence:** quit QEMU (even mid-edit) and run the same command again. Everything saved before the cut is there, and COM1 shows `[mintfs] mounted "MintOS HD"`.
7. **Host check:** `cc -std=c11 -O2 -DMINT_GPT_HOST_TEST tools/mintfs_tool.c kernel/fs/mintfs.c kernel/fs/mintos_disk.c -o build/mintfs-tool`, then `build/mintfs-tool check build/qemu-mintos-disk-1g.img` (should report `fsck: clean`) and `build/mintfs-tool ls build/qemu-mintos-disk-1g.img / -r`.

Delete `build/qemu-mintos-disk-1g.img` to start over with a blank disk.

Since 26.5.014.0 a MintOS disk without an account (and a blank disk when there is no MintOS disk) starts the **Setup Assistant** at boot, so steps 2–4 above become the data-saving test below.

### Data saving and Setup Assistant test (26.5.014.0)

On the host, `make check` runs:
- `test-data-saving` (`tools/test_data_saving.c`, 49 checks): the preferences parser and serializer, account-name rules (MintDE and the kernel must agree), name suggestions and initials, the accounts file, then `prefs.c` and `accounts.c` unchanged on the real block layer, MintOS-disk module and MintFS: debounced saving, `/Library/Preferences` vs `~/Library/Preferences`, account creation moving `/Users/mintos`, the kernel taking the home from the account at the next mount, the set-home validation, and the power-off flush hook (data written without close is lost on a cut and kept after the hook). It ends with fsck.
- `test_data_saving_contracts.py` (29 checks): syscall `0x4D50` and its stubs, log component `PREFS`, boot and shell wiring, the flush order before power-off and MintDE's memory budget.

In QEMU (delete `build/qemu-mintos-disk-1g.img` first for a blank disk):

1. `tools/run_qemu.sh --mintos-disk -- -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0`, then **2** at the selector.
2. **Setup Assistant** appears instead of the desktop: "Hello" cycles through languages. **Get Started**.
3. **MintOS HD:** only the blank 1 GB disk is offered. Type `FORMAT sdX` as shown and press Return; COM1 shows `[MintDE] Setup Assistant initialized MintOS HD`. (**Not Now** continues for the session only.)
4. **Account:** type a full name; the account name is suggested (edit it if you like). Continue; COM1 shows `[mintfs] home folder is now /Users/<account>` and `previous home moved`.
5. **Look:** pick Dark and an accent; the screen changes at once. **Start Using MintOS**.
6. Check: Terminal prompt `<account>@MintOS %`; Files' Favorites name the home after the account; Settings has the account card and **Users & Groups**.
7. **Notes:** ⌘N, type a title and text, ⌘N again for a second note. Both appear under Today in `~/Documents/Notes/<title>.txt`. Search filters them; ⌘⌫ moves a note to the Trash.
8. Change the wallpaper, open a folder in Files, run a few Terminal commands (`cat Library/Preferences/com.mintos.desktop.prefs` shows the saved settings).
9. **Apple menu › Restart…** and Return. COM1 shows `[MintDE] notes, history and preferences saved before power-off`, `[power] saving MintOS HD before power-off` and `[mintfs] final commit before power-off: clean`. After a QEMU warm reset the selector may ignore HMP keys (they go to the USB keyboard); quit QEMU and start it again instead, which is a cold boot from the same disk.
10. After the restart there is no Setup Assistant; COM1 shows `[mintfs] home folder /Users/<account> (from the MintOS account)`. Theme, accent, wallpaper, the selected note, Files' last folder and the Terminal history (Up arrow) are all back.
11. **Host check:** `build/mintfs-tool check build/qemu-mintos-disk-1g.img` reports `fsck: clean`; `build/mintfs-tool cat build/qemu-mintos-disk-1g.img /Library/Preferences/com.mintos.accounts` shows the account.

To run the assistant again: Settings › Users & Groups › **Run Setup Assistant…** (renaming the account renames its home folder).

### HTTPS test (26.5.012.0)

On the host, `tools/test_crypto.py`, `tools/test_x509.py` and `tools/test_tls.py` run as part of `make check`. They need an OpenSSL 3 command line, for example `brew install openssl`, and the tests skip politely without one. Add `--live` to `test_x509.py` and `test_tls.py` to also check real sites.

In QEMU (network as below):

1. **Terminal:** `curl -I https://example.com` prints the TLS 1.3 line ("certificate verified") and `HTTP/1.1 200 OK`. `curl -s https://www.google.com/` reports about 88 KB.
2. **Web:** open `https://en.wikipedia.org/wiki/Prague_Castle`. The address bar shows a padlock and `en.wikipedia.org`, and the page opens at the article title.
3. **Search:** type a few words; DuckDuckGo results load over HTTPS.
4. **Certificate errors:** a certificate for another name, or one from an untrusted authority, shows "This connection is not private" with the reason. The local `openssl s_server` tests in `tools/test_tls.py` produce these cases on demand.
5. **TLS 1.2-only server** (for example `https://tls-v1-2.badssl.com:1012/`): since 26.5.014.0 the page opens over TLS 1.2 (padlock shown). `curl -I` reports "TLS 1.2 with ECDHE-...".

### Web engine test (26.5.014.0)

On the host, `tools/test_web_engine.py` (part of `make check`) builds the engine with the system compiler and checks:
URL resolution (RFC 3986 examples), requests, cookies (RFC 6265 subset), form encoding and dates; the bounded heap;
inflate against Python's `zlib` (raw/zlib/gzip, every level and strategy, corrupt/truncated/bomb streams);
the HTTP/1.1 parser fed in random pieces (chunked, gzip, 1xx, redirects, Set-Cookie, Content-Disposition);
PNG/GIF/BMP decoding pixel-exact and baseline/progressive JPEG within ±8 of libjpeg-turbo (`tools/fixtures/web`);
HTML + CSS + layout of `tools/fixtures/web/article.html` (floats, tables, lists, forms, hidden content, var(), @media,
quirks mode) at several widths; malformed/deep markup; that every engine file builds for ring 3 with no SSE/x87
instructions; and source contracts. `tools/test_tls.py` adds `openssl s_server -tls1_2` cases (six ECDHE AEAD suites,
ECDSA and RSA certificates, X25519 and P-256, with and without extended master secret) and TLS 1.3 over P-256 via
HelloRetryRequest; `--live` also checks badssl.com.

`tools/test_web_layout.c` can lay out any saved page: `build/test-web-layout page.html 1880 sheet1.css ...` prints the
display list and the heap use per stage (useful when a page runs out of memory).

In QEMU (`tools/run_qemu.sh --mintos-disk -- -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0`,
native MintDE, a MintOS HD on the scratch disk; `tools/qemu_drive.py` automates boot, typing, clicks and screenshots over QMP):

1. **Wikipedia:** `en.wikipedia.org/wiki/Prague` shows the styled article: heading, links in Wikipedia blue, the infobox
   floated right with its photo, references and images further down loading as you scroll (Page Down).
2. **Forms:** open `lite.duckduckgo.com/lite/`, press Tab, type a query, press Return: the POST form submits and the
   results page shows the query field, selects, buttons and results.
3. **Hacker News** (`news.ycombinator.com`): the table layout with the orange header, story titles left-aligned.
4. **TLS 1.2:** `https://tls-v1-2.badssl.com:1012/` opens (green page, padlock).
5. **Downloads:** open a PDF link such as `https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf`:
   the Downloads panel opens with progress, the file is saved as `~/Downloads/dummy.pdf`, "Show in Files" opens Files.
6. **Favorites:** ⌘D on a page adds it (status "Added to Favorites"); it appears on the start page and in the book
   button's list, and survives a restart (`~/Library/Web/Bookmarks`). Cookies persist in `~/Library/Web/Cookies`.

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
