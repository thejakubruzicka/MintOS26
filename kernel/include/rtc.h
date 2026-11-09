#ifndef MINTOS26_RTC_H
#define MINTOS26_RTC_H
#include <stdint.h>
/* Read the CMOS real-time clock once at boot (bounded wait for the update-in-
 * progress window, BCD/binary and 12/24-hour aware). The wall clock is then
 * boot_epoch + PIT uptime. The RTC holds whatever the firmware keeps: local
 * time on most Windows-installed PCs, UTC on Linux/VM hosts. No timezone is
 * applied. Returns 0 and leaves the epoch at 0 if the RTC reads as invalid. */
int mint_rtc_init(void);
int64_t mint_rtc_boot_epoch(void);
/* Pure helpers exported for host tests. */
int64_t mint_rtc_days_from_civil(int64_t year, uint32_t month, uint32_t day);
uint8_t mint_rtc_bcd_to_binary(uint8_t value);
#endif
