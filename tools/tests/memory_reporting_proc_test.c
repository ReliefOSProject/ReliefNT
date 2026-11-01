/* Real procfs and VFS pseudo provider; physical totals and task fixtures are
 * controlled by the inherited proc test. Swap and policy producers get their
 * own source-linked controls. */
#define main inherited_proc_fixture_main
#define MEMORY_REPORTING_REAL_PRODUCERS 1
#include "procfs_directories_test.c"
#undef main

/** @brief Read a complete small proc file using the actual provider. */
static void read_text(const char *name, char *buffer, size_t capacity)
{
    uint32_t got = 0; assert(capacity && capacity <= UINT32_MAX);
    int rc = proc_read(name, 0, buffer, (uint32_t)capacity - 1, &got);
    if (rc) fprintf(stderr, "%s read returned %d\n", name, rc);
    assert(!rc && got < capacity); buffer[got] = 0;
}

/** @brief An empty swap table still exposes the standard usable header. */
static void empty_swaps(void)
{
    struct storage_node node; char output[256];
    assert(!proc_lookup("/proc/swaps", &node) && node.type == RELIEFOS_FS_TYPE_FILE);
    read_text("/proc/swaps", output, sizeof(output));
    assert(strstr(output, "Filename") && strstr(output, "Type") && strstr(output, "Size") &&
           strstr(output, "Used") && strstr(output, "Priority") && strchr(output, '\n'));
    assert(!strchr(strchr(output, '\n') + 1, '\n'));
    puts("PASS empty /proc/swaps exposes the standard table header");
}

/** @brief Report actual RAM/swap fields without treating swap as available RAM. */
static void memory_fields(void)
{
    char output[1024]; read_text("/proc/meminfo", output, sizeof(output));
    assert(strstr(output, "MemTotal: 1024 kB\n") && strstr(output, "MemFree: 512 kB\n"));
    assert(strstr(output, "MemAvailable:") && strstr(output, "Cached:") &&
           strstr(output, "Dirty:") && strstr(output, "SwapCached:") &&
           strstr(output, "Shmem: 4 kB\n") && strstr(output, "SwapTotal: 0 kB\n") &&
           strstr(output, "SwapFree: 0 kB\n"));
    puts("PASS /proc/meminfo emits RAM, cache, dirty and swap domains");
}

/** @brief Expose actual VM event families using standard field names. */
static void vm_fields(void)
{
    struct storage_node node; char output[1024];
    assert(!proc_lookup("/proc/vmstat", &node) && node.type == RELIEFOS_FS_TYPE_FILE);
    read_text("/proc/vmstat", output, sizeof(output));
    assert(strstr(output, "nr_free_pages 128\n") && strstr(output, "nr_shmem 1\n") &&
           strstr(output, "pgalloc_normal 10\n") && strstr(output, "pgfree 5\n") &&
           strstr(output, "pgfault ") && strstr(output, "pgmajfault ") &&
           strstr(output, "pswpin ") && strstr(output, "pswpout ") &&
           strstr(output, "pgscan_direct ") && strstr(output, "pgsteal_direct ") &&
           strstr(output, "oom_kill "));
    puts("PASS /proc/vmstat emits standard event names");
}

/** @brief Both per-process policy files must be discoverable. */
static void oom_fields(void)
{
    current.kind=TASK_KIND_USER; current.euid=current.fsuid=1000;
    fixture_total_kib=4096; /* Policy bias has page granularity in the producer. */
    current.gid=current.egid=current.fsgid=1000;
    foreign.kind=TASK_KIND_USER; foreign.euid=foreign.fsuid=2000;
    foreign.gid=foreign.egid=foreign.fsgid=2000; expose_foreign=true;
    sched_task_as(&current)->cr3=4096;
    struct storage_node node;
    assert(!proc_lookup("/proc/42/oom_score", &node) && node.type == RELIEFOS_FS_TYPE_FILE);
    assert(!proc_lookup("/proc/42/oom_score_adj", &node) && node.type == RELIEFOS_FS_TYPE_FILE);
    static const struct vfs_backend_ops ops={.lookup=proc_path_lookup,.retain=proc_path_retain,
        .release=proc_path_release,.same_node=proc_path_same};
    struct storage_node root_node={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=1};
    struct vfs_namespace ns={0}; struct vfs_mount *base=NULL,*proc=NULL;
    assert(!vfs_mount_new(&ops,NULL,NULL,&root_node,&base));
    assert(!vfs_namespace_init(&ns,base)); vfs_mount_put(base);
    struct vfs_path root={0},point={0},score={0},adj={0},foreign_adj={0},replacement={0};
    assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"procdir",0,&point));
    assert(!proc_create_vfs_mount(&proc));
    assert(!vfs_mount_attach(&ns,proc,&point)); vfs_mount_put(proc);
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/42/oom_score",0,&score));
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/42/oom_score_adj",0,&adj));
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/43/oom_score_adj",0,&foreign_adj));
    struct reliefos_permissions mode;
    assert(!vfs_path_permissions(&adj,&mode,false) && mode.mode==0644 && mode.uid==1000 && mode.gid==1000);
    assert(!vfs_path_permissions(&score,&mode,false) && mode.mode==0444);
    assert(!fs_permissions_check_path(&current,&adj,FS_ACCESS_WRITE,false));
    current.cap_effective=1ULL<<CAP_SYS_RESOURCE;
    assert(fs_permissions_check_path(&current,&foreign_adj,FS_ACCESS_WRITE,false)==-EACCES);
    current.cap_effective=0;
    char data[32]; uint32_t got=0,written=0;
    char original[32]; uint32_t original_len=0;
    assert(!vfs_path_read(&score,0,original,sizeof(original),&original_len) && original_len>=2);
    struct vfs_open_file *score_view=NULL;
    assert(!vfs_file_open(&score,LINUX_O_RDONLY,&score_view) && score_view);
    assert(!vfs_file_read(score_view,0,data,1,&got) && got==1 && data[0]==original[0]);
    assert(!vfs_path_write(&adj,37,"0x1f4\n",6,&written) && written==6);
    assert(!vfs_file_read(score_view,1,data,sizeof(data),&got) && got==original_len-1 &&
           !memcmp(data,original+1,got));
    assert(!vfs_path_read(&score,0,data,sizeof(data),&got) &&
           (got!=original_len || memcmp(data,original,got)));
    assert(!vfs_path_read(&adj,1,data,2,&got) && got==2 && !memcmp(data,"00",2));
    assert(!vfs_path_write(&adj,3,"0376\n",5,&written) && written==5);
    assert(vfs_path_write(&adj,0,"-1\n",3,&written)==-EACCES && !written);
    current.cap_effective=1ULL<<CAP_SYS_RESOURCE;
    assert(!vfs_path_write(&adj,0,"-0500\n",6,&written) && written==6);
    current.cap_effective=0;
    assert(!vfs_path_write(&adj,0,"-0x100\n",7,&written));
    assert(vfs_path_write(&adj,0,"-321\n",5,&written)==-EACCES && !written);
    assert(vfs_path_write(&adj,0,"1001\n",5,&written)==-EINVAL && !written);
    assert(vfs_path_write(&adj,0,"08\n",3,&written)==-EINVAL && !written);
    assert(!vfs_path_write(&adj,0,"-3\n",3,&written));
    assert(!vfs_path_read(&adj,0,data,sizeof(data),&got) && got==3 && !memcmp(data,"-3\n",3));
    assert(!vfs_path_truncate(&adj,0) && vfs_path_truncate(&adj,1)==-EINVAL);
    assert(!vfs_path_sync_after_write(&adj,true) && vfs_path_sync(&adj,false,true)==-EINVAL);
    sched_task_identity_detach(&current);
    assert(!vfs_file_read(score_view,0,data,sizeof(data),&got) && got==original_len &&
           !memcmp(data,original,got));
    vfs_file_close(score_view);
    assert(vfs_path_read(&adj,0,data,sizeof(data),&got)==-ENOENT && !got);
    assert(vfs_path_write(&adj,0,"0\n",2,&written)==-ENOENT && !written);
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/42/oom_score_adj",0,&replacement));
    assert(!vfs_path_equal(&adj,&replacement));
    vfs_path_put(&replacement); vfs_path_put(&foreign_adj); vfs_path_put(&adj); vfs_path_put(&score);
    vfs_path_put(&point); vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns) && !proc_path_pins);
    sched_task_identity_detach(&current); sched_task_identity_detach(&foreign);
    puts("PASS OOM score/adjustment DAC, capabilities, offsets, parsing and PID reuse");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "proc-empty-swaps")) empty_swaps();
    else if (!strcmp(argv[1], "proc-meminfo")) memory_fields();
    else if (!strcmp(argv[1], "proc-vmstat")) vm_fields();
    else if (!strcmp(argv[1], "proc-oom")) oom_fields();
    else abort();
    return 0;
}
