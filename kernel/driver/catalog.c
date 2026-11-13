#include <stdint.h>
#include <stddef.h>
#include "../include/driver_catalog.h"
#include "../include/e1000.h"
#include "../include/usb.h"
#include "../include/net.h"
#include "../include/pci.h"
#include "../include/input.h"
#include "../include/storage_diag.h"
#include "../include/hda.h"
#include "../include/uapi/mintos/audio.h"

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
    add(out, 4u, mint_input_ps2_keyboard_online() ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE,
        "PS/2 keyboard", mint_input_ps2_keyboard_online() ? "Set-1 keyboard events." : "No initialized PS/2 keyboard.");
    add(out, 5u, mint_input_ps2_mouse_online() ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE,
        "PS/2 mouse", mint_input_ps2_mouse_online() ? "PS/2 three-byte mouse packets." : "No initialized PS/2 mouse.");
    add(out, 6u, storage && storage->controller_count ? MINT_DRIVER_DIAGNOSTIC_ONLY : MINT_DRIVER_UNAVAILABLE,
        "AHCI / SATA", storage && storage->controller_count ? "Polled read-only sector I/O." : "No usable AHCI controller detected.");
    add(out, 7u, storage && storage->readonly_mount_active ? MINT_DRIVER_READ_ONLY : MINT_DRIVER_DIAGNOSTIC_ONLY,
        "FAT16 volume", storage && storage->readonly_mount_active ? "Read-only FAT16, writes off." : "No mount verified; writes disabled.");
    add(out, 8u, pci_has(0x15ADu, 0x0405u, 0, 0) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE,
        "VMware SVGA", pci_has(0x15ADu, 0x0405u, 0, 0) ? "Enumerated; no native SVGA driver." : "Adapter not detected.");
    MINT_E1000_INFO nic; (void)mint_e1000_get_info(&nic);
    int intel_nic = nic.state != MINT_E1000_NOT_FOUND;
    add(out, 9u, intel_nic ? (nic.state == MINT_E1000_ACTIVE ? MINT_DRIVER_ACTIVE : MINT_DRIVER_DEVICE_FOUND) :
        (pci_has(0, 0, 0x02u, 0) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE),
        "Ethernet controller", intel_nic ? "Intel E1000 family; see the Intel Ethernet row." :
        (pci_has(0, 0, 0x02u, 0) ? "Detected, but no driver for this network controller." : "No PCI network controller detected."));
    static MINT_USB_INFO usb; (void)mint_usb_get_info(&usb);
    add(out, 10u, usb.state == MINT_USB_STATE_RUNNING ? MINT_DRIVER_ACTIVE :
        (pci_has(0, 0, 0x0Cu, 0x03u) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE),
        "USB host controller (xHCI)", usb.state == MINT_USB_STATE_RUNNING ? "Polled xHCI: HID keyboard, mouse and tablet; no hubs yet." :
        usb.state == MINT_USB_STATE_FAILED ? "xHCI start failed; see Storage & Logs." :
        (pci_has(0, 0, 0x0Cu, 0x03u) ? "Only EHCI/UHCI/OHCI found; those are not supported." : "No USB-class controller detected."));
    add(out, 11u, pci_has(0, 0, 0x01u, 0x08u) ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_NOT_IMPLEMENTED,
        "NVMe storage", pci_has(0, 0, 0x01u, 0x08u) ? "Detected; NVMe I/O unsupported." : "No NVMe driver; no disk probes.");
    add(out, 12u, MINT_DRIVER_ACTIVE, "ACPI power", "S5 soft-off and restart; sleep states not implemented.");
    static MINT_AUDIO_INFO audio; (void)mint_audio_get_info(&audio);
    add(out, 13u, audio.state == MINT_AUDIO_RUNNING ? MINT_DRIVER_ACTIVE :
        audio.state == MINT_AUDIO_ABSENT ? MINT_DRIVER_UNAVAILABLE : MINT_DRIVER_DEVICE_FOUND,
        "Intel High Definition Audio",
        audio.state == MINT_AUDIO_RUNNING ? (audio.experimental ? "Experimental: 48 kHz 16-bit stereo output, polled, kernel mixer." :
                                             "Polled HDA output: 48 kHz 16-bit stereo, kernel mixer.") :
        audio.state == MINT_AUDIO_FOUND ? (audio.experimental ? "Detected; experimental, idle until started in Settings > Sound." :
                                           "Detected; the audio task is starting it.") :
        audio.state == MINT_AUDIO_STARTING ? "Starting: controller reset and codec discovery." :
        audio.state == MINT_AUDIO_FAILED ? "Start failed; see Storage & Logs (AUDIO)." :
        audio.state == MINT_AUDIO_STOPPED ? "Stopped cleanly; bus mastering off, DMA released." :
        "No HDA controller (PCI class 0x0403) detected.");
    static MINT_NET_INFO net_state; (void)mint_net_get_info(&net_state);
    add(out, 14u, net_state.state == MINT_NET_STATE_UP ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE, "IPv4 network stack",
        net_state.state == MINT_NET_STATE_UP ? "Connected: ARP, ICMP, UDP, DHCP, DNS and TCP client." :
        "Not connected; ARP, ICMP, UDP, DHCP, DNS and TCP client ready.");
    add(out, 15u, MINT_DRIVER_NOT_IMPLEMENTED, "USB mass storage", "USB mass storage unavailable; UEFI only.");
    add(out, 16u, pci && pci->devices_stored ? MINT_DRIVER_ACTIVE : MINT_DRIVER_UNAVAILABLE,
        "PCI BAR / IRQ inventory", pci && pci->devices_stored ?
        "Read-only BAR and interrupt metadata; BAR sizing writes are never issued." : "PCI inventory unavailable.");
    add(out, 17u, MINT_DRIVER_DIAGNOSTIC_ONLY, "16550 COM1 UART",
        "Bounded polled transmit for COM1 logging; receive/input driver not implemented.");
    add(out, 18u, nic.state == MINT_E1000_ACTIVE ? MINT_DRIVER_ACTIVE :
        (intel_nic && nic.state != MINT_E1000_FAILED ? MINT_DRIVER_DEVICE_FOUND : MINT_DRIVER_UNAVAILABLE),
        "Intel Ethernet (E1000)", nic.state == MINT_E1000_ACTIVE ?
        "Active: DMA RX/TX rings, polling; carries the IPv4 stack." :
        (nic.state == MINT_E1000_FAILED ? "Initialization failed; see COM1 logs." :
         (intel_nic ? "Detected; idle until Connect; bus mastering remains disabled." : "No E1000-family adapter detected.")));
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
