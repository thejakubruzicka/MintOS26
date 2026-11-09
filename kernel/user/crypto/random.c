/* Randomness for TLS key shares and nonces (MintOS ring 3, x86-64).
 *
 * Seed: RDSEED or RDRAND output when CPUID reports them (Skylake and newer,
 * QEMU -cpu max, most hypervisors), always combined with timestamp-counter
 * jitter samples and the wall clock. Output: SHA-512 over the seed and a
 * counter, re-keyed after every request (a simple hash DRBG). Without
 * RDRAND the jitter source is the only entropy; MintOS has no other device
 * entropy yet. */
#include "crypto.h"
#include <mintos/runtime.h>

static uint8_t pool[64];
static uint64_t generated;
static int seeded;

static uint64_t rdtsc_(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static void cpuid_(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static int hw_random(uint64_t *v, int seed) {
    for (int tries = 0; tries < 32; ++tries) {
        unsigned char ok;
        if (seed) __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
        else __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (ok) return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

static void reseed(void) {
    MINT_HASH h;
    mint_hash_init(&h, MINT_SHA512);
    mint_hash_update(&h, pool, sizeof(pool));
    uint32_t r[4];
    cpuid_(0, 0, r);
    uint32_t max_leaf = r[0];
    int has_rdrand = 0, has_rdseed = 0;
    if (max_leaf >= 1u) { cpuid_(1, 0, r); has_rdrand = (r[2] >> 30) & 1u; }
    if (max_leaf >= 7u) { cpuid_(7, 0, r); has_rdseed = (r[1] >> 18) & 1u; }
    for (int i = 0; i < 16; ++i) {
        uint64_t v = 0;
        if ((has_rdseed && hw_random(&v, 1)) || (has_rdrand && hw_random(&v, 0))) mint_hash_update(&h, &v, sizeof(v));
    }
    /* Jitter: the low bits of TSC deltas around a yield vary with scheduling,
       interrupts and cache state. */
    for (int i = 0; i < 64; ++i) {
        uint64_t t0 = rdtsc_();
        if ((i & 15) == 0) (void)mint_sched_yield();
        volatile uint32_t spin = (uint32_t)(t0 & 63u);
        while (spin--) { }
        uint64_t t1 = rdtsc_();
        mint_hash_update(&h, &t0, sizeof(t0));
        mint_hash_update(&h, &t1, sizeof(t1));
    }
    mint_timespec64 now = {0, 0};
    (void)mint_clock_gettime(MINT_CLOCK_REALTIME, &now);
    mint_hash_update(&h, &now, sizeof(now));
    mint_hash_final(&h, pool);
    seeded = 1;
}

void mint_random_bytes(void *out, size_t len) {
    if (!seeded || generated > (1u << 20)) { reseed(); generated = 0; }
    uint8_t *o = (uint8_t *)out, block[64];
    uint64_t counter = 0, tsc = rdtsc_();
    while (len) {
        MINT_HASH h;
        mint_hash_init(&h, MINT_SHA512);
        mint_hash_update(&h, pool, sizeof(pool));
        mint_hash_update(&h, &counter, sizeof(counter));
        mint_hash_update(&h, &tsc, sizeof(tsc));
        mint_hash_final(&h, block);
        size_t n = len < 32u ? len : 32u;            /* only half of each block is output */
        for (size_t i = 0; i < n; ++i) o[i] = block[i];
        o += n; len -= n; ++counter; generated += n;
    }
    /* Forward secrecy for earlier outputs: replace the pool. */
    mint_hash(MINT_SHA512, block, sizeof(block), pool);
    volatile uint8_t *v = block; for (size_t i = 0; i < sizeof(block); ++i) v[i] = 0;
}
