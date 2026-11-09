#include "include/mintos/runtime.h"

/* int 0x80 is MintOS's current syscall transport. This small library only
 * wraps implemented calls; reserved syscall numbers remain explicitly absent. */
static inline int64_t mint_syscall3(uint64_t number, uint64_t a0,
                                    uint64_t a1, uint64_t a2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = a0;
    register uint64_t rsi __asm__("rsi") = a1;
    register uint64_t rdx __asm__("rdx") = a2;
    __asm__ volatile("int $0x80"
                     : "+a"(rax)
                     : "D"(rdi), "S"(rsi), "d"(rdx)
                     : "rcx", "r11", "memory", "cc");
    return (int64_t)rax;
}

static inline int64_t mint_syscall5(uint64_t number, uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3, uint64_t a4) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = a0;
    register uint64_t rsi __asm__("rsi") = a1;
    register uint64_t rdx __asm__("rdx") = a2;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    __asm__ volatile("int $0x80"
                     : "+a"(rax)
                     : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory", "cc");
    return (int64_t)rax;
}

static inline int64_t mint_syscall6(uint64_t number, uint64_t a0, uint64_t a1,
                                    uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = a0;
    register uint64_t rsi __asm__("rsi") = a1;
    register uint64_t rdx __asm__("rdx") = a2;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile("int $0x80"
                     : "+a"(rax)
                     : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory", "cc");
    return (int64_t)rax;
}

int64_t mint_write(uint64_t fd, const void *buffer, size_t length) {
    return mint_syscall3(MINT_SYS_WRITE, fd, (uint64_t)(uintptr_t)buffer,
                         (uint64_t)length);
}

int64_t mint_getpid(void) {
    return mint_syscall3(MINT_SYS_GETPID, 0, 0, 0);
}

int64_t mint_sched_yield(void) {
    return mint_syscall3(MINT_SYS_SCHED_YIELD, 0, 0, 0);
}

int64_t mint_sleep_ticks(uint64_t ticks) {
    return mint_syscall3(MINT_SYS_SLEEP_TICKS, ticks, 0, 0);
}

int64_t mint_clock_gettime(int clock_id, mint_timespec64 *out) {
    return mint_syscall3(MINT_SYS_CLOCK_GETTIME, (uint64_t)(int64_t)clock_id,
                         (uint64_t)(uintptr_t)out, 0);
}

int64_t mint_desktop_event_poll(MINT_DESKTOP_EVENT *out) {
    return mint_syscall3(MINT_SYS_DESKTOP_EVENT_POLL, (uint64_t)(uintptr_t)out, 0, 0);
}

int64_t mint_ui_set_wallpaper(uint32_t variant) { return mint_syscall3(MINT_SYS_DISPLAY_SET_WALLPAPER, variant, 0, 0); }
int64_t mint_display_get_info(MINT_DISPLAY_INFO *out) {
    return mint_syscall3(MINT_SYS_DISPLAY_INFO, (uint64_t)(uintptr_t)out, 0, 0);
}

int64_t mint_ui_frame_begin(void) { return mint_syscall3(MINT_SYS_DISPLAY_FRAME_BEGIN, 0, 0, 0); }
int64_t mint_ui_frame_end(void) { return mint_syscall3(MINT_SYS_DISPLAY_FRAME_END, 0, 0, 0); }
int64_t mint_ui_capture_background(void) { return mint_syscall3(MINT_SYS_DISPLAY_BACKGROUND_CAPTURE, 0, 0, 0); }
int64_t mint_ui_restore_rect(int32_t x, int32_t y, int32_t width, int32_t height) {
    return mint_syscall5(MINT_SYS_DISPLAY_RESTORE_RECT, (uint64_t)(int64_t)x,
                         (uint64_t)(int64_t)y, (uint64_t)(int64_t)width,
                         (uint64_t)(int64_t)height, 0);
}

int64_t mint_input_get_info(MINT_INPUT_INFO *out) {
    return mint_syscall3(MINT_SYS_INPUT_INFO, (uint64_t)(uintptr_t)out, 0, 0);
}

int64_t mint_desktop_settings_get(MINT_DESKTOP_SETTINGS *out) {
    return mint_syscall3(MINT_SYS_DESKTOP_SETTINGS_GET, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_desktop_settings_set(const MINT_DESKTOP_SETTINGS *settings) {
    return mint_syscall3(MINT_SYS_DESKTOP_SETTINGS_SET, (uint64_t)(uintptr_t)settings, 0, 0);
}
int64_t mint_fs_open(const char *name, uint32_t flags) {
    return mint_syscall3(MINT_SYS_FS_OPEN, (uint64_t)(uintptr_t)name, flags, 0);
}
int64_t mint_fs_read(uint64_t handle, void *buffer, size_t length) {
    return mint_syscall3(MINT_SYS_FS_READ, handle, (uint64_t)(uintptr_t)buffer, (uint64_t)length);
}
int64_t mint_fs_write(uint64_t handle, const void *buffer, size_t length) {
    return mint_syscall3(MINT_SYS_FS_WRITE, handle, (uint64_t)(uintptr_t)buffer, (uint64_t)length);
}
int64_t mint_fs_close(uint64_t handle) {
    return mint_syscall3(MINT_SYS_FS_CLOSE, handle, 0, 0);
}
int64_t mint_fs_list(MINT_FS_DIRENT *entries, uint32_t capacity) {
    return mint_syscall3(MINT_SYS_FS_LIST, (uint64_t)(uintptr_t)entries, capacity, 0);
}
int64_t mint_storage_diag_get(MINT_STORAGE_SNAPSHOT *out) {
    return mint_syscall3(MINT_SYS_STORAGE_DIAGNOSTICS, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_storage_format_blank(const MINT_STORAGE_FORMAT_REQUEST *request) {
    return mint_syscall3(MINT_SYS_STORAGE_FORMAT_BLANK, (uint64_t)(uintptr_t)request, 0, 0);
}
int64_t mint_fat16_list_dir(const char *path, MINT_FAT16_DIRENT *entries, uint32_t capacity) {
    return mint_syscall3(MINT_SYS_FAT16_LIST, (uint64_t)(uintptr_t)path,
                         (uint64_t)(uintptr_t)entries, capacity);
}
int64_t mint_fat16_read_file(const char *path, void *buffer, size_t capacity) {
    return mint_syscall3(MINT_SYS_FAT16_READ, (uint64_t)(uintptr_t)path,
                         (uint64_t)(uintptr_t)buffer, (uint64_t)capacity);
}
int64_t mint_fat16_get_volumes(MINT_FAT16_VOLUME_LIST *out) {
    return mint_syscall3(MINT_SYS_FAT16_VOLUMES, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_fat16_select_volume(uint32_t ordinal) {
    return mint_syscall3(MINT_SYS_FAT16_SELECT_VOLUME, ordinal, 0, 0);
}
int64_t mint_driver_catalog_get(MINT_DRIVER_CATALOG *out) {
    return mint_syscall3(MINT_SYS_DRIVER_CATALOG, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_block_io_stats_get(uint32_t device_index, MINT_BLOCK_IO_STATS *out) {
    return mint_syscall3(MINT_SYS_BLOCK_IO_STATS, device_index, (uint64_t)(uintptr_t)out, 0);
}
int64_t mint_vfs_mount_table_get(MINT_VFS_MOUNT_TABLE *out) {
    return mint_syscall3(MINT_SYS_VFS_MOUNTS, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_pci_inventory_get(MINT_PCI_INVENTORY *out) {
    return mint_syscall3(MINT_SYS_PCI_INVENTORY, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_log_read(MINT_LOG_RECORD *out, uint32_t capacity,
                      uint32_t minimum_severity, uint32_t component_filter) {
    return mint_syscall6(MINT_SYS_LOG_READ, (uint64_t)(uintptr_t)out, capacity,
                         minimum_severity, component_filter, 0, 0);
}

int64_t mint_ui_fill_rect(int32_t x, int32_t y, int32_t width,
                               int32_t height, uint32_t rgb) {
    return mint_syscall5(MINT_SYS_DISPLAY_FILL_RECT,
                         (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                         (uint64_t)(int64_t)width, (uint64_t)(int64_t)height,
                         (uint64_t)rgb);
}
int64_t mint_ui_blend_round_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                                 uint32_t rgb, uint32_t alpha, uint32_t radius) {
    if (alpha > 255u) alpha = 255u;
    if (radius > 31u) radius = 31u;
    uint64_t packed = ((uint64_t)radius << 8) | alpha;
    return mint_syscall6(MINT_SYS_DISPLAY_BLEND_ROUND_RECT,
                         (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                         (uint64_t)(int64_t)width, (uint64_t)(int64_t)height,
                         (uint64_t)rgb, packed);
}


int64_t mint_ui_draw_bitmap_rgba(const uint8_t *rgba, uint32_t source_width,
                                 uint32_t source_height, int32_t x, int32_t y,
                                 uint32_t target_width, uint32_t target_height) {
    if (!rgba || !source_width || !source_height || source_width > 96u || source_height > 96u ||
        !target_width || !target_height || target_width > 128u || target_height > 128u)
        return -MINT_EINVAL;
    uint64_t source = (uint64_t)source_width | ((uint64_t)source_height << 16);
    uint64_t target = (uint64_t)target_width | ((uint64_t)target_height << 16);
    return mint_syscall6(MINT_SYS_DISPLAY_DRAW_BITMAP, (uint64_t)(uintptr_t)rgba, source,
                         (uint64_t)(int64_t)x, (uint64_t)(int64_t)y, target, 0u);
}

int64_t mint_ui_draw_text(const char *text, size_t length, int32_t x,
                               int32_t y, uint32_t rgb, uint32_t scale) {
    return mint_syscall6(MINT_SYS_DISPLAY_DRAW_TEXT,
                         (uint64_t)(uintptr_t)text, (uint64_t)length,
                         (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                         (uint64_t)rgb, (uint64_t)scale);
}

__attribute__((noreturn)) void mint_exit(uint64_t status) {
    (void)mint_syscall3(MINT_SYS_EXIT, status, 0, 0);
    for (;;) __asm__ volatile("ud2");
}

int64_t mint_e1000_start(void) {
    return mint_syscall3(MINT_SYS_E1000_START, 0, 0, 0);
}
int64_t mint_e1000_get_status(MINT_E1000_INFO *out) {
    return mint_syscall3(MINT_SYS_E1000_INFO, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_e1000_run_loopback_test(void) {
    return mint_syscall3(MINT_SYS_E1000_LOOPBACK_TEST, 0, 0, 0);
}

int64_t mint_power_get_status(MINT_POWER_INFO *out) {
    return mint_syscall3(MINT_SYS_POWER_INFO, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_power_request(uint32_t action) {
    return mint_syscall3(MINT_SYS_POWER_CONTROL, action, MINT_POWER_CONFIRM_TOKEN, 0);
}

int64_t mint_display_map_canvas(MINT_DISPLAY_CANVAS *out) {
    return mint_syscall3(MINT_SYS_DISPLAY_MAP_CANVAS, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_display_present(int32_t x, int32_t y, int32_t width, int32_t height) {
    return mint_syscall5(MINT_SYS_DISPLAY_PRESENT, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                         (uint64_t)(int64_t)width, (uint64_t)(int64_t)height, 0);
}
int64_t mint_display_cursor(int32_t x, int32_t y, uint32_t shape) {
    return mint_syscall3(MINT_SYS_DISPLAY_CURSOR, (uint64_t)(int64_t)x, (uint64_t)(int64_t)y, shape);
}
int64_t mint_ui_render_wallpaper(uint32_t variant) {
    return mint_syscall3(MINT_SYS_DISPLAY_WALLPAPER_RENDER, variant, 0, 0);
}
int64_t mint_fat16_read_prefix(const char *path, void *buffer, size_t capacity) {
    return mint_syscall5(MINT_SYS_FAT16_READ, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buffer,
                         (uint64_t)capacity, MINT_FAT16_READ_PREFIX, 0);
}
int64_t mint_usb_get_status(MINT_USB_INFO *out) {
    return mint_syscall3(MINT_SYS_USB_INFO, (uint64_t)(uintptr_t)out, 0, 0);
}
int64_t mint_net_get_status(MINT_NET_INFO *out) { return mint_syscall3(MINT_SYS_NET_INFO, (uint64_t)(uintptr_t)out, 0, 0); }
int64_t mint_net_set_up(uint32_t mode) { return mint_syscall3(MINT_SYS_NET_UP, mode, 0, 0); }
int64_t mint_net_resolve_host(const char *name, uint32_t *ip_out) {
    return mint_syscall3(MINT_SYS_NET_RESOLVE, (uint64_t)(uintptr_t)name, (uint64_t)(uintptr_t)ip_out, 0);
}
int64_t mint_net_ping_send(uint32_t ip, uint32_t seq) { return mint_syscall3(MINT_SYS_NET_PING, MINT_NET_PING_SEND, ip, seq); }
int64_t mint_net_ping_poll(uint32_t seq) { return mint_syscall3(MINT_SYS_NET_PING, MINT_NET_PING_POLL, 0, seq); }
int64_t mint_tcp_connect(uint32_t ip, uint16_t port) { return mint_syscall5(MINT_SYS_TCP, MINT_TCP_OP_CONNECT, ip, port, 0, 0); }
int64_t mint_tcp_send(int64_t handle, const void *data, size_t length) {
    return mint_syscall5(MINT_SYS_TCP, MINT_TCP_OP_SEND, (uint64_t)handle, (uint64_t)(uintptr_t)data, length, 0);
}
int64_t mint_tcp_recv(int64_t handle, void *out, size_t capacity) {
    return mint_syscall5(MINT_SYS_TCP, MINT_TCP_OP_RECV, (uint64_t)handle, (uint64_t)(uintptr_t)out, capacity, 0);
}
int64_t mint_tcp_state(int64_t handle) { return mint_syscall3(MINT_SYS_TCP, MINT_TCP_OP_STATE, (uint64_t)handle, 0); }
int64_t mint_tcp_close(int64_t handle) { return mint_syscall3(MINT_SYS_TCP, MINT_TCP_OP_CLOSE, (uint64_t)handle, 0); }
