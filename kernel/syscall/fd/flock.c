#include "../internal.h"

static bool syscall_flock_node_equal(const struct storage_node *a,
                                     const struct storage_node *b);

static struct reliefos_flock_entry *syscall_flock_find(const struct storage_node *node);

static struct reliefos_flock_entry *syscall_flock_alloc(const struct storage_node *node);

static bool syscall_flock_conflicts(const struct reliefos_flock_entry *entry,
                                    uint32_t requested, const struct task_file *owner);


static bool syscall_flock_node_equal(const struct storage_node *a,
                                     const struct storage_node *b)
{
    return a && b && a->type == b->type && a->first_cluster == b->first_cluster &&
           a->volume_id == b->volume_id;
}

static struct reliefos_flock_entry *syscall_flock_find(const struct storage_node *node)
{
    for (uint32_t i = 0; i < RELIEFOS_FLOCK_MAX; ++i)
        if (reliefos_flocks[i].used && syscall_flock_node_equal(&reliefos_flocks[i].node, node))
            return &reliefos_flocks[i];
    return NULL;
}

static struct reliefos_flock_entry *syscall_flock_alloc(const struct storage_node *node)
{
    for (uint32_t i = 0; i < RELIEFOS_FLOCK_MAX; ++i) {
        if (!reliefos_flocks[i].used) {
            reliefos_flocks[i] = (struct reliefos_flock_entry){
                .used = 1, .node = *node,
            };
            kernel_wait_queue_init(&reliefos_flocks[i].waiters);
            return &reliefos_flocks[i];
        }
    }
    return NULL;
}

void syscall_flock_release_owner(struct task_file *owner)
{
    if (!owner) return;
    for (uint32_t i = 0; i < RELIEFOS_FLOCK_MAX; ++i) {
        struct reliefos_flock_entry *entry = &reliefos_flocks[i];
        if (entry->used && entry->owner == owner) {
            entry->owner = NULL;
            entry->type = 0;
            (void)kernel_wait_queue_wake_all(&entry->waiters);
            entry->used = 0;
        } else if (entry->used && entry->type == RELIEFOS_FLOCK_SH) {
            for (uint32_t j = 0; j < entry->shared_count; ++j) {
                if (entry->shared_owners[j] == owner) {
                    for (uint32_t k = j + 1; k < entry->shared_count; ++k)
                        entry->shared_owners[k - 1] = entry->shared_owners[k];
                    entry->shared_owners[--entry->shared_count] = NULL;
                    owner->flock_type = 0;
                    if (!entry->shared_count) {
                        entry->type = 0;
                        (void)kernel_wait_queue_wake_all(&entry->waiters);
                        entry->used = 0;
                    }
                    break;
                }
            }
        }
    }
}

static bool syscall_flock_conflicts(const struct reliefos_flock_entry *entry,
                                    uint32_t requested, const struct task_file *owner)
{
    if (!entry || !entry->used || !entry->type) return false;
    if (entry->owner == owner) return false;
    if (entry->type == RELIEFOS_FLOCK_SH) {
        for (uint32_t i = 0; i < entry->shared_count; ++i)
            if (entry->shared_owners[i] == owner) return false;
    }
    return requested == RELIEFOS_FLOCK_EX || entry->type == RELIEFOS_FLOCK_EX;
}

int64_t syscall_flock(uint64_t fd_arg, uint32_t operation)
{
    struct task *task = sched_current_task();
    struct task_file *file;
    struct reliefos_flock_entry *entry;
    uint32_t requested;
    if (!task) return -RELIEFOS_ESRCH;
    if (operation & ~(RELIEFOS_FLOCK_SH | RELIEFOS_FLOCK_EX | RELIEFOS_FLOCK_NB | RELIEFOS_FLOCK_UN))
        return -RELIEFOS_EINVAL;
    if ((operation & (RELIEFOS_FLOCK_SH | RELIEFOS_FLOCK_EX)) ==
        (RELIEFOS_FLOCK_SH | RELIEFOS_FLOCK_EX)) return -RELIEFOS_EINVAL;
    file = task_file_for_fd(task, (int)(uint32_t)fd_arg);
    if (!file) return -RELIEFOS_EBADF;
    if (file->node.type != RELIEFOS_FS_TYPE_FILE) return -RELIEFOS_EBADF;
    entry = syscall_flock_find(&file->node);
    if (operation & RELIEFOS_FLOCK_UN) {
        if (!entry || !file->flock_type) return 0;
        if (file->flock_type == RELIEFOS_FLOCK_EX && entry->owner == file) {
            entry->owner = NULL;
        } else if (file->flock_type == RELIEFOS_FLOCK_SH && entry->type == RELIEFOS_FLOCK_SH) {
            for (uint32_t i = 0; i < entry->shared_count; ++i) {
                if (entry->shared_owners[i] == file) {
                    for (uint32_t j = i + 1; j < entry->shared_count; ++j)
                        entry->shared_owners[j - 1] = entry->shared_owners[j];
                    entry->shared_owners[--entry->shared_count] = NULL;
                    break;
                }
            }
        }
        file->flock_type = 0;
        if (!entry->owner && !entry->shared_count) {
            entry->type = 0;
            (void)kernel_wait_queue_wake_all(&entry->waiters);
            entry->used = 0;
        }
        return 0;
    }
    requested = operation & RELIEFOS_FLOCK_EX ? RELIEFOS_FLOCK_EX : RELIEFOS_FLOCK_SH;
    if (!entry) {
        entry = syscall_flock_alloc(&file->node);
        if (!entry) return -RELIEFOS_ENFILE;
    }
    if (syscall_flock_conflicts(entry, requested, file)) {
        if (operation & RELIEFOS_FLOCK_NB) return -RELIEFOS_EWOULDBLOCK;
        kernel_wait_queue_block_current(&entry->waiters);
        return KERNEL_SYSCALL_BLOCKED;
    }
    if (file->flock_type == requested) return 0;
    if (file->flock_type == RELIEFOS_FLOCK_SH && requested == RELIEFOS_FLOCK_EX) {
        if (entry->shared_count > 1) {
            if (operation & RELIEFOS_FLOCK_NB) return -RELIEFOS_EWOULDBLOCK;
            kernel_wait_queue_block_current(&entry->waiters);
            return KERNEL_SYSCALL_BLOCKED;
        }
        for (uint32_t i = 0; i < entry->shared_count; ++i) {
            if (entry->shared_owners[i] == file) {
                for (uint32_t j = i + 1; j < entry->shared_count; ++j)
                    entry->shared_owners[j - 1] = entry->shared_owners[j];
                entry->shared_owners[--entry->shared_count] = NULL;
                break;
            }
        }
    }
    entry->type = requested;
    if (requested == RELIEFOS_FLOCK_EX) entry->owner = file;
    else {
        entry->owner = NULL;
        if (entry->shared_count >= RELIEFOS_FLOCK_MAX) return -RELIEFOS_ENFILE;
        entry->shared_owners[entry->shared_count++] = file;
    }
    file->flock_type = requested;
    return 0;
}
