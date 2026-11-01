/* SPDX-License-Identifier: Apache-2.0 */
#define main proc_directory_fixture_main
#include "procfs_directories_test.c"
#undef main
#include "../../kernel/syscall/internal.h"

static char output[32768], sliced[32768];

static size_t read_all(const char *path)
{
    uint32_t n = 0;
    assert(!proc_read(path, 0, output, sizeof(output) - 1, &n));
    output[n] = 0;
    return n;
}

static void thread_paths(void)
{
    extra_tasks[0] = (struct task){.pid = 100, .tgid = 42, .name = "worker", .cpu_ticks = 77};
    extra_tasks[1] = (struct task){.pid = 200, .name = "outsider"};
    visible_tasks = 3;
    struct storage_node node;
    assert(!proc_lookup("/proc/42/task/100/status", &node));
    assert(!proc_lookup("/proc/100/status", &node));
    assert(proc_lookup("/proc/42/task/200/status", &node) == -ENOENT);
    assert(proc_lookup("/proc/42/task/100/task", &node) == -ENOENT);
    assert(proc_lookup("/proc/42/task/100/task/100/status", &node) == -ENOENT);
    assert(proc_lookup("/proc/100/task", &node) == -ENOENT);
    assert(proc_lookup("/proc/thread-self/task", &node) == -ENOENT);
    assert(proc_lookup("/proc/42/task/0100/status", &node) == -ENOENT);
    assert(proc_lookup("/proc/42/task/4294967296/status", &node) == -ENOENT);
    uint64_t offset = 0;
    struct reliefos_dir_entry entry;
    assert(proc_readdir("/proc/42/task", &offset, &entry) == 1 && !strcmp(entry.name, "42"));
    assert(proc_readdir("/proc/42/task", &offset, &entry) == 1 && !strcmp(entry.name, "100"));
    assert(proc_readdir("/proc/42/task", &offset, &entry) == 0);
    read_all("/proc/42/task/100/stat");
    assert(strstr(output, "100 (worker) R "));
    char target[64] = {0};
    assert(proc_readlink("/proc/thread-self", target, sizeof(target)) == 10);
    assert(!memcmp(target, "42/task/42", 10));
    visible_tasks = 1;
}

static void io_views(void)
{
    current.proc_read_chars = 10;
    current.proc_read_calls = 2;
    extra_tasks[0] =
        (struct task){.pid = 100, .tgid = 42, .proc_read_chars = 7, .proc_read_calls = 1};
    extra_tasks[1] = (struct task){.pid = 200, .proc_read_chars = 999};
    visible_tasks = 3;
    read_all("/proc/self/io");
    assert(strstr(output, "rchar: 17\n") && strstr(output, "syscr: 3\n"));
    read_all("/proc/42/task/42/io");
    assert(strstr(output, "rchar: 10\n"));
    read_all("/proc/thread-self/io");
    assert(strstr(output, "rchar: 10\n"));
    read_all("/proc/100/io");
    assert(strstr(output, "rchar: 7\n") && strstr(output, "syscr: 1\n"));
    visible_tasks = 1;
    current.proc_read_chars = current.proc_read_calls = 0;
}

static void memory_views(void)
{
    struct task_address_space_state *mm = sched_task_mm(&current);
    mm->as.user_page_count = 3;
    current.stack_low = 0x409000;
    current.stack_top = 0x40a000;
    mm->vmas[0] = (struct task_vma){.used = 1,
                                    .start = 0x409000,
                                    .end = 0x40a000,
                                    .prot = 3,
                                    .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    mm->vmas[1] = (struct task_vma){.used = 1,
                                    .start = 0x400000,
                                    .end = 0x402000,
                                    .prot = 5,
                                    .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_PRIVATE};
    mm->vmas[2] = (struct task_vma){.used = 1,
                                    .start = 0x402000,
                                    .end = 0x404000,
                                    .prot = 3,
                                    .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    mm->vmas[1].file_offset = 0x123456789ULL;
    extra_tasks[0] = (struct task){.pid = 100, .tgid = 42, .name = "same-mm", .shared_mm = mm};
    extra_tasks[1] = (struct task){.pid = 200, .name = "fork"};
    sched_task_as(&extra_tasks[1])->user_page_count = 1;
    visible_tasks = 3;
    fixture_ptes[0] = (struct fixture_pte){&mm->as, 0x400000, 0x1000, 0x20};
    fixture_ptes[1] = (struct fixture_pte){&mm->as, 0x402000, 0x2000, 0x60};
    fixture_ptes[2] = (struct fixture_pte){&mm->as, 0x409000, 0x3000, 0x60};
    fixture_ptes[3] = (struct fixture_pte){sched_task_as(&extra_tasks[1]), 0x402000, 0x2000, 0x60};
    fixture_pte_count = 4;
    fixture_cached_phys = 0x1000;
    read_all("/proc/self/maps");
    const char *text = strstr(output, "0000000000400000-0000000000402000 r-xp");
    const char *data = strstr(output, "0000000000402000-0000000000404000 rw-p");
    const char *stack = strstr(output, "0000000000409000-000000000040a000 rw-p");
    assert(text && data > text && stack > data && strstr(stack, "[stack]"));
    assert(strstr(text, "123456789 00:00 0"));
    read_all("/proc/self/statm");
    assert(!strcmp(output, "5 3 2 2 0 3 0\n"));
    read_all("/proc/self/smaps_rollup");
    assert(strstr(output, "Rss: 12 kB\n") && strstr(output, "Pss: 10 kB\n"));
    assert(strstr(output, "Shared_Dirty: 4 kB\n") && strstr(output, "Private_Dirty: 4 kB\n"));
    assert(strstr(output, "Anonymous: 8 kB\n") && strstr(output, "Referenced: 12 kB\n"));
    read_all("/proc/self/status");
    assert(strstr(output, "VmData:\t8 kB\n") && strstr(output, "VmStk:\t4 kB\n"));
    assert(strstr(output, "Threads:\t2\n"));
    mm->as.user_page_count = 0;
    sched_task_as(&extra_tasks[1])->user_page_count = 0;
    fixture_pte_count = 0;
    visible_tasks = 1;
    fixture_cached_phys = 0;
    memset(mm->vmas, 0, sizeof(mm->vmas));
    current.stack_low = current.stack_top = 0;
}

static void inconsistent_page_counts(void)
{
    /* More unique PTEs than the size metadata predicts must fail boundedly. */
    struct address_space *as = sched_task_as(&current);
    as->user_page_count = 0;
    fixture_pte_count = 17;
    for (unsigned i = 0; i < 17; ++i)
        fixture_ptes[i] = (struct fixture_pte){as, 0x400000 + i * 4096, (i + 1) * 4096, 0};
    unsigned before = proc_allocations;
    uint32_t n = 99;
    assert(proc_read("/proc/self/smaps", 0, output, sizeof(output), &n) == -ENOMEM && !n);
    assert(proc_allocations == before);
    fixture_pte_count = 0;
}

static void access_and_binary(void)
{
    const char *leaves[] = {"maps", "smaps", "smaps_rollup", "auxv", "io", "fdinfo/3"};
    expose_foreign = true;
    foreign.uid = foreign.euid = foreign.suid = 2000;
    foreign.gid = foreign.egid = foreign.sgid = 2000;
    foreign.files[0] = (struct task_file){.used = 1};
    for (unsigned i = 0; i < sizeof(leaves) / sizeof(*leaves); ++i) {
        char name[80];
        snprintf(name, sizeof(name), "/proc/43/%s", leaves[i]);
        uint32_t n = 99;
        assert(proc_read(name, 0, output, sizeof(output), &n) == -EACCES && !n);
        void *view = NULL;
        assert(proc_file_open(name, 0, &view) == -EACCES && !view);
    }
    current.cap_effective = 1ULL << CAP_SYS_PTRACE;
    assert(read_all("/proc/43/maps") == 0);
    current.cap_effective = 0;
    foreign.uid = foreign.euid = foreign.suid = current.fsuid;
    foreign.gid = foreign.egid = foreign.sgid = current.fsgid;
    sched_task_mm(&foreign)->nondumpable = true;
    uint32_t n = 99;
    assert(proc_read("/proc/43/auxv", 0, output, sizeof(output), &n) == -EACCES && !n);
    sched_task_mm(&foreign)->nondumpable = false;
    expose_foreign = false;
    struct task_address_space_state *mm = sched_task_mm(&current);
    uint64_t auxv[] = {6, 4096, 11, 1000, 0, 0};
    memcpy(mm->saved_auxv, auxv, sizeof(auxv));
    mm->saved_auxv_bytes = sizeof(auxv);
    assert(read_all("/proc/self/auxv") == sizeof(auxv) && !memcmp(output, auxv, sizeof(auxv)));
    for (unsigned i = 0; i < sizeof(auxv); ++i) {
        assert(!proc_read("/proc/self/auxv", i, sliced + i, 1, &n) && n == 1);
    }
    assert(!memcmp(auxv, sliced, sizeof(auxv)));
    assert(!proc_read("/proc/self/auxv", sizeof(auxv), sliced, 1, &n) && !n);
}

static void fd_details(void)
{
    struct task_file *f = &current.files[0];
    *f = (struct task_file){.used = 1,
                            .flags = LINUX_O_RDWR | LINUX_O_APPEND | TASK_FILE_FLAG_EVENTFD,
                            .fd_flags = 1,
                            .offset = 123,
                            .aux = 7,
                            .aux2 = 1};
    read_all("/proc/self/fdinfo/3");
    assert(strstr(output, "pos:\t123\n") && strstr(output, "flags:\t02002002\n"));
    assert(strstr(output, "eventfd-count: 0000000000000007\n") &&
           strstr(output, "eventfd-semaphore: 1\n"));
    struct task_file *alias = &current.files[1];
    *alias = (struct task_file){.used = 1, .description = f};
    read_all("/proc/self/fdinfo/4");
    assert(strstr(output, "pos:\t123\n") && strstr(output, "flags:\t02002\n"));
    struct task_timerfd timer = {
        .clockid = 1, .expiry_tick = 120, .interval_ticks = 10, .expirations = 2};
    f->flags = TASK_FILE_FLAG_TIMERFD;
    f->aux = (uintptr_t)&timer;
    read_all("/proc/self/fdinfo/3");
    assert(strstr(output, "ticks: 3\n") && strstr(output, "it_value: (0, 70000000)\n"));
    assert(timer.expirations == 2 && timer.expiry_tick == 120);
    f->socket_ino = (1ULL << 60) | 123;
    f->flags = TASK_FILE_FLAG_SOCKET;
    f->aux = 123;
    read_all("/proc/self/fdinfo/3");
    assert(strstr(output, "ino:\t1152921504606847099\n"));
    char link[64] = {0};
    int bytes = proc_readlink("/proc/self/fd/3", link, sizeof(link));
    assert(bytes == 28 && !memcmp(link, "socket:[1152921504606847099]", 28));
    *f = (struct task_file){0};
    *alias = (struct task_file){0};
    uint64_t offset = 0;
    struct reliefos_dir_entry entry;
    assert(!proc_readdir("/proc/self/fdinfo", &offset, &entry));
}

static void long_snapshots(void)
{
    struct task_address_space_state *mm = sched_task_mm(&current);
    for (unsigned i = 0; i < 100; ++i)
        mm->vmas[i] = (struct task_vma){.used = 1,
                                        .start = 0x100000ULL + i * 8192,
                                        .end = 0x101000ULL + i * 8192,
                                        .prot = 3,
                                        .flags = TASK_VMA_FLAG_ANON};
    size_t expected = read_all("/proc/self/smaps");
    assert(expected > 4096);
    void *view = NULL;
    assert(!proc_file_open("/proc/self/smaps", 0, &view) && view);
    memset(mm->vmas, 0, sizeof(mm->vmas));
    size_t total = 0;
    uint32_t n;
    do {
        assert(!proc_file_read(view, total, sliced + total, 7, &n));
        total += n;
    } while (n);
    assert(total == expected && !memcmp(sliced, output, total));
    assert(!proc_file_read(view, 0, sliced, 31, &n) && n == 31 && !memcmp(sliced, output, n));
    assert(!proc_file_read(view, UINT64_MAX, sliced, 1, &n) && !n);
    proc_file_close(view);
    unsigned before = proc_allocations;
    for (int budget = 0; budget < 4; ++budget) {
        view = NULL;
        proc_allocation_budget = budget;
        int rc = proc_file_open("/proc/self/status", 0, &view);
        proc_allocation_budget = -1;
        if (rc)
            assert(rc == -ENOMEM && !view);
        else
            proc_file_close(view);
        assert(proc_allocations == before);
    }
    /* Exercise the real mounted provider's open callback and independent view. */
    struct vfs_mount *mount = NULL;
    assert(!proc_create_vfs_mount(&mount));
    struct vfs_namespace ns = {0};
    assert(!vfs_namespace_init(&ns, mount));
    vfs_mount_put(mount);
    struct vfs_path root = {0}, path = {0};
    assert(!vfs_namespace_root(&ns, &root));
    assert(!vfs_path_lookup(&ns, &root, &root, "42/status", 0, &path));
    struct vfs_open_file *file = NULL;
    assert(!vfs_file_open(&path, 0, &file));
    vfs_path_put(&path);
    assert(!vfs_file_read(file, 0, output, sizeof(output) - 1, &n) && n);
    output[n] = 0;
    assert(strstr(output, "Name:\ttest\n"));
    vfs_file_close(file);
    vfs_path_put(&root);
    vfs_namespace_destroy(&ns);
}

int main(void)
{
    thread_paths();
    io_views();
    memory_views();
    inconsistent_page_counts();
    access_and_binary();
    fd_details();
    long_snapshots();
    read_all("/proc/loadavg");
    assert(!strcmp(output, "1.00 0.50 1.00 1/1 42\n"));
    read_all("/proc/meminfo");
    assert(strstr(output, "Cached: 16 kB\n") && strstr(output, "Shmem: 4 kB\n"));
    read_all("/proc/stat");
    assert(strstr(output, "cpu 10 0 0 20 ") && strstr(output, "ctxt 456\n"));
    read_all("/proc/vmstat");
    assert(strstr(output, "pgalloc_normal 10\n") && strstr(output, "pgfault 2\n"));
    read_all("/proc/swaps");
    assert(!strncmp(output, "Filename", 8));
    read_all("/proc/sys/fs/file-nr");
    assert(!strcmp(output, "2\t0\t18446744073709551615\n"));
    read_all("/proc/self/limits");
    assert(strstr(output, "Max open files"));
    current.proc_read_chars = 123;
    current.proc_write_calls = 3;
    current.proc_write_bytes = 4096;
    read_all("/proc/self/io");
    assert(strstr(output, "rchar: 123\n") && strstr(output, "write_bytes: 4096\n"));
    sched_task_identity_detach(&current);
    assert(!proc_allocations);
    puts("PASS extended proc: thread membership, sparse maps/PSS, FSCREDS, binary auxv, OFD "
         "details, large snapshots, seek/EOF and OOM");
}
