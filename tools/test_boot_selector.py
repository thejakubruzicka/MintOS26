#!/usr/bin/env python3
"""Static checks for the safe-default MintDE startup selector; not a VM test."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'uefi/main.c').read_text()
k=(root/'kernel/kernel.c').read_text()
checks={
    'EFI image handle is retained for ExitBootServices': 'boot_image_handle=image;' in s and 'exit_bs(boot_image_handle,map_key)' in s,
    'startup selector is displayed before UEFI main loop': 'if(wait_for_boot_choice())' in s and s.index('if(wait_for_boot_choice())') < s.index('for(;;){ticks++;'),
    'default path remains UEFI Desktop after timeout': 'for(int seconds=3;seconds>0;--seconds)' in s and 'return 0;' in s[s.index('static int wait_for_boot_choice'):s.index('static void execute_calc')],
    'keyboard selects MintDE experimental handoff': "c=='2'||c=='m'||c=='M'" in s and 'launch_native_kernel()' in s,
    'keyboard selects UEFI Desktop': "c=='1'||c=='u'||c=='U'||c==13" in s,
    'direct handoff is visibly identified as experimental': 'Native kernel / experimental one-way handoff' in s,
    'About MintOS and About MintDE remain distinct': 'Version 26.5.012.0  •  x86-64' in s and 'Version 2.2.0  •  Native desktop environment' in s,
    'native kernel identifies 26.5.012.0': '26.5.012.0' in k,
}
for name,ok in checks.items(): print(('PASS: ' if ok else 'FAIL: ')+name)
if not all(checks.values()): raise SystemExit(1)
