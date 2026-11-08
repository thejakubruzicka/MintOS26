.section .text.entry,"ax",@progbits
.code64
.global _start
.type _start,@function
.extern mint_user_main
.extern mint_exit
_start:
    xorq %rbp, %rbp
    call mint_user_main
    movq %rax, %rdi
    call mint_exit
    ud2
.size _start, .-_start
.section .note.GNU-stack,"",@progbits
