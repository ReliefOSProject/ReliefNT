/* SPDX-License-Identifier: Apache-2.0 */
/* Only the physical sector device is emulated. Native format bytes, registry
 * admission and module lifetime come from the actual kernel implementation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
#include "../../fs/fat/internal.h"
#include "../../fs/exfat/internal.h"
#include "../../kernel/module/internal.h"

static struct module owner;
static struct install_disk_state owned_disk;
static uint8_t *backing;
static const uint64_t first=2048,sectors=262144;
static unsigned writes,allocations;
static bool fail_write;
void *kernel_malloc(size_t bytes) { void *p=calloc(1,bytes);if(p)++allocations;return p; }
void kernel_free(void *p) { if(p){assert(allocations);--allocations;free(p);} }
void module_memory_free(struct module_memory *memory) { assert(!memory); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{ *flags=0;while(__atomic_exchange_n(&lock->state,1,__ATOMIC_ACQUIRE)){} }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{ (void)flags;__atomic_store_n(&lock->state,0,__ATOMIC_RELEASE); }
int storage_write_install_disk(const struct install_disk_state *disk,uint64_t lba,uint32_t count,const void *bytes)
{
    assert(disk==&owned_disk && lba>=first && count && lba<=first+sectors && count<=first+sectors-lba);
    assert(module_unload("formatfs",0)==-EWOULDBLOCK);
    ++writes;if(fail_write){fail_write=false;return -EIO;}
    memcpy(backing+lba*512,bytes,(size_t)count*512);return 0;
}
int storage_read_install_disk(const struct install_disk_state *disk,uint64_t lba,uint32_t count,void *bytes)
{
    assert(disk==&owned_disk && lba>=first && count && lba<=first+sectors && count<=first+sectors-lba);
    memcpy(bytes,backing+lba*512,(size_t)count*512);return 0;
}
static int forbidden_mount(const struct fs_mount_request *request,struct vfs_mount **out)
{ (void)request;(void)out;abort(); }
int main(int argc,char **argv)
{
    assert(argc==3);
    int (*format)(struct install_disk_state *,uint64_t,uint64_t)=
        !strcmp(argv[1],"vfat") ? install_format_fat32 :
        !strcmp(argv[1],"exfat") ? install_format_exfat :
        !strcmp(argv[1],"ext2") ? install_format_ext2 :
        !strcmp(argv[1],"ext4") ? install_format_ext4 : NULL;
    assert(format);
    backing=malloc((size_t)(first+sectors+2048)*512);assert(backing);
    memset(backing,0xa5,(size_t)(first+sectors+2048)*512);
    owned_disk=(struct install_disk_state){.present=true,.sector_count=first+sectors+2048};
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(runtime->name,"formatfs");strcpy(owner.name,"formatfs");
    runtime->owner=&owner;owner.runtime=runtime;module_list=runtime;runtime->state=MODULE_COMING;
    struct file_system_type type={.name=argv[1],.owner=&owner,.get_tree=forbidden_mount,.format=format};
    struct fs_type_ref ref={0};
    assert(!register_filesystem(&type));assert(fs_type_get(argv[1],&ref)==-ENODEV && !writes);
    runtime->state=MODULE_LIVE;assert(!fs_type_get(argv[1],&ref));
    assert(!unregister_filesystem(&type));
    /* An admitted operation keeps its code through deregistration and errors. */
    fail_write=true;assert(fs_type_format(&ref,&owned_disk,first,sectors)==-EIO && writes==1);
    assert(!fs_type_format(&ref,&owned_disk,first,sectors) && writes>1);
    for(uint64_t i=0;i<first*512;++i)assert(backing[i]==0xa5);
    for(uint64_t i=(first+sectors)*512;i<(first+sectors+2048)*512;++i)assert(backing[i]==0xa5);
    FILE *output=fopen(argv[2],"wb");assert(output);
    assert(fwrite(backing+first*512,512,sectors,output)==sectors && !fclose(output));
    fs_type_put(&ref);assert(!module_unload("formatfs",0) && !allocations);
    free(backing);puts("PASS native format: real bytes, sector bounds, COMING admission, owner held through error and deregistration");
}
