#include <stdint.h>
#include "../include/serial.h"

#define COM1 0x3F8u
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void mint_serial_init(void) {
    /* COM1, 38400 baud when the UART clock is 1.8432 MHz, 8N1. */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void mint_serial_putc(char c) {
    /* Bound the wait: an unconnected UART must never hang the kernel. */
    for (uint32_t i = 0; i < 100000u; ++i) {
        if (inb(COM1 + 5) & 0x20u) {
            outb(COM1, (uint8_t)c);
            return;
        }
    }
}

void mint_serial_write(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') mint_serial_putc('\r');
        mint_serial_putc(*s++);
    }
}

void mint_serial_hex64(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    mint_serial_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        mint_serial_putc(digits[(value >> shift) & 0xFu]);
}

void mint_serial_dec64(uint64_t value) {
    char digits[21];
    unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value && n < sizeof(digits));
    while (n) mint_serial_putc(digits[--n]);
}
