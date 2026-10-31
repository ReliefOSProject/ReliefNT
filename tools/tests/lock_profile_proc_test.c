#include <reliefnt/pseudo_fs.h>
#include <reliefnt/permissions.h>
/* Reuse the real proc/VFS fixture's environment, but exercise a fresh mount. */
#define main original_proc_tests_main
#include "procfs_directories_test.c"
#undef main

uint64_t lock_profile_test_clock(void) { return 0; }

static void test_control_mount(bool readonly)
{
    struct vfs_mount *mount = NULL;
    struct vfs_namespace ns = {0};
    struct vfs_path root = {0}, control = {0}, version = {0};
    struct reliefos_permissions mode;
    char all[8192], part[31];
    uint32_t got = 0, wrote = 99;
    assert(!proc_create_vfs_mount(&mount));
    if (readonly) assert(!vfs_mount_set_flags(mount, MS_RDONLY));
    assert(!vfs_namespace_init(&ns, mount)); vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns, &root));
    assert(!vfs_path_lookup(&ns, &root, &root, "reliefnt/lock_stats", 0, &control));
    assert(!vfs_path_lookup(&ns, &root, &root, "version", 0, &version));
    assert(!vfs_path_permissions(&control, &mode, false) && mode.mode == 0644 && mode.uid == 0);
    assert(!vfs_path_permissions(&version, &mode, false) && mode.mode == 0444);
    current.fsuid = 0; current.cap_effective = 1ULL << CAP_SYS_ADMIN;
    assert(fs_permissions_check_path(&current, &control, FS_ACCESS_WRITE, false) ==
           (readonly ? -RELIEFOS_EROFS : 0));
    if (!readonly) {
        assert(!vfs_path_truncate(&control, 0));
        assert(vfs_path_truncate(&control, 1) == -RELIEFOS_EINVAL);
        assert(vfs_path_truncate(&version, 0) == -RELIEFOS_EROFS);
        struct vfs_open_file *captured = NULL;
        assert(vfs_file_open(&version, LINUX_O_WRONLY, &captured) == -RELIEFOS_EROFS && !captured);
        assert(vfs_file_open(&version, LINUX_O_RDONLY | LINUX_O_TRUNC, &captured) == -RELIEFOS_EROFS && !captured);
        unsigned allocations = proc_allocations;
        proc_allocation_budget = 0;
        assert(!vfs_file_open(&control, LINUX_O_WRONLY, &captured) && !captured);
        proc_allocation_budget = -1;
        assert(proc_allocations == allocations);
        for (int budget = 0; budget < 3; ++budget) {
            proc_allocation_budget = budget;
            assert(vfs_file_open(&control, LINUX_O_RDONLY, &captured) == -RELIEFOS_ENOMEM && !captured);
            proc_allocation_budget = -1;
            assert(proc_allocations == allocations);
        }
        assert(!vfs_file_open(&control, 0, &captured) && captured);
        assert(!vfs_file_read(captured, 0, all, sizeof(all), &got) && got > 50);
        uint32_t n = 0;
        assert(!vfs_path_write(&control, 0, "1\n", 2, &wrote) && wrote == 2);
        assert(!vfs_file_read(captured, 0, part, sizeof(part), &n) && n == sizeof(part));
        assert(!memcmp(all, part, n));
        vfs_file_close(captured);
        assert(!vfs_path_read(&control, 0, all, sizeof(all), &got) && got > 50);
        assert(!vfs_path_read(&control, 7, part, sizeof(part), &n) && n == sizeof(part));
        assert(!memcmp(all + 7, part, n));
        assert(!vfs_path_read(&control, UINT64_MAX, part, sizeof(part), &n) && n == 0);
        assert(!vfs_path_write(&control, 0, "1\n", 2, &wrote) && wrote == 2);
        assert(vfs_path_write(&control, 1, "0", 1, &wrote) == -RELIEFOS_EINVAL && !wrote);
        assert(vfs_path_write(&control, 0, "bad", 3, &wrote) == -RELIEFOS_EINVAL && !wrote);
        current.fsuid = 0; current.cap_effective = 0;
        assert(vfs_path_write(&control, 0, "0", 1, &wrote) == -RELIEFOS_EPERM && !wrote);
        assert(vfs_path_truncate(&control, 0) == -RELIEFOS_EPERM);
        current.fsuid = 1000; current.cap_effective = 0;
        assert(fs_permissions_check_path(&current, &control, FS_ACCESS_WRITE, false) == -RELIEFOS_EACCES);
        assert(vfs_path_write(&control, 0, "0", 1, &wrote) == -RELIEFOS_EPERM && !wrote);
        current.cap_effective = 1ULL << CAP_DAC_OVERRIDE;
        assert(!fs_permissions_check_path(&current, &control, FS_ACCESS_WRITE, false));
        assert(vfs_path_write(&control, 0, "0", 1, &wrote) == -RELIEFOS_EPERM && !wrote);
        assert(vfs_path_write(&version, 0, "0", 1, &wrote) == -RELIEFOS_EROFS);
        current.fsuid = 0; current.cap_effective = 1ULL << CAP_SYS_ADMIN;
        assert(!vfs_path_remount(&root, MS_RDONLY));
        assert(fs_permissions_check_path(&current, &control, FS_ACCESS_WRITE, false) == -RELIEFOS_EROFS);
        assert(vfs_path_write(&control, 0, "0", 1, &wrote) == -RELIEFOS_EROFS);
        assert(!vfs_path_remount(&root, 0));
        assert(!vfs_path_write(&control, 0, "0", 1, &wrote) && wrote == 1);
    } else {
        assert(vfs_path_write(&control, 0, "0", 1, &wrote) == -RELIEFOS_EROFS && !wrote);
        assert(vfs_path_truncate(&control, 0) == -RELIEFOS_EROFS);
    }
    vfs_path_put(&version); vfs_path_put(&control); vfs_path_put(&root);
    vfs_namespace_destroy(&ns);
    current.fsuid = 1000; current.cap_effective = 0;
}
static void test_unrelated_generated_remount(void)
{
    static const struct vfs_pseudo_provider provider = {
        .root = "/proc", .filesystem = "test-other", .lookup = proc_lookup,
    };
    struct vfs_mount *mount = NULL;
    struct vfs_namespace ns = {0};
    struct vfs_path root = {0};
    assert(!vfs_pseudo_mount_new(&provider, &mount));
    assert(!vfs_namespace_init(&ns, mount)); vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns, &root));
    assert(vfs_path_remount(&root, MS_RDONLY) == -RELIEFOS_EOPNOTSUPP);
    vfs_path_put(&root); vfs_namespace_destroy(&ns);
}
int main(void)
{
    struct storage_node node;
    assert(!proc_lookup("/proc/reliefnt/lock_stats", &node) && "lock_stats must exist when configured");
    char probe[8];
    assert(!proc_read("/proc/reliefnt/lock_stats", 0, probe, sizeof(probe), NULL));
    test_control_mount(false); test_control_mount(true); test_unrelated_generated_remount();
    puts("PASS lock_stats proc lookup, access, offsets, control and read-only mounts");
}
