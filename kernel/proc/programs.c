/* 26.5.014.0: install the bundled programs onto MintOS HD (see programs.h). */
#include <stdint.h>
#include "../include/programs.h"
#include "../include/mintos_fs.h"
#include "../include/task.h"
#include "../include/serial.h"
#include "../include/log.h"
#include "../include/uapi/mintos/syscall.h"

static int64_t mkdir_parents(const char *path) {
    static char prefix[MINT_MFS_PATH_MAX];
    uint32_t n = 0;
    for (uint32_t i = 0; path[i] && i + 1u < sizeof(prefix); ++i) {
        if (path[i] == '/' && i > 0) {
            prefix[n] = '\0';
            MINT_MFS_STAT st;
            if (mint_mfs_stat(prefix, &st) < 0) {
                int64_t rc = mint_mfs_mkdir(prefix);
                if (rc < 0 && rc != -MINT_EEXIST) return rc;
            } else if (st.type != MINT_MFS_DIR) {
                return -MINT_ENOTDIR;
            }
        }
        prefix[n++] = path[i];
    }
    return 0;
}

static int64_t write_file(const char *path, const uint8_t *data, uint64_t size, uint64_t pid) {
    int64_t rc = mkdir_parents(path);
    if (rc < 0) return rc;
    int64_t h = mint_mfs_open(path, MINT_O_WRITE | MINT_O_CREATE | MINT_O_TRUNC, pid);
    if (h < 0) return h;
    uint64_t done = 0;
    while (done < size) {
        uint32_t chunk = (uint32_t)(size - done > MINT_MFS_IO_MAX ? MINT_MFS_IO_MAX : size - done);
        int64_t w = mint_mfs_write(h, data + done, chunk, pid);
        if (w <= 0) { (void)mint_mfs_close(h, pid); return w < 0 ? w : -MINT_EIO; }
        done += (uint64_t)w;
    }
    return mint_mfs_close(h, pid);
}

static int marker_current(uint64_t pid) {
    char have[80];
    int64_t h = mint_mfs_open(MINT_PROGRAMS_MARKER, MINT_O_READ, pid);
    if (h < 0) return 0;
    int64_t got = mint_mfs_read(h, have, sizeof(have) - 1u, pid);
    (void)mint_mfs_close(h, pid);
    if (got <= 0) return 0;
    have[got] = '\0';
    for (uint32_t i = 0;; ++i) {
        if (have[i] != mint_program_bundle_id[i]) return 0;
        if (!have[i]) return 1;
    }
}

int64_t mint_programs_install(int force) {
    MINT_MFS_VOLUME v;
    if (!mint_mfs_volume(&v) || v.state != MINT_MFS_VOLUME_READY) return -MINT_ENODEV;
    uint64_t pid = mint_task_current_pid();
    if (!force && marker_current(pid)) return 0;
    int64_t written = 0;
    for (uint64_t i = 0; i < mint_program_bundle_count; ++i) {
        const MINT_PROGRAM_FILE *f = &mint_program_bundle[i];
        int64_t rc = write_file(f->path, f->data, f->size, pid);
        if (rc < 0) {
            mint_serial_write("[programs] could not install ");
            mint_serial_write(f->path);
            mint_serial_write("\n");
            mint_log_emit(MINT_LOG_ERROR, MINT_LOG_COMPONENT_PROC, "installing a bundled program onto MintOS HD failed");
            return rc;
        }
        ++written;
    }
    uint64_t id_len = 0;
    while (mint_program_bundle_id[id_len]) ++id_len;
    int64_t rc = write_file(MINT_PROGRAMS_MARKER, (const uint8_t *)mint_program_bundle_id, id_len, pid);
    if (rc < 0) return rc;
    (void)mint_mfs_sync();
    mint_serial_write("[programs] installed ");
    mint_serial_dec64((uint64_t)written);
    mint_serial_write(" bundled files into " MINT_PROGRAMS_DIR " and /Library/Examples (bundle ");
    mint_serial_write(mint_program_bundle_id);
    mint_serial_write(")\n");
    mint_log_emit(MINT_LOG_INFO, MINT_LOG_COMPONENT_PROC, "bundled programs installed onto MintOS HD");
    return written;
}

void mint_programs_task(void *argument) {
    (void)argument;
    uint32_t installed_block = 0xFFFFFFFFu;
    for (;;) {
        MINT_MFS_VOLUME v;
        if (mint_mfs_volume(&v) && v.state == MINT_MFS_VOLUME_READY) {
            if (v.block_index != installed_block) {
                int64_t rc = mint_programs_install(0);
                if (rc >= 0) {
                    installed_block = v.block_index;
                    if (rc == 0) mint_serial_write("[programs] bundled programs on MintOS HD are current\n");
                }
            }
        } else {
            installed_block = 0xFFFFFFFFu;
        }
        mint_task_sleep_ticks(200u);
    }
}
