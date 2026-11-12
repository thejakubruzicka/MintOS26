/* 26.5.014.0: `syscall` instruction entry for the Linux x86-64 compatibility
 * subset (kernel/abi/linux_compat.c). MintOS's own ABI stays int 0x80.
 *
 * On entry (SFMASK has cleared IF/TF/DF/AC/NT): RCX = user RIP,
 * R11 = user RFLAGS, RSP = user stack, RAX = Linux syscall number,
 * RDI, RSI, RDX, R10, R8, R9 = arguments. Single core: the user RSP is
 * parked in a scratch word until the stack has switched to this task's RSP0.
 * The frame built here is identical to the int 0x80 frame (MINT_SYSCALL_FRAME),
 * and the return path is IRETQ (SYSRET is never used). */
.section .bss.linux_syscall,"aw",@nobits
.align 8
mint_linux_user_rsp_scratch:
    .quad 0

.section .text.linux_syscall,"ax",@progbits
.code64
.global mint_linux_syscall_entry
.type mint_linux_syscall_entry,@function
mint_linux_syscall_entry:
    movq %rsp, mint_linux_user_rsp_scratch(%rip)
    movq mint_arch_rsp0_shadow(%rip), %rsp
    pushq $0x33                                /* user SS */
    pushq mint_linux_user_rsp_scratch(%rip)    /* user RSP */
    pushq %r11                                 /* user RFLAGS */
    pushq $0x2b                                /* user CS */
    pushq %rcx                                 /* user RIP */
    pushq $0                                   /* error code slot */
    pushq $0x81                                /* vector slot: Linux syscall */
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rbx
    movq %rsp, %rdi
    andq $-16, %rsp
    callq mint_linux_syscall_dispatch
    movq %rbx, %rsp
    movq %rax, 112(%rbx)                       /* saved RAX = return value */
    movabsq $0x4D494E5445584954, %r10          /* MINTEXIT: exit_group or killed */
    cmpq %r10, %rax
    je .Lmint_linux_exit
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq
.Lmint_linux_exit:
    jmp mint_arch_return_from_user_fault
.size mint_linux_syscall_entry, .-mint_linux_syscall_entry

.section .note.GNU-stack,"",@progbits
