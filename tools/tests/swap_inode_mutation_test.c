/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
#include <reliefnt/page_cache.h>

struct storage_volume g_volumes[STORAGE_MAX_VOLUMES];
struct storage_volume g_shmem_volume;
struct storage_volume *g_active_volume=&g_volumes[0];
static unsigned writes;

/** @brief Enter the fixture's single execution transaction. @param flags Saved state. */
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags=0; }
/** @brief Leave fixture execution. @param flags Saved state. */
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
/** @brief Allocate metadata. @param n Byte count. @return Zero-filled allocation. */
void *kernel_malloc(size_t n) { return calloc(1,n); }
/** @brief Release metadata. @param p Owned allocation. */
void kernel_free(void *p) { free(p); }
/** @brief Report no cache pages. @param n Borrowed node. @return Zero. */
int page_cache_has_inode(const struct storage_node *n) { (void)n; return 0; }
/** @brief Update no cached pages. @param n Node. @param a Old size. @param b New size. */
void page_cache_resize_node(const struct storage_node *n,uint64_t a,uint64_t b)
{ (void)n;(void)a;(void)b; }
/** @brief Supply synthetic clustered timestamps. @param r Inode. @return Zero. */
int storage_cluster_load_times(struct storage_inode_ref *r) { (void)r; return 0; }
/** @brief Select the fixture provider. @param node Node. @param previous Saved volume.
 * @return Zero. */
int storage_select_node_volume(const struct storage_node *node,struct storage_volume **previous)
{ assert(node); if(previous)*previous=g_active_volume; return 0; }
/** @brief Restore the selected fixture provider. @param previous Saved volume. */
void storage_restore_volume(struct storage_volume *previous) { (void)previous; }
/** @brief Admit one fixture mutation. */
void storage_begin_mutation(void) { }
/** @brief Write through the actual retained-inode dispatch.
 * @param v Volume. @param n Node. @param off Offset. @param b Source.
 * @param len Length. @param done Completed bytes. @return Zero.
 */
static int fake_write(struct storage_volume *v,struct storage_node *n,uint64_t off,
                      const void *b,uint32_t len,uint32_t *done)
{ (void)v;(void)n;(void)off;(void)b; ++writes; *done=len; return 0; }

/** @brief Old open writers are rechecked after the swap claim is published.
 * @return Zero on success.
 */
int main(void)
{
    const struct storage_filesystem_ops ops={.write=fake_write};
    g_volumes[0].ready=true;g_volumes[0].fs_ops=&ops;
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_EXT2,
                              .first_cluster=7,.volume_id=0};
    struct storage_inode_ref *inode=NULL;
    assert(!storage_inode_get(&node,&inode) && inode);
    assert(!storage_inode_write_get(inode)); /* old OFD stays open */
    assert(!storage_inode_swap_claim(inode));
    assert(!storage_inode_write_get(inode)); /* later plain writable open */
    uint32_t done=999; const char byte='a';
    assert(storage_write_held_node(&node,0,&byte,1,&done)==-ETXTBSY);
    assert(!done && !writes);
    storage_inode_swap_release(inode);
    assert(!storage_write_held_node(&node,0,&byte,1,&done) && done==1 && writes==1);
    storage_inode_write_put(inode);storage_inode_write_put(inode);free(inode);
    puts("swap old-writer mutation: ok"); return 0;
}
