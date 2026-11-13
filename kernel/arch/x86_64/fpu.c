/* 26.5.014.0: FPU/SSE/AVX enablement and per-task state save/restore.
 * Only inline assembly touches the floating-point unit here; the C code is
 * compiled -mgeneral-regs-only like the rest of the kernel. */
#include <stdint.h>
#include "../../include/fpu.h"
#ifndef MINT_FPU_HOST_TEST
#include "../../include/serial.h"
#include "../../include/log.h"
#endif

void mint_fpu_state_make_clean(MINT_FPU_STATE *state) {
    if (!state) return;
    for (uint32_t i = 0; i < MINT_FPU_AREA_BYTES; ++i) state->bytes[i] = 0;
    state->bytes[MINT_FPU_FCW_OFFSET] = (uint8_t)(MINT_FPU_DEFAULT_FCW & 0xFFu);
    state->bytes[MINT_FPU_FCW_OFFSET + 1u] = (uint8_t)(MINT_FPU_DEFAULT_FCW >> 8);
    /* Abridged tag word 0 = every x87 register empty. */
    state->bytes[MINT_FPU_MXCSR_OFFSET] = (uint8_t)(MINT_FPU_DEFAULT_MXCSR & 0xFFu);
    state->bytes[MINT_FPU_MXCSR_OFFSET + 1u] = (uint8_t)(MINT_FPU_DEFAULT_MXCSR >> 8);
    /* XSAVE header (offset 512) stays zero: XSTATE_BV = 0 makes XRSTOR load
     * the init state of x87/SSE/AVX; MXCSR is still read from offset 24. */
}

#ifndef MINT_FPU_HOST_TEST
static uint32_t fpu_flags;
static uint32_t fpu_area_bytes;
static uint64_t fpu_xcr0;
static uint64_t fpu_saves, fpu_restores;

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t ea, eb, ec, ed;
    __asm__ volatile("cpuid" : "=a"(ea), "=b"(eb), "=c"(ec), "=d"(ed) : "a"(leaf), "c"(sub));
    if (a) *a = ea;
    if (b) *b = eb;
    if (c) *c = ec;
    if (d) *d = ed;
}

int mint_fpu_init(void) {
    uint32_t max_leaf = 0, ecx = 0, edx = 0;
    fpu_flags = 0; fpu_area_bytes = 0; fpu_xcr0 = 0;
    cpuid(0, 0, &max_leaf, 0, 0, 0);
    if (max_leaf < 1u) return 0;
    cpuid(1, 0, 0, 0, &ecx, &edx);
    int has_fxsr = (edx & (1u << 24)) != 0;
    int has_sse = (edx & (1u << 25)) != 0 && (edx & (1u << 26)) != 0; /* SSE + SSE2 */
    int has_xsave = (ecx & (1u << 26)) != 0;
    int has_avx = (ecx & (1u << 28)) != 0;
    if (!has_fxsr || !has_sse) {
        mint_serial_write("[fpu] CPU lacks FXSR/SSE2; ring 3 stays integer-only\n");
        return 0;
    }
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ULL << 2) | (1ULL << 3)); /* EM=0: no x87 emulation; TS=0: no lazy #NM */
    cr0 |= (1ULL << 1) | (1ULL << 5);    /* MP=1, NE=1: native x87 error reporting */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10);   /* OSFXSR + OSXMMEXCPT (#XM, not #UD, on SIMD faults) */
    if (has_xsave) cr4 |= (1ULL << 18);  /* OSXSAVE */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    fpu_flags = MINT_FPU_FLAG_SSE | MINT_FPU_FLAG_FXSR;
    fpu_area_bytes = 512u;
    if (has_xsave && max_leaf >= 0xDu) {
        uint32_t supported_lo = 0, supported_hi = 0;
        cpuid(0xD, 0, &supported_lo, 0, 0, &supported_hi);
        uint64_t supported = ((uint64_t)supported_hi << 32) | supported_lo;
        uint64_t want = 0x3u;                         /* x87 | SSE */
        if (has_avx && (supported & 0x4u)) want |= 0x4u; /* AVX (YMM upper halves) */
        want &= supported;
        if ((want & 0x3u) == 0x3u) {
            __asm__ volatile("xsetbv" :: "c"(0u), "a"((uint32_t)want), "d"((uint32_t)(want >> 32)) : "memory");
            uint32_t size = 0;
            cpuid(0xD, 0, 0, &size, 0, 0);            /* EBX: size for the enabled XCR0 */
            if (size >= 576u && size <= MINT_FPU_AREA_BYTES) {
                fpu_xcr0 = want;
                fpu_area_bytes = size;
                fpu_flags |= MINT_FPU_FLAG_XSAVE;
                if (want & 0x4u) fpu_flags |= MINT_FPU_FLAG_AVX;
            } else {
                /* Should not happen for x87|SSE|AVX; keep FXSAVE as the fallback. */
                __asm__ volatile("xsetbv" :: "c"(0u), "a"(3u), "d"(0u) : "memory");
                fpu_xcr0 = 3u;
            }
        }
    }
    /* Leave the live registers in the clean state too. */
    __asm__ volatile("fninit" ::: "memory");
    uint32_t mxcsr = MINT_FPU_DEFAULT_MXCSR;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr) : "memory");
    fpu_flags |= MINT_FPU_FLAG_READY;
    mint_serial_write("[fpu] x87/SSE enabled: CR0.MP|NE, CR4.OSFXSR|OSXMMEXCPT");
    if (fpu_flags & MINT_FPU_FLAG_XSAVE) {
        mint_serial_write(" + OSXSAVE; XCR0=");
        mint_serial_hex64(fpu_xcr0);
        mint_serial_write((fpu_flags & MINT_FPU_FLAG_AVX) ? " (x87, SSE, AVX)" : " (x87, SSE)");
    } else {
        mint_serial_write("; FXSAVE area");
    }
    mint_serial_write(" save area ");
    mint_serial_dec64(fpu_area_bytes);
    mint_serial_write(" bytes, eager per-task switching\n");
    return 1;
}

int mint_fpu_ready(void) { return (fpu_flags & MINT_FPU_FLAG_READY) != 0; }

void mint_fpu_save(MINT_FPU_STATE *state) {
    if (!state || !(fpu_flags & MINT_FPU_FLAG_READY)) return;
    if (fpu_flags & MINT_FPU_FLAG_XSAVE) {
        __asm__ volatile("xsave64 %0" : "+m"(*state)
                         : "a"((uint32_t)fpu_xcr0), "d"((uint32_t)(fpu_xcr0 >> 32)) : "memory");
    } else {
        __asm__ volatile("fxsave64 %0" : "+m"(*state) :: "memory");
    }
    ++fpu_saves;
}

void mint_fpu_restore(const MINT_FPU_STATE *state) {
    if (!state || !(fpu_flags & MINT_FPU_FLAG_READY)) return;
    if (fpu_flags & MINT_FPU_FLAG_XSAVE) {
        __asm__ volatile("xrstor64 %0" :: "m"(*state),
                         "a"((uint32_t)fpu_xcr0), "d"((uint32_t)(fpu_xcr0 >> 32)) : "memory");
    } else {
        __asm__ volatile("fxrstor64 %0" :: "m"(*state) : "memory");
    }
    ++fpu_restores;
}

uint32_t mint_fpu_read_mxcsr(void) {
    uint32_t value = 0;
    if (fpu_flags & MINT_FPU_FLAG_READY) __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
}

void mint_fpu_get_info(MINT_FPU_INFO *out) {
    if (!out) return;
    out->version = MINT_FPU_INFO_VERSION;
    out->flags = fpu_flags;
    out->area_bytes = fpu_area_bytes;
    out->reserved = 0;
    out->xcr0 = fpu_xcr0;
    out->saves = fpu_saves;
    out->restores = fpu_restores;
}
#endif
