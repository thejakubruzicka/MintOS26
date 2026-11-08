#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/include/vfs.h"
#include "../kernel/include/block.h"
#include "../kernel/include/fat16.h"

static MINT_BLOCK_DEVICE fake[3];
static MINT_FAT16_VOLUME_LIST volumes;
uint32_t mint_block_count(void) { return 3u; }
const MINT_BLOCK_DEVICE *mint_block_get(uint32_t index) { return index<3u?&fake[index]:0; }
int mint_fat16_volume_list(MINT_FAT16_VOLUME_LIST *out) { if(!out)return 0;*out=volumes;return 1; }
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); return 1; } else printf("PASS: %s\n",m); } while (0)
int main(void) {
    strcpy(fake[0].name,"sda"); strcpy(fake[1].name,"sdb"); strcpy(fake[2].name,"sdc");
    volumes.count=2;volumes.block_indices[0]=0;volumes.block_indices[1]=1;
    MINT_VFS_MOUNT_TABLE t;
    CHECK(mint_vfs_mount_table_snapshot(&t),"VFS snapshot succeeds");
    CHECK(t.version==MINT_VFS_ABI_VERSION && t.count==3u,"session RAM and both mounted volumes are represented");
    CHECK(strcmp(t.mounts[0].path,"/session")==0 && (t.mounts[0].flags&MINT_VFS_FLAG_VOLATILE),
          "RAMFS mount is explicitly volatile");
    CHECK(strcmp(t.mounts[1].path,"/Volumes/sda")==0 && strcmp(t.mounts[1].filesystem,"FAT16")==0 &&
          (t.mounts[1].flags&MINT_VFS_FLAG_READ_ONLY),"first FAT16 mount has stable path and read-only flag");
    CHECK(strcmp(t.mounts[2].path,"/Volumes/sdb")==0 && t.mounts[2].block_index==1u,
          "second volume stays associated with its real block index");
    volumes.count=3;volumes.block_indices[2]=99u;
    CHECK(mint_vfs_mount_table_snapshot(&t) && t.count==3u,
          "VFS safely ignores stale block indexes while preserving valid mounts");
    puts("PASS: VFS mount table checks complete");return 0;
}
