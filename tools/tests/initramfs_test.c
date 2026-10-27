/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/tmpfs.h>
#include <reliefnt/mount.h>
#include <reliefnt/time.h>
#include "../../fs/initramfs/unpack.h"

static size_t allocations,pages,attempts,fail_heap;
static bool fail_page;
void *kernel_malloc(size_t size)
{
    if(++attempts==fail_heap)return NULL;
    void *p=calloc(1,size);if(p)++allocations;return p;
}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
uint64_t mm_alloc_page(void)
{
    if(fail_page)return 0;
    void *p=aligned_alloc(4096,4096);if(p)++pages;return (uintptr_t)p;
}
void mm_free_page(uint64_t p){assert(p && pages);--pages;free((void *)(uintptr_t)p);}
void *mm_phys_to_virt(uint64_t p){return (void *)(uintptr_t)p;}
uint64_t mm_total_memory_kib(void){return 65536;}
void page_cache_invalidate_node(const struct storage_node *node){assert(node->flags&STORAGE_NODE_FLAG_TMPFS);}
void sched_truncate_file_mappings(const struct storage_node *node,uint64_t length){(void)length;assert(node);}
int time_wall_clock(struct reliefos_time_info *out){*out=(struct reliefos_time_info){.unix_seconds=5000};return 0;}
bool try_module_get(struct module *owner){assert(!owner);return true;}
void module_put(struct module *owner){assert(!owner);}

static unsigned char archive[65536];
static size_t used;
static void record(const char *name,uint32_t mode,uint32_t ino,uint32_t links,
                   const void *data,uint32_t length,uint32_t major,uint32_t minor)
{
    char header[111];
    assert(snprintf(header,sizeof(header),"070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
        ino,mode,12u,34u,links,123u,length,7u,8u,major,minor,(unsigned)strlen(name)+1,0u)==110);
    memcpy(archive+used,header,110);used+=110;
    memcpy(archive+used,name,strlen(name)+1);used+=strlen(name)+1;
    while(used%4)archive[used++]=0;
    if(length){memcpy(archive+used,data,length);used+=length;}
    while(used%4)archive[used++]=0;
}
static struct storage_node lookup(struct tmpfs_super *fs,const char *name)
{struct storage_node n;assert(!tmpfs_lookup(fs,name,&n));return n;}
static struct linux_stat_abi stat_node(struct tmpfs_super *fs,struct storage_node n)
{struct linux_stat_abi st;assert(!tmpfs_stat(fs,n.first_cluster,&st));return st;}
static void read_file(struct tmpfs_super *fs,const char *name,const char *expected)
{
    char data[128]={0};uint32_t length=0;struct storage_node n=lookup(fs,name);
    assert(!tmpfs_read(fs,n.first_cluster,0,data,sizeof(data),&length));
    assert(length==strlen(expected) && !memcmp(data,expected,length));
}
static void manual_archive(void)
{
    used=0;
    record(".",LINUX_S_IFDIR|0755,1,2,NULL,0,0,0);
    record("usr",LINUX_S_IFDIR|0700,2,2,NULL,0,0,0);
    record("bin",LINUX_S_IFLNK|0777,3,1,"/usr",4,0,0);
    record("bin/first",LINUX_S_IFREG|0640,4,3,NULL,0,0,0);
    record("usr/second",LINUX_S_IFREG|0640,4,3,"last data",9,0,0);
    record("usr/third",LINUX_S_IFREG|0640,4,3,NULL,0,0,0);
    record("dev",LINUX_S_IFDIR|0755,5,2,NULL,0,0,0);
    record("dev/console",LINUX_S_IFCHR|0600,6,1,NULL,0,5,1);
    record("dev/wide",LINUX_S_IFBLK|0600,7,1,NULL,0,259,0xabcde);
    record("pipe",LINUX_S_IFIFO|0620,8,1,NULL,0,0,0);
    record("socket",LINUX_S_IFSOCK|0600,9,1,NULL,0,0,0);
    record("link-a",LINUX_S_IFLNK|0777,10,2,"usr",3,0,0);
    record("link-b",LINUX_S_IFLNK|0777,10,2,"/usr",4,0,0);
    record("up",LINUX_S_IFLNK|0777,11,1,"../../usr",9,0,0);
    record("up/clipped",LINUX_S_IFREG|0644,12,1,"rooted",6,0,0);
    record("TRAILER!!!",0,0,1,NULL,0,0,0);
    record("separate",LINUX_S_IFREG|0600,4,2,"independent",11,0,0);
    record("replacement",LINUX_S_IFLNK|0777,20,1,"usr",3,0,0);
    record("replacement",LINUX_S_IFREG|0644,21,1,"regular",7,0,0);
}
static void inspect(struct tmpfs_super *fs)
{
    read_file(fs,"/usr/first","last data");read_file(fs,"/usr/second","last data");
    read_file(fs,"/usr/third","last data");read_file(fs,"/usr/clipped","rooted");
    read_file(fs,"/separate","independent");read_file(fs,"/replacement","regular");
    struct storage_node n=lookup(fs,"/usr/first");
    assert(n.first_cluster==lookup(fs,"/usr/third").first_cluster);
    struct linux_stat_abi st=stat_node(fs,n);
    assert(st.st_nlink==3 && st.st_uid==12 && st.st_gid==34 && (st.st_mode&07777)==0640 && st.mtime_sec==123);
    assert(stat_node(fs,lookup(fs,"/usr")).mtime_sec==123);
    assert(stat_node(fs,lookup(fs,"/")).mtime_sec==123);
    st=stat_node(fs,lookup(fs,"/dev/console"));assert(st.st_mode==(LINUX_S_IFCHR|0600) && st.st_rdev==0x501);
    st=stat_node(fs,lookup(fs,"/dev/wide"));
    assert((st.st_mode&LINUX_S_IFMT)==LINUX_S_IFBLK && st.st_rdev==((uint64_t)259<<8 | 0xde | (uint64_t)0xabc00<<12));
    assert(lookup(fs,"/pipe").type==RELIEFOS_FS_TYPE_FIFO && lookup(fs,"/socket").type==RELIEFOS_FS_TYPE_SOCKET);
    n=lookup(fs,"/link-a");assert(n.first_cluster==lookup(fs,"/link-b").first_cluster);
    char link[16];uint32_t length;assert(!tmpfs_readlink(fs,n.first_cluster,link,sizeof(link),&length));
    assert(length==4 && !memcmp(link,"/usr",4));
}
int main(int argc,char **argv)
{
    assert(argc==2);manual_archive();struct tmpfs_super *fs=NULL;
    assert(!initramfs_unpack(archive,used,0,&fs) && fs);inspect(fs);tmpfs_destroy(fs);assert(!allocations && !pages);
    /* Every allocator failure, including after publication inside the private
     * namespace, must leave no filesystem, path, hardlink hold or page alive. */
    bool completed=false;
    for(size_t failure=1;failure<1000;++failure) {
        attempts=0;fail_heap=failure;
        int rc=initramfs_unpack(archive,used,0,&fs);fail_heap=0;
        if(!rc){assert(fs);inspect(fs);tmpfs_destroy(fs);completed=true;break;}
        assert(rc==-12 && !fs && !allocations && !pages);
    }
    assert(completed && !allocations && !pages);
    fail_page=true;assert(initramfs_unpack(archive,used,0,&fs)==-12 && !fs);fail_page=false;
    assert(!allocations && !pages);
    size_t saved=used;record("late",LINUX_S_IFREG|0600,40,1,"end",3,0,0);archive[used++]=1;
    assert(initramfs_unpack(archive,used,0,&fs)==-8 && !fs && !allocations && !pages);used=saved;
    record("missing/parent",LINUX_S_IFREG|0600,40,1,NULL,0,0,0);
    assert(initramfs_unpack(archive,used,0,&fs)==-2 && !fs && !allocations && !pages);
    used=0;record("cycle",LINUX_S_IFLNK|0777,1,1,"cycle",5,0,0);
    record("cycle/file",LINUX_S_IFREG|0600,2,1,NULL,0,0,0);
    assert(initramfs_unpack(archive,used,0,&fs)==-40 && !fs && !allocations && !pages);
    used=0;record("embedded",LINUX_S_IFLNK|0777,1,1,"a\0b",3,0,0);
    assert(initramfs_unpack(archive,used,0,&fs)==-22 && !fs && !allocations && !pages);
    FILE *input=fopen(argv[1],"rb");assert(input);used=fread(archive,1,sizeof(archive),input);assert(feof(input));fclose(input);
    assert(!initramfs_unpack(archive,used,3,&fs));read_file(fs,"/usr/中文","native archive\n");read_file(fs,"/hard","native archive\n");
    assert(lookup(fs,"/usr/中文").first_cluster==lookup(fs,"/hard").first_cluster);
    tmpfs_destroy(fs);assert(!allocations && !pages);
    puts("PASS private initramfs objects, real VFS symlinks/root clamp, data-last hardlinks, devices, timestamps, GNU cpio and all-allocation rollback");
}
