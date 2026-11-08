#include <stdint.h>
#include <stddef.h>
#include "../include/arch.h"
#include "../include/task.h"
#include "../include/syscall.h"
#include "../include/address_space.h"
#include "../include/desktop_abi.h"
#include "../include/uapi/mintos/syscall.h"
#include "../include/input.h"
#include "../include/desktop_services.h"
#include "../include/ramfs.h"
#include "../include/storage_diag.h"
#include "../include/driver_catalog.h"
#include "../include/log.h"
#include "../include/serial.h"
#include "../include/block.h"
#include "../include/fat16.h"
#include "../include/vfs.h"
#include "../include/pci.h"
#include "../include/e1000.h"

/* Kernel-internal dispatcher. Callers here supply trusted kernel pointers; the
 * ring-3 entry below never passes raw user pointers into this function. */
int64_t mint_syscall_dispatch(uint64_t number, uint64_t arg0, uint64_t arg1,
                              uint64_t arg2, uint64_t arg3, uint64_t arg4,
                              uint64_t arg5) {
    (void)arg3; (void)arg4; (void)arg5;
    switch (number) {
    case MINT_SYS_WRITE: {
        if (arg0 != 1u && arg0 != 2u) return -MINT_EBADF;
        if (!arg1 && arg2) return -MINT_EFAULT;
        if (arg2 > 512u) return -MINT_EINVAL;
        const uint8_t *buf = (const uint8_t *)(uintptr_t)arg1;
        for (uint64_t i = 0; i < arg2; ++i) mint_serial_putc((char)buf[i]);
        return (int64_t)arg2;
    }
    case MINT_SYS_SCHED_YIELD:
        mint_task_yield();
        return 0;
    case MINT_SYS_GETPID:
        return (int64_t)mint_task_current_pid();
    case MINT_SYS_CLOCK_GETTIME: {
        if ((int64_t)arg0 != MINT_CLOCK_MONOTONIC) return -MINT_EINVAL;
        if (!arg1) return -MINT_EFAULT;
        mint_timespec64 *ts = (mint_timespec64 *)(uintptr_t)arg1;
        uint64_t ticks = mint_timer_ticks();
        ts->tv_sec = (int64_t)(ticks / 100u);
        ts->tv_nsec = (int64_t)((ticks % 100u) * 10000000u);
        return 0;
    }
    case MINT_SYS_EXIT:
        mint_task_exit();
    default:
        return -MINT_ENOSYS;
    }
}

/* Compatibility wrappers retained for the process manager; the backing state
 * is owned by the currently scheduled task rather than one global user run. */
void mint_syscall_user_exit_reset(void) { mint_task_user_exit_reset(); }
int mint_syscall_user_exit_seen(void) { return mint_task_user_exit_seen(); }
uint64_t mint_syscall_user_exit_code(void) { return mint_task_user_exit_code(); }

static int copy_user_cstring(const MINT_ADDRESS_SPACE *space, uint64_t address,
                             char *out, size_t capacity) {
    if (!space || !out || !capacity || !address) return -MINT_EFAULT;
    for (size_t i=0;i<capacity;i++) {
        if (address > UINT64_MAX-i ||
            !mint_address_space_validate_user_range(space,address+i,1u,0) ||
            !mint_address_space_copy_from_user(space,&out[i],address+i,1u)) return -MINT_EFAULT;
        if (out[i]=='\0') return 0;
    }
    out[capacity-1u]='\0';
    return -MINT_EINVAL;
}
static int bounded_text_equal(const char *a, const char *b, size_t capacity) {
    if (!a || !b) return 0;
    for (size_t i=0;i<capacity;i++) {
        if (a[i]!=b[i]) return 0;
        if (a[i]=='\0') return 1;
    }
    return 0;
}

int64_t mint_syscall_dispatch_user(uint64_t number, uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                   uint64_t arg5) {
    const MINT_ADDRESS_SPACE *space = mint_address_space_current();
    if (!space || !space->active) return -MINT_EPERM;
    (void)arg3; (void)arg4; (void)arg5;

    if (number == MINT_SYS_WRITE) {
        if (arg0 != 1u && arg0 != 2u) return -MINT_EBADF;
        if (arg2 > 512u) return -MINT_EINVAL;
        uint8_t local[512];
        if (arg2 && !mint_address_space_copy_from_user(space, local, arg1, (size_t)arg2))
            return -MINT_EFAULT;
        return mint_syscall_dispatch(number, arg0, (uint64_t)(uintptr_t)local, arg2, 0, 0, 0);
    }
    if (number == MINT_SYS_CLOCK_GETTIME) {
        if ((int64_t)arg0 != MINT_CLOCK_MONOTONIC) return -MINT_EINVAL;
        if (!mint_address_space_validate_user_range(space, arg1, sizeof(mint_timespec64), 1))
            return -MINT_EFAULT;
        mint_timespec64 ts = {0, 0};
        int64_t rc = mint_syscall_dispatch(number, arg0, (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0);
        if (rc < 0) return rc;
        return mint_address_space_copy_to_user(space, arg1, &ts, sizeof(ts)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_STORAGE_FORMAT_BLANK) {
        MINT_STORAGE_FORMAT_REQUEST request;
        const MINT_STORAGE_SNAPSHOT *snapshot=mint_storage_diag_snapshot();
        if (!snapshot) return -MINT_ENODEV;
        if (!mint_address_space_validate_user_range(space,arg0,sizeof(request),0) ||
            !mint_address_space_copy_from_user(space,&request,arg0,sizeof(request))) return -MINT_EFAULT;
        if (request.version!=MINT_STORAGE_FORMAT_REQUEST_VERSION ||
            request.device_index>=snapshot->device_count ||
            request.device_index>=MINT_STORAGE_MAX_DEVICES) return -MINT_EINVAL;
        const MINT_STORAGE_DEVICE_INFO *info=&snapshot->devices[request.device_index];
        if (!info->block_registered || info->block_index>=mint_block_count()) return -MINT_ENODEV;
        const MINT_BLOCK_DEVICE *dev=mint_block_get(info->block_index);
        if (!dev || !dev->write_sector || !dev->flush) return -MINT_ENODEV;
        if (request.expected_sector_count!=info->sector_count ||
            request.expected_sector_count!=dev->sector_count ||
            !bounded_text_equal(request.expected_serial,info->serial,sizeof(request.expected_serial)))
            return -MINT_EPERM;
        char expected[MINT_STORAGE_FORMAT_CONFIRM_LEN];
        const char prefix[]="FORMAT "; size_t pos=0;
        for (size_t i=0;i<sizeof(prefix)-1u && pos+1u<sizeof(expected);i++) expected[pos++]=prefix[i];
        for (size_t i=0;info->name[i] && pos+1u<sizeof(expected);i++) expected[pos++]=info->name[i];
        expected[pos]='\0';
        if (!bounded_text_equal(request.confirmation,expected,sizeof(request.confirmation)))
            return -MINT_EPERM;
        mint_log_emit(MINT_LOG_WARN,MINT_LOG_COMPONENT_PARTITION,
                      "explicitly confirmed blank developer disk initialization requested");
        int rc=mint_fat16_format_blank(info->block_index);
        if (rc<0) {
            mint_log_emit(MINT_LOG_ERROR,MINT_LOG_COMPONENT_FAT16,
                          "blank disk initialization failed; target disk remains unverified");
            mint_serial_write("[storage] Disk Utility format failed for block index ");
            mint_serial_dec64(info->block_index);
            mint_serial_write("; per-device read-only revalidation follows\n");
            mint_storage_diag_recheck();
            return rc;
        }
        mint_storage_diag_recheck();
        mint_serial_write("[storage] Disk Utility format succeeded for block index ");
        mint_serial_dec64(info->block_index);
        mint_serial_write("; FAT16 read-only validation requested\n");
        mint_log_emit(MINT_LOG_INFO,MINT_LOG_COMPONENT_PARTITION,
                      "developer disk initialization request succeeded; read-only validation completed");
        return 0;
    }
    if (number == MINT_SYS_BLOCK_IO_STATS) {
        MINT_BLOCK_IO_STATS stats;
        if (!mint_block_io_stats((uint32_t)arg0,&stats)) return -MINT_ENODEV;
        if (!mint_address_space_validate_user_range(space,arg1,sizeof(stats),1)) return -MINT_EFAULT;
        return mint_address_space_copy_to_user(space,arg1,&stats,sizeof(stats)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_VFS_MOUNTS) {
        MINT_VFS_MOUNT_TABLE table;
        if (!mint_vfs_mount_table_snapshot(&table)) return -MINT_ENODEV;
        if (!mint_address_space_validate_user_range(space,arg0,sizeof(table),1)) return -MINT_EFAULT;
        return mint_address_space_copy_to_user(space,arg0,&table,sizeof(table)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_PCI_INVENTORY) {
        const MINT_PCI_INVENTORY *inventory=mint_pci_get_inventory();
        if (!inventory) return -MINT_ENODEV;
        if (!mint_address_space_validate_user_range(space,arg0,sizeof(*inventory),1)) return -MINT_EFAULT;
        return mint_address_space_copy_to_user(space,arg0,inventory,sizeof(*inventory)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_E1000_START) {
        if (mint_e1000_init()) return 0;
        MINT_E1000_INFO status;
        (void)mint_e1000_get_info(&status);
        return status.state == MINT_E1000_FAILED ? -MINT_EIO : -MINT_ENODEV;
    }
    if (number == MINT_SYS_E1000_INFO) {
        MINT_E1000_INFO local;
        if (!mint_address_space_validate_user_range(space,arg0,sizeof(local),1)) return -MINT_EFAULT;
        if (!mint_e1000_get_info(&local)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space,arg0,&local,sizeof(local)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_E1000_LOOPBACK_TEST) {
        int rc=mint_e1000_loopback_test();
        if (rc<0) return -MINT_ENODEV;
        return rc ? 0 : -MINT_EIO;
    }
    if (number == MINT_SYS_FAT16_VOLUMES) {
        MINT_FAT16_VOLUME_LIST list;
        if (!mint_fat16_volume_list(&list)) return -MINT_ENODEV;
        if (!mint_address_space_validate_user_range(space,arg0,sizeof(list),1)) return -MINT_EFAULT;
        return mint_address_space_copy_to_user(space,arg0,&list,sizeof(list)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_FAT16_SELECT_VOLUME) {
        return mint_fat16_volume_select((uint32_t)arg0) ? 0 : -MINT_ENODEV;
    }
    if (number == MINT_SYS_FAT16_LIST) {
        char path[96];
        if (arg2==0 || arg2>MINT_FAT16_LIST_MAX) return -MINT_EINVAL;
        int path_rc=copy_user_cstring(space,arg0,path,sizeof(path));
        if (path_rc<0) return path_rc;
        size_t bytes=(size_t)arg2*sizeof(MINT_FAT16_DIRENT);
        if (!mint_address_space_validate_user_range(space,arg1,bytes,1)) return -MINT_EFAULT;
        MINT_FAT16_DIRENT entries[MINT_FAT16_LIST_MAX];
        int64_t count=mint_fat16_list_dir(path,entries,(uint32_t)arg2);
        if (count<0) return count;
        size_t written=(size_t)count*sizeof(entries[0]);
        return !written || mint_address_space_copy_to_user(space,arg1,entries,written) ? count : -MINT_EFAULT;
    }
    if (number == MINT_SYS_FAT16_READ) {
        char path[96]; uint8_t local[1024];
        if (arg2>sizeof(local)) return -MINT_EINVAL;
        int path_rc=copy_user_cstring(space,arg0,path,sizeof(path));
        if (path_rc<0) return path_rc;
        if (arg2 && !mint_address_space_validate_user_range(space,arg1,(size_t)arg2,1)) return -MINT_EFAULT;
        int64_t got=mint_fat16_read_file(path,local,(size_t)arg2);
        if (got<0) return got;
        if ((uint64_t)got>arg2) return -MINT_ENOSPC;
        return !got || mint_address_space_copy_to_user(space,arg1,local,(size_t)got) ? got : -MINT_EFAULT;
    }
    if (number == MINT_SYS_STORAGE_DIAGNOSTICS) {
        const MINT_STORAGE_SNAPSHOT *snapshot = mint_storage_diag_snapshot();
        if (!snapshot) return -MINT_ENODEV;
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(*snapshot), 1))
            return -MINT_EFAULT;
        return mint_address_space_copy_to_user(space, arg0, snapshot, sizeof(*snapshot))
            ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_DRIVER_CATALOG) {
        MINT_DRIVER_CATALOG catalog;
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(catalog), 1))
            return -MINT_EFAULT;
        mint_driver_catalog_snapshot(&catalog);
        return mint_address_space_copy_to_user(space, arg0, &catalog, sizeof(catalog))
            ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_LOG_READ) {
        uint32_t capacity = (uint32_t)arg1;
        if (!arg0 || !capacity || capacity > MINT_LOG_READ_MAX || arg1 > MINT_LOG_READ_MAX ||
            arg2 > MINT_LOG_ERROR) return -MINT_EINVAL;
        size_t bytes = (size_t)capacity * sizeof(MINT_LOG_RECORD);
        if (!mint_address_space_validate_user_range(space, arg0, bytes, 1)) return -MINT_EFAULT;
        MINT_LOG_RECORD local[MINT_LOG_READ_MAX];
        uint32_t count = mint_log_read_recent(local, capacity, (uint32_t)arg2, (uint32_t)arg3);
        if (count && !mint_address_space_copy_to_user(space, arg0, local,
                                                     (size_t)count * sizeof(MINT_LOG_RECORD)))
            return -MINT_EFAULT;
        return (int64_t)count;
    }
    if (number == MINT_SYS_EXIT) {
        uint64_t code = arg0;
        if (!mint_address_space_activate_kernel()) return -MINT_EFAULT;
        mint_task_user_exit_record(code);
        return (int64_t)MINT_SYSCALL_EXIT_SENTINEL;
    }
    if (number == MINT_SYS_SCHED_YIELD) {
        /* The scheduler saves this task's syscall continuation and RFLAGS,
         * changes CR3/RSP0/transition state, and later resumes this exact frame. */
        mint_task_yield();
        return 0;
    }
    if (number == MINT_SYS_DESKTOP_EVENT_POLL) {
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(MINT_DESKTOP_EVENT), 1))
            return -MINT_EFAULT;
        MINT_DESKTOP_EVENT event;
        if (!mint_desktop_event_next(&event)) return 0;
        return mint_address_space_copy_to_user(space, arg0, &event, sizeof(event)) ? 1 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_DISPLAY_INFO) {
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(MINT_DISPLAY_INFO), 1))
            return -MINT_EFAULT;
        MINT_DISPLAY_INFO info = {0};
        if (!mint_display_current_info(&info)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space, arg0, &info, sizeof(info)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_DISPLAY_FRAME_BEGIN)
        return mint_display_frame_begin() ? 0 : -MINT_ENODEV;
    if (number == MINT_SYS_DISPLAY_FRAME_END)
        return mint_display_frame_end() ? 0 : -MINT_ENODEV;
    if (number == MINT_SYS_DISPLAY_BACKGROUND_CAPTURE)
        return mint_display_current_capture_background() ? 0 : -MINT_ENODEV;
    if (number == MINT_SYS_DISPLAY_RESTORE_RECT) {
        int64_t x = (int64_t)arg0, y = (int64_t)arg1;
        int64_t width = (int64_t)arg2, height = (int64_t)arg3;
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX ||
            width <= 0 || width > 16384 || height <= 0 || height > 16384)
            return -MINT_EINVAL;
        return mint_display_current_restore_rect((int32_t)x, (int32_t)y,
                                                  (int32_t)width, (int32_t)height)
                   ? 0 : -MINT_ENODEV;
    }
    if (number == MINT_SYS_INPUT_INFO) {
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(MINT_INPUT_INFO), 1))
            return -MINT_EFAULT;
        MINT_INPUT_INFO info = {0};
        info.keyboard_ready = mint_input_keyboard_ready() ? 1u : 0u;
        info.mouse_ready = mint_input_mouse_ready() ? 1u : 0u;
        mint_input_get_counters(&info.keyboard_events, &info.pointer_motion_events,
                                &info.pointer_button_events);
        info.dropped_events = mint_desktop_event_overflow_count();
        return mint_address_space_copy_to_user(space, arg0, &info, sizeof(info)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_DESKTOP_SETTINGS_GET) {
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(MINT_DESKTOP_SETTINGS), 1))
            return -MINT_EFAULT;
        MINT_DESKTOP_SETTINGS settings = {0};
        if (!mint_desktop_settings_get(&settings)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space, arg0, &settings, sizeof(settings)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_DESKTOP_SETTINGS_SET) {
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(MINT_DESKTOP_SETTINGS), 0))
            return -MINT_EFAULT;
        MINT_DESKTOP_SETTINGS settings = {0};
        if (!mint_address_space_copy_from_user(space, &settings, arg0, sizeof(settings))) return -MINT_EFAULT;
        return mint_desktop_settings_set(&settings) ? 0 : -MINT_EINVAL;
    }
    if (number == MINT_SYS_FS_OPEN) {
        char name[MINT_RAMFS_NAME_MAX];
        if (arg0 > UINT64_MAX - (MINT_RAMFS_NAME_MAX - 1u)) return -MINT_EFAULT;
        int terminated = 0;
        for (size_t i = 0; i < sizeof(name); ++i) {
            if (!mint_address_space_validate_user_range(space, arg0 + i, 1u, 0)) return -MINT_EFAULT;
            if (!mint_address_space_copy_from_user(space, &name[i], arg0 + i, 1u)) return -MINT_EFAULT;
            if (name[i] == '\0') { terminated = 1; break; }
        }
        if (!terminated) return -MINT_EINVAL;
        return mint_ramfs_open(mint_task_current_pid(), name, (uint32_t)arg1);
    }
    if (number == MINT_SYS_FS_READ) {
        if (arg2 > 1024u) return -MINT_EINVAL;
        if (arg2 && !mint_address_space_validate_user_range(space, arg1, (size_t)arg2, 1)) return -MINT_EFAULT;
        uint8_t local[1024];
        int64_t got = mint_ramfs_read(mint_task_current_pid(), arg0, local, (size_t)arg2);
        if (got <= 0) return got;
        return mint_address_space_copy_to_user(space, arg1, local, (size_t)got) ? got : -MINT_EFAULT;
    }
    if (number == MINT_SYS_FS_WRITE) {
        if (arg2 > 1024u) return -MINT_EINVAL;
        uint8_t local[1024];
        if (arg2 && !mint_address_space_copy_from_user(space, local, arg1, (size_t)arg2)) return -MINT_EFAULT;
        return mint_ramfs_write(mint_task_current_pid(), arg0, local, (size_t)arg2);
    }
    if (number == MINT_SYS_FS_CLOSE)
        return mint_ramfs_close(mint_task_current_pid(), arg0);
    if (number == MINT_SYS_FS_LIST) {
        if (arg1 > MINT_RAMFS_MAX_FILES) return -MINT_EINVAL;
        size_t bytes = (size_t)arg1 * sizeof(MINT_FS_DIRENT);
        if (bytes && !mint_address_space_validate_user_range(space, arg0, bytes, 1)) return -MINT_EFAULT;
        MINT_FS_DIRENT entries[MINT_RAMFS_MAX_FILES];
        int64_t count = mint_ramfs_list(entries, (uint32_t)arg1);
        if (count < 0) return count;
        size_t written = (size_t)count * sizeof(MINT_FS_DIRENT);
        return written == 0 || mint_address_space_copy_to_user(space, arg0, entries, written) ? count : -MINT_EFAULT;
    }
    if (number == MINT_SYS_DISPLAY_FILL_RECT) {
        int64_t x = (int64_t)arg0, y = (int64_t)arg1;
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX ||
            arg2 == 0 || arg2 > 16384u || arg3 == 0 || arg3 > 16384u || arg4 > 0xFFFFFFu)
            return -MINT_EINVAL;
        if (!mint_display_current_fill_rect((int32_t)x, (int32_t)y,
                                            (int32_t)arg2, (int32_t)arg3,
                                            (uint32_t)arg4)) return -MINT_ENODEV;
        return 0;
    }
    if (number == MINT_SYS_DISPLAY_BLEND_ROUND_RECT) {
        int64_t x=(int64_t)arg0, y=(int64_t)arg1;
        int64_t w=(int64_t)arg2, h=(int64_t)arg3;
        uint32_t alpha=(uint32_t)(arg5 & 0xFFu);
        uint32_t radius=(uint32_t)((arg5 >> 8) & 0xFFu);
        if (x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX ||
            w<=0 || w>16384 || h<=0 || h>16384 || arg4>0xFFFFFFu ||
            alpha>255u || radius>31u) return -MINT_EINVAL;
        if (!mint_display_current_blend_round_rect((int32_t)x,(int32_t)y,
                (int32_t)w,(int32_t)h,(uint32_t)arg4,alpha,radius)) return -MINT_ENODEV;
        return 0;
    }
    if (number == MINT_SYS_DISPLAY_DRAW_BITMAP) {
        uint32_t packed_source=(uint32_t)arg1, packed_target=(uint32_t)arg4;
        uint32_t source_width=packed_source&0xFFFFu, source_height=packed_source>>16;
        uint32_t target_width=packed_target&0xFFFFu, target_height=packed_target>>16;
        int64_t x=(int64_t)arg2, y=(int64_t)arg3;
        if (arg1>UINT32_MAX || arg4>UINT32_MAX || arg5!=0u ||
            !source_width || source_width>96u || !source_height || source_height>96u ||
            !target_width || target_width>128u || !target_height || target_height>128u ||
            x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX)
            return -MINT_EINVAL;
        size_t source_bytes=(size_t)source_width*(size_t)source_height*4u;
        if (arg0>UINT64_MAX-source_bytes ||
            !mint_address_space_validate_user_range(space,arg0,source_bytes,0)) return -MINT_EFAULT;
        MINT_DISPLAY_INFO info;
        if (!mint_display_current_info(&info)) return -MINT_ENODEV;
        uint8_t row[96u*4u];
        for (uint32_t dy=0;dy<target_height;dy++) {
            int64_t target_y=y+(int64_t)dy;
            if (target_y<0 || target_y>=(int64_t)info.height) continue;
            uint32_t source_y=(uint32_t)(((uint64_t)dy*source_height)/target_height);
            uint64_t source_row=arg0+(uint64_t)source_y*source_width*4u;
            if (!mint_address_space_copy_from_user(space,row,source_row,(size_t)source_width*4u))
                return -MINT_EFAULT;
            if (!mint_display_current_draw_rgba_scaled_row((int32_t)x,(int32_t)target_y,
                    target_width,source_width,row)) return -MINT_ENODEV;
        }
        return 0;
    }
    if (number == MINT_SYS_DISPLAY_SET_WALLPAPER) {
        if (arg0 > 2u) return -MINT_EINVAL;
        return mint_display_current_draw_wallpaper((uint32_t)arg0) ? 0 : -MINT_EINVAL;
    }
    if (number == MINT_SYS_DISPLAY_DRAW_TEXT) {
        int64_t x = (int64_t)arg2, y = (int64_t)arg3;
        if (arg1 > 128u || arg4 > 0xFFFFFFu || arg5 < 1u || arg5 > 4u ||
            x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
            return -MINT_EINVAL;
        if (arg1 == 0) return 0;
        if (!mint_address_space_validate_user_range(space, arg0, (size_t)arg1, 0))
            return -MINT_EFAULT;
        char text[129];
        if (!mint_address_space_copy_from_user(space, text, arg0, (size_t)arg1))
            return -MINT_EFAULT;
        text[arg1] = '\0';
        if (!mint_display_current_draw_text(text, (int32_t)x, (int32_t)y,
                                            (uint32_t)arg4, (uint32_t)arg5))
            return -MINT_ENODEV;
        return 0;
    }
    if (number == MINT_SYS_SLEEP_TICKS) {
        if (arg0 == 0 || arg0 > 1000u) return -MINT_EINVAL;
        /* Sleep suspends this task's syscall continuation. The scheduler saves
           the task-local user transition and resumes it after the PIT deadline. */
        mint_task_sleep_ticks(arg0);
        return 0;
    }
    /* Remaining implemented calls use no user pointers (currently getpid). */
    return mint_syscall_dispatch(number, arg0, arg1, arg2, arg3, arg4, arg5);
}

int64_t mint_syscall_trap_dispatch(MINT_SYSCALL_FRAME *frame) {
    if (!frame || frame->vector != 0x80u || (frame->cs & 3u) != 3u)
        return -MINT_EPERM;
    return mint_syscall_dispatch_user(frame->rax, frame->rdi, frame->rsi, frame->rdx,
                                      frame->r10, frame->r8, frame->r9);
}
