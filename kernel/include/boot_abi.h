#ifndef MINTOS26_BOOT_ABI_H
#define MINTOS26_BOOT_ABI_H
#include <stdint.h>
#define MINT_BOOT_INFO_MAGIC 0x4d494e54424f4f54ULL /* "MINTBOOT" */
#define MINT_BOOT_INFO_ABI 1u
/* Stable hand-off contract for the future UEFI loader -> native kernel path.
 * Pointer fields are physical addresses until the kernel establishes paging. */
typedef struct {
    uint64_t base;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t pixel_format;
} MINT_FRAMEBUFFER_INFO;
typedef struct {
    uint64_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    MINT_FRAMEBUFFER_INFO framebuffer;
    uint64_t memory_map_physical;
    uint64_t memory_map_size;
    uint64_t memory_descriptor_size;
    uint32_t memory_descriptor_version;
    uint32_t reserved;
    uint64_t acpi_rsdp_physical;
    uint64_t system_table_physical; /* informational only; invalid after ExitBootServices */
} MINT_BOOT_INFO;
#endif
