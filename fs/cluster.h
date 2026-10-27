/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FS_CLUSTER_H
#define RELIEFNT_FS_CLUSTER_H
#include <reliefnt/storage.h>
#include "cluster_time.h"
struct storage_volume;

/* Native primitives consumed by the common held-cluster inode algorithm.
 * All calls borrow an instance/code pin, require its exact native selection
 * and storage execution ownership, and run without graph/module spinlocks.
 * collect fills the fixed execution-owned storage_old_chain workspace and
 * count output within FAT32_MAX_FILE_CLUSTERS; io transfers one cluster into
 * the caller's cluster-sized buffer. publish changes only this generation's
 * current linked directory record (the core skips it for removed objects).
 * Allocations and chain edits retain the common caller's rollback ordering.
 * Metadata/output pointers are caller-owned; every callback returns zero or
 * negative errno and retains no pointer to the caller's stack/workspace.
 */
struct storage_cluster_ops {
    uint32_t eoc;
    int (*link)(struct storage_volume *volume,uint32_t cluster,uint32_t next);
    int (*free_one)(struct storage_volume *volume,uint32_t cluster);
    int (*collect)(struct storage_volume *volume,const struct storage_node *node,uint32_t *count);
    int (*io)(struct storage_volume *volume,uint32_t cluster,void *bytes,bool write);
    int (*publish)(struct storage_volume *volume,const struct storage_node *node,const struct storage_node *next);
    int (*allocate_one)(struct storage_volume *volume,uint32_t *cluster);
    int (*times)(struct storage_volume *volume,const struct storage_node *node,struct storage_inode_times *out);
    int (*access)(struct storage_volume *volume,const struct storage_node *node,int64_t seconds,int64_t *actual);
    int (*new_file)(struct storage_volume *volume,const struct storage_node *parent,const char *name,struct storage_node *out);
    int (*drop_new)(struct storage_volume *volume,const struct storage_node *parent,const char *name,const struct storage_node *node);
};
#endif
