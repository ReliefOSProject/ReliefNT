/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: shmem scalar user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_scalar_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "task8_scalar_user_test.c"
#pragma clang diagnostic pop
#undef main
#include "../../kernel/ipc/shm.c"

static struct task_shm segment;
static struct kernel_object_table objects;
static unsigned char backing[65536];
struct kernel_object_table *kernel_objects(void) { return &objects; }
void *kernel_object_lookup(struct kernel_object_table *table,uint32_t handle,enum kernel_object_type type)
{ assert(table==&objects && handle==7 && type==KERNEL_OBJECT_DEVICE);return &segment; }
int storage_read_node(const struct storage_node *node,uint64_t offset,void *buf,uint32_t length,uint32_t *got)
{ assert(node==&segment.node && offset<=sizeof(backing) && length<=sizeof(backing)-offset);
  memcpy(buf,backing+offset,length);*got=length;return 0; }
int storage_write_held_node(struct storage_node *node,uint64_t offset,const void *buf,uint32_t length,uint32_t *written)
{ assert(node==&segment.node && offset<=sizeof(backing) && length<=sizeof(backing)-offset);
  memcpy(backing+offset,buf,length);*written=length;return 0; }
int main(void)
{
    current.kind=TASK_KIND_USER;current.syscall_file=&file;
    file.flags=TASK_FILE_FLAG_DEV_SHM|RELIEFOS_O_RDWR;file.aux=7;
    segment.node.size=sizeof(backing);memset(bytes,'a',sizeof(bytes));
    swapped=(uintptr_t)bytes;contended=true;
    assert(syscall_io_write(3,(uintptr_t)bytes,sizeof(bytes))==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!file.offset && !backing[0]);
    contended=false;assert(syscall_io_write(3,(uintptr_t)bytes,sizeof(bytes))==32768);
    assert(file.offset==32768 && backing[0]=='a' && !backing[32768]);
    file.offset=0;swapped=(uintptr_t)bytes;contended=true;
    assert(syscall_io_read(3,(uintptr_t)bytes,sizeof(bytes))==KERNEL_SYSCALL_SWAP_CONTENDED);
    contended=false;assert(syscall_io_read(3,(uintptr_t)bytes,sizeof(bytes))==32768);
    file.offset=0;segment.node.size=40000;segment.seals=LINUX_F_SEAL_GROW;
    assert(syscall_io_write(3,(uintptr_t)bytes,sizeof(bytes))==-RELIEFOS_EPERM);
    assert(!file.offset && segment.node.size==40000);
    puts("PASS shmem scalar: bounded resident slice and full-request grow seal admission");
    return 0;
}
