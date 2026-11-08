#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/include/block.h"
#include "../kernel/include/fat16.h"
#include "../kernel/include/storage_diag.h"
#include "../kernel/include/uapi/mintos/syscall.h"

#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } puts("PASS: " msg); } while (0)

typedef struct test_disk {
    uint8_t *bytes;
    uint64_t sectors;
    uint64_t writes;
    uint64_t flushes;
    uint64_t diagnostic_write_marks;
} TEST_DISK;

static int fake_read(void *ctx, uint64_t lba, uint8_t out[512]) {
    TEST_DISK *d=(TEST_DISK *)ctx;
    if (!d || !out || lba>=d->sectors) return 0;
    memcpy(out,d->bytes+(size_t)lba*512u,512u); return 1;
}
static int fake_write(void *ctx,uint64_t lba,const uint8_t in[512]) {
    TEST_DISK *d=(TEST_DISK *)ctx;
    if (!d || !in || lba>=d->sectors) return 0;
    memcpy(d->bytes+(size_t)lba*512u,in,512u); d->writes++; return 1;
}
static int fake_flush(void *ctx) { TEST_DISK *d=(TEST_DISK *)ctx; if(!d)return 0; d->flushes++; return 1; }
void mint_storage_diag_mark_write_attempt(void) { }

static uint16_t read_le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static int register_disk(TEST_DISK *disk,uint64_t sectors,const char *name) {
    MINT_BLOCK_DEVICE dev={0};
    size_t i=0; while(name[i] && i+1u<sizeof(dev.name)){dev.name[i]=name[i];i++;}
    dev.sector_count=sectors; dev.sector_size=512u; dev.flags=MINT_BLOCK_FLAG_READONLY;
    dev.context=disk; dev.read_sector=fake_read; dev.write_sector=fake_write; dev.flush=fake_flush;
    return mint_block_register(&dev);
}

int main(void) {
    const uint64_t sectors=65536u;
    TEST_DISK d={0}; d.sectors=sectors; d.bytes=(uint8_t *)calloc((size_t)sectors,512u);
    if(!d.bytes){puts("FAIL: allocate disposable 32 MiB host fixture");return 1;}
    mint_block_init(); CHECK(register_disk(&d,sectors,"sda"),"register blank disposable disk while generic registry remains read-only");
    uint8_t sector[512]; CHECK(mint_block_read(0,0,sector),"read initial blank LBA 0");
    CHECK(!mint_block_write(0,0,sector),"generic block write rejects read-only device");
    MINT_BLOCK_IO_STATS stats;
    CHECK(mint_block_io_stats(0u,&stats) && stats.writes_blocked==1u,
          "block-layer telemetry records rejected generic writes");
    CHECK(mint_block_read(0u,0u,sector), "read-only block read succeeds for telemetry test");
    CHECK(mint_block_io_stats(0u,&stats) && stats.read_calls>=2u && stats.read_success>=2u,
          "block-layer telemetry tracks read calls and successes");
    CHECK(mint_fat16_format_blank(0)==0,"confirmed-only formatter initializes blank FAT16 developer disk");
    CHECK(d.writes>0 && d.flushes>=2,"formatter writes sectors through private callback and flushes the device");
    CHECK(mint_block_read(0,0,sector) && sector[510]==0x55 && sector[511]==0xAA,"formatter commits a valid MBR signature last");
    CHECK(((uint32_t)sector[440] | ((uint32_t)sector[441]<<8) | ((uint32_t)sector[442]<<16) | ((uint32_t)sector[443]<<24)) == MINT_STORAGE_DEVELOPER_DISK_ID,"formatter writes the expected developer disk ID");
    CHECK(sector[450]==0x0E && sector[454]==0x00 && sector[455]==0x08,"formatter creates an MBR FAT16-LBA partition starting at LBA 2048");
    CHECK(mint_block_read(0,2048,sector) && sector[510]==0x55 && sector[511]==0xAA,"FAT16 partition boot sector has a valid signature");
    CHECK(sector[11]==0x00 && sector[12]==0x02 && sector[13]>0,"FAT16 BPB uses 512-byte sectors and nonzero cluster geometry");
    CHECK(mint_fat16_is_mounted() && mint_fat16_block_device_index()==0,"production filesystem parser mounts formatted volume read-only");
    char content[256]={0}; int64_t n=mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT",content,sizeof(content)-1u);
    CHECK(n>0 && strstr(content,"Welcome to MintOS26 Notes")!=NULL,"formatted volume reads Notes through the production FAT16 reader");
    MINT_FAT16_DIRENT entries[MINT_FAT16_LIST_MAX];
    int64_t count=mint_fat16_list_dir("MINTOS26/DOCUMENTS",entries,MINT_FAT16_LIST_MAX);
    int saw_notes=0,saw_settings=0,saw_documents=0;
    for(int64_t i=0;i<count;i++){if(strcmp(entries[i].name,"NOTES.TXT")==0)saw_notes=1;if(strcmp(entries[i].name,"SETTINGS.DAT")==0)saw_settings=1;}
    int64_t root_count=mint_fat16_list_dir("MINTOS26",entries,MINT_FAT16_LIST_MAX);
    for(int64_t i=0;i<root_count;i++) if(strcmp(entries[i].name,"DOCUMENTS")==0 && (entries[i].flags&MINT_FAT16_DIRENT_DIRECTORY)) saw_documents=1;
    CHECK(count>=5 && saw_notes && saw_settings,"directory-list API exposes actual formatted files and metadata");
    CHECK(saw_documents,"directory listing resolves VFAT long name DOCUMENTS rather than short alias");
    uint64_t writes_before=d.writes;
    CHECK(mint_fat16_format_blank(0)==-MINT_EPERM,"formatter refuses to overwrite an already partitioned disk");
    CHECK(d.writes==writes_before,"refusing nonblank media makes no additional writes");

    /* Register and format a second disk without resetting the block registry.
       This verifies that Files can enumerate/select more than one mounted volume. */
    TEST_DISK d2={0}; d2.sectors=sectors; d2.bytes=(uint8_t *)calloc((size_t)sectors,512u);
    if(!d2.bytes){puts("FAIL: allocate second disposable volume");return 1;}
    CHECK(register_disk(&d2,sectors,"sdb"),"register second disposable developer disk alongside the first");
    CHECK(mint_fat16_format_blank(1)==0,"initialize second blank FAT16 disk without unmounting the first");
    MINT_FAT16_VOLUME_LIST volumes;
    CHECK(mint_fat16_volume_list(&volumes) && volumes.count==2u,"volume registry retains both mounted FAT16 volumes");
    CHECK(volumes.block_indices[0]==0u && volumes.block_indices[1]==1u,"mounted volume list preserves block-device mapping");
    CHECK(mint_fat16_volume_select(0u)==1 && mint_fat16_block_device_index()==0u,"Files can select the first mounted device explicitly");
    memset(content,0,sizeof(content)); n=mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT",content,sizeof(content)-1u);
    CHECK(n>0 && strstr(content,"Welcome to MintOS26 Notes")!=NULL,"first selected volume reads its own Notes file");
    CHECK(mint_fat16_volume_select(1u)==1 && mint_fat16_block_device_index()==1u,"Files can select the second mounted device explicitly");
    memset(content,0,sizeof(content)); n=mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT",content,sizeof(content)-1u);
    CHECK(n>0 && strstr(content,"Welcome to MintOS26 Notes")!=NULL,"second selected volume reads its own Notes file");

    /* Rename the seed directory in the second volume to prove standard FAT16
       remains mountable even when MintOS-specific paths are absent. */
    uint8_t *boot2=d2.bytes+(size_t)2048u*512u;
    uint32_t reserved=read_le16(boot2+14u), fats=boot2[16u], fatsz=read_le16(boot2+22u);
    uint64_t root_lba=2048u+(uint64_t)reserved+(uint64_t)fats*fatsz;
    CHECK(root_lba<d2.sectors,"compute generic FAT16 root directory location from BPB");
    memcpy(d2.bytes+(size_t)root_lba*512u,"GENERIC    ",11u);
    CHECK(mint_fat16_try_mount(1u)==1,"standard FAT16 volume mounts even without MintOS seed-directory name");
    CHECK(mint_fat16_volume_list(&volumes) && volumes.count==2u,"generic FAT16 revalidation does not hide other mounted volumes");
    CHECK(mint_fat16_volume_select(1u)==1,"select generic FAT16 volume for directory listing");
    count=mint_fat16_list_dir("",entries,MINT_FAT16_LIST_MAX);
    int saw_generic=0;
    for(int64_t i=0;i<count;i++) if(strcmp(entries[i].name,"GENERIC")==0) saw_generic=1;
    CHECK(count>0 && saw_generic,"Files directory API lists a generic FAT16 root without MintOS seed files");
    CHECK(mint_fat16_read_file("MINTOS26/DOCUMENTS/NOTES.TXT",content,sizeof(content)-1u)==-MINT_ENOENT,
          "generic volume does not fabricate MintOS Notes paths");
    free(d2.bytes);
    free(d.bytes);

    /* An empty MBR is not enough to prove a medium is blank: reject GPT-like
       metadata at LBA 1 (and do so before touching any sectors). */
    TEST_DISK gpt={0}; gpt.sectors=sectors; gpt.bytes=(uint8_t *)calloc((size_t)sectors,512u);
    if(!gpt.bytes){puts("FAIL: allocate GPT safety fixture");return 1;}
    mint_block_init(); CHECK(register_disk(&gpt,sectors,"sda"),"register zero-MBR disk with possible GPT metadata");
    memcpy(gpt.bytes+512u,"EFI PART",8u);
    CHECK(mint_fat16_format_blank(0)==-MINT_EPERM,"formatter rejects a blank MBR when GPT metadata exists at LBA 1");
    CHECK(gpt.writes==0,"GPT rejection happens before any media writes");
    free(gpt.bytes);

    TEST_DISK backup_gpt={0}; backup_gpt.sectors=sectors; backup_gpt.bytes=(uint8_t *)calloc((size_t)sectors,512u);
    if(!backup_gpt.bytes){puts("FAIL: allocate backup GPT safety fixture");return 1;}
    mint_block_init(); CHECK(register_disk(&backup_gpt,sectors,"sda"),"register zero-MBR disk with possible backup GPT metadata");
    memcpy(backup_gpt.bytes+(size_t)(sectors-1u)*512u,"EFI PART",8u);
    CHECK(mint_fat16_format_blank(0)==-MINT_EPERM,"formatter rejects a blank MBR when backup GPT metadata exists at last LBA");
    CHECK(backup_gpt.writes==0,"backup GPT rejection happens before any media writes");
    free(backup_gpt.bytes);

    uint8_t first_sector[512]={0}; TEST_DISK large={0}; large.bytes=first_sector; large.sectors=52428800ull;
    mint_block_init(); CHECK(register_disk(&large,large.sectors,"sdb"),"register large-system-disk descriptor");
    CHECK(mint_fat16_format_blank(0)==-MINT_EINVAL,"formatter refuses disks larger than the 2 GiB safety ceiling");
    puts("PASS: formatter and directory listing regression tests complete");
    return 0;
}
