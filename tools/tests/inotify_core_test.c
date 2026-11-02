/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/fsnotify.h>
#include <reliefnt/mount.h>
#include <linux/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct inotify_event) == 16, "event header ABI");
_Static_assert(offsetof(struct inotify_event, mask) == 4, "mask ABI");
_Static_assert(offsetof(struct inotify_event, cookie) == 8, "cookie ABI");
_Static_assert(offsetof(struct inotify_event, len) == 12, "name length ABI");
_Static_assert(offsetof(struct inotify_event, name) == 16, "name ABI");
_Static_assert(IN_ACCESS == 0x00000001U && IN_ALL_EVENTS == 0x00000fffU,
               "inotify event bit ABI");
_Static_assert(IN_UNMOUNT == 0x00002000U && IN_Q_OVERFLOW == 0x00004000U &&
               IN_IGNORED == 0x00008000U && IN_EXCL_UNLINK == 0x04000000U &&
               IN_MASK_CREATE == 0x10000000U && IN_MASK_ADD == 0x20000000U &&
               IN_ISDIR == 0x40000000U && IN_ONESHOT == 0x80000000U,
               "inotify flag ABI");
_Static_assert(INOTIFY_IOC_SETNEXTWD == 0x40044900U, "inotify ioctl ABI");

static long allocation_budget = -1;
static unsigned allocations;
static unsigned wakeups;

void *kernel_malloc(size_t size)
{
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    void *p = calloc(1, size);
    if (p) ++allocations;
    return p;
}

void kernel_free(void *p)
{
    if (p) { assert(allocations); --allocations; free(p); }
}

static void wake(void *context)
{
    assert(context == &wakeups);
    ++wakeups;
}

static bool same_node(void *context, const struct storage_node *a,
                      const struct storage_node *b)
{
    (void)context;
    return a->volume_id == b->volume_id && a->inode_generation == b->inode_generation;
}
static const struct vfs_backend_ops operations = { .same_node = same_node };
static const struct vfs_backend_ops other_operations = { .same_node = same_node };
static int domain_a, domain_b;

static struct fsnotify_key key(uint32_t volume, uint32_t generation, uint32_t type)
{
    struct fsnotify_key k = { .ops = &operations, .context = &domain_a,
        .node = { .volume_id = volume, .inode_generation = generation, .type = type } };
    return k;
}

static struct fsnotify_instance *instance(uint32_t uid)
{
    struct fsnotify_instance *i = NULL;
    assert(fsnotify_create(uid, wake, &wakeups, &i) == 0 && i);
    return i;
}

static void expect_event(struct fsnotify_instance *i, int wd, uint32_t mask,
                         uint32_t cookie, const char *name, uint32_t padded)
{
    const struct inotify_event *event = NULL;
    size_t size = fsnotify_peek(i, &event);
    assert(size == 16 + padded && event);
    assert(event->wd == wd && event->mask == mask && event->cookie == cookie);
    assert(event->len == padded);
    if (name) {
        assert(strcmp(event->name, name) == 0);
        for (size_t n = strlen(name); n < padded; ++n) assert(event->name[n] == 0);
    } else assert(padded == 0);
    fsnotify_pop(i);
}

static void test_identity_masks_and_names(void)
{
    struct fsnotify_instance *i = instance(100);
    struct fsnotify_key a = key(3, 11, RELIEFOS_FS_TYPE_DIR);
    struct fsnotify_key alias = a;
    alias.node.entry_id = 99;
    int wd = fsnotify_add_watch(i, &a, IN_CREATE | IN_DELETE);
    assert(wd > 0);
    assert(fsnotify_add_watch(i, &alias, IN_CREATE | IN_MASK_CREATE) == -EEXIST);
    assert(fsnotify_add_watch(i, &alias, IN_MODIFY | IN_MASK_ADD) == wd);
    assert(fsnotify_add_watch(i, &a, IN_MASK_ADD | IN_MASK_CREATE | IN_CREATE) == -EINVAL);
    assert(fsnotify_add_watch(i, &a, 0) == -EINVAL);
    const uint32_t non_event_masks[] = { IN_ONESHOT, IN_ONLYDIR, IN_DONT_FOLLOW,
        IN_EXCL_UNLINK, IN_MASK_ADD, IN_UNMOUNT, IN_Q_OVERFLOW, IN_IGNORED, IN_ISDIR };
    for (size_t n = 0; n < sizeof(non_event_masks) / sizeof(non_event_masks[0]); ++n)
        assert(fsnotify_add_watch(i, &a, non_event_masks[n]) == wd);
    struct fsnotify_key only_create = key(3, 12, RELIEFOS_FS_TYPE_FILE);
    int create_wd = fsnotify_add_watch(i, &only_create, IN_MASK_CREATE);
    assert(create_wd > 0);
    assert(fsnotify_add_watch(i, &only_create, IN_MASK_CREATE) == -EEXIST);
    assert(fsnotify_remove_watch(i, create_wd) == 0);
    expect_event(i, create_wd, IN_IGNORED, 0, NULL, 0);
    assert(fsnotify_add_watch(i, &a, IN_CREATE | IN_UNMOUNT | IN_ISDIR) == wd);
    assert(fsnotify_add_watch(i, &a, IN_MODIFY | IN_MASK_ADD) == wd);
    assert(fsnotify_add_watch(i, &a, 0x00800000U | IN_CREATE) == -EINVAL);
    struct fsnotify_key different = alias;
    different.context = &domain_b;
    fsnotify_emit(&different, IN_CREATE, 10, "alpha", false);
    different = alias; different.ops = &other_operations;
    fsnotify_emit(&different, IN_CREATE, 10, "alpha", false);
    assert(!fsnotify_ready(i));
    fsnotify_emit(&alias, IN_CREATE | IN_ISDIR, 77, "alpha", false);
    fsnotify_emit(&a, IN_CREATE | IN_ISDIR, 77, "alpha", false);
    fsnotify_emit(&a, IN_CREATE | IN_ISDIR, 78, "alpha", false);
    fsnotify_emit(&a, IN_MODIFY, 0, "abcdefghijklmnop", false);
    assert(fsnotify_pending_bytes(i) == 112);
    expect_event(i, wd, IN_CREATE | IN_ISDIR, 77, "alpha", 16);
    expect_event(i, wd, IN_CREATE | IN_ISDIR, 78, "alpha", 16);
    expect_event(i, wd, IN_MODIFY, 0, "abcdefghijklmnop", 32);
    assert(!fsnotify_ready(i) && fsnotify_pending_bytes(i) == 0);
    assert(fsnotify_add_watch(i, &a, IN_ACCESS) == wd);
    fsnotify_emit(&a, IN_MODIFY, 0, NULL, false);
    assert(!fsnotify_ready(i));
    fsnotify_emit(&a, IN_ACCESS, 0, NULL, false);
    expect_event(i, wd, IN_ACCESS, 0, NULL, 0);
    assert(fsnotify_remove_watch(i, wd) == 0);
    expect_event(i, wd, IN_IGNORED, 0, NULL, 0);
    assert(fsnotify_remove_watch(i, wd) == -EINVAL);
    fsnotify_put(i);
}

static void test_oneshot_exclusion_and_lifecycle(void)
{
    struct fsnotify_instance *i = instance(101);
    struct fsnotify_key dir = key(7, 20, RELIEFOS_FS_TYPE_DIR);
    struct fsnotify_key file = key(7, 21, RELIEFOS_FS_TYPE_FILE);
    file.node.flags = STORAGE_NODE_FLAG_FAT32;
    int wd = fsnotify_add_watch(i, &dir, IN_CREATE | IN_ONESHOT | IN_EXCL_UNLINK);
    assert(wd > 0);
    fsnotify_emit(&dir, IN_CREATE, 0, "hidden", true);
    assert(!fsnotify_ready(i));
    fsnotify_emit(&dir, IN_CREATE, 0, "visible", false);
    expect_event(i, wd, IN_CREATE, 0, "visible", 16);
    expect_event(i, wd, IN_IGNORED, 0, NULL, 0);
    assert(fsnotify_remove_watch(i, wd) == -EINVAL);
    int dwd = fsnotify_add_watch(i, &dir, IN_DELETE_SELF);
    int fwd = fsnotify_add_watch(i, &file, IN_ACCESS);
    assert(dwd > 0 && fwd > 0 && dwd != fwd);
    fsnotify_delete(&dir);
    expect_event(i, dwd, IN_DELETE_SELF, 0, NULL, 0);
    expect_event(i, dwd, IN_IGNORED, 0, NULL, 0);
    assert(fsnotify_storage_node_watched(&file.node));
    struct storage_node other_volume = file.node;
    other_volume.volume_id++;
    assert(!fsnotify_storage_node_watched(&other_volume));
    struct fsnotify_key synthetic = key(7, 22, RELIEFOS_FS_TYPE_FILE);
    synthetic.ops = &other_operations;
    int swd = fsnotify_add_watch(i, &synthetic, IN_ACCESS);
    assert(swd > 0);
    struct storage_node other_native = synthetic.node;
    other_native.flags = STORAGE_NODE_FLAG_FAT32;
    assert(!fsnotify_storage_node_watched(&other_native));
    assert(fsnotify_remove_watch(i, swd) == 0);
    expect_event(i, swd, IN_IGNORED, 0, NULL, 0);
    fsnotify_unmount(&operations, &domain_a);
    expect_event(i, fwd, IN_UNMOUNT, 0, NULL, 0);
    expect_event(i, fwd, IN_IGNORED, 0, NULL, 0);
    assert(!fsnotify_storage_node_watched(&file.node));
    assert(!fsnotify_ready(i));
    fsnotify_put(i);
}

static void test_overflow_and_allocation_failure(void)
{
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_QUEUED_EVENTS, 2) == 0);
    struct fsnotify_instance *i = instance(102);
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_QUEUED_EVENTS, 16384) == 0);
    struct fsnotify_key k = key(1, 1, RELIEFOS_FS_TYPE_FILE);
    int wd = fsnotify_add_watch(i, &k, IN_MODIFY);
    assert(wd > 0);
    fsnotify_emit(&k, IN_MODIFY, 1, NULL, false);
    fsnotify_emit(&k, IN_MODIFY, 2, NULL, false);
    fsnotify_emit(&k, IN_MODIFY, 2, NULL, false);
    assert(fsnotify_pending_bytes(i) == 32);
    fsnotify_emit(&k, IN_MODIFY, 3, NULL, false);
    fsnotify_emit(&k, IN_MODIFY, 4, NULL, false);
    assert(fsnotify_pending_bytes(i) == 48);
    expect_event(i, wd, IN_MODIFY, 1, NULL, 0);
    expect_event(i, wd, IN_MODIFY, 2, NULL, 0);
    expect_event(i, -1, IN_Q_OVERFLOW, 0, NULL, 0);
    allocation_budget = 0;
    fsnotify_emit(&k, IN_MODIFY, 5, NULL, false);
    expect_event(i, -1, IN_Q_OVERFLOW, 0, NULL, 0);
    allocation_budget = -1;
    fsnotify_emit(&k, IN_MODIFY, 6, NULL, false);
    expect_event(i, wd, IN_MODIFY, 6, NULL, 0);
    fsnotify_put(i);
}

static void test_limits_lifetime_and_faults(void)
{
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_INSTANCES, 1) == 0);
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_WATCHES, 1) == 0);
    struct fsnotify_instance *i = instance(103), *out = (void *)1;
    assert(fsnotify_create(103, wake, &wakeups, &out) == -EMFILE && !out);
    struct fsnotify_instance *other = instance(104);
    struct fsnotify_key a = key(1, 1, RELIEFOS_FS_TYPE_FILE), b = key(1, 2, RELIEFOS_FS_TYPE_FILE);
    allocation_budget = 0;
    assert(fsnotify_add_watch(i, &a, IN_ACCESS) == -ENOMEM);
    allocation_budget = -1;
    int wd = fsnotify_add_watch(i, &a, IN_ACCESS);
    assert(wd > 0 && fsnotify_add_watch(i, &b, IN_ACCESS) == -ENOSPC);
    assert(fsnotify_add_watch(other, &b, IN_ACCESS) > 0);
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_WATCHES, 0) == 0);
    assert(fsnotify_add_watch(i, &a, IN_MODIFY) == wd);
    fsnotify_get(i);
    fsnotify_put(i);
    assert(fsnotify_create(103, wake, &wakeups, &out) == -EMFILE);
    fsnotify_put(i);
    struct fsnotify_instance *next = instance(103);
    fsnotify_put(next); fsnotify_put(other);
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_INSTANCES, 128) == 0);
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_WATCHES, 8192) == 0);
    allocation_budget = 0;
    out = (void *)1;
    assert(fsnotify_create(105, wake, &wakeups, &out) == -ENOMEM && !out);
    allocation_budget = 1;
    assert(fsnotify_create(105, wake, &wakeups, &out) == -ENOMEM && !out);
    allocation_budget = 2;
    assert(fsnotify_create(105, wake, &wakeups, &out) == -ENOMEM && !out);
    allocation_budget = -1;
    struct fsnotify_instance *recovered = instance(105);
    struct fsnotify_instance *same_uid = instance(105);
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_WATCHES, 1) == 0);
    int shared_wd = fsnotify_add_watch(recovered, &a, IN_ACCESS);
    assert(shared_wd > 0);
    assert(fsnotify_add_watch(same_uid, &b, IN_ACCESS) == -ENOSPC);
    fsnotify_put(recovered);
    assert(fsnotify_add_watch(same_uid, &b, IN_ACCESS) > 0);
    fsnotify_put(same_uid);
    assert(fsnotify_limit_set(FSNOTIFY_LIMIT_WATCHES, 8192) == 0);
    assert(fsnotify_next_cookie() != 0);
    uint32_t c = fsnotify_next_cookie();
    assert(c != 0 && fsnotify_next_cookie() != c);
}

int main(void)
{
    assert(sizeof(struct inotify_event) == 16);
    test_identity_masks_and_names();
    test_oneshot_exclusion_and_lifecycle();
    test_overflow_and_allocation_failure();
    test_limits_lifetime_and_faults();
    assert(wakeups > 0 && allocations == 0);
    puts("inotify core: all tests passed");
    return 0;
}
