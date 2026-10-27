/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MEMORY_BLOCK_FIXTURE_H
#define RELIEFNT_MEMORY_BLOCK_FIXTURE_H
/* Filesystem tests use the real block registry/transfer path with an owned
 * synchronous memory transport. No RAM pointer lives in a production volume. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
struct test_memory_disk {
    struct test_memory_disk *next;
    struct storage_volume *volume;
    struct block_device *identity;
    struct block_device_registration registration;
    uint8_t *bytes;
    uint64_t length, failure_lba;
};
static struct test_memory_disk *test_memory_disks;
static uint32_t test_memory_minor;

/** @brief Transfer real fixture sectors, optionally injecting a transport error.
 * @param context Owned synchronous disk; no user or DMA buffer is retained.
 * @param first First sector. @param count Nonzero sectors. @param buffer Current output.
 * @return Zero or EIO; the real registry has already checked complete geometry.
 */
static int test_memory_read(void *context,uint64_t first,uint32_t count,void *buffer)
{
    struct test_memory_disk *disk=context;
    if(first>=disk->failure_lba || count>disk->failure_lba-first)return -EIO;
    memcpy(buffer,disk->bytes+first*512,(size_t)count*512);return 0;
}
/** @brief Write through the fixture's independent registered backing.
 * @param context Owned disk. @param first First sector. @param count Sector count.
 * @param buffer Current input, never retained. @return Zero or injected EIO.
 */
static int test_memory_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    struct test_memory_disk *disk=context;
    if(first>=disk->failure_lba || count>disk->failure_lba-first)return -EIO;
    memcpy(disk->bytes+first*512,buffer,(size_t)count*512);return 0;
}
/** @brief Bind caller-owned image bytes through an actual immutable registration.
 * @param volume Empty-token native fixture instance.
 * @param bytes Allocated sector-aligned image, transferred until detach.
 * @param length Complete image bytes. @return None; malformed setup aborts the test.
 */
static inline void test_memory_bind(struct storage_volume *volume,uint8_t *bytes,uint64_t length)
{
    assert(volume && bytes && length && !(length%512) && !volume->device_ref.device);
    for(struct test_memory_disk *at=test_memory_disks;at;at=at->next)assert(at->volume!=volume);
    struct test_memory_disk *disk=calloc(1,sizeof(*disk));assert(disk);
    static const struct block_device_operations operations={.read=test_memory_read,.write=test_memory_write};
    uint32_t minor=test_memory_minor++;
    *disk=(struct test_memory_disk){.volume=volume,.bytes=bytes,.length=length,.failure_lba=UINT64_MAX,
        .registration={.operations=&operations,.context=disk,.info={.device=block_device_make_number(1,minor),
            .blocks=length/512,.block_size=512,.max_blocks=128,.flags=BLOCK_DEVICE_WRITE_THROUGH,
            .transport=BLOCK_TRANSPORT_MEMORY,.controller=(uint16_t)minor}}};
    assert(!register_block_device(&disk->registration));
    assert(!block_device_get(disk->registration.info.device,&volume->device_ref));
    disk->identity=volume->device_ref.device;disk->next=test_memory_disks;test_memory_disks=disk;
    volume->kind=STORAGE_VOLUME_BLOCK;volume->transport=STORAGE_TRANSPORT_MEMORY;
    volume->source_disk_id=UINT32_MAX;
    volume->esp_device=volume->ext_device=volume->exfat_device=disk->registration.info.device;
}
/** @brief Find backing for an owned instance or its temporary borrowed alias.
 * @param volume Instance whose owning block token remains live.
 * @return Borrowed fixture backing; a nonfixture identity aborts.
 */
static inline struct test_memory_disk *test_memory_disk(const struct storage_volume *volume)
{
    for(struct test_memory_disk *at=test_memory_disks;at;at=at->next)
        if(at->identity==volume->device_ref.device)return at;
    abort();
}
/** @brief Access fixture media solely for the independent disk-image oracle.
 * @param volume Borrowed registered fixture instance. @return Borrowed bytes.
 */
static inline uint8_t *test_memory_bytes(const struct storage_volume *volume)
{return test_memory_disk(volume)->bytes;}
/** @brief Snapshot the fixture image length without changing registered geometry.
 * @param volume Borrowed registered fixture. @return Complete bytes.
 */
static inline uint64_t test_memory_length(const struct storage_volume *volume)
{return test_memory_disk(volume)->length;}
/** @brief Retire the actual registration after native inode/cache users drain.
 * @param volume Owning instance; an automatically retired token may be empty.
 * @return Owned image bytes for the caller to free after any other test oracle.
 */
static inline uint8_t *test_memory_detach(struct storage_volume *volume)
{
    struct test_memory_disk **at=&test_memory_disks;
    while(*at && (*at)->volume!=volume)at=&(*at)->next;
    assert(*at);struct test_memory_disk *disk=*at;
    assert(!volume->device_ref.device || volume->device_ref.device==disk->identity);
    block_device_put(&volume->device_ref);
    assert(!unregister_block_device(&disk->registration));
    *at=disk->next;uint8_t *bytes=disk->bytes;free(disk);return bytes;
}
/** @brief Release quiescent fixture transport and its owned image allocation.
 * @param volume Owning native instance, with all mounted/inode users drained.
 */
static inline void test_memory_release(struct storage_volume *volume)
{free(test_memory_detach(volume));}
#endif
