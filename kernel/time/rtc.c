#include <stdint.h>
#include "../include/rtc.h"
#ifndef MINT_RTC_HOST_TEST
#include "../include/serial.h"
#endif

static int64_t boot_epoch;

uint8_t mint_rtc_bcd_to_binary(uint8_t v) { return (uint8_t)((v & 0x0Fu) + (v >> 4) * 10u); }

/* Howard Hinnant's days_from_civil: days since 1970-01-01 (proleptic Gregorian). */
int64_t mint_rtc_days_from_civil(int64_t y, uint32_t m, uint32_t d) {
    y -= m <= 2u;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153u * (m + (m > 2u ? (uint32_t)-3 : 9u)) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t mint_rtc_boot_epoch(void) { return boot_epoch; }

#ifndef MINT_RTC_HOST_TEST
static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static uint8_t cmos(uint8_t reg) { outb(0x70u, (uint8_t)(0x80u | reg)); return inb(0x71u); } /* NMI stays masked */

static int update_in_progress(void) { return (cmos(0x0Au) & 0x80u) != 0; }

int mint_rtc_init(void) {
    uint8_t t[6], u[6];
    for (int attempt = 0; attempt < 8; ++attempt) {
        for (uint32_t i = 0; i < 100000u && update_in_progress(); ++i) {}
        t[0] = cmos(0x00u); t[1] = cmos(0x02u); t[2] = cmos(0x04u);
        t[3] = cmos(0x07u); t[4] = cmos(0x08u); t[5] = cmos(0x09u);
        for (uint32_t i = 0; i < 100000u && update_in_progress(); ++i) {}
        u[0] = cmos(0x00u); u[1] = cmos(0x02u); u[2] = cmos(0x04u);
        u[3] = cmos(0x07u); u[4] = cmos(0x08u); u[5] = cmos(0x09u);
        int same = 1;
        for (int i = 0; i < 6; ++i) if (t[i] != u[i]) same = 0;
        if (same) break;
    }
    uint8_t status_b = cmos(0x0Bu);
    int pm = (t[2] & 0x80u) != 0;
    t[2] &= 0x7Fu;
    if (!(status_b & 0x04u)) for (int i = 0; i < 6; ++i) t[i] = mint_rtc_bcd_to_binary(t[i]);
    if (!(status_b & 0x02u)) { t[2] = (uint8_t)(t[2] % 12u); if (pm) t[2] = (uint8_t)(t[2] + 12u); }
    uint32_t sec = t[0], min = t[1], hour = t[2], day = t[3], mon = t[4], year = 2000u + t[5];
    if (sec > 59u || min > 59u || hour > 23u || day < 1u || day > 31u || mon < 1u || mon > 12u) {
        mint_serial_write("[rtc] CMOS clock invalid; wall clock unavailable\n");
        boot_epoch = 0;
        return 0;
    }
    boot_epoch = mint_rtc_days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    mint_serial_write("[rtc] CMOS wall clock ");
    mint_serial_dec64(year); mint_serial_write("-"); mint_serial_dec64(mon); mint_serial_write("-");
    mint_serial_dec64(day); mint_serial_write(" "); mint_serial_dec64(hour); mint_serial_write(":");
    mint_serial_dec64(min); mint_serial_write(" (no timezone applied)\n");
    return 1;
}
#endif
