#ifndef MINTOS26_FPU_H
#define MINTOS26_FPU_H
/* 26.5.014.0: x87/SSE/AVX state for ring-3 code.
 *
 * The kernel itself is compiled -mgeneral-regs-only and never touches FPU,
 * SSE or AVX registers, so whatever is in those registers always belongs to
 * the user task that ran last. The scheduler saves that state into the task
 * when the task switches out and restores the next user task's state before
 * it runs (eager switching; no CR0.TS / #NM laziness). */
#include <stdint.h>

/* x87 + SSE + AVX in the standard XSAVE layout is 832 bytes. AVX-512 state is
 * deliberately left disabled in XCR0 so the area never exceeds this size. */
#define MINT_FPU_AREA_BYTES 1024u

typedef struct mint_fpu_state {
    uint8_t bytes[MINT_FPU_AREA_BYTES];
} __attribute__((aligned(64))) MINT_FPU_STATE;

/* Offsets inside the legacy (FXSAVE) region shared by FXSAVE and XSAVE. */
#define MINT_FPU_FCW_OFFSET 0u
#define MINT_FPU_MXCSR_OFFSET 24u
#define MINT_FPU_XSTATE_BV_OFFSET 512u
#define MINT_FPU_DEFAULT_FCW 0x037Fu
#define MINT_FPU_DEFAULT_MXCSR 0x1F80u

#define MINT_FPU_FLAG_SSE    (1u << 0)
#define MINT_FPU_FLAG_FXSR   (1u << 1)
#define MINT_FPU_FLAG_XSAVE  (1u << 2)
#define MINT_FPU_FLAG_AVX    (1u << 3)
#define MINT_FPU_FLAG_READY  (1u << 4)

typedef struct mint_fpu_info {
    uint32_t version;
    uint32_t flags;          /* MINT_FPU_FLAG_* */
    uint32_t area_bytes;     /* bytes the CPU writes per save */
    uint32_t reserved;
    uint64_t xcr0;           /* enabled state components (0 without XSAVE) */
    uint64_t saves;
    uint64_t restores;
} MINT_FPU_INFO;
#define MINT_FPU_INFO_VERSION 1u

/* Pure helper (host-tested): write the architectural clean state (FCW 0x37F,
 * MXCSR 0x1F80 with all exceptions masked, empty x87 stack, zeroed XMM/YMM,
 * XSTATE_BV = 0) into an area. */
void mint_fpu_state_make_clean(MINT_FPU_STATE *state);

#ifndef MINT_FPU_HOST_TEST
/* Programs CR0/CR4 (and XCR0 when XSAVE exists). Returns 1 when SSE state
 * switching is available, 0 when the CPU lacks FXSR/SSE (ring 3 then stays
 * integer-only and SSE programs fault with #UD instead of corrupting state). */
int mint_fpu_init(void);
int mint_fpu_ready(void);
void mint_fpu_save(MINT_FPU_STATE *state);
void mint_fpu_restore(const MINT_FPU_STATE *state);
void mint_fpu_get_info(MINT_FPU_INFO *out);
uint32_t mint_fpu_read_mxcsr(void);
#endif

#endif
