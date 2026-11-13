# 26.5.014.0 — personality (sound, boot animation, wallpapers, desktop extras)

Branch `topic/personality`. Release numbers are left at 26.5.013.0 / MintDE 2.3.0 for the coordinator. A refreshed icon set is deferred to 26.6/26.7, as agreed.

## What changed

### Sound: Intel High Definition Audio driver and kernel mixer
- **`kernel/sound/hda_core.c`** contains the controller and codec logic, which the host can test against a register-level fake:
  - link reset with bounded waits;
  - CORB/RIRB setup, including QEMU's "RINTFL must be acknowledged" behaviour and both styles of read-pointer reset;
  - a fallback to the immediate-command registers when the CORB never answers;
  - codec discovery: the AFG, every widget's capabilities, pin capabilities, configuration default, amp capabilities (with the AFG defaults) and connection lists (long form, multi-request and range entries);
  - a breadth-first **widget-graph walk** from each output pin to an analog DAC. Line out, speaker and headphone pins are ranked, digital and "no connection" pins are skipped, and every pin prefers to share the first DAC;
  - path programming: power D0, connection selects, mixer input amps, output amps at 0 dB, pin control with HP drive, **EAPD**, converter format and stream tag;
  - hardware mute, the BDL builder and output stream descriptor setup.
- **`kernel/sound/hda.c`** is the polled kernel driver:
  - Boot does only a read-only PCI match on class 0x0403.
  - Under a hypervisor (CPUID hypervisor bit) the `audio` task starts the controller. On real hardware (the OptiPlex 7040's Realtek ALC3234 = ALC255) the driver is **experimental** and starts only from Settings › Sound or Terminal `sound start`, which needs a token, like the I219 NIC.
  - Output is one 48 kHz 16-bit stereo stream over a 256 KiB ring (8 BDL entries).
  - DMA memory lies below 4 GiB, and CPU cache lines are flushed with `clflush`, so non-snooping controllers work too. Intel TCSEL is cleared.
  - Stop order: stream off, CORB/RIRB off, link held in reset, bus mastering off **and read back**. Only then is DMA memory released; if bus mastering stays on, the memory is kept rather than freed.
- **`kernel/sound/audio_mixer.c`** is the mixer.
  - It runs from the **PIT interrupt** through `kernel/sound/system_status.c`, so sounds keep playing while MintDE is busy. That hub also samples CPU load for the Notification Center: a tick that lands right after the idle `hlt` counts as idle.
  - Voices are mixed additively with saturation, up to 500 ms ahead of the play position.
  - New sounds start 10 ms after the play position, so latency stays low despite the long lead.
  - Frames the hardware has played are silenced, so an underrun plays silence, never old audio.
  - A volume change re-renders the queued audio, so it takes effect at once.
  - The volume curve is quadratic.
  - User PCM is copied into four kernel-owned pool slots.
- Driver catalog row 13 is now **Intel High Definition Audio** and shows the live state. Log component **14 AUDIO**.
- **Syscalls** (personality range, `kernel/include/uapi/mintos/audio.h`, handled in `kernel/sound/audio_syscall.c`; userspace wrappers in `kernel/user/personality_runtime.c` and `mintos/personality.h`):

  | number | call |
  |---|---|
  | 0x4D58 | `MINT_SYS_AUDIO_INFO` – state, controller, codec, output paths, volume, counters |
  | 0x4D59 | `MINT_SYS_AUDIO_PLAY_SOUND` – built-in sound id, gain |
  | 0x4D5A | `MINT_SYS_AUDIO_QUEUE` – user 48 kHz int16 PCM, mono or stereo, ≤ 96 000 samples |
  | 0x4D5B | `MINT_SYS_AUDIO_VOLUME` – volume 0–100, mute |
  | 0x4D5C | `MINT_SYS_AUDIO_CONTROL` – start (token), stop, silence |
  | 0x4D5D | `MINT_SYS_BOOT_SPLASH` – finish / status of the boot animation |
  | 0x4D5E | `MINT_SYS_SYSTEM_STATUS` – CPU load, memory, tasks, uptime |

### MintOS sounds (original)
`tools/make_sounds.py` synthesises every sound at build time into `build/sounds/*.pcm`, which `kernel/sound/sounds.S` embeds. The tool uses only the Python standard library, a fixed-seed LCG, additive bells, filtered noise, Karplus-Strong and a small Schroeder reverb. Nothing is sampled and nothing imitates another platform.
- **Startup chime "Mint Morning":** an airy rising D–A–E–F♯–B arpeggio that settles into an open chord over a breathy pad. It is stereo, 3.05 s long and peaks at −3 dBFS.
- **UI sounds:** Trash (leaf crumple), Empty Trash, notification (E6→B6 marimba), volume tick and screenshot.
- **Five alert sounds:** Mint, Dewdrop, Fern, Ripple and Woodblock.
- Together they take 1110 KiB of the kernel image, which is now 16.2 MB of the 32 MiB span.

### Boot animation
- **`kernel/ui/splash_art.c`** is a pure integer renderer, shared by the kernel, MintDE and the host. It draws an anti-aliased folded mint leaf on a deep-green radial glow, with a thin progress bar.
- **`kernel/ui/boot_splash.c`** draws the first frame right after the GOP surface is validated. Each boot stage (paging, address spaces, devices, scheduler, ring-3 tests, desktop launch) moves the bar. Once the scheduler runs, a `splash` task fades the leaf in, eases the bar and sweeps a highlight along it at about 25 fps.
- Before MintDE maps the canvas, it calls `MINT_SYS_BOOT_SPLASH FINISH`, which draws the final frame. MintDE then **crossfades** from an identical rendering into the desktop (five steps; skipped when Reduce motion is on) and plays the chime.
- If the desktop cannot start, the old diagnostic panel replaces the animation. Serial logging is unchanged, apart from added `[splash]` lines.

### Wallpapers
- **`kernel/user/mintde/wallpapers.c`** is a procedural collection, rendered by MintDE with integer math at any size, so it adds no image data to the kernel.
  - **Mint Day** is dynamic: rolling mint hills under a sky, sun and moon that follow the RTC through night, dawn, day and dusk, with stars at night. It redraws every 15 minutes, or every 5 around dawn and dusk.
  - Designs: Leaf Light, Aurora, Fresh Dew, Mint Dunes.
  - Solid colours: Mint, Teal, Graphite, Midnight, Sand.
  - The kernel-rendered Mint Waves photos stay as ids 0–2.
- **Settings › Wallpaper** is now a picker with live thumbnails and a large preview. For Mint Day it shows a "Now showing: Night/Dawn/Day/Dusk" badge.

### Desktop extras
- **Notification Center:** click the date and time in the menu bar.
  - A glass panel holds the newest notifications (click one to open its app; Clear removes them all).
  - Widgets:
    - **Calendar:** the month, with today in the accent colour.
    - **System:** CPU load (sampled on the PIT), RAM, MintOS HD space and uptime.
    - **World Clock:** analog clocks for This Mac, London, New York and Tokyo, with EU/US daylight-saving rules. The hardware clock is read as UTC.
    - **Notes:** the first lines of `~/Documents/Notes.txt`.
- **Banners:**
  - They slide in at the top right and dismiss themselves after 5 s.
  - Hovering keeps a banner open and shows a close button. Clicking a banner opens its app.
  - Up to three stack.
- **API:** `notify_post(app, title, body)` (personality.h). It is used by:
  - Disk Utility, on erase success and failure;
  - Empty Trash, with the item count;
  - screenshots;
  - Settings, for the driver start and its Send Test Notification button;
  - Terminal `notify <text>`.
- **Settings › Notifications:** banners on/off, Do Not Disturb, notification sound, Send Test Notification and Clear All.
- **Control Center:**
  - A **Do Not Disturb** tile replaces the Input tile.
  - A new **Sound** block has a volume slider; dragging it applies the volume live and the feedback tick plays on release.
- **Volume keys:** Mute, Volume Down and Volume Up (PS/2 E0 20/2E/30, or USB keyboard-page usages 0x7F/0x81/0x80) show a volume HUD. Raising the volume unmutes.
- **Screenshots:** Command-Shift-3 saves `~/Desktop/Screenshot YYYY-MM-DD at HH.MM.SS.bmp` (24-bit BMP, written to a hidden temporary file and then renamed). It plays the screenshot sound and posts a notification.
- **Desktop items:** the files and folders in `~/Desktop` on MintOS HD appear as icons, in columns from the top right.
  - There are procedural mint folder icons and document icons with an extension badge; images get a picture.
  - Labels have two lines and a shadow. A selected item gets an accent pill.
  - Double-click opens a folder in Files and a text file in Notes; anything else is revealed in Files.
  - Return renames the item through Files. ⌘O opens it, ⌘⌫ moves it to the Trash (with the Trash sound) and the arrow keys move the selection.
  - The icons refresh within about 1 s of a change, triggered by the MintFS generation counter.
- **Settings › Sound:**
  - Sound Effects: alert sound (click to cycle and preview), alert volume, Play sound on startup, Play user interface sound effects, and Play feedback when volume is changed.
  - Output: volume, mute, output device (paths and codec) and driver status.
  - Buttons: **Start Audio Driver** (experimental hardware) or **Play Startup Sound**.
- **Terminal commands:**
  - `sound`, `sound start` and `sound stop`;
  - `volume [0-100|mute|unmute]`;
  - `play <startup|trash|emptytrash|notification|volume|screenshot|mint|dewdrop|fern|ripple|woodblock>`;
  - `beep [hz] [ms]`, a user-generated tone through `MINT_SYS_AUDIO_QUEUE`;
  - `notify <text>`.

## Tests (all in `make check` via `personality-check`)

| test | checks |
|---|---|
| `tools/test_hda.c` | 72: verb and format encoding, BDL, link reset in QEMU and hardware modes, 300 commands through wrapping CORB/RIRB, dead-CORB fallback within bounded time, stuck reset, QEMU codec, Realtek-like graph (multi-request and range connection lists, cycle, digital and unconnected pins skipped, shared DAC, EAPD, HP drive, mixer amps, AFG default amps), hostile codecs, stream descriptor |
| `tools/test_audio_mixer.c` | 29: gain curve, immediate mixing, saturation, silencing, volume re-render and mute, voice limits, a jittered DMA consumer that is bit-exact, starvation → silence → recovery |
| `tools/test_sounds.py` | every sound present, lengths and levels, no clipping, fade-out and soft onset, pitch detection (alert 880 Hz, E6/B6, chime chord), stereo, deterministic rebuild, total size |
| `tools/test_splash.py` | leaf colour, alpha fade-in, anti-aliasing, progress-bar fill at 0/500/1000, kernel and MintDE frames identical, other resolutions, shimmer |
| `tools/test_wallpapers.c` | 14: sine accuracy, phases at the right hours, palette continuity minute by minute (wraps at midnight), sun arc, stars only at night, refresh cadence, region render equals full render without writing outside it, determinism, day vs night |
| `tools/test_notify.c` | 26: queue order, banner expiry, hold on hover, stacking limit, overflow, dismiss, unread, UTF-8-safe truncation, age text |
| `tools/test_personality_contracts.py` | 37 static contracts (syscall range, safety order of the HDA stop, bounded waits, token start, boot order, no new wallpaper blobs, the 32 MiB span, original sound names, a prefs key for every field, USB volume keys, hooks) |

Host check on macOS: see `test-evidence/host-check-macos-26.5.014.0-personality-summary.txt` (all PASS; `test_context_switch.py` is Linux-only as before).

`tools/test_user_syscalls.c` and `tools/test_syscall_dispatch.c` have stubs for `mint_personality_syscall` and `mint_audio_get_info`, because the macOS wrapper hides missing symbols.

## QEMU evidence (`test-evidence/`)
The sessions ran headless and were driven by `tools/qemu_personality.py` (HMP 45464, QMP 45474, `ich9-intel-hda` + `hda-output` captured to a 48 kHz WAV):

- `qemu-26.5.014.0-personality-boot-animation.png`: the boot animation.
- `qemu-26.5.014.0-personality-boot-crossfade.png`: the crossfade into the desktop, caught mid-way.
- `qemu-26.5.014.0-personality-startup-chime-capture.wav`: the chime as captured from QEMU's HDA output.
- `qemu-26.5.014.0-personality-chime-analysis.txt`: `tools/analyze_wav.py` against `build/sounds/startup.pcm` at volume 70. Correlation is **1.00000**, the worst sample error is **0**, and there are no dropouts, no clipping and silence before the sound. The capture is bit-exact.
- `…-banner-disk-utility.png`: the "MintOS HD is ready" banner after Erase.
- `…-screenshot-banner.png`: the ⌘⇧3 banner with the screenshot on the desktop.
- `…-notification-center.png`: notifications plus the calendar, system, world clock and Notes widgets.
- `…-settings-sound.png`, `…-settings-wallpaper.png` and `…-settings-wallpaper-dynamic.png`: the Settings panes; the last shows Mint Day at night with the "Now showing: Night" badge.
- `…-wallpaper-mint-day.png`: the Mint Day wallpaper on the desktop.
- `…-control-center.png`: the Sound slider and the Do Not Disturb tile.
- `…-volume-hud.png`: the volume HUD after Volume Up on the USB keyboard (HMP `sendkey volumeup`).
- `qemu-26.5.014.0-personality-com1.log`: COM1, with the `[splash]`, `[audio]` and `[MintDE] notification:` lines.

## Merge notes for the coordinator
- **Shared files touched**, each with small, localised edits:
  - `kernel/kernel.c`: splash begin and progress, the diagnostic panel only on failure, `mint_hda_probe`, `mint_personality_timer_init`, and the splash and audio tasks.
  - `kernel/arch/x86_64/arch.c`: one IRQ0 hook, `mint_timer_irq_hook(rip, cs)`.
  - `kernel/abi/syscall.c`: one range check plus a declaration.
  - `kernel/driver/catalog.c`: row 13.
  - `kernel/include/log.h` and `kernel/console/log.c`: AUDIO = 14.
  - `kernel/usb/xhci.c`: one line maps the HID keyboard-page usages Mute/Volume Up/Volume Down (0x7F/0x80/0x81) to the PS/2 extended codes.
  - `kernel/user/desktop.c`: the hooks listed above.
  - `kernel/user/mintde/apps.c`: the `SET_SOUND`/`SET_NOTIFICATIONS` enum and arrays, pane dispatch, Trash and Disk Utility hooks, `files_open_location` and `notes_open_document`, the Terminal hook.
  - `kernel/user/mintde/mintde.h`: two includes.
  - `Makefile`: one block after `EFI_FLAGS`, plus `.DEFAULT_GOAL := all`.
- **Expected conflicts:**
  - The Settings enum: data-saving adds Users & Groups. Keep both, with `SET_SOUND, SET_NOTIFICATIONS` before `SET_COUNT`, and update the Spotlight map in `desktop.c` (Sound = 9, Notifications = 10 on this branch).
  - The log component enum (12, 13 and 15 come from the other sessions).
  - Notes: data-saving moved Notes into `notes.c` with a public `notes_open_file`. `notes_open_document` here then becomes `notes_open_file(path); shell_open_app(APP_NOTES);`, and the Notification Center Notes widget reads `~/Documents/Notes.txt`, so point it at the new notes store if that path changes.
  - `apps.c` and `desktop.c` hunks near the same lines as data-saving's.
- **Prefs:** every persisted personality setting is in `mde_personality_prefs personality_prefs` (`kernel/user/mintde/personality.h`). `personality_pref_keys[]` maps each field to a key:
  - `sound.volume`, `sound.muted`, `sound.alert`, `sound.alert_volume`, `sound.startup`, `sound.ui`, `sound.volume_feedback`;
  - `desktop.wallpaper`, `desktop.items`;
  - `notifications.banners`, `notifications.dnd`, `notifications.sound`.

  To wire it up:
  1. After `personality_init()`, call `prefs_get_int(key, *field)` for each entry, then `personality_prefs_apply()`.
  2. Save with `prefs_set_int` whenever `personality_prefs_generation` changes.

  On this branch, settings last for the session only; the kernel keeps the volume across MintDE restarts.
- **IRQ0 hook:** if app-platform changes the IRQ path or adds preemption, keep calling `mint_timer_irq_hook(frame->rip, frame->cs)` after the PIT EOI, with interrupts disabled.

## Known limits
- **Audio:**
  - Output only: no recording, no HDMI audio, no jack detection, and no unsolicited responses.
  - The mixer runs at 48 kHz only; there is no resampling.
  - Real hardware is untested. The ALC3234/ALC255 path is programmed generically, and Dell headset-mode COEF quirks are not applied, so the headphone jack may stay silent while line out and speakers work. It is experimental and starts only on request.
- **Volume keys:** PS/2 extended codes and the USB keyboard-page usages work. Many USB keyboards send media keys on the separate Consumer page, which the boot-protocol HID driver does not read.
- **CPU load:** a statistical sample per 10 ms tick, so it is approximate.
- **World Clock:** assumes the hardware clock is UTC. There is no timezone setting yet.
- **Wallpaper picker:** the Mint Waves photo thumbnails are drawn stand-ins, not the photos themselves.
- **Screenshots:** saved as 24-bit BMP, about 6 MB at 1080p; MintOS has no image viewer yet, so Files reveals them.
- **Persistence:** personality settings are not persisted until merged with the prefs store.
