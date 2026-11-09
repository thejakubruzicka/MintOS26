#ifndef MINTOS26_USER_RUNTIME_H
#define MINTOS26_USER_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/mintos/syscall.h>
#include <desktop_abi.h>
#include <ramfs.h>
#include <storage_diag.h>
#include <fat16.h>
#include <driver_catalog.h>
#include <log.h>
#include <block.h>
#include <vfs.h>
#include <pci.h>
#include <e1000.h>
#define MINT_POWER_USERSPACE 1
#include <power.h>
#define MINT_USB_USERSPACE 1
#include <usb.h>
#define MINT_NET_USERSPACE 1
#include <net.h>

/* Tiny freestanding userspace ABI. It is deliberately not a libc or Linux ABI. */
int64_t mint_write(uint64_t fd, const void *buffer, size_t length);
int64_t mint_getpid(void);
int64_t mint_sched_yield(void);
int64_t mint_sleep_ticks(uint64_t ticks);
int64_t mint_clock_gettime(int clock_id, mint_timespec64 *out);
int64_t mint_desktop_event_poll(MINT_DESKTOP_EVENT *out);
int64_t mint_display_get_info(MINT_DISPLAY_INFO *out);
int64_t mint_ui_set_wallpaper(uint32_t variant);
int64_t mint_input_get_info(MINT_INPUT_INFO *out);
int64_t mint_desktop_settings_get(MINT_DESKTOP_SETTINGS *out);
int64_t mint_desktop_settings_set(const MINT_DESKTOP_SETTINGS *settings);
int64_t mint_fs_open(const char *name, uint32_t flags);
int64_t mint_fs_read(uint64_t handle, void *buffer, size_t length);
int64_t mint_fs_write(uint64_t handle, const void *buffer, size_t length);
int64_t mint_fs_close(uint64_t handle);
int64_t mint_fs_list(MINT_FS_DIRENT *entries, uint32_t capacity);
int64_t mint_storage_diag_get(MINT_STORAGE_SNAPSHOT *out);
int64_t mint_storage_format_blank(const MINT_STORAGE_FORMAT_REQUEST *request);
int64_t mint_fat16_list_dir(const char *path, MINT_FAT16_DIRENT *entries, uint32_t capacity);
int64_t mint_fat16_read_file(const char *path, void *buffer, size_t capacity);
/* First `capacity` bytes of a file (previews); never fails just for size. */
int64_t mint_fat16_read_prefix(const char *path, void *buffer, size_t capacity);
int64_t mint_fat16_get_volumes(MINT_FAT16_VOLUME_LIST *out);
int64_t mint_fat16_select_volume(uint32_t ordinal);
int64_t mint_driver_catalog_get(MINT_DRIVER_CATALOG *out);
int64_t mint_block_io_stats_get(uint32_t device_index, MINT_BLOCK_IO_STATS *out);
int64_t mint_vfs_mount_table_get(MINT_VFS_MOUNT_TABLE *out);
int64_t mint_pci_inventory_get(MINT_PCI_INVENTORY *out);
int64_t mint_e1000_start(void);
int64_t mint_e1000_get_status(MINT_E1000_INFO *out);
int64_t mint_e1000_run_loopback_test(void);
/* ACPI power: status snapshot, and confirmed shutdown/restart (returns only
 * on failure with a negative MintOS errno). */
int64_t mint_power_get_status(MINT_POWER_INFO *out);
int64_t mint_power_request(uint32_t action);
/* MintDE 2 shared-canvas display (see desktop_abi.h). */
int64_t mint_display_map_canvas(MINT_DISPLAY_CANVAS *out);
int64_t mint_display_present(int32_t x, int32_t y, int32_t width, int32_t height);
int64_t mint_display_cursor(int32_t x, int32_t y, uint32_t shape);
int64_t mint_ui_render_wallpaper(uint32_t variant);
int64_t mint_usb_get_status(MINT_USB_INFO *out);
/* Networking (see net.h): non-blocking; -MINT_EAGAIN means "ask again later". */
int64_t mint_net_get_status(MINT_NET_INFO *out);
int64_t mint_net_set_up(uint32_t mode);
int64_t mint_net_resolve_host(const char *name, uint32_t *ip_out);
int64_t mint_net_ping_send(uint32_t ip, uint32_t seq);
int64_t mint_net_ping_poll(uint32_t seq);
int64_t mint_tcp_connect(uint32_t ip, uint16_t port);
int64_t mint_tcp_send(int64_t handle, const void *data, size_t length);
int64_t mint_tcp_recv(int64_t handle, void *out, size_t capacity);
int64_t mint_tcp_state(int64_t handle);
int64_t mint_tcp_close(int64_t handle);
int64_t mint_log_read(MINT_LOG_RECORD *out, uint32_t capacity, uint32_t minimum_severity, uint32_t component_filter);
int64_t mint_ui_frame_begin(void);
int64_t mint_ui_frame_end(void);
int64_t mint_ui_capture_background(void);
int64_t mint_ui_restore_rect(int32_t x, int32_t y, int32_t width, int32_t height);
int64_t mint_ui_fill_rect(int32_t x, int32_t y, int32_t width,
                               int32_t height, uint32_t rgb);
int64_t mint_ui_blend_round_rect(int32_t x, int32_t y, int32_t width, int32_t height,
                                 uint32_t rgb, uint32_t alpha, uint32_t radius);
int64_t mint_ui_draw_bitmap_rgba(const uint8_t *rgba, uint32_t source_width,
                                 uint32_t source_height, int32_t x, int32_t y,
                                 uint32_t target_width, uint32_t target_height);
int64_t mint_ui_draw_text(const char *text, size_t length, int32_t x,
                               int32_t y, uint32_t rgb, uint32_t scale);
__attribute__((noreturn)) void mint_exit(uint64_t status);

#endif
