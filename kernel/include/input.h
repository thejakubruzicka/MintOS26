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
/* Device-neutral injection (USB HID). Key: PS/2 set-1 make code + E0 flag.
 * Relative motion is in device counts, screen-down positive, and goes through
 * the same acceleration curve as PS/2. Absolute motion is scaled to the screen. */
void mint_input_inject_key(uint8_t make, int extended, int pressed);
void mint_input_inject_relative(int32_t dx, int32_t dy, uint32_t buttons);
void mint_input_inject_absolute(uint32_t x, uint32_t x_max, uint32_t y, uint32_t y_max, uint32_t buttons);
void mint_input_inject_wheel(int32_t delta);
void mint_input_set_usb_devices(int keyboard, int pointer);
int mint_input_usb_keyboard(void);
int mint_input_usb_pointer(void);
int mint_input_keyboard_ready(void);
int mint_input_mouse_ready(void);
/* PS/2 controller devices only (the *_ready functions also count USB HID). */
int mint_input_ps2_keyboard_online(void);
int mint_input_ps2_mouse_online(void);
void mint_input_get_counters(uint64_t *keyboard, uint64_t *pointer_motion,
                             uint64_t *pointer_buttons);

#endif
