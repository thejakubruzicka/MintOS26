/* 26.5.014.0 boot self-test: each copy fills XMM0-15 (and the YMM upper
 * halves when AVX is enabled) with a pattern derived from its pid, then
 * yields through int 0x80 and spins without syscalls (so the timer preempts
 * it) many times, and finally checks that every register still holds its
 * own pattern. Two copies run at once; a missing or wrong save/restore in
 * the scheduler shows up as the other copy's pattern. Built with SSE
 * enabled, unlike MintDE. Exit status 0 = pass. */
#include "../include/mintos/runtime.h"

#define ROUNDS 40u
#define SPIN 400000u

static uint64_t pattern[32] __attribute__((aligned(32)));
static uint64_t result[32] __attribute__((aligned(32)));
static uint64_t upper_pattern[32] __attribute__((aligned(32)));
static uint64_t upper_result[32] __attribute__((aligned(32)));

static void run_xmm_rounds(void) {
    __asm__ volatile(
        "movdqu 0(%0), %%xmm0\n\t"   "movdqu 16(%0), %%xmm1\n\t"  "movdqu 32(%0), %%xmm2\n\t"
        "movdqu 48(%0), %%xmm3\n\t"  "movdqu 64(%0), %%xmm4\n\t"  "movdqu 80(%0), %%xmm5\n\t"
        "movdqu 96(%0), %%xmm6\n\t"  "movdqu 112(%0), %%xmm7\n\t" "movdqu 128(%0), %%xmm8\n\t"
        "movdqu 144(%0), %%xmm9\n\t" "movdqu 160(%0), %%xmm10\n\t" "movdqu 176(%0), %%xmm11\n\t"
        "movdqu 192(%0), %%xmm12\n\t" "movdqu 208(%0), %%xmm13\n\t" "movdqu 224(%0), %%xmm14\n\t"
        "movdqu 240(%0), %%xmm15\n\t"
        "movq %2, %%r12\n\t"
        "1:\n\t"
        "movq $24, %%rax\n\t"          /* MINT_SYS_SCHED_YIELD */
        "int $0x80\n\t"
        "movq %3, %%r13\n\t"
        "2: pause\n\t"
        "decq %%r13\n\t"
        "jnz 2b\n\t"
        "decq %%r12\n\t"
        "jnz 1b\n\t"
        "movdqu %%xmm0, 0(%1)\n\t"   "movdqu %%xmm1, 16(%1)\n\t"  "movdqu %%xmm2, 32(%1)\n\t"
        "movdqu %%xmm3, 48(%1)\n\t"  "movdqu %%xmm4, 64(%1)\n\t"  "movdqu %%xmm5, 80(%1)\n\t"
        "movdqu %%xmm6, 96(%1)\n\t"  "movdqu %%xmm7, 112(%1)\n\t" "movdqu %%xmm8, 128(%1)\n\t"
        "movdqu %%xmm9, 144(%1)\n\t" "movdqu %%xmm10, 160(%1)\n\t" "movdqu %%xmm11, 176(%1)\n\t"
        "movdqu %%xmm12, 192(%1)\n\t" "movdqu %%xmm13, 208(%1)\n\t" "movdqu %%xmm14, 224(%1)\n\t"
        "movdqu %%xmm15, 240(%1)\n\t"
        :
        : "r"(pattern), "r"(result), "i"(ROUNDS), "i"(SPIN)
        : "rax", "r12", "r13", "memory", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

/* YMM upper halves: load full YMM registers, keep them across rounds,
 * then extract only the upper 128 bits for comparison. */
__attribute__((target("avx")))
static void run_ymm_rounds(void) {
    __asm__ volatile(
        "vmovdqu 0(%0), %%ymm0\n\t"   "vmovdqu 32(%0), %%ymm1\n\t"  "vmovdqu 64(%0), %%ymm2\n\t"
        "vmovdqu 96(%0), %%ymm3\n\t"  "vmovdqu 128(%0), %%ymm4\n\t" "vmovdqu 160(%0), %%ymm5\n\t"
        "vmovdqu 192(%0), %%ymm6\n\t" "vmovdqu 224(%0), %%ymm7\n\t"
        "movq %2, %%r12\n\t"
        "1:\n\t"
        "movq $24, %%rax\n\t"
        "int $0x80\n\t"
        "movq %3, %%r13\n\t"
        "2: pause\n\t"
        "decq %%r13\n\t"
        "jnz 2b\n\t"
        "decq %%r12\n\t"
        "jnz 1b\n\t"
        "vmovdqu %%ymm0, 0(%1)\n\t"   "vmovdqu %%ymm1, 32(%1)\n\t"  "vmovdqu %%ymm2, 64(%1)\n\t"
        "vmovdqu %%ymm3, 96(%1)\n\t"  "vmovdqu %%ymm4, 128(%1)\n\t" "vmovdqu %%ymm5, 160(%1)\n\t"
        "vmovdqu %%ymm6, 192(%1)\n\t" "vmovdqu %%ymm7, 224(%1)\n\t"
        "vzeroupper\n\t"
        :
        : "r"(upper_pattern), "r"(upper_result), "i"(ROUNDS / 2u), "i"(SPIN)
        : "rax", "r12", "r13", "memory", "cc",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

static int avx_usable(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    if (!(c & (1u << 27)) || !(c & (1u << 28))) return 0;   /* OSXSAVE, AVX */
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
    return (lo & 6u) == 6u;
}

static double series(uint32_t n) {
    double sum = 0.0;
    for (uint32_t k = 1; k <= n; ++k) sum += 1.0 / ((double)k * (double)k);
    return sum;
}

static void say(const char *text) {
    size_t n = 0;
    while (text[n]) ++n;
    (void)mint_write(1, text, n);
}

int64_t mint_user_main(void) {
    uint64_t pid = (uint64_t)mint_getpid();
    uint64_t seed = pid * 0x9E3779B97F4A7C15ULL;
    for (uint32_t i = 0; i < 32u; ++i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        pattern[i] = seed ^ pid;
        upper_pattern[i] = ~seed ^ (pid << 32);
    }
    run_xmm_rounds();
    for (uint32_t i = 0; i < 32u; ++i)
        if (result[i] != pattern[i]) { say("[fputest] XMM register changed across a task switch\n"); return 10 + i; }
    if (avx_usable()) {
        run_ymm_rounds();
        for (uint32_t i = 0; i < 32u; ++i)
            if (upper_result[i] != upper_pattern[i]) { say("[fputest] YMM register changed across a task switch\n"); return 50 + i; }
        say("[fputest] AVX YMM state verified\n");
    }
    /* Ordinary floating-point work (sum of 1/k^2 -> pi^2/6). */
    double s = series(20000u);
    if (!(s > 1.6448 && s < 1.6450)) { say("[fputest] double arithmetic gave a wrong result\n"); return 90; }
    uint32_t mxcsr = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    if ((mxcsr & 0xFFC0u) != 0x1F80u) { say("[fputest] MXCSR did not start in the default state\n"); return 91; }
    say("[fputest] XMM state and double arithmetic verified\n");
    return 0;
}
