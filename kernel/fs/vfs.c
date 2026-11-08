#include <stddef.h>
#include <stdint.h>
#include "../include/vfs.h"
#include "../include/block.h"
#include "../include/fat16.h"

static void copy_text(char *dst,size_t cap,const char *src) {
    if(!dst||!cap)return;
    size_t i=0; if(src)while(src[i]&&i+1u<cap){dst[i]=src[i];i++;} dst[i]='\0';
}
static void clear_table(MINT_VFS_MOUNT_TABLE *out) {
    uint8_t *p=(uint8_t *)out; for(size_t i=0;i<sizeof(*out);i++)p[i]=0;
}
int mint_vfs_mount_table_snapshot(MINT_VFS_MOUNT_TABLE *out) {
    if(!out)return 0;
    clear_table(out); out->version=MINT_VFS_ABI_VERSION;
    /* Session RAM is a real virtual mount, but explicitly volatile. */
    MINT_VFS_MOUNT *session=&out->mounts[out->count++];
    copy_text(session->path,sizeof(session->path),"/session");
    copy_text(session->source,sizeof(session->source),"ramfs");
    copy_text(session->filesystem,sizeof(session->filesystem),"RAMFS");
    session->flags=MINT_VFS_FLAG_VOLATILE; session->block_index=UINT32_MAX;

    MINT_FAT16_VOLUME_LIST volumes;
    if(!mint_fat16_volume_list(&volumes))return 1;
    for(uint32_t i=0;i<volumes.count;i++) {
        uint32_t index=volumes.block_indices[i];
        const MINT_BLOCK_DEVICE *dev=mint_block_get(index);
        if(!dev)continue;
        if(out->count>=MINT_VFS_MAX_MOUNTS){out->truncated=1u;break;}
        MINT_VFS_MOUNT *m=&out->mounts[out->count++];
        copy_text(m->source,sizeof(m->source),dev->name);
        copy_text(m->filesystem,sizeof(m->filesystem),"FAT16");
        /* Produce a safe /Volumes/<device-name> path, never from user input. */
        copy_text(m->path,sizeof(m->path),"/Volumes/");
        size_t n=9u;
        for(size_t j=0;dev->name[j]&&n+1u<sizeof(m->path);j++)m->path[n++]=dev->name[j];
        m->path[n]='\0';
        m->flags=MINT_VFS_FLAG_READ_ONLY; m->block_index=index;
    }
    return 1;
}
