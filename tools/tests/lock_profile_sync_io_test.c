/* Real regular/vector I/O through the retained proc and VFS providers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/pseudo_fs.h>
#include <reliefnt/permissions.h>
#define main original_proc_fixture_main
#include "procfs_directories_test.c"
#undef main
#include "../../kernel/syscall/io/file_io.c"
#include "../../kernel/syscall/io/vector.c"
#include "../../kernel/syscall/io/transfer.c"
#include "../../kernel/syscall/fs/sync.c"

static struct task_file *admitted_file;
struct task_file *task_file_for_io(struct task *task, int fd)
{ assert(task == &current && fd == 3); return admitted_file; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; abort(); }
uint64_t lock_profile_test_clock(void) { return 0; }
uint64_t time_uptime_us(void) { return 0; }
bool user_range_ok(uint64_t p, uint64_t n) { return p || !n; }
bool user_range_writable(uint64_t p, uint64_t n) { return p || !n; }
void storage_release_task_io(uint32_t pid) { (void)pid; }
void storage_drain_task_io(uint32_t pid) { (void)pid; }
void kernel_wait_queue_remove(struct kernel_wait_queue *q, struct task *t) { (void)q; (void)t; }
void task_file_put(struct task_file *file) { (void)file; }
int user_copy_to_task(struct task *task, uint64_t address, const void *source, uint64_t size)
{ (void)task; (void)address; (void)source; (void)size; abort(); }
int storage_sync_disk(uint32_t d) { (void)d; abort(); }
int storage_disk_block_read(uint32_t d, int32_t p, uint64_t o, void *b, uint32_t n, uint32_t *r)
{ (void)d; (void)p; (void)o; (void)b; (void)n; (void)r; abort(); }
int storage_disk_block_write(uint32_t d, int32_t p, uint64_t o, const void *b, uint32_t n, uint32_t *r)
{ (void)d; (void)p; (void)o; (void)b; (void)n; (void)r; abort(); }

static bool enabled(void)
{
    char data[512] = {0}; uint32_t got;
    assert(!lock_profile_read(0, data, sizeof(data)-1, &got));
    return strstr(data, "enabled=1") != NULL;
}
static int64_t write_value(struct task_file *file, const char *value, uint32_t length)
{
    return syscall_regular_io(&current, file, (uintptr_t)value, length, 0, true, false);
}
static int64_t write_vector(struct task_file *file, const char *suffix)
{
    struct iovec v[] = {{.iov_base=(void *)"1", .iov_len=1},
                        {.iov_base=(void *)suffix, .iov_len=1}};
    uint64_t raw[] = {3, (uintptr_t)v, 2, 0, 0, 0};
    file->offset = 0; file->io_owner = 0; current.syscall_file = file;
    current.syscall_file_number = LINUX_SYS_WRITEV;
    int64_t rc = regular_vector_io(&current, file, LINUX_SYS_WRITEV, raw, true, false, 0);
    current.syscall_file = NULL;
    return rc;
}
static int fake_write(const char *name, uint64_t offset, const void *buffer,
                      uint32_t length, uint32_t *written)
{
    (void)name; (void)offset; (void)buffer; *written = length; return 0;
}
static void test_non_proc_provider_is_not_exempt(void)
{
    static const struct vfs_pseudo_provider other = {
        .root="/proc", .filesystem="fixture", .lookup=proc_lookup, .write=fake_write,
    };
    struct vfs_mount *mount = NULL; struct vfs_namespace ns = {0};
    struct vfs_path root = {0}, path = {0};
    assert(!vfs_pseudo_mount_new(&other, &mount));
    assert(!vfs_namespace_init(&ns, mount)); vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns, &root));
    assert(!vfs_path_lookup(&ns, &root, &root, "reliefnt/lock_stats", 0, &path));
    struct task_file file = {.flags=LINUX_O_WRONLY|LINUX_O_DSYNC, .node=*vfs_path_node(&path)};
    vfs_path_get(&path, &file.path_ref);
    assert(write_value(&file, "1", 1) == -LINUX_EINVAL && file.offset == 1);
    assert(vfs_path_sync(&path, false, false) == -LINUX_EINVAL);
    vfs_path_put(&file.path_ref); vfs_path_put(&path); vfs_path_put(&root);
    vfs_namespace_destroy(&ns);
}
int main(void)
{
    struct vfs_mount *mount = NULL; struct vfs_namespace ns = {0};
    struct vfs_path root = {0}, control = {0}, version = {0}; struct vfs_open_file *view = NULL;
    assert(!proc_create_vfs_mount(&mount));
    assert(!vfs_namespace_init(&ns, mount)); vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns, &root));
    assert(!vfs_path_lookup(&ns, &root, &root, "reliefnt/lock_stats", 0, &control));
    assert(!vfs_path_lookup(&ns, &root, &root, "version", 0, &version));
    current.fsuid = 0; current.cap_effective = 1ULL << CAP_SYS_ADMIN;
    assert(!fs_permissions_check_path(&current, &control, FS_ACCESS_WRITE, false));
    struct task_file file = {.node=*vfs_path_node(&control)};
    vfs_path_get(&control, &file.path_ref);
    const uint32_t sync_flags[] = {LINUX_O_DSYNC, LINUX_O_SYNC};
    for (unsigned i = 0; i < sizeof(sync_flags)/sizeof(sync_flags[0]); ++i) {
        assert(!lock_profile_control("0", 1)); file.offset=0; file.io_owner=0;
        file.flags=LINUX_O_WRONLY|sync_flags[i];
        assert(!vfs_file_open(&control, file.flags, &view) && !view);
        current.cap_effective = 0;
        assert(write_value(&file, "1", 1) == -LINUX_EPERM && file.offset == 0 && !enabled());
        current.cap_effective = 1ULL << CAP_SYS_ADMIN;
        assert(!vfs_path_remount(&root, MS_RDONLY));
        assert(write_value(&file, "1", 1) == -LINUX_EROFS && file.offset == 0 && !enabled());
        assert(!vfs_path_remount(&root, 0));
        assert(write_value(&file, "bad", 3) == -LINUX_EINVAL && file.offset == 0 && !enabled());
        assert(write_value(&file, "1", 1) == 1 && file.offset == 1 && enabled());
        file.offset=0;
        assert(write_value(&file, "bad", 3) == -LINUX_EINVAL && file.offset == 0 && enabled());
        assert(vfs_path_sync(&control, false, false) == -LINUX_EINVAL);
        assert(vfs_path_sync(&version, false, false) == -LINUX_EINVAL);
        admitted_file = &file;
        assert(syscall_sync_object(&ns, &current, LINUX_SYS_FSYNC, 3) == -LINUX_EINVAL);
        assert(syscall_sync_object(&ns, &current, LINUX_SYS_FDATASYNC, 3) == -LINUX_EINVAL);
        struct task_transfer_state *transfer = kernel_malloc(sizeof(*transfer));
        assert(transfer);
        *transfer = (struct task_transfer_state){.input=&file, .output=&file, .total=1};
        current.transfer_io = transfer;
        assert(transfer_complete(&current) == 1 && !current.transfer_io);
        assert(!lock_profile_control("0", 1));
        assert(write_vector(&file, "x") == 1 && file.offset == 1 && enabled());
        assert(!lock_profile_control("0", 1));
        assert(write_vector(&file, "\n") == 1 && file.offset == 1 && enabled());
    }
    vfs_path_put(&file.path_ref); vfs_path_put(&version); vfs_path_put(&control); vfs_path_put(&root);
    vfs_namespace_destroy(&ns);
    test_non_proc_provider_is_not_exempt();
    puts("PASS synchronous control writes, explicit fsync, permissions and short writev");
}
