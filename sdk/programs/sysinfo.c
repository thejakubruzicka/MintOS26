/* sysinfo: what this MintOS system is running (MintOS SDK tool).
 * Uses POSIX/Linux-compat calls through musl (uname, sysinfo, clock_gettime)
 * and MintOS's own int 0x80 services for platform and process details. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <uapi/mintos/process.h>

static long mint_call(unsigned long n, unsigned long a0, unsigned long a1) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return ret;
}

static void cpu_brand(char out[49]) {
    unsigned int r[12];
    for (unsigned int i = 0; i < 3; ++i)
        __asm__ volatile("cpuid" : "=a"(r[4 * i]), "=b"(r[4 * i + 1]), "=c"(r[4 * i + 2]), "=d"(r[4 * i + 3])
                         : "a"(0x80000002u + i), "c"(0));
    memcpy(out, r, 48);
    out[48] = '\0';
    char *p = out;
    while (*p == ' ') ++p;
    memmove(out, p, strlen(p) + 1);
}

int main(void) {
    struct utsname u;
    struct sysinfo si;
    char brand[49];
    uname(&u);
    cpu_brand(brand);
    printf("System:     %s %s (%s), program pid %d\n", u.sysname, u.release, u.machine, (int)getpid());
    printf("CPU:        %s\n", brand);
    if (sysinfo(&si) == 0) {
        printf("Uptime:     %ld s\n", si.uptime);
        printf("Memory:     %lu MiB total, %lu MiB free\n", si.totalram * si.mem_unit >> 20, si.freeram * si.mem_unit >> 20);
    }
    MINT_PLATFORM_INFO pi;
    memset(&pi, 0, sizeof pi);
    if (mint_call(MINT_SYS_PLATFORM_INFO, (unsigned long)&pi, 0) == 0) {
        printf("FPU:        %s%s%s, %u-byte save area, %llu saves / %llu restores\n",
               (pi.fpu_flags & 1u) ? "SSE" : "none", (pi.fpu_flags & 4u) ? " + XSAVE" : "",
               (pi.fpu_flags & 8u) ? " + AVX" : "", pi.fpu_area_bytes,
               (unsigned long long)pi.fpu_saves, (unsigned long long)pi.fpu_restores);
        printf("Scheduler:  %llu tasks, %llu processes, %llu timer preemptions\n",
               (unsigned long long)pi.tasks_live, (unsigned long long)pi.processes_live,
               (unsigned long long)pi.preemptions);
        printf("User pages: %llu KiB in use of %llu KiB pooled (peak %llu KiB)\n",
               (unsigned long long)pi.pool_used_pages * 4, (unsigned long long)pi.pool_pages * 4,
               (unsigned long long)pi.pool_peak_pages * 4);
        printf("Linux ABI:  %s, %llu calls so far, %llu unsupported\n",
               pi.linux_compat ? "compat subset on" : "off", (unsigned long long)pi.linux_syscalls,
               (unsigned long long)pi.linux_unsupported);
        printf("This program: %llu demand-zero page faults\n", (unsigned long long)pi.demand_faults);
    }
    static MINT_PROC_INFO list[24];
    long n = mint_call(MINT_SYS_PROC_LIST, (unsigned long)list, 24);
    if (n > 0) {
        printf("Processes:\n");
        for (long i = 0; i < n; ++i)
            printf("  %5llu  %-16s %6llu KiB\n", (unsigned long long)list[i].pid, list[i].name,
                   (unsigned long long)list[i].owned_pages * 4);
    }
    return 0;
}
