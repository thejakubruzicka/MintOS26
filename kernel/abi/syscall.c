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
#include "../include/net.h"
#include "../include/power.h"
#include "../include/rtc.h"
#include "../include/usb.h"
#include "../include/process.h"
#include "../include/mintos_fs.h"
#include "../include/proc_syscalls.h"

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
        if ((int64_t)arg0 != MINT_CLOCK_MONOTONIC && (int64_t)arg0 != MINT_CLOCK_REALTIME)
            return -MINT_EINVAL;
        if ((int64_t)arg0 == MINT_CLOCK_REALTIME && !mint_rtc_boot_epoch()) return -MINT_ENODEV;
        if (!arg1) return -MINT_EFAULT;
        mint_timespec64 *ts = (mint_timespec64 *)(uintptr_t)arg1;
        uint64_t ticks = mint_timer_ticks();
        ts->tv_sec = (int64_t)(ticks / 100u) +
                     ((int64_t)arg0 == MINT_CLOCK_REALTIME ? mint_rtc_boot_epoch() : 0);
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

/* Validates a destructive disk request from Disk Utility: the device must be
 * the one the user saw (serial + size) and the confirmation must read
 * "FORMAT <device>". Returns the block index or a negative error. */
static int64_t validate_format_request(const MINT_ADDRESS_SPACE *space, uint64_t address,
                                       const MINT_STORAGE_DEVICE_INFO **out_info) {
    MINT_STORAGE_FORMAT_REQUEST request;
    const MINT_STORAGE_SNAPSHOT *snapshot=mint_storage_diag_snapshot();
    if (!snapshot) return -MINT_ENODEV;
    if (!mint_address_space_validate_user_range(space,address,sizeof(request),0) ||
        !mint_address_space_copy_from_user(space,&request,address,sizeof(request))) return -MINT_EFAULT;
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
    *out_info=info;
    return (int64_t)info->block_index;
}

/* File syscalls for the MintOS disk. Syscalls never interleave on the
 * cooperative scheduler, so the bounce buffers can be static. */
static int64_t fs_syscall(const MINT_ADDRESS_SPACE *space, uint64_t number, uint64_t arg0,
                          uint64_t arg1, uint64_t arg2) {
    static char path_a[MINT_MFS_PATH_MAX], path_b[MINT_MFS_PATH_MAX];
    static uint8_t io[MINT_MFS_IO_MAX];
    static MINT_MFS_DIRENT list[MINT_MFS_LIST_MAX];
    uint64_t pid=mint_task_current_pid();
    int rc;
    switch (number) {
    case MINT_SYS_MFS_OPEN:
        if ((rc=copy_user_cstring(space,arg0,path_a,sizeof(path_a)))<0) return rc;
        if (arg1 & ~(uint64_t)(MINT_O_READ|MINT_O_WRITE|MINT_O_CREATE|MINT_O_TRUNC|MINT_O_APPEND|MINT_O_EXCL)) return -MINT_EINVAL;
        return mint_mfs_open(path_a,(uint32_t)arg1,pid);
    case MINT_SYS_MFS_READ: {
        uint64_t len=arg2>sizeof(io)?sizeof(io):arg2;
        if (!len) return 0;
        if (!mint_address_space_validate_user_range(space,arg1,(size_t)len,1)) return -MINT_EFAULT;
        int64_t got=mint_mfs_read((int64_t)arg0,io,(uint32_t)len,pid);
        if (got>0 && !mint_address_space_copy_to_user(space,arg1,io,(size_t)got)) return -MINT_EFAULT;
        return got;
    }
    case MINT_SYS_MFS_WRITE: {
        uint64_t len=arg2>sizeof(io)?sizeof(io):arg2;
        if (!len) return 0;
        if (!mint_address_space_validate_user_range(space,arg1,(size_t)len,0) ||
            !mint_address_space_copy_from_user(space,io,arg1,(size_t)len)) return -MINT_EFAULT;
        return mint_mfs_write((int64_t)arg0,io,(uint32_t)len,pid);
    }
    case MINT_SYS_MFS_SEEK:
        if (arg2>MINT_SEEK_END) return -MINT_EINVAL;
        return mint_mfs_seek((int64_t)arg0,(int64_t)arg1,(uint32_t)arg2,pid);
    case MINT_SYS_MFS_CLOSE:
        return mint_mfs_close((int64_t)arg0,pid);
    case MINT_SYS_MFS_STAT: {
        MINT_MFS_STAT st;
        if ((rc=copy_user_cstring(space,arg0,path_a,sizeof(path_a)))<0) return rc;
        if (!mint_address_space_validate_user_range(space,arg1,sizeof(st),1)) return -MINT_EFAULT;
        int64_t r=mint_mfs_stat(path_a,&st);
        if (r<0) return r;
        return mint_address_space_copy_to_user(space,arg1,&st,sizeof(st)) ? 0 : -MINT_EFAULT;
    }
    case MINT_SYS_MFS_LIST: {
        /* arg1 = start index, arg2 = user array; capacity is MINT_MFS_LIST_MAX. */
        if ((rc=copy_user_cstring(space,arg0,path_a,sizeof(path_a)))<0) return rc;
        if (arg1>0xFFFFFFFFu) return -MINT_EINVAL;
        if (!mint_address_space_validate_user_range(space,arg2,sizeof(list),1)) return -MINT_EFAULT;
        int64_t n=mint_mfs_list(path_a,(uint32_t)arg1,list,MINT_MFS_LIST_MAX);
        if (n<=0) return n;
        return mint_address_space_copy_to_user(space,arg2,list,(size_t)n*sizeof(list[0])) ? n : -MINT_EFAULT;
    }
    case MINT_SYS_MFS_MKDIR:
        if ((rc=copy_user_cstring(space,arg0,path_a,sizeof(path_a)))<0) return rc;
        return mint_mfs_mkdir(path_a);
    case MINT_SYS_MFS_RENAME:
        if ((rc=copy_user_cstring(space,arg0,path_a,sizeof(path_a)))<0) return rc;
        if ((rc=copy_user_cstring(space,arg1,path_b,sizeof(path_b)))<0) return rc;
        return mint_mfs_rename(path_a,path_b);
    case MINT_SYS_MFS_REMOVE:
        if ((rc=copy_user_cstring(space,arg0,path_a,sizeof(path_a)))<0) return rc;
        return mint_mfs_remove(path_a);
    case MINT_SYS_MFS_SYNC:
        return mint_mfs_sync();
    case MINT_SYS_MFS_VOLUME: {
        MINT_MFS_VOLUME v;
        if (!mint_address_space_validate_user_range(space,arg0,sizeof(v),1)) return -MINT_EFAULT;
        (void)mint_mfs_volume(&v);
        return mint_address_space_copy_to_user(space,arg0,&v,sizeof(v)) ? 0 : -MINT_EFAULT;
    }
    default:
        return -MINT_ENOSYS;
    }
}

int64_t mint_syscall_dispatch_user(uint64_t number, uint64_t arg0, uint64_t arg1,
                                   uint64_t arg2, uint64_t arg3, uint64_t arg4,
                                   uint64_t arg5) {
    const MINT_ADDRESS_SPACE *space = mint_address_space_current();
    if (!space || !space->active) return -MINT_EPERM;
    (void)arg3; (void)arg4; (void)arg5;
    /* 26.5.014.0: processes, pipes and memory (app platform). */
    if (number >= MINT_SYS_APP_PLATFORM_FIRST && number <= MINT_SYS_APP_PLATFORM_LAST)
        return mint_proc_syscall(number, arg0, arg1, arg2, arg3, arg4, arg5);

    if (number == MINT_SYS_WRITE) {
        if (arg0 != 1u && arg0 != 2u) return -MINT_EBADF;
        if (arg2 > 512u) return -MINT_EINVAL;
        uint8_t local[512];
        if (arg2 && !mint_address_space_copy_from_user(space, local, arg1, (size_t)arg2))
            return -MINT_EFAULT;
        return mint_syscall_dispatch(number, arg0, (uint64_t)(uintptr_t)local, arg2, 0, 0, 0);
    }
    if (number == MINT_SYS_CLOCK_GETTIME) {
        if ((int64_t)arg0 != MINT_CLOCK_MONOTONIC && (int64_t)arg0 != MINT_CLOCK_REALTIME)
            return -MINT_EINVAL;
        if (!mint_address_space_validate_user_range(space, arg1, sizeof(mint_timespec64), 1))
            return -MINT_EFAULT;
        mint_timespec64 ts = {0, 0};
        int64_t rc = mint_syscall_dispatch(number, arg0, (uint64_t)(uintptr_t)&ts, 0, 0, 0, 0);
        if (rc < 0) return rc;
        return mint_address_space_copy_to_user(space, arg1, &ts, sizeof(ts)) ? 0 : -MINT_EFAULT;
    }
    if (number >= MINT_SYS_MFS_OPEN && number <= MINT_SYS_MFS_VOLUME)
        return fs_syscall(space, number, arg0, arg1, arg2);
    if (number == MINT_SYS_STORAGE_INIT_MINTOS) {
        const MINT_STORAGE_DEVICE_INFO *info=0;
        int64_t index=validate_format_request(space,arg0,&info);
        if (index<0) return index;
        mint_log_emit(MINT_LOG_WARN,MINT_LOG_COMPONENT_MINTFS,
                      "explicitly confirmed MintOS disk initialization requested");
        int rc=mint_mintos_disk_initialize((uint32_t)index);
        mint_serial_write(rc==0 ? "[storage] Disk Utility initialized a MintOS disk on block index "
                                : "[storage] MintOS disk initialization failed for block index ");
        mint_serial_dec64((uint64_t)index);
        mint_serial_write("\n");
        mint_storage_diag_recheck();
        return rc;
    }
    if (number == MINT_SYS_STORAGE_FORMAT_BLANK) {
        const MINT_STORAGE_DEVICE_INFO *info=0;
        int64_t index=validate_format_request(space,arg0,&info);
        if (index<0) return index;
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
    if (number == MINT_SYS_DISPLAY_MAP_CANVAS) {
        MINT_DISPLAY_CANVAS out;
        MINT_DISPLAY_INFO info;
        uint64_t canvas_pa = 0, wallpaper_pa = 0, scratch_pa = 0, bytes = 0;
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(out), 1)) return -MINT_EFAULT;
        if (!mint_display_current_info(&info) ||
            !mint_display_canvas_buffers(&canvas_pa, &wallpaper_pa, &scratch_pa, &bytes) ||
            !bytes || (bytes & 4095u)) return -MINT_ENODEV;
        const uint64_t two_mib = 2ull * 1024ull * 1024ull;
        uint64_t span = ((bytes + two_mib - 1u) & ~(two_mib - 1u)) + two_mib; /* + unmapped guard */
        uint64_t va0 = MINT_DISPLAY_CANVAS_USER_VA, va1 = va0 + span, va2 = va1 + span;
        MINT_ADDRESS_REGION existing;
        if (!mint_address_space_region_query(space, va0, &existing)) {
            /* The process borrows the kernel-owned buffers; they are never freed
               by its teardown. Write-back cacheable, never executable. */
            MINT_ADDRESS_SPACE *own = (MINT_ADDRESS_SPACE *)space;
            uint64_t pages = bytes / 4096u;
            if (!mint_address_space_map_shared(own, va0, canvas_pa, pages, MINT_PAGE_MAP_WRITE) ||
                !mint_address_space_map_shared(own, va1, wallpaper_pa, pages, MINT_PAGE_MAP_WRITE) ||
                !mint_address_space_map_shared(own, va2, scratch_pa, pages, MINT_PAGE_MAP_WRITE))
                return -MINT_ENOMEM;
        } else if (existing.kind != MINT_ASPACE_REGION_SHARED) {
            return -MINT_EINVAL;
        }
        mint_display_enter_canvas_mode();
        out = (MINT_DISPLAY_CANVAS){MINT_DISPLAY_CANVAS_VERSION, info.width, info.height,
                                    info.stride_pixels, va0, va1, va2, bytes, info.pixel_format, 0u};
        mint_serial_write("[display] shared canvas mapped into pid ");
        mint_serial_dec64(mint_task_current_pid());
        mint_serial_write("\n");
        return mint_address_space_copy_to_user(space, arg0, &out, sizeof(out)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_DISPLAY_PRESENT) {
        int64_t x = (int64_t)arg0, y = (int64_t)arg1, w = (int64_t)arg2, h = (int64_t)arg3;
        if (x < -16384 || x > 16384 || y < -16384 || y > 16384 || w < 0 || w > 16384 || h < 0 || h > 16384)
            return -MINT_EINVAL;
        return mint_display_present_canvas((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h) ? 0 : -MINT_ENODEV;
    }
    if (number == MINT_SYS_DISPLAY_CURSOR) {
        int64_t x = (int64_t)arg0, y = (int64_t)arg1;
        if (x < -16384 || x > 16384 || y < -16384 || y > 16384 || arg2 >= MINT_CURSOR_SHAPE_COUNT)
            return -MINT_EINVAL;
        return mint_display_set_overlay_cursor((int32_t)x, (int32_t)y, (uint32_t)arg2) ? 0 : -MINT_ENODEV;
    }
    if (number == MINT_SYS_DISPLAY_WALLPAPER_RENDER) {
        if (arg0 > MINT_WALLPAPER_DARK) return -MINT_EINVAL;
        return mint_display_render_wallpaper((uint32_t)arg0) ? 0 : -MINT_ENODEV;
    }
    if (number == MINT_SYS_USB_INFO) {
        static MINT_USB_INFO usb_local;
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(usb_local), 1)) return -MINT_EFAULT;
        if (!mint_usb_get_info(&usb_local)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space, arg0, &usb_local, sizeof(usb_local)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_NET_INFO) {
        static MINT_NET_INFO net_local;
        if (!mint_address_space_validate_user_range(space, arg0, sizeof(net_local), 1)) return -MINT_EFAULT;
        if (!mint_net_get_info(&net_local)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space, arg0, &net_local, sizeof(net_local)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_NET_UP) return mint_net_up((uint32_t)arg0);
    if (number == MINT_SYS_NET_RESOLVE) {
        char name[96];
        uint32_t ip = 0;
        int rc = copy_user_cstring(space, arg0, name, sizeof(name));
        if (rc < 0) return rc;
        if (!mint_address_space_validate_user_range(space, arg1, sizeof(ip), 1)) return -MINT_EFAULT;
        int64_t result = mint_net_resolve(name, &ip);
        if (result == 0 && !mint_address_space_copy_to_user(space, arg1, &ip, sizeof(ip))) return -MINT_EFAULT;
        return result;
    }
    if (number == MINT_SYS_NET_PING) {
        if (arg1 > 0xFFFFFFFFu || arg2 > 0xFFFFu) return -MINT_EINVAL;
        return mint_net_ping((uint32_t)arg0, (uint32_t)arg1, (uint32_t)arg2);
    }
    if (number == MINT_SYS_TCP) {
        /* Bounded bounce buffer; syscalls never interleave on the cooperative scheduler. */
        static uint8_t tcp_bounce[4096];
        switch (arg0) {
        case MINT_TCP_OP_CONNECT:
            if (arg1 > 0xFFFFFFFFu || !arg2 || arg2 > 0xFFFFu) return -MINT_EINVAL;
            return mint_net_tcp_connect((uint32_t)arg1, (uint16_t)arg2);
        case MINT_TCP_OP_SEND: {
            uint64_t length = arg3 > sizeof(tcp_bounce) ? sizeof(tcp_bounce) : arg3;
            if (length && (!mint_address_space_validate_user_range(space, arg2, (size_t)length, 0) ||
                           !mint_address_space_copy_from_user(space, tcp_bounce, arg2, (size_t)length))) return -MINT_EFAULT;
            return mint_net_tcp_send((int64_t)arg1, tcp_bounce, (uint32_t)length);
        }
        case MINT_TCP_OP_RECV: {
            uint64_t capacity = arg3 > sizeof(tcp_bounce) ? sizeof(tcp_bounce) : arg3;
            if (!capacity) return -MINT_EINVAL;
            if (!mint_address_space_validate_user_range(space, arg2, (size_t)capacity, 1)) return -MINT_EFAULT;
            int64_t got = mint_net_tcp_recv((int64_t)arg1, tcp_bounce, (uint32_t)capacity);
            if (got > 0 && !mint_address_space_copy_to_user(space, arg2, tcp_bounce, (size_t)got)) return -MINT_EFAULT;
            return got;
        }
        case MINT_TCP_OP_STATE: return mint_net_tcp_state((int64_t)arg1);
        case MINT_TCP_OP_CLOSE: return mint_net_tcp_close((int64_t)arg1);
        default: return -MINT_EINVAL;
        }
    }
    if (number == MINT_SYS_POWER_INFO) {
        MINT_POWER_INFO local;
        if (!mint_address_space_validate_user_range(space,arg0,sizeof(local),1)) return -MINT_EFAULT;
        if (!mint_power_get_info(&local)) return -MINT_ENODEV;
        return mint_address_space_copy_to_user(space,arg0,&local,sizeof(local)) ? 0 : -MINT_EFAULT;
    }
    if (number == MINT_SYS_POWER_CONTROL) {
        /* Requires the explicit token so a stray call cannot power off. */
        if (arg1 != MINT_POWER_CONFIRM_TOKEN) return -MINT_EPERM;
        if (arg0 == MINT_POWER_ACTION_SHUTDOWN) return mint_power_shutdown();
        if (arg0 == MINT_POWER_ACTION_RESTART) return mint_power_restart();
        return -MINT_EINVAL;
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
        /* 64 x 72-byte records do not belong on the 16 KiB transition stack;
           syscalls are serialized on this single-CPU, cooperative kernel. */
        static MINT_FAT16_DIRENT entries[MINT_FAT16_LIST_MAX];
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
        if (arg3 & ~(uint64_t)MINT_FAT16_READ_PREFIX) return -MINT_EINVAL;
        int64_t got=(arg3 & MINT_FAT16_READ_PREFIX) ? mint_fat16_read_prefix(path,local,(size_t)arg2)
                                                     : mint_fat16_read_file(path,local,(size_t)arg2);
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
    int64_t rc = mint_syscall_dispatch_user(frame->rax, frame->rdi, frame->rsi, frame->rdx,
                                            frame->r10, frame->r8, frame->r9);
    /* 26.5.014.0: a process killed while this syscall slept leaves here. */
    if (rc != (int64_t)MINT_SYSCALL_EXIT_SENTINEL && mint_process_current_killed() &&
        mint_address_space_activate_kernel()) {
        MINT_PROCESS *p = mint_process_current();
        mint_task_user_exit_record(MINT_EXIT_KILLED_BIT | (p ? p->kill_signal : 9u));
        return (int64_t)MINT_SYSCALL_EXIT_SENTINEL;
    }
    return rc;
}
