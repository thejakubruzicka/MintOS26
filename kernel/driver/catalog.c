#include <stdint.h>
#include <stddef.h>
#include "../include/driver_catalog.h"
#include "../include/e1000.h"
#include "../include/pci.h"
#include "../include/input.h"
#include "../include/storage_diag.h"

static void zero_bytes(void *out, size_t n) {
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < n; ++i) p[i] = 0;
}

static void copy_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    size_t i = 0;
    if (src) while (src[i] && i + 1u < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static MINT_DRIVER_RECORD *add(MINT_DRIVER_CATALOG *out, uint32_t id, uint32_t state,
                               const char *name, const char *detail) {
    if (!out || out->count >= MINT_DRIVER_CATALOG_MAX_RECORDS) return 0;
    MINT_DRIVER_RECORD *r = &out->records[out->count++];
    zero_bytes(r, sizeof(*r));
    r->id = id; r->state = state;
    copy_text(r->name, sizeof(r->name), name);
    copy_text(r->detail, sizeof(r->detail), detail);
    return r;
}

static int pci_has(uint16_t vid, uint16_t did, uint8_t class_code, uint8_t subclass) {
    const MINT_PCI_INVENTORY *inventory = mint_pci_get_inventory();
    if (!inventory) return 0;
    uint32_t n = inventory->devices_stored;
    if (n > MINT_PCI_MAX_DEVICES) n = MINT_PCI_MAX_DEVICES;
    for (uint32_t i = 0; i < n; ++i) {
        const MINT_PCI_DEVICE *d = &inventory->devices[i];
        if (vid && (d->vendor_id != vid || (did && d->device_id != did))) continue;
        if (class_code && (d->class_code != class_code || d->subclass != subclass)) continue;
        return 1;
    }
    return 0;
}

const char *mint_driver_state_name(uint32_t state) {
    switch (state) {
    case MINT_DRIVER_ACTIVE: return "ACTIVE";
    case MINT_DRIVER_READ_ONLY: return "READ-ONLY";
    case MINT_DRIVER_DIAGNOSTIC_ONLY: return "DIAGNOSTIC ONLY";
    case MINT_DRIVER_DEVICE_FOUND: return "DEVICE FOUND";
    case MINT_DRIVER_NOT_IMPLEMENTED: return "NOT IMPLEMENTED";
    case MINT_DRIVER_UNAVAILABLE: return "UNAVAILABLE";
    default: return "UNKNOWN";
    }
}

void mint_driver_catalog_snapshot(MINT_DRIVER_CATALOG *out) {
    if (!out) return;
    zero_bytes(out, sizeof(*out));
    out->version = MINT_DRIVER_CATALOG_VERSION;
    const MINT_PCI_INVENTORY *pci = mint_pci_get_inventory();
    const MINT_STORAGE_SNAPSHOT *storage = mint_storage_diag_snapshot();
    add(out, 1u, MINT_DRIVER_ACTIVE, "UEFI GOP display",
        "Software framebuffer; no acceleration.");
    add(out, 2u, pci && pci->devices_stored ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE,
        "PCI inventory", pci && pci->devices_stored ? "Bounded PCI inventory scan." : "PCI inventory unavailable.");
    add(out, 3u, MINT_DRIVER_ACTIVE, "PIT timer", "IRQ0 ticks at approximately 100 Hz.");
    add(out, 4u, mint_input_keyboard_ready() ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE,
        "PS/2 keyboard", mint_input_keyboard_ready() ? "Set-1 keyboard events." : "No initialized PS/2 keyboard.");
    add(out, 5u, mint_input_mouse_ready() ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE,
        "PS/2 mouse", mint_input_mouse_ready() ? "PS/2 three-byte mouse packets." : "No initialized PS/2 mouse.");
    add(out, 6u, storage && storage->controller_count ? MINT_DRIVER_DIAGNOSTIC_ONLY : MINT_DRIVER_UNAVAILABLE,
        "AHCI / SATA", storage && storage->controller_count ? "Polled read-only sector I/O." : "No usable AHCI controller detected.");
    add(out, 7u, storage && storage->readonly_mount_active ? MINT_DRIVER_READ_ONLY : MINT_DRIVER_DIAGNOSTIC_ONLY,
        "FAT16 volume", storage && storage->readonly_mount_active ? "Read-only FAT16, writes off." : "No mount verified; writes disabled.");
    add(out, 8u, pci_has(0x15ADu, 0x0405u, 0, 0) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE,
        "VMware SVGA", pci_has(0x15ADu, 0x0405u, 0, 0) ? "Enumerated; no native SVGA driver." : "Adapter not detected.");
    add(out, 9u, pci_has(0x8086u, 0x100Eu, 0, 0) || pci_has(0, 0, 0x02u, 0) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE,
        "Ethernet controller", pci_has(0x8086u, 0x100Eu, 0, 0) || pci_has(0, 0, 0x02u, 0) ? "Detected; packet/network stack absent." : "No PCI network controller detected.");
    add(out, 10u, pci_has(0, 0, 0x0Cu, 0x03u) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE,
        "USB host controller", pci_has(0, 0, 0x0Cu, 0x03u) ? "Found; no native USB stack." : "No USB-class controller detected.");
    add(out, 11u, pci_has(0, 0, 0x01u, 0x08u) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_NOT_IMPLEMENTED,
        "NVMe storage", pci_has(0, 0, 0x01u, 0x08u) ? "Detected; NVMe I/O unsupported." : "No NVMe driver; no disk probes.");
    add(out, 12u, MINT_DRIVER_NOT_IMPLEMENTED, "ACPI power / sleep", "Power/sleep not implemented.");
    add(out, 13u, MINT_DRIVER_NOT_IMPLEMENTED, "Audio output", "No native audio playback.");
    add(out, 14u, MINT_DRIVER_NOT_IMPLEMENTED, "Native network stack", "TCP/IP/DHCP/DNS absent.");
    add(out, 15u, MINT_DRIVER_NOT_IMPLEMENTED, "USB mass storage", "USB mass storage unavailable; UEFI only.");
    add(out, 16u, pci && pci->devices_stored ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE,
        "PCI BAR / IRQ inventory", pci && pci->devices_stored ?
        "Read-only BAR and interrupt metadata; BAR sizing writes are never issued." : "PCI inventory unavailable.");
    add(out, 17u, MINT_DRIVER_DIAGNOSTIC_ONLY, "16550 COM1 UART",
        "Bounded polled transmit for COM1 logging; receive/input driver not implemented.");
    MINT_E1000_INFO net; (void)mint_e1000_get_info(&net);
    add(out, 18u, net.state == MINT_E1000_ACTIVE ? MINT_DRIVER_ACTIVE :
        (pci_has(0x8086u, 0x100Eu, 0, 0) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE),
        "VirtualBox E1000", net.state == MINT_E1000_ACTIVE ?
        "Intel 82540EM active; DMA RX/TX rings initialized; polling, no IP stack." :
        (net.state == MINT_E1000_FAILED ? "Intel 82540EM initialization failed; see COM1 logs." :
         (pci_has(0x8086u, 0x100Eu, 0, 0) ? "Intel 82540EM detected; idle until Terminal `net start`; bus mastering remains disabled." : "E1000-compatible adapter not detected.")));
    add(out, 19u, pci_has(0x15ADu, 0x0405u, 0, 0) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE,
        "VirtualBox display adapter", pci_has(0x15ADu, 0x0405u, 0, 0) ?
        "VMware SVGA ID detected; software framebuffer in use, no acceleration." : "VMware SVGA-compatible device not detected.");
    add(out, 20u, pci_has(0x8086u, 0x2415u, 0, 0) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE,
        "ICH AC97 audio", pci_has(0x8086u, 0x2415u, 0, 0) ?
        "Audio controller detected; PCM/DMA driver not implemented." : "ICH AC97 audio controller not detected.");
    add(out, 21u, pci_has(0, 0, 0x01u, 0x01u) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE,
        "Legacy IDE controller", pci_has(0, 0, 0x01u, 0x01u) ?
        "IDE controller found; current block path is AHCI only." : "No legacy IDE controller found.");
}
