# MintOS26 26.5.011.0 / MintDE 2.1.0 worklog

Previous worklog: `docs/WORKLOG-26.5.010.0.md`.

## Goal

Device support and the Internet: native USB input (needed on the USB-only Dell OptiPlex 7040), a working network driver, a TCP/IP stack, and user-facing tools that make the network useful from MintDE.

## Design decisions

- **Polling, not interrupts.** xHCI and E1000 both run with interrupts masked. A `usb` and a `net` kernel task poll once per timer tick (100 Hz) on the cooperative scheduler. Syscalls and the tasks touch shared state without yielding, so no locks are needed. Latency is at most one tick, which is plenty for HID and for the TCP rates seen (66–144 KB/s over WAN in QEMU).
- **USB input reuses the PS/2 path.** HID usages are converted to set-1 make codes and injected through the same key decoder, so modifiers, Command shortcuts, repeat handling and the event queue behave identically. Pointer devices share the acceleration curve; absolute tablets bypass it. Injection masks interrupts because the PS/2 IRQ handler writes the same queue.
- **Report descriptors are parsed, not assumed.** Boot protocol is used only for keyboards. Mice and tablets keep report protocol, so wheels and absolute coordinates work. The parser is a bounded walk that records the first X/Y/wheel/button fields, with a boot-layout fallback.
- **Device registers above 64 GiB.** QEMU and many firmwares put 64-bit BARs far above RAM (QEMU's xHCI sits at 768 GiB), outside the identity map. `mint_mmio_map` maps them uncached into the vmalloc window. That window's page directory is shared by every address space, so the mapping is visible to ring-3-triggered syscalls too. These mappings have no allocation record, so they can never be freed into the RAM allocator.
- **The stack lives in the kernel and is non-blocking.** Every syscall returns at once (`-EAGAIN` means ask again). MintDE turns that into background jobs (Terminal) and a per-frame loader (Web), so the desktop never freezes on the network.
- **Everything is statically sized:** the ARP cache, DNS cache, ping slots and 8 TCP sockets with fixed buffers. Every parser bounds its reads by the packet length, and the DNS name walk has a loop guard. The pure parsers are host-tested with ASan/UBSan.
- **TCP keeps only what QEMU's slirp and real servers need:** active open, the MSS option, in-order delivery (anything out of order is dropped and re-acknowledged), go-back-N retransmission with exponential back-off, zero-window probes and a full close sequence. There is no listen, window scaling or SACK.
- **The PCH LAN (I219) is opt-in and conservative.** A full reset of the I217/I218/I219 needs the ME/PHY handshake that other OSes implement in thousands of lines. MintOS stops the RX/TX units instead, keeps the firmware's PHY setup, takes the software semaphore before touching the PHY, and applies the Sunrise Point TX-hang workaround. Because the driver is untested on hardware it never connects automatically.
- **The browser is honest about HTTPS.** Without TLS, an `https://` address gets an explanation page rather than a plaintext request. Address-bar words search wiby.me, which serves plain HTTP.

## Problems found during the work

- **`undefined symbol: memcpy`.** Struct copies in the stack made clang emit libc calls, so `kernel/lib/mem.c` (rep movsb/stosb) was added. MintDE links the same file, because the Web app's struct copies hit the same problem.
- **xHCI "BAR0 is not a usable MMIO range".** The BAR sat at 0xC000000000, above the 64 GiB identity map. This was fixed with the MMIO window described above.
- **Missing glyphs.** Pages showed `??` for non-breaking spaces and accented letters. Raw UTF-8 NBSP, soft hyphens and zero-width characters are now filtered in the HTML tokenizer. The eight text faces gained Latin-1 and Latin Extended-A, and a script check confirmed every pre-existing glyph is byte-identical.
- **Stale toolbar.** The Web loading bar stayed visible after a page finished, because background repaints only damaged the content area. They now damage the whole window.
- **Dropped keystrokes.** Typing "settings" quickly into Spotlight produced "sett". Each HID endpoint had one transfer in flight, re-armed only when the cooperative `usb` task ran. While MintDE repainted the blurred Spotlight panel (slow under TCG), the device's reports were overwritten. Each endpoint now keeps 8 transfers queued with separate buffers. A 72-character mixed-case sentence then arrived intact.
- **PS/2 rows claimed ACTIVE without a PS/2 controller.** The shared readiness flags count USB devices too. The catalog now asks the PS/2 driver directly.
- **Mojibake in the USB page.** A Python edit script used a non-raw string, so `\xC2\xB7` landed in the C source as the characters "Â·". The escapes were restored, and `test_usb_net_contracts.py` now rejects double-encoded UTF-8 anywhere in the sources.
- **Two mistakes in my own tests:** an off-by-one threshold for a truncated HID descriptor (the X/Y item completes at byte 54), and patching the wrong DHCP byte. The code was correct in both cases.

## Validation (QEMU 11.1 q35/OVMF, macOS host)

- **USB** with `qemu-xhci` + `usb-kbd` + `usb-tablet` and + `usb-mouse`, both with `-machine i8042=off`: keyboard (including ⌘Space and fast typing), absolute tablet and relative mouse motion and clicks drove the desktop.
- **Network** with `-nic user,model=e1000`:
  - DHCP lease 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3.
  - Ping to the gateway, about 0.2–2 ms.
  - `nslookup example.com`.
  - HTTP: `curl -I` example.com; info.cern.ch page; 151 KB from textfiles.com in 2.2 s; 93 KB from 68k.news in 0.6 s. Zero retransmits and zero dropped frames.
- **Web:** 68k.news live headlines, wiby.me search with Czech diacritics, link navigation and back.
- **Host checks:** all host checks pass except `test_context_switch.py`, which is Linux-only. The new `test_usb_net.c` (45 checks) also passes under ASan/UBSan; `test_usb_net_contracts.py` has 23 checks.
- **Not validated:** VirtualBox, and the physical OptiPlex 7040 (USB input and the experimental I219 driver).
