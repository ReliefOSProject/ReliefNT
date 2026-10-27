/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/devfs.h>
#include <reliefnt/audio.h>

static const struct {const char *name;uint32_t kind,type,flags,volume;} entries[]={
    {"/dev",100,RELIEFOS_FS_TYPE_DIR,STORAGE_NODE_FLAG_DEV_DIR,0},
    {"/dev/null",STORAGE_DEV_KIND_NULL,RELIEFOS_FS_TYPE_DEVICE,STORAGE_NODE_FLAG_DEV_NODE,0},
    {"/dev/console",STORAGE_DEV_KIND_CONSOLE,RELIEFOS_FS_TYPE_DEVICE,STORAGE_NODE_FLAG_DEV_NODE,0},
    {"/dev/sda",STORAGE_DEV_KIND_DISK,RELIEFOS_FS_TYPE_DEVICE,STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_DEV_BLOCK,7},
    {"/dev/snd",101,RELIEFOS_FS_TYPE_DIR,STORAGE_NODE_FLAG_DEV_DIR,0},
    {"/dev/snd/controlC0",STORAGE_DEV_KIND_AUDIO_CONTROL,RELIEFOS_FS_TYPE_DEVICE,STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_AUDIO_CONTROL,42},
};
int sound_service_card_index(uint32_t token){return token==42 ? 0 : -1;}
uint64_t storage_block_rdev(uint32_t volume)
{assert(volume==7);return (uint64_t)259<<8|0x12|(uint64_t)0xabc00<<12;}
int storage_devfs_lookup(const char *path,struct storage_node *out)
{
    for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);++i)if(!strcmp(path,entries[i].name)) {
        *out=(struct storage_node){.first_cluster=entries[i].kind,.type=entries[i].type,
            .flags=entries[i].flags,.volume_id=entries[i].volume};return 0;
    }
    return -2;
}
int storage_devfs_readdir_node(const struct storage_node *node,uint64_t *cursor,struct reliefos_dir_entry *out)
{
    const char *names[]={"null","console","sda","snd"};
    if(node->first_cluster==100 && *cursor<4) {
        unsigned i=(*cursor)++;*out=(struct reliefos_dir_entry){.type=i==3 ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_DEVICE};
        strcpy(out->name,names[i]);return 1;
    }
    if(node->first_cluster==101 && !(*cursor)++) {
        *out=(struct reliefos_dir_entry){.type=RELIEFOS_FS_TYPE_DEVICE};strcpy(out->name,"controlC0");return 1;
    }
    return 0;
}
int main(void)
{
    struct storage_node node;
    assert(!devfs_lookup_rdev(LINUX_S_IFCHR,0x103,&node) && node.first_cluster==STORAGE_DEV_KIND_NULL);
    assert(!devfs_lookup_rdev(LINUX_S_IFCHR,0x501,&node) && node.first_cluster==STORAGE_DEV_KIND_CONSOLE);
    assert(!devfs_lookup_rdev(LINUX_S_IFBLK,(uint64_t)259<<8|0x12|(uint64_t)0xabc00<<12,&node) && node.volume_id==7);
    assert(!devfs_lookup_rdev(LINUX_S_IFCHR,(uint64_t)116<<8,&node) && node.volume_id==42 && (node.flags&STORAGE_NODE_FLAG_AUDIO_CONTROL));
    assert(devfs_lookup_rdev(LINUX_S_IFBLK,0x103,&node)==-6);
    assert(devfs_lookup_rdev(LINUX_S_IFCHR,0xfffff,&node)==-6);
    assert(devfs_lookup_rdev(LINUX_S_IFREG,0x103,&node)==-22);
    assert(devfs_lookup_rdev(LINUX_S_IFCHR,0x103,NULL)==-22);
    struct linux_stat_abi st={.st_mode=LINUX_S_IFCHR,.st_rdev=0x501};
    node=(struct storage_node){.flags=STORAGE_NODE_FLAG_RDEV|STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_DEV_NODE,
        .first_cluster=STORAGE_DEV_KIND_NULL};devfs_stat_identity(&st,&node);assert(st.st_rdev==0x501);
    puts("PASS device lookup uses actual Linux type/rdev including wide block and nested ALSA, preserving real inode metadata");
}
