#ifndef MINTOS26_E1000_CORE_H
#define MINTOS26_E1000_CORE_H
#include <stdint.h>

static inline uint32_t mint_e1000_ring_next(uint32_t current, uint32_t count) {
    if (!count || current >= count) return 0u;
    return current + 1u == count ? 0u : current + 1u;
}
static inline int mint_e1000_dma_window_valid(uint64_t base, uint64_t bytes, uint64_t limit_exclusive) {
    return bytes && limit_exclusive && base < limit_exclusive && bytes <= limit_exclusive - base;
}
static inline int mint_e1000_frame_length_valid(uint32_t length, uint32_t capacity) {
    return length >= 14u && length <= 1518u && length <= capacity;
}
static inline uint32_t mint_e1000_mdic_read_command(uint8_t phy, uint8_t reg) {
    return ((uint32_t)(reg & 0x1Fu) << 16) | ((uint32_t)(phy & 0x1Fu) << 21) | (2u << 26);
}
static inline uint32_t mint_e1000_mdic_write_command(uint8_t phy, uint8_t reg, uint16_t value) {
    return (uint32_t)value | ((uint32_t)(reg & 0x1Fu) << 16) |
           ((uint32_t)(phy & 0x1Fu) << 21) | (1u << 26);
}
static inline int mint_e1000_mdic_result_ready(uint32_t value) {
    return (value & (1u << 28)) != 0u;
}
static inline int mint_e1000_mdic_result_error(uint32_t value) {
    return (value & (1u << 30)) != 0u;
}

/* VirtualBox's emulated 82540EM routes its internal loopback buffer path when
   RCTL.LBM is TCVR (bits 7:6 = 11b). LBM_MAC can transmit out the virtual NIC
   without feeding this guest's RX ring, which is why the older self-test timed
   out after TX completion. This bit pattern is an emulator diagnostic mode,
   not a claim that all physical E1000 variants implement loopback identically. */
static inline uint32_t mint_e1000_virtualbox_loopback_bits(void) {
    return (3u << 6);
}
static inline uint32_t mint_e1000_virtualbox_loopback_rctl(uint32_t original,
        uint32_t lbm_mask, uint32_t rx_enable, uint32_t accept_flags, uint32_t strip_crc) {
    return (original & ~lbm_mask) | rx_enable | accept_flags | strip_crc |
           mint_e1000_virtualbox_loopback_bits();
}

#endif
