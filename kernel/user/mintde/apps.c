/* MintDE 2 applications. The data/command logic (storage diagnostics, the
 * bounded Terminal, Files navigation, Notes persistence in the session RAMFS,
 * driver catalog) is carried over from MintDE 1.0.8 unchanged; every view is
 * redrawn in a macOS-style layout on top of the gfx/widgets layer. */
#include "mintde.h"

#define TERM_LINES 24u
#define TERM_COLS 96u

static MINT_FS_DIRENT fs_entries[MINT_RAMFS_MAX_FILES];
static uint32_t fs_entry_count;
static char note_buffer[512];
static size_t note_length;
static uint32_t notes_loaded;
static MINT_INPUT_INFO input_info;
static uint32_t disk_selected_device;
static uint32_t disk_format_prompt;
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
static uint32_t terminal_volume_ordinal;
static char terminal_current_path[96];

/* Legacy-name helpers used by the carried-over logic. */
#define terminal_equals_ci terminal_equals

/* Forward declarations for the carried-over logic. */
static const char *driver_state_label(uint32_t state);
static void refresh_driver_catalog(void);
static void refresh_fat_files(void);
static void refresh_files(void);
static void terminal_run_command(void);
static void terminal_push(const char *text);

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
    storage_serial_text(storage_snapshot.readonly_mount_active ? "FAT16: read-only mount validated; persistence writes remain disabled\r\n" : "FAT16 mount: not active; persistent writes remain disabled in 26.5.010.0\r\n");
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

static void terminal_push(const char *text) {
    if (terminal_output_count >= TERM_LINES) {
        for (uint32_t i=1u;i<TERM_LINES;i++) {
            (void)copy_text(terminal_output[i-1u],sizeof(terminal_output[i-1u]),terminal_output[i]);
            terminal_output_kind[i-1u]=terminal_output_kind[i];
        }
        terminal_output_count=TERM_LINES-1u;
    }
    (void)copy_text(terminal_output[terminal_output_count],sizeof(terminal_output[terminal_output_count]),text?text:"");
    terminal_output_kind[terminal_output_count]=0u;
    ++terminal_output_count;
}

static void terminal_push_command(const char *text) {
    if (terminal_output_count >= TERM_LINES) {
        for (uint32_t i=1u;i<TERM_LINES;i++) {
            (void)copy_text(terminal_output[i-1u],sizeof(terminal_output[i-1u]),terminal_output[i]);
            terminal_output_kind[i-1u]=terminal_output_kind[i];
        }
        terminal_output_count=TERM_LINES-1u;
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
        terminal_push("use sda | ls [name] | cd <dir> | pwd | cat <file>");
        terminal_push("power, shutdown, restart (asks for confirmation)"); return;
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
    if(terminal_equals(cmd,"ver")) { terminal_push("MintOS26 " MINTOS_VERSION " / MintDE " MINTDE_VERSION); return; }
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
static uint32_t driver_view;            /* 0 drivers, 1 PCI, 2 power */
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
    "Hardware & Drivers", "Network", "Disk Utility", "About MintOS", "About MintDE", "Launchpad", "Calculator"
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
        {900, 580}, {620, 470}, {900, 580}, {560, 380}, {560, 380}, {0, 0}, {240, 372}
    };
    *w = app < APP_COUNT ? sz[app][0] : 600;
    *h = app < APP_COUNT ? sz[app][1] : 400;
}
const char *app_window_title(uint32_t app) {
    if (app == APP_TERMINAL) return "root \xE2\x80\x94 mintos \xE2\x80\x94 bounded shell";
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
static uint32_t files_count(void) { return files_volume ? fat_entry_count : fs_entry_count; }
static const char *files_entry_name(uint32_t i) { return files_volume ? fat_entries[i].name : fs_entries[i].name; }
static uint64_t files_entry_size(uint32_t i) { return files_volume ? fat_entries[i].size : fs_entries[i].size; }
static int files_entry_is_dir(uint32_t i) { return files_volume && (fat_entries[i].flags & MINT_FAT16_DIRENT_DIRECTORY); }
static const char *files_kind(uint32_t i) {
    if (files_entry_is_dir(i)) return "Folder";
    const char *n = files_entry_name(i);
    size_t len = text_length(n);
    if (len > 4 && n[len - 4] == '.') {
        const char *e = n + len - 3;
        if (ci_contains(e, "TXT")) return "Plain Text";
        if (ci_contains(e, "PNG")) return "PNG Image";
        if (ci_contains(e, "ELF")) return "Executable";
        if (ci_contains(e, "RGB")) return "Raw Image";
        if (ci_contains(e, "DAT")) return "Data";
    }
    if (len > 3 && n[len - 2] == '.' && (n[len - 1] == 'C' || n[len - 1] == 'c')) return "C Source";
    if (len > 3 && n[len - 3] == '.' && ci_contains(n + len - 2, "md")) return "Markdown";
    return "Document";
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

static void files_paint(const mde_layout *l) {
    gfx_rect sb = l->sidebar;
    int32_t y = sb.y0 + 18;
    ui_sidebar_header(sb.x0 + 18, y, "Locations"); y += 10;
    ui_sidebar_item(R(sb.x0 + 10, y, W(sb) - 20, 28), "Session (RAM)", 0, 0, files_volume == 0, l->focused);
    ui_symbol_glyph(sb.x0 + 28, y + 14, SYM_DISK, OPAQUE(files_volume == 0 && l->focused ? 0xFFFFFFu : T.accent));
    for (uint32_t v = 0; v < fat_volume_list.count && v < MINT_FAT16_MAX_VOLUMES; ++v) {
        char label[40]; volume_label(v, label, sizeof(label));
        int32_t iy = y + 30 * (int32_t)(v + 1u);
        ui_sidebar_item(R(sb.x0 + 10, iy, W(sb) - 20, 28), label, 0, 0, files_volume == v + 1u, l->focused);
        ui_symbol_glyph(sb.x0 + 28, iy + 14, SYM_DISK, OPAQUE(files_volume == v + 1u && l->focused ? 0xFFFFFFu : T.text2));
    }
    /* toolbar */
    char title[48];
    if (files_path[0]) {
        size_t n = text_length(files_path);
        while (n && files_path[n - 1u] != '/') --n;
        copy_text(title, sizeof(title), files_path + n);
    } else if (files_volume) volume_label(files_volume - 1u, title, sizeof(title));
    else copy_text(title, sizeof(title), "Session (RAM)");
    int32_t tx = l->toolbar.x0 + 16;
    ui_symbol_glyph(tx + 8, l->toolbar.y0 + 26, SYM_CHEVRON_LEFT, OPAQUE(files_path[0] ? T.text : T.text3));
    ui_symbol_glyph(tx + 34, l->toolbar.y0 + 26, SYM_CHEVRON_RIGHT, OPAQUE(T.text3));
    ui_label(tx + 56, BL(R(0, l->toolbar.y0, 1, 52), FONT_HEADLINE), FONT_HEADLINE, title, T.text);
    ui_search_field(files_search_rect(l), files_search, files_search_focus);
    /* list */
    gfx_rect lr = files_list_rect(l);
    gfx_fill(S, lr.x0, lr.y0, W(lr), H(l->content), OPAQUE(T.content));
    int32_t name_x = lr.x0 + 16, size_r = lr.x1 - 170, kind_x = lr.x1 - 150;
    ui_label(name_x, lr.y0 + 17, FONT_CAPTION_BOLD, "Name", T.text2);
    ui_label_right(size_r, lr.y0 + 17, FONT_CAPTION_BOLD, "Size", T.text2);
    ui_label(kind_x, lr.y0 + 17, FONT_CAPTION_BOLD, "Kind", T.text2);
    gfx_hline(S, lr.x0, lr.y0 + 25, W(lr), T.separator);
    uint32_t idx[MINT_FAT16_LIST_MAX];
    uint32_t n = files_visible(idx, MINT_FAT16_LIST_MAX);
    int32_t rows = (H(lr) - 26) / 24;
    if (files_scroll > (int32_t)n - rows) files_scroll = (int32_t)n - rows;
    if (files_scroll < 0) files_scroll = 0;
    for (int32_t r = 0; r < rows && (uint32_t)(r + files_scroll) < n; ++r) {
        uint32_t i = idx[r + files_scroll];
        gfx_rect row = R(lr.x0, lr.y0 + 26 + r * 24, W(lr), 24);
        int sel = files_selected_entry == (int32_t)i;
        ui_list_row(R(row.x0 + 8, row.y0, W(row) - 16, 24), (uint32_t)r, sel, l->focused);
        if (sel) gfx_round_rect(S, row.x0 + 8, row.y0, W(row) - 16, 24, 5, OPAQUE(l->focused ? T.accent : T.selection_inactive));
        uint32_t fg = sel && l->focused ? 0xFFFFFFu : T.text;
        ui_symbol_glyph(name_x + 8, row.y0 + 12, files_entry_is_dir(i) ? SYM_FOLDER : SYM_DOC,
                        OPAQUE(sel && l->focused ? 0xFFFFFFu : (files_entry_is_dir(i) ? 0x4FA8F5u : T.text2)));
        gfx_text_fit(S, FONT_BODY, name_x + 24, BL(row, FONT_BODY), files_entry_name(i), size_r - name_x - 100, OPAQUE(fg));
        char sz[24];
        if (files_entry_is_dir(i)) copy_text(sz, sizeof(sz), "--"); else format_bytes(sz, sizeof(sz), files_entry_size(i));
        ui_label_right(size_r, BL(row, FONT_BODY), FONT_BODY, sz, sel && l->focused ? 0xFFFFFFu : T.text2);
        ui_label(kind_x, BL(row, FONT_BODY), FONT_BODY, files_kind(i), sel && l->focused ? 0xFFFFFFu : T.text2);
    }
    if (!n) ui_label_center(lr.x0 + W(lr) / 2, lr.y0 + 90, FONT_BODY, files_search[0] ? "No Results" : "This folder is empty", T.text3);
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
                char line[48]; size_t li = 0; int32_t lines = 0;
                for (size_t k = 0; k <= files_preview_length && lines < 11; ++k) {
                    char ch = k < files_preview_length ? files_preview[k] : '\n';
                    if (ch == '\r') continue;
                    if (ch == '\n' || li + 1u >= sizeof(line) || li >= 34u) {
                        line[li] = '\0';
                        ui_label(pv.x0 + 32, ty, FONT_CAPTION, line, T.text);
                        ty += 16; ++lines; li = 0;
                        if (ch != '\n' && ch) line[li++] = ch;
                    } else if (ch >= 32 && ch < 127) line[li++] = ch;
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
            ui_label_right(pv.x1 - 20, pv.y0 + 328, FONT_CAPTION, "Read only", T.text);
            ui_label(pv.x0 + 20, pv.y0 + 348, FONT_CAPTION, "Where", T.text2);
            ui_label_right(pv.x1 - 20, pv.y0 + 348, FONT_CAPTION, files_volume ? "FAT16 volume" : "Session (RAM)", T.text);
        }
    }
    /* path / status bar */
    gfx_rect st = R(l->content.x0, l->content.y1 - 26, W(l->content), 26);
    gfx_fill(S, st.x0, st.y0, W(st), 26, OPAQUE(T.window));
    gfx_hline(S, st.x0, st.y0, W(st), T.separator);
    char status[64]; format_u64(status, sizeof(status), n);
    str_append(status, sizeof(status), n == 1 ? " item" : " items");
    str_append(status, sizeof(status), files_volume ? ", read-only volume" : ", volatile session storage");
    ui_label_center(st.x0 + W(st) / 2, BL(st, FONT_CAPTION), FONT_CAPTION, status, T.text2);
}

static void files_choose_volume(uint32_t v) {
    files_volume = v; files_path[0] = '\0'; files_selected_entry = -1; files_preview_active = 0;
    files_scroll = 0; files_search[0] = '\0';
    if (v) refresh_fat_files(); else refresh_files();
}

static int files_click(const mde_layout *l, int32_t x, int32_t y, uint32_t clicks) {
    files_search_focus = in(files_search_rect(l), x, y);
    if (files_search_focus) return 1;
    gfx_rect sb = l->sidebar;
    if (in(sb, x, y)) {
        int32_t base = sb.y0 + 28;
        if (y >= base) {
            uint32_t item = (uint32_t)((y - base) / 30);
            if (item <= fat_volume_list.count && item <= MINT_FAT16_MAX_VOLUMES) files_choose_volume(item);
        }
        return 1;
    }
    if (in(R(l->toolbar.x0 + 12, l->toolbar.y0 + 12, 24, 28), x, y)) { files_go_up(); files_scroll = 0; return 1; }
    gfx_rect lr = files_list_rect(l);
    if (in(lr, x, y) && y >= lr.y0 + 26) {
        uint32_t idx[MINT_FAT16_LIST_MAX];
        uint32_t n = files_visible(idx, MINT_FAT16_LIST_MAX);
        int32_t r = (y - lr.y0 - 26) / 24 + files_scroll;
        if (r < 0 || (uint32_t)r >= n) { files_selected_entry = -1; files_preview_active = 0; return 1; }
        uint32_t i = idx[r];
        if (files_entry_is_dir(i)) {
            if (clicks >= 2) { files_select_entry(i); files_scroll = 0; files_search[0] = '\0'; }
            else { files_selected_entry = (int32_t)i; files_preview_active = 0; }
        } else files_select_entry(i);
        return 1;
    }
    return 0;
}

static int files_key(const MINT_DESKTOP_KEY_EVENT *k) {
    uint32_t ch = k->codepoint;
    if (files_search_focus) {
        size_t n = text_length(files_search);
        if (ch == 8u) { if (n) files_search[n - 1u] = '\0'; }
        else if (ch == 27u || ch == '\n') files_search_focus = 0;
        else if (ch >= 32u && ch < 127u && n + 1u < sizeof(files_search)) { files_search[n] = (char)ch; files_search[n + 1u] = '\0'; }
        files_scroll = 0;
        return 1;
    }
    uint32_t idx[MINT_FAT16_LIST_MAX];
    uint32_t n = files_visible(idx, MINT_FAT16_LIST_MAX);
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
        else files_select_entry(i);
        if (pos < files_scroll) files_scroll = pos;
        if (pos >= files_scroll + 12) files_scroll = pos - 11;
    }
    return 1;
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
    ui_sidebar_header(sb.x0 + 18, sb.y0 + 18, "Session");
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
    ui_label_center(c.x0 + W(c) / 2, c.y0 + 22, FONT_CAPTION, "Session (RAM) \xE2\x80\x94 cleared at restart", T.text3);
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
static int notes_key(const MINT_DESKTOP_KEY_EVENT *k) {
    load_notes();
    uint32_t ch = k->codepoint;
    if (ch == 8u) { if (note_length) note_buffer[--note_length] = '\0'; }
    else if ((ch == '\n' || (ch >= 32u && ch < 127u)) && note_length + 1u < sizeof(note_buffer)) {
        note_buffer[note_length++] = (char)ch; note_buffer[note_length] = '\0';
    } else return 0;
    save_notes();
    return 1;
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
    uint32_t first = terminal_output_count > (uint32_t)max_lines ? terminal_output_count - (uint32_t)max_lines : 0u;
    int32_t y = c.y0 + 8 + gfx_font_ascent(FONT_SMALL);
    for (uint32_t i = first; i < terminal_output_count; ++i) {
        if (terminal_output_kind[i]) {
            int32_t px = gfx_text_cells(S, FONT_SMALL, x, y, "root#mintos > ", TERM_CELL, OPAQUE(prompt));
            gfx_text_cells(S, FONT_SMALL, x + px, y, terminal_output[i], TERM_CELL, OPAQUE(fg));
        } else gfx_text_cells(S, FONT_SMALL, x, y, terminal_output[i], TERM_CELL, OPAQUE(fg));
        y += line_h;
    }
    int32_t px = gfx_text_cells(S, FONT_SMALL, x, y, "root#mintos > ", TERM_CELL, OPAQUE(prompt));
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
    if (ch == 12u || ((k->modifiers & MINT_KEYMOD_CTRL) && (ch == 'l' || ch == 'L'))) { terminal_output_count = 0; return 1; }
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
        format_u64(a, sizeof(a), input_info.keyboard_events);
        format_u64(bb, sizeof(bb), input_info.pointer_motion_events);
        format_u64(cc, sizeof(cc), input_info.pointer_button_events);
        format_u64(d, sizeof(d), input_info.dropped_events);
        const char *labels[6] = {"Keyboard", "Pointer", "Key events", "Pointer moves", "Button events", "Dropped events"};
        const char *values[6] = {input_info.keyboard_ready ? "PS/2, ready" : "Not detected",
                                 input_info.mouse_ready ? "PS/2, ready" : "Not detected", a, bb, cc, d};
        settings_rows(&b, labels, values, 6);
        settings_note(&b, "Command shortcuts use the Windows key. Pointer acceleration is built in.");
        settings_note(&b, "USB keyboards and mice need firmware legacy emulation until the USB driver arrives.");
        break;
    }
    case SET_STORAGE: {
        refresh_storage_data();
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
            ui_progress(R(g.x0 + 14, g.y0 + 46, W(g) - 28, 10), mounted ? 1000u : 0u, mounted ? T.accent : T.text3);
            ui_label(g.x0 + 14, g.y0 + 78, FONT_CAPTION, mounted ? "FAT16 \xE2\x80\x94 mounted read-only" : "Not mounted (blank or unsupported)", T.text2);
            ui_label_right(g.x1 - 14, g.y0 + 78, FONT_CAPTION, "Writes disabled", T.text2);
            b.y0 = g.y1 + 14;
        }
        if (!storage_snapshot.device_count) settings_note(&b, "No SATA disk was detected by the AHCI driver.");
        settings_note(&b, "Session (RAM) holds Notes and settings until the computer restarts.");
        break;
    }
    case SET_NETWORK: {
        (void)mint_e1000_get_status(&terminal_e1000_info);
        const char *state = terminal_e1000_info.state == MINT_E1000_ACTIVE ? "Driver active (polling)" :
                            terminal_e1000_info.state == MINT_E1000_FOUND ? "Detected, driver idle" :
                            terminal_e1000_info.state == MINT_E1000_FAILED ? "Initialization failed" : "Not detected";
        static const char hx[] = "0123456789ABCDEF";
        size_t m = 0;
        for (int i = 0; i < 6; ++i) { a[m++] = hx[terminal_e1000_info.mac[i] >> 4]; a[m++] = hx[terminal_e1000_info.mac[i] & 15u]; if (i < 5) a[m++] = ':'; }
        a[m] = '\0';
        const char *labels[4] = {"Ethernet (Intel 82540EM)", "MAC address", "Internet", "Wi-Fi"};
        const char *values[4] = {state, terminal_e1000_info.state == MINT_E1000_ACTIVE ? a : "\xE2\x80\x94", "Not available (no TCP/IP stack)", "No supported hardware"};
        settings_rows(&b, labels, values, 4);
        settings_note(&b, "Open the Network app to start the driver or run the internal loopback test.");
        break;
    }
    case SET_PRIVACY: {
        const char *labels[5] = {"Disk writes", "Network", "Memory protection", "Processes", "Firmware"};
        const char *values[5] = {"Blocked (read-only volumes)", "No outgoing connections", "NX, W^X and guard pages",
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
        ui_sidebar_item(ir, label, 0, 0, i == sel_item, l->focused);
        uint32_t sym = i < storage_snapshot.device_count ? SYM_DISK : i == storage_snapshot.device_count ? SYM_GEAR :
                       i == storage_snapshot.device_count + 1u ? SYM_DOC : SYM_WARNING;
        ui_symbol_glyph(ir.x0 + 18, ir.y0 + 14, sym, OPAQUE(i == sel_item && l->focused ? 0xFFFFFFu : T.text2));
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
    static const char *const items[3] = {"Drivers", "PCI Devices", "Power (ACPI)"};
    static const uint32_t syms[3] = {SYM_GEAR, SYM_INFO, SYM_WARNING};
    ui_sidebar_header(l->sidebar.x0 + 18, l->sidebar.y0 + 18, "Hardware");
    for (uint32_t i = 0; i < 3u; ++i) {
        gfx_rect ir = drivers_item_rect(l, i);
        ui_sidebar_item(ir, items[i], 0, 0, driver_view == i, l->focused);
        ui_symbol_glyph(ir.x0 + 18, ir.y0 + 14, syms[i], OPAQUE(driver_view == i && l->focused ? 0xFFFFFFu : T.text2));
    }
    toolbar_title(l, items[driver_view < 3u ? driver_view : 0u], 0);
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
    for (uint32_t i = 0; i < 3u; ++i) if (in(drivers_item_rect(l, i), x, y)) { driver_view = i; return 1; }
    return 0;
}

/* ------------------------------------------------------------ Network */
static void network_paint(const mde_layout *l) {
    gfx_rect c = l->content;
    gfx_fill(S, c.x0, c.y0, W(c), H(c), OPAQUE(T.group_bg));
    (void)mint_e1000_get_status(&terminal_e1000_info);
    gfx_image(S, c.x0 + 24, c.y0 + 20, 56, 56, mint_icon_network_128, 128, 128, 255u);
    ui_label(c.x0 + 96, c.y0 + 44, FONT_HEADLINE, "Ethernet", T.text);
    const char *state = terminal_e1000_info.state == MINT_E1000_ACTIVE ? "Driver active \xE2\x80\x94 no TCP/IP" :
                        terminal_e1000_info.state == MINT_E1000_FOUND ? "Intel 82540EM detected, driver idle" :
                        terminal_e1000_info.state == MINT_E1000_FAILED ? "Initialization failed (see COM1)" : "No supported adapter";
    uint32_t dot = terminal_e1000_info.state == MINT_E1000_ACTIVE ? 0x34C759u : terminal_e1000_info.state == MINT_E1000_FAILED ? 0xFF3B30u : 0xFF9500u;
    gfx_circle(S, c.x0 + 101, c.y0 + 62, 4, OPAQUE(dot));
    ui_label(c.x0 + 112, c.y0 + 66, FONT_BODY, state, T.text2);
    char v[4][40];
    format_u64(v[0], 40, terminal_e1000_info.tx_completed); str_append(v[0], 40, " / ");
    format_u64(v[1], 40, terminal_e1000_info.tx_submitted); str_append(v[0], 40, v[1]);
    format_u64(v[1], 40, terminal_e1000_info.rx_received);
    format_u64(v[2], 40, terminal_e1000_info.loopback_passed); str_append(v[2], 40, " passed, ");
    format_u64(v[3], 40, terminal_e1000_info.loopback_failed); str_append(v[2], 40, v[3]); str_append(v[2], 40, " failed");
    gfx_rect g = R(c.x0 + 24, c.y0 + 96, W(c) - 48, 4 * 34);
    ui_group(g);
    const char *labels[4] = {"Frames sent (completed / submitted)", "Frames received", "Loopback tests", "Internet"};
    const char *values[4] = {v[0], v[1], v[2], "Not available"};
    for (uint32_t i = 0; i < 4u; ++i) ui_group_row(R(g.x0, g.y0 + (int32_t)i * 34, W(g), 34), i, 4u, labels[i], values[i]);
    int active = terminal_e1000_info.state == MINT_E1000_ACTIVE;
    ui_button(R(c.x0 + 24, g.y1 + 18, 120, 28), "Start Driver", !active && terminal_e1000_info.state == MINT_E1000_FOUND,
              terminal_e1000_info.state == MINT_E1000_FOUND);
    ui_button(R(c.x0 + 156, g.y1 + 18, 140, 28), "Loopback Test", 0, active);
    if (network_status[0]) ui_label(c.x0 + 310, g.y1 + 37, FONT_BODY, network_status, T.text2);
    ui_label(c.x0 + 28, c.y1 - 34, FONT_CAPTION, "The driver is opt-in. The loopback test never sends packets onto the network.", T.text2);
    ui_label(c.x0 + 28, c.y1 - 16, FONT_CAPTION, "No DHCP, DNS or sockets yet.", T.text2);
}
static int network_click(const mde_layout *l, int32_t x, int32_t y) {
    gfx_rect c = l->content;
    int32_t by = c.y0 + 96 + 4 * 34 + 18;
    if (in(R(c.x0 + 24, by, 120, 28), x, y)) {
        int64_t rc = mint_e1000_start();
        copy_text(network_status, sizeof(network_status), rc == 0 ? "Driver started; DMA rings active." :
                  rc == -MINT_ENODEV ? "No adapter detected." : "Start failed; see COM1 log.");
        return 1;
    }
    if (in(R(c.x0 + 156, by, 140, 28), x, y)) {
        int64_t rc = mint_e1000_run_loopback_test();
        copy_text(network_status, sizeof(network_status), rc == 0 ? "Loopback passed." : "Loopback failed; details in COM1 / Terminal net status.");
        return 1;
    }
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
    int32_t w = 460, h = 214;
    return R(l->frame.x0 + (W(l->frame) - w) / 2, l->content.y0 - 1, w, h);
}
/* Only offer Erase for what the kernel formatter will accept: a registered
   disk with no MBR signature that is not mounted. The kernel re-checks
   everything (zeroed sectors, size, serial, confirmation phrase). */
static int disk_is_erasable(uint32_t i) {
    if (i >= storage_snapshot.device_count || i >= MINT_STORAGE_MAX_DEVICES) return 0;
    MINT_STORAGE_DEVICE_INFO *d = &storage_snapshot.devices[i];
    if (!d->block_registered || d->mbr_signature_ok) return 0;
    for (uint32_t v = 0; v < fat_volume_list.count; ++v) if (fat_volume_list.block_indices[v] == d->block_index) return 0;
    return 1;
}
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
        if (sel) gfx_round_rect(S, ir.x0, ir.y0, W(ir), H(ir), 6, l->focused ? OPAQUE(T.accent) : (T.dark ? ARGB(0x30, 0xFF, 0xFF, 0xFF) : ARGB(0x1C, 0, 0, 0)));
        uint32_t fg = sel && l->focused ? 0xFFFFFFu : T.text;
        gfx_image(S, ir.x0 + 6, ir.y0 + 6, 28, 28, mint_icon_disk_utility_64, 64, 64, 255u);
        gfx_text_fit(S, FONT_BODY_MEDIUM, ir.x0 + 42, ir.y0 + 17, d->model[0] ? d->model : d->name, W(ir) - 50, OPAQUE(fg));
        char cap[32]; format_bytes(cap, sizeof(cap), d->capacity_bytes);
        char sub[48]; copy_text(sub, sizeof(sub), d->name); str_append(sub, sizeof(sub), " \xC2\xB7 "); str_append(sub, sizeof(sub), cap);
        ui_label(ir.x0 + 42, ir.y0 + 33, FONT_CAPTION, sub, sel && l->focused ? 0xE8F0FFu : T.text2);
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
    str_append(sub, sizeof(sub), d->mbr_signature_ok ? "Master Boot Record" : "Uninitialized");
    ui_label(c.x0 + 102, c.y0 + 70, FONT_BODY, sub, T.text2);
    int mounted = 0;
    for (uint32_t v = 0; v < fat_volume_list.count; ++v) if (d->block_registered && fat_volume_list.block_indices[v] == d->block_index) mounted = 1;
    gfx_rect bar = R(c.x0 + 24, c.y0 + 100, W(c) - 48, 22);
    ui_progress(bar, 1000u, mounted ? T.accent : (T.dark ? 0x48484Au : 0xD1D1D6u));
    ui_label(bar.x0 + 4, bar.y1 + 20, FONT_CAPTION, mounted ? "MINTOS26 (FAT16)" : "Free / unformatted", T.text2);
    ui_label_right(bar.x1 - 4, bar.y1 + 20, FONT_CAPTION, cap, T.text2);
    char ser[48]; copy_text(ser, sizeof(ser), d->serial[0] ? d->serial : "unavailable");
    char sectors[32]; format_u64(sectors, sizeof(sectors), d->sector_count);
    const char *labels[6] = {"Location", "Connection", "Device", "Partition Map", "Serial Number", "Sectors"};
    const char *values[6] = {"Internal", "SATA (AHCI)", d->name, d->mbr_signature_ok ? "Master Boot Record" : "None (blank)", ser, sectors};
    gfx_rect g = R(c.x0 + 24, bar.y1 + 36, W(c) - 48, 3 * 34);
    ui_group(g);
    int32_t half = W(g) / 2;
    for (uint32_t i = 0; i < 6u; ++i) {
        gfx_rect cell = R(g.x0 + (int32_t)(i / 3u) * half, g.y0 + (int32_t)(i % 3u) * 34, half, 34);
        ui_group_row(cell, i % 3u, 3u, labels[i], values[i]);
    }
    gfx_vline(S, g.x0 + half, g.y0 + 6, H(g) - 12, T.separator);
    ui_label(c.x0 + 28, g.y1 + 26, FONT_CAPTION,
             disk_result[0] ? disk_result : disk_is_erasable(disk_selected_device) ?
             "This disk looks blank. Erase creates one FAT16 volume (32 MB \xE2\x80\x93 2 GB disks only)." :
             "This disk has a partition table or is in use, so it cannot be erased. Other writes stay disabled.",
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
        ui_label(sh.x0 + 84, sh.y0 + 58, FONT_CAPTION, "Erasing destroys all data on this disk. It then gets an MBR", T.text2);
        ui_label(sh.x0 + 84, sh.y0 + 74, FONT_CAPTION, "with one FAT16 volume that MintOS mounts read-only.", T.text2);
        char phrase[32]; disk_expected_phrase(phrase, sizeof(phrase));
        char prompt[64]; copy_text(prompt, sizeof(prompt), "Type \xE2\x80\x9C"); str_append(prompt, sizeof(prompt), phrase);
        str_append(prompt, sizeof(prompt), "\xE2\x80\x9D to confirm:");
        ui_label(sh.x0 + 84, sh.y0 + 104, FONT_BODY, prompt, T.text);
        ui_text_field(R(sh.x0 + 84, sh.y0 + 114, W(sh) - 104, 26), disk_confirmation, phrase, 1);
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
        int64_t rc = mint_storage_format_blank(&request);
        if (rc == 0) copy_text(disk_result, sizeof(disk_result), "Success: disk erased and its FAT16 volume validated read-only.");
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
    "26.5.010.0  MintDE 2: macOS-style desktop, shared canvas, RTC clock",
    "26.5.009.0  Real-hardware display, mouse, ACPI power, RAID-mode SATA",
    "26.5.008.0  VirtualBox E1000 RX loopback and descriptor diagnostics",
    "26.5.007.0  E1000 TX timeout isolation",
    "26.5.006.0  E1000 loopback diagnostics and terminal prompt",
    "26.5.005.0  E1000 driver foundation",
    "26.5.004.0  Driver foundation release",
    "26.5.003.0  PNG assets and VFS telemetry"
};
static const char *const de_versions[8] = {
    "MintDE 2.0.0  Windows, Dock, Launchpad, Spotlight, Control Center",
    "MintDE 1.0.8  Shut Down / Restart and precise pointer",
    "MintDE 1.0.7  VirtualBox loopback and RX diagnostics",
    "MintDE 1.0.6  E1000 loopback failure diagnostics",
    "MintDE 1.0.5  E1000 loopback diagnostics and terminal prompt",
    "MintDE 1.0.4  E1000 driver UI",
    "MintDE 1.0.3  Dock, menus and window polish",
    "MintDE 1.0.2  PNG icons and glass"
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
    terminal_input_length = 0u; terminal_input[0] = '\0'; terminal_output_count = 0u;
    terminal_push("MintOS26 Terminal \xE2\x80\x94 type help for commands");
    refresh_storage_data();
    refresh_files();
    calc_format(0);
}
void app_opened(uint32_t app) {
    if (app == APP_FILES) { refresh_storage_data(); if (files_volume) refresh_fat_files(); else refresh_files(); }
    if (app == APP_STORAGE || app == APP_DISKS || app == APP_SETTINGS || app == APP_MONITOR) refresh_storage_data();
    if (app == APP_DRIVERS || app == APP_MONITOR) refresh_driver_catalog();
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
    default: return 0;
    }
}
int app_scroll(uint32_t app, const mde_layout *l, int32_t x, int32_t y, int32_t delta) {
    (void)l; (void)x; (void)y;
    if (app == APP_FILES) { files_scroll += delta * 3; if (files_scroll < 0) files_scroll = 0; return 1; }
    return 0;
}
int app_tick(uint32_t app) {
    ++ui_ticks;
    /* Blink the text caret at ~1 Hz in Terminal and Notes. */
    return (app == APP_TERMINAL || app == APP_NOTES) && (ui_ticks % 50u) == 0u;
}
uint32_t app_cursor(uint32_t app, const mde_layout *l, int32_t x, int32_t y) {
    if ((app == APP_TERMINAL || app == APP_NOTES) && in(l->content, x, y)) return MINT_CURSOR_SHAPE_IBEAM;
    if (app == APP_FILES && in(files_search_rect(l), x, y)) return MINT_CURSOR_SHAPE_IBEAM;
    return MINT_CURSOR_SHAPE_ARROW;
}
