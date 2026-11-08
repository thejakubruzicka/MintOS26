#ifndef MINTOS26_SERIAL_H
#define MINTOS26_SERIAL_H
#include <stdint.h>
void mint_serial_init(void);
void mint_serial_write(const char *s);
void mint_serial_putc(char c);
void mint_serial_hex64(uint64_t value);
void mint_serial_dec64(uint64_t value);
#endif
