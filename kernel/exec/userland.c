/*
 * ReliefOS userland launcher: prepares process images, stacks, and arguments.
 * Creates user tasks, maps executables, and enters the Ring-3 scheduler path.
 */
#include <reliefnt/arch.h>
#include <reliefnt/console.h>
#include <reliefnt/elf.h>
#include <reliefnt/input.h>
#include <reliefnt/kernel.h>
#include <reliefnt/mm.h>
#include <reliefnt/pty.h>
#include <reliefnt/random.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/uts.h>
#include <reliefnt/syscall.h>
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>
#include <reliefnt/userland.h>
#include <reliefnt/svga.h>
#include <reliefos/layout.h>
#include <reliefnt/permissions.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <linux/mount.h>

#define USER_STACK_TOP (RELIEFNT_USER_TOP - 0x1000ULL)
#define EXEC_STACK_ALIGN 16ULL

struct exec_launch {
    uint32_t argc;
    uint32_t envc;
    uint32_t data_len;
    const char *argv[SCHED_EXEC_ARG_MAX + 1];
    const char *envp[SCHED_EXEC_ENV_MAX + 1];
    char data[SCHED_EXEC_DATA_MAX];
};

/*
 * Service image identity markers: the M1 authority decision. A root executor
 * running a root-owned, non-group/world-writable image whose gid is one of
 * these reserved, memberless groups receives TASK_FLAG_SERVICE (plus
 * TASK_FLAG_WINDOW_SERVER for the window-server role). The path name grants
 * nothing. Mirrored by the rootfs staging plan and guest /etc/group:
 *
 *     reliefos-window-server:x:60001:  desktop.elf          role window server
 *     reliefos-service:x:60002:        windowd.elf, imd.elf role service
 */
#define RELIEFOS_GID_WINDOW_SERVER 60001u
#define RELIEFOS_GID_SERVICE 60002u

static uint32_t init_pid;
static uint32_t desktop_pid;
static uint32_t windowd_pid;
static uint32_t tty_pid;
/* Boot loader command line captured at init for the /proc/cmdline mechanism. */
static const char *boot_cmdline;
/* elf.c keeps a bounded header scratch buffer and ASLR state at file scope.
 * Serialize lazy image construction so APs cannot overwrite that state while
 * the BSP (or another AP) is mapping a different executable. */
static struct kernel_spinlock image_load_lock = KERNEL_SPINLOCK_INIT;

void userland_loader_lock(uint64_t *flags)
{
    /* ELF header probing uses one scratch buffer, but loading may perform
     * storage IO. Preserve interrupt delivery while waiting for the scratch
     * buffer so AP timers continue to account progress. */
    __asm__ volatile("pushfq; popq %0" : "=r"(*flags) : : "memory");
    kernel_spin_lock(&image_load_lock);
}

void userland_loader_unlock(uint64_t flags)
{
    kernel_spin_unlock(&image_load_lock);
    kernel_irq_restore(flags);
}

/**
 * @brief Copy src into dst up to dst_len-1 chars, always NUL-terminating dst.
 */
static void copy_text(char *dst, uint32_t dst_len, const char *src)
{
    uint32_t i = 0;
    if (!dst || dst_len == 0) {
        return;
    }
    if (src) {
        while (i + 1 < dst_len && src[i]) {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = 0;
}

/**
 * @brief Copy the final path component (after the last '/') into dst.
 */
static void task_name_from_path(const char *path, char *dst, uint32_t dst_len)
{
    const char *name = path;
    if (!path) {
        copy_text(dst, dst_len, "");
        return;
    }
    for (const char *p = path; *p; ++p) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    copy_text(dst, dst_len, name);
}

/**
 * @brief Append one directory entry (type + name) to entries if it fits; always increments *count.
 */
/**
 * @brief Free the page-backed buffer allocated for a loaded executable image.
 */
static void free_image_buffer(const void *image, size_t image_len)
{
    uint32_t pages;
    if (!image || !image_len) {
        return;
    }
    pages = (uint32_t)((image_len + 4095ULL) / 4096ULL);
    mm_free_pages((uint64_t)(uintptr_t)image, pages);
}

/**
 * @brief Pack a NUL-terminated argv/envp array into the shared data buffer and record pointers into dst; returns the entry count, or 0xffffffff on overflow.
 */
static uint32_t copy_exec_vector(char *dst[], uint32_t dst_cap,
                                 const char *const src[], char *data,
                                 uint32_t data_cap, uint32_t *data_len)
{
    uint32_t count = 0;
    if (!dst || !dst_cap || !data || !data_len) {
        return 0;
    }
    if (src) {
        while (src[count] && count + 1 < dst_cap) {
            uint32_t start = *data_len;
            uint32_t len = 0;
            while (src[count][len]) {
                if (*data_len + len + 1 >= data_cap) {
                    return 0xffffffffu;
                }
                data[*data_len + len] = src[count][len];
                ++len;
            }
            data[*data_len + len] = 0;
            dst[count] = data + start;
            *data_len += len + 1;
            ++count;
        }
        if (src[count]) {
            return 0xffffffffu;
        }
    }
    dst[count] = 0;
    return count;
}

/**
 * @brief Populate launch with the packed argv/envp for path; returns 0, or a negative errno (-22 no path, -7 overflow).
 */
static int build_exec_launch(struct exec_launch *launch, const char *path,
                             const char *const argv[], const char *const envp[])
{
    uint32_t data_len = 0;
    uint32_t argc;
    uint32_t envc;
    if (!launch || !path || !path[0]) {
        return -22;
    }
    for (uint32_t i = 0; i < SCHED_EXEC_ARG_MAX + 1; ++i) {
        launch->argv[i] = 0;
    }
    for (uint32_t i = 0; i < SCHED_EXEC_ENV_MAX + 1; ++i) {
        launch->envp[i] = 0;
    }
    if (!argv || !argv[0]) {
        argv = (const char *const[]){path, 0};
    }
    argc = copy_exec_vector((char **)launch->argv, SCHED_EXEC_ARG_MAX + 1,
                            argv, launch->data, sizeof(launch->data), &data_len);
    if (argc == 0xffffffffu) {
        return -7;
    }
    envc = copy_exec_vector((char **)launch->envp, SCHED_EXEC_ENV_MAX + 1,
                            envp, launch->data, sizeof(launch->data), &data_len);
    if (envc == 0xffffffffu) {
        return -7;
    }
    launch->argc = argc;
    launch->envc = envc;
    launch->data_len = data_len;
    return 0;
}

/**
 * @brief Return a kernel pointer to the physical page backing vaddr, or NULL if unmapped.
 */
static void *user_ptr_for_phys(const struct address_space *as, uint64_t vaddr)
{
    uint64_t phys = address_space_user_page_phys(as, vaddr);
    if (!phys) {
        return 0;
    }
    return (void *)(uintptr_t)(phys + (vaddr & 0xfffULL));
}

/**
 * @brief Write value into the address space at vaddr byte by byte; returns 0, or -12 on an unmapped page.
 */
static int write_user_u64(const struct address_space *as, uint64_t vaddr, uint64_t value)
{
    for (uint32_t i = 0; i < sizeof(value); ++i) {
        uint8_t *dst = (uint8_t *)user_ptr_for_phys(as, vaddr + i);
        if (!dst) {
            return -12;
        }
        *dst = (uint8_t)((value >> (i * 8)) & 0xffu);
    }
    return 0;
}

/**
 * @brief Lay out argv/envp (and the dynamic-launch record) on the task's user stack and point the initial frame at them.
 */
static int prepare_user_exec_stack(struct task *task, const char *execfn)
{
    uint64_t sp;
    uint64_t argc_base;
    uint64_t argv_base;
    uint64_t envp_base;
    uint64_t auxv_base;
    uint64_t random_base;
    uint64_t strings_base;
    uint64_t execfn_base;
    uint32_t argc;
    size_t path_len;
    uint64_t argv_bytes;
    uint64_t envp_bytes;
    const uint64_t auxv_count = 16;
    const uint64_t auxv_bytes = auxv_count * 2 * sizeof(uint64_t);
    enum {
        AT_NULL = 0,
        AT_PHDR = 3,
        AT_PHENT = 4,
        AT_PHNUM = 5,
        AT_PAGESZ = 6,
        AT_BASE = 7,
        AT_ENTRY = 9,
        AT_UID = 11,
        AT_EUID = 12,
        AT_GID = 13,
        AT_EGID = 14,
        AT_SECURE = 23,
        AT_RANDOM = 25,
        AT_RSEQ_FEATURE_SIZE = 27,
        AT_RSEQ_ALIGN = 28,
        AT_EXECFN = 31,
    };
    uint64_t launch_base = 0;
    if (!task) {
        return -22;
    }
    sp = task->stack_top;
    if (task->dynamic_launch.abi_major) {
        sp = (sp - sizeof(task->dynamic_launch)) & ~(EXEC_STACK_ALIGN - 1ULL);
        launch_base = sp;
    }
    argc = task->exec_argc ? task->exec_argc : 1;
    if (!execfn) execfn = task->path;
    path_len = __builtin_strlen(execfn);
    strings_base = (sp - task->exec_data_len - path_len - 2) & ~(EXEC_STACK_ALIGN - 1ULL);
    execfn_base = strings_base + task->exec_data_len;
    random_base = (strings_base - 16ULL) & ~(EXEC_STACK_ALIGN - 1ULL);
    envp_bytes = (uint64_t)(task->exec_envc + 1) * sizeof(uint64_t);
    argv_bytes = (uint64_t)(argc + 1) * sizeof(uint64_t);
    /* Align the entire vector once. Linux crt scans consecutive words. */
    argc_base = (random_base - auxv_bytes - envp_bytes - argv_bytes - sizeof(uint64_t)) &
                ~(EXEC_STACK_ALIGN - 1ULL);
    argv_base = argc_base + sizeof(uint64_t);
    envp_base = argv_base + argv_bytes;
    auxv_base = envp_base + envp_bytes;
    if (argc_base < task->stack_top - (uint64_t)RELIEFNT_USER_STACK_PAGES * 4096ULL) {
        return -12;
    }

    for (uint32_t i = 0; i < task->exec_data_len; ++i) {
        char *dst = (char *)user_ptr_for_phys(sched_task_as(task), strings_base + i);
        if (!dst) {
            return -12;
        }
        *dst = task->exec_data[i];
    }

    for (size_t i = 0; i < path_len + 2; ++i) {
        char *dst = (char *)user_ptr_for_phys(sched_task_as(task), execfn_base + i);
        if (!dst) return -12;
        *dst = i < path_len ? execfn[i] : 0;
    }
    /* Linux supplies an empty argv[0] when execve receives an empty vector. */
    if (!task->exec_argc &&
        write_user_u64(sched_task_as(task), argv_base, execfn_base + path_len + 1) < 0) return -12;

    for (uint32_t i = 0; i < task->exec_argc; ++i) {
        uintptr_t ptr = (uintptr_t)task->exec_argv[i];
        uintptr_t data_begin = (uintptr_t)task->exec_data;
        uintptr_t data_end = data_begin + task->exec_data_len;
        if (!task->exec_argv[i] || ptr < data_begin || ptr >= data_end) {
            return -22;
        }
        uint64_t offset = (uint64_t)(ptr - data_begin);
        if (write_user_u64(sched_task_as(task), argv_base + (uint64_t)i * sizeof(uint64_t),
                           strings_base + offset) < 0) {
            return -12;
        }
    }
    if (write_user_u64(sched_task_as(task), argv_base + (uint64_t)argc * sizeof(uint64_t), 0) < 0) {
        return -12;
    }
    for (uint32_t i = 0; i < task->exec_envc; ++i) {
        uintptr_t ptr = (uintptr_t)task->exec_envp[i];
        uintptr_t data_begin = (uintptr_t)task->exec_data;
        uintptr_t data_end = data_begin + task->exec_data_len;
        if (!task->exec_envp[i] || ptr < data_begin || ptr >= data_end) {
            return -22;
        }
        uint64_t offset = (uint64_t)(ptr - data_begin);
        if (write_user_u64(sched_task_as(task), envp_base + (uint64_t)i * sizeof(uint64_t),
                           strings_base + offset) < 0) {
            return -12;
        }
    }
    if (write_user_u64(sched_task_as(task), envp_base + (uint64_t)task->exec_envc * sizeof(uint64_t), 0) < 0) {
        return -12;
    }

    for (uint32_t i = 0; i < 16; ++i) {
        uint8_t *dst = (uint8_t *)user_ptr_for_phys(sched_task_as(task), random_base + i);
        if (!dst) return -12;
        *dst = task->dynamic_launch.random[i];
    }
    {
        uint64_t auxv[auxv_count * 2] = {
            AT_PHDR, task->dynamic_launch.main_phdr,
            AT_PHENT, 56,
            AT_PHNUM, task->exec_phnum,
            AT_PAGESZ, 4096,
            AT_BASE, task->dynamic_launch.interp_base,
            AT_ENTRY, task->dynamic_launch.main_entry,
            AT_UID, task->uid,
            AT_EUID, task->euid,
            AT_GID, task->gid,
            AT_EGID, task->egid,
            AT_SECURE, task->secure_exec || task->uid != task->euid || task->gid != task->egid,
            AT_RANDOM, random_base,
            AT_RSEQ_FEATURE_SIZE, 32,
            AT_RSEQ_ALIGN, 32,
            AT_EXECFN, execfn_base,
            AT_NULL, 0,
        };
        for (uint32_t i = 0; i < sizeof(auxv) / sizeof(auxv[0]); ++i) {
            if (write_user_u64(sched_task_as(task), auxv_base + (uint64_t)i * sizeof(uint64_t), auxv[i]) < 0) {
                return -12;
            }
        }
    }
    if (write_user_u64(sched_task_as(task), argc_base, argc) < 0) return -12;

    if (launch_base) {
        for (uint32_t i = 0; i < sizeof(task->dynamic_launch); ++i) {
            uint8_t *dst = (uint8_t *)user_ptr_for_phys(sched_task_as(task), launch_base + i);
            if (!dst) {
                return -12;
            }
            *dst = ((const uint8_t *)&task->dynamic_launch)[i];
        }
    }

    struct task_address_space_state *mm=sched_task_mm(task);
    mm->arg_start=task->exec_argc? strings_base+(uint64_t)(task->exec_argv[0]-task->exec_data):execfn_base+path_len+1;
    mm->arg_end=task->exec_argc? strings_base+(uint64_t)(task->exec_argv[task->exec_argc-1]-task->exec_data)+
        __builtin_strlen(task->exec_argv[task->exec_argc-1])+1:mm->arg_start+1;
    mm->env_start=task->exec_envc?strings_base+(uint64_t)(task->exec_envp[0]-task->exec_data):mm->arg_end;
    mm->env_end=task->exec_envc?strings_base+(uint64_t)(task->exec_envp[task->exec_envc-1]-task->exec_data)+
        __builtin_strlen(task->exec_envp[task->exec_envc-1])+1:mm->env_start;
    task->frame.rsp = argc_base;
    task->frame.rdi = argc;
    task->frame.rsi = argv_base;
    task->frame.rdx = envp_base;
    task->frame.r8 = launch_base;
    return 0;
}

/**
 * @brief Map or load the task's pending executable, set the entry frame and exec stack, and mark it started.
 */
static int userland_prepare_exec_credentials(struct task *next, const struct task *old)
{
    struct reliefos_permissions file;
    uint64_t mount_flags;
    int ret = fs_permissions_get(next->path, &next->image_node, &file);
    if (ret < 0) return ret;
    ret = storage_node_mount_flags(&next->image_node, &mount_flags);
    if (ret < 0) return ret;
    /* bprm_fill_uid reads the held ELF inode under the execution/storage lock.
     * An interpreter's set-ID bits never replace the main ELF's authority. */
    if (!old->no_new_privs && !(mount_flags & MS_NOSUID)) {
        if ((file.mode & 06000) && !sched_task_mm(next)->executable_inode) return -95;
        if (file.mode & 04000) next->euid = file.uid;
        if ((file.mode & 02010) == 02010) next->egid = file.gid;
    }
    /* Linux commoncap.c:cap_bprm_creds_from_file, without file capabilities. */
    bool effective = !(old->securebits & SECBIT_NOROOT) && !next->euid;
    bool setid = next->euid != old->uid || next->egid != old->gid;
    uint64_t permitted = 0;
    if (!(old->securebits & SECBIT_NOROOT) && (!next->uid || !next->euid))
        permitted = old->cap_bset | old->cap_inheritable;
    if (old->no_new_privs && (setid || (permitted & ~old->cap_permitted))) {
        next->euid = next->uid;
        next->egid = next->gid;
        permitted &= old->cap_permitted;
    }
    next->suid = next->fsuid = next->euid;
    next->sgid = next->fsgid = next->egid;
    if (setid) next->cap_ambient = 0;
    next->cap_permitted = permitted | next->cap_ambient;
    next->cap_effective = effective ? next->cap_permitted : next->cap_ambient;
    next->securebits &= ~SECBIT_KEEP_CAPS;
    next->secure_exec = setid || (next->uid &&
        (effective || (next->cap_permitted & ~next->cap_ambient)));
    /* setup_new_exec resets dumpability, then commit_creds may lower it. */
    if (old->uid != old->euid || old->gid != old->egid ||
        old->euid != next->euid || old->egid != next->egid ||
        old->fsuid != next->fsuid || old->fsgid != next->fsgid ||
        (next->cap_permitted & ~old->cap_permitted))
        sched_task_mm(next)->nondumpable = true;
    return 0;
}

static int userland_load_task_image_locked(struct task *task, const struct task *old, const char *execfn)
{
    struct elf_image_info loaded;

    if (!task) {
        return -22;
    }
    if ((task->flags & TASK_FLAG_STARTED) && task->frame.rip != 0) {
        return 0;
    }
    /* A started task must have a complete saved frame. Rebuilding a zero RIP
     * from the ELF entry would hide a scheduler ownership bug and lose the
     * rest of the register state, so fail it instead of changing semantics. */
    if ((task->flags & TASK_FLAG_STARTED) && task->entry != 0 && task->frame.rip == 0) {
        console_printf("[reliefnt] refusing started task with zero RIP pid=%u entry=0x%llx\n",
                       task->pid, (unsigned long long)task->entry);
        return -8;
    }
    int entropy_result = kernel_random_fill(task->dynamic_launch.random, sizeof(task->dynamic_launch.random));
    if (entropy_result < 0) {
        console_printf("[reliefnt] image entropy failed path=%s ret=%d\n", task->path, entropy_result);
        return entropy_result;
    }
    if (task->flags & TASK_FLAG_PENDING_LOAD) {
        if (task->image_node.type != RELIEFOS_FS_TYPE_FILE) {
            char resolved[RELIEFOS_FS_PATH_LEN];
            int ret = fs_permissions_resolve(task, sched_task_cwd(task), task->path,
                                             resolved, sizeof(resolved), false);
            if (!ret) ret = storage_lookup_path(resolved, &task->image_node);
            if (!ret) copy_text(task->path, sizeof(task->path), resolved);
            if (ret < 0 || task->image_node.type != RELIEFOS_FS_TYPE_FILE) {
                console_printf("[reliefnt] executable lookup failed path=%s ret=%d\n",
                               task->path, ret < 0 ? ret : -21);
                return ret < 0 ? ret : -13;
            }
        }
        int ret = storage_inode_get(&task->image_node, &sched_task_mm(task)->executable_inode);
        if (ret < 0) return ret;
        ret = elf64_map_task_image(task, &task->image_node, &loaded);
        if (ret < 0) {
            console_printf("[reliefnt] failed to map executable %s\n", task->name);
            return ret;
        }
        sched_task_mm(task)->program_break_base = loaded.program_break;
        if (!sched_task_mm(task)->program_break_base) {
            sched_task_mm(task)->program_break_base = (loaded.high_vaddr + loaded.load_bias + 4095ULL) & ~4095ULL;
        }
        sched_task_mm(task)->program_break = sched_task_mm(task)->program_break_base;
        task->flags &= ~TASK_FLAG_PENDING_LOAD;
    } else if (task->image && task->image_len) {
        if (!elf64_load_address_space(sched_task_as(task), task->image, task->image_len, &loaded)) {
            console_printf("[reliefnt] failed to load %s into private address space\n", task->name);
            return -8;
        }
        free_image_buffer(task->image, task->image_len);
        task->image = NULL;
        task->image_len = 0;
    } else {
        console_printf("[reliefnt] image has no pending load path=%s flags=0x%x node=%u\n",
                       task->path, task->flags, task->image_node.type);
        return -8;
    }

    task->entry = loaded.interpreter_entry;
    task->exec_phnum = loaded.phnum;
    task->dynamic_launch.main_entry = loaded.entry;
    task->dynamic_launch.main_phdr = loaded.phdr_vaddr;
    task->dynamic_launch.main_base = loaded.load_bias;
    task->frame.rip = task->entry;
    task->frame.rsp = task->stack_top;
    task->frame.rflags = 0x202;
    task->frame.cs = RELIEFNT_USER_CS;
    task->frame.ss = RELIEFNT_USER_DS;
    if (old) {
        int ret = userland_prepare_exec_credentials(task, old);
        if (ret < 0) return ret;
    }
    int ret = prepare_user_exec_stack(task, execfn);
    if (ret < 0) {
        console_printf("[reliefnt] failed to prepare argv/envp for %s\n", task->name);
        return ret;
    }
    task->flags |= TASK_FLAG_STARTED;

    console_printf("[reliefnt] %s prepared lazy Ring-3 image entry=0x%llx cr3=0x%llx\n",
                   task->name,
                   (unsigned long long)task->entry,
                   (unsigned long long)(*sched_task_as(task)).cr3);
    return 0;
}

static bool userland_load_task_image(struct task *task)
{
    bool result;
    uint64_t execution_flags;
    uint64_t loader_flags;
    /* Keep the lock order consistent with page-fault lazy mapping:
     * execution transaction -> ELF scratch lock -> storage.  Taking the ELF
     * lock first here used to deadlock with a CPU handling a file-backed page
     * fault: the scheduler-side loader then waited for storage's execution
     * lock while the fault path already owned that lock and waited for the
     * ELF scratch buffer.  The execution lock is reentrant, so storage calls
     * below safely join this transaction. */
    kernel_execution_lock_irqsave(&execution_flags);
    userland_loader_lock(&loader_flags);
    int load_result = userland_load_task_image_locked(task, NULL, NULL);
    result = load_result == 0;
    if (!result)
        console_printf("[reliefnt] image load failed pid=%u path=%s flags=0x%x ret=%d\n",
                       task->pid, task->path, task->flags, load_result);
    userland_loader_unlock(loader_flags);
    kernel_execution_unlock_irqrestore(execution_flags);
    return result;
}

/**
 * @brief Create the user task for path, attach image/exec/fd parameters, and return its PID or a negative errno.
 */
static int64_t spawn_pending_image_ex(const char *path, const char *task_name,
                                      const struct storage_node *node,
                                      const struct exec_launch *launch,
                                      uint32_t parent, uint32_t flags, uint32_t pty_id,
                                      int stdin_fd, int stdout_fd, int stderr_fd,
                                      int make_ready)
{
    uint32_t pid = sched_create_user_task(task_name, 0, USER_STACK_TOP, parent, flags);
    if (!pid) {
        return -12;
    }
    sched_set_task_path(pid, path);
    sched_set_task_image_node(pid, node);
    if (launch) {
        sched_set_task_exec_params(pid, launch->argc, (char *const *)launch->argv,
                                   launch->envc, (char *const *)launch->envp,
                                   launch->data, launch->data_len);
    }
    if (pty_id) {
        struct task *task = sched_find(pid);
        if (task) {
            task->pty_id = pty_id;
        }
    }
    if (stdin_fd >= 0 || stdout_fd >= 0 || stderr_fd >= 0) {
        struct task *child = sched_find(pid);
        struct task *parent_task = sched_find(parent);
        if (!child || syscall_inherit_task_fds(parent_task, child,
                                               stdin_fd, stdout_fd, stderr_fd) < 0) {
            sched_exit(pid, 127);
            return -9;
        }
    }
    /* Make the task runnable only after every field used by the loader and
     * scheduler has been published.  Bootstrap callers may defer this one
     * final transition while they attach the controlling PTY. */
    if (make_ready) {
        sched_mark_ready(pid);
    }
    return pid;
}

/**
 * @brief Create a pending-image task at path; a TASK_FLAG_SERVICE caller gets its node resolved here, ordinary callers skip the lookup.
 */
static int64_t spawn_path_internal_ex(const char *path, const char *task_name,
                                      const struct exec_launch *launch,
                                      uint32_t parent, uint32_t flags, uint32_t pty_id,
                                      int stdin_fd, int stdout_fd, int stderr_fd,
                                      int make_ready)
{
    struct storage_node node;
    int ret;
    if (flags & TASK_FLAG_SERVICE) {
        ret = storage_lookup_path(path, &node);
        if (ret < 0 || node.type != RELIEFOS_FS_TYPE_FILE) {
            console_printf("[reliefnt] spawn lookup failed path=%s ret=%d\n", path,
                           ret < 0 ? ret : -21);
            return ret < 0 ? ret : -21;
        }
        return spawn_pending_image_ex(path, task_name, &node, launch, parent, flags, pty_id,
                                      stdin_fd, stdout_fd, stderr_fd, make_ready);
    }
    return spawn_pending_image_ex(path, task_name, NULL, launch, parent, flags, pty_id,
                                  stdin_fd, stdout_fd, stderr_fd, make_ready);
}

static int64_t spawn_path_internal(const char *path, const char *task_name,
                                   const struct exec_launch *launch,
                                   uint32_t parent, uint32_t flags, uint32_t pty_id,
                                   int stdin_fd, int stdout_fd, int stderr_fd)
{
    return spawn_path_internal_ex(path, task_name, launch, parent, flags, pty_id,
                                  stdin_fd, stdout_fd, stderr_fd, 1);
}

static int64_t spawn_path_internal_deferred(const char *path, const char *task_name,
                                            const struct exec_launch *launch,
                                            uint32_t parent, uint32_t flags, uint32_t pty_id,
                                            int stdin_fd, int stdout_fd, int stderr_fd)
{
    return spawn_path_internal_ex(path, task_name, launch, parent, flags, pty_id,
                                  stdin_fd, stdout_fd, stderr_fd, 0);
}

/**
 * @brief Wait for runnable work, dropping the execution transaction while idle.
 * @param execution_flags IRQ state of the single held execution transaction;
 * updated when it is reacquired after idle. Caller must not hold other locks.
 * @return A task reserved by the current CPU, with the transaction held.
 */
static struct task *wait_for_runnable_task(uint64_t *execution_flags)
{
    struct task *next;
    for (;;) {
        input_process_pending();
        if ((next = sched_select_next_user())) break;
        /* Selection retired the previous CR3 and task reservation. Never
         * sleep holding the transaction needed by CPUs producing work. */
        kernel_execution_unlock_irqrestore(*execution_flags);
        __asm__ volatile("sti; hlt; cli");
        kernel_execution_lock_irqsave(execution_flags);
    }
    return next;
}

/**
 * @brief Serialize task retirement and selection against exec, exit and reaping.
 * @param frame Current user frame, or NULL on AP entry or a selection retry.
 * Caller has interrupts disabled and must not hold the execution transaction.
 * @return A CPU-reserved task ready for user entry, or NULL for an invalid frame.
 */
struct task *userland_schedule_from_frame(struct trap_frame *frame)
{
    uint64_t execution_flags;
    kernel_execution_lock_irqsave(&execution_flags);
    input_process_pending();
    struct task *current;
    struct task *next;
retry:
    current = sched_current_task();
    if (current && current->kind == TASK_KIND_USER && frame &&
        current->state != TASK_EXITED) {
        /* A faulting user RIP may be zero. Deliver its synchronous signal
         * before validating the frame that will actually return to userspace. */
        if ((frame->cs & 3ULL) == 3ULL) {
            /* Install a pending user signal handler on this live return
             * frame before it is published to the scheduler. */
            arch_fpu_save(current->fpu_state);
            (void)kernel_signal_deliver_pending(current, frame);
            if (current->state != TASK_EXITED && !sched_capture_current_user_frame(frame)) {
                console_printf("[reliefnt] rejected scheduler frame pid=%u rip=0x%llx cs=0x%llx\n",
                               current->pid,
                               (unsigned long long)frame->rip,
                               (unsigned long long)frame->cs);
                kernel_execution_unlock_irqrestore(execution_flags);
                return NULL;
            }
        } else {
            console_printf("[reliefnt] ignored invalid scheduler frame pid=%u rip=0x%llx cs=0x%llx\n",
                           current->pid,
                           (unsigned long long)frame->rip,
                           (unsigned long long)frame->cs);
            kernel_execution_unlock_irqrestore(execution_flags);
            return NULL;
        }
    }
    if (current && current->kind == TASK_KIND_USER && current->state == TASK_EXITED) {
        /* Stop using the retiring page tables before releasing the last mm
         * reference. Other threads retain their own reference to shared mm. */
        paging_load_cr3(paging_kernel_cr3());
        sched_quiesce_exited_current();
        sched_release_task_resources(current);
    }

    next = sched_select_next_user();
    if (!next && current && current->kind == TASK_KIND_USER && current->state == TASK_READY) {
        next = sched_reclaim_current_user();
    }
    if (!next) {
        next = wait_for_runnable_task(&execution_flags);
    }
    if (!next) {
        kernel_execution_unlock_irqrestore(execution_flags);
        return NULL;
    }
    /* A task that was woken for a pending signal never passed through the
     * live-frame path above; prepare its saved frame before entering it. */
    (void)kernel_signal_deliver_pending(next, &next->frame);
    if (next->state == TASK_EXITED) { frame = NULL; goto retry; }
    if (next->state == TASK_STOPPED || next->state == TASK_BLOCKED) {
        (void)sched_capture_current_user_frame(&next->frame);
        frame = NULL;
        goto retry;
    }
    if (!userland_load_task_image(next)) {
        sched_exit(next->pid, 127);
        frame = NULL;
        goto retry;
    }
    arch_set_user_fs(next->fs_base);
    arch_fpu_restore(next->fpu_state);
    kernel_execution_unlock_irqrestore(execution_flags);
    return next;
}

static void userland_enter_task(struct task *task) __attribute__((noreturn));

/**
 * @brief Log and enter the task's user frame at its CR3; falls into the idle loop if none.
 */
static void userland_enter_task(struct task *task)
{
    if (!task) {
        console_printf("[reliefnt] no runnable Ring-3 task, entering idle\n");
        kernel_idle_loop();
    }
    console_printf("[reliefnt] scheduler entering pid=%u name=%s rip=0x%llx rsp=0x%llx cr3=0x%llx\n",
                   task->pid,
                   task->name,
                   (unsigned long long)task->frame.rip,
                   (unsigned long long)task->frame.rsp,
                   (unsigned long long)(*sched_task_as(task)).cr3);
    smp_mark_bsp_user_entry();
    arch_enter_user_frame(&task->frame, (*sched_task_as(task)).cr3);
}

/**
 * @brief Empty an early-boot transient tree without following links or mounts.
 * @param root Absolute directory on the root volume; no userspace exists yet.
 * @return Zero or negative errno. A failure prevents stale state being reused.
 * The path itself is the traversal stack. Directory enumeration restarts after
 * deletion because ext2 entries can coalesce. No recursion or fixed entry cap.
 */
static int userland_clear_transient_tree(const char *root)
{
    char path[RELIEFOS_FS_PATH_LEN];
    struct storage_node base;
    uint32_t root_length = (uint32_t)__builtin_strlen(root);
    int ret = storage_lookup_path(root, &base);
    if (ret < 0) return ret;
    if (base.type != RELIEFOS_FS_TYPE_DIR || !(base.flags & STORAGE_NODE_FLAG_EXT2))
        return -20;
    copy_text(path, sizeof(path), root);
    for (;;) {
        struct storage_node directory;
        struct reliefos_dir_entry entry;
        uint64_t cursor = 0;
        uint32_t length = (uint32_t)__builtin_strlen(path);
        ret = storage_lookup_path(path, &directory);
        if (ret < 0) return ret;
        if (directory.volume_id != base.volume_id) return -18;
        if (directory.type != RELIEFOS_FS_TYPE_DIR) return -20;
        do {
            ret = storage_readdir_node(&directory, &cursor, &entry);
        } while (ret > 0 && entry.name[0] == '.' &&
                 (!entry.name[1] || (entry.name[1] == '.' && !entry.name[2])));
        if (ret < 0) return ret;
        if (ret == 0) {
            if (length == root_length) return 0;
            ret = storage_rmdir(path);
            if (ret < 0) return ret;
            while (length > root_length && path[length - 1] != '/') --length;
            path[length - 1] = 0;
            continue;
        }
        uint32_t name_length = 0;
        while (name_length < sizeof(entry.name) && entry.name[name_length]) {
            if (entry.name[name_length] == '/') return -5;
            ++name_length;
        }
        if (!name_length || name_length == sizeof(entry.name)) return -5;
        if (length + 1 + name_length >= sizeof(path)) return -36;
        path[length] = '/';
        copy_text(path + length + 1, sizeof(path) - length - 1, entry.name);
        struct storage_node child;
        ret = storage_lookup_path(path, &child);
        if (ret < 0) return ret;
        if (child.volume_id != base.volume_id) return -18;
        if (child.type == RELIEFOS_FS_TYPE_DIR) continue;
        ret = storage_unlink(path); /* literal link/socket/file, never its target */
        if (ret < 0) return ret;
        path[length] = 0;
    }
}

/** @brief Reset boot-scoped transient state before any process can open an IPC endpoint. */
static int userland_prepare_runtime(void)
{
    static const struct { const char *path; uint32_t mode; } directories[] = {
        {"/run", 0755}, {"/run/lock", 0755},
        {"/dev/shm", 01777},
    };
    int ret = userland_clear_transient_tree("/run");
    if (ret < 0) return ret;
    ret = userland_clear_transient_tree("/dev/shm");
    if (ret < 0) return ret;
    for (uint32_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        struct storage_node node;
        struct reliefos_permissions mode = {directories[i].mode, 0, 0};
        ret = storage_lookup_path(directories[i].path, &node);
        if (ret == -2) {
            ret = storage_mkdir(directories[i].path);
            if (ret < 0) return ret;
            ret = storage_lookup_path(directories[i].path, &node);
        }
        if (ret < 0) return ret;
        if (node.type != RELIEFOS_FS_TYPE_DIR) return -20;
        ret = storage_inode_permissions(&node, &mode, true);
        if (ret < 0) return ret;
    }
    return 0;
}

/**
 * @brief Return the boot loader kernel command line captured at init time.
 * @return NUL-terminated raw command line for /proc/cmdline; never NULL, an
 * empty string when the boot handoff carried none. The string lives in boot
 * loader memory that stays mapped for the kernel lifetime.
 */
const char *userland_boot_cmdline(void)
{
    return boot_cmdline ? boot_cmdline : "";
}

/**
 * @brief Prepare the root and console, then execute only the configured PID 1.
 */
void userland_init(const struct boot_info *boot)
{
    int64_t pid;

    console_printf("[reliefnt] userland storage load started modules=%u\n",
                   boot ? boot->module_count : 0);
    boot_cmdline = boot ? boot->cmdline : 0;

    if (!storage_ready()) {
        console_printf("[reliefnt] no block-backed root filesystem available for userland\n");
        kernel_idle_loop();
    }

    int runtime_ret = userland_prepare_runtime();
    if (runtime_ret < 0) {
        console_printf("[reliefnt] runtime directory initialization failed ret=%d\n", runtime_ret);
        kernel_idle_loop();
    }

    const char *env[] = {"PATH=" RELIEFOS_DEFAULT_PATH, "HOME=/root", "PWD=/",
                        "TERM=xterm-256color", NULL};
    char init_path[256] = "/sbin/init";
    if (boot) {
        const char *arg = boot->cmdline;
        while (*arg) {
            while (*arg == ' ') ++arg;
            const char *end = arg;
            while (*end && *end != ' ') ++end;
            if (end - arg > 5 && __builtin_memcmp(arg, "init=", 5) == 0) {
                size_t length = (size_t)(end - arg - 5);
                if (arg[5] != '/' || length >= sizeof(init_path)) {
                    console_printf("[reliefnt] invalid init= override\n");
                    kernel_idle_loop();
                }
                __builtin_memcpy(init_path, arg + 5, length);
                init_path[length] = 0;
            }
            arg = end;
        }
    }
    const char *argv[] = {init_path, NULL};
    struct exec_launch launch = {0};
    if (build_exec_launch(&launch, init_path, argv, env) < 0) {
        console_printf("[reliefnt] cannot prepare PID 1 arguments\n");
        kernel_idle_loop();
    }
    pid = spawn_path_internal_deferred(init_path, "init", &launch, 0, 0, 0, -1, -1, -1);
    if (pid != 1) {
        console_printf("[reliefnt] cannot execute PID 1 path=%s ret=%lld; boot stopped\n",
                       init_path, (long long)pid);
        kernel_idle_loop();
    }
    init_pid = (uint32_t)pid;
    console_printf("[reliefnt] PID 1 path=%s console=tty1\n", init_path);
    sched_mark_ready(init_pid);
}

/**
 * @brief Enter the first runnable user task; falls to the idle loop if userland failed to load.
 */
void userland_enter_first(void)
{
    struct task *first;
    if (!init_pid && !desktop_pid && !windowd_pid && !tty_pid) {
        console_printf("[reliefnt] no Ring-3 userland loaded\n");
        kernel_idle_loop();
    }
    /* Load and reserve the first task on the BSP before releasing APs. This
     * avoids concurrent lazy ELF mapping and gives every AP a stable initial
     * task table/address space to observe. */
    first = userland_schedule_from_frame(NULL);
    userland_enter_task(first);
}

/**
 * @brief Terminate the current user task and let the scheduler own the single exit log event.
 * @param code Process exit status passed to the scheduler.
 */
void userland_process_exit(uint64_t code)
{
    sched_exit(sched_current_pid(), code);
}

/**
 * @brief Prepare a replacement image and stack before committing exec.
 * @param path Canonical name for the executable object.
 * @param held Resolved object held stable under the execution transaction.
 * @param execfn Original exec filename for AT_EXECFN, including fd-relative names.
 * @param argc Number of entries in argv.
 * @param argv Kernel-owned argv pointers into data.
 * @param envc Number of entries in envp.
 * @param envp Kernel-owned envp pointers into data.
 * @param data Packed argument/environment storage copied from user memory.
 * @param data_len Number of valid data bytes.
 * @return Zero after committing the new image, or a negative errno-style value with no change.
 */
int userland_exec_current_node(const char *path, const struct storage_node *held, const char *execfn,
                               uint32_t argc, char *const argv[],
                               uint32_t envc, char *const envp[],
                               const char *data, uint32_t data_len)
{
    struct task *task = sched_current_task();
    struct storage_node node;
    struct task *prepared = NULL;
    char task_name[SCHED_TASK_NAME_LEN];
    uint32_t preserved_flags;
    int ret;
    if (!task || task->kind != TASK_KIND_USER || !path || !path[0]) {
        return -22;
    }
    if (task->nproc_exceeded) {
        if (sched_user_task_count(task->uid) > sched_task_limits(task)->nproc.rlim_cur) return -11;
        task->nproc_exceeded = false;
    }
    if (held) { node = *held; ret = storage_inode_refresh(&node); }
    else ret = storage_lookup_path(path, &node);
    if (ret < 0 || node.type != RELIEFOS_FS_TYPE_FILE) {
        return ret < 0 ? ret : -13;
    }
    ret = fs_permissions_check_node(task, path, &node, FS_ACCESS_EXEC, false);
    if (ret < 0) return ret;
    if (argc > SCHED_EXEC_ARG_MAX || envc > SCHED_EXEC_ENV_MAX || data_len > SCHED_EXEC_DATA_MAX)
        return -7;
    task_name_from_path(path, task_name, sizeof(task_name));
    prepared = kernel_malloc(sizeof(*prepared));
    if (!prepared) return -12;
    __builtin_memset(prepared, 0, sizeof(*prepared));
    prepared->credentials = task->credentials;
    copy_text(prepared->cwd, sizeof(prepared->cwd), sched_task_cwd(task));
    copy_text(prepared->root_dir, sizeof(prepared->root_dir), sched_task_root(task));
    prepared->limits = *sched_task_limits(task);
    prepared->stack_top = USER_STACK_TOP;
    prepared->stack_low = USER_STACK_TOP - (uint64_t)RELIEFNT_USER_STACK_PAGES * 4096ULL;
    prepared->address_space.initial_stack_top = prepared->stack_top;
    prepared->address_space.initial_stack_low = prepared->stack_low;
    prepared->image_node = node;
    prepared->name = task_name;
    prepared->flags = TASK_FLAG_PENDING_LOAD;
    prepared->address_space.nondumpable = fs_permissions_check_node(task, path, &node, FS_ACCESS_READ, false) < 0;
    copy_text(prepared->path, sizeof(prepared->path), path);
    sched_copy_task_exec_params(prepared, argc, argv, envc, envp, data, data_len);
    ret = -12;
    if (!task_name[0] || !address_space_create(sched_task_as(prepared)) ||
        !address_space_map_user_stack(sched_task_as(prepared), USER_STACK_TOP)) goto failed;

    uint64_t loader_flags;
    userland_loader_lock(&loader_flags);
    ret = userland_load_task_image_locked(prepared, task, execfn);
    userland_loader_unlock(loader_flags);
    if (ret < 0) goto failed;

    ret = sched_prepare_exec_current(task);
    if (ret < 0) goto failed;
    /* The ELF mappings and initial stack are complete. Only now retire the
     * old mm, close CLOEXEC descriptors and reset process execution state. */
    svga_gpu_release_owner(task->pid);
    sched_exec_replace_mm(task, sched_task_as(prepared));
    task->address_space = prepared->address_space;
    task->credentials = prepared->credentials;
    task->entry = prepared->entry;
    task->stack_top = prepared->stack_top;
    task->stack_low = prepared->stack_low;
    sched_task_mm(task)->program_break_base = sched_task_mm(prepared)->program_break_base;
    sched_task_mm(task)->program_break = sched_task_mm(prepared)->program_break;
    task->loader_state = prepared->loader_state;
    sched_copy_task_exec_params(task, argc, argv, envc, envp, data, data_len);
    /* POSIX execve preserves ignored signals but resets caught dispositions. */
    kernel_signal_reset_handlers(task);
    task->frame = prepared->frame;
    /**
 * @brief exec replaces the image and its authority. A child of the desktop is never allowed to retain window-server/service privileges across exec.
 */
    preserved_flags = task->flags & (TASK_FLAG_ELEVATED_ADMIN | TASK_FLAG_WAITABLE_CHILD);
    /* Authority is tied to a root-owned, non-writable image, root
     * credentials and the image's reserved role gid, never to ancestry,
     * argv, or a user-selected path (see RELIEFOS_GID_* above). */
    struct reliefos_permissions permissions;
    if (!task->uid && !task->euid &&
        storage_inode_permissions(&node, &permissions, false) == 0 &&
        permissions.uid == 0 && !(permissions.mode & 0022)) {
        if (permissions.gid == RELIEFOS_GID_WINDOW_SERVER) {
            preserved_flags |= TASK_FLAG_SERVICE | TASK_FLAG_WINDOW_SERVER;
            desktop_pid = task->pid;
        } else if (permissions.gid == RELIEFOS_GID_SERVICE) {
            preserved_flags |= TASK_FLAG_SERVICE;
            windowd_pid = task->pid;
        }
    }
    task->flags = preserved_flags | TASK_FLAG_STARTED;
    copy_text(task->name_storage, sizeof(task->name_storage), task_name);
    task->name = task->name_storage;
    syscall_close_cloexec_files(task);

    arch_fpu_task_init(task->fpu_state);
    arch_fpu_restore(task->fpu_state);
    kernel_free(prepared);
    console_printf("[reliefnt] exec pid=%u path=%s pty=%u committed cr3=0x%llx\n",
                   task->pid, path, task->pty_id,
                   (unsigned long long)(*sched_task_as(task)).cr3);
    return 0;
failed:
    address_space_destroy(sched_task_as(prepared));
    sched_task_vma_release(prepared);
    kernel_free(prepared);
    return ret;
}

/**
 * @brief Build the exec vector and spawn path as a child of the current task; returns the child PID or a negative errno.
 */
int64_t userland_spawn_path_argv(const char *path,
                                 const char *const argv[],
                                 const char *const envp[],
                                 uint32_t pty_id)
{
    char task_name[SCHED_TASK_NAME_LEN];
    struct exec_launch launch;
    uint32_t parent = sched_current_pid();
    int64_t pid;
    int ret;

    if (!path || !path[0]) {
        return -22;
    }
    task_name_from_path(path, task_name, sizeof(task_name));
    if (!task_name[0]) {
        return -22;
    }

    ret = build_exec_launch(&launch, path, argv, envp);
    if (ret < 0) {
        return ret;
    }

    pid = spawn_path_internal(path, task_name, &launch, parent, 0, pty_id, -1, -1, -1);
    if (pid > 0) {
        console_printf("[reliefnt] spawn path=%s pid=%u parent=%u\n",
                       path,
                       (unsigned)pid,
                       parent);
    }
    return pid;
}

/**
 * @brief Spawns a user executable with explicitly inherited standard streams.
 * @param path NUL-terminated executable path in ReliefOS Unix syntax.
 * @param argv Optional NUL-terminated argument vector copied into the child.
 * @param envp Optional NUL-terminated environment vector copied into the child.
 * @param pty_id Active PTY inherited by the child; it must belong to the caller.
 * @param stdin_fd Caller file descriptor used as the child's standard input.
 * @param stdout_fd Caller file descriptor used as the child's standard output.
 * @param stderr_fd Caller file descriptor used as the child's standard error.
 * @return Positive child PID on success, or a negative errno value on failure.
 */
int64_t userland_spawn_path_argv_with_fds(const char *path,
                                          const char *const argv[],
                                          const char *const envp[],
                                          uint32_t pty_id,
                                          int stdin_fd, int stdout_fd,
                                          int stderr_fd)
{
    char task_name[SCHED_TASK_NAME_LEN];
    struct exec_launch launch;
    uint32_t parent = sched_current_pid();
    int64_t pid;
    int ret;
    if (!path || !path[0]) return -22;
    task_name_from_path(path, task_name, sizeof(task_name));
    ret = build_exec_launch(&launch, path, argv, envp);
    if (ret < 0) return ret;
    pid = spawn_path_internal(path, task_name, &launch, parent, 0, pty_id,
                              stdin_fd, stdout_fd, stderr_fd);
    if (pid > 0) {
        console_printf("[reliefnt] spawn path=%s pid=%u parent=%u fds=%d,%d,%d\n",
                       path, (unsigned)pid, parent, stdin_fd, stdout_fd, stderr_fd);
    }
    return pid;
}

/**
 * @brief Spawn path as a child of parent_pid and tag it with the given user identity/session; returns the PID or a negative errno.
 */
int64_t userland_spawn_path_argv_for_user(const char *path,
                                          const char *const argv[],
                                          const char *const envp[],
                                          uint32_t parent_pid,
                                          const struct reliefos_user_info *user,
                                          uint32_t session_id)
{
    char task_name[SCHED_TASK_NAME_LEN];
    struct exec_launch launch;
    int64_t pid;
    int ret;

    if (!path || !path[0] || !user || !user->uid || !session_id) {
        return -22;
    }
    task_name_from_path(path, task_name, sizeof(task_name));
    if (!task_name[0]) {
        return -22;
    }
    ret = build_exec_launch(&launch, path, argv, envp);
    if (ret < 0) {
        return ret;
    }
    pid = spawn_path_internal(path, task_name, &launch, parent_pid, 0, 0, -1, -1, -1);
    if (pid > 0) {
        sched_set_task_identity((uint32_t)pid, user, session_id);
        console_printf("[reliefnt] spawn trusted path=%s pid=%u parent=%u user=%u\n",
                       path, (unsigned)pid, parent_pid, user->uid);
    }
    return pid;
}

/**
 * @brief Spawn path attached to pty_id with no argv/envp.
 */
int64_t userland_spawn_path_with_pty(const char *path, uint32_t pty_id)
{
    return userland_spawn_path_argv(path, 0, 0, pty_id);
}

/**
 * @brief Spawn path with no argv/envp or PTY.
 */
int64_t userland_spawn_path(const char *path)
{
    return userland_spawn_path_with_pty(path, 0);
}

/**
 * @brief List directory entries into entries (special-casing /dev) and set *out_count; returns count or a negative errno.
 */
int userland_list_dir(const char *path, struct reliefos_dir_entry *entries,
                      uint32_t capacity, uint32_t *out_count)
{
    uint32_t count = 0;
    int ret;

    if (!path || !out_count) {
        return -22;
    }
    if (capacity > RELIEFOS_FS_MAX_ENTRIES) {
        capacity = RELIEFOS_FS_MAX_ENTRIES;
    }

    ret = storage_list_dir(path, entries, capacity, &count);
    *out_count = count;
    return ret;
}
