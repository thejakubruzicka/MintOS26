#ifndef MINTOS26_UAPI_SYSCALL_H
#define MINTOS26_UAPI_SYSCALL_H
#include <stdint.h>

/* MintOS syscall numbering. A few base values match Linux x86-64 numbers for
 * familiarity only; this is NOT a Linux-compatible ABI. 26.5.008.0 adds the bounded polling E1000 interface and device telemetry; 26.5.009.0 adds ACPI power info/control. */
#define MINT_SYSABI_VERSION 1u
#define MINT_SYS_READ          0u
#define MINT_SYS_WRITE         1u
#define MINT_SYS_OPEN          2u
#define MINT_SYS_CLOSE         3u
#define MINT_SYS_SCHED_YIELD  24u
#define MINT_SYS_GETPID       39u
#define MINT_SYS_EXIT         60u
#define MINT_SYS_CLOCK_GETTIME 228u
/* MintOS-specific desktop-service calls; not part of the Linux ABI. */
#define MINT_SYS_DESKTOP_EVENT_POLL 0x4D00u
#define MINT_SYS_DISPLAY_INFO       0x4D01u
#define MINT_SYS_DISPLAY_FILL_RECT  0x4D02u
#define MINT_SYS_DISPLAY_DRAW_TEXT  0x4D03u
#define MINT_SYS_SLEEP_TICKS        0x4D04u
#define MINT_SYS_INPUT_INFO         0x4D05u
#define MINT_SYS_DISPLAY_FRAME_BEGIN 0x4D06u
#define MINT_SYS_DISPLAY_FRAME_END   0x4D07u
#define MINT_SYS_DISPLAY_BACKGROUND_CAPTURE 0x4D08u
#define MINT_SYS_DISPLAY_RESTORE_RECT       0x4D09u
#define MINT_SYS_DESKTOP_SETTINGS_GET       0x4D0Au
#define MINT_SYS_DESKTOP_SETTINGS_SET       0x4D0Bu
#define MINT_SYS_DISPLAY_SET_WALLPAPER      0x4D0Cu
#define MINT_SYS_FS_OPEN                    0x4D10u
#define MINT_SYS_FS_READ                    0x4D11u
#define MINT_SYS_FS_WRITE                   0x4D12u
#define MINT_SYS_FS_CLOSE                   0x4D13u
#define MINT_SYS_FS_LIST                    0x4D14u
#define MINT_SYS_STORAGE_DIAGNOSTICS        0x4D15u
#define MINT_SYS_LOG_READ                   0x4D16u
#define MINT_SYS_DRIVER_CATALOG             0x4D17u
#define MINT_SYS_STORAGE_FORMAT_BLANK       0x4D18u
#define MINT_SYS_FAT16_LIST                 0x4D19u
#define MINT_SYS_FAT16_READ                 0x4D1Au
#define MINT_SYS_FAT16_VOLUMES              0x4D1Bu
#define MINT_SYS_FAT16_SELECT_VOLUME        0x4D1Cu
#define MINT_SYS_DISPLAY_BLEND_ROUND_RECT    0x4D1Du
#define MINT_SYS_DISPLAY_DRAW_BITMAP         0x4D1Eu
#define MINT_SYS_BLOCK_IO_STATS              0x4D1Fu
#define MINT_SYS_VFS_MOUNTS                  0x4D20u
#define MINT_SYS_PCI_INVENTORY               0x4D21u
#define MINT_SYS_E1000_START                  0x4D24u
#define MINT_SYS_E1000_INFO                   0x4D22u
#define MINT_SYS_E1000_LOOPBACK_TEST           0x4D23u
/* 26.5.009.0: ACPI power information and confirmed shutdown/restart. */
#define MINT_SYS_POWER_INFO                    0x4D25u
#define MINT_SYS_POWER_CONTROL                 0x4D26u
/* 26.5.010.0: shared-canvas display model for MintDE 2 (see desktop_abi.h). */
#define MINT_SYS_DISPLAY_MAP_CANVAS            0x4D27u
#define MINT_SYS_DISPLAY_PRESENT               0x4D28u
#define MINT_SYS_DISPLAY_CURSOR                0x4D29u
#define MINT_SYS_DISPLAY_WALLPAPER_RENDER      0x4D2Au
/* 26.5.011.0: USB (xHCI/HID) status and networking (TCP/IP). */
#define MINT_SYS_USB_INFO                      0x4D2Bu
#define MINT_SYS_NET_INFO                      0x4D30u
#define MINT_SYS_NET_UP                        0x4D31u
#define MINT_SYS_NET_RESOLVE                   0x4D32u
#define MINT_SYS_NET_PING                      0x4D33u
#define MINT_SYS_TCP                           0x4D34u
/* MintOS disk files (MintFS). Paths are absolute UTF-8 C strings. */
#define MINT_SYS_MFS_OPEN                      0x4D40u
#define MINT_SYS_MFS_READ                      0x4D41u
#define MINT_SYS_MFS_WRITE                     0x4D42u
#define MINT_SYS_MFS_SEEK                      0x4D43u
#define MINT_SYS_MFS_CLOSE                     0x4D44u
#define MINT_SYS_MFS_STAT                      0x4D45u
#define MINT_SYS_MFS_LIST                      0x4D46u
#define MINT_SYS_MFS_MKDIR                     0x4D47u
#define MINT_SYS_MFS_RENAME                    0x4D48u
#define MINT_SYS_MFS_REMOVE                    0x4D49u
#define MINT_SYS_MFS_SYNC                      0x4D4Au
#define MINT_SYS_MFS_VOLUME                    0x4D4Bu
#define MINT_SYS_STORAGE_INIT_MINTOS           0x4D4Cu
/* Data saving (26.5.014.0, range 0x4D50-0x4D57) */
#define MINT_SYS_MFS_SET_HOME                  0x4D50u

#define MINT_CLOCK_REALTIME  0
#define MINT_CLOCK_MONOTONIC 1

/* Kernel dispatcher returns negative errno values, as raw Linux syscalls do. */
#define MINT_EPERM       1
#define MINT_ENOENT      2
#define MINT_EIO         5
#define MINT_EINTR       4
#define MINT_EBADF       9
#define MINT_EAGAIN     11
#define MINT_ENOMEM     12
#define MINT_ENODEV     19
#define MINT_EFAULT     14
#define MINT_EINVAL     22
#define MINT_EROFS      30
#define MINT_ENOSPC     28
#define MINT_ENOSYS     38
#define MINT_EBUSY      16
#define MINT_EPIPE      32
#define MINT_ENETDOWN  100
#define MINT_ENETUNREACH 101
#define MINT_ECONNRESET 104
#define MINT_ENOTCONN  107
#define MINT_ETIMEDOUT 110
#define MINT_ECONNREFUSED 111
#define MINT_EHOSTUNREACH 113
#define MINT_EEXIST     17
#define MINT_ENOTDIR    20
#define MINT_EISDIR     21
#define MINT_EMFILE     24
#define MINT_EFBIG      27
#define MINT_ENAMETOOLONG 36
#define MINT_ENOTEMPTY  39
#define MINT_ECORRUPT  117

typedef struct mint_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
} mint_timespec64;

#endif
