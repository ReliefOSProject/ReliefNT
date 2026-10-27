/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FS_TYPE_H
#define RELIEFNT_FS_TYPE_H
#include <reliefnt/types.h>

struct module;
struct vfs_mount;
struct storage_node;
struct storage_filesystem_ops;
struct fs_registration;
struct install_disk_state;
#define FS_REQUIRES_DEV 1u

/* All strings/objects are kernel-owned and borrowed for the factory call.
 * A block source is an already resolved node, never a parsed device name. */
struct fs_mount_request {
    const char *source;
    const char *options;
    const struct storage_node *source_node;
    uint64_t flags;
    uint32_t uid,gid;
};

/* Immutable from registration until the last admitted reference retires.
 * Registration is a self-owned module resource, not an external self-pin. */
struct file_system_type {
    const char *name;
    struct module *owner;
    uint32_t flags;
    int (*get_tree)(const struct fs_mount_request *request,struct vfs_mount **out);
    const struct storage_filesystem_ops *storage;
    int (*format)(struct install_disk_state *disk,uint64_t first,uint64_t sectors);
};
struct fs_type_ref {
    const struct file_system_type *type;
    struct fs_registration *registration;
};

/** @brief Publish a filesystem name with automatic failed-init/unload removal.
 * @param type Persistent immutable descriptor with a nonempty simple name and factory.
 * @return Zero, EBUSY for a duplicate, or negative validation/allocation/owner errno.
 * Task context; caller is the owner's initializer or already pins its code.
 * No registry/module spinlock spans allocation or provider code. COMING owners
 * may register but cannot admit users. Caller retains the descriptor until all
 * admitted references retire; module ownership supplies this lifetime.
 */
int register_filesystem(const struct file_system_type *type);
/** @brief Close name admission while preserving already admitted references.
 * @param type Exact registered descriptor, stable for this call.
 * @return Zero or EINVAL if not registered. Task context; no provider callback.
 * Existing factories and mounts remain valid; their references prevent module
 * unloading. An ownerless descriptor must outlive all of its admitted users.
 */
int unregister_filesystem(const struct file_system_type *type);
/** @brief Resolve a registered LIVE implementation and acquire its code pin.
 * @param name Required case-sensitive terminated filesystem name.
 * @param out Required empty reference output; no ownership on failure.
 * @return Zero, ENODEV for absent/closed/COMING code, or EINVAL/EOVERFLOW.
 * IRQ-safe; short registry-to-module lock nesting only, no callbacks/I/O.
 */
int fs_type_get(const char *name,struct fs_type_ref *out);
/** @brief Consume one admitted implementation reference, accepting an empty one.
 * @param reference Owned reference cleared before release; NULL is accepted.
 * @return None. IRQ-safe; no provider callback or I/O. The last core registry
 * allocation may be freed. Never copy a reference without acquiring another.
 */
void fs_type_put(struct fs_type_ref *reference);
/** @brief Construct an owned detached mount through an admitted implementation.
 * @param reference Borrowed nonempty reference held across this call.
 * @param request Borrowed validated kernel mount operands.
 * @param out Required output receiving one mount with the same provider owner.
 * @return Zero or negative factory/input/owner errno; no output on failure.
 * Task context under VFS execution ownership. No registry/module lock spans
 * the callback. The factory must acquire independent context and mount pins,
 * and fully roll back construction on failure; the reference is not consumed.
 */
int fs_type_get_tree(const struct fs_type_ref *reference,const struct fs_mount_request *request,
                     struct vfs_mount **out);
/** @brief Run an admitted native formatter with its code pinned through all writes.
 * @param reference Borrowed admitted type/code reference, required.
 * @param disk Borrowed authorized unmounted managed target disk.
 * @param first First validated target sector. @param sectors Validated extent length.
 * @return Zero or input/unsupported/native errno. Task context under storage
 * execution/mutation ownership; caller validates partition/protection policy.
 * No registry/module lock spans native I/O. Unregister closes new admission
 * while this existing operation remains valid until its caller releases ref.
 */
int fs_type_format(const struct fs_type_ref *reference,struct install_disk_state *disk,
                     uint64_t first,uint64_t sectors);
#endif
