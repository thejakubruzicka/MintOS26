#!/usr/bin/env python3
"""Static contracts for the 26.5.014.0 personality work (sound, boot
animation, wallpapers, notifications, desktop items)."""
from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]


def rd(p: str) -> str:
    return (root / p).read_text(errors="replace")


audio_h = rd("kernel/include/uapi/mintos/audio.h")
sys_h = rd("kernel/include/uapi/mintos/syscall.h")
syscall_c = rd("kernel/abi/syscall.c")
asys = rd("kernel/sound/audio_syscall.c")
hda = rd("kernel/sound/hda.c")
core = rd("kernel/sound/hda_core.c")
mixer = rd("kernel/sound/audio_mixer.c")
status = rd("kernel/sound/system_status.c")
kernel = rd("kernel/kernel.c")
arch = rd("kernel/arch/x86_64/arch.c")
splash = rd("kernel/ui/boot_splash.c")
log_h, log_c = rd("kernel/include/log.h"), rd("kernel/console/log.c")
catalog = rd("kernel/driver/catalog.c")
makefile = rd("Makefile")
wall_s = rd("kernel/ui/wallpaper.S")
sounds = rd("tools/make_sounds.py")
desktop = rd("kernel/user/desktop.c")
apps = rd("kernel/user/mintde/apps.c")
mintde_h = rd("kernel/user/mintde/mintde.h")
pers_h = rd("kernel/user/mintde/personality.h")
pers_c = rd("kernel/user/mintde/personality.c")
extras = rd("kernel/user/mintde/extras.c")
items = rd("kernel/user/mintde/desktop_items.c")
psettings = rd("kernel/user/mintde/personality_settings.c")
walls = rd("kernel/user/mintde/wallpapers.c")

ours = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define (MINT_SYS_\w+)\s+0x([0-9A-Fa-f]+)u", audio_h)}
theirs = {int(m.group(1), 16) for m in re.finditer(r"#define MINT_SYS_\w+\s+0x([0-9A-Fa-f]+)u", sys_h)}


def kernel_span() -> int:
    elf = (root / "build/mintos26-kernel.elf").read_bytes()
    phoff, phentsize, phnum = struct.unpack_from("<Q", elf, 32)[0], struct.unpack_from("<H", elf, 54)[0], struct.unpack_from("<H", elf, 56)[0]
    lo, hi = None, 0
    for i in range(phnum):
        p_type, _, _, vaddr, _, _, memsz = struct.unpack_from("<IIQQQQQ", elf, phoff + i * phentsize)
        if p_type != 1:
            continue
        lo = vaddr if lo is None else min(lo, vaddr)
        hi = max(hi, vaddr + memsz)
    return hi - (lo or 0)


apple_names = ["basso", "blow", "bottle", "frog", "funk", "glass", "hero", "morse", "ping", "purr", "sosumi",
               "submarine", "tink", "boop", "breeze", "bubble", "crystal", "heroine", "mezzo", "pebble", "pluck",
               "pong", "sonar", "sonumi", "submerge"]
sound_names = re.findall(r'\("(\w+)", \w+\)', sounds)
pref_fields = re.findall(r"uint32_t (\w+);", pers_h[pers_h.index("typedef struct mde_personality_prefs"):pers_h.index("} mde_personality_prefs;")])

checks = {
    "personality syscalls stay inside the reserved range 0x4D58-0x4D5F":
        ours and all(0x4D58 <= v <= 0x4D5F for v in ours.values()) and len(set(ours.values())) == len(ours),
    "no personality syscall collides with the base ABI": not (set(ours.values()) & theirs),
    "dispatcher routes the whole range to mint_personality_syscall":
        "number >= 0x4D58u && number <= 0x4D5Fu" in syscall_c and "mint_personality_syscall(space" in syscall_c,
    "audio syscalls validate and copy every user pointer":
        asys.count("mint_address_space_validate_user_range") >= 3 and "copy_from_user(space, dst" in asys and
        "MINT_AUDIO_QUEUE_MAX_FRAMES" in asys,
    "log component 14 is AUDIO": "MINT_LOG_COMPONENT_AUDIO = 14" in log_h and '"AUDIO"' in log_c,
    "HDA probe at boot is read-only PCI matching (class 0x0403)":
        "d->class_code == 0x04u && d->subclass == 0x03u" in hda and "mint_hda_probe();" in kernel,
    "experimental (non-VM) controllers wait for an explicit, token-checked start":
        "if (!device || info.experimental) return;" in hda and "MINT_AUDIO_START_TOKEN" in asys and "hypervisor_present()" in hda,
    "bus mastering is enabled only after the DMA structures are zeroed":
        hda.index("zero((void *)(uintptr_t)ring_pa") < hda.index("pci_set_command(command | 0x0006u)"),
    "clean stop: stream and CORB/RIRB off, bus mastering off and confirmed before DMA is freed":
        hda.index("mint_hda_stop_corb_rirb(&hw, &link);") < hda.index("pci_set_command(pci_command() & ~0x0004u);")
        < hda.index("if (pci_command() & 0x0004u)") < hda.index("release_dma();\n    return 1;"),
    "HDA waits are bounded (no unbounded loops in the core)":
        "for (;;)" not in core and "while (1)" not in core and "limit_ticks * 4u + 8u" in core and "now(hw) - start >= 50u" in core,
    "CORB/RIRB with immediate-command fallback": "immediate_command(hw, link, verb, response)" in core and "HDA_ICIS_ICB" in core,
    "widget-graph walk skips digital and unconnected pins": "HDA_WCAP_DIGITAL" in core and "HDA_CFG_PORT(w->config) != 1u" in core,
    "EAPD, HP drive, amps and connection selects are programmed":
        all(x in core for x in ["HDA_VERB_SET_EAPD", "HDA_PINCTL_HP_EN", "HDA_VERB4_SET_AMP", "HDA_VERB_SET_CONN_SELECT"]),
    "mixer feeds the ring from the PIT hook, interrupts off in syscall paths":
        "mint_timer_irq_hook(frame->rip, frame->cs)" in arch and "mint_audio_timer_tick();" in status and "irq_save()" in hda,
    "underruns play silence, never stale audio": "m->ring[idx] = 0;" in mixer and "underruns" in mixer,
    "driver catalog reports Intel High Definition Audio": "Intel High Definition Audio" in catalog and "mint_audio_get_info" in catalog,
    "boot animation starts right after the framebuffer is validated":
        kernel.index("mint_boot_splash_begin(&kernel_surface);") < kernel.index("mint_arch_init();"),
    "diagnostic panel still shown when the desktop cannot start":
        kernel.count("show_diagnostic_panel();") >= 3 and "TSS/TR INSTALLED; DF/NMI/MC IST ARMED" in kernel,
    "serial boot logging unchanged": 'MintOS26 26.5.013.0 native kernel entry' in kernel and "[splash]" in splash,
    "MintDE stops the boot animation before mapping the canvas and crossfades":
        desktop.index("personality_boot_handoff()") < desktop.index("mint_display_map_canvas(&canvas)") and
        "personality_paint_splash" in pers_c and "mint_splash_render" in pers_c,
    "no new raw wallpaper blobs (32 MiB kernel span)": wall_s.count(".incbin") == 2 and "wallpaper_render" in walls,
    "kernel image still fits the 32 MiB load span": kernel_span() < 32 * 1024 * 1024,
    "sounds are synthesised, not sampled (no audio input is read)":
        "read_bytes" not in sounds and "wave.open" not in sounds and "class LCG" in sounds and "def reverb" in sounds,
    "built-in sound names are original (no other platform's sound names)":
        sound_names and not any(a in n.lower().split("_") for n in sound_names for a in apple_names),
    "every persisted personality preference has a prefs key":
        all(f in pers_c for f in pref_fields if f != "version") and pers_c.count('{"') >= len(pref_fields) - 1,
    "notify_post is used by Disk Utility, Empty Trash, screenshots and Settings":
        apps.count("notify_post(APP_DISKS") >= 2 and 'notify_post(APP_FILES, "Trash emptied"' in apps and
        'notify_post(APP_FILES, "Screenshot"' in extras and "notify_post(APP_SETTINGS" in psettings,
    "Notification Center opens from the menu bar clock":
        "gfx_rect_contains(clock_rect(), x, y)) { control_center_open = 0; extras_toggle_notification_center();" in desktop,
    "Notification Center widgets: calendar, system, world clock, notes":
        all(x in extras for x in ["widget_calendar", "widget_system", "widget_world_clock", "widget_notes", "mint_system_status_get"]),
    "Settings has Sound and Notifications panes and the wallpaper picker":
        "SET_SOUND, SET_NOTIFICATIONS" in apps and "settings_wallpaper_paint(b)" in apps and "settings_sound_click" in apps,
    "Control Center volume slider and Do Not Disturb": "extras_cc_sound_paint(cc_sound_rect())" in desktop and "extras_cc_dnd_paint(b)" in desktop,
    "volume keys (mute, down, up) handled": all(x in extras for x in ["0x120u", "0x12Eu", "0x130u"]),
    "USB HID Mute/Volume Up/Volume Down usages reach MintDE as the PS/2 extended codes":
        "case 0x7F: return 0x120; case 0x80: return 0x130; case 0x81: return 0x12E;" in rd("kernel/usb/xhci.c"),
    "desktop items come from ~/Desktop on MintOS HD and refresh on change":
        '"/Desktop"' in items and "v.generation == last_generation" in items and "extras_paint_desktop(d)" in desktop,
    "personality hooks are included once from mintde.h": mintde_h.count('#include "personality.h"') == 1,
    "personality tests run in make check": "check: personality-check" in makefile and all(
        x in makefile for x in ["test_hda.c", "test_audio_mixer.c", "test_sounds.py", "test_splash.py", "test_notify.c", "test_wallpapers.c"]),
    "release numbers untouched (the coordinator bumps them)":
        "VERSION := 26.5.013.0" in makefile and '#define MINTDE_VERSION "2.3.0"' in mintde_h,
    "release notes for the topic exist": (root / "docs/notes-26.5.014.0-personality.md").exists(),
}

failed = 0
for name, ok in checks.items():
    print(("PASS: " if ok else "FAIL: ") + name)
    failed += 0 if ok else 1
print(f"{'FAIL' if failed else 'PASS'}: {len(checks) - failed}/{len(checks)} personality contracts")
sys.exit(1 if failed else 0)
