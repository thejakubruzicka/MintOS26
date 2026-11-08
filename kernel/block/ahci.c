#include <stdint.h>
#include <stddef.h>
#include "../include/block.h"
#include "../include/pci.h"
#include "../include/paging.h"
#include "../include/storage_diag.h"
#include "../include/storage_diag_internal.h"
#include "../include/log.h"
#include "../include/serial.h"

#define AHCI_PCI_CLASS 0x01u
#define AHCI_PCI_SUBCLASS 0x06u
#define AHCI_PCI_PROGIF 0x01u
#define AHCI_BAR5_OFFSET 0x24u
#define AHCI_GHC 0x04u
#define AHCI_CAP2 0x24u
#define AHCI_BOHC 0x28u
#define AHCI_PI 0x0Cu
#define AHCI_VS 0x10u
/* Intel RST "RAID On" (the Dell OptiPlex factory default) presents the PCH
 * SATA controller as class 01:04:00, but its ABAR is the same AHCI register
 * set (Linux's ahci driver binds these IDs too). MintOS accepts it only after
 * the AHCI version and ports-implemented registers validate; RAID volumes
 * themselves are not assembled, each member disk is read individually. */
#define PCI_VENDOR_INTEL 0x8086u
#define PCI_SUBCLASS_RAID 0x04u
#define AHCI_PORT_BASE 0x100u
#define AHCI_PORT_STRIDE 0x80u
#define AHCI_PX_CLB 0x00u
#define AHCI_PX_CLBU 0x04u
#define AHCI_PX_FB 0x08u
#define AHCI_PX_FBU 0x0Cu
#define AHCI_PX_IS 0x10u
#define AHCI_PX_IE 0x14u
#define AHCI_PX_CMD 0x18u
#define AHCI_PX_TFD 0x20u
#define AHCI_PX_SIG 0x24u
#define AHCI_PX_SSTS 0x28u
#define AHCI_PX_SERR 0x30u
#define AHCI_PX_CI 0x38u
#define AHCI_CMD_ST (1u << 0)
#define AHCI_CMD_FRE (1u << 4)
#define AHCI_CMD_FR (1u << 14)
#define AHCI_CMD_CR (1u << 15)
#define AHCI_TFD_BSY 0x80u
#define AHCI_TFD_DRQ 0x08u
#define AHCI_TFD_ERR 0x01u
#define AHCI_PXIS_TFES (1u << 30)
#define AHCI_SIG_ATA 0x00000101u
#define AHCI_CMD_IDENTIFY 0xECu
#define AHCI_CMD_READ_DMA 0xC8u
#define AHCI_CMD_WRITE_DMA 0xCAu
#define AHCI_CMD_READ_DMA_EXT 0x25u
#define AHCI_CMD_WRITE_DMA_EXT 0x35u
#define ATA_CMD_FLUSH_CACHE 0xE7u
#define ATA_CMD_FLUSH_CACHE_EXT 0xEAu
#define AHCI_WAIT_LIMIT 500000u
#define AHCI_COMMAND_SLOTS 8u
#define AHCI_PORT_MAX 32u
#define ATA_LBA28_LIMIT (1ULL << 28)
#define ATA_LBA48_LIMIT (1ULL << 48)

/* All DMA structures are static, aligned, bounded and identity-mapped. The
   registered block devices remain read-only to generic callers. A private
   sector-write callback exists solely for the blank-disk formatter, which
   checks identity, size, blank LBA 0 and explicit user confirmation first. */
typedef struct __attribute__((packed)) ahci_command_header {
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} AHCI_COMMAND_HEADER;
typedef struct __attribute__((packed)) ahci_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;
} AHCI_PRDT_ENTRY;
typedef struct __attribute__((packed, aligned(128))) ahci_command_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    AHCI_PRDT_ENTRY prdt[1];
} AHCI_COMMAND_TABLE;
typedef struct ahci_port_slot {
    volatile uint8_t *registers;
    uint32_t controller_index;
    uint32_t hardware_port;
    uint64_t sectors;
    uint32_t active;
    uint32_t lba48_supported;
    uint32_t flush_cache_supported;
    uint32_t flush_cache_ext_supported;
} AHCI_PORT_SLOT;

static AHCI_COMMAND_HEADER command_lists[AHCI_COMMAND_SLOTS][32] __attribute__((aligned(1024)));
static uint8_t received_fis[AHCI_COMMAND_SLOTS][256] __attribute__((aligned(256)));
static AHCI_COMMAND_TABLE command_tables[AHCI_COMMAND_SLOTS] __attribute__((aligned(128)));
static uint8_t sector_buffers[AHCI_COMMAND_SLOTS][512] __attribute__((aligned(512)));
static AHCI_PORT_SLOT ports[AHCI_COMMAND_SLOTS];
static volatile uint8_t *controller_hbas[MINT_STORAGE_MAX_CONTROLLERS];
static uint32_t dma_slots_used;

static uint32_t mmio_read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(void *)(base + offset);
}
static void mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(void *)(base + offset) = value;
}
static void bytes_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; ++i) b[i] = 0;
}
static int wait_mask(volatile uint8_t *regs, uint32_t off, uint32_t mask, uint32_t value) {
    for (uint32_t i = 0; i < AHCI_WAIT_LIMIT; ++i)
        if ((mmio_read32(regs, off) & mask) == value) return 1;
    return 0;
}
static int stop_engine(volatile uint8_t *regs) {
    uint32_t cmd = mmio_read32(regs, AHCI_PX_CMD);
    cmd &= ~AHCI_CMD_ST;
    mmio_write32(regs, AHCI_PX_CMD, cmd);
    if (!wait_mask(regs, AHCI_PX_CMD, AHCI_CMD_CR, 0u)) return 0;
    cmd = mmio_read32(regs, AHCI_PX_CMD);
    cmd &= ~AHCI_CMD_FRE;
    mmio_write32(regs, AHCI_PX_CMD, cmd);
    return wait_mask(regs, AHCI_PX_CMD, AHCI_CMD_FR, 0u);
}
static int start_port(uint32_t slot) {
    AHCI_PORT_SLOT *p = &ports[slot];
    volatile uint8_t *regs = p->registers;
    if (!stop_engine(regs)) return 0;
    bytes_zero(command_lists[slot], sizeof(command_lists[slot]));
    bytes_zero(received_fis[slot], sizeof(received_fis[slot]));
    bytes_zero(&command_tables[slot], sizeof(command_tables[slot]));
    mmio_write32(regs, AHCI_PX_CLB, (uint32_t)(uintptr_t)&command_lists[slot][0]);
    mmio_write32(regs, AHCI_PX_CLBU, (uint32_t)((uint64_t)(uintptr_t)&command_lists[slot][0] >> 32));
    mmio_write32(regs, AHCI_PX_FB, (uint32_t)(uintptr_t)&received_fis[slot][0]);
    mmio_write32(regs, AHCI_PX_FBU, (uint32_t)((uint64_t)(uintptr_t)&received_fis[slot][0] >> 32));
    mmio_write32(regs, AHCI_PX_IE, 0u); /* bounded polling; no interrupt dependency */
    mmio_write32(regs, AHCI_PX_IS, 0xFFFFFFFFu);
    mmio_write32(regs, AHCI_PX_SERR, 0xFFFFFFFFu);
    uint32_t cmd = mmio_read32(regs, AHCI_PX_CMD);
    cmd |= AHCI_CMD_FRE;
    mmio_write32(regs, AHCI_PX_CMD, cmd);
    if (!wait_mask(regs, AHCI_PX_CMD, AHCI_CMD_FR, AHCI_CMD_FR)) return 0;
    cmd |= AHCI_CMD_ST;
    mmio_write32(regs, AHCI_PX_CMD, cmd);
    return wait_mask(regs, AHCI_PX_CMD, AHCI_CMD_CR, AHCI_CMD_CR);
}

static int issue_data_command(uint32_t slot, uint8_t command, uint64_t lba, int is_write) {
    if (slot >= dma_slots_used || !ports[slot].active) return 0;
    AHCI_PORT_SLOT *p = &ports[slot];
    volatile uint8_t *regs = p->registers;
    if (lba >= p->sectors && p->sectors) return 0;
    int lba28 = command == AHCI_CMD_READ_DMA || command == AHCI_CMD_WRITE_DMA;
    int lba48 = command == AHCI_CMD_READ_DMA_EXT || command == AHCI_CMD_WRITE_DMA_EXT;
    if (lba28 && lba >= ATA_LBA28_LIMIT) return 0;
    if (lba48 && (!p->lba48_supported || lba >= ATA_LBA48_LIMIT)) return 0;
    if (!wait_mask(regs, AHCI_PX_TFD, AHCI_TFD_BSY | AHCI_TFD_DRQ, 0u)) return 0;
    if (mmio_read32(regs, AHCI_PX_CI) & 1u) return 0;

    AHCI_COMMAND_HEADER *header = &command_lists[slot][0];
    AHCI_COMMAND_TABLE *table = &command_tables[slot];
    bytes_zero(header, sizeof(*header));
    bytes_zero(table, sizeof(*table));
    header->flags = (uint16_t)(5u | (is_write ? (1u << 6) : 0u)); /* CFL=5 DW; W=1 for host-to-device */
    header->prdt_length = 1u;
    header->ctba = (uint32_t)(uintptr_t)table;
    header->ctbau = (uint32_t)((uint64_t)(uintptr_t)table >> 32);
    table->cfis[0] = 0x27u; /* Register Host-to-Device FIS */
    table->cfis[1] = 0x80u; /* command bit */
    table->cfis[2] = command;
    if (lba28 || lba48) {
        table->cfis[4] = (uint8_t)lba;
        table->cfis[5] = (uint8_t)(lba >> 8);
        table->cfis[6] = (uint8_t)(lba >> 16);
        table->cfis[7] = 0x40u | (lba28 ? (uint8_t)((lba >> 24) & 0x0Fu) : 0u);
        table->cfis[12] = 1u; /* one sector */
        if (lba48) {
            table->cfis[8] = (uint8_t)(lba >> 24);
            table->cfis[9] = (uint8_t)(lba >> 32);
            table->cfis[10] = (uint8_t)(lba >> 40);
            table->cfis[13] = 0u; /* sector-count high byte */
        }
    }
    table->prdt[0].dba = (uint32_t)(uintptr_t)sector_buffers[slot];
    table->prdt[0].dbau = (uint32_t)((uint64_t)(uintptr_t)sector_buffers[slot] >> 32);
    table->prdt[0].dbc_i = 511u;
    __asm__ volatile("mfence" ::: "memory");
    mmio_write32(regs, AHCI_PX_IS, 0xFFFFFFFFu);
    mmio_write32(regs, AHCI_PX_CI, 1u);
    for (uint32_t i = 0; i < AHCI_WAIT_LIMIT; ++i) {
        if (mmio_read32(regs, AHCI_PX_IS) & AHCI_PXIS_TFES) return 0;
        if (!(mmio_read32(regs, AHCI_PX_CI) & 1u)) {
            __asm__ volatile("mfence" ::: "memory");
            return (mmio_read32(regs, AHCI_PX_TFD) & AHCI_TFD_ERR) == 0u;
        }
    }
    return 0;
}

static int ahci_read_sector(void *context, uint64_t lba, uint8_t out[512]) {
    AHCI_PORT_SLOT *p = (AHCI_PORT_SLOT *)context;
    if (!p || !out || !p->active || lba >= p->sectors || lba >= ATA_LBA48_LIMIT) return 0;
    uint32_t slot = (uint32_t)(p - ports);
    uint8_t command = p->lba48_supported ? AHCI_CMD_READ_DMA_EXT : AHCI_CMD_READ_DMA;
    if (!p->lba48_supported && lba >= ATA_LBA28_LIMIT) return 0;
    if (!issue_data_command(slot, command, lba, 0)) return 0;
    for (uint32_t i = 0; i < 512u; ++i) out[i] = sector_buffers[slot][i];
    return 1;
}

/* The callback is private to the explicitly confirmed blank-disk formatter;
   the block registry continues to reject ordinary writes for every disk. */
static int ahci_write_sector(void *context, uint64_t lba, const uint8_t data[512]) {
    AHCI_PORT_SLOT *p = (AHCI_PORT_SLOT *)context;
    if (!p || !data || !p->active || lba >= p->sectors || lba >= ATA_LBA48_LIMIT) return 0;
    uint32_t slot = (uint32_t)(p - ports);
    uint8_t command = p->lba48_supported ? AHCI_CMD_WRITE_DMA_EXT : AHCI_CMD_WRITE_DMA;
    if (!p->lba48_supported && lba >= ATA_LBA28_LIMIT) return 0;
    for (uint32_t i = 0; i < 512u; ++i) sector_buffers[slot][i] = data[i];
    __asm__ volatile("mfence" ::: "memory");
    return issue_data_command(slot, command, lba, 1);
}

static int ahci_flush_cache(void *context) {
    AHCI_PORT_SLOT *p = (AHCI_PORT_SLOT *)context;
    if (!p || !p->active) return 0;
    uint8_t flush_command = p->flush_cache_ext_supported ? ATA_CMD_FLUSH_CACHE_EXT :
                            p->flush_cache_supported ? ATA_CMD_FLUSH_CACHE : 0u;
    if (!flush_command) return 0; /* Never issue a command the drive didn't advertise. */
    uint32_t slot = (uint32_t)(p - ports);
    volatile uint8_t *regs = p->registers;
    if (!wait_mask(regs, AHCI_PX_TFD, AHCI_TFD_BSY | AHCI_TFD_DRQ, 0u)) return 0;
    if (mmio_read32(regs, AHCI_PX_CI) & 1u) return 0;
    AHCI_COMMAND_HEADER *header = &command_lists[slot][0];
    AHCI_COMMAND_TABLE *table = &command_tables[slot];
    bytes_zero(header, sizeof(*header));
    bytes_zero(table, sizeof(*table));
    header->flags = 5u;
    header->prdt_length = 0u;
    header->ctba = (uint32_t)(uintptr_t)table;
    header->ctbau = (uint32_t)((uint64_t)(uintptr_t)table >> 32);
    table->cfis[0] = 0x27u;
    table->cfis[1] = 0x80u;
    table->cfis[2] = flush_command;
    __asm__ volatile("mfence" ::: "memory");
    mmio_write32(regs, AHCI_PX_IS, 0xFFFFFFFFu);
    mmio_write32(regs, AHCI_PX_CI, 1u);
    for (uint32_t i = 0; i < AHCI_WAIT_LIMIT; ++i) {
        if (mmio_read32(regs, AHCI_PX_IS) & AHCI_PXIS_TFES) return 0;
        if (!(mmio_read32(regs, AHCI_PX_CI) & 1u)) {
            __asm__ volatile("mfence" ::: "memory");
            return (mmio_read32(regs, AHCI_PX_TFD) & AHCI_TFD_ERR) == 0u;
        }
    }
    return 0;
}

static int identify_port(uint32_t slot, uint16_t words[256], MINT_ATA_CAPACITY_INFO *capacity) {
    bytes_zero(sector_buffers[slot], sizeof(sector_buffers[slot]));
    ports[slot].active = 1u;
    if (!issue_data_command(slot, AHCI_CMD_IDENTIFY, 0u, 0)) {
        /* IDENTIFY is a device-to-host command but not an LBA READ command; a
           zero sector count is intentionally allowed before capacity is known. */
        return 0;
    }
    for (uint32_t i = 0; i < 256u; ++i)
        words[i] = (uint16_t)(sector_buffers[slot][i * 2u] |
                             ((uint16_t)sector_buffers[slot][i * 2u + 1u] << 8));
    if (!mint_ata_parse_capacity(words, capacity)) return -1;
    ports[slot].lba48_supported=capacity->lba48_supported;
    ports[slot].flush_cache_supported=capacity->flush_cache_supported;
    ports[slot].flush_cache_ext_supported=capacity->flush_cache_ext_supported;
    return 1;
}

static void controller_failure(MINT_STORAGE_CONTROLLER_INFO *ci, const char *message) {
    ci->state = MINT_STORAGE_CTRL_FAILED;
    uint32_t i = 0;
    while (message && message[i] && i + 1u < sizeof(ci->reason)) { ci->reason[i] = message[i]; ++i; }
    ci->reason[i] = '\0';
    mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_AHCI, ci->reason);
}
static void set_device_message(char *out, uint32_t capacity, const char *message) {
    uint32_t i = 0;
    if (message) while (message[i] && i + 1u < capacity) { out[i] = message[i]; ++i; }
    out[i] = '\0';
}
static void log_bdf(const MINT_STORAGE_CONTROLLER_INFO *ci) {
    mint_serial_write("[ahci] controller ");
    mint_serial_putc("0123456789ABCDEF"[ci->bus >> 4]);
    mint_serial_putc("0123456789ABCDEF"[ci->bus & 15u]); mint_serial_putc(':');
    mint_serial_putc("0123456789ABCDEF"[ci->device >> 4]);
    mint_serial_putc("0123456789ABCDEF"[ci->device & 15u]); mint_serial_putc('.');
    mint_serial_dec64(ci->function);
    mint_serial_write(" vendor="); mint_serial_hex64(ci->vendor_id);
    mint_serial_write(" device="); mint_serial_hex64(ci->device_id);
    mint_serial_write(" BAR5="); mint_serial_hex64(ci->abar); mint_serial_write("\n");
}
static void log_port_implemented(const MINT_STORAGE_CONTROLLER_INFO *ci) {
    mint_serial_write("[ahci] controller PI=");
    mint_serial_hex64(ci->port_implemented);
    mint_serial_write("\n");
}

static int ahci_version_plausible(uint32_t vs) {
    if (vs == 0x00000905u) return 1;                         /* AHCI 0.95 */
    return (vs >> 16) == 1u && (vs & 0xFFFFu) <= 0x0301u;    /* AHCI 1.0 .. 1.3.1 */
}

static int is_intel_raid_mode_ahci(const MINT_PCI_DEVICE *d) {
    return d->vendor_id == PCI_VENDOR_INTEL && d->class_code == AHCI_PCI_CLASS &&
           d->subclass == PCI_SUBCLASS_RAID && d->programming_interface == 0x00u;
}

static int probe_controller(uint32_t controller_index, const MINT_PCI_DEVICE *d,
                            MINT_STORAGE_SNAPSHOT *snapshot) {
    MINT_STORAGE_CONTROLLER_INFO *ci = &snapshot->controllers[controller_index];
    ci->bus = d->bus; ci->device = d->device; ci->function = d->function;
    ci->vendor_id = d->vendor_id; ci->device_id = d->device_id;
    uint32_t bar5 = mint_pci_read_config32(d->bus, d->device, d->function, AHCI_BAR5_OFFSET);
    ci->abar = (bar5 == 0xFFFFFFFFu) ? 0u : (uint64_t)(bar5 & ~0x0Fu);
    log_bdf(ci); /* preserve BDF/BAR evidence even when the controller fails validation */
    if (!bar5 || bar5 == 0xFFFFFFFFu || (bar5 & 1u)) {
        controller_failure(ci, "BAR5 invalid or I/O BAR; AHCI MMIO cannot be accessed");
        return 0;
    }
    uint64_t abar = (uint64_t)(bar5 & ~0x0Fu);
    ci->abar = abar;
    if (!abar || abar >= MINT_IDENTITY_MAP_LIMIT ||
        !mint_paging_mark_uncached_range(abar, 0x2000u)) {
        controller_failure(ci, "BAR5 is outside the identity map or could not be marked uncached");
        return 0;
    }
    /* Enable memory decoding and bus mastering only; preserve the command
       register and write zero to status W1C bits. This does not write media. */
    uint32_t old_cmdstat = mint_pci_read_config32(d->bus, d->device, d->function, 0x04u);
    volatile uint8_t *hba = (volatile uint8_t *)(uintptr_t)abar;
    if (is_intel_raid_mode_ahci(d)) {
        /* Enable memory decoding only, prove the ABAR is a real AHCI HBA, and
           only then allow bus mastering below. */
        uint32_t mem_only = (old_cmdstat & 0xFFFFu) | (1u << 1);
        if (!mint_pci_write_config32(d->bus, d->device, d->function, 0x04u, mem_only)) {
            controller_failure(ci, "PCI command register could not enable MMIO");
            return 0;
        }
        uint32_t vs = mmio_read32(hba, AHCI_VS), pi = mmio_read32(hba, AHCI_PI);
        if (!ahci_version_plausible(vs) || !pi || pi == 0xFFFFFFFFu) {
            (void)mint_pci_write_config32(d->bus, d->device, d->function, 0x04u, old_cmdstat & 0xFFFFu);
            controller_failure(ci, "Intel RAID-mode controller does not expose AHCI registers; set SATA to AHCI in firmware");
            return 0;
        }
        mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_AHCI,
                      "Intel RST RAID-mode SATA accepted through its AHCI interface; RAID volumes are not assembled");
        mint_serial_write("[ahci] Intel RAID-mode (class 01:04) controller validated as AHCI; firmware SATA mode 'AHCI' is still recommended\n");
    }
    uint32_t command = (old_cmdstat & 0xFFFFu) | (1u << 1) | (1u << 2);
    if (!mint_pci_write_config32(d->bus, d->device, d->function, 0x04u, command)) {
        controller_failure(ci, "PCI command register could not enable MMIO/DMA");
        return 0;
    }
    controller_hbas[controller_index] = hba;
    uint32_t cap2 = mmio_read32(hba, AHCI_CAP2);
    if (cap2 & 1u) {
        uint32_t bohc = mmio_read32(hba, AHCI_BOHC);
        mmio_write32(hba, AHCI_BOHC, bohc | (1u << 1));
        uint32_t i;
        for (i = 0; i < AHCI_WAIT_LIMIT; ++i) {
            bohc = mmio_read32(hba, AHCI_BOHC);
            if (!(bohc & ((1u << 0) | (1u << 4)))) break;
        }
        if (i == AHCI_WAIT_LIMIT) {
            controller_failure(ci, "BIOS/OS ownership handoff timed out");
            return 0;
        }
    }
    uint32_t ghc = mmio_read32(hba, AHCI_GHC);
    mmio_write32(hba, AHCI_GHC, ghc | (1u << 31)); /* AHCI enable; no global reset */
    ci->port_implemented = mmio_read32(hba, AHCI_PI);
    log_port_implemented(ci);
    ci->state = MINT_STORAGE_CTRL_READY;
    set_device_message(ci->reason, sizeof(ci->reason),
                       "controller ready; serialized DMA with bounded polling; generic media writes remain blocked");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_AHCI, ci->reason);
    return 1;
}

static void probe_ports(uint32_t controller_index, MINT_STORAGE_SNAPSHOT *snapshot) {
    volatile uint8_t *hba = controller_hbas[controller_index];
    MINT_STORAGE_CONTROLLER_INFO *ci = &snapshot->controllers[controller_index];
    if (!hba || ci->state != MINT_STORAGE_CTRL_READY) return;
    uint32_t pi = ci->port_implemented;
    for (uint32_t hw_port = 0; hw_port < AHCI_PORT_MAX; ++hw_port) {
        if (!(pi & (1u << hw_port))) continue;
        volatile uint8_t *regs = hba + AHCI_PORT_BASE + hw_port * AHCI_PORT_STRIDE;
        uint32_t ssts = mmio_read32(regs, AHCI_PX_SSTS);
        uint32_t sig = mmio_read32(regs, AHCI_PX_SIG);
        if ((ssts & 0x0Fu) != 3u || ((ssts >> 8) & 0x0Fu) != 1u) {
            mint_serial_write("[ahci] controller-index="); mint_serial_dec64(controller_index);
            mint_serial_write(" port="); mint_serial_dec64(hw_port);
            mint_serial_write(" link not active SSTS="); mint_serial_hex64(ssts);
            mint_serial_write(" SIG="); mint_serial_hex64(sig); mint_serial_write("\n");
            mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_AHCI,
                          "implemented port link is not active (SSTS DET/IPM check failed)");
            continue;
        }
        if (sig != AHCI_SIG_ATA) {
            mint_serial_write("[ahci] controller-index="); mint_serial_dec64(controller_index);
            mint_serial_write(" port="); mint_serial_dec64(hw_port);
            mint_serial_write(" connected port has unsupported signature=");
            mint_serial_hex64(sig); mint_serial_write("\n");
            mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_AHCI,
                          "online port has non-ATA signature; ATAPI/port multiplier unsupported");
            continue;
        }
        if (snapshot->device_count >= MINT_STORAGE_MAX_DEVICES || dma_slots_used >= AHCI_COMMAND_SLOTS) {
            ++snapshot->device_overflow;
            ci->state = MINT_STORAGE_CTRL_DEVICE_LIMIT;
            set_device_message(ci->reason, sizeof(ci->reason), "device list limit reached; remaining ports skipped");
            mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_AHCI, ci->reason);
            return;
        }
        uint32_t slot = dma_slots_used++;
        MINT_STORAGE_DEVICE_INFO *di = &snapshot->devices[snapshot->device_count++];
        di->controller_index = controller_index;
        di->port = hw_port;
        di->block_index = UINT32_MAX;
        di->block_registered = 0u;
        di->sector_size = 512u;
        di->capacity_source = MINT_STORAGE_CAPACITY_UNKNOWN;
        di->identify_ok = 0u;
        di->read_lba0_ok = 0u;
        di->name[0] = 's'; di->name[1] = 'd';
        di->name[2] = (char)('a' + ((snapshot->device_count - 1u) % 26u)); di->name[3] = '\0';
        set_device_message(di->read_status, sizeof(di->read_status), "not yet read");
        set_device_message(di->write_status, sizeof(di->write_status),
                           "DISABLED - read-only diagnostics; no writes attempted");
        set_device_message(di->filesystem_status, sizeof(di->filesystem_status), "not checked");
        set_device_message(di->mount_status, sizeof(di->mount_status),
                           "NOT MOUNTED - read-only mount validation failed; persistence unavailable");
        ports[slot].registers = regs;
        ports[slot].controller_index = controller_index;
        ports[slot].hardware_port = hw_port;
        ports[slot].sectors = 0u;
        ports[slot].active = 0u;
        mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_AHCI,
                      "SATA link online with ATA device signature; starting bounded IDENTIFY probe");
        if (!start_port(slot)) {
            set_device_message(di->read_status, sizeof(di->read_status),
                               "FAILED: command engine could not stop/start within wait bound");
            set_device_message(di->filesystem_status, sizeof(di->filesystem_status),
                               "not checked: IDENTIFY unavailable");
            set_device_message(di->mount_status, sizeof(di->mount_status), "NOT MOUNTED");
            mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_AHCI, di->read_status);
            continue;
        }
        uint16_t words[256];
        MINT_ATA_CAPACITY_INFO cap = {0};
        int identify_result = identify_port(slot, words, &cap);
        if (identify_result <= 0) {
            ports[slot].active = 0u;
            if (identify_result < 0) {
                di->reported_lba28_sectors = cap.lba28_sectors;
                di->reported_lba48_sectors = cap.lba48_sectors;
                di->lba48_supported = cap.lba48_supported;
                di->capacity_source = cap.source;
                set_device_message(di->read_status, sizeof(di->read_status),
                                   "FAILED: IDENTIFY completed but LBA capacity validation rejected it");
                mint_serial_write("[identify] capacity rejected on controller-index=");
                mint_serial_dec64(controller_index); mint_serial_write(" port=");
                mint_serial_dec64(hw_port); mint_serial_write(" lba28_raw=");
                mint_serial_dec64(cap.lba28_sectors); mint_serial_write(" lba48_supported=");
                mint_serial_write(cap.lba48_supported ? "yes" : "no");
                mint_serial_write(" lba48_raw="); mint_serial_dec64(cap.lba48_sectors);
                mint_serial_write("\n");
            } else {
                set_device_message(di->read_status, sizeof(di->read_status),
                                   "FAILED: ATA IDENTIFY command timed out or returned an error");
            }
            set_device_message(di->filesystem_status, sizeof(di->filesystem_status),
                               identify_result < 0 ? "not checked: IDENTIFY LBA capacity was invalid" :
                                                     "not checked: ATA IDENTIFY command failed");
            mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_AHCI, di->read_status);
            continue;
        }
        di->identify_ok = 1u;
        di->lba48_supported = cap.lba48_supported;
        di->capacity_source = cap.source;
        di->reported_lba28_sectors = cap.lba28_sectors;
        di->reported_lba48_sectors = cap.lba48_sectors;
        di->sector_count = cap.selected_sectors;
        di->capacity_bytes = cap.selected_sectors <= UINT64_MAX / 512u ? cap.selected_sectors * 512u : UINT64_MAX;
        ports[slot].sectors = cap.selected_sectors;
        ports[slot].lba48_supported = cap.lba48_supported;
        ports[slot].flush_cache_supported = cap.flush_cache_supported;
        ports[slot].flush_cache_ext_supported = cap.flush_cache_ext_supported;
        mint_ata_copy_identify_string(words, 27u, 20u, di->model, sizeof(di->model));
        mint_ata_copy_identify_string(words, 10u, 10u, di->serial, sizeof(di->serial));
        MINT_BLOCK_DEVICE dev = {0};
        for (uint32_t j = 0; j + 1u < sizeof(dev.name) && di->name[j]; ++j) dev.name[j] = di->name[j];
        dev.sector_count = cap.selected_sectors;
        dev.sector_size = 512u;
        dev.flags = MINT_BLOCK_FLAG_READONLY;
        dev.context = &ports[slot];
        dev.read_sector = ahci_read_sector;
        /* Kept READONLY in the registry. The only code allowed to invoke this
           callback directly is the confirmed blank-disk formatter. */
        dev.write_sector = ahci_write_sector;
        dev.flush = ahci_flush_cache;
        di->block_index = mint_block_count();
        if (!mint_block_register(&dev)) {
            di->block_index = UINT32_MAX;
            ports[slot].active = 0u;
            set_device_message(di->read_status, sizeof(di->read_status),
                               "IDENTIFY OK, but read-only block registry rejected device");
            set_device_message(di->filesystem_status, sizeof(di->filesystem_status),
                               "not checked: device registration failed");
            set_device_message(di->mount_status, sizeof(di->mount_status), "NOT MOUNTED");
            mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_BLOCK, di->read_status);
            continue;
        }
        di->block_registered = 1u;
        if (!mint_storage_diag_inspect(dev.sector_count, dev.read_sector, dev.context, di)) {
            set_device_message(di->filesystem_status, sizeof(di->filesystem_status),
                               "internal diagnostic parser rejected the device descriptor");
            mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_PARTITION, di->filesystem_status);
        } else {
            mint_log_emit(di->valid_mintos_fat16_count ? MINT_LOG_INFO : MINT_LOG_WARN,
                          MINT_LOG_COMPONENT_PARTITION, di->filesystem_status);
        }
        mint_serial_write("[block] "); mint_serial_write(di->name);
        mint_serial_write(" controller-index="); mint_serial_dec64(controller_index);
        mint_serial_write(" port="); mint_serial_dec64(hw_port);
        mint_serial_write(" sectors="); mint_serial_dec64(di->sector_count);
        mint_serial_write(" bytes="); mint_serial_dec64(di->capacity_bytes);
        mint_serial_write(" capacity-source=");
        mint_serial_write(di->capacity_source == MINT_STORAGE_CAPACITY_LBA48 ? "LBA48" : "LBA28");
        mint_serial_write(" read-only=yes\n");
        mint_serial_write("[identify] "); mint_serial_write(di->name);
        mint_serial_write(" lba28_raw="); mint_serial_dec64(di->reported_lba28_sectors);
        mint_serial_write(" lba48_supported="); mint_serial_write(di->lba48_supported ? "yes" : "no");
        mint_serial_write(" lba48_raw="); mint_serial_dec64(di->reported_lba48_sectors);
        mint_serial_write(" selected_sectors="); mint_serial_dec64(di->sector_count);
        mint_serial_write("\n");
        for (uint32_t pi = 0; pi < di->partition_count && pi < MINT_STORAGE_MAX_PARTITIONS; ++pi) {
            MINT_STORAGE_PARTITION_INFO *part = &di->partitions[pi];
            mint_log_emit(part->state == MINT_STORAGE_PART_VALID_MINTOS_FAT16 ? MINT_LOG_INFO : MINT_LOG_WARN,
                          part->state == MINT_STORAGE_PART_VALID_MINTOS_FAT16 ? MINT_LOG_COMPONENT_FAT16 : MINT_LOG_COMPONENT_PARTITION,
                          part->reason);
        }
        mint_serial_write("[storage] "); mint_serial_write(di->name);
        mint_serial_write(" LBA0 signature=");
        mint_serial_hex64(di->lba0_signature[0]); mint_serial_write(" ");
        mint_serial_hex64(di->lba0_signature[1]); mint_serial_write(" CRC32=");
        mint_serial_hex64(di->lba0_crc32); mint_serial_write(" repeat=");
        mint_serial_write(di->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_MATCH ? "MATCH" :
                          di->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_DIFFERS ? "DIFFERS" :
                          di->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_FAILED ? "FAILED" : "UNKNOWN");
        mint_serial_write(" MBR-disk-id="); mint_serial_hex64(di->mbr_disk_id);
        mint_serial_write(" developer-marker=");
        mint_serial_write(di->developer_disk_id_match ? "MATCH" : "MISMATCH/UNKNOWN");
        mint_serial_write("\n[storage] "); mint_serial_write(di->name);
        mint_serial_write(" filesystem: "); mint_serial_write(di->filesystem_status);
        mint_serial_write("\n");
    }
}

int mint_ahci_init(void) {
    MINT_STORAGE_SNAPSHOT *snapshot = mint_storage_diag_mutable_snapshot();
    const MINT_PCI_INVENTORY *inv = mint_pci_get_inventory();
    if (!snapshot || !inv) return -1;
    dma_slots_used = 0u;
    uint32_t supported = 0u;
    for (uint32_t i = 0; i < inv->devices_stored; ++i) {
        const MINT_PCI_DEVICE *d = &inv->devices[i];
        int standard_ahci = d->class_code == AHCI_PCI_CLASS && d->subclass == AHCI_PCI_SUBCLASS &&
                            d->programming_interface == AHCI_PCI_PROGIF;
        if (!standard_ahci && !is_intel_raid_mode_ahci(d)) continue;
        if (snapshot->controller_count >= MINT_STORAGE_MAX_CONTROLLERS) {
            ++snapshot->controller_overflow;
            mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_AHCI,
                          "controller inventory limit reached; later PCI AHCI controllers skipped");
            break;
        }
        uint32_t ci = snapshot->controller_count++;
        if (probe_controller(ci, d, snapshot)) {
            ++supported;
            probe_ports(ci, snapshot);
        }
    }
    if (!snapshot->controller_count) {
        mint_log_emit(MINT_LOG_WARN, MINT_LOG_COMPONENT_AHCI,
                      "PCI inventory contains no class 01:06:01 AHCI or Intel RAID-mode AHCI controller");
    }
    for (uint32_t i = 0; i < snapshot->controller_count; ++i) {
        if (snapshot->controllers[i].state == MINT_STORAGE_CTRL_READY ||
            snapshot->controllers[i].state == MINT_STORAGE_CTRL_DEVICE_LIMIT) continue;
        ++supported;
    }
    (void)supported;
    return (int)mint_block_count();
}
