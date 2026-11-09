#ifndef MINTOS26_E1000_H
#define MINTOS26_E1000_H
#include <stdint.h>

#define MINT_E1000_INFO_VERSION 5u
#define MINT_E1000_TX_RING_COUNT 16u
#define MINT_E1000_RX_RING_COUNT 32u
#define MINT_E1000_MAX_FRAME 1518u

enum mint_e1000_loopback_stage {
    MINT_E1000_LB_NONE = 0,
    MINT_E1000_LB_PHY_READ = 1,
    MINT_E1000_LB_PHY_WRITE = 2,
    MINT_E1000_LB_TX_TIMEOUT = 3,
    MINT_E1000_LB_RX_TIMEOUT = 4,
    MINT_E1000_LB_RX_INVALID = 5,
    MINT_E1000_LB_FRAME_MISMATCH = 6,
    MINT_E1000_LB_PHY_RESTORE = 7,
    MINT_E1000_LB_LINK_RESET_STUCK = 8
};

enum mint_e1000_state {
    MINT_E1000_NOT_FOUND = 0,
    MINT_E1000_FOUND = 1,
    MINT_E1000_ACTIVE = 2,
    MINT_E1000_FAILED = 3
};

typedef struct mint_e1000_info {
    uint32_t version;
    uint32_t struct_size;
    uint32_t state;
    uint32_t last_error;
    uint8_t bus, device, function, reserved0;
    uint64_t mmio_base;
    uint8_t mac[6];
    uint8_t reserved1[2];
    uint32_t tx_ring_count, rx_ring_count;
    uint64_t tx_submitted, tx_completed, tx_errors;
    uint64_t rx_received, rx_dropped, rx_errors;
    uint64_t loopback_tests, loopback_passed, loopback_failed;
    /* Last opt-in loopback diagnostics, captured before restoring normal mode. */
    uint32_t loopback_stage;
    uint32_t diag_hw_status, diag_ctrl, diag_rctl, diag_tctl;
    uint32_t diag_tdh, diag_tdt, diag_rdh, diag_rdt;
    uint32_t diag_tx_desc_status, diag_rx_desc_status;
    uint32_t diag_tx_index, diag_rx_index;
    uint16_t diag_rx_length;
    uint8_t diag_rx_errors, diag_reserved3;
    uint64_t diag_rx_buffer_address;
    uint32_t diag_mdic;
    uint16_t diag_phy_control, diag_phy_status;
    uint32_t diag_pci_command;
    uint32_t diag_tdbal, diag_tdbah, diag_tdlen;
    uint32_t diag_rdbal, diag_rdbah, diag_rdlen;
    uint64_t diag_tx_buffer_address;
    uint16_t diag_tx_length;
    uint8_t diag_tx_command;
    uint32_t reserved2;
    /* Version 5: adapter identity and link state. */
    uint16_t device_id;
    uint8_t experimental;            /* PCH-integrated I217/I218/I219 */
    uint8_t link_up;
    char name[40];
} MINT_E1000_INFO;

/* Read-only PCI match at boot; never enables MMIO or DMA. */
int mint_e1000_probe(void);
/* Explicitly starts the bounded polling driver after Terminal: net start. */
int mint_e1000_init(void);
int mint_e1000_get_info(MINT_E1000_INFO *out);
/* Runs a VirtualBox 82540EM internal TCVR-loopback diagnostic; sends no external packets. */
int mint_e1000_loopback_test(void);
/* Packet interface for the IPv4 stack. send queues one Ethernet frame (no
   FCS) and returns 1, or 0 if no descriptor freed up within the bounded
   wait; poll returns 1 with a frame, 0 when the ring is empty, -1 for a
   dropped/invalid frame. */
int mint_e1000_active(void);
int mint_e1000_link_up(void);
int mint_e1000_mac(uint8_t out[6]);
int mint_e1000_send(const void *frame, uint32_t length);
int mint_e1000_poll(void *out, uint32_t capacity, uint32_t *length);
#endif
