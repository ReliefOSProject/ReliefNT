#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/module.h>
#include <reliefnt/heap.h>
#include <reliefnt/mount.h>
#include <reliefnt/usercopy.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/errno.h>
#include "../../block/core/storage_internal.h"
static struct task current;
static struct task_file file;
static struct storage_inode_ref inode;
static int verify_snapshot_guard;
static unsigned char image[128];
static const char arguments[4096]="mode=test";
static unsigned calls,copies,reads,pins;
static int io_error,read_only_error,short_eof,load_status,pin_failure;
static size_t heap_objects;
static uint64_t provider_size=sizeof(image);
static bool async_requested=true;
struct task *sched_current_task(void){return &current;}
void *kernel_malloc(size_t n){void *p=calloc(1,n);if(p)++heap_objects;return p;}
void kernel_free(void *p){if(p){--heap_objects;free(p);}}
int user_copy_from_task(struct task *t,void *out,uint64_t address,uint64_t length)
{
    assert(t==&current);++copies;
    if(!address || address+length<address)return -14;
    memcpy(out,(void *)(uintptr_t)address,(size_t)length);return 0;
}
struct task_file *task_file_for_io(struct task *t,int fd){assert(t==&current);return fd==7 ? &file : NULL;}
struct task_file *task_file_get(struct task_file *f){assert(f==&file);if(pin_failure)return NULL;++pins;return f;}
void task_file_put(struct task_file *f){assert(f==&file && pins);--pins;}
void kernel_execution_lock_irqsave(uint64_t *flags){*flags=0;}
void kernel_execution_unlock_irqrestore(uint64_t flags){(void)flags;}
void storage_inode_retain(struct storage_inode_ref *reference)
{assert(reference==&inode && reference->references);++reference->references;}
int storage_inode_put(struct storage_inode_ref *reference)
{if(reference){assert(reference==&inode && reference->references>1);--reference->references;}return 0;}
int storage_inode_refresh(struct storage_node *n)
{(void)n;fputs("unexpected bare module metadata refresh\n",stderr);abort();}
int storage_read_node(const struct storage_node *n,uint64_t offset,void *buffer,uint32_t length,uint32_t *got)
{(void)n;(void)offset;(void)buffer;(void)length;(void)got;fputs("unexpected bare module snapshot read\n",stderr);abort();}
void storage_set_io_async_context(bool requested){async_requested=requested;}
#ifdef CONFIG_MODULES
bool try_module_get(struct module *owner){assert(!owner);return true;}
void module_put(struct module *owner){assert(!owner);}
#endif
static int provider_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{(void)context;(void)parent;(void)name;(void)out;return -ENOENT;}
static int provider_retain(void *context,const struct storage_node *node,void **pin)
{
    assert(context==&inode);*pin=NULL;
    if(node->type==RELIEFOS_FS_TYPE_FILE){storage_inode_retain(&inode);*pin=&inode;}return 0;
}
static void provider_release(void *context,const struct storage_node *node,void *pin)
{assert(context==&inode);if(node->type==RELIEFOS_FS_TYPE_FILE){assert(pin==&inode);assert(!storage_inode_put(pin));}else assert(!pin);}
static bool provider_same(void *context,const struct storage_node *left,const struct storage_node *right)
{(void)context;return left->type==right->type && left->first_cluster==right->first_cluster;}
static int provider_stat(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{
    assert(context==&inode && vfs_path_node(path)->type==RELIEFOS_FS_TYPE_FILE);
    if(!read_only_error && io_error)return io_error;
    *out=(struct linux_stat_abi){.st_size=(int64_t)provider_size,.st_mode=0100400};return 0;
}
static int provider_read(void *context,const struct storage_node *n,uint64_t offset,void *buffer,uint32_t length,uint32_t *got,uint64_t mount_flags)
{
    (void)mount_flags;
    assert(context==&inode && n->type==RELIEFOS_FS_TYPE_FILE && !async_requested);++reads;
    if(verify_snapshot_guard)assert(storage_inode_write_get(&inode)==-ETXTBSY);
    if(io_error)return io_error;
    if(short_eof){*got=0;return 0;}
    if(length>13)length=13;assert(offset+length<=sizeof(image));
    memcpy(buffer,image+offset,length);*got=length;return 0;
}
static void bind_provider(void)
{
    static const struct vfs_backend_ops ops={.lookup=provider_lookup,.retain=provider_retain,
        .release=provider_release,.same_node=provider_same,.getattr=provider_stat,.read=provider_read};
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&ops,&inode,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&file.node,&file.path_ref));vfs_mount_put(mount);
}
int module_load_image(const void *data,size_t size,const char *params,unsigned flags)
{
    if(verify_snapshot_guard) {
        /* Linux releases the file read guard before executing module init. */
        assert(!storage_inode_write_get(&inode));storage_inode_write_put(&inode);
    }
    ++calls;assert(!flags && size==sizeof(image));assert(data!=image);assert(!memcmp(data,image,size));
    assert(params && !strcmp(params,arguments));return load_status;
}
int module_unload(const char *name,unsigned flags){assert(!strcmp(name,"provider") && flags==0x800);++calls;return -11;}
int main(void)
{
#ifndef CONFIG_MODULES
    assert(syscall_modules(175,0,128,0)==-ENOSYS);
    assert(syscall_modules(313,99,0,0)==-ENOSYS);
    assert(syscall_modules(176,0,0,0)==-ENOSYS);
    assert(!copies && !calls && !reads && !heap_objects);
    puts("PASS module syscall disabled configuration returns ENOSYS before usercopy");
    return 0;
#endif
#ifndef CONFIG_MODULE_UNLOAD
    assert(syscall_modules(176,0,0,0)==-ENOSYS);
    assert(!copies && !calls);
    puts("PASS unload-disabled configuration returns ENOSYS before permission/usercopy");
    return 0;
#endif
    memset(image,0x5a,sizeof(image));
    file=(struct task_file){.used=1,.references=1,.offset=9,.node={.type=RELIEFOS_FS_TYPE_FILE,.size=sizeof(image)}};
    inode=(struct storage_inode_ref){.node=file.node,.references=1};file.inode=&inode;
    assert(syscall_modules(175,0,128,0)==-EPERM);
    assert(syscall_modules(313,99,0,0)==-EPERM);
    assert(syscall_modules(176,0,0,0)==-EPERM);
    assert(!copies && !calls && !reads && !heap_objects);
    current.cap_effective=1ULL<<CAP_SYS_MODULE;
    assert(syscall_modules(175,0,1,0)==-ENOEXEC);
    assert(syscall_modules(175,0,128,(uintptr_t)arguments)==-EFAULT);
    assert(!syscall_modules(175,(uintptr_t)image,sizeof(image),(uintptr_t)arguments));
    assert(calls==1 && !heap_objects);
    bind_provider();size_t backing_objects=heap_objects;uint64_t backing_references=inode.references;
    assert(syscall_modules(313,99,(uintptr_t)arguments,0)==-EBADF);
    assert(syscall_modules(313,7,(uintptr_t)arguments,8)==-EINVAL);
    file.flags=TASK_FILE_FLAG_PATH;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-EBADF);file.flags=LINUX_O_WRONLY;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-EBADF);file.flags=0;
    file.flags=LINUX_O_RDWR;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-ETXTBSY);file.flags=0;
    pin_failure=1;assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-ENOMEM);pin_failure=0;
    assert(!storage_inode_write_get(&inode));
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-ETXTBSY);
    storage_inode_write_put(&inode);verify_snapshot_guard=1;
    assert(!syscall_modules(313,7,(uintptr_t)arguments,0));
    assert(calls==2 && reads>1 && !pins && heap_objects==backing_objects && file.offset==9);
    provider_size=0;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-EINVAL);
    provider_size=63;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-ENOEXEC);
    provider_size=UINT64_C(1)<<31;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-EFBIG);
    provider_size=sizeof(image);
    assert(inode.references==backing_references);
    short_eof=1;assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-EIO);short_eof=0;
    io_error=-5;assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-EIO);io_error=0;
    load_status=-EAGAIN;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==-EAGAIN);load_status=0;
    /* Reserved native snapshot-retry result, consumed by syscall entry only. */
    io_error=-EAGAIN;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==MODULE_FILE_IO_RETRY);
    read_only_error=1;
    assert(syscall_modules(313,7,(uintptr_t)arguments,0)==MODULE_FILE_IO_RETRY);
    io_error=read_only_error=0;
    assert(inode.references==backing_references+1 && storage_inode_write_get(&inode)==-ETXTBSY);
    module_file_cancel(&current);module_file_cancel(&current);
    assert(inode.references==backing_references && !storage_inode_write_get(&inode));storage_inode_write_put(&inode);
    assert(syscall_modules(313,7,(uintptr_t)arguments,MODULE_INIT_COMPRESSED_FILE)==-EOPNOTSUPP);
    assert(syscall_modules(313,7,(uintptr_t)arguments,MODULE_INIT_IGNORE_VERMAGIC)==-ENOEXEC);
    char name[4096]={0};strcpy(name,"provider");
    assert(syscall_modules(176,(uintptr_t)name,0x800,0)==-EWOULDBLOCK);
    assert(heap_objects==backing_objects && !pins && inode.references==backing_references);
    vfs_path_put(&file.path_ref);assert(!heap_objects && inode.references==1);
    puts("PASS module syscall ABI: capability-before-usercopy, owned copies, flags, stable OFD, short reads, I/O errors, positional finit_module");
}
