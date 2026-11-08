#include <stdint.h>
#include "../include/block.h"

static MINT_BLOCK_DEVICE devices[MINT_BLOCK_MAX_DEVICES];
static uint32_t device_count;
static MINT_BLOCK_IO_STATS io_stats[MINT_BLOCK_MAX_DEVICES];

static void bytes_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; ++i) b[i] = 0;
}

void mint_block_init(void) {
    bytes_zero(devices, sizeof(devices));
    bytes_zero(io_stats, sizeof(io_stats));
    device_count = 0;
}

int mint_block_register(const MINT_BLOCK_DEVICE *device) {
    if (!device || !device->sector_count || device->sector_size != MINT_BLOCK_SECTOR_SIZE ||
        !device->read_sector || !device->name[0] || device_count >= MINT_BLOCK_MAX_DEVICES)
        return 0;
    if (!(device->flags & MINT_BLOCK_FLAG_READONLY) && !device->write_sector) return 0;
    uint32_t index=device_count++;
    devices[index] = *device;
    io_stats[index].version=MINT_BLOCK_IO_STATS_VERSION;
    io_stats[index].device_index=index;
    return 1;
}

uint32_t mint_block_count(void) { return device_count; }

const MINT_BLOCK_DEVICE *mint_block_get(uint32_t index) {
    return index < device_count ? &devices[index] : 0;
}

int mint_block_read(uint32_t index, uint64_t lba, uint8_t out[MINT_BLOCK_SECTOR_SIZE]) {
    const MINT_BLOCK_DEVICE *d = mint_block_get(index);
    if (index >= device_count) return 0;
    ++io_stats[index].read_calls;
    if (!d || !out || lba >= d->sector_count) { ++io_stats[index].read_failures; return 0; }
    if (d->read_sector(d->context, lba, out)) { ++io_stats[index].read_success; return 1; }
    ++io_stats[index].read_failures;
    return 0;
}

int mint_block_write(uint32_t index, uint64_t lba, const uint8_t data[MINT_BLOCK_SECTOR_SIZE]) {
    const MINT_BLOCK_DEVICE *d = mint_block_get(index);
    if (index >= device_count) return 0;
    if (!d || !data || lba >= d->sector_count || (d->flags & MINT_BLOCK_FLAG_READONLY) ||
        !d->write_sector) { ++io_stats[index].writes_blocked; return 0; }
    ++io_stats[index].write_calls;
    if (d->write_sector(d->context, lba, data)) { ++io_stats[index].write_success; return 1; }
    ++io_stats[index].write_failures;
    return 0;
}

int mint_block_io_stats(uint32_t index, MINT_BLOCK_IO_STATS *out) {
    if (!out || index >= device_count) return 0;
    *out=io_stats[index];
    return 1;
}
