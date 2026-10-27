#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/tmpfs.h>
#include <reliefnt/time.h>
#include <linux/mount.h>
#include <linux/limits.h>
static struct {
    uint64_t phys;
    unsigned refs;
} pages[64];
static int fail_page_after = -1, fail_heap_after = -1;
void *mm_phys_to_virt(uint64_t physical) { return (void *)(uintptr_t)physical; }
uint64_t mm_alloc_page(void)
{
    if (fail_page_after == 0) return 0;
    if (fail_page_after > 0) --fail_page_after;
    for (unsigned i = 0; i < 64; ++i)
        if (!pages[i].refs) {
            void *p = aligned_alloc(4096, 4096);
            assert(p);
            pages[i].phys = (uintptr_t)p;
            pages[i].refs = 1;
            return pages[i].phys;
        }
    return 0;
}
void mm_retain_page(uint64_t phys)
{
    for (unsigned i = 0; i < 64; ++i)
        if (pages[i].phys == phys && pages[i].refs) {
            ++pages[i].refs;
            return;
        }
    abort();
}
void mm_free_page(uint64_t phys)
{
    for (unsigned i = 0; i < 64; ++i)
        if (pages[i].phys == phys && pages[i].refs) {
            if (!--pages[i].refs)
                free((void *)(uintptr_t)phys);
            return;
        }
    abort();
}
static unsigned invalidations;
void sched_truncate_file_mappings(const struct storage_node *node, uint64_t length)
{
    assert(node->flags & STORAGE_NODE_FLAG_TMPFS);
    (void)length;
    ++invalidations;
}
void *kernel_malloc(size_t size)
{
    if (fail_heap_after == 0) return NULL;
    if (fail_heap_after > 0) --fail_heap_after;
    return malloc(size);
}
void kernel_free(void *p) { free(p); }
void page_cache_invalidate_node(const struct storage_node *node)
{
    assert(node->flags & STORAGE_NODE_FLAG_TMPFS);
}
uint64_t mm_total_memory_kib(void) { return 65536; }
int time_wall_clock(struct reliefos_time_info *out)
{
    memset(out, 0, sizeof(*out));
    out->unix_seconds = 1234;
    return 0;
}
static void retained_directory_mutations(void)
{
    struct tmpfs_super *fs=NULL;struct storage_node root,directory,source,replacement,node;
    assert(!tmpfs_new(8,0,"size=0,nr_inodes=0",0,0,&fs));
    assert(!tmpfs_lookup(fs,"/",&root));
    assert(!tmpfs_create(fs,"/before",LINUX_S_IFDIR|0755,NULL,&directory));
    assert(!tmpfs_hold(fs,directory.first_cluster));
    assert(!tmpfs_create_child(fs,directory.first_cluster,"source",LINUX_S_IFREG|0640,NULL,NULL,&source));
    assert(!tmpfs_rename(fs,"/before","/after"));
    assert(!tmpfs_create(fs,"/before",LINUX_S_IFDIR|0755,NULL,&replacement));
    assert(!tmpfs_link_child(fs,source.first_cluster,directory.first_cluster,"alias"));
    assert(tmpfs_lookup_child(fs,replacement.first_cluster,"alias",&node)==-2);
    assert(!tmpfs_rename_child(fs,directory.first_cluster,"source",root.first_cluster,"moved"));
    assert(!tmpfs_lookup(fs,"/moved",&node) && node.first_cluster==source.first_cluster);
    assert(!tmpfs_unlink_child(fs,directory.first_cluster,"alias",false));
    assert(!tmpfs_unlink_child(fs,root.first_cluster,"moved",false));
    assert(tmpfs_link_child(fs,replacement.first_cluster,directory.first_cluster,"dir-link")==-1);
    assert(tmpfs_unlink_child(fs,directory.first_cluster,"../escape",false)==-22);
    assert(tmpfs_rename_child(fs,directory.first_cluster,".",root.first_cluster,"bad")==-22);
    assert(!tmpfs_unlink_child(fs,root.first_cluster,"after",true));
    assert(tmpfs_link_child(fs,replacement.first_cluster,directory.first_cluster,"lost")==-2);
    tmpfs_drop(fs,directory.first_cluster);tmpfs_destroy(fs);
    puts("PASS retained tmpfs directory unlink/link/rename without text replay");
}
static void real_preallocation(void)
{
    struct tmpfs_super *fs=NULL;struct storage_node file;
    struct linux_stat_abi st;struct linux_statfs_abi space;
    assert(!tmpfs_new(9,0,"size=12k",0,0,&fs));
    assert(!tmpfs_create(fs,"/reserved",LINUX_S_IFREG|0600,NULL,&file));
    uint32_t done=0;char data[12288];
    assert(!tmpfs_write(fs,file.first_cluster,3,"keep",4,&done) && done==4);
    /* KEEP_SIZE really reserves the two additional pages, without changing
     * existing bytes or exposing the reserved range beyond EOF. */
    assert(!tmpfs_fallocate(fs,file.first_cluster,1,4097,8191));
    assert(!tmpfs_stat(fs,file.first_cluster,&st) && st.st_size==7 && st.st_blocks==24);
    assert(!tmpfs_read(fs,file.first_cluster,4097,data,1,&done) && !done);
    fail_heap_after=0;fail_page_after=0;
    assert(!tmpfs_fallocate(fs,file.first_cluster,0,0,sizeof(data)));
    assert(!tmpfs_write(fs,file.first_cluster,8192,"end",3,&done) && done==3);
    fail_heap_after=fail_page_after=-1;
    assert(!tmpfs_read(fs,file.first_cluster,0,data,sizeof(data),&done) && done==sizeof(data));
    for (unsigned i=0;i<done;++i) {
        char expected=i>=3 && i<7 ? "keep"[i-3] : i>=8192 && i<8195 ? "end"[i-8192] : 0;
        assert(data[i]==expected);
    }
    assert(tmpfs_fallocate(fs,file.first_cluster,2,0,1)==-95);
    assert(tmpfs_fallocate(fs,file.first_cluster,0,0,0)==-22);
    assert(tmpfs_fallocate(fs,file.first_cluster,0,INT64_MAX,1)==-27);
    tmpfs_set_flags(fs,MS_RDONLY);
    assert(tmpfs_fallocate(fs,file.first_cluster,0,0,1)==-30);
    tmpfs_destroy(fs);

    assert(!tmpfs_new(9,0,"size=8k",0,0,&fs));
    assert(!tmpfs_create(fs,"/rollback",LINUX_S_IFREG|0600,NULL,&file));
    assert(!tmpfs_write(fs,file.first_cluster,0,"old",3,&done));
    /* Failure after a newly allocated page must undo only new backing, while
     * retaining the old page, its contents and the old logical size. */
    assert(tmpfs_fallocate(fs,file.first_cluster,0,4096,8192)==-28);
    assert(!tmpfs_stat(fs,file.first_cluster,&st) && st.st_size==3 && st.st_blocks==8);
    tmpfs_statfs(fs,&space);assert(space.f_bfree==1);
    tmpfs_destroy(fs);
    assert(!tmpfs_new(9,0,"size=16k",0,0,&fs));
    assert(!tmpfs_create(fs,"/rollback",LINUX_S_IFREG|0600,NULL,&file));
    assert(!tmpfs_write(fs,file.first_cluster,0,"old",3,&done));
    for (unsigned kind=0;kind<2;++kind) {
        if (kind) fail_heap_after=1; else fail_page_after=1;
        assert(tmpfs_fallocate(fs,file.first_cluster,0,4096,8192)==-12);
        fail_heap_after=fail_page_after=-1;
        assert(!tmpfs_stat(fs,file.first_cluster,&st) && st.st_size==3 && st.st_blocks==8);
        tmpfs_statfs(fs,&space);assert(space.f_bfree==3);
        assert(!tmpfs_read(fs,file.first_cluster,0,data,3,&done) && done==3 && !memcmp(data,"old",3));
    }
    tmpfs_destroy(fs);
    puts("PASS tmpfs real preallocation/KEEP_SIZE, zero holes, quota/OOM rollback and allocation-free writes");
}
static void long_link_payload(void)
{
    struct tmpfs_super *fs=NULL;struct storage_node link;struct linux_stat_abi st;
    char target[LINUX_PATH_MAX+1],copied[LINUX_PATH_MAX];uint32_t read=0;
    memset(target,'x',sizeof(target));target[LINUX_PATH_MAX-1]=0;
    assert(!tmpfs_new(10,0,"nr_inodes=2",0,0,&fs));
    assert(!tmpfs_create(fs,"/long",LINUX_S_IFLNK|0777,target,&link));
    assert(!tmpfs_stat(fs,link.first_cluster,&st) && st.st_size==LINUX_PATH_MAX-1);
    assert(!tmpfs_readlink(fs,link.first_cluster,copied,sizeof(copied),&read) &&
        read==LINUX_PATH_MAX-1 && !memcmp(copied,target,read));
    assert(!tmpfs_unlink(fs,"/long",false));
    target[LINUX_PATH_MAX-1]='x';target[LINUX_PATH_MAX]=0;
    assert(tmpfs_create(fs,"/too-long",LINUX_S_IFLNK|0777,target,&link)==-36);
    assert(tmpfs_lookup(fs,"/too-long",&link)==-2);
    assert(!tmpfs_create(fs,"/after-failure",LINUX_S_IFLNK|0777,"ok",&link));
    tmpfs_destroy(fs);
    puts("PASS tmpfs Linux PATH_MAX symlink payload, exact readlink and failure quota rollback");
}
int main(void)
{
    long_link_payload();
    real_preallocation();
    retained_directory_mutations();
    struct tmpfs_super *fs = NULL;
    struct storage_node a, b;
    struct linux_stat_abi st;
    struct linux_statfs_abi space;
    char data[16384], got[16384];
    memset(data, 0xab, sizeof(data));
    uint32_t done;
    assert(tmpfs_new(3, 0, "size=8k,nr_inodes=8,mode=1770,uid=42,gid=43", 0, 0, &fs) == 0);
    assert(tmpfs_lookup(fs, "/", &a) == 0 && tmpfs_stat(fs, a.first_cluster, &st) == 0);
    assert(st.st_uid == 42 && st.st_gid == 43 && st.st_mode == 041770 && st.st_nlink == 2);
    assert(tmpfs_create(fs, "/a", 0100640, NULL, &a) == 0);
    assert(tmpfs_write(fs, a.first_cluster, 0, data, sizeof(data), &done) == -28 && done == 8192);
    tmpfs_statfs(fs, &space);
    assert(space.f_type == 0x01021994 && space.f_blocks == 2 && space.f_bfree == 0);
    assert(tmpfs_truncate(fs, a.first_cluster, 17) == 0);
    assert(tmpfs_truncate(fs, a.first_cluster, 8192) == 0);
    assert(tmpfs_read(fs, a.first_cluster, 0, got, sizeof(got), &done) == 0 && done == 8192);
    assert(!memcmp(got, data, 17));
    for (unsigned i = 17; i < done; ++i)
        assert(got[i] == 0);
    assert(tmpfs_create(fs, "/d", 0040700, NULL, &b) == 0);
    assert(tmpfs_hold(fs, a.first_cluster) == 0);
    assert(tmpfs_link(fs, "/a", "/d/link") == 0);
    assert(tmpfs_rename(fs, "/a", "/d/moved") == 0);
    assert(tmpfs_lookup(fs, "/a", &b) == -2);
    assert(tmpfs_unlink(fs, "/d", true) == -39);
    assert(tmpfs_unlink(fs, "/d/link", false) == 0);
    assert(tmpfs_unlink(fs, "/d/moved", false) == 0);
    assert(tmpfs_stat(fs, a.first_cluster, &st) == 0 && st.st_nlink == 0 && st.st_size == 8192);
    assert(tmpfs_read(fs, a.first_cluster, 0, got, 17, &done) == 0 && done == 17 && !memcmp(data, got, 17));
    tmpfs_drop(fs, a.first_cluster);
    assert(tmpfs_stat(fs, a.first_cluster, &st) == -2);
    assert(tmpfs_create(fs, "/d/sub", 0040755, NULL, &a) == 0);
    assert(tmpfs_rename(fs, "/d", "/d/sub/cycle") == -22);
    assert(tmpfs_create(fs, "/symlink", 0120777, "d/sub", &a) == 0);
    assert(tmpfs_readlink(fs, a.first_cluster, got, 3, &done) == 0 && done == 3 && !memcmp(got, "d/s", 3));
    struct reliefos_permissions p = {0601, 1000, 1001};
    assert(tmpfs_permissions(fs, a.first_cluster, &p, true) == 0);
    assert(tmpfs_stat(fs, a.first_cluster, &st) == 0 && st.st_uid == 1000 && st.st_mode == 0120601);
    tmpfs_set_flags(fs, MS_RDONLY);
    assert(tmpfs_create(fs, "/no", 0100600, NULL, &b) == -30);
    assert(tmpfs_unlink(fs, "/symlink", false) == -30);
    assert(tmpfs_permissions(fs, a.first_cluster, &p, true) == -30);
    tmpfs_destroy(fs);
    assert(tmpfs_new(3, 0, "size=1k,unknown=yes", 0, 0, &fs) == -22 && !fs);
    assert(tmpfs_new(3, 0, "size=18446744073709551615G", 0, 0, &fs) == -22 && !fs);
    assert(tmpfs_new(3, 0, "nr_inodes=2", 0, 0, &fs) == 0);
    assert(tmpfs_create(fs, "/a", 0100600, NULL, &a) == 0);
    assert(tmpfs_create(fs, "/b", 0100600, NULL, &b) == -28);
    assert(tmpfs_link(fs, "/a", "/hardlink") == -28);
    tmpfs_destroy(fs);
    assert(tmpfs_new(3, 0, "size=4k", 0, 0, &fs) == 0);
    assert(tmpfs_create(fs, "/shared", 0100600, NULL, &a) == 0);
    assert(tmpfs_truncate(fs, a.first_cluster, 8192) == 0);
    uint64_t first, second;
    assert(tmpfs_get_page(fs, a.first_cluster, 0, &first) == 0);
    assert(tmpfs_get_page(fs, a.first_cluster, 0, &second) == 0 && first == second);
    assert(tmpfs_get_page(fs, a.first_cluster, 4096, &second) == -28);
    assert(tmpfs_get_page(fs, a.first_cluster, 8192, &second) == -22);
    strcpy((char *)(uintptr_t)first, "shared");
    assert(tmpfs_read(fs, a.first_cluster, 0, got, 7, &done) == 0 && !strcmp(got, "shared"));
    assert(tmpfs_write(fs, a.first_cluster, 0, "direct", 7, &done) == 0);
    assert(!strcmp((char *)(uintptr_t)first, "direct"));
    unsigned before = invalidations;
    assert(tmpfs_truncate(fs, a.first_cluster, 0) == 0 && invalidations == before + 1);
    tmpfs_statfs(fs, &space);
    assert(space.f_bfree == 1);
    /* Mapping references outlive removal of the inode's ownership. */
    assert(!strcmp((char *)(uintptr_t)first, "direct"));
    mm_free_page(first);
    mm_free_page(first);
    tmpfs_destroy(fs);
    for (unsigned i = 0; i < 64; ++i)
        assert(!pages[i].refs);
    puts("PASS tmpfs sparse pages, limits, metadata, links, rename cycles, held unlink and read-only");
}
