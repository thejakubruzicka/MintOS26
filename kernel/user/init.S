.section .text.entry,"ax",@progbits
.code64
.global _start
.type _start,@function
_start:
    movq $1, %rax                 /* write(1, banner, len) */
    movq $1, %rdi
    leaq .Lbanner(%rip), %rsi
    movq $(.Lbanner_end-.Lbanner), %rdx
    int $0x80
    cmpq $(.Lbanner_end-.Lbanner), %rax
    jne .Lfail

    /* Yield from CPL3. The kernel must preserve this syscall frame and resume
     * the same process after another task may have run under another CR3. */
    movq $24, %rax                /* sched_yield() */
    int $0x80
    testq %rax, %rax
    jne .Lfail

    movq $39, %rax                /* getpid() */
    int $0x80
    testq %rax, %rax
    jz .Lfail

    /* Deliberately unmapped but in the user address window. */
    movq $1, %rax
    movq $1, %rdi
    movabsq $0x1000230000, %rsi
    movq $1, %rdx
    int $0x80
    cmpq $-14, %rax               /* expect -EFAULT */
    jne .Lfail

    movq $228, %rax               /* clock_gettime(CLOCK_MONOTONIC, &clock_buf) */
    movq $1, %rdi
    leaq clock_buf(%rip), %rsi
    int $0x80
    testq %rax, %rax
    jne .Lfail
    cmpq $1000000000, clock_buf+8(%rip)
    jae .Lfail

    /* A second yield tests restoring the saved user RIP/RSP and CR3. */
    movq $24, %rax
    int $0x80
    testq %rax, %rax
    jne .Lfail

    movq $1, %rax
    movq $1, %rdi
    leaq .Lpassed(%rip), %rsi
    movq $(.Lpassed_end-.Lpassed), %rdx
    int $0x80
    movq $60, %rax                /* exit(0) */
    xorq %rdi, %rdi
    int $0x80
    ud2
.Lfail:
    movq $1, %rax
    movq $2, %rdi
    leaq .Lfailed(%rip), %rsi
    movq $(.Lfailed_end-.Lfailed), %rdx
    int $0x80
    movq $60, %rax
    movq $1, %rdi
    int $0x80
    ud2

.Lbanner:
    .ascii "[user-elf] static ELF64 image entered at CPL3; scheduler-safe yield online\n"
.Lbanner_end:
.Lpassed:
    .ascii "[user-elf] write/getpid/clock_gettime/EFAULT/exit passed\n"
.Lpassed_end:
.Lfailed:
    .ascii "[user-elf] syscall validation FAILED\n"
.Lfailed_end:

.section .data,"aw",@progbits
.p2align 3
.global user_data_tag
user_data_tag:
    .quad 0x4D494E5455534552

.section .bss,"aw",@nobits
.p2align 3
.global clock_buf
clock_buf:
    .skip 16

.section .note.GNU-stack,"",@progbits
