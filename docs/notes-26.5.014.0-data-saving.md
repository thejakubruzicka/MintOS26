# 26.5.014.0 — Data saving and the Setup Assistant (branch `topic/data-saving`)

MintOS now remembers things on MintOS HD the way macOS does. This covers:
- appearance, network auto-connect, Files' folder and Notes' note;
- a real account with its own home folder;
- many notes;
- the Terminal history.

Shut Down and Restart save everything first. The file formats and the API are in `docs/DATA-SAVING.md`.

## What changed

### Preferences store
New file: `kernel/user/mintde/prefs.c`; the API is in `userdata.h`, which `mintde.h` includes.
- **Keys:** a key is `<domain>.<name>`, for example `prefs_get_int("sound.volume", 75)` and `prefs_set_int(...)`. Adding a key is one line at the call site, with no registry to edit.
- **Files:** `system.*` keys go to `/Library/Preferences/com.mintos.system.prefs`. Every other domain goes to `~/Library/Preferences/com.mintos.<domain>.prefs`. These are text files of `name = value` lines with quoted strings.
- **Saving:** loaded lazily per domain. Written with the atomic `mint_file_save` about one second after the last change, or at once by `prefs_flush()`.
- **Without a MintOS disk:** everything works in memory for the session.
- **Saved now:**
  - appearance (theme, accent, UI scale, wallpaper, reduce motion; restored before the first frame);
  - `system.network_autoconnect`;
  - `files.last_folder`;
  - `notes.selected`.
  - Terminal history goes to `~/.mintsh_history` (up to 100 commands; Up/Down walk it).

### Accounts and the Setup Assistant
New files: `kernel/user/mintde/accounts.c` and `kernel/user/mintde/setup.c`.
- **When it appears:** on first boot with MintOS HD and no account, or with no MintOS disk but a blank disk, MintDE shows a full-screen Setup Assistant. It runs over the blurred wallpaper.
- **Welcome:** a large "Hello" crossfades through ten languages, next to the MintOS leaf.
- **MintOS HD:** offered only without a MintOS disk. Only blank disks are listed. The page reuses Disk Utility's typed `FORMAT sdX` confirmation and `mint_storage_init_mintos`. **Not Now** continues for the session only.
- **Account:**
  - Full name and account name. The account name is suggested from the full name, with accents removed and ß written as ss.
  - Both fields are validated live with explanations. An avatar shows the initials.
  - Continue creates `/Users/<account>` with Desktop, Documents, Downloads, Movies, Music, Pictures, Public, Library/Preferences and `.Trash`.
  - The previous home (the default `/Users/mintos`, or the old folder when renaming) is renamed into place, so files saved before setup are kept.
  - The account goes to `/Library/Preferences/com.mintos.accounts`.
- **Look:** Light, Dark or Auto plus the accent colour, previewed live on the whole screen.
- **Done:** a summary of the account, the home folder and where things are saved.
- **Kernel:**
  - At mount, `mint_mintos_disk_probe` reads the accounts file. A valid `home = "/Users/<account>"` that exists becomes `MINT_MFS_VOLUME.home`; otherwise the default is kept and a `PREFS` warning is logged.
  - New syscall `MINT_SYS_MFS_SET_HOME` (`0x4D50`, runtime `mint_fs_set_home`) switches the home right after setup. It accepts only an existing `/Users/<valid account>` folder.
  - Account-name rules are shared: `mint_account_name_valid` in the kernel and `account_name_valid` in MintDE.
- **Where the account shows up:**
  - **Terminal:** the prompt is `<account>@MintOS %`, and the window title is `<account> — mintsh — MintOS HD`.
  - **Files:** Favorites names the home after the account.
  - **Settings:** an account card sits at the top of the sidebar (avatar, full name, "MintOS Account"). The new **Users & Groups** pane shows the account name, home, shell and storage, with **Run Setup Assistant…**. Running it again can rename the account, and the home folder moves with it.
- **Disk Utility:** after initializing a MintOS disk, preferences and apps follow the new home. If there is no account yet, it points to Users & Groups.
- **Log component:** 12 is `PREFS`.

### Notes with many notes
New file: `kernel/user/mintde/notes.c`. The old single-note code was removed from `apps.c`.
- **Storage:** each note is a text file in `~/Documents/Notes/`. That location was chosen over `~/Library` so notes stay visible in Files and Terminal. The first line is the title and the file name; the file is renamed when the title changes.
- **The list:**
  - sorted by last edit;
  - grouped Today / Yesterday / Previous 7 Days / Previous 30 Days / Earlier;
  - each row shows the time or date and a preview;
  - a toolbar search field filters titles and note text.
- **Commands:**
  - New Note: ⌘N, the File menu or the compose button.
  - Delete: ⌘⌫, the File menu or the trash button. It moves the note to `~/.Trash`.
  - A new note left empty disappears, as in macOS Notes.
- **Saving:** about 1.2 s after typing stops, and before power-off.
- **Opening files:** Files still opens any text file in Notes. It is edited in place, listed under "Opened from Files" and never renamed.
- **Migration:** the 26.5.013.0 `~/Documents/Notes.txt` and a session-RAM note become notes the first time.
- **Without MintOS HD:** one session note, as before.

### Safe shutdown
- **Kernel:** `kernel/power/acpi.c` gained `mint_power_set_flush_hook`. `mint_power_shutdown` and `mint_power_restart` run the hook before `cli` and before any power register. `kernel.c` registers `mint_mfs_flush_for_power` (MintFS commit plus FLUSH CACHE) after mounting. A failed flush is logged but cannot block power-off.
- **MintDE:** the power dialog calls `app_save_all()` (Notes, Terminal history, preferences, then `mint_fs_flush`) before `mint_power_request`. The alert now says "Your notes and settings are saved on MintOS HD first." when a disk is present.

## Tests
- **`tools/test_data_saving.c`** (new, in `make check`): 49 checks. It covers:
  - the preferences parser and serializer (escapes, malformed lines, limits, round trip, ordering);
  - account rules, where MintDE and the kernel agree on 18 names;
  - name suggestions, initials, and the accounts-file round trip;
  - the kernel's parser refusing 8 tampered homes.

  It then runs the real `prefs.c` and `accounts.c` on the real block layer, `mintos_disk.c` and MintFS on a RAM disk:
  - session-only behaviour;
  - debounce timing;
  - the system and user files;
  - account creation moving `/Users/mintos`, with the layout;
  - a remount restoring the home, the account and every preference;
  - set-home validation and an account rename;
  - an invalid accounts file being ignored and logged;
  - the power-off flush hook (data written without close is lost on a cut and kept after the hook);
  - a final fsck.
- **`tools/test_data_saving_contracts.py`** (new, in `make check`): 29 checks. They cover:
  - the syscall number and range, user-copy bounds and harness stubs;
  - the log component;
  - the boot and shell wiring;
  - the Setup Assistant's blank-disk and typed-confirmation rules;
  - atomic-only preference writes;
  - the persisted keys, the Notes layout and commands;
  - flush ordering in both kernel power paths and in the MintDE dialog;
  - MintDE's memory budget (5.7 MiB of 8 MiB).
- **Other test changes:**
  - `tools/test_user_syscalls.c`: two set-home cases plus a stub.
  - `tools/test_syscall_dispatch.c`: a stub.
  - `tools/test_mintfs_contracts.py`: its Notes check was updated to the new Notes.
- **Full run:** `PATH="$PWD/tools/macos-host:/opt/homebrew/opt/binutils/bin:$PATH" make check` passes on macOS except `tools/test_context_switch.py`, which is Linux-only and always fails on macOS (as before).

## QEMU evidence (QEMU 11.1, q35/OVMF, 1920×1080, fresh blank 1 GiB disk)

Screenshots are `test-evidence/qemu-26.5.014.0-data-saving-*.png`.

**Boot 1** (`…-com1-boot1.log`):
- `01` Welcome ("Ahoj").
- `02`/`03` MintOS HD page: only sdc offered, `FORMAT sdc` typed.
- `04` "MintOS HD Is Ready".
- `05` Account: "Jakub Ruzicka" suggested `jakubruzicka`, edited to `jakub`.
- `06`/`07` Look: Dark and Violet, previewed live.
- `08` Done.
- `09` Desktop.
- `10` Notes: two notes under Today.
- `11` Terminal: `jakub@MintOS %`, `pwd` → `/Users/jakub`, the saved prefs file.
- `12` Users & Groups, after switching to the Light wallpaper.
- `13` Files in Documents.
- `14` Restart dialog. COM1 then shows, in order:
  - `[MintDE] notes, history and preferences saved before power-off`
  - `[power] saving MintOS HD before power-off`
  - `[mintfs] final commit before power-off: clean`
  - `[power] restart requested`

**Host fsck** (`…-host-fsck.txt`): clean. The disk has the accounts file, the desktop/files/notes preference files, `.mintsh_history` and both notes. `/Users/mintos` was moved into `/Users/jakub`.

**Boot 2** (cold boot, `…-com1-boot2.log`):
- COM1: `[mintfs] home folder /Users/jakub (from the MintOS account)`. No Setup Assistant.
- `15` Dark theme and the Light wallpaper restored.
- `16` Notes restored, with the selected note.
- `17` Files reopened in Documents, Violet accent.
- `18` Terminal history recalled with Up.
- `19` Shut Down dialog. The same flush chain appears, then `[power] entering ACPI S5`, and QEMU powered off.
- Host fsck afterwards: clean, generation 80.

**Changed after the evidence run:** three cosmetic Setup fixes. The card is now opaque (a shadow line showed through), the FORMAT prompt lost its colon to a short buffer, and the check mark on "MintOS HD Is Ready" is larger. Also, `prefs_set_home` now rewrites only domains that hold values (the run wrote an empty `com.mintos.system.prefs`). The host tests were rerun after these changes; the screenshots predate them.

## Known limits
- **One account per MintOS disk:** no passwords, no login window and no Log Out yet. Accounts are not a permission boundary: the kernel accepts any existing `/Users/<valid>` folder as home.
- **The Setup Assistant cannot be skipped** while MintOS HD has no account. Automated QEMU runs on a disk that was initialized without an account (for example by another branch's tests) will meet it once.
- **Notes editor:**
  - The caret stays at the end: typing, Return and Backspace only, with no cursor movement or selection (unchanged from 26.5.013.0).
  - A note is at most 16 KiB, and 128 notes are listed.
  - Search covers the open note's whole text but only the first ~500 bytes of the others.
- **Preference values are under 256 bytes.** For example, a Files folder path longer than that is not remembered.
- **QEMU automation notes:**
  - After an in-guest warm reset, QEMU's HMP `sendkey` goes to the USB keyboard, which the firmware boot selector does not read, so the evidence used a cold boot for the second start. The in-guest restart itself worked (firmware rebooted).
  - QMP double-clicks in Files did not register in my driver. Not investigated; single clicks and the Favorites sidebar were used instead.
- **Spotlight** does not list Users & Groups yet.
- **Not validated** on VirtualBox or the OptiPlex 7040.

## Notes for the coordinator (shared files touched)
- **`kernel/user/mintde/apps.c`:**
  - Notes code removed (now `notes.c`).
  - Settings enum gains `SET_USERS` (appended after `SET_POWER`) and the sidebar starts 56 px lower for the account card.
  - Terminal prompt, title and history next to `terminal_key`.
  - Files home label, last folder and Disk Utility success text.
  - `app_save_all` and `app_home_changed` at the end.
  - Dispatch hooks for Notes, prefs and history.
- **`kernel/user/desktop.c`:**
  - `userdata_start()` and `setup_boot_check()` in `mint_user_main`.
  - The setup branch in `compose`, `handle_event` and `tick`.
  - `userdata_store_appearance()` in `shell_apply_settings`.
  - ⌘N passthrough and the File menu Notes items (`ACT_NOTES_BASE = 300`).
  - `app_save_all()` in `run_power`, and the power alert caption.
- **`mintde.h`:** an include of `userdata.h` and a declarations block at the end.
- **Makefile:** `MINTDE_DATA_OBJS` and a pattern rule, plus the two `make check` lines after `test-mintos-disk`.
- **Kernel:**
  - `mintos_fs.h`, `mintos_disk.c`;
  - `syscall.h` (`0x4D50`), `syscall.c`;
  - `acpi.c`, `power.h`, `kernel.c` (one line);
  - `log.h`/`log.c` (component 12);
  - `runtime.c`/`runtime.h`.
- **No version strings were changed.**
- **`MINT_ASPACE_MAX_USER_FRAMES` is unchanged** (2048). MintDE uses about 5.7 MiB on this branch.
