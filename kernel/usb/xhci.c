/* MintOS26 xHCI (USB 3) host controller driver — polled, HID only.
 *
 * Scope: one controller, devices on root ports (no hubs), control transfers on
 * EP0 and interrupt-IN endpoints for HID keyboards (boot protocol), mice and
 * tablets (report protocol, parsed report descriptor). Interrupts stay masked;
 * a kernel task polls the event ring. All DMA memory lives below 4 GiB and is
 * owned by this driver; every wait is bounded. */
#include <stdint.h>
#include <stddef.h>
#include "../include/usb.h"
#ifndef MINT_USB_HOST_TEST
#include "../include/pci.h"
#include "../include/physmem.h"
#include "../include/paging.h"
#include "../include/vmalloc.h"
#include "../include/serial.h"
#include "../include/log.h"
#include "../include/task.h"
#include "../include/arch.h"
#include "../include/input.h"
#endif

/* ===================================================================== */
/*  Pure helpers (host-tested)                                            */
/* ===================================================================== */

uint8_t mint_xhci_interval_exponent(uint32_t speed, uint8_t b_interval) {
    if (speed >= 3u) {                         /* HS/SS: 2^(bInterval-1) microframes */
        uint8_t n = b_interval ? (uint8_t)(b_interval - 1u) : 0u;
        return n > 15u ? 15u : n;
    }
    /* FS/LS: bInterval frames -> exponent of 125 us units, floor(log2(ms * 8)) */
    uint32_t units = (uint32_t)(b_interval ? b_interval : 1u) * 8u, n = 0;
    while ((1u << (n + 1u)) <= units && n < 10u) ++n;
    return (uint8_t)(n < 3u ? 3u : n);
}

uint16_t mint_hid_usage_to_set1(uint8_t u) {
    static const uint8_t letters[26] = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                        0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    static const uint8_t row[0x3A - 0x1E] = {0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B, /* 1..0 */
        0x1C,0x01,0x0E,0x0F,0x39,0x0C,0x0D,0x1A,0x1B,0x2B,0x2B,0x27,0x28,0x29,0x33,0x34,0x35,0x3A};
    if (u >= 0x04u && u <= 0x1Du) return letters[u - 0x04u];
    if (u >= 0x1Eu && u <= 0x39u) return row[u - 0x1Eu];
    if (u >= 0x3Au && u <= 0x43u) return (uint16_t)(0x3Bu + (u - 0x3Au));   /* F1..F10 */
    switch (u) {
    case 0x44: return 0x57; case 0x45: return 0x58;                       /* F11 F12 */
    case 0x46: return 0x137; case 0x47: return 0x46;
    case 0x49: return 0x152; case 0x4A: return 0x147; case 0x4B: return 0x149;
    case 0x4C: return 0x153; case 0x4D: return 0x14F; case 0x4E: return 0x151;
    case 0x4F: return 0x14D; case 0x50: return 0x14B; case 0x51: return 0x150; case 0x52: return 0x148;
    case 0x53: return 0x45; case 0x54: return 0x135; case 0x55: return 0x37; case 0x56: return 0x4A;
    case 0x57: return 0x4E; case 0x58: return 0x11C;
    case 0x59: return 0x4F; case 0x5A: return 0x50; case 0x5B: return 0x51; case 0x5C: return 0x4B;
    case 0x5D: return 0x4C; case 0x5E: return 0x4D; case 0x5F: return 0x47; case 0x60: return 0x48;
    case 0x61: return 0x49; case 0x62: return 0x52; case 0x63: return 0x53; case 0x64: return 0x56;
    case 0x65: return 0x15D;
    case 0x7F: return 0x120; case 0x80: return 0x130; case 0x81: return 0x12E;   /* Mute, Volume Up/Down (26.5.014.0) */
    default: return 0;
    }
}

int32_t mint_hid_field_value(const uint8_t *report, size_t length, const MINT_HID_FIELD *f) {
    if (!report || !f || !f->present || !f->size || f->size > 32u) return 0;
    uint32_t v = 0;
    for (uint32_t i = 0; i < f->size; ++i) {
        uint32_t bit = (uint32_t)f->bit + i;
        if (bit / 8u >= length) break;
        if (report[bit / 8u] & (1u << (bit % 8u))) v |= 1u << i;
    }
    if (f->min < 0 && f->size < 32u && (v & (1u << (f->size - 1u)))) v |= ~0u << f->size; /* sign-extend */
    return (int32_t)v;
}

static int32_t item_signed(uint32_t v, uint32_t bytes) {
    if (bytes == 1u) return (int8_t)v;
    if (bytes == 2u) return (int16_t)v;
    return (int32_t)v;
}

/* Minimal HID report-descriptor walk: finds X/Y (Generic Desktop 0x30/0x31),
 * the wheel (0x38) and the first button bit in the first report that has X/Y. */
int mint_hid_parse_pointer(const uint8_t *d, size_t len, MINT_HID_POINTER_LAYOUT *out) {
    if (!d || !out) return 0;
    MINT_HID_POINTER_LAYOUT cur = {0};
    uint32_t page = 0, rsize = 0, rcount = 0, usage_min = 0, usage_max = 0, nusage = 0;
    uint32_t usages[16];
    int32_t lmin = 0, lmax = 0;
    uint32_t offset = 0;
    int have_ids = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t prefix = d[i++];
        if (prefix == 0xFEu) {                       /* long item */
            if (i + 1u >= len) break;
            i += 2u + d[i];
            continue;
        }
        uint32_t bytes = prefix & 3u; if (bytes == 3u) bytes = 4u;
        if (i + bytes > len) break;
        uint32_t v = 0;
        for (uint32_t b = 0; b < bytes; ++b) v |= (uint32_t)d[i + b] << (8u * b);
        i += bytes;
        uint32_t type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1u) {                            /* global */
            switch (tag) {
            case 0: page = v; break;
            case 1: lmin = item_signed(v, bytes); break;
            case 2: lmax = bytes == 4u ? (int32_t)v : (lmin < 0 ? item_signed(v, bytes) : (int32_t)v); break;
            case 7: rsize = v; break;
            case 8:
                if (cur.x.present && cur.y.present) { *out = cur; return 1; }
                have_ids = 1; cur = (MINT_HID_POINTER_LAYOUT){0}; cur.report_id = (uint8_t)v; offset = 0; break;
            case 9: rcount = v; break;
            default: break;
            }
        } else if (type == 2u) {                     /* local */
            if (tag == 0 && nusage < 16u) usages[nusage++] = bytes == 4u ? (v & 0xFFFFu) : v;
            else if (tag == 1) usage_min = v;
            else if (tag == 2) usage_max = v;
        } else if (type == 0u) {                     /* main */
            if (tag == 8) {                          /* Input */
                int relative = (v & 4u) != 0, constant = (v & 1u) != 0;
                for (uint32_t k = 0; k < rcount && rsize; ++k) {
                    uint32_t usage = k < nusage ? usages[k] : (nusage ? usages[nusage - 1u] :
                                     (usage_min + k <= usage_max ? usage_min + k : 0u));
                    uint16_t bit = (uint16_t)(offset + k * rsize);
                    MINT_HID_FIELD f = {bit, (uint16_t)rsize, lmin, lmax, 1u, (uint8_t)relative};
                    if (constant) continue;
                    if (page == 0x01u && usage == 0x30u && !cur.x.present) cur.x = f;
                    else if (page == 0x01u && usage == 0x31u && !cur.y.present) cur.y = f;
                    else if (page == 0x01u && usage == 0x38u && !cur.wheel.present) cur.wheel = f;
                    else if (page == 0x09u) {
                        if (!cur.button_count) cur.button_bit = bit;
                        ++cur.button_count;
                    }
                }
                offset += rcount * rsize;
            }
            nusage = 0; usage_min = usage_max = 0;
        }
    }
    (void)have_ids;
    if (cur.x.present && cur.y.present) { *out = cur; return 1; }
    return 0;
}

#ifndef MINT_USB_HOST_TEST
/* ===================================================================== */
/*  Controller                                                            */
/* ===================================================================== */
#define XHCI_DMA_LIMIT 0x100000000ULL
#define RING_TRBS 256u
#define WAIT_TICKS 50u                 /* 500 ms command / transfer timeout */
#define MAX_HID_IFACES 3u

enum { TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
       TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12,
       TRB_EVALUATE_CTX = 13, TRB_NOOP_CMD = 23,
       TRB_EV_TRANSFER = 32, TRB_EV_COMMAND = 33, TRB_EV_PORT = 34 };
enum { CC_SUCCESS = 1, CC_SHORT_PACKET = 13 };
#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_PR  (1u << 4)
#define PORTSC_PP  (1u << 9)
#define PORTSC_CHANGE_BITS (0x7Fu << 17)
#define PORTSC_WPR (1u << 31)

typedef struct __attribute__((packed)) { uint64_t param; uint32_t status; uint32_t control; } TRB;
typedef struct { TRB *trb; uint64_t pa; uint32_t enq, cycle; } RING;

typedef struct {
    uint8_t kind, iface, dci, protocol;
    uint16_t mps, report_len;
    RING ring;
    MINT_HID_POINTER_LAYOUT layout;
    uint8_t last[8];
    uint8_t *buf; uint64_t buf_pa;
} HID_IFACE;

typedef struct {
    int used;
    uint32_t port, slot, speed;
    uint64_t out_pa, in_pa, page_pa;
    uint8_t *in;
    RING ep0;
    uint8_t *ctl; uint64_t ctl_pa;     /* control data scratch (512 bytes) */
    HID_IFACE hid[MAX_HID_IFACES];
    uint32_t hid_count;
    uint64_t pages[8]; uint32_t page_count;
    MINT_USB_DEVICE_INFO info;
} USB_DEV;

static volatile uint8_t *cap, *op, *rt, *dbs;
static uint32_t max_ports, ctx_size, usb3_first, usb3_count;
static uint64_t *dcbaa;
static RING cmd;
static TRB *evt; static uint64_t evt_pa; static uint32_t evt_deq, evt_cycle;
static USB_DEV devs[MINT_USB_MAX_DEVICES];
static MINT_USB_INFO usb_info;
static int controller_ready;
static uint8_t pci_bus, pci_dev, pci_fn;
/* completion slots filled by the event dispatcher */
static volatile int cmd_done; static uint64_t cmd_trb_pa; static uint32_t cmd_code, cmd_slot;
static volatile int xfer_done; static uint32_t xfer_slot, xfer_code, xfer_residual;
static uint32_t port_events;

static inline uint32_t r32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void w32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void w64(volatile uint8_t *b, uint32_t o, uint64_t v) { w32(b, o, (uint32_t)v); w32(b, o + 4u, (uint32_t)(v >> 32)); }
static void zero(void *p, size_t n) { uint8_t *q = (uint8_t *)p; for (size_t i = 0; i < n; ++i) q[i] = 0; }
static void copy_str(char *d, size_t cap_, const char *s) { size_t i = 0; while (s && s[i] && i + 1u < cap_) { d[i] = s[i]; ++i; } d[i] = 0; }
static void status_text(const char *s) { copy_str(usb_info.status, sizeof(usb_info.status), s); }

static uint64_t dma_page(void) {
    uint64_t pa = mint_physmem_claim_contiguous_below(1, XHCI_DMA_LIMIT);
    if (pa) zero((void *)(uintptr_t)pa, 4096u);
    return pa;
}
static void ring_init(RING *r, uint64_t pa) {
    r->pa = pa; r->trb = (TRB *)(uintptr_t)pa; r->enq = 0; r->cycle = 1;
    zero(r->trb, 4096u);
}
static TRB *ring_push(RING *r, uint64_t param, uint32_t status, uint32_t control) {
    TRB *t = &r->trb[r->enq];
    t->param = param; t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = (control & ~1u) | r->cycle;
    if (++r->enq == RING_TRBS - 1u) {                    /* last slot is the link TRB */
        TRB *l = &r->trb[r->enq];
        l->param = r->pa; l->status = 0;
        __asm__ volatile("" ::: "memory");
        l->control = ((uint32_t)TRB_LINK << 10) | (1u << 1) | r->cycle;  /* toggle cycle */
        r->enq = 0; r->cycle ^= 1u;
    }
    return t;
}
static uint64_t trb_pa(RING *r, TRB *t) { return r->pa + (uint64_t)((uint8_t *)t - (uint8_t *)r->trb); }
static void doorbell(uint32_t slot, uint32_t target) {
    __asm__ volatile("mfence" ::: "memory");
    w32(dbs, slot * 4u, target);
}

/* Each HID endpoint keeps HID_INFLIGHT transfers queued, each with its own
 * 64-byte buffer, so the controller keeps polling the device at its own
 * interval even while the cooperative usb task is not running. Completed
 * reports wait in the event ring instead of being overwritten on the device
 * (a single in-flight transfer lost fast key taps while MintDE was busy). */
#define HID_INFLIGHT 8u
#define HID_SLOT 64u
_Static_assert(MAX_HID_IFACES * HID_INFLIGHT * HID_SLOT <= 4096u, "HID report buffers share one DMA page");
static void handle_report(USB_DEV *d, HID_IFACE *h, const uint8_t *r, uint32_t got);
static void requeue(USB_DEV *d, HID_IFACE *h, uint64_t buffer_pa) {
    ring_push(&h->ring, buffer_pa, h->mps, ((uint32_t)TRB_NORMAL << 10) | (1u << 5) | (1u << 2)); /* IOC | ISP */
    doorbell(d->slot, h->dci);
}

static USB_DEV *dev_by_slot(uint32_t slot) {
    for (uint32_t i = 0; i < MINT_USB_MAX_DEVICES; ++i) if (devs[i].used && devs[i].slot == slot) return &devs[i];
    return 0;
}

/* Drain the event ring, dispatching completions and HID reports. */
static void poll_events(void) {
    for (uint32_t guard = 0; guard < RING_TRBS; ++guard) {
        volatile TRB *e = &evt[evt_deq];
        uint32_t control = e->control;
        if ((control & 1u) != evt_cycle) break;
        __asm__ volatile("lfence" ::: "memory");
        uint64_t param = e->param; uint32_t status = e->status;
        uint32_t type = (control >> 10) & 0x3Fu, code = status >> 24;
        if (type == TRB_EV_COMMAND) {
            cmd_trb_pa = param; cmd_code = code; cmd_slot = control >> 24; cmd_done = 1;
        } else if (type == TRB_EV_TRANSFER) {
            uint32_t slot = control >> 24, ep = (control >> 16) & 0x1Fu;
            USB_DEV *d = dev_by_slot(slot);
            if (d && ep == 1u) { xfer_slot = slot; xfer_code = code; xfer_residual = status & 0xFFFFFFu; xfer_done = 1; }
            else if (d) {
                for (uint32_t k = 0; k < d->hid_count; ++k) {
                    HID_IFACE *h = &d->hid[k];
                    if (h->dci != ep) continue;
                    /* The event names the completed TRB; that TRB names its buffer. */
                    if (param < h->ring.pa || param >= h->ring.pa + 4096u || (param & 15u)) { ++usb_info.transfer_errors; break; }
                    uint64_t buffer_pa = ((volatile TRB *)(uintptr_t)param)->param;
                    if (buffer_pa < h->buf_pa || buffer_pa >= h->buf_pa + HID_INFLIGHT * HID_SLOT || ((buffer_pa - h->buf_pa) % HID_SLOT)) {
                        ++usb_info.transfer_errors; break;
                    }
                    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
                        uint32_t residual = status & 0xFFFFFFu;
                        handle_report(d, h, (const uint8_t *)(uintptr_t)buffer_pa, h->mps > residual ? h->mps - residual : 0u);
                    } else ++usb_info.transfer_errors;
                    requeue(d, h, buffer_pa);
                }
            }
        } else if (type == TRB_EV_PORT) {
            ++port_events;
        }
        if (++evt_deq == RING_TRBS) { evt_deq = 0; evt_cycle ^= 1u; }
        w64(rt, 0x20u + 0x18u, (evt_pa + (uint64_t)evt_deq * 16u) | (1u << 3));
    }
}

static int wait_flag(volatile int *flag) {
    uint64_t start = mint_timer_ticks();
    while (!*flag) {
        poll_events();
        if (*flag) break;
        if (mint_timer_ticks() - start > WAIT_TICKS) return 0;
        mint_task_yield();
    }
    return 1;
}

static int command(uint64_t param, uint32_t status, uint32_t control, uint32_t *slot_out) {
    cmd_done = 0;
    TRB *t = ring_push(&cmd, param, status, control);
    uint64_t pa = trb_pa(&cmd, t);
    doorbell(0, 0);
    for (int tries = 0; tries < 4; ++tries) {
        if (!wait_flag(&cmd_done)) return 0;
        if (cmd_trb_pa == pa) break;
        cmd_done = 0;                                /* completion for an older command */
    }
    if (cmd_trb_pa != pa || cmd_code != CC_SUCCESS) return 0;
    if (slot_out) *slot_out = cmd_slot;
    return 1;
}

static uint8_t *ctx(uint8_t *base, uint32_t index) { return base + (uint64_t)index * ctx_size; }

static int control(USB_DEV *d, uint8_t rt_, uint8_t req, uint16_t value, uint16_t index, uint16_t length) {
    if (length > 512u) return 0;
    int in = (rt_ & 0x80u) != 0;
    uint64_t setup = (uint64_t)rt_ | ((uint64_t)req << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) | ((uint64_t)length << 48);
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    xfer_done = 0;
    ring_push(&d->ep0, setup, 8u, ((uint32_t)TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (length) ring_push(&d->ep0, d->ctl_pa, length, ((uint32_t)TRB_DATA << 10) | (in ? (1u << 16) : 0u));
    ring_push(&d->ep0, 0, 0, ((uint32_t)TRB_STATUS << 10) | (1u << 5) | ((length && in) ? 0u : (1u << 16)));
    doorbell(d->slot, 1);
    if (!wait_flag(&xfer_done)) return 0;
    return xfer_code == CC_SUCCESS || xfer_code == CC_SHORT_PACKET;
}

static void release_device(USB_DEV *d, int controller_safe) {
    if (d->slot) {
        uint32_t ignored;
        if (command(0, 0, ((uint32_t)TRB_DISABLE_SLOT << 10) | (d->slot << 24), &ignored)) controller_safe = 1;
        dcbaa[d->slot] = 0;
    }
    /* A bounded leak beats freeing pages the controller might still write. */
    if (controller_safe)
        for (uint32_t i = 0; i < d->page_count; ++i) (void)mint_physmem_release_contiguous(d->pages[i], 1);
    zero(d, sizeof(*d));
}

static uint64_t dev_page(USB_DEV *d) {
    if (d->page_count >= 8u) return 0;
    uint64_t pa = dma_page();
    if (pa) d->pages[d->page_count++] = pa;
    return pa;
}

static uint32_t speed_mps0(uint32_t speed) { return speed == 2u ? 8u : speed == 1u ? 8u : speed == 3u ? 64u : 512u; }

static void handle_report(USB_DEV *d, HID_IFACE *h, const uint8_t *r, uint32_t got) {
    (void)d;
    ++usb_info.reports;
    if (h->kind == MINT_USB_KIND_KEYBOARD) {
        if (got < 3u) return;
        static const uint16_t mods[8] = {0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C};
        uint8_t changed = (uint8_t)(r[0] ^ h->last[0]);
        for (int b = 0; b < 8; ++b)
            if (changed & (1u << b)) mint_input_inject_key((uint8_t)mods[b], mods[b] >> 8, (r[0] >> b) & 1u);
        if (r[2] == 1u) return;                                    /* rollover error */
        for (int k = 2; k < 8 && (uint32_t)k < got; ++k) {         /* releases */
            uint8_t u = h->last[k]; if (u < 4u) continue;
            int still = 0; for (int j = 2; j < 8 && (uint32_t)j < got; ++j) if (r[j] == u) still = 1;
            uint16_t sc = mint_hid_usage_to_set1(u);
            if (!still && sc) mint_input_inject_key((uint8_t)sc, sc >> 8, 0);
        }
        for (int k = 2; k < 8 && (uint32_t)k < got; ++k) {         /* presses */
            uint8_t u = r[k]; if (u < 4u) continue;
            int was = 0; for (int j = 2; j < 8; ++j) if (h->last[j] == u) was = 1;
            uint16_t sc = mint_hid_usage_to_set1(u);
            if (!was && sc) mint_input_inject_key((uint8_t)sc, sc >> 8, 1);
        }
        for (int k = 0; k < 8; ++k) h->last[k] = (uint32_t)k < got ? r[k] : 0;
        return;
    }
    /* Pointer (report protocol): optional report ID byte, then parsed fields. */
    const MINT_HID_POINTER_LAYOUT *L = &h->layout;
    const uint8_t *p = r; uint32_t n = got;
    if (L->report_id) { if (!n || p[0] != L->report_id) return; ++p; --n; }
    uint32_t buttons = 0;
    for (uint32_t b = 0; b < L->button_count && b < 3u; ++b) {
        uint32_t bit = L->button_bit + b;
        if (bit / 8u < n && (p[bit / 8u] & (1u << (bit % 8u)))) buttons |= 1u << b;
    }
    int32_t x = mint_hid_field_value(p, n, &L->x), y = mint_hid_field_value(p, n, &L->y);
    if (L->x.relative) mint_input_inject_relative(x, y, buttons);
    else mint_input_inject_absolute((uint32_t)(x - L->x.min), (uint32_t)(L->x.max - L->x.min),
                                    (uint32_t)(y - L->y.min), (uint32_t)(L->y.max - L->y.min), buttons);
    if (L->wheel.present) { int32_t w = mint_hid_field_value(p, n, &L->wheel); if (w) mint_input_inject_wheel(-w); }
}

static void log_dev(const char *what, USB_DEV *d) {
    mint_serial_write("[usb] port "); mint_serial_dec64(d->port);
    mint_serial_write(" slot "); mint_serial_dec64(d->slot);
    mint_serial_write(": "); mint_serial_write(what); mint_serial_write("\n");
}

static void read_string(USB_DEV *d, uint8_t index, char *out, size_t cap_) {
    out[0] = 0;
    if (!index || !control(d, 0x80, 6, (uint16_t)(0x0300u | index), 0x0409, 64)) return;
    uint32_t len = d->ctl[0]; size_t o = 0;
    for (uint32_t i = 2; i + 1u < len && i < 64u && o + 1u < cap_; i += 2) {
        uint16_t c = (uint16_t)(d->ctl[i] | (d->ctl[i + 1] << 8));
        out[o++] = (c >= 32u && c < 127u) ? (char)c : '?';
    }
    out[o] = 0;
}

static int configure_hid(USB_DEV *d, uint8_t config_value) {
    /* Configure Endpoint: add every HID interrupt-IN endpoint in one command. */
    zero(d->in, 4096u);
    uint32_t *icc = (uint32_t *)ctx(d->in, 0);
    uint32_t max_dci = 1;
    icc[1] = 1u;                                             /* A0: slot context */
    for (uint32_t k = 0; k < d->hid_count; ++k) {
        HID_IFACE *h = &d->hid[k];
        icc[1] |= 1u << h->dci;
        if (h->dci > max_dci) max_dci = h->dci;
    }
    uint32_t *out_slot = (uint32_t *)ctx((uint8_t *)(uintptr_t)d->out_pa, 0);
    uint32_t *slot = (uint32_t *)ctx(d->in, 1);
    for (int i = 0; i < 4; ++i) slot[i] = out_slot[i];
    slot[0] = (slot[0] & ~(0x1Fu << 27)) | (max_dci << 27);
    for (uint32_t k = 0; k < d->hid_count; ++k) {
        HID_IFACE *h = &d->hid[k];
        uint32_t *ep = (uint32_t *)ctx(d->in, 1u + h->dci);
        ep[0] = (uint32_t)h->protocol << 16;                 /* interval exponent stored in protocol */
        ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)h->mps << 16);   /* CErr 3, Interrupt IN */
        ep[2] = (uint32_t)h->ring.pa | 1u; ep[3] = (uint32_t)(h->ring.pa >> 32);
        ep[4] = h->mps | ((uint32_t)h->mps << 16);
    }
    if (!command(d->in_pa, 0, ((uint32_t)TRB_CONFIGURE_EP << 10) | (d->slot << 24), 0)) return 0;
    return control(d, 0x00, 9, config_value, 0, 0);          /* SET_CONFIGURATION */
}

/* Address a newly connected root-port device and bring up its HID interfaces. */
static void enumerate_port(uint32_t port, uint32_t speed) {
    USB_DEV *d = 0;
    for (uint32_t i = 0; i < MINT_USB_MAX_DEVICES; ++i) if (!devs[i].used) { d = &devs[i]; break; }
    if (!d) return;
    zero(d, sizeof(*d));
    d->used = 1; d->port = port; d->speed = speed;
    uint32_t slot = 0;
    if (!command(0, 0, (uint32_t)TRB_ENABLE_SLOT << 10, &slot) || !slot || slot > 255u) { d->used = 0; return; }
    d->slot = slot;
    d->out_pa = dev_page(d); d->in_pa = dev_page(d);
    uint64_t ep0 = dev_page(d); d->ctl_pa = dev_page(d);
    if (!d->out_pa || !d->in_pa || !ep0 || !d->ctl_pa) { release_device(d, 0); return; }
    d->in = (uint8_t *)(uintptr_t)d->in_pa; d->ctl = (uint8_t *)(uintptr_t)d->ctl_pa;
    ring_init(&d->ep0, ep0);
    dcbaa[slot] = d->out_pa;
    uint32_t *icc = (uint32_t *)ctx(d->in, 0);
    icc[1] = 3u;                                             /* A0 slot, A1 EP0 */
    uint32_t *sc = (uint32_t *)ctx(d->in, 1);
    sc[0] = (speed << 20) | (1u << 27);
    sc[1] = port << 16;
    uint32_t *e0 = (uint32_t *)ctx(d->in, 2);
    e0[1] = (3u << 1) | (4u << 3) | (speed_mps0(speed) << 16);
    e0[2] = (uint32_t)d->ep0.pa | 1u; e0[3] = (uint32_t)(d->ep0.pa >> 32);
    e0[4] = 8u;
    if (!command(d->in_pa, 0, ((uint32_t)TRB_ADDRESS_DEVICE << 10) | (slot << 24), 0)) {
        log_dev("Address Device failed", d); release_device(d, 0); return;
    }
    if (!control(d, 0x80, 6, 0x0100, 0, 8)) { log_dev("device descriptor read failed", d); release_device(d, 1); return; }
    uint32_t mps0 = d->ctl[7];
    if (speed <= 2u && mps0 && mps0 != speed_mps0(speed)) {  /* full-speed EP0 size fix-up */
        zero(d->in, 4096u);
        ((uint32_t *)ctx(d->in, 0))[1] = 2u;
        uint32_t *e = (uint32_t *)ctx(d->in, 2);
        e[1] = (3u << 1) | (4u << 3) | (mps0 << 16);
        e[2] = (uint32_t)d->ep0.pa | 1u; e[3] = (uint32_t)(d->ep0.pa >> 32); e[4] = 8u;
        (void)command(d->in_pa, 0, ((uint32_t)TRB_EVALUATE_CTX << 10) | (slot << 24), 0);
    }
    if (!control(d, 0x80, 6, 0x0100, 0, 18)) { release_device(d, 1); return; }
    d->info.port = (uint8_t)port; d->info.slot = (uint8_t)slot; d->info.speed = (uint8_t)speed;
    d->info.vendor_id = (uint16_t)(d->ctl[8] | (d->ctl[9] << 8));
    d->info.product_id = (uint16_t)(d->ctl[10] | (d->ctl[11] << 8));
    d->info.device_class = d->ctl[4];
    uint8_t iproduct = d->ctl[15];
    if (d->ctl[4] == 9u) { d->info.kind = MINT_USB_KIND_HUB; log_dev("USB hub (hubs are not supported yet)", d); return; }
    if (!control(d, 0x80, 6, 0x0200, 0, 9)) { release_device(d, 1); return; }
    uint32_t total = (uint32_t)(d->ctl[2] | (d->ctl[3] << 8));
    uint8_t config_value = d->ctl[5];
    if (total > 512u) total = 512u;
    if (!control(d, 0x80, 6, 0x0200, 0, (uint16_t)total)) { release_device(d, 1); return; }
    /* Walk interfaces/endpoints. The control buffer is reused below, so copy. */
    uint8_t cfg[512]; for (uint32_t i = 0; i < total; ++i) cfg[i] = d->ctl[i];
    HID_IFACE *cur = 0; uint16_t cur_report_len = 0;
    for (uint32_t i = 0; i + 2u <= total && cfg[i] >= 2u; i += cfg[i]) {
        uint8_t len = cfg[i], type = cfg[i + 1];
        if (i + len > total) break;
        if (type == 4u && len >= 9u) {                       /* interface */
            ++d->info.interfaces; cur = 0;
            if (cfg[i + 5] == 3u && d->hid_count < MAX_HID_IFACES) {
                cur = &d->hid[d->hid_count];
                zero(cur, sizeof(*cur));
                cur->iface = cfg[i + 2];
                cur->kind = cfg[i + 6] == 1u && cfg[i + 7] == 1u ? MINT_USB_KIND_KEYBOARD : MINT_USB_KIND_MOUSE;
            }
        } else if (type == 0x21u && len >= 9u && cur) {      /* HID descriptor */
            cur_report_len = (uint16_t)(cfg[i + 7] | (cfg[i + 8] << 8));
            cur->report_len = cur_report_len;
        } else if (type == 5u && len >= 7u && cur && !cur->dci) {
            uint8_t addr = cfg[i + 2], attr = cfg[i + 3];
            if ((addr & 0x80u) && (attr & 3u) == 3u) {
                cur->dci = (uint8_t)((addr & 0x0Fu) * 2u + 1u);
                cur->mps = (uint16_t)((cfg[i + 4] | (cfg[i + 5] << 8)) & 0x7FFu);
                if (cur->mps > 64u) cur->mps = 64u;
                cur->protocol = mint_xhci_interval_exponent(speed, cfg[i + 6]);
                ++d->hid_count; cur = 0;
            }
        }
    }
    read_string(d, iproduct, d->info.product, sizeof(d->info.product));
    if (!d->hid_count) { d->info.kind = MINT_USB_KIND_OTHER; log_dev("non-HID device left unconfigured", d); return; }
    for (uint32_t k = 0; k < d->hid_count; ++k) {
        HID_IFACE *h = &d->hid[k];
        uint64_t rp = dev_page(d), bp = (k == 0) ? dev_page(d) : d->hid[0].buf_pa + HID_INFLIGHT * HID_SLOT * k;
        if (!rp || !bp) { release_device(d, 1); return; }
        ring_init(&h->ring, rp);
        h->buf_pa = bp; h->buf = (uint8_t *)(uintptr_t)bp;
    }
    if (!configure_hid(d, config_value)) { log_dev("Configure Endpoint / SET_CONFIGURATION failed", d); release_device(d, 1); return; }
    int keyboard = 0, pointer = 0;
    for (uint32_t k = 0; k < d->hid_count; ++k) {
        HID_IFACE *h = &d->hid[k];
        if (h->kind == MINT_USB_KIND_KEYBOARD) {
            (void)control(d, 0x21, 0x0B, 0, h->iface, 0);    /* SET_PROTOCOL boot */
            (void)control(d, 0x21, 0x0A, 0, h->iface, 0);    /* SET_IDLE 0 */
            keyboard = 1;
        } else {
            uint16_t rl = h->report_len ? h->report_len : 256u; if (rl > 512u) rl = 512u;
            if (control(d, 0x81, 6, 0x2200, h->iface, rl) &&
                mint_hid_parse_pointer(d->ctl, rl, &h->layout)) {
                h->kind = h->layout.x.relative ? MINT_USB_KIND_MOUSE : MINT_USB_KIND_TABLET;
                pointer = 1;
            } else {
                /* Fall back to the boot mouse layout: buttons, dx, dy. */
                (void)control(d, 0x21, 0x0B, 0, h->iface, 0);
                h->layout = (MINT_HID_POINTER_LAYOUT){0, 3, 0, {8, 8, -127, 127, 1, 1}, {16, 8, -127, 127, 1, 1}, {24, 8, -127, 127, 0, 1}};
                pointer = 1;
            }
        }
        ++d->info.hid_interfaces;
        for (uint32_t t = 0; t < HID_INFLIGHT; ++t) requeue(d, h, h->buf_pa + (uint64_t)t * HID_SLOT);
    }
    d->info.kind = keyboard ? MINT_USB_KIND_KEYBOARD : d->hid[0].kind;
    mint_input_set_usb_devices(keyboard || mint_input_usb_keyboard(), pointer || mint_input_usb_pointer());
    log_dev(keyboard ? "HID keyboard ready" : d->info.kind == MINT_USB_KIND_TABLET ? "HID tablet ready" : "HID mouse ready", d);
}

static uint32_t portsc_offset(uint32_t port) { return 0x400u + 0x10u * (port - 1u); }

static int port_is_usb3(uint32_t port) { return usb3_count && port >= usb3_first && port < usb3_first + usb3_count; }

static int reset_port(uint32_t port, uint32_t *speed_out) {
    uint32_t off = portsc_offset(port);
    uint32_t v = r32(op, off);
    if (!(v & PORTSC_CCS)) return 0;
    if (!(v & PORTSC_PED) || !port_is_usb3(port)) {
        uint32_t keep = v & (PORTSC_PP | (3u << 14) | (7u << 25));
        w32(op, off, keep | (port_is_usb3(port) ? PORTSC_WPR : PORTSC_PR));
        uint64_t start = mint_timer_ticks();
        while (mint_timer_ticks() - start < 30u) {
            v = r32(op, off);
            if ((v & PORTSC_PED) && !(v & PORTSC_PR)) break;
            mint_task_sleep_ticks(1);
        }
    }
    v = r32(op, off);
    w32(op, off, (v & (PORTSC_PP | (3u << 14) | (7u << 25))) | (v & PORTSC_CHANGE_BITS)); /* clear changes */
    if (!(v & PORTSC_PED)) return 0;
    *speed_out = (v >> 10) & 0xFu;
    return *speed_out != 0u;
}

static void scan_ports(void) {
    for (uint32_t port = 1; port <= max_ports; ++port) {
        int known = 0;
        for (uint32_t i = 0; i < MINT_USB_MAX_DEVICES; ++i) if (devs[i].used && devs[i].port == port) known = 1;
        uint32_t v = r32(op, portsc_offset(port));
        if (known && !(v & PORTSC_CCS)) {
            for (uint32_t i = 0; i < MINT_USB_MAX_DEVICES; ++i)
                if (devs[i].used && devs[i].port == port) { log_dev("disconnected", &devs[i]); release_device(&devs[i], 0); }
            w32(op, portsc_offset(port), (v & (PORTSC_PP | (3u << 14) | (7u << 25))) | (v & PORTSC_CHANGE_BITS));
            continue;
        }
        if (known || !(v & PORTSC_CCS)) continue;
        uint32_t speed = 0;
        if (reset_port(port, &speed)) enumerate_port(port, speed);
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < MINT_USB_MAX_DEVICES; ++i) if (devs[i].used) ++n;
    usb_info.device_count = n;
}

static int bios_handoff(volatile uint8_t *base, uint32_t xecp_dwords) {
    uint32_t off = xecp_dwords * 4u;
    for (int guard = 0; off && guard < 64; ++guard) {
        uint32_t v = r32(base, off);
        uint32_t id = v & 0xFFu, next = (v >> 8) & 0xFFu;
        if (id == 1u) {                                      /* USB Legacy Support */
            if (v & (1u << 16)) {
                w32(base, off, v | (1u << 24));              /* OS owned */
                uint64_t start = mint_timer_ticks();
                while ((r32(base, off) & (1u << 16)) && mint_timer_ticks() - start < 100u) mint_task_sleep_ticks(1);
                if (r32(base, off) & (1u << 16)) {
                    mint_serial_write("[usb] BIOS kept xHCI ownership; forcing OS ownership\n");
                    w32(base, off, (r32(base, off) & ~(1u << 16)) | (1u << 24));
                }
            }
            uint32_t ctl = r32(base, off + 4u);
            w32(base, off + 4u, (ctl & 0xFFFF1F00u) | 0xE0000000u); /* disable SMIs, clear W1C status */
        } else if (id == 2u) {                               /* Supported Protocol */
            uint32_t major = v >> 24, ports = r32(base, off + 8u);
            if (major == 3u) { usb3_first = ports & 0xFFu; usb3_count = (ports >> 8) & 0xFFu; }
        }
        if (!next) break;
        off += next * 4u;
    }
    return 1;
}

static int start_controller(const MINT_PCI_DEVICE *pd) {
    const MINT_PCI_BAR *bar = &pd->bars[0];
    uint64_t base = bar->address;
    mint_serial_write("[usb] xHCI at PCI "); mint_serial_dec64(pd->bus); mint_serial_write(":");
    mint_serial_dec64(pd->device); mint_serial_write("."); mint_serial_dec64(pd->function);
    mint_serial_write(" BAR0 kind="); mint_serial_dec64(bar->kind); mint_serial_write(" base="); mint_serial_hex64(base);
    mint_serial_write("\n");
    void *mapped = 0;
    if ((bar->kind != MINT_PCI_BAR_MEMORY32 && bar->kind != MINT_PCI_BAR_MEMORY64) || !base ||
        !(mapped = mint_mmio_map(base, 0x10000u))) {
        status_text("xHCI BAR0 is not a usable MMIO range"); return 0;
    }
    pci_bus = pd->bus; pci_dev = pd->device; pci_fn = pd->function;
    uint32_t command_reg = mint_pci_read_config32(pci_bus, pci_dev, pci_fn, 0x04u) & 0xFFFFu;
    (void)mint_pci_write_config32(pci_bus, pci_dev, pci_fn, 0x04u, command_reg | 0x0002u); /* memory, no DMA yet */
    cap = (volatile uint8_t *)mapped;
    uint32_t caplen = cap[0], hcs1 = r32(cap, 4), hcs2 = r32(cap, 8), hcc1 = r32(cap, 0x10);
    if (r32(cap, 0) == 0xFFFFFFFFu || !caplen) { status_text("xHCI registers not responding"); return 0; }
    op = cap + caplen; rt = cap + (r32(cap, 0x18) & ~0x1Fu); dbs = cap + (r32(cap, 0x14) & ~3u);
    max_ports = hcs1 >> 24; ctx_size = (hcc1 & 4u) ? 64u : 32u;
    uint32_t max_slots = hcs1 & 0xFFu; if (max_slots > 32u) max_slots = 32u;
    (void)bios_handoff(cap, hcc1 >> 16);
    /* halt then reset */
    w32(op, 0, r32(op, 0) & ~1u);
    uint64_t t0 = mint_timer_ticks();
    while (!(r32(op, 4) & 1u) && mint_timer_ticks() - t0 < 20u) mint_task_sleep_ticks(1);
    w32(op, 0, 2u);                                          /* USBCMD.HCRST */
    t0 = mint_timer_ticks();
    while (((r32(op, 0) & 2u) || (r32(op, 4) & (1u << 11))) && mint_timer_ticks() - t0 < 100u) mint_task_sleep_ticks(1);
    if ((r32(op, 0) & 2u) || (r32(op, 4) & (1u << 11))) { status_text("xHCI reset timed out"); return 0; }
    w32(op, 0x38, max_slots);
    uint64_t dcbaa_pa = dma_page(), cmd_pa = dma_page(); evt_pa = dma_page();
    uint64_t erst_pa = dma_page();
    if (!dcbaa_pa || !cmd_pa || !evt_pa || !erst_pa) { status_text("no DMA memory below 4 GiB"); return 0; }
    dcbaa = (uint64_t *)(uintptr_t)dcbaa_pa;
    uint32_t scratch = ((hcs2 >> 27) & 0x1Fu) | (((hcs2 >> 21) & 0x1Fu) << 5);
    if (scratch) {
        uint64_t arr_pa = dma_page();
        if (!arr_pa || scratch > 512u) { status_text("scratchpad allocation failed"); return 0; }
        uint64_t *arr = (uint64_t *)(uintptr_t)arr_pa;
        for (uint32_t i = 0; i < scratch; ++i) { arr[i] = dma_page(); if (!arr[i]) { status_text("scratchpad pages unavailable"); return 0; } }
        dcbaa[0] = arr_pa;
    }
    ring_init(&cmd, cmd_pa);
    evt = (TRB *)(uintptr_t)evt_pa; evt_deq = 0; evt_cycle = 1;
    uint64_t *erst = (uint64_t *)(uintptr_t)erst_pa;
    erst[0] = evt_pa; erst[1] = RING_TRBS;
    w64(op, 0x30, dcbaa_pa);
    w64(op, 0x18, cmd_pa | 1u);
    w32(rt, 0x20u + 0x08u, 1u);                              /* ERSTSZ */
    w64(rt, 0x20u + 0x18u, evt_pa);                          /* ERDP */
    w64(rt, 0x20u + 0x10u, erst_pa);                         /* ERSTBA */
    w32(rt, 0x20u, 0u);                                      /* IMAN: interrupts off (polled) */
    /* DMA structures are owned and initialized: only now allow bus mastering. */
    (void)mint_pci_write_config32(pci_bus, pci_dev, pci_fn, 0x04u, command_reg | 0x0006u);
    w32(op, 0, 1u);                                          /* Run, INTE off */
    t0 = mint_timer_ticks();
    while ((r32(op, 4) & 1u) && mint_timer_ticks() - t0 < 20u) mint_task_sleep_ticks(1);
    if (r32(op, 4) & 1u) { status_text("xHCI did not start"); return 0; }
    if (!command(0, 0, (uint32_t)TRB_NOOP_CMD << 10, 0)) { status_text("xHCI command ring not responding"); return 0; }
    usb_info.ports = max_ports; usb_info.vendor_id = pd->vendor_id; usb_info.device_id = pd->device_id;
    return 1;
}

void mint_usb_task(void *argument) {
    (void)argument;
    zero(&usb_info, sizeof(usb_info));
    usb_info.version = MINT_USB_INFO_VERSION;
    const MINT_PCI_INVENTORY *inv = mint_pci_get_inventory();
    const MINT_PCI_DEVICE *found = 0;
    for (uint32_t i = 0; inv && i < inv->devices_stored && i < MINT_PCI_MAX_DEVICES; ++i) {
        const MINT_PCI_DEVICE *d = &inv->devices[i];
        if (d->class_code == 0x0Cu && d->subclass == 0x03u && d->programming_interface == 0x30u) {
            ++usb_info.controllers;
            if (!found) found = d;
        }
    }
    if (!found) {
        status_text("No xHCI (USB 3) controller found");
        mint_serial_write("[usb] no xHCI controller; USB input unavailable\n");
        return;
    }
    if (!start_controller(found)) {
        usb_info.state = MINT_USB_STATE_FAILED;
        mint_serial_write("[usb] xHCI start failed: "); mint_serial_write(usb_info.status); mint_serial_write("\n");
        mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_USB, usb_info.status);
        return;
    }
    controller_ready = 1;
    usb_info.state = MINT_USB_STATE_RUNNING;
    status_text("Running (polled, HID keyboards, mice and tablets)");
    mint_serial_write("[usb] xHCI running: ports="); mint_serial_dec64(max_ports);
    mint_serial_write(" ctx="); mint_serial_dec64(ctx_size);
    mint_serial_write(" usb3-ports="); mint_serial_dec64(usb3_first); mint_serial_write("+"); mint_serial_dec64(usb3_count);
    mint_serial_write("\n");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_USB, "xHCI controller running; scanning root ports");
    mint_task_sleep_ticks(10);                               /* let ports report connections */
    scan_ports();
    uint32_t seen_port_events = port_events;
    for (;;) {
        poll_events();
        if (port_events != seen_port_events) { seen_port_events = port_events; mint_task_sleep_ticks(5); scan_ports(); }
        mint_task_sleep_ticks(1);
    }
}

int mint_usb_get_info(MINT_USB_INFO *out) {
    if (!out) return 0;
    *out = usb_info;
    uint32_t n = 0;
    for (uint32_t i = 0; i < MINT_USB_MAX_DEVICES; ++i) if (devs[i].used) out->devices[n++] = devs[i].info;
    out->device_count = n;
    return 1;
}
#endif
