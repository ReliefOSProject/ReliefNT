/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include "../../../mm/swap/internal.h"
#include <reliefnt/devfs.h>
#include <reliefnt/console.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/mm.h>
#include <reliefnt/panic.h>
#include <reliefnt/signal.h>
#include <reliefnt/swap_internal.h>
#include <reliefnt/swap_report.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>

#define SWAP_PAGE_BYTES 4096u
#define SWAP_SECTOR_BYTES 512u
#define SWAP_BATCH 32u
#define SWAP_FLAG_PREFER 0x8000u

/* An unpublished preparation owns the same pins and exclusions as an active
 * area. All links and claims are serialized by execution ownership. */
struct swap_backing {
    struct swap_backing *next;
    struct vfs_path path;
    struct vfs_open_file *open;
    struct storage_inode_ref *inode;
    struct storage_node node;
    struct block_device_ref device;
    struct swap_storage_claim claim;
    struct swap_backend *backend;
    struct swap_area *area;
    uint64_t first,count;
    bool inode_claimed, inode_writer, raw;
};
struct swap_preparation { struct swap_storage_token storage; uint32_t pid; };
static struct swap_backing *swap_backings;
static int32_t swap_default_priority=-2;

/** @brief Snapshot published swap areas and render their retained live paths.
 * @param out Required owned result, NULL when no area exists. @return Zero or
 * negative errno without a partial result. The sole execution writer pins the
 * backing list, path/mount and caller root across rendering; the slot lock is
 * held only while copying each area's counters. No provider I/O occurs.
 */
int swap_report_capture(struct swap_report **out)
{
    if (!out) return -EINVAL;
    *out = NULL; size_t count = 0;
    for (struct swap_backing *b = swap_backings; b; b = b->next) {
        if (count == (SIZE_MAX - sizeof(struct swap_report)) / sizeof(struct swap_report_row))
            return -EOVERFLOW;
        ++count;
    }
    if (!count) return 0;
    struct swap_report *report = kernel_malloc(sizeof(*report) + count * sizeof(report->rows[0]));
    if (!report) return -ENOMEM;
    report->count = count; size_t i = 0; int rc = 0;
    struct task *task = sched_current_task();
    for (struct swap_backing *b = swap_backings; b; b = b->next, ++i) {
        struct swap_report_row *row = &report->rows[i];
        row->raw = b->raw; row->path[0] = 0;
        rc = swap_area_report(b->area, &row->total, &row->used, &row->priority);
        if (rc) break;
        struct vfs_namespace *ns = vfs_path_namespace(&b->path);
        if (!ns) { rc = -ENOENT; break; }
        struct vfs_path root = {0};
        const struct vfs_path *caller_root = task ? &sched_task_paths(task)->root : NULL;
        if (caller_root && vfs_path_namespace(caller_root) == ns) vfs_path_get(caller_root, &root);
        else rc = vfs_namespace_root(ns, &root);
        if (!rc) rc = vfs_path_format_link(ns, &root, &b->path, row->path, sizeof(row->path));
        vfs_path_put(&root);
        if (rc) break;
    }
    if (rc) { kernel_free(report); return rc; }
    *out = report; return 0;
}

/** @brief Release an immutable swap report after rendering.
 * @param report Owned snapshot, nullable. No path/backend reference is held.
 */
void swap_report_free(struct swap_report *report) { kernel_free(report); }

/** @brief Capture the original swap pathname before taking storage ownership.
 * @param checked Caller-owned VFS_PATH_MAX-byte buffer.
 * @param user_path Original user pathname.
 * @return Zero, terminal copy errno or private prerequisite contention.
 * An armed syscall scope owns accepted bytes through a retry; the resulting
 * local copy is the sole input to the subsequent activation/deactivation.
 */
static int swap_copy_path(char *checked,uint64_t user_path)
{
    struct task *task=sched_current_task();
    if(task && task->lookup_scope.armed) {
        struct task_user_text text={0};
        int rc=task_lookup_user_text(task,user_path,VFS_PATH_MAX,false,&text);
        if(rc)return rc;
        size_t i=0;do{checked[i]=text.text[i];}while(checked[i++] && i<VFS_PATH_MAX);
        return 0;
    }
    return copy_user_path(checked,VFS_PATH_MAX,user_path);
}

/** @brief Release the synchronous phase at a bounded execution checkpoint.
 * @param opaque Borrowed preparation retaining all path/inode/device claims.
 * @return Zero or terminal errno after execution ownership is reacquired.
 * No provider command or storage token may survive the temporary suspension.
 */
static int swap_checkpoint(void *opaque)
{
    struct swap_preparation *prep=opaque;
    bool active=prep->storage.active;
    if (active) swap_storage_end(&prep->storage);
    struct kernel_execution_suspend_token token={0};
    int rc=kernel_execution_suspend_irqrestore(&token);
    if (!rc) {
        if (kernel_execution_resume_irqsave(&token)) panic("swap execution resume");
        if (sched_current_pid()!=prep->pid) rc=-ESRCH;
    }
    if (!rc && active) rc=swap_storage_begin(&prep->storage);
    return rc;
}

/** @brief Release a private or drained area's exact ownership in reverse order.
 * @param backing Owned unpublished backing, nullable.
 * @param prep Preparation used for bounded metadata retirement.
 */
static void swap_backing_destroy(struct swap_backing *backing,struct swap_preparation *prep)
{
    if (!backing) return;
    swap_area_destroy_checkpoint(backing->area,swap_checkpoint,prep);
    swap_backend_destroy_checkpoint(backing->backend,swap_checkpoint,prep);
    swap_storage_release_range(&backing->claim);
    if (backing->inode_claimed) storage_inode_swap_release(backing->inode);
    if (backing->inode_writer) storage_inode_write_put(backing->inode);
    if (backing->inode) (void)storage_inode_put(backing->inode);
    if (backing->device.device) block_device_put(&backing->device);
    vfs_file_close(backing->open);
    vfs_path_put(&backing->path);
    kernel_free(backing);
}

/** @brief Resolve and authorize the same read/write access as a normal open.
 * @param path Kernel-owned pathname snapshot. @param backing Empty owned backing record.
 * @return Zero or lookup/open/DAC/type errno. The resolved path remains held.
 */
static int swap_open_path(const char *path,struct swap_backing *backing)
{
    struct task *task=sched_current_task();
    struct vfs_lookup_result found={0};
    int rc=resolve_kernel_object_at_flags(vfs_system_namespace(),task,LINUX_AT_FDCWD,
                                         path,false,false,VFS_LOOKUP_FOLLOW,&found);
    if (rc) return rc;
    backing->path=found.path;
    const struct storage_node *node=vfs_path_node(&backing->path);
    if (!node) return -ENOENT;
    if (node->type==RELIEFOS_FS_TYPE_DIR) return -EISDIR;
    rc=fs_permissions_check_path(task,&backing->path,FS_ACCESS_READ|FS_ACCESS_WRITE,false);
    if (rc) return rc;
    backing->node=*node;
    if (node->flags&STORAGE_NODE_FLAG_RDEV) {
        struct linux_stat_abi stat;
        rc=vfs_path_stat(&backing->path,&stat);
        if (!rc) rc=devfs_lookup_rdev(stat.st_mode&LINUX_S_IFMT,stat.st_rdev,&backing->node);
        if (rc) return rc;
    }
    backing->raw=(backing->node.flags&STORAGE_NODE_FLAG_DEV_BLOCK)!=0;
    if (backing->raw) {
        if (vfs_path_mount_flags(&backing->path)&MS_NODEV) return -EACCES;
        if (backing->node.first_cluster!=STORAGE_DEV_KIND_DISK) return -EINVAL;
        return 0;
    }
    if (node->type!=RELIEFOS_FS_TYPE_FILE) return -EINVAL;
    /* Only native ext providers implement the audited stable mapping contract. */
    if (!(node->flags&STORAGE_NODE_FLAG_EXT_FAMILY)) return -EOPNOTSUPP;
    rc=storage_inode_get(&backing->node,&backing->inode);
    if (!rc && !backing->inode) rc=-EOPNOTSUPP;
    if (!rc) rc=storage_inode_write_get(backing->inode);
    if (!rc) backing->inode_writer=true;
    if (!rc) rc=vfs_file_open(&backing->path,LINUX_O_RDWR,&backing->open);
    return rc;
}

/** @brief Acquire an actual block identity and physical partition geometry.
 * @param backing Held device path. @return Zero or negative provider errno.
 */
static int swap_raw_device(struct swap_backing *backing)
{
    uint32_t disk=STORAGE_BLOCK_DISK_ID(backing->node.volume_id);
    int32_t part=STORAGE_BLOCK_PARTITION(backing->node.volume_id);
    int rc=storage_disk_block_get(disk,&backing->device);
    if (!rc) rc=storage_disk_block_info(disk,part,&backing->first,&backing->count);
    if (!rc) {
        struct block_device_info info;
        rc=block_device_info(&backing->device,&info);
        if (!rc && (info.block_size!=SWAP_SECTOR_BYTES ||
                    !(info.flags&BLOCK_DEVICE_SWAP_SAFE) ||
                    (info.flags&BLOCK_DEVICE_READ_ONLY))) rc=-EOPNOTSUPP;
        if (!rc && (!backing->count || backing->first>=info.blocks ||
                    backing->count>info.blocks-backing->first)) rc=-EINVAL;
    }
    return rc;
}

/** @brief Read one raw header with the provider's exact command limit.
 * @param prep Live synchronous phase. @param backing Held block identity.
 * @param header Writable page. @return Zero or terminal provider errno.
 */
static int swap_raw_header(struct swap_preparation *prep,struct swap_backing *backing,uint8_t *header)
{
    struct block_device_info info;int rc=block_device_info(&backing->device,&info);
    if (rc) return rc;
    uint32_t done=0,total=SWAP_PAGE_BYTES/SWAP_SECTOR_BYTES;
    while (done<total) {
        uint32_t n=total-done;if (n>info.max_blocks)n=info.max_blocks;
        if (!n) return -EINVAL;
        rc=swap_storage_transfer(&prep->storage,&backing->device,backing->first+done,
                                 n,header+done*SWAP_SECTOR_BYTES,false);
        if (rc) return rc;
        done+=n;
    }
    return 0;
}

/** @brief Build fixed physical extents from the claimed filesystem provider.
 * @param prep Live preparation. @param backing Retained claimed file/backend.
 * @param sectors Exact header-declared sector coverage. @return Zero or errno.
 */
static int swap_map_file(struct swap_preparation *prep,struct swap_backing *backing,uint64_t sectors)
{
    uint64_t cursor=0;unsigned steps=0;
    while (cursor<sectors) {
        struct swap_storage_extent extent;
        int rc=swap_storage_file_extent(&backing->node,cursor*SWAP_SECTOR_BYTES,&extent);
        if (rc) return rc;
        if (extent.logical!=cursor || !extent.length) return -EINVAL;
        uint64_t n=extent.length;
        if (n>sectors-cursor)n=sectors-cursor;
        if (extent.unwritten) {
            if (n>SWAP_PAGE_BYTES/SWAP_SECTOR_BYTES)n=SWAP_PAGE_BYTES/SWAP_SECTOR_BYTES;
            rc=swap_storage_file_initialize(&backing->node,cursor*SWAP_SECTOR_BYTES,
                                             n*SWAP_SECTOR_BYTES);
            if (rc) return rc;
            /* A provider must actually convert the run. Requery once and
             * terminate if it did not, rather than retrying without progress. */
            rc=swap_storage_file_extent(&backing->node,cursor*SWAP_SECTOR_BYTES,&extent);
            if (rc) return rc;
            if (extent.logical!=cursor || extent.unwritten || !extent.length) return -EIO;
        } else {
            rc=swap_backend_add_extent_checkpoint(backing->backend,cursor,extent.physical,n,
                                                  swap_checkpoint,prep);
            if (rc) return rc;
            cursor+=n;
        }
        if (++steps==SWAP_BATCH) {
            steps=0;rc=swap_checkpoint(prep);if (rc) return rc;
        }
    }
    return 0;
}

/** @brief Prepare the exact regular-file inode and exclude dynamic mutation.
 * @param backing Opened native file. @return Zero or negative errno.
 */
static int swap_claim_file(struct swap_backing *backing)
{
    int rc=swap_file_check_in_use(backing->inode,&backing->node);
    if (!rc) rc=storage_inode_swap_claim(backing->inode);
    if (!rc) backing->inode_claimed=true;
    if (!rc) rc=swap_storage_file_device(&backing->node,&backing->device,
                                          &backing->first,&backing->count);
    if (!rc) rc=swap_storage_claim_range(&backing->claim,&backing->device,
                                          backing->first,backing->count,true);
    return rc;
}

/** @brief Prepare, flush and atomically publish one retained swap area.
 * @param user_path User pathname. @param flags Validated 32-bit Linux flags.
 * @return Zero or terminal negative errno. Caller holds execution ownership.
 */
int swap_activate_user(uint64_t user_path,uint32_t flags)
{
    /* The activation reservation itself forbids slot I/O. Resolve any
     * swapped pathname pages before acquiring it, after capability checks. */
    char *checked=kernel_malloc(VFS_PATH_MAX);if (!checked) return -ENOMEM;
    int rc=swap_copy_path(checked,user_path);
    if (rc) {kernel_free(checked);return rc;}
    struct swap_preparation prep={.pid=sched_current_pid()};
    rc=swap_storage_begin(&prep.storage);if (rc) {kernel_free(checked);return rc;}
    struct swap_backing *backing=kernel_malloc(sizeof(*backing));
    uint8_t *header=NULL;
    if (!backing) {rc=-ENOMEM;goto out;}
    *backing=(struct swap_backing){0};
    rc=swap_open_path(checked,backing);if (rc) goto out;
    uint64_t capacity=0;
    if (backing->raw) {
        rc=swap_raw_device(backing);if (rc) goto out;
        int busy=storage_disk_block_busy(STORAGE_BLOCK_DISK_ID(backing->node.volume_id),
                                         STORAGE_BLOCK_PARTITION(backing->node.volume_id));
        if (busy) {rc=busy<0 ? busy : -EBUSY;goto out;}
        rc=swap_storage_claim_range(&backing->claim,&backing->device,
                                    backing->first,backing->count,false);
        if (rc) goto out;
        capacity=backing->count/(SWAP_PAGE_BYTES/SWAP_SECTOR_BYTES);
        if (!capacity) {rc=-EINVAL;goto out;}
    } else {
        rc=swap_claim_file(backing);if (rc) goto out;
        struct linux_stat_abi stat;
        rc=vfs_path_stat(&backing->path,&stat);if (rc) goto out;
        if (stat.st_size<0) {rc=-EINVAL;goto out;}
        capacity=(uint64_t)stat.st_size/SWAP_PAGE_BYTES;
    }
    header=kernel_malloc(SWAP_PAGE_BYTES);if (!header) {rc=-ENOMEM;goto out;}
    if (backing->raw) rc=swap_raw_header(&prep,backing,header);
    else {
        uint32_t done=0;
        rc=vfs_path_read(&backing->path,0,header,SWAP_PAGE_BYTES,&done);
        if (!rc && done!=SWAP_PAGE_BYTES) rc=-EINVAL;
    }
    if (rc) goto out;
    uint32_t last,bad;
    rc=swap_header_validate(header,SWAP_PAGE_BYTES,capacity,&last,&bad);if (rc) goto out;
    (void)bad;
    struct block_device_info info;
    struct block_device_ref backend_device={0};
    rc=block_device_info(&backing->device,&info);
    if (!rc) rc=block_device_get(info.device,&backend_device);
    if (!rc) rc=swap_backend_create(&backing->backend,&backend_device,backing->first,backing->count);
    if (backend_device.device) block_device_put(&backend_device);
    if (rc) goto out;
    uint64_t sectors=((uint64_t)last+1)*(SWAP_PAGE_BYTES/SWAP_SECTOR_BYTES);
    if (backing->raw) rc=swap_backend_add_extent(backing->backend,0,backing->first,sectors);
    else rc=swap_map_file(&prep,backing,sectors);
    if (rc) goto out;
    if (!backing->raw) rc=vfs_path_sync(&backing->path,false,false);
    if (!rc) rc=swap_storage_flush(&prep.storage,&backing->device);
    if (rc) goto out;
    int32_t priority=(flags&SWAP_FLAG_PREFER) ? (int32_t)(flags&0x7fffu) : swap_default_priority;
    rc=swap_area_create_checkpoint(header,SWAP_PAGE_BYTES,capacity,priority,&backing->area,
                                   swap_checkpoint,&prep);
    if (rc) goto out;
    backing->area->backend=backing->backend;
    /* Recompute the implicit priority after checkpoints, at actual publication. */
    if (!(flags&SWAP_FLAG_PREFER)) backing->area->priority=swap_default_priority;
    rc=swap_area_publish(backing->area);
    if (!rc) {
        if (!(flags&SWAP_FLAG_PREFER) && swap_default_priority>INT32_MIN) --swap_default_priority;
        backing->next=swap_backings;swap_backings=backing;backing=NULL;
    }
out:
    kernel_free(checked);
    kernel_free(header);
    swap_backing_destroy(backing,&prep);
    swap_storage_end(&prep.storage);
    return rc==-EAGAIN ? -EIO : rc;
}

/** @brief Compare an opened raw alias by exact retained device and range.
 * @param candidate Opened path. @param existing Active area. @return True if same.
 */
static bool swap_raw_same(const struct swap_backing *candidate,const struct swap_backing *existing)
{
    return existing->raw && candidate->device.device==existing->device.device &&
           candidate->first==existing->first && candidate->count==existing->count;
}

/** @brief Count currently free ordinary frames without using the reserve.
 * @return Ordinary free frames in the current PMM snapshot.
 */
static uint64_t swap_ordinary_free(void)
{
    uint64_t free=mm_free_memory_kib()/4, reserve=mm_emergency_reserve_pages();
    return free>reserve ? free-reserve : 0;
}

/** @brief Reacquire the current caller's identity and fatal-signal state.
 * @param prep Stack-owned scalar syscall identity. @return Zero while the
 * original caller remains live, -ESRCH after a caller change or -EINTR for a
 * pending fatal signal. The current task is borrowed only under the writer and
 * never retained across a checkpoint.
 */
static int swap_caller_cancelled(const struct swap_preparation *prep)
{
    struct task *task=sched_current_task();
    if (!task || sched_current_pid()!=prep->pid) return -ESRCH;
    return kernel_signal_fatal_pending(task) ? -EINTR : 0;
}

/** @brief Recover some frames consumed by a failed partial swapoff.
 * @param prep Stack-owned checkpoint state with no active storage token.
 * @param before Ordinary free frames before draining. @param restored Number
 * of pages materialized by this drain. @return Zero when the target was reached,
 * or a terminal errno when best-effort recovery stopped short. Only scalars and
 * the caller-owned reclaim cursor cross checkpoints. The scan is non-OOM and
 * cannot release more frames than this drain materialized.
 */
static int swap_recover_failed(struct swap_preparation *prep,uint64_t before,uint64_t restored)
{
    uint64_t free=swap_ordinary_free();
    if (!restored || free>=before) return 0;
    uint64_t missing=before-free;
    uint64_t target=free+(restored<missing ? restored : missing);
    /* A physical page can cover 512 page-table entries; cache overflow chunks
     * cover 64 entries, in addition to 4096 fixed slots. Charge at least one
     * call per potential entry so even one-frame recovery has a finite bound.
     * This bounds best-effort work under churn; it is not a reservation or a
     * promise to exhaust all multiply shared address spaces. */
    uint64_t pages=mm_total_memory_kib()/4;
    uint64_t budget=pages>(UINT64_MAX-4096u)/576u ? UINT64_MAX : pages*576u+4096u;
    struct pressure_cursor cursor={0};
    while (budget && restored && swap_ordinary_free()<target) {
        int cancelled=swap_caller_cancelled(prep);
        if (cancelled) return cancelled;
        --budget;
        uint32_t limit=restored<32u ? (uint32_t)restored : 32u, released=0;
        int rc=memory_reclaim_swapoff_step(&cursor,target,limit,&released);
        if (released>restored) return -EIO;
        restored-=released;
        if (swap_ordinary_free()>=target) return 0;
        if (rc!=-EAGAIN) return rc ? rc : -ENOMEM;
        if (!restored) break;
        rc=swap_checkpoint(prep);
        if (rc) return rc;
    }
    return swap_ordinary_free()>=target ? 0 : -ENOMEM;
}

/** @brief Evacuate and detach the exact active area's retained backing.
 * @param user_path User pathname. @return Zero or terminal negative errno.
 * Failed evacuation reenables allocation without losing surviving slot data.
 */
int swap_deactivate_user(uint64_t user_path)
{
    char *checked=kernel_malloc(VFS_PATH_MAX);if (!checked) return -ENOMEM;
    int rc=swap_copy_path(checked,user_path);
    if (rc) {kernel_free(checked);return rc;}
    struct swap_preparation prep={.pid=sched_current_pid()};
    rc=swap_storage_begin(&prep.storage);if (rc) {kernel_free(checked);return rc;}
    struct swap_backing lookup={0};
    rc=swap_open_path(checked,&lookup);
    kernel_free(checked);
    if (!rc && lookup.raw) rc=swap_raw_device(&lookup);
    struct swap_backing **link=&swap_backings;
    if (!rc) {
        while (*link && (lookup.raw ? !swap_raw_same(&lookup,*link) :
               ((*link)->raw || !vfs_path_same_inode(&lookup.path,&(*link)->path)))) link=&(*link)->next;
        if (!*link) rc=-EINVAL;
    }
    if (!rc) {
        struct swap_backing *old=*link;
        uint64_t ordinary_before=swap_ordinary_free();
        rc=swap_area_begin_drain(old->area);
        if (!rc) {
            struct swap_drain_cursor cursor={0};
            uint32_t work;
            bool checkpoint_failed=false;
            /* A slot transfer acquires its own P29 token; never nest the
             * pathname-resolution reservation around the backend. */
            swap_storage_end(&prep.storage);
            do {
                rc=swap_drain_step(old->area,&cursor,SWAP_BATCH,&work);
                if (!rc) {
                    rc=swap_checkpoint(&prep);
                    if (!rc) rc=swap_caller_cancelled(&prep);
                    if (rc) checkpoint_failed=true;
                }
            } while (!rc);
            if (rc==1) {
                /* Other areas may have changed while the gate was released. */
                link=&swap_backings;
                while (*link && *link!=old) link=&(*link)->next;
                if (!*link) panic("drained swap backing missing");
                *link=old->next;
                swap_backing_destroy(old,&prep);
                rc=0;
            } else {
                swap_area_abort_drain(old->area);
                /* After re-enabling the area another swapoff may retire it at
                 * a checkpoint. Do not retain either backing/list pointer. */
                old=NULL;link=NULL;
                int recovery=(checkpoint_failed || rc==-EINTR || rc==-ESRCH) ? rc :
                    swap_recover_failed(&prep,ordinary_before,cursor.restored);
                if (recovery)
                    console_warn("[memory] swapoff recovery incomplete rc=%d free=%llu before=%llu\n",
                                 recovery,(unsigned long long)swap_ordinary_free(),
                                 (unsigned long long)ordinary_before);
            }
        }
    }
    if (lookup.device.device) block_device_put(&lookup.device);
    if (lookup.inode_writer) storage_inode_write_put(lookup.inode);
    if (lookup.inode) (void)storage_inode_put(lookup.inode);
    vfs_file_close(lookup.open);vfs_path_put(&lookup.path);
    swap_storage_end(&prep.storage);
    return rc==-EAGAIN ? -EIO : rc==SWAP_STORAGE_CONTENDED ? -EBUSY : rc;
}
