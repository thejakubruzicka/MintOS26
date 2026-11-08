/* Host test for the production ACPI parser (kernel/power/acpi.c) using
 * synthetic firmware tables placed in a fake physical address arena. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/include/power.h"

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); return 1; } \
                         printf("PASS: %s\n", m); } while (0)

static uint8_t arena[0x10000];
static const uint8_t *arena_map(uint64_t phys, uint64_t len) {
    if (phys >= sizeof(arena) || len > sizeof(arena) - phys) return 0;
    return arena + phys;
}
static void w16(uint64_t at, uint16_t v) { arena[at] = (uint8_t)v; arena[at + 1] = (uint8_t)(v >> 8); }
static void w32(uint64_t at, uint32_t v) { for (int i = 0; i < 4; ++i) arena[at + i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint64_t at, uint64_t v) { for (int i = 0; i < 8; ++i) arena[at + i] = (uint8_t)(v >> (8 * i)); }
static void fix_sum(uint64_t at, uint32_t len, uint32_t sum_off) {
    arena[at + sum_off] = 0; uint8_t s = 0;
    for (uint32_t i = 0; i < len; ++i) s = (uint8_t)(s + arena[at + i]);
    arena[at + sum_off] = (uint8_t)(0u - s);
}
static void sdt(uint64_t at, const char *sig, uint32_t len) {
    memcpy(arena + at, sig, 4); w32(at + 4, len); arena[at + 8] = 2;
    memcpy(arena + at + 10, "DELL  ", 6);
}
static void rsdp(uint64_t at, uint8_t rev, uint32_t rsdt, uint64_t xsdt) {
    memcpy(arena + at, "RSD PTR ", 8); memcpy(arena + at + 9, "DELL  ", 6);
    arena[at + 15] = rev; w32(at + 16, rsdt); w32(at + 20, 36); w64(at + 24, xsdt);
    fix_sum(at, 20, 8); fix_sum(at, 36, 32);
}
/* Dell/Intel-PCH style: Name(\_S5_, Package(0x04){0x07, 0x07, Zero, Zero}) */
static const uint8_t dell_s5[] = { 0x08, '\\', '_', 'S', '5', '_', 0x12, 0x0A, 0x04,
                                   0x0A, 0x07, 0x0A, 0x07, 0x00, 0x00 };
/* Decoy: a method called _S5_ (MethodOp, not NameOp) must be ignored. */
static const uint8_t decoy[] = { 0x14, 0x08, '_', 'S', '5', '_', 0x00, 0xA4, 0x00 };

static void build(int with_dsdt_s5, int bad_fadt_sum) {
    memset(arena, 0, sizeof(arena));
    rsdp(0x100, 2, 0x900, 0x200);
    /* XSDT: SSDT then FACP */
    sdt(0x200, "XSDT", 36 + 16); w64(0x200 + 36, 0x600); w64(0x200 + 44, 0x300); fix_sum(0x200, 52, 9);
    /* RSDT (ACPI 1.0 path) with the same two tables */
    sdt(0x900, "RSDT", 36 + 8); w32(0x900 + 36, 0x600); w32(0x900 + 40, 0x300); fix_sum(0x900, 44, 9);
    /* FADT, ACPI 6 length */
    sdt(0x300, "FACP", 276);
    w32(0x300 + 40, 0x500);           /* DSDT */
    w32(0x300 + 48, 0xB2);            /* SMI_CMD */
    arena[0x300 + 52] = 0xA0;         /* ACPI_ENABLE */
    w32(0x300 + 56, 0x1800);          /* PM1a_EVT */
    w32(0x300 + 64, 0x1804);          /* PM1a_CNT (32-bit legacy field) */
    w16(0x300 + 109, 0x0003);         /* legacy devices + 8042 */
    w32(0x300 + 112, (1u << 10));     /* RESET_REG_SUP */
    arena[0x300 + 116] = 1; arena[0x300 + 117] = 8; w64(0x300 + 120, 0xCF9); /* RESET_REG: IO 0xCF9 */
    arena[0x300 + 128] = 0x06;        /* RESET_VALUE */
    arena[0x300 + 172] = 1; arena[0x300 + 173] = 16; w64(0x300 + 176, 0x1804); /* X_PM1a_CNT */
    fix_sum(0x300, 276, 9);
    if (bad_fadt_sum) arena[0x300 + 200] ^= 0x5A;
    /* DSDT: decoy first, then (optionally) the real \_S5_ */
    uint32_t len = 36;
    memcpy(arena + 0x500 + len, decoy, sizeof(decoy)); len += sizeof(decoy);
    if (with_dsdt_s5) { memcpy(arena + 0x500 + len, dell_s5, sizeof(dell_s5)); len += sizeof(dell_s5); }
    sdt(0x500, "DSDT", len); fix_sum(0x500, len, 9);
    /* SSDT: Name(_S5_, Package(){Zero, Zero}) using a 2-byte PkgLength */
    static const uint8_t ssdt_s5[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x40, 0x00, 0x02, 0x00, 0x00 };
    sdt(0x600, "SSDT", 36 + sizeof(ssdt_s5)); memcpy(arena + 0x600 + 36, ssdt_s5, sizeof(ssdt_s5));
    fix_sum(0x600, 36 + sizeof(ssdt_s5), 9);
}

int main(void) {
    MINT_POWER_INFO info;
    build(1, 0);
    CHECK(mint_acpi_parse(0x100, arena_map, &info), "ACPI 2.0 RSDP/XSDT parsed");
    CHECK((info.flags & (MINT_POWER_HAVE_RSDP | MINT_POWER_HAVE_FADT | MINT_POWER_HAVE_S5)) ==
          (MINT_POWER_HAVE_RSDP | MINT_POWER_HAVE_FADT | MINT_POWER_HAVE_S5), "RSDP, FADT and \\_S5_ found");
    CHECK(info.slp_typa == 7 && info.slp_typb == 7 && !(info.flags & MINT_POWER_S5_FROM_SSDT),
          "Dell/Intel PCH style \\_S5_ SLP_TYP=7 read from DSDT, MethodOp decoy ignored");
    CHECK(info.pm1a_cnt == 0x1804 && info.pm1a_evt == 0x1800 && info.smi_cmd == 0xB2 && info.acpi_enable == 0xA0,
          "PM1a control/event, SMI_CMD and ACPI_ENABLE decoded");
    CHECK((info.flags & MINT_POWER_HAVE_RESET_REG) && info.reset_space == MINT_ACPI_SPACE_IO &&
          info.reset_address == 0xCF9 && info.reset_value == 0x06, "FADT RESET_REG (IO 0xCF9 = 0x06) decoded");
    CHECK((info.flags & MINT_POWER_HAVE_8042) && (info.flags & MINT_POWER_HAVE_LEGACY_DEV),
          "IAPC_BOOT_ARCH 8042/legacy flags decoded");
    CHECK(strcmp(info.oem_id, "DELL  ") == 0 && info.acpi_revision == 2 && info.table_count == 2,
          "OEM ID, revision and XSDT entry count reported");

    build(0, 0);
    CHECK(mint_acpi_parse(0x100, arena_map, &info) && (info.flags & MINT_POWER_S5_FROM_SSDT) &&
          info.slp_typa == 0 && info.slp_typb == 0, "\\_S5_ falls back to SSDT with multi-byte PkgLength");

    build(1, 0);
    rsdp(0x100, 0, 0x900, 0);
    CHECK(mint_acpi_parse(0x100, arena_map, &info) && info.acpi_revision == 0 &&
          (info.flags & MINT_POWER_HAVE_S5) && info.slp_typa == 7, "ACPI 1.0 RSDT path parsed");

    build(1, 1);
    CHECK(mint_acpi_parse(0x100, arena_map, &info) && !(info.flags & MINT_POWER_HAVE_FADT) &&
          !(info.flags & MINT_POWER_HAVE_S5), "FADT with a bad checksum is rejected");

    build(1, 0);
    arena[0x100 + 3] ^= 1;
    CHECK(!mint_acpi_parse(0x100, arena_map, &info) && !(info.flags & MINT_POWER_HAVE_RSDP),
          "RSDP with a bad signature is rejected");
    CHECK(!mint_acpi_parse(0xFFF8, arena_map, &info), "RSDP outside mapped memory is rejected");

    build(1, 0);
    w32(0x500 + 4, 0x7FFFFFFF); /* absurd DSDT length */
    CHECK(mint_acpi_parse(0x100, arena_map, &info) && (info.flags & MINT_POWER_HAVE_FADT) &&
          (info.flags & MINT_POWER_S5_FROM_SSDT), "oversized DSDT length is bounded; SSDT fallback used");

    uint8_t t1 = 9, t2 = 9;
    static const uint8_t truncated[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x06 };
    CHECK(!mint_acpi_find_s5(truncated, sizeof(truncated), &t1, &t2), "truncated \\_S5_ package rejected");
    return 0;
}
