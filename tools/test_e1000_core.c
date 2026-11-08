#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../kernel/include/e1000_core.h"

int main(void) {
    assert(mint_e1000_ring_next(0,8)==1);
    assert(mint_e1000_ring_next(6,8)==7);
    assert(mint_e1000_ring_next(7,8)==0);
    assert(mint_e1000_ring_next(8,8)==0);
    assert(mint_e1000_ring_next(0,0)==0);
    assert(mint_e1000_dma_window_valid(0x100000,0x10000,0x100000000ULL));
    assert(mint_e1000_dma_window_valid(0xFFFF0000ULL,0x10000,0x100000000ULL));
    assert(!mint_e1000_dma_window_valid(0xFFFFF000ULL,0x2000,0x100000000ULL));
    assert(!mint_e1000_dma_window_valid(0,0,0x100000000ULL));
    assert(mint_e1000_frame_length_valid(14,2048));
    assert(mint_e1000_frame_length_valid(1518,2048));
    assert(!mint_e1000_frame_length_valid(13,2048));
    assert(!mint_e1000_frame_length_valid(1519,2048));
    assert(!mint_e1000_frame_length_valid(128,64));
    assert(mint_e1000_mdic_read_command(1,0)==((1u<<21)|(2u<<26)));
    assert(mint_e1000_mdic_write_command(1,0,0x4140u)==(0x4140u|(1u<<21)|(1u<<26)));
    assert(mint_e1000_mdic_result_ready(1u<<28));
    assert(!mint_e1000_mdic_result_ready(0u));
    assert(mint_e1000_mdic_result_error(1u<<30));
    uint32_t rctl_original=0x0400805Au;
    uint32_t rctl_loop=mint_e1000_virtualbox_loopback_rctl(rctl_original,0xC0u,0x02u,0x8018u,0x04000000u);
    assert((rctl_loop & 0xC0u) == 0xC0u); /* VirtualBox loopback-buffer path */
    assert((rctl_loop & ~0xC0u) == ((rctl_original | 0x02u | 0x8018u | 0x04000000u) & ~0xC0u));
    assert((mint_e1000_virtualbox_loopback_bits() >> 6) == 3u);
    puts("PASS: E1000 ring wrap, 32-bit DMA bounds, frame lengths, MDIC encoding and VirtualBox TCVR loopback mode");
    return 0;
}
