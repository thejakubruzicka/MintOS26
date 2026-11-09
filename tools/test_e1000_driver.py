#!/usr/bin/env python3
"""Source contracts for the polling-only Intel 82540EM driver."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
drv=(root/'kernel/net/e1000.c').read_text()
phys=(root/'kernel/mm/physmem.c').read_text()
header=(root/'kernel/include/e1000.h').read_text()
syscall=(root/'kernel/abi/syscall.c').read_text()
terminal=(root/'kernel/user/desktop.c').read_text()+''.join(f.read_text() for f in sorted((root/'kernel/user/mintde').glob('*.c')) if f.name!='font_data.c')
checks={
 'boot does read-only detection and defers DMA startup to an explicit command': 'mint_e1000_probe()' in (root/'kernel/kernel.c').read_text() and 'Terminal `net start`' in drv and 'MINT_SYS_E1000_START' in syscall and 'terminal_equals(cmd,"net start")' in terminal,
 'driver binds only the known Intel 82540EM VID:DID': 'E1000_VENDOR 0x8086u' in drv and 'E1000_DEVICE_82540EM 0x100Eu' in drv,
 'BAR0 is validated and mapped uncached before register access': 'mint_paging_mark_uncached_range(bar->address,E1000_MMIO_MAP_SIZE)' in drv and 'BAR0 is not a valid mapped MMIO range' in drv,
 'DMA pool is constrained below the 32-bit device limit': 'mint_physmem_claim_contiguous_below(E1000_DMA_PAGES, E1000_DMA_MAX_EXCLUSIVE)' in drv and 'mint_e1000_dma_window_valid' in drv,
 'PCI bus mastering is enabled only after ring memory is prepared': drv.find('mint_physmem_claim_contiguous_below') < drv.find('command |= 0x0004u'),
 'driver masks interrupts and uses bounded polling': 'mmio_write(REG_IMC,0xFFFFFFFFu)' in drv and 'E1000_WAIT_LIMIT' in drv and 'loopback diagnostic' in header.lower(),
 'fixed RX/TX rings and descriptor DMA addresses are initialized': all(s in drv for s in ['REG_RDBAL','REG_RDLEN','REG_RDT','REG_TDBAL','REG_TDLEN','REG_TDT','rx_buffer_pa(i)','tx_buffer_pa(i)']),
 'receive and transmit paths bound frame length and descriptor waits': 'mint_e1000_frame_length_valid(length,capacity)' in drv and 'for(uint32_t wait=0;wait<E1000_WAIT_LIMIT' in drv and '++info.tx_errors' in drv,
 'VirtualBox loopback uses TCVR mode and restores original CTRL/RCTL': 'mint_e1000_virtualbox_loopback_rctl' in drv and 'mint_e1000_virtualbox_loopback_bits' in (root/'kernel/include/e1000_core.h').read_text() and 'mmio_write(REG_CTRL,original_ctrl)' in drv and 'mmio_write(REG_RCTL,original_rctl)' in drv and 'phy_write(MDIC_PHY_CONTROL' not in drv,
 'loopback captures failure-stage and descriptor/register diagnostics before restoring normal state': 'capture_loopback_diagnostics(failure_stage)' in drv and drv.find('capture_loopback_diagnostics(failure_stage)') < drv.find('mmio_write(REG_RCTL,original_rctl)') and 'diag_tx_desc_status' in header and 'MINT_E1000_LB_TX_TIMEOUT' in drv and 'REG_TDH' in drv and 'REG_RDH' in drv and 'PHYCTRL/PHYSTS=' in drv and 'CTRL/RCTL/TCTL:' in terminal,
 'controller reset clears CTRL.LRST before configuring rings': 'CTRL_LRST (1u << 3)' in drv and 'ctrl_after_reset & ~CTRL_LRST' in drv and 'CTRL.LRST remained asserted after reset' in drv,
 'loopback clears CTRL.LRST and avoids forcing PHY speed/duplex': '(CTRL_LRST|CTRL_SPD_SEL|CTRL_FRCSPD|CTRL_FRCDPX)' in drv and 'MINT_E1000_LB_LINK_RESET_STUCK' in drv and 'PHY' in drv and 'PHY state was' in drv,
 'TX/RX failure snapshot records the actual submitted descriptor and RX buffer': all(k in drv for k in ['diag_tx_command=txd->command','diag_tx_length=txd->length','diag_tx_buffer_address=txd->buffer_address','diag_rx_length=rxd->length','diag_rx_errors=rxd->errors','diag_rx_buffer_address=rxd->buffer_address','runtime.last_tx_valid ? runtime.last_tx_index : runtime.tx_head','diag_tdbal=mmio_read(REG_TDBAL)','diag_tdlen=mmio_read(REG_TDLEN)','diag_pci_command=mint_pci_read_config32']) and all(k in header for k in ['diag_tx_index','diag_rx_index','diag_rx_buffer_address','diag_tx_buffer_address','diag_tx_command','diag_tdlen','diag_pci_command']),
 'TX diagnostic requests Report Status before waiting on DD': 'TX_CMD_EOP|TX_CMD_IFCS|TX_CMD_RS' in drv and 'TXCMD/LEN/BUF=' in drv,
 'loopback selects VirtualBox TCVR mode instead of backend-routed MAC loopback': 'RCTL.LBM is TCVR (11b)' in drv and 'mint_e1000_virtualbox_loopback_rctl' in drv and 'phy_write(MDIC_PHY_CONTROL,MDIC_LOOPBACK_PHY_CTRL)' not in drv and 'mint_e1000_virtualbox_loopback_bits' in (root/'kernel/include/e1000_core.h').read_text(),
 'Terminal reports the last transmitted descriptor and failing RX descriptor details': 'TX index/cmd/len:' in terminal and 'RX index/status/len/errors:' in terminal and 'RX buffer physical:' in terminal,
 'partial init disables PCI bus mastering before releasing DMA memory': 'static int disable_bus_master(void)' in drv and 'static int cleanup_failed_init(int mmio_responsive)' in drv and drv.find('disable_bus_master()') < drv.find('mint_physmem_release_contiguous(runtime.dma_physical'),
 'bus mastering is explicitly cleared before reset and ring setup': 'command &= ~0x0004u; /* bus mastering must remain off during reset/ring setup */' in drv,
 'E1000 diagnostic ABI is explicitly versioned': 'MINT_E1000_INFO_VERSION 4u' in header and 'loopback_stage' in header and 'diag_rx_buffer_address' in header,
 'userspace E1000 status copies through validated user-memory boundary': 'MINT_SYS_E1000_INFO' in syscall and 'mint_address_space_validate_user_range(space,arg0,sizeof(local),1)' in syscall,
 'terminal exposes read-only status and opt-in loopback test': 'terminal_equals(cmd,"net status")' in terminal and 'terminal_equals(cmd,"net test")' in terminal,
 'terminal visibly distinguishes command history and active prompt': 'root#mintos >' in terminal and 'terminal_push_command(cmd)' in terminal and 'terminal_output_kind' in terminal,
 'physical allocator provides a bounded contiguous allocation API': 'mint_physmem_claim_contiguous_below' in phys and 'max_address_exclusive - base' in phys,
}
failed=[]
for name,ok in checks.items():
 print(('PASS: ' if ok else 'FAIL: ')+name)
 if not ok: failed.append(name)
if failed: raise SystemExit(1)
