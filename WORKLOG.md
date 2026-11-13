# MintOS26 26.5.014.0 / MintDE 2.4.0 worklog

Previous worklog: `docs/WORKLOG-26.5.013.0.md`. Per-topic details, decisions and known limits are in `docs/notes-26.5.014.0-{data-saving,app-platform,web-engine,personality}.md`.

## Goal

The user asked for several topics at once:
- "proper data saving";
- "working webpages";
- "software";
- "more individual personality".

They chose to run them in parallel, the web engine before a NetSurf port, and all four personality areas with icons deferred to 26.6/26.7. They also approved downloading stb_image, musl and Lua.

## How it was built

- **Branches:** 26.5.013.0 was committed first (`d3e23ac`). Four sessions then worked in separate git worktrees (`worktrees/<topic>`, branches `topic/<topic>`):
  - data-saving;
  - app-platform;
  - web-engine;
  - personality.
- **Merge plan:** syscall ranges and log components were reserved per topic up front, and only the coordinator changed versions, so the branches merged with small, mechanical conflicts.
- **Usage limits:** they paused the sessions several times. The work continued two sessions at a time, with WIP commits so nothing was lost.
- **Merging:** the branches were merged into `dev/26.5.009.0` in this order: data-saving, app-platform, web-engine, personality. The combination was built after each merge.

## Merge conflicts and integration fixes

- **Conflicts:**
  - **Log component enums:** union of 12, 13 and 14.
  - **`MINT_ASPACE_MAX_USER_FRAMES`:** app-platform's header (frame limits and the 256-region table) was kept.
  - **Makefile object lists:** both kept.
  - **Settings pane enum:** Power, Sound, Notifications, then Users & Groups. Spotlight's pane map was extended.
  - **File menu items** for Notes and Web: both kept.
  - **IRQ0:**
    - Personality's timer hook (mixer and CPU sampling) now runs *before* app-platform's ring-3 preemption.
    - That way it always runs at interrupt time with interrupts disabled, never after a task switch.
- **Integration:**
  - **Personality settings** (volume, alert, startup sound, wallpaper, notifications, desktop items) are now read from and written to the data-saving preferences store. A saved volume wins over the kernel default after a reboot.
  - **The Web app** had `/Users/mintos/…` hard-coded for Downloads, Favorites and Cookies. With accounts, the home is `/Users/<account>`, so the paths are now built from the volume's home folder, and a contract test guards it. A finished download also posts a notification.
  - **Web page heap:** raised from 8 to 32 MiB. app-platform's frame pool removes the old ~14 MiB physmem cap, and MintDE is now about 37 MB of its 64 MiB.
  - **Notes widget:** Notification Center's Notes widget now shows the most recently edited note in `~/Documents/Notes`, not the old single `Notes.txt`.
  - **Clock label:** the World Clock label "This Mac" became "Local".
  - **Startup-order contract:** `test_data_saving_contracts.py` now accounts for `personality_init()` between the appearance restore and the wallpaper.

## Validation

- **Host:** every `make check` command passes on macOS except the Linux-only `tools/test_context_switch.py`. The final summary is in `test-evidence/host-check-macos-26.5.014.0-summary.txt`.
- **Combined QEMU 11.1 q35/OVMF pass:** a fresh blank 1 GiB disk, with the ich9-intel-hda output captured to WAV.
  - **First boot:** boot animation, then the Setup Assistant.
    - It initialized MintOS HD and the bundled programs were installed.
    - It created the account `alexmint` (`/Users/alexmint`), and Dark with the purple accent was applied.
  - **Terminal:** the prompt `alexmint@MintOS %`, then `hello` (HOME is the account's home), `sysinfo` (preemption, FPU and Linux compat), and `lua t.lua` with floats and strings plus the bundled primes example.
  - **Other apps:** Notes made a new note. Web showed Wikipedia "Mentha" over HTTPS with styling, the infobox and its photo. Settings › Wallpaper set Aurora.
  - **Restart** from the Apple menu. COM1 shows `[power] saving MintOS HD before power-off`. After the reboot everything came back: the account home, Dark with purple, the Aurora wallpaper, and the note, which Notification Center's Notes widget also shows.
  - **The startup chime** was captured on both boots (≈2.7 s audible, peak −9.2 dBFS).
  - **Host fsck:** clean. The preference files for files, notes, desktop, sound and notifications are there, and Web cookies are under the account's home.
  - Evidence: `test-evidence/qemu-26.5.014.0-combined-*`.
- **Not validated:** VirtualBox and the physical OptiPlex 7040 (HDA on the ALC3234 and the I219 are experimental there).
- **Known limits:** listed in each topic note. In short:
  - no JavaScript, SVG, web fonts or CSS grid;
  - Greek and other non-Latin scripts show as `?`;
  - BBC News renders only partly;
  - audio is output only, at 48 kHz;
  - GUI apps are not separate processes yet.
