#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../kernel/include/address_space.h"
#include "../kernel/include/arch.h"
#include "../kernel/include/task.h"
#include "../kernel/include/serial.h"
#include "../kernel/include/syscall.h"
#include "../kernel/include/uapi/mintos/syscall.h"
#include "../kernel/include/desktop_abi.h"
#include "../kernel/include/desktop_services.h"
#include "../kernel/include/ramfs.h"
#include "../kernel/include/storage_diag.h"
#include "../kernel/include/log.h"
#include "../kernel/include/driver_catalog.h"
#include "../kernel/include/e1000.h"
#include "../kernel/include/pci.h"
#include "../kernel/include/block.h"
#include "../kernel/include/fat16.h"
#include "../kernel/include/vfs.h"

#define MOCK_PAGE_COUNT 1024u
#define MOCK_PHYS_BASE 0x00100000ULL
#define USER_CODE_VA (MINT_ASPACE_USER_BASE + 0x00200000ULL)
#define USER_DATA_VA (USER_CODE_VA + MINT_PAGING_PAGE_SIZE)
#define BAD_USER_VA  (USER_CODE_VA + 0x00300000ULL)
#define USER_DATA_BYTES 16384u

typedef struct mock_page { uint64_t physical; int active; uint8_t bytes[4096]; } MOCK_PAGE;
static MOCK_PAGE pages[MOCK_PAGE_COUNT];
static uint64_t current_root;
static uint64_t shared_low[MINT_ASPACE_SHARED_PDPT_ENTRIES];
static uint64_t fake_ticks = 12345;
static uint64_t yield_calls;
static uint64_t sleep_calls;
static uint64_t last_sleep_ticks;
static int mock_exit_seen;
static uint64_t mock_exit_code;
static char output[2048];
static size_t output_length;
static uint32_t display_pixels[640u * 480u];
static MINT_STORAGE_SNAPSHOT test_storage_snapshot;
static MINT_PCI_INVENTORY test_pci_inventory;
static MINT_LOG_RECORD test_log_records[2];
static uint32_t test_log_count;
static MINT_BLOCK_DEVICE test_format_device;
static uint32_t test_format_calls;
static uint32_t test_storage_rechecks;

const MINT_STORAGE_SNAPSHOT *mint_storage_diag_snapshot(void) { return &test_storage_snapshot; }
const MINT_BLOCK_DEVICE *mint_block_get(uint32_t index) { return index==0u ? &test_format_device : 0; }
uint32_t mint_block_count(void) { return 1u; }
int mint_block_io_stats(uint32_t index,MINT_BLOCK_IO_STATS *out) {
    if(index!=0u||!out)return 0;memset(out,0,sizeof(*out));out->version=MINT_BLOCK_IO_STATS_VERSION;
    out->device_index=index;out->read_calls=12u;out->read_success=11u;out->read_failures=1u;out->writes_blocked=3u;return 1;
}
int mint_vfs_mount_table_snapshot(MINT_VFS_MOUNT_TABLE *out) {
    if(!out)return 0;memset(out,0,sizeof(*out));out->version=MINT_VFS_ABI_VERSION;out->count=3u;
    memcpy(out->mounts[0].path,"/session",sizeof("/session"));memcpy(out->mounts[0].source,"ramfs",sizeof("ramfs"));
    memcpy(out->mounts[0].filesystem,"RAMFS",sizeof("RAMFS"));out->mounts[0].flags=MINT_VFS_FLAG_VOLATILE;
    memcpy(out->mounts[1].path,"/Volumes/sda",sizeof("/Volumes/sda"));memcpy(out->mounts[1].source,"sda",sizeof("sda"));
    memcpy(out->mounts[1].filesystem,"FAT16",sizeof("FAT16"));out->mounts[1].flags=MINT_VFS_FLAG_READ_ONLY;out->mounts[1].block_index=0u;
    memcpy(out->mounts[2].path,"/Volumes/sdb",sizeof("/Volumes/sdb"));memcpy(out->mounts[2].source,"sdb",sizeof("sdb"));
    memcpy(out->mounts[2].filesystem,"FAT16",sizeof("FAT16"));out->mounts[2].flags=MINT_VFS_FLAG_READ_ONLY;out->mounts[2].block_index=1u;return 1;
}
static int test_block_read(void *ctx,uint64_t lba,uint8_t out[512]) { (void)ctx;(void)lba;(void)out;return 1; }
static int test_block_write(void *ctx,uint64_t lba,const uint8_t in[512]) { (void)ctx;(void)lba;(void)in;return 1; }
static int test_block_flush(void *ctx) { (void)ctx;return 1; }
int mint_fat16_format_blank(uint32_t block_index) { if(block_index!=0u)return -MINT_EINVAL;test_format_calls++;return 0; }
int64_t mint_fat16_list_dir(const char *path,MINT_FAT16_DIRENT *entries,uint32_t capacity) { (void)path;(void)entries;(void)capacity;return -MINT_ENODEV; }
int64_t mint_fat16_read_file(const char *path,void *buffer,size_t capacity) { (void)path;(void)buffer;(void)capacity;return -MINT_ENODEV; }
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

int mint_fat16_volume_list(MINT_FAT16_VOLUME_LIST *out) { if(!out)return 0; memset(out,0,sizeof(*out)); return 1; }
int mint_fat16_volume_select(uint32_t ordinal) { (void)ordinal; return 0; }
void mint_storage_diag_recheck(void) { test_storage_rechecks++; }
void mint_log_emit(uint32_t severity,uint32_t component,const char *message) { (void)severity;(void)component;(void)message; }
int mint_e1000_init(void) { return 1; }
int mint_e1000_get_info(MINT_E1000_INFO *out) {
    if (!out) return 0;
    memset(out,0,sizeof(*out)); out->version=MINT_E1000_INFO_VERSION;
    out->struct_size=sizeof(*out); out->state=MINT_E1000_ACTIVE;
    out->bus=0; out->device=3; out->function=0; out->mmio_base=0xF0000000ULL;
    out->tx_submitted=2; out->tx_completed=2; out->rx_received=1; return 1;
}
int mint_e1000_loopback_test(void) { return 1; }
const MINT_PCI_INVENTORY *mint_pci_get_inventory(void) { return &test_pci_inventory; }
uint32_t mint_log_read_recent(MINT_LOG_RECORD *out, uint32_t capacity,
                              uint32_t minimum_severity, uint32_t component_filter) {
    if (!out || !capacity) return 0u;
    if (capacity > MINT_LOG_READ_MAX) capacity = MINT_LOG_READ_MAX;
    uint32_t written = 0u;
    for (uint32_t i = 0; i < test_log_count && written < capacity; ++i) {
        if (test_log_records[i].severity < minimum_severity ||
            (component_filter && test_log_records[i].component != component_filter)) continue;
        out[written++] = test_log_records[i];
    }
    return written;
}

static uint64_t mock_claim(void *ctx) {
    (void)ctx;
    for (unsigned i=0;i<MOCK_PAGE_COUNT;i++) if (!pages[i].active) {
        pages[i].active=1; pages[i].physical=MOCK_PHYS_BASE+(uint64_t)i*4096ULL;
        memset(pages[i].bytes,0,sizeof(pages[i].bytes)); return pages[i].physical;
    }
    return 0;
}
static int mock_release(void *ctx,uint64_t pa) {
    (void)ctx; for(unsigned i=0;i<MOCK_PAGE_COUNT;i++) if(pages[i].active&&pages[i].physical==pa){pages[i].active=0;return 1;} return 0;
}
static void *mock_pointer(void *ctx,uint64_t pa) {
    (void)ctx; if(pa&4095u)return 0; for(unsigned i=0;i<MOCK_PAGE_COUNT;i++)if(pages[i].active&&pages[i].physical==pa)return pages[i].bytes; return 0;
}
static int mock_claimed(void *ctx,uint64_t pa){(void)ctx;for(unsigned i=0;i<MOCK_PAGE_COUNT;i++)if(pages[i].active&&pages[i].physical==pa)return 1;return 0;}
static void mock_load_root(void *ctx,uint64_t pa){(void)ctx;current_root=pa;}
static void mock_invlpg(void *ctx,uint64_t va){(void)ctx;(void)va;}
static unsigned active_pages(void){unsigned n=0;for(unsigned i=0;i<MOCK_PAGE_COUNT;i++)n+=(pages[i].active!=0);return n;}
void mint_serial_putc(char c){if(output_length+1<sizeof(output)){output[output_length++]=c;output[output_length]=0;}}
void mint_serial_write(const char*s){if(s)while(*s)mint_serial_putc(*s++);}
void mint_serial_init(void){}
void mint_serial_hex64(uint64_t v){(void)v;}
void mint_serial_dec64(uint64_t v){(void)v;}
uint64_t mint_timer_ticks(void){return fake_ticks;}
int mint_input_keyboard_ready(void){return 1;}
int mint_input_mouse_ready(void){return 1;}
int mint_input_ps2_keyboard_online(void){return 1;}
int mint_input_ps2_mouse_online(void){return 1;}
void mint_input_get_counters(uint64_t *keyboard,uint64_t *motion,uint64_t *buttons){
    if (keyboard) *keyboard=11;
    if (motion) *motion=22;
    if (buttons) *buttons=3;
}
uint64_t mint_task_current_pid(void){return 42;}
void mint_task_yield(void){++yield_calls;}
void mint_task_sleep_ticks(uint64_t ticks){++sleep_calls;last_sleep_ticks=ticks;}
_Noreturn void mint_task_exit(void){abort();}
void mint_task_user_exit_reset(void){mock_exit_seen=0;mock_exit_code=0;}
void mint_task_user_exit_record(uint64_t code){mock_exit_seen=1;mock_exit_code=code;}
int mint_task_user_exit_seen(void){return mock_exit_seen;}
uint64_t mint_task_user_exit_code(void){return mock_exit_code;}

#define CHECK(c,label) do{if(!(c)){fprintf(stderr,"FAIL: %s\n",label);return 1;}puts("PASS: " label);}while(0)

int main(void){
    MINT_ADDRESS_SPACE_BACKEND backend={0,mock_claim,mock_release,mock_pointer,mock_claimed,mock_load_root,mock_invlpg};
    for(unsigned i=0;i<MINT_ASPACE_SHARED_PDPT_ENTRIES;i++) shared_low[i]=(0x40000000ULL+(uint64_t)i*4096ULL)|0x3ULL;
    CHECK(mint_address_space_system_init(&backend,0x00800000ULL,shared_low,0x200003ULL,1),"initialize production address-space manager");
    CHECK(mint_desktop_core_init(),"initialize kernel desktop event service");
    test_storage_snapshot.version = MINT_STORAGE_DIAG_VERSION;
    test_pci_inventory.devices_stored = 3u;
    test_pci_inventory.devices_found = 3u;
    test_pci_inventory.devices[0].vendor_id = 0x8086u;
    test_pci_inventory.devices[0].device_id = 0x100Eu;
    test_pci_inventory.devices[0].class_code = 0x02u;
    test_pci_inventory.devices[0].subclass = 0u;
    test_pci_inventory.devices[1].vendor_id = 0x15ADu;
    test_pci_inventory.devices[1].device_id = 0x0405u;
    test_pci_inventory.devices[1].class_code = 0x03u;
    test_pci_inventory.devices[1].subclass = 0u;
    test_pci_inventory.devices[2].vendor_id = 0x8086u;
    test_pci_inventory.devices[2].device_id = 0x265Cu;
    test_pci_inventory.devices[2].class_code = 0x0Cu;
    test_pci_inventory.devices[2].subclass = 0x03u;
    test_storage_snapshot.probe_complete = 1u;
    test_storage_snapshot.controller_count = 1u;
    test_storage_snapshot.device_count = 1u;
    test_storage_snapshot.persistent_mount_active = 0u;
    test_storage_snapshot.writes_attempted = 0u;
    memcpy(test_storage_snapshot.devices[0].name, "sda", sizeof("sda"));
    memcpy(test_storage_snapshot.devices[0].serial, "TEST-SERIAL", sizeof("TEST-SERIAL"));
    test_storage_snapshot.devices[0].sector_count = 1048576u;
    test_storage_snapshot.devices[0].block_index = 0u;
    test_storage_snapshot.devices[0].block_registered = 1u;
    memcpy(test_storage_snapshot.devices[0].mount_status, "NOT MOUNTED", sizeof("NOT MOUNTED"));
    memcpy(test_format_device.name,"sda",sizeof("sda"));
    test_format_device.sector_count=1048576u; test_format_device.sector_size=512u;
    test_format_device.flags=MINT_BLOCK_FLAG_READONLY; test_format_device.read_sector=test_block_read;
    test_format_device.write_sector=test_block_write; test_format_device.flush=test_block_flush;
    test_log_count = 1u;
    test_log_records[0].sequence = 77u;
    test_log_records[0].severity = MINT_LOG_WARN;
    test_log_records[0].component = MINT_LOG_COMPONENT_AHCI;
    memcpy(test_log_records[0].message, "test AHCI warning", sizeof("test AHCI warning"));
    mint_desktop_settings_init();
    mint_ramfs_init();
    MINT_DESKTOP_EVENT queued={0}; queued.type=MINT_DESKTOP_EVENT_APP_REQUEST;
    queued.data.app.app_id=0x4D494E54u; queued.data.app.argument=0x00260053u;
    CHECK(mint_desktop_event_post(&queued),"queue synthetic desktop-service event");
    MINT_FRAMEBUFFER_INFO framebuffer={(uint64_t)(uintptr_t)display_pixels,sizeof(display_pixels),640,480,640,1};
    MINT_DISPLAY_SURFACE surface={0};
    CHECK(mint_display_surface_init(&surface,&framebuffer),"register safe display metadata service");
    static uint32_t display_back[640u * 480u], display_backdrop[640u * 480u];
    CHECK(mint_display_attach_backbuffers(display_back,display_backdrop,640u * 480u),"attach per-mode display backbuffers as the kernel does");
    MINT_ADDRESS_SPACE space={0};
    CHECK(mint_address_space_create(&space),"create isolated test address space");
    CHECK(mint_address_space_region_reserve(&space,USER_CODE_VA,4096,MINT_PAGE_MAP_EXEC,MINT_ASPACE_REGION_IMAGE_TEXT),"reserve RX code region");
    CHECK(mint_address_space_region_reserve(&space,USER_DATA_VA,USER_DATA_BYTES,MINT_PAGE_MAP_WRITE,MINT_ASPACE_REGION_IMAGE_DATA),"reserve RW data region");
    uint64_t code=mint_address_space_alloc_frame(&space), data_frames[3];
    data_frames[0]=mint_address_space_alloc_frame(&space);
    data_frames[1]=mint_address_space_alloc_frame(&space);
    data_frames[2]=mint_address_space_alloc_frame(&space);
    CHECK(code&&data_frames[0]&&data_frames[1]&&data_frames[2]&&code!=data_frames[0],"allocate private code and data frames");
    const char msg[]="mint";
    memcpy(mock_pointer(0,code),msg,sizeof(msg));
    int mapped = mint_address_space_map(&space,USER_CODE_VA,code,MINT_PAGE_MAP_EXEC);
    for (uint32_t i = 0; i < 3u; ++i)
        mapped = mapped && mint_address_space_map(&space,USER_DATA_VA+(uint64_t)i*4096u,data_frames[i],MINT_PAGE_MAP_WRITE);
    CHECK(mapped,"map RX code and multi-page RW userspace data region");
    CHECK(mint_address_space_activate(&space)&&current_root==space.root_physical,"activate user address space");
    output_length=0;output[0]=0;
    CHECK(mint_syscall_dispatch_user(MINT_SYS_WRITE,1,USER_CODE_VA,4,0,0,0)==4,"user write accepts a mapped read-only code buffer");
    CHECK(output_length==4&&memcmp(output,"mint",4)==0,"user write copies from user memory into kernel-owned buffer");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_WRITE,1,BAD_USER_VA,1,0,0,0)==-MINT_EFAULT,"unmapped user buffer returns EFAULT without dereference");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_WRITE,1,USER_CODE_VA,513,0,0,0)==-MINT_EINVAL,"user write enforces transfer limit");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_GETPID,0,0,0,0,0,0)==42,"user getpid returns current task ID");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_STORAGE_DIAGNOSTICS,USER_DATA_VA,0,0,0,0,0)==0,
          "storage diagnostic syscall copies a bounded snapshot to userspace");
    MINT_STORAGE_SNAPSHOT received_storage={0};
    CHECK(mint_address_space_copy_from_user(&space,&received_storage,USER_DATA_VA,sizeof(received_storage)) &&
          received_storage.version==MINT_STORAGE_DIAG_VERSION && received_storage.probe_complete==1u &&
          received_storage.device_count==1u && received_storage.devices[0].sector_count==1048576u &&
          received_storage.persistent_mount_active==0u && received_storage.writes_attempted==0u,
          "userspace sees device capacity and truthful not-mounted/write-disabled state");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_BLOCK_IO_STATS,0u,USER_DATA_VA,0,0,0,0)==0,
          "block I/O stats syscall returns bounded per-device counters");
    MINT_BLOCK_IO_STATS received_stats={0};
    CHECK(mint_address_space_copy_from_user(&space,&received_stats,USER_DATA_VA,sizeof(received_stats)) &&
          received_stats.device_index==0u && received_stats.read_success==11u && received_stats.writes_blocked==3u,
          "userspace receives read success/failure and blocked-write statistics");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_BLOCK_IO_STATS,0u,USER_CODE_VA,0,0,0,0)==-MINT_EFAULT,
          "block I/O stats syscall rejects a read-only user destination");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_VFS_MOUNTS,USER_DATA_VA,0,0,0,0,0)==0,
          "VFS mount-table syscall copies bounded mount metadata");
    MINT_VFS_MOUNT_TABLE received_mounts={0};
    CHECK(mint_address_space_copy_from_user(&space,&received_mounts,USER_DATA_VA,sizeof(received_mounts)) &&
          received_mounts.count==3u && strcmp(received_mounts.mounts[1].path,"/Volumes/sda")==0 &&
          strcmp(received_mounts.mounts[2].path,"/Volumes/sdb")==0,
          "userspace can distinguish multiple volumes by stable mount path");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_PCI_INVENTORY,USER_DATA_VA,0,0,0,0,0)==0,
          "PCI inventory syscall copies a bounded device snapshot");
    MINT_PCI_INVENTORY received_pci={0};
    CHECK(mint_address_space_copy_from_user(&space,&received_pci,USER_DATA_VA,sizeof(received_pci)) &&
          received_pci.devices_stored==3u && received_pci.devices[1].vendor_id==0x15ADu,
          "userspace receives the PCI inventory without direct port I/O");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_STORAGE_DIAGNOSTICS,USER_CODE_VA,0,0,0,0,0)==-MINT_EFAULT,
          "storage diagnostic syscall rejects a read-only userspace destination");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_LOG_READ,USER_CODE_VA,1,MINT_LOG_WARN,MINT_LOG_COMPONENT_AHCI,0,0)==-MINT_EFAULT,
          "log read rejects a read-only userspace output buffer");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_LOG_READ,USER_DATA_VA,1,MINT_LOG_WARN,MINT_LOG_COMPONENT_AHCI,0,0)==1,
          "log read returns a bounded severity/component-filtered record");
    MINT_LOG_RECORD received_log={0};
    CHECK(mint_address_space_copy_from_user(&space,&received_log,USER_DATA_VA,sizeof(received_log)) &&
          received_log.sequence==77u && received_log.severity==MINT_LOG_WARN &&
          received_log.component==MINT_LOG_COMPONENT_AHCI && strcmp(received_log.message,"test AHCI warning")==0,
          "userspace receives intact structured log record");
    MINT_STORAGE_FORMAT_REQUEST format_request={0};
    format_request.version=MINT_STORAGE_FORMAT_REQUEST_VERSION;
    format_request.device_index=0u; format_request.expected_sector_count=1048576u;
    memcpy(format_request.expected_serial,"TEST-SERIAL",sizeof("TEST-SERIAL"));
    memcpy(format_request.confirmation,"format sda",sizeof("format sda"));
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA,&format_request,sizeof(format_request)) &&
          mint_syscall_dispatch_user(MINT_SYS_STORAGE_FORMAT_BLANK,USER_DATA_VA,0,0,0,0,0)==-MINT_EPERM &&
          test_format_calls==0u,
          "blank-disk formatter syscall rejects an incorrect confirmation without invoking formatter");
    memcpy(format_request.confirmation,"FORMAT sda",sizeof("FORMAT sda"));
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA,&format_request,sizeof(format_request)) &&
          mint_syscall_dispatch_user(MINT_SYS_STORAGE_FORMAT_BLANK,USER_DATA_VA,0,0,0,0,0)==0 &&
          test_format_calls==1u && test_storage_rechecks==1u,
          "blank-disk formatter syscall requires matching device identity/capacity and exact typed confirmation");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_LOG_READ,USER_DATA_VA,MINT_LOG_READ_MAX+1u,MINT_LOG_WARN,0,0,0)==-MINT_EINVAL,
          "log read rejects requests beyond its fixed maximum");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_CLOCK_GETTIME,MINT_CLOCK_MONOTONIC,USER_DATA_VA,0,0,0,0)==0,"user clock_gettime writes through validated user copy");
    mint_timespec64 ts={0,0};
    CHECK(mint_address_space_copy_from_user(&space,&ts,USER_DATA_VA,sizeof(ts))&&ts.tv_sec==123&&ts.tv_nsec==450000000,"clock_gettime wrote expected time into user data page");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_CLOCK_GETTIME,MINT_CLOCK_MONOTONIC,USER_CODE_VA,0,0,0,0)==-MINT_EFAULT,"clock_gettime rejects a read-only code destination");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_SCHED_YIELD,0,0,0,0,0,0)==0&&yield_calls==1,"user sched_yield dispatches through the task scheduler");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_SLEEP_TICKS,2,0,0,0,0,0)==0&&sleep_calls==1&&last_sleep_ticks==2,
          "user sleep-ticks syscall blocks through the task scheduler");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_SLEEP_TICKS,0,0,0,0,0,0)==-MINT_EINVAL,
          "user sleep-ticks rejects a zero interval");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_SLEEP_TICKS,1001,0,0,0,0,0)==-MINT_EINVAL,
          "user sleep-ticks rejects unbounded intervals");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DESKTOP_EVENT_POLL,USER_CODE_VA,0,0,0,0,0)==-MINT_EFAULT,"desktop event poll rejects read-only user output without consuming event");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DESKTOP_EVENT_POLL,USER_DATA_VA,0,0,0,0,0)==1,"desktop event poll copies one queued event into validated user memory");
    MINT_DESKTOP_EVENT received={0};
    CHECK(mint_address_space_copy_from_user(&space,&received,USER_DATA_VA,sizeof(received))&&received.type==MINT_DESKTOP_EVENT_APP_REQUEST&&received.data.app.app_id==0x4D494E54u,"userspace receives event payload and type intact");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DESKTOP_EVENT_POLL,USER_DATA_VA,0,0,0,0,0)==0,"desktop event poll reports empty queue without blocking");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_INFO,USER_CODE_VA,0,0,0,0,0)==-MINT_EFAULT,"display-info rejects read-only user output");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_INFO,USER_DATA_VA,0,0,0,0,0)==0,"display-info copies current display metadata to userspace");
    MINT_DISPLAY_INFO display={0};
    CHECK(mint_address_space_copy_from_user(&space,&display,USER_DATA_VA,sizeof(display))&&display.width==640&&display.height==480&&display.stride_pixels==640,"userspace receives safe display dimensions without a framebuffer pointer");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FILL_RECT,10,10,12,8,0x00112233u,0)==0,
          "display rectangle syscall accepts bounded in-range draw command");
    CHECK(display_pixels[10u*640u+10u]==0x00112233u&&display_pixels[17u*640u+21u]==0x00112233u,
          "display rectangle syscall draws through kernel-owned surface");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FILL_RECT,10,10,0,8,0x00112233u,0)==-MINT_EINVAL,
          "display rectangle syscall rejects zero width");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FILL_RECT,10,10,16385,8,0x00112233u,0)==-MINT_EINVAL,
          "display rectangle syscall rejects oversized width");
    const char ui_text[]="MINT";
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA,ui_text,sizeof(ui_text)-1u),
          "prepare valid text buffer in writable userspace memory");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_DRAW_TEXT,USER_DATA_VA,4,40,40,0x00ABCDEFu,2)==0,
          "display text syscall copies bounded text and validates scale");
    size_t text_ink = 0, text_edge = 0;
    for (uint32_t ty = 40u; ty < 70u; ++ty) {
        for (uint32_t tx = 40u; tx < 128u; ++tx) {
            uint32_t px = display_pixels[ty * 640u + tx];
            if (px) ++text_ink;
            if (px && px != 0x00ABCDEFu) ++text_edge;
        }
    }
    CHECK(text_ink > 0 && text_edge > 0,
          "display text syscall rasterizes anti-aliased glyphs into registered surface");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_DRAW_TEXT,BAD_USER_VA,4,40,40,0x00ABCDEFu,2)==-MINT_EFAULT,
          "display text syscall rejects unmapped userspace string");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_DRAW_TEXT,USER_DATA_VA,129,40,40,0x00ABCDEFu,2)==-MINT_EINVAL,
          "display text syscall rejects oversized strings");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_DRAW_TEXT,USER_DATA_VA,4,40,40,0x00ABCDEFu,5)==-MINT_EINVAL,
          "display text syscall rejects unsupported font scale");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_INPUT_INFO,USER_DATA_VA,0,0,0,0,0)==0,
          "input-info returns readiness and runtime event counters");
    MINT_INPUT_INFO input={0};
    CHECK(mint_address_space_copy_from_user(&space,&input,USER_DATA_VA,sizeof(input)) &&
          input.keyboard_ready==1u && input.mouse_ready==1u && input.keyboard_events==11u &&
          input.pointer_motion_events==22u && input.pointer_button_events==3u,
          "input-info exposes key, pointer-motion and button counters");
    MINT_DESKTOP_SETTINGS settings={0};
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DESKTOP_SETTINGS_GET,USER_DATA_VA,0,0,0,0,0)==0,
          "desktop settings query writes the versioned settings contract");
    CHECK(mint_address_space_copy_from_user(&space,&settings,USER_DATA_VA,sizeof(settings)) &&
          settings.version==MINT_DESKTOP_SETTINGS_VERSION && settings.theme==MINT_THEME_LIGHT,
          "userspace sees default theme and settings version");
    settings.theme=MINT_THEME_DARK; settings.accent=MINT_ACCENT_BLUE; settings.ui_scale=2u;
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA,&settings,sizeof(settings)) &&
          mint_syscall_dispatch_user(MINT_SYS_DESKTOP_SETTINGS_SET,USER_DATA_VA,0,0,0,0,0)==0,
          "settings syscall accepts a validated user preferences structure");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DESKTOP_SETTINGS_GET,USER_DATA_VA,0,0,0,0,0)==0 &&
          mint_address_space_copy_from_user(&space,&settings,USER_DATA_VA,sizeof(settings)) &&
          settings.theme==MINT_THEME_DARK && settings.accent==MINT_ACCENT_BLUE && settings.ui_scale==2u,
          "settings values remain available to userspace after an update");
    settings.theme=88u;
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA,&settings,sizeof(settings)) &&
          mint_syscall_dispatch_user(MINT_SYS_DESKTOP_SETTINGS_SET,USER_DATA_VA,0,0,0,0,0)==-MINT_EINVAL,
          "settings syscall rejects invalid enum values");
    const char notes_path[]="NOTES.TXT";
    const char note_data[]="COM1 TEST NOTE";
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA,notes_path,sizeof(notes_path)),
          "prepare a bounded userspace path for the RAM filesystem");
    int64_t note_handle=mint_syscall_dispatch_user(MINT_SYS_FS_OPEN,USER_DATA_VA,MINT_FS_OPEN_WRITE|MINT_FS_OPEN_TRUNC,0,0,0,0);
    CHECK(note_handle>=3,"userspace opens a writable session file and receives a process-owned handle");
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA+128u,note_data,sizeof(note_data)-1u) &&
          mint_syscall_dispatch_user(MINT_SYS_FS_WRITE,(uint64_t)note_handle,USER_DATA_VA+128u,sizeof(note_data)-1u,0,0,0)==(int64_t)sizeof(note_data)-1,
          "filesystem write validates and copies user bytes");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_FS_CLOSE,(uint64_t)note_handle,0,0,0,0,0)==0,
          "filesystem close releases user file handle");
    CHECK(mint_address_space_copy_to_user(&space,USER_DATA_VA,notes_path,sizeof(notes_path)) &&
          (note_handle=mint_syscall_dispatch_user(MINT_SYS_FS_OPEN,USER_DATA_VA,MINT_FS_OPEN_READ,0,0,0,0))>=3,
          "userspace can reopen its session note for reading");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_FS_READ,(uint64_t)note_handle,USER_DATA_VA+256u,64u,0,0,0)==(int64_t)sizeof(note_data)-1,
          "filesystem read copies data back to writable user memory");
    char read_note[sizeof(note_data)]={0};
    CHECK(mint_address_space_copy_from_user(&space,read_note,USER_DATA_VA+256u,sizeof(note_data)-1u) &&
          memcmp(read_note,note_data,sizeof(note_data)-1u)==0,
          "notes contents survive close and reopen in the same session");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_FS_CLOSE,(uint64_t)note_handle,0,0,0,0,0)==0,
          "filesystem closes reopened note handle");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_FS_LIST,USER_DATA_VA, MINT_RAMFS_MAX_FILES,0,0,0,0)>=3,
          "filesystem list returns bounded directory entries");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FRAME_BEGIN,0,0,0,0,0,0)==0,
          "begin retained frame before capturing desktop background");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FILL_RECT,100,100,20,20,0x00444444u,0)==0,
          "draw base region into retained frame");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_BACKGROUND_CAPTURE,0,0,0,0,0,0)==0,
          "capture unobstructed desktop background in kernel memory");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FILL_RECT,105,105,10,10,0x00FF0000u,0)==0 &&
          mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FRAME_END,0,0,0,0,0,0)==0,
          "commit overlay region as one frame");
    CHECK(display_pixels[105u*640u+105u]==0x00FF0000u,
          "frame commit presents the overlay region");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FRAME_BEGIN,0,0,0,0,0,0)==0 &&
          mint_syscall_dispatch_user(MINT_SYS_DISPLAY_RESTORE_RECT,100,100,20,20,0,0)==0,
          "restore old window rectangle from retained desktop background");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FILL_RECT,105,105,5,5,0x0000FF00u,0)==0 &&
          mint_syscall_dispatch_user(MINT_SYS_DISPLAY_FRAME_END,0,0,0,0,0,0)==0,
          "present a moved window region without redrawing the whole screen");
    CHECK(display_pixels[105u*640u+105u]==0x0000FF00u &&
          display_pixels[114u*640u+114u]==0x00444444u && display_pixels[300u*640u+300u]==0,
          "partial present restores uncovered pixels and preserves unrelated scanout");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DISPLAY_RESTORE_RECT,0,0,0,20,0,0)==-MINT_EINVAL,
          "restore-rectangle syscall rejects zero-size damage");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DRIVER_CATALOG,USER_CODE_VA,0,0,0,0,0)==-MINT_EFAULT,
          "driver catalog rejects read-only user output");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_DRIVER_CATALOG,USER_DATA_VA,0,0,0,0,0)==0,
          "driver catalog copies a bounded snapshot to writable userspace");
    MINT_DRIVER_CATALOG catalog_copy;
    CHECK(mint_address_space_copy_from_user(mint_address_space_current(), &catalog_copy,
                                            USER_DATA_VA, sizeof(catalog_copy)),
          "driver catalog snapshot is readable from user memory");
    CHECK(catalog_copy.version==MINT_DRIVER_CATALOG_VERSION && catalog_copy.count>=10u &&
          catalog_copy.count<=MINT_DRIVER_CATALOG_MAX_RECORDS,
          "driver catalog version and bounded record count are valid");
    int found_e1000=0, found_usb=0, found_svga=0;
    for (uint32_t ci=0; ci<catalog_copy.count && ci<MINT_DRIVER_CATALOG_MAX_RECORDS; ++ci) {
        /* The mocked E1000 reports ACTIVE; the mocked xHCI driver has not started. */
        if (strcmp(catalog_copy.records[ci].name,"Ethernet controller")==0 &&
            catalog_copy.records[ci].state==MINT_DRIVER_ACTIVE) found_e1000=1;
        if (strcmp(catalog_copy.records[ci].name,"USB host controller (xHCI)")==0 &&
            catalog_copy.records[ci].state==MINT_DRIVER_DEVICE_FOUND) found_usb=1;
        if (strcmp(catalog_copy.records[ci].name,"VMware SVGA")==0 &&
            catalog_copy.records[ci].state==MINT_DRIVER_DEVICE_FOUND) found_svga=1;
    }
    CHECK(found_e1000 && found_usb && found_svga,
          "driver catalog reflects each driver's state for discovered hardware");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_E1000_START,0,0,0,0,0,0)==0,
          "E1000 startup is a distinct explicit user request");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_E1000_INFO,USER_CODE_VA,0,0,0,0,0)==-MINT_EFAULT,
          "E1000 info rejects read-only code output");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_E1000_INFO,USER_DATA_VA,0,0,0,0,0)==0,
          "E1000 info copies bounded RX/TX status to userspace");
    MINT_E1000_INFO e1000_copy;
    CHECK(mint_address_space_copy_from_user(mint_address_space_current(),&e1000_copy,USER_DATA_VA,sizeof(e1000_copy)) &&
          e1000_copy.version==MINT_E1000_INFO_VERSION && e1000_copy.state==MINT_E1000_ACTIVE &&
          e1000_copy.tx_completed==2u && e1000_copy.rx_received==1u,
          "E1000 userspace status carries a versioned driver state and counters");
    CHECK(mint_syscall_dispatch_user(MINT_SYS_E1000_LOOPBACK_TEST,0,0,0,0,0,0)==0,
          "E1000 internal loopback test syscall returns bounded diagnostic result");
    MINT_SYSCALL_FRAME frame={0}; frame.vector=0x80;frame.cs=0x2b;frame.rax=MINT_SYS_GETPID;
    CHECK(mint_syscall_trap_dispatch(&frame)==42,"int-0x80 trap checks vector and ring-3 origin");
    mint_syscall_user_exit_reset();
    CHECK((uint64_t)mint_syscall_dispatch_user(MINT_SYS_EXIT,7,0,0,0,0,0)==MINT_SYSCALL_EXIT_SENTINEL&&
          current_root==0x00800000ULL&&mint_syscall_user_exit_seen()&&mint_syscall_user_exit_code()==7,
          "user exit switches back to kernel root and records exit status");
    CHECK(mint_address_space_destroy(&space),"destroy address space and release user frames");
    CHECK(active_pages()==0,"no mock physical-page leak after user syscall test");
    puts("PASS: ring-3 syscall validation tests complete");
    return 0;
}
