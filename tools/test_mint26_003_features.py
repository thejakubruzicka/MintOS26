#!/usr/bin/env python3
"""Source contracts for the 26.5.003 filesystem, driver I/O and MintDE increment."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
app=(root/'kernel/user/desktop.c').read_text()
display=(root/'kernel/ui/display_surface.c').read_text()
syscalls=(root/'kernel/abi/syscall.c').read_text()
uapi=(root/'kernel/include/uapi/mintos/syscall.h').read_text()
ahci=(root/'kernel/block/ahci.c').read_text()
block=(root/'kernel/block/block.c').read_text()
vfs=(root/'kernel/fs/vfs.c').read_text()
make=(root/'Makefile').read_text()
icons=['files','launchpad','terminal','settings','system-monitor','disk-utility','database','storage','cpu','mintde','windows','notes']
checks={
 'Settings dropdowns apply typed options rather than cycle blindly': all(s in app for s in ['settings_option_label','settings_dropdown_cursor','apply_settings_dropdown','handle_settings_dropdown_click']),
 'Settings session panel is clipped to the window bottom': 'int32_t info_h = h - 514' in app and 'info_h < 32' in app,
 'Liquid-glass service blends alpha and samples a captured backdrop': 'display_background_valid && alpha < 250u' in display and 'Cross-shaped five-tap backdrop blur' in display,
 'per-app icons exist as independent PNG and linked RGBA assets': all((root/'assets/icons'/(n+'.png')).is_file() and (root/'assets/icons'/(n+'.rgba')).is_file() for n in icons),
 'the UEFI Desktop/data image receive independent PNG icon files': 'ICON_ESP_ARGS' in make and 'ICON_DATA_ARGS' in make and 'MINTOS26/ICONS/' in make,
 'bounded bitmap syscall validates readable user source memory': 'MINT_SYS_DISPLAY_DRAW_BITMAP' in uapi and 'mint_address_space_validate_user_range(space,arg0,source_bytes,0)' in syscalls,
 'bitmap renderer clips and alpha-blends individual RGBA pixels': 'mint_display_current_draw_rgba_scaled_row' in display and 'uint32_t inv = 255u - alpha' in display,
 'LBA48 read/write EXT and flush commands are capability-gated': all(s in ahci for s in ['AHCI_CMD_READ_DMA_EXT','AHCI_CMD_WRITE_DMA_EXT','ATA_CMD_FLUSH_CACHE_EXT','p->lba48_supported']),
 'generic block I/O has bounded per-device read/failure/denied-write counters': all(s in block for s in ['read_calls','read_success','read_failures','writes_blocked','mint_block_io_stats']),
 'VFS mount table maps each mounted volume by stable block index': all(s in vfs for s in ['/session','/Volumes/','MINT_VFS_FLAG_READ_ONLY','block_indices']),
 'terminal has explicit bounded hardware and I/O commands': all('terminal_equals(cmd,"'+s+'")' in app for s in ['pci','resources','drivers','mounts','io','logs','cpu']),
 'terminal filesystem commands preserve selected volume and current directory': all(s in app for s in ['terminal_select_named_volume','terminal_current_path','terminal_path_parent','mint_fat16_list_dir(terminal_current_path']),
 'current and historical changelog source files remain in the repository': all((root/'changelogs'/s).is_file() for s in ['MintOS26-26.5.008.0.md','MintOS26-26.5.007.0.md','MintOS26-26.5.006.0.md','MintOS26-26.5.005.0.md','MintOS26-26.5.004.0.md','MintOS26-26.5.003.0.md','MintOS26-26.5.002.0.md','MintOS26-26.5.001.0.md','MintOS26-26.4.004.4.md','MintDE-1.0.7.md','MintDE-1.0.6.md','MintDE-1.0.5.md','MintDE-1.0.4.md','MintDE-1.0.3.md','MintDE-1.0.2.md','MintDE-1.0.1.md','MintDE-1.0.0.md']),
 'ext4 remains a roadmap target until journal/recovery and write tests exist': 'writable ext4 requires journal replay/recovery' in (root/'docs/FILESYSTEM-ROADMAP.md').read_text().lower(),
}
for name,ok in checks.items(): print(('PASS: ' if ok else 'FAIL: ')+name)
if not all(checks.values()): raise SystemExit(1)
