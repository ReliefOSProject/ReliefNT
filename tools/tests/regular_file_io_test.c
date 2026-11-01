#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../kernel/syscall/io/file_io.c"
#include "../../kernel/syscall/io/vector_internal.h"
#include <reliefnt/mount.h>
#include <reliefnt/module.h>
#include <linux/mount.h>

static unsigned char disk[1048576], data[1048576];
static unsigned char *fixture_user, *fixture_alias;
static unsigned fixture_refs;
static bool fixture_device,fixture_readonly;
static uint64_t fixture_device_cache_flags;
static bool fixture_expect_drain, fixture_drained;

/** @brief Translate the simulated current user page to its retained alias.
 * @param as Unused fixture address space. @param va User virtual address.
 * @return Aligned original-frame identity for the modeled mapped page.
 */
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t va)
{ (void)as; return (va & ~4095ULL) == (uintptr_t)fixture_user ? (uintptr_t)fixture_alias : 0; }

/** @brief Report fixture page flags. @param as Unused root. @param va User address.
 * @return User, present and writable flags for the modeled page.
 */
uint64_t address_space_user_page_flags(const struct address_space *as, uint64_t va)
{ (void)as; (void)va; return RELIEFNT_PAGE_USER | RELIEFNT_PAGE_PRESENT |
    (fixture_readonly ? 0 : RELIEFNT_PAGE_WRITABLE) | (fixture_device ? RELIEFNT_PAGE_DEVICE : 0) |
    fixture_device_cache_flags; }

/** @brief Return a direct alias for fixture physical bytes. @param phys Address.
 * @return The host pointer modeling the physical direct map.
 */
void *mm_phys_to_virt(uint64_t phys) { return (void *)(uintptr_t)phys; }
/** @brief Retain a modeled frame. @param phys Original frame identity. */
void mm_retain_page(uint64_t phys) { assert(phys == (uintptr_t)fixture_alias); ++fixture_refs; }
/** @brief Release a modeled frame. @param phys Original frame identity. */
void mm_free_page(uint64_t phys)
{ assert(phys == (uintptr_t)fixture_alias && fixture_refs && (!fixture_expect_drain || fixture_drained)); --fixture_refs; }
/** @brief Snapshot fixture frame owners. @param phys Frame identity. @return Owner count. */
uint32_t mm_page_refcount(uint64_t phys) { return phys == (uintptr_t)fixture_alias ? fixture_refs : 0; }
/** @brief Find no cache-owned fixture frame. @param phys Frame identity. @return Negative. */
int page_cache_retain(uint64_t phys) { (void)phys; return -1; }
/** @brief Reject a cache release in this non-cache fixture. @param phys Frame identity. */
void page_cache_release(uint64_t phys) { (void)phys; abort(); }
/** @brief Validate the modeled resident range. @param address User start. @param len Length.
 * @return True when the complete interval lies within the fixture page.
 */
bool user_range_ok(uint64_t address, uint64_t len)
{ return address >= (uintptr_t)fixture_user && len <= 4096 && address - (uintptr_t)fixture_user <= 4096 - len; }
/** @brief Validate the modeled writable range. @param address Start. @param len Length.
 * @return True for the fixture page.
 */
bool user_range_writable(uint64_t address, uint64_t len) { return !fixture_readonly && user_range_ok(address, len); }
static uint64_t disk_size = sizeof(disk), error_offset = UINT64_MAX;
static int injected_error;
static uint64_t clock_step, clock_us;
static unsigned disk_syncs, volume_syncs;
static int sync_error;
int storage_sync_disk(uint32_t id) { (void)id; ++disk_syncs; return sync_error; }
int storage_sync_volume(uint32_t id) { (void)id; abort(); }
uint64_t time_uptime_us(void) { clock_us += clock_step; return clock_us; }
void storage_release_task_io(uint32_t pid) { assert(pid == 10); }
/** @brief Drain the fixture provider before releasing a staged chunk.
 * @param pid Fixture task identity.
 */
void storage_drain_task_io(uint32_t pid) { assert(pid == 10); fixture_drained = true; }
static int transfer(uint64_t offset, void *buffer, uint32_t length, uint32_t *done, bool writing)
{
    *done = 0;
    if (offset == error_offset) {
        int ret = injected_error;
        error_offset = UINT64_MAX;
        return ret;
    }
    uint64_t limit=writing ? sizeof(disk) : disk_size;
    if(offset>=limit)return 0;
    if(length>limit-offset)length=(uint32_t)(limit-offset);
    if (writing)
        memcpy(disk + offset, buffer, length);
    else
        memcpy(buffer, disk + offset, length);
    if(writing && offset+length>disk_size)disk_size=offset+length;
    *done = length;
    return 0;
}
int storage_read_node_cursor(const struct storage_node *node, uint64_t offset, void *buffer, uint32_t length,
                             uint32_t *done, struct storage_read_cursor *cursor)
{
    (void)node;(void)offset;(void)buffer;(void)length;(void)done;(void)cursor;fputs("unexpected legacy inode read\n",stderr);abort();
}
int storage_write_held_node(struct storage_node *node, uint64_t offset, const void *buffer, uint32_t length,
                            uint32_t *done)
{
    (void)node;(void)offset;(void)buffer;(void)length;(void)done;abort();
}
int storage_inode_refresh(struct storage_node *node)
{
    (void)node;abort();
}
int storage_disk_block_read(uint32_t disk_id, int32_t part, uint64_t offset, void *buffer, uint32_t length,
                            uint32_t *done)
{
    (void)disk_id;
    (void)part;
    return transfer(offset, buffer, length, done, false);
}
int storage_disk_block_write(uint32_t disk_id, int32_t part, uint64_t offset, const void *buffer,
                             uint32_t length, uint32_t *done)
{
    (void)disk_id;
    (void)part;
    return transfer(offset, (void *)buffer, length, done, true);
}

/* The syscall owns a real path/provider. Bare inode I/O entry points
 * above are poisoned so a diagnostic-name fallback cannot satisfy this test. */
static unsigned provider_pins,provider_reads,provider_writes,cursor_reads;
static struct storage_read_cursor *expected_cursor;
static bool provider_readonly,last_data_only;
static bool capture_enabled;
static unsigned captures,capture_reads,capture_closes;
void *kernel_malloc(size_t size){return calloc(1,size);}
void kernel_free(void *memory){free(memory);}
static int provider_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{(void)context;(void)parent;(void)name;(void)out;return -LINUX_ENOENT;}
static int provider_retain(void *context,const struct storage_node *node,void **pin)
{assert(context==disk && node && pin);++provider_pins;*pin=NULL;return 0;}
static void provider_release(void *context,const struct storage_node *node,void *pin)
{assert(context==disk && node && !pin && provider_pins);--provider_pins;}
static bool provider_same(void *context,const struct storage_node *a,const struct storage_node *b)
{assert(context==disk);return a->first_cluster==b->first_cluster;}
static int provider_read(void *context,const struct storage_node *node,uint64_t offset,void *buffer,uint32_t length,uint32_t *read,uint64_t mount_flags)
{(void)mount_flags;assert(context==disk && node && provider_pins);++provider_reads;return transfer(offset,buffer,length,read,false);}
static int provider_read_cursor(void *context,const struct storage_node *node,uint64_t offset,
                                   void *buffer,uint32_t length,uint32_t *read,struct storage_read_cursor *cursor,uint64_t mount_flags)
{
    assert(cursor==expected_cursor);++cursor_reads;
    return provider_read(context,node,offset,buffer,length,read,mount_flags);
}
static int provider_write(void *context,struct storage_node *node,uint64_t offset,const void *buffer,uint32_t length,uint32_t *written)
{
    assert(context==disk && node && provider_pins);++provider_writes;
    int rc=transfer(offset,(void *)buffer,length,written,true);
    node->first_cluster=9;node->size=disk_size;return rc;
}
static int provider_stat(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{assert(context==disk && vfs_path_node(path));*out=(struct linux_stat_abi){.st_size=(int64_t)disk_size};return 0;}
static uint64_t provider_flags(void *context){assert(context==disk);return provider_readonly ? MS_RDONLY : 0;}
static int provider_sync(void *context,const struct storage_node *node,bool filesystem,bool data_only)
{assert(context==disk && node && !filesystem);++volume_syncs;last_data_only=data_only;return sync_error;}
static int provider_open(void *context,const struct storage_node *node,uint32_t flags,void **out)
{
    assert(context==disk && node);(void)flags;*out=NULL;
    if(capture_enabled){*out=kernel_malloc(14);assert(*out);memcpy(*out,"captured view",14);++captures;}
    return 0;
}
static int provider_open_read(void *context,const struct storage_node *node,void *state,
                                  uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags)
{
    (void)mount_flags;
    assert(context==disk && node && state);++capture_reads;*read=0;
    if(offset<13){*read=13-offset<capacity ? (uint32_t)(13-offset) : capacity;memcpy(buffer,(char *)state+offset,*read);}
    return 0;
}
static void provider_close(void *context,const struct storage_node *node,void *state)
{assert(context==disk && node && state);++capture_closes;kernel_free(state);}
static const struct vfs_backend_ops provider_ops={.lookup=provider_lookup,.retain=provider_retain,
    .release=provider_release,.same_node=provider_same,.read=provider_read,.read_cursor=provider_read_cursor,.write=provider_write,
    .getattr=provider_stat,.mount_flags=provider_flags,.sync=provider_sync,
    .file_open=provider_open,.file_read=provider_open_read,.file_close=provider_close};

int main(void)
{
    struct task *task = calloc(1, sizeof(*task)), *other = calloc(1, sizeof(*other));
    struct task_file file = {.used = 1, .flags = RELIEFOS_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=2}};
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&provider_ops,disk,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&file.node,&file.path_ref));vfs_mount_put(mount);
    strcpy(file.path,"/a/diagnostic/name/that/was/replaced");expected_cursor=&file.read_cursor;
    assert(task && other);
    task->pid = 10;
    other->pid = 11;
    task->syscall_file = other->syscall_file = &file;
    task->syscall_file_number = other->syscall_file_number = LINUX_SYS_READ;
    for (unsigned i = 0; i < sizeof(disk); ++i)
        disk[i] = i * 13 + i / 257;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == sizeof(data));
    assert(!memcmp(data, disk, sizeof(data)) && file.offset == sizeof(data));
    file.offset = 0;
    memset(data, 0, sizeof(data));
    error_offset = 65536;
    injected_error = -LINUX_EAGAIN;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == -LINUX_EAGAIN);
    assert(file.offset == 65536 && task->regular_io.done == 65536 && file.io_owner == 10);
    assert(syscall_regular_io(other, &file, (uintptr_t)data, 1, 0, false, false) == -LINUX_EAGAIN);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == sizeof(data));
    assert(!memcmp(data, disk, sizeof(data)) && !file.io_owner);
    file.offset = 0;
    error_offset = 98304;
    injected_error = -LINUX_EIO;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, false, false) == 98304);
    assert(file.offset == 98304 && !file.io_owner);
    error_offset = file.offset;
    injected_error = -LINUX_EIO;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 10, 0, false, false) == -LINUX_EIO);
    file.offset = disk_size - 17;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 100, 0, false, false) == 17);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 100, 0, false, false) == 0);
    file.offset = 123;
    task->syscall_file_number = LINUX_SYS_PWRITE64;
    memset(data, 0xa5, sizeof(data));
    error_offset = 32768;
    injected_error = -LINUX_EAGAIN;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, true, true) == -LINUX_EAGAIN);
    assert(file.offset == 123);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, true, true) == sizeof(data));
    assert(!memcmp(data, disk, sizeof(disk)) && file.offset == 123);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 2, INT64_MAX, true, true) == -LINUX_EINVAL);
    clock_step = 2000;
    unsigned yields = 0;
    int64_t result;
    do {
        result = syscall_regular_io(task, &file, (uintptr_t)data, sizeof(data), 0, true, true);
        if (result == -LINUX_EAGAIN) ++yields;
    } while (result == -LINUX_EAGAIN && yields < 40);
    assert(result == sizeof(data) && yields == 31);
    assert(!memcmp(data, disk, sizeof(data)) && file.offset == 123);
    assert(!task->regular_io.active);
    clock_step = 0;
    /* A completed vector element is not the end of its parent operation.
     * Another execution context must not interleave on the same OFD. */
    const uint64_t vectors[]={LINUX_SYS_READV,LINUX_SYS_WRITEV,LINUX_SYS_PREADV,
        LINUX_SYS_PWRITEV,LINUX_SYS_PREADV2,LINUX_SYS_PWRITEV2};
    struct task_vector_state vector_state={0};
    for(unsigned i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i) {
        task->vector_io=&vector_state;task->syscall_file_number=vectors[i];
        file.offset=i>=2 ? 123 : 0;file.io_owner=task->pid;
        assert(syscall_regular_io(task,&file,(uintptr_t)data,3,0,i&1,i>=2)==3);
        assert(syscall_regular_io(other,&file,(uintptr_t)(data+10),1,0,false,false)==-LINUX_EAGAIN);
        assert(syscall_regular_io(task,&file,(uintptr_t)(data+3),3,3,i&1,i>=2)==3);
        assert(file.offset==(i>=2 ? 123 : 6));
        task->vector_io=NULL;file.io_owner=0; /* Parent vector finishes here. */
        assert(syscall_regular_io(other,&file,(uintptr_t)(data+10),1,0,false,false)==1);
    }
    file.offset=123;
    task->syscall_file_number = LINUX_SYS_PREAD64;
    file.flags |= TASK_FILE_FLAG_DEV_BLOCK | TASK_FILE_FLAG_DEV_NODE;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 2, 510, false, true) == -LINUX_EBADF);
    file.node.flags = STORAGE_NODE_FLAG_DEV_BLOCK | STORAGE_NODE_FLAG_DEV_NODE;
    file.node.first_cluster = STORAGE_DEV_KIND_DISK;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 2, 510, false, true) == 2);
    assert(!memcmp(data, disk + 510, 2));
    task->syscall_file_number = LINUX_SYS_PWRITE64;
    file.flags |= LINUX_O_SYNC;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 512, 0, true, true) == 512);
    assert(disk_syncs == 1 && volume_syncs == 0);
    file.flags &= ~(TASK_FILE_FLAG_DEV_BLOCK | TASK_FILE_FLAG_DEV_NODE);
    file.node=*vfs_path_node(&file.path_ref);
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 512, 0, true, true) == 512);
    assert(disk_syncs == 1 && volume_syncs == 1);
    sync_error = -LINUX_EIO;
    assert(syscall_regular_io(task, &file, (uintptr_t)data, 512, 0, true, true) == -LINUX_EIO);
    assert(!last_data_only);
    sync_error=0;file.flags=(file.flags&~LINUX_O_SYNC)|LINUX_O_DSYNC;
    assert(syscall_regular_io(task,&file,(uintptr_t)data,512,0,true,true)==512 && last_data_only);
    unsigned old_writes=provider_writes;provider_readonly=true;
    assert(syscall_regular_io(task,&file,(uintptr_t)data,1,0,true,true)==-LINUX_EROFS && provider_writes==old_writes);
    provider_readonly=false;file.flags=RELIEFOS_O_RDWR|RELIEFOS_O_APPEND;
    file.node.size=1;disk_size=200;file.offset=5;task->syscall_file_number=LINUX_SYS_WRITE;
    assert(syscall_regular_io(task,&file,(uintptr_t)data,3,0,true,false)==3 && file.offset==203);
    assert(!memcmp(disk+200,data,3) && file.node.first_cluster==9 && file.node.size==203);
    assert(provider_reads && provider_writes && cursor_reads==provider_reads && provider_pins==2);
    capture_enabled=true;
    assert(!vfs_file_open(&file.path_ref,LINUX_O_RDONLY,&file.provider_file) && file.provider_file && captures==1);
    unsigned old_reads=provider_reads;file.offset=0;task->syscall_file_number=LINUX_SYS_READ;
    memset(disk,'x',32);memset(data,0,32);
    assert(syscall_regular_io(task,&file,(uintptr_t)data,32,0,false,false)==13);
    assert(!memcmp(data,"captured view",13) && capture_reads==1 && provider_reads==old_reads);
    vfs_file_close(file.provider_file);file.provider_file=NULL;assert(capture_closes==1);
    /* A CLONE_VM peer can replace the VA after the provider has returned EAGAIN. */
    int shared = memfd_create("regular-read-original", 0);
    assert(shared >= 0 && !ftruncate(shared, 4096));
    fixture_user = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shared, 0);
    fixture_alias = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shared, 0);
    assert(fixture_user != MAP_FAILED && fixture_alias != MAP_FAILED);
    fixture_refs = 1; task->kind = TASK_KIND_USER;
    file.offset = 0; task->syscall_file_number = LINUX_SYS_READ;
    memset(disk, 0x71, 64); error_offset = 0; injected_error = -LINUX_EAGAIN;
    assert(syscall_regular_io(task, &file, (uintptr_t)fixture_user, 64, 0, false, false) == -LINUX_EAGAIN);
    assert(mmap(fixture_user, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == fixture_user);
    assert(syscall_regular_io(task, &file, (uintptr_t)fixture_user, 64, 0, false, false) == 64);
    for (unsigned i = 0; i < 64; ++i) assert(!fixture_user[i] && fixture_alias[i] == 0x71);
    assert(fixture_refs == 1);
    assert(mmap(fixture_user, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shared, 0) == fixture_user);
    memset(fixture_alias, 0x93, 64); file.flags = RELIEFOS_O_RDWR; file.offset = 128;
    task->syscall_file_number = LINUX_SYS_WRITE; error_offset = 128; injected_error = -LINUX_EAGAIN;
    assert(syscall_regular_io(task, &file, (uintptr_t)fixture_user, 64, 0, true, false) == -LINUX_EAGAIN);
    assert(mmap(fixture_user, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == fixture_user);
    memset(fixture_user, 0x32, 64);
    assert(syscall_regular_io(task, &file, (uintptr_t)fixture_user, 64, 0, true, false) == 64);
    for (unsigned i = 0; i < 64; ++i) assert(disk[128 + i] == 0x93 && fixture_user[i] == 0x32);
    assert(fixture_refs == 1);
    assert(mmap(fixture_user, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shared, 0) == fixture_user);
    file.offset = 0; task->syscall_file_number = LINUX_SYS_READ; error_offset = 0;
    assert(syscall_regular_io(task, &file, (uintptr_t)fixture_user, 64, 0, false, false) == -LINUX_EAGAIN);
    assert(fixture_refs == 2);
    fixture_expect_drain = true; fixture_drained = false;
    storage_drain_task_io(task->pid); task_regular_io_stage_cancel(task);
    assert(fixture_refs == 1 && fixture_drained); fixture_expect_drain = false;
    task->regular_io.active = false; file.io_owner = 0; close(shared);
    munmap(fixture_user, 4096); munmap(fixture_alias, 4096); task->kind = TASK_KIND_KERNEL;
    vfs_path_put(&file.path_ref);assert(!provider_pins);
    free(task);
    free(other);
    puts("PASS full file transfers, async continuation, shared offsets, EOF, error progress and genuine block-node validation");
    return 0;
}
