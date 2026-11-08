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

    /* A left-button packet moves 5 counts right and 5 counts down (mouse -Y,
     * screen +Y). Per the PS/2 spec the negative Y needs the byte-0 sign bit
     * (0x20) as well as the low byte 0xFB. Normal speed keeps 4 px/count. */
    event_count = 0; cursor_updates = 0;
    mint_input_process_mouse_byte(0x01u); /* bad sync byte must be ignored */
    mint_input_process_mouse_byte(0x29u);
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

    /* 9-bit deltas: magnitudes beyond 127 must keep their direction. */
    CHECK(mint_ps2_mouse_delta(0xC8u, 0) == 200, "+200 counts decode without sign inversion");
    CHECK(mint_ps2_mouse_delta(0x38u, 1) == -200, "-200 counts decode from the byte-0 sign bit");
    CHECK(mint_ps2_mouse_delta(0xFBu, 1) == -5 && mint_ps2_mouse_delta(0x00u, 1) == -256,
          "small negative and saturated negative deltas decode");
    event_count = 0;
    /* Fast flick right (+200 X) while button 1 stays held: must move right
     * and saturate at the display edge, never jump left. */
    mint_input_process_mouse_byte(0x09u);
    mint_input_process_mouse_byte(0xC8u);
    mint_input_process_mouse_byte(0x00u);
    CHECK(event_count == 1u && events[0].type == MINT_DESKTOP_EVENT_POINTER_MOVE &&
          events[0].data.pointer.dx > 0 && events[0].data.pointer.x == 1023,
          "fast +X motion moves right and clamps at the right edge");
    /* Fast flick left with the sign bit and an overflow bit set (Linux-style:
     * overflow is ignored, 9-bit value used). */
    event_count = 0;
    mint_input_process_mouse_byte(0x59u);
    mint_input_process_mouse_byte(0x38u);
    mint_input_process_mouse_byte(0x00u);
    CHECK(event_count == 1u && events[0].data.pointer.dx < 0 && events[0].data.pointer.x == 0,
          "fast -X motion with overflow bit moves left and clamps at the left edge");

    /* Acceleration curve: precise when slow, 4 px/count normal, capped when fast. */
    CHECK(mint_ps2_mouse_gain_q8(1u) == 512u, "slow motion gain is 2 px/count");
    CHECK(mint_ps2_mouse_gain_q8(8u) == 1024u, "normal motion gain is 4 px/count");
    CHECK(mint_ps2_mouse_gain_q8(200u) == 1792u, "fast motion gain caps at 7 px/count");
    for (uint32_t s = 1u; s < 64u; ++s)
        CHECK(mint_ps2_mouse_gain_q8(s + 1u) >= mint_ps2_mouse_gain_q8(s),
              "acceleration curve is monotonic");
    /* Sub-pixel carry: four slow 1-count packets give 8 px total, not 0 or 4. */
    int32_t total = 0;
    for (int i = 0; i < 4; ++i) { int32_t px = 0, py = 0; mint_ps2_mouse_accelerate(1, 0, &px, &py); total += px; }
    CHECK(total == 8, "slow motion accumulates sub-pixel remainders");
    puts("PASS: PS/2 9-bit deltas, fast-motion direction, edge clamping and acceleration curve");
    puts("PASS: PS/2 keyboard set-1 decoder, modifiers, extended keys, mouse packets and button events");
    return 0;
}
