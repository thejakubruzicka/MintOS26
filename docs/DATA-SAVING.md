# How MintOS remembers things (26.5.014.0)

Everything is stored as small text files on **MintOS HD** (MintFS), written with the atomic `mint_file_save`: a temporary `.~name` is written first and then renamed over the old file, so a power cut leaves either the old or the new version. Without a MintOS disk, MintDE keeps everything in memory for the session.

## Where things live

| What | File |
|---|---|
| The account | `/Library/Preferences/com.mintos.accounts` |
| Settings for the whole computer | `/Library/Preferences/com.mintos.system.prefs` |
| The account's settings | `~/Library/Preferences/com.mintos.<domain>.prefs` (`desktop`, `files`, `notes`, …) |
| Notes | `~/Documents/Notes/<title>.txt`, one plain text file per note |
| Terminal history | `~/.mintsh_history`, one command per line (like zsh's `~/.zsh_history`) |

`~` is the account's home, `/Users/<account>`.

## File format

UTF-8 lines of `name = value`:

```
# com.mintos.desktop — MintOS preferences, written by MintDE.
accent = 2
theme = 1
wallpaper = 1
```

- A value is an integer (`-12`, `true`/`false` read as 1/0) or a double-quoted string with `\"`, `\\`, `\n`, `\t` and `\r` escapes.
- `#` starts a comment. Unknown or malformed lines are skipped, so a file from a newer MintOS still loads.
- MintDE writes keys in alphabetical order with a short header.
- Limits: names are lowercase `a-z 0-9 _ - .` (a key is at most 63 bytes including its domain); values are under 256 bytes; 160 values and 24 domains in total.

## Preferences API (`kernel/user/mintde/userdata.h`)

A key is `<domain>.<name>`. The domain picks the file: `system.*` goes to `/Library/Preferences/com.mintos.system.prefs`, anything else to `~/Library/Preferences/com.mintos.<domain>.prefs`. A domain's file is read the first time one of its keys is used.

```c
int64_t volume = prefs_get_int("sound.volume", 75);   /* fallback if unset */
prefs_set_int("sound.volume", volume);                 /* saved ~1 s later */
const char *page = prefs_get_str("web.home_page", "https://example.org");
prefs_set_str("web.home_page", page);
```

**Adding a key is one line at the call site.** There is no registry to edit. Pick a domain named after your feature so its keys get their own file.

- `prefs_set_*` returns 1 when the value is stored. Setting the same value again does nothing.
- MintDE calls `prefs_background()` every frame. It saves the changed domains about one second after the last change.
- `prefs_flush()` saves at once. It runs before Shut Down, Restart and a change of the account.

Keys used in 26.5.014.0:

| Key | Meaning |
|---|---|
| `desktop.theme`, `desktop.accent`, `desktop.ui_scale`, `desktop.wallpaper`, `desktop.reduce_motion` | Appearance (restored when MintDE starts) |
| `system.network_autoconnect` | Connect Ethernet when MintDE starts |
| `files.last_folder` | The folder Files opens at |
| `notes.selected` | The note Notes opens at |

## The account

```
version = 1
account = "jakub"
full_name = "Jakub Ruzicka"
home = "/Users/jakub"
created = 1794499941
```

- **At mount:** the kernel reads the `home` line. If it names an existing `/Users/<valid account>` folder, that folder becomes the volume's home (`MINT_MFS_VOLUME.home`). Otherwise the kernel keeps `/Users/mintos` and logs a `PREFS` warning.
- **Changing the home at run time:** `MINT_SYS_MFS_SET_HOME` (`0x4D50`, runtime `mint_fs_set_home`) does it. The kernel checks the same rules.
- **Account names:** 1–31 bytes of `a-z 0-9 _ - .`, starting with a letter. These names are reserved: `root`, `daemon`, `nobody`, `guest`, `shared`, `system`, `wheel`. `mint_account_name_valid` (kernel) and `account_name_valid` (MintDE) are tested to agree.
- **Creating an account** makes `/Users/<account>` with Desktop, Documents, Downloads, Movies, Music, Pictures, Public, Library/Preferences and `.Trash`.
- **Moving the previous home:** if the previous home exists and the new folder does not, the previous home is **renamed**, not copied. That is the default `/Users/mintos` from 26.5.013.0, or the account's old folder when the Setup Assistant renames the account. Files saved before setup move with it.

## Notes

- Each note is a text file in `~/Documents/Notes`.
- **Title and file name:** the first line is the title and also the file name. The file is renamed when the title changes, and `Title 2.txt` is used if the name is taken.
- **Saving:** a note is saved about 1.2 s after typing stops.
- **New and delete:** a new note that is left empty is removed. Delete moves the note to `~/.Trash`.
- **Files opened from elsewhere:** text files opened from Files outside the Notes folder are edited in place and never renamed.
- **First run:** the first time the Notes folder is created, the 26.5.013.0 `~/Documents/Notes.txt` and any session-RAM note become notes.

## Safe shutdown

1. The MintDE power dialog calls `app_save_all()`: Notes, Terminal history, preferences, then `mint_fs_flush()`.
2. The kernel's `mint_power_shutdown`/`mint_power_restart` run the registered flush hook (`mint_mfs_flush_for_power`: MintFS commit plus disk-cache flush). This happens before interrupts are disabled and before any power register is touched.
3. A failed flush is logged, but it cannot block the power-off.
