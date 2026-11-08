#ifndef MINTOS26_INPUT_H
#define MINTOS26_INPUT_H
#include <stdint.h>

/* Initializes the legacy PS/2 controller and unmasks only IRQs whose devices
 * acknowledged initialization. Call with interrupts disabled, after the PIC
 * has been remapped and the desktop event queue is ready. */
int mint_input_init(void);
void mint_input_keyboard_irq(void);
void mint_input_mouse_irq(void);

/* Byte-level decoders are kept separate from port I/O so host tests can feed
 * real PS/2 set-1 scancodes and packet bytes without privileged instructions. */
void mint_input_process_keyboard_byte(uint8_t byte);
void mint_input_process_mouse_byte(uint8_t byte);
/* Pure helpers exposed for host tests: 9-bit PS/2 delta decode and the Q8
 * fixed-point pointer acceleration curve (counts per packet -> pixels). */
int32_t mint_ps2_mouse_delta(uint8_t low, int negative);
uint32_t mint_ps2_mouse_gain_q8(uint32_t speed);
void mint_ps2_mouse_accelerate(int32_t dx, int32_t dy, int32_t *px, int32_t *py);
int mint_input_keyboard_ready(void);
int mint_input_mouse_ready(void);
void mint_input_get_counters(uint64_t *keyboard, uint64_t *pointer_motion,
                             uint64_t *pointer_buttons);

#endif
