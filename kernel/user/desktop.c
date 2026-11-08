#include "include/mintos/runtime.h"

/* Individually authored PNG masters live under assets/icons/. These 96x96
 * straight-alpha RGBA payloads are generated from those PNGs and linked into
 * the ring-3 app so the kernel can safely composite them through one bounded syscall. */
extern const uint8_t mint_icon_files_rgba[];
extern const uint8_t mint_icon_launchpad_rgba[];
extern const uint8_t mint_icon_terminal_rgba[];
extern const uint8_t mint_icon_settings_rgba[];
extern const uint8_t mint_icon_system_monitor_rgba[];
extern const uint8_t mint_icon_disk_utility_rgba[];
extern const uint8_t mint_icon_database_rgba[];
extern const uint8_t mint_icon_storage_rgba[];
extern const uint8_t mint_icon_cpu_rgba[];
extern const uint8_t mint_icon_mintde_rgba[];
extern const uint8_t mint_icon_windows_rgba[];
extern const uint8_t mint_icon_notes_rgba[];

/* MintDE 1.0.7 — native ring-3 desktop for MintOS26 26.5.008.0.
 * A mouse-driven shell with a menu bar, desktop dock, versioned About panels,
 * bounded native services, and no desktop shortcut clutter. */
static uint32_t C_WALL = 0x233D67u;
static uint32_t C_SKY_A = 0x2A6FA4u;
static uint32_t C_SKY_B = 0xD8B2A0u;
static uint32_t C_WAVE_A = 0x4AAFAAu;
static uint32_t C_WAVE_B = 0x596FC6u;
static uint32_t C_WAVE_C = 0xE3A05Fu;
static uint32_t C_MENU = 0xEDF4F7u;
static uint32_t C_MENU_DARK = 0xD8E6EDu;
static uint32_t C_WINDOW = 0xF4F8FAu;
static uint32_t C_CONTENT = 0xE7F0F4u;
static uint32_t C_TEXT = 0x20394Au;
static uint32_t C_MUTED = 0x607887u;
static uint32_t C_ACCENT = 0x24BFA6u;
static uint32_t C_BLUE = 0x4B91E2u;
static uint32_t C_ORANGE = 0xF0A35Eu;
static uint32_t C_RED = 0xFF6259u;
static uint32_t C_YELLOW = 0xFFBD44u;
static uint32_t C_GREEN = 0x28C840u;
static uint32_t C_VIOLET = 0x9A83E8u;
static uint32_t C_BORDER = 0xC7D7E0u;
#define APP_COUNT   12u

enum { APP_FILES = 0, APP_NOTES, APP_TERMINAL, APP_SETTINGS, APP_MONITOR, APP_STORAGE,
       APP_DRIVERS, APP_NETWORK, APP_DISKS, APP_ABOUT_OS, APP_ABOUT_MINTDE, APP_LAUNCHPAD };

static MINT_DESKTOP_SETTINGS desktop_settings;
static MINT_FS_DIRENT fs_entries[MINT_RAMFS_MAX_FILES];
static uint32_t fs_entry_count;
static char note_buffer[512];
static size_t note_length;
static uint32_t notes_loaded;
static uint32_t window_focused = 1u;
static void paint_shell(void);
static void paint_cursor(void);
static void paint_menu_dropdown(void);
static void paint_settings_dropdown(void);
static void refresh_fat_files(void);
static void open_app(uint32_t app);
static void refresh_driver_catalog(void);
static const char *driver_state_label(uint32_t state);

static MINT_DISPLAY_INFO display;
static MINT_INPUT_INFO input_info;
static int32_t pointer_x, pointer_y;
static uint32_t selected_app = APP_FILES;
static uint32_t library_open;
static uint32_t app_window_open;
static int64_t last_clock_second = -1;
static int32_t cached_margin;
static int32_t active_menu = -1;
static uint32_t disk_selected_device;
static uint32_t disk_format_prompt;
static char disk_confirmation[ MINT_STORAGE_FORMAT_CONFIRM_LEN ];
static size_t disk_confirmation_length;
static char disk_result[112];
static uint32_t files_volume;
static char files_path[96];
static MINT_FAT16_DIRENT fat_entries[MINT_FAT16_LIST_MAX];
static uint32_t fat_entry_count;
static int32_t files_selected_entry = -1;
static uint32_t files_preview_active;
static char files_preview[512];
static size_t files_preview_length;
static int32_t cached_win_x, cached_win_y, cached_win_w, cached_win_h;
static int32_t window_offset_x, window_offset_y, drag_offset_x, drag_offset_y;
static uint32_t window_dragging;
static uint32_t window_resizing;
static int32_t resize_anchor_x, resize_anchor_y, resize_start_w, resize_start_h;
static int32_t custom_win_w, custom_win_h;
static uint32_t window_minimized;
static uint32_t window_maximized;
static uint32_t hover_setting_control;
static uint32_t keyboard_setting_focus;
static uint32_t settings_category = 1u; /* Appearance */
static uint32_t open_settings_dropdown;
static uint32_t settings_dropdown_cursor;
static uint32_t show_changelog; /* 0=hidden, 1=version list, 2=release notes */
static uint32_t changelog_version;
static int32_t hovered_menu_item = -1;
static uint32_t background_snapshot_ready;
static MINT_STORAGE_SNAPSHOT storage_snapshot;
static MINT_FAT16_VOLUME_LIST fat_volume_list;
/* Large syscall result buffers live in the static data segment, not on the
 * 16 KiB userspace stack. The terminal and Settings/Files views share one
 * cooperative desktop process, so these scratch areas are intentionally reused. */
static MINT_PCI_INVENTORY terminal_pci_inventory;
static MINT_VFS_MOUNT_TABLE terminal_mount_table;
static MINT_LOG_RECORD terminal_log_scratch[MINT_LOG_READ_MAX];
static MINT_FS_DIRENT terminal_ramfs_entries[MINT_RAMFS_MAX_FILES];
static MINT_DRIVER_CATALOG driver_catalog;
static MINT_E1000_INFO terminal_e1000_info;
static uint32_t driver_catalog_ok;
static uint32_t driver_page;
static MINT_LOG_RECORD storage_log_records[MINT_LOG_READ_MAX];
static uint32_t storage_log_count;
static uint32_t storage_view;
static uint32_t storage_page;
static uint32_t storage_diag_ok;
static char terminal_input[96];
static size_t terminal_input_length;
static char terminal_output[10][96];
static uint8_t terminal_output_kind[10]; /* 1 = entered command, 0 = command output */
static uint32_t terminal_output_count;
static uint32_t terminal_volume_ordinal;
static char terminal_current_path[96];

static size_t text_length(const char *s) {
    size_t n = 0; if (s) while (s[n] && n < 128u) ++n; return n;
}
static int draw_text(const char *s, int32_t x, int32_t y, uint32_t color, uint32_t scale) {
    size_t n = text_length(s);
    uint32_t configured = desktop_settings.ui_scale ? desktop_settings.ui_scale : 1u;
    /* Typography presets combine by level rather than multiplying sizes.
       Normal=1x, Large=1.25x, Extra large=1.5x; headings keep their hierarchy. */
    uint32_t actual = scale + configured - 1u;
    if (actual > 4u) actual = 4u;
    return mint_ui_draw_text(s, n, x, y, color, actual) == 0;
}
static int fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
    return mint_ui_fill_rect(x, y, w, h, c) == 0;
}
static void format_u64(char *out, size_t capacity, uint64_t value) {
    if (!out || capacity < 2u) return;
    char reverse[24]; size_t n = 0, k = 0;
    do { reverse[n++] = (char)('0' + value % 10u); value /= 10u; }
    while (value && n < sizeof(reverse));
    while (n && k + 1u < capacity) out[k++] = reverse[--n];
    out[k] = '\0';
}
static const char *storage_severity_name(uint32_t severity) {
    if (severity == MINT_LOG_DEBUG) return "DEBUG";
    if (severity == MINT_LOG_INFO) return "INFO";
    if (severity == MINT_LOG_WARN) return "WARN";
    return "ERROR";
}
static const char *storage_component_name(uint32_t component) {
    switch (component) {
        case MINT_LOG_COMPONENT_PCI: return "PCI";
        case MINT_LOG_COMPONENT_AHCI: return "AHCI";
        case MINT_LOG_COMPONENT_BLOCK: return "BLOCK";
        case MINT_LOG_COMPONENT_PARTITION: return "PARTITION";
        case MINT_LOG_COMPONENT_FAT16: return "FAT16";
        case MINT_LOG_COMPONENT_VFS: return "VFS";
        case MINT_LOG_COMPONENT_E1000: return "E1000";
        default: return "KERNEL";
    }
}
static const char *storage_capacity_source(uint32_t source) {
    if (source == MINT_STORAGE_CAPACITY_LBA48) return "LBA48";
    if (source == MINT_STORAGE_CAPACITY_LBA28) return "LBA28";
    return "unknown";
}
static const char *storage_controller_state(uint32_t state) {
    if (state == MINT_STORAGE_CTRL_READY) return "ready";
    if (state == MINT_STORAGE_CTRL_FAILED) return "failed";
    if (state == MINT_STORAGE_CTRL_DEVICE_LIMIT) return "device limit";
    return "not probed";
}
static void format_hex64(char *out, size_t capacity, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    if (!out || capacity < 4u) return;
    char reverse[16]; size_t n = 0, k = 0;
    do { reverse[n++] = digits[value & 15u]; value >>= 4; } while (value && n < sizeof(reverse));
    out[k++] = '0'; out[k++] = 'x';
    while (n && k + 1u < capacity) out[k++] = reverse[--n];
    out[k] = '\0';
}
static int32_t storage_control_width(uint32_t index) {
    static const int32_t widths[7] = {60, 78, 48, 50, 54, 62, 90};
    return index < 7u ? widths[index] : 0;
}
static int32_t storage_control_x(int32_t start, uint32_t index) {
    int32_t x = start;
    for (uint32_t i = 0; i < index && i < 7u; ++i) x += storage_control_width(i) + 8;
    return x;
}
static const char *storage_table_state(uint32_t state) {
    if (state == MINT_STORAGE_TABLE_READ_FAILED) return "read failed";
    if (state == MINT_STORAGE_TABLE_BAD_SIGNATURE) return "bad MBR signature";
    if (state == MINT_STORAGE_TABLE_PARTITIONS_FOUND) return "partitions found";
    if (state == MINT_STORAGE_TABLE_SUPERFLOPPY) return "superfloppy candidate";
    if (state == MINT_STORAGE_TABLE_UNSTABLE_READ) return "LBA0 reads differ";
    if (state == MINT_STORAGE_TABLE_NO_PARTITIONS) return "no partitions";
    return "not checked";
}
static const char *storage_partition_state(uint32_t state) {
    if (state == MINT_STORAGE_PART_INVALID_ENTRY) return "invalid MBR entry";
    if (state == MINT_STORAGE_PART_OUT_OF_RANGE) return "OUT OF RANGE";
    if (state == MINT_STORAGE_PART_BOOT_READ_FAILED) return "boot read failed";
    if (state == MINT_STORAGE_PART_BAD_SIGNATURE) return "bad boot signature";
    if (state == MINT_STORAGE_PART_INVALID_BPB) return "invalid FAT BPB";
    if (state == MINT_STORAGE_PART_NOT_FAT16_GEOMETRY) return "not FAT16 geometry";
    if (state == MINT_STORAGE_PART_MINTOS_LABEL_MISSING) return "MintOS label missing";
    if (state == MINT_STORAGE_PART_VALID_MINTOS_FAT16) return "MintOS26 FAT16 valid";
    if (state == MINT_STORAGE_PART_VALID_FAT16_OTHER_LABEL) return "other FAT16 label";
    return "not a supported FAT16 partition";
}
static void refresh_storage_data(void) {
    storage_diag_ok = mint_storage_diag_get(&storage_snapshot) == 0 &&
                      storage_snapshot.version == MINT_STORAGE_DIAG_VERSION &&
                      storage_snapshot.probe_complete != 0u;
    if (!storage_diag_ok) {
        uint8_t *p = (uint8_t *)&storage_snapshot;
        for (size_t i = 0; i < sizeof(storage_snapshot); ++i) p[i] = 0u;
    }
    uint32_t minimum = storage_view == 3u ? MINT_LOG_WARN :
                       storage_view == 4u ? MINT_LOG_ERROR : MINT_LOG_DEBUG;
    int64_t count = mint_log_read(storage_log_records, MINT_LOG_READ_MAX, minimum, 0u);
    storage_log_count = count > 0 ? (uint32_t)count : 0u;
    (void)mint_fat16_get_volumes(&fat_volume_list);
    if (fat_volume_list.count > MINT_FAT16_MAX_VOLUMES) fat_volume_list.count = MINT_FAT16_MAX_VOLUMES;
}
static void storage_serial_text(const char *text) {
    if (!text) return;
    size_t n = text_length(text);
    if (n) (void)mint_write(1, text, n);
}
static void export_storage_report(void) {
    refresh_storage_data();
    storage_serial_text("\r\n=== MintDE Storage & Logs export (read-only) ===\r\n");
    storage_serial_text(storage_snapshot.readonly_mount_active ? "FAT16: read-only mount validated; persistence writes remain disabled\r\n" : "FAT16 mount: not active; persistent writes remain disabled in 26.5.008.0\r\n");
    char number[24];
    format_u64(number, sizeof(number), storage_snapshot.controller_count);
    storage_serial_text("Controllers: "); storage_serial_text(number); storage_serial_text("\r\n");
    format_u64(number, sizeof(number), storage_snapshot.device_count);
    storage_serial_text("Devices: "); storage_serial_text(number); storage_serial_text("\r\n");
    for (uint32_t i = 0; i < storage_snapshot.controller_count && i < MINT_STORAGE_MAX_CONTROLLERS; ++i) {
        MINT_STORAGE_CONTROLLER_INFO *c = &storage_snapshot.controllers[i];
        storage_serial_text("[controller] BDF=");
        format_u64(number, sizeof(number), c->bus); storage_serial_text(number); storage_serial_text(":");
        format_u64(number, sizeof(number), c->device); storage_serial_text(number); storage_serial_text(".");
        format_u64(number, sizeof(number), c->function); storage_serial_text(number);
        storage_serial_text(" VID:DID="); format_hex64(number, sizeof(number), c->vendor_id); storage_serial_text(number);
        storage_serial_text(":"); format_hex64(number, sizeof(number), c->device_id); storage_serial_text(number);
        storage_serial_text(" BAR5="); format_hex64(number, sizeof(number), c->abar); storage_serial_text(number);
        storage_serial_text(" PI="); format_hex64(number, sizeof(number), c->port_implemented); storage_serial_text(number);
        storage_serial_text(" state="); storage_serial_text(storage_controller_state(c->state));
        storage_serial_text(" reason="); storage_serial_text(c->reason); storage_serial_text("\r\n");
    }
    for (uint32_t i = 0; i < storage_snapshot.device_count && i < MINT_STORAGE_MAX_DEVICES; ++i) {
        MINT_STORAGE_DEVICE_INFO *d = &storage_snapshot.devices[i];
        storage_serial_text("[device] "); storage_serial_text(d->name);
        storage_serial_text(" model="); storage_serial_text(d->model);
        storage_serial_text(" serial="); storage_serial_text(d->serial);
        storage_serial_text(" controller_index="); format_u64(number, sizeof(number), d->controller_index); storage_serial_text(number);
        storage_serial_text(" port="); format_u64(number, sizeof(number), d->port); storage_serial_text(number);
        storage_serial_text(" sectors="); format_u64(number, sizeof(number), d->sector_count); storage_serial_text(number);
        storage_serial_text(" bytes="); format_u64(number, sizeof(number), d->capacity_bytes); storage_serial_text(number);
        storage_serial_text(" capacity_source="); storage_serial_text(storage_capacity_source(d->capacity_source));
        storage_serial_text(" lba28_raw="); format_u64(number, sizeof(number), d->reported_lba28_sectors); storage_serial_text(number);
        storage_serial_text(" lba48_raw="); format_u64(number, sizeof(number), d->reported_lba48_sectors); storage_serial_text(number);
        storage_serial_text(" lba48_supported="); storage_serial_text(d->lba48_supported ? "yes" : "no");
        char sig_a[5], sig_b[5], crc_text[24], disk_id_text[24];
        format_hex64(crc_text, sizeof(crc_text), d->lba0_crc32);
        format_hex64(disk_id_text, sizeof(disk_id_text), d->mbr_disk_id);
        sig_a[0] = sig_b[0] = '0'; sig_a[1] = sig_b[1] = 'x';
        static const char hex_digits[] = "0123456789ABCDEF";
        sig_a[2] = hex_digits[d->lba0_signature[0] >> 4]; sig_a[3] = hex_digits[d->lba0_signature[0] & 15u]; sig_a[4] = '\0';
        sig_b[2] = hex_digits[d->lba0_signature[1] >> 4]; sig_b[3] = hex_digits[d->lba0_signature[1] & 15u]; sig_b[4] = '\0';
        storage_serial_text("\r\n  LBA0 signature="); storage_serial_text(sig_a); storage_serial_text(" "); storage_serial_text(sig_b);
        storage_serial_text(" CRC32="); storage_serial_text(crc_text);
        storage_serial_text(" repeat=");
        storage_serial_text(d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_MATCH ? "MATCH" :
                            d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_DIFFERS ? "DIFFERS" :
                            d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_FAILED ? "FAILED" : "UNKNOWN");
        storage_serial_text(" MBR-disk-id="); storage_serial_text(disk_id_text);
        storage_serial_text(" developer-marker="); storage_serial_text(d->developer_disk_id_match ? "MATCH" : "MISMATCH/UNKNOWN");
        storage_serial_text("\r\n  read: "); storage_serial_text(d->read_status);
        storage_serial_text("\r\n  table: "); storage_serial_text(storage_table_state(d->partition_table_state));
        storage_serial_text("\r\n  filesystem: "); storage_serial_text(d->filesystem_status);
        for (uint32_t p = 0; p < d->partition_count && p < MINT_STORAGE_MAX_PARTITIONS; ++p) {
            MINT_STORAGE_PARTITION_INFO *part = &d->partitions[p];
            storage_serial_text("\r\n  [partition "); format_u64(number, sizeof(number), part->table_index); storage_serial_text(number);
            storage_serial_text("] type="); format_hex64(number, sizeof(number), part->type); storage_serial_text(number);
            storage_serial_text(" bootable="); storage_serial_text(part->bootable ? "yes" : "no");
            storage_serial_text(" start_lba="); format_u64(number, sizeof(number), part->start_lba); storage_serial_text(number);
            storage_serial_text(" sectors="); format_u64(number, sizeof(number), part->sector_count); storage_serial_text(number);
            storage_serial_text(" state="); storage_serial_text(storage_partition_state(part->state));
            storage_serial_text(" reason="); storage_serial_text(part->reason);
        }
        storage_serial_text("\r\n  mount: "); storage_serial_text(d->mount_status);
        storage_serial_text("\r\n  write: "); storage_serial_text(d->write_status);
        storage_serial_text("\r\n");
    }
    storage_serial_text("Recent filtered log records: ");
    format_u64(number, sizeof(number), storage_log_count); storage_serial_text(number);
    storage_serial_text("\r\n");
    for (uint32_t i = 0; i < storage_log_count; ++i) {
        MINT_LOG_RECORD *r = &storage_log_records[i];
        storage_serial_text("[#"); format_u64(number, sizeof(number), r->sequence);
        storage_serial_text(number); storage_serial_text("][");
        storage_serial_text(storage_severity_name(r->severity)); storage_serial_text("][");
        storage_serial_text(storage_component_name(r->component)); storage_serial_text("] ");
        storage_serial_text(r->message); storage_serial_text("\r\n");
    }
    storage_serial_text("=== end export ===\r\n");
}

static uint32_t blend(uint32_t a, uint32_t b, uint32_t t, uint32_t den) {
    uint32_t ar=(a>>16)&255u, ag=(a>>8)&255u, ab=a&255u;
    uint32_t br=(b>>16)&255u, bg=(b>>8)&255u, bb=b&255u;
    uint32_t r=(ar*(den-t)+br*t)/den, g=(ag*(den-t)+bg*t)/den, bl=(ab*(den-t)+bb*t)/den;
    return (r<<16)|(g<<8)|bl;
}
static int32_t integer_sqrt(int32_t value) {
    int32_t x=0;
    while ((x+1)>0 && (x+1)*(x+1)<=value) ++x;
    return x;
}
static void round_box(int32_t x, int32_t y, int32_t w, int32_t h,
                      int32_t radius, uint32_t color) {
    if (w<=0 || h<=0) return;
    if (radius<1) { (void)fill(x,y,w,h,color); return; }
    if (radius>18) radius=18;
    if (radius*2>w) radius=w/2;
    if (radius*2>h) radius=h/2;
    if (radius<1) { (void)fill(x,y,w,h,color); return; }
    int32_t r2=radius*radius;
    for (int32_t row=0;row<radius;row++) {
        int32_t dy=radius-row-1;
        int32_t inside=integer_sqrt(r2-dy*dy);
        int32_t inset=radius-inside;
        if (inset<0) inset=0;
        if (w-2*inset>0) {
            (void)fill(x+inset,y+row,w-2*inset,1,color);
            (void)fill(x+inset,y+h-row-1,w-2*inset,1,color);
        }
    }
    (void)fill(x,y+radius,w,h-2*radius,color);
}
static void glass_box(int32_t x, int32_t y, int32_t w, int32_t h,
                      int32_t radius, uint32_t color, uint32_t alpha) {
    if (mint_ui_blend_round_rect(x,y,w,h,color,alpha,(uint32_t)(radius<0?0:radius)) != 0) {
        /* Older display-service builds retain a readable opaque fallback. */
        round_box(x,y,w,h,radius,color);
        return;
    }
    /* A thin specular edge helps translucent materials read as glass without
       resorting to hard polygon corners or opaque outlines. */
    if(w>radius*2+4 && h>4) {
        (void)mint_ui_blend_round_rect(x+radius,y+1,w-radius*2,1,0xFFFFFFu,38u,0u);
        (void)mint_ui_blend_round_rect(x+radius,y+h-2,w-radius*2,1,0x8ABDDCu,20u,0u);
    }
}
static void line(int32_t x, int32_t y, int32_t w, uint32_t color) {
    (void)fill(x, y, w, 1, color);
}

static const char *const app_names[APP_COUNT] = {
    "Files", "Notes", "Terminal", "Settings", "System Monitor", "Storage & Logs",
    "Hardware & Drivers", "Network", "Disk Utility", "About MintOS", "About MintDE", "Launchpad"
};
static uint32_t app_colors[APP_COUNT] = {
    0x4B91E2u, 0x9A83E8u, 0x24BFA6u, 0xF0A35Eu,
    0x28C840u, 0x487EBBu, 0x2DAA92u, 0x4A8BD4u, 0x8C9FB5u, 0x24BFA6u, 0x4B91E2u, 0x24BFA6u
};

static void apply_palette(void) {
    if (desktop_settings.theme == MINT_THEME_DARK) {
        C_WALL=0x0B1420u; C_SKY_A=0x152C43u; C_SKY_B=0x36495Bu;
        C_WAVE_A=0x267F83u; C_WAVE_B=0x364F9Au; C_WAVE_C=0x9A6840u;
        C_MENU=0x172838u; C_MENU_DARK=0x253A4Cu; C_WINDOW=0x1C2E40u;
        C_CONTENT=0x23394Cu; C_TEXT=0xE7F0F6u; C_MUTED=0xA9BBC8u;
        C_BORDER=0x40576Au;
    } else if (desktop_settings.theme == MINT_THEME_WARM) {
        C_WALL=0x30313Bu; C_SKY_A=0x5A7192u; C_SKY_B=0xEDC5A5u;
        C_WAVE_A=0x55B5A5u; C_WAVE_B=0x8575C5u; C_WAVE_C=0xE19A5Au;
        C_MENU=0xF7EEE6u; C_MENU_DARK=0xEAD8CAu; C_WINDOW=0xFFF9F4u;
        C_CONTENT=0xF4E7DDu; C_TEXT=0x3E3030u; C_MUTED=0x806C67u;
        C_BORDER=0xDCC7B9u;
    } else {
        C_WALL=0x233D67u; C_SKY_A=0x2A6FA4u; C_SKY_B=0xD8B2A0u;
        C_WAVE_A=0x4AAFAAu; C_WAVE_B=0x596FC6u; C_WAVE_C=0xE3A05Fu;
        C_MENU=0xEDF4F7u; C_MENU_DARK=0xD8E6EDu; C_WINDOW=0xF4F8FAu;
        C_CONTENT=0xE7F0F4u; C_TEXT=0x20394Au; C_MUTED=0x607887u;
        C_BORDER=0xC7D7E0u;
    }
    static const uint32_t accents[5] = {0x24BFA6u,0x4B91E2u,0x9A83E8u,0xF0A35Eu,0xE76584u};
    C_ACCENT=accents[desktop_settings.accent <= MINT_ACCENT_ROSE ? desktop_settings.accent : 0u];
    if (desktop_settings.wallpaper == 1u) {
        C_SKY_A = desktop_settings.theme == MINT_THEME_DARK ? 0x102D3Bu : 0x4B90B8u;
        C_SKY_B = desktop_settings.theme == MINT_THEME_DARK ? 0x244B59u : 0xC7E2E8u;
        C_WAVE_A = 0x279C9Au; C_WAVE_B = 0x427FB5u; C_WAVE_C = 0xE0B36Du;
    } else if (desktop_settings.wallpaper == 2u) {
        C_SKY_A = 0x66568Fu; C_SKY_B = 0xE6B4A4u;
        C_WAVE_A = 0x63B7A6u; C_WAVE_B = 0x8269B5u; C_WAVE_C = 0xE58D6Cu;
    }
    app_colors[0]=C_BLUE; app_colors[1]=C_VIOLET; app_colors[2]=C_ACCENT;
    app_colors[3]=C_ORANGE; app_colors[4]=C_GREEN; app_colors[5]=0x487EBBu;
    app_colors[9]=C_ACCENT; app_colors[10]=C_BLUE; app_colors[11]=C_VIOLET;
}

static void refresh_files(void) {
    int64_t count = mint_fs_list(fs_entries, MINT_RAMFS_MAX_FILES);
    fs_entry_count = count > 0 ? (uint32_t)count : 0u;
}

static size_t copy_text(char *out, size_t capacity, const char *in) {
    size_t n=0; if (!out || !capacity) return 0;
    while (in && in[n] && n+1u<capacity) { out[n]=in[n]; ++n; }
    out[n]='\0'; return n;
}
static int path_append(char *out, size_t capacity, const char *base, const char *leaf) {
    size_t n=copy_text(out,capacity,base);
    size_t original=n;
    if (!out || !capacity || !leaf) return 0;
    if (n && n+1u<capacity) out[n++]='/';
    size_t i=0;
    while (leaf[i] && n+1u<capacity) out[n++]=leaf[i++];
    out[n]='\0';
    return i==text_length(leaf) && n>original;
}
static void terminal_push(const char *text) {
    if (terminal_output_count >= 10u) {
        for (uint32_t i=1u;i<10u;i++) {
            (void)copy_text(terminal_output[i-1u],sizeof(terminal_output[i-1u]),terminal_output[i]);
            terminal_output_kind[i-1u]=terminal_output_kind[i];
        }
        terminal_output_count=9u;
    }
    (void)copy_text(terminal_output[terminal_output_count],sizeof(terminal_output[terminal_output_count]),text?text:"");
    terminal_output_kind[terminal_output_count]=0u;
    ++terminal_output_count;
}
static void terminal_push_command(const char *text) {
    if (terminal_output_count >= 10u) {
        for (uint32_t i=1u;i<10u;i++) {
            (void)copy_text(terminal_output[i-1u],sizeof(terminal_output[i-1u]),terminal_output[i]);
            terminal_output_kind[i-1u]=terminal_output_kind[i];
        }
        terminal_output_count=9u;
    }
    (void)copy_text(terminal_output[terminal_output_count],sizeof(terminal_output[terminal_output_count]),text?text:"");
    terminal_output_kind[terminal_output_count]=1u;
    ++terminal_output_count;
}
static void terminal_append(char *dst,size_t capacity,const char *src) {
    if (!dst || !capacity || !src) return;
    size_t n=text_length(dst), i=0;
    while(src[i] && n+1u<capacity) dst[n++]=src[i++];
    dst[n]='\0';
}
static int terminal_equals(const char *a,const char *b) {
    size_t i=0;
    while(a && b && a[i] && b[i]) {
        char ca=a[i], cb=b[i];
        if(ca>='A'&&ca<='Z')ca=(char)(ca-'A'+'a');
        if(cb>='A'&&cb<='Z')cb=(char)(cb-'A'+'a');
        if(ca!=cb)return 0; ++i;
    }
    return a && b && a[i]=='\0' && b[i]=='\0';
}
static int terminal_block_is_mounted(uint32_t block_index) {
    (void)mint_fat16_get_volumes(&fat_volume_list);
    for(uint32_t i=0;i<fat_volume_list.count && i<MINT_FAT16_MAX_VOLUMES;i++)
        if(fat_volume_list.block_indices[i]==block_index)return 1;
    return 0;
}
static int terminal_starts_with(const char *text,const char *prefix) {
    size_t i=0; if(!text||!prefix)return 0;
    while(prefix[i]){char a=text[i],b=prefix[i];if(!a)return 0;
        if(a>='A'&&a<='Z')a=(char)(a-'A'+'a');if(b>='A'&&b<='Z')b=(char)(b-'A'+'a');
        if(a!=b)return 0;i++;}return 1;
}
static void terminal_reset_path(void) { terminal_current_path[0]='\0'; }
static int terminal_select_named_volume(const char *name) {
    /* Do not allocate MINT_STORAGE_SNAPSHOT (~9 KiB) on the four-page user
     * stack. The old local caused a stack guard page fault at `use sda/sdb`. */
    if(!name||mint_fat16_get_volumes(&fat_volume_list)<0||
       mint_storage_diag_get(&storage_snapshot)<0)return 0;
    for(uint32_t vi=0;vi<fat_volume_list.count&&vi<MINT_FAT16_MAX_VOLUMES;vi++) {
        uint32_t block=fat_volume_list.block_indices[vi];
        for(uint32_t di=0;di<storage_snapshot.device_count&&di<MINT_STORAGE_MAX_DEVICES;di++) {
            if(storage_snapshot.devices[di].block_registered&&storage_snapshot.devices[di].block_index==block&&
               terminal_equals(storage_snapshot.devices[di].name,name)) {
                if(mint_fat16_select_volume(vi)!=0)return 0;
                terminal_volume_ordinal=vi;terminal_reset_path();return 1;
            }
        }
    }
    return 0;
}
static int terminal_fat_path(char *out,size_t capacity,const char *leaf) {
    if(!out||!capacity||!leaf)return 0;
    if(!terminal_current_path[0])return copy_text(out,capacity,leaf)==text_length(leaf);
    return path_append(out,capacity,terminal_current_path,leaf);
}
static void terminal_path_parent(void) {
    size_t n=text_length(terminal_current_path);
    while(n&&terminal_current_path[n-1u]!='/')--n;
    if(n)terminal_current_path[n-1u]='\0';else terminal_current_path[0]='\0';
}
static void terminal_run_command(void) {
    char cmd[96]; (void)copy_text(cmd,sizeof(cmd),terminal_input);
    if(!cmd[0]) return;
    static const char prompt_text[]="root#mintos > ";
    (void)mint_write(1,"[MintDE terminal] ",18u);
    (void)mint_write(1,prompt_text,sizeof(prompt_text)-1u);
    (void)mint_write(1,cmd,text_length(cmd)); (void)mint_write(1,"\r\n",2u);
    terminal_push_command(cmd);
    if(terminal_equals(cmd,"clear")) { terminal_output_count=0u; return; }
    if(terminal_equals(cmd,"help")) {
        terminal_push("Commands: help, clear, ver, uptime, status, cpu");
        terminal_push("pci, resources, drivers, mounts, io, logs, net");
        terminal_push("drives, volumes, use sda/sdb, net start/status/test, ls, cd, pwd, cat");
        terminal_push("use sda | ls [name] | cd <dir> | pwd | cat <file>"); return;
    }
    if(terminal_equals(cmd,"ver")) { terminal_push("MintOS26 26.5.008.0 / MintDE 1.0.7"); return; }
    if(terminal_equals(cmd,"net start")) {
        int64_t rc = mint_e1000_start();
        (void)mint_e1000_get_status(&terminal_e1000_info);
        if(rc == 0) terminal_push("E1000 started; bounded DMA rings active; interrupts masked");
        else if(rc == -MINT_ENODEV) terminal_push("E1000 not detected; no DMA was enabled");
        else terminal_push("E1000 initialization failed; see COM1 logs; device remains unavailable");
        return;
    }
    if(terminal_equals(cmd,"net") || terminal_equals(cmd,"net status")) {
        if(mint_e1000_get_status(&terminal_e1000_info)<0){terminal_push("E1000 status service unavailable");return;}
        if(terminal_e1000_info.state==MINT_E1000_ACTIVE) terminal_push("VirtualBox E1000: ACTIVE; polling RX/TX rings; no TCP/IP stack");
        else if(terminal_e1000_info.state==MINT_E1000_FOUND) terminal_push("VirtualBox E1000: detected but not initialized");
        else if(terminal_e1000_info.state==MINT_E1000_FAILED) terminal_push("VirtualBox E1000: initialization failed; see COM1 logs");
        else terminal_push("VirtualBox E1000: not detected");
        char row[96]="",num[24],hex[24];
        terminal_append(row,sizeof(row),"PCI ");format_u64(num,sizeof(num),terminal_e1000_info.bus);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),":");format_u64(num,sizeof(num),terminal_e1000_info.device);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),".");format_u64(num,sizeof(num),terminal_e1000_info.function);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row)," MMIO ");format_hex64(hex,sizeof(hex),terminal_e1000_info.mmio_base);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"TX submitted/completed/errors: ");format_u64(num,sizeof(num),terminal_e1000_info.tx_submitted);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),terminal_e1000_info.tx_completed);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),terminal_e1000_info.tx_errors);terminal_append(row,sizeof(row),num);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RX received/dropped/errors: ");format_u64(num,sizeof(num),terminal_e1000_info.rx_received);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),terminal_e1000_info.rx_dropped);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),terminal_e1000_info.rx_errors);terminal_append(row,sizeof(row),num);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"Loopback stage/error: ");format_u64(num,sizeof(num),terminal_e1000_info.loopback_stage);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),terminal_e1000_info.last_error);terminal_append(row,sizeof(row),num);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"CTRL/RCTL/TCTL: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_ctrl);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rctl);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tctl);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"TDH/TDT/TXD: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tdh);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tdt);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tx_desc_status);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"TX index/cmd/len: ");format_u64(num,sizeof(num),terminal_e1000_info.diag_tx_index);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tx_command);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tx_length);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"TX buffer physical: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tx_buffer_address);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"TX ring BAH/BAL/LEN: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tdbah);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tdbal);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tdlen);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"PCI command: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_pci_command);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RDH/RDT/RXD: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rdh);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rdt);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_desc_status);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RX index/status/len/errors: ");format_u64(num,sizeof(num),terminal_e1000_info.diag_rx_index);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_desc_status);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_length);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_errors);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RX buffer physical: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_buffer_address);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RX index/status/len/errors: ");format_u64(num,sizeof(num),terminal_e1000_info.diag_rx_index);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_desc_status);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_length);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_errors);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RX buffer physical: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_buffer_address);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"MDIC/PHYCTRL/PHYSTS: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_mdic);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_phy_control);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_phy_status);terminal_append(row,sizeof(row),hex);terminal_push(row);
        terminal_push("No TCP/IP, DHCP, DNS, or external packet send service yet");return;
    }
    if(terminal_equals(cmd,"net test")) {
        (void)mint_e1000_get_status(&terminal_e1000_info);
        if(terminal_e1000_info.state!=MINT_E1000_ACTIVE) {
            terminal_push("E1000 is idle; run net start before the loopback test"); return;
        }
        int64_t rc=mint_e1000_run_loopback_test();
        (void)mint_e1000_get_status(&terminal_e1000_info);
        if(rc==0) terminal_push("VirtualBox E1000 internal loopback: PASS");
        else {
            static const char *const phase[9]={"none","reserved legacy stage 1","reserved legacy stage 2","TX descriptor timed out","RX descriptor timed out","RX descriptor invalid","frame mismatch","reserved legacy stage 7","CTRL.LRST stayed asserted"};
            uint32_t stage=terminal_e1000_info.loopback_stage;
            terminal_push(stage<9u?phase[stage]:"Loopback failed; inspect E1000 register snapshot in COM1");
        }
        char diag[88]="Loopback diag stage/error: ",num[24],hex[24];
        format_u64(num,sizeof(num),terminal_e1000_info.loopback_stage);terminal_append(diag,sizeof(diag),num);
        terminal_append(diag,sizeof(diag),"/");format_u64(num,sizeof(num),terminal_e1000_info.last_error);terminal_append(diag,sizeof(diag),num);terminal_push(diag);
        char row[128]="Loopback tests/pass/fail: ";format_u64(num,sizeof(num),terminal_e1000_info.loopback_tests);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),terminal_e1000_info.loopback_passed);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),terminal_e1000_info.loopback_failed);terminal_append(row,sizeof(row),num);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"CTRL/RCTL/TCTL: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_ctrl);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rctl);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tctl);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"TX index/cmd/len/status: ");format_u64(num,sizeof(num),terminal_e1000_info.diag_tx_index);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tx_command);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tx_length);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_tx_desc_status);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RX index/status/len/errors: ");format_u64(num,sizeof(num),terminal_e1000_info.diag_rx_index);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_desc_status);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_length);terminal_append(row,sizeof(row),hex);
        terminal_append(row,sizeof(row),"/");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_errors);terminal_append(row,sizeof(row),hex);terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"RX buffer physical: ");format_hex64(hex,sizeof(hex),terminal_e1000_info.diag_rx_buffer_address);terminal_append(row,sizeof(row),hex);terminal_push(row);return;
    }
    if(terminal_equals(cmd,"uptime")) {
        mint_timespec64 now={0,0}; char number[24], linebuf[48]="Uptime: ";
        if(mint_clock_gettime(MINT_CLOCK_MONOTONIC,&now)!=0){terminal_push("Monotonic clock unavailable");return;}
        format_u64(number,sizeof(number),(uint64_t)now.tv_sec); terminal_append(linebuf,sizeof(linebuf),number); terminal_append(linebuf,sizeof(linebuf)," seconds"); terminal_push(linebuf); return;
    }
    if(terminal_equals(cmd,"status")) {
        refresh_storage_data();
        terminal_push("Kernel services: ring-3 syscall ABI active");
        terminal_push(storage_snapshot.readonly_mount_active?"Filesystem: FAT16 mounted read-only":"Filesystem: RAM session only");
        terminal_push("Persistent file writes: disabled"); return;
    }
    if(terminal_equals(cmd,"cpu")) {
        terminal_push("Architecture: x86-64; GDT/TSS and IDT active");
        terminal_push("Timer: PIT near 100 Hz; cooperative scheduler active");
        terminal_push("CPUID feature and per-core topology API not yet exported"); return;
    }
    if(terminal_equals(cmd,"pci")) {
        if(mint_pci_inventory_get(&terminal_pci_inventory)<0){terminal_push("PCI inventory unavailable");return;}
        char row[96],num[24],hex[24];
        for(uint32_t i=0;i<terminal_pci_inventory.devices_stored&&i<MINT_PCI_MAX_DEVICES&&i<12u;i++) {
            const MINT_PCI_DEVICE *d=&terminal_pci_inventory.devices[i]; row[0]='\0';
            format_u64(num,sizeof(num),d->bus);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row),":");
            format_u64(num,sizeof(num),d->device);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row),".");
            format_u64(num,sizeof(num),d->function);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row)," ");
            format_hex64(hex,sizeof(hex),d->vendor_id);terminal_append(row,sizeof(row),hex);terminal_append(row,sizeof(row),":");
            format_hex64(hex,sizeof(hex),d->device_id);terminal_append(row,sizeof(row),hex);terminal_append(row,sizeof(row)," class ");
            format_hex64(hex,sizeof(hex),d->class_code);terminal_append(row,sizeof(row),hex);terminal_append(row,sizeof(row),":");
            format_hex64(hex,sizeof(hex),d->subclass);terminal_append(row,sizeof(row),hex);terminal_push(row);
        }
        return;
    }
    if(terminal_equals(cmd,"resources")) {
        if(mint_pci_inventory_get(&terminal_pci_inventory)<0){terminal_push("PCI resource inventory unavailable");return;}
        for(uint32_t i=0;i<terminal_pci_inventory.devices_stored&&i<MINT_PCI_MAX_DEVICES;i++) {
            const MINT_PCI_DEVICE *d=&terminal_pci_inventory.devices[i];
            char row[96]="",num[24],hex[24];
            format_u64(num,sizeof(num),d->bus);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row),":");
            format_u64(num,sizeof(num),d->device);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row),".");
            format_u64(num,sizeof(num),d->function);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row)," ");
            format_hex64(hex,sizeof(hex),d->vendor_id);terminal_append(row,sizeof(row),hex);terminal_append(row,sizeof(row),":");
            format_hex64(hex,sizeof(hex),d->device_id);terminal_append(row,sizeof(row),hex);
            terminal_append(row,sizeof(row)," IRQ ");format_u64(num,sizeof(num),d->interrupt_line);terminal_append(row,sizeof(row),num);
            terminal_append(row,sizeof(row)," pin ");format_u64(num,sizeof(num),d->interrupt_pin);terminal_append(row,sizeof(row),num);
            terminal_push(row);
            for(uint32_t b=0;b<d->bar_slots&&b<MINT_PCI_BAR_SLOTS;b++) {
                const MINT_PCI_BAR *bar=&d->bars[b];
                if(bar->kind==MINT_PCI_BAR_UNASSIGNED||bar->kind==MINT_PCI_BAR_ABSENT||bar->kind==MINT_PCI_BAR_MEMORY64_UPPER)continue;
                row[0]='\0';terminal_append(row,sizeof(row),"  BAR");format_u64(num,sizeof(num),b);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row)," ");
                switch(bar->kind) {case MINT_PCI_BAR_IO:terminal_append(row,sizeof(row),"I/O ");break;
                    case MINT_PCI_BAR_MEMORY32:terminal_append(row,sizeof(row),"MEM32 ");break;
                    case MINT_PCI_BAR_MEMORY20:terminal_append(row,sizeof(row),"MEM20 ");break;
                    case MINT_PCI_BAR_MEMORY64:terminal_append(row,sizeof(row),"MEM64 ");break;
                    default:terminal_append(row,sizeof(row),"RESERVED ");break;}
                format_hex64(hex,sizeof(hex),bar->address);terminal_append(row,sizeof(row),hex);
                if(bar->flags&8u)terminal_append(row,sizeof(row)," prefetch");
                terminal_push(row);
            }
        }
        terminal_push("Read-only PCI resource decode; BARs were not resized or reprogrammed");return;
    }
    if(terminal_equals(cmd,"drivers")) {
        if(mint_driver_catalog_get(&driver_catalog)<0){terminal_push("Driver catalogue unavailable");return;}
        for(uint32_t i=0;i<driver_catalog.count&&i<MINT_DRIVER_CATALOG_MAX_RECORDS;i++) {
            char row[96]="";terminal_append(row,sizeof(row),driver_catalog.records[i].name);
            terminal_append(row,sizeof(row)," - ");terminal_append(row,sizeof(row),driver_state_label(driver_catalog.records[i].state));terminal_push(row);
        }
        terminal_push("FOUND/DIAG ONLY does not mean a full functional driver"); return;
    }
    if(terminal_equals(cmd,"mounts")) {
        if(mint_vfs_mount_table_get(&terminal_mount_table)<0){terminal_push("VFS mount table unavailable");return;}
        for(uint32_t i=0;i<terminal_mount_table.count&&i<MINT_VFS_MAX_MOUNTS;i++) {
            char row[96]="";terminal_append(row,sizeof(row),terminal_mount_table.mounts[i].path);terminal_append(row,sizeof(row),"  ");
            terminal_append(row,sizeof(row),terminal_mount_table.mounts[i].filesystem);terminal_append(row,sizeof(row)," from ");
            terminal_append(row,sizeof(row),terminal_mount_table.mounts[i].source);
            terminal_append(row,sizeof(row),(terminal_mount_table.mounts[i].flags&MINT_VFS_FLAG_READ_ONLY)?" [read-only]":
                (terminal_mount_table.mounts[i].flags&MINT_VFS_FLAG_VOLATILE)?" [volatile]":"");terminal_push(row);
        }
        if(terminal_mount_table.truncated)terminal_push("Mount table truncated by configured limit");return;
    }
    if(terminal_equals(cmd,"io")) {
        refresh_storage_data();
        for(uint32_t i=0;i<storage_snapshot.device_count&&i<MINT_STORAGE_MAX_DEVICES;i++) {
            MINT_STORAGE_DEVICE_INFO *d=&storage_snapshot.devices[i]; MINT_BLOCK_IO_STATS stats;
            if(!d->block_registered||mint_block_io_stats_get(d->block_index,&stats)<0)continue;
            char row[96]="",num[24];terminal_append(row,sizeof(row),d->name);terminal_append(row,sizeof(row)," reads ");
            format_u64(num,sizeof(num),stats.read_success);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row),"/");
            format_u64(num,sizeof(num),stats.read_calls);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row)," failed ");
            format_u64(num,sizeof(num),stats.read_failures);terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row)," generic writes blocked ");
            format_u64(num,sizeof(num),stats.writes_blocked);terminal_append(row,sizeof(row),num);terminal_push(row);
        }
        terminal_push("Block counters cover generic I/O; Disk Utility writes are logged separately");return;
    }
    if(terminal_equals(cmd,"logs")) {
        int64_t count=mint_log_read(terminal_log_scratch,5u,MINT_LOG_INFO,0u);
        if(count<=0){terminal_push("No recent INFO+ records");return;}
        for(int64_t i=0;i<count;i++){char row[96]="",num[24];format_u64(num,sizeof(num),terminal_log_scratch[i].sequence);
            terminal_append(row,sizeof(row),"#");terminal_append(row,sizeof(row),num);terminal_append(row,sizeof(row)," ");
            terminal_append(row,sizeof(row),terminal_log_scratch[i].message);terminal_push(row);}return;
    }
    if(terminal_equals(cmd,"drives")) {
        refresh_storage_data();
        if(!storage_snapshot.device_count){terminal_push("No registered block devices");return;}
        for(uint32_t i=0;i<storage_snapshot.device_count && i<MINT_STORAGE_MAX_DEVICES && i<9u;i++) {
            MINT_STORAGE_DEVICE_INFO *d=&storage_snapshot.devices[i];
            char linebuf[96]=""; terminal_append(linebuf,sizeof(linebuf),d->name); terminal_append(linebuf,sizeof(linebuf)," ");
            terminal_append(linebuf,sizeof(linebuf),d->model[0]?d->model:"Unknown model"); terminal_append(linebuf,sizeof(linebuf)," ");
            char size[24]; format_u64(size,sizeof(size),d->capacity_bytes/(1024u*1024u)); terminal_append(linebuf,sizeof(linebuf),size); terminal_append(linebuf,sizeof(linebuf)," MiB");
            terminal_append(linebuf,sizeof(linebuf),terminal_block_is_mounted(d->block_index)?" [mounted RO]":" [not mounted]");
            terminal_push(linebuf);
        }
        return;
    }
    if(terminal_equals(cmd,"volumes")) {
        (void)mint_fat16_get_volumes(&fat_volume_list);
        char linebuf[96]="Mounted read-only volumes: ", number[24]; format_u64(number,sizeof(number),fat_volume_list.count);
        terminal_append(linebuf,sizeof(linebuf),number); terminal_push(linebuf);
        for(uint32_t v=0;v<fat_volume_list.count && v<MINT_FAT16_MAX_VOLUMES;v++) {
            char row[72]="Volume "; format_u64(number,sizeof(number),v+1u); terminal_append(row,sizeof(row),number); terminal_append(row,sizeof(row)," -> ");
            for(uint32_t d=0;d<storage_snapshot.device_count && d<MINT_STORAGE_MAX_DEVICES;d++) if(storage_snapshot.devices[d].block_registered && storage_snapshot.devices[d].block_index==fat_volume_list.block_indices[v]) terminal_append(row,sizeof(row),storage_snapshot.devices[d].name);
            terminal_push(row);
        }
        return;
    }
    if(terminal_starts_with(cmd,"use ")) {
        if(terminal_select_named_volume(cmd+4)) { char row[80]="Selected ";terminal_append(row,sizeof(row),cmd+4);terminal_append(row,sizeof(row)," (read-only)");terminal_push(row); }
        else terminal_push("No mounted volume matches that device; use 'volumes' or 'mounts'");
        return;
    }
    if(terminal_equals(cmd,"pwd")) {
        char row[96]="/session";
        (void)mint_fat16_get_volumes(&fat_volume_list);
        if(fat_volume_list.count && terminal_volume_ordinal<fat_volume_list.count) {
            (void)mint_fat16_select_volume(terminal_volume_ordinal);
            (void)copy_text(row,sizeof(row),"/Volumes/");
            if(mint_storage_diag_get(&storage_snapshot)>=0) {
                uint32_t block=fat_volume_list.block_indices[terminal_volume_ordinal];
                for(uint32_t d=0;d<storage_snapshot.device_count&&d<MINT_STORAGE_MAX_DEVICES;d++) if(storage_snapshot.devices[d].block_registered&&storage_snapshot.devices[d].block_index==block) {
                    terminal_append(row,sizeof(row),storage_snapshot.devices[d].name);break;
                }
            }
            if(terminal_current_path[0]){terminal_append(row,sizeof(row),"/");terminal_append(row,sizeof(row),terminal_current_path);}
        }
        terminal_push(row);return;
    }
    if(terminal_equals(cmd,"cd ..")) { terminal_path_parent(); return; }
    if(terminal_equals(cmd,"cd .")) return;
    if(terminal_starts_with(cmd,"cd ")) {
        (void)mint_fat16_get_volumes(&fat_volume_list);
        if(!fat_volume_list.count || terminal_volume_ordinal>=fat_volume_list.count) {terminal_push("cd requires a mounted FAT16 volume; use sda/sdb first");return;}
        (void)mint_fat16_select_volume(terminal_volume_ordinal);
        int64_t n=mint_fat16_list_dir(terminal_current_path,fat_entries,MINT_FAT16_LIST_MAX);
        const char *want=cmd+3; int found=0;
        for(int64_t i=0;i<n&&i<(int64_t)MINT_FAT16_LIST_MAX;i++) if((fat_entries[i].flags&MINT_FAT16_DIRENT_DIRECTORY)&&terminal_equals(fat_entries[i].name,want)) { found=1;break; }
        if(!found){terminal_push("Directory not found");return;}
        char next[96];if(!path_append(next,sizeof(next),terminal_current_path,want)){terminal_push("Path is too long");return;}
        (void)copy_text(terminal_current_path,sizeof(terminal_current_path),next);return;
    }
    if(terminal_starts_with(cmd,"ls") && (cmd[2]=='\0'||cmd[2]==' ')) {
        if(cmd[2]==' ') {
            if(!terminal_select_named_volume(cmd+3)){terminal_push("Unknown mounted disk; use 'volumes'");return;}
        }
        (void)mint_fat16_get_volumes(&fat_volume_list);
        if(fat_volume_list.count && terminal_volume_ordinal<fat_volume_list.count &&
           mint_fat16_select_volume(terminal_volume_ordinal)==0) {
            int64_t n=mint_fat16_list_dir(terminal_current_path,fat_entries,MINT_FAT16_LIST_MAX);
            if(n<=0){terminal_push("Mounted volume directory is empty or unreadable");return;}
            for(int64_t i=0;i<n&&i<(int64_t)MINT_FAT16_LIST_MAX;i++){char row[80]="";terminal_append(row,sizeof(row),fat_entries[i].name);if(fat_entries[i].flags&MINT_FAT16_DIRENT_DIRECTORY)terminal_append(row,sizeof(row),"/");terminal_push(row);}
        } else {
            int64_t n=mint_fs_list(terminal_ramfs_entries,MINT_RAMFS_MAX_FILES);
            if(n<=0){terminal_push("Session RAM filesystem is empty");return;}
            for(int64_t i=0;i<n&&i<(int64_t)MINT_RAMFS_MAX_FILES;i++)terminal_push(terminal_ramfs_entries[i].name);
        }
        return;
    }
    if((cmd[0]=='c'||cmd[0]=='C')&&(cmd[1]=='a'||cmd[1]=='A')&&(cmd[2]=='t'||cmd[2]=='T')&&cmd[3]==' ') {
        const char *arg=cmd+4; char name[64]; (void)copy_text(name,sizeof(name),arg);
        for(size_t i=0;name[i];i++)if(name[i]>='a'&&name[i]<='z')name[i]=(char)(name[i]-'a'+'A');
        int64_t handle=mint_fs_open(name,MINT_FS_OPEN_READ);
        if(handle>=0) {
            char content[86]; int64_t got=mint_fs_read((uint64_t)handle,content,sizeof(content)-1u); (void)mint_fs_close((uint64_t)handle);
            if(got<0)terminal_push("Read failed"); else {content[got]='\0'; if(!content[0])terminal_push("(empty file)"); else terminal_push(content);} return;
        }
        if(mint_fat16_get_volumes(&fat_volume_list)>=0 && fat_volume_list.count && terminal_volume_ordinal<fat_volume_list.count && mint_fat16_select_volume(terminal_volume_ordinal)==0) {
            char path[96]; char content[86];
            if(terminal_fat_path(path,sizeof(path),name)) {
                int64_t got=mint_fat16_read_file(path,content,sizeof(content)-1u);
                if(got>=0){content[got]='\0';terminal_push(content);return;}
            }
        }
        terminal_push("File not found in session RAM or mounted volume"); return;
    }
    terminal_push("Unknown command; type help");
}
static void refresh_fat_files(void) {
    if (!files_volume || mint_fat16_select_volume(files_volume-1u) != 0) {
        fat_entry_count=0u;
        return;
    }
    int64_t n=mint_fat16_list_dir(files_path,fat_entries,MINT_FAT16_LIST_MAX);
    fat_entry_count=n>0?(uint32_t)n:0u;
}
static void files_preview_ram(const char *name) {
    files_preview_length=0; files_preview[0]='\0'; files_preview_active=0;
    int64_t handle=mint_fs_open(name,MINT_FS_OPEN_READ);
    if (handle<0) return;
    int64_t n=mint_fs_read((uint64_t)handle,files_preview,sizeof(files_preview)-1u);
    (void)mint_fs_close((uint64_t)handle);
    if (n<0) return;
    files_preview_length=(size_t)n; files_preview[files_preview_length]='\0'; files_preview_active=1u;
}
static void files_select_entry(uint32_t index) {
    files_selected_entry=(int32_t)index; files_preview_active=0; files_preview_length=0; files_preview[0]='\0';
    if (files_volume) {
        if (mint_fat16_select_volume(files_volume-1u) != 0) return;
        if (index>=fat_entry_count) return;
        MINT_FAT16_DIRENT *e=&fat_entries[index];
        if (e->flags&MINT_FAT16_DIRENT_DIRECTORY) {
            char next[96];
            if (path_append(next,sizeof(next),files_path,e->name)) {
                copy_text(files_path,sizeof(files_path),next); files_selected_entry=-1; refresh_fat_files();
            }
        } else {
            char full[96]; if (!path_append(full,sizeof(full),files_path,e->name)) return;
            int64_t n=mint_fat16_read_file(full,files_preview,sizeof(files_preview)-1u);
            if (n>=0) { files_preview_length=(size_t)n; files_preview[files_preview_length]='\0'; files_preview_active=1u; }
        }
    } else {
        if (index<fs_entry_count) files_preview_ram(fs_entries[index].name);
    }
}
static void files_go_up(void) {
    if (!files_path[0]) { files_selected_entry=-1; files_preview_active=0; return; }
    size_t n=text_length(files_path);
    while (n && files_path[n-1u]!='/') --n;
    if (n) files_path[n-1u]='\0'; else files_path[0]='\0';
    files_selected_entry=-1; files_preview_active=0; refresh_fat_files();
}

static void load_notes(void) {
    if (notes_loaded) return;
    note_length = 0; note_buffer[0] = '\0';
    int64_t h = mint_fs_open("NOTES.TXT", MINT_FS_OPEN_READ);
    if (h >= 0) {
        int64_t n = mint_fs_read((uint64_t)h, note_buffer, sizeof(note_buffer)-1u);
        if (n > 0) note_length = (size_t)n;
        note_buffer[note_length] = '\0';
        (void)mint_fs_close((uint64_t)h);
    }
    notes_loaded = 1u;
}

static void save_notes(void) {
    int64_t h = mint_fs_open("NOTES.TXT", MINT_FS_OPEN_WRITE | MINT_FS_OPEN_TRUNC);
    if (h < 0) return;
    (void)mint_fs_write((uint64_t)h, note_buffer, note_length);
    (void)mint_fs_close((uint64_t)h);
}

static int update_desktop_settings(void) {
    desktop_settings.version = MINT_DESKTOP_SETTINGS_VERSION;
    if (!mint_desktop_settings_set(&desktop_settings)) return 0;
    (void)mint_desktop_settings_get(&desktop_settings);
    apply_palette();
    paint_shell();
    return 1;
}

static const char *accent_name(uint32_t accent) {
    static const char *const names[5] = {"MINT","BLUE","VIOLET","ORANGE","ROSE"};
    return names[accent <= MINT_ACCENT_ROSE ? accent : 0u];
}

static void paint_wallpaper(void) {
    int32_t w = (int32_t)display.width, h = (int32_t)display.height;
    if (mint_ui_set_wallpaper(desktop_settings.wallpaper) == 0) return;
    (void)fill(0, 0, w, h, C_WALL);
    /* Approximate the blue-to-apricot wallpaper gradient without giving ring 3
       access to the framebuffer. Broad bands keep syscall and raster cost low. */
    int32_t sky_end = h * 68 / 100;
    for (uint32_t i = 0; i < 24u; ++i) {
        int32_t y0 = (int32_t)((uint64_t)h * i * 68u / 2400u);
        int32_t y1 = (int32_t)((uint64_t)h * (i + 1u) * 68u / 2400u);
        if (y1 <= y0) y1 = y0 + 1;
        (void)fill(0, y0, w, y1 - y0, blend(C_SKY_A, C_SKY_B, i, 23u));
    }
    /* Three soft, overlapping oval bands create a more organic desktop field. */
    const int32_t centers_x[3] = {w * 58 / 100, w * 27 / 100, w * 76 / 100};
    const int32_t centers_y[3] = {h * 53 / 100, h * 69 / 100, h * 45 / 100};
    const int32_t radii_x[3] = {w * 41 / 100, w * 35 / 100, w * 31 / 100};
    const int32_t radii_y[3] = {h * 42 / 100, h * 32 / 100, h * 29 / 100};
    const uint32_t colors[3] = {C_WAVE_A, C_WAVE_C, C_WAVE_B};
    for (uint32_t shape = 0; shape < 3u; ++shape) {
        int32_t ry = radii_y[shape];
        for (int32_t dy = -ry; dy <= ry; dy += 3) {
            int64_t dy2 = (int64_t)dy * dy;
            int64_t ry2 = (int64_t)ry * ry;
            int32_t span = (int32_t)((int64_t)radii_x[shape] * (ry2 - dy2) / (ry2 ? ry2 : 1));
            if (span < 0) span = 0;
            int32_t yy = centers_y[shape] + dy;
            if (yy < 45 || yy >= h - 18) continue;
            uint32_t c = blend(colors[shape], C_WALL, (uint32_t)(dy + ry), (uint32_t)(2 * ry ? 2 * ry : 1));
            (void)fill(centers_x[shape] - span, yy, span * 2, 7, c);
        }
    }
    (void)fill(0, sky_end, w, h - sky_end, blend(C_WAVE_C, C_WALL, 2u, 5u));
    /* A low horizon band adds depth while retaining the original MintOS colors. */
    (void)fill(0, h * 64 / 100, w, 2, 0x9AB4C8u);
}

static void app_icon(uint32_t app, int32_t x, int32_t y, int32_t size) {
    static const uint8_t *const icon_payloads[APP_COUNT] = {
        mint_icon_files_rgba, mint_icon_notes_rgba, mint_icon_terminal_rgba,
        mint_icon_settings_rgba, mint_icon_system_monitor_rgba, mint_icon_storage_rgba,
        mint_icon_cpu_rgba, mint_icon_database_rgba, mint_icon_disk_utility_rgba,
        mint_icon_mintde_rgba, mint_icon_windows_rgba, mint_icon_launchpad_rgba
    };
    uint32_t c = app_colors[app % APP_COUNT];
    if (mint_ui_draw_bitmap_rgba(icon_payloads[app % APP_COUNT], 96u, 96u,
                                 x, y, (uint32_t)size, (uint32_t)size) == 0) return;
    /* Primitive fallback keeps the shell usable if the bitmap service is unavailable. */
    round_box(x, y, size, size, 8, c);
    if (app == APP_FILES) {
        (void)fill(x + size/5, y + size/4, size*3/5, size*2/5, 0xF3D56Cu);
        (void)fill(x + size/5, y + size/3, size*3/5, size/3, 0xFFF0B2u);
    } else if (app == APP_NOTES) {
        (void)fill(x + size/4, y + size/6, size/2, size*2/3, 0xF8F4FFu);
        for (int32_t k = 0; k < 3; ++k) line(x + size/3, y + size/3 + k * (size/8), size/3, C_VIOLET);
    } else if (app == APP_TERMINAL) {
        (void)fill(x + size/6, y + size/6, size*2/3, size*2/3, 0x132C40u);
        line(x + size/4, y + size/3, size/4, C_ACCENT);
        line(x + size/4, y + size/2, size/3, C_TEXT);
    } else if (app == APP_SETTINGS) {
        /* Compact gear mark instead of the previous oversized plus sign. */
        int32_t cx=x+size/2, cy=y+size/2, tooth=size/7;
        round_box(cx-tooth/2,y+size/6,tooth,size/5,3,C_TEXT);
        round_box(cx-tooth/2,y+size*2/3,tooth,size/5,3,C_TEXT);
        round_box(x+size/6,cy-tooth/2,size/5,tooth,3,C_TEXT);
        round_box(x+size*2/3,cy-tooth/2,size/5,tooth,3,C_TEXT);
        round_box(x+size/4,y+size/4,size/2,size/2,size/5,C_TEXT);
        round_box(x+size*3/8,y+size*3/8,size/4,size/4,size/8,c);
    } else if (app == APP_MONITOR) {
        round_box(x+size/6,y+size/6,size*2/3,size*2/3,5,0xEDF5FAu);
        (void)fill(x+size/4,y+size*2/3,size/2,2,0x93A8B8u);
        (void)fill(x+size/4,y+size/2,size/9,size/5,C_BLUE);
        (void)fill(x+size*4/9,y+size*2/5,size/9,size*3/10,C_ACCENT);
        (void)fill(x+size*5/8,y+size/3,size/9,size*2/5,C_ORANGE);
    } else if (app == APP_STORAGE) {
        (void)fill(x + size/5, y + size/4, size*3/5, size/2, 0xE9F2F8u);
        (void)fill(x + size/4, y + size/3, size/2, 2, 0x487EBBu);
        round_box(x + size*2/3, y + size*3/5, 4, 4, 2, C_ACCENT);
    } else if (app == APP_DRIVERS) {
        for (int32_t k = 0; k < 3; ++k) {
            round_box(x + size/5, y + size/5 + k*(size/5), size*3/5, size/9, 3, 0xF3FBFFu);
            round_box(x + size/3, y + size/5 + k*(size/5), size/10, size/9, 3, C_ACCENT);
        }
    } else if (app == APP_NETWORK) {
        round_box(x + size/2 - 3, y + size/5, 6, 6, 3, 0xF3FBFFu);
        round_box(x + size/5, y + size/2, size*3/5, size/9, 3, 0xF3FBFFu);
        line(x + size/3, y + size/3, size/3, 0xF3FBFFu);
        (void)fill(x + size/4, y + size*2/3, size/2, 3, 0xF3FBFFu);
    } else if (app == APP_DISKS) {
        round_box(x + size/6, y + size/5, size*2/3, size*3/5, 7, 0xEAF3FAu);
        round_box(x + size/4, y + size/3, size/2, size/3, size/6, 0xBFD5E6u);
        round_box(x + size/2-2, y + size/2-2, 4, 4, 2, C_BLUE);
        (void)fill(x + size/4, y + size*3/4, size/2, 2, C_ACCENT);
    } else if (app == APP_ABOUT_OS) {
        round_box(x+size/5,y+size/5,size*3/5,size*3/5,size/6,0xEAF6F5u);
        round_box(x+size/3,y+size/3,size/3,size/3,size/10,C_ACCENT);
        (void)fill(x+size/2-1,y+size/4,2,size/2,0xF7FFFFu);
        (void)fill(x+size/4,y+size/2-1,size/2,2,0xF7FFFFu);
    } else if (app == APP_ABOUT_MINTDE) {
        round_box(x+size/5,y+size/5,size*3/5,size*3/5,size/6,0xEAF3FAu);
        round_box(x+size/3,y+size/3,size/3,size/3,size/10,C_BLUE);
        round_box(x+size/2-size/12,y+size/4,size/6,size/2,size/12,C_ACCENT);
    } else {
        for (int32_t k = 0; k < 3; ++k)
            (void)fill(x + size/4 + k * (size/6), y + size/3, size/12, size/3, C_TEXT);
        (void)fill(x + size/4, y + size*2/3, size/2, 3, C_TEXT);
    }
}

static void paint_menu_bar(void) {
    int32_t w=(int32_t)display.width;
    (void)fill(0,0,w,42,C_MENU); (void)fill(0,40,w,2,C_BORDER);
    if(active_menu==0) round_box(39,5,82,30,8,C_MENU_DARK);
    round_box(14,10,20,20,9,C_ACCENT);
    round_box(19,14,10,12,4,C_MENU);
    (void)draw_text("MintDE",44,13,C_TEXT,1);
    const char *labels[7]={"MintDE","Desktop","File","Edit","View","Window","Help"};
    const int32_t lefts[7]={40,142,220,268,316,368,442};
    const int32_t widths[7]={76,76,44,44,44,66,48};
    for (uint32_t i=1;i<7u;i++) {
        if (active_menu==(int32_t)i) round_box(lefts[i]-5,5,widths[i]+10,30,8,C_MENU_DARK);
        (void)draw_text(labels[i],lefts[i]+8,13,C_TEXT,1);
    }
    (void)fill(w-280,5,270,30,C_MENU);
    (void)draw_text(input_info.keyboard_ready?"Keyboard":"Keyboard?",w-230,13,input_info.keyboard_ready?C_ACCENT:C_ORANGE,1);
    (void)draw_text(input_info.mouse_ready?"Pointer":"Pointer?",w-105,13,input_info.mouse_ready?C_ACCENT:C_ORANGE,1);
}
static uint32_t menu_item_count(int32_t menu) {
    static const uint8_t counts[7]={5,9,4,3,4,3,4};
    return menu>=0 && menu<7 ? counts[menu] : 0u;
}
static const char *menu_item_label(int32_t menu,uint32_t item) {
    static const char *const items[7][9]={
        {"About MintDE","About MintOS","Settings","System Monitor","Hardware & Drivers"},
        {"Applications","Files","Notes","Settings","System Monitor","Disk Utility","Storage & Logs","Hardware & Drivers","Network"},
        {"Open Files","Open Notes","Disk Utility","Close Window"},
        {"Open Notes","Open Settings","Save Notes"},
        {"Toggle Light / Dark","Next Accent Color","Change Wallpaper","Refresh System Monitor"},
        {"Minimize Window","Maximize / Restore","Close Window"},
        {"System Monitor","Storage & Logs","Hardware & Drivers","Settings"}
    };
    uint32_t n=menu_item_count(menu); return item<n?items[menu][item]:"";
}
static int32_t menu_item_action(int32_t menu,uint32_t item) {
    if(menu==0){ static const int32_t a[5]={APP_ABOUT_MINTDE,APP_ABOUT_OS,APP_SETTINGS,APP_MONITOR,APP_DRIVERS}; return item<5u?a[item]:-1; }
    if(menu==1){ static const int32_t a[9]={201,APP_FILES,APP_NOTES,APP_SETTINGS,APP_MONITOR,APP_DISKS,APP_STORAGE,APP_DRIVERS,APP_NETWORK}; return item<9u?a[item]:-1; }
    if(menu==2){ static const int32_t a[4]={APP_FILES,APP_NOTES,APP_DISKS,202}; return item<4u?a[item]:-1; }
    if(menu==3){ static const int32_t a[3]={APP_NOTES,APP_SETTINGS,203}; return item<3u?a[item]:-1; }
    if(menu==4){ static const int32_t a[4]={204,205,206,207}; return item<4u?a[item]:-1; }
    if(menu==5){ static const int32_t a[3]={208,209,202}; return item<3u?a[item]:-1; }
    if(menu==6){ static const int32_t a[4]={APP_MONITOR,APP_STORAGE,APP_DRIVERS,APP_SETTINGS}; return item<4u?a[item]:-1; }
    return -1;
}
static int32_t menu_left(int32_t menu) {
    static const int32_t x[7]={40,142,220,268,316,368,442};
    int32_t value=menu>=0&&menu<7?x[menu]:40;
    int32_t max=(int32_t)display.width-224; if(value>max)value=max; if(value<4)value=4; return value;
}
static void paint_menu_dropdown(void) {
    if(active_menu<0 || active_menu>6) return;
    uint32_t count=menu_item_count(active_menu); int32_t x=menu_left(active_menu), y=44;
    round_box(x+3,y+5,214,(int32_t)count*31+8,14,0x102030u);
    glass_box(x,y,214,(int32_t)count*31+8,14,C_WINDOW,224u);
    glass_box(x+4,y+3,206,(int32_t)count*31+2,11,C_MENU,112u);
    for(uint32_t i=0;i<count;i++){
        int32_t iy=y+5+(int32_t)i*31;
        if ((int32_t)i==hovered_menu_item) glass_box(x+5,iy,204,28,7,C_ACCENT,52u);
        (void)draw_text(menu_item_label(active_menu,i),x+14,iy+8,C_TEXT,1);
        if(i+1u<count) (void)fill(x+12,iy+29,190,1,blend(C_BORDER,C_WINDOW,1u,3u));
    }
}
static void paint_cursor(void) {
    /* Crisp 22x30 black-outline/white-fill arrow. Pointer coordinates denote
       the tip; drawing stays clipped by the bounded rectangle service. */
    static const uint8_t left[30] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,9,10
    };
    static const uint8_t right[30] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,20,21,21,20,19,18,17,17,18,19
    };
    int32_t x=pointer_x, y=pointer_y;
    for (int32_t row=0; row<30; ++row) {
        int32_t l=left[row], r=right[row];
        if (r<=l) continue;
        (void)fill(x+l,y+row,r-l,1,0x07131Fu);
        if (r-l>2) (void)fill(x+l+1,y+row,r-l-2,1,0xFFFFFFu);
    }
}

#define MINTDE_DOCK_APP_COUNT 10u
static const uint32_t mintde_dock_apps[MINTDE_DOCK_APP_COUNT] = {
    APP_FILES, APP_NOTES, APP_TERMINAL, APP_SETTINGS, APP_MONITOR,
    APP_STORAGE, APP_DRIVERS, APP_NETWORK, APP_DISKS, APP_LAUNCHPAD
};
static void dock_geometry(int32_t *left, int32_t *top, int32_t *cell, int32_t *total) {
    int32_t w=(int32_t)display.width, h=(int32_t)display.height;
    int32_t c=52, width=(int32_t)MINTDE_DOCK_APP_COUNT*c+24;
    if (width>w-16) { c=(w-40)/(int32_t)MINTDE_DOCK_APP_COUNT; width=(int32_t)MINTDE_DOCK_APP_COUNT*c+24; }
    if (c<38) c=38;
    width=(int32_t)MINTDE_DOCK_APP_COUNT*c+24;
    if (left) *left=(w-width)/2;
    if (top) *top=h-64;
    if (cell) *cell=c;
    if (total) *total=width;
}
static void paint_dock(void) {
    int32_t left=0, top=0, cell=0, total=0;
    dock_geometry(&left,&top,&cell,&total);
    if ((int32_t)display.height<480) return;
    uint32_t glass=blend(C_MENU,C_WALL,1u,3u);
    glass_box(left,top,total,56,20,glass,176u);
    glass_box(left+1,top+1,total-2,54,19,C_MENU_DARK,118u);
    (void)fill(left+22,top+2,total-44,1,blend(C_BORDER,C_WINDOW,1u,2u));
    for(uint32_t i=0;i<MINTDE_DOCK_APP_COUNT;i++){
        uint32_t app=mintde_dock_apps[i];
        int32_t ix=left+12+(int32_t)i*cell+(cell-36)/2;
        app_icon(app,ix,top+7,36);
        if(app_window_open && selected_app==app && !window_minimized)
            round_box(left+12+(int32_t)i*cell+cell/2-2,top+47,4,4,2,C_ACCENT);
    }
}
static int handle_dock_click(int32_t x,int32_t y) {
    int32_t left=0,top=0,cell=0,total=0;
    dock_geometry(&left,&top,&cell,&total);
    if(x<left||x>=left+total||y<top||y>=top+56) return 0;
    for(uint32_t i=0;i<MINTDE_DOCK_APP_COUNT;i++){
        int32_t ix=left+12+(int32_t)i*cell;
        if(x>=ix&&x<ix+cell){open_app(mintde_dock_apps[i]);return 1;}
    }
    active_menu=-1; return 1;
}

static void menu_execute(int32_t action) {
    active_menu=-1; hovered_menu_item=-1;
    if(action>=0 && action<(int32_t)APP_COUNT){ open_app((uint32_t)action); return; }
    switch(action){
        case 200: library_open=0; app_window_open=0; window_minimized=0; window_focused=0; disk_result[0]='\0'; break;
        case 201: open_app(APP_LAUNCHPAD); return;
        case 202: app_window_open=0; window_minimized=0; window_maximized=0; window_focused=0; open_settings_dropdown=0u; break;
        case 203: save_notes(); break;
        case 204: desktop_settings.theme=(desktop_settings.theme==MINT_THEME_DARK)?MINT_THEME_LIGHT:MINT_THEME_DARK; (void)update_desktop_settings(); return;
        case 205: desktop_settings.accent=(desktop_settings.accent+1u)%5u; (void)update_desktop_settings(); return;
        case 206: desktop_settings.wallpaper=(desktop_settings.wallpaper+1u)%3u; (void)update_desktop_settings(); return;
        case 207: refresh_storage_data(); refresh_driver_catalog(); (void)mint_input_get_info(&input_info); break;
        case 208: window_minimized=1; open_settings_dropdown=0u; break;
        case 209: window_maximized=!window_maximized; window_minimized=0; open_settings_dropdown=0u; break;
        default: break;
    }
    paint_shell();
}
static void handle_menu_bar_click(int32_t x) {
    int32_t menu=x<130?0:x<218?1:x<266?2:x<314?3:x<362?4:x<438?5:6;
    active_menu=active_menu==menu?-1:menu; hovered_menu_item=-1; paint_shell();
}

static void paint_files_view(int32_t x, int32_t y, int32_t w, int32_t h, int32_t pad) {
    int32_t sx=x+pad+18, sy=y+124, sw=176, sh=h-174;
    if (sh<150) sh=150;
    int32_t lx=sx+sw+18, ly=sy+45, lw=w-(lx-x)-pad-22;
    if (lw<160) lw=160;
    (void)draw_text("Files",x+pad+26,y+91,C_TEXT,2);
    (void)draw_text("Navigate folders, inspect metadata, and preview text files",x+pad+26,y+116,C_MUTED,1);
    round_box(sx,sy,sw,sh,12,C_WINDOW);
    (void)draw_text("LOCATIONS",sx+14,sy+14,C_MUTED,1);
    round_box(sx+8,sy+36,sw-16,31,8,!files_volume?C_MENU_DARK:C_WINDOW);
    (void)draw_text("Session (RAM)",sx+18,sy+45,!files_volume?C_ACCENT:C_TEXT,1);
    uint32_t mounted_count=fat_volume_list.count;
    if (mounted_count > MINT_FAT16_MAX_VOLUMES) mounted_count=MINT_FAT16_MAX_VOLUMES;
    for (uint32_t vi=0; vi<mounted_count; ++vi) {
        int32_t ry=sy+72+(int32_t)vi*35;
        uint32_t active=(files_volume==vi+1u);
        round_box(sx+8,ry,sw-16,32,7,active?C_MENU_DARK:C_WINDOW);
        char label[32]="Developer volume ";
        const char *device_name="disk";
        for(uint32_t di=0;di<storage_snapshot.device_count && di<MINT_STORAGE_MAX_DEVICES;di++) {
            if(storage_snapshot.devices[di].block_registered &&
               storage_snapshot.devices[di].block_index==fat_volume_list.block_indices[vi]) {
                device_name=storage_snapshot.devices[di].name; break;
            }
        }
        size_t lp=text_length(label);
        for(size_t ci=0;device_name[ci] && lp+1u<sizeof(label);++ci) label[lp++]=device_name[ci];
        label[lp]='\0';
        (void)draw_text(label,sx+16,ry+5,active?C_ACCENT:C_TEXT,1);
        (void)draw_text("FAT16 / read-only",sx+16,ry+19,C_MUTED,1);
    }
    int32_t source_y=sy+78+(int32_t)(mounted_count?mounted_count:1u)*35;
    (void)draw_text("SOURCE",sx+14,source_y,C_MUTED,1);
    (void)draw_text(files_volume?"FAT16 / read-only":"RAM / volatile",sx+14,source_y+21,C_TEXT,1);
    int32_t up_y=source_y+44;
    round_box(sx+8,up_y,sw-16,29,7,C_CONTENT);
    (void)draw_text("Up one level",sx+18,up_y+8,C_ACCENT,1);
    round_box(lx,sy,lw,34,8,C_WINDOW);
    (void)draw_text("Path:",lx+12,sy+10,C_MUTED,1);
    (void)draw_text(files_volume?(files_path[0]?files_path:"/ (volume root)"):("/ (session root)"),lx+58,sy+10,C_TEXT,1);
    int32_t list_y=ly, list_h=sh-47; if (list_h<76) list_h=76;
    int32_t detail_w=files_preview_active?lw*55/100:lw; if (detail_w<130) detail_w=lw;
    round_box(lx,list_y,detail_w,list_h,10,C_WINDOW);
    (void)draw_text("Name",lx+12,list_y+10,C_MUTED,1);
    (void)draw_text("Size",lx+detail_w-80,list_y+10,C_MUTED,1);
    if (files_volume) refresh_fat_files(); else refresh_files();
    uint32_t count=files_volume?fat_entry_count:fs_entry_count;
    int32_t row_y=list_y+34;
    for (uint32_t i=0;i<count && i<16u && row_y<list_y+list_h-15;i++,row_y+=28) {
        const char *name=files_volume?fat_entries[i].name:fs_entries[i].name;
        uint32_t size=files_volume?fat_entries[i].size:fs_entries[i].size;
        uint32_t is_dir=files_volume && (fat_entries[i].flags&MINT_FAT16_DIRENT_DIRECTORY);
        if (files_selected_entry==(int32_t)i) round_box(lx+6,row_y-4,detail_w-12,25,6,C_MENU_DARK);
        if (is_dir) round_box(lx+12,row_y+2,14,12,3,C_BLUE);
        else round_box(lx+14,row_y+1,10,14,2,C_MUTED);
        (void)draw_text(name,lx+34,row_y,C_TEXT,1);
        char sz[24]; format_u64(sz,sizeof(sz),size);
        (void)draw_text(is_dir?"Folder":sz,lx+detail_w-80,row_y,C_MUTED,1);
    }
    if (!count) (void)draw_text(files_volume?"This directory is empty":"No session files",lx+14,list_y+42,C_MUTED,1);
    if (files_preview_active) {
        int32_t dx=lx+detail_w+9, dw=lw-detail_w-9;
        if (dw>80) {
            round_box(dx,list_y,dw,list_h,10,C_WINDOW);
            (void)draw_text("Preview",dx+12,list_y+10,C_MUTED,1);
            int32_t py=list_y+36; size_t off=0;
            while (off<files_preview_length && py<list_y+list_h-12) {
                char linebuf[34]; size_t n=0;
                while (off<files_preview_length && files_preview[off]!='\n' && n+1u<sizeof(linebuf)) {
                    char c=files_preview[off++]; if (c=='\r') continue; linebuf[n++]=c;
                }
                if (off<files_preview_length && files_preview[off]=='\n') ++off;
                linebuf[n]='\0'; (void)draw_text(linebuf,dx+12,py,C_TEXT,1); py+=21;
            }
            if (!files_preview_length) (void)draw_text("Empty file",dx+12,list_y+40,C_MUTED,1);
        }
    }
    (void)draw_text(files_volume?"Mounted media is read-only; changes are not allowed yet.":"Session files are held in RAM and reset after reboot.",x+pad+224,y+h-32,files_volume?C_ACCENT:C_ORANGE,1);
}

static void paint_home_window(int32_t x, int32_t y, int32_t w, int32_t h) {
    round_box(x + 7, y + 9, w, h, 12, 0x17304Au);
    glass_box(x, y, w, h, 12, C_WINDOW, 218u);
    glass_box(x+1, y+1, w-2, 42, 12, C_MENU_DARK, 190u);
    round_box(x + 20, y + 15, 12, 12, 6, C_RED);
    round_box(x + 40, y + 15, 12, 12, 6, C_YELLOW);
    round_box(x + 60, y + 15, 12, 12, 6, C_GREEN);
    (void)draw_text(library_open ? "Applications" : app_names[selected_app],
                    x + 92, y + 12, C_TEXT, 1);
    if (library_open) {
        int32_t pad = w / 22; if (pad < 24) pad = 24;
        (void)draw_text("Your workspace", x + pad + 24, y + 84, C_TEXT, 2);
        (void)draw_text("Choose an app to open", x + pad + 24, y + 116, C_MUTED, 1);
        int compact_cards = h < 530 || w < 760;
        int32_t grid_x = x + 50, grid_y = y + (compact_cards ? 132 : 142);
        int32_t cell_w = (w - 100) / 3;
        int32_t card_h = compact_cards ? 72 : 88;
        int32_t row_step = compact_cards ? 80 : 104;
        static const char *const descriptions[APP_COUNT] = {
            "Browse files", "Quick notes", "Shell commands",
            "Appearance", "System health", "Disk checks and logs",
            "Driver status", "Network status", "Partition and format disks",
            "OS version and kernel", "MintDE version and session", "Open app library"
        };
        for (uint32_t i = 0; i < APP_COUNT; ++i) {
            int32_t col = (int32_t)(i % 3u), row = (int32_t)(i / 3u);
            int32_t cx = grid_x + col * cell_w;
            int32_t cy = grid_y + row * row_step;
            glass_box(cx, cy, cell_w - 14, card_h, 9, C_WINDOW, 218u);
            (void)fill(cx, cy, cell_w - 14, 2, app_colors[i]);
            app_icon(i, cx + 12, cy + (compact_cards ? 14 : 17), compact_cards ? 40 : 44);
            (void)draw_text(app_names[i], cx + (compact_cards ? 60 : 70),
                            cy + (compact_cards ? 13 : 22), C_TEXT, 1);
            (void)draw_text(descriptions[i], cx + (compact_cards ? 60 : 70),
                            cy + (compact_cards ? 42 : 49), C_MUTED, 1);
            if (!compact_cards) (void)draw_text("Open  >", cx + 70, cy + 68, C_ACCENT, 1);
        }
        (void)draw_text("Settings and files are currently session-backed.",
                        x + pad + 24, y + h - 34, C_MUTED, 1);
    } else {
        int32_t pad = w / 22; if (pad < 24) pad = 24;
        glass_box(x + pad, y + 62, w - 2 * pad, h - 84, 8, C_CONTENT, 214u);
        if (selected_app == APP_FILES) {
            paint_files_view(x,y,w,h,pad);
        } else if (selected_app == APP_NOTES) {
            (void)draw_text("Notes", x + pad + 26, y + 91, C_TEXT, 2);
            (void)fill(x + pad + 26, y + 132, w / 2, 2, C_BORDER);
            (void)draw_text("Type to edit - Backspace erases - Enter adds a line", x + pad + 26, y + 155, C_MUTED, 1);
            int32_t line_y = y + 194;
            size_t i = 0;
            while (i < note_length && line_y < y + h - 28) {
                char line_text[48]; size_t n = 0;
                while (i < note_length && note_buffer[i] != '\n' && n < sizeof(line_text)-1u) {
                    char c = note_buffer[i++];
                    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                    line_text[n++] = c;
                }
                if (i < note_length && note_buffer[i] == '\n') ++i;
                line_text[n] = '\0';
                (void)draw_text(line_text, x + pad + 26, line_y, C_TEXT, 1);
                line_y += 24;
            }
            (void)draw_text("Session RAM file - resets after reboot", x + pad + 26, y + h - 34, C_MUTED, 1);
        } else if (selected_app == APP_TERMINAL) {
            (void)draw_text("Terminal", x + pad + 26, y + 91, C_TEXT, 2);
            (void)draw_text("Bounded MintDE command shell; commands are read-only", x + pad + 26, y + 120, C_MUTED, 1);
            int32_t ty=y+145;
            uint32_t first=terminal_output_count>8u?terminal_output_count-8u:0u;
            for(uint32_t ti=first;ti<terminal_output_count;ti++) {
                glass_box(x+pad+22,ty-4,w-2*pad-44,22,5,C_WINDOW,terminal_output_kind[ti]?190u:145u);
                if(terminal_output_kind[ti]) {
                    (void)draw_text("root#mintos >",x+pad+32,ty,C_ACCENT,1);
                    (void)draw_text(terminal_output[ti],x+pad+138,ty,C_TEXT,1);
                } else {
                    (void)draw_text(terminal_output[ti],x+pad+32,ty,C_TEXT,1);
                }
                ty+=25;
            }
            round_box(x+pad+22,y+h-78,w-2*pad-44,34,8,C_MENU_DARK);
            (void)draw_text("root#mintos >",x+pad+32,y+h-67,C_ACCENT,1);
            size_t visible_chars=(size_t)((w-2*pad-184)/8); if(visible_chars>72u)visible_chars=72u;
            const char *input_view=terminal_input;
            if(terminal_input_length>visible_chars)input_view=terminal_input+terminal_input_length-visible_chars;
            (void)draw_text(input_view,x+pad+138,y+h-67,C_TEXT,1);
            if(!terminal_output_count) { terminal_push("MintOS26 Terminal ready - type help"); }
        } else if (selected_app == APP_SETTINGS) {
            int32_t inner_left=x+pad+18;
            int32_t inner_w=w-2*pad-36;
            (void)draw_text("Settings",x+pad+26,y+80,C_TEXT,2);
            (void)draw_text("Personalize and inspect this MintDE session",x+pad+26,y+105,C_MUTED,1);
            static const char *const category_names[6]={"General","Appearance","Displays","Input","Storage","Privacy"};
            int32_t tab_y=y+128, tab_w=(inner_w-5*6)/6;
            if(tab_w<62) tab_w=62;
            for(uint32_t ti=0;ti<6u;ti++) {
                int32_t tx=inner_left+(int32_t)ti*(tab_w+6);
                glass_box(tx,tab_y,tab_w,27,8,settings_category==ti?C_MENU_DARK:C_WINDOW,
                          settings_category==ti?205u:166u);
                (void)draw_text(category_names[ti],tx+7,tab_y+8,settings_category==ti?C_ACCENT:C_TEXT,1);
            }
            if(settings_category==1u) {

            int32_t inner_w = w - 2 * pad - 36;
            int32_t gap = 20;
            int32_t card_w = (inner_w - gap) / 2;
            int32_t left_x = x + pad + 18;
            int32_t right_x = left_x + card_w + gap;
            int32_t card_y = y + 164;
            int32_t card_h = 286;
            glass_box(left_x, card_y, card_w, card_h, 14, C_CONTENT, 188u);
            glass_box(right_x, card_y, card_w, card_h, 14, C_CONTENT, 188u);
            (void)draw_text("Appearance", left_x + 18, y + 180, C_TEXT, 2);
            (void)draw_text("Personalize your workspace", left_x + 18, y + 205, C_MUTED, 1);
            (void)draw_text("Theme", left_x + 20, y + 226, C_TEXT, 1);
            (void)draw_text("Accent color", left_x + 20, y + 263, C_TEXT, 1);
            (void)draw_text("Text size", left_x + 20, y + 300, C_TEXT, 1);
            (void)draw_text("Wallpaper", left_x + 20, y + 337, C_TEXT, 1);
            (void)draw_text("Reduced motion", left_x + 20, y + 374, C_TEXT, 1);
            int32_t bx = left_x + card_w - 174;
            int32_t bw = 154;
            int32_t bh = 28;
            const int32_t row_top[5] = {y + 214, y + 251, y + 288, y + 325, y + 362};
            const char *theme = desktop_settings.theme == MINT_THEME_DARK ? "Dark  >" :
                                desktop_settings.theme == MINT_THEME_WARM ? "Warm  >" : "Light  >";
            const char *accent = accent_name(desktop_settings.accent);
            char scale_text[24];
            if (desktop_settings.ui_scale == 1u) {
                scale_text[0]='N'; scale_text[1]='o'; scale_text[2]='r'; scale_text[3]='m';
                scale_text[4]='a'; scale_text[5]='l'; scale_text[6]=' '; scale_text[7]='>'; scale_text[8]='\0';
            } else if (desktop_settings.ui_scale == 2u) {
                scale_text[0]='L'; scale_text[1]='a'; scale_text[2]='r'; scale_text[3]='g';
                scale_text[4]='e'; scale_text[5]=' '; scale_text[6]='>'; scale_text[7]='\0';
            } else {
                scale_text[0]='X'; scale_text[1]='-'; scale_text[2]='L'; scale_text[3]='a';
                scale_text[4]='r'; scale_text[5]='g'; scale_text[6]='e'; scale_text[7]=' '; scale_text[8]='>'; scale_text[9]='\0';
            }
            char wallpaper_text[24] = "Wallpaper 1  >";
            wallpaper_text[10] = (char)('1' + desktop_settings.wallpaper);
            const char *motion = (desktop_settings.flags & MINT_DESKTOP_SETTING_REDUCE_MOTION) ? "On  >" : "Off  >";
            const char *values[5] = {theme, accent, scale_text, wallpaper_text, motion};
            for (uint32_t r = 0; r < 5u; ++r) {
                uint32_t id = r + 1u;
                uint32_t selected = (hover_setting_control == id || keyboard_setting_focus == id);
                round_box(bx, row_top[r], bw, bh, 7, selected ? C_MENU_DARK : C_WINDOW);
                (void)fill(bx, row_top[r], bw, 1, selected ? C_ACCENT : C_BORDER);
                (void)draw_text(values[r], bx + 11, row_top[r] + 7, selected ? C_ACCENT : C_TEXT, 1);
            }
            (void)draw_text("Input & activity", right_x + 18, y + 180, C_TEXT, 2);
            (void)draw_text("Live hardware counters", right_x + 18, y + 205, C_MUTED, 1);
            int32_t refresh_x = right_x + card_w - 112;
            round_box(refresh_x, y + 177, 94, 27, 7,
                      (hover_setting_control == 6u || keyboard_setting_focus == 6u) ? C_MENU_DARK : C_WINDOW);
            (void)draw_text("Refresh", refresh_x + 12, y + 184, C_ACCENT, 1);
            (void)draw_text("Keyboard", right_x + 20, y + 234, C_TEXT, 1);
            (void)draw_text(input_info.keyboard_ready ? "Connected" : "Not detected",
                            right_x + card_w - 158, y + 234,
                            input_info.keyboard_ready ? C_ACCENT : C_ORANGE, 1);
            (void)draw_text("Pointer", right_x + 20, y + 264, C_TEXT, 1);
            (void)draw_text(input_info.mouse_ready ? "Connected" : "Not detected",
                            right_x + card_w - 158, y + 264,
                            input_info.mouse_ready ? C_ACCENT : C_ORANGE, 1);
            char event_count[24];
            (void)draw_text("Key events", right_x + 20, y + 294, C_MUTED, 1);
            format_u64(event_count, sizeof(event_count), input_info.keyboard_events);
            (void)draw_text(event_count, right_x + card_w - 80, y + 294, C_TEXT, 1);
            (void)draw_text("Pointer moves", right_x + 20, y + 322, C_MUTED, 1);
            format_u64(event_count, sizeof(event_count), input_info.pointer_motion_events);
            (void)draw_text(event_count, right_x + card_w - 80, y + 322, C_TEXT, 1);
            (void)draw_text("Button events", right_x + 20, y + 350, C_MUTED, 1);
            format_u64(event_count, sizeof(event_count), input_info.pointer_button_events);
            (void)draw_text(event_count, right_x + card_w - 80, y + 350, C_TEXT, 1);
            (void)draw_text("Queue drops", right_x + 20, y + 378, C_MUTED, 1);
            format_u64(event_count, sizeof(event_count), input_info.dropped_events);
            (void)draw_text(event_count, right_x + card_w - 80, y + 378, C_TEXT, 1);

            int32_t info_y = y + 464;
            int32_t info_h = h - 514; /* stop 50px panel spill that caused ghosting */
            if (info_h < 32) info_h = 32;
            glass_box(x + pad + 18, info_y, w - 2 * pad - 36, info_h, 12, C_CONTENT, 194u);
            (void)draw_text("Session services", x + pad + 34, y + 476, C_TEXT, 1);
            refresh_files();
            char files_text[48] = "RAM files: ";
            char files_num[16];
            format_u64(files_num, sizeof(files_num), fs_entry_count);
            size_t fp = text_length(files_text);
            for (size_t j = 0; files_num[j] && fp + 1u < sizeof(files_text); ++j) files_text[fp++] = files_num[j];
            files_text[fp] = '\0';
            (void)draw_text(files_text, x + pad + 34, y + 506, C_TEXT, 1);
            char generation[24];
            format_u64(generation, sizeof(generation), desktop_settings.generation);
            (void)draw_text("Settings generation", x + pad + 220, y + 506, C_MUTED, 1);
            (void)draw_text(generation, x + pad + 390, y + 506, C_TEXT, 1);
            char mounted_text[48]="Read-only volumes: "; char mounted_count[16];
            format_u64(mounted_count,sizeof(mounted_count),storage_snapshot.readonly_mount_count);
            size_t mp=text_length(mounted_text);
            for(size_t j=0;mounted_count[j]&&mp+1u<sizeof(mounted_text);j++)mounted_text[mp++]=mounted_count[j];
            mounted_text[mp]='\0';
            (void)draw_text(mounted_text,x+pad+500,y+506,C_ACCENT,1);
            (void)draw_text("Theme and files are session-only and reset after reboot.",
                            x + pad + 34, y + 534, C_MUTED, 1);
            } else {
                int32_t panel_x=x+pad+18, panel_y=y+164, panel_w=w-2*pad-36, panel_h=h-214;
                if(panel_h<220) panel_h=220;
                glass_box(panel_x,panel_y,panel_w,panel_h,12,C_CONTENT,205u);
                if(settings_category==0u) {
                    (void)draw_text("General",panel_x+22,panel_y+24,C_TEXT,2);
                    (void)draw_text("About MintOS26",panel_x+22,panel_y+70,C_TEXT,1);
                    (void)draw_text("26.5.008.0  |  x86-64 kernel",panel_x+220,panel_y+70,C_MUTED,1);
                    (void)draw_text("MintDE version",panel_x+22,panel_y+108,C_TEXT,1);
                    (void)draw_text("1.0.2  |  native ring-3 desktop",panel_x+220,panel_y+108,C_MUTED,1);
                    (void)draw_text("Startup recovery",panel_x+22,panel_y+146,C_TEXT,1);
                    (void)draw_text("UEFI Desktop remains the normal boot path",panel_x+220,panel_y+146,C_MUTED,1);
                    (void)draw_text("General settings and preferences are session-only at this stage.",panel_x+22,panel_y+194,C_ORANGE,1);
                } else if(settings_category==2u) {
                    char dims[56]="Framebuffer: "; char number[24];
                    format_u64(number,sizeof(number),display.width); size_t pos=text_length(dims); for(size_t i=0;number[i]&&pos+1u<sizeof(dims);i++)dims[pos++]=number[i]; dims[pos]='\0';
                    (void)copy_text(dims+pos,sizeof(dims)-pos," x "); pos=text_length(dims); format_u64(number,sizeof(number),display.height); for(size_t i=0;number[i]&&pos+1u<sizeof(dims);i++)dims[pos++]=number[i]; dims[pos]='\0';
                    (void)draw_text("Displays",panel_x+22,panel_y+24,C_TEXT,2);
                    (void)draw_text(dims,panel_x+22,panel_y+76,C_TEXT,1);
                    (void)draw_text("Display mode is supplied by UEFI GOP; runtime mode switching is not yet a native driver.",panel_x+22,panel_y+112,C_MUTED,1);
                    (void)draw_text("Text scale",panel_x+22,panel_y+158,C_TEXT,1);
                    (void)draw_text(desktop_settings.ui_scale==1u?"Normal":desktop_settings.ui_scale==2u?"Large":"Extra large",panel_x+220,panel_y+158,C_ACCENT,1);
                } else if(settings_category==3u) {
                    (void)mint_input_get_info(&input_info);
                    (void)draw_text("Input",panel_x+22,panel_y+24,C_TEXT,2);
                    (void)draw_text("Keyboard",panel_x+22,panel_y+76,C_TEXT,1);
                    (void)draw_text(input_info.keyboard_ready?"Ready":"Not detected",panel_x+220,panel_y+76,input_info.keyboard_ready?C_ACCENT:C_ORANGE,1);
                    (void)draw_text("Pointer",panel_x+22,panel_y+112,C_TEXT,1);
                    (void)draw_text(input_info.mouse_ready?"Ready":"Not detected",panel_x+220,panel_y+112,input_info.mouse_ready?C_ACCENT:C_ORANGE,1);
                    char v[24]; format_u64(v,sizeof(v),input_info.dropped_events);
                    (void)draw_text("Dropped desktop events",panel_x+22,panel_y+148,C_TEXT,1);
                    (void)draw_text(v,panel_x+220,panel_y+148,C_MUTED,1);
                    (void)draw_text("USB tablet and advanced HID support are still planned driver work.",panel_x+22,panel_y+194,C_MUTED,1);
                } else if(settings_category==4u) {
                    (void)mint_fat16_get_volumes(&fat_volume_list);
                    (void)draw_text("Storage",panel_x+22,panel_y+24,C_TEXT,2);
                    char v[24]; format_u64(v,sizeof(v),fat_volume_list.count);
                    (void)draw_text("Mounted FAT16 volumes",panel_x+22,panel_y+76,C_TEXT,1);
                    (void)draw_text(v,panel_x+260,panel_y+76,C_ACCENT,1);
                    (void)draw_text("Mount policy",panel_x+22,panel_y+112,C_TEXT,1);
                    (void)draw_text("Read-only",panel_x+260,panel_y+112,C_ACCENT,1);
                    (void)draw_text("Persistent writes are not enabled. Disk Utility formats only explicitly confirmed blank media.",panel_x+22,panel_y+160,C_ORANGE,1);
                } else {
                    (void)draw_text("Privacy & Security",panel_x+22,panel_y+24,C_TEXT,2);
                    (void)draw_text("Userspace isolation",panel_x+22,panel_y+76,C_TEXT,1);
                    (void)draw_text("Separate address spaces and validated syscall copies",panel_x+235,panel_y+76,C_MUTED,1);
                    (void)draw_text("Network activity",panel_x+22,panel_y+112,C_TEXT,1);
                    (void)draw_text("No native TCP/IP stack enabled",panel_x+235,panel_y+112,C_MUTED,1);
                    (void)draw_text("Log handling",panel_x+22,panel_y+148,C_TEXT,1);
                    (void)draw_text("Bounded in-memory ring + serial export",panel_x+235,panel_y+148,C_MUTED,1);
                }
            }
        } else if (selected_app == APP_MONITOR) {
            mint_timespec64 uptime={0,0}; (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC,&uptime);
            (void)mint_input_get_info(&input_info); refresh_storage_data(); refresh_driver_catalog();
            (void)draw_text("System Monitor",x+pad+26,y+91,C_TEXT,2);
            (void)draw_text("Live kernel and desktop status; unavailable metrics are not guessed.",x+pad+26,y+120,C_MUTED,1);
            int32_t gx=x+pad+24, gy=y+151, gw=(w-2*pad-60)/3, gh=88;
            const char *labels[6]={"UPTIME","DESKTOP PROCESS","INPUT","STORAGE","FILESYSTEM","DRIVER CATALOG"};
            char vals[6][48];
            format_u64(vals[0],sizeof(vals[0]),(uint64_t)uptime.tv_sec);
            (void)copy_text(vals[0]+text_length(vals[0]),sizeof(vals[0])-text_length(vals[0])," seconds");
            format_u64(vals[1],sizeof(vals[1]),(uint64_t)mint_getpid());
            (void)copy_text(vals[2],sizeof(vals[2]),input_info.mouse_ready?"Keyboard + pointer":"Keyboard / pointer partial");
            format_u64(vals[3],sizeof(vals[3]),storage_snapshot.device_count);
            (void)copy_text(vals[3]+text_length(vals[3]),sizeof(vals[3])-text_length(vals[3])," devices");
            (void)copy_text(vals[4],sizeof(vals[4]),storage_snapshot.readonly_mount_active?"FAT16 read-only":"Not mounted / RAMFS");
            (void)copy_text(vals[5],sizeof(vals[5]),driver_catalog_ok?"Inventory available":"Inventory unavailable");
            for(uint32_t i=0;i<6u;i++){
                int32_t cx=gx+(int32_t)(i%3u)*(gw+8), cy=gy+(int32_t)(i/3u)*(gh+12);
                round_box(cx,cy,gw,gh,12,C_WINDOW);
                (void)fill(cx,cy,gw,3,i==4u&&!storage_snapshot.readonly_mount_active?C_ORANGE:C_ACCENT);
                (void)draw_text(labels[i],cx+13,cy+13,C_MUTED,1);
                (void)draw_text(vals[i],cx+13,cy+42,C_TEXT,1);
            }
            int32_t ey=gy+2*(gh+12)+8;
            round_box(gx,ey,w-2*pad-48,84,12,C_WINDOW);
            (void)draw_text("Input event counters",gx+16,ey+12,C_TEXT,1);
            char cnt[24]; format_u64(cnt,sizeof(cnt),input_info.keyboard_events);
            (void)draw_text("Keys",gx+18,ey+42,C_MUTED,1); (void)draw_text(cnt,gx+62,ey+42,C_TEXT,1);
            format_u64(cnt,sizeof(cnt),input_info.pointer_motion_events);
            (void)draw_text("Pointer moves",gx+210,ey+42,C_MUTED,1); (void)draw_text(cnt,gx+310,ey+42,C_TEXT,1);
            format_u64(cnt,sizeof(cnt),input_info.dropped_events);
            (void)draw_text("Dropped events",gx+450,ey+42,C_MUTED,1); (void)draw_text(cnt,gx+560,ey+42,C_TEXT,1);
            (void)draw_text("CPU utilization and per-process memory are not exposed by this kernel build.",gx,y+h-32,C_MUTED,1);
        } else if (selected_app == APP_STORAGE) {
            refresh_storage_data();
            (void)draw_text("Storage & Logs", x + pad + 26, y + 91, C_TEXT, 2);
            (void)draw_text(storage_snapshot.readonly_mount_active ?
                            "Read-only FAT16 validated; file writes remain disabled." :
                            "Read-only probe. Media writes disabled; persistence unavailable.",
                            x + pad + 390, y + 97, C_ORANGE, 1);
            const char *control_names[7] = {"Devices", "Controllers", "All", "Warn+", "Errors", "Refresh", "Export"};
            int32_t control_x = x + pad + 20;
            int32_t control_y = y + 116;
            for (uint32_t ci = 0; ci < 7u; ++ci) {
                uint32_t active = (ci == 0u && storage_view == 0u) ||
                                  (ci == 1u && storage_view == 1u) ||
                                  (ci == 2u && storage_view == 2u) ||
                                  (ci == 3u && storage_view == 3u) ||
                                  (ci == 4u && storage_view == 4u);
                int32_t bx = storage_control_x(control_x, ci);
                int32_t bw = storage_control_width(ci);
                round_box(bx, control_y, bw, 26, 6,
                          ci == 6u ? C_ACCENT : (active ? C_MENU_DARK : C_WINDOW));
                (void)draw_text(control_names[ci], bx + 6, control_y + 6,
                                ci == 6u ? 0xFFFFFFu : (active ? C_ACCENT : C_TEXT), 1);
            }
            char count[24], page[24];
            format_u64(count, sizeof(count), storage_snapshot.controller_count);
            (void)draw_text("Controllers", control_x, y + 157, C_MUTED, 1);
            (void)draw_text(count, control_x + 104, y + 157, C_TEXT, 1);
            format_u64(count, sizeof(count), storage_snapshot.device_count);
            (void)draw_text("Devices", control_x + 164, y + 157, C_MUTED, 1);
            (void)draw_text(count, control_x + 220, y + 157, C_TEXT, 1);
            (void)draw_text(storage_diag_ok ? "Probe complete" : "Diagnostic syscall failed",
                            control_x + 276, y + 157, storage_diag_ok ? C_ACCENT : C_RED, 1);
            char mounts_label[40]="RO mounts: ";
            format_u64(count,sizeof(count),storage_snapshot.readonly_mount_count);
            size_t ml=text_length(mounts_label);
            for(size_t j=0;count[j]&&ml+1u<sizeof(mounts_label);j++)mounts_label[ml++]=count[j];
            mounts_label[ml]='\0';
            (void)draw_text(mounts_label,control_x+388,y+157,C_ACCENT,1);
            uint32_t item_count = storage_view == 1u ? storage_snapshot.controller_count : storage_snapshot.device_count;
            uint32_t page_count = storage_view == 0u ? item_count : (item_count + 1u) / 2u;
            if (storage_page >= page_count) storage_page = page_count ? page_count - 1u : 0u;
            format_u64(page, sizeof(page), page_count ? storage_page + 1u : 0u);
            (void)draw_text("Page", x + w - pad - 142, y + 157, C_MUTED, 1);
            (void)draw_text(page, x + w - pad - 102, y + 157, C_TEXT, 1);
            round_box(x + w - pad - 78, y + 146, 30, 25, 5, C_WINDOW);
            (void)draw_text("<", x + w - pad - 68, y + 151, C_TEXT, 1);
            round_box(x + w - pad - 42, y + 146, 30, 25, 5, C_WINDOW);
            (void)draw_text(">", x + w - pad - 32, y + 151, C_TEXT, 1);

            if (storage_view == 0u) {
                (void)draw_text("Device identity and on-disk checks (read-only)", control_x, y + 181, C_MUTED, 1);
                if (storage_page < storage_snapshot.device_count && storage_page < MINT_STORAGE_MAX_DEVICES) {
                    MINT_STORAGE_DEVICE_INFO *d = &storage_snapshot.devices[storage_page];
                    int32_t dy = y + 198;
                    int32_t card_h = h - 232;
                    if (card_h > 310) card_h = 310;
                    if (card_h < 225) card_h = 225;
                    round_box(control_x, dy, w - 2 * pad - 36, card_h, 10, C_WINDOW);
                    (void)fill(control_x, dy, 3, card_h, app_colors[APP_STORAGE]);
                    (void)draw_text(d->name, control_x + 13, dy + 8, C_TEXT, 1);
                    (void)draw_text(d->model[0] ? d->model : "Model unavailable", control_x + 100, dy + 8, C_TEXT, 1);
                    (void)draw_text("Serial", control_x + 13, dy + 28, C_MUTED, 1);
                    (void)draw_text(d->serial[0] ? d->serial : "unavailable", control_x + 76, dy + 28, C_TEXT, 1);
                    format_u64(count, sizeof(count), d->sector_count);
                    (void)draw_text("Sectors", control_x + 13, dy + 48, C_MUTED, 1);
                    (void)draw_text(count, control_x + 82, dy + 48, C_TEXT, 1);
                    format_u64(count, sizeof(count), d->capacity_bytes / (1024u * 1024u));
                    (void)draw_text("Capacity", control_x + 245, dy + 48, C_MUTED, 1);
                    (void)draw_text(count, control_x + 322, dy + 48, C_TEXT, 1);
                    (void)draw_text("MiB", control_x + 372, dy + 48, C_MUTED, 1);
                    (void)draw_text(storage_capacity_source(d->capacity_source), control_x + 412, dy + 48, C_MUTED, 1);
                    format_u64(count, sizeof(count), d->reported_lba28_sectors);
                    (void)draw_text("LBA28 raw", control_x + 13, dy + 68, C_MUTED, 1);
                    (void)draw_text(count, control_x + 89, dy + 68, C_TEXT, 1);
                    format_u64(count, sizeof(count), d->reported_lba48_sectors);
                    (void)draw_text("LBA48 raw", control_x + 245, dy + 68, C_MUTED, 1);
                    (void)draw_text(count, control_x + 322, dy + 68, C_TEXT, 1);
                    (void)draw_text(d->read_status[0] ? d->read_status : "sector read status unavailable",
                                    control_x + 13, dy + 88, d->read_lba0_ok ? C_ACCENT : C_ORANGE, 1);
                    format_u64(count, sizeof(count), d->partition_count);
                    (void)draw_text("MBR entries", control_x + 13, dy + 108, C_MUTED, 1);
                    (void)draw_text(count, control_x + 94, dy + 108, C_TEXT, 1);
                    (void)draw_text(storage_table_state(d->partition_table_state), control_x + 145, dy + 108, C_MUTED, 1);
                    char sig_a[5], sig_b[5], disk_id_text[24];
                    static const char sig_digits[] = "0123456789ABCDEF";
                    sig_a[0] = sig_b[0] = '0'; sig_a[1] = sig_b[1] = 'x';
                    sig_a[2] = sig_digits[d->lba0_signature[0] >> 4]; sig_a[3] = sig_digits[d->lba0_signature[0] & 15u]; sig_a[4] = '\0';
                    sig_b[2] = sig_digits[d->lba0_signature[1] >> 4]; sig_b[3] = sig_digits[d->lba0_signature[1] & 15u]; sig_b[4] = '\0';
                    format_hex64(disk_id_text, sizeof(disk_id_text), d->mbr_disk_id);
                    (void)draw_text("LBA0 sig", control_x + 13, dy + 128, C_MUTED, 1);
                    (void)draw_text(sig_a, control_x + 79, dy + 128, C_TEXT, 1);
                    (void)draw_text(sig_b, control_x + 113, dy + 128, C_TEXT, 1);
                    (void)draw_text(d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_MATCH ? "repeat MATCH" :
                                    d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_DIFFERS ? "repeat DIFFERS" :
                                    d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_FAILED ? "repeat FAILED" : "repeat unknown",
                                    control_x + 180, dy + 128,
                                    d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_MATCH ? C_ACCENT : C_ORANGE, 1);
                    (void)draw_text("Disk ID", control_x + 13, dy + 148, C_MUTED, 1);
                    (void)draw_text(disk_id_text, control_x + 71, dy + 148, C_TEXT, 1);
                    (void)draw_text(d->developer_disk_id_match ? "Developer image marker matched" : "Image marker mismatch / unknown",
                                    control_x + 13, dy + 166, d->developer_disk_id_match ? C_ACCENT : C_ORANGE, 1);
                    if (d->partition_count && d->partition_count <= MINT_STORAGE_MAX_PARTITIONS) {
                        MINT_STORAGE_PARTITION_INFO *part = &d->partitions[0];
                        char hex_type[16], start_text[24], length_text[24];
                        format_hex64(hex_type, sizeof(hex_type), part->type);
                        format_u64(start_text, sizeof(start_text), part->start_lba);
                        format_u64(length_text, sizeof(length_text), part->sector_count);
                        (void)draw_text("Partition 1", control_x + 13, dy + 186, C_MUTED, 1);
                        (void)draw_text(hex_type, control_x + 104, dy + 186, C_TEXT, 1);
                        (void)draw_text("LBA", control_x + 152, dy + 186, C_MUTED, 1);
                        (void)draw_text(start_text, control_x + 187, dy + 186, C_TEXT, 1);
                        (void)draw_text("Sectors", control_x + 285, dy + 186, C_MUTED, 1);
                        (void)draw_text(length_text, control_x + 344, dy + 186, C_TEXT, 1);
                    } else {
                        (void)draw_text(d->filesystem_status, control_x + 13, dy + 186, C_ORANGE, 1);
                    }
                    int32_t footer_a = card_h - 31;
                    int32_t footer_b = card_h - 14;
                    (void)draw_text("Write: DISABLED; no media writes attempted", control_x + 13, dy + footer_a, C_ORANGE, 1);
                    (void)draw_text(storage_snapshot.readonly_mount_active ?
                                    "Mount: READ-ONLY; persistence writes still disabled" :
                                    "Mount: NOT MOUNTED; session defaults remain active",
                                    control_x + 13, dy + footer_b,
                                    storage_snapshot.readonly_mount_active ? C_ACCENT : C_ORANGE, 1);
                }
                if (!storage_snapshot.device_count)
                    (void)draw_text("No ATA disk was identified. Inspect Controllers and export COM1 diagnostics.",
                                    control_x, y + 218, C_ORANGE, 1);
            } else if (storage_view == 1u) {
                (void)draw_text("PCI AHCI controller inventory", control_x, y + 181, C_MUTED, 1);
                for (uint32_t i = 0; i < 2u; ++i) {
                    uint32_t idx = storage_page * 2u + i;
                    if (idx >= storage_snapshot.controller_count || idx >= MINT_STORAGE_MAX_CONTROLLERS) break;
                    MINT_STORAGE_CONTROLLER_INFO *c = &storage_snapshot.controllers[idx];
                    int32_t cy = y + 198 + (int32_t)i * 137;
                    round_box(control_x, cy, w - 2 * pad - 36, 128, 8, C_WINDOW);
                    (void)fill(control_x, cy, 3, 128, app_colors[APP_STORAGE]);
                    char vid[16], did[16], bar[24], ports[24], bus[16], dev[16], fn[16];
                    format_hex64(vid, sizeof(vid), c->vendor_id);
                    format_hex64(did, sizeof(did), c->device_id);
                    format_hex64(bar, sizeof(bar), c->abar);
                    format_hex64(ports, sizeof(ports), c->port_implemented);
                    format_u64(bus, sizeof(bus), c->bus);
                    format_u64(dev, sizeof(dev), c->device);
                    format_u64(fn, sizeof(fn), c->function);
                    (void)draw_text("AHCI controller", control_x + 13, cy + 9, C_TEXT, 1);
                    (void)draw_text(storage_controller_state(c->state), control_x + 180, cy + 9,
                                    c->state == MINT_STORAGE_CTRL_READY ? C_ACCENT : C_ORANGE, 1);
                    (void)draw_text("PCI B:D.F", control_x + 13, cy + 31, C_MUTED, 1);
                    (void)draw_text(bus, control_x + 91, cy + 31, C_TEXT, 1);
                    (void)draw_text(":", control_x + 122, cy + 31, C_TEXT, 1);
                    (void)draw_text(dev, control_x + 132, cy + 31, C_TEXT, 1);
                    (void)draw_text(".", control_x + 155, cy + 31, C_TEXT, 1);
                    (void)draw_text(fn, control_x + 165, cy + 31, C_TEXT, 1);
                    (void)draw_text("VID:DID", control_x + 205, cy + 31, C_MUTED, 1);
                    (void)draw_text(vid, control_x + 268, cy + 31, C_TEXT, 1);
                    (void)draw_text(":", control_x + 315, cy + 31, C_TEXT, 1);
                    (void)draw_text(did, control_x + 325, cy + 31, C_TEXT, 1);
                    (void)draw_text("BAR5", control_x + 13, cy + 53, C_MUTED, 1);
                    (void)draw_text(bar, control_x + 76, cy + 53, C_TEXT, 1);
                    (void)draw_text("PI", control_x + 260, cy + 53, C_MUTED, 1);
                    (void)draw_text(ports, control_x + 287, cy + 53, C_TEXT, 1);
                    (void)draw_text(c->reason[0] ? c->reason : "no controller failure detail recorded",
                                    control_x + 13, cy + 77, C_TEXT, 1);
                }
                if (!storage_snapshot.controller_count)
                    (void)draw_text("No PCI AHCI controller found in the bounded PCI inventory.",
                                    control_x, y + 211, C_ORANGE, 1);
            } else {
                const char *log_title = storage_view == 2u ? "Recent log records" :
                                        storage_view == 3u ? "Warnings and errors" : "Errors only";
                (void)draw_text(log_title, control_x, y + 181, C_MUTED, 1);
                int32_t ly = y + 205;
                for (uint32_t i = 0; i < storage_log_count && i < 9u; ++i) {
                    MINT_LOG_RECORD *r = &storage_log_records[i];
                    char seq[24]; format_u64(seq, sizeof(seq), r->sequence);
                    (void)draw_text(seq, control_x, ly, C_MUTED, 1);
                    (void)draw_text(storage_severity_name(r->severity), control_x + 54, ly,
                                    r->severity >= MINT_LOG_ERROR ? C_RED :
                                    r->severity >= MINT_LOG_WARN ? C_ORANGE : C_ACCENT, 1);
                    (void)draw_text(storage_component_name(r->component), control_x + 118, ly, C_TEXT, 1);
                    (void)draw_text(r->message, control_x + 214, ly, C_TEXT, 1);
                    ly += 25;
                }
                if (!storage_log_count)
                    (void)draw_text("No records match this filter yet.", control_x, ly, C_MUTED, 1);
            }
            if (storage_view != 0u)
                (void)draw_text(storage_snapshot.readonly_mount_active ?
                                "Write: DISABLED  |  FAT16: READ-ONLY  |  Settings remain session-only" :
                                "Write: DISABLED  |  Mount: NOT MOUNTED  |  persistence unavailable",
                                control_x, y + h - 34, C_ORANGE, 1);
        } else if (selected_app == APP_DRIVERS) {
            refresh_driver_catalog();
            (void)draw_text("Hardware & Drivers", x + pad + 26, y + 91, C_TEXT, 2);
            (void)draw_text("Discovery is not the same as driver support.", x + pad + 26, y + 122, C_MUTED, 1);
            uint32_t total_pages = (driver_catalog.count + 7u) / 8u;
            if (!total_pages) total_pages = 1u;
            if (driver_page >= total_pages) driver_page = total_pages - 1u;
            char page_label[32], page_number[12];
            format_u64(page_number, sizeof(page_number), driver_page + 1u);
            page_label[0] = 'P'; page_label[1] = 'a'; page_label[2] = 'g'; page_label[3] = 'e'; page_label[4] = ' ';
            size_t p = 5u; for (size_t j=0; page_number[j] && p+1u<sizeof(page_label); ++j) page_label[p++]=page_number[j];
            page_label[p++]='/';
            char pages_text[12]; format_u64(pages_text, sizeof(pages_text), total_pages);
            for (size_t j=0; pages_text[j] && p+1u<sizeof(page_label); ++j) page_label[p++]=pages_text[j]; page_label[p]='\0';
            (void)draw_text(page_label, x + w - pad - 142, y + 96, C_MUTED, 1);
            round_box(x + w - pad - 78, y + 86, 30, 25, 5, C_WINDOW);
            (void)draw_text("<", x + w - pad - 68, y + 91, C_TEXT, 1);
            round_box(x + w - pad - 42, y + 86, 30, 25, 5, C_WINDOW);
            (void)draw_text(">", x + w - pad - 32, y + 91, C_TEXT, 1);
            int32_t gx = x + pad + 18, gy = y + 143;
            int32_t card_w = (w - 2 * pad - 54) / 2;
            for (uint32_t ci = 0; ci < 8u; ++ci) {
                uint32_t idx = driver_page * 8u + ci;
                if (!driver_catalog_ok || idx >= driver_catalog.count) break;
                MINT_DRIVER_RECORD *r = &driver_catalog.records[idx];
                int32_t cx = gx + (int32_t)(ci % 2u) * (card_w + 18);
                int32_t cy = gy + (int32_t)(ci / 2u) * 62;
                round_box(cx, cy, card_w, 54, 8, C_WINDOW);
                (void)fill(cx, cy, 3, 54, r->state == MINT_DRIVER_ACTIVE || r->state == MINT_DRIVER_READ_ONLY ? C_ACCENT : C_ORANGE);
                (void)draw_text(r->name, cx + 12, cy + 6, C_TEXT, 1);
                (void)draw_text(driver_state_label(r->state), cx + card_w - 118, cy + 6,
                                r->state == MINT_DRIVER_ACTIVE || r->state == MINT_DRIVER_READ_ONLY ? C_ACCENT : C_ORANGE, 1);
                (void)draw_text(r->detail, cx + 12, cy + 29, C_MUTED, 1);
            }
            if (!driver_catalog_ok)
                (void)draw_text("Driver catalog is unavailable; inspect COM1 logs.", gx, gy, C_ORANGE, 1);
            (void)draw_text("No macOS kexts are used: MintOS26 uses its own freestanding driver interfaces.",
                            x + pad + 26, y + h - 34, C_MUTED, 1);
        } else if (selected_app == APP_DISKS) {
            refresh_storage_data();
            (void)draw_text("Disk Utility",x+pad+26,y+91,C_TEXT,2);
            (void)draw_text("Initialize an empty disposable SATA disk. This erases the selected disk.",x+pad+26,y+121,C_MUTED,1);
            int32_t gx=x+pad+24, gy=y+153, gw=w-2*pad-48;
            round_box(gx,gy,gw,storage_snapshot.device_count?((int32_t)storage_snapshot.device_count*62+16):74,12,C_WINDOW);
            (void)draw_text("Detected disks",gx+14,gy+10,C_MUTED,1);
            for(uint32_t i=0;i<storage_snapshot.device_count && i<MINT_STORAGE_MAX_DEVICES && i<5u;i++){
                MINT_STORAGE_DEVICE_INFO *d=&storage_snapshot.devices[i]; int32_t ry=gy+34+(int32_t)i*62;
                round_box(gx+8,ry,gw-16,53,8,disk_selected_device==i?C_MENU_DARK:C_CONTENT);
                (void)draw_text(d->name,gx+20,ry+8,disk_selected_device==i?C_ACCENT:C_TEXT,1);
                (void)draw_text(d->model[0]?d->model:"Unknown model",gx+85,ry+8,C_TEXT,1);
                if(gw>760) (void)draw_text(d->serial,gx+270,ry+8,C_MUTED,1);
                char capacity[24]; format_u64(capacity,sizeof(capacity),d->capacity_bytes/(1024u*1024u));
                (void)draw_text(capacity,gx+gw-160,ry+8,C_TEXT,1); (void)draw_text("MiB",gx+gw-115,ry+8,C_MUTED,1);
                if(gw<=760) {
                    (void)draw_text("Serial:",gx+20,ry+30,C_MUTED,1);
                    (void)draw_text(d->serial,gx+74,ry+30,C_MUTED,1);
                } else {
                    (void)draw_text(d->mbr_signature_ok?"Partition table exists":"No MBR signature; guard checks before format",gx+20,ry+30,d->mbr_signature_ok?C_MUTED:C_ORANGE,1);
                }
            }
            int32_t by=gy+44+(int32_t)(storage_snapshot.device_count?storage_snapshot.device_count:1u)*62;
            round_box(gx,by,244,34,9,C_ACCENT);
            (void)draw_text("Initialize selected blank disk",gx+13,by+10,0xFFFFFFu,1);
            (void)draw_text("32 MiB – 2 GiB only; MBR must be all zeroes.",gx+258,by+10,C_MUTED,1);
            if(disk_format_prompt){
                int32_t py=by+48; round_box(gx,py,gw,104,11,C_WINDOW);
                (void)draw_text("Destructive confirmation",gx+14,py+10,C_ORANGE,1);
                (void)draw_text("Type the exact phrase below, then press Enter:",gx+14,py+34,C_TEXT,1);
                char expected[32]="FORMAT "; size_t ep=7;
                if(disk_selected_device<storage_snapshot.device_count) { const char *dn=storage_snapshot.devices[disk_selected_device].name; for(size_t j=0;dn[j] && ep+1u<sizeof(expected);j++) expected[ep++]=dn[j]; } expected[ep]='\0';
                (void)draw_text(expected,gx+14,py+58,C_ACCENT,1);
                round_box(gx+170,py+52,gw-190,30,6,C_CONTENT);
                (void)draw_text(disk_confirmation[0]?disk_confirmation:"(type here)",gx+182,py+61,C_TEXT,1);
            }
            if(disk_result[0]) (void)draw_text(disk_result,gx, y+h-34,
                (disk_result[0]=='S')?C_ACCENT:C_ORANGE,1);
            else (void)draw_text("Other file writes remain disabled; the new volume mounts read-only.",gx,y+h-34,C_MUTED,1);
        } else if (selected_app == APP_NETWORK) {
            refresh_driver_catalog();
            (void)draw_text("Network", x + pad + 26, y + 91, C_TEXT, 2);
            (void)draw_text("Hardware inventory is read-only; no packets are sent.", x + pad + 26, y + 122, C_MUTED, 1);
            int32_t nx = x + pad + 24, ny = y + 150;
            round_box(nx, ny, w - 2 * pad - 48, 72, 10, C_WINDOW);
            (void)fill(nx, ny, 3, 72, C_BLUE);
            (void)draw_text("Ethernet controller", nx + 18, ny + 10, C_TEXT, 1);
            (void)draw_text("PCI discovery only", nx + 218, ny + 10, C_ORANGE, 1);
            (void)draw_text("Native NIC packet driver is not implemented in this release.", nx + 18, ny + 38, C_MUTED, 1);
            round_box(nx, ny + 88, w - 2 * pad - 48, 72, 10, C_WINDOW);
            (void)fill(nx, ny + 88, 3, 72, C_ORANGE);
            (void)draw_text("TCP/IP services", nx + 18, ny + 98, C_TEXT, 1);
            (void)draw_text("NOT IMPLEMENTED", nx + 218, ny + 98, C_ORANGE, 1);
            (void)draw_text("Sockets, DHCP, DNS and network requests are unavailable.", nx + 18, ny + 126, C_MUTED, 1);
            round_box(nx, ny + 176, w - 2 * pad - 48, 72, 10, C_WINDOW);
            (void)fill(nx, ny + 176, 3, 72, C_ACCENT);
            (void)draw_text("Safe next step", nx + 18, ny + 186, C_TEXT, 1);
            (void)draw_text("Start with a bounded E1000 driver and RX-only packet tests in a disposable VM.",
                            nx + 18, ny + 214, C_MUTED, 1);
            (void)draw_text("Network hardware may be listed by PCI even while all network services remain unavailable.",
                            x + pad + 26, y + h - 34, C_MUTED, 1);
        } else if (selected_app == APP_ABOUT_OS) {
            int32_t ax=x+pad+22, ay=y+79, aw=w-2*pad-44;
            app_icon(APP_ABOUT_OS,ax+6,ay+10,82);
            (void)draw_text("MintOS26",ax+108,ay+8,C_TEXT,3);
            (void)draw_text("Version 26.5.008.0  \xE2\x80\xA2  x86-64",ax+110,ay+55,C_ACCENT,1);
            (void)draw_text("The operating system, kernel and hardware services.",ax+110,ay+82,C_MUTED,1);
            round_box(ax+aw-164,ay+18,150,30,9,C_MENU_DARK);
            (void)draw_text(show_changelog==0u?"Changelog  >":show_changelog==1u?"About details":"Version list",
                            ax+aw-154,ay+27,C_ACCENT,1);
            int32_t row_y=ay+119, row_h=(h<620)?45:52, gap=8;
            if(show_changelog==1u) {
                const char *versions[8]={"26.5.008.0  - VirtualBox E1000 RX loopback and descriptor diagnostics","26.5.007.0  - E1000 TX timeout isolation","26.5.006.0  - E1000 loopback diagnostics and terminal prompt","26.5.005.0  - E1000 driver foundation","26.5.004.0  - Driver foundation release","26.5.003.0  - PNG assets and VFS telemetry","26.5.002.0  - Multi-volume storage","26.5.001.0  - MintDE 1.0 milestone"};
                int32_t list_h=(h<620)?36:40, list_gap=6;
                for(uint32_t ci=0;ci<8u;ci++){ round_box(ax,row_y,aw,list_h,9,C_WINDOW); (void)draw_text(versions[ci],ax+14,row_y+11,C_TEXT,1); (void)draw_text(">",ax+aw-26,row_y+11,C_ACCENT,1); row_y+=list_h+list_gap; }
            } else if(show_changelog==2u) {
                static const char *const headings[8]={"26.5.008.0 - current","26.5.007.0","26.5.006.0","26.5.005.0","26.5.004.0","26.5.003.0","26.5.002.0","26.5.001.0"};
                static const char *const notes[8][4]={
                    {"Use RCTL.LBM_TCVR for VirtualBox's emulated internal loopback path","Snapshot the last transmitted descriptor rather than the next ring slot","Include RX descriptor index, length, errors and buffer address","E1000 remains opt-in; no TCP/IP or external packet transmission"},
                    {"Cleared CTRL.LRST during reset and loopback setup","Added TX descriptor command/length/DMA buffer diagnostics","Added TX/RX ring base and length plus PCI command capture","E1000 remains opt-in; no TCP/IP or external packet transmission"},
                    {"E1000 internal PHY/MAC loopback now reports the exact failing phase","Bounded PHY MDIC access and saved register snapshots added","Terminal history distinguishes root#mintos commands from output","DMA remains opt-in; no TCP/IP or external packet transmission"},
                    {"Fixed terminal volume selection stack overflow","Ring-3 faults now terminate the offending process","Read-only PCI BAR, IRQ and resource inventory","Driver catalog expanded; AHCI remains read-only"},
                    {"Separate PNG app icons and bounded RGBA draw syscall","VFS mount table and per-device block I/O telemetry","PCI / AHCI diagnostics improved","Generic media writes remain gated"},
                    {"Multi-volume FAT16 read-only mount registry","MintDE menus, About pages and terminal diagnostics","Verified disk images and blank-disk formatting","General persistent file writes still disabled"},
                    {"Separate About MintOS and About MintDE","Safe UEFI Desktop / MintDE boot selector","Read-only Files navigation and driver inventory","Cooperative ring-3 scheduler and syscall smoke tests"},
                    {"Disk Utility and explicit blank-media confirmation","Read-only FAT16 validation and volume navigation","Storage & Logs and bounded kernel log ring","UEFI Desktop retained as normal boot path"}
                };
                uint32_t ver=changelog_version<8u?changelog_version:0u;
                round_box(ax,row_y,aw,36,9,C_MENU_DARK); (void)draw_text(headings[ver],ax+14,row_y+10,C_ACCENT,1); row_y+=44;
                for(uint32_t ci=0;ci<4u;ci++){ round_box(ax,row_y,aw,row_h,10,C_WINDOW); (void)draw_text(notes[ver][ci],ax+14,row_y+13,C_TEXT,1); row_y+=row_h+gap; }
            } else {
                const char *labels[4]={"BOOT MODEL","MEMORY & PROTECTION","KERNEL SERVICES","STORAGE STATUS"};
                const char *values[4]={
                    "UEFI loader -> ExitBootServices -> MintOS kernel",
                    "NX/W^X, guarded stacks, paging and isolated user address spaces",
                    "GDT/TSS, IDT, PIT scheduler, ELF64 ring-3 and validated syscalls",
                    storage_snapshot.readonly_mount_active ? "FAT16 mounted read-only; general file writes remain disabled" : "No persistent volume mounted; session files remain in RAM"
                };
                for(uint32_t i=0;i<4u;i++){
                    round_box(ax,row_y,aw,row_h,10,C_WINDOW);
                    (void)fill(ax,row_y,3,row_h,(i==3u&&!storage_snapshot.readonly_mount_active)?C_ORANGE:C_ACCENT);
                    (void)draw_text(labels[i],ax+14,row_y+6,C_MUTED,1);
                    (void)draw_text(values[i],ax+156,row_y+6,C_TEXT,1);
                    row_y+=row_h+gap;
                }
                (void)draw_text("Recovery: restart the VM and choose UEFI Desktop in the boot selector.",ax,y+h-34,C_MUTED,1);
            }
        } else if (selected_app == APP_ABOUT_MINTDE) {
            int32_t ax=x+pad+22, ay=y+79, aw=w-2*pad-44;
            app_icon(APP_ABOUT_MINTDE,ax+6,ay+10,82);
            (void)draw_text("MintDE",ax+108,ay+8,C_TEXT,3);
            (void)draw_text("Version 1.0.7  \xE2\x80\xA2  Native desktop environment",ax+110,ay+55,C_ACCENT,1);
            (void)draw_text("The desktop shell is versioned separately from MintOS.",ax+110,ay+82,C_MUTED,1);
            round_box(ax+aw-164,ay+18,150,30,9,C_MENU_DARK);
            (void)draw_text(show_changelog==0u?"Changelog  >":show_changelog==1u?"About details":"Version list",
                            ax+aw-154,ay+27,C_ACCENT,1);
            int32_t row_y=ay+119, row_h=(h<620)?45:52, gap=8;
            if(show_changelog==1u) {
                const char *versions[8]={"MintDE 1.0.7  - VirtualBox loopback and RX diagnostics","MintDE 1.0.6  - E1000 loopback failure diagnostics","MintDE 1.0.5  - E1000 loopback diagnostics and terminal prompt","MintDE 1.0.4  - E1000 driver UI","MintDE 1.0.3  - Dock, menus and window polish","MintDE 1.0.2  - PNG icons and glass","MintDE 1.0.1  - Workspace polish","MintDE 1.0.0  - Native desktop milestone"};
                int32_t list_h=(h<620)?36:40, list_gap=6;
                for(uint32_t ci=0;ci<8u;ci++){ round_box(ax,row_y,aw,list_h,9,C_WINDOW); (void)draw_text(versions[ci],ax+14,row_y+11,C_TEXT,1); (void)draw_text(">",ax+aw-26,row_y+11,C_ACCENT,1); row_y+=list_h+list_gap; }
            } else if(show_changelog==2u) {
                static const char *const headings[8]={"MintDE 1.0.7 - current","MintDE 1.0.6","MintDE 1.0.5","MintDE 1.0.4","MintDE 1.0.3","MintDE 1.0.2","MintDE 1.0.1","MintDE 1.0.0"};
                static const char *const notes[8][4]={
                    {"Uses the VirtualBox emulated loopback buffer path for RX self-test","Reports the actual TX descriptor and full RX descriptor state","Terminal and COM1 keep the prompt and failure details distinct","External packet transmission and TCP/IP remain disabled"},
                    {"Cleared the link-reset bit before submitting the diagnostic frame","Terminal shows TX command, descriptor length and DMA ring metadata","Loopback failure state is preserved before normal mode restoration","External packet transmission remains disabled"},
                    {"E1000 loopback stages and register state shown after failure","Terminal input history uses root#mintos > prompt","Bounded PHY MDIC access and loopback restoration added","External packet transmission remains disabled"},
                    {"Terminal disk selection no longer uses a huge stack snapshot","PCI BAR/IRQ resources are visible for driver bring-up","Ring-3 exceptions are contained to the affected process","New diagnostics commands: resources, io, mounts and drivers"},
                    {"PNG app icons rendered through a bounded bitmap syscall","Settings drop-downs and fixed content bounds","Corrected cursor damage and dock redraw ordering","Alpha blended panels and window resizing retained"},
                    {"Launchpad is accessible from the dock","Separate About panels and hidden changelog entry","FAT16 per-volume selection in Files","Resizable window and bounded terminal commands"},
                    {"Initial MintDE shell with a draggable window and menu bar","Separate About MintOS and About MintDE identity","UEFI Desktop remains the recovery path","Persistent disk writes remain disabled"},
                    {"Initial native ring-3 desktop shell","Menu bar, dock and basic desktop window","Separate MintDE version from MintOS","Experimental one-way kernel handoff"}
                };
                uint32_t ver=changelog_version<8u?changelog_version:0u;
                round_box(ax,row_y,aw,36,9,C_MENU_DARK); (void)draw_text(headings[ver],ax+14,row_y+10,C_ACCENT,1); row_y+=44;
                for(uint32_t ci=0;ci<4u;ci++){ round_box(ax,row_y,aw,row_h,10,C_WINDOW); (void)draw_text(notes[ver][ci],ax+14,row_y+13,C_TEXT,1); row_y+=row_h+gap; }
            } else {
                const char *labels[4]={"WINDOW SYSTEM","INPUT","BUILT-IN APPS","RELEASE STATUS"};
                const char *values[4]={
                    "Mouse-driven windows, rounded controls, menus and dock",
                    "Kernel input event service with PS/2 keyboard and mouse",
                    "Files, Notes, Settings, System Monitor and Storage tools",
                    "1.0 milestone; direct kernel handoff remains experimental"
                };
                for(uint32_t i=0;i<4u;i++){
                    round_box(ax,row_y,aw,row_h,10,C_WINDOW);
                    (void)fill(ax,row_y,3,row_h,i==3u?C_ORANGE:C_BLUE);
                    (void)draw_text(labels[i],ax+14,row_y+6,C_MUTED,1);
                    (void)draw_text(values[i],ax+156,row_y+6,C_TEXT,1);
                    row_y+=row_h+gap;
                }
                (void)draw_text("MintDE is a native ring-3 process; it is not the UEFI Desktop.",ax,y+h-34,C_MUTED,1);
            }
        }
    }
    if (app_window_open && selected_app == APP_SETTINGS && !window_minimized && open_settings_dropdown)
        paint_settings_dropdown();
    if (app_window_open && !window_minimized && !window_maximized) {
        int32_t gx=x+w-22, gy=y+h-22;
        (void)fill(gx+5,gy+14,9,1,C_MUTED); (void)fill(gx+9,gy+10,5,1,C_MUTED);
        (void)fill(gx+13,gy+6,1,1,C_MUTED);
    }
}

static void paint_shell(void) {
    apply_palette();
    (void)mint_ui_frame_begin();
    int32_t w = (int32_t)display.width, h = (int32_t)display.height;
    if (w < 640 || h < 480) {
        (void)fill(0, 0, w, h, C_WALL);
        (void)draw_text("MintDE - Mint Desktop Environment", 24, 28, C_WINDOW, 2);
        (void)draw_text("Display is too small for this layout", 24, 74, C_WINDOW, 1);
        (void)mint_ui_frame_end();
        return;
    }
    cached_margin=w/24; if(cached_margin<22) cached_margin=22;
    paint_wallpaper(); paint_menu_bar();
    int32_t win_w=w*78/100, win_h=h*78/100;
    if(win_w>w-2*cached_margin-16) win_w=w-2*cached_margin-16;
    if(win_h>h-60) win_h=h-60;
    if(win_w<520) win_w=w-2*cached_margin;
    if(win_h<450) win_h=h-64;
    if (app_window_open && selected_app == APP_SETTINGS && !window_maximized) {
        int32_t settings_min_h = h - 56;
        if (settings_min_h > 650) settings_min_h = 650;
        if (win_h < settings_min_h) win_h = settings_min_h;
    }
    if(!window_maximized && custom_win_w>0 && custom_win_h>0) {
        int32_t min_w=w<660?w-16:560, min_h=h<520?h-56:420;
        if (app_window_open && selected_app == APP_SETTINGS) {
            int32_t settings_min_h = h - 56; if (settings_min_h > 650) settings_min_h = 650;
            if (settings_min_h > min_h) min_h = settings_min_h;
        }
        if(custom_win_w<min_w)custom_win_w=min_w; if(custom_win_h<min_h)custom_win_h=min_h;
        if(custom_win_w>w-16)custom_win_w=w-16; if(custom_win_h>h-56)custom_win_h=h-56;
        win_w=custom_win_w; win_h=custom_win_h;
    }
    if(window_maximized){
        win_w=w-16; win_h=h-52; cached_win_x=8; cached_win_y=46;
    } else {
        cached_win_x=(w-win_w)/2+window_offset_x;
        cached_win_y=54+window_offset_y;
    }
    if(cached_win_x<8) cached_win_x=8;
    if(cached_win_y<44) cached_win_y=44;
    if(cached_win_x+win_w>w-8) cached_win_x=w-win_w-8;
    if(cached_win_y+win_h>h-8) cached_win_y=h-win_h-8;
    cached_win_w=win_w; cached_win_h=win_h;
    /* Capture wallpaper/menu chrome before drawing the active window. This is
       the source for bounded cursor and window damage restoration. */
    background_snapshot_ready=mint_ui_capture_background()==0;
    if(!window_minimized && (library_open || app_window_open)) paint_home_window(cached_win_x,cached_win_y,win_w,win_h);
    paint_menu_dropdown(); paint_dock(); paint_cursor();
    last_clock_second=-1;
    (void)mint_ui_frame_end();
}

static void move_window_to(int32_t wanted_x, int32_t wanted_y) {
    if (window_maximized || window_minimized) return;
    int32_t old_x = cached_win_x, old_y = cached_win_y;
    int32_t max_x = (int32_t)display.width - cached_win_w - 8;
    int32_t max_y = (int32_t)display.height - cached_win_h - 8;
    if (wanted_x < 8) wanted_x = 8;
    if (wanted_y < 44) wanted_y = 44;
    if (wanted_x > max_x) wanted_x = max_x;
    if (wanted_y > max_y) wanted_y = max_y;
    if (wanted_x == old_x && wanted_y == old_y) return;
    if (!background_snapshot_ready || mint_ui_frame_begin() != 0) {
        window_offset_x += wanted_x - old_x;
        window_offset_y += wanted_y - old_y;
        paint_shell();
        return;
    }
    /* Restore shadow and content from the base scene; 12 px includes rounded
       corners and the subtle drop shadow around the window. */
    (void)mint_ui_restore_rect(old_x - 12, old_y - 12,
                               cached_win_w + 24, cached_win_h + 24);
    cached_win_x = wanted_x;
    cached_win_y = wanted_y;
    window_offset_x += wanted_x - old_x;
    window_offset_y += wanted_y - old_y;
    paint_home_window(cached_win_x, cached_win_y, cached_win_w, cached_win_h);
    paint_dock();
    paint_cursor();
    (void)mint_ui_frame_end();
}

static void update_clock(void) {
    mint_timespec64 now = {0,0};
    if (mint_clock_gettime(MINT_CLOCK_MONOTONIC, &now) != 0 || now.tv_sec == last_clock_second) return;
    last_clock_second = now.tv_sec;
    (void)mint_ui_frame_begin();
    int32_t w = (int32_t)display.width;
    (void)fill(w - 455, 5, 135, 31, C_MENU);
    char line_buf[20] = "UPTIME "; char rev[12]; size_t n = 0, k = 7;
    uint64_t v = (uint64_t)now.tv_sec;
    do { rev[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < sizeof(rev));
    while (n && k < sizeof(line_buf) - 2u) line_buf[k++] = rev[--n];
    line_buf[k++] = 'S'; line_buf[k] = '\0';
    (void)draw_text(line_buf, w - 450, 16, C_TEXT, 1);
    paint_cursor();
    (void)mint_ui_frame_end();
}

static const char *driver_state_label(uint32_t state) {
    switch (state) {
    case MINT_DRIVER_ACTIVE: return "ACTIVE";
    case MINT_DRIVER_READ_ONLY: return "READ-ONLY";
    case MINT_DRIVER_DIAGNOSTIC_ONLY: return "DIAG ONLY";
    case MINT_DRIVER_DEVICE_FOUND: return "FOUND";
    case MINT_DRIVER_NOT_IMPLEMENTED: return "NOT BUILT";
    case MINT_DRIVER_UNAVAILABLE: return "UNAVAILABLE";
    default: return "UNKNOWN";
    }
}

static void refresh_driver_catalog(void) {
    driver_catalog_ok = mint_driver_catalog_get(&driver_catalog) == 0 &&
                        driver_catalog.version == MINT_DRIVER_CATALOG_VERSION &&
                        driver_catalog.count <= MINT_DRIVER_CATALOG_MAX_RECORDS;
    if (!driver_catalog_ok) {
        uint8_t *p = (uint8_t *)&driver_catalog;
        for (size_t i = 0; i < sizeof(driver_catalog); ++i) p[i] = 0u;
    }
}

static void open_app(uint32_t app) {
    if (app >= APP_COUNT) return;
    if (app == APP_LAUNCHPAD) {
        selected_app=APP_LAUNCHPAD; library_open=1u; app_window_open=0u;
        window_minimized=0u; window_focused=0u; active_menu=-1; show_changelog=0u;
        paint_shell(); return;
    }
    selected_app = app; app_window_open = 1; library_open = 0; window_minimized = 0; window_focused = 1u;
    open_settings_dropdown=0u; show_changelog=0u;

    if (app == APP_SETTINGS) {
        (void)mint_input_get_info(&input_info);
        (void)mint_desktop_settings_get(&desktop_settings);
    }
    if (app == APP_NOTES) load_notes();
    if (app == APP_TERMINAL && terminal_output_count==0u) terminal_push("MintOS26 Terminal - type help");
    if (app == APP_FILES) { refresh_storage_data(); refresh_files(); }
    if (app == APP_DISKS) { refresh_storage_data(); if(disk_selected_device>=storage_snapshot.device_count)disk_selected_device=0u; disk_format_prompt=0u; disk_confirmation[0]='\0'; disk_confirmation_length=0u; }
    if (app == APP_STORAGE) refresh_storage_data();
    if (app == APP_DRIVERS || app == APP_NETWORK) refresh_driver_catalog();
    paint_shell();
}
static void settings_geometry(int32_t *left_x, int32_t *right_x, int32_t *card_w,
                              int32_t *card_y) {
    int32_t w = cached_win_w;
    int32_t pad = w / 22; if (pad < 24) pad = 24;
    int32_t inner_w = w - 2 * pad - 36;
    int32_t cw = (inner_w - 20) / 2;
    int32_t lx = cached_win_x + pad + 18;
    if (left_x) *left_x = lx;
    if (right_x) *right_x = lx + cw + 20;
    if (card_w) *card_w = cw;
    if (card_y) *card_y = cached_win_y + 164;
}

/* Settings rows are whole-row hit targets, not just text glyphs. This makes
 * the controls usable with both a pointer and keyboard navigation. */
static uint32_t settings_control_at(int32_t px, int32_t py) {
    if (!app_window_open || selected_app != APP_SETTINGS || window_minimized || settings_category != 1u) return 0u;
    int32_t lx, rx, cw, cy;
    settings_geometry(&lx, &rx, &cw, &cy);
    (void)rx; (void)cy;
    static const int32_t tops[5] = {214,251,288,325,362};
    if (px >= lx + 10 && px < lx + cw - 10) {
        int32_t rel_y = py - cached_win_y;
        for (uint32_t i = 0; i < 5u; ++i)
            if (rel_y >= tops[i] && rel_y < tops[i] + 30) return i + 1u;
    }
    int32_t refresh_x = rx + cw - 112;
    int32_t refresh_y = cached_win_y + 177;
    if (px >= refresh_x && px < refresh_x + 94 && py >= refresh_y && py < refresh_y + 27)
        return 6u;
    /* The entire status panel is also a convenient refresh target. */
    if (px >= rx && px < rx + cw && py >= cached_win_y + 216 && py < cached_win_y + 251)
        return 6u;
    return 0u;
}

static uint32_t settings_option_count(uint32_t id) {
    static const uint8_t counts[6]={0u,3u,5u,3u,3u,2u};
    return id<6u?counts[id]:0u;
}
static const char *settings_option_label(uint32_t id,uint32_t option) {
    static const char *const themes[3]={"Light","Dark","Warm"};
    static const char *const accents[5]={"Mint","Blue","Violet","Orange","Rose"};
    static const char *const scales[3]={"Normal","Large","Extra Large"};
    static const char *const wallpapers[3]={"Alpine","Ocean","Dusk"};
    static const char *const motion[2]={"Off","On"};
    switch(id){
        case 1u: return option<3u?themes[option]:"";
        case 2u: return option<5u?accents[option]:"";
        case 3u: return option<3u?scales[option]:"";
        case 4u: return option<3u?wallpapers[option]:"";
        case 5u: return option<2u?motion[option]:"";
        default: return "";
    }
}
static uint32_t settings_option_current(uint32_t id) {
    switch(id){
        case 1u: return desktop_settings.theme<3u?desktop_settings.theme:0u;
        case 2u: return desktop_settings.accent<5u?desktop_settings.accent:0u;
        case 3u: return desktop_settings.ui_scale>=1u&&desktop_settings.ui_scale<=3u?desktop_settings.ui_scale-1u:0u;
        case 4u: return desktop_settings.wallpaper<3u?desktop_settings.wallpaper:0u;
        case 5u: return (desktop_settings.flags&MINT_DESKTOP_SETTING_REDUCE_MOTION)?1u:0u;
        default: return 0u;
    }
}
static void settings_dropdown_bounds(uint32_t id,int32_t *x,int32_t *y,int32_t *w,int32_t *h) {
    int32_t lx=0,rx=0,cw=0,cy=0;
    settings_geometry(&lx,&rx,&cw,&cy); (void)rx; (void)cy;
    static const int32_t rowtop[5]={214,251,288,325,362};
    uint32_t count=settings_option_count(id);
    int32_t bx=lx+cw-174, by=cached_win_y+(id>=1u&&id<=5u?rowtop[id-1u]:214);
    int32_t pw=170, ph=(int32_t)count*27+8, py=by+30;
    if(py+ph>cached_win_y+cached_win_h-18) py=by-ph-4;
    if(py<cached_win_y+48) py=cached_win_y+48;
    if(x)*x=bx-8; if(y)*y=py; if(w)*w=pw; if(h)*h=ph;
}
static void paint_settings_dropdown(void) {
    uint32_t id=open_settings_dropdown, count=settings_option_count(open_settings_dropdown);
    if(!id||!count) return;
    int32_t x=0,y=0,w=0,h=0; settings_dropdown_bounds(id,&x,&y,&w,&h);
    round_box(x+3,y+5,w,h,12,0x142638u);
    glass_box(x,y,w,h,12,C_WINDOW,232u);
    for(uint32_t i=0;i<count;i++){
        int32_t iy=y+4+(int32_t)i*27;
        if(i==settings_dropdown_cursor) glass_box(x+5,iy,w-10,25,7,C_ACCENT,54u);
        (void)draw_text(settings_option_label(id,i),x+13,iy+6,
                        i==settings_dropdown_cursor?C_ACCENT:C_TEXT,1);
        if(i==settings_option_current(id)) (void)draw_text("*",x+w-24,iy+6,C_ACCENT,1);
    }
}
static void apply_settings_dropdown(uint32_t id,uint32_t option) {
    if(option>=settings_option_count(id)) return;
    switch(id){
        case 1u: desktop_settings.theme=option; break;
        case 2u: desktop_settings.accent=option; break;
        case 3u: desktop_settings.ui_scale=option+1u; break;
        case 4u: desktop_settings.wallpaper=option; break;
        case 5u: if(option) desktop_settings.flags|=MINT_DESKTOP_SETTING_REDUCE_MOTION;
                 else desktop_settings.flags&=~MINT_DESKTOP_SETTING_REDUCE_MOTION; break;
        default: return;
    }
    open_settings_dropdown=0u;
    (void)update_desktop_settings();
}
static int handle_settings_dropdown_click(int32_t px,int32_t py) {
    uint32_t id=open_settings_dropdown, count=settings_option_count(open_settings_dropdown);
    if(!id||!count) return 0;
    int32_t x=0,y=0,w=0,h=0; settings_dropdown_bounds(id,&x,&y,&w,&h);
    if(px>=x&&px<x+w&&py>=y&&py<y+h){
        int32_t option=(py-y-4)/27;
        if(option>=0&&(uint32_t)option<count) apply_settings_dropdown(id,(uint32_t)option);
        else {open_settings_dropdown=0u;paint_shell();}
        return 1;
    }
    open_settings_dropdown=0u;
    return 2;
}

static void activate_setting(uint32_t id) {
    switch (id) {
        case 1u: desktop_settings.theme = (desktop_settings.theme + 1u) % 3u; break;
        case 2u: desktop_settings.accent = (desktop_settings.accent + 1u) % 5u; break;
        case 3u: desktop_settings.ui_scale = desktop_settings.ui_scale >= 3u ? 1u : desktop_settings.ui_scale + 1u; break;
        case 4u: desktop_settings.wallpaper = (desktop_settings.wallpaper + 1u) % 3u; break;
        case 5u: desktop_settings.flags ^= MINT_DESKTOP_SETTING_REDUCE_MOTION; break;
        case 6u:
            (void)mint_input_get_info(&input_info);
            paint_shell();
            return;
        default: return;
    }
    (void)update_desktop_settings();
}

static void handle_key(const MINT_DESKTOP_KEY_EVENT *key) {
    if (!key || !key->pressed) return;
    uint32_t ch = key->codepoint;
    if (ch == 27u && open_settings_dropdown) { open_settings_dropdown=0u; paint_shell(); return; }
    if (ch == 27u) { library_open = 0; app_window_open = 0; window_minimized = 0; window_maximized = 0; window_focused = 0; window_resizing=0u; open_settings_dropdown=0u; paint_shell(); return; }

    if (window_focused && app_window_open && selected_app==APP_DISKS && disk_format_prompt) {
        if (ch==8u || ch==127u) {
            if(disk_confirmation_length) disk_confirmation[--disk_confirmation_length]='\0';
            paint_shell(); return;
        }
        if(ch=='\n' || ch=='\r') {
            MINT_STORAGE_FORMAT_REQUEST request={0};
            request.version=MINT_STORAGE_FORMAT_REQUEST_VERSION;
            request.device_index=disk_selected_device;
            if(disk_selected_device<storage_snapshot.device_count && disk_selected_device<MINT_STORAGE_MAX_DEVICES) {
                MINT_STORAGE_DEVICE_INFO *device=&storage_snapshot.devices[disk_selected_device];
                request.expected_sector_count=device->sector_count;
                (void)copy_text(request.expected_serial,sizeof(request.expected_serial),device->serial);
                (void)copy_text(request.confirmation,sizeof(request.confirmation),disk_confirmation);
                int64_t rc=mint_storage_format_blank(&request);
                disk_result[0]='\0';
                if(rc==0) (void)copy_text(disk_result,sizeof(disk_result),"Success: disk formatted and FAT16 read-only validated.");
                else {
                    char msg[112]="Initialization failed (error "; char code[20]; format_u64(code,sizeof(code),(uint64_t)(-rc));
                    size_t n=text_length(msg); for(size_t i=0;code[i]&&n+2u<sizeof(msg);i++)msg[n++]=code[i]; msg[n++]=')'; msg[n]='\0';
                    (void)copy_text(disk_result,sizeof(disk_result),msg);
                }
            } else (void)copy_text(disk_result,sizeof(disk_result),"Error: selected disk is no longer available.");
            disk_format_prompt=0u; disk_confirmation_length=0u; disk_confirmation[0]='\0';
            refresh_storage_data(); paint_shell(); return;
        }
        if(ch>=32u && ch<=126u && disk_confirmation_length+1u<sizeof(disk_confirmation)) {
            disk_confirmation[disk_confirmation_length++]=(char)ch; disk_confirmation[disk_confirmation_length]='\0';
            paint_shell(); return;
        }
        return;
    }

    if (window_focused && app_window_open && selected_app == APP_TERMINAL) {
        if (ch==8u || ch==127u) {
            if(terminal_input_length) terminal_input[--terminal_input_length]='\0';
            paint_shell(); return;
        }
        if(ch=='\n' || ch=='\r') {
            terminal_run_command(); terminal_input_length=0u; terminal_input[0]='\0';
            refresh_storage_data(); paint_shell(); return;
        }
        if(ch>=32u && ch<=126u && terminal_input_length+1u<sizeof(terminal_input)) {
            terminal_input[terminal_input_length++]=(char)ch; terminal_input[terminal_input_length]='\0'; paint_shell(); return;
        }
        return;
    }
    /* Keyboard input goes to the focused Notes view before global shortcuts. */
    if (window_focused && app_window_open && selected_app == APP_NOTES) {
        load_notes();
        if (ch == 8u) {
            if (note_length) {
                --note_length; note_buffer[note_length] = '\0'; save_notes(); paint_shell();
            }
        } else if (ch == '\n' || (ch >= 32u && ch <= 126u)) {
            if (note_length + 1u < sizeof(note_buffer)) {
                note_buffer[note_length++] = (char)ch; note_buffer[note_length] = '\0';
                save_notes(); paint_shell();
            }
        }
        return;
    }

    if (window_focused && app_window_open && selected_app == APP_SETTINGS) {
        if (open_settings_dropdown && (ch == '\n' || ch == ' ')) {
            apply_settings_dropdown(open_settings_dropdown,settings_dropdown_cursor); return;
        }
        if (ch == 9u) {
            keyboard_setting_focus = keyboard_setting_focus >= 6u ? 1u : keyboard_setting_focus + 1u;
            hover_setting_control = keyboard_setting_focus;
            paint_shell(); return;
        }
        if ((ch == '\n' || ch == ' ') && keyboard_setting_focus) {
            if (keyboard_setting_focus <= 5u) {
                open_settings_dropdown = keyboard_setting_focus;
                settings_dropdown_cursor = settings_option_current(open_settings_dropdown);
                paint_shell();
            } else activate_setting(keyboard_setting_focus);
            return;
        }
        if (open_settings_dropdown && (ch == ']' || ch == '[')) {
            uint32_t count=settings_option_count(open_settings_dropdown);
            if(count){ if(ch==']') settings_dropdown_cursor=(settings_dropdown_cursor+1u)%count;
                       else settings_dropdown_cursor=settings_dropdown_cursor?settings_dropdown_cursor-1u:count-1u; }
            paint_shell(); return;
        }
        if (ch == 'D' || ch == 'd') { activate_setting(1u); return; }
        if (ch == 'A' || ch == 'a') { activate_setting(2u); return; }
        if (ch == '+' || ch == '=') { activate_setting(3u); return; }
        if (ch == '-') {
            desktop_settings.ui_scale = desktop_settings.ui_scale <= 1u ? 3u : desktop_settings.ui_scale - 1u;
            (void)update_desktop_settings(); return;
        }
        if (ch == 'M' || ch == 'm') { activate_setting(5u); return; }
        if (ch == 'W' || ch == 'w') { activate_setting(4u); return; }
        if (ch == 'R' || ch == 'r') { activate_setting(6u); return; }
    }

    if (ch == '\n' && !app_window_open && !library_open) { library_open=1u; paint_shell(); return; }
    if ((ch == 'R' || ch == 'r') && app_window_open && selected_app == APP_SETTINGS) {
        (void)mint_input_get_info(&input_info); paint_shell(); return;
    }
}
static int rects_intersect(int32_t ax,int32_t ay,int32_t aw,int32_t ah,
                           int32_t bx,int32_t by,int32_t bw,int32_t bh) {
    return ax<bx+bw && bx<ax+aw && ay<by+bh && by<ay+ah;
}
static void repaint_pointer_move(int32_t old_x,int32_t old_y) {
    if(!background_snapshot_ready || mint_ui_frame_begin()!=0) { paint_shell(); return; }
    int32_t rx=(old_x<pointer_x?old_x:pointer_x)-2;
    int32_t ry=(old_y<pointer_y?old_y:pointer_y)-2;
    int32_t right=(old_x>pointer_x?old_x:pointer_x)+24;
    int32_t bottom=(old_y>pointer_y?old_y:pointer_y)+32;
    if(right-rx>160 || bottom-ry>160) { paint_shell(); return; }
    int32_t rw=right-rx, rh=bottom-ry;
    if(rx<0){rw+=rx;rx=0;} if(ry<0){rh+=ry;ry=0;}
    if(rx+rw>(int32_t)display.width) rw=(int32_t)display.width-rx;
    if(ry+rh>(int32_t)display.height) rh=(int32_t)display.height-ry;
    if(rw>0 && rh>0) (void)mint_ui_restore_rect(rx,ry,rw,rh);
    if(!window_minimized && (library_open || app_window_open) && rects_intersect(rx,ry,rw,rh,cached_win_x,cached_win_y,cached_win_w,cached_win_h))
        paint_home_window(cached_win_x,cached_win_y,cached_win_w,cached_win_h);
    if(active_menu>=0 && rects_intersect(rx,ry,rw,rh,menu_left(active_menu),44,214,(int32_t)menu_item_count(active_menu)*31+8))
        paint_menu_dropdown();
    paint_dock(); paint_cursor(); (void)mint_ui_frame_end();
}
static int handle_files_pointer_click(int32_t px,int32_t py) {
    if(!app_window_open || selected_app!=APP_FILES || window_minimized) return 0;
    int32_t pad=cached_win_w/22; if(pad<24)pad=24;
    int32_t sx=cached_win_x+pad+18, sy=cached_win_y+124, sw=176, sh=cached_win_h-174;
    if(sh<150)sh=150;
    if(px>=sx+8 && px<sx+sw-8 && py>=sy+36 && py<sy+67) {
        files_volume=0; files_path[0]='\0'; files_selected_entry=-1; files_preview_active=0; refresh_files(); paint_shell(); return 1;
    }
    uint32_t mounted_count=fat_volume_list.count;
    if(mounted_count>MINT_FAT16_MAX_VOLUMES) mounted_count=MINT_FAT16_MAX_VOLUMES;
    for(uint32_t vi=0; vi<mounted_count; ++vi) {
        int32_t ry=sy+72+(int32_t)vi*35;
        if(px>=sx+8 && px<sx+sw-8 && py>=ry && py<ry+32) {
            files_volume=vi+1u; (void)mint_fat16_select_volume(vi);
            files_path[0]='\0'; files_selected_entry=-1; files_preview_active=0;
            files_preview_length=0; refresh_fat_files(); paint_shell(); return 1;
        }
    }
    int32_t source_y=sy+78+(int32_t)(mounted_count?mounted_count:1u)*35;
    int32_t up_y=source_y+44;
    if(px>=sx+8 && px<sx+sw-8 && py>=up_y && py<up_y+29) {
        files_go_up(); paint_shell(); return 1;
    }
    int32_t lx=sx+sw+18, list_y=sy+45, lw=cached_win_w-(lx-cached_win_x)-pad-22;
    if(lw<160)lw=160;
    int32_t detail_w=files_preview_active?lw*55/100:lw; if(detail_w<130)detail_w=lw;
    if(px>=lx && px<lx+detail_w && py>=list_y+30) {
        uint32_t idx=(uint32_t)(py-(list_y+30))/28u;
        uint32_t count=files_volume?fat_entry_count:fs_entry_count;
        int32_t row_y=list_y+34+(int32_t)idx*28;
        if(idx<count && idx<16u && py<row_y+21) { files_select_entry(idx); paint_shell(); return 1; }
    }
    (void)sh; return 0;
}

static int handle_disk_utility_pointer_click(int32_t px,int32_t py) {
    if(!app_window_open || selected_app!=APP_DISKS || window_minimized) return 0;
    refresh_storage_data();
    int32_t pad=cached_win_w/22; if(pad<24)pad=24;
    int32_t gx=cached_win_x+pad+24, gy=cached_win_y+153, gw=cached_win_w-2*pad-48;
    uint32_t shown=storage_snapshot.device_count<5u?storage_snapshot.device_count:5u;
    for(uint32_t i=0;i<shown;i++){
        int32_t ry=gy+34+(int32_t)i*62;
        if(px>=gx+8 && px<gx+gw-8 && py>=ry && py<ry+53){
            disk_selected_device=i; disk_format_prompt=0; disk_confirmation_length=0; disk_confirmation[0]='\0'; disk_result[0]='\0'; paint_shell(); return 1;
        }
    }
    int32_t by=gy+44+(int32_t)(storage_snapshot.device_count?storage_snapshot.device_count:1u)*62;
    if(px>=gx && px<gx+244 && py>=by && py<by+34){
        if(disk_selected_device>=storage_snapshot.device_count || disk_selected_device>=MINT_STORAGE_MAX_DEVICES) {
            copy_text(disk_result,sizeof(disk_result),"No disk selected. Refresh storage discovery first.");
        } else {
            disk_format_prompt=1u; disk_confirmation_length=0; disk_confirmation[0]='\0'; disk_result[0]='\0'; window_focused=1u;
        }
        paint_shell(); return 1;
    }
    return 0;
}
static int handle_menu_dropdown_click(int32_t px,int32_t py) {
    if(active_menu<0) return 0;
    int32_t left=menu_left(active_menu); uint32_t count=menu_item_count(active_menu);
    if(px>=left && px<left+214 && py>=44 && py<44+(int32_t)count*31+8) {
        uint32_t index=py<=49?0u:(uint32_t)(py-49)/31u;
        if(index<count) menu_execute(menu_item_action(active_menu,index)); else {active_menu=-1;paint_shell();}
        return 1;
    }
    if(py>=44 || px<0) { active_menu=-1; paint_shell(); }
    return 0;
}

static void handle_pointer_button(const MINT_DESKTOP_POINTER_EVENT *p) {
    if (!p || !(p->changed_buttons & 1u)) return;
    int32_t x = p->x, y = p->y;
    if (!(p->buttons & 1u)) { window_dragging = 0; window_resizing=0u; return; }

    if(active_menu>=0 && handle_menu_dropdown_click(x,y)) return;
    if (y >= 0 && y < 42) { handle_menu_bar_click(x); return; }
    if (handle_dock_click(x,y)) return;

    if ((app_window_open || library_open) && !window_minimized &&
        x >= cached_win_x && x < cached_win_x + cached_win_w &&
        y >= cached_win_y && y < cached_win_y + cached_win_h) window_focused = 1u;
    if(app_window_open && !window_minimized && !window_maximized &&
       x>=cached_win_x+cached_win_w-24 && x<cached_win_x+cached_win_w &&
       y>=cached_win_y+cached_win_h-24 && y<cached_win_y+cached_win_h) {
        window_resizing=1u; window_dragging=0u; resize_anchor_x=x; resize_anchor_y=y;
        resize_start_w=cached_win_w; resize_start_h=cached_win_h;
        custom_win_w=cached_win_w; custom_win_h=cached_win_h; return;
    }
    if ((app_window_open || library_open) && !window_minimized &&
        x >= cached_win_x && x < cached_win_x + cached_win_w &&
        y >= cached_win_y && y < cached_win_y + 42) {
        window_focused = 1u;
        /* macOS-style window controls: close, minimize and maximize/restore. */
        if (x >= cached_win_x + 14 && x < cached_win_x + 34) {
            app_window_open = 0; library_open = 0; window_minimized = 0;
            window_maximized = 0; window_focused = 0; window_resizing=0u; paint_shell(); return;
        }
        if (x >= cached_win_x + 34 && x < cached_win_x + 54) {
            window_minimized = 1u; window_dragging = 0; window_resizing=0u; paint_shell(); return;
        }
        if (x >= cached_win_x + 54 && x < cached_win_x + 76) {
            window_maximized = !window_maximized; window_dragging = 0; window_resizing=0u; paint_shell(); return;
        }
        if (!window_maximized) {
            window_dragging = 1; drag_offset_x = x - cached_win_x; drag_offset_y = y - cached_win_y;
        }
        return;
    }

    if(handle_files_pointer_click(x,y)) return;
    if(handle_disk_utility_pointer_click(x,y)) return;

    if (app_window_open && selected_app == APP_STORAGE && !window_minimized &&
        x >= cached_win_x && x < cached_win_x + cached_win_w &&
        y >= cached_win_y + 114 && y < cached_win_y + 143) {
        int32_t pad = cached_win_w / 22; if (pad < 24) pad = 24;
        int32_t control_x = cached_win_x + pad + 20;
        for (uint32_t ci = 0; ci < 7u; ++ci) {
            int32_t bx = storage_control_x(control_x, ci);
            int32_t bw = storage_control_width(ci);
            if (x >= bx && x < bx + bw) {
                if (ci == 0u) { storage_view = 0u; storage_page = 0u; }
                else if (ci == 1u) { storage_view = 1u; storage_page = 0u; }
                else if (ci == 2u) storage_view = 2u;
                else if (ci == 3u) storage_view = 3u;
                else if (ci == 4u) storage_view = 4u;
                else if (ci == 5u) { (void)mint_input_get_info(&input_info); }
                else { export_storage_report(); paint_shell(); return; }
                refresh_storage_data(); paint_shell(); return;
            }
        }
    }
    if (app_window_open && selected_app == APP_STORAGE && !window_minimized &&
        storage_view <= 1u && y >= cached_win_y + 144 && y < cached_win_y + 174) {
        int32_t pad = cached_win_w / 22; if (pad < 24) pad = 24;
        if (x >= cached_win_x + cached_win_w - pad - 78 && x < cached_win_x + cached_win_w - pad - 48) {
            if (storage_page) --storage_page; paint_shell(); return;
        }
        if (x >= cached_win_x + cached_win_w - pad - 42 && x < cached_win_x + cached_win_w - pad - 12) {
            uint32_t items = storage_view == 1u ? storage_snapshot.controller_count : storage_snapshot.device_count;
            uint32_t page_count = storage_view == 0u ? items : (items + 1u) / 2u;
            if (storage_page + 1u < page_count) ++storage_page; paint_shell(); return;
        }
    }

    if (app_window_open && selected_app == APP_DRIVERS && !window_minimized &&
        y >= cached_win_y + 84 && y < cached_win_y + 114) {
        int32_t pad = cached_win_w / 22; if (pad < 24) pad = 24;
        uint32_t total_pages = (driver_catalog.count + 7u) / 8u;
        if (!total_pages) total_pages = 1u;
        if (x >= cached_win_x + cached_win_w - pad - 78 && x < cached_win_x + cached_win_w - pad - 48) {
            if (driver_page) --driver_page; paint_shell(); return;
        }
        if (x >= cached_win_x + cached_win_w - pad - 42 && x < cached_win_x + cached_win_w - pad - 12) {
            if (driver_page + 1u < total_pages) ++driver_page; paint_shell(); return;
        }
    }

    if (app_window_open && selected_app==APP_SETTINGS && !window_minimized &&
        y>=cached_win_y+128 && y<cached_win_y+156) {
        int32_t pad=cached_win_w/22; if(pad<24)pad=24;
        int32_t inner_left=cached_win_x+pad+18, inner_w=cached_win_w-2*pad-36;
        int32_t tab_w=(inner_w-5*6)/6; if(tab_w<62)tab_w=62;
        for(uint32_t ti=0;ti<6u;ti++) {
            int32_t tx=inner_left+(int32_t)ti*(tab_w+6);
            if(x>=tx && x<tx+tab_w) { settings_category=ti; hover_setting_control=0u; keyboard_setting_focus=0u; open_settings_dropdown=0u; paint_shell(); return; }
        }
    }
    if (app_window_open && (selected_app==APP_ABOUT_OS || selected_app==APP_ABOUT_MINTDE) && !window_minimized) {
        int32_t pad=cached_win_w/22; if(pad<24)pad=24;
        int32_t ax=cached_win_x+pad+22, aw=cached_win_w-2*pad-44;
        if(x>=ax+aw-164 && x<ax+aw-14 && y>=cached_win_y+97 && y<cached_win_y+127) {
            if(show_changelog==0u) show_changelog=1u;
            else if(show_changelog==1u) show_changelog=0u;
            else show_changelog=1u;
            paint_shell(); return;
        }
        if(show_changelog==1u) {
            /* Keep hit targets identical to the compact eight-row list painted above. */
            int32_t row_y=cached_win_y+79+119;
            int32_t list_h=(cached_win_h<620)?36:40, list_gap=6;
            for(uint32_t ci=0;ci<8u;ci++) {
                if(y>=row_y && y<row_y+list_h && x>=ax && x<ax+aw) {
                    changelog_version=ci; show_changelog=2u; paint_shell(); return;
                }
                row_y+=list_h+list_gap;
            }
        }
    }
    if (open_settings_dropdown) {
        int dropdown_click=handle_settings_dropdown_click(x,y);
        if(dropdown_click==1) return;
        if(dropdown_click==2) { paint_shell(); return; }
    }
    uint32_t setting_id = settings_control_at(x, y);
    if (setting_id) {
        hover_setting_control = setting_id;
        keyboard_setting_focus = setting_id;
        if (setting_id <= 5u) {
            open_settings_dropdown = open_settings_dropdown == setting_id ? 0u : setting_id;
            settings_dropdown_cursor = settings_option_current(setting_id);
            paint_shell();
        } else activate_setting(setting_id);
        return;
    }

    if (!(x >= cached_win_x && x < cached_win_x + cached_win_w &&
          y >= cached_win_y && y < cached_win_y + cached_win_h)) window_focused = 0u;

    if (!app_window_open && x >= cached_win_x + cached_margin + 24 &&
        x < cached_win_x + cached_margin + 270 && y > cached_win_y + cached_win_h - 126 &&
        y < cached_win_y + cached_win_h - 55) {
        library_open = 1; window_minimized = 0; paint_shell(); return;
    }
    if (library_open) {
        int compact_cards = cached_win_h < 530 || cached_win_w < 760;
        int32_t grid_x = cached_win_x + 50;
        int32_t grid_y = cached_win_y + (compact_cards ? 132 : 142);
        int32_t cell_w = (cached_win_w - 100) / 3;
        int32_t card_h = compact_cards ? 72 : 88;
        int32_t row_step = compact_cards ? 80 : 104;
        for (uint32_t i=0; i<APP_COUNT; ++i) {
            int32_t ix = grid_x + (int32_t)(i % 3u) * cell_w;
            int32_t iy = grid_y + (int32_t)(i / 3u) * row_step;
            if (x >= ix && x < ix + cell_w - 14 && y >= iy && y < iy + card_h) { open_app(i); return; }
        }
    }
}

static void handle_event(const MINT_DESKTOP_EVENT *event) {
    if (!event) return;
    if (event->type == MINT_DESKTOP_EVENT_KEY) handle_key(&event->data.key);
    else if (event->type == MINT_DESKTOP_EVENT_POINTER_MOVE) {
        int32_t old_x=pointer_x, old_y=pointer_y;
        pointer_x=event->data.pointer.x; pointer_y=event->data.pointer.y;
        if(pointer_x<0)pointer_x=0; if(pointer_y<0)pointer_y=0;
        if(pointer_x>=(int32_t)display.width)pointer_x=(int32_t)display.width-1;
        if(pointer_y>=(int32_t)display.height)pointer_y=(int32_t)display.height-1;
        if (window_dragging && (event->data.pointer.buttons & 1u)) {
            int32_t target_x = pointer_x - drag_offset_x;
            int32_t target_y = pointer_y - drag_offset_y;
            move_window_to(target_x, target_y);
        } else if (app_window_open && selected_app == APP_SETTINGS && !window_minimized) {
            uint32_t now_hover = settings_control_at(pointer_x, pointer_y);
            if (now_hover != hover_setting_control) {
                hover_setting_control = now_hover;
                paint_shell();
            } else repaint_pointer_move(old_x,old_y);
        } else repaint_pointer_move(old_x,old_y);
    } else if (event->type == MINT_DESKTOP_EVENT_POINTER_BUTTON) handle_pointer_button(&event->data.pointer);
    else if (event->type == MINT_DESKTOP_EVENT_APP_REQUEST) {
        uint32_t app = event->data.app.app_id % APP_COUNT; selected_app = app;
    }
}

int64_t mint_user_main(void) {
    if (mint_display_get_info(&display) != 0 || !display.width || !display.height ||
        display.width > 16384u || display.height > 16384u || display.stride_pixels < display.width) {
        static const char failed[] = "[user-desktop] display metadata unavailable\n";
        (void)mint_write(1, failed, sizeof(failed) - 1u); return 1;
    }
    input_info = (MINT_INPUT_INFO){0};
    (void)mint_input_get_info(&input_info);
    pointer_x = (int32_t)display.width / 2; pointer_y = (int32_t)display.height / 2;
    /* Start on a clean workspace. Launchpad is opened from the dock/menu; no welcome window. */
    selected_app=APP_FILES; library_open=0u; app_window_open=0u; window_focused=0u; show_changelog=0u;
    disk_selected_device=0u; disk_format_prompt=0u; disk_confirmation[0]='\0'; disk_result[0]='\0';
    files_volume=0u; files_path[0]='\0'; files_selected_entry=-1; files_preview_active=0u;
    terminal_input_length=0u; terminal_input[0]='\0'; terminal_output_count=0u;
    refresh_storage_data();
    if (!mint_desktop_settings_get(&desktop_settings)) {
        desktop_settings.version = MINT_DESKTOP_SETTINGS_VERSION; desktop_settings.theme = MINT_THEME_LIGHT;
        desktop_settings.accent = MINT_ACCENT_MINT; desktop_settings.ui_scale = 1u;
    }
    apply_palette(); refresh_files();
    paint_shell();
    static const char ready[] = "[user-desktop] MintDE 1.0.7; E1000 diagnostics, dock, split About pages, mouse menus and settings ready\n";
    (void)mint_write(1, ready, sizeof(ready) - 1u);
    for (;;) {
        MINT_DESKTOP_EVENT events[16];
        uint32_t count = 0;
        while (count < (uint32_t)(sizeof(events) / sizeof(events[0]))) {
            MINT_DESKTOP_EVENT event = {0};
            int64_t got = mint_desktop_event_poll(&event);
            if (got != 1) break;
            events[count++] = event;
        }
        for (uint32_t i = 0; i < count;) {
            if (events[i].type == MINT_DESKTOP_EVENT_POINTER_MOVE) {
                uint32_t last = i;
                while (last + 1u < count &&
                       events[last + 1u].type == MINT_DESKTOP_EVENT_POINTER_MOVE &&
                       events[last + 1u].data.pointer.buttons == events[i].data.pointer.buttons)
                    ++last;
                handle_event(&events[last]);
                i = last + 1u;
            } else {
                handle_event(&events[i++]);
            }
        }
        update_clock();
        if (count == 0u) (void)mint_sleep_ticks(1u);
        else (void)mint_sched_yield();
    }
}
