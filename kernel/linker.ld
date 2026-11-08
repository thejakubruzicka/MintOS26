OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_entry)
PHDRS
{
  text PT_LOAD FLAGS(5);    /* PF_R | PF_X */
  rodata PT_LOAD FLAGS(4);  /* PF_R; non-executable when NX is available */
  data PT_LOAD FLAGS(6);    /* PF_R | PF_W */
}
SECTIONS
{
  /* The UEFI loader reserves these fixed physical/virtual addresses. */
  . = 0x04000000;
  __kernel_start = .;
  .text : ALIGN(0x1000) {
    __kernel_text_start = .;
    KEEP(*(.text.entry)) *(.text .text.*)
    __kernel_text_end = .;
  } :text
  .rodata : ALIGN(0x1000) {
    __kernel_rodata_start = .;
    *(.rodata .rodata.*)
    __kernel_rodata_end = .;
  } :rodata
  .data : ALIGN(0x1000) {
    __kernel_data_start = .;
    *(.data .data.*)
  } :data
  /* Emergency stacks each sit between unmapped 4 KiB guard pages. The kernel
     page-table builder removes these pages from its identity map at runtime. */
  .guarded_stacks (NOLOAD) : ALIGN(0x1000) {
    __guard_rsp0_before = .; . += 0x1000;
    __stack_rsp0_start = .; KEEP(*(.bss.ist_rsp0)); __stack_rsp0_end = .;
    __guard_rsp0_after = .; . += 0x1000;
    __guard_df_before = .; . += 0x1000;
    __stack_df_start = .; KEEP(*(.bss.ist_df)); __stack_df_end = .;
    __guard_df_after = .; . += 0x1000;
    __guard_nmi_before = .; . += 0x1000;
    __stack_nmi_start = .; KEEP(*(.bss.ist_nmi)); __stack_nmi_end = .;
    __guard_nmi_after = .; . += 0x1000;
    __guard_mc_before = .; . += 0x1000;
    __stack_mc_start = .; KEEP(*(.bss.ist_mc)); __stack_mc_end = .;
    __guard_mc_after = .; . += 0x1000;
  } :data
  .bss : ALIGN(0x1000) { *(COMMON) *(.bss .bss.*) } :data
  __kernel_end = ALIGN(., 0x1000);
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
