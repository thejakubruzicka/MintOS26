#include <stdint.h>
#include <stdio.h>
#include "../kernel/include/pci.h"

static char log_sink[4096];
static size_t log_used;
void mint_serial_putc(char c) {
    if (log_used + 1u < sizeof(log_sink)) {
        log_sink[log_used++] = c;
        log_sink[log_used] = '\0';
    }
}
void mint_serial_write(const char *s) { if (s) while (*s) mint_serial_putc(*s++); }
void mint_serial_dec64(uint64_t n) { (void)n; }
void mint_serial_hex64(uint64_t n) { (void)n; }
void mint_serial_init(void) {}

#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } puts("PASS: " msg); } while (0)

int main(void) {
    MINT_PCI_DEVICE d = {0};
    CHECK(mint_pci_decode_function(0, 2, 0, 0xBEEF80EEu, 0x03000001u,
                                   0x00800000u, 0x00070003u, &d),
          "decode a present PCI display function");
    CHECK(d.bus == 0 && d.device == 2 && d.function == 0 &&
          d.vendor_id == 0x80EEu && d.device_id == 0xBEEFu,
          "PCI BDF and vendor/device identifiers are decoded");
    CHECK(d.class_code == 0x03u && d.subclass == 0x00u &&
          d.programming_interface == 0x00u && d.revision == 1u,
          "PCI class, subclass, programming interface and revision are decoded");
    CHECK(d.header_type == 0x80u && d.command == 3u && d.status == 7u,
          "PCI multifunction header and command/status fields are decoded");
    CHECK(!mint_pci_decode_function(0, 0, 0, 0xFFFFFFFFu, 0, 0, 0, &d),
          "absent PCI function is rejected");
    CHECK(!mint_pci_decode_function(0, 32, 0, 0x12348086u, 0, 0, 0, &d),
          "out-of-range PCI device number is rejected");
    CHECK(!mint_pci_decode_function(0, 0, 8, 0x12348086u, 0, 0, 0, &d),
          "out-of-range PCI function number is rejected");
    CHECK(MINT_PCI_MAX_DEVICES == 64u, "PCI inventory uses a bounded device array");
    MINT_PCI_BAR bar = {0};
    CHECK(mint_pci_decode_bar(0x0000C001u, 0u, 0u, &bar) &&
          bar.kind == MINT_PCI_BAR_IO && bar.address == 0xC000u && bar.flags == 1u,
          "PCI I/O BAR decodes address and flags without sizing writes");
    CHECK(mint_pci_decode_bar(0xF0624008u, 0u, 2u, &bar) &&
          bar.kind == MINT_PCI_BAR_MEMORY32 && bar.address == 0xF0624000u && (bar.flags & 8u),
          "PCI 32-bit prefetchable memory BAR decodes accurately");
    CHECK(mint_pci_decode_bar(0xF0000004u, 0x00000012u, 1u, &bar) &&
          bar.kind == MINT_PCI_BAR_MEMORY64 && bar.address == 0x12F0000000ULL,
          "PCI 64-bit memory BAR combines high and low address without probing writes");
    CHECK(mint_pci_decode_bar(0u, 0u, 0u, &bar) && bar.kind == MINT_PCI_BAR_UNASSIGNED,
          "zero BAR remains unknown/unassigned rather than being guessed as a resource");
    CHECK(mint_pci_decode_bar(0xFFFFFFFFu, 0u, 0u, &bar) && bar.kind == MINT_PCI_BAR_ABSENT,
          "all-ones BAR is reported absent");
    CHECK(!mint_pci_decode_bar(0u, 0u, MINT_PCI_BAR_SLOTS, &bar),
          "BAR decoder rejects out-of-range slot numbers");
    return 0;
}
