/* SPDX-License-Identifier: Apache-2.0 */
#include <linux/mount.h>
#include <reliefnt/pseudo_fs.h>
#include <reliefnt/permissions.h>
#include <reliefnt/inventory.h>
#include <reliefnt/module.h>
#include "internal.h"
#include <linux/errno.h>

/** @brief Retain only the actual module instance represented by this key.
 * @param name Borrowed intrinsic generated sysfs pathname.
 * @param out Receives an owned non-code identity, NULL for static inventory.
 * @param magic Required output, always false; sysfs links are ordinary links.
 * @return Zero or ENOENT; caller owns execution and releases the token. For
 * holders links the importing instance is authoritative: its dependency
 * keeps the provider instance stable. No module code or locks are retained.
 */
static int sysfs_key_identity(const char *name,void **out,bool *magic)
{
    *out=NULL;*magic=false;
    if(__builtin_strncmp(name,"/sys/module/",12))return 0;
    const char *part=name+12;size_t length=0;
    while(part[length] && part[length]!='/')++length;
    if(!__builtin_strncmp(part+length,"/holders/",9))part+=length+9;
    length=0;while(part[length] && part[length]!='/')++length;
    if(!length || length>=MODULE_NAME_LEN)return -ENOENT;
    char module_name[MODULE_NAME_LEN];__builtin_memcpy(module_name,part,length);module_name[length]=0;
    if(module_parameter_builtin_exists(module_name))return 0;
#ifdef CONFIG_MODULES
    *out=module_identity_get(module_name);return *out ? 0 : -ENOENT;
#else
    return -ENOENT;
#endif
}
/** @brief Reject generated keys belonging to an unloaded module instance.
 * @param identity Borrowed retained instance token, never module image memory.
 * @return True only while that original runtime exists. No callback/code pin.
 */
static bool sysfs_key_alive(void *identity)
{
#ifdef CONFIG_MODULES
    return module_identity_alive(identity);
#else
    return !identity;
#endif
}
/** @brief Release a generated sysfs inode's non-code lifetime token.
 * @param identity Owned module-instance token, nullable; consumed once.
 * Final token free is outside the registry lock, without device or FS I/O.
 */
static void sysfs_key_put(void *identity)
{
#ifdef CONFIG_MODULES
    module_identity_put(identity);
#else
    (void)identity;
#endif
}

/** @brief Construct a detached sysfs mount with descriptor-authorized stores.
 * @param out Required empty output, receiving one owned mount on success.
 * @return Zero or negative errno with balanced generated inode identities.
 * Execution-owned task context; inventory stays immutable, while parameter
 * modes and setters follow their owning module descriptors.
 */
int sysfs_create_vfs_mount(struct vfs_mount **out)
{
    static const struct vfs_pseudo_provider provider={.root="/sys",.filesystem="sysfs",.lookup=sysfs_lookup,
        .read=sysfs_read,.write=sysfs_parameter_write,.truncate=sysfs_parameter_truncate,
        .readdir=sysfs_readdir,.readlink=sysfs_readlink,.permissions=sysfs_parameter_permissions,
        .get_identity=sysfs_key_identity,.identity_alive=sysfs_key_alive,.put_identity=sysfs_key_put
    };
    return vfs_pseudo_mount_new(&provider,out);
}
