#include <stdint.h>
#include <stddef.h>
#include "../include/e1000.h"
#include "../include/e1000_core.h"
#include "../include/pci.h"
#include "../include/physmem.h"
#include "../include/paging.h"
#include "../include/serial.h"
#include "../include/log.h"

#define E1000_VENDOR 0x8086u
#define E1000_DEVICE_82540EM 0x100Eu
#define E1000_MMIO_MIN_SIZE 0x6000u
#define E1000_MMIO_MAP_SIZE 0x6000u
#define E1000_DMA_PAGES 16u
#define E1000_DMA_MAX_EXCLUSIVE 0x100000000ULL
#define E1000_WAIT_LIMIT 250000u
#define E1000_RX_DESC_OFF 0x1000u
#define E1000_RX_BUF_OFF  0x2000u
#define E1000_TX_BUF_OFF  0x6000u
#define E1000_BUFFER_SIZE 2048u

#define REG_CTRL  0x0000u
#define REG_STATUS 0x0008u
#define REG_MDIC  0x0020u
#define REG_EERD  0x0014u
#define REG_IMC   0x00D8u
#define REG_RCTL  0x0100u
#define REG_TCTL  0x0400u
#define REG_TIPG  0x0410u
#define REG_RDBAL 0x2800u
#define REG_RDBAH 0x2804u
#define REG_RDLEN 0x2808u
#define REG_RDH   0x2810u
#define REG_RDT   0x2818u
#define REG_TDBAL 0x3800u
#define REG_TDBAH 0x3804u
#define REG_TDLEN 0x3808u
#define REG_TDH   0x3810u
#define REG_TDT   0x3818u
#define REG_MTA   0x5200u
#define REG_RAL0  0x5400u
#define REG_RAH0  0x5404u

#define CTRL_RST (1u << 26)
#define CTRL_FD (1u << 0)
#define CTRL_LRST (1u << 3) /* link reset: must be clear before TX is enabled */
#define CTRL_SLU (1u << 6)
#define STATUS_LU (1u << 1)
#define CTRL_SPD_SEL (3u << 8)
#define CTRL_FRCSPD (1u << 11)
#define CTRL_FRCDPX (1u << 12)
#define CTRL_SPD_1000 (2u << 8)
#define MDIC_PHY_ADDR 1u
#define MDIC_READY (1u << 28)
#define MDIC_ERROR (1u << 30)
#define MDIC_PHY_CONTROL 0u
#define MDIC_PHY_STATUS 1u
#define RCTL_EN (1u << 1)
#define RCTL_LBM_SHIFT 6u
#define RCTL_BAM (1u << 15)
#define RCTL_UPE (1u << 3)
#define RCTL_MPE (1u << 4)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define TX_CMD_EOP (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS (1u << 3)
#define RX_STATUS_DD (1u << 0)
#define TX_STATUS_DD (1u << 0)
#define RX_ERROR_MASK 0xFFu
#define TX_FRAME_MIN 60u
#define TX_FRAME_TEST_TYPE 0x88B5u

typedef struct __attribute__((packed)) e1000_rx_desc {
    uint64_t buffer_address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} E1000_RX_DESC;
typedef struct __attribute__((packed)) e1000_tx_desc {
    uint64_t buffer_address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} E1000_TX_DESC;

_Static_assert(sizeof(E1000_RX_DESC) == 16, "E1000 RX descriptor ABI");
_Static_assert(sizeof(E1000_TX_DESC) == 16, "E1000 TX descriptor ABI");

typedef struct e1000_runtime {
    volatile uint8_t *registers;
    uint64_t dma_physical;
    uint8_t *dma_virtual;
    uint32_t tx_head;
    uint32_t rx_next;
    uint32_t last_tx_index;
    uint8_t last_tx_valid;
    uint8_t initialized;
} E1000_RUNTIME;

static E1000_RUNTIME runtime;
static MINT_E1000_INFO info;

static void zero_bytes(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    while (n--) *b++ = 0;
}
static uint32_t mmio_read(uint32_t offset) {
    if (!runtime.registers || (offset & 3u) || offset >= E1000_MMIO_MAP_SIZE) return 0xFFFFFFFFu;
    uint32_t v = *(volatile uint32_t *)(void *)(runtime.registers + offset);
    __asm__ volatile("lfence" ::: "memory");
    return v;
}
static void mmio_write(uint32_t offset, uint32_t value) {
    if (!runtime.registers || (offset & 3u) || offset >= E1000_MMIO_MAP_SIZE) return;
    __asm__ volatile("sfence" ::: "memory");
    *(volatile uint32_t *)(void *)(runtime.registers + offset) = value;
    (void)*(volatile uint32_t *)(void *)(runtime.registers + REG_STATUS);
}
static int wait_clear(uint32_t offset, uint32_t mask) {
    for (uint32_t i = 0; i < E1000_WAIT_LIMIT; ++i) if (!(mmio_read(offset) & mask)) return 1;
    return 0;
}
static int phy_read(uint8_t reg, uint16_t *value) {
    if (!runtime.registers || !value || reg > 31u) return 0;
    mmio_write(REG_MDIC, mint_e1000_mdic_read_command(MDIC_PHY_ADDR, reg));
    for (uint32_t i=0; i<E1000_WAIT_LIMIT; ++i) {
        uint32_t result=mmio_read(REG_MDIC);
        if (mint_e1000_mdic_result_ready(result)) {
            if (mint_e1000_mdic_result_error(result)) return 0;
            *value=(uint16_t)(result & 0xFFFFu);
            return 1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}
static E1000_RX_DESC *rx_ring(void);
static E1000_TX_DESC *tx_ring(void);
static void capture_loopback_diagnostics(uint32_t stage) {
    info.loopback_stage=stage;
    info.diag_hw_status=mmio_read(REG_STATUS);
    info.diag_ctrl=mmio_read(REG_CTRL);
    info.diag_rctl=mmio_read(REG_RCTL);
    info.diag_tctl=mmio_read(REG_TCTL);
    info.diag_tdh=mmio_read(REG_TDH); info.diag_tdt=mmio_read(REG_TDT);
    info.diag_rdh=mmio_read(REG_RDH); info.diag_rdt=mmio_read(REG_RDT);
    uint32_t tx_index = runtime.last_tx_valid ? runtime.last_tx_index : runtime.tx_head;
    if (tx_index >= MINT_E1000_RING_COUNT) tx_index = 0u;
    uint32_t rx_index = runtime.rx_next;
    if (rx_index >= MINT_E1000_RING_COUNT) rx_index = 0u;
    /* Descriptor status/length are updated asynchronously by device DMA. */
    __asm__ volatile("lfence" ::: "memory");
    volatile E1000_TX_DESC *txd=&tx_ring()[tx_index];
    volatile E1000_RX_DESC *rxd=&rx_ring()[rx_index];
    info.diag_tx_index=tx_index; info.diag_rx_index=rx_index;
    info.diag_tx_desc_status=txd->status;
    info.diag_tx_buffer_address=txd->buffer_address;
    info.diag_tx_length=txd->length;
    info.diag_tx_command=txd->command;
    info.diag_rx_desc_status=rxd->status;
    info.diag_rx_length=rxd->length; info.diag_rx_errors=rxd->errors;
    info.diag_rx_buffer_address=rxd->buffer_address;
    info.diag_tdbal=mmio_read(REG_TDBAL); info.diag_tdbah=mmio_read(REG_TDBAH); info.diag_tdlen=mmio_read(REG_TDLEN);
    info.diag_rdbal=mmio_read(REG_RDBAL); info.diag_rdbah=mmio_read(REG_RDBAH); info.diag_rdlen=mmio_read(REG_RDLEN);
    info.diag_pci_command=mint_pci_read_config32(info.bus,info.device,info.function,0x04u)&0xFFFFu;
    info.diag_mdic=mmio_read(REG_MDIC);
    (void)phy_read(MDIC_PHY_CONTROL,&info.diag_phy_control);
    (void)phy_read(MDIC_PHY_STATUS,&info.diag_phy_status);
    info.last_error=100u+stage;
}
static void log_loopback_failure(uint32_t stage, const char *message) {
    mint_serial_write("[e1000] loopback FAIL stage="); mint_serial_dec64(stage);
    mint_serial_write(" STATUS="); mint_serial_hex64(info.diag_hw_status);
    mint_serial_write(" CTRL="); mint_serial_hex64(info.diag_ctrl);
    mint_serial_write(" RCTL="); mint_serial_hex64(info.diag_rctl);
    mint_serial_write(" TCTL="); mint_serial_hex64(info.diag_tctl);
    mint_serial_write(" TDH/TDT="); mint_serial_hex64(info.diag_tdh); mint_serial_putc('/'); mint_serial_hex64(info.diag_tdt);
    mint_serial_write(" RDH/RDT="); mint_serial_hex64(info.diag_rdh); mint_serial_putc('/'); mint_serial_hex64(info.diag_rdt);
    mint_serial_write(" TXIDX/TXD="); mint_serial_hex64(info.diag_tx_index); mint_serial_putc('/'); mint_serial_hex64(info.diag_tx_desc_status);
    mint_serial_write(" RXIDX/RXD/LEN/ERR/BUF="); mint_serial_hex64(info.diag_rx_index); mint_serial_putc('/'); mint_serial_hex64(info.diag_rx_desc_status); mint_serial_putc('/'); mint_serial_hex64(info.diag_rx_length); mint_serial_putc('/'); mint_serial_hex64(info.diag_rx_errors); mint_serial_putc('/'); mint_serial_hex64(info.diag_rx_buffer_address);
    mint_serial_write(" TXCMD/LEN/BUF="); mint_serial_hex64(info.diag_tx_command); mint_serial_putc('/'); mint_serial_hex64(info.diag_tx_length); mint_serial_putc('/'); mint_serial_hex64(info.diag_tx_buffer_address);
    mint_serial_write(" TDBAL/H/LEN="); mint_serial_hex64(info.diag_tdbal); mint_serial_putc('/'); mint_serial_hex64(info.diag_tdbah); mint_serial_putc('/'); mint_serial_hex64(info.diag_tdlen);
    mint_serial_write(" RDBAL/H/LEN="); mint_serial_hex64(info.diag_rdbal); mint_serial_putc('/'); mint_serial_hex64(info.diag_rdbah); mint_serial_putc('/'); mint_serial_hex64(info.diag_rdlen);
    mint_serial_write(" PCI-CMD="); mint_serial_hex64(info.diag_pci_command);
    mint_serial_write(" MDIC="); mint_serial_hex64(info.diag_mdic);
    mint_serial_write(" PHYCTRL/PHYSTS="); mint_serial_hex64(info.diag_phy_control); mint_serial_putc('/'); mint_serial_hex64(info.diag_phy_status);
    mint_serial_write("\n");
    mint_log_emit(MINT_LOG_WARN,MINT_LOG_COMPONENT_E1000,message);
}
static E1000_RX_DESC *rx_ring(void) { return (E1000_RX_DESC *)(void *)(runtime.dma_virtual + E1000_RX_DESC_OFF); }
static E1000_TX_DESC *tx_ring(void) { return (E1000_TX_DESC *)(void *)runtime.dma_virtual; }
static uint8_t *rx_buffer(uint32_t i) { return runtime.dma_virtual + E1000_RX_BUF_OFF + i * E1000_BUFFER_SIZE; }
static uint8_t *tx_buffer(uint32_t i) { return runtime.dma_virtual + E1000_TX_BUF_OFF + i * E1000_BUFFER_SIZE; }
static uint64_t rx_buffer_pa(uint32_t i) { return runtime.dma_physical + E1000_RX_BUF_OFF + i * E1000_BUFFER_SIZE; }
static uint64_t tx_buffer_pa(uint32_t i) { return runtime.dma_physical + E1000_TX_BUF_OFF + i * E1000_BUFFER_SIZE; }

static int valid_mac(const uint8_t m[6]) {
    uint8_t any = 0, all = 0xFFu;
    for (uint32_t i = 0; i < 6u; ++i) { any |= m[i]; all &= m[i]; }
    return any && all != 0xFFu && !(m[0] & 1u);
}
static void choose_mac_from_regs(void) {
    uint32_t low = mmio_read(REG_RAL0), high = mmio_read(REG_RAH0);
    info.mac[0]=(uint8_t)low; info.mac[1]=(uint8_t)(low>>8); info.mac[2]=(uint8_t)(low>>16); info.mac[3]=(uint8_t)(low>>24);
    info.mac[4]=(uint8_t)high; info.mac[5]=(uint8_t)(high>>8);
    if (valid_mac(info.mac) && (high & 0x80000000u)) return;
    /* Safe locally administered fallback for emulators that expose no valid RAR0. */
    info.mac[0]=0x02u; info.mac[1]=0x26u; info.mac[2]=0x5Au;
    info.mac[3]=info.bus; info.mac[4]=(uint8_t)((info.device<<3)|(info.function&7u)); info.mac[5]=0x01u;
    mmio_write(REG_RAL0, (uint32_t)info.mac[0] | ((uint32_t)info.mac[1]<<8) |
               ((uint32_t)info.mac[2]<<16) | ((uint32_t)info.mac[3]<<24));
    mmio_write(REG_RAH0, (uint32_t)info.mac[4] | ((uint32_t)info.mac[5]<<8) | 0x80000000u);
}
static void serial_hex8(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    mint_serial_putc(digits[(value >> 4) & 0x0Fu]);
    mint_serial_putc(digits[value & 0x0Fu]);
}

/* Never free descriptor/buffer memory until PCI bus mastering is confirmed off.
   This is also used on partial-start failures so a later retry cannot overwrite
   DMA pages still owned by an active device. */
static int disable_bus_master(void) {
    uint32_t raw = mint_pci_read_config32(info.bus, info.device, info.function, 0x04u);
    if (raw == 0xFFFFFFFFu) return 0;
    uint32_t command = raw & 0xFFFFu;
    if (command & 0x0004u) {
        command &= ~0x0004u;
        if (!mint_pci_write_config32(info.bus, info.device, info.function, 0x04u, command)) return 0;
        raw = mint_pci_read_config32(info.bus, info.device, info.function, 0x04u);
        if (raw == 0xFFFFFFFFu || (raw & 0x0004u)) return 0;
    }
    return 1;
}

static int cleanup_failed_init(int mmio_responsive) {
    if (!disable_bus_master()) {
        /* Intentionally retain the DMA allocation if hardware ownership cannot
           be revoked; leaking pages is safer than use-after-free DMA corruption. */
        runtime.initialized = 0u;
        runtime.registers = 0;
        return 0;
    }
    if (runtime.registers && mmio_responsive) {
        mmio_write(REG_IMC, 0xFFFFFFFFu);
        mmio_write(REG_RCTL, 0u);
        mmio_write(REG_TCTL, 0u);
    }
    if (runtime.dma_physical && !mint_physmem_release_contiguous(runtime.dma_physical, E1000_DMA_PAGES)) {
        runtime.initialized = 0u;
        runtime.registers = 0;
        return 0;
    }
    runtime.registers = 0;
    runtime.dma_physical = 0u;
    runtime.dma_virtual = 0;
    runtime.tx_head = 0u;
    runtime.rx_next = 0u;
    runtime.initialized = 0u;
    return 1;
}

static void set_failed(uint32_t code, const char *message) {
    info.state = MINT_E1000_FAILED; info.last_error = code;
    mint_serial_write("[e1000] "); mint_serial_write(message); mint_serial_write("\n");
    mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_E1000, message);
}

int mint_e1000_probe(void) {
    if (runtime.initialized) return 1;
    zero_bytes(&info, sizeof(info));
    info.version = MINT_E1000_INFO_VERSION;
    info.struct_size = sizeof(info);
    info.state = MINT_E1000_NOT_FOUND;
    const MINT_PCI_INVENTORY *inv = mint_pci_get_inventory();
    if (!inv) return 0;
    uint32_t count = inv->devices_stored < MINT_PCI_MAX_DEVICES
        ? inv->devices_stored : MINT_PCI_MAX_DEVICES;
    for (uint32_t i = 0; i < count; ++i) {
        const MINT_PCI_DEVICE *d = &inv->devices[i];
        if (d->vendor_id != E1000_VENDOR || d->device_id != E1000_DEVICE_82540EM) continue;
        info.state = MINT_E1000_FOUND;
        info.bus = d->bus; info.device = d->device; info.function = d->function;
        info.mmio_base = d->bars[0].address;
        mint_serial_write("[e1000] Intel 82540EM detected by read-only PCI scan; driver idle, DMA disabled; use Terminal `net start`\n");
        mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_E1000,
                      "Intel 82540EM detected; driver remains idle until explicit net start; DMA disabled");
        return 1;
    }
    mint_serial_write("[e1000] Intel 82540EM-compatible NIC not detected; no DMA enabled\n");
    return 0;
}

int mint_e1000_init(void) {
    if (runtime.initialized) return 1;
    if (runtime.dma_physical != 0u) {
        info.state = MINT_E1000_FAILED; info.last_error = 11u;
        mint_serial_write("[e1000] refusing retry: previous DMA allocation could not be safely reclaimed\n");
        mint_log_emit(MINT_LOG_ERROR,MINT_LOG_COMPONENT_E1000,"E1000 retry refused because prior DMA ownership is unresolved");
        return 0;
    }
    zero_bytes(&info, sizeof(info)); info.version=MINT_E1000_INFO_VERSION; info.struct_size=sizeof(info);
    info.state=MINT_E1000_NOT_FOUND;
    const MINT_PCI_INVENTORY *inv = mint_pci_get_inventory();
    if (!inv) return 0;
    const MINT_PCI_DEVICE *nic = 0;
    uint32_t count = inv->devices_stored < MINT_PCI_MAX_DEVICES ? inv->devices_stored : MINT_PCI_MAX_DEVICES;
    for (uint32_t i=0; i<count; ++i) {
        const MINT_PCI_DEVICE *d=&inv->devices[i];
        if (d->vendor_id==E1000_VENDOR && d->device_id==E1000_DEVICE_82540EM) { nic=d; break; }
    }
    if (!nic) { mint_serial_write("[e1000] Intel 82540EM-compatible NIC not detected\n"); return 0; }
    info.state=MINT_E1000_FOUND; info.bus=nic->bus; info.device=nic->device; info.function=nic->function;
    const MINT_PCI_BAR *bar=&nic->bars[0]; info.mmio_base=bar->address;
    if ((bar->kind!=MINT_PCI_BAR_MEMORY32 && bar->kind!=MINT_PCI_BAR_MEMORY64) ||
        (bar->address & 0xFFFu) || bar->address>=mint_paging_identity_limit() ||
        E1000_MMIO_MAP_SIZE>mint_paging_identity_limit()-bar->address) {
        set_failed(1u,"BAR0 is not a valid mapped MMIO range"); return 0;
    }
    if (!mint_paging_mark_uncached_range(bar->address,E1000_MMIO_MAP_SIZE)) {
        set_failed(2u,"could not mark BAR0 uncached"); return 0;
    }
    runtime.registers=(volatile uint8_t *)(uintptr_t)bar->address;
    uint64_t dma=mint_physmem_claim_contiguous_below(E1000_DMA_PAGES, E1000_DMA_MAX_EXCLUSIVE);
    if (!dma) { runtime.registers=0; set_failed(3u,"no contiguous DMA memory below 4 GiB"); return 0; }
    runtime.dma_physical=dma; runtime.dma_virtual=(uint8_t *)(uintptr_t)dma;
    if ((dma & 0xFFFu) || !mint_e1000_dma_window_valid(dma,E1000_DMA_PAGES*4096ULL,E1000_DMA_MAX_EXCLUSIVE)) {
        (void)cleanup_failed_init(0);
        set_failed(4u,"DMA allocation exceeds 32-bit device address limit"); return 0;
    }
    zero_bytes(runtime.dma_virtual,E1000_DMA_PAGES*4096u);

    /* Enable memory decoding only; do not write PCI status W1C bits. Bus master
       stays disabled until descriptor memory has been prepared and validated. */
    uint32_t command = mint_pci_read_config32(info.bus,info.device,info.function,0x04u) & 0xFFFFu;
    command &= ~0x0004u; /* bus mastering must remain off during reset/ring setup */
    command |= 0x0002u;
    if (!mint_pci_write_config32(info.bus,info.device,info.function,0x04u,command)) {
        (void)cleanup_failed_init(0);
        set_failed(5u,"could not enable PCI memory decoding"); return 0;
    }
    if (mmio_read(REG_STATUS)==0xFFFFFFFFu) {
        (void)cleanup_failed_init(0);
        set_failed(6u,"MMIO status read returned all ones"); return 0;
    }
    /* Mask interrupts and perform the controller reset with a bounded wait. */
    mmio_write(REG_IMC,0xFFFFFFFFu);
    mmio_write(REG_CTRL,mmio_read(REG_CTRL)|CTRL_RST);
    if (!wait_clear(REG_CTRL,CTRL_RST)) {
        (void)cleanup_failed_init(0); set_failed(7u,"controller reset timed out; DMA was not enabled"); return 0;
    }
    if (mmio_read(REG_STATUS)==0xFFFFFFFFu) { (void)cleanup_failed_init(0); set_failed(8u,"controller vanished after reset"); return 0; }
    /* Some emulated 82540EM states expose CTRL.LRST after reset. Leaving it
       asserted keeps STATUS.LU down and the TX descriptor engine stalled. */
    uint32_t ctrl_after_reset=mmio_read(REG_CTRL);
    if (ctrl_after_reset & CTRL_LRST) mmio_write(REG_CTRL,ctrl_after_reset & ~CTRL_LRST);
    if (mmio_read(REG_CTRL) & CTRL_LRST) {
        (void)cleanup_failed_init(1); set_failed(11u,"CTRL.LRST remained asserted after reset"); return 0;
    }
    choose_mac_from_regs();
    /* Drop stale multicast filters. These are ordinary device registers only. */
    for (uint32_t off=REG_MTA; off<REG_RAL0; off+=4u) mmio_write(off,0u);

    E1000_TX_DESC *tx=tx_ring(); E1000_RX_DESC *rx=rx_ring();
    for (uint32_t i=0;i<MINT_E1000_RING_COUNT;++i) {
        tx[i].buffer_address=tx_buffer_pa(i); tx[i].length=0; tx[i].checksum_offset=0;
        tx[i].command=TX_CMD_EOP|TX_CMD_IFCS|TX_CMD_RS; tx[i].status=TX_STATUS_DD; tx[i].checksum_start=0; tx[i].special=0;
        rx[i].buffer_address=rx_buffer_pa(i); rx[i].length=0; rx[i].checksum=0; rx[i].status=0; rx[i].errors=0; rx[i].special=0;
    }
    mmio_write(REG_RCTL,0u); mmio_write(REG_TCTL,0u);
    mmio_write(REG_RDBAL,(uint32_t)(dma+E1000_RX_DESC_OFF)); mmio_write(REG_RDBAH,0u);
    mmio_write(REG_RDLEN,MINT_E1000_RING_COUNT*sizeof(E1000_RX_DESC));
    mmio_write(REG_RDH,0u); mmio_write(REG_RDT,MINT_E1000_RING_COUNT-1u);
    mmio_write(REG_TDBAL,(uint32_t)dma); mmio_write(REG_TDBAH,0u);
    mmio_write(REG_TDLEN,MINT_E1000_RING_COUNT*sizeof(E1000_TX_DESC));
    mmio_write(REG_TDH,0u); mmio_write(REG_TDT,0u);
    mmio_write(REG_TIPG,10u|(8u<<10)|(6u<<20));

    command |= 0x0004u;
    if (!mint_pci_write_config32(info.bus,info.device,info.function,0x04u,command)) {
        (void)cleanup_failed_init(1); set_failed(9u,"could not enable PCI bus mastering"); return 0;
    }
    /* Receive broadcast + valid unicast, strip CRC. Keep interrupt path masked. */
    mmio_write(REG_RCTL,RCTL_EN|RCTL_BAM|RCTL_SECRC);
    mmio_write(REG_TCTL,0x0103F0FAu|TCTL_EN|TCTL_PSP);
    __asm__ volatile("mfence" ::: "memory");
    if ((mmio_read(REG_RCTL)&RCTL_EN)==0u || (mmio_read(REG_TCTL)&TCTL_EN)==0u) {
        int responsive = mmio_read(REG_STATUS) != 0xFFFFFFFFu;
        (void)cleanup_failed_init(responsive); set_failed(10u,"descriptor engines did not enable"); return 0;
    }
    runtime.tx_head=0; runtime.rx_next=0; runtime.last_tx_index=0u; runtime.last_tx_valid=0u; runtime.initialized=1u;
    info.state=MINT_E1000_ACTIVE; info.last_error=0; info.tx_ring_count=MINT_E1000_RING_COUNT; info.rx_ring_count=MINT_E1000_RING_COUNT;
    mint_serial_write("[e1000] Intel 82540EM driver active; bounded polling RX/TX rings initialized; MAC=");
    for(uint32_t i=0;i<6u;++i){ if(i)mint_serial_putc(':'); serial_hex8(info.mac[i]); }
    mint_serial_write(" DMA below 4 GiB; interrupts masked\n");
    mint_log_emit(MINT_LOG_INFO,MINT_LOG_COMPONENT_E1000,"Intel 82540EM initialized with bounded polling RX/TX rings; interrupts masked");
    return 1;
}

static int transmit_frame(const uint8_t *frame, uint16_t length) {
    if (!runtime.initialized || !frame || length<14u || length>MINT_E1000_MAX_FRAME) return 0;
    uint32_t i=runtime.tx_head; volatile E1000_TX_DESC *d=&tx_ring()[i];
    runtime.last_tx_index=i; runtime.last_tx_valid=1u;
    for(uint32_t wait=0; wait<E1000_WAIT_LIMIT && !(d->status&TX_STATUS_DD); ++wait) { }
    if (!(d->status&TX_STATUS_DD)) { ++info.tx_errors; return 0; }
    uint16_t padded=length<TX_FRAME_MIN?TX_FRAME_MIN:length;
    uint8_t *buf=tx_buffer(i); for(uint32_t j=0;j<padded;++j) buf[j]=j<length?frame[j]:0;
    d->buffer_address=tx_buffer_pa(i); d->length=padded; d->checksum_offset=0;
    d->command=TX_CMD_EOP|TX_CMD_IFCS|TX_CMD_RS; d->status=0; d->checksum_start=0; d->special=0;
    __asm__ volatile("mfence" ::: "memory");
    mmio_write(REG_TDT,(i+1u)%MINT_E1000_RING_COUNT);
    ++info.tx_submitted;
    for(uint32_t wait=0;wait<E1000_WAIT_LIMIT;++wait) {
        __asm__ volatile("pause");
        if(d->status&TX_STATUS_DD){++info.tx_completed;runtime.tx_head=mint_e1000_ring_next(i,MINT_E1000_RING_COUNT);return 1;}
    }
    ++info.tx_errors;
    return 0;
}
static int receive_frame(uint8_t *out, uint16_t capacity, uint16_t *length_out) {
    if (!runtime.initialized || !out || !length_out || !capacity) return -1;
    uint32_t i=runtime.rx_next; volatile E1000_RX_DESC *d=&rx_ring()[i];
    __asm__ volatile("lfence" ::: "memory");
    if (!(d->status&RX_STATUS_DD)) return 0;
    uint16_t length=d->length; uint8_t errors=d->errors;
    int ok=mint_e1000_frame_length_valid(length,capacity) && length<=E1000_BUFFER_SIZE && (errors&RX_ERROR_MASK)==0;
    if(ok) { const uint8_t *src=rx_buffer(i); for(uint32_t j=0;j<length;++j)out[j]=src[j]; *length_out=length; ++info.rx_received; }
    else { ++info.rx_dropped; ++info.rx_errors; }
    d->length=0; d->checksum=0; d->errors=0; d->special=0; d->status=0;
    __asm__ volatile("mfence" ::: "memory");
    mmio_write(REG_RDT,i);
    runtime.rx_next=mint_e1000_ring_next(i,MINT_E1000_RING_COUNT);
    return ok?1:-1;
}

int mint_e1000_loopback_test(void) {
    if (!runtime.initialized) return -1;
    ++info.loopback_tests;
    info.loopback_stage=MINT_E1000_LB_NONE;
    runtime.last_tx_valid=0u;
    uint32_t original_rctl=mmio_read(REG_RCTL);
    uint32_t original_ctrl=mmio_read(REG_CTRL);
    uint32_t failure_stage=MINT_E1000_LB_NONE;
    /* VirtualBox's emulated 82540EM only takes its internal loopback-buffer path
       when RCTL.LBM is TCVR (11b). LBM_MAC (01b) lets TX complete but routes the
       frame out to the virtual network backend, leaving this guest's RX ring
       empty. This test targets VirtualBox's emulation; it is not a universal
       physical-NIC diagnostic. Do not touch PHY registers or force speed/duplex. */
    {
        uint32_t ctrl=(original_ctrl & ~(CTRL_LRST|CTRL_SPD_SEL|CTRL_FRCSPD|CTRL_FRCDPX)) | CTRL_SLU;
        mmio_write(REG_CTRL,ctrl);
        if (mmio_read(REG_CTRL)&CTRL_LRST) {
            failure_stage=MINT_E1000_LB_LINK_RESET_STUCK;
            goto cleanup;
        }
    }
    /* Bounded register settle. VirtualBox's internal TCVR loopback is an
       emulator route; record STATUS.LU but do not use it as a TX gate. */
    for (uint32_t settle=0; settle<20000u; ++settle) {
        (void)mmio_read(REG_STATUS);
        __asm__ volatile("pause");
    }
    uint32_t test_rctl=mint_e1000_virtualbox_loopback_rctl(original_rctl,
                       (3u<<RCTL_LBM_SHIFT),RCTL_EN,RCTL_BAM|RCTL_UPE|RCTL_MPE,RCTL_SECRC);
    mmio_write(REG_RCTL,test_rctl);
    {
        uint8_t frame[TX_FRAME_MIN]; zero_bytes(frame,sizeof(frame));
        for(uint32_t i=0;i<6u;++i){frame[i]=info.mac[i];frame[6u+i]=info.mac[i];}
        frame[12]=(uint8_t)(TX_FRAME_TEST_TYPE>>8); frame[13]=(uint8_t)TX_FRAME_TEST_TYPE;
        static const uint8_t marker[]="MINTOS26-E1000-LOOPBACK";
        for(uint32_t i=0;i<sizeof(marker)-1u;++i)frame[14u+i]=marker[i];
        int tx_ok=transmit_frame(frame,sizeof(frame));
        if (!tx_ok) {
            failure_stage=MINT_E1000_LB_TX_TIMEOUT;
            goto cleanup;
        }
        uint8_t received[E1000_BUFFER_SIZE]; uint16_t got=0; int rx=0;
        for(uint32_t i=0;i<E1000_WAIT_LIMIT;++i){
            rx=receive_frame(received,sizeof(received),&got);
            if(rx!=0)break;
            __asm__ volatile("pause");
        }
        if(rx==0){failure_stage=MINT_E1000_LB_RX_TIMEOUT;goto cleanup;}
        if(rx<0){failure_stage=MINT_E1000_LB_RX_INVALID;goto cleanup;}
        if(got<14u+sizeof(marker)-1u){failure_stage=MINT_E1000_LB_FRAME_MISMATCH;goto cleanup;}
        uint32_t match=1;
        for(uint32_t i=0;i<6u;++i)if(received[i]!=info.mac[i]||received[6u+i]!=info.mac[i])match=0;
        if(received[12]!=(uint8_t)(TX_FRAME_TEST_TYPE>>8)||received[13]!=(uint8_t)TX_FRAME_TEST_TYPE)match=0;
        for(uint32_t i=0;i<sizeof(marker)-1u;++i)if(received[14u+i]!=marker[i])match=0;
        if(!match){failure_stage=MINT_E1000_LB_FRAME_MISMATCH;goto cleanup;}
    }
cleanup:
    /* Preserve the exact failing register/descriptor state before restoring the
       normal link, so the COM1 snapshot explains the failure rather than cleanup. */
    if (failure_stage!=MINT_E1000_LB_NONE) capture_loopback_diagnostics(failure_stage);
    /* Always restore the original receive/control configuration. PHY state was
       not modified by this VirtualBox-specific RCTL loopback diagnostic. */
    mmio_write(REG_RCTL,original_rctl);
    mmio_write(REG_CTRL,original_ctrl);
    if (failure_stage!=MINT_E1000_LB_NONE) {
        ++info.loopback_failed;
        static const char *const reasons[]={"none","reserved legacy stage 1","reserved legacy stage 2",
          "TX descriptor completion timed out","RX descriptor completion timed out","RX descriptor reported invalid frame",
          "loopback frame payload mismatch","reserved legacy stage 7","CTRL.LRST remained asserted before TX"};
        log_loopback_failure(failure_stage,reasons[failure_stage<=MINT_E1000_LB_LINK_RESET_STUCK?failure_stage:0u]);
        return 0;
    }
    ++info.loopback_passed;
    info.loopback_stage=MINT_E1000_LB_NONE; info.last_error=0u;
    mint_log_emit(MINT_LOG_INFO,MINT_LOG_COMPONENT_E1000,"VirtualBox E1000 internal TCVR loopback TX/RX test passed");
    mint_serial_write("[e1000] VirtualBox E1000 internal TCVR loopback TX/RX test passed\n");
    return 1;
}

int mint_e1000_get_info(MINT_E1000_INFO *out) {
    if(!out)return 0;
    /* Avoid compiler-emitted libc memcpy in the freestanding kernel. */
    volatile uint8_t *dst=(volatile uint8_t *)out;
    const uint8_t *src=(const uint8_t *)&info;
    for(size_t i=0;i<sizeof(*out);++i) dst[i]=src[i];
    out->version=MINT_E1000_INFO_VERSION; out->struct_size=sizeof(*out);
    return 1;
}
