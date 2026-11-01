/* Real VFS dentry/mount/rename, swap slots, retained reporting and proc stream;
 * the namespace's storage backend is modeled by the inherited fixture. */
#define main inherited_mount_tree_main
#include "mount_tree_test.c"
#undef main
#include "../../kernel/syscall/memory/swap_area.c"
#include "../../fs/proc/internal.h"
#include <reliefnt/memory_stats.h>

struct task *sched_current_task(void) { return NULL; }
void memory_snapshot_get(struct memory_snapshot *out) { *out=(struct memory_snapshot){0}; }
void memory_events_get(struct memory_events *out) { *out=(struct memory_events){0}; }

/** @brief Consume one immutable proc view in small positional fragments. */
static size_t read_fragments(void *view,char *buffer,size_t capacity)
{
    size_t offset=0;
    for (;;) {
        uint32_t got=0;
        assert(capacity-offset>13);
        assert(!proc_memory_read(view,offset,buffer+offset,13,&got));
        offset+=got; if (!got) break;
    }
    buffer[offset]=0; return offset;
}

/** @brief Exercise real retained-path rendering and separate drain domains. */
int main(void)
{
    struct vfs_namespace ns={0}; struct vfs_mount *base=mount_new(&a,NULL);
    assert(!vfs_namespace_init(&ns,base)); vfs_mount_put(base);
    struct vfs_path root={0}; assert(!vfs_namespace_root(&ns,&root));
    struct vfs_path directory=find(&ns,&root,&root,"mnt",2);
    struct vfs_path old=find(&ns,&root,&directory,"old",4);
    struct vfs_path other=find(&ns,&root,&root,"other",3);
    struct vfs_path file=find(&ns,&root,&root,"file",5);
    enum { AREAS=24 }; struct swap_backing back[AREAS]={0};
    uint8_t header[4096]={0}; header[1024]=1; header[1028]=3;
    memcpy(header+4086,"SWAPSPACE2",10);
    for (unsigned i=0;i<AREAS;++i) {
        assert(!swap_area_create(header,sizeof(header),4,i ? -2-(int)i : 99,&back[i].area));
        assert(!swap_area_publish(back[i].area));
        vfs_path_get(i&1 ? &file : &old,&back[i].path);
        back[i].raw=(i==1); back[i].next=i+1<AREAS ? &back[i+1] : NULL;
    }
    swap_backings=&back[0];
    uint64_t token=swap_slot_alloc(); assert(token);
    struct swap_stats stats; swap_get_stats(&stats);
    assert(stats.total==AREAS*3 && stats.free==AREAS*3-1 && stats.used==1);
    void *before=NULL; assert(!proc_memory_open("/proc/swaps",&before));
    char long_name[240]; memset(long_name,'q',220); long_name[0]='s'; long_name[1]=' ';
    long_name[2]='\t'; long_name[3]='\n'; long_name[4]='\\'; long_name[220]=0;
    struct vfs_rename *change=NULL;
    assert(!vfs_rename_prepare(&ns,&directory,&other,long_name,&change));
    a_entries[1].parent=3; a_entries[1].name=long_name; vfs_rename_commit(change);
    assert(!swap_area_begin_drain(back[0].area)); swap_get_stats(&stats);
    assert(stats.total==1+(AREAS-1)*3 && stats.free==(AREAS-1)*3 && stats.used==1);
    void *after=NULL; assert(!proc_memory_open("/proc/swaps",&after));
    char first[16000],second[16000]; size_t n1=read_fragments(before,first,sizeof(first));
    size_t n2=read_fragments(after,second,sizeof(second));
    assert(n1 && n2>1024 && strstr(first,"/mnt/old\tfile\t12\t4\t99\n"));
    assert(!strstr(first,"/other/s") && strstr(second,"/other/s\\040\\011\\012\\134"));
    assert(strstr(second,"\tfile\t12\t4\t99\n") && strstr(second,"\tpartition\t12\t0\t-3\n"));
    swap_area_abort_drain(back[0].area); swap_get_stats(&stats);
    assert(stats.total==AREAS*3 && stats.free==AREAS*3-1 && stats.used==1);
    proc_memory_close(before); proc_memory_close(after); swap_slot_put(token); swap_backings=NULL;
    for (unsigned i=0;i<AREAS;++i) {
        assert(!swap_area_unpublish(back[i].area)); swap_area_destroy(back[i].area);
        vfs_path_put(&back[i].path);
    }
    vfs_path_put(&file); vfs_path_put(&other); vfs_path_put(&old);
    vfs_path_put(&directory); vfs_path_put(&root); assert(!vfs_namespace_destroy(&ns));
    a_entries[1].parent=1; a_entries[1].name="mnt"; no_pins(&a); assert(!allocations);
    printf("PASS %u real slot areas, live ancestor rename, octal streaming (%zu bytes), stable snapshot, drain rollback\n",AREAS,n2);
    return 0;
}
