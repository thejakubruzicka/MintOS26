/* Personality PIT hub (26.5.014.0): one hook on every timer tick that keeps
 * the HDA ring fed and samples CPU load for the Notification Center. A tick
 * that interrupts kernel code right after a HLT instruction found the CPU
 * idle (the scheduler halts when nothing is ready); everything else counts
 * as busy. Runs with interrupts disabled; bounded and allocation-free. */
#include <stdint.h>
#include "../include/hda.h"
#include "../include/physmem.h"
#include "../include/paging.h"
#include "../include/task.h"
#include "../include/arch.h"
#include "../include/serial.h"
#include "../include/uapi/mintos/audio.h"

extern void (*volatile mint_timer_irq_hook)(uint64_t interrupted_rip, uint64_t interrupted_cs);

static volatile uint32_t window_ticks, window_idle, load_permille;

static void personality_tick(uint64_t rip, uint64_t cs) {
    ++window_ticks;
    /* Kernel text is identity-mapped, so the byte before a ring-0 RIP is readable. */
    if ((cs & 3u) == 0u && rip > 0x1000u && rip < MINT_IDENTITY_MAP_LIMIT &&
        *(const volatile uint8_t *)(uintptr_t)(rip - 1u) == 0xF4u) ++window_idle;
    if (window_ticks >= 100u) {
        load_permille = (window_ticks - window_idle) * 1000u / window_ticks;
        window_ticks = 0;
        window_idle = 0;
    }
    mint_audio_timer_tick();
}

void mint_personality_timer_init(void) {
    mint_timer_irq_hook = personality_tick;
    mint_serial_write("[audio] PIT hook installed: mixer service and CPU-load sampling\n");
}

int mint_system_status(MINT_SYSTEM_STATUS *out) {
    if (!out) return 0;
    out->version = MINT_SYSTEM_STATUS_VERSION;
    out->cpu_load_permille = load_permille;
    out->tasks = (uint32_t)mint_task_live_count();
    out->reserved = 0;
    out->memory_total_bytes = mint_physmem_total_pages() * 4096u;
    out->memory_free_bytes = mint_physmem_free_pages() * 4096u;
    out->uptime_ms = mint_timer_ticks() * 10u;
    return 1;
}
