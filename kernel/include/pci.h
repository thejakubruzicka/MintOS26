#ifndef MINTOS26_PCI_H
#define MINTOS26_PCI_H
#include <stdint.h>

#define MINT_PCI_INVENTORY_VERSION 2u
#define MINT_PCI_MAX_DEVICES 64u
#define MINT_PCI_BAR_SLOTS 6u

enum mint_pci_bar_kind {
    MINT_PCI_BAR_UNASSIGNED = 0, /* zero BAR; size probing is intentionally not performed */
    MINT_PCI_BAR_IO = 1,
    MINT_PCI_BAR_MEMORY32 = 2,
    MINT_PCI_BAR_MEMORY20 = 3,
    MINT_PCI_BAR_MEMORY64 = 4,
    MINT_PCI_BAR_MEMORY64_UPPER = 5,
    MINT_PCI_BAR_RESERVED = 6,
    MINT_PCI_BAR_ABSENT = 7
};

typedef struct mint_pci_bar {
    uint64_t address;
    uint32_t raw_low;
    uint32_t raw_high;
    uint8_t kind;
    uint8_t slot;
    uint8_t flags; /* raw PCI BAR flag nibble / I/O flag bits */
    uint8_t reserved;
} MINT_PCI_BAR;

typedef struct mint_pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    uint8_t revision;
    uint8_t header_type;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t bar_slots;
    uint8_t reserved;
    MINT_PCI_BAR bars[MINT_PCI_BAR_SLOTS];
} MINT_PCI_DEVICE;

typedef struct mint_pci_inventory {
    uint32_t version;
    uint32_t devices_found;
    uint32_t devices_stored;
    uint32_t overflow_count;
    uint32_t config_slots_scanned;
    MINT_PCI_DEVICE devices[MINT_PCI_MAX_DEVICES];
} MINT_PCI_INVENTORY;

/* Pure decoder used by host tests and by the legacy config-space enumerator. */
int mint_pci_decode_function(uint8_t bus, uint8_t device, uint8_t function,
                             uint32_t id_register, uint32_t class_register,
                             uint32_t header_register, uint32_t command_status_register,
                             MINT_PCI_DEVICE *out);
/* Decode an already-programmed BAR without writing PCI configuration space. */
int mint_pci_decode_bar(uint32_t raw_low, uint32_t raw_high, uint8_t slot,
                        MINT_PCI_BAR *out);
int mint_pci_enumerate(void);
/* Explicit config-space access is reserved for initializing a selected driver. */
uint32_t mint_pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
int mint_pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
const MINT_PCI_INVENTORY *mint_pci_get_inventory(void);

#endif
