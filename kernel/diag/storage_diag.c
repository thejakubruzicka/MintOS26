#include <stdint.h>
#include <stddef.h>
#include "../include/storage_diag.h"
#include "../include/storage_diag_internal.h"
#include "../include/block.h"
#include "../include/log.h"
#include "../include/fat16.h"
#include "../include/serial.h"

static MINT_STORAGE_SNAPSHOT snapshot;

static void text_copy(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    size_t i = 0;
    if (src) while (src[i] && i + 1u < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static void bytes_zero(void *p, size_t n) {
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < n; ++i) b[i] = 0;
}
static MINT_STORAGE_DEVICE_INFO *device_for_block_index(uint32_t block_index) {
    for (uint32_t i=0; i<snapshot.device_count && i<MINT_STORAGE_MAX_DEVICES; ++i) {
        MINT_STORAGE_DEVICE_INFO *device=&snapshot.devices[i];
        if (device->block_registered && device->block_index==block_index) return device;
    }
    return 0;
}

MINT_STORAGE_SNAPSHOT *mint_storage_diag_mutable_snapshot(void) { return &snapshot; }
const MINT_STORAGE_SNAPSHOT *mint_storage_diag_snapshot(void) { return &snapshot; }

void mint_storage_diag_init(void) {
    bytes_zero(&snapshot, sizeof(snapshot));
    snapshot.version = MINT_STORAGE_DIAG_VERSION;
    mint_block_init();
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_BLOCK,
                  "storage diagnostics started; media writes are disabled");
    if (mint_ahci_init() < 0) {
        mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_AHCI,
                      "AHCI discovery failed before device registration");
    }
    snapshot.persistent_mount_active = 0u;
    snapshot.readonly_mount_active = 0u;
    snapshot.writes_attempted = 0u;
    /* Mount every valid MintOS26 volume. One bad/blank disk must not hide a
       second usable volume; registry indexes are independent of SATA port rows. */
    for (uint32_t block_index = 0; block_index < mint_block_count() &&
         block_index < MINT_STORAGE_MAX_DEVICES; ++block_index) {
        MINT_STORAGE_DEVICE_INFO *device=device_for_block_index(block_index);
        if (!mint_fat16_try_mount(block_index)) continue;
        uint8_t note_probe[4096], settings_probe[4096];
        int64_t note_bytes = mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT", note_probe, sizeof(note_probe));
        int64_t settings_bytes = mint_fat16_read_file("MINTOS26/DOCUMENTS/SETTINGS.DAT", settings_probe, sizeof(settings_probe));
        snapshot.readonly_mount_active = 1u;
        if (note_bytes >= 0 && settings_bytes >= 0) {
            if (device) text_copy(device->filesystem_status, sizeof(device->filesystem_status),
                                  "MintOS26 FAT16; Notes and Settings seeds readable");
            mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_PARTITION,
                          "MintOS26 FAT16 mounted read-only; seed files readable; writes disabled");
            mint_serial_write("[storage] FAT16 mounted read-only on block index ");
            mint_serial_dec64(block_index); mint_serial_write("; Notes and Settings seed files readable\n");
        } else {
            /* Standard FAT16 media is still mountable and browseable. Seed files
               are MintOS conveniences, not a filesystem-validity requirement. */
            if (device) text_copy(device->filesystem_status, sizeof(device->filesystem_status),
                                  "FAT16 mounted read-only; MintOS26 seed files absent");
            mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_PARTITION,
                          "standard FAT16 volume mounted read-only; MintOS26 seed files are optional");
            mint_serial_write("[storage] generic FAT16 mounted read-only on block index ");
            mint_serial_dec64(block_index); mint_serial_write("; seed files absent or unreadable\n");
        }
        if (device) text_copy(device->mount_status, sizeof(device->mount_status),
                              "READ-ONLY MOUNTED; ordinary writes disabled");
    }
    MINT_FAT16_VOLUME_LIST mounted_list;
    if (mint_fat16_volume_list(&mounted_list) && mounted_list.count) {
        snapshot.readonly_mount_active = 1u;
        snapshot.readonly_mount_count = mounted_list.count;
        (void)mint_fat16_volume_select(0u); /* deterministic initial Files volume */
    } else snapshot.readonly_mount_count = 0u;
    for (uint32_t i = 0; i < snapshot.device_count && i < MINT_STORAGE_MAX_DEVICES; ++i) {
        MINT_STORAGE_DEVICE_INFO *device=&snapshot.devices[i];
        if (!device->block_registered) {
            text_copy(device->mount_status, sizeof(device->mount_status),
                      "NOT MOUNTED - device was not registered for block I/O");
        } else if (mint_fat16_is_device_mounted(device->block_index)) {
            text_copy(device->mount_status, sizeof(device->mount_status),
                      "READ-ONLY MOUNTED; persistence writes disabled");
        } else {
            text_copy(device->mount_status, sizeof(device->mount_status),
                      snapshot.readonly_mount_active ?
                      "NOT MOUNTED - no compatible MintOS26 volume on this device" :
                      "NOT MOUNTED - read-only mount validation failed; persistence unavailable");
        }
    }
    snapshot.probe_complete = 1u;
    mint_serial_write("[storage] diagnostic probe complete controllers=");
    mint_serial_dec64(snapshot.controller_count);
    mint_serial_write(" devices="); mint_serial_dec64(snapshot.device_count);
    mint_serial_write(" readonly-mounted="); mint_serial_write(snapshot.readonly_mount_active ? "yes" : "no");
    mint_serial_write(" volumes="); mint_serial_dec64(snapshot.readonly_mount_count);
    mint_serial_write(" persistent=no writes-disabled=yes\n");
    mint_log_emit(snapshot.device_count ? MINT_LOG_INFO : MINT_LOG_WARN,
                  MINT_LOG_COMPONENT_BLOCK,
                  snapshot.readonly_mount_active ? "read-only filesystem validation completed; persistent writes remain disabled" :
                  (snapshot.device_count ? "read-only storage inventory completed; no MintOS26 volume mounted" :
                                           "no ATA device was successfully identified; see controller diagnostics"));
}

void mint_storage_diag_mark_write_attempt(void) {
    snapshot.writes_attempted=1u;
}

void mint_storage_diag_recheck(void) {
    snapshot.readonly_mount_active=0u;
    snapshot.readonly_mount_count=0u;
    snapshot.persistent_mount_active=0u;
    for (uint32_t block_index=0; block_index<mint_block_count() &&
         block_index<MINT_STORAGE_MAX_DEVICES; ++block_index) {
        MINT_STORAGE_DEVICE_INFO *device=device_for_block_index(block_index);
        if (!device || !mint_fat16_try_mount(block_index)) continue;
        uint8_t note_probe[4096], settings_probe[4096];
        int64_t note_bytes=mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT",note_probe,sizeof(note_probe));
        int64_t settings_bytes=mint_fat16_read_file("MINTOS26/DOCUMENTS/SETTINGS.DAT",settings_probe,sizeof(settings_probe));
        snapshot.readonly_mount_active=1u;
        if(note_bytes>=0 && settings_bytes>=0) {
            text_copy(device->filesystem_status,sizeof(device->filesystem_status),
                      "MintOS26 FAT16; Notes and Settings seeds readable");
            mint_log_emit(MINT_LOG_INFO,MINT_LOG_COMPONENT_PARTITION,
                          "MintOS26 FAT16 read-only revalidation passed; seed files readable");
            mint_serial_write("[storage] read-only revalidation passed for block index ");
            mint_serial_dec64(block_index);
            mint_serial_write("; MintOS26 seed files readable; writes remain disabled\n");
        } else {
            text_copy(device->filesystem_status,sizeof(device->filesystem_status),
                      "Standard FAT16 volume; MintOS26 seed files absent");
            mint_log_emit(MINT_LOG_INFO,MINT_LOG_COMPONENT_PARTITION,
                          "generic FAT16 read-only revalidation passed; seed files are optional");
            mint_serial_write("[storage] generic FAT16 revalidation passed for block index ");
            mint_serial_dec64(block_index);
            mint_serial_write("; seed files optional; writes remain disabled\n");
        }
        text_copy(device->mount_status,sizeof(device->mount_status),
                  "READ-ONLY MOUNTED; ordinary file writes remain disabled");
    }
    MINT_FAT16_VOLUME_LIST mounted_list;
    if (mint_fat16_volume_list(&mounted_list) && mounted_list.count) {
        snapshot.readonly_mount_active=1u;
        snapshot.readonly_mount_count=mounted_list.count;
        (void)mint_fat16_volume_select(0u);
    } else snapshot.readonly_mount_count=0u;
    for (uint32_t i=0; i<snapshot.device_count && i<MINT_STORAGE_MAX_DEVICES; ++i) {
        MINT_STORAGE_DEVICE_INFO *device=&snapshot.devices[i];
        if (!device->block_registered) {
            text_copy(device->mount_status,sizeof(device->mount_status),
                      "NOT MOUNTED - device was not registered for block I/O");
        } else if (mint_fat16_is_device_mounted(device->block_index)) {
            text_copy(device->mount_status,sizeof(device->mount_status),
                      "READ-ONLY MOUNTED; ordinary file writes disabled");
        } else {
            text_copy(device->mount_status,sizeof(device->mount_status),
                      snapshot.readonly_mount_active ?
                      "NOT MOUNTED - no compatible MintOS26 volume on this device" :
                      "NOT MOUNTED - initialize blank disk in Disk Utility if appropriate");
        }
    }
    snapshot.probe_complete=1u;
}
