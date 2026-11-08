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
int mint_input_keyboard_ready(void);
int mint_input_mouse_ready(void);
void mint_input_get_counters(uint64_t *keyboard, uint64_t *pointer_motion,
                             uint64_t *pointer_buttons);

#endif
