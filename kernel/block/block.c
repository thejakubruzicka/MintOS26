#include <stdint.h>
#include "../include/block.h"

static MINT_BLOCK_DEVICE devices[MINT_BLOCK_MAX_DEVICES];
static uint32_t device_count;
static MINT_BLOCK_IO_STATS io_stats[MINT_BLOCK_MAX_DEVICES];
static struct { uint8_t active; uint32_t owner; uint64_t first, count; } grants[MINT_BLOCK_MAX_DEVICES];

static void bytes_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; ++i) b[i] = 0;
}

void mint_block_init(void) {
    bytes_zero(devices, sizeof(devices));
    bytes_zero(io_stats, sizeof(io_stats));
    bytes_zero(grants, sizeof(grants));
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

int mint_block_read_sectors(uint32_t index, uint64_t lba, uint32_t count, uint8_t *out) {
    const MINT_BLOCK_DEVICE *d = mint_block_get(index);
    if (!d || !out || !count || count > 8u || lba >= d->sector_count || count > d->sector_count - lba) return 0;
    if (d->read_sectors) {
        ++io_stats[index].read_calls;
        if (d->read_sectors(d->context, lba, count, out)) { ++io_stats[index].read_success; return 1; }
        ++io_stats[index].read_failures;
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) if (!mint_block_read(index, lba + i, out + i * MINT_BLOCK_SECTOR_SIZE)) return 0;
    return 1;
}

int mint_block_grant_writes(uint32_t index, uint64_t first_lba, uint64_t sectors, uint32_t owner) {
    const MINT_BLOCK_DEVICE *d = mint_block_get(index);
    if (!d || !owner || !sectors || first_lba >= d->sector_count || sectors > d->sector_count - first_lba) return 0;
    if (!d->write_sector || !d->flush) return 0;
    if (grants[index].active && grants[index].owner != owner) return 0;
    grants[index].active = 1; grants[index].owner = owner; grants[index].first = first_lba; grants[index].count = sectors;
    return 1;
}
void mint_block_revoke_writes(uint32_t index, uint32_t owner) {
    if (index < MINT_BLOCK_MAX_DEVICES && grants[index].active && grants[index].owner == owner) bytes_zero(&grants[index], sizeof(grants[index]));
}
int mint_block_write_granted(uint32_t index, uint64_t lba, uint32_t count, const uint8_t *data, uint32_t owner) {
    const MINT_BLOCK_DEVICE *d = mint_block_get(index);
    if (!d || !data || !count || count > 8u) return 0;
    if (!grants[index].active || grants[index].owner != owner || lba < grants[index].first ||
        lba - grants[index].first >= grants[index].count || count > grants[index].count - (lba - grants[index].first)) {
        ++io_stats[index].writes_blocked;
        return 0;
    }
    ++io_stats[index].write_calls;
    int ok;
    if (d->write_sectors) ok = d->write_sectors(d->context, lba, count, data);
    else { ok = 1; for (uint32_t i = 0; ok && i < count; ++i) ok = d->write_sector(d->context, lba + i, data + i * MINT_BLOCK_SECTOR_SIZE); }
    if (ok) ++io_stats[index].write_success; else ++io_stats[index].write_failures;
    return ok;
}
int mint_block_flush_granted(uint32_t index, uint32_t owner) {
    const MINT_BLOCK_DEVICE *d = mint_block_get(index);
    if (!d || !grants[index].active || grants[index].owner != owner || !d->flush) return 0;
    return d->flush(d->context);
}
