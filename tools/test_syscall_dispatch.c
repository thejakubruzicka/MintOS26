#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/task.h"
#include "../kernel/include/arch.h"
#include "../kernel/include/serial.h"
#include "../kernel/include/syscall.h"
#include "../kernel/include/address_space.h"
#include "../kernel/include/uapi/mintos/syscall.h"

/* 26.5.014.0 personality: syscalls 0x4D58-0x4D5F are handled in kernel/sound/audio_syscall.c. */
#include "../kernel/include/uapi/mintos/audio.h"
static int personality_syscalls_routed;
int64_t mint_personality_syscall(const MINT_ADDRESS_SPACE *space, uint64_t number, uint64_t a0,
                                 uint64_t a1, uint64_t a2, uint64_t a3);
int64_t mint_personality_syscall(const MINT_ADDRESS_SPACE *space, uint64_t number, uint64_t a0,
                                 uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)space; (void)a0; (void)a1; (void)a2; (void)a3;
    ++personality_syscalls_routed;
    return number >= MINT_SYS_AUDIO_INFO && number <= 0x4D5Fu ? -MINT_ENODEV : -MINT_ENOSYS;
}
#include "../kernel/include/storage_diag.h"
#include "../kernel/include/log.h"
#include "../kernel/include/block.h"
#include "../kernel/include/fat16.h"
#include "../kernel/include/vfs.h"
#include "../kernel/include/pci.h"
#include "../kernel/include/driver_catalog.h"
#include "../kernel/include/e1000.h"


/* 26.5.009.0 / 26.5.010.0 kernel services referenced by syscall.c. */
#include "../kernel/include/power.h"
#include "../kernel/include/rtc.h"
static int64_t mock_rtc_epoch;
int64_t mint_rtc_boot_epoch(void) { return mock_rtc_epoch; }
int mint_power_get_info(MINT_POWER_INFO *out) { if (out) { MINT_POWER_INFO z = {0}; *out = z; out->version = MINT_POWER_INFO_VERSION; } return 1; }
int mint_power_shutdown(void) { return -MINT_ENOSYS; }
int mint_power_restart(void) { return -MINT_ENOSYS; }
/* 26.5.011.0 USB and network services referenced by syscall.c. */
#include "../kernel/include/usb.h"
#include "../kernel/include/net.h"
int mint_usb_get_info(MINT_USB_INFO *out) { if (!out) return 0; memset(out, 0, sizeof(*out)); out->version = MINT_USB_INFO_VERSION; return 1; }
int mint_net_get_info(MINT_NET_INFO *out) { if (!out) return 0; memset(out, 0, sizeof(*out)); out->version = MINT_NET_INFO_VERSION; return 1; }
int64_t mint_net_up(uint32_t mode) { return mode <= MINT_NET_MODE_DHCP ? 0 : -MINT_EINVAL; }
int64_t mint_net_resolve(const char *name, uint32_t *ip_out) { (void)name; (void)ip_out; return -MINT_ENETDOWN; }
int64_t mint_net_ping(uint32_t op, uint32_t ip, uint32_t seq) { (void)op; (void)ip; (void)seq; return -MINT_ENETDOWN; }
int64_t mint_net_tcp_connect(uint32_t ip, uint16_t port) { (void)ip; (void)port; return -MINT_ENETDOWN; }
int64_t mint_net_tcp_send(int64_t h, const uint8_t *d, uint32_t n) { (void)h; (void)d; (void)n; return -MINT_EBADF; }
int64_t mint_net_tcp_recv(int64_t h, uint8_t *o, uint32_t n) { (void)h; (void)o; (void)n; return -MINT_EBADF; }
int64_t mint_net_tcp_state(int64_t h) { (void)h; return -MINT_EBADF; }
int64_t mint_net_tcp_close(int64_t h) { (void)h; return -MINT_EBADF; }
int64_t mint_fat16_read_prefix(const char *path, void *buffer, size_t capacity) { (void)path; (void)buffer; (void)capacity; return -MINT_ENODEV; }
#include "../kernel/include/mintos_fs.h"
int64_t mint_mfs_open(const char *p, uint32_t f, uint64_t pid) { (void)p; (void)f; (void)pid; return -MINT_ENODEV; }
int64_t mint_mfs_read(int64_t fd, void *b, uint32_t n, uint64_t pid) { (void)fd; (void)b; (void)n; (void)pid; return -MINT_EBADF; }
int64_t mint_mfs_write(int64_t fd, const void *b, uint32_t n, uint64_t pid) { (void)fd; (void)b; (void)n; (void)pid; return -MINT_EBADF; }
int64_t mint_mfs_seek(int64_t fd, int64_t o, uint32_t w, uint64_t pid) { (void)fd; (void)o; (void)w; (void)pid; return -MINT_EBADF; }
int64_t mint_mfs_close(int64_t fd, uint64_t pid) { (void)fd; (void)pid; return -MINT_EBADF; }
int64_t mint_mfs_stat(const char *p, MINT_MFS_STAT *o) { (void)p; (void)o; return -MINT_ENODEV; }
int64_t mint_mfs_list(const char *p, uint32_t s, MINT_MFS_DIRENT *o, uint32_t c) { (void)p; (void)s; (void)o; (void)c; return -MINT_ENODEV; }
int64_t mint_mfs_mkdir(const char *p) { (void)p; return -MINT_ENODEV; }
int64_t mint_mfs_rename(const char *a, const char *b) { (void)a; (void)b; return -MINT_ENODEV; }
int64_t mint_mfs_remove(const char *p) { (void)p; return -MINT_ENODEV; }
int64_t mint_mfs_sync(void) { return -MINT_ENODEV; }
int mint_mfs_volume(MINT_MFS_VOLUME *o) { (void)o; return 0; }
int64_t mint_mfs_set_home(const char *p) { (void)p; return -MINT_ENODEV; }
int mint_mintos_disk_initialize(uint32_t i) { (void)i; return -MINT_ENODEV; }
int mint_address_space_map_shared(MINT_ADDRESS_SPACE *s, uint64_t va, uint64_t pa, uint64_t pages, uint32_t f) {
    (void)s; (void)va; (void)pa; (void)pages; (void)f; return 0;
}
int mint_address_space_region_query(const MINT_ADDRESS_SPACE *s, uint64_t a, MINT_ADDRESS_REGION *r) { (void)s; (void)a; (void)r; return 0; }

static char output[2048];
static size_t output_len;
static uint64_t fake_ticks = 12345;
static uint64_t yield_calls;
static uint64_t sleep_calls;
static uint64_t last_sleep_ticks;

static MINT_STORAGE_SNAPSHOT mock_storage_snapshot;
const MINT_STORAGE_SNAPSHOT *mint_storage_diag_snapshot(void) { return &mock_storage_snapshot; }
void mint_driver_catalog_snapshot(MINT_DRIVER_CATALOG *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->version = MINT_DRIVER_CATALOG_VERSION;
}
uint32_t mint_log_read_recent(MINT_LOG_RECORD *out, uint32_t capacity,
                              uint32_t minimum_severity, uint32_t component_filter) {
    (void)out; (void)capacity; (void)minimum_severity; (void)component_filter;
    return 0u;
}
/* New storage services are deliberately unavailable in this legacy trusted-kernel
 * dispatcher fixture. Dedicated tests exercise their production implementations. */
const MINT_BLOCK_DEVICE *mint_block_get(uint32_t index) { (void)index; return 0; }
uint32_t mint_block_count(void) { return 0u; }
int mint_block_io_stats(uint32_t index,MINT_BLOCK_IO_STATS *out) {
    if(index!=0u||!out)return 0;memset(out,0,sizeof(*out));out->version=MINT_BLOCK_IO_STATS_VERSION;return 1;
}
int mint_vfs_mount_table_snapshot(MINT_VFS_MOUNT_TABLE *out) {
    if(!out)return 0;memset(out,0,sizeof(*out));out->version=MINT_VFS_ABI_VERSION;return 1;
}
static MINT_PCI_INVENTORY mock_pci_inventory;
int mint_e1000_init(void) { return 1; }
int mint_e1000_get_info(MINT_E1000_INFO *out) {
    if (!out) return 0;
    memset(out,0,sizeof(*out)); out->version=MINT_E1000_INFO_VERSION;
    out->struct_size=sizeof(*out); out->state=MINT_E1000_ACTIVE;
    out->bus=0; out->device=3; out->function=0; out->mmio_base=0xF0000000ULL;
    out->tx_submitted=2; out->tx_completed=2; out->rx_received=1; return 1;
}
int mint_e1000_loopback_test(void) { return 1; }
const MINT_PCI_INVENTORY *mint_pci_get_inventory(void) { return &mock_pci_inventory; }
int mint_fat16_format_blank(uint32_t block_index) { (void)block_index; return -MINT_ENODEV; }
int64_t mint_fat16_list_dir(const char *path, MINT_FAT16_DIRENT *entries, uint32_t capacity) {
    (void)path; (void)entries; (void)capacity; return -MINT_ENODEV;
}
int64_t mint_fat16_read_file(const char *path, void *buffer, size_t capacity) {
    (void)path; (void)buffer; (void)capacity; return -MINT_ENODEV;
}
int mint_fat16_volume_list(MINT_FAT16_VOLUME_LIST *out) {
    if (!out) return 0; memset(out,0,sizeof(*out)); return 1;
}
int mint_fat16_volume_select(uint32_t ordinal) { (void)ordinal; return 0; }
void mint_storage_diag_recheck(void) { }
void mint_log_emit(uint32_t severity, uint32_t component, const char *message) {
    (void)severity; (void)component; (void)message;
}

void mint_serial_putc(char c) {
    if (output_len + 1 < sizeof(output)) {
        output[output_len++] = c;
        output[output_len] = 0;
    }
}
void mint_serial_write(const char *s) { if (s) while (*s) mint_serial_putc(*s++); }
void mint_serial_init(void) {}
void mint_serial_hex64(uint64_t value) { (void)value; }
void mint_serial_dec64(uint64_t value) { (void)value; }
uint64_t mint_timer_ticks(void) { return fake_ticks; }
int mint_input_keyboard_ready(void) { return 1; }
int mint_input_mouse_ready(void) { return 0; }
int mint_input_ps2_keyboard_online(void) { return 1; }
int mint_input_ps2_mouse_online(void) { return 0; }
void mint_input_get_counters(uint64_t *keyboard, uint64_t *motion, uint64_t *buttons) {
    if (keyboard) *keyboard = 0;
    if (motion) *motion = 0;
    if (buttons) *buttons = 0;
}
uint64_t mint_task_current_pid(void) { return 42; }
void mint_task_yield(void) { yield_calls++; }
void mint_task_sleep_ticks(uint64_t ticks) { sleep_calls++; last_sleep_ticks = ticks; }
_Noreturn void mint_task_exit(void) { abort(); }
static int mock_exit_seen;
static uint64_t mock_exit_code;
void mint_task_user_exit_reset(void) { mock_exit_seen = 0; mock_exit_code = 0; }
void mint_task_user_exit_record(uint64_t code) { mock_exit_seen = 1; mock_exit_code = code; }
int mint_task_user_exit_seen(void) { return mock_exit_seen; }
uint64_t mint_task_user_exit_code(void) { return mock_exit_code; }
/* The legacy trusted-kernel syscall test does not initialize an address space.
 * User-mode symbols are stubbed here; the separate user-syscall test exercises
 * their production implementations against a mock physical-memory backend. */
const MINT_ADDRESS_SPACE *mint_address_space_current(void) { return 0; }
int mint_address_space_validate_user_range(const MINT_ADDRESS_SPACE *space, uint64_t va, size_t n, int wr) { (void)space; (void)va; (void)n; (void)wr; return 0; }
int mint_address_space_copy_from_user(const MINT_ADDRESS_SPACE *space, void *dst, uint64_t src, size_t n) { (void)space; (void)dst; (void)src; (void)n; return 0; }
int mint_address_space_copy_to_user(const MINT_ADDRESS_SPACE *space, uint64_t dst, const void *src, size_t n) { (void)space; (void)dst; (void)src; (void)n; return 0; }
int mint_address_space_activate_kernel(void) { return 0; }

#define EXPECT(cond, name) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", name); return 1; } else { printf("PASS: %s\n", name); } } while (0)

int main(void) {
    const char bytes[] = "mint";
    output_len = 0; output[0] = 0;
    EXPECT(mint_syscall_dispatch(MINT_SYS_WRITE, 1, (uintptr_t)bytes, 4, 0, 0, 0) == 4,
           "write returns the number of bytes written");
    EXPECT(output_len == 4 && memcmp(output, "mint", 4) == 0,
           "write sends exact bytes to the serial console");
    EXPECT(mint_syscall_dispatch(MINT_SYS_WRITE, 7, (uintptr_t)bytes, 4, 0, 0, 0) == -MINT_EBADF,
           "write rejects unsupported file descriptors");
    EXPECT(mint_syscall_dispatch(MINT_SYS_WRITE, 1, 0, 1, 0, 0, 0) == -MINT_EFAULT,
           "write rejects a null non-empty buffer");
    EXPECT(mint_syscall_dispatch(MINT_SYS_WRITE, 1, (uintptr_t)bytes, 513, 0, 0, 0) == -MINT_EINVAL,
           "write enforces its bounded transfer size");
    EXPECT(mint_syscall_dispatch(MINT_SYS_GETPID, 0, 0, 0, 0, 0, 0) == 42,
           "getpid returns the current task ID");
    EXPECT(mint_syscall_dispatch(MINT_SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0) == 0 && yield_calls == 1,
           "sched_yield calls the task yield hook");
    mint_task_sleep_ticks(3u);
    EXPECT(sleep_calls == 1 && last_sleep_ticks == 3u,
           "sleep hook records a bounded scheduler deadline request");
    mint_timespec64 ts = {-1,-1};
    EXPECT(mint_syscall_dispatch(MINT_SYS_CLOCK_GETTIME, MINT_CLOCK_MONOTONIC,
                                 (uintptr_t)&ts, 0, 0, 0, 0) == 0,
           "clock_gettime accepts CLOCK_MONOTONIC");
    EXPECT(ts.tv_sec == 123 && ts.tv_nsec == 450000000,
           "clock_gettime converts 100 Hz ticks to nanoseconds");
    mock_rtc_epoch = 0;
    EXPECT(mint_syscall_dispatch(MINT_SYS_CLOCK_GETTIME, MINT_CLOCK_REALTIME,
                                 (uintptr_t)&ts, 0, 0, 0, 0) == -MINT_ENODEV,
           "clock_gettime(REALTIME) reports ENODEV when the CMOS RTC was invalid");
    mock_rtc_epoch = 1793404800; /* 2026-10-31 00:00:00 */
    EXPECT(mint_syscall_dispatch(MINT_SYS_CLOCK_GETTIME, MINT_CLOCK_REALTIME,
                                 (uintptr_t)&ts, 0, 0, 0, 0) == 0 && ts.tv_sec == 1793404800 + 123,
           "clock_gettime(REALTIME) is the RTC boot epoch plus PIT uptime");
    EXPECT(mint_syscall_dispatch(MINT_SYS_CLOCK_GETTIME, 7, (uintptr_t)&ts, 0, 0, 0, 0) == -MINT_EINVAL,
           "clock_gettime rejects unknown clocks");
    EXPECT(mint_syscall_dispatch(9999, 0, 0, 0, 0, 0, 0) == -MINT_ENOSYS,
           "unknown syscalls return ENOSYS");
    return 0;
}

/* 26.5.014.0 app-platform services reached from syscall.c (process/pipe/
 * memory calls and the kill check on syscall exit). */
#include "../kernel/include/process.h"
int mint_process_current_killed(void) { return 0; }
MINT_PROCESS *mint_process_current(void) { return 0; }
int64_t mint_proc_syscall(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)n; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -MINT_ENOSYS;
}
