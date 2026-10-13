/*
 * ReliefOS storage facade.
 *
 * The implementation is split by responsibility: filesystem families live
 * under fs/, storage controllers under drivers/storage/, and the remaining
 * storage-module glue under storage/.  These modules are included into one
 * translation unit deliberately: the storage cache, active-volume selector,
 * and transport scratch buffers remain private to the storage subsystem
 * while each feature has a focused source file.
 */
#include "../block/storage_internal.h"
#include <reliefnt/tmpfs.h>
#include "../block/storage_state.c"
#include "../../fs/storage_ext2_cache.c"
#include "../storage/ide/storage_ide.c"
#include "../storage/ahci/storage_ahci.c"
#include "../storage/nvme/storage_nvme.c"
#include "../block/storage_block.c"
#include "../../fs/storage_ext4_format.c"
#include "../../fs/storage_ext4_checksum.c"
#include "../../fs/storage_ext4_cache.c"
#include "../../fs/storage_ext4_alloc.c"
#include "../../fs/storage_ext4_extent.c"
#include "../../fs/storage_ext4_ops.c"
#include "../../fs/storage_ext4_xattr.c"
#include "../../fs/storage_ext4_journal.c"
#include "../../fs/storage_ext4_dir.c"
#include "../block/storage_sync.c"
#include "../../fs/storage_iso.c"
#include "../../fs/storage_fat32.c"
#include "../../fs/storage_inode.c"
#include "../../fs/storage_ext4_vfs.c"
#include "../../fs/storage_ext2.c"
#include "../../fs/storage_exfat.c"
#include "../../fs/storage_ext4_mount.c"
#include "../../fs/storage_mount.c"
#include "../../kernel/reliefnt/audio/storage_audio.c"
#include "../../fs/storage_vfs.c"
#include "../../fs/storage_mounts.c"
#include "../../fs/storage_tmpfs.c"
#include "../../fs/storage_permissions.c"
#include "../../fs/storage_sidecar.c"
#include "../../fs/storage_statfs.c"
#include "../block/storage_installer.c"
#include "../block/storage_disk.c"
#include "../block/storage_identity.c"
