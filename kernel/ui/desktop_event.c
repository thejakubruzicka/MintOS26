#include <stdint.h>
#include <stddef.h>
#include "../include/desktop_abi.h"

static MINT_DESKTOP_EVENT_QUEUE kernel_event_queue;
static int kernel_event_queue_ready;

static void zero_bytes(void *ptr, size_t size) {
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < size; ++i) p[i] = 0;
}

void mint_desktop_event_queue_init(MINT_DESKTOP_EVENT_QUEUE *q) {
    if (!q) return;
    zero_bytes(q, sizeof(*q));
}

int mint_desktop_event_push(MINT_DESKTOP_EVENT_QUEUE *q, const MINT_DESKTOP_EVENT *event) {
    if (!q || !event || event->type == MINT_DESKTOP_EVENT_NONE ||
        event->type > MINT_DESKTOP_EVENT_QUIT) return 0;
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    if ((uint32_t)(head - tail) >= MINT_DESKTOP_EVENT_CAPACITY) {
        __atomic_add_fetch(&q->dropped_events, 1u, __ATOMIC_RELAXED);
        return 0;
    }
    q->events[head & MINT_DESKTOP_EVENT_MASK] = *event;
    __atomic_store_n(&q->head, head + 1u, __ATOMIC_RELEASE);
    return 1;
}

int mint_desktop_event_pop(MINT_DESKTOP_EVENT_QUEUE *q, MINT_DESKTOP_EVENT *out) {
    if (!q || !out) return 0;
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    if (tail == head) return 0;
    *out = q->events[tail & MINT_DESKTOP_EVENT_MASK];
    __atomic_store_n(&q->tail, tail + 1u, __ATOMIC_RELEASE);
    return 1;
}

uint32_t mint_desktop_event_count(const MINT_DESKTOP_EVENT_QUEUE *q) {
    if (!q) return 0;
    uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
    uint32_t count = head - tail;
    return count > MINT_DESKTOP_EVENT_CAPACITY ? MINT_DESKTOP_EVENT_CAPACITY : count;
}

uint64_t mint_desktop_event_dropped(const MINT_DESKTOP_EVENT_QUEUE *q) {
    return q ? __atomic_load_n(&q->dropped_events, __ATOMIC_RELAXED) : 0;
}

int mint_desktop_event_selftest(void) {
    MINT_DESKTOP_EVENT_QUEUE q;
    MINT_DESKTOP_EVENT event, out;
    mint_desktop_event_queue_init(&q);
    for (uint32_t i = 0; i < MINT_DESKTOP_EVENT_CAPACITY; ++i) {
        zero_bytes(&event, sizeof(event));
        event.type = MINT_DESKTOP_EVENT_KEY;
        event.data.key.codepoint = 0x41u + i;
        event.timestamp_ms = 1000u + i;
        if (!mint_desktop_event_push(&q, &event)) return 0;
    }
    event.type = MINT_DESKTOP_EVENT_QUIT;
    if (mint_desktop_event_push(&q, &event) || mint_desktop_event_dropped(&q) != 1u ||
        mint_desktop_event_count(&q) != MINT_DESKTOP_EVENT_CAPACITY) return 0;
    for (uint32_t i = 0; i < MINT_DESKTOP_EVENT_CAPACITY / 2u; ++i) {
        if (!mint_desktop_event_pop(&q, &out) || out.type != MINT_DESKTOP_EVENT_KEY ||
            out.data.key.codepoint != 0x41u + i || out.timestamp_ms != 1000u + i) return 0;
    }
    for (uint32_t i = 0; i < MINT_DESKTOP_EVENT_CAPACITY / 2u; ++i) {
        zero_bytes(&event, sizeof(event));
        event.type = MINT_DESKTOP_EVENT_POINTER_MOVE;
        event.data.pointer.x = (int32_t)i * 3;
        event.data.pointer.y = -(int32_t)i * 2;
        if (!mint_desktop_event_push(&q, &event)) return 0;
    }
    for (uint32_t i = MINT_DESKTOP_EVENT_CAPACITY / 2u; i < MINT_DESKTOP_EVENT_CAPACITY; ++i) {
        if (!mint_desktop_event_pop(&q, &out) || out.type != MINT_DESKTOP_EVENT_KEY ||
            out.data.key.codepoint != 0x41u + i) return 0;
    }
    for (uint32_t i = 0; i < MINT_DESKTOP_EVENT_CAPACITY / 2u; ++i) {
        if (!mint_desktop_event_pop(&q, &out) || out.type != MINT_DESKTOP_EVENT_POINTER_MOVE ||
            out.data.pointer.x != (int32_t)i * 3 || out.data.pointer.y != -(int32_t)i * 2) return 0;
    }
    if (mint_desktop_event_pop(&q, &out) || mint_desktop_event_count(&q) != 0 ||
        mint_desktop_event_dropped(&q) != 1u) return 0;
    return 1;
}


int mint_desktop_core_init(void) {
    kernel_event_queue_ready = 0;
    mint_desktop_event_queue_init(&kernel_event_queue);
    if (!mint_desktop_event_selftest()) return 0;
    kernel_event_queue_ready = 1;
    return 1;
}

int mint_desktop_event_post(const MINT_DESKTOP_EVENT *event) {
    if (!kernel_event_queue_ready) return 0;
    return mint_desktop_event_push(&kernel_event_queue, event);
}

int mint_desktop_event_next(MINT_DESKTOP_EVENT *event_out) {
    if (!kernel_event_queue_ready) return 0;
    return mint_desktop_event_pop(&kernel_event_queue, event_out);
}

uint64_t mint_desktop_event_overflow_count(void) {
    return mint_desktop_event_dropped(&kernel_event_queue);
}
