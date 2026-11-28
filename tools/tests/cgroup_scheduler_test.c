/* SPDX-License-Identifier: Apache-2.0 */
/* Real scheduler, hierarchy and cgroup backend. Model only MM/CPU, empty
 * descriptors, and the deferred notification boundary in this host fixture. */
#define main exit_logging_fixture_main
#include "task_exit_logging_test.c"
#undef main
#include <reliefnt/deferred.h>
#include <reliefnt/fsnotify.h>
#include <reliefnt/text_stream.h>

static bool fail_clone_setup;

/** @brief Supply physical memory for the scheduler's default process limits. */
uint64_t mm_total_memory_kib(void) { return 65536; }
/** @brief Give each modeled address space a nonzero root; no pages are accessed. */
bool address_space_create(struct address_space *as)
{ as->cr3 = 0x2000; return true; }
/** @brief Accept only a stack on a created address space. */
bool address_space_map_user_stack(struct address_space *as, uint64_t top)
{ assert(as->cr3 && top); return true; }
/** @brief Model successful MM duplication; fork still runs its real scheduler path. */
bool address_space_clone_cow(struct address_space *from, struct address_space *to)
{ assert(from->cr3 && !to->cr3); return address_space_create(to); }
/** @brief Supply a generation without allocating a host page table. */
int mm_context_ensure_generation(struct address_space *as, uint64_t *generation)
{ assert(as->cr3 && !scheduler_lock_depth); *generation = 1; return 0; }
/** @brief The fixture never writes through a Linux clone TID pointer. */
bool user_range_writable(uint64_t address, uint64_t length)
{ (void)address; (void)length; abort(); }
/** @brief Reject unexpected user-memory writes. */
int user_copy_to_task(struct task *task, uint64_t address, const void *source, uint64_t length)
{ (void)task; (void)address; (void)source; (void)length; abort(); }
/** @brief Reject unexpected page-permission queries. */
bool address_space_user_page_writable(const struct address_space *as, uint64_t address)
{ (void)as; (void)address; abort(); }
/** @brief Reject unexpected COW faults. */
bool address_space_handle_cow_fault(struct address_space *as, uint64_t address)
{ (void)as; (void)address; abort(); }
/** @brief No host FPU state is needed for scheduler accounting. */
void arch_fpu_task_init(void *state) { assert(state); }
/** @brief No host FPU state is needed when cloning an abstract task. */
void arch_fpu_save(void *state) { assert(state); }
/** @brief Only empty descriptor tables are modeled. */
int syscall_clone_task_files(const struct task *parent, struct task *child)
{ assert(!parent->file_extra_count && !child->file_extra_count && !parent->pty_fd_count); return 0; }
/** @brief Inject a failure after fork has reserved its slot and cloned descriptors. */
int task_sysv_sem_clone(struct task *parent, struct task *child, uint64_t flags)
{ assert(!parent->sysv_undo && !child->sysv_undo && flags == 17); return fail_clone_setup ? -ENOMEM : 0; }
/** @brief No executable inode is installed in the modeled MM. */
void storage_inode_retain(struct storage_inode_ref *inode) { assert(!inode); }
/** @brief Reject an executable write denial on an empty image. */
int storage_inode_deny_write(struct storage_inode_ref *inode) { assert(!inode); abort(); }
/** @brief The fixture starts from root credentials with no supplementary groups. */
void task_groups_retain(struct task_groups *groups) { assert(!groups); }
/** @brief No mapped file descriptors are owned by the modeled MM. */
struct task_file *task_file_get(struct task_file *file) { assert(!file); return NULL; }
/** @brief Reject a device-backed VMA in the empty modeled address space. */
int device_mapping_get(struct device_mapping *mapping) { (void)mapping; abort(); }
/** @brief Reject a device-backed range in the empty modeled address space. */
int device_mapping_retain_range(struct device_mapping *mapping, uint64_t offset, uint64_t length)
{ (void)mapping; (void)offset; (void)length; abort(); }
/** @brief The modeled address space has no SysV shared-memory attachments. */
int sysv_shm_vma_clone(struct task *task, struct task_vma *dst, const struct task_vma *src)
{ assert(task && dst && src && !src->sysv_shm_attachment); return 0; }
/** @brief Scheduling work may wake tasks and must not hold the scheduler lock. */
void kernel_deferred_request(enum kernel_deferred_service service)
{ assert(service == DEFERRED_VFS && !scheduler_lock_depth); }
/** @brief No inotify watchers; delivery itself must still run outside the lock. */
void fsnotify_emit(const struct fsnotify_key *key, uint32_t mask, uint32_t cookie,
                   const char *name, bool unlinked)
{ assert(key && mask == IN_MODIFY && !cookie && !name && !unlinked && !scheduler_lock_depth); }
/** @brief The fixtures for inotify itself live in the mount backend suite. */
bool fsnotify_has_watches(void) { return false; }
/** @brief Reject an unexpected watch deletion. */
void fsnotify_delete(const struct fsnotify_key *key) { (void)key; abort(); }

/** @brief Create and pin a child through the mounted cgroup backend. */
static struct vfs_path make_group(const struct vfs_path *parent, const char *name)
{
    struct storage_node node = {0};
    const struct reliefos_permissions mode = {.mode = 0755};
    struct vfs_path path;
    assert(!parent->mount->ops->create_node(parent->mount->context, vfs_path_node(parent),
                                           name, LINUX_S_IFDIR, &mode, NULL, &node, NULL));
    assert(!vfs_path_lookup_child(parent, name, &path));
    return path;
}

/** @brief Read or write a cgroup control and check the exact returned bytes. */
static void control(const struct vfs_path *group, const char *name, const char *text, bool write)
{
    struct vfs_path path;
    char bytes[64];
    uint32_t count = 0;
    assert(!vfs_path_lookup_child(group, name, &path));
    struct storage_node node = *vfs_path_node(&path);
    if (write)
        assert(!path.mount->ops->write(path.mount->context, &node, 0, text, strlen(text), &count));
    else
        assert(!path.mount->ops->read(path.mount->context, &node, 0, bytes, sizeof(bytes), &count, 0));
    assert(count == strlen(text) && (write || !memcmp(bytes, text, count)));
    vfs_path_put(&path);
}

/** @brief Check the real membership emitter, including its deleted-path suffix. */
static void membership(const struct task *task, const char *expected)
{
    char bytes[80];
    struct text_stream text = {.buffer = bytes, .capacity = sizeof(bytes)};
    cgroup2_proc_emit(task, &text);
    assert(text.written == strlen(expected) && !memcmp(bytes, expected, text.written));
}

/** @brief Check fork, internal publication, exit and reap against a real hierarchy. */
int main(void)
{
    next_pid = 200;
    uint32_t published = proc_created;
    int64_t actor_pid = sched_create_user_task("actor", RELIEFNT_USER_BASE, USER_STACK_TOP, 0, 0);
    assert(actor_pid > 1 && !sched_publish_user_task((uint32_t)actor_pid));
    struct task *actor = sched_find((uint32_t)actor_pid);
    assert(actor && actor->state == TASK_READY && proc_created == published + 1);
    current_task[0] = actor;
    current_pid[0] = actor->pid;

    struct vfs_mount *mount = NULL;
    struct vfs_namespace ns = {0};
    struct vfs_path root;
    assert(!cgroup2_create_vfs_mount(NULL, 0, NULL, &mount));
    assert(!vfs_namespace_init(&ns, mount) && !vfs_namespace_root(&ns, &root));
    vfs_mount_put(mount);
    struct vfs_path parent = make_group(&root, "jobs");
    struct vfs_path caller = make_group(&parent, "caller");
    struct vfs_path worker = make_group(&parent, "worker");
    control(&root, "cgroup.subtree_control", "+pids", true);
    control(&parent, "cgroup.subtree_control", "+pids", true);
    control(&parent, "pids.max", "2", true);
    control(&caller, "cgroup.procs", "0", true);
    membership(actor, "0::/jobs/caller\n");

    fail_clone_setup = true;
    assert(sched_clone_current(&actor->frame, 17, 0, 0, 0, 0) == -ENOMEM);
    fail_clone_setup = false;
    control(&parent, "pids.current", "1\n", false);
    control(&parent, "pids.peak", "1\n", false);
    assert(proc_created == published + 1);

    int64_t child_pid = sched_clone_current(&actor->frame, 17, 0, 0, 0, 0);
    struct task *child = child_pid > 0 ? sched_find((uint32_t)child_pid) : NULL;
    assert(child && child->state == TASK_READY && proc_created == published + 2);
    membership(child, "0::/jobs/caller\n");
    char pid[16];
    snprintf(pid, sizeof(pid), "%u", child->pid);
    control(&worker, "cgroup.procs", pid, true);
    assert(sched_clone_current(&actor->frame, 17, 0, 0, 0, 0) == -EAGAIN);
    control(&parent, "pids.current", "2\n", false);
    control(&parent, "pids.peak", "2\n", false);
    control(&parent, "pids.events.local", "max 1\n", false);
    control(&caller, "pids.events", "max 0\n", false);

    int64_t draft = sched_create_user_task("draft", RELIEFNT_USER_BASE, USER_STACK_TOP, actor->pid, 0);
    assert(draft > 0 && !sched_wait_reap(actor->pid, (int32_t)draft, LINUX_WEXITED, NULL, NULL));
    assert(sched_publish_user_task((uint32_t)draft) == -EAGAIN);
    sched_exit((uint32_t)draft, 127);
    control(&parent, "pids.current", "2\n", false);
    control(&parent, "pids.events", "max 2\n", false);

    sched_exit((uint32_t)child_pid, 23);
    control(&worker, "cgroup.events", "populated 0\nfrozen 0\n", false);
    control(&worker, "pids.current", "1\n", false);
    int status = 0;
    assert(sched_wait_reap(actor->pid, (int32_t)child_pid, LINUX_WEXITED | LINUX_WNOWAIT,
                           &status, NULL) == child_pid && status == 23 << 8);
    assert(sched_clone_current(&actor->frame, 17, 0, 0, 0, 0) == -EAGAIN);
    assert(!worker.mount->ops->remove_node(worker.mount->context, &worker, true));
    membership(child, "0::/jobs/worker (deleted)\n");

    /* A retained zombie is not a valid source of membership at publication. */
    draft = sched_create_user_task("root-fallback", RELIEFNT_USER_BASE, USER_STACK_TOP,
                                  (uint32_t)child_pid, 0);
    assert(draft > 0 && !sched_publish_user_task((uint32_t)draft));
    membership(sched_find((uint32_t)draft), "0::/\n");
    sched_exit((uint32_t)draft, 0);
    assert(sched_wait_reap(actor->pid, (int32_t)child_pid, LINUX_WEXITED,
                           &status, NULL) == child_pid && !sched_find((uint32_t)child_pid));
    control(&parent, "pids.current", "1\n", false);

    draft = sched_create_user_task("published", RELIEFNT_USER_BASE, USER_STACK_TOP, actor->pid, 0);
    assert(draft > 0 && !sched_publish_user_task((uint32_t)draft));
    membership(sched_find((uint32_t)draft), "0::/jobs/caller\n");
    control(&caller, "pids.current", "2\n", false);
    sched_exit((uint32_t)draft, 0);
    control(&caller, "pids.current", "1\n", false);
    control(&root, "cgroup.procs", "0", true);
    assert(!caller.mount->ops->remove_node(caller.mount->context, &caller, true));
    assert(!parent.mount->ops->remove_node(parent.mount->context, &parent, true));
    vfs_path_put(&worker);
    vfs_path_put(&caller);
    vfs_path_put(&parent);
    vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns));
    cgroup2_service(16);
    sched_exit(actor->pid, 0);
    for (uint32_t i = 0; i < task_count; ++i) {
        sched_task_identity_detach(tasks[i]);
        free(tasks[i]);
    }
    free(tasks);
    tasks = NULL;
    current_task[0] = NULL;
    puts("PASS cgroup2 real scheduler: ancestor admission, clone rollback, launch, exit, WNOWAIT and reap");
    return 0;
}
