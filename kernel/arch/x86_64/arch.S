.section .text.arch,"ax",@progbits
.code64

.global mint_load_gdt
.type mint_load_gdt,@function
mint_load_gdt:
    lgdt (%rdi)
    pushq $0x08
    leaq .Lmint_gdt_reloaded(%rip), %rax
    pushq %rax
    lretq
.Lmint_gdt_reloaded:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    retq
.size mint_load_gdt, .-mint_load_gdt

.global mint_load_idt
.type mint_load_idt,@function
mint_load_idt:
    lidt (%rdi)
    retq
.size mint_load_idt, .-mint_load_idt

.global mint_load_tss
.type mint_load_tss,@function
mint_load_tss:
    ltr %di
    retq
.size mint_load_tss, .-mint_load_tss

.macro MINT_ISR_NOERR n
.global mint_isr_\n
.type mint_isr_\n,@function
mint_isr_\n:
    pushq $0
    pushq $\n
    jmp mint_isr_common
.endm
.macro MINT_ISR_ERR n
.global mint_isr_\n
.type mint_isr_\n,@function
mint_isr_\n:
    pushq $\n
    jmp mint_isr_common
.endm

MINT_ISR_NOERR 0
MINT_ISR_NOERR 1
MINT_ISR_NOERR 2
MINT_ISR_NOERR 3
MINT_ISR_NOERR 4
MINT_ISR_NOERR 5
MINT_ISR_NOERR 6
MINT_ISR_NOERR 7
MINT_ISR_ERR   8
MINT_ISR_NOERR 9
MINT_ISR_ERR   10
MINT_ISR_ERR   11
MINT_ISR_ERR   12
MINT_ISR_ERR   13
MINT_ISR_ERR   14
MINT_ISR_NOERR 15
MINT_ISR_NOERR 16
MINT_ISR_ERR   17
MINT_ISR_NOERR 18
MINT_ISR_NOERR 19
MINT_ISR_NOERR 20
MINT_ISR_ERR   21
MINT_ISR_NOERR 22
MINT_ISR_NOERR 23
MINT_ISR_NOERR 24
MINT_ISR_NOERR 25
MINT_ISR_NOERR 26
MINT_ISR_NOERR 27
MINT_ISR_NOERR 28
MINT_ISR_ERR   29
MINT_ISR_ERR   30
MINT_ISR_NOERR 31

.type mint_isr_common,@function
mint_isr_common:
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
    movq %rsp, %rdi
    andq $-16, %rsp
    callq mint_exception_dispatch
.Lmint_exception_halt:
    cli
    hlt
    jmp .Lmint_exception_halt

.section .rodata.isr_table,"a",@progbits
.align 8
.global mint_exception_stub_table
.type mint_exception_stub_table,@object
mint_exception_stub_table:
    .quad mint_isr_0, mint_isr_1, mint_isr_2, mint_isr_3
    .quad mint_isr_4, mint_isr_5, mint_isr_6, mint_isr_7
    .quad mint_isr_8, mint_isr_9, mint_isr_10, mint_isr_11
    .quad mint_isr_12, mint_isr_13, mint_isr_14, mint_isr_15
    .quad mint_isr_16, mint_isr_17, mint_isr_18, mint_isr_19
    .quad mint_isr_20, mint_isr_21, mint_isr_22, mint_isr_23
    .quad mint_isr_24, mint_isr_25, mint_isr_26, mint_isr_27
    .quad mint_isr_28, mint_isr_29, mint_isr_30, mint_isr_31
.size mint_exception_stub_table, .-mint_exception_stub_table


.section .text.irq,"ax",@progbits
.code64
/* Legacy PIC IRQ0 (PIT). Unlike exception stubs this path restores the saved
 * context and iretq returns to the interrupted instruction stream. */
.global mint_irq0_stub
.type mint_irq0_stub,@function
mint_irq0_stub:
    pushq $0
    pushq $32
    jmp mint_irq_common
.size mint_irq0_stub, .-mint_irq0_stub

/* Native PS/2 keyboard and auxiliary mouse interrupt vectors. */
.global mint_irq1_stub
.type mint_irq1_stub,@function
mint_irq1_stub:
    pushq $0
    pushq $33
    jmp mint_irq_common
.size mint_irq1_stub, .-mint_irq1_stub

.global mint_irq12_stub
.type mint_irq12_stub,@function
mint_irq12_stub:
    pushq $0
    pushq $44
    jmp mint_irq_common
.size mint_irq12_stub, .-mint_irq12_stub

.type mint_irq_common,@function
mint_irq_common:
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
    /* RBX is callee-saved in the C ABI and retains the exact saved-frame RSP. */
    movq %rsp, %rbx
    movq %rsp, %rdi
    andq $-16, %rsp
    callq mint_irq_dispatch
    movq %rbx, %rsp
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
    addq $16, %rsp /* discard vector and synthetic error code */
    iretq
.size mint_irq_common, .-mint_irq_common

.section .text.task_switch,"ax",@progbits
.code64
/* Cooperative context switch. Saves RFLAGS as well as the SysV callee-saved
 * registers and RSP; new_rsp points at r15,r14,r13,r12,rbx,rbp,rflags,ret.
 * Saving IF is essential: user int-0x80 handlers enter with IF clear, while
 * normal kernel tasks and the scheduler commonly resume with IF set. */
.global mint_context_switch
.type mint_context_switch,@function
mint_context_switch:
    pushfq
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    popfq
    retq
.size mint_context_switch, .-mint_context_switch

.section .note.GNU-stack,"",@progbits


.section .text.syscall,"ax",@progbits
.code64
/* Ring-3 user ABI: RAX=number, RDI,RSI,RDX,R10,R8,R9=arguments.
 * The C dispatcher validates/copies every user pointer before use. */
.global mint_syscall_stub
.type mint_syscall_stub,@function
mint_syscall_stub:
    pushq $0
    pushq $128
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
    callq mint_syscall_trap_dispatch
    movq %rbx, %rsp
    movq %rax, 112(%rbx) /* saved user RAX return value */
    movabsq $0x4D494E5445584954, %r10 /* MINTEXIT: this task requested process exit */
    cmpq %r10, %rax
    je .Lmint_user_exit
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
    addq $16, %rsp /* vector and synthetic error code */
    iretq
.Lmint_user_exit:
    jmp mint_arch_return_from_user_fault

/* Common non-iret return used for both SYS_EXIT and contained user faults.
 * The C caller must have switched to the kernel CR3 and recorded an exit code.
 * It abandons the privilege-transition/exception stack and resumes exactly
 * after mint_arch_enter_user() on the scheduler task's original kernel stack. */
.global mint_arch_return_from_user_fault
.type mint_arch_return_from_user_fault,@function
mint_arch_return_from_user_fault:
    cli
    movq mint_arch_user_transition_current(%rip), %r10
    testq %r10, %r10
    jz .Lmint_user_exit_fatal
    movq 0(%r10), %rsp
    movq $0, 0(%r10)
    movq 8(%r10), %rax
    movq $0, 8(%r10)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    pushq %rax
    popfq
    retq
.size mint_arch_return_from_user_fault, .-mint_arch_return_from_user_fault
.Lmint_user_exit_fatal:
    cli
.Lmint_user_exit_halt:
    hlt
    jmp .Lmint_user_exit_halt
.size mint_syscall_stub, .-mint_syscall_stub

.global mint_arch_enter_user
.type mint_arch_enter_user,@function
mint_arch_enter_user:
    /* Preserve the kernel task's ABI callee-saved registers and stack. The
     * single-core first user task has a dedicated TSS.RSP0 privilege stack. */
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq mint_arch_user_transition_current(%rip), %rax
    testq %rax, %rax
    jz .Lmint_user_enter_fatal
    movq %rsp, 0(%rax)
    /* Preserve this task's kernel interrupt state. SYS_EXIT returns to this
     * stack instead of iretq, so flags must be restored from task-local state. */
    pushfq
    popq %rcx
    movq %rcx, 8(%rax)
    movq %rdi, %r12 /* user entry RIP */
    movq %rsi, %r13 /* user stack top */
    pushq $0x33     /* ring-3 SS */
    pushq %r13      /* ring-3 RSP */
    pushfq
    popq %rax
    btrq $12, %rax  /* never expose IOPL to user mode */
    btrq $13, %rax
    btsq $9, %rax   /* user returns with maskable interrupts enabled */
    pushq %rax
    pushq $0x2b     /* ring-3 CS */
    pushq %r12      /* user RIP */
    iretq
.Lmint_user_enter_fatal:
    cli
.Lmint_user_enter_halt:
    hlt
    jmp .Lmint_user_enter_halt
.size mint_arch_enter_user, .-mint_arch_enter_user

.section .note.GNU-stack,"",@progbits
