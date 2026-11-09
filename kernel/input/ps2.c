#include <stdint.h>
#include <stddef.h>
#include "../include/input.h"
#include "../include/arch.h"
#include "../include/serial.h"
#include "../include/desktop_abi.h"

#define PS2_DATA   0x60u
#define PS2_STATUS 0x64u
#define PS2_CMD    0x64u
#define PS2_WAIT_LIMIT 100000u
#define KMOD_SHIFT 0x01u
#define KMOD_CTRL  0x02u
#define KMOD_ALT   0x04u
#define KMOD_CAPS  0x08u
#define KMOD_GUI   0x10u /* Windows/Super key: MintDE treats it as Command */

static int keyboard_online;
static int mouse_online;
static uint8_t modifiers;
static uint8_t extended_prefix;
static int32_t mouse_x = 640;
static int32_t mouse_y = 360;
static uint32_t mouse_buttons;
static uint8_t mouse_packet[3];
static uint8_t mouse_packet_index;
static volatile uint64_t keyboard_event_count;
static volatile uint64_t pointer_motion_count;
static volatile uint64_t pointer_button_count;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port) : "memory");
}
static inline void io_wait(void) { outb(0x80u, 0); }

static int wait_input_empty(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i)
        if ((inb(PS2_STATUS) & 0x02u) == 0) return 1;
    return 0;
}
static int wait_output_full(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i)
        if (inb(PS2_STATUS) & 0x01u) return 1;
    return 0;
}
static int command(uint8_t value) {
    if (!wait_input_empty()) return 0;
    outb(PS2_CMD, value); io_wait(); return 1;
}
static int data_write(uint8_t value) {
    if (!wait_input_empty()) return 0;
    outb(PS2_DATA, value); io_wait(); return 1;
}
static int data_read(uint8_t *value) {
    if (!value || !wait_output_full()) return 0;
    *value = inb(PS2_DATA); return 1;
}
static void drain_output(void) {
    for (uint32_t i = 0; i < 32u && (inb(PS2_STATUS) & 0x01u); ++i)
        (void)inb(PS2_DATA);
}
static int read_config(uint8_t *config) {
    return command(0x20u) && data_read(config);
}
static int write_config(uint8_t config) {
    return command(0x60u) && data_write(config);
}
static int wait_ack(void) {
    uint8_t b = 0;
    for (uint32_t i = 0; i < 8u; ++i) {
        if (!data_read(&b)) return 0;
        if (b == 0xFAu) return 1;
        if (b == 0xFEu) return 0; /* RESEND; boot setup will fail closed */
        /* Ignore interleaved status bytes, but do not spin without a bound. */
    }
    return 0;
}
static int keyboard_command(uint8_t value) {
    return data_write(value) && wait_ack();
}
static int mouse_command(uint8_t value) {
    return command(0xD4u) && data_write(value) && wait_ack();
}

static uint32_t keycodepoint(uint8_t sc, int shift, int caps) {
    static const char normal[0x3B] = {
        [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',
        [0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',[0x0C]='-',[0x0D]='=',
        [0x0E]=8,[0x0F]=9,[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',
        [0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
        [0x1A]='[',[0x1B]=']',[0x1C]='\n',[0x1E]='a',[0x1F]='s',[0x20]='d',
        [0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
        [0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',[0x2C]='z',[0x2D]='x',
        [0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',
        [0x34]='.',[0x35]='/',[0x37]='*',[0x39]=' '
    };
    static const char shifted[0x3B] = {
        [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
        [0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',[0x0C]='_',[0x0D]='+',
        [0x0E]=8,[0x0F]=9,[0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',
        [0x14]='T',[0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',
        [0x1A]='{',[0x1B]='}',[0x1C]='\n',[0x1E]='A',[0x1F]='S',[0x20]='D',
        [0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',
        [0x27]=':',[0x28]='"',[0x29]='~',[0x2B]='|',[0x2C]='Z',[0x2D]='X',
        [0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',
        [0x34]='>',[0x35]='?',[0x37]='*',[0x39]=' '
    };
    if (sc >= 0x3Bu) return 0;
    char c = (shift ? shifted[sc] : normal[sc]);
    if (caps && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if (caps && !shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    else if (caps && shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return (uint8_t)c;
}

static void keyboard_key(uint8_t make, uint8_t ext, uint8_t pressed);
void mint_input_process_keyboard_byte(uint8_t byte) {
    if (byte == 0xE0u) { extended_prefix = 1u; return; }
    uint8_t make = (uint8_t)(byte & 0x7Fu);
    uint8_t pressed = (uint8_t)((byte & 0x80u) == 0);
    uint8_t ext = extended_prefix;
    extended_prefix = 0;
    keyboard_key(make, ext, pressed);
}

/* Shared by PS/2 and USB HID: one set-1 key (make code + E0 flag). */
static void keyboard_key(uint8_t make, uint8_t ext, uint8_t pressed) {
    if (!ext && (make == 0x2Au || make == 0x36u)) {
        if (pressed) modifiers |= KMOD_SHIFT; else modifiers &= (uint8_t)~KMOD_SHIFT;
        return;
    }
    if (make == 0x1Du) {
        if (pressed) modifiers |= KMOD_CTRL; else modifiers &= (uint8_t)~KMOD_CTRL;
        /* Right Ctrl is an E0-prefixed code; only clear when both variants are up. */
        return;
    }
    if (!ext && make == 0x38u) {
        if (pressed) modifiers |= KMOD_ALT; else modifiers &= (uint8_t)~KMOD_ALT;
        return;
    }
    if (!ext && make == 0x3Au && pressed) { modifiers ^= KMOD_CAPS; return; }
    if (ext && (make == 0x5Bu || make == 0x5Cu)) {
        /* Left/right GUI keys. Unlike Shift/Ctrl/Alt this also posts an event so
           MintDE can see Command releases (e.g. ending a Cmd+Tab switch). */
        if (pressed) modifiers |= KMOD_GUI; else modifiers &= (uint8_t)~KMOD_GUI;
    }

    MINT_DESKTOP_EVENT event = {0};
    event.type = MINT_DESKTOP_EVENT_KEY;
    event.timestamp_ms = mint_timer_ticks() * 10u;
    event.data.key.scancode = (uint16_t)make | (ext ? 0x100u : 0u);
    event.data.key.modifiers = modifiers;
    event.data.key.pressed = pressed;
    event.data.key.codepoint = ext ? 0u : keycodepoint(make, (modifiers & KMOD_SHIFT) != 0,
                                                        (modifiers & KMOD_CAPS) != 0);
    if (make == 0x01u && pressed) event.data.key.codepoint = 27u;
    ++keyboard_event_count;
    (void)mint_desktop_event_post(&event);
}

int32_t mint_ps2_mouse_delta(uint8_t low, int negative) {
    return negative ? (int32_t)low - 256 : (int32_t)low;
}

/* Integer-only (no SSE in the kernel) pointer acceleration in Q8 fixed point.
 * Speed is an octagonal |v| estimate in counts per packet. Slow motion gets
 * 2 px/count for precise targeting, normal motion keeps the long-standing
 * 4 px/count VirtualBox feel, and fast flicks ramp to 7 px/count so a 1080p-
 * to-4K screen is crossable without lifting the mouse. Sub-pixel remainders
 * are carried so slow drags do not stall or drift. */
static int32_t accel_remainder_x, accel_remainder_y;
uint32_t mint_ps2_mouse_gain_q8(uint32_t speed) {
    if (speed <= 1u) return 512u;
    if (speed < 6u) return 512u + (speed - 1u) * 512u / 5u;
    if (speed <= 12u) return 1024u;
    if (speed < 36u) return 1024u + (speed - 12u) * 768u / 24u;
    return 1792u;
}
void mint_ps2_mouse_accelerate(int32_t dx, int32_t dy, int32_t *px, int32_t *py) {
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx), ay = (uint32_t)(dy < 0 ? -dy : dy);
    uint32_t speed = ax > ay ? ax + ay / 2u : ay + ax / 2u;
    int32_t gain = (int32_t)mint_ps2_mouse_gain_q8(speed);
    if (!dx) accel_remainder_x = 0;
    if (!dy) accel_remainder_y = 0;
    int32_t sx = dx * gain + accel_remainder_x;
    int32_t sy = dy * gain + accel_remainder_y;
    /* Round toward zero and keep the signed remainder for the next packet. */
    int32_t qx = sx / 256, qy = sy / 256;
    accel_remainder_x = sx - qx * 256;
    accel_remainder_y = sy - qy * 256;
    if (px) *px = qx;
    if (py) *py = qy;
}

static void pointer_relative(int32_t dx, int32_t dy, uint32_t buttons);
void mint_input_process_mouse_byte(uint8_t byte) {
    if (mouse_packet_index == 0u && (byte & 0x08u) == 0) return; /* packet sync */
    mouse_packet[mouse_packet_index++] = byte;
    if (mouse_packet_index < 3u) return;
    mouse_packet_index = 0;
    /* PS/2 movement is 9-bit two's complement: the low eight bits are in
     * bytes 1/2 and the sign bits are byte-0 bits 4 (X) and 5 (Y). Reading the
     * low byte as int8_t inverts any motion beyond +/-127 counts, which real
     * mice and coalescing hypervisors produce during fast movement. Overflow
     * bits (6/7) are ignored like Linux psmouse; the 9-bit value saturates. */
    int32_t dx = mint_ps2_mouse_delta(mouse_packet[1], (mouse_packet[0] & 0x10u) != 0);
    int32_t dy = -mint_ps2_mouse_delta(mouse_packet[2], (mouse_packet[0] & 0x20u) != 0);
    pointer_relative(dx, dy, (uint32_t)(mouse_packet[0] & 0x07u));
}

static void post_pointer(int32_t old_x, int32_t old_y, int32_t px, int32_t py, uint32_t buttons);
/* Accelerated relative motion in device counts (screen-down positive). */
static void pointer_relative(int32_t dx, int32_t dy, uint32_t buttons) {
    MINT_DISPLAY_INFO display = {0};
    if (!mint_display_current_info(&display) || !display.width || !display.height) return;
    int32_t old_x = mouse_x, old_y = mouse_y;
    int32_t px = 0, py = 0;
    mint_ps2_mouse_accelerate(dx, dy, &px, &py);
    mouse_x += px; mouse_y += py;
    post_pointer(old_x, old_y, px, py, buttons);
}

static void post_pointer(int32_t old_x, int32_t old_y, int32_t px, int32_t py, uint32_t buttons) {
    MINT_DISPLAY_INFO display = {0};
    if (!mint_display_current_info(&display) || !display.width || !display.height) return;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if ((uint32_t)mouse_x >= display.width) mouse_x = (int32_t)display.width - 1;
    if ((uint32_t)mouse_y >= display.height) mouse_y = (int32_t)display.height - 1;
    uint32_t changed = buttons ^ mouse_buttons;
    if (old_x != mouse_x || old_y != mouse_y) {
        MINT_DESKTOP_EVENT event = {0};
        event.type = MINT_DESKTOP_EVENT_POINTER_MOVE;
        event.timestamp_ms = mint_timer_ticks() * 10u;
        event.data.pointer.x = mouse_x; event.data.pointer.y = mouse_y;
        event.data.pointer.dx = mouse_x - old_x; event.data.pointer.dy = mouse_y - old_y;
        event.data.pointer.buttons = buttons;
        event.data.pointer.changed_buttons = changed;
        ++pointer_motion_count;
        (void)mint_desktop_event_post(&event);
    }
    if (changed) {
        MINT_DESKTOP_EVENT event = {0};
        event.type = MINT_DESKTOP_EVENT_POINTER_BUTTON;
        event.timestamp_ms = mint_timer_ticks() * 10u;
        event.data.pointer.x = mouse_x; event.data.pointer.y = mouse_y;
        event.data.pointer.dx = px; event.data.pointer.dy = py;
        event.data.pointer.buttons = buttons;
        event.data.pointer.changed_buttons = changed;
        ++pointer_button_count;
        (void)mint_desktop_event_post(&event);
    }
    mouse_buttons = buttons;
}

void mint_input_keyboard_irq(void) {
    uint8_t status = inb(PS2_STATUS);
    if ((status & 0x01u) && !(status & 0x20u)) {
        uint8_t byte = inb(PS2_DATA);
        if (keyboard_online) mint_input_process_keyboard_byte(byte);
    }
}
void mint_input_mouse_irq(void) {
    uint8_t status = inb(PS2_STATUS);
    if ((status & 0x01u) && (status & 0x20u)) {
        uint8_t byte = inb(PS2_DATA);
        if (mouse_online) mint_input_process_mouse_byte(byte);
    }
}
static int usb_keyboard_present, usb_pointer_present;
int mint_input_keyboard_ready(void) { return keyboard_online || usb_keyboard_present; }
int mint_input_mouse_ready(void) { return mouse_online || usb_pointer_present; }
int mint_input_ps2_keyboard_online(void) { return keyboard_online; }
int mint_input_ps2_mouse_online(void) { return mouse_online; }
void mint_input_get_counters(uint64_t *keyboard, uint64_t *pointer_motion,
                             uint64_t *pointer_buttons) {
    if (keyboard) *keyboard = keyboard_event_count;
    if (pointer_motion) *pointer_motion = pointer_motion_count;
    if (pointer_buttons) *pointer_buttons = pointer_button_count;
}

int mint_input_init(void) {
    keyboard_online = 0; mouse_online = 0; modifiers = 0; extended_prefix = 0;
    mouse_packet_index = 0; mouse_buttons = 0;
    accel_remainder_x = 0; accel_remainder_y = 0;
    keyboard_event_count = 0; pointer_motion_count = 0; pointer_button_count = 0;
    MINT_DISPLAY_INFO display = {0};
    if (mint_display_current_info(&display) && display.width && display.height) {
        mouse_x = (int32_t)(display.width / 2u);
        mouse_y = (int32_t)(display.height / 2u);
    }

    /* Prevent controller IRQs while the command byte/devices are configured. */
    if (!command(0xADu) || !command(0xA7u)) {
        mint_serial_write("[input] PS/2 controller command timeout; native input unavailable\n");
        mint_arch_enable_input_irqs(0, 0); return 0;
    }
    drain_output();
    uint8_t config = 0;
    if (!read_config(&config)) {
        mint_serial_write("[input] PS/2 controller config read failed\n");
        mint_arch_enable_input_irqs(0, 0); return 0;
    }
    config &= (uint8_t)~0x03u; /* device IRQs initially disabled */
    config |= 0x30u;            /* both device clocks disabled for setup */
    config |= 0x40u;            /* translate keyboard set 2 to set 1 */
    if (!write_config(config) || !command(0xAEu)) {
        mint_serial_write("[input] PS/2 keyboard port unavailable\n");
    } else {
        config &= (uint8_t)~0x10u; config &= (uint8_t)~0x01u;
        (void)write_config(config);
        if (keyboard_command(0xF4u)) keyboard_online = 1;
    }

    /* Try the second PS/2 channel. USB-tablet-only VirtualBox configurations
     * legitimately fail this probe; the test guide explains the VM setting. */
    if (command(0xA8u)) {
        config &= (uint8_t)~0x20u; config &= (uint8_t)~0x02u;
        (void)write_config(config);
        int mouse_ok = mouse_command(0xF6u) && mouse_command(0xF4u);
        if (mouse_ok) mouse_online = 1;
        else (void)command(0xA7u);
    }

    config |= 0x40u;
    if (keyboard_online) { config &= (uint8_t)~0x10u; config |= 0x01u; }
    else { config |= 0x10u; config &= (uint8_t)~0x01u; }
    if (mouse_online) { config &= (uint8_t)~0x20u; config |= 0x02u; }
    else { config |= 0x20u; config &= (uint8_t)~0x02u; }
    (void)write_config(config);
    mint_arch_enable_input_irqs(keyboard_online, mouse_online);

    mint_serial_write(keyboard_online ? "[input] PS/2 keyboard online (IRQ1, set-1 translation)\n"
                                      : "[input] PS/2 keyboard not detected; check VM keyboard/controller\n");
    mint_serial_write(mouse_online ? "[input] PS/2 mouse online (IRQ12, 3-byte packets)\n"
                                  : "[input] PS/2 mouse unavailable; set VirtualBox Pointing Device to PS/2 Mouse\n");
    return keyboard_online || mouse_online;
}

/* ---- Device-neutral injection used by the USB HID driver. These run from a
 * kernel task, so they mask interrupts to stay atomic against the PS/2 IRQ
 * handlers that share the decoder state and the event queue producer. */
static uint64_t irq_save(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return f; }
static void irq_restore(uint64_t f) { if (f & (1ULL << 9)) __asm__ volatile("sti" ::: "memory"); }

void mint_input_inject_key(uint8_t make, int extended, int pressed) {
    uint64_t f = irq_save();
    keyboard_key((uint8_t)(make & 0x7Fu), (uint8_t)(extended != 0), (uint8_t)(pressed != 0));
    irq_restore(f);
}
void mint_input_inject_relative(int32_t dx, int32_t dy, uint32_t buttons) {
    uint64_t f = irq_save();
    pointer_relative(dx, dy, buttons & 0x07u);
    irq_restore(f);
}
void mint_input_inject_absolute(uint32_t x, uint32_t x_max, uint32_t y, uint32_t y_max, uint32_t buttons) {
    MINT_DISPLAY_INFO display = {0};
    if (!x_max || !y_max || !mint_display_current_info(&display) || !display.width || !display.height) return;
    uint64_t f = irq_save();
    int32_t old_x = mouse_x, old_y = mouse_y;
    if (x > x_max) x = x_max;
    if (y > y_max) y = y_max;
    mouse_x = (int32_t)((uint64_t)x * (display.width - 1u) / x_max);
    mouse_y = (int32_t)((uint64_t)y * (display.height - 1u) / y_max);
    post_pointer(old_x, old_y, mouse_x - old_x, mouse_y - old_y, buttons & 0x07u);
    irq_restore(f);
}
void mint_input_inject_wheel(int32_t delta) {
    if (!delta) return;
    MINT_DESKTOP_EVENT event = {0};
    event.type = MINT_DESKTOP_EVENT_SCROLL;
    event.timestamp_ms = mint_timer_ticks() * 10u;
    event.data.pointer.x = mouse_x; event.data.pointer.y = mouse_y;
    event.data.pointer.dy = delta;
    uint64_t f = irq_save();
    (void)mint_desktop_event_post(&event);
    irq_restore(f);
}
void mint_input_set_usb_devices(int keyboard, int pointer) {
    usb_keyboard_present = keyboard != 0; usb_pointer_present = pointer != 0;
}
int mint_input_usb_keyboard(void) { return usb_keyboard_present; }
int mint_input_usb_pointer(void) { return usb_pointer_present; }
