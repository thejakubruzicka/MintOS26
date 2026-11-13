/* MintOS26 ACPI power management: table discovery, ACPI S5 soft-off and
 * system reset. Only fixed-hardware FADT registers and the \_S5_ package
 * object are used; there is no AML interpreter. Every table is validated
 * (signature, length bound, checksum) before any field is trusted. */
#include <stdint.h>
#include <stddef.h>
#include "../include/power.h"
#ifndef MINT_ACPI_HOST_TEST
#include "../include/paging.h"
#include "../include/serial.h"
#include "../include/log.h"
#endif

#define ACPI_MAX_TABLE_BYTES (16ull * 1024ull * 1024ull)
#define ACPI_MAX_TABLES 64u
#define SDT_HEADER_LEN 36u

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int checksum_ok(const uint8_t *p, uint64_t length) {
    uint8_t sum = 0;
    for (uint64_t i = 0; i < length; ++i) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}
static int sig_eq(const uint8_t *p, const char *sig, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (p[i] != (uint8_t)sig[i]) return 0;
    return 1;
}

/* Map and validate a System Description Table. Returns its full byte view. */
static const uint8_t *map_sdt(mint_acpi_map_fn map, uint64_t physical, const char *sig,
                              uint32_t *length_out) {
    if (!physical) return 0;
    const uint8_t *h = map(physical, SDT_HEADER_LEN);
    if (!h) return 0;
    uint32_t length = rd32(h + 4);
    if (length < SDT_HEADER_LEN || length > ACPI_MAX_TABLE_BYTES) return 0;
    if (sig && !sig_eq(h, sig, 4)) return 0;
    const uint8_t *t = map(physical, length);
    if (!t || !checksum_ok(t, length)) return 0;
    if (length_out) *length_out = length;
    return t;
}

/* AML data-object integer for a package element. Returns bytes consumed or 0. */
static uint32_t aml_small_int(const uint8_t *p, uint64_t avail, uint8_t *value) {
    if (!avail) return 0;
    switch (p[0]) {
    case 0x00: *value = 0; return 1;          /* ZeroOp */
    case 0x01: *value = 1; return 1;          /* OneOp */
    case 0xFF: *value = 0xFF; return 1;       /* OnesOp */
    case 0x0A: if (avail < 2) return 0; *value = p[1]; return 2;   /* BytePrefix */
    case 0x0B: if (avail < 3) return 0; *value = p[1]; return 3;   /* WordPrefix */
    case 0x0C: if (avail < 5) return 0; *value = p[1]; return 5;   /* DWordPrefix */
    default: return 0;
    }
}

/* Locate Name(\_S5_, Package(){SLP_TYPa, SLP_TYPb, ...}) in AML bytecode. */
int mint_acpi_find_s5(const uint8_t *aml, uint64_t length, uint8_t *typa, uint8_t *typb) {
    if (!aml || length < 8u || !typa || !typb) return 0;
    for (uint64_t i = 1; i + 4u < length; ++i) {
        if (aml[i] != '_' || aml[i + 1] != 'S' || aml[i + 2] != '5' || aml[i + 3] != '_') continue;
        /* NameOp directly before the NameSeg, optionally with a root prefix. */
        int named = aml[i - 1] == 0x08u || (i >= 2u && aml[i - 1] == '\\' && aml[i - 2] == 0x08u);
        if (!named) continue;
        uint64_t p = i + 4u;
        if (p >= length || aml[p] != 0x12u) continue;          /* PackageOp */
        ++p;
        if (p >= length) return 0;
        uint32_t follow = (uint32_t)(aml[p] >> 6);              /* PkgLength byte count */
        p += 1u + follow;
        if (p >= length) return 0;
        uint8_t elements = aml[p++];
        if (elements < 1u) continue;
        uint8_t a = 0, b = 0;
        uint32_t used = aml_small_int(aml + p, length - p, &a);
        if (!used) continue;
        p += used;
        if (elements >= 2u) {
            used = aml_small_int(aml + p, length - p, &b);
            if (!used) continue;
        } else {
            b = a;
        }
        *typa = (uint8_t)(a & 7u);
        *typb = (uint8_t)(b & 7u);
        return 1;
    }
    return 0;
}

int mint_acpi_parse(uint64_t rsdp_physical, mint_acpi_map_fn map, MINT_POWER_INFO *out) {
    if (!map || !out) return 0;
    for (size_t i = 0; i < sizeof(*out); ++i) ((uint8_t *)out)[i] = 0;
    out->version = MINT_POWER_INFO_VERSION;
    if (!rsdp_physical) return 0;
    const uint8_t *rsdp = map(rsdp_physical, 20u);
    if (!rsdp || !sig_eq(rsdp, "RSD PTR ", 8) || !checksum_ok(rsdp, 20u)) return 0;
    uint32_t revision = rsdp[15];
    uint64_t root = rd32(rsdp + 16);
    int xsdt = 0;
    if (revision >= 2u) {
        const uint8_t *r2 = map(rsdp_physical, 36u);
        if (r2 && rd32(r2 + 20) >= 36u && checksum_ok(r2, 36u) && rd64(r2 + 24)) {
            root = rd64(r2 + 24);
            xsdt = 1;
        }
    }
    out->flags |= MINT_POWER_HAVE_RSDP;
    out->acpi_revision = revision;
    for (uint32_t i = 0; i < 6u; ++i) out->oem_id[i] = (char)rsdp[9 + i];
    out->oem_id[6] = 0;

    uint32_t root_len = 0;
    const uint8_t *rt = map_sdt(map, root, xsdt ? "XSDT" : "RSDT", &root_len);
    if (!rt) return 1;
    uint32_t entry_size = xsdt ? 8u : 4u;
    uint32_t count = (root_len - SDT_HEADER_LEN) / entry_size;
    if (count > ACPI_MAX_TABLES) count = ACPI_MAX_TABLES;
    out->table_count = count;

    const uint8_t *fadt = 0; uint32_t fadt_len = 0;
    for (uint32_t i = 0; i < count && !fadt; ++i) {
        const uint8_t *e = rt + SDT_HEADER_LEN + (uint64_t)i * entry_size;
        uint64_t phys = xsdt ? rd64(e) : rd32(e);
        fadt = map_sdt(map, phys, "FACP", &fadt_len);
    }
    if (!fadt || fadt_len < 116u) return 1;
    out->flags |= MINT_POWER_HAVE_FADT;
    out->smi_cmd = rd32(fadt + 48);
    out->acpi_enable = fadt[52];
    out->pm1a_evt = rd32(fadt + 56);
    out->pm1a_cnt = rd32(fadt + 64);
    out->pm1b_cnt = rd32(fadt + 68);
    out->iapc_boot_arch = rd16(fadt + 109);
    out->fadt_flags = rd32(fadt + 112);
    if (out->iapc_boot_arch & 0x0001u) out->flags |= MINT_POWER_HAVE_LEGACY_DEV;
    if (out->iapc_boot_arch & 0x0002u) out->flags |= MINT_POWER_HAVE_8042;
    if (out->fadt_flags & (1u << 20)) out->flags |= MINT_POWER_HW_REDUCED;
    if (fadt_len >= 129u && (out->fadt_flags & (1u << 10))) {
        const uint8_t *gas = fadt + 116;
        uint64_t address = rd64(gas + 4);
        if (address && gas[0] <= MINT_ACPI_SPACE_PCI) {
            out->reset_space = gas[0];
            out->reset_address = address;
            out->reset_value = fadt[128];
            out->flags |= MINT_POWER_HAVE_RESET_REG;
        }
    }
    /* Prefer 64-bit extended blocks when present and in SystemIO space. */
    if (fadt_len >= 184u) {
        const uint8_t *x = fadt + 172;
        if (x[0] == MINT_ACPI_SPACE_IO && rd64(x + 4) && rd64(x + 4) <= 0xFFFFu)
            out->pm1a_cnt = (uint32_t)rd64(x + 4);
    }
    if (fadt_len >= 196u) {
        const uint8_t *x = fadt + 184;
        if (x[0] == MINT_ACPI_SPACE_IO && rd64(x + 4) && rd64(x + 4) <= 0xFFFFu)
            out->pm1b_cnt = (uint32_t)rd64(x + 4);
    }
    if (fadt_len >= 160u) {
        const uint8_t *x = fadt + 148;
        if (x[0] == MINT_ACPI_SPACE_IO && rd64(x + 4) && rd64(x + 4) <= 0xFFFFu)
            out->pm1a_evt = (uint32_t)rd64(x + 4);
    }
    if (out->pm1a_cnt > 0xFFFFu) out->pm1a_cnt = 0;
    if (out->pm1b_cnt > 0xFFFFu) out->pm1b_cnt = 0;
    if (out->pm1a_evt > 0xFFFFu) out->pm1a_evt = 0;
    if (out->smi_cmd > 0xFFFFu) out->smi_cmd = 0;

    uint64_t dsdt_phys = rd32(fadt + 40);
    if (fadt_len >= 148u && rd64(fadt + 140)) dsdt_phys = rd64(fadt + 140);
    uint32_t dsdt_len = 0;
    const uint8_t *dsdt = map_sdt(map, dsdt_phys, "DSDT", &dsdt_len);
    uint8_t a = 0, b = 0;
    if (dsdt && mint_acpi_find_s5(dsdt + SDT_HEADER_LEN, dsdt_len - SDT_HEADER_LEN, &a, &b)) {
        out->flags |= MINT_POWER_HAVE_S5;
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *e = rt + SDT_HEADER_LEN + (uint64_t)i * entry_size;
            uint64_t phys = xsdt ? rd64(e) : rd32(e);
            uint32_t len = 0;
            const uint8_t *ssdt = map_sdt(map, phys, "SSDT", &len);
            if (ssdt && mint_acpi_find_s5(ssdt + SDT_HEADER_LEN, len - SDT_HEADER_LEN, &a, &b)) {
                out->flags |= MINT_POWER_HAVE_S5 | MINT_POWER_S5_FROM_SSDT;
                break;
            }
        }
    }
    out->slp_typa = a;
    out->slp_typb = b;
    return 1;
}

#ifndef MINT_ACPI_HOST_TEST
static MINT_POWER_INFO power_info;
static int power_ready;

static const uint8_t *identity_map(uint64_t physical, uint64_t length) {
    /* ACPI tables live in RAM-backed firmware regions inside the identity
       window; the vmalloc arena at its top is never firmware memory. */
    if (!length || physical > MINT_VMALLOC_BASE || length > MINT_VMALLOC_BASE - physical) return 0;
    return (const uint8_t *)(uintptr_t)physical;
}

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port) : "memory"); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port) : "memory"); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_delay(void) { outb(0x80u, 0); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port) : "memory"); }

static int running_under_hypervisor(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0));
    return (c >> 31) & 1u;
}

/* Fallback scan of the legacy BIOS area for firmware that did not publish an
   ACPI 2.0 configuration table entry. UEFI identity-maps this range. */
static uint64_t scan_bios_rsdp(void) {
    for (uint64_t p = 0xE0000u; p < 0x100000u; p += 16u) {
        const uint8_t *r = (const uint8_t *)(uintptr_t)p;
        if (sig_eq(r, "RSD PTR ", 8) && checksum_ok(r, 20u)) return p;
    }
    return 0;
}

int mint_power_init(uint64_t rsdp_physical) {
    uint64_t rsdp = rsdp_physical ? rsdp_physical : scan_bios_rsdp();
    int ok = mint_acpi_parse(rsdp, identity_map, &power_info);
    if (running_under_hypervisor()) power_info.flags |= MINT_POWER_HYPERVISOR;
    power_ready = 1;
    mint_serial_write("[acpi] RSDP=");
    mint_serial_hex64(rsdp);
    mint_serial_write(" rev=");
    mint_serial_dec64(power_info.acpi_revision);
    mint_serial_write(" oem=");
    mint_serial_write(power_info.oem_id[0] ? power_info.oem_id : "?");
    mint_serial_write(" tables=");
    mint_serial_dec64(power_info.table_count);
    mint_serial_write((power_info.flags & MINT_POWER_HAVE_FADT) ? " FADT" : " no-FADT");
    mint_serial_write(" PM1a_CNT=");
    mint_serial_hex64(power_info.pm1a_cnt);
    if (power_info.flags & MINT_POWER_HAVE_S5) {
        mint_serial_write(" S5=");
        mint_serial_dec64(power_info.slp_typa);
        mint_serial_write("/");
        mint_serial_dec64(power_info.slp_typb);
    } else {
        mint_serial_write(" S5=missing");
    }
    mint_serial_write((power_info.flags & MINT_POWER_HAVE_RESET_REG) ? " reset-reg" : " no-reset-reg");
    mint_serial_write((power_info.flags & MINT_POWER_HAVE_8042) ? " 8042" : " no-8042-flag");
    mint_serial_write("\n");
    mint_log_emit(ok ? MINT_LOG_INFO : MINT_LOG_WARN, MINT_LOG_COMPONENT_KERNEL,
                  ok ? "ACPI tables parsed for shutdown/restart"
                     : "ACPI RSDP unavailable; restart uses legacy reset paths only");
    return ok;
}

int mint_power_get_info(MINT_POWER_INFO *out) {
    if (!out || !power_ready) return 0;
    *out = power_info;
    return 1;
}

static void enable_acpi_mode(void) {
    if (!power_info.pm1a_cnt) return;
    if (inw((uint16_t)power_info.pm1a_cnt) & 1u) return;           /* SCI_EN already set */
    if (!power_info.smi_cmd || !power_info.acpi_enable) return;
    outb((uint16_t)power_info.smi_cmd, power_info.acpi_enable);
    for (uint32_t i = 0; i < 3000000u; ++i) {                        /* ~3 s of port-0x80 delays */
        if (inw((uint16_t)power_info.pm1a_cnt) & 1u) return;
        io_delay();
    }
}

static void write_slp(uint32_t port, uint8_t slp_typ) {
    if (!port) return;
    uint16_t v = inw((uint16_t)port);
    v = (uint16_t)((v & ~(uint16_t)(0x1C00u | 0x2000u)) | ((uint16_t)(slp_typ & 7u) << 10));
    outw((uint16_t)port, v);
    outw((uint16_t)port, (uint16_t)(v | 0x2000u));                   /* SLP_EN */
}

/* Safe shutdown: whatever registered the flush hook (MintFS: commit + disk
 * cache flush) runs once, with interrupts still enabled, before any power
 * register is touched. A failed flush is logged but cannot block power-off. */
static int (*power_flush_hook)(void);
void mint_power_set_flush_hook(int (*hook)(void)) { power_flush_hook = hook; }
static void power_flush(void) {
    if (!power_flush_hook) return;
    mint_serial_write("[power] saving MintOS HD before power-off\n");
    if (power_flush_hook() != 0) mint_serial_write("[power] MintOS HD flush reported an error\n");
}

int mint_power_shutdown(void) {
    power_flush();
    __asm__ volatile("cli" ::: "memory");
    mint_serial_write("[power] shutdown requested\n");
    if ((power_info.flags & MINT_POWER_HAVE_S5) && power_info.pm1a_cnt &&
        !(power_info.flags & MINT_POWER_HW_REDUCED)) {
        enable_acpi_mode();
        if (power_info.pm1a_evt) outw((uint16_t)power_info.pm1a_evt, 0x8000u); /* clear WAK_STS */
        mint_serial_write("[power] entering ACPI S5 (soft-off)\n");
        write_slp(power_info.pm1b_cnt, power_info.slp_typb);
        write_slp(power_info.pm1a_cnt, power_info.slp_typa);
        for (uint32_t i = 0; i < 2000000u; ++i) io_delay();
    }
    if (power_info.flags & MINT_POWER_HYPERVISOR) {
        /* Known virtual-machine PM ports, tried only under a hypervisor. */
        mint_serial_write("[power] ACPI S5 did not complete; trying VM power-off ports\n");
        outw(0x604u, 0x2000u);   /* QEMU q35 ICH9 */
        outw(0xB004u, 0x2000u);  /* QEMU/Bochs PIIX */
        outw(0x4004u, 0x3400u);  /* VirtualBox */
        for (uint32_t i = 0; i < 1000000u; ++i) io_delay();
    }
    mint_serial_write("[power] shutdown did not complete; firmware may require AML _PTS\n");
    __asm__ volatile("sti" ::: "memory");
    return -38; /* -MINT_ENOSYS */
}

static void reset_via_acpi(void) {
    if (!(power_info.flags & MINT_POWER_HAVE_RESET_REG)) return;
    uint64_t a = power_info.reset_address;
    uint8_t v = power_info.reset_value;
    if (power_info.reset_space == MINT_ACPI_SPACE_IO && a <= 0xFFFFu) {
        outb((uint16_t)a, v);
    } else if (power_info.reset_space == MINT_ACPI_SPACE_MEMORY && a && a < MINT_VMALLOC_BASE) {
        *(volatile uint8_t *)(uintptr_t)a = v;
    } else if (power_info.reset_space == MINT_ACPI_SPACE_PCI) {
        /* Segment 0, bus 0: device in bits 32..47, function 16..31, register 0..15. */
        uint32_t dev = (uint32_t)(a >> 32) & 0x1Fu, fn = (uint32_t)(a >> 16) & 7u, reg = (uint32_t)a & 0xFFu;
        outl(0xCF8u, 0x80000000u | (dev << 11) | (fn << 8) | (reg & 0xFCu));
        outb((uint16_t)(0xCFCu + (reg & 3u)), v);
    }
    for (uint32_t i = 0; i < 500000u; ++i) io_delay();
}

int mint_power_restart(void) {
    power_flush();
    __asm__ volatile("cli" ::: "memory");
    mint_serial_write("[power] restart requested\n");
    reset_via_acpi();
    /* Intel PCH reset control register: soft then hard reset. */
    outb(0xCF9u, 0x02u); io_delay();
    outb(0xCF9u, 0x06u);
    for (uint32_t i = 0; i < 500000u; ++i) io_delay();
    /* 8042 pulse-reset, bounded wait for the input buffer to drain. */
    for (uint32_t i = 0; i < 100000u && (inb(0x64u) & 0x02u); ++i) io_delay();
    outb(0x64u, 0xFEu);
    for (uint32_t i = 0; i < 500000u; ++i) io_delay();
    /* Last resort: triple fault through an empty IDT. */
    mint_serial_write("[power] legacy resets ignored; forcing triple fault\n");
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } empty = {0, 0};
    __asm__ volatile("lidt %0; int3" :: "m"(empty) : "memory");
    for (;;) __asm__ volatile("hlt");
}
#endif
