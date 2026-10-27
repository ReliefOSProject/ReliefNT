/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/permissions.h>
#include <linux/errno.h>

/** @brief Decode a generated parameter key without global pathname routing.
 * @param path Borrowed intrinsic sysfs key.
 * @param module Required fixed-size module output.
 * @param name Required borrowed local-name output.
 * @return True for one parameter component; no allocation, locks or pins.
 */
bool sysfs_parameter_key(const char *path,char module[MODULE_NAME_LEN],const char **name)
{
    if(__builtin_strncmp(path,"/sys/module/",12))return false;
    const char *part=path+12;size_t length=0;while(part[length] && part[length]!='/')++length;
    if(!length || length>=MODULE_NAME_LEN || __builtin_strncmp(part+length,"/parameters/",12))return false;
    *name=part+length+12;
    if(!**name)return false;
    for(const char *p=*name;*p;++p)if(*p=='/')return false;
    __builtin_memcpy(module,part,length);module[length]=0;return true;
}
/** @brief Snapshot one exact visible attribute without retaining its image.
 * @param path Intrinsic generated key.
 * @param out Required descriptor copy.
 * @return Zero or negative errno. Execution ownership keeps lookup stable;
 * each registry query temporarily pins and then releases the LIVE runtime.
 */
static int sysfs_parameter_info(const char *path,struct module_parameter_info *out)
{
    char module[MODULE_NAME_LEN];const char *name;
    if(!sysfs_parameter_key(path,module,&name))return -ENOENT;
    for(size_t i=0;;++i) {
        int rc=module_parameter_at(module,i,out);if(rc)return rc;
        if(!__builtin_strcmp(out->name,name))return 0;
    }
}
/** @brief Apply one write only to a descriptor-authorized parameter.
 * @param path Retained intrinsic key, checked for original-instance liveness.
 * @param offset Byte position, ignored as with sysfs whole-value stores.
 * @param buffer Validated kernel payload.
 * @param length Byte count.
 * @param written Required byte result.
 * @return Zero or negative errno; execution ownership required. Each store
 * has a balanced LIVE image pin, with no registry lock across allocation.
 */
int sysfs_parameter_write(const char *path,uint64_t offset,const void *buffer,uint32_t length,uint32_t *written)
{
    (void)offset;*written=0;char module[MODULE_NAME_LEN];const char *name;
    if(!sysfs_parameter_key(path,module,&name))return -EROFS;
    return module_parameter_write(module,name,buffer,length,written);
}
/** @brief Preserve parameters across open-time O_TRUNC.
 * @param path Retained intrinsic key with checked instance liveness.
 * @param length Requested size, zero for ordinary shell redirection.
 * @return Zero for an existing writable attribute at zero length, otherwise
 * negative errno. Execution ownership required; no mutation or allocation.
 */
int sysfs_parameter_truncate(const char *path,uint64_t length)
{
    struct module_parameter_info info;int rc=sysfs_parameter_info(path,&info);
    if(rc)return rc==-ENOENT ? -EROFS : rc;
    if(!(info.permissions&0222u))return -EIO;
    return length ? -EINVAL : 0;
}
/** @brief Expose exact declared parameter permissions and immutable metadata.
 * @param path Intrinsic retained key.
 * @param node Retained generated inode.
 * @param value Required metadata output.
 * @param write Metadata setter request, always rejected.
 * @return Zero or negative errno. Execution-owned task context; no pins or
 * borrowed image pointers escape and inventory modes keep their old policy.
 */
int sysfs_parameter_permissions(const char *path,const struct storage_node *node,struct reliefos_permissions *value,bool write)
{
    if(write)return -EROFS;
    char module[MODULE_NAME_LEN];const char *name;
    if(!sysfs_parameter_key(path,module,&name))return fs_virtual_permissions(path,node,value,false);
    struct module_parameter_info info;int rc=sysfs_parameter_info(path,&info);
    if(!rc)*value=(struct reliefos_permissions){info.permissions,0,0};
    return rc;
}
