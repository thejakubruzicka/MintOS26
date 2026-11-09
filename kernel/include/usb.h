#ifndef MINTOS26_USB_H
#define MINTOS26_USB_H
#include <stdint.h>
#include <stddef.h>

/* Polled xHCI host controller driver with USB HID keyboard, mouse and
 * tablet support. No hubs, mass storage or isochronous transfers yet. */
#define MINT_USB_INFO_VERSION 1u
#define MINT_USB_MAX_DEVICES 8u

enum { MINT_USB_STATE_NONE = 0, MINT_USB_STATE_RUNNING = 1, MINT_USB_STATE_FAILED = 2 };
enum { MINT_USB_KIND_UNKNOWN = 0, MINT_USB_KIND_KEYBOARD = 1, MINT_USB_KIND_MOUSE = 2,
       MINT_USB_KIND_TABLET = 3, MINT_USB_KIND_HUB = 4, MINT_USB_KIND_OTHER = 5 };

typedef struct mint_usb_device_info {
    uint8_t port, slot, speed, kind;      /* speed: 1 FS, 2 LS, 3 HS, 4 SS */
    uint16_t vendor_id, product_id;
    uint8_t device_class, interfaces, hid_interfaces, reserved;
    char product[32];
} MINT_USB_DEVICE_INFO;

typedef struct mint_usb_info {
    uint32_t version;
    uint32_t state;
    uint32_t controllers;                 /* xHCI functions found on PCI */
    uint32_t ports;
    uint16_t vendor_id, device_id;        /* active controller */
    uint32_t device_count;
    uint64_t reports;                     /* HID reports processed */
    uint64_t transfer_errors;
    char status[64];
    MINT_USB_DEVICE_INFO devices[MINT_USB_MAX_DEVICES];
} MINT_USB_INFO;

#ifndef MINT_USB_USERSPACE
/* Kernel task entry: initializes the first xHCI controller and then polls its
 * event ring forever (cooperatively yielding). Safe if no controller exists. */
void mint_usb_task(void *argument);
int mint_usb_get_info(MINT_USB_INFO *out);

/* Pure helpers exported for host tests. */
typedef struct mint_hid_field { uint16_t bit, size; int32_t min, max; uint8_t present, relative; } MINT_HID_FIELD;
typedef struct mint_hid_pointer_layout {
    uint8_t report_id, button_count;
    uint16_t button_bit;
    MINT_HID_FIELD x, y, wheel;
} MINT_HID_POINTER_LAYOUT;
int mint_hid_parse_pointer(const uint8_t *desc, size_t length, MINT_HID_POINTER_LAYOUT *out);
int32_t mint_hid_field_value(const uint8_t *report, size_t length, const MINT_HID_FIELD *field);
uint16_t mint_hid_usage_to_set1(uint8_t usage);   /* (E0 flag << 8) | make, 0 if unmapped */
uint8_t mint_xhci_interval_exponent(uint32_t speed, uint8_t b_interval);
#endif
#endif
