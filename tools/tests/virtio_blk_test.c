/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <string.h>
#include <linux/errno.h>
#include "../../drivers/storage/virtio_blk/internal.h"
#include <reliefnt/console.h>
#include <reliefnt/time.h>

static struct virtio_blk disk;
static _Alignas(4096) unsigned char ring[8192], backing[135168];
static _Alignas(4) unsigned char common[56];
static uint32_t config[8];
static uint16_t notify;
static bool may_yield, complete_on_poll, work_clock_rolls_back;
static uint32_t pid = 17, expected_type, expected_bytes;
static uint64_t expected_sector, now, ticks;
static unsigned clock_samples, clock_complete_after;
static unsigned reset_count, submitted;
static int reset_result;

void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_unlock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }
void console_log(enum console_log_level level, const char *fmt, ...) { (void)level; (void)fmt; }
bool storage_async_can_yield(void) { return may_yield; }
uint32_t sched_current_pid(void) { return pid; }
uint64_t time_uptime_us(void) { return now; }
uint64_t time_ticks(void) { return ticks; }
int virtio_pci_reset(struct virtio_pci *d)
{
    ++reset_count;
    if (reset_result) return reset_result;
    common[20] = 0;
    d->live = false;
    return 0;
}

static void complete(uint8_t status, uint32_t length, bool bad_id)
{
    struct virtio_queue *q = &disk.queue;
    assert((uint16_t)(q->available[1] - q->used_header[1]) == 1);
    uint16_t head = q->available[2 + (q->used_header[1] & (q->size - 1))];
    struct virtio_descriptor header = q->descriptors[head];
    assert(header.length == 16 && header.flags == 1);
    struct {uint32_t type, reserved; uint64_t sector;} wire;
    memcpy(&wire, (void *)(uintptr_t)header.address, sizeof(wire));
    assert(wire.type == expected_type && !wire.reserved && wire.sector == expected_sector);
    uint16_t next = header.next;
    if (expected_bytes) {
        struct virtio_descriptor data = q->descriptors[next];
        assert(data.length == expected_bytes && data.flags == (expected_type ? 1 : 3));
        if (!expected_type)
            memset((void *)(uintptr_t)data.address, 0x45, data.length);
        else
            for (uint32_t i = 0; i < data.length; ++i)
                assert(((const unsigned char *)(uintptr_t)data.address)[i] == 0xa7);
        next = data.next;
    }
    struct virtio_descriptor tail = q->descriptors[next];
    assert(tail.length == 1 && tail.flags == 2);
    *(volatile uint8_t *)(uintptr_t)tail.address = status;
    q->used_ring[q->used_header[1] & (q->size - 1)] =
        (struct virtio_used_element){bad_id ? q->size : head, length};
    ++q->used_header[1];
    ++submitted;
}

uint64_t time_work_counter_us(void)
{
    ++clock_samples;
    if ((complete_on_poll || (clock_complete_after && clock_samples == clock_complete_after)) &&
        disk.queue.pending &&
        disk.queue.available[1] != disk.queue.used_header[1])
        complete(0, expected_type == 0 ? expected_bytes + 1 : 1, false);
    now = work_clock_rolls_back && clock_samples == 2 ? 1 : now + 1000;
    return now;
}

static void prepare(uint64_t features, uint32_t block_size, uint32_t sectors)
{
    memset(&disk, 0, sizeof(disk));
    memset(common, 0, sizeof(common));
    memset(config, 0, sizeof(config));
    common[20] = 15;
    config[0] = sectors;
    config[3] = 1;
    config[5] = block_size;
    disk.transport = (struct virtio_pci){.common=common,.config=(void *)config,
        .features=features,.live=true,.mappings[3].length=sizeof(config)};
    disk.dma = (struct dma_buffer){.cpu=backing,.physical=(uintptr_t)backing,.capacity=sizeof(backing)};
    const struct dma_buffer qmem = {.cpu=ring,.physical=(uintptr_t)ring,.capacity=sizeof(ring)};
    disk.queue.memory = qmem;
    assert(!virtio_queue_bind(&disk.queue, ring, (uintptr_t)ring, sizeof(ring), 8));
    disk.queue.notify = &notify;
    assert(!virtio_blk_configure(&disk));
    disk.online = true;
    may_yield = complete_on_poll = false;
    reset_result = reset_count = submitted = 0;
    now = ticks = 0;
    clock_samples = clock_complete_after = 0;
    work_clock_rolls_back = false;
}

static void expect(uint32_t type, uint64_t sector, uint32_t bytes)
{ expected_type=type; expected_sector=sector; expected_bytes=bytes; }

int main(void)
{
    const struct block_device_operations *ops = &virtio_blk_operations;
    unsigned char buf[4096];

    prepare(0, 0, 1024); /* BLK_SIZE absent: use the protocol default. */
    assert(disk.registration.info.block_size == 512 && disk.registration.info.blocks == 1024);
    assert(disk.registration.info.flags & BLOCK_DEVICE_WRITE_THROUGH);
    assert(!ops->flush(&disk) && !disk.queue.available[1]);

    prepare((1ull << 6) | (1ull << 9), 4096, 1024);
    assert(disk.registration.info.block_size == 4096 && disk.registration.info.blocks == 128);
    assert(!(disk.registration.info.flags & BLOCK_DEVICE_WRITE_THROUGH));
    assert(ops->read(&disk, 128, 1, buf) == -EINVAL && !disk.queue.available[1]);
    complete_on_poll = true;
    expect(0, 24, sizeof(buf));
    assert(!ops->read(&disk, 3, 1, buf));
    for (unsigned i = 0; i < sizeof(buf); ++i) assert(buf[i] == 0x45);
    memset(buf, 0xa7, sizeof(buf));
    expect(1, 32, sizeof(buf));
    assert(!ops->write(&disk, 4, 1, buf));
    expect(4, 0, 0);
    assert(!ops->flush(&disk) && submitted == 3);

    ++common[21]; /* Unchanged geometry is still usable after a notification. */
    expect(0, 40, sizeof(buf));
    assert(!ops->read(&disk, 5, 1, buf));
    ++common[21]; config[0] /= 2;
    assert(ops->read(&disk, 5, 1, buf) == -EIO && reset_count == 1 && submitted == 4);

    prepare(1ull << 5, 0, 1024);
    assert(disk.registration.info.flags & BLOCK_DEVICE_READ_ONLY);
    assert(ops->write(&disk, 0, 1, buf) == -EROFS && !disk.queue.available[1]);

    prepare((1ull << 6) | (1ull << 1) | (1ull << 2), 4096, 1024);
    config[2] = 9000; config[3] = 1;
    assert(!virtio_blk_configure(&disk) && disk.registration.info.max_blocks == 2);
    config[3] = 0;
    assert(virtio_blk_configure(&disk) < 0);
    config[3] = 1; config[0] = 1023;
    assert(virtio_blk_configure(&disk) < 0);
    config[0] = 1024; config[1] = UINT32_MAX;
    assert(virtio_blk_configure(&disk) < 0);
    config[1] = 0; config[5] = 1000;
    assert(virtio_blk_configure(&disk) < 0);
    config[5] = 4096; disk.transport.mappings[3].length = 8;
    assert(virtio_blk_configure(&disk) < 0);

    /* Replay never copies failed/short data or keeps the caller's address. */
    prepare(0, 0, 1024); may_yield = true;
    memset(buf, 0xa7, sizeof(buf));
    expect(0, 8, 512);
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN);
    complete(1, 1, false);
    assert(ops->read(&disk, 8, 1, buf) == -EIO && disk.online);
    assert(buf[0] == 0xa7);
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN);
    complete(2, 1, false);
    assert(ops->read(&disk, 8, 1, buf) == -EOPNOTSUPP && disk.online);
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN);
    complete(0, 4, false);
    assert(ops->read(&disk, 8, 1, buf) == -EIO && !disk.online);
    assert(buf[0] == 0xa7 && reset_count == 1);

    prepare(0, 0, 1024); may_yield = true;
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN);
    ops->cancel(&disk, pid);
    complete(0, 513, false);
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN);
    complete(0, 513, false);
    assert(!ops->read(&disk, 8, 1, buf) && submitted == 2 && buf[0] == 0x45);

    prepare(0, 0, 1024); may_yield = true;
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN);
    complete(0, 513, true);
    reset_result = -ETIMEDOUT;
    assert(ops->read(&disk, 8, 1, buf) == -EIO && reset_count == 1);
    assert(!disk.online && disk.transport.live && disk.queue.pending && disk.dma.cpu == backing);
    assert(ops->read(&disk, 8, 1, buf) == -EIO && reset_count == 1);
    reset_result = 0;
    assert(!virtio_blk_stop(&disk) && !disk.transport.live);

    /* A PIT/TSC fallback can return a lower sample while the request is live.
     * A lower work sample isn't five seconds of elapsed time. */
    prepare(0, 0, 1024);
    memset(buf, 0xa7, sizeof(buf));
    work_clock_rolls_back = true;
    clock_complete_after = 3;
    expect(1, 8, 512);
    assert(!ops->write(&disk, 8, 1, buf) && submitted == 1 && !reset_count);

    /* A pending read outlives the syscall's original CPU and time fraction.
     * Only monotonic delivered ticks may expire the asynchronous deadline. */
    prepare(0, 0, 1024); may_yield = true;
    now = 2000; ticks = 90;
    expect(0, 8, 512);
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN);
    now = 1500; ticks += 5 * RELIEFNT_TICK_HZ - 1;
    assert(ops->read(&disk, 8, 1, buf) == -EAGAIN && !reset_count);
    ++ticks;
    assert(ops->read(&disk, 8, 1, buf) == -ETIMEDOUT && reset_count == 1);

    prepare(1ull << 9, 0, 1024); /* Lost completion cannot replay a write. */
    memset(buf, 0xa7, sizeof(buf));
    expect(1, 8, 512);
    assert(ops->write(&disk, 8, 1, buf) == -ETIMEDOUT);
    assert(disk.queue.available[1] == 1 && reset_count == 1 && !disk.online);
    assert(ops->flush(&disk) == -EIO);
    return 0;
}
