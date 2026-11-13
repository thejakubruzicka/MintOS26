#!/usr/bin/env python3
"""Source contracts for data saving (26.5.014.0): preferences, accounts and the
Setup Assistant, Notes with many notes, and safe shutdown. Behaviour is covered
by tools/test_data_saving.c (preferences, accounts and the power-off flush on
the real MintFS); these checks pin the wiring that host tests cannot reach:
syscall numbers and bounds, boot and shell integration, the order of the
flushes before power-off, and the release rules for parallel branches."""
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rd = lambda p: (root / p).read_text(encoding="utf-8")
uapi = rd("kernel/include/uapi/mintos/syscall.h")
syscall = rd("kernel/abi/syscall.c")
runtime_c = rd("kernel/user/runtime.c")
runtime_h = rd("kernel/user/include/mintos/runtime.h")
fs_h = rd("kernel/include/mintos_fs.h")
disk = rd("kernel/fs/mintos_disk.c")
acpi = rd("kernel/power/acpi.c")
kernel = rd("kernel/kernel.c")
log_h = rd("kernel/include/log.h")
log_c = rd("kernel/console/log.c")
prefs = rd("kernel/user/mintde/prefs.c")
accounts = rd("kernel/user/mintde/accounts.c")
setup = rd("kernel/user/mintde/setup.c")
notes = rd("kernel/user/mintde/notes.c")
apps = rd("kernel/user/mintde/apps.c")
desktop = rd("kernel/user/desktop.c")
mintde_h = rd("kernel/user/mintde/mintde.h")
userdata_h = rd("kernel/user/mintde/userdata.h")
aspace = rd("kernel/include/address_space.h")
make = rd("Makefile")
stub_user = rd("tools/test_user_syscalls.c")
stub_dispatch = rd("tools/test_syscall_dispatch.c")

def between(s, a, b):
    i = s.index(a)
    return s[i:s.index(b, i)]

def before(s, first, second):
    return first in s and second in s and s.index(first) < s.index(second)

shutdown = between(acpi, "int mint_power_shutdown(void) {", "static void reset_via_acpi(")
restart = between(acpi, "int mint_power_restart(void) {", "#endif")
run_power = between(desktop, "static void run_power(void) {", "/* ------------------------------------------------------------ compose")
save_all = between(apps, "void app_save_all(void) {", "void app_home_changed(void) {")
probe = between(disk, "void mint_mintos_disk_probe(void) {", "/* ------------------------------------------------------------------ initialization")
set_home = between(disk, "int64_t mint_mfs_set_home(const char *path) {", "int mint_mfs_flush_for_power(void) {")
create = between(accounts, "int64_t account_create(", "\n}\n")

def elf_memory(path):
    """Bytes of the LOAD segments of a 64-bit ELF (0 if it is not built)."""
    p = root / path
    if not p.exists():
        return 0
    b = p.read_bytes()
    phoff, = struct.unpack_from("<Q", b, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", b, 0x36)
    total = 0
    for i in range(phnum):
        ptype, = struct.unpack_from("<I", b, phoff + i * phentsize)
        if ptype == 1:
            memsz, = struct.unpack_from("<Q", b, phoff + i * phentsize + 0x28)
            total += (memsz + 4095) // 4096 * 4096
    return total

frames = int(aspace.split("#define MINT_ASPACE_MAX_USER_FRAMES")[1].split("u")[0])
desktop_bytes = elf_memory("build/user-desktop.elf")

checks = {
    # --- ABI: reserved number, bounds, wrapper and harness stubs
    "MINT_SYS_MFS_SET_HOME is 0x4D50, inside the data-saving range 0x4D50-0x4D57":
        "#define MINT_SYS_MFS_SET_HOME                  0x4D50u" in uapi and
        not any(f"0x4D5{d}u" in uapi.replace("0x4D50u", "") for d in "1234567"),
    "the set-home syscall copies a bounded user string and the kernel validates the path":
        "copy_user_cstring(space, arg0, home_path, sizeof(home_path))" in syscall and "mint_mfs_set_home(home_path)" in syscall and
        "mint_account_name_valid(path + i)" in set_home and "is_directory(path)" in set_home,
    "runtime wrapper mint_fs_set_home exists and is declared":
        "int64_t mint_fs_set_home(const char *path)" in runtime_c and "int64_t mint_fs_set_home(const char *path);" in runtime_h,
    "both syscall harnesses stub mint_mfs_set_home (macOS dynamic_lookup hides missing symbols)":
        "mint_mfs_set_home(const char *" in stub_user and "mint_mfs_set_home(const char *" in stub_dispatch and
        "MINT_SYS_MFS_SET_HOME" in stub_user,
    "log component 12 is PREFS":
        "MINT_LOG_COMPONENT_PREFS = 12" in log_h and 'case MINT_LOG_COMPONENT_PREFS: return "PREFS";' in log_c,
    # --- accounts and the home folder
    "the kernel reads the account's home from /Library/Preferences/com.mintos.accounts when it mounts":
        '#define MINT_ACCOUNTS_PATH "/Library/Preferences/com.mintos.accounts"' in fs_h and
        "if (mount_partition(i, first, last)) { load_account_home(); return; }" in probe,
    "an unusable accounts file keeps the default home and is logged":
        "keeping the default home folder" in disk and "MINT_LOG_COMPONENT_PREFS" in disk,
    "account names follow one rule set in the kernel and MintDE (reserved names included)":
        all(n in disk and n in accounts for n in ['"root"', '"daemon"', '"nobody"', '"guest"', '"shared"', '"system"', '"wheel"']) and
        "MINT_ACCOUNT_NAME_MAX 31u" in fs_h,
    "creating an account builds the macOS home layout and moves the previous home instead of copying":
        all(f'"{d}"' in create or f'"{d}"' in accounts for d in ["Desktop", "Documents", "Downloads", "Movies", "Music", "Pictures", "Library", ".Trash"]) and
        "mint_path_rename(v.home, a.home)" in create and "!exists(a.home)" in create,
    "the accounts file is written atomically before the kernel's home changes":
        before(create, "mint_file_save(MINT_ACCOUNTS_PATH", "mint_fs_set_home(a.home)"),
    "Terminal prompt and window title use the account name":
        'account_short_name()); str_append(prompt, sizeof(prompt), "@MintOS % ")' in apps and
        '" \\xE2\\x80\\x94 mintsh \\xE2\\x80\\x94 MintOS HD"' in apps,
    "Files' Favorites name the home after the account folder, not a fixed name":
        "home_leaf" in apps and "MINT_MFS_HOME_NAME, sym[i]" not in apps,
    "Settings has Users & Groups and an account card that can rerun the Setup Assistant":
        '"Users & Groups"' in apps and "users_card_paint(settings_account_rect(l)" in apps and
        "setup_begin(account.present)" in setup,
    # --- Setup Assistant
    "MintDE starts with the saved preferences and shows the Setup Assistant on first boot":
        before(desktop, "userdata_start();", "theme_apply(&desktop_settings, night_mode);") and
        before(desktop, "userdata_start();", "personality_init();") and          # saved sound/wallpaper prefs
        before(desktop, "personality_init();", "render_wallpaper();\n    app_init();") and
        "setup_boot_check();" in desktop and "if (!account.present) setup_begin(0);" in setup,
    "the Setup Assistant owns the screen and the input while it runs":
        "if (setup_active()) { setup_paint(d); return; }" in desktop and "setup_click(pointer_x, pointer_y)" in desktop and
        "setup_key(&e->data.key)" in desktop,
    "Setup only offers blank disks and initializes them through the typed FORMAT confirmation":
        "d->mbr_signature_ok" in setup and "mint_storage_init_mintos(&rq)" in setup and '"FORMAT "' in setup and
        "phrase_matches()) run_disk_init();" in setup,
    "Setup flushes pending data before the home moves":
        before(setup, "app_save_all();", "account_create(full_name, acct, &migrated)") and "prefs_set_home(account.home)" in setup,
    # --- preferences
    "preferences are written only with the atomic mint_file_save, per domain, in the macOS locations":
        "mint_file_save(path_buf, file_buf, (size_t)n)" in prefs and "mint_file_open" not in prefs and
        '"/Library/Preferences/com.mintos.system.prefs"' in prefs and '"/Library/Preferences/com.mintos."' in prefs,
    "saving is debounced (about one second after the last change)":
        "PREFS_SAVE_DELAY_MS 1000u" in prefs and "prefs_background(now)" in apps,
    "appearance, network auto-connect, Files' folder and Notes' note are persisted":
        all(k in setup for k in ['"desktop.theme"', '"desktop.accent"', '"desktop.ui_scale"', '"desktop.wallpaper"', '"desktop.reduce_motion"']) and
        '"system.network_autoconnect"' in apps and '"files.last_folder"' in apps and '"notes.selected"' in notes and
        "userdata_store_appearance();" in desktop,
    "Terminal history is kept in ~/.mintsh_history and walked with the arrow keys":
        '"/.mintsh_history"' in apps and "term_history_add(terminal_input)" in apps and "k->scancode == 0x150u" in apps,
    # --- Notes
    "Notes keeps many notes in ~/Documents/Notes, sorted by edit time, grouped and searchable":
        '"/Documents/Notes"' in notes and all(s in notes for s in ['"Today"', '"Yesterday"', '"Previous 7 Days"', '"Previous 30 Days"']) and
        "notes[j - 1u].mtime < t.mtime" in notes and "ci_has(notes[i].title, search)" in notes,
    "Command-N makes a note when Notes is focused; Delete moves a note to the Trash":
        "if (focused_app == APP_NOTES) break;" in desktop and '"New Note"' in desktop and
        'copy_text(trash, sizeof(trash), notes_home); str_append(trash, sizeof(trash), "/.Trash");' in notes and
        "mint_path_rename(open_path, work)" in notes,
    "Files still opens text files in Notes":
        "notes_open_file(files_work2); shell_open_app(APP_NOTES);" in apps,
    # --- safe shutdown
    "the kernel flush hook runs before interrupts are disabled in both Shut Down and Restart":
        before(shutdown, "power_flush();", '__asm__ volatile("cli"') and before(restart, "power_flush();", '__asm__ volatile("cli"'),
    "the kernel registers the MintFS final commit as the power flush hook after mounting":
        before(kernel, "mint_mintos_disk_probe();", "mint_power_set_flush_hook(mint_mfs_flush_for_power);") and
        "mintfs_commit(&fs)" in between(disk, "int mint_mfs_flush_for_power(void) {", "int mint_mfs_volume("),
    "the MintDE power dialog saves Notes, history and preferences before asking the kernel":
        before(run_power, "app_save_all();", "mint_power_request(action)") and
        all(s in save_all for s in ["notes_flush()", "term_history_save()", "prefs_flush()", "mint_fs_flush()"]),
    # --- memory budget
    "MintDE stays within the user frame limit (only 2048 or 16384 frames allowed)":
        frames in (2048, 16384) and (desktop_bytes == 0 or desktop_bytes + 64 * 1024 <= frames * 4096),
    "make check runs the data-saving host test and these contracts":
        "tools/test_data_saving.c kernel/user/mintde/prefs.c kernel/user/mintde/accounts.c" in make and
        "tools/test_data_saving_contracts.py" in make and "userdata.h" in mintde_h and "prefs_parse_text" in userdata_h,
}

failed = 0
for name, ok in checks.items():
    print(("PASS: " if ok else "FAIL: ") + name)
    failed += not ok
print(f"{len(checks)} data-saving contracts, {failed} failures")
if desktop_bytes:
    print(f"MintDE image: {desktop_bytes // 1024} KiB of {frames * 4 // 1024} MiB")
raise SystemExit(1 if failed else 0)
