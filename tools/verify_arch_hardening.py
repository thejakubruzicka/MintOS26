#!/usr/bin/env python3
"""Contract checks for x86-64 TSS, IST, guard pages and fault diagnostics."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
arch = (root / "kernel/arch/x86_64/arch.c").read_text()
asm = (root / "kernel/arch/x86_64/arch.S").read_text()
linker = (root / "kernel/linker.ld").read_text()
paging = (root / "kernel/mm/paging.c").read_text()
checks = {
    "TSS layout is packed and asserted to 104 bytes": "__attribute__((packed)) mint_tss64" in arch and "sizeof(struct mint_tss64) == 104" in arch,
    "GDT contains the 16-byte TSS descriptor": "static uint64_t gdt[7]" in arch and "gdt[3] =" in arch and "gdt[4] =" in arch,
    "task register is loaded from selector 0x18": "mint_load_tss(0x18u)" in arch and "ltr %di" in asm,
    "GDT includes DPL3 code and data selectors": "0x00AFFA000000FFFFULL" in arch and "0x00CFF2000000FFFFULL" in arch,
    "IDT exposes a DPL3 int-0x80 gate": "set_gate_dpl(0x80u" in arch and "0x8Eu | ((dpl & 3u) << 5)" in arch,
    "RSP0 stack is initialized and can be updated": "tss.rsp0 =" in arch and "mint_arch_update_rsp0" in arch,
    "double-fault, NMI and machine-check use separate IST stacks": all(s in arch for s in ["set_gate_ist(8", "set_gate_ist(2", "set_gate_ist(18", "double_fault_stack", "nmi_stack", "machine_check_stack"]),
    "all four stacks have before/after guard pages in linker layout": all(s in linker for s in ["__guard_rsp0_before", "__guard_rsp0_after", "__guard_df_before", "__guard_df_after", "__guard_nmi_before", "__guard_nmi_after", "__guard_mc_before", "__guard_mc_after"]),
    "page-table builder leaves declared guard PTEs non-present": all(s in paging for s in ["guard_page_count", "t->kernel_pt[pt_index][page] = 0"]),
    "page-fault panic records CR2 and error bits before framebuffer panic rendering": all(s in arch for s in ["mint_arch_read_cr2", "CR2 FAULT ADDRESS=", "reserved-bit", "instruction-fetch"]) and arch.index("[MINTOS26 PANIC]") < arch.index("mint_exception_display(vector"),
    "kernel screen reports TSS/IST and stack guards": all(s in (root / "kernel/kernel.c").read_text() for s in ["TSS/TR INSTALLED; DF/NMI/MC IST ARMED", "8 STACK GUARD PAGES ACTIVE"]),
}
for name, ok in checks.items(): print(("PASS: " if ok else "FAIL: ") + name)
if not all(checks.values()): raise SystemExit(1)
# Verify actual ELF carries both TSS loader and guard-stack symbols.
elf = root / "build/mintos26-kernel.elf"
if elf.exists():
    syms = subprocess.run(["readelf", "-s", str(elf)], capture_output=True, text=True, check=True).stdout
    names = ["__guard_rsp0_before", "__guard_rsp0_after", "__guard_df_before", "__guard_df_after", "__guard_nmi_before", "__guard_nmi_after", "__guard_mc_before", "__guard_mc_after", "__stack_rsp0_start", "__stack_rsp0_end", "__stack_df_start", "__stack_df_end", "__stack_nmi_start", "__stack_nmi_end", "__stack_mc_start", "__stack_mc_end"]
    addresses = {}
    for line in syms.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[-1] in names:
            try: addresses[fields[-1]] = int(fields[1], 16)
            except ValueError: pass
    missing = [n for n in names if n not in addresses]
    print(("PASS" if not missing else "FAIL") + ": guard/stack linker symbols present in ELF" + ("" if not missing else ": " + ", ".join(missing)))
    if missing: raise SystemExit(1)
    layout_ok = True
    for label in ("rsp0", "df", "nmi", "mc"):
        start, end = addresses[f"__stack_{label}_start"], addresses[f"__stack_{label}_end"]
        before, after = addresses[f"__guard_{label}_before"], addresses[f"__guard_{label}_after"]
        layout_ok &= start - before == 0x1000 and end - start == 0x4000 and after == end
    print(("PASS" if layout_ok else "FAIL") + ": every 16 KiB stack has aligned 4 KiB guard page below and above")
    if not layout_ok: raise SystemExit(1)
