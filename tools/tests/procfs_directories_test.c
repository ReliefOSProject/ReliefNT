#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../include/reliefnt/audio.h"
int sound_service_card_snapshot(uint32_t index, uint32_t *out_id,
                        struct audio_card_identity *identity)
{ (void)index; (void)out_id; (void)identity; return -2; }
#include "../../fs/proc/procfs.c"
int pty_inode_permissions(const struct storage_node *node,struct reliefos_permissions *value,bool write)
{(void)node;(void)value;(void)write;abort();}

static struct task_pty_fd fixture_ptys[2];
static struct task current = {.pid = 42, .name = "test", .uid = 1000,
    .pty_fds = fixture_ptys, .pty_fd_count = 2, .pty_fd_capacity = 2};
static struct task foreign = {.pid = 43, .name = "foreign", .uid = 2000};
static bool expose_foreign;
static struct task extra_tasks[100];
static uint32_t visible_tasks = 1;
struct task_pty_fd *task_pty_fd_for_fd(struct task *t, int fd)
{ for (unsigned i = 0; i < sched_task_pty_capacity(t); ++i)
    if (t->pty_fds[i].used && t->pty_fds[i].fd == fd) return &t->pty_fds[i];
  return NULL; }
uint32_t sched_task_pty_capacity(const struct task *t) { return t->pty_fd_count; }
struct task_pty_fd *sched_task_pty_at(struct task *t, uint32_t i)
{ return i < t->pty_fd_count ? &t->pty_fds[i] : NULL; }
uint32_t sched_task_file_capacity(const struct task *t) { (void)t; return SCHED_TASK_FILE_MAX; }
struct task_file *task_file_for_fd(struct task *t, int fd)
{ return fd>=3 && fd<3+SCHED_TASK_FILE_MAX && t->files[fd-3].used ?
    task_file_description(&t->files[fd-3]) : NULL; }
uint32_t pty_vt_number(uint32_t id) { return id >= 1 && id <= 6 ? id : 0; }
uint64_t address_space_user_resident_kib(const struct address_space *as)
{ assert(as == sched_task_as(&current)); return 12; }
struct task *sched_current_task(void) { return &current; }
uint32_t sched_current_pid(void) { return current.pid; }
struct task *sched_task_at(uint32_t index)
{ return index < visible_tasks ? (index ? &extra_tasks[index - 1] : &current) : NULL; }
struct task *sched_find(uint32_t pid)
{
    if (pid == current.pid) return &current;
    if (expose_foreign && pid == foreign.pid) return &foreign;
    for (uint32_t i = 1; i < visible_tasks; ++i)
        if (extra_tasks[i - 1].pid == pid) return &extra_tasks[i - 1];
    return NULL;
}
uint32_t sched_snapshot(struct task_snapshot_info *out, uint32_t capacity, uint64_t *tick)
{
    if (tick) *tick = 123;
    if (!out || !capacity) return visible_tasks;
    uint32_t count = visible_tasks < capacity ? visible_tasks : capacity;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = (struct task_snapshot_info){.pid = sched_task_at(i)->pid};
    return count;
}
uint64_t time_uptime_ms(void) { return 1234; }
uint64_t mm_total_memory_kib(void) { return 1024; }
uint64_t mm_free_memory_kib(void) { return 512; }
#ifndef TEST_REAL_INVENTORY
uint32_t smp_cpu_count(void) { return 1; }
#endif
void sched_cpu_ticks(uint64_t *busy, uint64_t *idle) { *busy = 10; *idle = 20; }
void sched_cpu_ticks_per_cpu(uint64_t *busy, uint64_t *idle, uint32_t capacity)
{ if (capacity) sched_cpu_ticks(busy, idle); }
void sched_cpu_runtime_snapshot(uint64_t *busy, uint64_t *idle,
    uint32_t *pids, uint32_t *ready, uint32_t capacity)
{ (void)pids; (void)ready;
  for (uint32_t cpu = 0; cpu < capacity; ++cpu) {
    if (busy) busy[cpu] = 10;
    if (idle) idle[cpu] = 20;
  } }
uint64_t sched_task_cpu_ticks(const struct task *task, bool process)
{ (void)process; return task->cpu_ticks; }
/* All release interfaces must use this record, including its suffix. */
static const struct reliefos_system_info fixture_system = {
    .kernel_name = "ReliefNT", .kernel_version = "9.8.7-0123",
    .build_time = "2026-09-11 01:02:03",
};
const struct reliefos_system_info *reliefnt_system_info(void) { return &fixture_system; }

const char *userland_boot_cmdline(void) { return ""; }
void linux_uts_names(char host[65], char domain[65])
{ strcpy(host, "fixture-host"); strcpy(domain, "(none)"); }
#ifndef TEST_REAL_INVENTORY
int cpu_inventory_read(uint64_t o,void *b,uint32_t n,uint32_t *r) { (void)o;(void)b;(void)n;*r=0;return 0; }
int sysfs_lookup(const char *p,struct storage_node *n) { (void)p;(void)n;return -2; }
int sysfs_read(const char *p,uint64_t o,void *b,uint32_t n,uint32_t *r) { (void)p;(void)o;(void)b;(void)n;*r=0;return -2; }
int sysfs_readlink(const char *p,char *b,uint32_t n) { (void)p;(void)b;(void)n;return -2; }
int sysfs_readdir(const char *p,uint64_t *o,struct reliefos_dir_entry *e) { (void)p;(void)o;(void)e;return -2; }
#endif
struct task_vma *sched_task_vma_at(struct task *t,uint32_t i) { return i<SCHED_TASK_VMA_MAX? &sched_task_mm(t)->vmas[i]:NULL; }
int pty_get_foreground_pgid(uint32_t p,uint32_t *g) { (void)p;*g=0;return -2; }
int driver_manager_list(struct reliefos_driver_list *query) { query->count = 0; return 0; }
static char argument_pages[8192];
bool address_space_user_page_readable(const struct address_space *a,uint64_t v) { (void)a;return v>=0x400000 && v<0x402000; }
uint64_t address_space_user_page_phys(const struct address_space *a,uint64_t v) { (void)a;return (v&~4095ULL)-0x400000+0x1000; }
void *paging_kernel_direct_map(uint64_t p) { return p>=0x1000 && p<0x3000?argument_pages+p-0x1000:NULL; }

static int proc_allocation_budget=-1;
static unsigned proc_allocations;
void *kernel_malloc(uint64_t size)
{
    if(!proc_allocation_budget)return NULL;
    if(proc_allocation_budget>0)--proc_allocation_budget;
    void *memory=malloc(size);if(memory)++proc_allocations;return memory;
}
void kernel_free(void *memory)
{if(memory){assert(proc_allocations);--proc_allocations;}free(memory);}
static unsigned proc_path_pins;
static char proc_long_directory[256],proc_long_file[256];
static int proc_path_lookup(void *context,const struct storage_node *parent,
                             const char *name,struct storage_node *out)
{
    (void)context;
    if(parent->first_cluster==1 && (!strcmp(name,"procdir") || !strcmp(name,"else"))) {
        *out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR,
            .first_cluster=!strcmp(name,"procdir") ? 2 : 3};return 0;
    }
    if((parent->first_cluster==1 && !strcmp(name,proc_long_directory)) ||
       (parent->first_cluster==4 && !strcmp(name,proc_long_file))) {
        *out=(struct storage_node){.type=parent->first_cluster==1 ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE,
            .first_cluster=parent->first_cluster==1 ? 4 : 5};return 0;
    }
    return -RELIEFOS_ENOENT;
}
static int proc_path_retain(void *context,const struct storage_node *node,void **pin)
{ (void)context;(void)node;++proc_path_pins;*pin=&proc_path_pins;return 0; }
static void proc_path_release(void *context,const struct storage_node *node,void *pin)
{ (void)context;(void)node;assert(pin==&proc_path_pins && proc_path_pins);--proc_path_pins; }
static bool proc_path_same(void *context,const struct storage_node *a,const struct storage_node *b)
{ (void)context;return a->first_cluster==b->first_cluster; }
static void test_proc_fd_object_link(void)
{
    static const struct vfs_backend_ops ops={.lookup=proc_path_lookup,.retain=proc_path_retain,
        .release=proc_path_release,.same_node=proc_path_same};
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=1};
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=17};
    struct vfs_mount *mount=NULL;assert(!vfs_mount_new(&ops,NULL,NULL,&root,&mount));
    struct task_file *file=&current.files[0];
    *file=(struct task_file){.used=1,.node=node};strcpy(file->path,"/memfd:display-only (deleted)");
    assert(!vfs_path_anonymous(mount,&node,&file->path_ref));vfs_mount_put(mount);
    memcpy(file->path,"/memfd:",7);memset(file->path+7,'z',249);
    memcpy(file->path+256," (deleted)",11);
    char diagnostic[268];memset(diagnostic,'#',sizeof(diagnostic));
    assert(proc_readlink("/proc/self/fd/3",diagnostic,sizeof(diagnostic))==266);
    assert(!memcmp(diagnostic,file->path,266) && diagnostic[266]=='#');
    struct vfs_path target={0};
    assert(!proc_fd_get_path(&current,3,&target));
    assert(vfs_path_equal(&file->path_ref,&target));
    char name[64];assert(vfs_path_backend_name(&target,name,sizeof(name))==-RELIEFOS_ENOENT);
    vfs_path_put(&file->path_ref);*file=(struct task_file){0};
    assert(vfs_path_node(&target)->first_cluster==17 && proc_path_pins==2);
    vfs_path_put(&target);assert(!proc_path_pins);
    assert(proc_fd_get_path(&current,3,&target)==-RELIEFOS_ENOENT && !target.mount);
    puts("PASS proc FD magic target owns the anonymous file object without replaying display names");
}
static void test_proc_mount_objects(void)
{
    static const struct vfs_backend_ops ops={.lookup=proc_path_lookup,.retain=proc_path_retain,
        .release=proc_path_release,.same_node=proc_path_same};
    struct storage_node node={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=1};
    struct vfs_namespace ns={0};struct vfs_mount *base=NULL,*proc=NULL;
    assert(!vfs_mount_new(&ops,NULL,NULL,&node,&base));
    assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    struct vfs_path root={0},point={0},destination={0},mounted={0},version={0};
    assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"procdir",0,&point));
    assert(!vfs_path_lookup(&ns,&root,&root,"else",0,&destination));
    assert(!proc_create_vfs_mount(&proc));
    assert(!vfs_mount_attach(&ns,proc,&point));vfs_mount_put(proc);
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir",0,&mounted));
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/sys/kernel/osrelease",VFS_LOOKUP_FOLLOW,&version));
    char bytes[64];uint32_t got=0;
    assert(!vfs_path_read(&version,0,bytes,sizeof(bytes),&got) && got==11 && !memcmp(bytes,"9.8.7-0123\n",11));
    struct reliefos_permissions mode;
    assert(!vfs_path_permissions(&version,&mode,false) && mode.mode==0444 && !mode.uid && !mode.gid);
    assert(vfs_path_permissions(&version,&mode,true)==-RELIEFOS_EROFS);
    struct task_file *file=&current.files[0];
    *file=(struct task_file){.used=1,.node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=17}};
    strcpy(file->path,"/memfd:held (deleted)");
    assert(!vfs_path_anonymous(root.mount,&file->node,&file->path_ref));
    vfs_path_get(&root,&current.fs_paths.root);vfs_path_get(&destination,&current.fs_paths.cwd);
    struct vfs_path mounts_view={0};char mount_text[512];
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/self/mountinfo",VFS_LOOKUP_FOLLOW,&mounts_view));
    assert(!vfs_path_read(&mounts_view,0,mount_text,sizeof(mount_text)-1,&got));mount_text[got]=0;
    assert(strstr(mount_text,"2 1 0:201 / /procdir ro - proc proc ro\n"));
    struct vfs_open_file *old_view=NULL,*new_view=NULL;
    unsigned open_baseline=proc_allocations;
    for(int budget=0;budget<3;++budget) {
        struct vfs_open_file *attempt=NULL;proc_allocation_budget=budget;
        int rc=vfs_file_open(&mounts_view,0,&attempt);proc_allocation_budget=-1;
        if(budget<2)assert(rc==-ENOMEM && !attempt);
        else {assert(!rc && attempt);vfs_file_close(attempt);}
        assert(proc_allocations==open_baseline);
    }
    assert(!vfs_file_open(&mounts_view,0,&old_view) && old_view);
    vfs_path_put(&current.fs_paths.root);vfs_path_get(&destination,&current.fs_paths.root);
    assert(!vfs_file_open(&mounts_view,0,&new_view) && new_view);
    assert(!vfs_file_read(old_view,0,mount_text,sizeof(mount_text)-1,&got));mount_text[got]=0;
    assert(strstr(mount_text,"/procdir ro - proc proc ro\n"));
    assert(!vfs_file_read(new_view,0,mount_text,sizeof(mount_text)-1,&got) && !got);
    vfs_path_put(&current.fs_paths.root);vfs_path_get(&root,&current.fs_paths.root);
    assert(!vfs_file_read(new_view,0,mount_text,sizeof(mount_text)-1,&got) && !got);
    vfs_path_get(&version,&sched_task_mm(&current)->executable_path);
    memset(proc_long_directory,'a',255);memset(proc_long_file,'b',255);
    char long_name[600],long_text[600];
    snprintf(long_name,sizeof(long_name),"/%s/%s",proc_long_directory,proc_long_file);
    struct vfs_path long_object={0};
    assert(!vfs_path_lookup(&ns,&root,&root,long_name,VFS_LOOKUP_FOLLOW,&long_object));
    struct task_file *long_fd=&current.files[1];*long_fd=(struct task_file){.used=1,.node=*vfs_path_node(&long_object)};
    strcpy(long_fd->path,"/deliberately-stale");vfs_path_get(&long_object,&long_fd->path_ref);
    assert(proc_readlink("/proc/self/fd/4",long_text,sizeof(long_text))==(int)strlen(long_name) &&
        !memcmp(long_text,long_name,strlen(long_name)));
    uint64_t fd_cursor=4;struct reliefos_dir_entry fd_entry;
    assert(proc_readdir("/proc/self/fd",&fd_cursor,&fd_entry)==1 && !strcmp(fd_entry.name,"4"));
    vfs_path_put(&long_object);vfs_path_put(&long_fd->path_ref);long_fd->path[0]=0;
    fd_cursor=4;
    assert(proc_readdir("/proc/self/fd",&fd_cursor,&fd_entry)==1 && !strcmp(fd_entry.name,"4"));
    *long_fd=(struct task_file){0};
    assert(proc_readlink("/proc/self/cwd",bytes,sizeof(bytes))==5 && !memcmp(bytes,"/else",5));
    struct vfs_path target={0},old_process={0},old_status={0};
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/self/fd/3",VFS_LOOKUP_FOLLOW,&target));
    assert(vfs_path_equal(&target,&file->path_ref));vfs_path_put(&target);
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/self/root",VFS_LOOKUP_FOLLOW,&target));
    assert(vfs_path_equal(&target,&root));vfs_path_put(&target);
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/self/cwd",VFS_LOOKUP_FOLLOW,&target));
    assert(vfs_path_equal(&target,&destination));vfs_path_put(&target);
    assert(!vfs_path_lookup(&ns,&root,&root,"/procdir/42",VFS_LOOKUP_FOLLOW,&old_process));
    assert(!vfs_path_lookup(&ns,&root,&old_process,"status",VFS_LOOKUP_FOLLOW,&old_status));
    assert(!vfs_mount_move(&ns,&mounted,&destination));
    assert(!vfs_path_read(&mounts_view,0,mount_text,sizeof(mount_text)-1,&got));mount_text[got]=0;
    assert(strstr(mount_text,"2 1 0:201 / /else ro - proc proc ro\n") && !strstr(mount_text,"/procdir"));
    assert(!vfs_file_read(old_view,0,mount_text,sizeof(mount_text)-1,&got));mount_text[got]=0;
    assert(strstr(mount_text,"/else ro - proc proc ro\n") && !strstr(mount_text,"/procdir"));
    assert(!vfs_file_read(new_view,0,mount_text,sizeof(mount_text)-1,&got));mount_text[got]=0;
    assert(strstr(mount_text,"2 1 0:201 / / ro - proc proc ro\n"));
    vfs_path_put(&mounts_view);
    const char *actual_executable="/else/sys/kernel/osrelease";
    assert(proc_readlink("/proc/self/exe",bytes,sizeof(bytes))==(int)strlen(actual_executable) &&
        !memcmp(bytes,actual_executable,strlen(actual_executable)));
    assert(!vfs_path_lookup(&ns,&root,&root,"/else/self/exe",VFS_LOOKUP_FOLLOW,&target));
    assert(vfs_path_same_inode(&target,&version));vfs_path_put(&target);
    assert(!vfs_path_read(&version,0,bytes,sizeof(bytes),&got) && got==11 && !memcmp(bytes,"9.8.7-0123\n",11));
    assert(!vfs_path_lookup(&ns,&root,&root,"/else/self/fd/3",VFS_LOOKUP_FOLLOW,&target));
    assert(vfs_path_equal(&target,&file->path_ref));vfs_path_put(&target);
    uint64_t cursor=0;struct reliefos_dir_entry entry;
    bool found_self=false;int next;
    while((next=vfs_path_readdir(&mounted,&cursor,&entry))>0)
        if(!strcmp(entry.name,"self"))found_self=true;
    assert(!next && found_self);
    sched_task_identity_detach(&current); /* Simulate reap/reuse with the same PID and task address. */
    assert(vfs_path_lookup(&ns,&root,&old_process,"status",0,&target)==-RELIEFOS_ENOENT);
    assert(vfs_path_read(&old_status,0,bytes,sizeof(bytes),&got)==-RELIEFOS_ENOENT && !got);
    assert(!vfs_file_read(old_view,0,mount_text,sizeof(mount_text)-1,&got));mount_text[got]=0;
    assert(strstr(mount_text,"/else ro - proc proc ro\n"));
    assert(!vfs_file_read(new_view,0,mount_text,sizeof(mount_text)-1,&got));mount_text[got]=0;
    assert(strstr(mount_text,"2 1 0:201 / / ro - proc proc ro\n"));
    vfs_file_close(old_view);vfs_file_close(new_view);
    assert(!vfs_path_lookup(&ns,&root,&root,"/else/42",0,&target));
    assert(!vfs_path_equal(&old_process,&target));vfs_path_put(&target);
    vfs_path_put(&old_status);vfs_path_put(&old_process);vfs_path_put(&version);
    vfs_path_put(&file->path_ref);*file=(struct task_file){0};
    vfs_path_put(&sched_task_mm(&current)->executable_path);
    vfs_path_put(&current.fs_paths.root);vfs_path_put(&current.fs_paths.cwd);
    vfs_path_put(&mounted);vfs_path_put(&destination);vfs_path_put(&point);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns) && !proc_path_pins);
    sched_task_identity_detach(&current);
    puts("PASS real proc mount: moved attachment, held-node read/readdir, FD/root/cwd magic links and dead PID identity isolation");
}
static void test_proc_mount_failures(void)
{
    unsigned before=proc_allocations,failed=0,passed=0;
    for(int budget=0;budget<7;++budget) {
        struct vfs_mount *mount=NULL;proc_allocation_budget=budget;
        int rc=proc_create_vfs_mount(&mount);proc_allocation_budget=-1;
        if(rc){assert(rc==-ENOMEM && !mount);++failed;}
        else {++passed;vfs_mount_put(mount);}
        assert(proc_allocations==before);
    }
    assert(failed && passed);
    struct task_identity *identity=NULL;
    assert(!sched_task_identity_get(&current,&identity));
    struct vfs_namespace ns={0};struct vfs_mount *mount=NULL;struct vfs_path root={0};
    assert(!proc_create_vfs_mount(&mount) && !vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns,&root));unsigned baseline=proc_allocations;
    failed=passed=0;
    for(int budget=0;budget<13;++budget) {
        struct vfs_path path={0};proc_allocation_budget=budget;
        int rc=vfs_path_lookup(&ns,&root,&root,"42/status",VFS_LOOKUP_FOLLOW,&path);
        proc_allocation_budget=-1;
        if(rc){assert(rc==-ENOMEM && !path.mount);++failed;}
        else {++passed;vfs_path_put(&path);}
        /* A borrowed lookup result may leave one bounded weak cache key,
         * while every failed held dentry/PID ownership must roll back. */
        assert(proc_allocations<=baseline+1);
    }
    assert(failed && passed);
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    sched_task_identity_put(identity);sched_task_identity_detach(&current);
    assert(proc_allocations==before);
    puts("PASS real proc mount allocation failures: context, inode keys, weak cache and PID references reclaimed");
}

int main(void)
{
    test_proc_fd_object_link();
    test_proc_mount_objects();
    test_proc_mount_failures();
    strcpy(current.path, "/usr/lib/reliefos/apps/procsys/procsys.elf");
    struct storage_node node;
    assert(proc_lookup("/proc/42", &node) == 0 && node.type == RELIEFOS_FS_TYPE_DIR);
    assert(proc_lookup("/proc/self", &node) == 0 && node.type == RELIEFOS_FS_TYPE_SYMLINK);
    assert(proc_lookup("/proc/43", &node) == -2);
    assert(proc_lookup("/proc/4294967338", &node) == -2);
    assert(proc_lookup("/proc/42unknown/stat", &node) == -2);
    assert(proc_lookup("/proc/42/stat", &node) == 0 && node.type == RELIEFOS_FS_TYPE_FILE);
    char contents[512];
    uint32_t got;
    assert(proc_read("/proc/sys/kernel/osrelease", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    assert(!strcmp(contents, "9.8.7-0123\n"));
    assert(proc_read("/proc/version", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    assert(!strcmp(contents, "ReliefNT version 9.8.7-0123 (2026-09-11 01:02:03)\n"));
    assert(proc_read("/proc/42/stat", 0, contents, sizeof(contents) - 1, &got) == 0 && got);
    contents[got] = 0;
    assert(strstr(contents, "42 (test)"));
    assert(proc_lookup("/proc/42/status", &node) == 0 && node.type == RELIEFOS_FS_TYPE_FILE);
    char link[64] = {0};
    assert(proc_readlink("/proc/self", link, sizeof(link)) == 2);
    assert(!memcmp(link, "42", 2));
    memset(link, 0, sizeof(link));
    /* A display filename alone is not an executed inode. The positive
     * owned-MM path case is exercised with the mounted graph below. */
    assert(proc_readlink("/proc/42/exe", link, sizeof(link)) == -RELIEFOS_ENOENT);
    assert(proc_readlink("/proc/42/stat", link, sizeof(link)) == -22);
    current.pty_fds[0] = (struct task_pty_fd){.used = 1, .fd = 0,
        .pty_id = 2, .endpoint = TASK_PTY_ENDPOINT_SLAVE};
    assert(proc_readlink("/proc/self/fd/0", link, sizeof(link)) == 9);
    assert(!memcmp(link, "/dev/tty2", 9));
    assert(proc_lookup("/proc/42/fd", &node) == 0 && node.type == RELIEFOS_FS_TYPE_DIR);
    assert(proc_lookup("/proc/42/fd/0", &node) == 0 && node.type == RELIEFOS_FS_TYPE_SYMLINK);
    assert(proc_readlink("/proc/self/fd/99", link, sizeof(link)) == -2);
    assert(proc_readlink("/proc/self/fd/4294967296", link, sizeof(link)) == -2);
    current.euid = current.suid = current.fsuid = 1001;
    current.as.cr3 = 4096;
    current.gid = 4000000000U; current.egid = current.sgid = current.fsgid = 4000000001U;
    assert(proc_read("/proc/42/status", 0, contents, sizeof(contents) - 1, &got) == 0 && got);
    contents[got] = 0;
    assert(strstr(contents, "Name:\ttest\n"));
    assert(strstr(contents, "Uid:\t1000\t1001\t1001\t1001\n"));
    assert(strstr(contents, "Gid:\t4000000000\t4000000001\t4000000001\t4000000001\n"));
    assert(strstr(contents, "VmRSS:\t12 kB\n"));
    sched_task_mm(&current)->env_start = 0x400ffc;
    sched_task_mm(&current)->env_end = 0x401008;
    memcpy(argument_pages + 4092, "VAR=a\0X=two\0", 12);
    assert(proc_lookup("/proc/self/environ", &node) == 0);
    assert(proc_read("/proc/self/environ", 0, contents, sizeof(contents), &got) == 0 && got == 12);
    assert(!memcmp(contents, "VAR=a\0X=two\0", 12));
    argument_pages[4096] = 'b';
    assert(proc_read("/proc/self/environ", 4, contents, 3, &got) == 0 && got == 3);
    assert(!memcmp(contents, "b\0X", 3));
    assert(proc_read("/proc/self/environ", 12, contents, sizeof(contents), &got) == 0 && got == 0);
    expose_foreign = true;
    foreign.uid=foreign.euid=foreign.suid=2000;
    strcpy(foreign.path,"/foreign-executable");
    uint32_t saved_euid=current.euid,saved_fsuid=current.fsuid;
    current.euid=current.fsuid=0; /* UID 0 without CAP_SYS_PTRACE is not a bypass. */
    assert(proc_readlink("/proc/43/exe",link,sizeof(link))==-RELIEFOS_EACCES);
    struct vfs_path denied={0};
    assert(proc_fd_get_path(&foreign,3,&denied)==-RELIEFOS_EACCES && !denied.mount);
    current.euid=saved_euid;current.fsuid=saved_fsuid;
    sched_task_mm(&foreign)->as.cr3 = 4096;
    sched_task_mm(&foreign)->env_start = sched_task_mm(&current)->env_start;
    sched_task_mm(&foreign)->env_end = sched_task_mm(&current)->env_end;
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == -RELIEFOS_EACCES && got == 0);
    foreign.uid = foreign.euid = foreign.suid = current.fsuid;
    foreign.gid = foreign.egid = foreign.sgid = current.fsgid;
    uint64_t foreign_cursor=0;struct reliefos_dir_entry foreign_entry;
    assert(proc_readdir("/proc/43/fd",&foreign_cursor,&foreign_entry)==0);
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == 0 && got == 12);
    sched_task_mm(&foreign)->nondumpable = true;
    assert(proc_readdir("/proc/43/fd",&foreign_cursor,&foreign_entry)==-RELIEFOS_EACCES);
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == -RELIEFOS_EACCES && got == 0);
    current.cap_effective |= 1ULL << CAP_SYS_PTRACE;
    assert(proc_read("/proc/43/environ", 0, contents, sizeof(contents), &got) == 0 && got == 12);
    current.cap_effective = 0;
    expose_foreign = false;
    uint64_t offset = 0;
    struct reliefos_dir_entry entry;
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "stat"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "cmdline"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "status"));
    const char *extra[] = {"mounts", "mountinfo", "exe", "cwd", "root"};
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, extra[i]));
        assert(entry.type == (i >= 2 ? RELIEFOS_FS_TYPE_SYMLINK : RELIEFOS_FS_TYPE_FILE));
    }
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "comm"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "environ"));
    assert(proc_readdir("/proc/42", &offset, &entry) == 1 && !strcmp(entry.name, "fd") && entry.type == RELIEFOS_FS_TYPE_DIR);
    assert(proc_readdir("/proc/42", &offset, &entry) == 0);
    offset = 0;
    assert(proc_readdir("/proc/42/fd", &offset, &entry) == 1 && !strcmp(entry.name, "0"));
    assert(proc_readdir("/proc/42/fd", &offset, &entry) == 0);
    current.pty_fds[1] = (struct task_pty_fd){.used = 1, .fd = 2048,
        .pty_id = 2, .endpoint = TASK_PTY_ENDPOINT_SLAVE};
    assert(!proc_lookup("/proc/42/fd/2048", &node) && node.type == RELIEFOS_FS_TYPE_SYMLINK);
    assert(proc_readlink("/proc/self/fd/2048", link, sizeof(link)) == 9);
    offset = 1;
    assert(proc_readdir("/proc/42/fd", &offset, &entry) == 1 && !strcmp(entry.name, "2048"));
    assert(proc_readdir("/proc/42/fd", &offset, &entry) == 0);
    assert(proc_lookup("/proc/mounts", &node) == 0 && node.type == RELIEFOS_FS_TYPE_SYMLINK);
    memset(link, '#', sizeof(link));
    assert(proc_readlink("/proc/mounts", link, 4) == 4 && !memcmp(link, "self", 4) && link[4] == '#');
    strcpy(sched_task_cwd(&current), "/home/test");
    assert(proc_readlink("/proc/self/cwd", link, sizeof(link)) == -RELIEFOS_ENOENT);
    assert(proc_readlink("/proc/self/root", link, sizeof(link)) == -RELIEFOS_ENOENT);
    assert(proc_read("/proc/self/mountinfo", 0, contents, sizeof(contents), &got) == -RELIEFOS_ENOENT && !got);
    assert(proc_read("/proc/filesystems", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    /* tmpfs now has a real mount backend, also exercised by the FIFO VM probe.
     * ext4 is listed because the ext-family backend serves both revisions. */
    assert(strstr(contents, "\text2\n") && strstr(contents, "\text4\n") &&
           strstr(contents, "\ttmpfs\n"));
    assert(proc_read("/proc/sys/kernel/hostname", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    assert(!strcmp(contents, "fixture-host\n"));
    offset = 0;
    int found = 0;
    while (proc_readdir("/proc", &offset, &entry) > 0) {
        char path[256];
        snprintf(path, sizeof(path), "/proc/%s", entry.name);
        assert(proc_lookup(path, &node) == 0 && entry.type == node.type);
        if (!strcmp(entry.name, "42")) found = 1;
    }
    assert(found);
    /* Process cookies cannot be scheduler array indexes: reaping swaps the
     * last live entry into the removed slot between getdents calls. */
    visible_tasks=4;
    extra_tasks[0]=(struct task){.pid=100,.name="retiring"};
    extra_tasks[1]=(struct task){.pid=200,.name="stable"};
    extra_tasks[2]=(struct task){.pid=300,.name="moved"};
    offset=0;
    do { assert(proc_readdir("/proc",&offset,&entry)==1); } while(strcmp(entry.name,"42"));
    assert(proc_readdir("/proc",&offset,&entry)==1 && !strcmp(entry.name,"100"));
    extra_tasks[0]=extra_tasks[2];visible_tasks=3;
    assert(proc_readdir("/proc",&offset,&entry)==1 && !strcmp(entry.name,"200"));
    assert(proc_readdir("/proc",&offset,&entry)==1 && !strcmp(entry.name,"300"));
    assert(proc_readdir("/proc",&offset,&entry)==0);
    extra_tasks[0]=(struct task){.pid=UINT32_MAX,.name="last-pid"};visible_tasks=2;
    offset=0;
    do { assert(proc_readdir("/proc",&offset,&entry)==1); } while(strcmp(entry.name,"42"));
    assert(proc_readdir("/proc",&offset,&entry)==1 && !strcmp(entry.name,"4294967295"));
    assert(proc_readdir("/proc",&offset,&entry)==0);
    /* Both proc enumeration and Linux stat's thread count must see tasks
     * created after the scheduler's initial 64 slots. */
    visible_tasks = 101;
    for (uint32_t i = 0; i < 100; ++i)
        extra_tasks[i] = (struct task){.pid = 100 + i, .tgid = 42, .name = "thread"};
    offset = 0;
    unsigned numeric_entries = 0;
    while (proc_readdir("/proc", &offset, &entry) > 0)
        if (entry.name[0] >= '0' && entry.name[0] <= '9') ++numeric_entries;
    if (numeric_entries != 101)
        fprintf(stderr, "proc only exposed %u of 101 tasks\n", numeric_entries);
    assert(numeric_entries == 101);
    assert(proc_read("/proc/42/stat", 0, contents, sizeof(contents) - 1, &got) == 0);
    contents[got] = 0;
    char *field = strchr(contents, ')') + 2;
    for (unsigned n = 3; n < 20; ++n) field = strchr(field, ' ') + 1;
    assert(!strncmp(field, "101 ", 4));
    puts("PASS procfs task directory lookup, traversal, enumeration and overflow rejection");
}
