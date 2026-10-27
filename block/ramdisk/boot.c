/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/boot_block.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/paging.h>
#include <linux/errno.h>

struct boot_image {uint8_t *bytes;uint64_t physical,length;};
static struct boot_image boot_image;
static struct block_device_registration boot_registration;
static bool registered;

/** @brief Validate a complete synchronous RAM transport extent without overflow.
 * @param image Borrowed permanent backing. @param first First logical sector.
 * @param count Nonzero sector count. @param buffer Readable/writable caller extent.
 * @return Zero or EINVAL; pure, no ownership transfer/lock/I/O or payload access.
 */
static int boot_extent(const struct boot_image *image,uint64_t first,uint32_t count,const void *buffer)
{
    uint64_t blocks=image->length/512;
    return !buffer || !count || first>=blocks || count>blocks-first ? -EINVAL : 0;
}
/** @brief Read exact requested bytes from reserved boot-image backing.
 * @param context Permanent boot_image, borrowed and pinned by the registry.
 * @param first First sector. @param count Sector count.
 * @param buffer Writable count*512-byte stable kernel destination.
 * @return Zero or EINVAL. Caller owns storage execution; no provider/registry
 * lock spans this bounded copy. Source reservation remains permanent.
 */
static int boot_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    struct boot_image *image=context;int rc=boot_extent(image,first,count,buffer);
    if(!rc)__builtin_memcpy(buffer,image->bytes+first*512,(size_t)count*512);
    return rc;
}
/** @brief Store exact requested bytes in coherent ephemeral boot-image backing.
 * @param context Permanent boot_image, borrowed and pinned by the registry.
 * @param first First sector. @param count Sector count.
 * @param buffer Readable count*512-byte stable kernel source.
 * @return Zero or EINVAL. Caller owns storage execution; no lock or hardware I/O
 * occurs. Only the already reserved memory image changes, never boot media.
 */
static int boot_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    struct boot_image *image=context;int rc=boot_extent(image,first,count,buffer);
    if(!rc)__builtin_memcpy(image->bytes+first*512,buffer,(size_t)count*512);
    return rc;
}
/** @brief Publish reserved memory as one standard Linux RAM block identity.
 * @param physical Reserved physical start, not a CPU address.
 * @param length Sector-aligned bytes, held through shutdown.
 * @param device Required dev_t output, zero on failure.
 * @return Zero or EINVAL/ENOEXEC/EBUSY/native registry errno. BSP exclusive
 * setup before external storage users; no full-image read/allocation/copy.
 * Failed registration clears provisional backing; repeated identical binding
 * preserves the exact registration and all existing references.
 */
int block_boot_image_bind(uint64_t physical,uint64_t length,uint64_t *device)
{
    if(!device)return -EINVAL;*device=0;
    if(!physical || length<512 || length%512)return -ENOEXEC;
    if(registered) {
        if(boot_image.physical!=physical || boot_image.length!=length)return -EBUSY;
        *device=boot_registration.info.device;return 0;
    }
    if(physical>UINT64_MAX-length || !paging_kernel_direct_map_range(physical,length))return -EINVAL;
    uint8_t *mapping=paging_kernel_direct_map(physical);if(!mapping)return -EINVAL;
    static const struct block_device_operations ops={.read=boot_read,.write=boot_write};
    boot_image=(struct boot_image){mapping,physical,length};
    boot_registration=(struct block_device_registration){.info={
        .device=block_device_make_number(1,0),.blocks=length/512,.block_size=512,.max_blocks=128,
        .flags=BLOCK_DEVICE_WRITE_THROUGH,.transport=BLOCK_TRANSPORT_MEMORY},
        .operations=&ops,.context=&boot_image};
    static const char model[]="Multiboot installer root";
    __builtin_memcpy(boot_registration.info.model,model,sizeof(model));
    int rc=register_block_device(&boot_registration);
    if(rc){boot_image=(struct boot_image){0};boot_registration=(struct block_device_registration){0};return rc;}
    registered=true;*device=boot_registration.info.device;return 0;
}
/** @brief Classify a unique filesystem image separately from the newc module.
 * @param boot Borrowed handoff, nullable. @param device Required dev_t output.
 * @return One bound, zero absent, or EINVAL/EEXIST/ENOEXEC/native registration
 * errno. BSP exclusive setup after mm reservation; no source payload reads,
 * disk I/O, filesystem parser invocation or ownership transfer on failure.
 */
int block_boot_image_register(const struct boot_info *boot,uint64_t *device)
{
    if(!device)return -EINVAL;*device=0;if(!boot)return 0;
    if(boot->module_count>sizeof(boot->modules)/sizeof(boot->modules[0]))return -EINVAL;
    const struct boot_module *selected=NULL;
    for(uint32_t i=0;i<boot->module_count;++i) {
        const struct boot_module *module=&boot->modules[i];
        if(!module->name || (__builtin_strcmp(module->name,"reliefos-installer-root") &&
                            __builtin_strcmp(module->name,"leonos-installer-root")))continue;
        if(selected)return -EEXIST;
        selected=module;
    }
    if(!selected)return 0;
    if(selected->end<=selected->start)return -ENOEXEC;
    int rc=block_boot_image_bind(selected->start,selected->end-selected->start,device);
    return rc ? rc : 1;
}
