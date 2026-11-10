/* MintDE 2 applications. The data/command logic (storage diagnostics, the
 * bounded Terminal, Files navigation, Notes persistence in the session RAMFS,
 * driver catalog) is carried over from MintDE 1.0.8 unchanged; every view is
 * redrawn in a macOS-style layout on top of the gfx/widgets layer. */
#include "mintde.h"
#include "tls.h"

#define TERM_LINES 240u
#define TERM_COLS 96u
#define TERM_WRAP 92u

static MINT_FS_DIRENT fs_entries[MINT_RAMFS_MAX_FILES];
static uint32_t fs_entry_count;
#define NOTE_MAX 16384u
#define NOTE_RAM_MAX (MINT_RAMFS_FILE_CAPACITY - 1u)
static char note_buffer[NOTE_MAX];
static size_t note_length;
static uint32_t notes_loaded;
static MINT_INPUT_INFO input_info;
static uint32_t disk_selected_device;
static uint32_t disk_format_prompt;
static uint32_t disk_format_kind;           /* 0 MintOS (MintFS), 1 MS-DOS (FAT16) */
static char disk_confirmation[MINT_STORAGE_FORMAT_CONFIRM_LEN];
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
static MINT_STORAGE_SNAPSHOT storage_snapshot;
static MINT_FAT16_VOLUME_LIST fat_volume_list;
/* Large syscall result buffers live in the static data segment, not on the
 * user stack; views share these scratch areas cooperatively. */
static MINT_PCI_INVENTORY terminal_pci_inventory;
static MINT_VFS_MOUNT_TABLE terminal_mount_table;
static MINT_LOG_RECORD terminal_log_scratch[MINT_LOG_READ_MAX];
static MINT_FS_DIRENT terminal_ramfs_entries[MINT_RAMFS_MAX_FILES];
static MINT_DRIVER_CATALOG driver_catalog;
static MINT_E1000_INFO terminal_e1000_info;
static uint32_t driver_catalog_ok;
static MINT_LOG_RECORD storage_log_records[MINT_LOG_READ_MAX];
static uint32_t storage_log_count;
static uint32_t storage_view;      /* 0 device, 1 controllers, 2 all logs, 3 warn+, 4 errors */
static uint32_t storage_page;      /* selected device */
static uint32_t storage_diag_ok;
static char terminal_input[TERM_COLS];
static size_t terminal_input_length;
static char terminal_output[TERM_LINES][TERM_COLS];
static uint8_t terminal_output_kind[TERM_LINES]; /* 1 = entered command, 0 = output */
static uint32_t terminal_output_count;
static uint32_t terminal_output_first;   /* ring index of the oldest line */
static int32_t terminal_scroll;          /* lines scrolled back from the newest */
static uint32_t terminal_serial;         /* bumps on every new line */
static uint32_t terminal_volume_ordinal;
static char terminal_current_path[96];

static void hex4(char *out, uint16_t v) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 4; ++i) out[i] = hx[(v >> (12 - 4 * i)) & 15u];
    out[4] = '\0';
}
static void bytes_clear(void *p, size_t n) { uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0; }
/* Legacy-name helpers used by the carried-over logic. */
#define terminal_equals_ci terminal_equals

/* Forward declarations for the carried-over logic. */
static const char *driver_state_label(uint32_t state);
static void refresh_driver_catalog(void);
static void refresh_fat_files(void);
static void refresh_files(void);
static void terminal_run_command(void);
static void terminal_push(const char *text);
static int terminal_network_command(const char *cmd);
static const char *network_state_text(const MINT_NET_INFO *n, uint32_t *dot, int *connected);
static int network_toggle(void);
static uint32_t network_autoconnect;
static void terminal_cancel_job(void);
static uint64_t term_ms(void);
static int terminal_mfs_command(const char *cmd);
static const char *terminal_prompt(void);
static int term_mfs_active(void);
static int terminal_mfs_mode;
static int terminal_job_active(void);
static const char *terminal_job_hint(void);

/* ====================================================================== */
/*  Carried-over MintDE 1.0.8 logic                                       */
/* ====================================================================== */
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
        case MINT_LOG_COMPONENT_USB: return "USB";
        case MINT_LOG_COMPONENT_NET: return "NET";
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
    storage_serial_text(storage_snapshot.readonly_mount_active ? "FAT16: read-only mount validated; persistence writes remain disabled\r\n" : "FAT16 mount: not active; persistent writes remain disabled in 26.5.012.0\r\n");
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

#define TERM_AT(i) terminal_output[(terminal_output_first + (uint32_t)(i)) % TERM_LINES]
#define TERM_KIND(i) terminal_output_kind[(terminal_output_first + (uint32_t)(i)) % TERM_LINES]
static void terminal_add(const char *text, uint8_t kind) {
    uint32_t slot;
    if (terminal_output_count < TERM_LINES) slot = (terminal_output_first + terminal_output_count++) % TERM_LINES;
    else { slot = terminal_output_first; terminal_output_first = (terminal_output_first + 1u) % TERM_LINES; }
    (void)copy_text(terminal_output[slot], sizeof(terminal_output[slot]), text ? text : "");
    terminal_output_kind[slot] = kind;
    terminal_scroll = 0;
    ++terminal_serial;
}
static void terminal_clear(void) { terminal_output_count = 0u; terminal_output_first = 0u; terminal_scroll = 0; }
static void terminal_push(const char *text) { terminal_add(text, 0u); }
static void terminal_push_command(const char *text) { terminal_add(text, 1u); }

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
    if(terminal_equals(cmd,"clear")) { terminal_clear(); return; }
    if(terminal_network_command(cmd)) return;
    if(terminal_mfs_command(cmd)) return;
    if(terminal_equals(cmd,"help")) {
        terminal_push("Commands: help, clear, ver, uptime, status, cpu, usb");
        terminal_push("Network: net up, net down, ifconfig, ping <host>, nslookup <host>, curl [-I] [-s] http(s)://...");
        terminal_push("pci, resources, drivers, mounts, io, logs, net");
        terminal_push("drives, volumes, use sda/sdb, net start/status/test, ls, cd, pwd, cat");
        terminal_push("use sda | ls [name] | cd <dir> | pwd | cat <file>");
        terminal_push("power, shutdown, restart (asks for confirmation)");
        terminal_push("MintOS HD: ls [-l], cd, pwd, cat, open, mkdir, touch, rm [-r], rmdir, mv, cp, echo txt > file, df, sync");
        terminal_push("use hd switches to MintOS HD; use sda/sdb to a read-only FAT16 volume"); return;
    }
    if(terminal_equals(cmd,"shutdown") || terminal_equals(cmd,"poweroff")) {
        shell_request_power(MINT_POWER_ACTION_SHUTDOWN); terminal_push("Confirm shutdown in the dialog (Enter) or press Esc"); return;
    }
    if(terminal_equals(cmd,"restart") || terminal_equals(cmd,"reboot")) {
        shell_request_power(MINT_POWER_ACTION_RESTART); terminal_push("Confirm restart in the dialog (Enter) or press Esc"); return;
    }
    if(terminal_equals(cmd,"power")) {
        MINT_POWER_INFO pi;
        if(mint_power_get_status(&pi)<0){terminal_push("ACPI power service unavailable");return;}
        char row[96]="",num[24],hex[24];
        terminal_append(row,sizeof(row),"ACPI rev ");format_u64(num,sizeof(num),pi.acpi_revision);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row)," OEM ");terminal_append(row,sizeof(row),pi.oem_id[0]?pi.oem_id:"?");
        terminal_append(row,sizeof(row)," tables ");format_u64(num,sizeof(num),pi.table_count);terminal_append(row,sizeof(row),num);
        terminal_append(row,sizeof(row),(pi.flags&MINT_POWER_HYPERVISOR)?" (virtual machine)":" (hardware)");terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"Shutdown: ");
        if(pi.flags&MINT_POWER_HAVE_S5){terminal_append(row,sizeof(row),"ACPI S5 SLP_TYP ");format_u64(num,sizeof(num),pi.slp_typa);terminal_append(row,sizeof(row),num);
            terminal_append(row,sizeof(row),"/");format_u64(num,sizeof(num),pi.slp_typb);terminal_append(row,sizeof(row),num);
            terminal_append(row,sizeof(row)," PM1a_CNT ");format_hex64(hex,sizeof(hex),pi.pm1a_cnt);terminal_append(row,sizeof(row),hex);}
        else terminal_append(row,sizeof(row),"no \\_S5_ object; power button required");
        terminal_push(row);
        row[0]='\0';terminal_append(row,sizeof(row),"Restart: ");
        if(pi.flags&MINT_POWER_HAVE_RESET_REG){terminal_append(row,sizeof(row),"ACPI reset reg ");format_hex64(hex,sizeof(hex),pi.reset_address);terminal_append(row,sizeof(row),hex);
            terminal_append(row,sizeof(row),", then 0xCF9/8042");}
        else terminal_append(row,sizeof(row),"0xCF9, then 8042 pulse, then triple fault");
        terminal_push(row);
        terminal_push((pi.flags&MINT_POWER_HAVE_8042)?"FADT reports an 8042 (PS/2) controller":"FADT does not report an 8042; USB input may need firmware emulation");
        return;
    }
    if(terminal_equals(cmd,"usb")) {
        static MINT_USB_INFO usb;
        if(mint_usb_get_status(&usb)!=0){terminal_push("USB status service unavailable");return;}
        char row[TERM_COLS],num[24],hex[24];
        copy_text(row,sizeof(row),"xHCI: ");str_append(row,sizeof(row),usb.status[0]?usb.status:"no controller");terminal_push(row);
        static const char *const kinds[6]={"unknown","keyboard","mouse","tablet","hub","other"};
        for(uint32_t i=0;i<usb.device_count&&i<MINT_USB_MAX_DEVICES;i++){
            MINT_USB_DEVICE_INFO *dv=&usb.devices[i];
            copy_text(row,sizeof(row),"  port ");format_u64(num,sizeof(num),dv->port);str_append(row,sizeof(row),num);
            str_append(row,sizeof(row)," slot ");format_u64(num,sizeof(num),dv->slot);str_append(row,sizeof(row),num);
            str_append(row,sizeof(row),"  ");hex4(hex,dv->vendor_id);str_append(row,sizeof(row),hex);
            str_append(row,sizeof(row),":");hex4(hex,dv->product_id);str_append(row,sizeof(row),hex);
            str_append(row,sizeof(row),"  ");str_append(row,sizeof(row),kinds[dv->kind<6u?dv->kind:0u]);
            str_append(row,sizeof(row),"  ");str_append(row,sizeof(row),dv->product);terminal_push(row);
        }
        copy_text(row,sizeof(row),"HID reports: ");format_u64(num,sizeof(num),usb.reports);str_append(row,sizeof(row),num);
        str_append(row,sizeof(row),"  transfer errors: ");format_u64(num,sizeof(num),usb.transfer_errors);str_append(row,sizeof(row),num);terminal_push(row);
        return;
    }
    if(terminal_equals(cmd,"ver")) { terminal_push("MintOS26 " MINTOS_VERSION " / MintDE " MINTDE_VERSION); return; }
    if(terminal_equals(cmd,"net start")) {
        int64_t rc = mint_e1000_start();
        (void)mint_e1000_get_status(&terminal_e1000_info);
        if(rc == 0) terminal_push("E1000 started; bounded DMA rings active; interrupts masked (use `net up` for IPv4)");
        else if(rc == -MINT_ENODEV) terminal_push("No E1000-family adapter detected; no DMA was enabled");
        else terminal_push("E1000 initialization failed; see COM1 logs; device remains unavailable");
        return;
    }
    if(terminal_equals(cmd,"net") || terminal_equals(cmd,"net status")) {
        if(mint_e1000_get_status(&terminal_e1000_info)<0){terminal_push("E1000 status service unavailable");return;}
        char title[TERM_COLS]; copy_text(title,sizeof(title),terminal_e1000_info.name[0]?terminal_e1000_info.name:"Intel E1000");
        if(terminal_e1000_info.state==MINT_E1000_ACTIVE) str_append(title,sizeof(title),": ACTIVE; polling RX/TX rings; IPv4 stack attached");
        else if(terminal_e1000_info.state==MINT_E1000_FOUND) str_append(title,sizeof(title),": detected but not initialized");
        else if(terminal_e1000_info.state==MINT_E1000_FAILED) str_append(title,sizeof(title),": initialization failed; see COM1 logs");
        else copy_text(title,sizeof(title),"Intel E1000 family: not detected");
        terminal_push(title);
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
        terminal_push("IPv4 configuration and traffic counters: run ifconfig");return;
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
        if(terminal_select_named_volume(cmd+4)) { terminal_mfs_mode=0; char row[80]="Selected ";terminal_append(row,sizeof(row),cmd+4);terminal_append(row,sizeof(row)," (read-only)");terminal_push(row); }
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

static void refresh_files(void) {
    int64_t count = mint_fs_list(fs_entries, MINT_RAMFS_MAX_FILES);
    fs_entry_count = count > 0 ? (uint32_t)count : 0u;
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
            int64_t n=mint_fat16_read_prefix(full,files_preview,sizeof(files_preview)-1u);
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

/* Notes live in ~/Documents/Notes.txt on MintOS HD when it is available
 * (saved ~1 s after typing stops, atomically); otherwise in the session RAM
 * store as before. Files can open other text files in Notes. */
static char notes_path[MINT_MFS_PATH_MAX];
static uint32_t notes_on_disk, notes_dirty, notes_readonly, notes_save_failed;
static uint64_t notes_edit_ms;

static int notes_default_path(char *out, size_t cap) {
    MINT_MFS_VOLUME v;
    if (mint_fs_get_volume(&v) != 0 || v.state != MINT_MFS_VOLUME_READY) return 0;
    copy_text(out, cap, v.home); str_append(out, cap, "/Documents/Notes.txt");
    return 1;
}
static void load_notes(void) {
    if (notes_loaded) return;
    note_length = 0; note_buffer[0] = '\0'; notes_readonly = 0; notes_dirty = 0; notes_save_failed = 0;
    char def[MINT_MFS_PATH_MAX];
    notes_on_disk = (uint32_t)notes_default_path(def, sizeof(def));
    int migrate = 0;
    if (notes_on_disk) {
        if (!notes_path[0]) copy_text(notes_path, sizeof(notes_path), def);
        int64_t n = mint_file_load(notes_path, note_buffer, sizeof(note_buffer) - 1u);
        if (n >= 0) {
            note_length = (size_t)n; note_buffer[note_length] = '\0';
            if (note_length >= sizeof(note_buffer) - 1u) notes_readonly = 1u;           /* too large to edit safely */
            for (size_t i = 0; i < note_length; ++i) if (!note_buffer[i]) { notes_readonly = 1u; break; }
            notes_loaded = 1u;
            return;
        }
        int is_default = 1;
        for (size_t i = 0; def[i] || notes_path[i]; ++i) if (def[i] != notes_path[i]) { is_default = 0; break; }
        if (n == -MINT_ENOENT && is_default) migrate = 1;     /* first run: carry the session note over */
        else if (n == -MINT_ENOENT) { notes_loaded = 1u; return; }   /* opened file vanished: start empty */
        else { notes_on_disk = 0; notes_path[0] = '\0'; }
    }
    int64_t h = mint_fs_open("NOTES.TXT", MINT_FS_OPEN_READ);
    if (h >= 0) {
        int64_t n = mint_fs_read((uint64_t)h, note_buffer, NOTE_RAM_MAX);
        if (n > 0) note_length = (size_t)n;
        note_buffer[note_length] = '\0';
        (void)mint_fs_close((uint64_t)h);
    }
    if (migrate && note_length) { notes_dirty = 1u; notes_edit_ms = 0; }
    notes_loaded = 1u;
}

static void save_notes(void) {
    if (notes_on_disk) { notes_dirty = 1u; notes_edit_ms = term_ms(); return; }
    int64_t h = mint_fs_open("NOTES.TXT", MINT_FS_OPEN_WRITE | MINT_FS_OPEN_TRUNC);
    if (h < 0) return;
    (void)mint_fs_write((uint64_t)h, note_buffer, note_length > NOTE_RAM_MAX ? NOTE_RAM_MAX : note_length);
    (void)mint_fs_close((uint64_t)h);
}
static void notes_flush(void) {
    if (!notes_on_disk || !notes_dirty || !notes_path[0]) return;
    int64_t rc = mint_file_save(notes_path, note_buffer, note_length);
    notes_save_failed = rc < 0;
    if (rc >= 0) notes_dirty = 0;
    else notes_edit_ms = term_ms() + 5000u;              /* retry later */
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

/* ====================================================================== */
/*  MintDE 2 application views                                            */
/* ====================================================================== */
#define S (&gfx_screen)

static uint64_t ui_ticks;              /* advanced by app_tick (cursor blink) */
static uint32_t settings_category = 1u; /* Appearance */
static uint32_t show_changelog;         /* About: 0 = details, 1 = release notes */
static uint32_t driver_view;            /* 0 drivers, 1 PCI, 2 USB, 3 power */
static uint32_t monitor_tab;
static int32_t files_scroll;
static char files_search[32];
static uint32_t files_search_focus;
static char terminal_last_command[TERM_COLS];
static uint32_t network_busy_message;
static char network_status[96];

static int32_t BL(gfx_rect r, int font) {
    return r.y0 + (r.y1 - r.y0 - gfx_font_line(font)) / 2 + gfx_font_ascent(font);
}
static gfx_rect R(int32_t x, int32_t y, int32_t w, int32_t h) { return gfx_rect_xywh(x, y, w, h); }
static int in(gfx_rect r, int32_t x, int32_t y) { return gfx_rect_contains(r, x, y); }
static int32_t W(gfx_rect r) { return r.x1 - r.x0; }
static int32_t H(gfx_rect r) { return r.y1 - r.y0; }
static int ci_contains(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    for (size_t i = 0; hay[i]; ++i) {
        size_t j = 0;
        while (needle[j] && hay[i + j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            ++j;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ metadata */
static const char *const app_names[APP_COUNT] = {
    "Files", "Notes", "Terminal", "Settings", "System Monitor", "Storage & Logs",
    "Hardware & Drivers", "Network", "Disk Utility", "About MintOS", "About MintDE", "Launchpad", "Calculator", "Web"
};
const char *app_name(uint32_t app) { return app < APP_COUNT ? app_names[app] : ""; }

const uint8_t *app_icon(uint32_t app, int large) {
    switch (app) {
    case APP_FILES: return large ? mint_icon_files_128 : mint_icon_files_64;
    case APP_NOTES: return large ? mint_icon_notes_128 : mint_icon_notes_64;
    case APP_TERMINAL: return large ? mint_icon_terminal_128 : mint_icon_terminal_64;
    case APP_SETTINGS: return large ? mint_icon_settings_128 : mint_icon_settings_64;
    case APP_MONITOR: return large ? mint_icon_system_monitor_128 : mint_icon_system_monitor_64;
    case APP_STORAGE: return large ? mint_icon_console_128 : mint_icon_console_64;
    case APP_DRIVERS: return large ? mint_icon_system_info_128 : mint_icon_system_info_64;
    case APP_NETWORK: return large ? mint_icon_network_128 : mint_icon_network_64;
    case APP_DISKS: return large ? mint_icon_disk_utility_128 : mint_icon_disk_utility_64;
    case APP_ABOUT_OS: return large ? mint_icon_about_mintos_128 : mint_icon_about_mintos_64;
    case APP_ABOUT_MINTDE: return large ? mint_icon_mintde_128 : mint_icon_mintde_64;
    case APP_LAUNCHPAD: return large ? mint_icon_launchpad_128 : mint_icon_launchpad_64;
    case APP_CALCULATOR: return large ? mint_icon_calculator_128 : mint_icon_calculator_64;
    case APP_WEB: return large ? mint_icon_web_128 : mint_icon_web_64;
    default: return large ? mint_icon_trash_128 : mint_icon_trash_64;
    }
}

int app_chrome(uint32_t app) {
    switch (app) {
    case APP_TERMINAL: case APP_NETWORK: return CHROME_TITLE;
    case APP_ABOUT_OS: case APP_ABOUT_MINTDE: case APP_CALCULATOR: return CHROME_PLAIN;
    default: return CHROME_TOOLBAR;
    }
}
int32_t app_sidebar_width(uint32_t app) {
    switch (app) {
    case APP_FILES: return 190;
    case APP_NOTES: return 230;
    case APP_SETTINGS: return 230;
    case APP_STORAGE: case APP_DRIVERS: return 200;
    case APP_DISKS: return 220;
    default: return 0;
    }
}
void app_default_size(uint32_t app, int32_t *w, int32_t *h) {
    static const int16_t sz[APP_COUNT][2] = {
        {920, 560}, {780, 540}, {880, 520}, {860, 620}, {880, 560}, {940, 600},
        {900, 580}, {680, 600}, {900, 580}, {560, 380}, {560, 380}, {0, 0}, {240, 372}, {1000, 700}
    };
    *w = app < APP_COUNT ? sz[app][0] : 600;
    *h = app < APP_COUNT ? sz[app][1] : 400;
}
const char *app_window_title(uint32_t app) {
    if (app == APP_TERMINAL) return term_mfs_active() ? "mintos \xE2\x80\x94 mintsh \xE2\x80\x94 MintOS HD"
                                                                   : "root \xE2\x80\x94 mintos \xE2\x80\x94 bounded shell";
    if (app == APP_WEB) return web_title();
    return app_name(app);
}

static void toolbar_title(const mde_layout *l, const char *title, int with_nav) {
    int32_t x = l->toolbar.x0 + (W(l->sidebar) ? 16 : 92);
    if (with_nav) {
        ui_symbol_glyph(x + 6, l->toolbar.y0 + 26, SYM_CHEVRON_LEFT, OPAQUE(T.text2));
        ui_symbol_glyph(x + 30, l->toolbar.y0 + 26, SYM_CHEVRON_RIGHT, OPAQUE(T.text3));
        x += 50;
    }
    ui_label(x, BL(R(0, l->toolbar.y0, 1, 52), FONT_HEADLINE), FONT_HEADLINE, title, T.text);
}

/* ------------------------------------------------------------ Files */
/* Locations: MintOS HD (read/write MintFS, Finder-style), FAT16 volumes
 * (read-only) and the session RAM store. files_volume selects one. */
#define FILES_MFS 0xFFFFu                 /* files_volume value for MintOS HD */
#define FILES_MAX 256u
static MINT_MFS_DIRENT mfs_entries[FILES_MAX];
static uint32_t mfs_entry_count;
static char mfs_path[MINT_MFS_PATH_MAX] = "/";
static MINT_MFS_VOLUME mfs_volume;
static int mfs_ready, files_started;
static uint32_t files_renaming, files_rename_select;   /* select: the name (without extension) is selected */
static size_t files_rename_caret;                       /* byte offset of the caret */
static char files_rename_text[MINT_MFS_NAME_MAX + 1];
enum { FILES_CONFIRM_NONE = 0, FILES_CONFIRM_DELETE, FILES_CONFIRM_EMPTY_TRASH };
static uint32_t files_confirm;
static char files_message[96];
static uint64_t files_message_at;
static char files_work[MINT_MFS_PATH_MAX], files_work2[MINT_MFS_PATH_MAX];

static int files_on_mfs(void) { return files_volume == FILES_MFS; }
static uint32_t files_count(void) { return files_on_mfs() ? mfs_entry_count : files_volume ? fat_entry_count : fs_entry_count; }
static const char *files_entry_name(uint32_t i) {
    return files_on_mfs() ? mfs_entries[i].name : files_volume ? fat_entries[i].name : fs_entries[i].name;
}
static uint64_t files_entry_size(uint32_t i) {
    return files_on_mfs() ? mfs_entries[i].size : files_volume ? fat_entries[i].size : fs_entries[i].size;
}
static int files_entry_is_dir(uint32_t i) {
    if (files_on_mfs()) return mfs_entries[i].type == MINT_MFS_DIR;
    return files_volume && (fat_entries[i].flags & MINT_FAT16_DIRENT_DIRECTORY);
}
static const char *files_kind(uint32_t i) {
    if (files_entry_is_dir(i)) return "Folder";
    const char *n = files_entry_name(i);
    size_t len = text_length(n), dot = len;
    while (dot && n[dot - 1u] != '.') --dot;
    if (!dot || dot == 1u) return "Document";
    const char *e = n + dot;
    size_t el = len - dot;
    if (el == 3 && ci_contains(e, "txt")) return "Plain Text";
    if (el == 3 && ci_contains(e, "png")) return "PNG Image";
    if (el == 3 && ci_contains(e, "elf")) return "Executable";
    if (el == 3 && ci_contains(e, "rgb")) return "Raw Image";
    if (el == 3 && ci_contains(e, "dat")) return "Data";
    if (el == 1 && (e[0] == 'c' || e[0] == 'C')) return "C Source";
    if (el == 1 && (e[0] == 'h' || e[0] == 'H')) return "C Header";
    if (el == 2 && ci_contains(e, "md")) return "Markdown";
    if ((el == 4 && ci_contains(e, "html")) || (el == 3 && ci_contains(e, "htm"))) return "HTML Document";
    return "Document";
}
static int files_is_text(uint32_t i) {
    const char *k = files_kind(i);
    return !files_entry_is_dir(i) && (k[0] == 'P' || k[0] == 'C' || k[0] == 'M' || k[0] == 'H' || k[0] == 'D');
}
static void volume_label(uint32_t vi, char *out, size_t cap) {
    copy_text(out, cap, "Volume");
    if (vi >= fat_volume_list.count) return;
    uint32_t block = fat_volume_list.block_indices[vi];
    for (uint32_t d = 0; d < storage_snapshot.device_count && d < MINT_STORAGE_MAX_DEVICES; ++d)
        if (storage_snapshot.devices[d].block_registered && storage_snapshot.devices[d].block_index == block) {
            copy_text(out, cap, "MINTOS26 (");
            str_append(out, cap, storage_snapshot.devices[d].name);
            str_append(out, cap, ")");
            return;
        }
}

/* ---- MintOS HD helpers */
static int same_text(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static int name_order(const char *a, const char *b) {      /* Finder: case-insensitive */
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}
static int mfs_join(char *out, size_t cap, const char *dir, const char *name) {
    size_t n = text_length(dir), m = text_length(name);
    size_t slash = n && dir[n - 1u] == '/' ? 0u : 1u;
    if (!m || n + slash + m + 1u > cap) return 0;
    copy_text(out, cap, dir);
    if (slash) str_append(out, cap, "/");
    str_append(out, cap, name);
    return 1;
}
static void mfs_parent(char *path) {
    size_t n = text_length(path);
    while (n > 1u && path[n - 1u] != '/') --n;
    path[n > 1u ? n - 1u : 1u] = '\0';
    if (!path[0]) { path[0] = '/'; path[1] = '\0'; }
}
static void mfs_trash_path(char *out, size_t cap) { copy_text(out, cap, mfs_volume.home); str_append(out, cap, "/.Trash"); }
static int mfs_in_trash(void) {
    char t[96]; mfs_trash_path(t, sizeof(t));
    size_t n = text_length(t);
    for (size_t i = 0; i < n; ++i) if (mfs_path[i] != t[i]) return 0;
    return mfs_path[n] == '\0' || mfs_path[n] == '/';
}
static void mfs_refresh_volume(void) {
    mfs_ready = mint_fs_get_volume(&mfs_volume) == 0 && mfs_volume.state == MINT_MFS_VOLUME_READY;
}
static int disk_is_mintos(const MINT_STORAGE_DEVICE_INFO *d) {
    return mfs_ready && d->block_registered && d->block_index == mfs_volume.block_index;
}
static void refresh_mfs_files(void) {
    static MINT_MFS_DIRENT chunk[MINT_MFS_LIST_MAX];
    mfs_entry_count = 0;
    for (uint32_t start = 0; mfs_entry_count < FILES_MAX;) {
        int64_t n = mint_dir_list(mfs_path, start, chunk);
        if (n <= 0) break;
        for (int64_t i = 0; i < n && mfs_entry_count < FILES_MAX; ++i)
            if (chunk[i].name[0] != '.') mfs_entries[mfs_entry_count++] = chunk[i];   /* hidden items */
        start += (uint32_t)n;
        if (n < (int64_t)MINT_MFS_LIST_MAX) break;
    }
    for (uint32_t i = 1; i < mfs_entry_count; ++i) {
        MINT_MFS_DIRENT t = mfs_entries[i];
        uint32_t j = i;
        while (j && name_order(mfs_entries[j - 1u].name, t.name) > 0) { mfs_entries[j] = mfs_entries[j - 1u]; --j; }
        mfs_entries[j] = t;
    }
}
static void files_note(const char *text) {
    copy_text(files_message, sizeof(files_message), text);
    files_message_at = term_ms();
}
static const char *mfs_error_text(int64_t rc) {
    switch (-rc) {
    case MINT_EEXIST: return "An item with that name already exists.";
    case MINT_ENOSPC: return "MintOS HD is full.";
    case MINT_ENOTEMPTY: return "The folder isn\xE2\x80\x99t empty.";
    case MINT_ENAMETOOLONG: return "The name is too long.";
    case MINT_EINVAL: return "That name or location can\xE2\x80\x99t be used.";
    case MINT_EBUSY: return "The item is in use.";
    case MINT_ENOENT: return "The item no longer exists.";
    case MINT_ENODEV: case MINT_EROFS: return "MintOS HD isn\xE2\x80\x99t available.";
    case MINT_EIO: return "A disk error occurred; restart to recover.";
    default: return "The operation couldn\xE2\x80\x99t be completed.";
    }
}
static int32_t mfs_find(const char *name) {
    for (uint32_t i = 0; i < mfs_entry_count; ++i) if (same_text(mfs_entries[i].name, name)) return (int32_t)i;
    return -1;
}
static void files_go_mfs(const char *path) {
    files_volume = FILES_MFS;
    copy_text(mfs_path, sizeof(mfs_path), path);
    files_selected_entry = -1; files_preview_active = 0; files_scroll = 0; files_search[0] = '\0';
    files_renaming = 0; files_confirm = 0;
    refresh_mfs_files();
}
static void mfs_go_up(void) {
    if (same_text(mfs_path, "/")) return;
    char leaf[MINT_MFS_NAME_MAX + 1];
    size_t n = text_length(mfs_path), s = n;
    while (s && mfs_path[s - 1u] != '/') --s;
    copy_text(leaf, sizeof(leaf), mfs_path + s);
    copy_text(files_work, sizeof(files_work), mfs_path);
    mfs_parent(files_work);
    files_go_mfs(files_work);
    files_selected_entry = mfs_find(leaf);           /* Finder keeps the folder selected */
}
static void mfs_select(uint32_t i) {
    files_selected_entry = (int32_t)i; files_preview_active = 0; files_preview_length = 0; files_preview[0] = '\0';
    if (i >= mfs_entry_count || mfs_entries[i].type == MINT_MFS_DIR) return;
    if (!mfs_join(files_work, sizeof(files_work), mfs_path, mfs_entries[i].name)) return;
    int64_t n = mint_file_load(files_work, files_preview, sizeof(files_preview) - 1u);
    if (n < 0) return;
    files_preview_length = (size_t)n; files_preview[files_preview_length] = '\0'; files_preview_active = 1u;
}
static void notes_open_file(const char *path);
static void mfs_open(uint32_t i) {
    if (i >= mfs_entry_count || !mfs_join(files_work2, sizeof(files_work2), mfs_path, mfs_entries[i].name)) return;
    if (mfs_entries[i].type == MINT_MFS_DIR) { files_go_mfs(files_work2); return; }
    if (files_is_text(i)) { notes_open_file(files_work2); shell_open_app(APP_NOTES); return; }
    files_note("MintOS has no app to open this kind of file yet.");
}
/* Unique sibling name: "base", "base 2", ... or "stem copy.ext", "stem copy 2.ext". */
static int mfs_unique(char *out, size_t cap, const char *dir, const char *name, int copy) {
    size_t len = text_length(name), dot = len;
    while (dot && name[dot - 1u] != '.') --dot;
    size_t stem = copy && dot > 1u ? dot - 1u : len;
    MINT_MFS_STAT st;
    for (uint32_t k = 1; k < 1000u; ++k) {
        char cand[MINT_MFS_NAME_MAX + 1];
        size_t o = 0;
        for (size_t q = 0; q < stem && o + 1u < sizeof(cand); ++q) cand[o++] = name[q];
        cand[o] = '\0';
        if (copy) str_append(cand, sizeof(cand), " copy");
        if (k > 1u) { char nb[12]; format_u64(nb, sizeof(nb), k); str_append(cand, sizeof(cand), " "); str_append(cand, sizeof(cand), nb); }
        if (stem < len) str_append(cand, sizeof(cand), name + stem);
        if (!mfs_join(out, cap, dir, cand)) return 0;
        if (mint_path_stat(out, &st) == -MINT_ENOENT) return 1;
    }
    return 0;
}
/* Deletes a file or a whole folder tree (MintFS removes only empty folders). */
static int64_t mfs_delete_tree(const char *path) {
    static char cur[MINT_MFS_PATH_MAX], child[MINT_MFS_PATH_MAX];
    static MINT_MFS_DIRENT list[MINT_MFS_LIST_MAX];
    MINT_MFS_STAT st;
    int64_t rc = mint_path_stat(path, &st);
    if (rc < 0) return rc;
    if (st.type != MINT_MFS_DIR) return mint_path_remove(path);
    copy_text(cur, sizeof(cur), path);
    size_t root = text_length(path);
    for (uint32_t guard = 0; guard < 200000u; ++guard) {
        int64_t n = mint_dir_list(cur, 0, list);
        if (n < 0) return n;
        if (n == 0) {
            if ((rc = mint_path_remove(cur)) < 0) return rc;
            if (text_length(cur) <= root) return 0;
            mfs_parent(cur);
            continue;
        }
        for (int64_t i = 0; i < n; ++i) {
            if (!mfs_join(child, sizeof(child), cur, list[i].name)) return -MINT_ENAMETOOLONG;
            if (list[i].type == MINT_MFS_DIR) { copy_text(cur, sizeof(cur), child); break; }
            if ((rc = mint_path_remove(child)) < 0) return rc;
        }
    }
    return -MINT_EIO;
}
static void files_after_change(const char *select_name) {
    refresh_mfs_files();
    files_selected_entry = select_name ? mfs_find(select_name) : -1;
    files_preview_active = 0;
    if (files_selected_entry >= 0) mfs_select((uint32_t)files_selected_entry);
}
static void files_new_folder(void) {
    if (!files_on_mfs() || !mfs_ready) return;
    for (uint32_t k = 1; k < 200u; ++k) {
        char name[40]; copy_text(name, sizeof(name), "untitled folder");
        if (k > 1u) { char nb[12]; format_u64(nb, sizeof(nb), k); str_append(name, sizeof(name), " "); str_append(name, sizeof(name), nb); }
        if (!mfs_join(files_work, sizeof(files_work), mfs_path, name)) { files_note(mfs_error_text(-MINT_ENAMETOOLONG)); return; }
        int64_t rc = mint_dir_create(files_work);
        if (rc == -MINT_EEXIST) continue;
        if (rc < 0) { files_note(mfs_error_text(rc)); return; }
        files_search[0] = '\0';
        files_after_change(name);
        files_renaming = 1; files_rename_select = 1; copy_text(files_rename_text, sizeof(files_rename_text), name);
        files_rename_caret = text_length(files_rename_text);
        return;
    }
}
static void files_begin_rename(void) {
    if (!files_on_mfs() || files_selected_entry < 0 || (uint32_t)files_selected_entry >= mfs_entry_count) return;
    files_renaming = 1; files_rename_select = 1;
    copy_text(files_rename_text, sizeof(files_rename_text), mfs_entries[files_selected_entry].name);
    files_rename_caret = text_length(files_rename_text);
}
/* Finder selects the name without its extension when renaming a file. */
static size_t files_rename_stem(void) {
    size_t n = text_length(files_rename_text), dot = n;
    int dir = files_selected_entry >= 0 && (uint32_t)files_selected_entry < mfs_entry_count &&
              mfs_entries[files_selected_entry].type == MINT_MFS_DIR;
    if (dir) return n;
    while (dot && files_rename_text[dot - 1u] != '.') --dot;
    return dot > 1u ? dot - 1u : n;
}
static void files_rename_replace_stem(void) {
    size_t stem = files_rename_stem(), n = text_length(files_rename_text), o = 0;
    for (size_t i = stem; i <= n; ++i) files_rename_text[o++] = files_rename_text[i];
    files_rename_select = 0; files_rename_caret = 0;
}
static void files_commit_rename(void) {
    if (!files_renaming) return;
    files_renaming = 0;
    if (files_selected_entry < 0 || (uint32_t)files_selected_entry >= mfs_entry_count) return;
    const char *old = mfs_entries[files_selected_entry].name;
    if (!files_rename_text[0] || same_text(old, files_rename_text)) return;
    if (files_rename_text[0] == '.') { files_note("Names that begin with a dot are reserved for the system."); return; }
    if (!mfs_join(files_work, sizeof(files_work), mfs_path, old) ||
        !mfs_join(files_work2, sizeof(files_work2), mfs_path, files_rename_text)) { files_note(mfs_error_text(-MINT_ENAMETOOLONG)); return; }
    MINT_MFS_STAT st;
    /* Changing only the case of a name is a rename onto itself. */
    if (!(name_order(old, files_rename_text) == 0) && mint_path_stat(files_work2, &st) == 0) {
        files_note("An item with that name already exists. Choose a different name."); return;
    }
    int64_t rc = mint_path_rename(files_work, files_work2);
    if (rc < 0) { files_note(mfs_error_text(rc)); return; }
    char keep[MINT_MFS_NAME_MAX + 1]; copy_text(keep, sizeof(keep), files_rename_text);
    files_after_change(keep);
}
static void files_duplicate(void) {
    if (!files_on_mfs() || files_selected_entry < 0 || (uint32_t)files_selected_entry >= mfs_entry_count) return;
    MINT_MFS_DIRENT *e = &mfs_entries[files_selected_entry];
    if (e->type == MINT_MFS_DIR) { files_note("Folders can\xE2\x80\x99t be duplicated yet."); return; }
    static uint8_t buf[16384];
    if (!mfs_join(files_work, sizeof(files_work), mfs_path, e->name) ||
        !mfs_unique(files_work2, sizeof(files_work2), mfs_path, e->name, 1)) { files_note(mfs_error_text(-MINT_ENAMETOOLONG)); return; }
    int64_t in_fd = mint_file_open(files_work, MINT_O_READ);
    if (in_fd < 0) { files_note(mfs_error_text(in_fd)); return; }
    int64_t out_fd = mint_file_open(files_work2, MINT_O_WRITE | MINT_O_CREATE | MINT_O_EXCL);
    if (out_fd < 0) { (void)mint_file_close(in_fd); files_note(mfs_error_text(out_fd)); return; }
    int64_t rc = 0;
    for (;;) {
        int64_t n = mint_file_read(in_fd, buf, sizeof(buf));
        if (n <= 0) { rc = n; break; }
        if ((rc = mint_file_write(out_fd, buf, (size_t)n)) < 0) break;
    }
    (void)mint_file_close(in_fd);
    int64_t c = mint_file_close(out_fd);
    if (rc < 0 || c < 0) { (void)mint_path_remove(files_work2); files_note(mfs_error_text(rc < 0 ? rc : c)); return; }
    size_t s = text_length(files_work2);
    while (s && files_work2[s - 1u] != '/') --s;
    char keep[MINT_MFS_NAME_MAX + 1]; copy_text(keep, sizeof(keep), files_work2 + s);
    files_after_change(keep);
}
static void files_move_to_trash(void) {
    if (!files_on_mfs() || files_selected_entry < 0 || (uint32_t)files_selected_entry >= mfs_entry_count) return;
    if (mfs_in_trash()) { files_confirm = FILES_CONFIRM_DELETE; return; }
    char trash[96]; mfs_trash_path(trash, sizeof(trash));
    int64_t rc = mint_dir_create(trash);
    if (rc < 0 && rc != -MINT_EEXIST) { files_note(mfs_error_text(rc)); return; }
    const char *name = mfs_entries[files_selected_entry].name;
    if (!mfs_join(files_work, sizeof(files_work), mfs_path, name) ||
        !mfs_unique(files_work2, sizeof(files_work2), trash, name, 0)) { files_note(mfs_error_text(-MINT_ENAMETOOLONG)); return; }
    if ((rc = mint_path_rename(files_work, files_work2)) < 0) { files_note(mfs_error_text(rc)); return; }
    uint32_t was = (uint32_t)files_selected_entry;
    files_after_change(0);
    if (mfs_entry_count) mfs_select(was < mfs_entry_count ? was : mfs_entry_count - 1u);
}
static void files_delete_now(void) {
    files_confirm = 0;
    if (files_selected_entry < 0 || (uint32_t)files_selected_entry >= mfs_entry_count) return;
    if (!mfs_join(files_work2, sizeof(files_work2), mfs_path, mfs_entries[files_selected_entry].name)) return;
    int64_t rc = mfs_delete_tree(files_work2);
    if (rc < 0) files_note(mfs_error_text(rc));
    files_after_change(0);
}
static void files_empty_trash(void) {
    static MINT_MFS_DIRENT list[MINT_MFS_LIST_MAX];
    char trash[96]; mfs_trash_path(trash, sizeof(trash));
    files_confirm = 0;
    for (uint32_t guard = 0; guard < 100000u; ++guard) {
        int64_t n = mint_dir_list(trash, 0, list);
        if (n <= 0) break;
        if (!mfs_join(files_work2, sizeof(files_work2), trash, list[0].name)) break;
        int64_t rc = mfs_delete_tree(files_work2);
        if (rc < 0) { files_note(mfs_error_text(rc)); break; }
    }
    if (files_on_mfs()) files_after_change(0);
}
static uint32_t mfs_trash_items(void) {
    static MINT_MFS_DIRENT one[MINT_MFS_LIST_MAX];
    char trash[96]; mfs_trash_path(trash, sizeof(trash));
    int64_t n = mint_dir_list(trash, 0, one);
    return n > 0 ? (uint32_t)n : 0u;
}

int files_command_enabled(uint32_t command) {
    if (!files_on_mfs() || !mfs_ready) return 0;
    int sel = files_selected_entry >= 0 && (uint32_t)files_selected_entry < mfs_entry_count;
    switch (command) {
    case FILES_CMD_NEW_FOLDER: return 1;
    case FILES_CMD_RENAME: case FILES_CMD_TRASH: return sel;
    case FILES_CMD_DUPLICATE: return sel && mfs_entries[files_selected_entry].type != MINT_MFS_DIR;
    case FILES_CMD_EMPTY_TRASH: return mfs_trash_items() > 0u;
    default: return 0;
    }
}
int files_command(uint32_t command) {
    if (!files_command_enabled(command)) return 0;
    files_commit_rename();
    switch (command) {
    case FILES_CMD_NEW_FOLDER: files_new_folder(); break;
    case FILES_CMD_RENAME: files_begin_rename(); break;
    case FILES_CMD_DUPLICATE: files_duplicate(); break;
    case FILES_CMD_TRASH: files_move_to_trash(); break;
    case FILES_CMD_EMPTY_TRASH: files_confirm = FILES_CONFIRM_EMPTY_TRASH; break;
    default: return 0;
    }
    return 1;
}
int files_show_trash(void) {
    mfs_refresh_volume();
    if (!mfs_ready) return 0;
    char trash[96]; mfs_trash_path(trash, sizeof(trash));
    (void)mint_dir_create(trash);
    files_started = 1;
    files_go_mfs(trash);
    return 1;
}

/* ---- sidebar (Finder: Favorites + Locations) */
typedef struct { const char *label; uint32_t symbol; uint32_t volume; const char *path; } files_place;
static files_place places[16];
static uint32_t place_count, places_fav;
static char place_paths[5][80];
static char place_fat_labels[MINT_FAT16_MAX_VOLUMES][40];
static void files_build_places(void) {
    uint32_t n = 0; places_fav = 0;
    if (mfs_ready) {
        static const char *const sub[5] = {0, "Applications", "Desktop", "Documents", "Downloads"};
        static const uint32_t sym[5] = {SYM_HOUSE, SYM_APPS, SYM_DESKTOP, SYM_DOC, SYM_DOWNLOAD};
        for (uint32_t i = 0; i < 5u; ++i) {
            if (i == 1u) copy_text(place_paths[i], sizeof(place_paths[i]), "/Applications");
            else {
                copy_text(place_paths[i], sizeof(place_paths[i]), mfs_volume.home);
                if (sub[i]) { str_append(place_paths[i], sizeof(place_paths[i]), "/"); str_append(place_paths[i], sizeof(place_paths[i]), sub[i]); }
            }
            places[n++] = (files_place){i ? sub[i] : MINT_MFS_HOME_NAME, sym[i], FILES_MFS, place_paths[i]};
        }
        places_fav = n;
        places[n++] = (files_place){mfs_volume.label[0] ? mfs_volume.label : "MintOS HD", SYM_DRIVE, FILES_MFS, "/"};
    }
    for (uint32_t v = 0; v < fat_volume_list.count && v < MINT_FAT16_MAX_VOLUMES && n < 15u; ++v) {
        volume_label(v, place_fat_labels[v], sizeof(place_fat_labels[v]));
        places[n++] = (files_place){place_fat_labels[v], SYM_DRIVE, v + 1u, 0};
    }
    places[n++] = (files_place){"Session (RAM)", SYM_RAM, 0, 0};
    place_count = n;
}
static gfx_rect files_place_rect(const mde_layout *l, uint32_t i) {
    gfx_rect sb = l->sidebar;
    int32_t y = sb.y0 + 28 + 30 * (int32_t)i;
    if (places_fav && i >= places_fav) y += 28;          /* room for the "Locations" header */
    return R(sb.x0 + 10, y, W(sb) - 20, 28);
}
static int files_place_selected(uint32_t i) {
    if (places[i].volume != files_volume) return 0;
    return !files_on_mfs() || same_text(places[i].path, mfs_path);
}
static void files_choose_place(uint32_t i) {
    files_renaming = 0; files_confirm = 0;
    if (places[i].volume == FILES_MFS) { files_go_mfs(places[i].path); return; }
    files_volume = places[i].volume; files_path[0] = '\0'; files_selected_entry = -1; files_preview_active = 0;
    files_scroll = 0; files_search[0] = '\0';
    if (files_volume) refresh_fat_files(); else refresh_files();
}

static uint32_t files_visible(uint32_t *idx, uint32_t cap) {
    uint32_t n = 0, total = files_count();
    for (uint32_t i = 0; i < total && n < cap; ++i)
        if (ci_contains(files_entry_name(i), files_search)) idx[n++] = i;
    return n;
}
static gfx_rect files_list_rect(const mde_layout *l) {
    gfx_rect c = l->content;
    if (files_preview_active || (files_selected_entry >= 0 && !files_entry_is_dir((uint32_t)files_selected_entry)))
        c.x1 -= 280;
    c.y1 -= 26;
    return c;
}
static gfx_rect files_search_rect(const mde_layout *l) { return R(l->toolbar.x1 - 196, l->toolbar.y0 + 12, 180, 28); }
static gfx_rect files_newfolder_rect(const mde_layout *l) { gfx_rect s = files_search_rect(l); return R(s.x0 - 84, s.y0, 34, 28); }
static gfx_rect files_trash_rect(const mde_layout *l) { gfx_rect s = files_search_rect(l); return R(s.x0 - 44, s.y0, 34, 28); }
static gfx_rect files_empty_rect(const mde_layout *l) { gfx_rect s = files_search_rect(l); return R(s.x0 - 84, s.y0, 72, 28); }
static gfx_rect files_alert_rect(const mde_layout *l) {
    int32_t w = 420, h = 132;
    return R(l->frame.x0 + (W(l->frame) - w) / 2, l->content.y0 - 1, w, h);
}
static int files_dates_shown(gfx_rect lr) { return files_on_mfs() && W(lr) >= 560; }

static void files_title(char *out, size_t cap) {
    if (files_on_mfs()) {
        if (same_text(mfs_path, "/")) { copy_text(out, cap, mfs_volume.label[0] ? mfs_volume.label : "MintOS HD"); return; }
        if (mfs_in_trash() && same_text(mfs_path + text_length(mfs_path) - 7u, "/.Trash")) { copy_text(out, cap, "Trash"); return; }
        size_t n = text_length(mfs_path);
        while (n && mfs_path[n - 1u] != '/') --n;
        copy_text(out, cap, mfs_path + n);
        return;
    }
    if (files_path[0]) {
        size_t n = text_length(files_path);
        while (n && files_path[n - 1u] != '/') --n;
        copy_text(out, cap, files_path + n);
    } else if (files_volume) volume_label(files_volume - 1u, out, cap);
    else copy_text(out, cap, "Session (RAM)");
}

static void files_paint(const mde_layout *l) {
    mfs_refresh_volume();
    if (files_on_mfs() && !mfs_ready) { files_volume = 0; refresh_files(); }
    files_build_places();
    gfx_rect sb = l->sidebar;
    if (places_fav) {
        ui_sidebar_header(sb.x0 + 18, sb.y0 + 18, "Favorites");
        ui_sidebar_header(sb.x0 + 18, files_place_rect(l, places_fav).y0 - 10, "Locations");
    } else ui_sidebar_header(sb.x0 + 18, sb.y0 + 18, "Locations");
    for (uint32_t i = 0; i < place_count; ++i)
        ui_sidebar_row(files_place_rect(l, i), places[i].label, places[i].symbol, files_place_selected(i));
    /* toolbar */
    char title[64]; files_title(title, sizeof(title));
    int32_t tx = l->toolbar.x0 + 16;
    int can_up = files_on_mfs() ? !same_text(mfs_path, "/") : files_path[0] != '\0';
    ui_symbol_glyph(tx + 8, l->toolbar.y0 + 26, SYM_CHEVRON_LEFT, OPAQUE(can_up ? T.text : T.text3));
    ui_symbol_glyph(tx + 34, l->toolbar.y0 + 26, SYM_CHEVRON_RIGHT, OPAQUE(T.text3));
    ui_label(tx + 56, BL(R(0, l->toolbar.y0, 1, 52), FONT_HEADLINE), FONT_HEADLINE, title, T.text);
    if (files_on_mfs() && mfs_in_trash()) {
        ui_button(files_empty_rect(l), "Empty", 0, mfs_entry_count > 0u);
    } else if (files_on_mfs()) {
        gfx_rect nf = files_newfolder_rect(l), tr = files_trash_rect(l);
        int sel = files_selected_entry >= 0;
        ui_symbol_glyph(nf.x0 + W(nf) / 2, nf.y0 + H(nf) / 2, SYM_FOLDER_PLUS, OPAQUE(T.text2));
        ui_symbol_glyph(tr.x0 + W(tr) / 2, tr.y0 + H(tr) / 2, SYM_TRASH, OPAQUE(sel ? T.text2 : T.text3));
    }
    ui_search_field(files_search_rect(l), files_search, files_search_focus);
    /* list */
    gfx_rect lr = files_list_rect(l);
    gfx_fill(S, lr.x0, lr.y0, W(lr), H(l->content), OPAQUE(T.content));
    int dates = files_dates_shown(lr);
    int32_t name_x = lr.x0 + 16, size_r = lr.x1 - 170, kind_x = lr.x1 - 150, date_x = lr.x1 - 370;
    int32_t name_w = (dates ? date_x - 12 : size_r - 100) - (name_x + 24);
    ui_label(name_x, lr.y0 + 17, FONT_CAPTION_BOLD, "Name", T.text2);
    if (dates) ui_label(date_x, lr.y0 + 17, FONT_CAPTION_BOLD, "Date Modified", T.text2);
    ui_label_right(size_r, lr.y0 + 17, FONT_CAPTION_BOLD, "Size", T.text2);
    ui_label(kind_x, lr.y0 + 17, FONT_CAPTION_BOLD, "Kind", T.text2);
    gfx_hline(S, lr.x0, lr.y0 + 25, W(lr), T.separator);
    uint32_t idx[FILES_MAX];
    uint32_t n = files_visible(idx, FILES_MAX);
    int32_t rows = (H(lr) - 26) / 24;
    if (files_scroll > (int32_t)n - rows) files_scroll = (int32_t)n - rows;
    if (files_scroll < 0) files_scroll = 0;
    for (int32_t r = 0; r < rows && (uint32_t)(r + files_scroll) < n; ++r) {
        uint32_t i = idx[r + files_scroll];
        gfx_rect row = R(lr.x0, lr.y0 + 26 + r * 24, W(lr), 24);
        int sel = files_selected_entry == (int32_t)i;
        ui_list_row(R(row.x0 + 8, row.y0, W(row) - 16, 24), (uint32_t)r, sel, l->focused);
        if (sel) gfx_round_rect(S, row.x0 + 8, row.y0, W(row) - 16, 24, 5, OPAQUE(l->focused ? T.accent : T.selection_inactive));
        uint32_t fg = sel && l->focused ? 0xFFFFFFu : T.text, fg2 = sel && l->focused ? 0xFFFFFFu : T.text2;
        ui_symbol_glyph(name_x + 8, row.y0 + 12, files_entry_is_dir(i) ? SYM_FOLDER : SYM_DOC,
                        OPAQUE(sel && l->focused ? 0xFFFFFFu : (files_entry_is_dir(i) ? 0x4FA8F5u : T.text2)));
        if (sel && files_renaming) {
            gfx_rect fr = R(name_x + 20, row.y0 + 1, name_w + 8, 22);
            ui_text_field_caret(fr, files_rename_text, "", 1, files_rename_select ? files_rename_stem() : files_rename_caret);
            if (files_rename_select) {
                char stem[MINT_MFS_NAME_MAX + 1]; size_t sl = files_rename_stem();
                for (size_t q = 0; q < sl; ++q) stem[q] = files_rename_text[q];
                stem[sl] = '\0';
                int32_t sw = gfx_text_width(FONT_BODY, stem);
                if (sw > W(fr) - 16) sw = W(fr) - 16;
                gfx_round_rect(S, fr.x0 + 6, fr.y0 + 3, sw + 4, H(fr) - 6, 3, WITH_ALPHA(T.accent, 0x55));
            }
        } else {
            gfx_text_fit(S, FONT_BODY, name_x + 24, BL(row, FONT_BODY), files_entry_name(i), name_w, OPAQUE(fg));
        }
        char sz[40];
        if (dates) { shell_format_date(sz, sizeof(sz), mfs_entries[i].mtime); gfx_text_fit(S, FONT_BODY, date_x, BL(row, FONT_BODY), sz, size_r - date_x - 80, OPAQUE(fg2)); }
        if (files_entry_is_dir(i)) copy_text(sz, sizeof(sz), "--"); else format_bytes(sz, sizeof(sz), files_entry_size(i));
        ui_label_right(size_r, BL(row, FONT_BODY), FONT_BODY, sz, fg2);
        ui_label(kind_x, BL(row, FONT_BODY), FONT_BODY, files_kind(i), fg2);
    }
    if (!n) ui_label_center(lr.x0 + W(lr) / 2, lr.y0 + 90, FONT_BODY,
                            files_search[0] ? "No Results" : (files_on_mfs() && mfs_in_trash()) ? "Trash is empty" : "This folder is empty", T.text3);
    /* preview pane (Quick Look style) */
    if (lr.x1 < l->content.x1) {
        gfx_rect pv = R(lr.x1, l->content.y0, l->content.x1 - lr.x1, H(l->content) - 26);
        gfx_fill(S, pv.x0, pv.y0, W(pv), H(pv), OPAQUE(T.window));
        gfx_vline(S, pv.x0, pv.y0, H(pv), T.separator);
        if (files_selected_entry >= 0 && (uint32_t)files_selected_entry < files_count()) {
            uint32_t i = (uint32_t)files_selected_entry;
            gfx_round_rect(S, pv.x0 + 20, pv.y0 + 18, W(pv) - 40, 200, 8, OPAQUE(T.content));
            gfx_round_rect_stroke(S, pv.x0 + 20, pv.y0 + 18, W(pv) - 40, 200, 8, T.separator);
            int32_t ty = pv.y0 + 36;
            if (files_preview_active) {
                /* word-wrapped text preview */
                char line[96]; size_t li = 0; int32_t lines = 0, maxw = W(pv) - 64;
                for (size_t k = 0; k <= files_preview_length && lines < 11; ++k) {
                    char ch = k < files_preview_length ? files_preview[k] : '\n';
                    if (ch == '\r') continue;
                    if (ch == '\t') ch = ' ';
                    if (ch != '\n' && (unsigned char)ch >= 32u && li + 1u < sizeof(line)) {
                        line[li++] = ch; line[li] = '\0';
                        if (gfx_text_width(FONT_CAPTION, line) <= maxw) continue;
                        size_t cut = li - 1u;
                        while (cut && line[cut] != ' ') --cut;
                        if (!cut) cut = li - 1u;
                        char rest[96]; size_t rn = 0;
                        for (size_t q = cut + (line[cut] == ' ' ? 1u : 0u); q < li; ++q) rest[rn++] = line[q];
                        line[cut] = '\0';
                        ui_label(pv.x0 + 32, ty, FONT_CAPTION, line, T.text);
                        ty += 16; ++lines;
                        for (size_t q = 0; q < rn; ++q) line[q] = rest[q];
                        li = rn; line[li] = '\0';
                        continue;
                    }
                    if (ch == '\n') {
                        line[li] = '\0';
                        ui_label(pv.x0 + 32, ty, FONT_CAPTION, line, T.text);
                        ty += 16; ++lines; li = 0;
                    }
                }
            } else {
                ui_label_center(pv.x0 + W(pv) / 2, pv.y0 + 120, FONT_BODY, "No preview", T.text3);
            }
            gfx_text_fit(S, FONT_HEADLINE, pv.x0 + 20, pv.y0 + 246, files_entry_name(i), W(pv) - 40, OPAQUE(T.text));
            char sz[32]; format_bytes(sz, sizeof(sz), files_entry_size(i));
            char meta[64]; copy_text(meta, sizeof(meta), files_kind(i)); str_append(meta, sizeof(meta), " \xE2\x80\x94 ");
            str_append(meta, sizeof(meta), sz);
            ui_label(pv.x0 + 20, pv.y0 + 268, FONT_BODY, meta, T.text2);
            ui_label(pv.x0 + 20, pv.y0 + 300, FONT_CAPTION_BOLD, "Information", T.text2);
            gfx_hline(S, pv.x0 + 20, pv.y0 + 308, W(pv) - 40, T.separator);
            ui_label(pv.x0 + 20, pv.y0 + 328, FONT_CAPTION, "Access", T.text2);
            ui_label_right(pv.x1 - 20, pv.y0 + 328, FONT_CAPTION, files_on_mfs() ? "Read & Write" : "Read only", T.text);
            ui_label(pv.x0 + 20, pv.y0 + 348, FONT_CAPTION, "Where", T.text2);
            ui_label_right(pv.x1 - 20, pv.y0 + 348, FONT_CAPTION,
                           files_on_mfs() ? (mfs_volume.label[0] ? mfs_volume.label : "MintOS HD") : files_volume ? "FAT16 volume" : "Session (RAM)", T.text);
            if (files_on_mfs()) {
                char when[40]; shell_format_date(when, sizeof(when), mfs_entries[i].mtime);
                ui_label(pv.x0 + 20, pv.y0 + 368, FONT_CAPTION, "Modified", T.text2);
                ui_label_right(pv.x1 - 20, pv.y0 + 368, FONT_CAPTION, when, T.text);
            }
        }
    }
    /* path / status bar */
    gfx_rect st = R(l->content.x0, l->content.y1 - 26, W(l->content), 26);
    gfx_fill(S, st.x0, st.y0, W(st), 26, OPAQUE(T.window));
    gfx_hline(S, st.x0, st.y0, W(st), T.separator);
    char status[96];
    if (files_message[0] && term_ms() - files_message_at < 6000u) {
        copy_text(status, sizeof(status), files_message);
        ui_symbol_glyph(st.x0 + W(st) / 2 - gfx_text_width(FONT_CAPTION, status) / 2 - 14, st.y0 + 13, SYM_WARNING, OPAQUE(0xFF9500u));
    } else {
        format_u64(status, sizeof(status), n);
        str_append(status, sizeof(status), n == 1 ? " item" : " items");
        if (files_on_mfs()) {
            char fb[32]; format_bytes(fb, sizeof(fb), mfs_volume.free_bytes);
            str_append(status, sizeof(status), ", "); str_append(status, sizeof(status), fb); str_append(status, sizeof(status), " available");
        } else str_append(status, sizeof(status), files_volume ? ", read-only volume" : ", volatile session storage");
    }
    ui_label_center(st.x0 + W(st) / 2, BL(st, FONT_CAPTION), FONT_CAPTION, status, T.text2);
    /* confirmation alert (sheet) */
    if (files_confirm) {
        gfx_rect sh = files_alert_rect(l);
        gfx_shadow(S, sh.x0, sh.y0, W(sh), H(sh), 10, 28, 12, ARGB(0x60, 0, 0, 0));
        gfx_round_rect(S, sh.x0, sh.y0, W(sh), H(sh), 10, OPAQUE(T.window));
        gfx_round_rect_stroke(S, sh.x0, sh.y0, W(sh), H(sh), 10, T.separator);
        gfx_image(S, sh.x0 + 20, sh.y0 + 20, 48, 48, mint_icon_trash_128, 128, 128, 255u);
        char t1[96];
        if (files_confirm == FILES_CONFIRM_EMPTY_TRASH) copy_text(t1, sizeof(t1), "Permanently erase the items in the Trash?");
        else {
            copy_text(t1, sizeof(t1), "Delete \xE2\x80\x9C");
            if (files_selected_entry >= 0 && (uint32_t)files_selected_entry < mfs_entry_count) str_append(t1, sizeof(t1), mfs_entries[files_selected_entry].name);
            str_append(t1, sizeof(t1), "\xE2\x80\x9D immediately?");
        }
        gfx_text_fit(S, FONT_HEADLINE, sh.x0 + 84, sh.y0 + 38, t1, W(sh) - 104, OPAQUE(T.text));
        ui_label(sh.x0 + 84, sh.y0 + 62, FONT_CAPTION, "You can\xE2\x80\x99t undo this action.", T.text2);
        ui_button(R(sh.x1 - 228, sh.y1 - 46, 96, 28), "Cancel", 0, 1);
        ui_button(R(sh.x1 - 122, sh.y1 - 46, 102, 28), files_confirm == FILES_CONFIRM_EMPTY_TRASH ? "Empty Trash" : "Delete", 1, 1);
    }
}

static int files_click(const mde_layout *l, int32_t x, int32_t y, uint32_t clicks) {
    if (files_confirm) {
        gfx_rect sh = files_alert_rect(l);
        if (in(R(sh.x1 - 228, sh.y1 - 46, 96, 28), x, y)) files_confirm = 0;
        else if (in(R(sh.x1 - 122, sh.y1 - 46, 102, 28), x, y)) {
            if (files_confirm == FILES_CONFIRM_EMPTY_TRASH) files_empty_trash(); else files_delete_now();
        }
        return 1;
    }
    gfx_rect lr = files_list_rect(l);
    if (files_renaming) {
        /* a click inside the edited name keeps editing; anywhere else commits it (Finder) */
        if (in(lr, x, y) && y >= lr.y0 + 26 && (y - lr.y0 - 26) / 24 + files_scroll >= 0) {
            uint32_t idx[FILES_MAX]; uint32_t n = files_visible(idx, FILES_MAX);
            int32_t r = (y - lr.y0 - 26) / 24 + files_scroll;
            if (r >= 0 && (uint32_t)r < n && (int32_t)idx[r] == files_selected_entry) return 1;
        }
        files_commit_rename();
    }
    files_search_focus = in(files_search_rect(l), x, y);
    if (files_search_focus) return 1;
    if (in(l->sidebar, x, y)) {
        for (uint32_t i = 0; i < place_count; ++i)
            if (in(files_place_rect(l, i), x, y)) { files_choose_place(i); break; }
        return 1;
    }
    if (in(R(l->toolbar.x0 + 12, l->toolbar.y0 + 12, 24, 28), x, y)) {
        if (files_on_mfs()) mfs_go_up(); else files_go_up();
        files_scroll = 0; return 1;
    }
    if (files_on_mfs() && mfs_in_trash()) {
        if (in(files_empty_rect(l), x, y)) { if (mfs_entry_count) files_confirm = FILES_CONFIRM_EMPTY_TRASH; return 1; }
    } else if (files_on_mfs()) {
        if (in(files_newfolder_rect(l), x, y)) { files_new_folder(); return 1; }
        if (in(files_trash_rect(l), x, y)) { files_move_to_trash(); return 1; }
    }
    if (in(lr, x, y) && y >= lr.y0 + 26) {
        uint32_t idx[FILES_MAX];
        uint32_t n = files_visible(idx, FILES_MAX);
        int32_t r = (y - lr.y0 - 26) / 24 + files_scroll;
        if (r < 0 || (uint32_t)r >= n) { files_selected_entry = -1; files_preview_active = 0; return 1; }
        uint32_t i = idx[r];
        if (files_on_mfs()) {
            if (clicks >= 2) { mfs_open(i); files_search[0] = '\0'; }
            else mfs_select(i);
            return 1;
        }
        if (files_entry_is_dir(i)) {
            if (clicks >= 2) { files_select_entry(i); files_scroll = 0; files_search[0] = '\0'; }
            else { files_selected_entry = (int32_t)i; files_preview_active = 0; }
        } else files_select_entry(i);
        return 1;
    }
    return 0;
}

/* Appends a typed codepoint to a UTF-8 buffer (Finder rename, Notes). */
static int utf8_append(char *buf, size_t *len, size_t cap, uint32_t cp) {
    char t[4]; size_t n;
    if (cp < 0x80u) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800u) { t[0] = (char)(0xC0u | (cp >> 6)); t[1] = (char)(0x80u | (cp & 0x3Fu)); n = 2; }
    else if (cp < 0x10000u) { t[0] = (char)(0xE0u | (cp >> 12)); t[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); t[2] = (char)(0x80u | (cp & 0x3Fu)); n = 3; }
    else return 0;
    if (*len + n + 1u > cap) return 0;
    for (size_t i = 0; i < n; ++i) buf[(*len)++] = t[i];
    buf[*len] = '\0';
    return 1;
}
static void utf8_backspace(char *buf, size_t *len) {
    if (!*len) return;
    do --*len; while (*len && ((unsigned char)buf[*len] & 0xC0u) == 0x80u);
    buf[*len] = '\0';
}

static int files_key(const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    int cmd = (k->modifiers & MINT_KEYMOD_GUI) != 0, shift = (k->modifiers & MINT_KEYMOD_SHIFT) != 0;
    int alt = (k->modifiers & MINT_KEYMOD_ALT) != 0;
    if (files_confirm) {
        if (ch == 27u) files_confirm = 0;
        else if (ch == '\n') { if (files_confirm == FILES_CONFIRM_EMPTY_TRASH) files_empty_trash(); else files_delete_now(); }
        return 1;
    }
    if (files_renaming) {
        if (ch == 27u) { files_renaming = 0; return 1; }
        if (ch == '\n' || ch == 9u) { files_commit_rename(); return 1; }
        size_t len = text_length(files_rename_text);
        if (files_rename_caret > len) files_rename_caret = len;
        if (k->scancode == 0x14Bu || k->scancode == 0x14Du) {           /* left / right */
            if (files_rename_select) { files_rename_caret = k->scancode == 0x14Bu ? 0 : files_rename_stem(); files_rename_select = 0; return 1; }
            if (k->scancode == 0x14Bu) { while (files_rename_caret && ((unsigned char)files_rename_text[--files_rename_caret] & 0xC0u) == 0x80u) {} }
            else if (files_rename_caret < len) { do ++files_rename_caret; while (files_rename_caret < len && ((unsigned char)files_rename_text[files_rename_caret] & 0xC0u) == 0x80u); }
            return 1;
        }
        if (files_rename_select && (ch == 8u || (!cmd && ch >= 32u && ch != 127u && ch != '/'))) files_rename_replace_stem();
        if (ch == 8u) {
            if (files_rename_caret) {
                size_t from = files_rename_caret;
                while (files_rename_caret && ((unsigned char)files_rename_text[--files_rename_caret] & 0xC0u) == 0x80u) {}
                for (size_t i = from; i <= len; ++i) files_rename_text[files_rename_caret + i - from] = files_rename_text[i];
            }
        } else if (!cmd && ch >= 32u && ch != 127u && ch != '/') {
            char tail[MINT_MFS_NAME_MAX + 1];
            copy_text(tail, sizeof(tail), files_rename_text + files_rename_caret);
            files_rename_text[files_rename_caret] = '\0';
            size_t n = files_rename_caret;
            if (utf8_append(files_rename_text, &n, sizeof(files_rename_text), ch) && n + text_length(tail) < sizeof(files_rename_text)) {
                files_rename_caret = n;
                str_append(files_rename_text, sizeof(files_rename_text), tail);
            } else {
                files_rename_text[files_rename_caret] = '\0';              /* too long: restore */
                str_append(files_rename_text, sizeof(files_rename_text), tail);
            }
        }
        return 1;
    }
    if (files_search_focus) {
        size_t n = text_length(files_search);
        if (ch == 8u) { if (n) files_search[n - 1u] = '\0'; }
        else if (ch == 27u || ch == '\n') files_search_focus = 0;
        else if (ch >= 32u && ch < 127u && n + 1u < sizeof(files_search)) { files_search[n] = (char)ch; files_search[n + 1u] = '\0'; }
        files_scroll = 0;
        return 1;
    }
    if (files_on_mfs()) {
        if (cmd && shift && (ch == 'n' || ch == 'N')) return files_command(FILES_CMD_NEW_FOLDER) || 1;
        if (cmd && ch == 8u) {
            if (shift) return files_command(FILES_CMD_EMPTY_TRASH) || 1;
            if (alt) { if (files_selected_entry >= 0) files_confirm = FILES_CONFIRM_DELETE; return 1; }
            return files_command(FILES_CMD_TRASH) || 1;
        }
        if (cmd && (ch == 'd' || ch == 'D')) return files_command(FILES_CMD_DUPLICATE) || 1;
        if (cmd && k->scancode == 0x148u) { mfs_go_up(); return 1; }
        if (cmd && (k->scancode == 0x150u || ch == 'o' || ch == 'O')) {
            if (files_selected_entry >= 0) mfs_open((uint32_t)files_selected_entry);
            return 1;
        }
        if (ch == '\n') { files_begin_rename(); return 1; }
        if (ch == 8u) { mfs_go_up(); return 1; }
    }
    if (cmd) return 0;
    uint32_t idx[FILES_MAX];
    uint32_t n = files_visible(idx, FILES_MAX);
    int32_t pos = -1;
    for (uint32_t i = 0; i < n; ++i) if ((int32_t)idx[i] == files_selected_entry) pos = (int32_t)i;
    if (k->scancode == 0x150u && n) { pos = pos + 1 < (int32_t)n ? pos + 1 : pos; }
    else if (k->scancode == 0x148u && n) { pos = pos > 0 ? pos - 1 : 0; }
    else if (ch == '\n' && pos >= 0) { files_select_entry(idx[pos]); files_scroll = 0; return 1; }
    else if (ch == 8u) { files_go_up(); files_scroll = 0; return 1; }
    else return 0;
    if (pos >= 0) {
        uint32_t i = idx[pos];
        if (files_entry_is_dir(i)) { files_selected_entry = (int32_t)i; files_preview_active = 0; }
        else if (files_on_mfs()) mfs_select(i);
        else files_select_entry(i);
        if (pos < files_scroll) files_scroll = pos;
        if (pos >= files_scroll + 12) files_scroll = pos - 11;
    }
    return 1;
}

/* ------------------------------------------------------------ Terminal on MintOS HD
 * When a MintOS disk is mounted the shell works there (home folder first),
 * with the usual commands; `use sda` switches back to a read-only FAT16 volume. */
static int terminal_mfs_mode = 1;
static char term_cwd[MINT_MFS_PATH_MAX];
static char term_path_a[MINT_MFS_PATH_MAX], term_path_b[MINT_MFS_PATH_MAX], term_norm[MINT_MFS_PATH_MAX];

static int term_mfs_active(void) {
    mfs_refresh_volume();
    if (mfs_ready && !term_cwd[0]) copy_text(term_cwd, sizeof(term_cwd), mfs_volume.home);
    return terminal_mfs_mode && mfs_ready;
}
static const char *terminal_prompt(void) { return term_mfs_active() ? "mintos@MintOS % " : "root#mintos > "; }

/* Shell path (absolute, relative, ~, ., ..) -> normalised absolute path. */
static int term_resolve(const char *arg, char *out, size_t cap) {
    if (!arg || !arg[0] || same_text(arg, "~")) { copy_text(out, cap, mfs_volume.home); return 1; }
    if (arg[0] == '/') copy_text(term_norm, sizeof(term_norm), arg);
    else if (arg[0] == '~' && arg[1] == '/') { copy_text(term_norm, sizeof(term_norm), mfs_volume.home); str_append(term_norm, sizeof(term_norm), arg + 1); }
    else { copy_text(term_norm, sizeof(term_norm), term_cwd); str_append(term_norm, sizeof(term_norm), "/"); str_append(term_norm, sizeof(term_norm), arg); }
    copy_text(out, cap, "/");
    size_t i = 0, n = text_length(term_norm);
    while (i < n) {
        while (i < n && term_norm[i] == '/') ++i;
        size_t s = i;
        while (i < n && term_norm[i] != '/') ++i;
        size_t len = i - s;
        if (!len || (len == 1 && term_norm[s] == '.')) continue;
        if (len == 2 && term_norm[s] == '.' && term_norm[s + 1] == '.') { mfs_parent(out); continue; }
        if (len > MINT_MFS_NAME_MAX || text_length(out) + len + 2u > cap) return 0;
        if (!same_text(out, "/")) str_append(out, cap, "/");
        size_t o = text_length(out);
        for (size_t q = 0; q < len; ++q) out[o + q] = term_norm[s + q];
        out[o + len] = '\0';
    }
    return 1;
}
/* Splits "cmd a "b c" d" into up to `max` arguments (quotes allow spaces). */
static int term_args(const char *s, char out[][MINT_MFS_NAME_MAX + 1], int max) {
    int n = 0;
    while (*s && *s != ' ') ++s;                       /* skip the command word */
    while (n < max) {
        while (*s == ' ') ++s;
        if (!*s) break;
        size_t o = 0;
        char quote = (*s == '"' || *s == '\'') ? *s++ : 0;
        while (*s && (quote ? *s != quote : *s != ' ')) { if (o < MINT_MFS_NAME_MAX) out[n][o++] = *s; ++s; }
        if (quote && *s) ++s;
        out[n++][o] = '\0';
    }
    return n;
}
static void term_error(const char *what, const char *arg, int64_t rc) {
    char row[TERM_COLS];
    copy_text(row, sizeof(row), what); str_append(row, sizeof(row), ": ");
    if (arg && arg[0]) { str_append(row, sizeof(row), arg); str_append(row, sizeof(row), ": "); }
    str_append(row, sizeof(row), mfs_error_text(rc));
    terminal_push(row);
}
static int term_is_dir(const char *path) {
    MINT_MFS_STAT st;
    return mint_path_stat(path, &st) == 0 && st.type == MINT_MFS_DIR;
}
/* "dst" may name an existing folder: then the item keeps its name inside it. */
static int term_target(const char *src, const char *dst_arg, char *out, size_t cap) {
    if (!term_resolve(dst_arg, out, cap)) return 0;
    if (term_is_dir(out)) {
        size_t s = text_length(src);
        while (s && src[s - 1u] != '/') --s;
        char leaf[MINT_MFS_NAME_MAX + 1]; copy_text(leaf, sizeof(leaf), src + s);
        if (!same_text(out, "/")) str_append(out, cap, "/");
        if (text_length(out) + text_length(leaf) + 1u > cap) return 0;
        str_append(out, cap, leaf);
    }
    return 1;
}
static int64_t mfs_copy_file(const char *from, const char *to) {
    static uint8_t buf[16384];
    int64_t in_fd = mint_file_open(from, MINT_O_READ);
    if (in_fd < 0) return in_fd;
    int64_t out_fd = mint_file_open(to, MINT_O_WRITE | MINT_O_CREATE | MINT_O_TRUNC);
    if (out_fd < 0) { (void)mint_file_close(in_fd); return out_fd; }
    int64_t rc = 0;
    for (;;) {
        int64_t n = mint_file_read(in_fd, buf, sizeof(buf));
        if (n <= 0) { rc = n; break; }
        if ((rc = mint_file_write(out_fd, buf, (size_t)n)) < 0) break;
    }
    (void)mint_file_close(in_fd);
    int64_t c = mint_file_close(out_fd);
    return rc < 0 ? rc : c;
}
static void term_cat(const char *path) {
    static char text[8192];
    int64_t n = mint_file_load(path, text, sizeof(text) - 1u);
    if (n < 0) { term_error("cat", path, n); return; }
    text[n] = '\0';
    char row[TERM_COLS]; size_t o = 0;
    for (int64_t i = 0; i <= n; ++i) {
        char ch = i < n ? text[i] : '\n';
        if (ch == '\r') continue;
        if (ch == '\n' || o + 1u >= TERM_WRAP) {
            if (i == n && o == 0) break;                     /* file ends with a newline */
            row[o] = '\0'; terminal_push(row); o = 0;
            if (ch != '\n') row[o++] = ch;
            if (i == n) break;
            continue;
        }
        row[o++] = ch == '\t' ? ' ' : ch;
    }
    if (n == (int64_t)sizeof(text) - 1) terminal_push("... (first 8 KB shown)");
}
static void term_ls(const char *path, int longform) {
    static MINT_MFS_DIRENT list[128], chunk[MINT_MFS_LIST_MAX];
    uint32_t count = 0;
    for (uint32_t start = 0; count < 128u;) {
        int64_t n = mint_dir_list(path, start, chunk);
        if (n < 0) { term_error("ls", path, n); return; }
        if (n == 0) break;
        for (int64_t i = 0; i < n && count < 128u; ++i) if (chunk[i].name[0] != '.') list[count++] = chunk[i];
        start += (uint32_t)n;
        if (n < (int64_t)MINT_MFS_LIST_MAX) break;
    }
    for (uint32_t i = 1; i < count; ++i) {
        MINT_MFS_DIRENT t = list[i]; uint32_t j = i;
        while (j && name_order(list[j - 1u].name, t.name) > 0) { list[j] = list[j - 1u]; --j; }
        list[j] = t;
    }
    if (!count) { terminal_push(longform ? "total 0" : "(empty folder)"); return; }
    for (uint32_t i = 0; i < count; ++i) {
        char row[TERM_COLS * 2];
        if (longform) {
            char sz[24], when[40];
            copy_text(row, sizeof(row), list[i].type == MINT_MFS_DIR ? "drwxr-xr-x  " : "-rw-r--r--  ");
            if (list[i].type == MINT_MFS_DIR) copy_text(sz, sizeof(sz), "--"); else format_bytes(sz, sizeof(sz), list[i].size);
            for (size_t pad = text_length(sz); pad < 11u; ++pad) str_append(row, sizeof(row), " ");
            str_append(row, sizeof(row), sz); str_append(row, sizeof(row), "  ");
            shell_format_date(when, sizeof(when), list[i].mtime);
            str_append(row, sizeof(row), when);
            for (size_t pad = text_length(when); pad < 20u; ++pad) str_append(row, sizeof(row), " ");
            str_append(row, sizeof(row), "  ");
        } else row[0] = '\0';
        str_append(row, sizeof(row), list[i].name);
        if (list[i].type == MINT_MFS_DIR) str_append(row, sizeof(row), "/");
        row[TERM_COLS - 1u] = '\0';
        terminal_push(row);
    }
}

static int terminal_mfs_command(const char *cmd) {
    if (terminal_equals(cmd, "use hd") || terminal_equals(cmd, "use mintos")) {
        mfs_refresh_volume();
        if (!mfs_ready) { terminal_push("No MintOS disk. Disk Utility can make a blank disk a MintOS disk."); return 1; }
        terminal_mfs_mode = 1; terminal_push("Using MintOS HD (read/write). 'use sda' switches to a FAT16 volume."); return 1;
    }
    if (!term_mfs_active()) return 0;
    static char a[3][MINT_MFS_NAME_MAX + 1];
    int argc = term_args(cmd, a, 3);
    char word[16]; size_t w = 0;
    while (cmd[w] && cmd[w] != ' ' && w + 1u < sizeof(word)) { word[w] = cmd[w]; ++w; }
    word[w] = '\0';
    int64_t rc;
    if (same_text(word, "pwd")) { terminal_push(term_cwd); return 1; }
    if (same_text(word, "cd")) {
        if (!term_resolve(argc ? a[0] : "~", term_path_a, sizeof(term_path_a))) { terminal_push("cd: path is too long"); return 1; }
        if (!term_is_dir(term_path_a)) { term_error("cd", argc ? a[0] : "~", -MINT_ENOENT); return 1; }
        copy_text(term_cwd, sizeof(term_cwd), term_path_a);
        return 1;
    }
    if (same_text(word, "ls")) {
        int longform = argc && same_text(a[0], "-l");
        const char *target = argc > longform ? a[longform] : ".";
        if (!term_resolve(target, term_path_a, sizeof(term_path_a))) { terminal_push("ls: path is too long"); return 1; }
        if (!term_is_dir(term_path_a)) {
            MINT_MFS_STAT st;
            if ((rc = mint_path_stat(term_path_a, &st)) < 0) { term_error("ls", target, rc); return 1; }
            terminal_push(target);
            return 1;
        }
        term_ls(term_path_a, longform);
        return 1;
    }
    if (same_text(word, "cat") || same_text(word, "open")) {
        if (!argc) { terminal_push(same_text(word, "cat") ? "usage: cat <file>" : "usage: open <file>"); return 1; }
        if (!term_resolve(a[0], term_path_a, sizeof(term_path_a))) { terminal_push("path is too long"); return 1; }
        if (same_text(word, "open")) {
            if (term_is_dir(term_path_a)) { files_go_mfs(term_path_a); files_started = 1; shell_open_app(APP_FILES); }
            else { MINT_MFS_STAT st; if ((rc = mint_path_stat(term_path_a, &st)) < 0) { term_error("open", a[0], rc); return 1; }
                   notes_open_file(term_path_a); shell_open_app(APP_NOTES); }
            return 1;
        }
        term_cat(term_path_a);
        return 1;
    }
    if (same_text(word, "mkdir") || same_text(word, "rmdir") || same_text(word, "touch") || same_text(word, "rm")) {
        int recursive = same_text(word, "rm") && argc && (same_text(a[0], "-r") || same_text(a[0], "-rf"));
        if (argc <= recursive) { char u[48]; copy_text(u, sizeof(u), "usage: "); str_append(u, sizeof(u), word); str_append(u, sizeof(u), recursive || same_text(word, "rm") ? " [-r] <path>" : " <path>"); terminal_push(u); return 1; }
        const char *arg = a[recursive];
        if (!term_resolve(arg, term_path_a, sizeof(term_path_a)) || same_text(term_path_a, "/")) { terminal_push("That path can't be changed."); return 1; }
        if (same_text(word, "mkdir")) rc = mint_dir_create(term_path_a);
        else if (same_text(word, "touch")) {
            MINT_MFS_STAT st;
            if (mint_path_stat(term_path_a, &st) == 0) return 1;
            rc = mint_file_open(term_path_a, MINT_O_WRITE | MINT_O_CREATE);
            if (rc >= 0) rc = mint_file_close(rc);
        } else if (same_text(word, "rmdir")) {
            rc = term_is_dir(term_path_a) ? mint_path_remove(term_path_a) : -MINT_ENOTDIR;
        } else {
            if (term_is_dir(term_path_a) && !recursive) { terminal_push("rm: is a folder (use rm -r or rmdir)"); return 1; }
            rc = recursive ? mfs_delete_tree(term_path_a) : mint_path_remove(term_path_a);
        }
        if (rc < 0) term_error(word, arg, rc);
        else if ((same_text(word, "rm") || same_text(word, "rmdir")) && term_cwd[0]) {
            /* never leave the shell inside a deleted folder */
            size_t n = text_length(term_path_a);
            int inside = 1;
            for (size_t i = 0; i < n; ++i) if (term_cwd[i] != term_path_a[i]) { inside = 0; break; }
            if (inside && (term_cwd[n] == '\0' || term_cwd[n] == '/')) { copy_text(term_cwd, sizeof(term_cwd), term_path_a); mfs_parent(term_cwd); }
        }
        return 1;
    }
    if (same_text(word, "mv") || same_text(word, "cp")) {
        if (argc < 2) { terminal_push(same_text(word, "mv") ? "usage: mv <from> <to>" : "usage: cp <file> <to>"); return 1; }
        if (!term_resolve(a[0], term_path_a, sizeof(term_path_a)) ||
            !term_target(term_path_a, a[1], term_path_b, sizeof(term_path_b))) { terminal_push("path is too long"); return 1; }
        if (same_text(word, "mv")) rc = mint_path_rename(term_path_a, term_path_b);
        else rc = term_is_dir(term_path_a) ? -MINT_EISDIR : mfs_copy_file(term_path_a, term_path_b);
        if (rc < 0) term_error(word, a[0], rc);
        return 1;
    }
    if (same_text(word, "echo")) {
        const char *text = cmd + 4;
        while (*text == ' ') ++text;
        const char *gt = text;
        while (*gt && *gt != '>') ++gt;
        if (!*gt) { terminal_push(text); return 1; }
        int append = gt[1] == '>';
        char body[TERM_COLS]; size_t bl = (size_t)(gt - text);
        while (bl && text[bl - 1u] == ' ') --bl;
        if (bl >= 2u && (text[0] == '"' || text[0] == '\'') && text[bl - 1u] == text[0]) { ++text; bl -= 2u; }
        if (bl + 2u > sizeof(body)) bl = sizeof(body) - 2u;
        for (size_t i = 0; i < bl; ++i) body[i] = text[i];
        body[bl++] = '\n'; body[bl] = '\0';
        const char *name = gt + (append ? 2 : 1);
        while (*name == ' ') ++name;
        if (!*name || !term_resolve(name, term_path_a, sizeof(term_path_a))) { terminal_push("echo: missing file name"); return 1; }
        rc = mint_file_open(term_path_a, MINT_O_WRITE | MINT_O_CREATE | (append ? MINT_O_APPEND : MINT_O_TRUNC));
        if (rc >= 0) { int64_t fd = rc; rc = mint_file_write(fd, body, bl); int64_t c = mint_file_close(fd); if (rc >= 0) rc = c; }
        if (rc < 0) term_error("echo", name, rc);
        return 1;
    }
    if (same_text(word, "df")) {
        char row[TERM_COLS], n1[32], n2[32], n3[32];
        format_bytes(n1, sizeof(n1), mfs_volume.total_bytes);
        format_bytes(n2, sizeof(n2), mfs_volume.total_bytes - mfs_volume.free_bytes);
        format_bytes(n3, sizeof(n3), mfs_volume.free_bytes);
        terminal_push("Volume        Size      Used      Avail     Mounted on");
        copy_text(row, sizeof(row), mfs_volume.label);
        for (size_t p = text_length(row); p < 14u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), n1); for (size_t p = text_length(n1); p < 10u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), n2); for (size_t p = text_length(n2); p < 10u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), n3); for (size_t p = text_length(n3); p < 10u; ++p) str_append(row, sizeof(row), " ");
        str_append(row, sizeof(row), "/ (MintFS, ");
        str_append(row, sizeof(row), mfs_volume.device); str_append(row, sizeof(row), ")");
        terminal_push(row);
        return 1;
    }
    if (same_text(word, "sync")) { rc = mint_fs_flush(); if (rc < 0) term_error("sync", 0, rc); return 1; }
    return 0;
}

/* ------------------------------------------------------------ Notes */
static void notes_title(char *out, size_t cap) {
    size_t i = 0;
    while (i < note_length && note_buffer[i] != '\n' && i + 1u < cap) { out[i] = note_buffer[i]; ++i; }
    out[i] = '\0';
    if (!out[0]) copy_text(out, cap, "New Note");
}
static void notes_paint(const mde_layout *l) {
    load_notes();
    gfx_rect sb = l->sidebar;
    ui_sidebar_header(sb.x0 + 18, sb.y0 + 18, notes_on_disk ? "On My MintOS" : "Session");
    gfx_rect item = R(sb.x0 + 10, sb.y0 + 28, W(sb) - 20, 52);
    gfx_round_rect(S, item.x0, item.y0, W(item), H(item), 7, OPAQUE(l->focused ? 0xF7C833u : T.selection_inactive));
    char title[40]; notes_title(title, sizeof(title));
    gfx_text_fit(S, FONT_BODY_BOLD, item.x0 + 12, item.y0 + 20, title, W(item) - 24, OPAQUE(l->focused ? 0x1D1D1Fu : T.text));
    char preview[40]; size_t p = 0, i = 0;
    while (i < note_length && note_buffer[i] != '\n') ++i;
    if (i < note_length) ++i;
    while (i < note_length && p + 1u < sizeof(preview)) { char c = note_buffer[i++]; preview[p++] = c == '\n' ? ' ' : c; }
    preview[p] = '\0';
    char sub[64]; copy_text(sub, sizeof(sub), "Today  "); str_append(sub, sizeof(sub), p ? preview : "No additional text");
    gfx_text_fit(S, FONT_CAPTION, item.x0 + 12, item.y0 + 40, sub, W(item) - 24, OPAQUE(l->focused ? 0x4A4030u : T.text2));
    toolbar_title(l, "Notes", 0);
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.content));
    char where[MINT_MFS_NAME_MAX + 48];
    if (!notes_on_disk) copy_text(where, sizeof(where), "Session (RAM) \xE2\x80\x94 cleared at restart");
    else if (notes_readonly) copy_text(where, sizeof(where), "Too large to edit in Notes \xE2\x80\x94 read only");
    else if (notes_save_failed) copy_text(where, sizeof(where), "Not saved \xE2\x80\x94 MintOS HD reported an error");
    else {
        size_t s0 = text_length(notes_path);
        while (s0 && notes_path[s0 - 1u] != '/') --s0;
        copy_text(where, sizeof(where), notes_path + s0);
        str_append(where, sizeof(where), notes_dirty ? " \xE2\x80\x94 Edited" : " \xE2\x80\x94 Saved on MintOS HD");
    }
    ui_label_center(c.x0 + W(c) / 2, c.y0 + 22, FONT_CAPTION, where, T.text3);
    /* word-wrapped editor */
    int32_t x = c.x0 + 28, y = c.y0 + 54, maxw = W(c) - 56, line_h = 21;
    char line[160]; size_t ln = 0; int first = 1;
    size_t k = 0;
    int32_t caret_x = x, caret_y = y;
    while (k <= note_length) {
        char ch = k < note_length ? note_buffer[k] : '\n';
        if (ch == '\n' || ln + 1u >= sizeof(line)) {
            line[ln] = '\0';
            ui_label(x, y, first ? FONT_TITLE : FONT_BODY, line, T.text);
            caret_x = x + gfx_text_width(first ? FONT_TITLE : FONT_BODY, line); caret_y = y;
            y += first ? 32 : line_h; ln = 0; first = 0;
            ++k;
            if (k > note_length) break;
            continue;
        }
        line[ln++] = ch; line[ln] = '\0';
        if (gfx_text_width(first ? FONT_TITLE : FONT_BODY, line) > maxw) {
            size_t cut = ln - 1u;
            while (cut && line[cut] != ' ') --cut;
            if (!cut) cut = ln - 1u;
            char rest[160]; size_t rn = 0;
            for (size_t q = cut + (line[cut] == ' ' ? 1u : 0u); q < ln; ++q) rest[rn++] = line[q];
            line[cut] = '\0';
            ui_label(x, y, first ? FONT_TITLE : FONT_BODY, line, T.text);
            y += first ? 32 : line_h; first = 0;
            for (size_t q = 0; q < rn; ++q) line[q] = rest[q];
            ln = rn;
        }
        ++k;
        if (y > c.y1 - 10) break;
    }
    if (l->focused && ((ui_ticks / 50u) & 1u) == 0u)
        gfx_fill(S, caret_x + 1, caret_y - (note_length && note_buffer[note_length - 1] != '\n' && caret_y == c.y0 + 54 ? 20 : 14),
                 2, note_length && caret_y == c.y0 + 54 ? 24 : 17, OPAQUE(0xE0A800u));
}
static void notes_open_file(const char *path) {
    notes_flush();
    copy_text(notes_path, sizeof(notes_path), path);
    notes_loaded = 0;
    load_notes();
}
static int notes_key(const MINT_DESKTOP_KEY_EVENT *k) {
    load_notes();
    uint32_t ch = k->codepoint;
    if (k->modifiers & MINT_KEYMOD_GUI) {
        if ((ch == 's' || ch == 'S') && notes_on_disk) { notes_flush(); return 1; }
        return 0;
    }
    if (notes_readonly) return 0;
    size_t cap = notes_on_disk ? sizeof(note_buffer) : NOTE_RAM_MAX + 1u;
    if (ch == 8u) utf8_backspace(note_buffer, &note_length);
    else if (ch == '\n' || (ch >= 32u && ch != 127u)) { if (!utf8_append(note_buffer, &note_length, cap, ch)) return 1; }
    else return 0;
    save_notes();
    return 1;
}

/* ------------------------------------------------------------ Terminal network jobs
 * Network commands never block the desktop: each starts a job that the shell
 * advances once per frame through app_background(), even while Terminal is
 * in the background. Ctrl+C cancels. */
enum { JOB_NONE = 0, JOB_NET_UP, JOB_PING, JOB_LOOKUP, JOB_HTTP };
enum { PH_RESOLVE = 0, PH_RUN, PH_CONNECT, PH_TLS, PH_RECEIVE };
static struct term_job {
    uint32_t kind, phase;
    char host[80], path[160];
    uint16_t port;
    uint8_t head_only, waiting, silent, secure;
    uint32_t ip, count, sent, received;
    uint64_t started, sent_at, next_at, last_data;
    uint64_t rtt_sum, rtt_min, rtt_max;
    int64_t handle;
    uint64_t bytes;
    uint32_t lines;
    char line[TERM_COLS];
    size_t line_len;
} job = {.handle = -1};
static MINT_NET_INFO net_info;
static uint8_t net_rx_buffer[16384];
static MINT_TLS term_tls;
static int64_t term_tls_send(void *ctx, const void *data, size_t len) {
    int64_t r = mint_tcp_send(*(int64_t *)ctx, data, len > 4096u ? 4096u : len);
    return r < 0 ? -1 : r;
}
static int64_t term_tls_recv(void *ctx, void *out, size_t cap) {
    int64_t r = mint_tcp_recv(*(int64_t *)ctx, out, cap > 4096u ? 4096u : cap);
    if (r == -MINT_EAGAIN) return 0;
    return r > 0 ? r : -1;
}

static uint64_t term_ms(void) {
    mint_timespec64 t = {0, 0};
    (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static int terminal_job_active(void) { return job.kind != JOB_NONE; }
static const char *terminal_job_hint(void) {
    return job.kind == JOB_NET_UP ? "Waiting for the network\xE2\x80\xA6 (Ctrl+C to stop)" :
           job.kind == JOB_PING ? "Pinging\xE2\x80\xA6 (Ctrl+C to stop)" :
           job.kind == JOB_LOOKUP ? "Looking up\xE2\x80\xA6 (Ctrl+C to stop)" : "Transferring\xE2\x80\xA6 (Ctrl+C to stop)";
}
static const char *net_error_text(int64_t rc) {
    switch (-rc) {
    case MINT_ENETDOWN: return "network is down (run: net up)";
    case MINT_ENOENT: return "host not found";
    case MINT_ETIMEDOUT: return "timed out";
    case MINT_ECONNREFUSED: return "connection refused";
    case MINT_ECONNRESET: return "connection reset by peer";
    case MINT_EHOSTUNREACH: return "host unreachable";
    case MINT_EINVAL: return "invalid name or address";
    case MINT_EBUSY: return "too many open connections";
    case MINT_ENODEV: return "no supported network adapter";
    case MINT_EIO: return "network adapter failed to start";
    default: return "network error";
    }
}
static void job_finish(void) {
    if (job.handle >= 0 && job.secure && (job.phase == PH_TLS || job.phase == PH_RECEIVE)) mint_tls_close(&term_tls);
    if (job.handle >= 0) (void)mint_tcp_close(job.handle);
    job.handle = -1;
    job.kind = JOB_NONE;
}
static void terminal_cancel_job(void) {
    if (job.kind == JOB_NONE) return;
    terminal_push("^C");
    job_finish();
}
static void push_wrapped(const char *text) {
    size_t n = text_length(text);
    if (!n) { terminal_push(""); return; }
    for (size_t at = 0; at < n;) {
        char part[TERM_COLS];
        size_t len = n - at < TERM_WRAP ? n - at : TERM_WRAP;
        while (len > 1u && at + len < n && ((uint8_t)text[at + len] & 0xC0u) == 0x80u) --len;   /* keep UTF-8 whole */
        for (size_t i = 0; i < len; ++i) part[i] = text[at + i];
        part[len] = '\0';
        terminal_push(part);
        at += len;
    }
}
static void ip_append(char *out, size_t cap, uint32_t ip) {
    char t[16]; (void)mint_ipv4_format(t, sizeof(t), ip); str_append(out, cap, t);
}
static void num_append(char *out, size_t cap, uint64_t v) { char t[24]; format_u64(t, sizeof(t), v); str_append(out, cap, t); }
static void ms_append(char *out, size_t cap, uint64_t us) {
    num_append(out, cap, us / 1000u);
    char frac[5] = {'.', (char)('0' + (us / 100u) % 10u), (char)('0' + (us / 10u) % 10u), (char)('0' + us % 10u), 0};
    str_append(out, cap, frac);
}
static uint32_t prefix_length(uint32_t mask) { uint32_t n = 0; while (mask & 0x80000000u) { ++n; mask <<= 1; } return n; }

static void print_ifconfig(void) {
    if (mint_net_get_status(&net_info) != 0) { terminal_push("Network status service unavailable"); return; }
    char row[TERM_COLS];
    static const char *const states[] = {"down", "no link", "requesting address (DHCP)", "connected", "failed"};
    copy_text(row, sizeof(row), "en0: "); str_append(row, sizeof(row), net_info.adapter);
    str_append(row, sizeof(row), " \xE2\x80\x94 ");
    str_append(row, sizeof(row), net_info.state <= MINT_NET_STATE_FAILED ? states[net_info.state] : "unknown");
    terminal_push(row);
    if (!net_info.driver_active) {
        terminal_push(net_info.nic_device_id ? "    adapter idle; run `net up` to connect" : "    no Intel E1000-family adapter was found");
        return;
    }
    static const char hx[] = "0123456789abcdef";
    copy_text(row, sizeof(row), "    ether ");
    for (int i = 0; i < 6; ++i) {
        char b[4] = {hx[net_info.mac[i] >> 4], hx[net_info.mac[i] & 15u], i < 5 ? ':' : 0, 0};
        str_append(row, sizeof(row), b);
    }
    str_append(row, sizeof(row), net_info.link_up ? "   link up" : "   no link");
    terminal_push(row);
    if (net_info.state == MINT_NET_STATE_UP) {
        copy_text(row, sizeof(row), "    inet "); ip_append(row, sizeof(row), net_info.ip);
        str_append(row, sizeof(row), " netmask "); ip_append(row, sizeof(row), net_info.netmask);
        str_append(row, sizeof(row), " broadcast "); ip_append(row, sizeof(row), net_info.ip | ~net_info.netmask);
        terminal_push(row);
        copy_text(row, sizeof(row), "    gateway "); ip_append(row, sizeof(row), net_info.gateway);
        str_append(row, sizeof(row), "   dns "); ip_append(row, sizeof(row), net_info.dns);
        str_append(row, sizeof(row), "   dhcp "); ip_append(row, sizeof(row), net_info.dhcp_server);
        terminal_push(row);
        copy_text(row, sizeof(row), "    lease "); num_append(row, sizeof(row), net_info.lease_seconds);
        str_append(row, sizeof(row), " s, obtained "); num_append(row, sizeof(row), net_info.lease_elapsed_seconds);
        str_append(row, sizeof(row), " s ago");
        terminal_push(row);
    } else {
        copy_text(row, sizeof(row), "    "); str_append(row, sizeof(row), net_info.status); terminal_push(row);
    }
    char size[24];
    copy_text(row, sizeof(row), "    RX "); num_append(row, sizeof(row), net_info.rx_frames);
    format_bytes(size, sizeof(size), net_info.rx_bytes);
    str_append(row, sizeof(row), " frames ("); str_append(row, sizeof(row), size);
    str_append(row, sizeof(row), ")   TX "); num_append(row, sizeof(row), net_info.tx_frames);
    format_bytes(size, sizeof(size), net_info.tx_bytes);
    str_append(row, sizeof(row), " frames ("); str_append(row, sizeof(row), size);
    str_append(row, sizeof(row), ")   dropped "); num_append(row, sizeof(row), net_info.rx_dropped);
    terminal_push(row);
    copy_text(row, sizeof(row), "    TCP "); num_append(row, sizeof(row), net_info.tcp_open_sockets);
    str_append(row, sizeof(row), " open, segments in/out "); num_append(row, sizeof(row), net_info.tcp_segments_in);
    str_append(row, sizeof(row), "/"); num_append(row, sizeof(row), net_info.tcp_segments_out);
    str_append(row, sizeof(row), ", retransmits "); num_append(row, sizeof(row), net_info.tcp_retransmits);
    str_append(row, sizeof(row), ", DNS queries "); num_append(row, sizeof(row), net_info.dns_queries);
    terminal_push(row);
}

static void job_flush_line(void) {
    job.line[job.line_len] = '\0';
    push_wrapped(job.line);
    job.line_len = 0; ++job.lines;
}
static void job_text(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = b[i];
        if (c == '\r') continue;
        if (c == '\n') { job_flush_line(); continue; }
        if (c == '\t') c = ' ';
        if (c < 32u || c == 127u) c = '?';                /* UTF-8 passes through for the Latin glyphs */
        if (job.line_len + 1u >= sizeof(job.line) && c < 0x80u) job_flush_line();
        if (job.line_len + 1u >= sizeof(job.line)) continue;
        job.line[job.line_len++] = (char)c;
    }
}

/* Splits http(s)://host[:port]/path. Returns 0 if malformed, 1 for http, 2 for https. */
static int parse_url(const char *url, char *host, size_t host_cap, uint16_t *port, char *path, size_t path_cap) {
    const char *p = url;
    int secure = 0;
    if (terminal_starts_with(p, "https://")) { p += 8; secure = 1; }
    else if (terminal_starts_with(p, "http://")) p += 7;
    size_t n = 0;
    while (*p && *p != '/' && *p != ':' && n + 1u < host_cap) host[n++] = *p++;
    host[n] = '\0';
    if (!n) return 0;
    uint32_t pv = secure ? 443u : 80u;
    if (*p == ':') {
        ++p; pv = 0;
        while (*p >= '0' && *p <= '9') { pv = pv * 10u + (uint32_t)(*p++ - '0'); if (pv > 65535u) return 0; }
        if (!pv) return 0;
    }
    *port = (uint16_t)pv;
    if (*p && *p != '/') return 0;
    copy_text(path, path_cap, *p ? p : "/");
    return secure ? 2 : 1;
}

static void job_start(uint32_t kind, const char *host) {
    job.kind = kind; job.phase = PH_RESOLVE;
    copy_text(job.host, sizeof(job.host), host);
    job.started = term_ms(); job.sent = job.received = 0; job.waiting = 0;
    job.rtt_sum = 0; job.rtt_min = ~0ull; job.rtt_max = 0;
    job.handle = -1; job.bytes = 0; job.lines = 0; job.line_len = 0;
}

static int terminal_network_command(const char *cmd) {
    char row[TERM_COLS];
    int is_net = terminal_equals(cmd, "net up") || terminal_equals(cmd, "net connect") || terminal_equals(cmd, "dhcp") ||
                 terminal_equals(cmd, "net down") || terminal_equals(cmd, "net disconnect") ||
                 terminal_equals(cmd, "ifconfig") || terminal_equals(cmd, "ipconfig") || terminal_equals(cmd, "ip") ||
                 terminal_starts_with(cmd, "ping ") || terminal_starts_with(cmd, "nslookup ") || terminal_starts_with(cmd, "host ") ||
                 terminal_starts_with(cmd, "curl ") || terminal_starts_with(cmd, "http ");
    if (!is_net) return 0;
    if (job.kind != JOB_NONE) { terminal_push("A network command is still running; press Ctrl+C to stop it"); return 1; }
    if (terminal_equals(cmd, "ifconfig") || terminal_equals(cmd, "ipconfig") || terminal_equals(cmd, "ip")) { print_ifconfig(); return 1; }
    if (terminal_equals(cmd, "net down") || terminal_equals(cmd, "net disconnect")) {
        (void)mint_net_set_up(MINT_NET_MODE_DOWN);
        terminal_push("Network disconnected; open connections were reset");
        return 1;
    }
    if (terminal_equals(cmd, "net up") || terminal_equals(cmd, "net connect") || terminal_equals(cmd, "dhcp")) {
        int64_t rc = mint_net_set_up(MINT_NET_MODE_DHCP);
        if (rc != 0) { copy_text(row, sizeof(row), "net: "); str_append(row, sizeof(row), net_error_text(rc)); terminal_push(row); return 1; }
        (void)mint_net_get_status(&net_info);
        copy_text(row, sizeof(row), "Starting "); str_append(row, sizeof(row), net_info.adapter);
        str_append(row, sizeof(row), "; requesting an address with DHCP\xE2\x80\xA6");
        terminal_push(row);
        job_start(JOB_NET_UP, "");
        return 1;
    }
    const char *arg = cmd;
    while (*arg && *arg != ' ') ++arg;
    while (*arg == ' ') ++arg;
    if (terminal_starts_with(cmd, "ping ")) {
        char host[80]; size_t n = 0;
        while (arg[n] && arg[n] != ' ' && n + 1u < sizeof(host)) { host[n] = arg[n]; ++n; }
        host[n] = '\0';
        uint32_t count = 4;
        const char *c = arg + n;
        while (*c == ' ') ++c;
        if (*c == '-' && (c[1] == 'c' || c[1] == 'n')) { c += 2; while (*c == ' ') ++c; }
        if (*c >= '0' && *c <= '9') { count = 0; while (*c >= '0' && *c <= '9') count = count * 10u + (uint32_t)(*c++ - '0'); }
        if (!host[0] || !count || count > 100u) { terminal_push("usage: ping <host> [count 1-100]"); return 1; }
        job_start(JOB_PING, host); job.count = count;
        return 1;
    }
    if (terminal_starts_with(cmd, "nslookup ") || terminal_starts_with(cmd, "host ")) {
        if (!*arg) { terminal_push("usage: nslookup <host>"); return 1; }
        job_start(JOB_LOOKUP, arg);
        return 1;
    }
    /* curl / http */
    uint8_t head_only = 0, silent = 0;
    while (arg[0] == '-' && arg[1] && arg[2] == ' ') {
        if (arg[1] == 'I' || arg[1] == 'i') head_only = 1;
        else if (arg[1] == 's' || arg[1] == 'o') silent = 1;
        arg += 3; while (*arg == ' ') ++arg;
    }
    char host[80], path[160]; uint16_t port = 80;
    int ok = parse_url(arg, host, sizeof(host), &port, path, sizeof(path));
    if (!ok) { terminal_push("usage: curl [-I] [-s] http(s)://host[:port]/path   (-s: count bytes only)"); return 1; }
    job_start(JOB_HTTP, host);
    job.port = port; job.head_only = head_only; job.silent = silent; job.secure = ok == 2;
    copy_text(job.path, sizeof(job.path), path);
    return 1;
}

static int curl_send_request(uint64_t now) {
    char request[400];
    copy_text(request, sizeof(request), job.head_only ? "HEAD " : "GET ");
    str_append(request, sizeof(request), job.path);
    str_append(request, sizeof(request), " HTTP/1.0\r\nHost: ");
    str_append(request, sizeof(request), job.host);
    if (job.port != (job.secure ? 443u : 80u)) { str_append(request, sizeof(request), ":"); num_append(request, sizeof(request), job.port); }
    str_append(request, sizeof(request), "\r\nUser-Agent: MintOS26-curl/1.1\r\nAccept: */*\r\nConnection: close\r\n\r\n");
    int64_t want = (int64_t)text_length(request);
    int64_t rc = job.secure ? mint_tls_write(&term_tls, request, (size_t)want) : mint_tcp_send(job.handle, request, (size_t)want);
    if (rc < want) { terminal_push("curl: (55) failed to send the request"); job_finish(); return 1; }
    terminal_push("* Request sent");
    terminal_push("");
    job.phase = PH_RECEIVE; job.last_data = now;
    return 1;
}

/* Advances the running job; returns 1 when the Terminal needs a repaint. */
static int terminal_job_step(void) {
    uint32_t serial = terminal_serial;
    uint64_t now = term_ms();
    char row[TERM_COLS];
    if (job.kind == JOB_NET_UP) {
        if (mint_net_get_status(&net_info) != 0) { terminal_push("Network status service unavailable"); job_finish(); }
        else if (net_info.state == MINT_NET_STATE_UP) {
            copy_text(row, sizeof(row), "Connected: "); ip_append(row, sizeof(row), net_info.ip);
            str_append(row, sizeof(row), "/"); num_append(row, sizeof(row), prefix_length(net_info.netmask));
            str_append(row, sizeof(row), ", gateway "); ip_append(row, sizeof(row), net_info.gateway);
            str_append(row, sizeof(row), ", DNS "); ip_append(row, sizeof(row), net_info.dns);
            terminal_push(row); job_finish();
        } else if (net_info.state == MINT_NET_STATE_FAILED || net_info.state == MINT_NET_STATE_DOWN) {
            copy_text(row, sizeof(row), "net: "); str_append(row, sizeof(row), net_info.status); terminal_push(row); job_finish();
        } else if (now - job.started > 25000u) {
            copy_text(row, sizeof(row), "Still waiting ("); str_append(row, sizeof(row), net_info.status);
            str_append(row, sizeof(row), "); check `ifconfig` later"); terminal_push(row); job_finish();
        }
        return serial != terminal_serial;
    }
    if (job.phase == PH_RESOLVE) {
        int64_t rc = mint_net_resolve_host(job.host, &job.ip);
        if (rc == -MINT_EAGAIN) {
            if (now - job.started > 10000u) rc = -MINT_ETIMEDOUT; else return 0;
        }
        const char *tool = job.kind == JOB_PING ? "ping" : job.kind == JOB_LOOKUP ? "nslookup" : "curl";
        if (rc < 0) {
            copy_text(row, sizeof(row), tool); str_append(row, sizeof(row), ": ");
            str_append(row, sizeof(row), job.host); str_append(row, sizeof(row), ": ");
            str_append(row, sizeof(row), job.kind == JOB_HTTP && rc == -MINT_ENOENT ? "could not resolve host" : net_error_text(rc));
            terminal_push(row); job_finish();
            return 1;
        }
        if (job.kind == JOB_LOOKUP) {
            (void)mint_net_get_status(&net_info);
            copy_text(row, sizeof(row), "Server:   "); ip_append(row, sizeof(row), net_info.dns); terminal_push(row);
            copy_text(row, sizeof(row), "Name:     "); str_append(row, sizeof(row), job.host); terminal_push(row);
            copy_text(row, sizeof(row), "Address:  "); ip_append(row, sizeof(row), job.ip); terminal_push(row);
            job_finish();
            return 1;
        }
        if (job.kind == JOB_PING) {
            copy_text(row, sizeof(row), "PING "); str_append(row, sizeof(row), job.host);
            str_append(row, sizeof(row), " ("); ip_append(row, sizeof(row), job.ip); str_append(row, sizeof(row), "): 56 data bytes");
            terminal_push(row);
            job.phase = PH_RUN; job.next_at = now;
            return 1;
        }
        int64_t h = mint_tcp_connect(job.ip, job.port);
        if (h < 0) {
            copy_text(row, sizeof(row), "curl: (7) "); str_append(row, sizeof(row), net_error_text(h)); terminal_push(row);
            job_finish(); return 1;
        }
        job.handle = h; job.phase = PH_CONNECT; job.sent_at = now;
        copy_text(row, sizeof(row), "* Connecting to "); str_append(row, sizeof(row), job.host);
        str_append(row, sizeof(row), " ("); ip_append(row, sizeof(row), job.ip); str_append(row, sizeof(row), ") port ");
        num_append(row, sizeof(row), job.port);
        terminal_push(row);
        return 1;
    }
    if (job.kind == JOB_PING) {
        if (job.waiting) {
            int64_t r = mint_net_ping_poll(job.sent - 1u);
            if (r >= 0) {
                uint64_t us = (uint64_t)r & 0xFFFFFFFFu, ttl = (uint64_t)r >> 32;
                copy_text(row, sizeof(row), "64 bytes from "); ip_append(row, sizeof(row), job.ip);
                str_append(row, sizeof(row), ": icmp_seq="); num_append(row, sizeof(row), job.sent - 1u);
                str_append(row, sizeof(row), " ttl="); num_append(row, sizeof(row), ttl);
                str_append(row, sizeof(row), " time="); ms_append(row, sizeof(row), us); str_append(row, sizeof(row), " ms");
                terminal_push(row);
                ++job.received; job.rtt_sum += us;
                if (us < job.rtt_min) job.rtt_min = us;
                if (us > job.rtt_max) job.rtt_max = us;
                job.waiting = 0;
            } else if (now - job.sent_at >= 2000u) {
                copy_text(row, sizeof(row), "Request timeout for icmp_seq "); num_append(row, sizeof(row), job.sent - 1u);
                terminal_push(row); job.waiting = 0;
            }
        }
        if (!job.waiting) {
            if (job.sent >= job.count) {
                copy_text(row, sizeof(row), "--- "); str_append(row, sizeof(row), job.host); str_append(row, sizeof(row), " ping statistics ---");
                terminal_push(row);
                copy_text(row, sizeof(row), ""); num_append(row, sizeof(row), job.sent);
                str_append(row, sizeof(row), " packets transmitted, "); num_append(row, sizeof(row), job.received);
                str_append(row, sizeof(row), " packets received, ");
                num_append(row, sizeof(row), (uint64_t)(job.sent - job.received) * 100u / job.sent);
                str_append(row, sizeof(row), "% packet loss");
                terminal_push(row);
                if (job.received) {
                    copy_text(row, sizeof(row), "round-trip min/avg/max = "); ms_append(row, sizeof(row), job.rtt_min);
                    str_append(row, sizeof(row), "/"); ms_append(row, sizeof(row), job.rtt_sum / job.received);
                    str_append(row, sizeof(row), "/"); ms_append(row, sizeof(row), job.rtt_max); str_append(row, sizeof(row), " ms");
                    terminal_push(row);
                }
                job_finish();
            } else if (now >= job.next_at) {
                int64_t rc = mint_net_ping_send(job.ip, job.sent);
                if (rc < 0) {
                    copy_text(row, sizeof(row), "ping: sendto: "); str_append(row, sizeof(row), net_error_text(rc));
                    terminal_push(row); job_finish();
                } else { ++job.sent; job.waiting = 1; job.sent_at = now; job.next_at = now + 1000u; }
            }
        }
        return serial != terminal_serial;
    }
    /* HTTP */
    if (job.phase == PH_CONNECT) {
        int64_t st = mint_tcp_state(job.handle);
        if (st == MINT_TCP_STATE_ESTABLISHED) {
            if (job.secure) {
                mint_timespec64 wall = {0, 0};
                MINT_TLS_IO io = {&job.handle, term_tls_send, term_tls_recv};
                mint_tls_init(&term_tls, &io, job.host, mint_clock_gettime(MINT_CLOCK_REALTIME, &wall) == 0 ? wall.tv_sec : 0);
                job.phase = PH_TLS; job.sent_at = now;
                terminal_push("* TLS 1.3 handshake\xE2\x80\xA6");
                return 1;
            }
            return curl_send_request(now);
        }
        if (st != MINT_TCP_STATE_CONNECTING || now - job.sent_at > 15000u) {
            copy_text(row, sizeof(row), "curl: (7) Failed to connect to "); str_append(row, sizeof(row), job.host);
            str_append(row, sizeof(row), " port "); num_append(row, sizeof(row), job.port); str_append(row, sizeof(row), ": ");
            str_append(row, sizeof(row), st == MINT_TCP_STATE_REFUSED ? "connection refused" :
                                         st == MINT_TCP_STATE_UNREACHABLE ? "host unreachable" : "timed out");
            terminal_push(row); job_finish();
            return 1;
        }
        return 0;
    }
    if (job.phase == PH_TLS) {
        int rc = mint_tls_step(&term_tls);
        if (rc == MINT_TLS_AGAIN) {
            if (now - job.sent_at > 20000u) { terminal_push("curl: (35) TLS handshake timed out"); job_finish(); return 1; }
            return 0;
        }
        if (rc != MINT_TLS_DONE) {
            copy_text(row, sizeof(row), term_tls.error == MINT_TLS_ERR_CERT ? "curl: (60) SSL certificate problem: " : "curl: (35) TLS connect error: ");
            str_append(row, sizeof(row), mint_tls_error_text(&term_tls));
            push_wrapped(row); job_finish();
            return 1;
        }
        copy_text(row, sizeof(row), "* TLS 1.3 with ");
        str_append(row, sizeof(row), mint_tls_suite_name(term_tls.suite));
        str_append(row, sizeof(row), "; certificate verified");
        push_wrapped(row);
        return curl_send_request(now);
    }
    for (uint32_t burst = 0; burst < 16u && job.kind == JOB_HTTP; ++burst) {
        int64_t n;
        if (job.secure) {
            n = mint_tls_read(&term_tls, net_rx_buffer, sizeof(net_rx_buffer));
            if (n == MINT_TLS_WOULD_BLOCK) n = -MINT_EAGAIN;
            else if (n < 0) {
                if (job.line_len) job_flush_line();
                copy_text(row, sizeof(row), "curl: (56) TLS: "); str_append(row, sizeof(row), mint_tls_error_text(&term_tls));
                push_wrapped(row); job_finish(); break;
            }
        } else n = mint_tcp_recv(job.handle, net_rx_buffer, 4096u);
        if (n > 0) {
            if (!job.silent) job_text(net_rx_buffer, (size_t)n);
            job.bytes += (uint64_t)n; job.last_data = now;
            if (job.lines > 1000u) { terminal_push("[output truncated after 1000 lines]"); job_finish(); }
            continue;
        }
        if (n == -MINT_EAGAIN) {
            if (now - job.last_data > 20000u) { terminal_push("curl: (28) operation timed out"); job_finish(); }
            break;
        }
        if (job.line_len) job_flush_line();
        if (n == 0) {
            uint64_t ms = now - job.started;
            if (!ms) ms = 1;
            copy_text(row, sizeof(row), "[");
            num_append(row, sizeof(row), job.bytes); str_append(row, sizeof(row), " bytes in ");
            num_append(row, sizeof(row), ms); str_append(row, sizeof(row), " ms, ");
            num_append(row, sizeof(row), job.bytes * 1000u / ms / 1024u); str_append(row, sizeof(row), " KB/s; connection closed]");
            terminal_push(row);
        } else {
            copy_text(row, sizeof(row), "curl: (56) "); str_append(row, sizeof(row), net_error_text(n)); terminal_push(row);
        }
        job_finish();
    }
    return serial != terminal_serial;
}

/* ------------------------------------------------------------ Terminal */
#define TERM_CELL 8
static void terminal_paint(const mde_layout *l) {
    gfx_rect c = l->content;
    uint32_t bg = T.dark ? 0x1C1C1Eu : 0xFFFFFFu, fg = T.dark ? 0xE6E6EAu : 0x1D1D1Fu;
    uint32_t prompt = T.dark ? 0x64D2A0u : 0x13865Au;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(bg));
    int32_t line_h = 17, x = c.x0 + 10;
    int32_t max_lines = (H(c) - 16) / line_h - 1;
    if (max_lines < 1) max_lines = 1;
    int32_t max_scroll = (int32_t)terminal_output_count - max_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (terminal_scroll > max_scroll) terminal_scroll = max_scroll;
    if (terminal_scroll < 0) terminal_scroll = 0;
    uint32_t end = terminal_output_count - (uint32_t)terminal_scroll;
    uint32_t first = end > (uint32_t)max_lines ? end - (uint32_t)max_lines : 0u;
    int32_t y = c.y0 + 8 + gfx_font_ascent(FONT_SMALL);
    for (uint32_t i = first; i < end; ++i) {
        if (TERM_KIND(i)) {
            int32_t px = gfx_text_cells(S, FONT_SMALL, x, y, terminal_prompt(), TERM_CELL, OPAQUE(prompt));
            gfx_text_cells(S, FONT_SMALL, x + px, y, TERM_AT(i), TERM_CELL, OPAQUE(fg));
        } else gfx_text_cells(S, FONT_SMALL, x, y, TERM_AT(i), TERM_CELL, OPAQUE(fg));
        y += line_h;
    }
    if (terminal_scroll) {
        char note[48] = "\xE2\x86\x91 ";
        char num[16]; format_u64(num, sizeof(num), (uint64_t)terminal_scroll);
        str_append(note, sizeof(note), num); str_append(note, sizeof(note), " more lines below");
        ui_label_right(c.x1 - 12, c.y1 - 10, FONT_CAPTION, note, T.text2);
        return;
    }
    if (terminal_job_active()) {
        ui_label(x, y, FONT_SMALL, terminal_job_hint(), T.text3);
        return;
    }
    int32_t px = gfx_text_cells(S, FONT_SMALL, x, y, terminal_prompt(), TERM_CELL, OPAQUE(prompt));
    int32_t ix = gfx_text_cells(S, FONT_SMALL, x + px, y, terminal_input, TERM_CELL, OPAQUE(fg));
    if (!l->focused) gfx_round_rect_stroke(S, x + px + ix, y - 12, TERM_CELL, 16, 0, OPAQUE(T.text3));
    else if (((ui_ticks / 50u) & 1u) == 0u) gfx_fill(S, x + px + ix, y - 12, TERM_CELL, 16, WITH_ALPHA(fg, 0xB0));
}
static int terminal_key(const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    if (ch == '\n') {
        terminal_input[terminal_input_length] = '\0';
        if (terminal_input_length) copy_text(terminal_last_command, sizeof(terminal_last_command), terminal_input);
        terminal_run_command();
        terminal_input_length = 0; terminal_input[0] = '\0';
        return 1;
    }
    if (ch == 8u) { if (terminal_input_length) terminal_input[--terminal_input_length] = '\0'; return 1; }
    if (k->scancode == 0x148u) { /* Up arrow: recall the last command */
        terminal_input_length = copy_text(terminal_input, sizeof(terminal_input), terminal_last_command);
        return 1;
    }
    if (ch == 12u || ((k->modifiers & MINT_KEYMOD_CTRL) && (ch == 'l' || ch == 'L'))) { terminal_clear(); return 1; }
    if (ch == 3u || ((k->modifiers & MINT_KEYMOD_CTRL) && (ch == 'c' || ch == 'C'))) { terminal_cancel_job(); return 1; }
    if (ch >= 32u && ch < 127u && terminal_input_length + 1u < sizeof(terminal_input)) {
        terminal_input[terminal_input_length++] = (char)ch; terminal_input[terminal_input_length] = '\0';
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ Settings */
enum { SET_GENERAL, SET_APPEARANCE, SET_WALLPAPER, SET_DISPLAYS, SET_INPUT, SET_STORAGE,
       SET_NETWORK, SET_PRIVACY, SET_POWER, SET_COUNT };
static const char *const settings_names[SET_COUNT] = {
    "General", "Appearance", "Wallpaper", "Displays", "Input", "Storage", "Network", "Privacy", "Power"
};
static const uint32_t settings_colors[SET_COUNT] = {
    0x8E8E93u, 0x3A3A3Cu, 0x32ADE6u, 0x007AFFu, 0x8E8E93u, 0x636366u, 0x0A84FFu, 0x5856D6u, 0x34C759u
};
static const uint32_t settings_symbols[SET_COUNT] = {
    SYM_GEAR, SYM_TOGGLES, SYM_DOC, SYM_INFO, SYM_TOGGLES, SYM_DISK, SYM_INFO, SYM_CHECK, SYM_WARNING
};
static gfx_rect settings_item_rect(const mde_layout *l, uint32_t i) {
    return R(l->sidebar.x0 + 10, l->sidebar.y0 + 50 + (int32_t)i * 30, W(l->sidebar) - 20, 28);
}
static gfx_rect settings_body(const mde_layout *l) {
    int32_t w = W(l->content) - 56; if (w > 640) w = 640;
    return R(l->content.x0 + 28, l->content.y0 + 22, w, H(l->content) - 30);
}
static void wallpaper_thumb(gfx_rect r, int dark, int selected, int auto_mode) {
    if (selected) gfx_round_rect(S, r.x0 - 4, r.y0 - 4, W(r) + 8, H(r) + 8, 10, OPAQUE(T.accent));
    uint32_t top = dark ? 0x141A40u : 0x86BEF0u, bottom = dark ? 0x4A2A68u : 0xF6D8C8u;
    if (auto_mode) {
        gfx_rect left = R(r.x0, r.y0, W(r) / 2, H(r));
        gfx_round_rect_grad(S, r.x0, r.y0, W(r), H(r), 7, OPAQUE(0x86BEF0u), OPAQUE(0xF6D8C8u));
        gfx_rect saved = gfx_clip_get();
        gfx_clip_set(gfx_rect_intersect(saved, R(r.x0 + W(r) / 2, r.y0, W(r) / 2, H(r))));
        gfx_round_rect_grad(S, r.x0, r.y0, W(r), H(r), 7, OPAQUE(0x141A40u), OPAQUE(0x4A2A68u));
        gfx_clip_set(saved);
        (void)left;
    } else gfx_round_rect_grad(S, r.x0, r.y0, W(r), H(r), 7, OPAQUE(top), OPAQUE(bottom));
    gfx_rect saved = gfx_clip_get();
    gfx_clip_set(gfx_rect_intersect(saved, r));
    int32_t h = H(r);
    for (int i = 0; i < 3; ++i) {
        uint32_t c = dark ? (i == 0 ? 0x1E6E86u : i == 1 ? 0x2E44A6u : 0x5A3AA6u) : (i == 0 ? 0x58C8C0u : i == 1 ? 0x4C98E6u : 0x8C70E6u);
        gfx_round_rect(S, r.x0 - 20 + i * 14, r.y0 + h * (50 + i * 14) / 100, W(r) + 40, h, h / 2, WITH_ALPHA(c, 0xD0));
    }
    gfx_clip_set(saved);
    gfx_round_rect_stroke(S, r.x0, r.y0, W(r), H(r), 7, T.separator);
}

static void settings_rows(gfx_rect *at, const char *const *labels, const char *const *values, uint32_t n) {
    gfx_rect g = R(at->x0, at->y0, W(*at), (int32_t)n * 36);
    ui_group(g);
    for (uint32_t i = 0; i < n; ++i) ui_group_row(R(g.x0, g.y0 + (int32_t)i * 36, W(g), 36), i, n, labels[i], values[i]);
    at->y0 = g.y1 + 18;
}
static void settings_note(gfx_rect *at, const char *text) {
    ui_label(at->x0 + 4, at->y0 + 2, FONT_CAPTION, text, T.text2);
    at->y0 += 22;
}

static void settings_paint(const mde_layout *l) {
    gfx_rect sb = l->sidebar;
    ui_search_field(R(sb.x0 + 10, sb.y0 + 10, W(sb) - 20, 28), "", 0);
    for (uint32_t i = 0; i < SET_COUNT; ++i) {
        gfx_rect ir = settings_item_rect(l, i);
        ui_sidebar_item(ir, settings_names[i], 0, settings_colors[i], settings_category == i, l->focused);
        ui_symbol_glyph(ir.x0 + 18, ir.y0 + 14, settings_symbols[i], OPAQUE(0xFFFFFFu));
    }
    toolbar_title(l, settings_names[settings_category < SET_COUNT ? settings_category : 0], 1);
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.group_bg));
    gfx_rect b = settings_body(l);
    char a[48], bb[48], cc[48], d[48], e[48];
    switch (settings_category) {
    case SET_GENERAL: {
        gfx_rect hero = R(b.x0, b.y0, W(b), 92);
        ui_group(hero);
        gfx_image(S, hero.x0 + 14, hero.y0 + 14, 64, 64, mint_icon_about_mintos_128, 128, 128, 255u);
        ui_label(hero.x0 + 92, hero.y0 + 40, FONT_TITLE, "MintOS26", T.text);
        ui_label(hero.x0 + 92, hero.y0 + 62, FONT_BODY, "Version " MINTOS_VERSION "  \xC2\xB7  MintDE " MINTDE_VERSION, T.text2);
        b.y0 = hero.y1 + 18;
        mint_timespec64 up = {0, 0}; (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &up);
        format_u64(a, sizeof(a), (uint64_t)up.tv_sec / 60u); str_append(a, sizeof(a), " min");
        const char *labels[5] = {"Kernel", "Boot", "Desktop", "Session storage", "Uptime"};
        const char *values[5] = {"x86-64, freestanding", "UEFI \xE2\x86\x92 MintOS native kernel", "MintDE " MINTDE_VERSION " (ring 3)",
                                 "RAM only \xE2\x80\x94 cleared at restart", a};
        settings_rows(&b, labels, values, 5);
        settings_note(&b, "Software updates are installed by writing a new MintOS image.");
        break;
    }
    case SET_APPEARANCE: {
        gfx_rect g = R(b.x0, b.y0, W(b), 148);
        ui_group(g);
        ui_label(g.x0 + 14, g.y0 + 24, FONT_BODY, "Appearance", T.text);
        static const char *const names[3] = {"Light", "Dark", "Auto"};
        static const uint32_t themes[3] = {MINT_THEME_LIGHT, MINT_THEME_DARK, MINT_THEME_WARM};
        for (int i = 0; i < 3; ++i) {
            gfx_rect t = R(g.x1 - 3 * 100 - 6 + i * 100, g.y0 + 18, 84, 56);
            wallpaper_thumb(t, i == 1, desktop_settings.theme == themes[i], i == 2);
            gfx_round_rect(S, t.x0 + 8, t.y0 + 8, 40, 26, 4, OPAQUE(i == 1 ? 0x2C2C2Eu : 0xFFFFFFu));
            ui_label_center(t.x0 + W(t) / 2, t.y1 + 20, FONT_CAPTION, names[i], T.text);
        }
        gfx_hline(S, g.x0 + 12, g.y0 + 112, W(g) - 24, T.separator);
        ui_label(g.x0 + 14, g.y0 + 134, FONT_BODY, "Accent color", T.text);
        static const uint32_t swatch[5] = {0x00B894u, 0x007AFFu, 0xAF52DEu, 0xFF9500u, 0xFF2D55u};
        for (int i = 0; i < 5; ++i) {
            int32_t cx = g.x1 - 5 * 28 - 4 + i * 28, cy = g.y0 + 129;
            gfx_circle(S, cx, cy, 9, OPAQUE(swatch[i]));
            if ((uint32_t)i == desktop_settings.accent) gfx_circle(S, cx, cy, 3, OPAQUE(0xFFFFFFu));
        }
        b.y0 = g.y1 + 18;
        gfx_rect g2 = R(b.x0, b.y0, W(b), 40);
        ui_group(g2);
        ui_label(g2.x0 + 14, BL(g2, FONT_BODY), FONT_BODY, "Reduce motion", T.text);
        ui_switch(g2.x1 - 52, g2.y0 + 9, (desktop_settings.flags & MINT_DESKTOP_SETTING_REDUCE_MOTION) != 0);
        b.y0 = g2.y1 + 10;
        settings_note(&b, "Auto switches between Light and Dark using the computer's clock (19:00 \xE2\x80\x93 07:00).");
        break;
    }
    case SET_WALLPAPER: {
        static const char *const names[3] = {"Mint Waves (Automatic)", "Mint Waves Light", "Mint Waves Dark"};
        for (int i = 0; i < 3; ++i) {
            gfx_rect t = R(b.x0 + 8 + i * 200, b.y0 + 8, 180, 110);
            wallpaper_thumb(t, i == 2, desktop_settings.wallpaper == (uint32_t)i, i == 0);
            ui_label_center(t.x0 + 90, t.y1 + 22, FONT_CAPTION, names[i], T.text);
        }
        b.y0 += 160;
        settings_note(&b, "Original MintOS artwork. Automatic follows the Light or Dark appearance.");
        break;
    }
    case SET_DISPLAYS: {
        MINT_DISPLAY_INFO di = {0}; (void)mint_display_get_info(&di);
        format_u64(a, sizeof(a), di.width); str_append(a, sizeof(a), " \xC3\x97 ");
        format_u64(bb, sizeof(bb), di.height); str_append(a, sizeof(a), bb);
        format_u64(bb, sizeof(bb), di.stride_pixels); str_append(bb, sizeof(bb), " pixels");
        format_bytes(cc, sizeof(cc), di.byte_size);
        const char *labels[5] = {"Resolution", "Row stride", "Pixel format", "Framebuffer", "Video memory window"};
        const char *values[5] = {a, bb, di.pixel_format ? "BGRX 8-bit" : "RGBX 8-bit", "UEFI GOP (firmware)", cc};
        settings_rows(&b, labels, values, 5);
        settings_note(&b, "Choose a resolution in UEFI Desktop \xE2\x80\xBA Settings before starting MintDE.");
        settings_note(&b, "There is no GPU driver; the firmware framebuffer is write-combined for speed.");
        break;
    }
    case SET_INPUT: {
        (void)mint_input_get_info(&input_info);
        static MINT_USB_INFO usb_info;
        if (mint_usb_get_status(&usb_info) != 0) bytes_clear(&usb_info, sizeof(usb_info));
        const char *kbd = input_info.keyboard_ready ? "PS/2, ready" : "Not detected";
        const char *ptr = input_info.mouse_ready ? "PS/2, ready" : "Not detected";
        for (uint32_t i = 0; i < usb_info.device_count && i < MINT_USB_MAX_DEVICES; ++i) {
            if (usb_info.devices[i].kind == MINT_USB_KIND_KEYBOARD) kbd = "USB, ready";
            if (usb_info.devices[i].kind == MINT_USB_KIND_MOUSE) ptr = "USB mouse, ready";
            if (usb_info.devices[i].kind == MINT_USB_KIND_TABLET) ptr = "USB tablet, ready";
        }
        format_u64(a, sizeof(a), input_info.keyboard_events);
        format_u64(bb, sizeof(bb), input_info.pointer_motion_events);
        format_u64(cc, sizeof(cc), input_info.pointer_button_events);
        format_u64(d, sizeof(d), input_info.dropped_events);
        const char *labels[6] = {"Keyboard", "Pointer", "Key events", "Pointer moves", "Button events", "Dropped events"};
        const char *values[6] = {kbd, ptr, a, bb, cc, d};
        settings_rows(&b, labels, values, 6);
        settings_note(&b, "Command shortcuts use the Windows key. Pointer acceleration is built in.");
        settings_note(&b, "USB keyboards, mice and tablets work through the xHCI driver (no USB hubs yet).");
        break;
    }
    case SET_STORAGE: {
        refresh_storage_data();
        mfs_refresh_volume();
        if (mfs_ready) {
            gfx_rect g = R(b.x0, b.y0, W(b), 92);
            ui_group(g);
            ui_symbol_glyph(g.x0 + 26, g.y0 + 26, SYM_DRIVE, OPAQUE(T.accent));
            copy_text(a, sizeof(a), mfs_volume.label); str_append(a, sizeof(a), "  \xC2\xB7  "); str_append(a, sizeof(a), mfs_volume.device);
            gfx_text_fit(S, FONT_BODY_MEDIUM, g.x0 + 46, g.y0 + 31, a, W(g) - 200, OPAQUE(T.text));
            uint64_t used = mfs_volume.total_bytes - mfs_volume.free_bytes;
            format_bytes(bb, sizeof(bb), mfs_volume.free_bytes); str_append(bb, sizeof(bb), " available");
            ui_label_right(g.x1 - 14, g.y0 + 31, FONT_BODY, bb, T.text2);
            uint32_t permille = mfs_volume.total_bytes ? (uint32_t)(used * 1000u / mfs_volume.total_bytes) : 0u;
            ui_progress(R(g.x0 + 14, g.y0 + 46, W(g) - 28, 10), permille < 8u ? 8u : permille, T.accent);
            ui_label(g.x0 + 14, g.y0 + 78, FONT_CAPTION, "MintFS \xE2\x80\x94 mounted read/write, copy-on-write", T.text2);
            format_u64(cc, sizeof(cc), mfs_volume.items); str_append(cc, sizeof(cc), " items");
            ui_label_right(g.x1 - 14, g.y0 + 78, FONT_CAPTION, cc, T.text2);
            b.y0 = g.y1 + 14;
        }
        for (uint32_t i = 0; i < storage_snapshot.device_count && i < 3u; ++i) {
            MINT_STORAGE_DEVICE_INFO *dv = &storage_snapshot.devices[i];
            gfx_rect g = R(b.x0, b.y0, W(b), 92);
            ui_group(g);
            ui_symbol_glyph(g.x0 + 26, g.y0 + 26, SYM_DISK, OPAQUE(T.text2));
            copy_text(a, sizeof(a), dv->name); str_append(a, sizeof(a), "  \xC2\xB7  ");
            str_append(a, sizeof(a), dv->model[0] ? dv->model : "Unknown model");
            gfx_text_fit(S, FONT_BODY_MEDIUM, g.x0 + 46, g.y0 + 31, a, W(g) - 200, OPAQUE(T.text));
            format_bytes(bb, sizeof(bb), dv->capacity_bytes);
            ui_label_right(g.x1 - 14, g.y0 + 31, FONT_BODY, bb, T.text2);
            int mounted = 0;
            (void)mint_fat16_get_volumes(&fat_volume_list);
            for (uint32_t v = 0; v < fat_volume_list.count; ++v) if (fat_volume_list.block_indices[v] == dv->block_index && dv->block_registered) mounted = 1;
            int mintos = disk_is_mintos(dv);
            ui_progress(R(g.x0 + 14, g.y0 + 46, W(g) - 28, 10), mounted || mintos ? 1000u : 0u, mounted || mintos ? T.accent : T.text3);
            ui_label(g.x0 + 14, g.y0 + 78, FONT_CAPTION, mintos ? "GUID partition map \xE2\x80\x94 MintOS disk" :
                     mounted ? "FAT16 \xE2\x80\x94 mounted read-only" : "Not mounted (blank or unsupported)", T.text2);
            ui_label_right(g.x1 - 14, g.y0 + 78, FONT_CAPTION, mintos ? "Writes: MintFS partition only" : "Writes disabled", T.text2);
            b.y0 = g.y1 + 14;
        }
        if (!storage_snapshot.device_count) settings_note(&b, "No SATA disk was detected by the AHCI driver.");
        settings_note(&b, mfs_ready ? "Files and Notes are saved on MintOS HD. Settings still last until restart." :
                                      "Session (RAM) holds Notes and settings until restart. Disk Utility can make a blank disk a MintOS disk.");
        break;
    }
    case SET_NETWORK: {
        if (mint_net_get_status(&net_info) != 0) bytes_clear(&net_info, sizeof(net_info));
        uint32_t dot; int connected;
        const char *state = network_state_text(&net_info, &dot, &connected);
        int up = net_info.state == MINT_NET_STATE_UP;
        a[0] = bb[0] = cc[0] = '\0';
        if (up) { ip_append(a, sizeof(a), net_info.ip); ip_append(bb, sizeof(bb), net_info.gateway); ip_append(cc, sizeof(cc), net_info.dns); }
        copy_text(d, sizeof(d), net_info.adapter[0] ? net_info.adapter : "None");
        const char *labels[5] = {"Ethernet", "IP Address", "Router", "DNS Server", "Adapter"};
        const char *values[5] = {state, up ? a : "\xE2\x80\x94", up ? bb : "\xE2\x80\x94", up ? cc : "\xE2\x80\x94", d};
        settings_rows(&b, labels, values, 5);
        gfx_rect g = R(b.x0, b.y0, W(b), 36);
        ui_group(g);
        ui_label(g.x0 + 14, g.y0 + 23, FONT_BODY, "Connect automatically when MintDE starts", T.text);
        ui_switch(g.x1 - 58, g.y0 + 7, (int)network_autoconnect);
        b.y0 = g.y1 + 18;
        int have_adapter = net_info.nic_device_id || net_info.driver_active;
        ui_button(R(b.x0, b.y0, 120, 28), connected ? "Disconnect" : "Connect", !connected && have_adapter, have_adapter);
        b.y0 += 46;
        settings_note(&b, "Automatic connection applies to virtual-machine adapters; experimental drivers start only on request.");
        settings_note(&b, "Wi-Fi is not supported.");
        break;
    }
    case SET_PRIVACY: {
        const char *labels[5] = {"Disk writes", "Network", "Memory protection", "Processes", "Firmware"};
        const char *values[5] = {"Blocked (read-only volumes)", "Outgoing only; nothing listens", "NX, W^X and guard pages",
                                 "Isolated ring-3 address spaces", "UEFI Desktop is the recovery path"};
        settings_rows(&b, labels, values, 5);
        break;
    }
    case SET_POWER: {
        MINT_POWER_INFO pi = {0};
        int ok = mint_power_get_status(&pi) == 0;
        format_u64(a, sizeof(a), pi.acpi_revision);
        copy_text(bb, sizeof(bb), ok && (pi.flags & MINT_POWER_HAVE_S5) ? "ACPI S5 soft-off" : "Not available");
        copy_text(cc, sizeof(cc), ok && (pi.flags & MINT_POWER_HAVE_RESET_REG) ? "ACPI reset register" : "PCH reset (0xCF9)");
        copy_text(d, sizeof(d), pi.oem_id[0] ? pi.oem_id : "Unknown");
        copy_text(e, sizeof(e), (pi.flags & MINT_POWER_HYPERVISOR) ? "Virtual machine" : "Physical computer");
        const char *labels[5] = {"Shut Down", "Restart", "Firmware vendor", "ACPI revision", "Platform"};
        const char *values[5] = {bb, cc, d, a, e};
        settings_rows(&b, labels, values, 5);
        ui_button(R(b.x0, b.y0, 110, 28), "Restart\xE2\x80\xA6", 0, 1);
        ui_button(R(b.x0 + 122, b.y0, 120, 28), "Shut Down\xE2\x80\xA6", 0, 1);
        break;
    }
    default: break;
    }
}

static int settings_click(const mde_layout *l, int32_t x, int32_t y) {
    for (uint32_t i = 0; i < SET_COUNT; ++i)
        if (in(settings_item_rect(l, i), x, y)) { settings_category = i; return 1; }
    gfx_rect b = settings_body(l);
    if (settings_category == SET_APPEARANCE) {
        static const uint32_t themes[3] = {MINT_THEME_LIGHT, MINT_THEME_DARK, MINT_THEME_WARM};
        gfx_rect g = R(b.x0, b.y0, W(b), 148);
        for (int i = 0; i < 3; ++i)
            if (in(R(g.x1 - 3 * 100 - 6 + i * 100, g.y0 + 14, 92, 84), x, y)) {
                desktop_settings.theme = themes[i]; (void)shell_apply_settings(); return 1;
            }
        for (int i = 0; i < 5; ++i)
            if (in(R(g.x1 - 5 * 28 - 4 + i * 28 - 12, g.y0 + 117, 24, 24), x, y)) {
                desktop_settings.accent = (uint32_t)i; (void)shell_apply_settings(); return 1;
            }
        if (in(R(b.x1 - 60, g.y1 + 18, 60, 40), x, y)) {
            desktop_settings.flags ^= MINT_DESKTOP_SETTING_REDUCE_MOTION; (void)shell_apply_settings(); return 1;
        }
    } else if (settings_category == SET_WALLPAPER) {
        for (int i = 0; i < 3; ++i)
            if (in(R(b.x0 + 8 + i * 200, b.y0 + 8, 180, 110), x, y)) {
                desktop_settings.wallpaper = (uint32_t)i; (void)shell_apply_settings(); return 1;
            }
    } else if (settings_category == SET_NETWORK) {
        int32_t sy = b.y0 + 5 * 36 + 18;
        if (in(R(b.x1 - 70, sy, 70, 36), x, y)) { network_autoconnect = !network_autoconnect; return 1; }
        if (in(R(b.x0, sy + 36 + 18, 120, 28), x, y)) return network_toggle();
    } else if (settings_category == SET_POWER) {
        int32_t by = b.y0 + 5 * 36 + 18;
        if (in(R(b.x0, by, 110, 28), x, y)) { shell_request_power(MINT_POWER_ACTION_RESTART); return 1; }
        if (in(R(b.x0 + 122, by, 120, 28), x, y)) { shell_request_power(MINT_POWER_ACTION_SHUTDOWN); return 1; }
    }
    return 0;
}

/* ------------------------------------------------------------ System Monitor */
static gfx_rect monitor_tabs_rect(const mde_layout *l) { return R(l->toolbar.x0 + W(l->toolbar) / 2 - 130, l->toolbar.y0 + 12, 260, 28); }
static void stat_card(gfx_rect r, const char *label, const char *value, const char *sub) {
    ui_group(r);
    ui_label(r.x0 + 14, r.y0 + 24, FONT_CAPTION_BOLD, label, T.text2);
    gfx_text_fit(S, FONT_TITLE, r.x0 + 14, r.y0 + 56, value, W(r) - 28, OPAQUE(T.text));
    if (sub) gfx_text_fit(S, FONT_CAPTION, r.x0 + 14, r.y0 + 78, sub, W(r) - 28, OPAQUE(T.text3));
}
static void monitor_paint(const mde_layout *l) {
    toolbar_title(l, "System Monitor", 0);
    static const char *const tabs[3] = {"Overview", "Disk", "Input"};
    ui_segmented(monitor_tabs_rect(l), tabs, 3, monitor_tab);
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.group_bg));
    char a[48], b2[48], cc[48];
    int32_t pad = 20, cw = (W(c) - pad * 4) / 3;
    if (monitor_tab == 0) {
        mint_timespec64 up = {0, 0}; (void)mint_clock_gettime(MINT_CLOCK_MONOTONIC, &up);
        uint64_t s = (uint64_t)up.tv_sec;
        format_u64(a, sizeof(a), s / 3600u); str_append(a, sizeof(a), "h ");
        format_u64(b2, sizeof(b2), (s / 60u) % 60u); str_append(a, sizeof(a), b2); str_append(a, sizeof(a), "m ");
        format_u64(b2, sizeof(b2), s % 60u); str_append(a, sizeof(a), b2); str_append(a, sizeof(a), "s");
        stat_card(R(c.x0 + pad, c.y0 + pad, cw, 92), "UPTIME", a, "PIT timer, 100 Hz");
        format_u64(b2, sizeof(b2), (uint64_t)mint_getpid());
        stat_card(R(c.x0 + pad * 2 + cw, c.y0 + pad, cw, 92), "MINTDE PROCESS", b2, "ring 3, isolated address space");
        stat_card(R(c.x0 + pad * 3 + cw * 2, c.y0 + pad, cw, 92), "SCHEDULER", "Cooperative", "round-robin, per-task CR3");
        refresh_storage_data(); refresh_driver_catalog();
        format_u64(a, sizeof(a), storage_snapshot.device_count);
        stat_card(R(c.x0 + pad, c.y0 + pad * 2 + 92, cw, 92), "DISKS", a, "AHCI, read-only");
        format_u64(b2, sizeof(b2), fat_volume_list.count);
        stat_card(R(c.x0 + pad * 2 + cw, c.y0 + pad * 2 + 92, cw, 92), "MOUNTED VOLUMES", b2, "FAT16, read-only");
        format_u64(cc, sizeof(cc), driver_catalog_ok ? driver_catalog.count : 0u);
        stat_card(R(c.x0 + pad * 3 + cw * 2, c.y0 + pad * 2 + 92, cw, 92), "DRIVER RECORDS", cc, "see Hardware & Drivers");
        ui_label(c.x0 + pad, c.y0 + pad * 3 + 200, FONT_CAPTION, "CPU load and per-process memory are not measured by this kernel build; no values are guessed.", T.text2);
    } else if (monitor_tab == 1) {
        refresh_storage_data();
        gfx_rect t = R(c.x0 + pad, c.y0 + pad, W(c) - pad * 2, 32 + 30 * (int32_t)(storage_snapshot.device_count ? storage_snapshot.device_count : 1u));
        ui_group(t);
        static const char *const heads[5] = {"Device", "Reads", "Read failures", "Writes blocked", "Writes"};
        int32_t colw = W(t) / 5;
        for (int i = 0; i < 5; ++i) ui_label(t.x0 + 14 + i * colw, t.y0 + 21, FONT_CAPTION_BOLD, heads[i], T.text2);
        for (uint32_t i = 0; i < storage_snapshot.device_count && i < MINT_STORAGE_MAX_DEVICES; ++i) {
            MINT_BLOCK_IO_STATS st = {0};
            MINT_STORAGE_DEVICE_INFO *dv = &storage_snapshot.devices[i];
            if (dv->block_registered) (void)mint_block_io_stats_get(dv->block_index, &st);
            int32_t ry = t.y0 + 32 + (int32_t)i * 30;
            gfx_hline(S, t.x0 + 12, ry, W(t) - 24, T.separator);
            ui_label(t.x0 + 14, ry + 20, FONT_BODY, dv->name, T.text);
            uint64_t v[4] = {st.read_calls, st.read_failures, st.writes_blocked, st.write_success};
            for (int k = 0; k < 4; ++k) { format_u64(a, sizeof(a), v[k]); ui_label(t.x0 + 14 + (k + 1) * colw, ry + 20, FONT_BODY, a, T.text); }
        }
        if (!storage_snapshot.device_count) ui_label(t.x0 + 14, t.y0 + 52, FONT_BODY, "No block devices", T.text3);
    } else {
        (void)mint_input_get_info(&input_info);
        format_u64(a, sizeof(a), input_info.keyboard_events);
        stat_card(R(c.x0 + pad, c.y0 + pad, cw, 92), "KEY EVENTS", a, input_info.keyboard_ready ? "PS/2 keyboard ready" : "keyboard not detected");
        format_u64(b2, sizeof(b2), input_info.pointer_motion_events);
        stat_card(R(c.x0 + pad * 2 + cw, c.y0 + pad, cw, 92), "POINTER MOVES", b2, input_info.mouse_ready ? "PS/2 mouse ready" : "mouse not detected");
        format_u64(cc, sizeof(cc), input_info.dropped_events);
        stat_card(R(c.x0 + pad * 3 + cw * 2, c.y0 + pad, cw, 92), "DROPPED EVENTS", cc, "bounded 64-event queue");
    }
}
static int monitor_click(const mde_layout *l, int32_t x, int32_t y) {
    gfx_rect t = monitor_tabs_rect(l);
    if (in(t, x, y)) { int32_t s = ui_segment_at(t, 3, x); if (s >= 0) monitor_tab = (uint32_t)s; return 1; }
    return 0;
}

/* ------------------------------------------------------------ Storage & Logs */
static uint32_t storage_item_count(void) { return storage_snapshot.device_count + 4u; }
static gfx_rect storage_item_rect(const mde_layout *l, uint32_t i) {
    int32_t y = l->sidebar.y0 + 28 + (int32_t)i * 30;
    if (i >= storage_snapshot.device_count) y += 26;          /* "Controllers" header */
    if (i >= storage_snapshot.device_count + 1u) y += 26;     /* "Reports" header */
    return R(l->sidebar.x0 + 10, y, W(l->sidebar) - 20, 28);
}
static void storage_paint(const mde_layout *l) {
    refresh_storage_data();
    gfx_rect sb = l->sidebar;
    ui_sidebar_header(sb.x0 + 18, sb.y0 + 18, "Devices");
    uint32_t sel_item = storage_view == 0u ? storage_page : storage_view == 1u ? storage_snapshot.device_count
                        : storage_snapshot.device_count + storage_view - 1u;
    for (uint32_t i = 0; i < storage_item_count(); ++i) {
        gfx_rect ir = storage_item_rect(l, i);
        const char *label = i < storage_snapshot.device_count ? storage_snapshot.devices[i].name :
                            i == storage_snapshot.device_count ? "AHCI Controllers" :
                            i == storage_snapshot.device_count + 1u ? "All Messages" :
                            i == storage_snapshot.device_count + 2u ? "Warnings" : "Errors";
        if (i == storage_snapshot.device_count) ui_sidebar_header(sb.x0 + 18, ir.y0 - 8, "Hardware");
        if (i == storage_snapshot.device_count + 1u) ui_sidebar_header(sb.x0 + 18, ir.y0 - 8, "Reports");
        uint32_t sym = i < storage_snapshot.device_count ? SYM_DRIVE : i == storage_snapshot.device_count ? SYM_CHIP :
                       i == storage_snapshot.device_count + 1u ? SYM_LIST : SYM_WARNING;
        ui_sidebar_row(ir, label, sym, i == sel_item);
    }
    toolbar_title(l, storage_view == 0u ? "Device" : storage_view == 1u ? "Controllers" : "Kernel Log", 0);
    ui_button(R(l->toolbar.x1 - 230, l->toolbar.y0 + 12, 90, 28), "Refresh", 0, 1);
    ui_button(R(l->toolbar.x1 - 130, l->toolbar.y0 + 12, 114, 28), "Export to COM1", 0, 1);
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.group_bg));
    gfx_rect b = R(c.x0 + 24, c.y0 + 20, W(c) - 48, H(c) - 24);
    char v[16][48];
    if (storage_view == 0u) {
        if (storage_page >= storage_snapshot.device_count) { ui_label(b.x0, b.y0 + 20, FONT_BODY, "No ATA disk was identified.", T.text2); return; }
        MINT_STORAGE_DEVICE_INFO *d = &storage_snapshot.devices[storage_page];
        copy_text(v[0], 48, d->model[0] ? d->model : "unavailable");
        copy_text(v[1], 48, d->serial[0] ? d->serial : "unavailable");
        format_bytes(v[2], 48, d->capacity_bytes); str_append(v[2], 48, " (");
        str_append(v[2], 48, storage_capacity_source(d->capacity_source)); str_append(v[2], 48, ")");
        format_u64(v[3], 48, d->sector_count);
        copy_text(v[4], 48, d->read_status[0] ? d->read_status : "unavailable");
        copy_text(v[5], 48, storage_table_state(d->partition_table_state));
        format_hex64(v[6], 48, d->mbr_disk_id);
        copy_text(v[7], 48, d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_MATCH ? "Two reads match" :
                            d->lba0_repeat_status == MINT_STORAGE_LBA0_REPEAT_DIFFERS ? "Reads DIFFER" : "Not verified");
        copy_text(v[8], 48, d->filesystem_status);
        copy_text(v[9], 48, d->mount_status[0] ? d->mount_status : "not mounted");
        copy_text(v[10], 48, "Disabled \xE2\x80\x94 no media writes attempted");
        const char *labels[11] = {"Model", "Serial", "Capacity", "Sectors", "Sector 0", "Partition table", "Disk ID",
                                  "Read check", "File system", "Mount", "Writes"};
        const char *values[11] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10]};
        gfx_rect g = R(b.x0, b.y0, W(b), 11 * 34);
        ui_group(g);
        for (uint32_t i = 0; i < 11u; ++i) ui_group_row(R(g.x0, g.y0 + (int32_t)i * 34, W(g), 34), i, 11u, labels[i], values[i]);
    } else if (storage_view == 1u) {
        int32_t y = b.y0;
        for (uint32_t i = 0; i < storage_snapshot.controller_count && i < MINT_STORAGE_MAX_CONTROLLERS; ++i) {
            MINT_STORAGE_CONTROLLER_INFO *ct = &storage_snapshot.controllers[i];
            char bdf[24], tmp[16];
            format_u64(bdf, sizeof(bdf), ct->bus); str_append(bdf, sizeof(bdf), ":");
            format_u64(tmp, sizeof(tmp), ct->device); str_append(bdf, sizeof(bdf), tmp); str_append(bdf, sizeof(bdf), ".");
            format_u64(tmp, sizeof(tmp), ct->function); str_append(bdf, sizeof(bdf), tmp);
            format_hex64(v[0], 48, ct->vendor_id); str_append(v[0], 48, ":"); format_hex64(v[1], 48, ct->device_id); str_append(v[0], 48, v[1]);
            format_hex64(v[2], 48, ct->abar); format_hex64(v[3], 48, ct->port_implemented);
            const char *labels[6] = {"PCI address", "Vendor:Device", "ABAR (BAR5)", "Ports implemented", "State", "Detail"};
            const char *values[6] = {bdf, v[0], v[2], v[3], storage_controller_state(ct->state), ct->reason[0] ? ct->reason : "\xE2\x80\x94"};
            gfx_rect g = R(b.x0, y, W(b), 6 * 34);
            ui_group(g);
            for (uint32_t k = 0; k < 6u; ++k) ui_group_row(R(g.x0, g.y0 + (int32_t)k * 34, W(g), 34), k, 6u, labels[k], values[k]);
            y = g.y1 + 16;
        }
        if (!storage_snapshot.controller_count) ui_label(b.x0, b.y0 + 20, FONT_BODY, "No AHCI controller found in the PCI inventory.", T.text2);
    } else {
        gfx_rect g = R(b.x0, b.y0, W(b), H(b) - 8);
        ui_group(g);
        ui_label(g.x0 + 14, g.y0 + 20, FONT_CAPTION_BOLD, "#", T.text2);
        ui_label(g.x0 + 60, g.y0 + 20, FONT_CAPTION_BOLD, "Type", T.text2);
        ui_label(g.x0 + 130, g.y0 + 20, FONT_CAPTION_BOLD, "Source", T.text2);
        ui_label(g.x0 + 230, g.y0 + 20, FONT_CAPTION_BOLD, "Message", T.text2);
        int32_t rows = (H(g) - 30) / 26;
        for (uint32_t i = 0; i < storage_log_count && (int32_t)i < rows; ++i) {
            MINT_LOG_RECORD *r = &storage_log_records[i];
            int32_t ry = g.y0 + 30 + (int32_t)i * 26;
            if (i & 1u) gfx_fill(S, g.x0 + 1, ry, W(g) - 2, 26, OPAQUE(T.row_alt));
            format_u64(v[0], 48, r->sequence);
            ui_label(g.x0 + 14, ry + 18, FONT_CAPTION, v[0], T.text2);
            ui_badge(g.x0 + 56, ry + 4, storage_severity_name(r->severity),
                     r->severity >= MINT_LOG_ERROR ? 0xFF3B30u : r->severity >= MINT_LOG_WARN ? 0xFF9500u : 0x34C759u);
            ui_label(g.x0 + 130, ry + 18, FONT_CAPTION, storage_component_name(r->component), T.text);
            gfx_text_fit(S, FONT_CAPTION, g.x0 + 230, ry + 18, r->message, W(g) - 244, OPAQUE(T.text));
        }
        if (!storage_log_count) ui_label(g.x0 + 14, g.y0 + 54, FONT_BODY, "No messages match this report.", T.text3);
    }
}
static int storage_click(const mde_layout *l, int32_t x, int32_t y) {
    for (uint32_t i = 0; i < storage_item_count(); ++i)
        if (in(storage_item_rect(l, i), x, y)) {
            if (i < storage_snapshot.device_count) { storage_view = 0u; storage_page = i; }
            else storage_view = i - storage_snapshot.device_count + 1u;
            refresh_storage_data();
            return 1;
        }
    if (in(R(l->toolbar.x1 - 230, l->toolbar.y0 + 12, 90, 28), x, y)) { refresh_storage_data(); return 1; }
    if (in(R(l->toolbar.x1 - 130, l->toolbar.y0 + 12, 114, 28), x, y)) { export_storage_report(); return 1; }
    return 0;
}

/* ------------------------------------------------------------ Hardware & Drivers */
static gfx_rect drivers_item_rect(const mde_layout *l, uint32_t i) {
    return R(l->sidebar.x0 + 10, l->sidebar.y0 + 28 + (int32_t)i * 30, W(l->sidebar) - 20, 28);
}
static const char *pci_class_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x01: return s == 0x06 ? "SATA (AHCI)" : s == 0x08 ? "NVMe" : s == 0x04 ? "RAID" : s == 0x01 ? "IDE" : "Storage";
    case 0x02: return "Ethernet";
    case 0x03: return "Display";
    case 0x04: return "Multimedia";
    case 0x06: return s == 0x00 ? "Host bridge" : s == 0x01 ? "ISA bridge" : s == 0x04 ? "PCI bridge" : "Bridge";
    case 0x0C: return s == 0x03 ? "USB controller" : s == 0x05 ? "SMBus" : "Serial bus";
    case 0x08: return "System peripheral";
    case 0x11: return "Signal processing";
    default: return "Other";
    }
}
static void drivers_paint(const mde_layout *l) {
    static const char *const items[4] = {"Drivers", "PCI Devices", "USB", "Power (ACPI)"};
    static const uint32_t syms[4] = {SYM_GEAR_LINE, SYM_CHIP, SYM_USB, SYM_BOLT};
    ui_sidebar_header(l->sidebar.x0 + 18, l->sidebar.y0 + 18, "Hardware");
    for (uint32_t i = 0; i < 4u; ++i) {
        gfx_rect ir = drivers_item_rect(l, i);
        ui_sidebar_row(ir, items[i], syms[i], driver_view == i);
    }
    toolbar_title(l, items[driver_view < 4u ? driver_view : 0u], 0);
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.group_bg));
    gfx_rect g = R(c.x0 + 24, c.y0 + 20, W(c) - 48, H(c) - 56);
    ui_group(g);
    char a[32], b[32];
    if (driver_view == 0u) {
        refresh_driver_catalog();
        ui_label(g.x0 + 14, g.y0 + 20, FONT_CAPTION_BOLD, "Name", T.text2);
        ui_label(g.x0 + 230, g.y0 + 20, FONT_CAPTION_BOLD, "Status", T.text2);
        ui_label(g.x0 + 360, g.y0 + 20, FONT_CAPTION_BOLD, "Detail", T.text2);
        int32_t rows = (H(g) - 30) / 28;
        for (uint32_t i = 0; driver_catalog_ok && i < driver_catalog.count && (int32_t)i < rows; ++i) {
            MINT_DRIVER_RECORD *r = &driver_catalog.records[i];
            int32_t ry = g.y0 + 30 + (int32_t)i * 28;
            if (i & 1u) gfx_fill(S, g.x0 + 1, ry, W(g) - 2, 28, OPAQUE(T.row_alt));
            gfx_text_fit(S, FONT_BODY, g.x0 + 14, ry + 19, r->name, 205, OPAQUE(T.text));
            uint32_t col = r->state == MINT_DRIVER_ACTIVE || r->state == MINT_DRIVER_READ_ONLY ? 0x34C759u :
                           r->state == MINT_DRIVER_DEVICE_FOUND || r->state == MINT_DRIVER_DIAGNOSTIC_ONLY ? 0xFF9500u : 0x8E8E93u;
            ui_badge(g.x0 + 226, ry + 5, driver_state_label(r->state), col);
            gfx_text_fit(S, FONT_CAPTION, g.x0 + 360, ry + 19, r->detail, W(g) - 374, OPAQUE(T.text2));
        }
        if (!driver_catalog_ok) ui_label(g.x0 + 14, g.y0 + 54, FONT_BODY, "Driver catalog unavailable.", T.text3);
    } else if (driver_view == 1u) {
        (void)mint_pci_inventory_get(&terminal_pci_inventory);
        ui_label(g.x0 + 14, g.y0 + 20, FONT_CAPTION_BOLD, "Address", T.text2);
        ui_label(g.x0 + 110, g.y0 + 20, FONT_CAPTION_BOLD, "Vendor:Device", T.text2);
        ui_label(g.x0 + 260, g.y0 + 20, FONT_CAPTION_BOLD, "Class", T.text2);
        ui_label(g.x0 + 420, g.y0 + 20, FONT_CAPTION_BOLD, "IRQ", T.text2);
        int32_t rows = (H(g) - 30) / 24;
        for (uint32_t i = 0; i < terminal_pci_inventory.devices_stored && (int32_t)i < rows; ++i) {
            MINT_PCI_DEVICE *d = &terminal_pci_inventory.devices[i];
            int32_t ry = g.y0 + 30 + (int32_t)i * 24;
            if (i & 1u) gfx_fill(S, g.x0 + 1, ry, W(g) - 2, 24, OPAQUE(T.row_alt));
            format_u64(a, sizeof(a), d->bus); str_append(a, sizeof(a), ":"); format_u64(b, sizeof(b), d->device);
            str_append(a, sizeof(a), b); str_append(a, sizeof(a), "."); format_u64(b, sizeof(b), d->function); str_append(a, sizeof(a), b);
            ui_label(g.x0 + 14, ry + 17, FONT_CAPTION, a, T.text);
            format_hex64(a, sizeof(a), d->vendor_id); str_append(a, sizeof(a), ":"); format_hex64(b, sizeof(b), d->device_id);
            str_append(a, sizeof(a), b + 2);
            ui_label(g.x0 + 110, ry + 17, FONT_CAPTION, a, T.text);
            ui_label(g.x0 + 260, ry + 17, FONT_CAPTION, pci_class_name(d->class_code, d->subclass), T.text);
            format_u64(a, sizeof(a), d->interrupt_line);
            ui_label(g.x0 + 420, ry + 17, FONT_CAPTION, d->interrupt_pin ? a : "\xE2\x80\x94", T.text2);
        }
    } else if (driver_view == 2u) {
        static MINT_USB_INFO usb;
        if (mint_usb_get_status(&usb) != 0) bytes_clear(&usb, sizeof(usb));
        char head[96];
        copy_text(head, sizeof(head), usb.state == MINT_USB_STATE_RUNNING ? "xHCI controller running" :
                                       usb.state == MINT_USB_STATE_FAILED ? "xHCI controller failed" : "No xHCI controller");
        if (usb.state == MINT_USB_STATE_RUNNING) {
            str_append(head, sizeof(head), "  \xC2\xB7  "); format_u64(a, sizeof(a), usb.ports); str_append(head, sizeof(head), a);
            str_append(head, sizeof(head), " ports  \xC2\xB7  "); format_u64(a, sizeof(a), usb.reports); str_append(head, sizeof(head), a);
            str_append(head, sizeof(head), " HID reports");
        }
        ui_label(g.x0 + 14, g.y0 + 22, FONT_BODY_MEDIUM, head, T.text);
        if (usb.status[0]) gfx_text_fit(S, FONT_CAPTION, g.x0 + 14, g.y0 + 40, usb.status, W(g) - 28, OPAQUE(T.text2));
        ui_label(g.x0 + 14, g.y0 + 66, FONT_CAPTION_BOLD, "Port", T.text2);
        ui_label(g.x0 + 64, g.y0 + 66, FONT_CAPTION_BOLD, "Device", T.text2);
        ui_label(g.x0 + 300, g.y0 + 66, FONT_CAPTION_BOLD, "Kind", T.text2);
        ui_label(g.x0 + 400, g.y0 + 66, FONT_CAPTION_BOLD, "Speed", T.text2);
        ui_label(g.x0 + 490, g.y0 + 66, FONT_CAPTION_BOLD, "Vendor:Product", T.text2);
        static const char *const kinds[6] = {"Unknown", "Keyboard", "Mouse", "Tablet", "Hub", "Other"};
        static const char *const speeds[5] = {"\xE2\x80\x94", "Full (12M)", "Low (1.5M)", "High (480M)", "Super (5G)"};
        for (uint32_t i = 0; i < usb.device_count && i < MINT_USB_MAX_DEVICES; ++i) {
            MINT_USB_DEVICE_INFO *dv = &usb.devices[i];
            int32_t ry = g.y0 + 76 + (int32_t)i * 26;
            if (i & 1u) gfx_fill(S, g.x0 + 1, ry, W(g) - 2, 26, OPAQUE(T.row_alt));
            format_u64(a, sizeof(a), dv->port); ui_label(g.x0 + 14, ry + 18, FONT_CAPTION, a, T.text);
            gfx_text_fit(S, FONT_BODY, g.x0 + 64, ry + 18, dv->product[0] ? dv->product : "USB device", 225, OPAQUE(T.text));
            ui_label(g.x0 + 300, ry + 18, FONT_CAPTION, kinds[dv->kind < 6u ? dv->kind : 0u], T.text);
            ui_label(g.x0 + 400, ry + 18, FONT_CAPTION, speeds[dv->speed < 5u ? dv->speed : 0u], T.text2);
            hex4(a, dv->vendor_id); hex4(b, dv->product_id);
            str_append(a, sizeof(a), ":"); str_append(a, sizeof(a), b);
            ui_label(g.x0 + 490, ry + 18, FONT_CAPTION, a, T.text2);
        }
        if (!usb.device_count)
            ui_label(g.x0 + 14, g.y0 + 100, FONT_BODY, usb.state == MINT_USB_STATE_RUNNING ? "No USB devices connected." :
                                                       "USB devices are available only through firmware emulation.", T.text3);
    } else {
        MINT_POWER_INFO pi = {0};
        (void)mint_power_get_status(&pi);
        char v[6][40];
        format_u64(v[0], 40, pi.acpi_revision); copy_text(v[1], 40, pi.oem_id[0] ? pi.oem_id : "?");
        format_hex64(v[2], 40, pi.pm1a_cnt);
        format_u64(v[3], 40, pi.slp_typa); str_append(v[3], 40, " / "); format_u64(a, sizeof(a), pi.slp_typb); str_append(v[3], 40, a);
        format_hex64(v[4], 40, pi.reset_address);
        copy_text(v[5], 40, (pi.flags & MINT_POWER_HAVE_8042) ? "Reported" : "Not reported");
        const char *labels[6] = {"ACPI revision", "OEM", "PM1a control port", "S5 sleep type", "Reset register", "8042 (PS/2) controller"};
        for (uint32_t i = 0; i < 6u; ++i) ui_group_row(R(g.x0, g.y0 + (int32_t)i * 34, W(g), 34), i, 6u, labels[i], v[i]);
    }
    ui_label(c.x0 + 28, c.y1 - 14, FONT_CAPTION, "A detected device is not the same as driver support. MintOS uses its own drivers, not macOS kexts.", T.text2);
}
static int drivers_click(const mde_layout *l, int32_t x, int32_t y) {
    for (uint32_t i = 0; i < 4u; ++i) if (in(drivers_item_rect(l, i), x, y)) { driver_view = i; return 1; }
    return 0;
}

/* ------------------------------------------------------------ Network */
static uint32_t network_autoconnect = 1u;
static void mac_text(char *out, size_t cap, const uint8_t mac[6]) {
    static const char hx[] = "0123456789ABCDEF";
    size_t m = 0;
    for (int i = 0; i < 6 && m + 3u < cap; ++i) {
        out[m++] = hx[mac[i] >> 4]; out[m++] = hx[mac[i] & 15u];
        if (i < 5) out[m++] = ':';
    }
    out[m] = '\0';
}
/* Status text, dot colour and whether a Disconnect action applies. */
static const char *network_state_text(const MINT_NET_INFO *n, uint32_t *dot, int *connected) {
    *connected = 0;
    if (!n->nic_device_id && !n->driver_active) { *dot = 0x8E8E93u; return "No supported adapter"; }
    switch (n->state) {
    case MINT_NET_STATE_UP: *dot = 0x34C759u; *connected = 1; return "Connected";
    case MINT_NET_STATE_DHCP: *dot = 0xFF9500u; *connected = 1; return "Connecting\xE2\x80\xA6";
    case MINT_NET_STATE_NO_LINK: *dot = 0xFF9500u; *connected = 1; return "Cable unplugged";
    case MINT_NET_STATE_FAILED: *dot = 0xFF3B30u; return n->status[0] ? n->status : "Not connected";
    default: *dot = 0x8E8E93u; return "Not connected";
    }
}
static gfx_rect network_button_rect(const mde_layout *l) { gfx_rect c = l->content; return R(c.x1 - 24 - 120, c.y0 + 34, 120, 28); }
static void network_paint(const mde_layout *l) {
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.group_bg));
    if (mint_net_get_status(&net_info) != 0) bytes_clear(&net_info, sizeof(net_info));
    uint32_t dot; int connected;
    const char *state = network_state_text(&net_info, &dot, &connected);
    gfx_image(S, c.x0 + 24, c.y0 + 18, 56, 56, mint_icon_network_128, 128, 128, 255u);
    ui_label(c.x0 + 96, c.y0 + 42, FONT_HEADLINE, "Ethernet", T.text);
    gfx_circle(S, c.x0 + 101, c.y0 + 60, 4, OPAQUE(dot));
    char line[96];
    copy_text(line, sizeof(line), state);
    if (net_info.state == MINT_NET_STATE_UP) { str_append(line, sizeof(line), " \xE2\x80\x94 "); ip_append(line, sizeof(line), net_info.ip); }
    ui_label(c.x0 + 112, c.y0 + 64, FONT_BODY, line, T.text2);
    int have_adapter = net_info.nic_device_id || net_info.driver_active;
    ui_button(network_button_rect(l), connected ? "Disconnect" : net_info.state == MINT_NET_STATE_FAILED ? "Try Again" : "Connect",
              !connected && have_adapter, have_adapter);

    char v[10][48];
    int up = net_info.state == MINT_NET_STATE_UP;
    const char *dash = "\xE2\x80\x94";
    copy_text(v[0], 48, up ? "Using DHCP" : dash);
    v[1][0] = v[2][0] = v[3][0] = v[4][0] = '\0';
    if (up) {
        ip_append(v[1], 48, net_info.ip); ip_append(v[2], 48, net_info.netmask);
        ip_append(v[3], 48, net_info.gateway); ip_append(v[4], 48, net_info.dns);
    }
    gfx_rect g = R(c.x0 + 24, c.y0 + 96, W(c) - 48, 5 * 32);
    ui_group(g);
    const char *l1[5] = {"Configure IPv4", "IP Address", "Subnet Mask", "Router", "DNS Server"};
    const char *v1[5] = {v[0], up ? v[1] : dash, up ? v[2] : dash, up ? v[3] : dash, up ? v[4] : dash};
    for (uint32_t i = 0; i < 5u; ++i) ui_group_row(R(g.x0, g.y0 + (int32_t)i * 32, W(g), 32), i, 5u, l1[i], v1[i]);

    copy_text(v[5], 48, net_info.adapter[0] ? net_info.adapter : "None");
    if (net_info.driver_active) mac_text(v[6], 48, net_info.mac); else copy_text(v[6], 48, dash);
    copy_text(v[7], 48, !net_info.driver_active ? "Driver idle" : net_info.link_up ? "Up" : "No link");
    gfx_rect h = R(g.x0, g.y1 + 16, W(g), 3 * 32);
    ui_group(h);
    const char *l2[3] = {"Adapter", "MAC Address", "Link"};
    const char *v2[3] = {v[5], v[6], v[7]};
    for (uint32_t i = 0; i < 3u; ++i) ui_group_row(R(h.x0, h.y0 + (int32_t)i * 32, W(h), 32), i, 3u, l2[i], v2[i]);

    char size[24];
    format_u64(v[8], 48, net_info.rx_frames); str_append(v[8], 48, " packets, ");
    format_bytes(size, sizeof(size), net_info.rx_bytes); str_append(v[8], 48, size);
    format_u64(v[9], 48, net_info.tx_frames); str_append(v[9], 48, " packets, ");
    format_bytes(size, sizeof(size), net_info.tx_bytes); str_append(v[9], 48, size);
    gfx_rect k = R(g.x0, h.y1 + 16, W(g), 2 * 32);
    ui_group(k);
    ui_group_row(R(k.x0, k.y0, W(k), 32), 0, 2u, "Received", v[8]);
    ui_group_row(R(k.x0, k.y0 + 32, W(k), 32), 1, 2u, "Sent", v[9]);

    int32_t y = k.y1 + 22;
    if (network_status[0]) { ui_label(c.x0 + 28, y, FONT_CAPTION, network_status, T.text2); y += 18; }
    if (net_info.adapter_flags & MINT_NET_ADAPTER_EXPERIMENTAL) {
        ui_label(c.x0 + 28, y, FONT_CAPTION, "This adapter's driver is experimental; it relies on the firmware's PHY setup.", T.text2); y += 18;
    }
    ui_label(c.x0 + 28, c.y1 - 16, FONT_CAPTION, "IPv4 with DHCP, DNS, ICMP and TCP. Secure web pages use MintOS\xE2\x80\x99s TLS 1.3 with certificate checks.", T.text2);
}
static int network_toggle(void) {
    if (mint_net_get_status(&net_info) != 0) return 0;
    uint32_t dot; int connected;
    (void)network_state_text(&net_info, &dot, &connected);
    int64_t rc = mint_net_set_up(connected ? MINT_NET_MODE_DOWN : MINT_NET_MODE_DHCP);
    copy_text(network_status, sizeof(network_status), connected ? "Disconnected." :
              rc == 0 ? "Requesting an address\xE2\x80\xA6" : rc == -MINT_ENODEV ? "No supported adapter was found." :
              "The adapter could not be started; see Storage & Logs.");
    if (!connected) network_autoconnect = 1u;
    return 1;
}
static int network_click(const mde_layout *l, int32_t x, int32_t y) {
    if (in(network_button_rect(l), x, y)) return network_toggle();
    (void)network_busy_message;
    return 0;
}

/* ------------------------------------------------------------ Disk Utility */
static gfx_rect disks_item_rect(const mde_layout *l, uint32_t i) {
    return R(l->sidebar.x0 + 10, l->sidebar.y0 + 28 + (int32_t)i * 44, W(l->sidebar) - 20, 40);
}
static gfx_rect disks_erase_rect(const mde_layout *l) { return R(l->toolbar.x1 - 196, l->toolbar.y0 + 12, 84, 28); }
static gfx_rect disks_info_rect(const mde_layout *l) { return R(l->toolbar.x1 - 104, l->toolbar.y0 + 12, 88, 28); }
static gfx_rect disks_sheet(const mde_layout *l) {
    int32_t w = 480, h = 252;
    return R(l->frame.x0 + (W(l->frame) - w) / 2, l->content.y0 - 1, w, h);
}
/* Only offer Erase for what the kernel formatter will accept: a registered
   disk with no MBR signature that is not mounted. The kernel re-checks
   everything (zeroed sectors, size, serial, confirmation phrase). */
static int disk_is_erasable(uint32_t i) {
    if (i >= storage_snapshot.device_count || i >= MINT_STORAGE_MAX_DEVICES) return 0;
    MINT_STORAGE_DEVICE_INFO *d = &storage_snapshot.devices[i];
    if (!d->block_registered || d->mbr_signature_ok || disk_is_mintos(d)) return 0;
    for (uint32_t v = 0; v < fat_volume_list.count; ++v) if (fat_volume_list.block_indices[v] == d->block_index) return 0;
    return 1;
}
static gfx_rect disks_kind_rect(gfx_rect sh) { return R(sh.x0 + 84, sh.y0 + 92, W(sh) - 104, 26); }
static void disk_expected_phrase(char *out, size_t cap) {
    copy_text(out, cap, "FORMAT ");
    if (disk_selected_device < storage_snapshot.device_count) str_append(out, cap, storage_snapshot.devices[disk_selected_device].name);
}
static void disks_paint(const mde_layout *l) {
    refresh_storage_data();
    ui_sidebar_header(l->sidebar.x0 + 18, l->sidebar.y0 + 18, "Internal");
    for (uint32_t i = 0; i < storage_snapshot.device_count && i < MINT_STORAGE_MAX_DEVICES && i < 8u; ++i) {
        MINT_STORAGE_DEVICE_INFO *d = &storage_snapshot.devices[i];
        gfx_rect ir = disks_item_rect(l, i);
        int sel = disk_selected_device == i;
        if (sel) gfx_round_rect(S, ir.x0, ir.y0, W(ir), H(ir), 6, T.sidebar_selection);
        uint32_t fg = T.text;
        gfx_image(S, ir.x0 + 6, ir.y0 + 6, 28, 28, mint_icon_disk_utility_64, 64, 64, 255u);
        gfx_text_fit(S, FONT_BODY_MEDIUM, ir.x0 + 42, ir.y0 + 17, d->model[0] ? d->model : d->name, W(ir) - 50, OPAQUE(fg));
        char cap[32]; format_bytes(cap, sizeof(cap), d->capacity_bytes);
        char sub[48]; copy_text(sub, sizeof(sub), d->name); str_append(sub, sizeof(sub), " \xC2\xB7 "); str_append(sub, sizeof(sub), cap);
        ui_label(ir.x0 + 42, ir.y0 + 33, FONT_CAPTION, sub, T.text2);
    }
    toolbar_title(l, "Disk Utility", 0);
    ui_button(disks_erase_rect(l), "Erase\xE2\x80\xA6", 0, disk_is_erasable(disk_selected_device));
    ui_button(disks_info_rect(l), "Refresh", 0, 1);
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.group_bg));
    if (disk_selected_device >= storage_snapshot.device_count) {
        ui_label_center(c.x0 + W(c) / 2, c.y0 + 80, FONT_BODY, "No disks found", T.text3);
        return;
    }
    MINT_STORAGE_DEVICE_INFO *d = &storage_snapshot.devices[disk_selected_device];
    gfx_image(S, c.x0 + 24, c.y0 + 18, 64, 64, mint_icon_disk_utility_128, 128, 128, 255u);
    gfx_text_fit(S, FONT_TITLE, c.x0 + 102, c.y0 + 46, d->model[0] ? d->model : d->name, W(c) - 130, OPAQUE(T.text));
    char cap[32]; format_bytes(cap, sizeof(cap), d->capacity_bytes);
    char sub[96]; copy_text(sub, sizeof(sub), cap); str_append(sub, sizeof(sub), " SATA Internal Physical Disk \xC2\xB7 ");
    mfs_refresh_volume();
    int mintos = disk_is_mintos(d);
    str_append(sub, sizeof(sub), mintos ? "GUID Partition Map" : d->mbr_signature_ok ? "Master Boot Record" : "Uninitialized");
    ui_label(c.x0 + 102, c.y0 + 70, FONT_BODY, sub, T.text2);
    int mounted = 0;
    for (uint32_t v = 0; v < fat_volume_list.count; ++v) if (d->block_registered && fat_volume_list.block_indices[v] == d->block_index) mounted = 1;
    gfx_rect bar = R(c.x0 + 24, c.y0 + 100, W(c) - 48, 22);
    if (mintos) {
        uint64_t used = mfs_volume.total_bytes - mfs_volume.free_bytes;
        uint32_t permille = mfs_volume.total_bytes ? (uint32_t)(used * 1000u / mfs_volume.total_bytes) : 0u;
        ui_progress(bar, permille < 8u ? 8u : permille, T.accent);
        char label[96]; copy_text(label, sizeof(label), mfs_volume.label); str_append(label, sizeof(label), " (MintFS) \xC2\xB7 ");
        char ub[32]; format_bytes(ub, sizeof(ub), used); str_append(label, sizeof(label), ub); str_append(label, sizeof(label), " used");
        ui_label(bar.x0 + 4, bar.y1 + 20, FONT_CAPTION, label, T.text2);
        char fb[40]; format_bytes(fb, sizeof(fb), mfs_volume.free_bytes); str_append(fb, sizeof(fb), " free");
        ui_label_right(bar.x1 - 4, bar.y1 + 20, FONT_CAPTION, fb, T.text2);
    } else {
        ui_progress(bar, 1000u, mounted ? T.accent : (T.dark ? 0x48484Au : 0xD1D1D6u));
        ui_label(bar.x0 + 4, bar.y1 + 20, FONT_CAPTION, mounted ? "MINTOS26 (FAT16)" : "Free / unformatted", T.text2);
        ui_label_right(bar.x1 - 4, bar.y1 + 20, FONT_CAPTION, cap, T.text2);
    }
    char ser[48]; copy_text(ser, sizeof(ser), d->serial[0] ? d->serial : "unavailable");
    char sectors[32]; format_u64(sectors, sizeof(sectors), d->sector_count);
    const char *labels[6] = {"Location", "Connection", "Device", "Partition Map", "Serial Number", "Sectors"};
    const char *values[6] = {"Internal", "SATA (AHCI)", d->name, mintos ? "GUID Partition Map" : d->mbr_signature_ok ? "Master Boot Record" : "None (blank)", ser, sectors};
    gfx_rect g = R(c.x0 + 24, bar.y1 + 36, W(c) - 48, 3 * 34);
    ui_group(g);
    int32_t half = W(g) / 2;
    for (uint32_t i = 0; i < 6u; ++i) {
        gfx_rect cell = R(g.x0 + (int32_t)(i / 3u) * half, g.y0 + (int32_t)(i % 3u) * 34, half, 34);
        ui_group_row(cell, i % 3u, 3u, labels[i], values[i]);
    }
    gfx_vline(S, g.x0 + half, g.y0 + 6, H(g) - 12, T.separator);
    ui_label(c.x0 + 28, g.y1 + 26, FONT_CAPTION,
             disk_result[0] ? disk_result : mintos ?
             "MintOS disk: Files and Notes save here. Writes stay inside its MintFS partition." :
             disk_is_erasable(disk_selected_device) ?
             "This disk looks blank. Erase can make it a MintOS disk (read/write) or a FAT16 volume." :
             "This disk has a partition table or is in use, so it cannot be erased. Writes to it stay disabled.",
             disk_result[0] ? (disk_result[0] == 'S' ? 0x34C759u : 0xFF9500u) : T.text2);
    if (disk_format_prompt) {
        gfx_rect sh = disks_sheet(l);
        gfx_shadow(S, sh.x0, sh.y0, W(sh), H(sh), 10, 28, 12, ARGB(0x60, 0, 0, 0));
        gfx_round_rect(S, sh.x0, sh.y0, W(sh), H(sh), 10, OPAQUE(T.window));
        gfx_round_rect_stroke(S, sh.x0, sh.y0, W(sh), H(sh), 10, T.separator);
        gfx_image(S, sh.x0 + 20, sh.y0 + 20, 48, 48, mint_icon_disk_utility_128, 128, 128, 255u);
        char title[64]; copy_text(title, sizeof(title), "Erase \xE2\x80\x9C"); str_append(title, sizeof(title), d->name);
        str_append(title, sizeof(title), "\xE2\x80\x9D?");
        ui_label(sh.x0 + 84, sh.y0 + 36, FONT_HEADLINE, title, T.text);
        ui_label(sh.x0 + 84, sh.y0 + 58, FONT_CAPTION, "Erasing destroys all data on this disk. It then gets", T.text2);
        ui_label(sh.x0 + 84, sh.y0 + 74, FONT_CAPTION, disk_format_kind == 0 ?
                 "a GUID partition map with one read/write MintOS (MintFS) volume." :
                 "an MBR with one FAT16 volume that MintOS mounts read-only.", T.text2);
        static const char *const kinds[2] = {"MintOS (MintFS)", "MS-DOS (FAT16)"};
        ui_segmented(disks_kind_rect(sh), kinds, 2, disk_format_kind);
        char phrase[32]; disk_expected_phrase(phrase, sizeof(phrase));
        char prompt[64]; copy_text(prompt, sizeof(prompt), "Type \xE2\x80\x9C"); str_append(prompt, sizeof(prompt), phrase);
        str_append(prompt, sizeof(prompt), "\xE2\x80\x9D to confirm:");
        ui_label(sh.x0 + 84, sh.y0 + 144, FONT_BODY, prompt, T.text);
        ui_text_field(R(sh.x0 + 84, sh.y0 + 154, W(sh) - 104, 26), disk_confirmation, phrase, 1);
        ui_button(R(sh.x1 - 214, sh.y1 - 44, 90, 28), "Cancel", 0, 1);
        int match = text_length(disk_confirmation) == text_length(phrase) && ci_contains(disk_confirmation, phrase) &&
                    ci_contains(phrase, disk_confirmation);
        ui_button(R(sh.x1 - 112, sh.y1 - 44, 92, 28), "Erase", match, match);
    }
}
static void disks_run_format(void) {
    MINT_STORAGE_FORMAT_REQUEST request = {0};
    request.version = MINT_STORAGE_FORMAT_REQUEST_VERSION;
    request.device_index = disk_selected_device;
    disk_result[0] = '\0';
    if (disk_selected_device < storage_snapshot.device_count && disk_selected_device < MINT_STORAGE_MAX_DEVICES) {
        MINT_STORAGE_DEVICE_INFO *device = &storage_snapshot.devices[disk_selected_device];
        request.expected_sector_count = device->sector_count;
        (void)copy_text(request.expected_serial, sizeof(request.expected_serial), device->serial);
        (void)copy_text(request.confirmation, sizeof(request.confirmation), disk_confirmation);
        int64_t rc = disk_format_kind == 0 ? mint_storage_init_mintos(&request) : mint_storage_format_blank(&request);
        if (rc == 0 && disk_format_kind == 0) {
            copy_text(disk_result, sizeof(disk_result), "Success: \xE2\x80\x9CMintOS HD\xE2\x80\x9D is ready. Files and Notes now save to it.");
            mfs_refresh_volume();
            notes_loaded = 0;
        } else if (rc == 0) copy_text(disk_result, sizeof(disk_result), "Success: disk erased and its FAT16 volume validated read-only.");
        else if (rc == -MINT_EBUSY && disk_format_kind == 0) copy_text(disk_result, sizeof(disk_result), "A MintOS disk is already in use. Only one can be mounted at a time.");
        else {
            copy_text(disk_result, sizeof(disk_result), "Erase failed (error ");
            char code[20]; format_u64(code, sizeof(code), (uint64_t)(-rc));
            str_append(disk_result, sizeof(disk_result), code); str_append(disk_result, sizeof(disk_result), "). The disk was not changed if checks failed.");
        }
    } else copy_text(disk_result, sizeof(disk_result), "Error: the selected disk is no longer available.");
    disk_format_prompt = 0; disk_confirmation[0] = '\0'; disk_confirmation_length = 0;
    refresh_storage_data();
}
static int disks_click(const mde_layout *l, int32_t x, int32_t y) {
    if (disk_format_prompt) {
        gfx_rect sh = disks_sheet(l);
        if (in(R(sh.x1 - 214, sh.y1 - 44, 90, 28), x, y)) { disk_format_prompt = 0; disk_confirmation[0] = '\0'; disk_confirmation_length = 0; return 1; }
        if (in(disks_kind_rect(sh), x, y)) { int32_t k = ui_segment_at(disks_kind_rect(sh), 2, x); if (k >= 0) disk_format_kind = (uint32_t)k; return 1; }
        if (in(R(sh.x1 - 112, sh.y1 - 44, 92, 28), x, y)) {
            char phrase[32]; disk_expected_phrase(phrase, sizeof(phrase));
            if (text_length(disk_confirmation) == text_length(phrase) && ci_contains(disk_confirmation, phrase)) disks_run_format();
            return 1;
        }
        return 1;
    }
    for (uint32_t i = 0; i < storage_snapshot.device_count && i < 8u; ++i)
        if (in(disks_item_rect(l, i), x, y)) { disk_selected_device = i; disk_result[0] = '\0'; return 1; }
    if (in(disks_erase_rect(l), x, y) && disk_is_erasable(disk_selected_device)) {
        disk_format_prompt = 1; disk_confirmation[0] = '\0'; disk_confirmation_length = 0; disk_result[0] = '\0'; return 1;
    }
    if (in(disks_info_rect(l), x, y)) { refresh_storage_data(); return 1; }
    return 0;
}
static int disks_key(const MINT_DESKTOP_KEY_EVENT *k) {
    if (!disk_format_prompt) return 0;
    uint32_t ch = k->codepoint;
    if (ch == 27u) { disk_format_prompt = 0; disk_confirmation[0] = '\0'; disk_confirmation_length = 0; return 1; }
    if (ch == 8u) { if (disk_confirmation_length) disk_confirmation[--disk_confirmation_length] = '\0'; return 1; }
    if (ch == '\n') {
        char phrase[32]; disk_expected_phrase(phrase, sizeof(phrase));
        if (text_length(disk_confirmation) == text_length(phrase) && ci_contains(disk_confirmation, phrase)) disks_run_format();
        return 1;
    }
    if (ch >= 32u && ch < 127u && disk_confirmation_length + 1u < sizeof(disk_confirmation)) {
        disk_confirmation[disk_confirmation_length++] = (char)ch; disk_confirmation[disk_confirmation_length] = '\0';
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ About windows */
static const char *const os_versions[8] = {
    "26.5.013.0  MintFS: read/write copy-on-write disks, MintOS HD",
    "26.5.012.0  TLS 1.3, certificate checks, HTTPS in Web and curl",
    "26.5.011.0  USB input, Intel Ethernet, IPv4/TCP stack, Web browser",
    "26.5.010.0  MintDE 2: macOS-style desktop, shared canvas, RTC clock",
    "26.5.009.0  Real-hardware display, mouse, ACPI power, RAID-mode SATA",
    "26.5.008.0  VirtualBox E1000 RX loopback and descriptor diagnostics",
    "26.5.007.0  E1000 TX timeout isolation",
    "26.5.006.0  E1000 loopback diagnostics and terminal prompt"
};
static const char *const de_versions[8] = {
    "MintDE 2.3.0  Finder on MintOS HD, Trash, Notes saved to disk",
    "MintDE 2.2.0  Secure web browsing, padlock, DuckDuckGo search",
    "MintDE 2.1.0  Web browser, Network app, Terminal network tools",
    "MintDE 2.0.0  Windows, Dock, Launchpad, Spotlight, Control Center",
    "MintDE 1.0.8  Shut Down / Restart and precise pointer",
    "MintDE 1.0.7  VirtualBox loopback and RX diagnostics",
    "MintDE 1.0.6  E1000 loopback failure diagnostics",
    "MintDE 1.0.5  E1000 loopback diagnostics and terminal prompt"
};
static void about_paint(uint32_t app, const mde_layout *l) {
    gfx_rect f = l->frame;
    int32_t cx = f.x0 + W(f) / 2;
    if (show_changelog) {
        ui_label_center(cx, f.y0 + 52, FONT_HEADLINE, "Release Notes", T.text);
        const char *const *list = app == APP_ABOUT_OS ? os_versions : de_versions;
        gfx_rect g = R(f.x0 + 24, f.y0 + 68, W(f) - 48, 8 * 30);
        ui_group(g);
        for (uint32_t i = 0; i < 8u; ++i) {
            gfx_rect row = R(g.x0, g.y0 + (int32_t)i * 30, W(g), 30);
            if (i) gfx_hline(S, row.x0 + 12, row.y0, W(row) - 24, T.separator);
            gfx_text_fit(S, i == 0 ? FONT_BODY_MEDIUM : FONT_BODY, row.x0 + 14, BL(row, FONT_BODY), list[i], W(row) - 28, OPAQUE(i == 0 ? T.text : T.text2));
        }
        ui_button(R(cx - 50, f.y1 - 44, 100, 28), "Done", 1, 1);
        return;
    }
    gfx_image(S, cx - 56, f.y0 + 34, 112, 112, app_icon(app, 1), 128, 128, 255u);
    ui_label_center(cx, f.y0 + 186, FONT_HUGE, app == APP_ABOUT_OS ? "MintOS26" : "MintDE", T.text);
    ui_label_center(cx, f.y0 + 210, FONT_BODY, app == APP_ABOUT_OS ? "Version " MINTOS_VERSION : "Version " MINTDE_VERSION, T.text2);
    MINT_DISPLAY_INFO di = {0}; (void)mint_display_get_info(&di);
    char res[40], tmp[16]; format_u64(res, sizeof(res), di.width); str_append(res, sizeof(res), " \xC3\x97 ");
    format_u64(tmp, sizeof(tmp), di.height); str_append(res, sizeof(res), tmp);
    const char *k1 = app == APP_ABOUT_OS ? "Kernel" : "Desktop", *v1 = app == APP_ABOUT_OS ? "x86-64 freestanding, UEFI boot" : "Native ring-3 environment";
    const char *k2 = app == APP_ABOUT_OS ? "Desktop" : "Font", *v2 = app == APP_ABOUT_OS ? "MintDE " MINTDE_VERSION : "Inter (SIL Open Font License)";
    int32_t ky = f.y0 + 246;
    ui_label_right(cx - 8, ky, FONT_BODY, k1, T.text); ui_label(cx + 8, ky, FONT_BODY, v1, T.text2);
    ui_label_right(cx - 8, ky + 20, FONT_BODY, k2, T.text); ui_label(cx + 8, ky + 20, FONT_BODY, v2, T.text2);
    ui_label_right(cx - 8, ky + 40, FONT_BODY, "Display", T.text); ui_label(cx + 8, ky + 40, FONT_BODY, res, T.text2);
    ui_button(R(cx - 130, f.y1 - 52, 120, 28), "More Info\xE2\x80\xA6", 0, 1);
    ui_button(R(cx + 10, f.y1 - 52, 120, 28), "Release Notes", 0, 1);
}
static int about_click(uint32_t app, const mde_layout *l, int32_t x, int32_t y) {
    gfx_rect f = l->frame;
    int32_t cx = f.x0 + W(f) / 2;
    if (show_changelog) { if (in(R(cx - 50, f.y1 - 44, 100, 28), x, y)) { show_changelog = 0; return 1; } return 0; }
    if (in(R(cx + 10, f.y1 - 52, 120, 28), x, y)) { show_changelog = 1; return 1; }
    if (in(R(cx - 130, f.y1 - 52, 120, 28), x, y)) {
        settings_category = app == APP_ABOUT_OS ? SET_GENERAL : SET_APPEARANCE;
        shell_open_app(APP_SETTINGS);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ Calculator */
/* Fixed-point decimal arithmetic: values are scaled by 10^6. */
#define CALC_SCALE 1000000ll
static int64_t calc_acc, calc_cur;
static int calc_op, calc_entering, calc_decimals, calc_error;
static char calc_display[32] = "0";
static void calc_format(int64_t v) {
    if (calc_error) { copy_text(calc_display, sizeof(calc_display), "Error"); return; }
    char buf[32]; size_t n = 0; int neg = v < 0; uint64_t a = (uint64_t)(neg ? -v : v);
    uint64_t ip = a / CALC_SCALE, fp = a % CALC_SCALE;
    char ib[24]; format_u64(ib, sizeof(ib), ip);
    if (neg) buf[n++] = '-';
    for (size_t i = 0; ib[i]; ++i) {
        size_t rem = text_length(ib) - i;
        buf[n++] = ib[i];
        if (rem > 1 && rem % 3 == 1) buf[n++] = ',';
    }
    if (fp) {
        buf[n++] = '.';
        char fb[8]; for (int i = 5; i >= 0; --i) { fb[i] = (char)('0' + fp % 10); fp /= 10; }
        int last = 5; while (last > 0 && fb[last] == '0') --last;
        for (int i = 0; i <= last; ++i) buf[n++] = fb[i];
    }
    buf[n] = '\0';
    copy_text(calc_display, sizeof(calc_display), buf);
}
static int64_t calc_apply(int64_t a, int64_t b, int op) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return (a / 1000) * (b / 1000);
    case '/': if (!b) { calc_error = 1; return 0; } return (a * 1000) / (b / 1000 ? b / 1000 : 1);
    default: return b;
    }
}
static void calc_input(char k) {
    if (calc_error && k != 'C') return;
    if (k >= '0' && k <= '9') {
        if (!calc_entering) { calc_cur = 0; calc_entering = 1; calc_decimals = 0; }
        if (calc_decimals) { if (calc_decimals < 7) { int64_t p = CALC_SCALE; for (int i = 0; i < calc_decimals; ++i) p /= 10; calc_cur += (calc_cur < 0 ? -1 : 1) * (k - '0') * p; ++calc_decimals; } }
        else if (calc_cur < 100000000000ll * CALC_SCALE / 10) calc_cur = calc_cur * 10 + (k - '0') * CALC_SCALE;
        calc_format(calc_cur);
        if (calc_decimals == 1) str_append(calc_display, sizeof(calc_display), ".");
    } else if (k == '.') {
        if (!calc_entering) { calc_cur = 0; calc_entering = 1; }
        if (!calc_decimals) { calc_decimals = 1; calc_format(calc_cur); str_append(calc_display, sizeof(calc_display), "."); }
    } else if (k == 'C') {
        calc_acc = calc_cur = 0; calc_op = 0; calc_entering = 0; calc_decimals = 0; calc_error = 0; calc_format(0);
    } else if (k == 'n') { calc_cur = -calc_cur; calc_format(calc_cur); }
    else if (k == '%') { calc_cur /= 100; calc_format(calc_cur); }
    else if (k == '+' || k == '-' || k == '*' || k == '/' || k == '=') {
        if (calc_op && calc_entering) calc_acc = calc_apply(calc_acc, calc_cur, calc_op);
        else if (!calc_op) calc_acc = calc_cur;
        calc_op = k == '=' ? 0 : k;
        calc_entering = 0; calc_decimals = 0;
        if (k == '=') calc_cur = calc_acc;
        calc_format(calc_acc);
    }
}
static const char calc_keys[20] = {'C', 'n', '%', '/', '7', '8', '9', '*', '4', '5', '6', '-', '1', '2', '3', '+', '0', '0', '.', '='};
static gfx_rect calc_key_rect(const mde_layout *l, int i) {
    gfx_rect f = l->frame;
    int32_t kw = (W(f) - 2) / 4, kh = 52, top = f.y1 - 5 * kh - 1;
    int col = i % 4, row = i / 4;
    if (i == 16) return R(f.x0 + 1, top + 4 * kh, kw * 2, kh);
    if (i == 17) return R(0, 0, 0, 0);
    return R(f.x0 + 1 + col * kw, top + row * kh, kw, kh);
}
static void calculator_paint(const mde_layout *l) {
    gfx_rect f = l->frame;
    gfx_round_rect(S, f.x0, f.y0, W(f), H(f), 10, OPAQUE(0x2B2B2Du));
    int32_t font = gfx_text_width(FONT_DISPLAY, calc_display) > W(f) - 30 ? FONT_HUGE : FONT_DISPLAY;
    ui_label_right(f.x1 - 16, f.y1 - 5 * 52 - 14, font, calc_display, 0xFFFFFFu);
    static const char *const labels[20] = {"AC", "\xC2\xB1", "%", "\xC3\xB7", "7", "8", "9", "\xC3\x97", "4", "5", "6", "\xE2\x88\x92",
                                           "1", "2", "3", "+", "0", "", ".", "="};
    for (int i = 0; i < 20; ++i) {
        if (i == 17) continue;
        gfx_rect k = calc_key_rect(l, i);
        int op = (i % 4 == 3) || i == 19;
        int top = i < 3;
        uint32_t bg = op ? 0xFF9F0Au : top ? 0x5C5C5Fu : 0x77777Au;
        if (op && calc_op && calc_keys[i] == calc_op && !calc_entering) bg = 0xFFE3BCu;
        gfx_fill(S, k.x0, k.y0, W(k) - 1, H(k) - 1, OPAQUE(bg));
        ui_label_center(k.x0 + W(k) / 2, BL(k, FONT_LARGE), FONT_LARGE, labels[i],
                        (op && calc_op && calc_keys[i] == calc_op && !calc_entering) ? 0xFF9F0Au : 0xFFFFFFu);
    }
}
static int calculator_click(const mde_layout *l, int32_t x, int32_t y) {
    for (int i = 0; i < 20; ++i) if (i != 17 && in(calc_key_rect(l, i), x, y)) { calc_input(calc_keys[i]); return 1; }
    return 0;
}
static int calculator_key(const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || ch == '=') { calc_input((char)ch); return 1; }
    if (ch == '\n') { calc_input('='); return 1; }
    if (ch == 27u || ch == 8u) { calc_input('C'); return 1; }
    return 0;
}

/* ------------------------------------------------------------ dispatch */
void settings_select_pane(uint32_t pane) { if (pane < SET_COUNT) settings_category = pane; }
void app_init(void) {
    files_volume = 0u; files_path[0] = '\0'; files_selected_entry = -1; files_preview_active = 0u;
    terminal_input_length = 0u; terminal_input[0] = '\0'; terminal_clear();
    terminal_push("MintOS26 Terminal \xE2\x80\x94 type help for commands");
    /* Virtual-machine adapters connect on their own; experimental drivers
       (PCH-integrated Intel LAN on real PCs) start only when asked. */
    if (network_autoconnect && mint_net_get_status(&net_info) == 0 && net_info.nic_device_id &&
        !(net_info.adapter_flags & MINT_NET_ADAPTER_EXPERIMENTAL) && net_info.state == MINT_NET_STATE_DOWN)
        (void)mint_net_set_up(MINT_NET_MODE_DHCP);
    refresh_storage_data();
    refresh_files();
    calc_format(0);
}
void app_opened(uint32_t app) {
    if (app == APP_FILES) {
        refresh_storage_data(); mfs_refresh_volume();
        if (!files_started) { files_started = 1; if (mfs_ready) files_go_mfs(mfs_volume.home); else refresh_files(); }
        else if (files_on_mfs()) {
            char keep[MINT_MFS_NAME_MAX + 1] = {0};
            if (files_selected_entry >= 0 && (uint32_t)files_selected_entry < mfs_entry_count)
                copy_text(keep, sizeof(keep), mfs_entries[files_selected_entry].name);
            files_renaming = 0;
            files_after_change(keep[0] ? keep : 0);
        }
        else if (files_volume) refresh_fat_files(); else refresh_files();
    }
    if (app == APP_NOTES && !notes_on_disk) notes_loaded = 0;     /* MintOS HD may have appeared */
    if (app == APP_STORAGE || app == APP_DISKS || app == APP_SETTINGS || app == APP_MONITOR) refresh_storage_data();
    if (app == APP_DRIVERS || app == APP_MONITOR) refresh_driver_catalog();
    if (app == APP_WEB) web_opened();
    if (app == APP_ABOUT_OS || app == APP_ABOUT_MINTDE) show_changelog = 0;
}
void app_paint(uint32_t app, const mde_layout *l) {
    switch (app) {
    case APP_FILES: files_paint(l); break;
    case APP_NOTES: notes_paint(l); break;
    case APP_TERMINAL: terminal_paint(l); break;
    case APP_SETTINGS: settings_paint(l); break;
    case APP_MONITOR: monitor_paint(l); break;
    case APP_STORAGE: storage_paint(l); break;
    case APP_DRIVERS: drivers_paint(l); break;
    case APP_NETWORK: network_paint(l); break;
    case APP_DISKS: disks_paint(l); break;
    case APP_ABOUT_OS: case APP_ABOUT_MINTDE: about_paint(app, l); break;
    case APP_CALCULATOR: calculator_paint(l); break;
    case APP_WEB: web_paint(l); break;
    default: break;
    }
}
int app_click(uint32_t app, const mde_layout *l, int32_t x, int32_t y, uint32_t clicks) {
    switch (app) {
    case APP_FILES: return files_click(l, x, y, clicks);
    case APP_SETTINGS: return settings_click(l, x, y);
    case APP_MONITOR: return monitor_click(l, x, y);
    case APP_STORAGE: return storage_click(l, x, y);
    case APP_DRIVERS: return drivers_click(l, x, y);
    case APP_NETWORK: return network_click(l, x, y);
    case APP_DISKS: return disks_click(l, x, y);
    case APP_ABOUT_OS: case APP_ABOUT_MINTDE: return about_click(app, l, x, y);
    case APP_CALCULATOR: return calculator_click(l, x, y);
    case APP_WEB: return web_click(l, x, y);
    default: return 0;
    }
}
int app_key(uint32_t app, const mde_layout *l, const MINT_DESKTOP_KEY_EVENT *k) {
    (void)l;
    switch (app) {
    case APP_FILES: return files_key(k);
    case APP_NOTES: return notes_key(k);
    case APP_TERMINAL: return terminal_key(k);
    case APP_DISKS: return disks_key(k);
    case APP_CALCULATOR: return calculator_key(k);
    case APP_WEB: return web_key(l, k);
    default: return 0;
    }
}
int app_scroll(uint32_t app, const mde_layout *l, int32_t x, int32_t y, int32_t delta) {
    (void)l; (void)x; (void)y;
    if (app == APP_FILES) { files_scroll += delta * 3; if (files_scroll < 0) files_scroll = 0; return 1; }
    if (app == APP_TERMINAL) { terminal_scroll -= delta * 3; if (terminal_scroll < 0) terminal_scroll = 0; return 1; }
    if (app == APP_WEB) return web_scroll(l, delta);
    return 0;
}
uint32_t app_background(void) {
    uint32_t dirty = 0;
    if (job.kind != JOB_NONE && terminal_job_step()) dirty |= 1u << APP_TERMINAL;
    if (web_background()) dirty |= 1u << APP_WEB;
    /* Network views follow the live link/DHCP state twice a second. */
    static uint64_t last_poll;
    static uint32_t last_signature;
    uint64_t now = term_ms();
    if (notes_dirty && notes_on_disk && (int64_t)(now - notes_edit_ms) >= 1200) { notes_flush(); dirty |= 1u << APP_NOTES; }
    if (now - last_poll >= 500u) {
        last_poll = now;
        if (mint_net_get_status(&net_info) == 0) {
            uint32_t signature = net_info.state * 2654435761u ^ net_info.ip ^ (net_info.link_up << 7) ^
                                 (uint32_t)net_info.rx_frames ^ ((uint32_t)net_info.tx_frames << 3) ^ (net_info.driver_active << 9);
            if (signature != last_signature) { last_signature = signature; dirty |= (1u << APP_NETWORK) | (1u << APP_SETTINGS); }
        }
    }
    return dirty;
}
int app_tick(uint32_t app) {
    ++ui_ticks;
    /* Blink the text caret at ~1 Hz in Terminal and Notes. */
    return (app == APP_TERMINAL || app == APP_NOTES) && (ui_ticks % 50u) == 0u;
}
uint32_t app_cursor(uint32_t app, const mde_layout *l, int32_t x, int32_t y) {
    if ((app == APP_TERMINAL || app == APP_NOTES) && in(l->content, x, y)) return MINT_CURSOR_SHAPE_IBEAM;
    if (app == APP_FILES && (in(files_search_rect(l), x, y) || files_renaming)) return MINT_CURSOR_SHAPE_IBEAM;
    if (app == APP_WEB) return web_cursor(l, x, y);
    return MINT_CURSOR_SHAPE_ARROW;
}
