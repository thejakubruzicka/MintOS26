#ifndef MINTOS26_DRIVER_CATALOG_H
#define MINTOS26_DRIVER_CATALOG_H
#include <stdint.h>

#define MINT_DRIVER_CATALOG_VERSION 2u
#define MINT_DRIVER_CATALOG_MAX_RECORDS 24u
#define MINT_DRIVER_NAME_LEN 32u
#define MINT_DRIVER_DETAIL_LEN 96u

enum mint_driver_state {
    MINT_DRIVER_ACTIVE = 1,
    MINT_DRIVER_READ_ONLY = 2,
    MINT_DRIVER_DIAGNOSTIC_ONLY = 3,
    MINT_DRIVER_DEVICE_FOUND = 4,
    MINT_DRIVER_NOT_IMPLEMENTED = 5,
    MINT_DRIVER_UNAVAILABLE = 6
};

typedef struct mint_driver_record {
    uint32_t id;
    uint32_t state;
    char name[MINT_DRIVER_NAME_LEN];
    char detail[MINT_DRIVER_DETAIL_LEN];
} MINT_DRIVER_RECORD;

typedef struct mint_driver_catalog {
    uint32_t version;
    uint32_t count;
    MINT_DRIVER_RECORD records[MINT_DRIVER_CATALOG_MAX_RECORDS];
} MINT_DRIVER_CATALOG;

void mint_driver_catalog_snapshot(MINT_DRIVER_CATALOG *out);
const char *mint_driver_state_name(uint32_t state);
#endif
