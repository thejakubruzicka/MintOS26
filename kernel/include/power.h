#ifndef MINTOS26_POWER_H
#define MINTOS26_POWER_H
#include <stdint.h>

/* ACPI power-management discovery shared by the kernel and MintDE. Only the
 * fixed-hardware FADT registers and the \_S5_ package are used; MintOS has no
 * AML interpreter, so _PTS/_GTS methods are not executed. */
#define MINT_POWER_INFO_VERSION 1u

#define MINT_POWER_HAVE_RSDP        0x0001u
#define MINT_POWER_HAVE_FADT        0x0002u
#define MINT_POWER_HAVE_S5          0x0004u
#define MINT_POWER_HAVE_RESET_REG   0x0008u
#define MINT_POWER_HW_REDUCED       0x0010u
#define MINT_POWER_HAVE_8042        0x0020u /* FADT IAPC_BOOT_ARCH: 8042 present */
#define MINT_POWER_HYPERVISOR       0x0040u
#define MINT_POWER_S5_FROM_SSDT     0x0080u
#define MINT_POWER_HAVE_LEGACY_DEV  0x0100u /* FADT IAPC_BOOT_ARCH: legacy devices */

/* GAS address-space identifiers used by the reset register. */
#define MINT_ACPI_SPACE_MEMORY 0u
#define MINT_ACPI_SPACE_IO     1u
#define MINT_ACPI_SPACE_PCI    2u

typedef struct mint_power_info {
    uint32_t version;
    uint32_t flags;
    uint32_t acpi_revision;   /* RSDP revision: 0 = ACPI 1.0, 2+ = XSDT */
    uint32_t table_count;     /* entries in RSDT/XSDT */
    uint32_t pm1a_cnt;        /* SystemIO port, 0 if absent */
    uint32_t pm1b_cnt;
    uint32_t pm1a_evt;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t slp_typa;         /* \_S5_ package element 0 */
    uint8_t slp_typb;         /* \_S5_ package element 1 */
    uint8_t reset_space;      /* GAS space id of RESET_REG */
    uint8_t reset_value;
    uint8_t reserved8[3];
    uint64_t reset_address;
    uint32_t fadt_flags;
    uint16_t iapc_boot_arch;
    uint16_t reserved16;
    char oem_id[8];           /* NUL-terminated RSDP OEM ID, e.g. "DELL  " */
} MINT_POWER_INFO;

/* MINT_SYS_POWER_CONTROL actions and the required confirmation token, so a
 * stray or fuzzed syscall cannot power the machine off. */
#define MINT_POWER_ACTION_SHUTDOWN 1u
#define MINT_POWER_ACTION_RESTART  2u
#define MINT_POWER_CONFIRM_TOKEN   0x504F5752u /* "POWR" */

#ifndef MINT_POWER_USERSPACE
/* Parse ACPI tables starting at the RSDP physical address (0 = scan the BIOS
 * area). Every table is length- and checksum-validated before use. */
int mint_power_init(uint64_t rsdp_physical);
int mint_power_get_info(MINT_POWER_INFO *out);
/* Return only on failure (negative MintOS errno). */
int mint_power_shutdown(void);
int mint_power_restart(void);
/* Runs before shutdown/restart touch any power register (MintFS final commit). */
void mint_power_set_flush_hook(int (*hook)(void));

/* Pure parser entry point for host tests. map(physical, length) returns a
 * readable pointer for [physical, physical+length) or 0. */
typedef const uint8_t *(*mint_acpi_map_fn)(uint64_t physical, uint64_t length);
int mint_acpi_parse(uint64_t rsdp_physical, mint_acpi_map_fn map, MINT_POWER_INFO *out);
int mint_acpi_find_s5(const uint8_t *aml, uint64_t length, uint8_t *typa, uint8_t *typb);
#endif

#endif
