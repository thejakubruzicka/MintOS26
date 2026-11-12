#!/usr/bin/env python3
"""Source contracts for MintFS / MintOS disks: write safety, boot wiring,
syscall bounds and the MintDE integration. Behaviour is covered by
test_mintfs.c (filesystem torture), test_mintos_disk.c (block grants + file
API) and test_gpt.py (partition table); these checks pin what those host
tests cannot reach."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rd = lambda p: (root / p).read_text(encoding="utf-8")
mintfs = rd("kernel/fs/mintfs.c")
disk = rd("kernel/fs/mintos_disk.c")
block = rd("kernel/block/block.c")
block_h = rd("kernel/include/block.h")
ahci = rd("kernel/block/ahci.c")
kernel = rd("kernel/kernel.c")
process = rd("kernel/proc/process.c")
syscall = rd("kernel/abi/syscall.c")
uapi = rd("kernel/include/uapi/mintos/syscall.h")
runtime = rd("kernel/user/runtime.c")
apps = rd("kernel/user/mintde/apps.c")
notes = rd("kernel/user/mintde/notes.c")
desktop = rd("kernel/user/desktop.c")
make = rd("Makefile")

def between(s, a, b):
    i = s.index(a)
    return s[i:s.index(b, i)]

init = between(disk, "int mint_mintos_disk_initialize(", "/* ------------------------------------------------------------------ descriptors */")
fs_sys = between(syscall, "static int64_t fs_syscall(", "int64_t mint_syscall_dispatch_user(")

checks = {
    # --- write safety
    "AHCI disks stay READONLY in the block registry; multi-sector writes are reachable only via a grant":
        "dev.flags = MINT_BLOCK_FLAG_READONLY;" in ahci and "dev.write_sectors = ahci_write_sectors;" in ahci and
        "write_sectors" not in between(block, "int mint_block_write(", "int mint_block_io_stats("),
    "generic block writes still refuse READONLY devices":
        "(d->flags & MINT_BLOCK_FLAG_READONLY)" in between(block, "int mint_block_write(", "int mint_block_io_stats("),
    "granted writes check owner and the whole range before touching the device":
        "grants[index].owner != owner || lba < grants[index].first" in block and
        "count > grants[index].count - (lba - grants[index].first)" in block and "++io_stats[index].writes_blocked;" in block,
    "a grant cannot be taken over by another owner":
        "if (grants[index].active && grants[index].owner != owner) return 0;" in block,
    "initialization requires a blank disk: zeroed MBR, GPT header, both entry arrays' edges and the last sector":
        "const uint64_t probe[] = {0, 1, 2, 33, n - 33u, n - 1u};" in init and "if (!sector_zero(s)) return -MINT_EPERM;" in init,
    "initialization requires at least 64 MiB and refuses while a MintOS disk is mounted":
        "if (n < 131072u) return -MINT_EINVAL;" in init and "return -MINT_EBUSY;       /* one MintOS disk at a time */" in init,
    "the whole-disk grant used for the GPT is revoked before MintFS mounts with a partition-only grant":
        init.index("mint_block_grant_writes(index, 0, n, GRANT_OWNER)") < init.index("mint_block_revoke_writes(index, GRANT_OWNER);") <
        init.index("mount_partition(index, first, last)") and
        "mint_block_grant_writes(index, first, last - first + 1u, GRANT_OWNER)" in disk,
    "existing disks are mounted only after the GPT header and partition array CRCs verify":
        "mint_gpt_crc32(0, copy, 92) != r32(h + 16)" in disk and "mint_gpt_crc32(0, e, 16384) != r32(h + 88)" in disk and
        "MINT_GPT_MINTFS_TYPE" in disk,
    "MintFS refuses writes on read-only or failed mounts":
        "if (fs->read_only) return MINTFS_EROFS;" in mintfs,
    "Disk Utility's MintOS initialization uses the same identity + typed-confirmation gate as the FAT16 formatter":
        "int64_t index=validate_format_request(space,arg0,&info);" in between(syscall, "if (number == MINT_SYS_STORAGE_INIT_MINTOS)", "if (number == MINT_SYS_STORAGE_FORMAT_BLANK)") and
        'const char prefix[]="FORMAT "' in syscall and "request.expected_sector_count!=dev->sector_count" in syscall,
    # --- boot and process wiring
    "the MintOS disk is probed after the vmalloc arena is reserved, and the fs commit task exists":
        "if (paging_ok && arena_reserved) mint_mintos_disk_probe();" in kernel and 'mint_task_create("fs", mint_mfs_task, 0)' in kernel and
        kernel.index("int arena_reserved = ") < kernel.index("mint_mintos_disk_probe();"),
    "file handles are closed on every process exit path":
        "mint_ramfs_close_owner(p->pid);\n    mint_mfs_close_all(p->pid);" in process,
    "the background task commits pending data about every 2 s":
        "mint_timer_ticks() - last_commit_tick >= 200u" in disk,
    # --- syscall ABI
    "13 MintFS syscalls occupy 0x4D40-0x4D4C without colliding with the session RAMFS calls":
        all(f"#define MINT_SYS_MFS_{n}" in uapi for n in ["OPEN", "READ", "WRITE", "SEEK", "CLOSE", "STAT", "LIST", "MKDIR", "RENAME", "REMOVE", "SYNC", "VOLUME"]) and
        "#define MINT_SYS_STORAGE_INIT_MINTOS           0x4D4Cu" in uapi and "#define MINT_SYS_FS_OPEN                    0x4D10u" in uapi,
    "file syscalls copy paths with bounds, validate user buffers and cap transfers at MINT_MFS_IO_MAX":
        fs_sys.count("copy_user_cstring(space,") >= 6 and "static uint8_t io[MINT_MFS_IO_MAX];" in fs_sys and
        "arg2>sizeof(io)?sizeof(io):arg2" in fs_sys and "mint_address_space_validate_user_range(space,arg1,(size_t)len,1)" in fs_sys and
        "mint_address_space_validate_user_range(space,arg1,(size_t)len,0)" in fs_sys,
    "file syscalls act for the calling process only":
        "uint64_t pid=mint_task_current_pid();" in fs_sys and "mint_mfs_read((int64_t)arg0,io,(uint32_t)len,pid)" in fs_sys,
    "unknown open flags are refused":
        "if (arg1 & ~(uint64_t)(MINT_O_READ|MINT_O_WRITE|MINT_O_CREATE|MINT_O_TRUNC|MINT_O_APPEND|MINT_O_EXCL)) return -MINT_EINVAL;" in fs_sys,
    "the runtime's save is atomic: write a hidden sibling, then rename over the original":
        "tmp[o++] = '.'; tmp[o++] = '~';" in runtime and "int64_t r = mint_path_rename(tmp, path);" in runtime,
    # --- MintDE
    "Files shows Finder Favorites and Locations for MintOS HD":
        all(x in apps for x in ['"Favorites"', '"Locations"', '"Applications"', '"Desktop"', '"Documents"', '"Downloads"', "SYM_HOUSE", "files_choose_place"]),
    "Files offers New Folder, Rename, Duplicate, Move to Trash and Empty Trash, with a confirmation for permanent deletion":
        all(x in apps for x in ["files_new_folder", "files_begin_rename", "files_duplicate", "files_move_to_trash", "files_empty_trash",
                                "FILES_CONFIRM_DELETE", "Permanently erase the items in the Trash?", "You can\\xE2\\x80\\x99t undo this action."]),
    "Finder rename selects the name without its extension":
        "files_rename_stem" in apps and "files_rename_select" in apps,
    "the File menu and Dock Trash drive the same Files commands":
        "ACT_FILES_BASE + FILES_CMD_NEW_FOLDER" in desktop and "files_command(act - ACT_FILES_BASE)" in desktop and
        "if (files_show_trash()) shell_open_app(APP_FILES);" in desktop,
    "Notes saves notes in ~/Documents/Notes with the atomic save, debounced, and falls back to session RAM (26.5.014.0)":
        '"/Documents/Notes"' in notes and "mint_file_save(open_path, text, text_len)" in notes and
        "SAVE_DELAY_MS 1200u" in notes and 'mint_fs_open("NOTES.TXT", MINT_FS_OPEN_READ)' in notes and
        "notes_background(now)" in apps,
    "Disk Utility can erase a blank disk as MintOS (MintFS) or FAT16, and never offers Erase for the MintOS disk":
        '"MintOS (MintFS)", "MS-DOS (FAT16)"' in apps and "mint_storage_init_mintos(&request)" in apps and
        "d->mbr_signature_ok || disk_is_mintos(d)" in apps,
    "Terminal works on MintOS HD with file commands":
        "terminal_mfs_command(cmd)" in apps and all(f'same_text(word, "{w}")' in apps for w in ["ls", "cd", "cat", "mkdir", "rm", "mv", "cp", "echo", "df"]),
    # --- tests are wired into make check
    "make check runs the MintFS torture test, the MintOS-disk integration test, the GPT test and these contracts":
        "tools/test_mintfs.c kernel/fs/mintfs.c" in make and "tools/test_mintos_disk.c" in make and
        "tools/test_gpt.py" in make and "tools/test_mintfs_contracts.py" in make,
}

failed = 0
for name, ok in checks.items():
    print(("PASS: " if ok else "FAIL: ") + name)
    failed += not ok
print(f"{len(checks)} MintFS contracts, {failed} failures")
raise SystemExit(1 if failed else 0)
