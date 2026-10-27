/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DEVFS_INTERNAL_H
#define RELIEFNT_DEVFS_INTERNAL_H
#include <reliefnt/storage.h>
#define DEVFS_SHM_DIRECTORY 0x53484du
/* Legacy pathname callers normalize first and consult this selector once.
 * The mounted backend itself neither checks this flag nor routes volumes. */
int storage_devfs_lookup(const char *path,struct storage_node *out);
int storage_devfs_readdir_node(const struct storage_node *node,uint64_t *cursor,
                                struct reliefos_dir_entry *entry);
#endif
