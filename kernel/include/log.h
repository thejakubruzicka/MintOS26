#ifndef MINTOS26_LOG_H
#define MINTOS26_LOG_H
#include <stdint.h>

#define MINT_LOG_RING_CAPACITY 64u
#define MINT_LOG_MESSAGE_SIZE 96u
#define MINT_LOG_READ_MAX 12u

enum mint_log_severity {
    MINT_LOG_DEBUG = 0,
    MINT_LOG_INFO = 1,
    MINT_LOG_WARN = 2,
    MINT_LOG_ERROR = 3
};
enum mint_log_component {
    MINT_LOG_COMPONENT_KERNEL = 1,
    MINT_LOG_COMPONENT_PCI = 2,
    MINT_LOG_COMPONENT_AHCI = 3,
    MINT_LOG_COMPONENT_BLOCK = 4,
    MINT_LOG_COMPONENT_PARTITION = 5,
    MINT_LOG_COMPONENT_FAT16 = 6,
    MINT_LOG_COMPONENT_VFS = 7,
    MINT_LOG_COMPONENT_E1000 = 8,
    MINT_LOG_COMPONENT_USB = 9,
    MINT_LOG_COMPONENT_NET = 10,
    MINT_LOG_COMPONENT_MINTFS = 11,
    MINT_LOG_COMPONENT_AUDIO = 14      /* 26.5.014.0 personality: HDA sound */
};

typedef struct mint_log_record {
    uint64_t sequence;
    uint64_t ticks;
    uint32_t severity;
    uint32_t component;
    char message[MINT_LOG_MESSAGE_SIZE];
} MINT_LOG_RECORD;

void mint_log_init(void);
void mint_log_emit(uint32_t severity, uint32_t component, const char *message);
uint32_t mint_log_read_recent(MINT_LOG_RECORD *out, uint32_t capacity,
                              uint32_t minimum_severity, uint32_t component_filter);
uint64_t mint_log_total_dropped(void);
uint64_t mint_log_newest_sequence(void);

#endif
