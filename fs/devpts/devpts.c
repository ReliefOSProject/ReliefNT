/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/devpts.h>
#include <reliefnt/heap.h>
#include <reliefnt/pty.h>
#include <reliefnt/sched.h>
#include <linux/errno.h>
#include <linux/mount.h>
#include <linux/stat.h>
#include <linux/statfs.h>
#include "../vfs/mount_internal.h"

#define DEVPTS_LIMIT (1u << 20)
#define DEVPTS_ROOT_INO 1u
#define DEVPTS_PTMX_INO 2u

struct devpts_options {
    uint32_t uid, gid, mode, ptmxmode, maximum;
    bool set_uid, set_gid;
};

struct devpts_instance;
struct devpts_inode {
    struct devpts_instance *instance;
    struct devpts_inode *next;
    uint64_t ino, references;
    uint32_t number, pty_id, generation;
    bool linked;
    struct reliefos_permissions permissions;
};

struct devpts_instance {
    struct devpts_inode root, ptmx, *slaves;
    struct devpts_options options;
    uint64_t references, next_inode, flags, device;
    uint32_t identity;
};

static const struct vfs_backend_ops devpts_ops;
static uint32_t devpts_next_instance;

/** @brief Encode a Linux dev_t with a full 20-bit minor.
 * @param major Linux device major. @param minor Linux device minor.
 * @return Encoded native Linux device number. */
static uint64_t devpts_device(uint32_t major, uint32_t minor)
{
    return ((uint64_t)major << 8) | (minor & 255u) |
           ((uint64_t)(minor & ~255u) << 12);
}

/** @brief Parse a bounded unsigned option without temporary strings.
 * @param begin First digit. @param end Exclusive token end.
 * @param base Decimal or octal radix. @param limit Largest accepted value.
 * @param out Parsed output on success.
 * @return Zero or negative EINVAL, without changing out on failure. */
static int devpts_unsigned(const char *begin, const char *end, uint32_t base,
                            uint32_t limit, uint32_t *out)
{
    if (begin == end) return -EINVAL;
    uint32_t value = 0;
    for (const char *p = begin; p != end; ++p) {
        uint32_t digit = (uint32_t)(*p - '0');
        if (digit >= base || value > (limit - digit) / base) return -EINVAL;
        value = value * base + digit;
    }
    if (value > limit) return -EINVAL;
    *out = value;
    return 0;
}

/** @brief Reconfigure a copied option set atomically.
 * @param text Nullable comma-separated kernel mount data.
 * @param options Caller-owned candidate, committed only by the caller.
 * @return Zero or negative EINVAL. All mounts are independent instances. */
static int devpts_parse(const char *text, struct devpts_options *options)
{
    if (!text || !*text) return 0;
    while (*text) {
        const char *end = text;
        while (*end && *end != ',') ++end;
        if (text == end) {
            text = end + 1;
            continue;
        }
        const char *equals = text;
        while (equals != end && *equals != '=') ++equals;
        size_t length = (size_t)(equals - text);
        uint32_t value;
        if (length == 11 && !__builtin_memcmp(text, "newinstance", 11) && equals == end) {
            /* Independent instances are unconditional, including old callers. */
        } else {
            if (equals == end) return -EINVAL;
            bool uid = length == 3 && !__builtin_memcmp(text, "uid", 3);
            bool gid = length == 3 && !__builtin_memcmp(text, "gid", 3);
            bool mode = length == 4 && !__builtin_memcmp(text, "mode", 4);
            bool ptmxmode = length == 8 && !__builtin_memcmp(text, "ptmxmode", 8);
            bool maximum = length == 3 && !__builtin_memcmp(text, "max", 3);
            if (!uid && !gid && !mode && !ptmxmode && !maximum) return -EINVAL;
            uint32_t limit = mode || ptmxmode ? 07777u : maximum ? DEVPTS_LIMIT : UINT32_MAX - 1u;
            int rc = devpts_unsigned(equals + 1, end, mode || ptmxmode ? 8 : 10, limit, &value);
            if (rc) return rc;
            if (uid) { options->uid = value; options->set_uid = true; }
            if (gid) { options->gid = value; options->set_gid = true; }
            if (mode) options->mode = value;
            if (ptmxmode) options->ptmxmode = value;
            if (maximum) options->maximum = value;
        }
        text = *end ? end + 1 : end;
    }
    return 0;
}

/** @brief Export an inode's generation-checked identity without taking a pin.
 * @param inode Borrowed live inode. @param out Borrowed metadata output. */
static void devpts_node(const struct devpts_inode *inode, struct storage_node *out)
{
    bool root = inode->ino == DEVPTS_ROOT_INO;
    *out = (struct storage_node){
        .type = root ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEVPTS | (root ? 0 : STORAGE_NODE_FLAG_RDEV | STORAGE_NODE_FLAG_DEV_NODE),
        .first_cluster = inode->pty_id,
        .volume_id = inode->generation,
        .mount_generation = inode->instance->identity,
        .entry_id = inode->ino,
    };
    if (inode->pty_id) out->flags |= STORAGE_NODE_FLAG_PTY;
}

/** @brief Resolve only identities owned by this instance, including unlinked pins.
 * @param instance Borrowed backend. @param node Borrowed metadata.
 * @return Borrowed inode or NULL for stale/foreign metadata. */
static struct devpts_inode *devpts_find(struct devpts_instance *instance,
                                       const struct storage_node *node)
{
    if (!node || !(node->flags & STORAGE_NODE_FLAG_DEVPTS) ||
        node->mount_generation != instance->identity) return NULL;
    if (node->entry_id == DEVPTS_ROOT_INO) return &instance->root;
    if (node->entry_id == DEVPTS_PTMX_INO) return &instance->ptmx;
    for (struct devpts_inode *inode = instance->slaves; inode; inode = inode->next) {
        if (inode->ino == node->entry_id && inode->pty_id == node->first_cluster &&
            inode->generation == node->volume_id) return inode;
    }
    return NULL;
}

/** @brief Drop a mount or slave's instance reference under execution ownership.
 * @param instance Owned reference, freed after its last inode and mount retire. */
static void devpts_instance_put(struct devpts_instance *instance)
{
    if (!--instance->references) kernel_free(instance);
}

/** @brief Consume a slave inode reference and its final PTY/instance pins.
 * @param inode Owned slave pin. Internal session identity survives until here. */
static void devpts_inode_put(struct devpts_inode *inode)
{
    if (--inode->references) return;
    struct devpts_instance *instance = inode->instance;
    struct devpts_inode **link = &instance->slaves;
    while (*link != inode) link = &(*link)->next;
    *link = inode->next;
    pty_transfer_put(inode->pty_id, TASK_PTY_ENDPOINT_SLAVE);
    kernel_free(inode);
    devpts_instance_put(instance);
}

/** @brief Remove the slave name when its last master hangs up.
 * @param binding Owned publication pin, consumed exactly once by PTY core. */
static void devpts_hangup(void *binding)
{
    struct devpts_inode *inode = binding;
    inode->linked = false;
    devpts_inode_put(inode);
}

/** @brief Lookup one currently published child in an execution-owned instance.
 * @param context Borrowed instance. @param parent Borrowed directory identity.
 * @param name Child basename. @param out Borrowed metadata output.
 * @return Zero or negative ENOTDIR/ENOENT. No lookup caching is enabled. */
static int devpts_lookup(void *context, const struct storage_node *parent,
                          const char *name, struct storage_node *out)
{
    struct devpts_instance *instance = context;
    if (devpts_find(instance, parent) != &instance->root) return -ENOTDIR;
    if (!__builtin_strcmp(name, "ptmx")) {
        devpts_node(&instance->ptmx, out);
        return 0;
    }
    uint32_t number;
    size_t length = __builtin_strlen(name);
    if ((length > 1 && name[0] == '0') ||
        devpts_unsigned(name, name + length, 10, DEVPTS_LIMIT - 1, &number)) return -ENOENT;
    for (struct devpts_inode *inode = instance->slaves; inode; inode = inode->next) {
        if (inode->number == number && inode->linked) {
            devpts_node(inode, out);
            return 0;
        }
    }
    return -ENOENT;
}

/** @brief Acquire an opaque native inode pin under execution ownership.
 * @param context Borrowed backend. @param node Borrowed metadata.
 * @param pin Owned native pin output on success.
 * @return Zero or negative ENOENT. The enclosing mount retains root/ptmx. */
static int devpts_retain(void *context, const struct storage_node *node, void **pin)
{
    struct devpts_inode *inode = devpts_find(context, node);
    if (!inode) return -ENOENT;
    if (inode->pty_id) ++inode->references;
    *pin = inode;
    return 0;
}

/** @brief Consume a native inode pin after its final dentry retires.
 * @param context Borrowed backend. @param node Borrowed metadata.
 * @param pin Owned pin returned by retain. Root/ptmx live in the mount. */
static void devpts_release(void *context, const struct storage_node *node, void *pin)
{
    (void)context;
    (void)node;
    struct devpts_inode *inode = pin;
    if (inode->pty_id) devpts_inode_put(inode);
}

/** @brief Compare immutable per-instance inode identities.
 * @param context Borrowed backend. @param left First metadata. @param right Second metadata.
 * @return True for the same live inode, including retained unlinked slaves. */
static bool devpts_same(void *context, const struct storage_node *left,
                         const struct storage_node *right)
{
    struct devpts_inode *inode = devpts_find(context, left);
    return inode && inode == devpts_find(context, right);
}

/** @brief Retire the mount's base reference, allowing legacy-open PTYs to drain.
 * @param context Owned backend mount reference after all VFS pins retire. */
static void devpts_destroy(void *context)
{
    devpts_instance_put(context);
}

/** @brief Read/update inode permissions after the caller's DAC authorization.
 * @param context Borrowed backend. @param path Borrowed retained inode.
 * @param value Read result or new metadata. @param write Select update.
 * @return Zero or negative ENOENT. Requires execution ownership. */
static int devpts_permissions(void *context, const struct vfs_path *path,
                               struct reliefos_permissions *value, bool write)
{
    struct devpts_inode *inode = devpts_find(context, vfs_path_node(path));
    if (!inode) return -ENOENT;
    if (write) inode->permissions = *value;
    else *value = inode->permissions;
    return 0;
}

/** @brief Return stable metadata and instance-local Linux device identity.
 * @param context Borrowed backend. @param path Retained actual inode.
 * @param out Writable stat output. @return Zero or negative ENOENT. */
static int devpts_getattr(void *context, const struct vfs_path *path, struct linux_stat_abi *out)
{
    struct devpts_instance *instance = context;
    struct devpts_inode *inode = devpts_find(instance, vfs_path_node(path));
    if (!inode) return -ENOENT;
    bool root = inode == &instance->root;
    *out = (struct linux_stat_abi){
        .st_dev = instance->device, .st_ino = inode->ino,
        .st_nlink = root ? 2 : inode->linked ? 1 : 0,
        .st_mode = (root ? LINUX_S_IFDIR : LINUX_S_IFCHR) | inode->permissions.mode,
        .st_uid = inode->permissions.uid, .st_gid = inode->permissions.gid,
        .st_rdev = root ? 0 : inode->pty_id ? devpts_device(136, inode->number) : devpts_device(5, 2),
        .st_blksize = 1024,
    };
    return 0;
}

/** @brief Check whether a slave still reserves an index and PTY quota.
 * @param inode Borrowed slave with its own session transfer pin.
 * @return True while published or held by an endpoint/controlling attachment.
 * Retained path metadata alone keeps internal identity alive and permits public
 * index reuse after hangup. Execution ownership stabilizes the reference scan. */
static bool devpts_reserved(const struct devpts_inode *inode)
{
    return inode->linked || sched_pty_reference_count(inode->pty_id) > 1;
}

/** @brief Count sessions reserving this instance's max quota.
 * @param instance Borrowed backend under execution ownership.
 * @return Published sessions plus hung-up sessions with live endpoints. */
static uint32_t devpts_allocated(const struct devpts_instance *instance)
{
    uint32_t count = 0;
    for (const struct devpts_inode *inode = instance->slaves; inode; inode = inode->next)
        if (devpts_reserved(inode)) ++count;
    return count;
}

/** @brief Report the devpts magic and currently reserved PTY quota.
 * @param context Borrowed backend. @param node Borrowed identity.
 * @param out Writable statfs result. @return Zero or negative ENOENT. */
static int devpts_statfs(void *context, const struct storage_node *node, struct linux_statfs_abi *out)
{
    struct devpts_instance *instance = context;
    if (!devpts_find(instance, node)) return -ENOENT;
    *out = (struct linux_statfs_abi){.f_type = 0x1cd1, .f_bsize = 1024,
        .f_files = instance->options.maximum, .f_namelen = 255};
    uint32_t allocated = devpts_allocated(instance);
    out->f_ffree = allocated < instance->options.maximum ?
        instance->options.maximum - allocated : 0;
    out->f_fsid[0] = (int32_t)instance->identity;
    return 0;
}

/** @brief Enumerate ptmx and published slaves using stable monotonic inode cookies.
 * @param context Borrowed backend. @param node Borrowed directory identity.
 * @param cursor Owned iteration cookie. @param entry Writable dirent result.
 * @param flags Current attachment flags, unused.
 * @return One for an entry, zero at end, or negative ENOTDIR. */
static int devpts_readdir(void *context, const struct storage_node *node, uint64_t *cursor,
                           struct reliefos_dir_entry *entry, uint64_t flags)
{
    (void)flags;
    struct devpts_instance *instance = context;
    if (devpts_find(instance, node) != &instance->root) return -ENOTDIR;
    struct devpts_inode *next = NULL;
    if (*cursor < DEVPTS_PTMX_INO) next = &instance->ptmx;
    else for (struct devpts_inode *inode = instance->slaves; inode; inode = inode->next) {
        if (inode->linked && inode->ino > *cursor && (!next || inode->ino < next->ino)) next = inode;
    }
    if (!next) return 0;
    *entry = (struct reliefos_dir_entry){.type = RELIEFOS_FS_TYPE_DEVICE};
    if (next == &instance->ptmx) __builtin_memcpy(entry->name, "ptmx", 5);
    else {
        char digits[10];
        uint32_t value = next->number, count = 0;
        do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
        for (uint32_t i = 0; i < count; ++i) entry->name[i] = digits[count - i - 1];
    }
    *cursor = next->ino;
    return 1;
}

/** @brief Return shared superblock policy without allocation or I/O.
 * @param context Borrowed instance. @return Shared policy flags. */
static uint64_t devpts_flags(void *context)
{
    return ((struct devpts_instance *)context)->flags;
}

/** @brief Atomically change options and shared mount policy.
 * @param context Borrowed instance. @param flags New shared policy flags.
 * @param options Optional copied mount data. @return Zero or negative EINVAL.
 * Unspecified options return to defaults and existing slaves retain metadata.
 * Execution ownership is required. */
static int devpts_remount(void *context, uint64_t flags, const char *options)
{
    struct devpts_instance *instance = context;
    struct devpts_options candidate = {.mode = 0600, .maximum = DEVPTS_LIMIT};
    int rc = devpts_parse(options, &candidate);
    if (rc) return rc;
    instance->options = candidate;
    instance->ptmx.permissions.mode = candidate.ptmxmode;
    instance->flags = flags & VFS_SUPER_POLICY_MASK;
    return 0;
}

static const struct vfs_backend_ops devpts_ops = {
    .lookup = devpts_lookup, .retain = devpts_retain, .release = devpts_release,
    .same_node = devpts_same, .destroy = devpts_destroy,
    .permissions = devpts_permissions, .getattr = devpts_getattr,
    .statfs = devpts_statfs, .readdir = devpts_readdir,
    .mount_flags = devpts_flags, .remount_options = devpts_remount,
};

/** @brief Construct an owned independent devpts backend and attachment.
 * @param source Optional diagnostic label. @param flags Final mount policy.
 * @param options Optional supported mount data. @param uid Root/ptmx UID.
 * @param gid Root/ptmx GID. @param out Owned detached mount output.
 * @return Zero or negative errno with complete rollback. Execution-owned. */
int devpts_create_vfs_mount(const char *source, uint64_t flags, const char *options,
                            uint32_t uid, uint32_t gid, struct vfs_mount **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (flags & ~(VFS_MOUNT_POLICY_MASK | VFS_SUPER_POLICY_MASK | MS_STRICTATIME)) return -EOPNOTSUPP;
    struct devpts_options parsed = {.mode = 0600, .maximum = DEVPTS_LIMIT};
    int rc = devpts_parse(options, &parsed);
    if (rc) return rc;
    if (devpts_next_instance >= DEVPTS_LIMIT - 0x10000u - 1u) return -ENOSPC;
    struct devpts_instance *instance = kernel_malloc(sizeof(*instance));
    if (!instance) return -ENOMEM;
    *instance = (struct devpts_instance){.options = parsed, .references = 1,
        .next_inode = 3, .flags = flags & VFS_SUPER_POLICY_MASK,
        .identity = ++devpts_next_instance};
    instance->device = devpts_device(0, instance->identity + 0x10000u);
    instance->root = (struct devpts_inode){.instance = instance, .ino = DEVPTS_ROOT_INO,
        .linked = true, .permissions = {0755, uid, gid}};
    instance->ptmx = (struct devpts_inode){.instance = instance, .ino = DEVPTS_PTMX_INO,
        .linked = true, .permissions = {parsed.ptmxmode, uid, gid}};
    struct storage_node root;
    devpts_node(&instance->root, &root);
    rc = vfs_mount_new(&devpts_ops, instance, NULL, &root, out);
    if (rc) { devpts_instance_put(instance); return rc; }
    rc = vfs_mount_set_flags(*out, flags & ~VFS_SUPER_POLICY_MASK);
    if (!rc) rc = vfs_mount_describe(*out, source && *source ? source : "devpts", "devpts", instance->device);
    if (rc) { vfs_mount_put(*out); *out = NULL; }
    return rc;
}

/** @brief Resolve a devpts ptmx directly or through a legacy device's sibling pts.
 * @param device Borrowed actual device. @param root Borrowed caller root.
 * @param out Owned ptmx path, empty on failure.
 * @return Zero or negative errno. Requires execution ownership. */
int devpts_resolve_ptmx(const struct vfs_path *device, const struct vfs_path *root,
                        struct vfs_path *out)
{
    if (!out) return -EINVAL;
    *out = (struct vfs_path){0};
    if (!vfs_path_node(device)) return -ENODEV;
    if (device->mount->ops == &devpts_ops) {
        struct devpts_instance *instance = vfs_path_context(device);
        if (devpts_find(instance, vfs_path_node(device)) != &instance->ptmx) return -ENODEV;
        vfs_path_get(device, out);
        return 0;
    }
    struct vfs_namespace *ns = vfs_path_namespace(device);
    if (!ns || !vfs_path_node(root)) return -ENODEV;
    struct vfs_path parent = {0};
    vfs_path_get(device, &parent);
    vfs_path_parent(root, &parent);
    int rc = vfs_path_lookup(ns, root, &parent, "pts/ptmx", VFS_LOOKUP_FOLLOW, out);
    vfs_path_put(&parent);
    if (rc) return rc;
    if (out->mount->ops == &devpts_ops) {
        struct devpts_instance *instance = vfs_path_context(out);
        if (devpts_find(instance, vfs_path_node(out)) == &instance->ptmx) return 0;
    }
    vfs_path_put(out);
    return -ENODEV;
}

/** @brief Allocate the first free instance-local index and bind a new session.
 * @param ptmx Borrowed authorized multiplexor. @param owner_pid Session owner.
 * @param uid Default slave UID. @param gid Default slave GID.
 * @param out Internal ID, zero on failure.
 * @return Zero or negative errno, rolling back all pins on failure.
 * Execution ownership serializes numbering, publication and callback delivery. */
int devpts_new_pty(const struct vfs_path *ptmx, uint32_t owner_pid,
                    uint32_t uid, uint32_t gid, uint32_t *out)
{
    if (!out) return -EINVAL;
    *out = 0;
    if (!vfs_path_node(ptmx) || ptmx->mount->ops != &devpts_ops) return -ENODEV;
    struct devpts_instance *instance = vfs_path_context(ptmx);
    if (devpts_find(instance, vfs_path_node(ptmx)) != &instance->ptmx) return -ENODEV;
    if (devpts_allocated(instance) >= instance->options.maximum || instance->next_inode == UINT64_MAX) return -ENOSPC;
    uint32_t number = 0;
    /* Endpoint references reserve indices after unlink, metadata-only pins do not. */
    for (;;) {
        bool used = false;
        for (struct devpts_inode *old = instance->slaves; old; old = old->next) {
            if (old->number == number && devpts_reserved(old)) { used = true; break; }
        }
        if (!used) break;
        if (++number == DEVPTS_LIMIT) return -ENOSPC;
    }
    struct devpts_inode *inode = kernel_malloc(sizeof(*inode));
    if (!inode) return -ENOMEM;
    int32_t id = pty_create(owner_pid);
    if (id < 0) { kernel_free(inode); return id; }
    struct storage_node node;
    int rc = pty_get_node((uint32_t)id, &node);
    if (!rc) rc = pty_transfer_get((uint32_t)id, TASK_PTY_ENDPOINT_SLAVE);
    if (rc) { (void)pty_destroy(owner_pid, (uint32_t)id); kernel_free(inode); return rc; }
    *inode = (struct devpts_inode){.instance = instance, .ino = instance->next_inode++,
        .references = 1, .number = number, .pty_id = (uint32_t)id,
        .generation = node.volume_id, .linked = true,
        .permissions = {instance->options.mode,
            instance->options.set_uid ? instance->options.uid : uid,
            instance->options.set_gid ? instance->options.gid : gid},
        .next = instance->slaves};
    instance->slaves = inode;
    ++instance->references;
    rc = pty_bind_devpts((uint32_t)id, number, inode, devpts_hangup);
    if (rc) { devpts_inode_put(inode); (void)pty_destroy(owner_pid, (uint32_t)id); return rc; }
    *out = (uint32_t)id;
    return 0;
}

/** @brief Resolve a live slave using its retained instance/inode/generation.
 * @param path Borrowed actual slave. @param out Internal session ID output.
 * @return Zero or negative ENODEV/EIO. Requires execution ownership. */
int devpts_slave_pty(const struct vfs_path *path, uint32_t *out)
{
    if (!out) return -EINVAL;
    *out = 0;
    if (!vfs_path_node(path) || path->mount->ops != &devpts_ops) return -ENODEV;
    struct devpts_inode *inode = devpts_find(vfs_path_context(path), vfs_path_node(path));
    if (!inode || !inode->pty_id) return -ENODEV;
    if (!inode->linked || !pty_is_active(inode->pty_id)) return -EIO;
    *out = inode->pty_id;
    return 0;
}
