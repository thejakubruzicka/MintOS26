#include <stdint.h>
#include "../../include/arch.h"
#include "../../include/serial.h"
#include "../../include/input.h"
#include "../../include/address_space.h"
#include "../../include/process.h"
#include "../../include/task.h"

#define PIC1_COMMAND 0x20u
#define PIC1_DATA    0x21u
#define PIC2_COMMAND 0xA0u
#define PIC2_DATA    0xA1u
#define PIT_CHANNEL0 0x40u
#define PIT_COMMAND  0x43u
#define PIT_INPUT_HZ 1193182u
#define MINT_TIMER_HZ 100u

struct __attribute__((packed)) table_ptr {
    uint16_t limit;
    uint64_t base;
};
struct __attribute__((packed)) idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
};

/* Null, ring-0 code/data, and a 16-byte 64-bit TSS descriptor (entries 3+4). */
static uint64_t gdt[7] __attribute__((aligned(16))) = {
    0x0000000000000000ULL,
    0x00AF9A000000FFFFULL, /* 64-bit ring-0 executable code */
    0x00CF92000000FFFFULL, /* ring-0 writable data */
    0, 0,                 /* 16-byte 64-bit TSS descriptor */
    0x00AFFA000000FFFFULL, /* 64-bit ring-3 executable/readable code, selector 0x2b */
    0x00CFF2000000FFFFULL  /* ring-3 writable data, selector 0x33 */
};
struct __attribute__((packed)) mint_tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
_Static_assert(sizeof(struct mint_tss64) == 104, "x86-64 TSS must be 104 bytes");
_Static_assert(__builtin_offsetof(MINT_USER_TRANSITION, saved_kernel_rsp) == 0, "user transition RSP asm offset");
_Static_assert(__builtin_offsetof(MINT_USER_TRANSITION, saved_kernel_rflags) == 8, "user transition RFLAGS asm offset");
_Static_assert(__builtin_offsetof(MINT_USER_TRANSITION, exit_seen) == 16, "user transition exit flag offset");
_Static_assert(__builtin_offsetof(MINT_USER_TRANSITION, exit_code) == 24, "user transition exit code offset");

MINT_USER_TRANSITION *volatile mint_arch_user_transition_current;

void mint_arch_set_user_transition(MINT_USER_TRANSITION *transition) {
    mint_arch_user_transition_current = transition;
}

static struct mint_tss64 tss __attribute__((aligned(16)));
#define MINT_EXCEPTION_STACK_SIZE (16u * 1024u)
static uint8_t rsp0_stack[MINT_EXCEPTION_STACK_SIZE] __attribute__((aligned(4096), section(".bss.ist_rsp0")));
static uint8_t double_fault_stack[MINT_EXCEPTION_STACK_SIZE] __attribute__((aligned(4096), section(".bss.ist_df")));
static uint8_t nmi_stack[MINT_EXCEPTION_STACK_SIZE] __attribute__((aligned(4096), section(".bss.ist_nmi")));
static uint8_t machine_check_stack[MINT_EXCEPTION_STACK_SIZE] __attribute__((aligned(4096), section(".bss.ist_mc")));
static struct idt_gate idt[256] __attribute__((aligned(16)));
static volatile uint64_t timer_ticks;
static uint8_t timer_initialized;
static uint8_t pic1_mask = 0xFFu;
static uint8_t pic2_mask = 0xFFu;

extern void mint_load_gdt(const struct table_ptr *ptr);
extern void mint_load_idt(const struct table_ptr *ptr);
extern void mint_load_tss(uint16_t selector);
extern void *mint_exception_stub_table[32];
extern void mint_irq0_stub(void);
extern void mint_irq1_stub(void);
extern void mint_irq12_stub(void);
extern void mint_syscall_stub(void);

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port) : "memory");
}
static inline void io_wait(void) {
    outb(0x80u, 0u);
}

static void set_gate_ist(unsigned vector, uintptr_t handler, uint8_t ist_index) {
    struct idt_gate *g = &idt[vector];
    g->offset_low = (uint16_t)handler;
    g->selector = 0x08;
    g->ist = (uint8_t)(ist_index & 7u);
    g->attributes = 0x8E; /* present, ring 0, interrupt gate */
    g->offset_middle = (uint16_t)(handler >> 16);
    g->offset_high = (uint32_t)(handler >> 32);
    g->reserved = 0;
}
static void set_gate(unsigned vector, uintptr_t handler) {
    set_gate_ist(vector, handler, 0);
}
static void set_gate_dpl(unsigned vector, uintptr_t handler, uint8_t dpl) {
    set_gate_ist(vector, handler, 0);
    idt[vector].attributes = (uint8_t)(0x8Eu | ((dpl & 3u) << 5));
}

static void install_tss(void) {
    uintptr_t base = (uintptr_t)&tss;
    uint64_t limit = sizeof(tss) - 1u;
    tss = (struct mint_tss64){0};
    tss.rsp0 = (uintptr_t)(rsp0_stack + sizeof(rsp0_stack));
    tss.ist1 = (uintptr_t)(double_fault_stack + sizeof(double_fault_stack));
    tss.ist2 = (uintptr_t)(nmi_stack + sizeof(nmi_stack));
    tss.ist3 = (uintptr_t)(machine_check_stack + sizeof(machine_check_stack));
    tss.iomap_base = sizeof(tss); /* no I/O bitmap: TSS limit ends before it */
    gdt[3] = (limit & 0xFFFFu) |
             ((uint64_t)(base & 0xFFFFFFu) << 16) |
             (0x89ULL << 40) |
             ((limit & 0xF0000u) << 32) |
             ((uint64_t)(base & 0xFF000000u) << 32);
    gdt[4] = (uint64_t)(base >> 32);
    mint_load_tss(0x18u);
}

int mint_arch_nx_supported(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000000u), "c"(0));
    if (eax < 0x80000001u) return 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0x80000001u), "c"(0));
    return (edx & (1u << 20)) != 0; /* extended EDX.NX */
}

int mint_arch_prepare_4level_paging(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    /* LA57 cannot be toggled while long-mode paging is active. Refuse to
     * reinterpret a firmware PML5 as a PML4 rather than risk a triple fault. */
    if (cr4 & (1ULL << 12)) {
        mint_serial_write("[paging] firmware uses five-level paging; keeping its page tables\n");
        return 0;
    }
    cr4 |= (1ULL << 5) | (1ULL << 4); /* CR4.PAE + CR4.PSE */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    return 1;
}

void mint_arch_enable_nxe(void) {
    uint32_t lo, hi;
    const uint32_t msr_efer = 0xC0000080u;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr_efer));
    lo |= (1u << 11); /* EFER.NXE */
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr_efer) : "memory");
}

void mint_arch_enable_write_protect(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1ULL << 16); /* CR0.WP: supervisor writes respect read-only PTEs */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

void mint_arch_load_cr3(uint64_t physical_root) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(physical_root) : "memory");
}

void mint_arch_init(void) {
    __asm__ volatile("cli; cld" ::: "memory");
    mint_arch_user_transition_current = 0;
    struct table_ptr gdtr = { (uint16_t)(sizeof(gdt) - 1), (uint64_t)(uintptr_t)gdt };
    mint_load_gdt(&gdtr);
    install_tss();
    for (unsigned i = 0; i < 256; ++i) {
        idt[i].offset_low = 0; idt[i].selector = 0; idt[i].ist = 0;
        idt[i].attributes = 0; idt[i].offset_middle = 0; idt[i].offset_high = 0; idt[i].reserved = 0;
    }
    for (unsigned i = 0; i < 32; ++i)
        set_gate(i, (uintptr_t)mint_exception_stub_table[i]);
    set_gate_ist(2, (uintptr_t)mint_exception_stub_table[2], 2);  /* NMI IST2 */
    set_gate_ist(8, (uintptr_t)mint_exception_stub_table[8], 1);  /* double fault IST1 */
    set_gate_ist(18, (uintptr_t)mint_exception_stub_table[18], 3); /* machine check IST3 */
    set_gate(32, (uintptr_t)mint_irq0_stub); /* remapped legacy PIC IRQ0 / PIT */
    set_gate(33, (uintptr_t)mint_irq1_stub); /* PS/2 keyboard */
    set_gate(44, (uintptr_t)mint_irq12_stub); /* PS/2 auxiliary mouse */
    set_gate_dpl(0x80u, (uintptr_t)mint_syscall_stub, 3u); /* user-callable int 0x80 */
    struct table_ptr idtr = { (uint16_t)(sizeof(idt) - 1), (uint64_t)(uintptr_t)idt };
    mint_load_idt(&idtr);
    mint_serial_write("[arch] kernel GDT loaded; 64-bit TSS installed at selector 0x18\n");
    mint_serial_write("[arch] dedicated guarded stacks configured for RSP0, double fault, NMI and machine check\n");
    mint_serial_write("[arch] IDT installed for exceptions 0..31, PIT/PS2 IRQs and DPL3 int-0x80 gate\n");
    mint_serial_write("[arch] maskable interrupts remain disabled until PIT/PIC initialization\n");
}

/* Remap the legacy 8259 PIC to vectors 0x20..0x2f. Only IRQ0 is unmasked;
 * all other hardware IRQ lines remain masked until their handlers exist. */
static void remap_pic_for_timer_only(void) {
    uint8_t master_mask = 0xFFu, slave_mask = 0xFFu;
    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
    outb(PIC1_COMMAND, 0x11u); io_wait();
    outb(PIC2_COMMAND, 0x11u); io_wait();
    outb(PIC1_DATA, 0x20u);    io_wait();
    outb(PIC2_DATA, 0x28u);    io_wait();
    outb(PIC1_DATA, 0x04u);    io_wait();
    outb(PIC2_DATA, 0x02u);    io_wait();
    outb(PIC1_DATA, 0x01u);    io_wait();
    outb(PIC2_DATA, 0x01u);    io_wait();
    pic1_mask = 0xFEu; /* IRQ0 enabled, all other IRQs masked */
    pic2_mask = 0xFFu;
    outb(PIC1_DATA, pic1_mask);
    outb(PIC2_DATA, pic2_mask);
}


uint64_t mint_arch_read_cr2(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

void mint_arch_update_rsp0(uint64_t stack_top) {
    /* RSP0 is used only for privilege transitions into ring 0. */
    if ((stack_top & 0xFULL) == 0 && stack_top != 0) tss.rsp0 = stack_top;
}

uint64_t mint_arch_current_rsp0(void) { return tss.rsp0; }

void mint_timer_init(void) {
    if (timer_initialized) return;
    __asm__ volatile("cli" ::: "memory");
    timer_ticks = 0;
    remap_pic_for_timer_only();
    uint16_t divisor = (uint16_t)((PIT_INPUT_HZ + MINT_TIMER_HZ / 2u) / MINT_TIMER_HZ);
    outb(PIT_COMMAND, 0x36u); /* channel 0, lo/hi byte, rate-generator/square-wave mode */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
    timer_initialized = 1;
    mint_serial_write("[irq] legacy PIC remapped; IRQ0 alone is unmasked\n");
    mint_serial_write("[timer] PIT channel 0 configured near 100 Hz; waiting for first IRQ\n");
}

uint64_t mint_timer_ticks(void) {
    return timer_ticks;
}

void mint_arch_enable_input_irqs(int keyboard_enabled, int mouse_enabled) {
    /* IRQ2 is the cascade line and must be open for any slave-PIC device. */
    if (keyboard_enabled) pic1_mask &= (uint8_t)~0x02u;
    else pic1_mask |= 0x02u;
    if (mouse_enabled) {
        pic1_mask &= (uint8_t)~0x04u;
        pic2_mask &= (uint8_t)~0x10u; /* slave IRQ12 == slave input 4 */
    } else {
        pic2_mask |= 0x10u;
        if (!keyboard_enabled) pic1_mask |= 0x04u;
    }
    outb(PIC1_DATA, pic1_mask);
    outb(PIC2_DATA, pic2_mask);
}


/* IRQ stubs save a uniform frame. Acknowledge the originating PIC before
 * returning, including the slave PIC for IRQ12, or input/timer interrupts stop. */
void mint_irq_dispatch(const struct mint_exception_frame *frame) {
    if (!frame) return;
    if (frame->vector == 32u) {
        timer_ticks++;
        outb(PIC1_COMMAND, 0x20u);
    } else if (frame->vector == 33u) {
        mint_input_keyboard_irq();
        outb(PIC1_COMMAND, 0x20u);
    } else if (frame->vector == 44u) {
        mint_input_mouse_irq();
        outb(PIC2_COMMAND, 0x20u);
        outb(PIC1_COMMAND, 0x20u);
    } else {
        mint_serial_write("[irq] unexpected vector reached IRQ dispatcher\n");
        if (frame->vector >= 40u && frame->vector < 48u) outb(PIC2_COMMAND, 0x20u);
        outb(PIC1_COMMAND, 0x20u);
    }
}

static void serial_hex(const char *label, uint64_t v) {
    mint_serial_write(label); mint_serial_hex64(v); mint_serial_write("\n");
}

/* This is deliberately a panic path; returning would execute an exception frame
 * we do not yet support. The normal IRQ path has its own register restore/iretq. */
__attribute__((noreturn)) void mint_exception_dispatch(const struct mint_exception_frame *f) {
    /* A CPL3 exception must fail only its owning process, not halt MintOS.
     * In particular, the old `use sda/sdb` code could exhaust the small user
     * stack. Restore the kernel CR3, record a nonzero process exit code, and
     * resume the scheduler continuation saved before entering ring 3. */
    if (f && (f->cs & 3u) == 3u && f->vector < 32u &&
        mint_task_current_process() != 0 &&
        mint_task_current_user_transition() != 0) {
        uint64_t fault_address = f->vector == 14u ? mint_arch_read_cr2() : 0;
        mint_serial_write("\n[user-fault] terminating ring-3 process: vector=");
        mint_serial_dec64(f->vector);
        mint_serial_write(" RIP="); mint_serial_hex64(f->rip);
        mint_serial_write(" ERROR="); mint_serial_hex64(f->error_code);
        if (f->vector == 14u) { mint_serial_write(" CR2="); mint_serial_hex64(fault_address); }
        mint_serial_write("\n");
        if (mint_address_space_activate_kernel()) {
            mint_task_user_exit_record(0x8000000000000000ULL | (f->vector << 32) | (f->error_code & 0xFFFFFFFFu));
            mint_arch_return_from_user_fault();
        }
        mint_serial_write("[user-fault] unable to restore kernel address space; escalating to panic\n");
    }
    static const char *names[32] = {
        "DIVIDE ERROR", "DEBUG", "NON MASKABLE INTERRUPT", "BREAKPOINT",
        "OVERFLOW", "BOUND RANGE", "INVALID OPCODE", "DEVICE NOT AVAILABLE",
        "DOUBLE FAULT", "COPROCESSOR OVERRUN", "INVALID TSS", "SEGMENT NOT PRESENT",
        "STACK SEGMENT FAULT", "GENERAL PROTECTION FAULT", "PAGE FAULT", "RESERVED 15",
        "X87 FLOATING POINT", "ALIGNMENT CHECK", "MACHINE CHECK", "SIMD FLOATING POINT",
        "VIRTUALIZATION", "CONTROL PROTECTION", "RESERVED 22", "RESERVED 23",
        "RESERVED 24", "RESERVED 25", "RESERVED 26", "RESERVED 27",
        "HYPERVISOR INJECTION", "VMM COMMUNICATION", "SECURITY EXCEPTION", "RESERVED 31"
    };
    /* Capture and log the fault before touching the framebuffer. If the fault
       itself was caused by a bad framebuffer mapping, COM1 still gets the cause. */
    uint64_t vector = f ? f->vector : UINT64_MAX;
    uint64_t fault_address = (f && f->vector == 14u) ? mint_arch_read_cr2() : 0;
    mint_serial_write("\n[MINTOS26 PANIC] CPU exception: ");
    mint_serial_dec64(vector);
    mint_serial_write(" ");
    if (f && f->vector < 32) mint_serial_write(names[f->vector]);
    mint_serial_write("\n");
    if (f) { serial_hex("RIP=", f->rip); serial_hex("ERROR=", f->error_code); serial_hex("RFLAGS=", f->rflags); }
    if (f && f->vector == 14u) {
        serial_hex("CR2 FAULT ADDRESS=", fault_address);
        mint_serial_write("PAGE FAULT FLAGS: ");
        mint_serial_write((f->error_code & 1u) ? "protection-violation " : "not-present ");
        mint_serial_write((f->error_code & 2u) ? "write " : "read ");
        mint_serial_write((f->error_code & 4u) ? "user " : "supervisor ");
        if (f->error_code & 8u) mint_serial_write("reserved-bit ");
        if (f->error_code & 16u) mint_serial_write("instruction-fetch ");
        mint_serial_write("\n");
    }
    mint_exception_display(vector, f ? f->rip : 0, f ? f->error_code : 0);
    for (;;) __asm__ volatile("cli; hlt" ::: "memory");
}
