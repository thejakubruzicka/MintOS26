#!/usr/bin/env python3
"""Static checks for the first bounded ring-3 desktop shell ELF."""
from pathlib import Path
import struct
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: verify_native_desktop.py DESKTOP.ELF")
root = Path(__file__).resolve().parents[1]
elf_path = Path(sys.argv[1])
data = elf_path.read_bytes()
assert data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1, "desktop payload must be ELF64 little-endian"
assert struct.unpack_from("<HH", data, 16) == (2, 0x3E), "desktop payload must be static x86-64 ET_EXEC"
phoff = struct.unpack_from("<Q", data, 32)[0]
phentsize, phnum = struct.unpack_from("<HH", data, 54)
assert phentsize >= 56 and 0 < phnum <= 32 and phoff + phentsize * phnum <= len(data), "invalid program headers"
flags = []
for i in range(phnum):
    off = phoff + i * phentsize
    p_type, p_flags = struct.unpack_from("<II", data, off)
    if p_type == 1:
        flags.append(p_flags)
assert any((f & 5) == 5 and not (f & 2) for f in flags), "desktop ELF lacks RX text segment"
assert any((f & 6) == 6 and not (f & 1) for f in flags), "desktop ELF lacks separate RW/NX data segment"
print("PASS: native desktop is a static ELF64 x86-64 executable")
print("PASS: native desktop has RX text and separate RW/NX data")

uapi = (root / "kernel/include/uapi/mintos/syscall.h").read_text()
runtime = (root / "kernel/user/runtime.c").read_text()
app = (root / "kernel/user/desktop.c").read_text()
kernel = (root / "kernel/kernel.c").read_text()
dispatch = (root / "kernel/abi/syscall.c").read_text()
display = (root / "kernel/ui/display_surface.c").read_text()
pci_header = (root / "kernel/include/pci.h").read_text()
pci = (root / "kernel/bus/pci.c").read_text()
ahci = (root / "kernel/block/ahci.c").read_text()
checks = {
    "bounded display fill and text syscalls are versioned": "MINT_SYS_DISPLAY_FILL_RECT" in uapi and "MINT_SYS_DISPLAY_DRAW_TEXT" in uapi,
    "userspace runtime wraps drawing and bounded sleep services": "mint_ui_fill_rect" in runtime and "mint_ui_draw_text" in runtime and "mint_sleep_ticks" in runtime,
    "native shell queries display dimensions before rendering": "mint_display_get_info(&display)" in app and "paint_shell();" in app,
    "native shell handles desktop keyboard and pointer events": "MINT_DESKTOP_EVENT_KEY" in app and "MINT_DESKTOP_EVENT_POINTER_MOVE" in app and "MINT_DESKTOP_EVENT_POINTER_BUTTON" in app,
    "kernel validates text buffers and bounds before drawing": "mint_address_space_copy_from_user(space, text" in dispatch and "arg1 > 128u" in dispatch and "arg2 > 16384u" in dispatch,
    "kernel launches shell only after both ring-3 smoke workers pass": "ring3_tests_completed < MINT_RING3_TEST_WORKERS" in kernel and "native_desktop_task" in kernel,
    "native shell keeps firmware desktop fallback path": "BOOTKERNEL" in (root / "uefi/main.c").read_text(),
    "MintDE uses a functional menu bar, app library and retained dock without desktop shortcut clutter": all(x in app for x in ["paint_menu_bar", "paint_menu_dropdown", "Applications", "APP_DISKS", "paint_cursor", "paint_dock", "handle_dock_click"]) and "desktop_shortcut" not in app,
    "native shell displays actual keyboard and mouse readiness": all(x in app for x in ["input_info.keyboard_ready", "input_info.mouse_ready", "mint_input_get_info"]),
    "MintDE supports keyboard input, mouse interactions, menus and a visible pointer": all(x in app for x in ["handle_key", "handle_pointer_button", "handle_menu_dropdown_click", "paint_cursor", "MINT_DESKTOP_EVENT_POINTER_MOVE"]),
    "MintDE Files provides directory listing, navigation and file preview": all(x in app for x in ["paint_files_view", "mint_fat16_list_dir", "files_go_up", "files_preview_active", "Session (RAM)", "Developer volume"]),
    "MintDE has a Disk Utility with explicit blank-media confirmation": all(x in app for x in ["Disk Utility", "Initialize selected blank disk", "Destructive confirmation", "disk_confirmation", "mint_storage_format_blank", "d->serial", "No MBR signature; guard checks before format"]),
    "MintDE System Monitor labels unavailable CPU/process-memory metrics honestly": all(x in app for x in ["System Monitor", "Input event counters", "CPU utilization and per-process memory are not exposed"]),
    "MintDE provides a separate hardware and driver catalog app": all(x in app for x in ["Hardware & Drivers", "mint_driver_catalog_get", "APP_DRIVERS"]),
    "MintDE makes network stack limitations explicit": all(x in app for x in ["APP_NETWORK", "TCP/IP services", "NOT IMPLEMENTED"]),
    "terminal volume selection avoids a multi-kilobyte stack snapshot": "MINT_STORAGE_SNAPSHOT storage;" not in app and "terminal_select_named_volume" in app,
    "PCI resource inventory includes read-only BAR/IRQ decoding": all(x in pci_header for x in ["MINT_PCI_BAR_SLOTS", "interrupt_line", "interrupt_pin"]) and "populate_device_resources" in pci,
    "ring-3 CPU faults are contained to the current process": "(f->cs & 3u) == 3u" in (root / "kernel/arch/x86_64/arch.c").read_text() and "mint_arch_return_from_user_fault" in (root / "kernel/arch/x86_64/arch.S").read_text(),
    "Storage layout exposes readonly mount state without claiming persistence": "storage_snapshot.readonly_mount_active" in app and "persistence writes still disabled" in app,
    "kernel PS/2 bridge is compiled into native kernel": "ps2-input.obj" in (root / "Makefile").read_text() and "mint_input_init()" in kernel,
    "scheduler limits activation logging to once per process": "logged_user_root" in (root / "kernel/task/scheduler.c").read_text(),
    "native window drag restores its retained background": "mint_ui_capture_background" in app and "mint_ui_restore_rect" in app and "move_window_to(target_x, target_y)" in app,
    "display service retains a backbuffer and presents the damage rectangle": all(x in display for x in ["display_backbuffer", "display_background", "damage_x0", "damage_y0", "damage_x1", "damage_y1"]),
    "native event loop batches events and coalesces pointer motion": "events[16]" in app and "mint_sched_yield" in app,
    "Settings exposes live hardware-event counters and queue drops": all(x in app for x in ["Key events", "Pointer moves", "Button events", "Queue drops", "input_info.dropped_events"]),
    "native font uses proportional anti-aliased DejaVu-derived glyph masks": all(x in display for x in ["font_aa_width", "font_advance", "native_font_cov_bilinear", "native_blend_coverage"]),
    "native draw-text supports mixed-case ASCII with bounded fractional-size presets": "normalized[i]" not in app and "actual = scale + configured - 1u" in app and "scale_q8[5] = {0u, 256u, 320u, 384u, 512u}" in display,
    "Settings appearance rows are mouse hit targets that mutate the versioned settings API": all(x in app for x in ["settings_control_at", "activate_setting", "mint_desktop_settings_set", "hover_setting_control"]),
    "Settings controls support keyboard focus, Enter/Space activation and refresh": all(x in app for x in ["keyboard_setting_focus", "ch == 9u", "activate_setting(keyboard_setting_focus)", "case 6u"]),
    "menu bar labels route clicks to applications and window actions": all(x in app for x in ["handle_menu_bar_click", "menu_item_action", "open_app((uint32_t)action)", "window_maximized=!window_maximized"]),
    "window controls close, minimize and maximize/restore": all(x in app for x in ["window_minimized = 1u", "window_maximized = !window_maximized", "app_window_open = 0"]),
    "application library renders visible cards and hit-tests the same responsive grid": "Choose an app to open" in app and "grid_y = cached_win_y + (compact_cards ? 132 : 142)" in app and "grid_y = cached_win_y + (compact_cards ? 132 : 142)" in app[app.find("static void handle_pointer_button"):],
    "native shell requests a kernel-owned photo wallpaper with color-grade variants": "MINT_SYS_DISPLAY_SET_WALLPAPER" in uapi and "mint_ui_set_wallpaper" in runtime and "mint_display_current_draw_wallpaper" in display,
    "wallpaper resource is embedded only in the native kernel": "wallpaper.obj" in (root / "Makefile").read_text() and '.incbin "assets/wallpaper-1920x1080.rgb"' in (root / "kernel/ui/wallpaper.S").read_text(),
    "MintDE starts on a clean workspace and contains no Welcome-window fallback": "static uint32_t library_open;" in app and "static uint32_t app_window_open;" in app and "if(!window_minimized && (library_open || app_window_open)) paint_home_window" in app and "Welcome to MintDE" not in app and "Ready for a fresh start?" not in app,
    "Launchpad is directly accessible from the retained dock": "APP_LAUNCHPAD" in app and "#define MINTDE_DOCK_APP_COUNT 10u" in app and "APP_NETWORK, APP_DISKS, APP_LAUNCHPAD" in app,
    "MintDE window materials use validated alpha-blended rounded rectangles": "MINT_SYS_DISPLAY_BLEND_ROUND_RECT" in uapi and "mint_ui_blend_round_rect" in runtime and "mint_display_current_blend_round_rect" in display and "static void glass_box" in app,
    "MintDE menus expose dropdown item hit-testing and hover state": "paint_menu_dropdown" in app and "handle_menu_dropdown_click" in app and "hovered_menu_item" in app and "menu_item_action" in app,
    "MintDE Settings contains dedicated categories including Storage and Privacy": all(x in app for x in ["General", "Appearance", "Displays", "Input", "Storage", "Privacy"]) and "settings_category" in app,
    "About changelog is hidden by default and lists versioned history on demand":
        'show_changelog==0u?"Changelog  >"' in app and "show_changelog=2u" in app and
        all((root / "changelogs" / n).is_file() for n in ["MintOS26-26.5.008.0.md", "MintOS26-26.5.007.0.md", "MintOS26-26.5.006.0.md", "MintOS26-26.5.005.0.md", "MintOS26-26.5.004.0.md", "MintOS26-26.5.003.0.md", "MintOS26-26.5.002.0.md", "MintOS26-26.5.001.0.md", "MintDE-1.0.7.md", "MintDE-1.0.6.md", "MintDE-1.0.5.md", "MintDE-1.0.4.md", "MintDE-1.0.3.md", "MintDE-1.0.0.md"]),
    "Files enumerates mounted FAT16 volumes with explicit per-volume selection": all(x in app for x in ["mint_fat16_get_volumes", "mint_fat16_select_volume", "files_volume", "Developer volume"]) and "MINT_FAT16_MAX_VOLUMES 8u" in (root / "kernel/include/fat16.h").read_text(),
    "standard FAT16 volumes do not require MintOS seed files to mount": "standard FAT16 volume is browseable even without MintOS seed files" in (root / "kernel/fs/fat16.c").read_text() and "MintOS26 seed files are optional" in (root / "kernel/diag/storage_diag.c").read_text(),
    "software cursor is not duplicated by the PS/2 kernel cursor overlay": "paint_cursor" in app and "mint_display_cursor_move" not in (root / "kernel/input/ps2.c").read_text(),
    "native windows implement mouse-driven resize via the bottom-right grip": "window_resizing" in app and "resize_anchor_x" in app and "resize_start_w" in app,
    "built-in terminal has bounded useful diagnostic commands": all(x in app for x in ["terminal_run_command", "help", "uptime", "drives", "volumes", "cat "]),
    "bounded read-only PCI discovery is part of the kernel": "pci.obj" in (root / "Makefile").read_text() and "mint_pci_enumerate" in kernel and "MINT_PCI_MAX_DEVICES" in pci_header,
    "PCI inventory is read-only; AHCI separately enables MMIO/DMA without rewriting BARs":
        "mint_pci_enumerate" in pci and "mint_pci_write_config32" in pci and
        "uint32_t command = (old_cmdstat & 0xFFFFu) | (1u << 1) | (1u << 2)" in ahci and
        "mint_pci_write_config32(d->bus, d->device, d->function, 0x04u, command)" in ahci and
        "mint_pci_write_config32(d->bus, d->device, d->function, AHCI_BAR5_OFFSET" not in ahci,
    "per-app PNG icon pack is shipped and embedded in the ring-3 desktop":
        "MINT_SYS_DISPLAY_DRAW_BITMAP" in uapi and "mint_ui_draw_bitmap_rgba" in runtime and
        "mint_display_current_draw_rgba_scaled_row" in display and
        all((root / "assets/icons" / (n+".png")).is_file() and (root / "assets/icons" / (n+".rgba")).is_file()
            for n in ["files","launchpad","terminal","settings","system-monitor","disk-utility","database","storage","cpu","mintde","windows","notes"]),
    "PNG RGBA syscall validates user memory and bounds source and target dimensions":
        "MINT_SYS_DISPLAY_DRAW_BITMAP" in dispatch and "mint_address_space_validate_user_range(space,arg0,source_bytes,0)" in dispatch and
        "source_width>96u" in dispatch and "target_width>128u" in dispatch,
    "Settings uses real drop-down selection and bounded session panel geometry":
        "settings_option_label" in app and "paint_settings_dropdown" in app and "apply_settings_dropdown" in app and
        "int32_t info_h = h - 514" in app,
    "kernel supports ATA DMA EXT and FLUSH CACHE EXT on LBA48-capable disks":
        "AHCI_CMD_READ_DMA_EXT" in ahci and "AHCI_CMD_WRITE_DMA_EXT" in ahci and
        "ATA_CMD_FLUSH_CACHE_EXT" in ahci and "ports[slot].lba48_supported" in ahci,
    "block layer tracks per-device read outcomes and blocked generic writes":
        "MINT_BLOCK_IO_STATS" in (root / "kernel/include/block.h").read_text() and
        "writes_blocked" in (root / "kernel/block/block.c").read_text() and "MINT_SYS_BLOCK_IO_STATS" in uapi,
    "VFS provides stable per-volume mount names and marks session RAM volatile":
        "MINT_VFS_MOUNT_TABLE" in (root / "kernel/include/vfs.h").read_text() and
        '"/session"' in (root / "kernel/fs/vfs.c").read_text() and '"/Volumes/"' in (root / "kernel/fs/vfs.c").read_text() and
        "MINT_SYS_VFS_MOUNTS" in dispatch,
    "terminal offers bounded PCI, driver, mount and block-I/O diagnostics":
        all('terminal_equals(cmd,"'+n+'")' in app for n in ["pci","drivers","mounts","io","logs","cpu"]) and
        "terminal_select_named_volume" in app and "terminal_current_path" in app,
    "all eight changelog versions use matching compact list hit targets": "int32_t list_h=(cached_win_h<620)?36:40, list_gap=6;" in (root / "kernel/user/desktop.c").read_text() and "for(uint32_t ci=0;ci<8u;ci++)" in (root / "kernel/user/desktop.c").read_text() and "row_y+=list_h+list_gap" in (root / "kernel/user/desktop.c").read_text(),
}
for name, ok in checks.items():
    print(("PASS: " if ok else "FAIL: ") + name)
if not all(checks.values()):
    raise SystemExit(1)
