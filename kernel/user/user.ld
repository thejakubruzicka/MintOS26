OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
PHDRS
{
  text PT_LOAD FLAGS(5); /* PF_R | PF_X */
  data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}
SECTIONS
{
  . = 0x1000200000;
  .text ALIGN(0x1000) : { *(.text.entry) *(.text .text.*) *(.ltext .ltext.*) *(.rodata .rodata.*) *(.lrodata .lrodata.*) } :text
  . = ALIGN(0x1000);
  .data : { *(.data .data.*) *(.bss .bss.*) *(.lbss .lbss.*) *(COMMON) } :data
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
