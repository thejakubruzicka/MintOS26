#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/include/input.h"
#include "../kernel/include/desktop_abi.h"

static MINT_DESKTOP_EVENT events[64];
static size_t event_count;
static int cursor_updates;
static int32_t cursor_x, cursor_y;

int mint_desktop_event_post(const MINT_DESKTOP_EVENT *event) {
    if (!event || event_count >= 64u) return 0;
    events[event_count++] = *event;
    return 1;
}
uint64_t mint_timer_ticks(void) { return 123u; }
int mint_display_current_info(MINT_DISPLAY_INFO *info) {
    if (!info) return 0;
    *info = (MINT_DISPLAY_INFO){1024u,768u,1024u,0u,1024u*768u*4u};
    return 1;
}
void mint_display_cursor_move(int32_t x, int32_t y) {
    ++cursor_updates; cursor_x=x; cursor_y=y;
}
void mint_arch_enable_input_irqs(int keyboard, int mouse) { (void)keyboard; (void)mouse; }
void mint_serial_write(const char *s) { (void)s; }

#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } } while (0)

int main(void) {
    /* Left Shift + A: make/break with code point and modifier state. */
    mint_input_process_keyboard_byte(0x2Au);
    mint_input_process_keyboard_byte(0x1Eu);
    mint_input_process_keyboard_byte(0x9Eu);
    mint_input_process_keyboard_byte(0xAAu);
    CHECK(event_count == 2u, "printable make and break events delivered");
    CHECK(events[0].type == MINT_DESKTOP_EVENT_KEY && events[0].data.key.pressed == 1u &&
          events[0].data.key.codepoint == 'A' && (events[0].data.key.modifiers & 1u),
          "shifted uppercase key make decoded");
    CHECK(events[1].data.key.pressed == 0u && events[1].data.key.codepoint == 'A',
          "key break decoded");

    /* Caps lock is stateful, while an E0 arrow keeps its extended scancode. */
    mint_input_process_keyboard_byte(0x3Au);
    mint_input_process_keyboard_byte(0x1Eu);
    mint_input_process_keyboard_byte(0x9Eu);
    mint_input_process_keyboard_byte(0xE0u);
    mint_input_process_keyboard_byte(0x48u);
    CHECK(events[2].data.key.codepoint == 'A', "caps lock uppercases letters");
    CHECK(events[4].data.key.scancode == 0x148u && events[4].data.key.codepoint == 0u,
          "extended arrow scancode retained");

    /* A left-button packet moves 5 counts right and 5 counts up (screen y down). */
    event_count = 0; cursor_updates = 0;
    mint_input_process_mouse_byte(0x01u); /* bad sync byte must be ignored */
    mint_input_process_mouse_byte(0x09u);
    mint_input_process_mouse_byte(0x05u);
    mint_input_process_mouse_byte(0xFBu);
    CHECK(event_count == 2u, "mouse movement and button transition events delivered");
    CHECK(events[0].type == MINT_DESKTOP_EVENT_POINTER_MOVE && events[0].data.pointer.x == 660 &&
          events[0].data.pointer.y == 380 && events[0].data.pointer.dx == 20 &&
          events[0].data.pointer.dy == 20, "relative mouse packet normalized to display pixels");
    CHECK(events[1].type == MINT_DESKTOP_EVENT_POINTER_BUTTON &&
          events[1].data.pointer.buttons == 1u && events[1].data.pointer.changed_buttons == 1u,
          "left-button state transition reported");
    CHECK(cursor_updates == 0,
          "PS/2 path does not paint a second kernel cursor over the userspace cursor");
    puts("PASS: PS/2 keyboard set-1 decoder, modifiers, extended keys, mouse packets and button events");
    return 0;
}
