/* MintOS26 26.5.010.0 — UEFI Desktop with a time-bounded boot selector.
 * UEFI Desktop remains the safe default; choosing MintDE 2.0.0 loads the ELF64
 * kernel, exits boot services, and makes the one-way native handoff. */
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef int8_t i8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int16_t i16; typedef int32_t i32; typedef int64_t i64; typedef uint64_t UINTN;
typedef uint16_t CHAR16; typedef u64 EFI_STATUS;
#include "font_aa.h"
#include "../kernel/include/boot_abi.h"
#include "../kernel/include/desktop_abi.h"
typedef void *EFI_HANDLE; typedef void *EFI_EVENT; typedef u64 EFI_LBA; typedef u64 EFI_PHYSICAL_ADDRESS;
#define EFI_SUCCESS 0
#define EFI_ERROR(s) (((EFI_STATUS)(s) >> 63) != 0)
#define EFI_NOT_READY 0x8000000000000006ULL
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EFI_LOAD_ERROR 0x8000000000000001ULL
#define EFI_UNSUPPORTED 0x8000000000000003ULL
#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE 0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL
#define EFI_FILE_DIRECTORY 0x0000000000000010ULL
#define EFI_LOCATE_BY_PROTOCOL 2
#define EVT_TIMER 0x80000000u
#define EVT_NOTIFY_SIGNAL 0x00000200u
#define TPL_CALLBACK 8u
#define TIMER_PERIODIC 1u
#define TPL_APPLICATION 4u
#define EfiLoaderData 2
#define EfiBootServicesData 4
#define APP_NONE 0
#define APP_LAUNCHER 1
#define APP_FINDER 2
#define APP_TERMINAL 3
#define APP_NOTES 4
#define APP_CALCULATOR 5
#define APP_SETTINGS 6
#define APP_ABOUT 7
#define APP_MONITOR 8
#define APP_CALENDAR 9
#define APP_CLOCK 10
#define APP_CODE 11
#define APP_NETWORK 12
#define APP_INSTALLER 13
#define APP_ABOUT_MINTDE 14
#define WALL_W 1920
#define WALL_H 1080
#define WALL_BYTES (WALL_W*WALL_H*3u)
#define MAX_FILES 32
#define MAX_TEXT 1400
#define COLOR(r,g,b) (((u32)(r)<<16)|((u32)(g)<<8)|(u32)(b))

#define EFIAPI

typedef struct { u32 Data1; u16 Data2,Data3; u8 Data4[8]; } EFI_GUID;
typedef struct { u64 Signature; u32 Revision,HeaderSize,CRC32,Reserved; } EFI_TABLE_HEADER;
typedef struct { u16 Year; u8 Month,Day,Hour,Minute,Second,Pad1; u32 Nanosecond; i16 TimeZone; u8 Daylight,Pad2; } EFI_TIME;
typedef struct { u64 ResolutionX,ResolutionY,ResolutionZ; u8 LeftButton,RightButton; } EFI_SIMPLE_POINTER_MODE;
typedef struct { i32 RelativeMovementX,RelativeMovementY,RelativeMovementZ; u8 LeftButton,RightButton; } EFI_SIMPLE_POINTER_STATE;
typedef struct EFI_SIMPLE_POINTER_PROTOCOL EFI_SIMPLE_POINTER_PROTOCOL;
struct EFI_SIMPLE_POINTER_PROTOCOL { EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_POINTER_PROTOCOL*,u8); EFI_STATUS (EFIAPI *GetState)(EFI_SIMPLE_POINTER_PROTOCOL*,EFI_SIMPLE_POINTER_STATE*); EFI_EVENT WaitForInput; EFI_SIMPLE_POINTER_MODE *Mode; };
typedef struct { u64 CurrentX,CurrentY,CurrentZ; u32 ActiveButtons; } EFI_ABSOLUTE_POINTER_STATE;
typedef struct { u64 AbsoluteMinX,AbsoluteMinY,AbsoluteMinZ,AbsoluteMaxX,AbsoluteMaxY,AbsoluteMaxZ; u32 Attributes; } EFI_ABSOLUTE_POINTER_MODE;
typedef struct EFI_ABSOLUTE_POINTER_PROTOCOL EFI_ABSOLUTE_POINTER_PROTOCOL;
struct EFI_ABSOLUTE_POINTER_PROTOCOL { EFI_STATUS (EFIAPI *Reset)(EFI_ABSOLUTE_POINTER_PROTOCOL*,u8); EFI_STATUS (EFIAPI *GetState)(EFI_ABSOLUTE_POINTER_PROTOCOL*,EFI_ABSOLUTE_POINTER_STATE*); EFI_EVENT WaitForInput; EFI_ABSOLUTE_POINTER_MODE *Mode; };
/* Firmware-managed USB I/O fallback. The protocol layout follows UEFI 2.11 §17.2.5. */
typedef struct { u8 RequestType,Request; u16 Value,Index,Length; } EFI_USB_DEVICE_REQUEST;
typedef struct { u8 Length,DescriptorType; u16 BcdUSB; u8 DeviceClass,DeviceSubClass,DeviceProtocol,MaxPacketSize0; u16 IdVendor,IdProduct,BcdDevice; u8 Manufacturer,Product,SerialNumber,NumConfigurations; } EFI_USB_DEVICE_DESCRIPTOR;
typedef struct { u8 Length,DescriptorType,InterfaceNumber,AlternateSetting,NumEndpoints,InterfaceClass,InterfaceSubClass,InterfaceProtocol,Interface; } EFI_USB_INTERFACE_DESCRIPTOR;
typedef struct { u8 Length,DescriptorType,EndpointAddress,Attributes; u16 MaxPacketSize; u8 Interval; } EFI_USB_ENDPOINT_DESCRIPTOR;
typedef struct EFI_USB_IO_PROTOCOL EFI_USB_IO_PROTOCOL;
struct EFI_USB_IO_PROTOCOL {
 EFI_STATUS (EFIAPI *UsbControlTransfer)(EFI_USB_IO_PROTOCOL*,EFI_USB_DEVICE_REQUEST*,u32,u32,void*,UINTN,u32*);
 void *UsbBulkTransfer;
 void *UsbAsyncInterruptTransfer;
 EFI_STATUS (EFIAPI *UsbSyncInterruptTransfer)(EFI_USB_IO_PROTOCOL*,u8,void*,UINTN*,UINTN,u32*);
 void *UsbIsochronousTransfer; void *UsbAsyncIsochronousTransfer;
 EFI_STATUS (EFIAPI *UsbGetDeviceDescriptor)(EFI_USB_IO_PROTOCOL*,EFI_USB_DEVICE_DESCRIPTOR*);
 void *UsbGetConfigDescriptor;
 EFI_STATUS (EFIAPI *UsbGetInterfaceDescriptor)(EFI_USB_IO_PROTOCOL*,EFI_USB_INTERFACE_DESCRIPTOR*);
 EFI_STATUS (EFIAPI *UsbGetEndpointDescriptor)(EFI_USB_IO_PROTOCOL*,u8,EFI_USB_ENDPOINT_DESCRIPTOR*);
 void *UsbGetStringDescriptor; void *UsbGetSupportedLanguages; void *UsbPortReset;
};
#define EFI_USB_DATA_IN 0u
#define EFI_USB_DATA_OUT 1u
#define EFI_USB_NO_DATA 2u
typedef struct { u16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL { EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*,u8); EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL*,EFI_INPUT_KEY*); EFI_EVENT WaitForKey; };
typedef struct { u32 Version,HorizontalResolution,VerticalResolution,PixelFormat; struct {u32 RedMask,GreenMask,BlueMask,ReservedMask;} PixelInformation; u32 PixelsPerScanLine; } EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct { u32 MaxMode,Mode; EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info; UINTN SizeOfInfo; EFI_PHYSICAL_ADDRESS FrameBufferBase; UINTN FrameBufferSize; } EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL { EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL*,u32,UINTN*,EFI_GRAPHICS_OUTPUT_MODE_INFORMATION**); EFI_STATUS (EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL*,u32); EFI_STATUS (EFIAPI *Blt)(void*,...); EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode; };
/* Optional UEFI HTTP client ABI. This uses firmware TCP/IP/DNS if available; no native stack is implied. */
typedef struct EFI_SERVICE_BINDING_PROTOCOL EFI_SERVICE_BINDING_PROTOCOL;
struct EFI_SERVICE_BINDING_PROTOCOL { EFI_STATUS (EFIAPI *CreateChild)(EFI_SERVICE_BINDING_PROTOCOL*,EFI_HANDLE*); EFI_STATUS (EFIAPI *DestroyChild)(EFI_SERVICE_BINDING_PROTOCOL*,EFI_HANDLE); };
typedef struct { u8 UseDefaultAddress; u8 LocalAddress[4]; u8 LocalSubnet[4]; u16 LocalPort; } EFI_HTTPV4_ACCESS_POINT;
typedef struct { u32 HttpVersion; u32 TimeOutMillisec; u8 LocalAddressIsIPv6; union { EFI_HTTPV4_ACCESS_POINT *IPv4Node; void *IPv6Node; } AccessPoint; } EFI_HTTP_CONFIG_DATA;
typedef struct { u32 Method; CHAR16 *Url; } EFI_HTTP_REQUEST_DATA;
typedef struct { u32 StatusCode; } EFI_HTTP_RESPONSE_DATA;
typedef struct { char *FieldName; char *FieldValue; } EFI_HTTP_HEADER;
typedef struct { union { EFI_HTTP_REQUEST_DATA *Request; EFI_HTTP_RESPONSE_DATA *Response; } Data; UINTN HeaderCount; EFI_HTTP_HEADER *Headers; UINTN BodyLength; void *Body; } EFI_HTTP_MESSAGE;
typedef struct { EFI_EVENT Event; EFI_STATUS Status; EFI_HTTP_MESSAGE *Message; } EFI_HTTP_TOKEN;
typedef struct EFI_HTTP_PROTOCOL EFI_HTTP_PROTOCOL;
struct EFI_HTTP_PROTOCOL { EFI_STATUS (EFIAPI *GetModeData)(EFI_HTTP_PROTOCOL*,void*); EFI_STATUS (EFIAPI *Configure)(EFI_HTTP_PROTOCOL*,EFI_HTTP_CONFIG_DATA*); EFI_STATUS (EFIAPI *Request)(EFI_HTTP_PROTOCOL*,EFI_HTTP_TOKEN*); EFI_STATUS (EFIAPI *Cancel)(EFI_HTTP_PROTOCOL*,EFI_HTTP_TOKEN*); EFI_STATUS (EFIAPI *Response)(EFI_HTTP_PROTOCOL*,EFI_HTTP_TOKEN*); EFI_STATUS (EFIAPI *Poll)(EFI_HTTP_PROTOCOL*); };
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
 u64 Revision;
 EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL*,EFI_FILE_PROTOCOL**,CHAR16*,u64,u64);
 EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL*); EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL*);
 EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL*,UINTN*,void*); EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL*,UINTN*,void*);
 EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL*,u64*); EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL*,u64);
 EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL*,EFI_GUID*,UINTN*,void*); EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL*,EFI_GUID*,UINTN,void*);
 EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL*); void *OpenEx,*ReadEx,*WriteEx,*FlushEx;
};
typedef struct { u64 Revision; EFI_STATUS (EFIAPI *OpenVolume)(void*,EFI_FILE_PROTOCOL**); } EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct { u32 MediaId; u8 RemovableMedia,MediaPresent,LogicalPartition,ReadOnly,WriteCaching; u32 BlockSize,IoAlign; EFI_LBA LastBlock,LowestAlignedLba; u32 LogicalBlocksPerPhysicalBlock,OptimalTransferLength; } EFI_BLOCK_IO_MEDIA;
typedef struct { u64 Revision; EFI_BLOCK_IO_MEDIA *Media; void *Reset,*ReadBlocks,*WriteBlocks,*FlushBlocks; } EFI_BLOCK_IO_PROTOCOL;
typedef struct { u64 Size,FileSize,PhysicalSize; EFI_TIME CreateTime,LastAccessTime,ModificationTime; u64 Attribute; CHAR16 FileName[1]; } EFI_FILE_INFO;
typedef struct { u32 Revision; EFI_HANDLE ParentHandle; void *SystemTable; EFI_HANDLE DeviceHandle; void *FilePath; void *Reserved; u32 LoadOptionsSize; void *LoadOptions; void *ImageBase; u64 ImageSize; u32 ImageCodeType,ImageDataType; void *Unload; } EFI_LOADED_IMAGE_PROTOCOL;
typedef struct { EFI_TABLE_HEADER Hdr; void *FirmwareVendor; u32 FirmwareRevision; EFI_HANDLE ConsoleInHandle; EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn; EFI_HANDLE ConsoleOutHandle; void *ConOut; EFI_HANDLE StandardErrorHandle; void *StdErr; void *RuntimeServices; void *BootServices; UINTN NumberOfTableEntries; void *ConfigurationTable; } EFI_SYSTEM_TABLE;
typedef struct {
 EFI_TABLE_HEADER Hdr;
 void *RaiseTPL,*RestoreTPL,*AllocatePages,*FreePages,*GetMemoryMap;
 EFI_STATUS (EFIAPI *AllocatePool)(u32,UINTN,void**); EFI_STATUS (EFIAPI *FreePool)(void*);
 EFI_STATUS (EFIAPI *CreateEvent)(u32,UINTN,void*,void*,EFI_EVENT*);
 EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT,u32,u64);
 EFI_STATUS (EFIAPI *WaitForEvent)(UINTN,EFI_EVENT*,UINTN*);
 EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT);
 EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT);
 EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT);
 void *InstallProtocolInterface,*ReinstallProtocolInterface,*UninstallProtocolInterface;
 EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE,EFI_GUID*,void**);
 void *Reserved,*RegisterProtocolNotify,*LocateHandle,*LocateDevicePath,*InstallConfigurationTable;
 void *LoadImage,*StartImage,*Exit,*UnloadImage,*ExitBootServices,*GetNextMonotonicCount;
 EFI_STATUS (EFIAPI *Stall)(UINTN); void *SetWatchdogTimer,*ConnectController,*DisconnectController,*OpenProtocol,*CloseProtocol,*OpenProtocolInformation,*ProtocolsPerHandle;
 EFI_STATUS (EFIAPI *LocateHandleBuffer)(u32,EFI_GUID*,void*,UINTN*,EFI_HANDLE**);
 EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID*,void*,void**);
 void *InstallMultipleProtocolInterfaces,*UninstallMultipleProtocolInterfaces,*CalculateCrc32,*CopyMem,*SetMem,*CreateEventEx;
} EFI_BOOT_SERVICES;
typedef void *EFI_ALLOC_POOL_FN;
typedef struct { EFI_TABLE_HEADER Hdr; EFI_STATUS (EFIAPI *GetTime)(EFI_TIME*,void*); } EFI_RUNTIME_SERVICES;

static EFI_GUID GUID_GOP={0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static EFI_GUID GUID_SFS={0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_BLOCK_IO={0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_LOADED_IMAGE={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID GUID_SIMPLE_POINTER={0x31878c87,0x0b75,0x11d5,{0x9a,0x4f,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID GUID_ABSOLUTE_POINTER={0x8d59d32b,0xc655,0x4ae9,{0x9b,0x15,0xf2,0x59,0x04,0x99,0x2a,0x43}};
static EFI_GUID GUID_USB_IO={0x2b2f68d6,0x0cd2,0x44cf,{0x8e,0x8b,0xbb,0xa2,0x0b,0x1b,0x5b,0x75}};
static EFI_GUID GUID_SIMPLE_NETWORK={0xa19832b9,0xac25,0x11d3,{0x9a,0x2d,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
static EFI_GUID GUID_HTTP={0x7a59b29b,0x910b,0x4171,{0x82,0x42,0xa8,0x5a,0x0d,0xf2,0x5b,0x5b}};
static EFI_GUID GUID_HTTP_SERVICE_BINDING={0xbdc8e6af,0xd9bc,0x4379,{0xa7,0x2a,0xe0,0xc4,0xe7,0x5d,0xae,0x1c}};

static EFI_BOOT_SERVICES *bs;
static EFI_SYSTEM_TABLE *st;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
#define MAX_POINTER_DEVICES 16
#define MAX_WAIT_EVENTS (2 + MAX_POINTER_DEVICES * 2)
static EFI_SIMPLE_POINTER_PROTOCOL *pointer;
static EFI_ABSOLUTE_POINTER_PROTOCOL *absolute_pointer;
static EFI_SIMPLE_POINTER_PROTOCOL *relative_pointers[MAX_POINTER_DEVICES];
static EFI_ABSOLUTE_POINTER_PROTOCOL *absolute_pointers[MAX_POINTER_DEVICES];
static UINTN relative_pointer_count=0, absolute_pointer_count=0;
#define MAX_USB_MOUSE_DEVICES 16
typedef struct {
 EFI_USB_IO_PROTOCOL *io;
 u8 endpoint,interface_number,report_len,report_id,last_buttons,boot_format,absolute_axes;
 u16 max_packet,x_bit,y_bit,left_bit,right_bit;
 u8 x_size,y_size,left_size,right_size;
 i32 x_min,x_max,y_min,y_max;
 int active;
} USB_HID_POINTER;
static USB_HID_POINTER usb_mice[MAX_USB_MOUSE_DEVICES];
static UINTN usb_io_handle_count=0,usb_mouse_count=0,usb_hid_other_count=0;
static u64 usb_mouse_polls=0,usb_mouse_reports=0,usb_mouse_errors=0,usb_hid_descriptor_errors=0;
static int usb_setup_attempted=0,firmware_pointer_motion=0;
static i64 relative_remainder_x_fp[MAX_POINTER_DEVICES],relative_remainder_y_fp[MAX_POINTER_DEVICES];
static i32 last_rel_raw_dx=0,last_rel_raw_dy=0;
static u64 last_rel_resolution_x=0,last_rel_resolution_y=0;
static u8 relative_last_buttons[MAX_POINTER_DEVICES];
static u8 relative_buttons_known[MAX_POINTER_DEVICES];
/* PS/2 is attempted lazily only when UEFI discovered protocols have produced no
   successful states. This keeps machines such as the MacBook on their native path. */
static int ps2_attempted=0,ps2_available=0,ps2_packet_index=0;
static u8 ps2_packet[3];
static u64 ps2_packets=0,ps2_errors=0;
static char fallback_detail[160]="Fallback input: USB mouse scan pending; PS/2 idle";
static u64 relative_samples=0, absolute_samples=0, relative_motion_samples=0, absolute_motion_samples=0, absolute_rejected_samples=0;
static u64 relative_not_ready=0, absolute_not_ready=0, relative_errors=0, absolute_errors=0;
static u64 timer_wakeups=0, keyboard_wakeups=0, absolute_event_wakeups=0, relative_event_wakeups=0;
static EFI_EVENT input_timer_event=0;
static int timer_loop_ready=0;
static u64 last_abs_x=0,last_abs_y=0,last_abs_minx=0,last_abs_miny=0,last_abs_maxx=0,last_abs_maxy=0;
static int last_abs_device=-1,last_abs_seen=0,last_rel_dx=0,last_rel_dy=0,last_rel_left=0,last_rel_right=0;
static char pointer_detail[192]="Move the mouse to collect a UEFI pointer sample.";
static char event_detail[192]="UEFI event dispatcher: not initialized";
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin;
static u32 gop_modes[64],gop_mode_w[64],gop_mode_h[64];
static UINTN gop_mode_count=0;
static u64 pointer_events=0;
static char pointer_label[192]="Pointer: searching UEFI protocols...";
static char audio_label[112]="Audio: no native audio driver; output unavailable";
static EFI_FILE_PROTOCOL *boot_root,*data_root,*active_root;
#define MAX_FS_VOLUMES 12
#define MAX_FINDER_ENTRIES 64
typedef struct { EFI_HANDLE handle; EFI_FILE_PROTOCOL *root; char label[64]; int writable; u64 total_bytes; u32 block_size; } MINT_VOLUME;
typedef struct { char name[96]; u64 size; int is_dir; } FINDER_ENTRY;
static MINT_VOLUME fs_volumes[MAX_FS_VOLUMES];
static UINTN fs_volume_count=0, finder_volume=0, finder_entry_count=0, finder_scroll=0;
static FINDER_ENTRY finder_entries[MAX_FINDER_ENTRIES];
static char finder_path[256]="\\MINTOS26\\DOCUMENTS";
static char finder_status[160]="Select a file or folder.";
static char finder_preview_name[96]="";
static char finder_preview[768]="";
static int finder_selected=-1, finder_preview_open=0;
static u64 finder_last_click_tick=0;
static int finder_last_click_index=-1;
static int settings_dropdown=0;
static int network_http_available=0;
static char network_http_status[160]="HTTP request not run.";
static char network_http_body[1000]="";
static EFI_HANDLE boot_device,boot_image_handle;
static u32 *backbuffer,*desktop_base;
static MINT_DISPLAY_SURFACE desktop_backbuffer_surface;
static MINT_DESKTOP_EVENT_QUEUE uefi_desktop_events;
static int uefi_desktop_events_ready=0;
static u64 uefi_desktop_events_consumed=0;
static u8 *wallpaper;
static int desktop_base_dirty=1;
static u32 screen_w=1024,screen_h=768,pitch=1024,pixel_format=1;
static int mouse_x=510,mouse_y=380,mouse_left=0,mouse_right=0,mouse_prev_left=0,dragging=0;
static int active_app=APP_NONE,theme=0,sound_on=1,glass_on=1,launcher_open=0;
static int locale_id=0,timezone_id=0,clock_24h=1,cursor_style=0,settings_page=0;
static int calendar_month=1,calendar_year=2026;
static char code_text[MAX_TEXT]="/* MintOS26 Code Studio\n * Save source to the attached writable volume.\n */\n#include <stdint.h>\n\nint main(void) {\n    return 0;\n}\n";
static UINTN code_len=0;
static char installer_status[160]="No installation bundle prepared yet.";
static char network_label[160]="Network: checking firmware interfaces...";
static u32 network_adapters=0;
static int win_x=320,win_y=175,win_w=1280,win_h=720;
static u64 ticks=0;
static char cmdline[128],terminal_msg[256]="Welcome to MintOS26 Terminal. Type HELP.";
static char note_text[MAX_TEXT]="Welcome to MintOS26 Notes.\nYour notes are saved to the attached data volume when available.";
static UINTN note_len=0,cmd_len=0;
static char calculator_expr[96]="12 + 30",calc_result[96]="42";
static char storage_label[80]="Storage: searching UEFI volumes...";
static const u32 themes[3][8]={{0x16c7b7,0x0b1828,0x193347,0xf5f8fc,0x506777,0xf6b65c,0xfefefe,0xa7bccb},{0x19c37d,0x112d27,0x20483e,0xf1fbf6,0x4f6b60,0xeabf70,0xffffff,0x9dbbb0},{0x5cc8e8,0x142439,0x263f59,0xf4f8ff,0x4d687c,0xffcb92,0xffffff,0xaabbd2}};
static u32 C_ACCENT,C_DARK,C_PANEL,C_TEXT,C_MUTED,C_ORANGE,C_WHITE,C_BORDER;
static u32 *blur_a,*blur_b,*glass_tmp; static u32 blur_band_h;
static int glass_cache_valid=0,glass_cache_app=APP_NONE,glass_cache_x=0,glass_cache_y=0,glass_cache_w=0,glass_cache_h=0,glass_cache_theme=-1;
static u8 font_coverage(int glyph,int sample){u8 p=font_aa[glyph*112+sample/2];u8 v=(sample&1)?(p&15):(p>>4);return (u8)(v*17);}
static u8 font_cov_xy(int glyph,int x,int y){if(x<0||x>15||y<0||y>13)return 0;return font_coverage(glyph,y*16+x);}
static u8 font_cov_bilinear(int glyph,int xfp,int yfp,int left,int gw){
 if(xfp<0)xfp=0;if(yfp<0)yfp=0;if(xfp>((gw-1)<<8))xfp=(gw-1)<<8;if(yfp>(13<<8))yfp=13<<8;
 int x0=xfp>>8,y0=yfp>>8,x1=x0<gw-1?x0+1:x0,y1=y0<13?y0+1:y0,fx=xfp&255,fy=yfp&255;
 int a=font_cov_xy(glyph,left+x0,y0),b=font_cov_xy(glyph,left+x1,y0),c=font_cov_xy(glyph,left+x0,y1),d=font_cov_xy(glyph,left+x1,y1);
 int top=(a*(256-fx)+b*fx)>>8,bot=(c*(256-fx)+d*fx)>>8;return (u8)((top*(256-fy)+bot*fy)>>8);
}

void *memset(void *p,int v,size_t n){u8*d=p;while(n--)*d++=(u8)v;return p;}
void *memcpy(void *d,const void*s,size_t n){u8*o=d;const u8*i=s;while(n--)*o++=*i++;return d;}
int memcmp(const void*a,const void*b,size_t n){const u8*x=a,*y=b;while(n--){if(*x!=*y)return (int)*x-(int)*y;x++;y++;}return 0;}
size_t strlen(const char*s){size_t n=0;while(s&&s[n])n++;return n;}
static int str_eq(const char*a,const char*b){while(*a&&*b){char x=*a,y=*b;if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;if(x!=y)return 0;a++;b++;}return *a==*b;}
static int str_starts(const char*a,const char*b){while(*b){char x=*a++,y=*b++;if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;if(x!=y)return 0;}return 1;}
static void append_char(char*d,UINTN cap,UINTN *n,char c){if(*n+1<cap){d[(*n)++]=c;d[*n]=0;}}
static void append_str(char*d,UINTN cap,UINTN*n,const char*s){while(*s)append_char(d,cap,n,*s++);}
static void u16_ascii(CHAR16 *d,const char*s){while(*s)*d++=(u8)*s++;*d=0;}
static void ascii_from_u16(char *d,UINTN cap,const CHAR16*s){UINTN i=0;if(!cap)return;while(*s&&i+1<cap){d[i++]=(*s<128)?(char)*s:'?';s++;}d[i]=0;}
static void copy_text(char*d,UINTN cap,const char*s){if(!cap)return;UINTN i=0;while(s[i]&&i+1<cap){d[i]=s[i];i++;}d[i]=0;}
static int ascii_is_space(char c){return c==' '||c=='\t'||c=='\r'||c=='\n';}
static void u64_to_dec(char *o,u64 v){char t[24];int n=0;do{t[n++]=(char)('0'+v%10);v/=10;}while(v&&n<23);int j=0;while(n)o[j++]=t[--n];o[j]=0;}
static int X(int x){return (int)(((int64_t)x*screen_w)/1920);}
static int Y(int y){return (int)(((int64_t)y*screen_h)/1080);}
static int base_x(int x){return (int)(((int64_t)x*1920)/screen_w);}
static int base_y(int y){return (int)(((int64_t)y*1080)/screen_h);}
static int font_scale(void){return screen_w>=1500?2:1;}
static u32 blend(u32 dst,u32 src,u8 a){u32 r=((src>>16)&255)*a+((dst>>16)&255)*(255-a);u32 g=((src>>8)&255)*a+((dst>>8)&255)*(255-a);u32 b=(src&255)*a+(dst&255)*(255-a);return COLOR(r/255,g/255,b/255);}
static void update_backbuffer_surface(void){
 MINT_FRAMEBUFFER_INFO f;
 f.base=(u64)(uintptr_t)backbuffer;
 f.size=(u64)screen_w*(u64)screen_h*sizeof(u32);
 f.width=screen_w;f.height=screen_h;f.pixels_per_scanline=screen_w;
 /* The RAM backbuffer is canonical 0xRRGGBB, independent of GOP byte order. */
 f.pixel_format=1u;
 (void)mint_display_surface_init(&desktop_backbuffer_surface,&f);
}
static void pset(int x,int y,u32 c){
 if(x<0||y<0||x>=(int)screen_w||y>=(int)screen_h)return;
 if(desktop_backbuffer_surface.initialized){mint_display_put_pixel(&desktop_backbuffer_surface,x,y,c);return;}
 backbuffer[(u64)y*screen_w+x]=c;
}
static void fill_rect(int bx,int by,int bw,int bh,u32 col,u8 alpha){
 int x0=X(bx),y0=Y(by),x1=X(bx+bw),y1=Y(by+bh);
 if(alpha==255&&desktop_backbuffer_surface.initialized){
  mint_display_fill_rect(&desktop_backbuffer_surface,x0,y0,x1-x0,y1-y0,col);return;
 }
 if(x0<0)x0=0;if(y0<0)y0=0;if(x1>(int)screen_w)x1=screen_w;if(y1>(int)screen_h)y1=screen_h;
 for(int y=y0;y<y1;y++){u32*row=backbuffer+(u64)y*screen_w;for(int x=x0;x<x1;x++)row[x]=alpha==255?col:blend(row[x],col,alpha);}
}
static void round_rect(int bx,int by,int bw,int bh,int rad,u32 col,u8 alpha){int x0=X(bx),y0=Y(by),x1=X(bx+bw),y1=Y(by+bh);int rx=(X(rad)-X(0));if(rx<1)rx=1;int r2=rx*rx;if(x0<0)x0=0;if(y0<0)y0=0;if(x1>(int)screen_w)x1=screen_w;if(y1>(int)screen_h)y1=screen_h;for(int y=y0;y<y1;y++){for(int x=x0;x<x1;x++){int cl=0;if(x<X(bx)+rx&&y<Y(by)+rx){int dx=X(bx)+rx-1-x,dy=Y(by)+rx-1-y;cl=dx*dx+dy*dy<=r2;}else if(x>=X(bx+bw)-rx&&y<Y(by)+rx){int dx=x-(X(bx+bw)-rx),dy=Y(by)+rx-1-y;cl=dx*dx+dy*dy<=r2;}else if(x<X(bx)+rx&&y>=Y(by+bh)-rx){int dx=X(bx)+rx-1-x,dy=y-(Y(by+bh)-rx);cl=dx*dx+dy*dy<=r2;}else if(x>=X(bx+bw)-rx&&y>=Y(by+bh)-rx){int dx=x-(X(bx+bw)-rx),dy=y-(Y(by+bh)-rx);cl=dx*dx+dy*dy<=r2;}else cl=1;if(cl){u64 off=(u64)y*screen_w+x;backbuffer[off]=alpha==255?col:blend(backbuffer[off],col,alpha);}}}}
static void hline(int x,int y,int w,u32 c,u8 a){fill_rect(x,y,w,1,c,a);}static void vline(int x,int y,int h,u32 c,u8 a){fill_rect(x,y,1,h,c,a);}
static int glyph_width(char c){if(c<32||c>126)return 6;return font_advance[(u8)c-32];}
static void draw_text(int bx,int by,const char*s,u32 c,int scale){int fs=font_scale()*scale;int cursor=X(bx),top=Y(by);u32 rr=(c>>16)&255,gg=(c>>8)&255,bb=c&255;while(*s){u8 ch=(u8)*s++;if(ch<32||ch>126){cursor+=X(7*fs)-X(0);continue;}int idx=ch-32;int gw=font_aa_width[idx],left=font_aa_left[idx],adv=font_advance[idx];int outw=X(gw*fs)-X(0),outh=Y(14*fs)-Y(0);if(outw<1)outw=1;if(outh<1)outh=1;for(int sy=0;sy<outh;sy++){int py=top+sy;if(py<0||py>=(int)screen_h)continue;for(int sx=0;sx<outw;sx++){int px=cursor+sx;if(px<0||px>=(int)screen_w)continue;u8 cov;if(fs>1){int xf=((2*sx+1)*gw*128/outw)-128;int yf=((2*sy+1)*14*128/outh)-128;cov=font_cov_bilinear(idx,xf,yf,left,gw);}else{int srcy=sy*14/outh,srcx=left+sx*gw/outw;if(srcx>15)srcx=15;cov=font_coverage(idx,srcy*16+srcx);}if(cov<10)continue;u64 off=(u64)py*screen_w+px;backbuffer[off]=blend(backbuffer[off],COLOR(rr,gg,bb),cov);}}cursor+=X(adv*fs)-X(0);}}
static void draw_text_center(int cx,int y,const char*s,u32 c,int scale){int w=0;for(const char*p=s;*p;p++)w+=glyph_width(*p);draw_text(cx-w*font_scale()*scale/2,y,s,c,scale);}
static void draw_text_fit(int bx,int by,const char*s,u32 c,int scale,int max_width){
 if(!s||max_width<8)return;char tmp[256];UINTN n=0;int limit=max_width/(font_scale()*scale);if(limit<3)limit=3;int used=0;const char*p=s;
 while(*p&&n+1<sizeof(tmp)){int adv=glyph_width(*p);if(used+adv>limit-3){tmp[n++]='.';tmp[n++]='.';tmp[n++]='.';break;}tmp[n++]=*p++;used+=adv;}
 tmp[n]=0;draw_text(bx,by,tmp,c,scale);
}
static void draw_dropdown_row(int x,int y,int w,const char*label,const char*value,u32 ink,u32 muted,u32 soft){
 round_rect(x,y,w,42,12,soft,245);draw_text_fit(x+16,y+12,label,ink,1,w-470);
 int bx=x+w-440;round_rect(bx,y+4,420,34,10,C_WHITE,230);hline(bx+1,y+5,418,C_WHITE,220);draw_text_fit(bx+12,y+11,value,C_ACCENT,1,360);draw_text(bx+386,y+11,settings_dropdown?"v":"v",muted,1);
}
static void draw_wallpaper(void){if(wallpaper){for(u32 y=0;y<screen_h;y++){u32 sy=(u64)y*WALL_H/screen_h;for(u32 x=0;x<screen_w;x++){u32 sx=(u64)x*WALL_W/screen_w;u8*r=wallpaper+((u64)sy*WALL_W+sx)*3;backbuffer[(u64)y*screen_w+x]=COLOR(r[0],r[1],r[2]);}}}else{for(u32 y=0;y<screen_h;y++)for(u32 x=0;x<screen_w;x++){u32 t=(u64)y*100/screen_h;backbuffer[(u64)y*screen_w+x]=COLOR(11+t/2,32+t/3,47+t/2);}}}
static void blur_dock(void){int y0=Y(850);if(y0<0)y0=0;u32 bh=screen_h-y0;if(!bh||!blur_a||!blur_b||bh>blur_band_h)return;u32 w=screen_w;int radius=(screen_w>=1500)?9:5;
 for(u32 y=0;y<bh;y++){u32 yy=y+y0;for(u32 x=0;x<w;x++){u64 sr=0,sg=0,sb=0;u32 n=0;for(int k=-radius;k<=radius;k++){int xx=(int)x+k;if(xx<0)xx=0;if(xx>=(int)w)xx=(int)w-1;u32 p=backbuffer[(u64)yy*w+xx];sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;n++;}blur_a[(u64)y*w+x]=COLOR(sr/n,sg/n,sb/n);}}
 for(u32 y=0;y<bh;y++)for(u32 x=0;x<w;x++){u64 sr=0,sg=0,sb=0;u32 n=0;for(int k=-radius;k<=radius;k++){int yy=(int)y+k;if(yy<0)yy=0;if(yy>=(int)bh)yy=(int)bh-1;u32 p=blur_a[(u64)yy*w+x];sr+=(p>>16)&255;sg+=(p>>8)&255;sb+=p&255;n++;}blur_b[(u64)y*w+x]=COLOR(sr/n,sg/n,sb/n);}
 memcpy(backbuffer+(u64)y0*w,blur_b,(size_t)w*bh*4);
}
static void blur_active_window(void){
 if(!glass_on||!glass_tmp||active_app==APP_NONE)return;
 int x0=X(win_x+2),y0=Y(win_y+2),x1=X(win_x+win_w-2),y1=Y(win_y+win_h-2);if(x0<0)x0=0;if(y0<0)y0=0;if(x1>(int)screen_w)x1=screen_w;if(y1>(int)screen_h)y1=screen_h;if(x1<=x0||y1<=y0)return;
 int radius=screen_w>=1500?9:5;int hx0=x0-radius;if(hx0<0)hx0=0;int hx1=x1+radius;if(hx1>(int)screen_w)hx1=screen_w;int hy0=y0-radius;if(hy0<0)hy0=0;int hy1=y1+radius;if(hy1>(int)screen_h)hy1=screen_h;u32 w=screen_w;
 /* Cheap separable three-tap blur keeps a glass surface perceptible without a full-resolution convolution. */
 for(int y=hy0;y<hy1;y++){for(int x=hx0;x<hx1;x++){int xa=x-radius;if(xa<0)xa=0;int xb=x+radius;if(xb>=(int)w)xb=(int)w-1;u32 a=backbuffer[(u64)y*w+xa],b=backbuffer[(u64)y*w+x],c=backbuffer[(u64)y*w+xb];u32 r=(((a>>16)&255)+2*((b>>16)&255)+((c>>16)&255))/4,g=(((a>>8)&255)+2*((b>>8)&255)+((c>>8)&255))/4,bl=((a&255)+2*(b&255)+(c&255))/4;glass_tmp[(u64)y*w+x]=COLOR(r,g,bl);}}
 for(int y=y0;y<y1;y++){int ya=y-radius;if(ya<0)ya=0;int yb=y+radius;if(yb>=(int)screen_h)yb=(int)screen_h-1;for(int x=x0;x<x1;x++){u32 a=glass_tmp[(u64)ya*w+x],b=glass_tmp[(u64)y*w+x],c=glass_tmp[(u64)yb*w+x];u32 r=(((a>>16)&255)+2*((b>>16)&255)+((c>>16)&255))/4,g=(((a>>8)&255)+2*((b>>8)&255)+((c>>8)&255))/4,bl=((a&255)+2*(b&255)+(c&255))/4;u64 off=(u64)y*w+x;backbuffer[off]=COLOR(r,g,bl);}}
}
static void prepare_window_glass(void){
 if(!glass_on||active_app==APP_NONE||!glass_tmp){glass_cache_valid=0;return;}
 int x0=X(win_x+2),y0=Y(win_y+2),x1=X(win_x+win_w-2),y1=Y(win_y+win_h-2);if(x0<0)x0=0;if(y0<0)y0=0;if(x1>(int)screen_w)x1=screen_w;if(y1>(int)screen_h)y1=screen_h;if(x1<=x0||y1<=y0){glass_cache_valid=0;return;}
 int same=glass_cache_valid&&glass_cache_app==active_app&&glass_cache_x==win_x&&glass_cache_y==win_y&&glass_cache_w==win_w&&glass_cache_h==win_h&&glass_cache_theme==theme;
 if(same){for(int y=y0;y<y1;y++)memcpy(backbuffer+(u64)y*screen_w+x0,glass_tmp+(u64)y*screen_w+x0,(size_t)(x1-x0)*4);return;}
 blur_active_window();
 for(int y=y0;y<y1;y++)memcpy(glass_tmp+(u64)y*screen_w+x0,backbuffer+(u64)y*screen_w+x0,(size_t)(x1-x0)*4);
 glass_cache_valid=1;glass_cache_app=active_app;glass_cache_x=win_x;glass_cache_y=win_y;glass_cache_w=win_w;glass_cache_h=win_h;glass_cache_theme=theme;
}
static void present(void){volatile u32 *dst=(volatile u32*)(uintptr_t)gop->Mode->FrameBufferBase;u32 fmt=gop->Mode->Info->PixelFormat;for(u32 y=0;y<screen_h;y++){u32*src=backbuffer+(u64)y*screen_w;volatile u32*d=dst+(u64)y*pitch;if(fmt==0){for(u32 x=0;x<screen_w;x++){u32 p=src[x];d[x]=((p&255)<<16)|(p&0x00ff00)|((p>>16)&255);}}else{for(u32 x=0;x<screen_w;x++)d[x]=src[x];}}}
static void draw_line_px(int x0,int y0,int x1,int y1,u32 c){int dx=x1>x0?x1-x0:x0-x1,sx=x0<x1?1:-1,dy=-(y1>y0?y1-y0:y0-y1),sy=y0<y1?1:-1,err=dx+dy;for(;;){pset(x0,y0,c);if(x0==x1&&y0==y1)break;int e2=2*err;if(e2>=dy){err+=dy;x0+=sx;}if(e2<=dx){err+=dx;y0+=sy;}}}
static void draw_cursor(void){int x=mouse_x,y=mouse_y;u32 edge=COLOR(7,19,27),ink=cursor_style==1?C_ACCENT:COLOR(255,255,255);if(cursor_style==2){draw_line_px(x-11,y,x+11,y,edge);draw_line_px(x,y-11,x,y+11,edge);draw_line_px(x-8,y,x+8,y,ink);draw_line_px(x,y-8,x,y+8,ink);round_rect(x-2,y-2,5,5,2,ink,255);return;}for(int row=0;row<24;row++){int width=row<15?row/2+1:(row<20?7-(row-15):3);if(width<1)width=1;for(int col=0;col<=width;col++){u32 c=(col==0||col==width||row==0||row==23)?edge:ink;pset(x+col,y+row,c);}}draw_line_px(x+5,y+16,x+10,y+24,edge);draw_line_px(x+6,y+16,x+11,y+23,edge);draw_line_px(x+7,y+16,x+10,y+22,ink);}
static void set_theme(int t){theme=(t%3+3)%3;C_ACCENT=themes[theme][0];C_DARK=themes[theme][1];C_PANEL=themes[theme][2];C_TEXT=themes[theme][3];C_MUTED=themes[theme][4];C_ORANGE=themes[theme][5];C_WHITE=themes[theme][6];C_BORDER=themes[theme][7];}

static EFI_STATUS open_file(EFI_FILE_PROTOCOL*root,const char*path,u64 mode,u64 attr,EFI_FILE_PROTOCOL**out){CHAR16 p[256];if(!root)return EFI_UNSUPPORTED;u16_ascii(p,path);return root->Open(root,out,p,mode,attr);}
static EFI_STATUS read_file(EFI_FILE_PROTOCOL*root,const char*path,void*buf,UINTN cap,UINTN*readn){EFI_FILE_PROTOCOL*f=0;EFI_STATUS s=open_file(root,path,EFI_FILE_MODE_READ,0,&f);if(EFI_ERROR(s))return s;UINTN total=0;while(total<cap){UINTN n=cap-total;s=f->Read(f,&n,(u8*)buf+total);if(EFI_ERROR(s)||!n)break;total+=n;}f->Close(f);if(readn)*readn=total;return EFI_ERROR(s)?s:EFI_SUCCESS;}
static EFI_STATUS write_file(EFI_FILE_PROTOCOL*root,const char*path,const void*buf,UINTN n){EFI_FILE_PROTOCOL*f=0;EFI_STATUS s;if(!root)return EFI_UNSUPPORTED;s=open_file(root,path,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0,&f);if(!EFI_ERROR(s)&&f){f->Delete(f);f=0;}s=open_file(root,path,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0,&f);if(EFI_ERROR(s)||!f)return s;if(f->SetPosition)f->SetPosition(f,0);UINTN sz=n;s=f->Write(f,&sz,(void*)buf);if(!EFI_ERROR(s))f->Flush(f);f->Close(f);return EFI_ERROR(s)?s:(sz==n?EFI_SUCCESS:EFI_LOAD_ERROR);}
static int ensure_dir(EFI_FILE_PROTOCOL*root,const char*path){EFI_FILE_PROTOCOL*f=0;EFI_STATUS s=open_file(root,path,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY,&f);if(EFI_ERROR(s))return 0;f->Close(f);return 1;}
static int file_exists(EFI_FILE_PROTOCOL*root,const char*path){EFI_FILE_PROTOCOL*f=0;EFI_STATUS s=open_file(root,path,EFI_FILE_MODE_READ,0,&f);if(EFI_ERROR(s))return 0;f->Close(f);return 1;}
static void ensure_seed_files(void){if(!data_root)return;ensure_dir(data_root,"\\MINTOS26");ensure_dir(data_root,"\\MINTOS26\\DOCUMENTS");ensure_dir(data_root,"\\MINTOS26\\LOGS");if(!file_exists(data_root,"\\MINTOS26\\DOCUMENTS\\WELCOME.TXT")){const char*m="Welcome to MintOS26 26.5.010.0. This volume is backed by your firmware's writable filesystem driver.\r\n";write_file(data_root,"\\MINTOS26\\DOCUMENTS\\WELCOME.TXT",m,strlen(m));}if(!file_exists(data_root,"\\MINTOS26\\DOCUMENTS\\NOTES.TXT")){const char*m="Write your thoughts here.\r\n";write_file(data_root,"\\MINTOS26\\DOCUMENTS\\NOTES.TXT",m,strlen(m));}if(!file_exists(data_root,"\\MINTOS26\\LOGS\\BOOT.TXT")){const char*m="MintOS26 UEFI started.\r\n";write_file(data_root,"\\MINTOS26\\LOGS\\BOOT.TXT",m,strlen(m));}}
static void add_fs_volume(EFI_HANDLE h,EFI_FILE_PROTOCOL *root,int writable,u32 block_size,u64 blocks,const char *label){
 if(!root||fs_volume_count>=MAX_FS_VOLUMES)return;
 MINT_VOLUME *v=&fs_volumes[fs_volume_count++];v->handle=h;v->root=root;v->writable=writable;v->block_size=block_size;v->total_bytes=(u64)block_size*blocks;copy_text(v->label,sizeof(v->label),label);
}
static void find_storage(EFI_HANDLE image){
 fs_volume_count=0;data_root=0;
 if(boot_root)add_fs_volume(boot_device,boot_root,0,0,0,"Boot volume (read-only)");
 EFI_HANDLE*handles=0;UINTN count=0;EFI_STATUS s=bs->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL,&GUID_SFS,0,&count,&handles);
 if(EFI_ERROR(s)||!handles){active_root=boot_root;copy_text(storage_label,sizeof(storage_label),"Storage: UEFI file-system handles unavailable");return;}
 for(UINTN i=0;i<count&&fs_volume_count<MAX_FS_VOLUMES;i++){
  EFI_HANDLE h=handles[i];if(h==boot_device)continue;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*fs=0;s=bs->HandleProtocol(h,&GUID_SFS,(void**)&fs);if(EFI_ERROR(s)||!fs)continue;
  EFI_FILE_PROTOCOL*root=0;s=fs->OpenVolume(fs,&root);if(EFI_ERROR(s)||!root)continue;
  EFI_BLOCK_IO_PROTOCOL*bio=0;int writable=0;u32 bsiz=0;u64 blocks=0;
  if(!EFI_ERROR(bs->HandleProtocol(h,&GUID_BLOCK_IO,(void**)&bio))&&bio&&bio->Media&&bio->Media->MediaPresent){bsiz=bio->Media->BlockSize;blocks=bio->Media->LastBlock+1;writable=!bio->Media->ReadOnly;}
  char label[64];char nn[24];u64_to_dec(nn,(u64)(fs_volume_count+1));label[0]=0;UINTN ln=0;append_str(label,sizeof(label),&ln,"Volume ");append_str(label,sizeof(label),&ln,nn);append_str(label,sizeof(label),&ln,writable?" (RW)":" (RO)");
  add_fs_volume(h,root,writable,bsiz,blocks,label);
  if(writable&&!data_root){data_root=root;ensure_dir(data_root,"\\MINTOS26");}
 }
 bs->FreePool(handles);
 active_root=data_root?data_root:boot_root;
 if(data_root)copy_text(storage_label,sizeof(storage_label),"Storage: writable UEFI volumes enumerated");
 else copy_text(storage_label,sizeof(storage_label),"Storage: no writable UEFI volume; changes are volatile");
 (void)image;
}
static void finder_current_root(void){
 if(finder_volume<fs_volume_count&&fs_volumes[finder_volume].root)active_root=fs_volumes[finder_volume].root;
 else active_root=data_root?data_root:boot_root;
}
static int finder_path_join(char *out,UINTN cap,const char *name){
 UINTN n=0;out[0]=0;append_str(out,cap,&n,finder_path[0]?finder_path:"\\");if(n>1&&out[n-1]=='\\')n--;if(!(n==1&&out[0]=='\\')&&n+1<cap){out[n++]='\\';out[n]=0;}append_str(out,cap,&n,name);return n+1<cap;
}
static void finder_refresh(void){
 finder_current_root();finder_entry_count=0;finder_scroll=0;finder_selected=-1;finder_preview_open=0;finder_preview_name[0]=0;finder_preview[0]=0;
 if(!active_root){copy_text(finder_status,sizeof(finder_status),"No mounted filesystem is available.");return;}
 EFI_FILE_PROTOCOL*d=0;EFI_STATUS stx=open_file(active_root,finder_path,EFI_FILE_MODE_READ,0,&d);
 if(EFI_ERROR(stx)||!d){copy_text(finder_status,sizeof(finder_status),"Cannot open this directory; use Up to go back.");return;}
 for(int i=0;i<MAX_FINDER_ENTRIES;i++){
  u8 info_buf[2048];UINTN n=sizeof(info_buf);stx=d->Read(d,&n,info_buf);if(EFI_ERROR(stx)||!n)break;
  EFI_FILE_INFO*fi=(EFI_FILE_INFO*)info_buf;char name[96];ascii_from_u16(name,sizeof(name),fi->FileName);
  if(!name[0]||name[0]=='.')continue;
  FINDER_ENTRY*e=&finder_entries[finder_entry_count++];copy_text(e->name,sizeof(e->name),name);e->size=fi->FileSize;e->is_dir=(fi->Attribute&EFI_FILE_DIRECTORY)?1:0;
  if(finder_entry_count>=MAX_FINDER_ENTRIES)break;
 }
 d->Close(d);if(finder_entry_count)copy_text(finder_status,sizeof(finder_status),"Select an item; double-click or press Enter to open.");else copy_text(finder_status,sizeof(finder_status),"This folder is empty.");
}
static void finder_parent(void){
 UINTN n=strlen(finder_path);while(n>1&&finder_path[n-1]=='\\')finder_path[--n]=0;
 if(n<=1){copy_text(finder_path,sizeof(finder_path),"\\");finder_refresh();return;}
 while(n>1&&finder_path[n-1]!='\\')finder_path[--n]=0;
 if(n>1)finder_path[n-1]=0;else finder_path[1]=0;
 if(!finder_path[0])copy_text(finder_path,sizeof(finder_path),"\\");finder_refresh();
}
static int finder_selected_path(char *out,UINTN cap){if(finder_selected<0||(UINTN)finder_selected>=finder_entry_count)return 0;return finder_path_join(out,cap,finder_entries[finder_selected].name);}
static void finder_open_selected(void){
 if(finder_selected<0||(UINTN)finder_selected>=finder_entry_count)return;FINDER_ENTRY*e=&finder_entries[finder_selected];
 if(e->is_dir){char next[256];if(!finder_path_join(next,sizeof(next),e->name)){copy_text(finder_status,sizeof(finder_status),"Path is too long.");return;}copy_text(finder_path,sizeof(finder_path),next);finder_refresh();return;}
 char full[512];if(!finder_selected_path(full,sizeof(full)))return;UINTN n=0;EFI_STATUS rs=read_file(active_root,full,finder_preview,sizeof(finder_preview)-1,&n);
 if(EFI_ERROR(rs)){copy_text(finder_status,sizeof(finder_status),"Could not read the selected file.");return;}finder_preview[n]=0;copy_text(finder_preview_name,sizeof(finder_preview_name),e->name);finder_preview_open=1;copy_text(finder_status,sizeof(finder_status),"Text preview loaded (read-only). Use Notes or Code Studio to edit supported files.");
}
static void finder_create_item(int directory){
 if(finder_volume>=fs_volume_count||!fs_volumes[finder_volume].writable){copy_text(finder_status,sizeof(finder_status),"This volume is read-only.");return;}
 char full[512];const char *name=directory?"New Folder":"New File.txt";if(!finder_path_join(full,sizeof(full),name)){copy_text(finder_status,sizeof(finder_status),"Path is too long.");return;}
 EFI_FILE_PROTOCOL*f=0;EFI_STATUS rs=open_file(active_root,full,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,directory?EFI_FILE_DIRECTORY:0,&f);
 if(EFI_ERROR(rs)||!f){copy_text(finder_status,sizeof(finder_status),directory?"Could not create folder (name may exist).":"Could not create file (name may exist).");return;}
 if(!directory){const char *txt="New MintOS26 text file.\r\n";UINTN size=strlen(txt);rs=f->Write(f,&size,(void*)txt);if(EFI_ERROR(rs))copy_text(finder_status,sizeof(finder_status),"File created but initial text could not be written.");else copy_text(finder_status,sizeof(finder_status),"Created New File.txt.");}else copy_text(finder_status,sizeof(finder_status),"Created New Folder.");
 if(f->Flush)f->Flush(f);f->Close(f);finder_refresh();
}
static void finder_delete_selected(void){
 if(finder_selected<0||(UINTN)finder_selected>=finder_entry_count){copy_text(finder_status,sizeof(finder_status),"Select a file first.");return;}
 if(finder_volume>=fs_volume_count||!fs_volumes[finder_volume].writable){copy_text(finder_status,sizeof(finder_status),"This volume is read-only.");return;}
 char full[512];if(!finder_selected_path(full,sizeof(full)))return;EFI_FILE_PROTOCOL*f=0;EFI_STATUS rs=open_file(active_root,full,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0,&f);
 if(EFI_ERROR(rs)||!f){copy_text(finder_status,sizeof(finder_status),"Could not open the selected item for deletion.");return;}
 rs=f->Delete(f);if(EFI_ERROR(rs))copy_text(finder_status,sizeof(finder_status),"Delete failed; folder may not be empty.");else copy_text(finder_status,sizeof(finder_status),"Item deleted.");finder_refresh();
}
static void load_wallpaper(EFI_HANDLE image){EFI_STATUS s;EFI_LOADED_IMAGE_PROTOCOL*loaded=0;s=bs->HandleProtocol(image,&GUID_LOADED_IMAGE,(void**)&loaded);if(EFI_ERROR(s)||!loaded)return;boot_device=loaded->DeviceHandle;EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*fs=0;s=bs->HandleProtocol(boot_device,&GUID_SFS,(void**)&fs);if(EFI_ERROR(s)||!fs)return;s=fs->OpenVolume(fs,&boot_root);if(EFI_ERROR(s)||!boot_root)return;wallpaper=0;s=bs->AllocatePool(EfiLoaderData,WALL_BYTES,(void**)&wallpaper);if(EFI_ERROR(s)||!wallpaper){wallpaper=0;return;}UINTN n=0;s=read_file(boot_root,"\\MINTOS26\\WALLPAPE.RGB",wallpaper,WALL_BYTES,&n);if(EFI_ERROR(s)||n!=WALL_BYTES){bs->FreePool(wallpaper);wallpaper=0;}}

/* Diagnostic PCI probe only: finds common audio controllers but does not claim
 * a native HDA/AC'97 PCM driver exists. The current audio backend is PC speaker.
 */
static u32 pci_cfg_read32(u8 bus,u8 dev,u8 fn,u8 reg){u32 a=0x80000000u|((u32)bus<<16)|((u32)dev<<11)|((u32)fn<<8)|(reg&0xfcu);__asm__ volatile("outl %0, %1"::"a"(a),"Nd"((u16)0x0cf8));u32 v;__asm__ volatile("inl %1, %0":"=a"(v):"Nd"((u16)0x0cfc));return v;}
static void detect_audio_controller(void){copy_text(audio_label,sizeof(audio_label),"Audio: no compatible PCI audio controller detected; no PCM driver");for(u32 bus=0;bus<8;bus++){for(u32 dev=0;dev<32;dev++){u32 id0=pci_cfg_read32((u8)bus,(u8)dev,0,0);if((id0&0xffffu)==0xffffu)continue;u32 hdr=pci_cfg_read32((u8)bus,(u8)dev,0,0x0c);u32 nf=((hdr>>23)&1)?8:1;for(u32 fn=0;fn<nf;fn++){u32 id=pci_cfg_read32((u8)bus,(u8)dev,(u8)fn,0);if((id&0xffffu)==0xffffu)continue;u32 cc=pci_cfg_read32((u8)bus,(u8)dev,(u8)fn,8);u32 cls=(cc>>24)&255,sub=(cc>>16)&255;if(cls==4&&(sub==1||sub==3)){if(sub==3)copy_text(audio_label,sizeof(audio_label),"Audio: HDA-class controller detected; native driver not loaded");else copy_text(audio_label,sizeof(audio_label),"Audio: AC'97-class controller detected; native driver not loaded");return;}}}}}

static void speaker_tone(u32 hz,u32 ms){if(!sound_on||hz<80||hz>4000)return;u16 div=(u16)(1193182u/hz);u8 p61;__asm__ volatile("outb %0, %1"::"a"((u8)0xB6),"Nd"((u16)0x43));__asm__ volatile("outb %0, %1"::"a"((u8)(div&255)),"Nd"((u16)0x42));__asm__ volatile("outb %0, %1"::"a"((u8)(div>>8)),"Nd"((u16)0x42));__asm__ volatile("inb %1, %0":"=a"(p61):"Nd"((u16)0x61));p61|=3;__asm__ volatile("outb %0, %1"::"a"(p61),"Nd"((u16)0x61));bs->Stall(ms*1000);p61&=(u8)~3;__asm__ volatile("outb %0, %1"::"a"(p61),"Nd"((u16)0x61));}
static void speaker_chime(void){if(!sound_on)return;speaker_tone(659,100);bs->Stall(25000);speaker_tone(784,100);bs->Stall(25000);speaker_tone(988,160);}
static void load_settings(void){if(!data_root)return;char b[256];UINTN n=0;if(!EFI_ERROR(read_file(data_root,"\\MINTOS26\\SETTINGS.CFG",b,sizeof(b)-1,&n))){b[n]=0;for(UINTN i=0;i<n;i++){if(str_starts(b+i,"THEME=1"))set_theme(1);if(str_starts(b+i,"THEME=2"))set_theme(2);if(str_starts(b+i,"THEME=0"))set_theme(0);if(str_starts(b+i,"SOUND=0"))sound_on=0;if(str_starts(b+i,"SOUND=1"))sound_on=1;if(str_starts(b+i,"GLASS=0"))glass_on=0;if(str_starts(b+i,"GLASS=1"))glass_on=1;if(str_starts(b+i,"LANG=1"))locale_id=1;if(str_starts(b+i,"LANG=2"))locale_id=2;if(str_starts(b+i,"LANG=0"))locale_id=0;if(str_starts(b+i,"TZ=1"))timezone_id=1;if(str_starts(b+i,"TZ=2"))timezone_id=2;if(str_starts(b+i,"TZ=3"))timezone_id=3;if(str_starts(b+i,"TZ=0"))timezone_id=0;if(str_starts(b+i,"TIME24=0"))clock_24h=0;if(str_starts(b+i,"TIME24=1"))clock_24h=1;if(str_starts(b+i,"CURSOR=1"))cursor_style=1;if(str_starts(b+i,"CURSOR=2"))cursor_style=2;if(str_starts(b+i,"CURSOR=0"))cursor_style=0;}}}
static void save_settings(void){if(!data_root)return;char b[256];UINTN n=0;append_str(b,sizeof(b),&n,"THEME=");append_char(b,sizeof(b),&n,(char)('0'+theme));append_str(b,sizeof(b),&n,"\r\nSOUND=");append_char(b,sizeof(b),&n,sound_on?'1':'0');append_str(b,sizeof(b),&n,"\r\nGLASS=");append_char(b,sizeof(b),&n,glass_on?'1':'0');append_str(b,sizeof(b),&n,"\r\nLANG=");append_char(b,sizeof(b),&n,(char)('0'+locale_id));append_str(b,sizeof(b),&n,"\r\nTZ=");append_char(b,sizeof(b),&n,(char)('0'+timezone_id));append_str(b,sizeof(b),&n,"\r\nTIME24=");append_char(b,sizeof(b),&n,clock_24h?'1':'0');append_str(b,sizeof(b),&n,"\r\nCURSOR=");append_char(b,sizeof(b),&n,(char)('0'+cursor_style));append_str(b,sizeof(b),&n,"\r\n");write_file(data_root,"\\MINTOS26\\SETTINGS.CFG",b,n);}
static void save_notes(void){if(!data_root)return;write_file(data_root,"\\MINTOS26\\DOCUMENTS\\NOTES.TXT",note_text,note_len);}
static void load_notes(void){note_len=0;note_text[0]=0;if(data_root){UINTN n=0;if(!EFI_ERROR(read_file(data_root,"\\MINTOS26\\DOCUMENTS\\NOTES.TXT",note_text,sizeof(note_text)-1,&n))){note_len=n;note_text[n]=0;return;}}const char*m="Welcome to MintOS26 Notes.\nAttach a writable FAT volume to save notes.";note_len=strlen(m);memcpy(note_text,m,note_len+1);}
static void set_msg(const char*s){UINTN n=0;terminal_msg[0]=0;append_str(terminal_msg,sizeof(terminal_msg),&n,s);}
static void save_notes_and_status(void);
static void init_calendar(void); static void load_code(void); static void init_network_diag(void);
static void start_app(int app){active_app=app;launcher_open=0;win_x=300;win_y=150;win_w=1320;win_h=760;if(app==APP_FINDER){finder_path[0]=0;copy_text(finder_path,sizeof(finder_path),data_root?"\\MINTOS26\\DOCUMENTS":"\\");finder_volume=0;for(UINTN vi=0;vi<fs_volume_count;vi++)if(data_root&&fs_volumes[vi].root==data_root){finder_volume=vi;break;}finder_scroll=0;finder_refresh();}if(app==APP_NOTES)load_notes();if(app==APP_TERMINAL){cmd_len=0;cmdline[0]=0;}if(app==APP_CALCULATOR){calculator_expr[0]=0;calc_result[0]=0;}if(app==APP_CALENDAR)init_calendar();if(app==APP_CODE)load_code();if(app==APP_NETWORK)init_network_diag();if(app==APP_INSTALLER)copy_text(installer_status,sizeof(installer_status),data_root?"Ready to stage a recovery bundle.":"Attach a writable FAT data disk first.");if(app==APP_SETTINGS)settings_page=0;}
static void close_app(void){if(active_app==APP_NOTES)save_notes_and_status();active_app=APP_NONE;dragging=0;}
static void output_dir(EFI_FILE_PROTOCOL*root,char*out,UINTN cap){if(!root){const char*m="No filesystem mounted";memcpy(out,m,strlen(m)+1);return;}EFI_FILE_PROTOCOL*d=0;EFI_STATUS s=open_file(root,"\\MINTOS26\\DOCUMENTS",EFI_FILE_MODE_READ,0,&d);if(EFI_ERROR(s)){const char*m="MINTOS26/\n  DOCUMENTS/\n  LOGS/";memcpy(out,m,strlen(m)+1);return;}out[0]=0;UINTN used=0;u8 info_buf[1024];for(int i=0;i<16;i++){UINTN n=sizeof(info_buf);s=d->Read(d,&n,info_buf);if(EFI_ERROR(s)||!n)break;EFI_FILE_INFO*fi=(EFI_FILE_INFO*)info_buf;char name[80];ascii_from_u16(name,sizeof(name),fi->FileName);if(name[0]=='.')continue;append_str(out,cap,&used,"  ");append_str(out,cap,&used,name);if(fi->Attribute&EFI_FILE_DIRECTORY)append_str(out,cap,&used,"/");append_str(out,cap,&used,"\n");}d->Close(d);if(!used){const char*m="  (empty)\n";memcpy(out,m,strlen(m)+1);}}
/* Experimental ELF64 loader. Normal boot still remains in the UEFI desktop;
 * only the explicit BOOTKERNEL command exits boot services and transfers control. */
typedef struct {
 u8 ident[16]; u16 type,machine; u32 version; u64 entry,phoff,shoff; u32 flags;
 u16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx;
} ELF64_EHDR;
typedef struct { u32 type,flags; u64 offset,vaddr,paddr,filesz,memsz,align; } ELF64_PHDR;
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES_FN)(u32,u32,UINTN,u64*);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES_FN)(u64,UINTN);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP_FN)(UINTN*,void*,UINTN*,UINTN*,u32*);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES_FN)(EFI_HANDLE,UINTN);
typedef struct { EFI_GUID Guid; void *VendorTable; } EFI_CONFIGURATION_TABLE;
#define ELF_PT_LOAD 1u
#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_ET_EXEC 2u
#define ELF_EM_X86_64 62u
#define EFI_ALLOCATE_ANY_PAGES 0u
#define EFI_ALLOCATE_ADDRESS 2u
#define EFI_MEMORY_LOADER_CODE 1u
#define EFI_MEMORY_LOADER_DATA 2u
#define EFI_PAGE_SIZE 4096ULL
#define MINT_KERNEL_MAX_BYTES (24u*1024u*1024u) /* 26.5.010.0: kernel embeds light+dark 1080p wallpapers */

__attribute__((noreturn)) static void enter_native_kernel(u64 stack_top,u64 entry,MINT_BOOT_INFO *info){
 /* Microsoft x64 ABI: RCX holds the first argument, with 32 bytes of shadow
    space and 16-byte stack alignment before CALL. The kernel is never expected
    to return; if it does, remain safely halted. */
 __asm__ volatile(
  "movq %0, %%r10\n\t"
  "movq %1, %%r11\n\t"
  "movq %2, %%r8\n\t"
  "movq %%r10, %%rsp\n\t"
  "andq $-16, %%rsp\n\t"
  "subq $32, %%rsp\n\t"
  "movq %%r8, %%rcx\n\t"
  "callq *%%r11\n\t"
  "cli\n\t"
  "1: hlt\n\t"
  "jmp 1b\n\t"
  : : "r"(stack_top),"r"(entry),"r"(info)
  : "r10","r11","r8","rcx","rdx","r9","memory");
 __builtin_unreachable();
}

static int launch_native_kernel(void){
 if(!boot_root||!bs||!gop||!gop->Mode||!gop->Mode->Info){set_msg("BOOTKERNEL: boot volume or GOP unavailable.");return 0;}
 UINTN file_cap=MINT_KERNEL_MAX_BYTES,file_size=0;void *file=0;
 if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,file_cap,&file))||!file){set_msg("BOOTKERNEL: could not allocate ELF buffer.");return 0;}
 EFI_STATUS rs=read_file(boot_root,"\\MINTOS26\\KERNEL.ELF",file,file_cap,&file_size);
 if(EFI_ERROR(rs)||file_size<sizeof(ELF64_EHDR)){bs->FreePool(file);set_msg("BOOTKERNEL: MINTOS26/KERNEL.ELF is missing or unreadable.");return 0;}
 ELF64_EHDR *eh=(ELF64_EHDR*)file;
 if(eh->ident[0]!=0x7f||eh->ident[1]!='E'||eh->ident[2]!='L'||eh->ident[3]!='F'||
    eh->ident[4]!=2||eh->ident[5]!=1||eh->type!=ELF_ET_EXEC||eh->machine!=ELF_EM_X86_64||
    eh->phentsize!=sizeof(ELF64_PHDR)||!eh->phnum||eh->phnum>32||
    eh->phoff>file_size||(u64)eh->phnum*sizeof(ELF64_PHDR)>file_size-eh->phoff){
  bs->FreePool(file);set_msg("BOOTKERNEL: unsupported or malformed x86-64 ELF.");return 0;
 }
 ELF64_PHDR *ph=(ELF64_PHDR*)((u8*)file+eh->phoff);u64 low=~0ULL,high=0;UINTN loads=0;int entry_in_exec=0;
 for(UINTN i=0;i<eh->phnum;i++)if(ph[i].type==ELF_PT_LOAD){
  if(ph[i].vaddr!=ph[i].paddr||ph[i].filesz>ph[i].memsz||ph[i].offset>file_size||ph[i].filesz>file_size-ph[i].offset||ph[i].memsz==0||ph[i].paddr>~0ULL-ph[i].memsz||
     (ph[i].paddr&(EFI_PAGE_SIZE-1))||(ph[i].offset&(EFI_PAGE_SIZE-1))||((ph[i].flags&ELF_PF_W)&&(ph[i].flags&ELF_PF_X))){bs->FreePool(file);set_msg("BOOTKERNEL: invalid/alignment or W+X load segment.");return 0;}
  u64 raw_end=ph[i].paddr+ph[i].memsz;if(raw_end>~0ULL-(EFI_PAGE_SIZE-1)){bs->FreePool(file);set_msg("BOOTKERNEL: load segment end overflow.");return 0;}
  u64 lo=ph[i].paddr,hi=(raw_end+EFI_PAGE_SIZE-1)&~(EFI_PAGE_SIZE-1);
  if(hi<=lo){bs->FreePool(file);set_msg("BOOTKERNEL: segment address overflow.");return 0;}
  /* Loadable segments must not overlap at page granularity. */
  for(UINTN j=0;j<i;j++)if(ph[j].type==ELF_PT_LOAD){u64 je=ph[j].paddr+ph[j].memsz;je=(je+EFI_PAGE_SIZE-1)&~(EFI_PAGE_SIZE-1);if(lo<je&&ph[j].paddr<hi){bs->FreePool(file);set_msg("BOOTKERNEL: overlapping ELF load segments.");return 0;}}
  if(lo<low)low=lo;if(hi>high)high=hi;loads++;
  if((ph[i].flags&ELF_PF_X)&&!(ph[i].flags&ELF_PF_W)&&eh->entry>=lo&&eh->entry<raw_end)entry_in_exec=1;
 }
 if(!loads||low==~0ULL||high<=low||high-low>32ULL*1024*1024||!entry_in_exec){bs->FreePool(file);set_msg("BOOTKERNEL: no valid bounded executable load image/entry point.");return 0;}
 EFI_ALLOCATE_PAGES_FN alloc_pages=(EFI_ALLOCATE_PAGES_FN)bs->AllocatePages;
 if(!alloc_pages){bs->FreePool(file);set_msg("BOOTKERNEL: AllocatePages unavailable.");return 0;}
 /* Match ELF segment permissions to UEFI memory types: RX code and RW data
    are page-aligned and allocated separately. This avoids relying on one
    accidentally RWX span when firmware page protections are active. */
 u64 allocated_base[32];UINTN allocated_count[32];UINTN allocated_n=0;u64 kernel_entry_addr=eh->entry;
 for(UINTN i=0;i<eh->phnum;i++)if(ph[i].type==ELF_PT_LOAD){
  u64 seg_base=ph[i].paddr;u64 raw_end=ph[i].paddr+ph[i].memsz;u64 seg_end=(raw_end+EFI_PAGE_SIZE-1)&~(EFI_PAGE_SIZE-1);UINTN seg_pages=(UINTN)((seg_end-seg_base)/EFI_PAGE_SIZE);u64 alloc_addr=seg_base;
  u32 mem_type=(ph[i].flags&ELF_PF_X)?EFI_MEMORY_LOADER_CODE:EFI_MEMORY_LOADER_DATA;
  EFI_STATUS ars=alloc_pages(EFI_ALLOCATE_ADDRESS,mem_type,seg_pages,&alloc_addr);
  if(EFI_ERROR(ars)||alloc_addr!=seg_base){
   for(UINTN r=0;r<allocated_n;r++)if(bs->FreePages)((EFI_FREE_PAGES_FN)bs->FreePages)(allocated_base[r],allocated_count[r]);
   bs->FreePool(file);set_msg("BOOTKERNEL: could not reserve permission-separated ELF segments.");return 0;
  }
  allocated_base[allocated_n]=seg_base;allocated_count[allocated_n]=seg_pages;allocated_n++;
  volatile u8 *dst=(volatile u8*)(uintptr_t)seg_base;for(u64 j=0;j<seg_pages*EFI_PAGE_SIZE;j++)dst[j]=0;
  dst=(volatile u8*)(uintptr_t)ph[i].paddr;const u8 *src=(const u8*)file+ph[i].offset;
  for(u64 j=0;j<ph[i].filesz;j++)dst[j]=src[j];
 }
 bs->FreePool(file);file=0;
 /* Allocate a dedicated 64 KiB stack and boot-info block before freezing the map. */
 u64 stack_base=0; if(EFI_ERROR(alloc_pages(EFI_ALLOCATE_ANY_PAGES,EfiLoaderData,16,&stack_base))||!stack_base){set_msg("BOOTKERNEL: kernel loaded, but stack allocation failed; reboot advised.");return 0;}
 MINT_BOOT_INFO *info=0;if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,sizeof(MINT_BOOT_INFO),(void**)&info))||!info){set_msg("BOOTKERNEL: boot ABI allocation failed; reboot advised.");return 0;}
 memset(info,0,sizeof(*info));info->magic=MINT_BOOT_INFO_MAGIC;info->abi_version=MINT_BOOT_INFO_ABI;info->struct_size=sizeof(*info);
 info->framebuffer.base=gop->Mode->FrameBufferBase;info->framebuffer.size=gop->Mode->FrameBufferSize;
 info->framebuffer.width=gop->Mode->Info->HorizontalResolution;info->framebuffer.height=gop->Mode->Info->VerticalResolution;
 info->framebuffer.pixels_per_scanline=gop->Mode->Info->PixelsPerScanLine;info->framebuffer.pixel_format=gop->Mode->Info->PixelFormat;
 info->system_table_physical=(u64)(uintptr_t)st;
 /* ACPI 2.0 RSDP table pointer when firmware publishes it. */
 EFI_GUID acpi2={0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
 EFI_CONFIGURATION_TABLE *ct=(EFI_CONFIGURATION_TABLE*)st->ConfigurationTable;
 for(UINTN i=0;i<st->NumberOfTableEntries&&ct;i++)if(!memcmp(&ct[i].Guid,&acpi2,sizeof(EFI_GUID))){info->acpi_rsdp_physical=(u64)(uintptr_t)ct[i].VendorTable;break;}
 EFI_GET_MEMORY_MAP_FN get_map=(EFI_GET_MEMORY_MAP_FN)bs->GetMemoryMap;
 EFI_EXIT_BOOT_SERVICES_FN exit_bs=(EFI_EXIT_BOOT_SERVICES_FN)bs->ExitBootServices;
 if(!get_map||!exit_bs){set_msg("BOOTKERNEL: memory-map/exit services unavailable.");return 0;}
 UINTN map_size=0,map_key=0,desc_size=0;u32 desc_version=0;
 rs=get_map(&map_size,0,&map_key,&desc_size,&desc_version);
 if(rs!=EFI_BUFFER_TOO_SMALL||!desc_size){set_msg("BOOTKERNEL: could not size UEFI memory map.");return 0;}
 map_size+=desc_size*16+4096;void *map=0;
 if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,map_size,&map))||!map){set_msg("BOOTKERNEL: could not allocate memory map.");return 0;}
 /* After each failed ExitBootServices, only get the memory map and retry. */
 for(int tries=0;tries<3;tries++){
  UINTN actual=map_size;rs=get_map(&actual,map,&map_key,&desc_size,&desc_version);
  if(EFI_ERROR(rs)||actual>map_size){set_msg("BOOTKERNEL: failed to capture final memory map.");return 0;}
  info->memory_map_physical=(u64)(uintptr_t)map;info->memory_map_size=actual;
  info->memory_descriptor_size=desc_size;info->memory_descriptor_version=desc_version;
  rs=exit_bs(boot_image_handle,map_key);
  if(!EFI_ERROR(rs))enter_native_kernel(stack_base+16*EFI_PAGE_SIZE,kernel_entry_addr,info);
 }
 set_msg("BOOTKERNEL: ExitBootServices retries failed; reboot VM to recover.");return 0;
}

static void terminal_exec(void){cmdline[cmd_len]=0;while(cmd_len&&ascii_is_space(cmdline[cmd_len-1]))cmdline[--cmd_len]=0;char *p=cmdline;while(*p&&ascii_is_space(*p))p++;if(!*p){set_msg("");return;}if(str_eq(p,"BOOTKERNEL")||str_eq(p,"BOOTMINTDE")){set_msg("MintDE 2.0.0: experimental one-way kernel handoff...");if(!launch_native_kernel())start_app(APP_TERMINAL);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"CALENDAR")){start_app(APP_CALENDAR);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"CLOCK")){start_app(APP_CLOCK);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"CODE")){start_app(APP_CODE);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"NETWORK")){start_app(APP_NETWORK);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"INSTALL")){start_app(APP_INSTALLER);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"SETTINGS")){start_app(APP_SETTINGS);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"FILES")){start_app(APP_FINDER);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"NOTES")){start_app(APP_NOTES);cmd_len=0;cmdline[0]=0;return;}if(str_eq(p,"HELP"))set_msg("HELP  BOOTMINTDE  BOOTKERNEL  LS  CAT  WRITE <text>  THEME MINT|WARM|APRICOT  APPS  CALC  CODE  CLOCK  CALENDAR  NETWORK  INSTALL  UPTIME  ABOUT  CLEAR");else if(str_eq(p,"LS")){output_dir(active_root,terminal_msg,sizeof(terminal_msg));}else if(str_starts(p,"CAT")){char b[240];UINTN n=0;if(active_root&&!EFI_ERROR(read_file(active_root,"\\MINTOS26\\DOCUMENTS\\NOTES.TXT",b,sizeof(b)-1,&n))){b[n]=0;set_msg(b);}else set_msg("Could not read DOCUMENTS/NOTES.TXT");}else if(str_starts(p,"WRITE ")){const char*t=p+6;note_len=strlen(t);if(note_len>=sizeof(note_text))note_len=sizeof(note_text)-1;memcpy(note_text,t,note_len);note_text[note_len]=0;save_notes();set_msg(data_root?"Saved DOCUMENTS/NOTES.TXT":"No writable SATA volume; note is session-only");}else if(str_eq(p,"PWD"))set_msg("\\MINTOS26\\DOCUMENTS");else if(str_eq(p,"APPS")){launcher_open=1;active_app=APP_LAUNCHER;win_x=300;win_y=70;win_w=1320;win_h=930;set_msg("Applications launcher opened");}else if(str_eq(p,"ABOUT"))set_msg("MintOS26 26.5.010.0 UEFI Desktop");else if(str_eq(p,"SOUND")){sound_on=!sound_on;save_settings();if(sound_on)speaker_chime();set_msg(sound_on?"Startup chime enabled":"Startup chime disabled");}else if(str_eq(p,"SOUND TEST")||str_eq(p,"SAY CHIME")){speaker_chime();set_msg("No native audio driver yet. PC-speaker signal may be inaudible on this hardware.");}else if(str_starts(p,"THEME ")){char*t=p+6;if(str_eq(t,"MINT"))set_theme(1);else if(str_eq(t,"APRICOT"))set_theme(2);else set_theme(0);save_settings();set_msg("Appearance saved");}else if(str_eq(p,"UPTIME")){char b[64],n[24];u64_to_dec(n,ticks/100);UINTN z=0;b[0]=0;append_str(b,sizeof(b),&z,"Uptime: ");append_str(b,sizeof(b),&z,n);append_str(b,sizeof(b),&z," seconds");set_msg(b);}else if(str_eq(p,"CLEAR"))set_msg("Terminal cleared. Type HELP for commands.");else set_msg("Unknown command. Type HELP.");cmd_len=0;cmdline[0]=0;}
static void draw_icon(int cx,int cy,int kind,u32 bg){round_rect(cx-38,cy-38,76,76,20,bg,245);int s=kind;u32 white=COLOR(255,255,255);if(s==1){round_rect(cx-22,cy-16,44,33,5,white,255);fill_rect(cx-18,cy-20,36,8,COLOR(255,214,91),255);}else if(s==2){round_rect(cx-22,cy-24,44,48,7,COLOR(19,31,53),255);draw_text(cx-14,cy-7,">_",C_ACCENT,1);}else if(s==3){round_rect(cx-21,cy-26,42,52,4,white,255);fill_rect(cx-13,cy-15,26,3,C_ACCENT,255);fill_rect(cx-13,cy-5,26,3,C_ACCENT,255);fill_rect(cx-13,cy+5,19,3,C_ACCENT,255);}else if(s==4){round_rect(cx-23,cy-23,46,46,7,COLOR(25,35,50),255);for(int r=0;r<4;r++)for(int c=0;c<4;c++)round_rect(cx-15+c*9,cy-15+r*9,6,6,2,(r==3&&c==0)?C_ORANGE:COLOR(195,220,230),255);}else if(s==5){for(int a=0;a<8;a++){int xx=cx+((a==0||a==4)?0:(a<4?18:-18));int yy=cy+((a==2||a==6)?0:(a<2||a>6?-18:(a<4?18:-18)));round_rect(xx-5,yy-5,10,10,4,white,255);}round_rect(cx-14,cy-14,28,28,14,C_ACCENT,255);round_rect(cx-6,cy-6,12,12,6,white,255);}else if(s==6){round_rect(cx-22,cy-19,44,32,5,white,255);fill_rect(cx-4,cy+13,8,9,white,255);fill_rect(cx-15,cy+21,30,4,white,255);}else if(s==7){for(int r=0;r<3;r++)for(int c=0;c<3;c++)round_rect(cx-15+c*15,cy-15+r*15,10,10,4,white,255);}else if(s==8){round_rect(cx-24,cy-24,48,48,8,white,255);round_rect(cx-24,cy-24,48,15,6,COLOR(243,91,109),255);fill_rect(cx-24,cy-17,48,8,COLOR(243,91,109),255);for(int r=0;r<2;r++)for(int c=0;c<3;c++)round_rect(cx-15+c*14,cy-2+r*12,7,7,2,bg,255);}else if(s==9){round_rect(cx-26,cy-26,52,52,26,white,255);round_rect(cx-22,cy-22,44,44,22,COLOR(243,249,251),255);draw_line_px(cx,cy,cx,cy-15,bg);draw_line_px(cx,cy,cx+13,cy+7,bg);round_rect(cx-3,cy-3,6,6,3,bg,255);}else if(s==10){round_rect(cx-23,cy-27,46,54,7,COLOR(12,28,42),255);draw_text(cx-15,cy-9,"{ }",C_ACCENT,1);fill_rect(cx-14,cy+10,28,3,white,255);}else if(s==11){round_rect(cx-22,cy-22,44,44,22,white,255);round_rect(cx-17,cy-17,34,34,17,bg,255);draw_line_px(cx-20,cy,cx+20,cy,white);draw_line_px(cx,cy-20,cx,cy+20,white);round_rect(cx-5,cy-5,10,10,5,white,255);}else if(s==12){round_rect(cx-24,cy-20,48,40,5,white,255);round_rect(cx-18,cy-14,36,28,3,COLOR(28,46,60),255);draw_text(cx-12,cy-4,"OS",C_ACCENT,1);round_rect(cx-10,cy+20,20,5,3,white,255);}else{for(int r=0;r<3;r++)for(int c=0;c<3;c++)round_rect(cx-15+c*15,cy-15+r*15,10,10,4,white,255);}}
static int get_display_time(EFI_TIME*out);
static void draw_dock(void){round_rect(30,850,1860,185,50,C_DARK,glass_on?166:242);hline(90,855,1740,C_WHITE,65);draw_icon(105,940,7,C_ACCENT);vline(190,890,96,C_WHITE,100);const int centers[6]={620,760,900,1040,1180,1320};const int kinds[6]={1,2,3,4,5,6};const u32 bgs[6]={0x22c9b9,0xd74c75,0x7b57de,0xd5a540,0x5266d9,0x2c9c90};for(int i=0;i<6;i++){draw_icon(centers[i],940,kinds[i],bgs[i]);const char*labels[6]={"Files","Terminal","Notes","Calculator","Settings","Monitor"};draw_text_center(centers[i],985,labels[i],C_WHITE,1);}}
static void draw_menu(void){round_rect(0,0,1920,50,0,C_DARK,glass_on?185:255);fill_rect(0,47,1920,2,C_WHITE,80);draw_text(26,13,"mintOS26",C_WHITE,1);draw_text(210,15,"Desktop",C_WHITE,1);draw_text(365,15,"File",C_WHITE,1);draw_text(440,15,"Edit",C_WHITE,1);draw_text(510,15,"View",C_WHITE,1);draw_text(600,15,"Window",C_WHITE,1);draw_text(715,15,"Help",C_WHITE,1);char clk[64]="UEFI";EFI_TIME ti;if(get_display_time(&ti)){int h=ti.Hour;if(!clock_24h){h%=12;if(!h)h=12;}clk[0]=(char)('0'+h/10);clk[1]=(char)('0'+h%10);clk[2]=':';clk[3]=(char)('0'+ti.Minute/10);clk[4]=(char)('0'+ti.Minute%10);clk[5]=0;}else u64_to_dec(clk,ticks/6000);draw_text_fit(1770,15,clk,C_WHITE,1,100);}
static void draw_desktop_icons(void){draw_icon(1780,160,1,C_ACCENT);draw_text_center(1780,205,"Documents",C_WHITE,1);draw_icon(1780,285,3,0x4d63d9);draw_text_center(1780,330,"Notes",C_WHITE,1);draw_icon(1780,410,6,0x303f55);draw_text_center(1780,455,"System",C_WHITE,1);}
static int leap_year(int y){return (y%4==0&&y%100!=0)||y%400==0;}
static int days_in_month(int y,int m){static const int d[]={31,28,31,30,31,30,31,31,30,31,30,31};if(m==2&&leap_year(y))return 29;return m>=1&&m<=12?d[m-1]:30;}
static void adjust_minutes(EFI_TIME*t,int delta){while(delta>0){t->Minute++;if(t->Minute>=60){t->Minute=0;t->Hour++;}if(t->Hour>=24){t->Hour=0;t->Day++;}if(t->Day>days_in_month(t->Year,t->Month)){t->Day=1;t->Month++;}if(t->Month>12){t->Month=1;t->Year++;}delta--;}while(delta<0){t->Minute--;if((int)t->Minute<0){t->Minute=59;if(t->Hour==0){t->Hour=23;if(t->Day<=1){if(t->Month<=1){t->Month=12;t->Year--;}else t->Month-- ;t->Day=days_in_month(t->Year,t->Month);}else t->Day--;}else t->Hour--;}delta++;}}
static int get_display_time(EFI_TIME*out){EFI_RUNTIME_SERVICES*rs=(EFI_RUNTIME_SERVICES*)st->RuntimeServices;if(!rs||!rs->GetTime||EFI_ERROR(rs->GetTime(out,0)))return 0;int correction=0;if(timezone_id){if(out->TimeZone>=-1440&&out->TimeZone<=1440)correction=-out->TimeZone;else correction=0;if(timezone_id==2)correction+=60;else if(timezone_id==3)correction-=300;adjust_minutes(out,correction);}return 1;}
static const char*locale_name(void){return locale_id==1?"Czech":locale_id==2?"Deutsch":"English";}
static const char*zone_name(void){return timezone_id==1?"UTC":timezone_id==2?"Central Europe (UTC+1 fixed)":timezone_id==3?"US Eastern (UTC-5 fixed)":"Firmware local time";}
static int weekday_sun0(int y,int m,int d){static int t[]={0,3,2,5,0,3,5,1,4,6,2,4};if(m<3)y--;return (y+y/4-y/100+y/400+t[m-1]+d)%7;}
static void init_calendar(void){EFI_TIME t;if(get_display_time(&t)){calendar_year=t.Year;calendar_month=t.Month;}}
static void load_code(void){code_len=0;code_text[0]=0;if(data_root){UINTN n=0;if(!EFI_ERROR(read_file(data_root,"\\MINTOS26\\DEVELOP\\HELLO.C",code_text,sizeof(code_text)-1,&n))&&n){code_len=n;code_text[n]=0;return;}}const char*src="/* MintOS26 Code Studio - starter C file */\n#include <stdint.h>\n\nint main(void) {\n    return 0;\n}\n";copy_text(code_text,sizeof(code_text),src);code_len=strlen(code_text);}
static void save_code(void){if(!data_root){copy_text(installer_status,sizeof(installer_status),"No writable volume; source remains in memory.");return;}ensure_dir(data_root,"\\MINTOS26");ensure_dir(data_root,"\\MINTOS26\\DEVELOP");if(!EFI_ERROR(write_file(data_root,"\\MINTOS26\\DEVELOP\\HELLO.C",code_text,code_len)))copy_text(installer_status,sizeof(installer_status),"Saved DEVELOPMENT/HELLO.C on the data volume.");else copy_text(installer_status,sizeof(installer_status),"Could not save source; check the writable data volume.");}
static void init_network_diag(void){network_adapters=0;EFI_HANDLE*h=0;UINTN count=0;if(bs->LocateHandleBuffer&&!EFI_ERROR(bs->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL,&GUID_SIMPLE_NETWORK,0,&count,&h))&&h){network_adapters=(u32)count;bs->FreePool(h);}if(network_adapters){copy_text(network_label,sizeof(network_label),"Network adapter interface(s) exposed by UEFI.");}else copy_text(network_label,sizeof(network_label),"No UEFI network interface found. TCP/IP is not implemented yet.");}
static int stage_install_bundle(void){if(!data_root){copy_text(installer_status,sizeof(installer_status),"Attach a writable FAT data disk first.");return 0;}ensure_dir(data_root,"\\MINTOS26");ensure_dir(data_root,"\\MINTOS26\\INSTALL");u8*efi=0;UINTN n=0;if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,2*1024*1024,(void**)&efi))||!efi){copy_text(installer_status,sizeof(installer_status),"Could not allocate memory for the bundle.");return 0;}EFI_STATUS rs=read_file(boot_root,"\\EFI\\BOOT\\BOOTX64.EFI",efi,2*1024*1024,&n);if(EFI_ERROR(rs)||n<4096||EFI_ERROR(write_file(data_root,"\\MINTOS26\\INSTALL\\BOOTX64.EFI",efi,n))){bs->FreePool(efi);copy_text(installer_status,sizeof(installer_status),"Could not stage the EFI application.");return 0;}bs->FreePool(efi);const char*manifest="MintOS26 26.5.010.0 install-preparation bundle\r\nContains BOOTX64.EFI copied from the boot medium.\r\nThis is a staging/recovery bundle, not a complete installer: it does not partition disks, install an ESP, or create a UEFI boot entry.\r\n";if(EFI_ERROR(write_file(data_root,"\\MINTOS26\\INSTALL\\INSTALL.TXT",manifest,strlen(manifest)))){copy_text(installer_status,sizeof(installer_status),"EFI copied, but manifest write failed.");return 0;}copy_text(installer_status,sizeof(installer_status),"EFI recovery bundle staged in MINTOS26/INSTALL.");return 1;}
static const char*app_name(int app){if(locale_id==1){switch(app){case APP_LAUNCHER:return "Aplikace";case APP_FINDER:return "Soubory";case APP_TERMINAL:return "Terminal";case APP_NOTES:return "Poznamky";case APP_CALCULATOR:return "Kalkulacka";case APP_SETTINGS:return "Nastaveni";case APP_ABOUT:return "O MintOS26";case APP_MONITOR:return "Monitor systemu";case APP_CALENDAR:return "Kalendar";case APP_CLOCK:return "Hodiny";case APP_CODE:return "Code Studio";case APP_NETWORK:return "Sit";case APP_INSTALLER:return "Instalator";case APP_ABOUT_MINTDE:return "O MintDE";}}if(locale_id==2){switch(app){case APP_LAUNCHER:return "Programme";case APP_FINDER:return "Dateien";case APP_TERMINAL:return "Terminal";case APP_NOTES:return "Notizen";case APP_CALCULATOR:return "Rechner";case APP_SETTINGS:return "Einstellungen";case APP_ABOUT:return "Uber MintOS26";case APP_MONITOR:return "Systemmonitor";case APP_CALENDAR:return "Kalender";case APP_CLOCK:return "Uhr";case APP_CODE:return "Code Studio";case APP_NETWORK:return "Netzwerk";case APP_INSTALLER:return "Installer";case APP_ABOUT_MINTDE:return "Uber MintDE";}}switch(app){case APP_LAUNCHER:return "Applications";case APP_FINDER:return "Finder";case APP_TERMINAL:return "Terminal";case APP_NOTES:return "Notes";case APP_CALCULATOR:return "Calculator";case APP_SETTINGS:return "Settings";case APP_ABOUT:return "About MintOS26";case APP_MONITOR:return "System Monitor";case APP_CALENDAR:return "Calendar";case APP_CLOCK:return "Clock";case APP_CODE:return "Code Studio";case APP_NETWORK:return "Network";case APP_INSTALLER:return "Installer";case APP_ABOUT_MINTDE:return "About MintDE";default:return "Desktop";}}
static void draw_window_frame(void){round_rect(win_x+7,win_y+12,win_w,win_h,28,C_DARK,78);round_rect(win_x,win_y,win_w,win_h,28,0xeaf3f8,150);round_rect(win_x+1,win_y+1,win_w-2,win_h-2,27,C_WHITE,38);round_rect(win_x,win_y,win_w,60,28,COLOR(241,248,251),207);fill_rect(win_x,win_y+30,win_w,30,COLOR(241,248,251),207);round_rect(win_x+18,win_y+18,18,18,9,COLOR(255,99,102),255);round_rect(win_x+46,win_y+18,18,18,9,COLOR(255,189,64),255);round_rect(win_x+74,win_y+18,18,18,9,COLOR(43,201,121),255);draw_text(win_x+116,win_y+18,app_name(active_app),COLOR(35,55,69),1);hline(win_x+1,win_y+60,win_w-2,C_WHITE,160);hline(win_x+28,win_y+2,win_w-56,C_WHITE,105);}
static int activate_gop_mode(u32 mode);
static void settings_popup_geometry(int id,int *px,int *py,int *rows){
 int top=0,n=0;*px=win_x+win_w-504;
 if(id==3){top=335;n=3;}else if(id==4){top=385;n=(int)gop_mode_count;if(n>6)n=6;if(n<1)n=1;}
 else if(id==5){top=82;n=3;}else if(id==6){top=145;n=4;}else if(id==7){top=208;n=2;}
 *py=win_y+90+top+46;*rows=n;
 if(*py+n*38>win_y+win_h-12)*py=win_y+win_h-12-n*38;
}
static const char*settings_option_label(int id,int opt,char *buf,UINTN cap){
 if(id==3){static const char*p[]={"Classic arrow","Mint arrow","Precision crosshair"};return opt>=0&&opt<3?p[opt]:"Unknown";}
 if(id==5){static const char*p[]={"English","Czech","Deutsch"};return opt>=0&&opt<3?p[opt]:"Unknown";}
 if(id==6){static const char*p[]={"Firmware local time","UTC","Central Europe (UTC+1)","US Eastern (UTC-5)"};return opt>=0&&opt<4?p[opt]:"Unknown";}
 if(id==7){return opt==0?"24-hour clock":"12-hour clock";}
 if(id==4){if((UINTN)opt>=gop_mode_count)return "Current mode";char a[20],b[20];u64_to_dec(a,gop_mode_w[opt]);u64_to_dec(b,gop_mode_h[opt]);UINTN n=0;buf[0]=0;append_str(buf,cap,&n,a);append_str(buf,cap,&n," x ");append_str(buf,cap,&n,b);return buf;}
 return "";
}
static void draw_settings_popup(void){
 int px,py,rows;settings_popup_geometry(settings_dropdown,&px,&py,&rows);round_rect(px-4,py-4,428,rows*38+8,14,C_DARK,65);round_rect(px,py,420,rows*38,12,COLOR(247,251,253),250);
 for(int i=0;i<rows;i++){int ry=py+i*38;char tmp[48];const char*label=settings_option_label(settings_dropdown,i,tmp,sizeof(tmp));round_rect(px+5,ry+3,410,32,8,C_WHITE, i==((settings_dropdown==3?cursor_style:settings_dropdown==5?locale_id:settings_dropdown==6?timezone_id:settings_dropdown==7?(clock_24h?0:1):-1))?230:120);draw_text_fit(px+16,ry+10,label,COLOR(36,57,72),1,385);}
}
static int settings_popup_select(int bx,int by){
 if(!settings_dropdown)return 0;int px,py,rows;settings_popup_geometry(settings_dropdown,&px,&py,&rows);
 if(bx<px||bx>=px+420||by<py||by>=py+rows*38){settings_dropdown=0;return 0;}
 int opt=(by-py)/38;int id=settings_dropdown;settings_dropdown=0;
 if(id==3)cursor_style=opt;
 else if(id==4){if((UINTN)opt<gop_mode_count){if(!activate_gop_mode(gop_modes[opt]))set_msg("Could not activate selected display mode.");}}
 else if(id==5)locale_id=opt;else if(id==6)timezone_id=opt;else if(id==7)clock_24h=(opt==0);
 save_settings();desktop_base_dirty=1;return 1;
}
static void EFIAPI http_event_notify(EFI_EVENT ev,void *context){(void)ev;if(context)*(volatile int*)context=1;}
static void perform_http_get(void){
 copy_text(network_http_status,sizeof(network_http_status),"Looking for UEFI HTTP service...");network_http_body[0]=0;network_http_available=0;
 EFI_HANDLE*handles=0;UINTN count=0;EFI_STATUS rs=bs->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL,&GUID_HTTP_SERVICE_BINDING,0,&count,&handles);
 if(EFI_ERROR(rs)||!handles||!count){if(handles)bs->FreePool(handles);copy_text(network_http_status,sizeof(network_http_status),"No UEFI HTTP service binding. Firmware TCP/IP/DNS is unavailable.");return;}
 EFI_STATUS final=EFI_UNSUPPORTED;
 for(UINTN i=0;i<count;i++){
  EFI_SERVICE_BINDING_PROTOCOL*sb=0;if(EFI_ERROR(bs->HandleProtocol(handles[i],&GUID_HTTP_SERVICE_BINDING,(void**)&sb))||!sb||!sb->CreateChild||!sb->DestroyChild)continue;
  EFI_HANDLE child=0;rs=sb->CreateChild(sb,&child);if(EFI_ERROR(rs)||!child)continue;
  EFI_HTTP_PROTOCOL*http=0;rs=bs->HandleProtocol(child,&GUID_HTTP,(void**)&http);if(EFI_ERROR(rs)||!http){sb->DestroyChild(sb,child);continue;}
  EFI_HTTPV4_ACCESS_POINT ap;memset(&ap,0,sizeof(ap));ap.UseDefaultAddress=1;
  EFI_HTTP_CONFIG_DATA cfg;memset(&cfg,0,sizeof(cfg));cfg.HttpVersion=1;cfg.TimeOutMillisec=9000;cfg.LocalAddressIsIPv6=0;cfg.AccessPoint.IPv4Node=&ap;
  rs=http->Configure(http,&cfg);if(EFI_ERROR(rs)){sb->DestroyChild(sb,child);final=rs;continue;}
  volatile int req_done=0,resp_done=0;EFI_EVENT req_event=0,resp_event=0;
  rs=bs->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,http_event_notify,(void*)&req_done,&req_event);if(EFI_ERROR(rs)){http->Configure(http,0);sb->DestroyChild(sb,child);final=rs;continue;}
  rs=bs->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,http_event_notify,(void*)&resp_done,&resp_event);if(EFI_ERROR(rs)){bs->CloseEvent(req_event);http->Configure(http,0);sb->DestroyChild(sb,child);final=rs;continue;}
  CHAR16 url[]={ 'h','t','t','p',':','/','/','n','e','v','e','r','s','s','l','.','c','o','m','/',0 };
  EFI_HTTP_REQUEST_DATA rd;rd.Method=0;rd.Url=url;EFI_HTTP_HEADER headers[3]={{"Host","neverssl.com"},{"Connection","close"},{"User-Agent","MintOS26/26.5.010.0"}};
  EFI_HTTP_MESSAGE rq;memset(&rq,0,sizeof(rq));rq.Data.Request=&rd;rq.HeaderCount=3;rq.Headers=headers;
  EFI_HTTP_TOKEN rqt={req_event,EFI_NOT_READY,&rq};
  EFI_HTTP_RESPONSE_DATA rdata;memset(&rdata,0,sizeof(rdata));u8 body[900];memset(body,0,sizeof(body));EFI_HTTP_MESSAGE rp;memset(&rp,0,sizeof(rp));rp.Data.Response=&rdata;rp.Body=body;rp.BodyLength=sizeof(body)-1;
  EFI_HTTP_TOKEN rpt={resp_event,EFI_NOT_READY,&rp};
  EFI_STATUS q1=http->Response(http,&rpt);EFI_STATUS q2=http->Request(http,&rqt);final=EFI_ERROR(q1)?q1:q2;
  if(!EFI_ERROR(q1)&&!EFI_ERROR(q2)){
   for(int spin=0;spin<1500&&(!req_done||!resp_done);spin++){EFI_STATUS ps=http->Poll(http);if(EFI_ERROR(ps)&&ps!=EFI_NOT_READY){}bs->Stall(10000);}
   if(resp_done&&!EFI_ERROR(rpt.Status)){
    network_http_available=1;char code[24];u64_to_dec(code,rdata.StatusCode);copy_text(network_http_status,sizeof(network_http_status),"HTTP response status: ");
    /* Assemble status with bounded helpers rather than relying on a heap. */
    UINTN nn=strlen(network_http_status);append_str(network_http_status,sizeof(network_http_status),&nn,code);append_str(network_http_status,sizeof(network_http_status),&nn," (plain HTTP)");
    UINTN body_n=rp.BodyLength;if(body_n>=sizeof(network_http_body))body_n=sizeof(network_http_body)-1;for(UINTN j=0;j<body_n;j++){u8 c=body[j];network_http_body[j]=(c>=32&&c<127)?(char)c:(c=='\n'||c=='\r'?' ':' ');}network_http_body[body_n]=0;
    if(!body_n)copy_text(network_http_body,sizeof(network_http_body),"Response received; the server returned an empty body.");
   }else copy_text(network_http_status,sizeof(network_http_status),"HTTP request timed out or failed; check the VM network adapter.");
  }else copy_text(network_http_status,sizeof(network_http_status),"Firmware rejected the HTTP request before sending.");
  if(!req_done&&http->Cancel)http->Cancel(http,&rqt);if(!resp_done&&http->Cancel)http->Cancel(http,&rpt);
  http->Configure(http,0);bs->CloseEvent(req_event);bs->CloseEvent(resp_event);sb->DestroyChild(sb,child);
  if(network_http_available)break;
 }
 bs->FreePool(handles);
 if(!network_http_available&&final!=EFI_SUCCESS) { /* the status above remains the more specific result when set */ }
}
static void draw_app_content(void){int x=win_x+36,y=win_y+90;const u32 ink=COLOR(35,54,68),muted=COLOR(74,96,109),soft=COLOR(233,242,247);if(active_app==APP_LAUNCHER){draw_text(x,y,"More and more Mint.",ink,2);draw_text(x,y+36,"Your apps, tools, settings and experiments.",muted,1);int apps[13]={APP_FINDER,APP_TERMINAL,APP_NOTES,APP_CALCULATOR,APP_SETTINGS,APP_MONITOR,APP_CALENDAR,APP_CLOCK,APP_CODE,APP_NETWORK,APP_INSTALLER,APP_ABOUT,APP_ABOUT_MINTDE};int kinds[13]={1,2,3,4,5,6,8,9,10,11,12,7,13};u32 bg[13]={0x21c7b5,0xd74c75,0x7d5bda,0xd5a540,0x546bd8,0x2c9b91,0xe66c78,0x3384b7,0x273c55,0x1f9e9b,0x65748c,0x5b8b71,0x22bfa7};const char*desc[13]={"Browse and manage files","Commands and quick tools","Write and save notes","Everyday arithmetic","Appearance and region","Input, display and storage","Month-at-a-glance","Time and timezone","Edit C source files","Adapter diagnostics","Prepare a recovery bundle","OS version and capabilities","MintDE 1.0 information"};for(int i=0;i<13;i++){int col=i%4,row=i/4,tx=x+col*305,ty=y+82+row*175;round_rect(tx,ty,290,148,18,C_WHITE,200);round_rect(tx,ty,290,148,18,C_BORDER,85);draw_icon(tx+54,ty+55,kinds[i],bg[i]);draw_text_fit(tx+103,ty+37,app_name(apps[i]),ink,1,175);draw_text_fit(tx+103,ty+69,desc[i],muted,1,175);draw_text_fit(tx+103,ty+104,"Open app",C_ACCENT,1,175);}}
 else if(active_app==APP_FINDER){
  draw_text_fit(x,y,"Finder",ink,2,800);draw_text_fit(x,y+42,finder_path,muted,1,1000);
  round_rect(x,y+70,104,42,12,C_ACCENT,245);draw_text(x+27,y+82,"Up",C_WHITE,1);
  round_rect(x+114,y+70,172,42,12,soft,255);draw_text_fit(x+130,y+82,"New folder",ink,1,145);
  round_rect(x+296,y+70,156,42,12,soft,255);draw_text_fit(x+311,y+82,"New file",ink,1,130);
  round_rect(x+462,y+70,128,42,12,soft,255);draw_text_fit(x+476,y+82,"Delete",ink,1,100);
  round_rect(x+600,y+70,120,42,12,C_ACCENT,245);draw_text(x+622,y+82,"Open",C_WHITE,1);round_rect(x+738,y+70,110,42,12,soft,255);draw_text_fit(x+750,y+82,"Previous",ink,1,84);round_rect(x+856,y+70,100,42,12,soft,255);draw_text_fit(x+870,y+82,"Next",ink,1,75);
  round_rect(x,y+122,224,436,16,COLOR(246,250,252),225);draw_text(x+14,y+136,"Locations",ink,1);
  for(UINTN vi=0;vi<fs_volume_count&&vi<9;vi++){int vy=y+168+(int)vi*40;round_rect(x+8,vy,208,34,10,vi==finder_volume?C_ACCENT:soft,vi==finder_volume?245:225);draw_text_fit(x+20,vy+10,fs_volumes[vi].label,vi==finder_volume?C_WHITE:ink,1,182);}
  round_rect(x+238,y+122,win_w-326,436,16,C_WHITE,220);draw_text(x+260,y+136,"Name",muted,1);draw_text(x+930,y+136,"Size",muted,1);hline(x+252,y+162,win_w-354,C_BORDER,255);
  for(UINTN fi=0;fi<10&&finder_scroll+fi<finder_entry_count;fi++){UINTN ei=finder_scroll+fi;int fy=y+174+(int)fi*36;round_rect(x+250,fy,win_w-350,32,8,(int)ei==finder_selected?C_ACCENT:soft,(int)ei==finder_selected?220:165);draw_text_fit(x+265,fy+8,finder_entries[ei].name, (int)ei==finder_selected?C_WHITE:ink,1,650);char sizebuf[32];if(finder_entries[ei].is_dir)copy_text(sizebuf,sizeof(sizebuf),"Folder");else{u64_to_dec(sizebuf,finder_entries[ei].size);UINTN zn=strlen(sizebuf);if(zn+3<sizeof(sizebuf)){sizebuf[zn++]=' ';sizebuf[zn++]='B';sizebuf[zn]=0;}}draw_text_fit(x+930,fy+8,sizebuf,(int)ei==finder_selected?C_WHITE:muted,1,140);}char pageinfo[64];u64_to_dec(pageinfo,finder_scroll+1);UINTN pp=strlen(pageinfo);append_str(pageinfo,sizeof(pageinfo),&pp,"-");char pagelast[16];u64_to_dec(pagelast,finder_scroll+(finder_entry_count-finder_scroll<10?finder_entry_count-finder_scroll:10));append_str(pageinfo,sizeof(pageinfo),&pp,pagelast);append_str(pageinfo,sizeof(pageinfo),&pp," / ");u64_to_dec(pagelast,finder_entry_count);append_str(pageinfo,sizeof(pageinfo),&pp,pagelast);draw_text_fit(x+1030,y+142,pageinfo,muted,1,180);
  round_rect(x,y+574,win_w-100,78,14,COLOR(243,248,251),220);draw_text_fit(x+14,y+586,finder_preview_open?finder_preview_name:finder_status,ink,1,win_w-150);
  if(finder_preview_open){char line[120];UINTN n=0;int py=y+616;for(UINTN j=0;finder_preview[j]&&py<y+645;j++){char c=finder_preview[j];if(c=='\r')continue;if(c=='\n'){line[n]=0;draw_text_fit(x+14,py,line,muted,1,win_w-150);py+=24;n=0;}else if(n+1<sizeof(line))line[n++]=c;}}
  char volinfo[128]="Volume information unavailable";if(finder_volume<fs_volume_count){char cap[32];u64_to_dec(cap,fs_volumes[finder_volume].total_bytes/(1024*1024));copy_text(volinfo,sizeof(volinfo),fs_volumes[finder_volume].label);UINTN n=strlen(volinfo);append_str(volinfo,sizeof(volinfo),&n,"  ");append_str(volinfo,sizeof(volinfo),&n,cap);append_str(volinfo,sizeof(volinfo),&n," MiB");}draw_text_fit(x,y+660,volinfo,muted,1,win_w-100);
 }
 else if(active_app==APP_TERMINAL){round_rect(x-10,y-10,win_w-52,win_h-135,15,COLOR(12,25,37),255);draw_text(x,y,"MintOS26 Terminal  •  UEFI x86_64",C_ACCENT,1);draw_text(x,y+40,terminal_msg,COLOR(221,236,244),1);draw_text(x,y+100,"guest@mintos26  ~  $",C_ACCENT,1);draw_text(x,y+142,cmdline,COLOR(255,255,255),1);fill_rect(x+(int)cmd_len*17,y+142,8,20,C_ACCENT,255);draw_text(x,y+198,"HELP lists commands  ·  Esc closes this window",COLOR(173,193,205),1);}
 else if(active_app==APP_NOTES){draw_text(x,y,"Quick Notes",C_ACCENT,2);draw_text(x,y+44,"Saved to the attached writable volume when available.",muted,1);round_rect(x-10,y+78,win_w-52,win_h-190,14,C_WHITE,220);int cy=y+95;char line[160];int n=0;for(UINTN i=0;i<=note_len&&cy<win_y+win_h-100;i++){if(note_text[i]=='\n'||note_text[i]==0){line[n]=0;draw_text(x,cy,line,ink,1);cy+=28;n=0;}else if(n<150)line[n++]=note_text[i];}round_rect(x,y+win_h-180,154,44,12,C_ACCENT,240);draw_text(x+25,y+win_h-166,"Save Notes",C_WHITE,1);draw_text(x+170,y+win_h-166,"Ctrl+S saves • Esc closes",muted,1);}
 else if(active_app==APP_CALCULATOR){draw_text(x,y,"A little math, beautifully simple.",muted,1);round_rect(x,y+55,win_w-80,125,18,COLOR(229,239,243),245);draw_text(x+25,y+83,calculator_expr[0]?calculator_expr:"12 + 30",muted,1);draw_text(x+25,y+123,calc_result[0]?calc_result:"Use Terminal: CALC 12 + 30",ink,2);draw_text(x,y+220,"Try CALC <number> <operator> <number> in Terminal.",muted,1);}
 else if(active_app==APP_SETTINGS){
  draw_text(x,y,"Preferences",ink,2);round_rect(x,y+38,210,32,12,settings_page==0?C_ACCENT:soft,255);draw_text_fit(x+18,y+47,"Appearance",settings_page==0?C_WHITE:ink,1,180);round_rect(x+220,y+38,250,32,12,settings_page==1?C_ACCENT:soft,255);draw_text_fit(x+242,y+47,"Language & Time",settings_page==1?C_WHITE:ink,1,210);
  if(settings_page==0){const char*labels[3]={"Warm alpine","Mint clarity","Apricot glow"};for(int i=0;i<3;i++){int yy=y+85+i*48;round_rect(x,yy,win_w-100,40,12,i==theme?C_ACCENT:soft,245);draw_text(x+18,yy+11,labels[i],i==theme?C_WHITE:ink,1);round_rect(x+win_w-145,yy+9,22,22,11,themes[i][0],255);}
   round_rect(x,y+235,win_w-100,42,12,soft,245);draw_text_fit(x+16,y+247,"Startup chime (PC speaker only)",ink,1,650);round_rect(x+win_w-440,y+239,420,34,10,C_WHITE,230);draw_text(x+win_w-425,y+247,sound_on?"On":"Off",C_ACCENT,1);
   round_rect(x,y+285,win_w-100,42,12,soft,245);draw_text_fit(x+16,y+297,"Liquid-glass panels and dock blur",ink,1,650);round_rect(x+win_w-440,y+289,420,34,10,C_WHITE,230);draw_text(x+win_w-425,y+297,glass_on?"On":"Off",C_ACCENT,1);
   draw_dropdown_row(x,y+335,win_w-100,"Pointer style",cursor_style==0?"Classic arrow":cursor_style==1?"Mint arrow":"Precision crosshair",ink,muted,soft);
   char disp[80]="Choose a GOP mode";if(gop&&gop->Mode){u64_to_dec(disp,gop->Mode->Info->HorizontalResolution);UINTN n=strlen(disp);append_str(disp,sizeof(disp),&n," x ");char hh[20];u64_to_dec(hh,gop->Mode->Info->VerticalResolution);append_str(disp,sizeof(disp),&n,hh);}draw_dropdown_row(x,y+385,win_w-100,"Display mode",disp,ink,muted,soft);
   draw_text_fit(x,y+455,storage_label,muted,1,win_w-100);draw_text_fit(x,y+493,audio_label,muted,1,win_w-100);draw_text_fit(x,y+531,"Settings save to the writable data volume.",muted,1,win_w-100);
  }else{
   draw_dropdown_row(x,y+82,win_w-100,"Interface language",locale_name(),ink,muted,soft);
   draw_dropdown_row(x,y+145,win_w-100,"Timezone preset",zone_name(),ink,muted,soft);
   draw_dropdown_row(x,y+208,win_w-100,"Clock format",clock_24h?"24-hour":"12-hour",ink,muted,soft);
   draw_text_fit(x,y+285,"Language selection localizes app names and titles.",muted,1,win_w-100);draw_text_fit(x,y+320,"Timezone presets are display choices; firmware clock is not changed.",muted,1,win_w-100);draw_text_fit(x,y+355,"Fixed UTC offsets do not automatically apply daylight saving time.",muted,1,win_w-100);draw_text_fit(x,y+415,storage_label,muted,1,win_w-100);draw_text_fit(x,y+455,network_label,muted,1,win_w-100);
  }
 }
 else if(active_app==APP_ABOUT){
  draw_icon(x+66,y+66,12,C_ACCENT);
  draw_text(x+130,y+2,"MintOS26",C_ACCENT,3);
  draw_text(x+130,y+78,"Version 26.5.010.0  •  x86-64",ink,1);
  draw_text(x,y+128,"Operating system",ink,2);
  round_rect(x-8,y+172,win_w-84,58,14,soft,245);
  draw_text(x+14,y+188,"Boot environment",muted,1);draw_text(x+250,y+188,"UEFI Desktop (safe default)",ink,1);
  round_rect(x-8,y+242,win_w-84,58,14,soft,245);
  draw_text(x+14,y+258,"Native kernel",muted,1);draw_text(x+250,y+258,"MintOS26 x86-64 / opt-in handoff",ink,1);
  round_rect(x-8,y+312,win_w-84,58,14,soft,245);
  draw_text(x+14,y+328,"Recovery",muted,1);draw_text(x+250,y+328,"Choose UEFI Desktop at startup or reset VM",ink,1);
  draw_text_fit(x,y+406,"Firmware services power this desktop; kernel-only mode exits UEFI boot services.",muted,1,win_w-100);
  draw_text_fit(x,y+448,"Notes and settings in MintDE remain session-only until native filesystem writes are implemented.",muted,1,win_w-100);
 }
 else if(active_app==APP_ABOUT_MINTDE){
  draw_icon(x+66,y+66,13,C_ACCENT);
  draw_text(x+130,y+2,"MintDE",C_ACCENT,3);
  draw_text(x+130,y+78,"Version 2.0.0  •  Native desktop environment",ink,1);
  draw_text(x,y+128,"Desktop environment",ink,2);
  round_rect(x-8,y+172,win_w-84,58,14,soft,245);
  draw_text(x+14,y+188,"Architecture",muted,1);draw_text(x+250,y+188,"Ring-3 ELF64 user-space process",ink,1);
  round_rect(x-8,y+242,win_w-84,58,14,soft,245);
  draw_text(x+14,y+258,"Interface",muted,1);draw_text(x+250,y+258,"Menu bar, draggable windows, software pointer and dock",ink,1);
  round_rect(x-8,y+312,win_w-84,58,14,soft,245);
  draw_text(x+14,y+328,"Core apps",muted,1);draw_text(x+250,y+328,"Files, Notes, Settings, System Monitor, storage tools",ink,1);
  draw_text_fit(x,y+406,"MintDE is versioned separately from the MintOS26 operating system.",muted,1,win_w-100);
  draw_text_fit(x,y+448,"MintDE 2.0.0 improves the native desktop; direct kernel boot remains experimental.",muted,1,win_w-100);
 }
 else if(active_app==APP_MONITOR){char a[32],b[32],c[32];u64_to_dec(a,screen_w);u64_to_dec(b,screen_h);u64_to_dec(c,ticks/100);draw_text(x,y,"System overview",ink,2);draw_text(x,y+65,"Boot: UEFI x86_64",ink,1);draw_text(x,y+105,"Display: ",ink,1);draw_text(x+140,y+105,a,C_ACCENT,1);draw_text(x+190,y+105,"x",ink,1);draw_text(x+215,y+105,b,C_ACCENT,1);draw_text(x,y+145,"Framebuffer: GOP + RAM backbuffer",ink,1);draw_text(x,y+185,"Input loop: UEFI events + timer + HID/PS2 fallback",ink,1);draw_text(x,y+225,"Uptime (approx):",ink,1);draw_text(x+250,y+225,c,C_ACCENT,1);draw_text(x,y+280,storage_label,muted,1);draw_text(x,y+325,pointer_label,muted,1);draw_text(x,y+365,pointer_detail,muted,1);draw_text_fit(x,y+405,event_detail,muted,1,win_w-80);draw_text(x,y+445,fallback_detail,ink,1);draw_text(x,y+485,audio_label,muted,1);draw_text(x,y+535,"Internet stack, native display driver and PCM audio remain future work.",muted,1);}
 else if(active_app==APP_CALENDAR){static const char*months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};char yr[16];u64_to_dec(yr,(u64)calendar_year);draw_text(x,y,"Your month",muted,1);round_rect(x,y+34,52,38,12,soft,255);draw_text(x+19,y+43,"<",ink,1);round_rect(x+win_w-150,y+34,52,38,12,soft,255);draw_text(x+win_w-132,y+43,">",ink,1);draw_text_center(x+win_w/2,y+40,months[(calendar_month-1+12)%12],ink,2);draw_text_center(x+win_w/2+170,y+44,yr,muted,1);const char*wd[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};int cw=(win_w-100)/7;for(int c=0;c<7;c++)draw_text(x+14+c*cw,y+91,wd[c],C_ACCENT,1);int first=weekday_sun0(calendar_year,calendar_month,1),days=days_in_month(calendar_year,calendar_month);EFI_TIME now;int has=get_display_time(&now);for(int d=1;d<=days;d++){int n=first+d-1,col=n%7,row=n/7;int xx=x+col*cw,yy=y+119+row*66;int today=has&&now.Year==calendar_year&&now.Month==calendar_month&&now.Day==d;round_rect(xx,yy,cw-10,54,10,today?C_ACCENT:soft,today?240:210);char num[8];u64_to_dec(num,d);draw_text_center(xx+(cw-10)/2,yy+15,num,today?C_WHITE:ink,1);}draw_text(x,y+530,"Use arrows to change month. Calendar uses the firmware clock.",muted,1);}
 else if(active_app==APP_CLOCK){EFI_TIME t;if(get_display_time(&t)){char h[16],m[16],sec[16],day[16],year[16];int hh=t.Hour;if(!clock_24h){hh%=12;if(!hh)hh=12;}h[0]=(char)('0'+hh/10);h[1]=(char)('0'+hh%10);h[2]=0;m[0]=(char)('0'+t.Minute/10);m[1]=(char)('0'+t.Minute%10);m[2]=0;sec[0]=(char)('0'+t.Second/10);sec[1]=(char)('0'+t.Second%10);sec[2]=0;u64_to_dec(day,t.Day);u64_to_dec(year,t.Year);draw_text(x,y,"The time, clearly.",muted,1);draw_text(x+70,y+120,h,C_ACCENT,4);draw_text(x+235,y+120,":",ink,4);draw_text(x+280,y+120,m,C_ACCENT,4);draw_text(x+420,y+144,sec,muted,2);draw_text(x+70,y+220,"Date:",ink,1);draw_text(x+160,y+220,day,ink,1);draw_text(x+195,y+220,"/",muted,1);char mon[8];u64_to_dec(mon,t.Month);draw_text(x+225,y+220,mon,ink,1);draw_text(x+260,y+220,"/",muted,1);draw_text(x+290,y+220,year,ink,1);round_rect(x+70,y+275,win_w-200,60,14,soft,255);draw_text(x+92,y+295,zone_name(),C_ACCENT,1);draw_text(x+70,y+370,"Change timezone and 12/24-hour format in Settings.",muted,1);}else draw_text(x,y,"Firmware clock unavailable.",ink,1);}
 else if(active_app==APP_CODE){draw_text(x,y,"Code Studio",C_ACCENT,2);draw_text(x,y+38,"A small source editor for experiments. It does not compile code yet.",muted,1);round_rect(x-6,y+70,win_w-52,win_h-210,14,COLOR(14,29,43),255);int liney=y+88;char line[180];int ln=0;for(UINTN i=0;i<=code_len&&liney<win_y+win_h-130;i++){if(code_text[i]=='\n'||code_text[i]==0){line[ln]=0;char nbuf[8];u64_to_dec(nbuf,(u64)((liney-(y+88))/25+1));draw_text(x+5,liney,nbuf,COLOR(91,179,185),1);draw_text(x+48,liney,line,COLOR(225,238,243),1);liney+=25;ln=0;}else if(ln+1<sizeof(line))line[ln++]=code_text[i];}round_rect(x,y+win_h-124,160,42,12,C_ACCENT,245);draw_text(x+24,y+win_h-112,"Save source",C_WHITE,1);draw_text(x+178,y+win_h-112,"Ctrl+S   •   Esc closes",muted,1);draw_text(x,y+win_h-70,installer_status,muted,1);}
 else if(active_app==APP_NETWORK){draw_text(x,y,"Network connections",ink,2);round_rect(x,y+60,win_w-100,100,18,soft,245);draw_icon(x+55,y+110,11,C_ACCENT);draw_text_fit(x+112,y+85,network_label,ink,1,win_w-250);char n[32];u64_to_dec(n,network_adapters);draw_text(x+112,y+120,"UEFI Simple Network interfaces found:",muted,1);draw_text(x+520,y+120,n,C_ACCENT,1);round_rect(x,y+190,win_w-100,90,16,COLOR(242,248,250),240);draw_text_fit(x+20,y+210,network_http_status,ink,1,win_w-140);draw_text_fit(x+20,y+242,"Try HTTP GET uses UEFI network/TCP/IP services when available.",muted,1,win_w-140);round_rect(x,y+300,280,52,14,C_ACCENT,245);draw_text(x+35,y+317,"Try HTTP GET",C_WHITE,1);draw_text_fit(x,y+370,network_http_body,muted,1,win_w-100);draw_text_fit(x,y+480,"HTTPS, a browser engine, and an independent TCP/IP stack are not included yet.",muted,1,win_w-100);}
 else if(active_app==APP_INSTALLER){draw_text(x,y,"Install Assistant",ink,2);draw_text(x,y+42,"Prepare a recovery bundle on the attached writable data volume.",muted,1);round_rect(x,y+86,win_w-100,170,18,soft,245);draw_text(x+22,y+108,"This does not repartition disks or modify firmware boot entries.",ink,1);draw_text(x+22,y+148,"It copies the EFI app and a manifest into MINTOS26/INSTALL.",muted,1);draw_text(x+22,y+188,"A full installer needs partitioning, ESP setup and boot management.",muted,1);round_rect(x,y+290,300,58,16,C_ACCENT,245);draw_text(x+32,y+309,"Prepare recovery bundle",C_WHITE,1);draw_text(x,y+374,installer_status,ink,1);draw_text(x,y+430,"Keep a separate copy of your VM disk before experimenting.",muted,1);}}
static void draw_app_window(void){if(active_app==APP_NONE)return;draw_window_frame();draw_app_content();}
static void render(void){size_t bytes=(size_t)screen_w*screen_h*4;if(desktop_base_dirty||!desktop_base){glass_cache_valid=0;draw_wallpaper();if(glass_on)blur_dock();if(desktop_base)memcpy(desktop_base,backbuffer,bytes);desktop_base_dirty=0;}else memcpy(backbuffer,desktop_base,bytes);draw_menu();draw_desktop_icons();draw_dock();prepare_window_glass();draw_app_window();if(active_app==APP_SETTINGS&&settings_dropdown)draw_settings_popup();if(launcher_open&&active_app!=APP_LAUNCHER){int old=active_app;active_app=APP_LAUNCHER;draw_app_window();active_app=old;}draw_cursor();present();}
static void render_splash(void){for(u32 y=0;y<screen_h;y++){u32 t=(u64)y*100/screen_h;for(u32 x=0;x<screen_w;x++){u32 glow=(x>screen_w/3&&x<screen_w*2/3)?(u32)(24-(x>screen_w/2?x-screen_w/2:screen_w/2-x)*24/(screen_w/6+1)):0;backbuffer[(u64)y*screen_w+x]=COLOR(10+t/5+glow/3,24+t/4+glow,37+t/4+glow);}}draw_text(650,438,"mintOS",C_WHITE,3);draw_text(1012,445,"26",C_ACCENT,3);draw_text_center(960,555,"UEFI DESKTOP   /   26.5.010.0",COLOR(182,208,218),1);round_rect(760,640,400,8,4,C_WHITE,35);round_rect(760,640,245,8,4,C_ACCENT,255);present();}
static void render_boot_selector(int seconds_left){
 render_splash();
 round_rect(330,220,1260,620,32,C_DARK,238);
 draw_text(500,270,"Choose your MintOS26 session",C_WHITE,2);
 draw_text(500,316,"MintOS26 26.5.010.0  /  MintDE 2.0.0",COLOR(182,208,218),1);
 round_rect(420,382,500,190,22,COLOR(40,62,78),255);
 round_rect(1000,382,500,190,22,COLOR(40,62,78),255);
 round_rect(448,408,444,136,18,COLOR(236,246,250),245);
 round_rect(1028,408,444,136,18,COLOR(28,53,69),245);
 draw_text(485,430,"1  UEFI Desktop",COLOR(35,54,68),2);
 draw_text_fit(485,476,"Recommended / normal boot and recovery environment",COLOR(74,96,109),1,385);
 draw_text(1065,430,"2  MintDE 2.0.0",C_WHITE,2);
 draw_text_fit(1065,476,"Native kernel / experimental one-way handoff",COLOR(182,208,218),1,380);
 draw_text_fit(500,624,"Press 1 or Enter for UEFI Desktop, or 2 / M for MintDE.",C_WHITE,1,920);
 char countdown[96]="Auto-starting UEFI Desktop in ";UINTN used=(UINTN)strlen(countdown);u64_to_dec(countdown+used,(u64)seconds_left);used=(UINTN)strlen(countdown);append_str(countdown,sizeof(countdown),&used," seconds");
 draw_text_fit(500,666,countdown,C_ACCENT,1,760);
 draw_text_fit(500,710,"MintDE exits UEFI boot services. If it fails, restart and let this timer choose UEFI Desktop.",COLOR(182,208,218),1,920);
 present();
}
static int wait_for_boot_choice(void){
 for(int seconds=3;seconds>0;--seconds){
  render_boot_selector(seconds);
  for(int tick=0;tick<100;tick++){
   EFI_INPUT_KEY key;
   if(conin&&conin->ReadKeyStroke&&!EFI_ERROR(conin->ReadKeyStroke(conin,&key))){
    CHAR16 c=key.UnicodeChar;
    if(c=='2'||c=='m'||c=='M')return 1;
    if(c=='1'||c=='u'||c=='U'||c==13||key.ScanCode==0x17)return 0;
   }
   if(bs&&bs->Stall)bs->Stall(10000);
  }
 }
 return 0;
}


static void execute_calc(char *s){long a=0,b=0;char op=0;while(*s==' ')s++;while(*s>='0'&&*s<='9')a=a*10+(*s++-'0');while(*s==' ')s++;op=*s++;while(*s==' ')s++;while(*s>='0'&&*s<='9')b=b*10+(*s++-'0');long r=0;if(op=='+')r=a+b;else if(op=='-')r=a-b;else if(op=='*')r=a*b;else if(op=='/'&&b)r=a/b;else{copy_text(calc_result,sizeof(calc_result),"Invalid expression (try 24 * 7)");return;}char num[48];int neg=r<0;if(neg)r=-r;u64_to_dec(num,(u64)r);if(neg){calc_result[0]='-';memcpy(calc_result+1,num,strlen(num)+1);}else memcpy(calc_result,num,strlen(num)+1);}
static void terminal_execute_enhanced(void){cmdline[cmd_len]=0;if(str_starts(cmdline,"CALC ")){char e[96];copy_text(e,sizeof(e),cmdline+5);start_app(APP_CALCULATOR);copy_text(calculator_expr,sizeof(calculator_expr),e);execute_calc(e);cmd_len=0;cmdline[0]=0;return;}if(str_eq(cmdline,"APPS")){launcher_open=1;active_app=APP_NONE;win_x=300;win_y=70;win_w=1320;win_h=930;cmd_len=0;cmdline[0]=0;return;}terminal_exec();}
static void save_notes_and_status(void){save_notes();set_msg(data_root?"Notes saved to disk":"No writable data disk; note remains in memory");}
static void cycle_graphics_mode(void);
static void mouse_click(int x,int y){int bx=base_x(x),by=base_y(y);
 if(launcher_open||active_app==APP_LAUNCHER){int lx=bx-win_x-36,ly=by-win_y-90;int col=lx/305,row=(ly-82)/175;int apps[13]={APP_FINDER,APP_TERMINAL,APP_NOTES,APP_CALCULATOR,APP_SETTINGS,APP_MONITOR,APP_CALENDAR,APP_CLOCK,APP_CODE,APP_NETWORK,APP_INSTALLER,APP_ABOUT,APP_ABOUT_MINTDE};if(lx>=0&&col>=0&&col<4&&row>=0&&row<4&&row*4+col<13&&(lx-col*305)<290&&(ly>=82+row*175)&&(ly-(82+row*175))<148){start_app(apps[row*4+col]);return;}if(bx<win_x||bx>win_x+win_w||by<win_y||by>win_y+win_h){launcher_open=0;if(active_app==APP_LAUNCHER)active_app=APP_NONE;return;}if(by<win_y+60){launcher_open=0;if(active_app==APP_LAUNCHER)active_app=APP_NONE;}return;}
 if(active_app!=APP_NONE){if(bx>=win_x+win_w-52&&bx<=win_x+win_w-14&&by>=win_y+10&&by<=win_y+50){close_app();return;}if(bx>=win_x+16&&bx<=win_x+102&&by>=win_y+14&&by<=win_y+43){close_app();return;}if(bx>=win_x&&bx<=win_x+win_w&&by>=win_y&&by<=win_y+60){dragging=1;return;}
  int lx=bx-win_x-36,ly=by-win_y-90;
  if(active_app==APP_FINDER){
   if(ly>=70&&ly<116){if(lx<108){finder_parent();return;}if(lx>=114&&lx<286){finder_create_item(1);return;}if(lx>=296&&lx<452){finder_create_item(0);return;}if(lx>=462&&lx<590){finder_delete_selected();return;}if(lx>=600&&lx<720){finder_open_selected();return;}if(lx>=738&&lx<848){finder_scroll=finder_scroll>=10?finder_scroll-10:0;finder_selected=-1;finder_preview_open=0;return;}if(lx>=856&&lx<956){if(finder_scroll+10<finder_entry_count)finder_scroll+=10;finder_selected=-1;finder_preview_open=0;return;}}
   if(lx>=0&&lx<224&&ly>=168&&ly<528){int vi=(ly-168)/40;if(vi>=0&&(UINTN)vi<fs_volume_count){finder_volume=(UINTN)vi;finder_path[0]=0;copy_text(finder_path,sizeof(finder_path),"\\");finder_refresh();return;}}
   if(lx>=250&&ly>=174&&ly<534){int idx=(ly-174)/36;UINTN actual=finder_scroll+(UINTN)(idx<0?0:idx);if(idx>=0&&idx<10&&actual<finder_entry_count){finder_selected=(int)actual;if(finder_last_click_index==idx&&ticks-finder_last_click_tick<45)finder_open_selected();finder_last_click_index=idx;finder_last_click_tick=ticks;return;}}
   return;
  }
  if(active_app==APP_SETTINGS&&settings_dropdown){if(settings_popup_select(bx,by))return;settings_dropdown=0;}
  if(active_app==APP_SETTINGS){if(ly>=38&&ly<70){settings_page=(lx>=220)?1:0;settings_dropdown=0;return;}if(settings_page==0){for(int t=0;t<3;t++){int top=85+t*48;if(ly>=top&&ly<top+40){set_theme(t);save_settings();desktop_base_dirty=1;return;}}if(ly>=235&&ly<277){sound_on=!sound_on;save_settings();if(sound_on)speaker_chime();return;}if(ly>=285&&ly<327){glass_on=!glass_on;desktop_base_dirty=1;save_settings();return;}if(ly>=335&&ly<377){settings_dropdown=3;return;}if(ly>=385&&ly<427){settings_dropdown=4;return;}}else{if(ly>=82&&ly<132){settings_dropdown=5;return;}if(ly>=145&&ly<195){settings_dropdown=6;return;}if(ly>=208&&ly<258){settings_dropdown=7;return;}}return;}
  if(active_app==APP_NETWORK&&ly>=300&&ly<355&&lx>=0&&lx<300){perform_http_get();return;}
  if(active_app==APP_CALENDAR&&ly>=34&&ly<75){if(lx<75){calendar_month--;if(calendar_month<1){calendar_month=12;calendar_year--;}}else if(lx>win_w-170){calendar_month++;if(calendar_month>12){calendar_month=1;calendar_year++;}}return;}
  if(active_app==APP_NOTES&&ly>=win_h-180&&ly<win_h-130&&lx<180){save_notes_and_status();return;}
  if(active_app==APP_CODE&&ly>=win_h-124&&ly<win_h-78&&lx<175){save_code();return;}
  if(active_app==APP_INSTALLER&&ly>=290&&ly<350&&lx>=0&&lx<320){stage_install_bundle();return;}
  /* App bodies receive keyboard input; clicking the body is intentionally harmless. */
  return;}
 if(by<48){if(bx<350){launcher_open=1;active_app=APP_LAUNCHER;win_x=300;win_y=70;win_w=1320;win_h=930;return;}if(bx>=350&&bx<430){start_app(APP_FINDER);return;}if(bx>=430&&bx<500){if(active_app==APP_CODE)save_code();else if(active_app==APP_NOTES)save_notes_and_status();else start_app(APP_NOTES);return;}if(bx>=500&&bx<590){glass_on=!glass_on;desktop_base_dirty=1;save_settings();return;}if(bx>=590&&bx<705){if(active_app!=APP_NONE)close_app();else start_app(APP_MONITOR);return;}if(bx>=705&&bx<820){start_app(APP_ABOUT);return;}}
 if(by>=850){if(bx<205){launcher_open=1;active_app=APP_LAUNCHER;win_x=300;win_y=70;win_w=1320;win_h=930;return;}const int centers[6]={620,760,900,1040,1180,1320};const int apps[6]={APP_FINDER,APP_TERMINAL,APP_NOTES,APP_CALCULATOR,APP_SETTINGS,APP_MONITOR};for(int i=0;i<6;i++)if(bx>=centers[i]-50&&bx<=centers[i]+50){start_app(apps[i]);return;}}
 if(bx>1660&&by>115&&by<220){start_app(APP_FINDER);return;}if(bx>1660&&by>230&&by<355){start_app(APP_NOTES);return;}if(bx>1660&&by>360&&by<475){start_app(APP_MONITOR);return;}
}
static void handle_key(EFI_INPUT_KEY k){if(k.ScanCode==0x0e){cycle_graphics_mode();return;}if(k.ScanCode==0x0d){start_app(APP_MONITOR);return;}if(k.ScanCode==0x0f){start_app(APP_SETTINGS);return;}if(k.ScanCode==0x10){start_app(APP_TERMINAL);return;}if(k.ScanCode==0x11){start_app(APP_FINDER);return;}if(k.ScanCode==0x12){start_app(APP_NOTES);return;}if(k.ScanCode==0x13){start_app(APP_CALCULATOR);return;}if(k.ScanCode==0x17){if(active_app==APP_NOTES)save_notes_and_status();if(active_app!=APP_NONE)close_app();else launcher_open=0;return;}if(k.ScanCode==0x0c){theme=(theme+1)%3;set_theme(theme);save_settings();return;}if(k.ScanCode==0x0b){launcher_open=1;active_app=APP_LAUNCHER;win_x=300;win_y=70;win_w=1320;win_h=930;return;}
 if(active_app==APP_FINDER){if(k.ScanCode==0x09){finder_scroll=finder_scroll>=10?finder_scroll-10:0;finder_selected=-1;finder_preview_open=0;return;}if(k.ScanCode==0x0a){if(finder_scroll+10<finder_entry_count)finder_scroll+=10;finder_selected=-1;finder_preview_open=0;return;}if(k.UnicodeChar==13){finder_open_selected();return;}if(k.UnicodeChar==8){finder_parent();return;}return;}
 if(active_app==APP_TERMINAL){if(k.UnicodeChar==13){terminal_execute_enhanced();return;}if(k.UnicodeChar==8){if(cmd_len){cmdline[--cmd_len]=0;}return;}if(k.UnicodeChar>=32&&k.UnicodeChar<127&&cmd_len+1<sizeof(cmdline)){cmdline[cmd_len++]=(char)k.UnicodeChar;cmdline[cmd_len]=0;}return;}
 if(active_app==APP_NOTES){if(k.UnicodeChar==19){save_notes_and_status();return;}if(k.UnicodeChar==8){if(note_len){while(note_len&&note_text[note_len-1]==' ')note_len--;if(note_len)note_len--;note_text[note_len]=0;}return;}if(k.UnicodeChar==13){if(note_len+1<sizeof(note_text)){note_text[note_len++]='\n';note_text[note_len]=0;}return;}if(k.UnicodeChar>=32&&k.UnicodeChar<127&&note_len+1<sizeof(note_text)){note_text[note_len++]=(char)k.UnicodeChar;note_text[note_len]=0;}return;}
 if(active_app==APP_CODE){if(k.UnicodeChar==19){save_code();return;}if(k.UnicodeChar==8){if(code_len){while(code_len&&code_text[code_len-1]==' ')code_len--;if(code_len)code_len--;code_text[code_len]=0;}return;}if(k.UnicodeChar==13){if(code_len+1<sizeof(code_text)){code_text[code_len++]='\n';code_text[code_len]=0;}return;}if(k.UnicodeChar>=32&&k.UnicodeChar<127&&code_len+1<sizeof(code_text)){code_text[code_len++]=(char)k.UnicodeChar;code_text[code_len]=0;}return;}
 if(k.UnicodeChar==' '&&active_app==APP_NONE){launcher_open=1;active_app=APP_LAUNCHER;win_x=300;win_y=70;win_w=1320;win_h=930;}}

/* Firmware input is normalized into the same versioned event ABI used by the
 * native kernel. The current desktop remains single-threaded: one producer polls
 * UEFI and one consumer dispatches events on the UI loop. */
static int post_desktop_event(const MINT_DESKTOP_EVENT *event){
 if(!uefi_desktop_events_ready)return 0;
 return mint_desktop_event_push(&uefi_desktop_events,event);
}
static void dispatch_desktop_events(int *dirty){
 MINT_DESKTOP_EVENT event;
 while(mint_desktop_event_pop(&uefi_desktop_events,&event)){
  uefi_desktop_events_consumed++;
  if(event.type==MINT_DESKTOP_EVENT_KEY){
   if(event.data.key.pressed){EFI_INPUT_KEY key;key.ScanCode=event.data.key.scancode;key.UnicodeChar=(CHAR16)event.data.key.codepoint;handle_key(key);if(dirty)*dirty=1;}
  }else if(event.type==MINT_DESKTOP_EVENT_POINTER_MOVE||event.type==MINT_DESKTOP_EVENT_POINTER_BUTTON){
   int nx=event.data.pointer.x,ny=event.data.pointer.y;
   int nl=(event.data.pointer.buttons&1u)!=0,nr=(event.data.pointer.buttons&2u)!=0;
   if(nx<1)nx=1;if(ny<1)ny=1;
   if(nx>=(int)screen_w-2)nx=(int)screen_w-3;if(ny>=(int)screen_h-2)ny=(int)screen_h-3;
   if(nx!=mouse_x||ny!=mouse_y||nl!=mouse_left||nr!=mouse_right){mouse_x=nx;mouse_y=ny;mouse_left=nl;mouse_right=nr;pointer_events++;if(dirty)*dirty=1;}
  }
 }
}

static int alloc_frame_set(u32 w,u32 h,u32 **bb,u32 **base,u32 **ba,u32 **bb2,u32 **gt,u32 *bh){
 UINTN bytes=(UINTN)w*h*4;u32 docky=(u32)(((u64)850*h)/1080);*bh=h>docky?h-docky:1;*bb=0;*base=0;*ba=0;*bb2=0;*gt=0;
 if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,bytes,(void**)bb))||!*bb)goto fail;
 if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,bytes,(void**)base))||!*base)goto fail;
 if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,bytes,(void**)gt))||!*gt)goto fail;
 if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,(UINTN)w*(*bh)*4,(void**)ba)))*ba=0;
 if(EFI_ERROR(bs->AllocatePool(EfiLoaderData,(UINTN)w*(*bh)*4,(void**)bb2)))*bb2=0;
 if(!*ba||!*bb2){if(*ba)bs->FreePool(*ba);if(*bb2)bs->FreePool(*bb2);*ba=0;*bb2=0;}
 memset(*bb,0,bytes);memset(*base,0,bytes);memset(*gt,0,bytes);return 1;
fail:if(*bb)bs->FreePool(*bb);if(*base)bs->FreePool(*base);if(*ba)bs->FreePool(*ba);if(*bb2)bs->FreePool(*bb2);if(*gt)bs->FreePool(*gt);*bb=*base=*ba=*bb2=*gt=0;return 0;
}
static int activate_gop_mode(u32 mode){
 if(!gop||!gop->Mode)return 0;UINTN sz=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION*info=0;EFI_STATUS s=gop->QueryMode(gop,mode,&sz,&info);if(EFI_ERROR(s)||!info)return 0;u32 w=info->HorizontalResolution,h=info->VerticalResolution,fmt=info->PixelFormat;bs->FreePool(info);if(w<800||h<600||w>2560||h>1440||(fmt!=0&&fmt!=1))return 0;
 u32 *nb=0,*nd=0,*na=0,*nb2=0,*ngt=0,bh=0;if(!alloc_frame_set(w,h,&nb,&nd,&na,&nb2,&ngt,&bh))return 0;
 if(mode!=gop->Mode->Mode){s=gop->SetMode(gop,mode);if(EFI_ERROR(s)){bs->FreePool(nb);bs->FreePool(nd);if(na)bs->FreePool(na);if(nb2)bs->FreePool(nb2);if(ngt)bs->FreePool(ngt);return 0;}}
 if(backbuffer)bs->FreePool(backbuffer);if(desktop_base)bs->FreePool(desktop_base);if(blur_a)bs->FreePool(blur_a);if(blur_b)bs->FreePool(blur_b);if(glass_tmp)bs->FreePool(glass_tmp);
 backbuffer=nb;desktop_base=nd;blur_a=na;blur_b=nb2;glass_tmp=ngt;screen_w=w;screen_h=h;pitch=gop->Mode->Info->PixelsPerScanLine;if(pitch<w)pitch=w;pixel_format=gop->Mode->Info->PixelFormat;blur_band_h=bh;update_backbuffer_surface();desktop_base_dirty=1;if(mouse_x>=(int)w)mouse_x=(int)w-1;if(mouse_y>=(int)h)mouse_y=(int)h-1;return 1;
}
static void cycle_graphics_mode(void){if(!gop_mode_count){set_msg("No alternate GOP mode list available");return;}UINTN current=0;for(UINTN i=0;i<gop_mode_count;i++)if(gop_modes[i]==gop->Mode->Mode){current=i;break;}for(UINTN step=1;step<=gop_mode_count;step++){UINTN next=(current+step)%gop_mode_count;if(activate_gop_mode(gop_modes[next])){char a[24],b[24],msg[96];u64_to_dec(a,screen_w);u64_to_dec(b,screen_h);UINTN n=0;msg[0]=0;append_str(msg,sizeof(msg),&n,"Display changed to ");append_str(msg,sizeof(msg),&n,a);append_str(msg,sizeof(msg),&n," x ");append_str(msg,sizeof(msg),&n,b);set_msg(msg);return;}}set_msg("Could not switch GOP mode; retaining current display");}

static void init_graphics(void){EFI_STATUS s=bs->LocateProtocol(&GUID_GOP,0,(void**)&gop);if(EFI_ERROR(s)||!gop||!gop->Mode)return;gop_mode_count=0;u32 best=gop->Mode->Mode;u64 bestscore=0;int exact=-1;u32 fallback=gop->Mode->Mode;u64 fallbackscore=0;for(u32 i=0;i<gop->Mode->MaxMode;i++){UINTN sz=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION*info=0;s=gop->QueryMode(gop,i,&sz,&info);if(EFI_ERROR(s)||!info)continue;u32 w=info->HorizontalResolution,h=info->VerticalResolution,fmt=info->PixelFormat;if((fmt==0||fmt==1)&&w>=800&&h>=600&&w<=2560&&h<=1440){if(gop_mode_count<64){gop_modes[gop_mode_count]=i;gop_mode_w[gop_mode_count]=w;gop_mode_h[gop_mode_count]=h;gop_mode_count++;}u64 score=(u64)w*h;if(w==1920&&h==1080)exact=(int)i;if(w<=1920&&h<=1080&&score>fallbackscore){fallback=i;fallbackscore=score;}if(score>bestscore){bestscore=score;best=i;}}bs->FreePool(info);}if(exact>=0)best=(u32)exact;else if(fallbackscore)best=fallback;else if(!bestscore)best=gop->Mode->Mode;if(!activate_gop_mode(best)){screen_w=gop->Mode->Info->HorizontalResolution;screen_h=gop->Mode->Info->VerticalResolution;pitch=gop->Mode->Info->PixelsPerScanLine;pixel_format=gop->Mode->Info->PixelFormat;u32 *nb=0,*nd=0,*na=0,*nb2=0,*ngt=0,bh=0;if(alloc_frame_set(screen_w,screen_h,&nb,&nd,&na,&nb2,&ngt,&bh)){backbuffer=nb;desktop_base=nd;blur_a=na;blur_b=nb2;glass_tmp=ngt;blur_band_h=bh;update_backbuffer_surface();}}}
static void connect_uefi_controllers(void){if(!bs->LocateHandleBuffer||!bs->ConnectController)return;EFI_HANDLE *handles=0;UINTN count=0;if(EFI_ERROR(bs->LocateHandleBuffer(0,0,0,&count,&handles))||!handles)return;EFI_STATUS (EFIAPI *connect)(EFI_HANDLE,EFI_HANDLE*,void*,u8)=(EFI_STATUS (EFIAPI *)(EFI_HANDLE,EFI_HANDLE*,void*,u8))bs->ConnectController;for(UINTN i=0;i<count;i++)connect(handles[i],0,0,1);bs->FreePool(handles);}
/* Convert UEFI relative counts into conservative screen pixels using the pointer's
   advertised counts/mm value. The UEFI Appendix F guidance is a useful baseline,
   but a 2%-per-mm cursor can be far too sensitive on high-count trackpads, so MintOS
   starts at 0.5% of the display extent per physical millimetre and caps each 10 ms
   step. Fixed-point remainders preserve slow motion; each device has its own residual. */
#define POINTER_MAX_STEP 12
static int scale_relative_delta(i32 counts,u64 resolution,u32 extent,i64 *remainder){
 if(!counts||extent<2||!remainder)return 0;
 i64 pixels_fp=0;
 if(resolution>0&&resolution<=0xffffffffULL){
   i64 den=(i64)resolution*200; /* counts/mm -> 0.5% extent per mm */
   i64 num=(i64)counts*(i64)(extent-1)*65536;
   pixels_fp=num/den+*remainder;
   i64 whole=pixels_fp/65536;
   *remainder=pixels_fp-whole*65536;
   if(whole>POINTER_MAX_STEP){*remainder=0;return POINTER_MAX_STEP;}
   if(whole< -POINTER_MAX_STEP){*remainder=0;return -POINTER_MAX_STEP;}
   return (int)whole;
 }
 /* Zero/invalid resolution fallback: 1/64 pixel per count, still bounded. */
 pixels_fp=(i64)counts*1024+*remainder;
 i64 whole=pixels_fp/65536;*remainder=pixels_fp-whole*65536;
 if(whole>POINTER_MAX_STEP){*remainder=0;return POINTER_MAX_STEP;}if(whole< -POINTER_MAX_STEP){*remainder=0;return -POINTER_MAX_STEP;}
 return (int)whole;
}
static int wait_i8042_input_clear(void){for(u32 i=0;i<100000;i++){u8 s;__asm__ volatile("inb %1,%0":"=a"(s):"Nd"((u16)0x64));if(!(s&2))return 1;}return 0;}
static int wait_i8042_output(u8 *value,int aux){for(u32 i=0;i<100000;i++){u8 s;__asm__ volatile("inb %1,%0":"=a"(s):"Nd"((u16)0x64));if(s&1){u8 v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"((u16)0x60));if(((s&0x20)!=0)==(aux!=0)){*value=v;return 1;}}}return 0;}
static int ps2_write_command(u8 c){if(!wait_i8042_input_clear())return 0;__asm__ volatile("outb %0,%1"::"a"(c),"Nd"((u16)0x64));return 1;}
static int ps2_write_data(u8 c){if(!wait_i8042_input_clear())return 0;__asm__ volatile("outb %0,%1"::"a"(c),"Nd"((u16)0x60));return 1;}
static int ps2_write_mouse(u8 v){u8 ack=0;for(int attempt=0;attempt<3;attempt++){if(!ps2_write_command(0xd4)||!ps2_write_data(v)||!wait_i8042_output(&ack,1))return 0;if(ack==0xfa)return 1;if(ack!=0xfe)return 0;}return 0;}
static void try_init_ps2_fallback(void){
 if(ps2_attempted||firmware_pointer_motion||absolute_samples||relative_samples)return;
 ps2_attempted=1;
 /* Port 0x64 may not exist on modern hardware; use bounded ACK checks only. */
 if(!ps2_write_command(0xa8)){copy_text(fallback_detail,sizeof(fallback_detail),"PS/2 fallback unavailable; controller did not respond");return;}
 u8 cfg=0;
 if(ps2_write_command(0x20)&&wait_i8042_output(&cfg,0)){
   cfg=(u8)((cfg|0x02)&~0x20); /* enable AUX interrupt bit; keep clock enabled */
   if(ps2_write_command(0x60)&&ps2_write_data(cfg)){}
 }
 if(ps2_write_mouse(0xf6)&&ps2_write_mouse(0xf4)){
   ps2_available=1;copy_text(fallback_detail,sizeof(fallback_detail),"Fallback: PS/2 AUX mouse enabled; waiting for packets");
 }else{ps2_available=0;ps2_errors++;copy_text(fallback_detail,sizeof(fallback_detail),"Fallback: no PS/2 AUX mouse ACK; USB fallback remains available");}
}
static i32 hid_item_signed(u32 value,u8 size){
 if(size==1&&(value&0x80u))return (i32)(value|0xffffff00u);
 if(size==2&&(value&0x8000u))return (i32)(value|0xffff0000u);
 return (i32)value;
}
static u32 hid_report_bits(const u8 *data,UINTN bytes,UINTN bit,u8 count){
 u32 value=0;if(count>32)count=32;
 for(u8 i=0;i<count;i++){UINTN at=bit+i;if(at/8>=bytes)break;if(data[at/8]&(1u<<(at&7)))value|=(1u<<i);}
 return value;
}
static i32 hid_value_signed(u32 value,u8 bits,i32 logical_min){
 if(bits==0||bits>32)return 0;
 if(logical_min<0){if(bits<32&&(value&(1u<<(bits-1))))value|=(~0u<<bits);return (i32)value;}
 return (i32)value;
}
static int parse_hid_pointer_descriptor(const u8 *desc,UINTN len,USB_HID_POINTER *m){
 u32 usage_page=0,report_size=0,report_count=0; i32 logical_min=0,logical_max=0;
 u8 current_id=0,target_id=0,selected_seen=0;u16 offsets[256]={0};
 u32 local_usage[32];u8 local_count=0;u32 usage_min=0,usage_max=0;int have_min=0,have_max=0;
 m->x_bit=m->y_bit=m->left_bit=m->right_bit=0xffffu;m->x_size=m->y_size=m->left_size=m->right_size=0;
 m->x_min=m->y_min=0;m->x_max=m->y_max=0;m->absolute_axes=0;m->report_id=0;
 for(UINTN pos=0;pos<len;){
   u8 prefix=desc[pos++];if(prefix==0xfe){if(pos+2>len)return 0;u8 n=desc[pos];pos+=2;if(pos+n>len)return 0;pos+=n;continue;}
   u8 sz=(u8)(prefix&3u);if(sz==3)sz=4;u8 type=(prefix>>2)&3u,tag=(prefix>>4)&15u;
   if(pos+sz>len)return 0;u32 value=0;for(u8 b=0;b<sz;b++)value|=(u32)desc[pos++]<<(8*b);
   if(type==1){ /* Global items */
     switch(tag){case 0:usage_page=value;break;case 1:logical_min=hid_item_signed(value,sz);break;case 2:logical_max=(logical_min<0)?hid_item_signed(value,sz):(i32)value;break;case 7:report_size=value;break;case 8:current_id=(u8)value;if(!selected_seen){target_id=current_id;selected_seen=1;m->report_id=target_id;}break;case 9:report_count=value;break;default:break;}
   }else if(type==2){ /* Local usage items */
     if(tag==0&&local_count<32)local_usage[local_count++]=value;
     else if(tag==1){usage_min=value;have_min=1;}else if(tag==2){usage_max=value;have_max=1;}
   }else if(type==0){ /* Main items */
     if(tag==8&&report_size&&report_count){
       u16 base=offsets[current_id];int constant=(value&1u)!=0;int variable=(value&2u)!=0;int relative=(value&4u)!=0;
       if(!constant&&variable&&current_id==target_id){
         UINTN nfields=report_count>64?64:report_count;
         for(UINTN j=0;j<nfields;j++){
           u32 use=j<local_count?local_usage[j]:((have_min&&have_max)?usage_min+(u32)j:0);
           if(have_max&&use>usage_max)continue;
           u64 bit=(u64)base+(u64)j*report_size;if(bit>65534)continue;
           if(usage_page==0x01&&use==0x30&&report_size<=32&&report_size>0){m->x_bit=(u16)bit;m->x_size=(u8)report_size;m->x_min=logical_min;m->x_max=logical_max;if(relative)m->absolute_axes&=(u8)~1u;else m->absolute_axes|=1u;}
           if(usage_page==0x01&&use==0x31&&report_size<=32&&report_size>0){m->y_bit=(u16)bit;m->y_size=(u8)report_size;m->y_min=logical_min;m->y_max=logical_max;if(relative)m->absolute_axes&=(u8)~2u;else m->absolute_axes|=2u;}
           if(usage_page==0x09&&(use==1||use==2)&&report_size==1){if(use==1){m->left_bit=(u16)bit;m->left_size=1;}else{m->right_bit=(u16)bit;m->right_size=1;}}
         }
       }
       u32 next=(u32)offsets[current_id]+report_size*report_count;offsets[current_id]=(u16)(next>65535?65535:next);
     }
     /* Local items are reset after every main item. */
     local_count=0;usage_min=usage_max=0;have_min=have_max=0;
   }
 }
 if(m->x_size==0||m->y_size==0||m->x_bit==0xffffu||m->y_bit==0xffffu)return 0;
 if((m->absolute_axes&3u)==3u&&(m->x_max<=m->x_min||m->y_max<=m->y_min))return 0;
 u32 bits=offsets[target_id];u32 bytes=(bits+7)/8+(target_id?1u:0u);
 if(bytes<3)bytes=3;if(bytes>m->max_packet||bytes>64)return 0;m->report_len=(u8)bytes;m->report_id=target_id;
 return 1;
}
static int get_hid_report_descriptor(EFI_USB_IO_PROTOCOL *io,u8 iface,u8 *desc,UINTN cap,UINTN *outlen){
 if(!io||!io->UsbControlTransfer||!desc||!outlen||cap>65535)return 0;
 u8 hid[9]={0};EFI_USB_DEVICE_REQUEST req;u32 ts=0;
 req.RequestType=0x81;req.Request=0x06;req.Value=0x2100;req.Index=iface;req.Length=9;
 EFI_STATUS stx=io->UsbControlTransfer(io,&req,EFI_USB_DATA_IN,1000,hid,sizeof(hid),&ts);
 if(EFI_ERROR(stx)||ts!=0||hid[1]!=0x21||hid[5]==0)return 0;
 u16 len=(u16)(hid[7]|((u16)hid[8]<<8));if(!len||len>cap)return 0;
 req.Value=0x2200;req.Length=len;ts=0;
 stx=io->UsbControlTransfer(io,&req,EFI_USB_DATA_IN,1000,desc,len,&ts);
 if(EFI_ERROR(stx)||ts!=0)return 0;*outlen=len;return 1;
}
static void init_usb_mouse_fallback(void){
 if(usb_setup_attempted)return;usb_setup_attempted=1;
 if(!bs->LocateHandleBuffer||!bs->HandleProtocol)return;
 EFI_HANDLE *handles=0;UINTN count=0;
 EFI_STATUS s=bs->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL,&GUID_USB_IO,0,&count,&handles);
 if(EFI_ERROR(s)||!handles){copy_text(fallback_detail,sizeof(fallback_detail),"Fallback: no EFI USB I/O handles; PS/2 fallback armed");return;}
 usb_io_handle_count=count;
 for(UINTN i=0;i<count&&usb_mouse_count<MAX_USB_MOUSE_DEVICES;i++){
   EFI_USB_IO_PROTOCOL *io=0;EFI_USB_INTERFACE_DESCRIPTOR id;
   if(EFI_ERROR(bs->HandleProtocol(handles[i],&GUID_USB_IO,(void**)&io))||!io||!io->UsbGetInterfaceDescriptor||!io->UsbGetEndpointDescriptor||!io->UsbSyncInterruptTransfer)continue;
   if(EFI_ERROR(io->UsbGetInterfaceDescriptor(io,&id)))continue;
   if(id.InterfaceClass!=0x03)continue;
   u8 ep=0;u16 packet=0;
   for(u8 e=0;e<id.NumEndpoints&&e<16;e++){
     EFI_USB_ENDPOINT_DESCRIPTOR ed;
     if(EFI_ERROR(io->UsbGetEndpointDescriptor(io,e,&ed)))continue;
     if((ed.Attributes&3u)==3u&&(ed.EndpointAddress&0x80u)&&ed.MaxPacketSize>=3){ep=ed.EndpointAddress;packet=(u16)(ed.MaxPacketSize&0x07ffu);break;}
   }
   if(!ep||packet<3)continue;
   USB_HID_POINTER candidate;memset(&candidate,0,sizeof(candidate));candidate.io=io;candidate.endpoint=ep;candidate.interface_number=id.InterfaceNumber;candidate.max_packet=packet;candidate.active=1;
   if(id.InterfaceSubClass==0x01&&id.InterfaceProtocol==0x02&&io->UsbControlTransfer){
     EFI_USB_DEVICE_REQUEST req;u32 ts=0;req.RequestType=0x21;req.Request=0x0b;req.Value=0;req.Index=id.InterfaceNumber;req.Length=0;
     EFI_STATUS cs=io->UsbControlTransfer(io,&req,EFI_USB_NO_DATA,1000,0,0,&ts);
     if(EFI_ERROR(cs)||ts!=0)continue;
     req.Request=0x0a;ts=0;io->UsbControlTransfer(io,&req,EFI_USB_NO_DATA,1000,0,0,&ts);
     candidate.boot_format=1;candidate.report_id=0;candidate.report_len=(u8)(packet>8?8:packet);candidate.x_bit=8;candidate.y_bit=16;candidate.x_size=8;candidate.y_size=8;candidate.left_bit=0;candidate.right_bit=1;candidate.left_size=candidate.right_size=1;candidate.x_min=candidate.y_min=-127;candidate.x_max=candidate.y_max=127;
   }else{
     u8 desc[512];UINTN desc_len=0;
     if(!get_hid_report_descriptor(io,id.InterfaceNumber,desc,sizeof(desc),&desc_len)||!parse_hid_pointer_descriptor(desc,desc_len,&candidate)){usb_hid_other_count++;usb_hid_descriptor_errors++;continue;}
   }
   usb_mice[usb_mouse_count++]=candidate;
 }
 bs->FreePool(handles);
 if(usb_mouse_count)copy_text(fallback_detail,sizeof(fallback_detail),"Fallback: USB HID pointer found; waiting for input reports");
 else if(usb_hid_other_count)copy_text(fallback_detail,sizeof(fallback_detail),"Fallback: generic HID interfaces found; no parseable pointer report");
 else copy_text(fallback_detail,sizeof(fallback_detail),"Fallback: no USB HID pointer; PS/2 fallback armed");
}
static void poll_usb_mouse_fallback(int *dx,int *dy,int *left,int *right,int *has_abs,int *abs_x,int *abs_y){
 if(firmware_pointer_motion||!usb_mouse_count)return;
 for(UINTN i=0;i<usb_mouse_count;i++){
   USB_HID_POINTER *m=&usb_mice[i];if(!m->active||!m->io)continue;
   u8 data[64]={0};UINTN n=m->report_len;u32 ts=0;usb_mouse_polls++;
   EFI_STATUS s=m->io->UsbSyncInterruptTransfer(m->io,m->endpoint,data,&n,10,&ts);
   if(EFI_ERROR(s)){if(s!=EFI_NOT_READY&&s!=0x8000000000000012ULL)usb_mouse_errors++;continue;}
   if(ts!=0||n<m->report_len){if(ts!=0)usb_mouse_errors++;continue;}
   usb_mouse_reports++;
   UINTN off=(m->report_id?1u:0u);if(m->report_id&&(!n||data[0]!=m->report_id))continue;
   u32 xv,yv; i32 x,y;
   if(m->boot_format){
     u8 buttons=data[0]&3u;x=(i8)data[1];y=(i8)data[2];
     if(buttons!=m->last_buttons){*left=(buttons&1u)!=0;*right=(buttons&2u)!=0;m->last_buttons=buttons;}
     x*=2;y*=2;if(x>48)x=48;if(x< -48)x=-48;if(y>48)y=48;if(y< -48)y=-48;*dx+=x;*dy+=y;continue;
   }
   UINTN bytes=n-off;
   xv=hid_report_bits(data+off,bytes,m->x_bit,m->x_size);yv=hid_report_bits(data+off,bytes,m->y_bit,m->y_size);
   x=hid_value_signed(xv,m->x_size,m->x_min);y=hid_value_signed(yv,m->y_size,m->y_min);
   int lx=0,rx=0;if(m->left_size)lx=hid_report_bits(data+off,bytes,m->left_bit,1)!=0;if(m->right_size)rx=hid_report_bits(data+off,bytes,m->right_bit,1)!=0;
   if((m->absolute_axes&3u)==3u&&m->x_max>m->x_min&&m->y_max>m->y_min){
     if(x<m->x_min)x=m->x_min;if(x>m->x_max)x=m->x_max;if(y<m->y_min)y=m->y_min;if(y>m->y_max)y=m->y_max;
     int nx=(int)(((i64)(x-m->x_min)*(screen_w-1))/(m->x_max-m->x_min));int ny=(int)(((i64)(y-m->y_min)*(screen_h-1))/(m->y_max-m->y_min));
     if(nx<0)nx=0;if(ny<0)ny=0;if(nx>=(int)screen_w)nx=screen_w-1;if(ny>=(int)screen_h)ny=screen_h-1;
     if(nx!=mouse_x||ny!=mouse_y||lx!=*left||rx!=*right)*has_abs=1;*abs_x=nx;*abs_y=ny;*left=lx;*right=rx;
   }else{
     int sx=x*2,sy=y*2;if(sx>48)sx=48;if(sx< -48)sx=-48;if(sy>48)sy=48;if(sy< -48)sy=-48;*dx+=sx;*dy+=sy;
     *left=lx;*right=rx;
   }
 }
}
static void poll_ps2_fallback(int *dx,int *dy,int *left,int *right){
 if(!ps2_available||firmware_pointer_motion)return;
 for(int tries=0;tries<32;tries++){
   u8 status;__asm__ volatile("inb %1,%0":"=a"(status):"Nd"((u16)0x64));
   if(!(status&1))break;
   /* Never consume bytes belonging to the keyboard firmware console. */
   if(!(status&0x20))break;
   u8 b;__asm__ volatile("inb %1,%0":"=a"(b):"Nd"((u16)0x60));
   if(ps2_packet_index==0&&!(b&0x08))continue;
   ps2_packet[ps2_packet_index++]=b;
   if(ps2_packet_index==3){ps2_packet_index=0;ps2_packets++;
     int x=(int)(int8_t)ps2_packet[1],y=-(int)(int8_t)ps2_packet[2];
     if(ps2_packet[0]&0x40)x=(ps2_packet[0]&0x10)?-64:64;
     if(ps2_packet[0]&0x80)y=(ps2_packet[0]&0x20)?64:-64;
     if(x>48)x=48;if(x< -48)x=-48;if(y>48)y=48;if(y< -48)y=-48;
     *dx+=x*2;*dy+=y*2;*left=(ps2_packet[0]&1)!=0;*right=(ps2_packet[0]&2)!=0;
   }
 }
}
static void refresh_fallback_detail(void){
 char a[20],b[20],c[20],d[20],e[20],f[20],g[20],buf[160];UINTN n=0;buf[0]=0;
 u64_to_dec(a,usb_io_handle_count);u64_to_dec(b,usb_mouse_count);u64_to_dec(c,usb_mouse_reports);u64_to_dec(d,ps2_packets);u64_to_dec(e,usb_mouse_errors+ps2_errors+usb_hid_descriptor_errors);u64_to_dec(f,usb_mouse_polls);u64_to_dec(g,usb_hid_other_count);
 append_str(buf,sizeof(buf),&n,"USBIO:");append_str(buf,sizeof(buf),&n,a);append_str(buf,sizeof(buf),&n," HID:");append_str(buf,sizeof(buf),&n,b);append_str(buf,sizeof(buf),&n," other:");append_str(buf,sizeof(buf),&n,g);
 append_str(buf,sizeof(buf),&n," poll:");append_str(buf,sizeof(buf),&n,f);append_str(buf,sizeof(buf),&n," reports:");append_str(buf,sizeof(buf),&n,c);append_str(buf,sizeof(buf),&n," PS/2 pkts:");append_str(buf,sizeof(buf),&n,d);append_str(buf,sizeof(buf),&n," errors:");append_str(buf,sizeof(buf),&n,e);
 copy_text(fallback_detail,sizeof(fallback_detail),buf);
}

static void refresh_pointer_label(void){
 char a[24],r[24],as[24],rs[24],am[24],rm[24],buf[192]; UINTN n=0;
 u64_to_dec(a,absolute_pointer_count);u64_to_dec(r,relative_pointer_count);
 u64_to_dec(as,absolute_samples);u64_to_dec(rs,relative_samples);
 u64_to_dec(am,absolute_motion_samples);u64_to_dec(rm,relative_motion_samples);
 buf[0]=0;append_str(buf,sizeof(buf),&n,"Pointer A:");append_str(buf,sizeof(buf),&n,a);
 append_str(buf,sizeof(buf),&n," R:");append_str(buf,sizeof(buf),&n,r);
 if(!absolute_pointer_count&&!relative_pointer_count){
   append_str(buf,sizeof(buf),&n," | no firmware pointer protocol");
 }else{
   append_str(buf,sizeof(buf),&n," | OK A:");append_str(buf,sizeof(buf),&n,as);append_str(buf,sizeof(buf),&n," R:");append_str(buf,sizeof(buf),&n,rs);
   append_str(buf,sizeof(buf),&n," move A:");append_str(buf,sizeof(buf),&n,am);append_str(buf,sizeof(buf),&n," R:");append_str(buf,sizeof(buf),&n,rm);
 }
 copy_text(pointer_label,sizeof(pointer_label),buf);
}
static void refresh_pointer_detail(void){
 char a[24],b[24],c[24],d[24],e[24],f[24],g[24],h[24],j[24],buf[192];UINTN n=0;buf[0]=0;
 if(last_abs_seen&&last_abs_device>=0){
   u64_to_dec(a,(u64)last_abs_device);u64_to_dec(b,last_abs_x);u64_to_dec(c,last_abs_minx);u64_to_dec(d,last_abs_maxx);
   u64_to_dec(e,last_abs_y);u64_to_dec(f,last_abs_miny);u64_to_dec(g,last_abs_maxy);u64_to_dec(h,(u64)mouse_x);u64_to_dec(j,(u64)mouse_y);
   append_str(buf,sizeof(buf),&n,"ABS#");append_str(buf,sizeof(buf),&n,a);append_str(buf,sizeof(buf),&n," X=");append_str(buf,sizeof(buf),&n,b);
   append_str(buf,sizeof(buf),&n,"[");append_str(buf,sizeof(buf),&n,c);append_str(buf,sizeof(buf),&n,"..");append_str(buf,sizeof(buf),&n,d);append_str(buf,sizeof(buf),&n,"] Y=");append_str(buf,sizeof(buf),&n,e);
   append_str(buf,sizeof(buf),&n,"[");append_str(buf,sizeof(buf),&n,f);append_str(buf,sizeof(buf),&n,"..");append_str(buf,sizeof(buf),&n,g);append_str(buf,sizeof(buf),&n,"] cursor=");append_str(buf,sizeof(buf),&n,h);append_str(buf,sizeof(buf),&n,",");append_str(buf,sizeof(buf),&n,j);
 }else if(relative_samples){
   u64_to_dec(a,(u64)(last_rel_raw_dx<0?-(i64)last_rel_raw_dx:last_rel_raw_dx));u64_to_dec(b,(u64)(last_rel_raw_dy<0?-(i64)last_rel_raw_dy:last_rel_raw_dy));
   u64_to_dec(c,(u64)mouse_x);u64_to_dec(d,(u64)mouse_y);u64_to_dec(e,last_rel_resolution_x);u64_to_dec(f,last_rel_resolution_y);
   append_str(buf,sizeof(buf),&n,"REL raw X=");if(last_rel_raw_dx<0)append_str(buf,sizeof(buf),&n,"-");append_str(buf,sizeof(buf),&n,a);
   append_str(buf,sizeof(buf),&n," Y=");if(last_rel_raw_dy<0)append_str(buf,sizeof(buf),&n,"-");append_str(buf,sizeof(buf),&n,b);
   append_str(buf,sizeof(buf),&n," res=");append_str(buf,sizeof(buf),&n,e);append_str(buf,sizeof(buf),&n,",");append_str(buf,sizeof(buf),&n,f);
   append_str(buf,sizeof(buf),&n," px=");if(last_rel_dx<0)append_str(buf,sizeof(buf),&n,"-");u64_to_dec(g,(u64)(last_rel_dx<0?-(i64)last_rel_dx:last_rel_dx));append_str(buf,sizeof(buf),&n,g);
   append_str(buf,sizeof(buf),&n,",");if(last_rel_dy<0)append_str(buf,sizeof(buf),&n,"-");u64_to_dec(h,(u64)(last_rel_dy<0?-(i64)last_rel_dy:last_rel_dy));append_str(buf,sizeof(buf),&n,h);
   append_str(buf,sizeof(buf),&n," cursor=");append_str(buf,sizeof(buf),&n,c);append_str(buf,sizeof(buf),&n,",");append_str(buf,sizeof(buf),&n,d);
 }else copy_text(buf,sizeof(buf),"No successful pointer sample yet; move the mouse.");
 copy_text(pointer_detail,sizeof(pointer_detail),buf);
}
static void refresh_event_detail(void){
 char t[24],k[24],a[24],r[24],an[24],rn[24],ae[24],re[24],bad[24],q[24],d[24],buf[160]; UINTN n=0; buf[0]=0;
 u64_to_dec(t,timer_wakeups);u64_to_dec(k,keyboard_wakeups);u64_to_dec(a,absolute_event_wakeups);u64_to_dec(r,relative_event_wakeups);
 u64_to_dec(an,absolute_not_ready);u64_to_dec(rn,relative_not_ready);u64_to_dec(ae,absolute_errors);u64_to_dec(re,relative_errors);u64_to_dec(bad,absolute_rejected_samples);
 append_str(buf,sizeof(buf),&n,"NR A:");append_str(buf,sizeof(buf),&n,an);append_str(buf,sizeof(buf),&n," R:");append_str(buf,sizeof(buf),&n,rn);
 append_str(buf,sizeof(buf),&n," ERR A:");append_str(buf,sizeof(buf),&n,ae);append_str(buf,sizeof(buf),&n," R:");append_str(buf,sizeof(buf),&n,re);append_str(buf,sizeof(buf),&n," badA:");append_str(buf,sizeof(buf),&n,bad);
 append_str(buf,sizeof(buf),&n," | wake T:");append_str(buf,sizeof(buf),&n,t);append_str(buf,sizeof(buf),&n," K:");append_str(buf,sizeof(buf),&n,k);
 append_str(buf,sizeof(buf),&n," A:");append_str(buf,sizeof(buf),&n,a);append_str(buf,sizeof(buf),&n," R:");append_str(buf,sizeof(buf),&n,r);
 u64_to_dec(q,uefi_desktop_events_consumed);u64_to_dec(d,mint_desktop_event_dropped(&uefi_desktop_events));append_str(buf,sizeof(buf),&n," Q:");append_str(buf,sizeof(buf),&n,q);append_str(buf,sizeof(buf),&n," Qdrop:");append_str(buf,sizeof(buf),&n,d);
 copy_text(event_detail,sizeof(event_detail),buf);
}
static void init_pointer(void){
 connect_uefi_controllers(); absolute_pointer=0; pointer=0;
 absolute_pointer_count=0; relative_pointer_count=0;
 memset(absolute_pointers,0,sizeof(absolute_pointers));memset(relative_pointers,0,sizeof(relative_pointers));
 /* LocateProtocol returns only the first matching instance. Enumerate every handle so
    an inert touch device or stale firmware instance cannot hide a usable mouse. */
 if(bs->LocateHandleBuffer&&bs->HandleProtocol){
   EFI_HANDLE *handles=0;UINTN count=0;
   EFI_STATUS s=bs->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL,&GUID_ABSOLUTE_POINTER,0,&count,&handles);
   if(!EFI_ERROR(s)&&handles){
     for(UINTN i=0;i<count&&absolute_pointer_count<MAX_POINTER_DEVICES;i++){
       EFI_ABSOLUTE_POINTER_PROTOCOL *p=0;
       if(!EFI_ERROR(bs->HandleProtocol(handles[i],&GUID_ABSOLUTE_POINTER,(void**)&p))&&p){
         absolute_pointers[absolute_pointer_count++]=p;
         if(p->Reset)p->Reset(p,0);
       }
     }
     bs->FreePool(handles);
   }
   handles=0;count=0;
   s=bs->LocateHandleBuffer(EFI_LOCATE_BY_PROTOCOL,&GUID_SIMPLE_POINTER,0,&count,&handles);
   if(!EFI_ERROR(s)&&handles){
     for(UINTN i=0;i<count&&relative_pointer_count<MAX_POINTER_DEVICES;i++){
       EFI_SIMPLE_POINTER_PROTOCOL *p=0;
       if(!EFI_ERROR(bs->HandleProtocol(handles[i],&GUID_SIMPLE_POINTER,(void**)&p))&&p){
         relative_pointers[relative_pointer_count++]=p;
         if(p->Reset)p->Reset(p,0);
       }
     }
     bs->FreePool(handles);
   }
 }
 /* Compatibility fallback for firmware with incomplete handle enumeration. */
 if(!absolute_pointer_count&&bs->LocateProtocol){
   EFI_ABSOLUTE_POINTER_PROTOCOL *p=0;
   if(!EFI_ERROR(bs->LocateProtocol(&GUID_ABSOLUTE_POINTER,0,(void**)&p))&&p){
     absolute_pointers[absolute_pointer_count++]=p;if(p->Reset)p->Reset(p,0);
   }
 }
 if(!relative_pointer_count&&bs->LocateProtocol){
   EFI_SIMPLE_POINTER_PROTOCOL *p=0;
   if(!EFI_ERROR(bs->LocateProtocol(&GUID_SIMPLE_POINTER,0,(void**)&p))&&p){
     relative_pointers[relative_pointer_count++]=p;if(p->Reset)p->Reset(p,0);
   }
 }
 if(absolute_pointer_count)absolute_pointer=absolute_pointers[0];
 if(relative_pointer_count)pointer=relative_pointers[0];
 init_usb_mouse_fallback();
 refresh_pointer_label();refresh_pointer_detail();refresh_event_detail();refresh_fallback_detail();
}

/* Firmware pointer and USB-backed pointer protocols are event driven. A loop that only
   calls Stall() can leave firmware event notifications undispatched. Use a 100 Hz UEFI
   timer and WaitForEvent so firmware can service pointer/USB events while we run the UI. */
static void init_input_timer(void){
 input_timer_event=0;timer_loop_ready=0;
 if(!bs->CreateEvent||!bs->SetTimer||!bs->WaitForEvent)return;
 EFI_STATUS s=bs->CreateEvent(EVT_TIMER,TPL_APPLICATION,0,0,&input_timer_event);
 if(EFI_ERROR(s)||!input_timer_event){input_timer_event=0;return;}
 s=bs->SetTimer(input_timer_event,TIMER_PERIODIC,100000ULL); /* 10 ms in 100 ns units */
 if(EFI_ERROR(s)){if(bs->CloseEvent)bs->CloseEvent(input_timer_event);input_timer_event=0;return;}
 timer_loop_ready=1;
}
static void wait_for_ui_event(void){
 if(!timer_loop_ready||!input_timer_event||!bs->WaitForEvent){if(bs->Stall)bs->Stall(10000);return;}
 EFI_EVENT events[MAX_WAIT_EVENTS];u8 sources[MAX_WAIT_EVENTS];UINTN n=0;
 events[n]=input_timer_event;sources[n++]=0;
 if(conin&&conin->WaitForKey&&n<MAX_WAIT_EVENTS){events[n]=conin->WaitForKey;sources[n++]=1;}
 for(UINTN i=0;i<absolute_pointer_count&&n<MAX_WAIT_EVENTS;i++)if(absolute_pointers[i]&&absolute_pointers[i]->WaitForInput){events[n]=absolute_pointers[i]->WaitForInput;sources[n++]=2;}
 for(UINTN i=0;i<relative_pointer_count&&n<MAX_WAIT_EVENTS;i++)if(relative_pointers[i]&&relative_pointers[i]->WaitForInput){events[n]=relative_pointers[i]->WaitForInput;sources[n++]=3;}
 if(n==0){if(bs->Stall)bs->Stall(10000);return;}
 UINTN index=0;EFI_STATUS s=bs->WaitForEvent(n,events,&index);
 if(EFI_ERROR(s)||index>=n){if(bs->Stall)bs->Stall(10000);return;}
 switch(sources[index]){case 0:timer_wakeups++;break;case 1:keyboard_wakeups++;break;case 2:absolute_event_wakeups++;break;case 3:relative_event_wakeups++;break;}
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system){boot_image_handle=image;st=system;bs=(EFI_BOOT_SERVICES*)st->BootServices;conin=st->ConIn;if(!bs)return EFI_LOAD_ERROR;mint_desktop_event_queue_init(&uefi_desktop_events);uefi_desktop_events_ready=1;uefi_desktop_events_consumed=0;if(bs->SetWatchdogTimer)((EFI_STATUS(EFIAPI*)(u64,u64,u64,CHAR16*))bs->SetWatchdogTimer)(0,0,0,0);init_graphics();load_wallpaper(image);find_storage(image);ensure_seed_files();active_root=data_root?data_root:boot_root;set_theme(0);load_settings();init_pointer();init_input_timer();detect_audio_controller();init_network_diag();note_len=0;load_notes();mouse_x=(int)screen_w/2;mouse_y=(int)screen_h/2;mouse_prev_left=0;mouse_left=0;if(backbuffer){render_splash();bs->Stall(120000);}/* Do not auto-play a PC-speaker-only tone on modern machines; SOUND TEST is explicit. */
 if(!backbuffer){if(st->ConOut){/* Minimal fallback status for firmware console. */}return EFI_LOAD_ERROR;}if(wait_for_boot_choice()){set_msg("MintDE 1.0 selected; loading the native kernel...");if(!launch_native_kernel())start_app(APP_TERMINAL);}render();
 for(;;){ticks++;int dirty=0;EFI_INPUT_KEY key;if(conin&&conin->ReadKeyStroke){UINTN key_budget=32;while(key_budget--&&!EFI_ERROR(conin->ReadKeyStroke(conin,&key))){MINT_DESKTOP_EVENT ev;memset(&ev,0,sizeof(ev));ev.type=MINT_DESKTOP_EVENT_KEY;ev.data.key.codepoint=key.UnicodeChar;ev.data.key.scancode=key.ScanCode;ev.data.key.pressed=1;(void)post_desktop_event(&ev);}}
  int absolute_changed=0,rel_changed=0;
  int abs_x=mouse_x,abs_y=mouse_y,abs_left=mouse_left,abs_right=mouse_right;
  int rel_dx=0,rel_dy=0,rel_left=mouse_left,rel_right=mouse_right;
  /* Poll every absolute-pointer instance. Prefer the first real state change this
     frame; an unchanged/stale absolute device must not starve relative input. */
  for(UINTN i=0;i<absolute_pointer_count;i++){
    EFI_ABSOLUTE_POINTER_PROTOCOL *p=absolute_pointers[i];EFI_ABSOLUTE_POINTER_STATE ap;
    EFI_STATUS ps=p&&p->GetState?p->GetState(p,&ap):EFI_UNSUPPORTED;
    if(!EFI_ERROR(ps)){
      absolute_samples++;
      if(p->Mode){
        EFI_ABSOLUTE_POINTER_MODE *m=p->Mode;
        u64 minx=m->AbsoluteMinX,miny=m->AbsoluteMinY,maxx=m->AbsoluteMaxX,maxy=m->AbsoluteMaxY;
        last_abs_device=(int)i;last_abs_seen=1;last_abs_x=ap.CurrentX;last_abs_y=ap.CurrentY;
        last_abs_minx=minx;last_abs_miny=miny;last_abs_maxx=maxx;last_abs_maxy=maxy;
        /* UEFI coordinates are UINT64; current firmware mice use compact ranges. Guard
           degenerate or implausibly wide ranges so scale multiplication cannot wrap. */
        if(maxx>minx&&maxy>miny&&(maxx-minx)<=0xffffffffULL&&(maxy-miny)<=0xffffffffULL&&
           ap.CurrentX>=minx&&ap.CurrentY>=miny&&ap.CurrentX<=maxx&&ap.CurrentY<=maxy){
          u64 rx=maxx-minx,ry=maxy-miny,vx=ap.CurrentX-minx,vy=ap.CurrentY-miny;
          int nx=(int)((vx*(screen_w-1))/rx),ny=(int)((vy*(screen_h-1))/ry);
          if(nx<0)nx=0;if(ny<0)ny=0;if(nx>=(int)screen_w)nx=(int)screen_w-1;if(ny>=(int)screen_h)ny=(int)screen_h-1;
          int nl=(ap.ActiveButtons&1)!=0,nr=(ap.ActiveButtons&2)!=0;
          if(nx!=mouse_x||ny!=mouse_y||nl!=mouse_left||nr!=mouse_right)absolute_motion_samples++;
          if(!absolute_changed&&(nx!=mouse_x||ny!=mouse_y||nl!=mouse_left||nr!=mouse_right)){
            abs_x=nx;abs_y=ny;abs_left=nl;abs_right=nr;absolute_changed=1;
          }
        }else absolute_rejected_samples++;
      }else absolute_rejected_samples++;
    }else if(ps==EFI_NOT_READY)absolute_not_ready++;else absolute_errors++;
  }
  /* Drain relative instances too, even when absolute input changed, so the same mouse
     event cannot remain queued and be applied again on the following frame. */
  for(UINTN i=0;i<relative_pointer_count;i++){
    EFI_SIMPLE_POINTER_PROTOCOL *p=relative_pointers[i];EFI_SIMPLE_POINTER_STATE ps;
    EFI_STATUS status=p&&p->GetState?p->GetState(p,&ps):EFI_UNSUPPORTED;
    if(!EFI_ERROR(status)){
      relative_samples++;
      i32 rawx=ps.RelativeMovementX,rawy=ps.RelativeMovementY;
      u64 rx=(p&&p->Mode)?p->Mode->ResolutionX:0,ry=(p&&p->Mode)?p->Mode->ResolutionY:0;
      int dx=scale_relative_delta(rawx,rx,screen_w,&relative_remainder_x_fp[i<MAX_POINTER_DEVICES?i:0]);
      int dy=scale_relative_delta(rawy,ry,screen_h,&relative_remainder_y_fp[i<MAX_POINTER_DEVICES?i:0]);
      last_rel_raw_dx=rawx;last_rel_raw_dy=rawy;last_rel_resolution_x=rx;last_rel_resolution_y=ry;
      last_rel_dx=dx;last_rel_dy=dy;last_rel_left=ps.LeftButton;last_rel_right=ps.RightButton;
      int button_change=0;
      if(i<MAX_POINTER_DEVICES){u8 buttons=(u8)((ps.LeftButton?1:0)|(ps.RightButton?2:0));
        if(relative_buttons_known[i]&&relative_last_buttons[i]!=buttons)button_change=1;
        relative_last_buttons[i]=buttons;relative_buttons_known[i]=1;
      }else button_change=((int)ps.LeftButton)!=mouse_left||((int)ps.RightButton)!=mouse_right;
      if(rawx||rawy||button_change)relative_motion_samples++;
      if(!rel_changed&&(dx||dy||button_change)){
        rel_dx=dx;rel_dy=dy;rel_left=ps.LeftButton;rel_right=ps.RightButton;rel_changed=1;
      }
      if(rawx||rawy||button_change)firmware_pointer_motion=1;
    }else if(status==EFI_NOT_READY)relative_not_ready++;else relative_errors++;
  }
  /* Only use lower-level fallbacks until one firmware pointer actually moves. */
  int fb_dx=0,fb_dy=0,fb_left=mouse_left,fb_right=mouse_right,usb_abs_changed=0,usb_abs_x=mouse_x,usb_abs_y=mouse_y;
  if(!absolute_changed&&!rel_changed)poll_usb_mouse_fallback(&fb_dx,&fb_dy,&fb_left,&fb_right,&usb_abs_changed,&usb_abs_x,&usb_abs_y);
  if(!firmware_pointer_motion&&timer_wakeups>200&&absolute_samples==0&&relative_samples==0&&usb_mouse_reports==0&&!ps2_attempted)try_init_ps2_fallback();
  poll_ps2_fallback(&fb_dx,&fb_dy,&fb_left,&fb_right);
  if(fb_dx||fb_dy||fb_left!=mouse_left||fb_right!=mouse_right){if(!rel_changed&&!absolute_changed){rel_dx=fb_dx;rel_dy=fb_dy;rel_left=fb_left;rel_right=fb_right;rel_changed=1;}}
  if(usb_abs_changed&&!absolute_changed){abs_x=usb_abs_x;abs_y=usb_abs_y;abs_left=fb_left;abs_right=fb_right;absolute_changed=1;}
  if(absolute_changed||rel_changed){
    int nx=absolute_changed?abs_x:mouse_x+rel_dx;
    int ny=absolute_changed?abs_y:mouse_y+rel_dy;
    int nl=absolute_changed?abs_left:rel_left,nr=absolute_changed?abs_right:rel_right;
    if(nx<1)nx=1;if(ny<1)ny=1;
    if(nx>=(int)screen_w-2)nx=screen_w-3;if(ny>=(int)screen_h-2)ny=screen_h-3;
    MINT_DESKTOP_EVENT ev;memset(&ev,0,sizeof(ev));
    ev.type=((nl!=mouse_left)||(nr!=mouse_right))?MINT_DESKTOP_EVENT_POINTER_BUTTON:MINT_DESKTOP_EVENT_POINTER_MOVE;
    ev.data.pointer.x=nx;ev.data.pointer.y=ny;ev.data.pointer.dx=nx-mouse_x;ev.data.pointer.dy=ny-mouse_y;
    ev.data.pointer.buttons=(nl?1u:0u)|(nr?2u:0u);
    ev.data.pointer.changed_buttons=((nl!=mouse_left)?1u:0u)|((nr!=mouse_right)?2u:0u);
    (void)post_desktop_event(&ev);
    if(absolute_changed&&!usb_abs_changed)firmware_pointer_motion=1;
  }
  dispatch_desktop_events(&dirty);
  if((ticks%100)==0){refresh_pointer_label();refresh_pointer_detail();refresh_event_detail();refresh_fallback_detail();}

  if(mouse_left&&!mouse_prev_left){mouse_click(mouse_x,mouse_y);dirty=1;}if(dragging&&mouse_left){static int prevx=0,prevy=0;if(prevx&&prevy){win_x+=base_x(mouse_x)-base_x(prevx);win_y+=base_y(mouse_y)-base_y(prevy);if(win_x<0)win_x=0;if(win_y<50)win_y=50;if(win_x+win_w>1920)win_x=1920-win_w;if(win_y+win_h>1080)win_y=1080-win_h;}prevx=mouse_x;prevy=mouse_y;dirty=1;}else dragging=0;mouse_prev_left=mouse_left;
  /* Redraw on pointer input and at a modest cadence for the status clock. */
  if(dirty||(ticks%100==0))render();wait_for_ui_event();
 }
 return EFI_SUCCESS;
}
