/* Linux anonymous shmem files. tmpfs owns sparse pages and VMA inode pins. */
#include <reliefnt/heap.h>
#include <reliefnt/object.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <linux/fcntl.h>
#include <linux/mman.h>
#include <linux/errno.h>

struct task_shm {
    uint32_t refs;
    uint32_t seals;
    struct storage_node node;
    char label[SCHED_TASK_FILE_PATH_LEN];
};

struct task_shm_path_pin {
    struct task_shm *segment;
    struct storage_inode_ref *inode;
};
static struct vfs_mount *task_shm_mount;

/** @brief Find a generation-checked anonymous inode's shared seal state.
 * @param node Borrowed metadata under execution ownership, nullable.
 * @return Borrowed segment or NULL; never resolves a displayed filename.
 */
static struct task_shm *task_shm_for_node(const struct storage_node *node)
{
    if(!node || !(node->flags&STORAGE_NODE_FLAG_MEMFD) || node->entry_id>UINT32_MAX)return NULL;
    struct task_shm *segment=kernel_object_lookup(kernel_objects(),(uint32_t)node->entry_id,KERNEL_OBJECT_DEVICE);
    return segment && segment->node.first_cluster==node->first_cluster &&
        segment->node.volume_id==node->volume_id ? segment : NULL;
}

/** @brief Retire one seal-state reference after its owner stops using the inode.
 * @param segment Borrowed live segment with a held reference, or NULL.
 * @param handle Generation-checked registry handle identifying this segment.
 * Execution transaction required; the last owner removes the registry entry.
 */
static void task_shm_put(struct task_shm *segment,uint32_t handle)
{
    if(!segment || !segment->refs || --segment->refs)return;
    void *removed=NULL;
    if(!kernel_object_remove(kernel_objects(),handle,KERNEL_OBJECT_DEVICE,&removed))kernel_free(removed);
}

/** @brief Refuse directory lookups on the internal anonymous-only shmem mount.
 * @param context Unused builtin context.
 * @param parent Borrowed synthetic root.
 * @param name Requested component, never a memfd diagnostic label.
 * @param out Unchanged on error.
 * @return ENOENT; anonymous inodes are reached only through held paths.
 */
static int task_shm_path_lookup(void *context,const struct storage_node *parent,
                                  const char *name,struct storage_node *out)
{(void)context;(void)parent;(void)name;(void)out;return -ENOENT;}

/** @brief Retain backing inode and seals for an anonymous dentry.
 * @param context Unused builtin context.
 * @param node Borrowed anonymous inode or synthetic root under execution ownership.
 * @param out Receives one owned composite pin, or NULL for the synthetic root.
 * @return Zero or negative ENOENT/ENOMEM/backing error, with balanced rollback.
 */
static int task_shm_path_retain(void *context,const struct storage_node *node,void **out)
{
    (void)context;*out=NULL;if(node->type==RELIEFOS_FS_TYPE_DIR)return 0;
    struct task_shm *segment=task_shm_for_node(node);if(!segment)return -ENOENT;
    struct task_shm_path_pin *pin=kernel_malloc(sizeof(*pin));if(!pin)return -ENOMEM;
    int rc=storage_inode_get(node,&pin->inode);
    if(rc){kernel_free(pin);return rc;}
    pin->segment=segment;++segment->refs;*out=pin;return 0;
}

/** @brief Drop an anonymous dentry's backing and seal-state ownership.
 * @param context Unused builtin context.
 * @param node Stable metadata with the held registry handle.
 * @param value Owned composite pin, or NULL for the synthetic root.
 * Execution transaction required; inode release may free sparse backing pages.
 */
static void task_shm_path_release(void *context,const struct storage_node *node,void *value)
{
    (void)context;struct task_shm_path_pin *pin=value;if(!pin)return;
    (void)storage_inode_put(pin->inode);task_shm_put(pin->segment,(uint32_t)node->entry_id);kernel_free(pin);
}

/** @brief Compare generation-checked anonymous inode identities.
 * @param context Unused builtin context.
 * @param left Borrowed node under execution ownership.
 * @param right Borrowed node under execution ownership.
 * @return True only for the same backing inode and registry generation.
 */
static bool task_shm_path_same(void *context,const struct storage_node *left,const struct storage_node *right)
{(void)context;return left->entry_id==right->entry_id && left->first_cluster==right->first_cluster && left->volume_id==right->volume_id;}

/** @brief Query or change the anonymous inode's actual tmpfs DAC metadata.
 * @param context Unused builtin backing context.
 * @param path Borrowed retained anonymous path, never resolved by its label.
 * @param value Output metadata or already authorized replacement metadata.
 * @param write Select an update instead of a query.
 * @return Zero or backing errno; caller holds execution ownership and has
 * performed authorization. Anonymous labels never become namespace keys.
 */
static int task_shm_path_permissions(void *context,const struct vfs_path *path,
                                       struct reliefos_permissions *value,bool write)
{
    (void)context;return storage_inode_permissions(vfs_path_node(path),value,write);
}

/** @brief Read actual anonymous inode metadata, including zero link count.
 * @param context Unused builtin context.
 * @param path Borrowed retained anonymous inode path.
 * @param out Required native Linux stat output.
 * @return Zero or backing errno; no label lookup or ownership transfer.
 */
static int task_shm_path_getattr(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{(void)context;return storage_inode_stat(vfs_path_node(path),out);}

/** @brief Query the anonymous inode's actual tmpfs superblock.
 * @param context Unused builtin context.
 * @param node Retained anonymous inode, borrowed under execution ownership.
 * @param out Required Linux statfs output.
 * @return Zero or backing errno without namespace lookup.
 */
static int task_shm_path_statfs(void *context,const struct storage_node *node,struct linux_statfs_abi *out)
{(void)context;return storage_statfs(node,out);}

/** @brief Apply a size change through the single shared shmem seal authority.
 * @param segment Retained live anonymous segment under execution ownership.
 * @param size New nonnegative signed-off_t size.
 * @return Zero or EINVAL/EPERM/backing error; no mutation on seal rejection.
 * Caller supplies inode writer admission and keeps any VMA/provider pins.
 */
static int task_shm_resize(struct task_shm *segment,uint64_t size)
{
    if(size>INT64_MAX)return -RELIEFOS_EINVAL;
    if((size<segment->node.size && (segment->seals&LINUX_F_SEAL_SHRINK)) ||
       (size>segment->node.size && (segment->seals&LINUX_F_SEAL_GROW)))return -RELIEFOS_EPERM;
    return storage_truncate_held_node(&segment->node,size);
}

/** @brief Resize the exact anonymous path while preserving inode-wide seals.
 * @param context Unused builtin context.
 * @param node Held anonymous inode, updated only on success.
 * @param size Requested logical size.
 * @return Zero or negative errno. Caller authorizes/leases the inode; no
 * pathname lookup, OFD simulation or graph lock across backing work.
 */
static int task_shm_path_truncate(void *context,struct storage_node *node,uint64_t size)
{
    (void)context;struct task_shm *segment=task_shm_for_node(node);if(!segment)return -ENOENT;
    int rc=task_shm_resize(segment,size);if(!rc)*node=segment->node;return rc;
}

/** @brief Reserve anonymous tmpfs pages through the shared seal authority.
 * @param context Unused builtin context.
 * @param node Retained anonymous inode, refreshed on success.
 * @param mode Supported allocation flags, zero or Linux KEEP_SIZE (1).
 * @param offset Start byte, validated by the retained-path caller.
 * @param length Positive byte count with a representable end.
 * @return Zero or seal/backing errno. F_SEAL_GROW also rejects KEEP_SIZE
 * reservation beyond EOF, matching Linux shmem. Ordinary allocation does
 * not modify existing data; WRITE seals prohibit hole punching, which this
 * provider does not advertise. Caller owns execution and writer admission.
 */
static int task_shm_path_fallocate(void *context,struct storage_node *node,uint32_t mode,
                                     uint64_t offset,uint64_t length)
{
    (void)context;struct task_shm *segment=task_shm_for_node(node);if(!segment)return -ENOENT;
    if(mode&~1u)return -EOPNOTSUPP;
    if((segment->seals&LINUX_F_SEAL_GROW) && offset+length>segment->node.size)return -EPERM;
    int rc=storage_tmpfs_fallocate(&segment->node,mode,offset,length);
    if(!rc)*node=segment->node;return rc;
}

/** @brief Complete sync on anonymous RAM backing without inventing disk I/O.
 * @param context Unused builtin context.
 * @param node Borrowed anonymous inode retained by the caller.
 * @param filesystem Select filesystem sync rather than inode sync.
 * @param data_only Permit retrieval-independent metadata to be omitted.
 * @return Zero; tmpfs has no persistent device/cache to flush. Execution
 * ownership required, no allocation, namespace lookup or reference transfer.
 */
static int task_shm_path_sync(void *context,const struct storage_node *node,bool filesystem,bool data_only)
{(void)context;(void)node;(void)filesystem;(void)data_only;return 0;}

/** @brief Render the actual anonymous segment's immutable diagnostic label.
 * @param context Unused builtin context.
 * @param node Borrowed anonymous inode whose path retains seal/name state.
 * @param buffer Required output bytes, NUL-terminated on success.
 * @param capacity Writable capacity including NUL.
 * @return Zero, ENOENT for a missing segment/label, or ENAMETOOLONG. Caller
 * owns execution; no pathname lookup, I/O, locks or reference transfer occurs.
 * This label never grants access or becomes a namespace lookup key.
 */
static int task_shm_path_display(void *context,const struct storage_node *node,char *buffer,size_t capacity)
{
    (void)context;struct task_shm *segment=task_shm_for_node(node);
    if(!segment || !segment->label[0])return -ENOENT;
    size_t length=__builtin_strlen(segment->label);if(length>=capacity)return -ENAMETOOLONG;
    __builtin_memcpy(buffer,segment->label,length+1);return 0;
}

/** @brief Set times on the exact retained anonymous tmpfs inode.
 * @param context Unused builtin context.
 * @param node Borrowed live anonymous inode retaining its backing storage.
 * @param atime Authorized access time at the backing filesystem resolution.
 * @param mtime Authorized modification time at the same resolution.
 * @param set_atime Whether to update access time.
 * @param set_mtime Whether to update modification time.
 * @return Zero or negative backing errno, with no label lookup or ownership
 * transfer. Caller holds execution ownership without graph locks during I/O.
 */
static int task_shm_path_utimens(void *context,const struct storage_node *node,int64_t atime,int64_t mtime,
                                   bool set_atime,bool set_mtime)
{(void)context;return storage_inode_utimensat(node,atime,mtime,set_atime,set_mtime);}

/** @brief Obtain an owned anonymous path on the builtin internal shmem mount.
 * @param node Live anonymous inode whose registry entry already exists.
 * @param out Empty output receiving one path, empty on error.
 * @return Zero or negative allocation/backing error; execution ownership
 * serializes lazy mount creation. This mount is never a visible namespace root.
 */
static int task_shm_path_new(const struct storage_node *node,struct vfs_path *out)
{
    static const struct vfs_backend_ops ops={.lookup=task_shm_path_lookup,.retain=task_shm_path_retain,
        .release=task_shm_path_release,.same_node=task_shm_path_same,.permissions=task_shm_path_permissions,
        .getattr=task_shm_path_getattr,.statfs=task_shm_path_statfs,.truncate=task_shm_path_truncate,
        .fallocate=task_shm_path_fallocate,.sync=task_shm_path_sync,
        .display_name=task_shm_path_display,
        .utimens=task_shm_path_utimens};
    if(!task_shm_mount) {
        struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};
        int rc=vfs_mount_new(&ops,NULL,NULL,&root,&task_shm_mount);if(rc)return rc;
    }
    return vfs_path_anonymous(task_shm_mount,node,out);
}

/** @brief Resolve the seal state belonging to a pinned anonymous-file description.
 * @param file Borrowed description under execution ownership, nullable.
 * @return Borrowed seal state or NULL for a non-shmem/stale description.
 */
static struct task_shm *task_shm_for_file(const struct task_file *file)
{
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return NULL;
    return kernel_object_lookup(kernel_objects(), (uint32_t)file->aux, KERNEL_OBJECT_DEVICE);
}

/** @brief Retain seal-state ownership when an independent description is created.
 * @param file Live shmem description under execution ownership, nullable.
 */
void task_shm_retain(struct task_file *file)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (segment) ++segment->refs;
}

/** @brief Release seal state; backing inode/page lifetime is owned by OFDs and VMAs.
 * @param file Retiring shmem description under execution ownership, nullable.
 */
void task_shm_release(struct task_file *file)
{
    struct task_shm *segment = task_shm_for_file(file);
    task_shm_put(segment,file ? (uint32_t)file->aux : 0);
}

/** @brief Publish a bounded diagnostic label in the shared anonymous inode state.
 * @param file Attached unpublished OFD under execution ownership.
 * @param name Borrowed kernel basename, required; never resolved as a path.
 * @param length Valid bytes in name, excluding NUL; at most 249.
 * @return Zero or negative EINVAL/EBADF, without mutation on error. No allocation.
 * Reopen reads the same label after all original descriptors have closed.
 */
int task_shm_set_name(struct task_file *file,const char *name,size_t length)
{
    if(!name || length>VFS_NAME_MAX-6)return -EINVAL;
    struct task_shm *segment=task_shm_for_file(file);if(!segment)return -EBADF;
    __builtin_memcpy(segment->label,"/memfd:",7);
    __builtin_memcpy(segment->label+7,name,length);
    __builtin_memcpy(segment->label+7+length," (deleted)",11);
    __builtin_memcpy(file->path,segment->label,sizeof(file->path));return 0;
}

/** @brief Attach a zero-length anonymous tmpfs inode and independent seal state.
 * @param file Fresh description; receives the owned inode pin only on success.
 * @param create_flags Linux MFD flags, validated by the syscall entry.
 * @return Zero or negative EINVAL, ENOMEM, ENFILE or backing-store error;
 * no partially attached object is left on failure. Requires execution ownership.
 */
int task_shm_attach(struct task_file *file, uint32_t create_flags)
{
    if (!file || (file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    struct task_shm *segment = kernel_malloc(sizeof(*segment));
    if (!segment) return -RELIEFOS_ENOMEM;
    *segment = (struct task_shm){.refs = 1,
        .seals = create_flags & LINUX_MFD_ALLOW_SEALING ? 0 : LINUX_F_SEAL_SEAL};
    const struct task *task = sched_current_task();
    struct storage_inode_ref *inode = NULL;
    int ret = storage_shmem_create(task ? task->euid : 0, task ? task->egid : 0, &segment->node, &inode);
    if (ret < 0) { kernel_free(segment); return ret; }
    bool writable=file_can_write(file) && !(file->flags&TASK_FILE_FLAG_PATH);
    if(writable) {
        ret=storage_inode_write_get(inode);
        if(ret){(void)storage_inode_put(inode);kernel_free(segment);return ret;}
    }
    uint32_t handle = kernel_object_insert(kernel_objects(), segment, KERNEL_OBJECT_DEVICE);
    if (!handle) {
        if(writable)storage_inode_write_put(inode);
        (void)storage_inode_put(inode);
        kernel_free(segment);
        return -RELIEFOS_ENFILE;
    }
    segment->node.flags|=STORAGE_NODE_FLAG_MEMFD;segment->node.entry_id=handle;
    struct vfs_path path={0};ret=task_shm_path_new(&segment->node,&path);
    if(ret) {
        if(writable)storage_inode_write_put(inode);
        (void)storage_inode_put(inode);task_shm_put(segment,handle);return ret;
    }
    file->aux = handle;
    file->flags = (file->flags & ~TASK_FILE_FLAG_DEV_NODE) | TASK_FILE_FLAG_DEV_SHM;
    file->node = segment->node;
    file->inode = inode;
    file->inode_write_access=writable;
    file->path_ref=path;
    return task_shm_set_name(file,"",0);
}

/** @brief Attach an independent OFD to an authorized retained anonymous inode.
 * @param file Fresh unpublished description with validated status flags.
 * @param path Borrowed exact target, stable under execution ownership.
 * @return Zero or negative EINVAL/ENOENT/ENOMEM/ETXTBSY/backing error. Failure
 * leaves the fresh description unchanged; caller performs inode access checks.
 * Acquires a per-OFD writer lease before publishing, and shares inode-wide seals.
 */
int task_shm_reopen(struct task_file *file,const struct vfs_path *path)
{
    if(!file || file->inode || file->path_ref.mount || file->flags&TASK_FILE_FLAG_DEV_SHM)return -EINVAL;
    const struct storage_node *node=vfs_path_node(path);
    struct task_shm *segment=task_shm_for_node(node);if(!segment)return -ENOENT;
    struct storage_inode_ref *inode=NULL;int rc=storage_inode_get(node,&inode);if(rc)return rc;
    bool writable=file_can_write(file) && !(file->flags&TASK_FILE_FLAG_PATH);
    if(writable) {
        rc=storage_inode_write_get(inode);if(rc){(void)storage_inode_put(inode);return rc;}
    }
    struct vfs_path retained={0};vfs_path_get(path,&retained);++segment->refs;
    file->node=segment->node;file->inode=inode;file->inode_write_access=writable;
    file->aux=node->entry_id;file->path_ref=retained;file->offset=0;
    __builtin_memcpy(file->path,segment->label,sizeof(file->path));
    file->flags=(file->flags&~TASK_FILE_FLAG_DEV_NODE)|TASK_FILE_FLAG_DEV_SHM;return 0;
}

/** @brief Change logical size without requiring a physically contiguous allocation.
 * @param file Writable shmem description under execution ownership.
 * @param size New nonnegative signed-off_t size in bytes.
 * @return Zero or negative EINVAL/EBADF/EPERM/backing error. Shrink revokes
 * affected mappings before releasing pages; growth is sparse and zero-filled.
 */
int task_shm_truncate(struct task_file *file, uint64_t size)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if (!file_can_write(file)) return -RELIEFOS_EINVAL;
    int ret = task_shm_resize(segment,size);
    if (!ret) file->node.size = size;
    return ret;
}

/** @brief Read file bytes or zeros from holes, advancing the shared OFD offset.
 * @param file Pinned shmem description under execution ownership.
 * @param buffer Validated writable kernel-accessible destination for length bytes.
 * @param length Maximum byte count.
 * @return Bytes read, zero at EOF, or negative EBADF/EFAULT/backing error.
 */
int task_shm_read(struct task_file *file, void *buffer, uint32_t length)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if (length && !buffer) return -RELIEFOS_EFAULT;
    if (!length) return 0;
    uint32_t got = 0;
    int ret = storage_read_node(&segment->node, file->offset, buffer, length, &got);
    file->offset += got;
    return got ? (int)got : ret;
}

/** @brief Write sparse file pages, preserving partial progress on allocation failure.
 * @param file Pinned writable shmem OFD under execution ownership.
 * @param buffer Validated source for length bytes.
 * @param length Requested byte count.
 * @return Bytes written or negative EBADF/EFAULT/EFBIG/EPERM/backing error.
 * Gaps are zero-filled by tmpfs and no offset or length wraps signed off_t.
 */
int task_shm_write(struct task_file *file, const void *buffer, uint32_t length)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment || !file_can_write(file)) return -RELIEFOS_EBADF;
    if (length && !buffer) return -RELIEFOS_EFAULT;
    if (!length) return 0;
    if (segment->seals & (LINUX_F_SEAL_WRITE | LINUX_F_SEAL_FUTURE_WRITE)) return -RELIEFOS_EPERM;
    if (file->offset > INT64_MAX || length > (uint64_t)INT64_MAX - file->offset) return -RELIEFOS_EFBIG;
    if (file->offset + length > segment->node.size && (segment->seals & LINUX_F_SEAL_GROW))
        return -RELIEFOS_EPERM;
    uint32_t written = 0;
    int ret = storage_write_held_node(&segment->node, file->offset, buffer, length, &written);
    file->offset += written;
    if (written) {
        if (file->offset > segment->node.size) segment->node.size = file->offset;
        file->node.size = segment->node.size;
    }
    return written ? (int)written : ret;
}

/** @brief Enforce shared-map seals without restricting private COW mappings.
 * @param file Pinned anonymous-file description under execution ownership.
 * @param flags Validated Linux MAP flags.
 * @param prot Requested protection bits.
 * @param max_prot In/out maximum protection; cleared WRITE prevents later mprotect bypass.
 * @return Zero or negative EBADF/EPERM. Backing allocation is deferred to faults.
 */
int task_shm_mmap_check(const struct task_file *file, uint32_t flags, uint32_t prot, uint32_t *max_prot)
{
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if ((flags & LINUX_MAP_SHARED) &&
        (segment->seals & (LINUX_F_SEAL_WRITE | LINUX_F_SEAL_FUTURE_WRITE))) {
        if (prot & LINUX_PROT_WRITE) return -RELIEFOS_EPERM;
        if (max_prot) *max_prot &= ~LINUX_PROT_WRITE;
    }
    return 0;
}

/** @brief Check every task's potentially writable shared VMA for this inode.
 * @param segment Borrowed live seal state under execution ownership.
 * @return One if any shared mapping can write, zero otherwise. Private COW
 * mappings do not prevent sealing; no allocation or bounded snapshot is used.
 */
static int task_shm_writable_mapped(const struct task_shm *segment)
{
    struct task *other;
    for (uint32_t s = 0; (other = sched_task_at(s)); ++s) {
        for (uint32_t i = 0; i < sched_task_vma_capacity(other); ++i) {
            const struct task_vma *vma = sched_task_vma_at(other, i);
            if (!vma || !vma->used || !(vma->flags & TASK_VMA_FLAG_SHARED) ||
                !(vma->max_prot & LINUX_PROT_WRITE) || !(vma->flags & TASK_VMA_FLAG_FILE)) continue;
            if (vma->file_node.volume_id == segment->node.volume_id &&
                vma->file_node.first_cluster == segment->node.first_cluster &&
                (vma->file_node.flags & STORAGE_NODE_FLAG_TMPFS)) return 1;
        }
    }
    return 0;
}

/** @brief Read the inode-wide monotonic seal set.
 * @param file Pinned description under execution ownership, nullable.
 * @return Seal mask or negative EINVAL for non-shmem, EBADF for a stale handle.
 */
int64_t task_shm_get_seals(const struct task_file *file)
{
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    struct task_shm *segment = task_shm_for_file(file);
    return segment ? (int64_t)segment->seals : -RELIEFOS_EBADF;
}

/** @brief Add Linux seals atomically after access, flag and live-map checks.
 * @param file Writable shmem description under execution ownership.
 * @param seals Requested known seal bits; existing seals are never cleared.
 * @return Zero or negative EPERM/EINVAL/EBADF/EBUSY; failure changes no seals.
 */
int task_shm_add_seals(struct task_file *file, uint32_t seals)
{
    if (!file || !file_can_write(file)) return -RELIEFOS_EPERM;
    if (seals & ~LINUX_F_ALL_SEALS) return -RELIEFOS_EINVAL;
    if (!(file->flags & TASK_FILE_FLAG_DEV_SHM)) return -RELIEFOS_EINVAL;
    struct task_shm *segment = task_shm_for_file(file);
    if (!segment) return -RELIEFOS_EBADF;
    if (segment->seals & LINUX_F_SEAL_SEAL) return -RELIEFOS_EPERM;
    if ((seals & LINUX_F_SEAL_WRITE) && !(segment->seals & LINUX_F_SEAL_WRITE) &&
        task_shm_writable_mapped(segment)) return -RELIEFOS_EBUSY;
    segment->seals |= seals;
    return 0;
}
