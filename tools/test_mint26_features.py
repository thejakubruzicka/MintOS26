#!/usr/bin/env python3
"""Static source/asset contracts for MintOS26 26.5.005.0. Not a runtime VM test."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'uefi/main.c').read_text()
checks={
 'release reports 26.5.011.0': '26.5.011.0' in s,
 'launcher contains thirteen apps': 'int apps[13]' in s and 'row*4+col' in s,
 'About pages identify OS and desktop versions separately': all(k in s for k in ['Version 26.5.011.0  •  x86-64','Version 2.1.0  •  Native desktop environment']),
 'startup selector defaults safely to UEFI Desktop': all(k in s for k in ['wait_for_boot_choice()','Auto-starting UEFI Desktop','2  MintDE 2.0','boot_image_handle=image']),
 'Calendar and month-navigation handlers exist': 'APP_CALENDAR' in s and 'weekday_sun0' in s and 'calendar_month++' in s and 'calendar_month--' in s,
 'Clock uses UEFI RuntimeServices time': 'get_display_time(&t)' in s and 'zone_name()' in s,
 'Czech/German app labels are available': 'locale_id==1' in s and 'locale_id==2' in s,
 'timezone and 12/24-hour settings persist': all(k in s for k in ['timezone_id','clock_24h','TIME24=','TZ=']),
 'cursor styles are selectable and persistent': all(k in s for k in ['cursor_style==1','cursor_style==2','CURSOR=']),
 'font scale uses bilinear coverage sampling': 'font_cov_bilinear' in s and 'font_cov_xy' in s,
 'Code Studio loads and saves a C starter': all(k in s for k in ['APP_CODE','\\\\MINTOS26\\\\DEVELOP\\\\HELLO.C','save_code']),
 'Install Assistant clearly states staging limits': 'staging/recovery bundle, not a complete installer' in s and 'stage_install_bundle' in s,
 'Network app explicitly disclaims TCP/IP': 'TCP/IP is not implemented yet' in s and 'GUID_SIMPLE_NETWORK' in s,
 'settings tabs and dropdown selection are wired': all(k in s for k in ['settings_page=(lx>=220)?1:0','settings_popup_select','settings_dropdown=5','settings_dropdown=6','settings_dropdown=7']),
 'settings and notes support persistence': 'save_settings' in s and 'save_notes' in s,
 'documentation contains developer and locale notes': all((root/'assets'/n).exists() for n in ['DEVELOP.TXT','HELLO.C','APP.TXT','LOCALE.TXT']),
 '512 MiB writable data image requested': 'disk 1048576' in (root/'Makefile').read_text(),
 'Finder enumerates UEFI Simple File System volumes': all(k in s for k in ['MAX_FS_VOLUMES','LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL,&GUID_SFS','finder_refresh','finder_volume','GUID_BLOCK_IO','bio->Media']),
 'Finder navigates, previews, creates, and deletes files': all(k in s for k in ['finder_open_selected','finder_parent','finder_preview','finder_create_item','finder_delete_selected']),
 'Settings uses interactive dropdown controls': all(k in s for k in ['draw_dropdown_row','draw_settings_popup','settings_popup_select','settings_dropdown=3','settings_dropdown=7']),
 'launcher and settings text are width-fitted': s.count('draw_text_fit(')>25,
 'UEFI HTTP GET is firmware-mediated and bounded': all(k in s for k in ['GUID_HTTP_SERVICE_BINDING','perform_http_get','HttpVersion=1','HTTP response status','TCP/IP/DNS is unavailable']),
 'native shell composes into a retained canvas and presents damaged rectangles': 'mint_display_present(d.x0, d.y0, W(d), H(d))' in (root/'kernel/user/desktop.c').read_text() and 'mint_display_present_canvas' in (root/'kernel/ui/display_surface.c').read_text() and 'MINT_SYS_DISPLAY_PRESENT' in (root/'kernel/include/uapi/mintos/syscall.h').read_text(),
 'native windows support title-bar dragging and release': 'drag_app = app;' in (root/'kernel/user/desktop.c').read_text() and 'drag_app = -1;' in (root/'kernel/user/desktop.c').read_text() and 'drag_dx' in (root/'kernel/user/desktop.c').read_text(),
 'native kernel is an opt-in ELF64 boot path': all(k in s for k in ['BOOTKERNEL','launch_native_kernel','ExitBootServices']) and 'kernel_entry' in (root/'kernel/kernel.c').read_text(),
}
for name,ok in checks.items(): print(('PASS: ' if ok else 'FAIL: ')+name)
if not all(checks.values()): raise SystemExit(1)
