#include <stdint.h>
#include <stddef.h>
#include "../include/pci.h"
#include "../include/serial.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu

static MINT_PCI_INVENTORY inventory;

static void zero_bytes(void *ptr, size_t n) {
    uint8_t *p = (uint8_t *)ptr;
    while (n--) *p++ = 0;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" :: "a"(value), "Nd"(port) : "memory");
}
static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
uint32_t mint_pci_read_config32(uint8_t bus, uint8_t device, uint8_t function,
                                uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       ((uint32_t)offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

int mint_pci_write_config32(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t offset, uint32_t value) {
    if (device >= 32u || function >= 8u || (offset & 3u)) return 0;
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) | ((uint32_t)function << 8) |
                       ((uint32_t)offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
    return 1;
}

static uint32_t read_config32(uint8_t bus, uint8_t device, uint8_t function,
                              uint8_t offset) {
    return mint_pci_read_config32(bus, device, function, offset);
}

int mint_pci_decode_function(uint8_t bus, uint8_t device, uint8_t function,
                             uint32_t id_register, uint32_t class_register,
                             uint32_t header_register, uint32_t command_status_register,
                             MINT_PCI_DEVICE *out) {
    uint16_t vendor = (uint16_t)(id_register & 0xFFFFu);
    if (!out || vendor == 0xFFFFu || id_register == 0xFFFFFFFFu || device >= 32u || function >= 8u)
        return 0;
    zero_bytes(out, sizeof(*out));
    out->bus = bus;
    out->device = device;
    out->function = function;
    out->vendor_id = vendor;
    out->device_id = (uint16_t)(id_register >> 16);
    out->revision = (uint8_t)(class_register & 0xFFu);
    out->programming_interface = (uint8_t)((class_register >> 8) & 0xFFu);
    out->subclass = (uint8_t)((class_register >> 16) & 0xFFu);
    out->class_code = (uint8_t)((class_register >> 24) & 0xFFu);
    out->header_type = (uint8_t)((header_register >> 16) & 0xFFu);
    out->command = (uint16_t)(command_status_register & 0xFFFFu);
    out->status = (uint16_t)(command_status_register >> 16);
    return 1;
}

int mint_pci_decode_bar(uint32_t raw_low, uint32_t raw_high, uint8_t slot,
                        MINT_PCI_BAR *out) {
    if (!out || slot >= MINT_PCI_BAR_SLOTS) return 0;
    zero_bytes(out, sizeof(*out));
    out->slot = slot;
    out->raw_low = raw_low;
    out->raw_high = raw_high;
    if (raw_low == 0u) { out->kind = MINT_PCI_BAR_UNASSIGNED; return 1; }
    if (raw_low == 0xFFFFFFFFu) { out->kind = MINT_PCI_BAR_ABSENT; return 1; }
    if (raw_low & 1u) {
        out->kind = MINT_PCI_BAR_IO;
        out->flags = (uint8_t)(raw_low & 3u);
        out->address = (uint64_t)(raw_low & ~3u);
        return 1;
    }
    out->flags = (uint8_t)(raw_low & 0x0Fu);
    switch ((raw_low >> 1) & 3u) {
    case 0u:
        out->kind = MINT_PCI_BAR_MEMORY32;
        out->address = (uint64_t)(raw_low & ~0x0Fu);
        return 1;
    case 1u:
        out->kind = MINT_PCI_BAR_MEMORY20;
        out->address = (uint64_t)(raw_low & ~0x0Fu);
        return 1;
    case 2u:
        out->kind = MINT_PCI_BAR_MEMORY64;
        out->address = ((uint64_t)raw_high << 32) | (uint64_t)(raw_low & ~0x0Fu);
        return 1;
    default:
        out->kind = MINT_PCI_BAR_RESERVED;
        return 1;
    }
}

static void populate_device_resources(MINT_PCI_DEVICE *d) {
    if (!d) return;
    uint8_t header = (uint8_t)(d->header_type & 0x7Fu);
    uint8_t slots = header == 0u ? 6u : (header == 1u ? 2u : (header == 2u ? 1u : 0u));
    d->bar_slots = slots;
    uint32_t irq = read_config32(d->bus, d->device, d->function, 0x3Cu);
    d->interrupt_line = (uint8_t)(irq & 0xFFu);
    d->interrupt_pin = (uint8_t)((irq >> 8) & 0xFFu);
    for (uint8_t i = 0; i < slots; ++i) {
        uint32_t low = read_config32(d->bus, d->device, d->function,
                                     (uint8_t)(0x10u + 4u * i));
        uint32_t high = 0;
        if (low != 0u && low != 0xFFFFFFFFu && !(low & 1u) &&
            ((low >> 1) & 3u) == 2u && (uint8_t)(i + 1u) < slots) {
            high = read_config32(d->bus, d->device, d->function,
                                 (uint8_t)(0x10u + 4u * (i + 1u)));
        }
        (void)mint_pci_decode_bar(low, high, i, &d->bars[i]);
        if (d->bars[i].kind == MINT_PCI_BAR_MEMORY64 && (uint8_t)(i + 1u) < slots) {
            (void)mint_pci_decode_bar(high, 0u, (uint8_t)(i + 1u), &d->bars[i + 1u]);
            d->bars[i + 1u].kind = MINT_PCI_BAR_MEMORY64_UPPER;
            ++i;
        }
    }
}

static int scan_function(uint8_t bus, uint8_t device, uint8_t function,
                         uint8_t *header_type_out) {
    ++inventory.config_slots_scanned;
    uint32_t id = read_config32(bus, device, function, 0x00u);
    if (id == 0xFFFFFFFFu || (id & 0xFFFFu) == 0xFFFFu) return 0;
    uint32_t class_reg = read_config32(bus, device, function, 0x08u);
    uint32_t header_reg = read_config32(bus, device, function, 0x0Cu);
    uint32_t command_status = read_config32(bus, device, function, 0x04u);
    MINT_PCI_DEVICE decoded;
    if (!mint_pci_decode_function(bus, device, function, id, class_reg, header_reg,
                                  command_status, &decoded)) return 0;
    /* Resource discovery is read-only: never size BARs by writing all ones. */
    populate_device_resources(&decoded);
    ++inventory.devices_found;
    if (header_type_out) *header_type_out = decoded.header_type;
    if (inventory.devices_stored < MINT_PCI_MAX_DEVICES) {
        /* Keep freestanding code independent of libc memcpy implementations. */
        volatile uint8_t *dst = (volatile uint8_t *)&inventory.devices[inventory.devices_stored];
        const uint8_t *src = (const uint8_t *)&decoded;
        for (size_t bi = 0; bi < sizeof(decoded); ++bi) dst[bi] = src[bi];
        ++inventory.devices_stored;
    } else {
        ++inventory.overflow_count;
    }
    return 1;
}

static void hex8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    mint_serial_putc(digits[(value >> 4) & 0x0Fu]);
    mint_serial_putc(digits[value & 0x0Fu]);
}
static void hex16(uint16_t value) { hex8((uint8_t)(value >> 8)); hex8((uint8_t)value); }

static void log_device(const MINT_PCI_DEVICE *d) {
    if (!d) return;
    mint_serial_write("[pci] "); hex8(d->bus); mint_serial_putc(':');
    hex8(d->device); mint_serial_putc('.');
    mint_serial_putc((char)('0' + d->function));
    mint_serial_write(" vendor="); hex16(d->vendor_id);
    mint_serial_write(" device="); hex16(d->device_id);
    mint_serial_write(" class="); hex8(d->class_code); mint_serial_putc(':'); hex8(d->subclass);
    mint_serial_write(" prog-if="); hex8(d->programming_interface);
    if (d->class_code == 0x03u) mint_serial_write(" [display device; no GPU driver attached]");
    else if (d->class_code == 0x01u) mint_serial_write(" [mass-storage controller]");
    else if (d->class_code == 0x02u) mint_serial_write(" [network controller]");
    mint_serial_write("\n");
}

int mint_pci_enumerate(void) {
    uint8_t *inventory_bytes = (uint8_t *)&inventory;
    for (size_t i = 0; i < sizeof(inventory); ++i) inventory_bytes[i] = 0;
    inventory.version = MINT_PCI_INVENTORY_VERSION;
    mint_serial_write("[pci] enumerating PCI configuration space via mechanism 1\n");
    for (uint32_t bus = 0; bus < 256u; ++bus) {
        for (uint32_t dev = 0; dev < 32u; ++dev) {
            uint8_t header_type = 0;
            if (!scan_function((uint8_t)bus, (uint8_t)dev, 0u, &header_type)) continue;
            if (header_type & 0x80u) {
                for (uint32_t fn = 1; fn < 8u; ++fn)
                    (void)scan_function((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
            }
        }
    }
    for (uint32_t i = 0; i < inventory.devices_stored; ++i)
        log_device(&inventory.devices[i]);
    mint_serial_write("[pci] enumeration complete found=");
    mint_serial_dec64(inventory.devices_found);
    mint_serial_write(" stored="); mint_serial_dec64(inventory.devices_stored);
    mint_serial_write(" scanned-slots="); mint_serial_dec64(inventory.config_slots_scanned);
    mint_serial_write(" overflow="); mint_serial_dec64(inventory.overflow_count);
    mint_serial_write("\n");
    return (int)inventory.devices_stored;
}

const MINT_PCI_INVENTORY *mint_pci_get_inventory(void) { return &inventory; }
