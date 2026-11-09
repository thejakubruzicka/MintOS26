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
# MintDE 2 is split across the shell (desktop.c) and mintde/*.c modules.
app = (root / "kernel/user/desktop.c").read_text() + "".join(
    f.read_text() for f in sorted((root / "kernel/user/mintde").glob("*.c")) if f.name != "font_data.c")
shell = (root / "kernel/user/desktop.c").read_text()
kernel = (root / "kernel/kernel.c").read_text()
dispatch = (root / "kernel/abi/syscall.c").read_text()
display = (root / "kernel/ui/display_surface.c").read_text()
pci_header = (root / "kernel/include/pci.h").read_text()
pci = (root / "kernel/bus/pci.c").read_text()
ahci = (root / "kernel/block/ahci.c").read_text()
checks = {
    "bounded display fill and text syscalls are versioned": "MINT_SYS_DISPLAY_FILL_RECT" in uapi and "MINT_SYS_DISPLAY_DRAW_TEXT" in uapi,
    "userspace runtime wraps drawing and bounded sleep services": "mint_ui_fill_rect" in runtime and "mint_ui_draw_text" in runtime and "mint_sleep_ticks" in runtime,
    "native shell maps the shared canvas and checks its dimensions before rendering": "mint_display_map_canvas(&canvas)" in shell and "canvas.stride_pixels < canvas.width" in shell and "render();" in shell,
    "native shell handles desktop keyboard and pointer events": "MINT_DESKTOP_EVENT_KEY" in app and "MINT_DESKTOP_EVENT_POINTER_MOVE" in app and "MINT_DESKTOP_EVENT_POINTER_BUTTON" in app,
    "kernel validates text buffers and bounds before drawing": "mint_address_space_copy_from_user(space, text" in dispatch and "arg1 > 128u" in dispatch and "arg2 > 16384u" in dispatch,
    "kernel launches shell only after both ring-3 smoke workers pass": "ring3_tests_completed < MINT_RING3_TEST_WORKERS" in kernel and "native_desktop_task" in kernel,
    "native shell keeps firmware desktop fallback path": "BOOTKERNEL" in (root / "uefi/main.c").read_text(),
    "MintDE uses a functional menu bar, Launchpad and retained dock without desktop shortcut clutter": all(x in shell for x in ["paint_menu_bar", "paint_menu_dropdown", "paint_launchpad", "APP_DISKS", "paint_dock", "dock_hit", "mint_display_cursor"]) and "desktop_shortcut" not in app,
    "native shell displays actual keyboard and mouse readiness": all(x in app for x in ["input_info.keyboard_ready", "input_info.mouse_ready", "mint_input_get_info", "in.keyboard_ready", "in.mouse_ready"]),
    "MintDE supports keyboard input, mouse interactions, menus and a visible pointer": all(x in shell for x in ["key_pressed", "pointer_pressed", "menu_item_rect", "update_cursor", "MINT_CURSOR_SHAPE_ARROW", "MINT_DESKTOP_EVENT_POINTER_MOVE"]),
    "MintDE Files provides directory listing, navigation and file preview": all(x in app for x in ["files_paint", "mint_fat16_list_dir", "files_go_up", "files_preview_active", "Session (RAM)", "mint_fat16_read_prefix"]),
    "MintDE has a Disk Utility with explicit blank-media confirmation": all(x in app for x in ["Disk Utility", "disk_is_erasable", "Erasing destroys all data on this disk", "disk_confirmation", "mint_storage_format_blank", "device->serial", "FORMAT "]),
    "MintDE System Monitor labels unavailable CPU/process-memory metrics honestly": all(x in app for x in ["System Monitor", "POINTER MOVES", "CPU load and per-process memory are not measured"]),
    "MintDE provides a separate hardware and driver catalog app": all(x in app for x in ["Hardware & Drivers", "mint_driver_catalog_get", "APP_DRIVERS"]),
    "MintDE makes network stack limitations explicit": all(x in app for x in ["APP_NETWORK", "no TCP/IP", "No DHCP, DNS or sockets yet."]),
    "terminal volume selection avoids a multi-kilobyte stack snapshot": "MINT_STORAGE_SNAPSHOT storage;" not in app and "terminal_select_named_volume" in app,
    "PCI resource inventory includes read-only BAR/IRQ decoding": all(x in pci_header for x in ["MINT_PCI_BAR_SLOTS", "interrupt_line", "interrupt_pin"]) and "populate_device_resources" in pci,
    "ring-3 CPU faults are contained to the current process": "(f->cs & 3u) == 3u" in (root / "kernel/arch/x86_64/arch.c").read_text() and "mint_arch_return_from_user_fault" in (root / "kernel/arch/x86_64/arch.S").read_text(),
    "Storage layout exposes readonly mount state without claiming persistence": "storage_snapshot.readonly_mount_active" in app and "mounted read-only" in app and "Writes disabled" in app,
    "kernel PS/2 bridge is compiled into native kernel": "ps2-input.obj" in (root / "Makefile").read_text() and "mint_input_init()" in kernel,
    "scheduler limits activation logging to once per process": "logged_user_root" in (root / "kernel/task/scheduler.c").read_text(),
    "native window drag recomposes exactly the old and new window bounds": "shell_damage_window((uint32_t)drag_app)" in shell and shell.count("shell_damage_window((uint32_t)drag_app)") >= 2 and "gfx_copy(S, &gfx_wall, d)" in shell,
    "display service retains a backbuffer and presents the damage rectangle": all(x in display for x in ["display_backbuffer", "display_background", "damage_x0", "damage_y0", "damage_x1", "damage_y1"]),
    "native event loop batches events and coalesces pointer motion": "events[32]" in shell and "events[last + 1u].type == MINT_DESKTOP_EVENT_POINTER_MOVE" in shell and "mint_sched_yield" in shell,
    "Settings exposes live hardware-event counters and queue drops": all(x in app for x in ["Key events", "Pointer moves", "Button events", "Dropped events", "input_info.dropped_events"]),
    "native font uses proportional anti-aliased DejaVu-derived glyph masks": all(x in display for x in ["font_aa_width", "font_advance", "native_font_cov_bilinear", "native_blend_coverage"]),
    "MintDE renders UTF-8 text from pre-rasterized Inter faces at several sizes": all(x in app for x in ["utf8_next", "glyph_for", "gfx_text_fit"]) and "FONT_HEADLINE" in (root / "kernel/user/mintde/font.h").read_text() and (root / "docs/INTER_FONT_LICENSE.txt").is_file(),
    "Settings appearance controls are mouse hit targets that mutate the versioned settings API": all(x in app for x in ["settings_click", "desktop_settings.theme = themes[i]", "desktop_settings.accent = (uint32_t)i", "mint_desktop_settings_set"]),
    "Spotlight supports keyboard search, arrow selection and Enter activation": all(x in shell for x in ["spotlight_query", "spotlight_sel", "spotlight_activate(spotlight_sel)", "k->scancode == 0x150u"]),
    "menu bar items route clicks to applications and window actions": all(x in shell for x in ["run_action(menu_items[i].action)", "case ACT_SETTINGS: shell_open_app(APP_SETTINGS)", "case ACT_ZOOM", "case ACT_MINIMIZE"]),
    "traffic-light window controls close, minimize and zoom/restore": all(x in shell for x in ["if (light == 0) { shell_close_app", "if (light == 1) { minimize_app", "if (light == 2) { zoom_app", "w->zoomed = 1"]),
    "Launchpad paints and hit-tests the same responsive grid": shell.count("launchpad_cell(i, n)") >= 2 and "launchpad_apps[i]" in shell,
    "native shell requests a kernel-owned photo wallpaper with color-grade variants": "MINT_SYS_DISPLAY_SET_WALLPAPER" in uapi and "mint_ui_set_wallpaper" in runtime and "mint_display_current_draw_wallpaper" in display,
    "wallpaper resource is embedded only in the native kernel": "wallpaper.obj" in (root / "Makefile").read_text() and '.incbin "assets/wallpaper-1920x1080.rgb"' in (root / "kernel/ui/wallpaper.S").read_text(),
    "MintDE starts on a clean workspace and contains no Welcome-window fallback": "static uint32_t zcount;" in shell and "shell_open_app(" in shell and "Welcome to MintDE" not in app and "Ready for a fresh start?" not in app and "app_init();" in shell,
    "Launchpad is directly accessible from the retained dock": "static const uint32_t dock_apps[] = {APP_LAUNCHPAD" in shell and "if (app == APP_LAUNCHPAD) { launchpad_open = 1" in shell,
    "MintDE glass materials blur the composed backdrop inside rounded shapes": "gfx_blur_rounded" in app and "static void glass(gfx_rect r" in shell and "expand_for_glass" in shell and "rrect_cov" in app,
    "MintDE menus expose dropdown item hit-testing and hover state": all(x in shell for x in ["paint_menu_dropdown", "menu_item_rect", "hovered_menu_item", "menu_items[i].enabled"]),
    "MintDE Settings contains dedicated categories including Storage and Privacy": all(x in app for x in ["General", "Appearance", "Displays", "Input", "Storage", "Privacy"]) and "settings_category" in app,
    "About changelog is hidden by default and lists versioned history on demand":
        "show_changelog" in app and "Release Notes" in app and "os_versions[8]" in app and "de_versions[8]" in app and
        all((root / "changelogs" / n).is_file() for n in ["MintOS26-26.5.010.0.md", "MintOS26-26.5.009.0.md", "MintOS26-26.5.008.0.md", "MintOS26-26.5.007.0.md", "MintOS26-26.5.006.0.md", "MintOS26-26.5.005.0.md", "MintOS26-26.5.004.0.md", "MintOS26-26.5.003.0.md", "MintOS26-26.5.002.0.md", "MintOS26-26.5.001.0.md", "MintDE-2.0.0.md", "MintDE-1.0.8.md", "MintDE-1.0.7.md", "MintDE-1.0.6.md", "MintDE-1.0.5.md", "MintDE-1.0.4.md", "MintDE-1.0.3.md", "MintDE-1.0.0.md"]),
    "Files enumerates mounted FAT16 volumes with explicit per-volume selection": all(x in app for x in ["mint_fat16_get_volumes", "mint_fat16_select_volume", "files_volume", "files_choose_volume"]) and "MINT_FAT16_MAX_VOLUMES 8u" in (root / "kernel/include/fat16.h").read_text(),
    "standard FAT16 volumes do not require MintOS seed files to mount": "standard FAT16 volume is browseable even without MintOS seed files" in (root / "kernel/fs/fat16.c").read_text() and "MintOS26 seed files are optional" in (root / "kernel/diag/storage_diag.c").read_text(),
    "pointer is a single kernel overlay driven by MintDE, not painted by the PS/2 IRQ path": "mint_display_set_overlay_cursor" in display and "mint_display_cursor(pointer_x, pointer_y, shape)" in shell and "mint_display_cursor_move" not in (root / "kernel/input/ps2.c").read_text(),
    "native windows implement mouse-driven resize from the right/bottom edges and corner": all(x in shell for x in ["resize_edges_at", "resize_w0", "resize_h0", "MINT_CURSOR_SHAPE_RESIZE_NWSE"]),
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
        all((root / "assets/icons" / (n+".png")).is_file() and (root / "assets/icons" / (n+"-64.rgba")).is_file() and
            (root / "assets/icons" / (n+"-128.rgba")).is_file()
            for n in ["files","launchpad","terminal","settings","system-monitor","disk-utility","console","system-info","network","about-mintos","mintde","notes","calculator","trash"]),
    "PNG RGBA syscall validates user memory and bounds source and target dimensions":
        "MINT_SYS_DISPLAY_DRAW_BITMAP" in dispatch and "mint_address_space_validate_user_range(space,arg0,source_bytes,0)" in dispatch and
        "source_width>96u" in dispatch and "target_width>128u" in dispatch,
    "Settings panes use grouped rows inside a bounded content column":
        "settings_body" in app and "if (w > 640) w = 640;" in app and "ui_group_row" in app,
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
    "About lists eight versions and the Done/Release Notes buttons share paint and hit geometry": "for (uint32_t i = 0; i < 8u; ++i)" in app and app.count("R(cx - 50, f.y1 - 44, 100, 28)") >= 2 and app.count("R(cx + 10, f.y1 - 52, 120, 28)") >= 2,
}
for name, ok in checks.items():
    print(("PASS: " if ok else "FAIL: ") + name)
if not all(checks.values()):
    raise SystemExit(1)
