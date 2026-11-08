#include <stdint.h>
#include <stddef.h>
#include "../include/fat16.h"
#include "../include/block.h"
#include "../include/storage_diag.h"
#include "../include/log.h"
#include "../include/uapi/mintos/syscall.h"

#define FAT16_PART_TYPE_CHS 0x04u
#define FAT16_PART_TYPE_LBA 0x06u
#define FAT16_PART_TYPE_LBA_LARGE 0x0Eu
#define FAT16_ATTR_DIRECTORY 0x10u
#define FAT16_ATTR_VOLUME_ID 0x08u
#define FAT16_ATTR_LONG_NAME 0x0Fu
#define FAT16_EOC_MIN 0xFFF8u
#define FAT16_RESERVED_MIN 0xFFF0u
#define FAT16_PATH_MAX 96u
#define FAT16_COMPONENT_MAX 32u
#define FAT16_MAX_PATH_COMPONENTS 8u
#define FAT16_MAX_WALK 65525u

typedef struct fat16_volume {
    uint32_t block_index;
    uint32_t partition_sectors;
    uint32_t total_sectors;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_entries;
    uint32_t root_sectors;
    uint32_t fat_start;
    uint32_t root_start;
    uint32_t data_start;
    uint32_t cluster_count;
    uint64_t partition_lba;
    uint32_t mounted;
} FAT16_VOLUME;

typedef struct fat16_lfn_state {
    uint16_t units[260];
    uint8_t active;
    uint8_t checksum;
} FAT16_LFN_STATE;

typedef struct fat16_entry_ref {
    uint64_t sector_lba;
    uint32_t byte_offset;
    uint8_t entry[32];
} FAT16_ENTRY_REF;

static FAT16_VOLUME active_volume;
static FAT16_VOLUME mounted_volumes[MINT_FAT16_MAX_VOLUMES];
static uint32_t mounted_volume_count;
static uint32_t active_volume_ordinal = UINT32_MAX;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void bytes_zero(void *p, size_t n) { uint8_t *b=(uint8_t *)p; for(size_t i=0;i<n;i++) b[i]=0; }
static char upper_ascii(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }
static int allowed_sfn(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '$' || c == '~' || c == '!' ||
           c == '#' || c == '%' || c == '&' || c == '{' || c == '}' ||
           c == '(' || c == ')' || c == '@' || c == '`' || c == '\'';
}
static int component_to_sfn(const char *text, size_t length, uint8_t out[11]) {
    if (!text || !length || length >= FAT16_COMPONENT_MAX || !out) return 0;
    size_t dot = length;
    for (size_t i=0;i<length;i++) {
        if (text[i]=='.') { if (dot != length) return 0; dot=i; }
    }
    size_t base_len=dot, ext_len=dot==length ? 0u : length-dot-1u;
    if (!base_len || base_len>8u || ext_len>3u || (dot!=length && !ext_len)) return 0;
    for (size_t i=0;i<11;i++) out[i]=' ';
    for (size_t i=0;i<base_len;i++) { char c=upper_ascii(text[i]); if (!allowed_sfn(c)) return 0; out[i]=(uint8_t)c; }
    for (size_t i=0;i<ext_len;i++) { char c=upper_ascii(text[dot+1u+i]); if (!allowed_sfn(c)) return 0; out[8u+i]=(uint8_t)c; }
    return 1;
}
static void lfn_reset(FAT16_LFN_STATE *state) {
    state->active=0u; state->checksum=0u;
    for(size_t i=0;i<260u;i++) state->units[i]=0xFFFFu;
}
static uint8_t sfn_checksum(const uint8_t name[11]) {
    uint8_t sum=0;
    for(uint32_t i=0;i<11u;i++) sum=(uint8_t)(((sum&1u)<<7)+(sum>>1)+name[i]);
    return sum;
}
static void lfn_consume(FAT16_LFN_STATE *state, const uint8_t entry[32]) {
    uint8_t ordinal=(uint8_t)(entry[0]&0x1Fu);
    if (entry[0]&0x40u) lfn_reset(state);
    if (!state->active && !(entry[0]&0x40u)) return;
    if (!ordinal || ordinal>20u || entry[11]!=FAT16_ATTR_LONG_NAME) { lfn_reset(state); return; }
    if (entry[0]&0x40u) { state->active=1u; state->checksum=entry[13]; }
    else if (!state->active || state->checksum!=entry[13]) { lfn_reset(state); return; }
    static const uint8_t positions[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    uint32_t begin=(uint32_t)(ordinal-1u)*13u;
    for(uint32_t i=0;i<13u;i++) state->units[begin+i]=rd16(&entry[positions[i]]);
}
static int lfn_matches(const FAT16_LFN_STATE *state, const uint8_t short_name[11],
                       const char *wanted, size_t wanted_len) {
    if (!state || !state->active || state->checksum!=sfn_checksum(short_name) || !wanted) return 0;
    size_t length=0;
    while(length<260u && state->units[length]!=0u && state->units[length]!=0xFFFFu) length++;
    if(length!=wanted_len) return 0;
    for(size_t i=0;i<length;i++) {
        if(state->units[i]>0x7Fu) return 0; /* ASCII subset until full Unicode normalization exists. */
        if(upper_ascii((char)state->units[i])!=upper_ascii(wanted[i])) return 0;
    }
    return 1;
}
static int read_volume_sector(const FAT16_VOLUME *v, uint32_t relative, uint8_t out[512]) {
    if (!v || !out || relative >= v->total_sectors || v->partition_lba > UINT64_MAX-relative) return 0;
    return mint_block_read(v->block_index, v->partition_lba + relative, out);
}
static int fat16_valid_bpb(const uint8_t boot[512], const MINT_BLOCK_DEVICE *dev,
                           uint64_t partition_lba, uint32_t partition_sectors,
                           FAT16_VOLUME *out) {
    if (!boot || !dev || !out || boot[510]!=0x55u || boot[511]!=0xAAu) return 0;
    uint32_t bps=rd16(&boot[11]);
    uint32_t spc=boot[13], reserved=rd16(&boot[14]), fats=boot[16];
    uint32_t roots=rd16(&boot[17]);
    uint32_t total=rd16(&boot[19]); if (!total) total=rd32(&boot[32]);
    uint32_t fatsz=rd16(&boot[22]);
    if (bps!=512u || !spc || spc>128u || (spc & (spc-1u)) || !reserved ||
        !fats || fats>2u || !roots || !fatsz || !total || total>partition_sectors ||
        total>dev->sector_count || partition_lba>dev->sector_count-total) return 0;
    if (boot[54]!='F' || boot[55]!='A' || boot[56]!='T' || boot[57]!='1' ||
        boot[58]!='6') return 0;
    uint64_t root_bytes=(uint64_t)roots*32u;
    uint32_t root_secs=(uint32_t)((root_bytes+511u)/512u);
    uint64_t root_start=(uint64_t)reserved+(uint64_t)fats*fatsz;
    uint64_t data_start=root_start+root_secs;
    if (data_start>=total || root_start>UINT32_MAX || data_start>UINT32_MAX) return 0;
    uint32_t clusters=(uint32_t)((total-data_start)/spc);
    /* A FAT32 volume can carry a "FAT16" type label; cluster count remains authoritative. */
    if (clusters<4085u || clusters>=65525u) return 0;
    uint64_t fat_entries=(uint64_t)fatsz*512u/2u;
    if (fat_entries < (uint64_t)clusters+2u) return 0;
    out->block_index=0; out->partition_lba=partition_lba;
    out->partition_sectors=partition_sectors; out->total_sectors=total;
    out->sectors_per_cluster=spc; out->reserved_sectors=reserved; out->fat_count=fats;
    out->sectors_per_fat=fatsz; out->root_entries=roots; out->root_sectors=root_secs;
    out->fat_start=reserved; out->root_start=(uint32_t)root_start; out->data_start=(uint32_t)data_start;
    out->cluster_count=clusters; out->mounted=1u;
    return 1;
}
static int get_next_cluster(const FAT16_VOLUME *v, uint16_t cluster, uint16_t *next) {
    if (!v || !next || cluster<2u || (uint32_t)cluster>=v->cluster_count+2u) return 0;
    uint32_t byte_offset=(uint32_t)cluster*2u;
    uint32_t sector=v->fat_start+byte_offset/512u;
    uint8_t buf[512];
    if (!read_volume_sector(v, sector, buf)) return 0;
    *next=rd16(&buf[byte_offset%512u]);
    return 1;
}
static int cluster_to_sector(const FAT16_VOLUME *v, uint16_t cluster, uint32_t *lba_out) {
    if (!v || !lba_out || cluster<2u || (uint32_t)cluster>=v->cluster_count+2u) return 0;
    uint64_t lba=(uint64_t)v->data_start+(uint64_t)(cluster-2u)*v->sectors_per_cluster;
    if (lba+v->sectors_per_cluster>v->total_sectors || lba>UINT32_MAX) return 0;
    *lba_out=(uint32_t)lba; return 1;
}
static int entry_matches(const uint8_t *entry, const uint8_t short_name[11], int have_sfn,
                         const char *component, size_t component_len, FAT16_LFN_STATE *lfn) {
    if (entry[0]==0xE5u) { lfn_reset(lfn); return 0; }
    if (entry[11]==FAT16_ATTR_LONG_NAME) { lfn_consume(lfn,entry); return 0; }
    if (entry[11]&FAT16_ATTR_VOLUME_ID) { lfn_reset(lfn); return 0; }
    int same=0;
    if (have_sfn) {
        same=1; for(uint32_t j=0;j<11u;j++) if(entry[j]!=short_name[j]) { same=0; break; }
    }
    if (!same) same=lfn_matches(lfn,entry,component,component_len);
    lfn_reset(lfn);
    return same;
}
static int find_in_root(const FAT16_VOLUME *v, const uint8_t short_name[11], int have_sfn,
                        const char *component, size_t component_len, FAT16_ENTRY_REF *found) {
    uint8_t sector[512];
    uint32_t entries_seen=0;
    FAT16_LFN_STATE lfn; lfn_reset(&lfn);
    for (uint32_t s=0;s<v->root_sectors && entries_seen<v->root_entries;s++) {
        if (!read_volume_sector(v,v->root_start+s,sector)) return -1;
        for (uint32_t off=0;off<512u && entries_seen<v->root_entries;off+=32u,entries_seen++) {
            uint8_t *e=&sector[off];
            if (e[0]==0u) return 0;
            if (entry_matches(e,short_name,have_sfn,component,component_len,&lfn)) {
                found->sector_lba=v->root_start+s; found->byte_offset=off;
                for(uint32_t j=0;j<32u;j++) found->entry[j]=e[j]; return 1;
            }
        }
    }
    return 0;
}
static int find_in_directory(const FAT16_VOLUME *v, uint16_t first_cluster,
                             const uint8_t short_name[11], int have_sfn,
                             const char *component, size_t component_len,
                             FAT16_ENTRY_REF *found) {
    uint16_t cluster=first_cluster;
    uint32_t walked=0;
    uint8_t sector[512];
    FAT16_LFN_STATE lfn; lfn_reset(&lfn);
    while (cluster>=2u && (uint32_t)cluster<v->cluster_count+2u && walked++<v->cluster_count) {
        uint32_t first_lba=0; if (!cluster_to_sector(v,cluster,&first_lba)) return -1;
        for (uint32_t s=0;s<v->sectors_per_cluster;s++) {
            if (!read_volume_sector(v,first_lba+s,sector)) return -1;
            for (uint32_t off=0;off<512u;off+=32u) {
                uint8_t *e=&sector[off];
                if (e[0]==0u) return 0;
                if (entry_matches(e,short_name,have_sfn,component,component_len,&lfn)) {
                    found->sector_lba=first_lba+s; found->byte_offset=off;
                    for(uint32_t j=0;j<32u;j++) found->entry[j]=e[j]; return 1;
                }
            }
        }
        uint16_t next=0; if (!get_next_cluster(v,cluster,&next)) return -1;
        if (next>=FAT16_EOC_MIN) return 0;
        if (next<2u || next>=FAT16_RESERVED_MIN || (uint32_t)next>=v->cluster_count+2u) return -1;
        cluster=next;
    }
    return -1;
}
static int lookup_path(const FAT16_VOLUME *v, const char *path, FAT16_ENTRY_REF *ref) {
    if (!v || !v->mounted || !path || !ref) return -MINT_EINVAL;
    size_t path_len=0; while(path_len<FAT16_PATH_MAX && path[path_len]) path_len++;
    if (!path_len || path_len>=FAT16_PATH_MAX) return -MINT_EINVAL;
    size_t pos=0; uint16_t current_dir=0; int in_root=1;
    uint32_t component_count=0;
    while (pos<path_len) {
        while (pos<path_len && (path[pos]=='/' || path[pos]=='\\')) pos++;
        if (pos>=path_len) break;
        size_t begin=pos; while(pos<path_len && path[pos]!='/' && path[pos]!='\\') pos++;
        size_t len=pos-begin; if (!len) continue;
        if ((len==1u && path[begin]=='.') || (len==2u && path[begin]=='.' && path[begin+1u]=='.')) return -MINT_EINVAL;
        if (++component_count>FAT16_MAX_PATH_COMPONENTS) return -MINT_EINVAL;
        if (len>=FAT16_COMPONENT_MAX) return -MINT_EINVAL;
        uint8_t sfn[11]; int have_sfn=component_to_sfn(path+begin,len,sfn);
        for (size_t j=0;j<len;j++) if ((uint8_t)path[begin+j] < 0x20u) return -MINT_EINVAL;
        int rc=in_root ? find_in_root(v,sfn,have_sfn,path+begin,len,ref)
                       : find_in_directory(v,current_dir,sfn,have_sfn,path+begin,len,ref);
        if (rc<0) return -MINT_EIO;
        if (!rc) return -MINT_ENOENT;
        size_t look=pos; while(look<path_len && (path[look]=='/' || path[look]=='\\')) look++;
        int more=look<path_len;
        if (more) {
            if (!(ref->entry[11]&FAT16_ATTR_DIRECTORY)) return -MINT_ENOENT;
            current_dir=rd16(&ref->entry[26]);
            if (current_dir<2u || (uint32_t)current_dir>=v->cluster_count+2u) return -MINT_EIO;
            in_root=0;
        } else {
            if (ref->entry[11]&FAT16_ATTR_DIRECTORY) return -MINT_EINVAL;
            return 0;
        }
    }
    return -MINT_EINVAL;
}
static int install_mounted_volume(const FAT16_VOLUME *candidate) {
    if (!candidate || !candidate->mounted) return 0;
    uint32_t ordinal = UINT32_MAX;
    for (uint32_t i=0; i<mounted_volume_count; ++i) {
        if (mounted_volumes[i].mounted &&
            mounted_volumes[i].block_index == candidate->block_index) {
            ordinal = i;
            break;
        }
    }
    if (ordinal == UINT32_MAX) {
        if (mounted_volume_count >= MINT_FAT16_MAX_VOLUMES) return 0;
        ordinal = mounted_volume_count++;
    }
    mounted_volumes[ordinal] = *candidate;
    active_volume = *candidate;
    active_volume_ordinal = ordinal;
    return 1;
}

int mint_fat16_try_mount(uint32_t block_device_index) {
    const MINT_BLOCK_DEVICE *dev=mint_block_get(block_device_index);
    if (!dev || dev->sector_size!=512u || dev->sector_count<4096u) return 0;
    uint8_t mbr[512]; if (!mint_block_read(block_device_index,0,mbr)) return 0;
    if (mbr[510]!=0x55u || mbr[511]!=0xAAu) return 0;
    for (uint32_t i=0;i<4u;i++) {
        const uint8_t *e=&mbr[446u+i*16u];
        uint8_t type=e[4];
        if (type!=FAT16_PART_TYPE_CHS && type!=FAT16_PART_TYPE_LBA && type!=FAT16_PART_TYPE_LBA_LARGE) continue;
        uint32_t start=rd32(&e[8]), count=rd32(&e[12]);
        if (!count || start>=dev->sector_count || count>dev->sector_count-start) continue;
        uint8_t boot[512]; if (!mint_block_read(block_device_index,start,boot)) continue;
        FAT16_VOLUME candidate; bytes_zero(&candidate,sizeof(candidate));
        if (!fat16_valid_bpb(boot,dev,start,count,&candidate)) continue;
        candidate.block_index=block_device_index;
        /* A standard FAT16 volume is browseable even without MintOS seed files. */
        return install_mounted_volume(&candidate);
    }
    /* Also accept a superfloppy FAT16 volume (BPB directly at LBA 0). */
    FAT16_VOLUME candidate; bytes_zero(&candidate,sizeof(candidate));
    if (fat16_valid_bpb(mbr,dev,0u,(uint32_t)(dev->sector_count>UINT32_MAX?UINT32_MAX:dev->sector_count),&candidate)) {
        candidate.block_index=block_device_index;
        return install_mounted_volume(&candidate);
    }
    return 0;
}
void mint_fat16_unmount(void) {
    if (active_volume_ordinal < mounted_volume_count &&
        mounted_volumes[active_volume_ordinal].mounted &&
        mounted_volumes[active_volume_ordinal].block_index == active_volume.block_index) {
        uint32_t removed = active_volume_ordinal;
        for (uint32_t i=removed; i+1u<mounted_volume_count; ++i)
            mounted_volumes[i] = mounted_volumes[i+1u];
        if (mounted_volume_count) --mounted_volume_count;
        bytes_zero(&mounted_volumes[mounted_volume_count], sizeof(mounted_volumes[0]));
        if (!mounted_volume_count) {
            active_volume_ordinal = UINT32_MAX;
            bytes_zero(&active_volume,sizeof(active_volume));
        } else {
            if (removed >= mounted_volume_count) removed = mounted_volume_count - 1u;
            active_volume_ordinal = removed;
            active_volume = mounted_volumes[removed];
        }
    } else {
        bytes_zero(&active_volume,sizeof(active_volume));
        active_volume_ordinal = UINT32_MAX;
    }
}
int mint_fat16_is_mounted(void) { return active_volume.mounted!=0u; }
uint32_t mint_fat16_volume_count(void) { return mounted_volume_count; }
int mint_fat16_volume_select(uint32_t ordinal) {
    if (ordinal >= mounted_volume_count || !mounted_volumes[ordinal].mounted) return 0;
    active_volume_ordinal = ordinal;
    active_volume = mounted_volumes[ordinal];
    return 1;
}
int mint_fat16_volume_list(MINT_FAT16_VOLUME_LIST *out) {
    if (!out) return 0;
    bytes_zero(out, sizeof(*out));
    out->count = mounted_volume_count;
    out->selected_ordinal = active_volume_ordinal;
    for (uint32_t i=0; i<mounted_volume_count && i<MINT_FAT16_MAX_VOLUMES; ++i)
        out->block_indices[i] = mounted_volumes[i].block_index;
    return 1;
}
int mint_fat16_is_device_mounted(uint32_t block_device_index) {
    for (uint32_t i=0; i<mounted_volume_count; ++i)
        if (mounted_volumes[i].mounted && mounted_volumes[i].block_index==block_device_index) return 1;
    return 0;
}
uint32_t mint_fat16_block_device_index(void) { return active_volume.mounted ? active_volume.block_index : UINT32_MAX; }
uint64_t mint_fat16_partition_lba(void) { return active_volume.mounted ? active_volume.partition_lba : 0; }
uint32_t mint_fat16_cluster_size_bytes(void) { return active_volume.mounted ? active_volume.sectors_per_cluster*512u : 0; }

int64_t mint_fat16_read_file(const char *path, void *buffer, size_t capacity) {
    if (!active_volume.mounted) return -MINT_ENODEV;
    if (!buffer && capacity) return -MINT_EINVAL;
    FAT16_ENTRY_REF ref; int rc=lookup_path(&active_volume,path,&ref); if(rc) return rc;
    uint32_t size=rd32(&ref.entry[28]);
    if (size>capacity) return -MINT_ENOSPC;
    if (!size) return 0;
    uint16_t cluster=rd16(&ref.entry[26]);
    size_t copied=0; uint32_t walked=0;
    while(copied<size && walked++<active_volume.cluster_count) {
        uint32_t first=0; if(!cluster_to_sector(&active_volume,cluster,&first)) return -MINT_EIO;
        for(uint32_t s=0;s<active_volume.sectors_per_cluster && copied<size;s++) {
            uint8_t sector[512]; if(!read_volume_sector(&active_volume,first+s,sector)) return -MINT_EIO;
            size_t take=size-copied; if(take>512u) take=512u;
            uint8_t *out=(uint8_t *)buffer; for(size_t i=0;i<take;i++) out[copied+i]=sector[i];
            copied+=take;
        }
        if(copied>=size) break;
        uint16_t next=0; if(!get_next_cluster(&active_volume,cluster,&next)) return -MINT_EIO;
        if(next>=FAT16_EOC_MIN) return -MINT_EIO;
        if(next<2u || next>=FAT16_RESERVED_MIN || (uint32_t)next>=active_volume.cluster_count+2u) return -MINT_EIO;
        cluster=next;
    }
    return copied==size ? (int64_t)copied : -MINT_EIO;
}


/* The following formatter is deliberately narrow: it can initialize only a
 * blank, unpartitioned 32 MiB..2 GiB device after an explicit user confirmation
 * has passed through the syscall boundary. No generic write syscall exists. */
static void wr16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static int sector_is_zero(const uint8_t sector[512]) {
    for (uint32_t i=0;i<512u;i++) if (sector[i]) return 0;
    return 1;
}
static int sector_equal_local(const uint8_t a[512], const uint8_t b[512]) {
    for (uint32_t i=0;i<512u;i++) if (a[i]!=b[i]) return 0;
    return 1;
}
/* Return 1 only when all partition-table metadata windows are readable and
 * zero, 0 when non-zero metadata is present, and -1 on any read failure.
 * Include both ends of the medium so a GPT header/entry array is not silently
 * destroyed just because LBA 0 happens to be blank. */
static int blank_partition_metadata_windows(uint32_t index, uint64_t sectors) {
    uint8_t sector[512];
    const uint64_t primary_end = sectors < 34u ? sectors : 34u;
    for (uint64_t lba=1u; lba<primary_end; ++lba) {
        if (!mint_block_read(index,lba,sector)) return -1;
        if (!sector_is_zero(sector)) return 0;
    }
    uint64_t backup_start = sectors > 33u ? sectors-33u : 0u;
    for (uint64_t lba=backup_start; lba<sectors; ++lba) {
        if (!mint_block_read(index,lba,sector)) return -1;
        if (!sector_is_zero(sector)) return 0;
    }
    return 1;
}
static int format_write(uint32_t index, uint64_t lba, const uint8_t sector[512]) {
    const MINT_BLOCK_DEVICE *dev=mint_block_get(index);
    if (!dev || !dev->write_sector || !sector || lba>=dev->sector_count) return 0;
    /* Intentionally bypass mint_block_write's READONLY guard only within this
       single kernel formatter, after its blank-media and size checks. */
    mint_storage_diag_mark_write_attempt();
    return dev->write_sector(dev->context,lba,sector);
}
static void fat16_make_entry(uint8_t e[32], const char *name, const char *ext,
                             uint8_t attr, uint16_t first_cluster, uint32_t size) {
    bytes_zero(e,32u);
    for (uint32_t i=0;i<8u;i++) e[i]=' ';
    for (uint32_t i=0;i<3u;i++) e[8u+i]=' ';
    for (uint32_t i=0;name && name[i] && i<8u;i++) e[i]=(uint8_t)upper_ascii(name[i]);
    for (uint32_t i=0;ext && ext[i] && i<3u;i++) e[8u+i]=(uint8_t)upper_ascii(ext[i]);
    e[11]=attr;
    /* Safe deterministic date/time: 2026-10-11 12:00. */
    wr16(&e[14],0u); wr16(&e[16],0u); wr16(&e[18],0u);
    wr16(&e[22],0u); wr16(&e[24],0u);
    wr16(&e[26],first_cluster); wr32(&e[28],size);
}
static void fat16_make_dot(uint8_t e[32], int parent, uint16_t cluster) {
    bytes_zero(e,32u);
    for (uint32_t i=0;i<11u;i++) e[i]=' ';
    e[0]='.'; if (parent) e[1]='.';
    e[11]=FAT16_ATTR_DIRECTORY; wr16(&e[26],cluster);
}
static void fat16_make_lfn_one(uint8_t e[32],const char *name,const uint8_t short_name[11]) {
    static const uint8_t positions[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    bytes_zero(e,32u); e[0]=0x41u; e[11]=FAT16_ATTR_LONG_NAME; e[12]=0u;
    e[13]=sfn_checksum(short_name); wr16(&e[26],0u);
    for(uint32_t i=0;i<13u;i++) wr16(&e[positions[i]],0xFFFFu);
    uint32_t i=0; while(name && name[i] && i<12u){wr16(&e[positions[i]],(uint8_t)name[i]);i++;}
    if(i<13u) wr16(&e[positions[i]],0u);
}
static void fat16_copy_text(uint8_t out[512], const char *text, uint32_t *size_out) {
    bytes_zero(out,512u);
    uint32_t n=0;
    while (text && text[n] && n<511u) { out[n]=(uint8_t)text[n]; n++; }
    if (size_out) *size_out=n;
}
static uint32_t fat16_root_sectors(uint32_t root_entries) {
    return (root_entries*32u+511u)/512u;
}

int mint_fat16_format_blank(uint32_t block_device_index) {
    const uint64_t min_sectors=65536ull;      /* 32 MiB */
    const uint64_t max_sectors=4194304ull;    /* 2 GiB: keep the common 25 GiB system disk out */
    const uint32_t partition_start=2048u;
    const uint32_t root_entries=512u;
    const uint32_t root_sectors=fat16_root_sectors(root_entries);
    const uint32_t reserved_sectors=1u;
    const uint32_t fats=2u;
    const MINT_BLOCK_DEVICE *dev=mint_block_get(block_device_index);
    if (!dev || dev->sector_size!=512u || !dev->read_sector || !dev->write_sector || !dev->flush)
        return -MINT_ENODEV;
    if (dev->sector_count<min_sectors || dev->sector_count>max_sectors ||
        dev->sector_count<=partition_start || dev->sector_count>UINT32_MAX)
        return -MINT_EINVAL;

    uint8_t mbr_before[512], mbr_repeat[512];
    if (!mint_block_read(block_device_index,0u,mbr_before) ||
        !mint_block_read(block_device_index,0u,mbr_repeat)) return -MINT_EIO;
    if (!sector_equal_local(mbr_before,mbr_repeat)) return -MINT_EIO;
    if (!sector_is_zero(mbr_before)) return -MINT_EPERM;
    int blank_metadata=blank_partition_metadata_windows(block_device_index,dev->sector_count);
    if (blank_metadata<0) return -MINT_EIO;
    if (!blank_metadata) return -MINT_EPERM;

    uint32_t partition_sectors=(uint32_t)(dev->sector_count-partition_start);
    uint32_t spc=0, sectors_per_fat=0, cluster_count=0;
    for (uint32_t candidate_spc=1u;candidate_spc<=128u;candidate_spc*=2u) {
        uint32_t fatsz=1u;
        for (uint32_t pass=0;pass<16u;pass++) {
            uint64_t overhead=(uint64_t)reserved_sectors+(uint64_t)fats*fatsz+root_sectors;
            if (overhead>=partition_sectors) break;
            uint32_t clusters=(uint32_t)((partition_sectors-overhead)/candidate_spc);
            uint32_t required=(uint32_t)(((uint64_t)(clusters+2u)*2u+511u)/512u);
            if (required==fatsz) {
                if (clusters>=4085u && clusters<65525u &&
                    (uint64_t)fatsz*256u>=(uint64_t)clusters+2u) {
                    spc=candidate_spc; sectors_per_fat=fatsz; cluster_count=clusters;
                }
                break;
            }
            fatsz=required;
        }
        if (spc) break;
    }
    if (!spc || !sectors_per_fat || !cluster_count) return -MINT_EINVAL;
    uint32_t data_start=reserved_sectors+fats*sectors_per_fat+root_sectors;
    if (data_start>=partition_sectors) return -MINT_EINVAL;

    /* Revalidate the destructive-operation preconditions immediately before
       the first write: this is still the same stable, blank unpartitioned disk. */
    if (!mint_block_read(block_device_index,0u,mbr_before) ||
        !mint_block_read(block_device_index,0u,mbr_repeat)) return -MINT_EIO;
    if (!sector_equal_local(mbr_before,mbr_repeat)) return -MINT_EIO;
    if (!sector_is_zero(mbr_before)) return -MINT_EPERM;
    blank_metadata=blank_partition_metadata_windows(block_device_index,dev->sector_count);
    if (blank_metadata<0) return -MINT_EIO;
    if (!blank_metadata) return -MINT_EPERM;

    /* FAT sectors: preserve the two reserved entries and allocate clusters 2..8
       for MintOS26, Documents, Notes, Settings and a few useful seed files. */
    uint8_t sector[512];
    for (uint32_t copy=0;copy<fats;copy++) {
        for (uint32_t s=0;s<sectors_per_fat;s++) {
            bytes_zero(sector,sizeof(sector));
            if (!s) {
                wr16(&sector[0],0xFFF8u); wr16(&sector[2],0xFFFFu);
                for (uint32_t cluster=2u;cluster<=8u;cluster++)
                    wr16(&sector[cluster*2u],0xFFFFu);
            }
            uint64_t lba=(uint64_t)partition_start+reserved_sectors+
                         (uint64_t)copy*sectors_per_fat+s;
            if (!format_write(block_device_index,lba,sector)) return -MINT_EIO;
        }
    }

    /* Fixed root directory: one MINTOS26 directory entry, remaining space zero. */
    for (uint32_t s=0;s<root_sectors;s++) {
        bytes_zero(sector,sizeof(sector));
        if (!s) fat16_make_entry(&sector[0],"MINTOS26","",FAT16_ATTR_DIRECTORY,2u,0u);
        if (!format_write(block_device_index,(uint64_t)partition_start+reserved_sectors+
                          (uint64_t)fats*sectors_per_fat+s,sector)) return -MINT_EIO;
    }

    /* Cluster 2: MINTOS26 directory, cluster 3: Documents directory. */
    bytes_zero(sector,sizeof(sector));
    fat16_make_dot(&sector[0],0,2u);
    fat16_make_dot(&sector[32],1,0u);
    uint8_t documents_sfn[32];
    fat16_make_entry(documents_sfn,"DOCUME~1","",FAT16_ATTR_DIRECTORY,3u,0u);
    fat16_make_lfn_one(&sector[64],"DOCUMENTS",documents_sfn);
    for(uint32_t i=0;i<32u;i++) sector[96u+i]=documents_sfn[i];
    uint64_t cluster2=(uint64_t)partition_start+data_start;
    if (!format_write(block_device_index,cluster2,sector)) return -MINT_EIO;

    bytes_zero(sector,sizeof(sector));
    fat16_make_dot(&sector[0],0,3u);
    fat16_make_dot(&sector[32],1,2u);
    static const char notes[]="Welcome to MintOS26 Notes.\r\nNotes are session-only until persistent writes are enabled.\r\n";
    static const char settings[]="MINTOS26_SETTINGS_V1\r\ntheme=dark\r\naccent=mint\r\nscale=1\r\nwallpaper=0\r\nreduce_motion=0\r\n";
    static const char welcome[]="WELCOME TO MINTOS26\r\nThis volume was initialized by MintDE Disk Utility.\r\n";
    static const char about[]="MINTOS26\r\nExperimental x86-64 UEFI OS and native desktop.\r\n";
    static const char develop[]="DEVELOPMENT VOLUME\r\nBack up important data. This filesystem is currently read-only in MintDE.\r\n";
    uint32_t notes_size=0, settings_size=0, welcome_size=0, about_size=0, develop_size=0;
    fat16_make_entry(&sector[64],"NOTES","TXT",0x20u,4u,(uint32_t)(sizeof(notes)-1u));
    fat16_make_entry(&sector[96],"SETTINGS","DAT",0x20u,5u,(uint32_t)(sizeof(settings)-1u));
    fat16_make_entry(&sector[128],"WELCOME","TXT",0x20u,6u,(uint32_t)(sizeof(welcome)-1u));
    fat16_make_entry(&sector[160],"ABOUT","TXT",0x20u,7u,(uint32_t)(sizeof(about)-1u));
    fat16_make_entry(&sector[192],"DEVELOP","TXT",0x20u,8u,(uint32_t)(sizeof(develop)-1u));
    uint64_t cluster3=(uint64_t)partition_start+data_start+(uint64_t)(3u-2u)*spc;
    if (!format_write(block_device_index,cluster3,sector)) return -MINT_EIO;

    const char *contents[5]={notes,settings,welcome,about,develop};
    const uint32_t clusters[5]={4u,5u,6u,7u,8u};
    uint32_t *sizes[5]={&notes_size,&settings_size,&welcome_size,&about_size,&develop_size};
    for (uint32_t i=0;i<5u;i++) {
        fat16_copy_text(sector,contents[i],sizes[i]);
        uint64_t lba=(uint64_t)partition_start+data_start+
                     (uint64_t)(clusters[i]-2u)*spc;
        if (!format_write(block_device_index,lba,sector)) return -MINT_EIO;
    }

    /* FAT16 BPB. The type string is informational; cluster count is authoritative. */
    uint8_t boot[512]; bytes_zero(boot,sizeof(boot));
    boot[0]=0xEBu; boot[1]=0x3Cu; boot[2]=0x90u;
    for (uint32_t i=0;i<8u;i++) boot[3u+i]=(uint8_t)"MINTOS26"[i];
    wr16(&boot[11],512u); boot[13]=(uint8_t)spc; wr16(&boot[14],(uint16_t)reserved_sectors);
    boot[16]=(uint8_t)fats; wr16(&boot[17],(uint16_t)root_entries); wr16(&boot[19],0u);
    boot[21]=0xF8u; wr16(&boot[22],(uint16_t)sectors_per_fat);
    wr16(&boot[24],63u); wr16(&boot[26],255u); wr32(&boot[28],partition_start);
    wr32(&boot[32],partition_sectors); boot[36]=0x80u; boot[38]=0x29u;
    wr32(&boot[39],MINT_STORAGE_DEVELOPER_DISK_ID);
    for (uint32_t i=0;i<11u;i++) boot[43u+i]=(uint8_t)"MINTOS26   "[i];
    for (uint32_t i=0;i<8u;i++) boot[54u+i]=(uint8_t)"FAT16   "[i];
    boot[510]=0x55u; boot[511]=0xAAu;
    if (!format_write(block_device_index,partition_start,boot)) return -MINT_EIO;
    if (!dev->flush(dev->context)) return -MINT_EIO;

    /* Commit the MBR last, so a reset before this write leaves the disk
       unpartitioned and the formatter can safely be retried after inspection. */
    uint8_t mbr[512]; bytes_zero(mbr,sizeof(mbr));
    wr32(&mbr[440],MINT_STORAGE_DEVELOPER_DISK_ID);
    uint8_t *part=&mbr[446];
    part[1]=0xFEu; part[2]=0xFFu; part[3]=0xFFu; part[4]=FAT16_PART_TYPE_LBA_LARGE;
    part[5]=0xFEu; part[6]=0xFFu; part[7]=0xFFu;
    wr32(&part[8],partition_start); wr32(&part[12],partition_sectors);
    mbr[510]=0x55u; mbr[511]=0xAAu;
    if (!format_write(block_device_index,0u,mbr) || !dev->flush(dev->context)) return -MINT_EIO;

    uint8_t verify_mbr[512], verify_boot[512];
    if (!mint_block_read(block_device_index,0u,verify_mbr) ||
        !mint_block_read(block_device_index,partition_start,verify_boot) ||
        !sector_equal_local(mbr,verify_mbr) || !sector_equal_local(boot,verify_boot)) return -MINT_EIO;
    if (!mint_fat16_try_mount(block_device_index)) return -MINT_EIO;
    uint8_t probe[512];
    if (mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT",probe,sizeof(probe))<0 ||
        mint_fat16_read_file("MINTOS26/DOCUMENTS/SETTINGS.DAT",probe,sizeof(probe))<0) {
        mint_fat16_unmount(); return -MINT_EIO;
    }
    (void)notes_size; (void)settings_size; (void)welcome_size; (void)about_size; (void)develop_size;
    return 0;
}

static int fat16_find_directory(const FAT16_VOLUME *v, const char *path,
                                int *in_root_out, uint16_t *cluster_out) {
    if (!v || !v->mounted || !path || !in_root_out || !cluster_out) return -MINT_EINVAL;
    size_t n=0; while (n<FAT16_PATH_MAX && path[n]) n++;
    if (n>=FAT16_PATH_MAX) return -MINT_EINVAL;
    size_t pos=0; int in_root=1; uint16_t current=0; uint32_t components=0;
    while (pos<n) {
        while (pos<n && (path[pos]=='/' || path[pos]=='\\')) pos++;
        if (pos>=n) break;
        size_t begin=pos; while (pos<n && path[pos]!='/' && path[pos]!='\\') pos++;
        size_t len=pos-begin;
        if (!len || len>=FAT16_COMPONENT_MAX || ++components>FAT16_MAX_PATH_COMPONENTS) return -MINT_EINVAL;
        uint8_t sfn[11]; int have_sfn=component_to_sfn(path+begin,len,sfn);
        FAT16_ENTRY_REF found;
        int rc=in_root ? find_in_root(v,sfn,have_sfn,path+begin,len,&found)
                       : find_in_directory(v,current,sfn,have_sfn,path+begin,len,&found);
        if (rc<=0) return rc<0 ? -MINT_EIO : -MINT_ENOENT;
        if (!(found.entry[11]&FAT16_ATTR_DIRECTORY)) return -MINT_ENOENT;
        current=rd16(&found.entry[26]);
        if (current<2u || (uint32_t)current>=v->cluster_count+2u) return -MINT_EIO;
        in_root=0;
    }
    *in_root_out=in_root; *cluster_out=current; return 0;
}
static void fat16_dirent_name(const uint8_t e[32], char out[MINT_FAT16_DIRENT_NAME_MAX]) {
    size_t k=0;
    for (uint32_t i=0;i<8u && e[i]!=' ';i++) if (k+1u<MINT_FAT16_DIRENT_NAME_MAX) out[k++]=(char)e[i];
    size_t ext=0; while (ext<3u && e[8u+ext]!=' ') ext++;
    if (ext && k+1u<MINT_FAT16_DIRENT_NAME_MAX) out[k++]='.';
    for (size_t i=0;i<ext && k+1u<MINT_FAT16_DIRENT_NAME_MAX;i++) out[k++]=(char)e[8u+i];
    out[k]='\0';
}
static int fat16_lfn_to_name(const FAT16_LFN_STATE *state, const uint8_t short_entry[32],
                             char out[MINT_FAT16_DIRENT_NAME_MAX]) {
    if (!state || !state->active || state->checksum!=sfn_checksum(short_entry)) return 0;
    size_t n=0;
    while(n<260u && state->units[n]!=0u && state->units[n]!=0xFFFFu) {
        if(state->units[n]>0x7Fu || n+1u>=MINT_FAT16_DIRENT_NAME_MAX) return 0;
        out[n]=(char)state->units[n]; n++;
    }
    if(!n) return 0;
    out[n]='\0'; return 1;
}
static int fat16_add_dirent(const uint8_t e[32], MINT_FAT16_DIRENT *out, uint32_t capacity,
                            uint32_t *count, const FAT16_LFN_STATE *lfn) {
    if (e[0]==0u) return 1; /* end marker */
    if (e[0]==0xE5u || e[11]==FAT16_ATTR_LONG_NAME || (e[11]&FAT16_ATTR_VOLUME_ID)) return 0;
    if ((e[0]=='.' && (e[1]==' ' || e[1]=='.'))) return 0;
    if (*count<capacity) {
        MINT_FAT16_DIRENT *d=&out[*count]; bytes_zero(d,sizeof(*d));
        if(!fat16_lfn_to_name(lfn,e,d->name)) fat16_dirent_name(e,d->name);
        d->size=rd32(&e[28]);
        d->flags=(e[11]&FAT16_ATTR_DIRECTORY)?MINT_FAT16_DIRENT_DIRECTORY:0u;
    }
    ++*count; return 0;
}
int64_t mint_fat16_list_dir(const char *path, MINT_FAT16_DIRENT *entries, uint32_t capacity) {
    if (!active_volume.mounted) return -MINT_ENODEV;
    if (!entries || !capacity || capacity>MINT_FAT16_LIST_MAX) return -MINT_EINVAL;
    for (uint32_t i=0;i<capacity;i++) bytes_zero(&entries[i],sizeof(entries[i]));
    int in_root=0; uint16_t cluster=0;
    int rc=fat16_find_directory(&active_volume,path,&in_root,&cluster);
    if (rc) return rc;
    uint32_t count=0; uint8_t sector[512]; FAT16_LFN_STATE lfn; lfn_reset(&lfn);
    if (in_root) {
        uint32_t seen=0;
        for (uint32_t s=0;s<active_volume.root_sectors && seen<active_volume.root_entries;s++) {
            if (!read_volume_sector(&active_volume,active_volume.root_start+s,sector)) return -MINT_EIO;
            for (uint32_t off=0;off<512u && seen<active_volume.root_entries;off+=32u,seen++) {
                uint8_t *entry=&sector[off];
                if(entry[11]==FAT16_ATTR_LONG_NAME) { lfn_consume(&lfn,entry); continue; }
                int stop=fat16_add_dirent(entry,entries,capacity,&count,&lfn); lfn_reset(&lfn);
                if (stop) return count<capacity?(int64_t)count:(int64_t)capacity;
            }
        }
    } else {
        uint32_t walked=0;
        while (cluster>=2u && (uint32_t)cluster<active_volume.cluster_count+2u &&
               walked++<active_volume.cluster_count) {
            uint32_t first_lba=0; if (!cluster_to_sector(&active_volume,cluster,&first_lba)) return -MINT_EIO;
            for (uint32_t s=0;s<active_volume.sectors_per_cluster;s++) {
                if (!read_volume_sector(&active_volume,first_lba+s,sector)) return -MINT_EIO;
                for (uint32_t off=0;off<512u;off+=32u) {
                    uint8_t *entry=&sector[off];
                    if(entry[11]==FAT16_ATTR_LONG_NAME) { lfn_consume(&lfn,entry); continue; }
                    int stop=fat16_add_dirent(entry,entries,capacity,&count,&lfn); lfn_reset(&lfn);
                    if (stop) return count<capacity?(int64_t)count:(int64_t)capacity;
                }
            }
            uint16_t next=0; if (!get_next_cluster(&active_volume,cluster,&next)) return -MINT_EIO;
            if (next>=FAT16_EOC_MIN) break;
            if (next<2u || next>=FAT16_RESERVED_MIN || (uint32_t)next>=active_volume.cluster_count+2u) return -MINT_EIO;
            cluster=next;
        }
    }
    return count<capacity?(int64_t)count:(int64_t)capacity;
}
