#include <stdint.h>
#include <stddef.h>
#include "../include/log.h"
#include "../include/serial.h"
#include "../include/task.h"
#include "../include/arch.h"

static MINT_LOG_RECORD records[MINT_LOG_RING_CAPACITY];
static uint64_t next_sequence;
static uint64_t dropped_records;
static uint32_t record_count;

static const char *severity_name(uint32_t s) {
    switch (s) {
    case MINT_LOG_DEBUG: return "DEBUG";
    case MINT_LOG_INFO: return "INFO";
    case MINT_LOG_WARN: return "WARN";
    default: return "ERROR";
    }
}
static const char *component_name(uint32_t c) {
    switch (c) {
    case MINT_LOG_COMPONENT_PCI: return "PCI";
    case MINT_LOG_COMPONENT_AHCI: return "AHCI";
    case MINT_LOG_COMPONENT_BLOCK: return "BLOCK";
    case MINT_LOG_COMPONENT_PARTITION: return "PARTITION";
    case MINT_LOG_COMPONENT_FAT16: return "FAT16";
    case MINT_LOG_COMPONENT_VFS: return "VFS";
    case MINT_LOG_COMPONENT_E1000: return "E1000";
    case MINT_LOG_COMPONENT_USB: return "USB";
    case MINT_LOG_COMPONENT_NET: return "NET";
    case MINT_LOG_COMPONENT_MINTFS: return "MINTFS";
    case MINT_LOG_COMPONENT_PROC: return "PROC";
    default: return "KERNEL";
    }
}
static void copy_message(char out[MINT_LOG_MESSAGE_SIZE], const char *in) {
    uint32_t i = 0;
    if (in) while (in[i] && i + 1u < MINT_LOG_MESSAGE_SIZE) { out[i] = in[i]; ++i; }
    out[i] = '\0';
}
static void log_dec(uint64_t v) { mint_serial_dec64(v); }

void mint_log_init(void) {
    uint8_t *p = (uint8_t *)records;
    for (size_t i = 0; i < sizeof(records); ++i) p[i] = 0;
    next_sequence = 1u;
    dropped_records = 0u;
    record_count = 0u;
}

void mint_log_emit(uint32_t severity, uint32_t component, const char *message) {
    if (severity > MINT_LOG_ERROR) severity = MINT_LOG_ERROR;
    uint64_t seq = next_sequence++;
    if (!next_sequence) next_sequence = 1u;
    uint32_t index = (uint32_t)((seq - 1u) % MINT_LOG_RING_CAPACITY);
    MINT_LOG_RECORD *r = &records[index];
    if (record_count == MINT_LOG_RING_CAPACITY) ++dropped_records;
    else ++record_count;
    r->sequence = seq;
    r->ticks = mint_timer_ticks();
    r->severity = severity;
    r->component = component;
    copy_message(r->message, message);

    mint_serial_write("[log]["); mint_serial_write(severity_name(severity));
    mint_serial_write("]["); mint_serial_write(component_name(component));
    mint_serial_write("][#"); log_dec(seq); mint_serial_write("] ");
    mint_serial_write(r->message); mint_serial_write("\n");
}

uint32_t mint_log_read_recent(MINT_LOG_RECORD *out, uint32_t capacity,
                              uint32_t minimum_severity, uint32_t component_filter) {
    if (!out || !capacity) return 0u;
    if (capacity > MINT_LOG_READ_MAX) capacity = MINT_LOG_READ_MAX;
    uint64_t oldest = next_sequence > record_count ? next_sequence - record_count : 1u;
    uint32_t matches = 0u;
    for (uint64_t seq = oldest; seq < next_sequence; ++seq) {
        const MINT_LOG_RECORD *r = &records[(uint32_t)((seq - 1u) % MINT_LOG_RING_CAPACITY)];
        if (r->sequence != seq || r->severity < minimum_severity ||
            (component_filter && r->component != component_filter)) continue;
        ++matches;
    }
    uint32_t skip = matches > capacity ? matches - capacity : 0u;
    uint32_t written = 0u, matching_seen = 0u;
    for (uint64_t seq = oldest; seq < next_sequence; ++seq) {
        const MINT_LOG_RECORD *r = &records[(uint32_t)((seq - 1u) % MINT_LOG_RING_CAPACITY)];
        if (r->sequence != seq || r->severity < minimum_severity ||
            (component_filter && r->component != component_filter)) continue;
        if (matching_seen++ < skip) continue;
        out[written++] = *r;
    }
    return written;
}

uint64_t mint_log_total_dropped(void) { return dropped_records; }
uint64_t mint_log_newest_sequence(void) { return next_sequence > 1u ? next_sequence - 1u : 0u; }
