/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/devfs.h>
#include <linux/errno.h>

/** @brief Search one native provider directory by type and device number.
 * @param parent Borrowed intrinsic provider directory name, not a namespace path.
 * @param type Requested S_IFCHR/S_IFBLK. @param device Encoded Linux rdev.
 * @param children Permit the provider's one direct device subdirectory level.
 * @param out Required canonical metadata output, valid only on success.
 * @return Zero, ENXIO absent, or validation/enumeration errno. Existing native
 * nodes occupy the provider root or one of snd/input/pts/dri; disk-by-ID symlinks
 * are not followed. Execution ownership stabilizes enumeration; no ownership
 * transfer, global name resolution or graph/module lock across metadata I/O.
 */
static int devfs_find_device(const char *parent,uint32_t type,uint64_t device,bool children,struct storage_node *out)
{
    struct storage_node directory;int rc=storage_devfs_lookup(parent,&directory);if(rc)return rc;
    uint64_t cursor=0;struct reliefos_dir_entry entry;
    while((rc=storage_devfs_readdir_node(&directory,&cursor,&entry))>0) {
        if(!__builtin_strcmp(entry.name,".") || !__builtin_strcmp(entry.name,".."))continue;
        size_t base=__builtin_strlen(parent),length=0;
        while(length<sizeof(entry.name) && entry.name[length] && entry.name[length]!='/')++length;
        if(!length || length==sizeof(entry.name) || entry.name[length])return -EIO;
        char path[RELIEFOS_FS_PATH_LEN];
        if(base+length+2>sizeof(path))return -ENAMETOOLONG;
        __builtin_memcpy(path,parent,base);path[base]='/';__builtin_memcpy(path+base+1,entry.name,length+1);
        struct storage_node node;rc=storage_devfs_lookup(path,&node);
        if(rc==-ENOENT)continue;
        if(rc)return rc;
        if(children && node.type==RELIEFOS_FS_TYPE_DIR) {
            rc=devfs_find_device(path,type,device,false,out);
            if(!rc)return 0;
            if(rc!=-ENXIO && rc!=-ENOENT)return rc;
        }
        if(node.type!=RELIEFOS_FS_TYPE_DEVICE)continue;
        uint32_t actual=node.flags&STORAGE_NODE_FLAG_DEV_BLOCK ? LINUX_S_IFBLK : LINUX_S_IFCHR;
        if(actual!=type)continue;
        struct linux_stat_abi status={.st_mode=actual};devfs_stat_identity(&status,&node);
        if(status.st_rdev==device){*out=node;return 0;}
    }
    return rc<0 ? rc : -ENXIO;
}
/** @brief Resolve an actual device identity independent of mount/inode names.
 * @param type S_IFCHR/S_IFBLK. @param device Encoded Linux rdev.
 * @param out Required canonical metadata output, valid only on success.
 * @return Zero, EINVAL, ENXIO or provider metadata errno. Caller owns execution
 * and performs DAC/nodev/driver admission separately. No global namespace
 * routing, ownership transfer or graph/module lock across metadata callbacks.
 */
int devfs_lookup_rdev(uint32_t type,uint64_t device,struct storage_node *out)
{
    if(!out || (type!=LINUX_S_IFCHR && type!=LINUX_S_IFBLK))return -EINVAL;
    return devfs_find_device("/dev",type,device,true,out);
}
