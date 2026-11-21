/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/boot_block.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/paging.h>
#include <linux/errno.h>

enum { BOOT_IMAGE_ROOT, BOOT_IMAGE_PAYLOAD, BOOT_IMAGE_COUNT };

struct boot_image {
    uint8_t *bytes;
    uint64_t physical;
    uint64_t source_length;
    uint64_t length;
    bool read_only;
};

struct boot_image_slot {
    struct boot_image image;
    struct block_device_registration registration;
    bool registered;
};

static struct boot_image_slot boot_slots[BOOT_IMAGE_COUNT];

static bool boot_overlap(uint64_t start,uint64_t end,uint64_t other_start,uint64_t other_end)
{ return start<other_end && other_start<end; }

static int boot_validate_slot(unsigned slot,uint64_t physical,uint64_t length)
{
    if(!physical || length<512u || length>UINT64_MAX-511u || physical>UINT64_MAX-length ||
       (slot==BOOT_IMAGE_ROOT && length%512u))return -ENOEXEC;
    const struct boot_image_slot *entry=&boot_slots[slot];
    if(entry->registered && (entry->image.physical!=physical || entry->image.source_length!=length))
        return -EBUSY;
    const struct boot_image_slot *other=&boot_slots[slot ^ 1u];
    if(other->registered && boot_overlap(physical,physical+length,other->image.physical,
       other->image.physical+other->image.source_length))return -ENOEXEC;
    return entry->registered || (paging_kernel_direct_map_range(physical,length) &&
        paging_kernel_direct_map(physical)) ? 0 : -EINVAL;
}

/** @brief Validate one request against a sector-padded boot image. */
static int boot_extent(const struct boot_image *image,uint64_t first,uint32_t count,const void *buffer)
{
    uint64_t blocks=image->length/512u;
    return !buffer || !count || first>=blocks || count>blocks-first ? -EINVAL : 0;
}

/** @brief Read module bytes and zero-fill the last partial device sector. */
static int boot_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    struct boot_image *image=context;
    int rc=boot_extent(image,first,count,buffer);
    if(rc)return rc;
    uint64_t offset=first*512u;
    size_t length=(size_t)count*512u;
    if(offset<image->source_length) {
        size_t available=(size_t)(image->source_length-offset);
        size_t copy=available<length ? available : length;
        __builtin_memcpy(buffer,image->bytes+offset,copy);
        if(copy<length)__builtin_memset((uint8_t *)buffer+copy,0,length-copy);
    } else {
        __builtin_memset(buffer,0,length);
    }
    return 0;
}

/** @brief Update the ephemeral writable installer root, never the boot medium. */
static int boot_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    struct boot_image *image=context;
    int rc=boot_extent(image,first,count,buffer);
    if(rc)return rc;
    if(image->read_only)return -EROFS;
    uint64_t offset=first*512u;
    size_t length=(size_t)count*512u;
    if(offset>image->source_length || length>image->source_length-offset)return -EINVAL;
    __builtin_memcpy(image->bytes+offset,buffer,length);
    return 0;
}

/** @brief Publish one reserved Multiboot image as a Linux RAM block device.
 * @param slot Fixed standard ramN slot. @param physical Reserved source address.
 * @param length Exact module byte count. @param device Receives the dev_t.
 * @return Zero or negative validation/registration errno. Exclusive early boot;
 * backing remains reserved, no full-image allocation or copy is performed.
 */
static int block_boot_image_bind_slot(unsigned slot,uint64_t physical,uint64_t length,uint64_t *device)
{
    if(!device || slot>=BOOT_IMAGE_COUNT)return -EINVAL;
    *device=0;
    int rc=boot_validate_slot(slot,physical,length);
    if(rc)return rc;
    uint64_t padded=(length+511u)&~UINT64_C(511);
    struct boot_image_slot *entry=&boot_slots[slot];
    if(entry->registered) {
        *device=entry->registration.info.device;
        return 0;
    }
    uint8_t *mapping=paging_kernel_direct_map(physical);

    static const struct block_device_operations root_ops={.read=boot_read,.write=boot_write};
    static const struct block_device_operations payload_ops={.read=boot_read};
    bool read_only=slot==BOOT_IMAGE_PAYLOAD;
    entry->image=(struct boot_image){
        .bytes=mapping,.physical=physical,.source_length=length,.length=padded,.read_only=read_only
    };
    entry->registration=(struct block_device_registration){
        .info={
            .device=block_device_make_number(1u,slot),.blocks=padded/512u,.block_size=512u,
            .max_blocks=128u,.flags=BLOCK_DEVICE_WRITE_THROUGH|(read_only?BLOCK_DEVICE_READ_ONLY:0),
            .transport=BLOCK_TRANSPORT_MEMORY,.controller=(uint16_t)slot
        },
        .operations=read_only?&payload_ops:&root_ops,.context=&entry->image
    };
    const char *model=read_only ? "Multiboot installer payload" : "Multiboot installer root";
    size_t model_size=__builtin_strlen(model)+1u;
    __builtin_memcpy(entry->registration.info.model,model,model_size);
    rc=register_block_device(&entry->registration);
    if(rc) {
        *entry=(struct boot_image_slot){0};
        return rc;
    }
    entry->registered=true;
    *device=entry->registration.info.device;
    return 0;
}

/** @brief Bind the writable installer-root image to standard /dev/ram0. */
int block_boot_image_bind(uint64_t physical,uint64_t length,uint64_t *device)
{ return block_boot_image_bind_slot(BOOT_IMAGE_ROOT,physical,length,device); }

static bool boot_root_name(const char *name)
{ return name && (!__builtin_strcmp(name,"reliefos-installer-root") ||
                  !__builtin_strcmp(name,"leonos-installer-root")); }

/** @brief Register unique installer-root and optional SquashFS Multiboot modules.
 * @param boot Borrowed decoded handoff, NULL means no source.
 * @param device Receives ram0 when a root module is present, otherwise zero.
 * @return Number of bound modules or negative validation/registration errno.
 * Duplicate names, empty ranges and over-limit module lists fail closed.
 */
int block_boot_image_register(const struct boot_info *boot,uint64_t *device)
{
    if(!device)return -EINVAL;
    *device=0;
    if(!boot)return 0;
    if(boot->module_count>sizeof(boot->modules)/sizeof(boot->modules[0]))return -EINVAL;
    const struct boot_module *selected[BOOT_IMAGE_COUNT]={0};
    for(uint32_t i=0;i<boot->module_count;++i) {
        const struct boot_module *module=&boot->modules[i];
        unsigned slot;
        if(boot_root_name(module->name))slot=BOOT_IMAGE_ROOT;
        else if(module->name && !__builtin_strcmp(module->name,"reliefos-installer-payload"))
            slot=BOOT_IMAGE_PAYLOAD;
        else continue;
        if(selected[slot])return -EEXIST;
        if(module->end<=module->start)return -ENOEXEC;
        selected[slot]=module;
    }
    for(unsigned slot=0;slot<BOOT_IMAGE_COUNT;++slot) {
        const struct boot_module *module=selected[slot];
        if(!module)continue;
        int rc=boot_validate_slot(slot,module->start,module->end-module->start);
        if(rc)return rc;
        for(uint32_t i=0;i<boot->module_count;++i) {
            const struct boot_module *other=&boot->modules[i];
            if(other!=module && other->start<other->end &&
               boot_overlap(module->start,module->end,other->start,other->end))return -ENOEXEC;
        }
    }
    unsigned count=0;
    uint64_t root_device=0;
    for(unsigned slot=0;slot<BOOT_IMAGE_COUNT;++slot) {
        if(!selected[slot])continue;
        uint64_t image_device=0;
        int rc=block_boot_image_bind_slot(slot,selected[slot]->start,
            selected[slot]->end-selected[slot]->start,&image_device);
        if(rc)return rc;
        if(slot==BOOT_IMAGE_ROOT)root_device=image_device;
        ++count;
    }
    *device=root_device;
    return (int)count;
}
