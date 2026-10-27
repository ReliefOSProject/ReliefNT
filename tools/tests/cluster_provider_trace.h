/* SPDX-License-Identifier: Apache-2.0 */
/* Test the selected real FAT/exFAT primitive provider, not a copied codec. */
#include "../../fs/cluster.h"
static const struct storage_cluster_ops *cluster_provider;
static unsigned cluster_links,cluster_frees,cluster_collects,cluster_ios;
static unsigned cluster_publishes,cluster_allocations,cluster_times,cluster_accesses;
static int traced_cluster_link(struct storage_volume *volume,uint32_t cluster,uint32_t next)
{ ++cluster_links;return cluster_provider->link(volume,cluster,next); }
static int traced_cluster_free(struct storage_volume *volume,uint32_t cluster)
{ ++cluster_frees;return cluster_provider->free_one(volume,cluster); }
static int traced_cluster_collect(struct storage_volume *volume,const struct storage_node *node,uint32_t *count)
{ ++cluster_collects;return cluster_provider->collect(volume,node,count); }
static int traced_cluster_io(struct storage_volume *volume,uint32_t cluster,void *bytes,bool write)
{ ++cluster_ios;return cluster_provider->io(volume,cluster,bytes,write); }
static int traced_cluster_publish(struct storage_volume *volume,const struct storage_node *node,
                                  const struct storage_node *next)
{ ++cluster_publishes;return cluster_provider->publish(volume,node,next); }
static int traced_cluster_allocate(struct storage_volume *volume,uint32_t *cluster)
{ ++cluster_allocations;return cluster_provider->allocate_one(volume,cluster); }
static int traced_cluster_times(struct storage_volume *volume,const struct storage_node *node,
                                struct storage_inode_times *out)
{ ++cluster_times;return cluster_provider->times(volume,node,out); }
static int traced_cluster_access(struct storage_volume *volume,const struct storage_node *node,
                                 int64_t seconds,int64_t *actual)
{ ++cluster_accesses;return cluster_provider->access(volume,node,seconds,actual); }
static void cluster_provider_trace(struct storage_cluster_ops *table,const struct storage_cluster_ops *native)
{
    *table=*native;cluster_provider=native;
    table->link=traced_cluster_link;table->free_one=traced_cluster_free;
    table->collect=traced_cluster_collect;table->io=traced_cluster_io;
    table->publish=traced_cluster_publish;table->allocate_one=traced_cluster_allocate;
    table->times=traced_cluster_times;table->access=traced_cluster_access;
}
static void cluster_provider_checked(void)
{
    assert(cluster_links && cluster_frees && cluster_collects && cluster_ios);
    assert(cluster_publishes && cluster_allocations && cluster_times && cluster_accesses);
}
