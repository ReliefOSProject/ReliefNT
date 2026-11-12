/* Verify that independent NIC contexts retain distinct identities and owners. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/device/manager_internal.h"
#include <reliefnt/driver_manager_phase.h>
#include <reliefnt/proc_stats.h>

int32_t loading_slot = -1;
struct driver_slot driver_slots[RELIEFOS_DRIVER_MAX];
struct kernel_spinlock manager_service_lock = KERNEL_SPINLOCK_INIT;
static unsigned attached[64], detached[64], attaches, detaches;
void net_device_attached(uint32_t id) { attached[attaches++] = id; }
void net_device_detached(uint32_t id) { detached[detaches++] = id; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
void driver_memcpy(void *out, const void *in, uint64_t size) { memcpy(out, in, size); }
void driver_memzero(void *out, uint64_t size) { memset(out, 0, size); }
void *kernel_malloc(size_t size) { return calloc(1, size); }
void kernel_free(void *p) { free(p); }

struct nic { uint8_t mac[6]; unsigned tx; uint8_t rx; unsigned link_down; };
static int ready(void *arg) { return arg && !((struct nic *)arg)->link_down; }
static const uint8_t *mac(void *arg) { return ((struct nic *)arg)->mac; }
static int send(void *arg, const void *frame, uint32_t length)
{ assert(frame && length == 1); ++((struct nic *)arg)->tx; return 0; }
static int poll(void *arg, void *frame, uint32_t capacity, uint32_t *length)
{ assert(capacity); *(uint8_t *)frame = ((struct nic *)arg)->rx; *length = 1; return 1; }
static void info(void *arg, struct e1000_info *out)
{ *out = (struct e1000_info){.present = 1, .active = 1}; memcpy(out->mac, mac(arg), 6); }
static const struct reliefnt_netdev_ops ops = {1, sizeof(ops), ready, mac, send, poll, info};

int main(void)
{
    struct nic a = {{2,0,0,0,0,1},0,0xa1,0}, b = {{2,0,0,0,0,2},0,0xb2,0};
    struct nic c = {{2,0,0,0,0,3},0,0xc3,0};
    uint32_t ia, ib, ic;
    loading_slot = 0;
    assert(!driver_register_netdev(&ops, &a, &ia));
    assert(!driver_register_netdev(&ops, &b, &ib));
    assert(ia == 2 && ib == 3 && netdev_next(0) == 0 && !attaches);
    driver_manager_service_enable(0); driver_netdev_publish_owner(0);
    loading_slot = 1;
    assert(!driver_register_netdev(&ops, &c, &ic) && ic == 4);
    driver_manager_service_enable(1); driver_netdev_publish_owner(1);
    loading_slot = -1;
    assert(attaches == 3 && attached[0] == ia && attached[1] == ib && attached[2] == ic);
    assert(netdev_next(0) == ia && netdev_next(ia) == ib && netdev_next(ib) == ic);
    assert(netdev_is_ready(ib) && !memcmp(netdev_mac(ib), b.mac, 6));
    uint8_t byte = 9; uint32_t length = 0;
    assert(!netdev_send(ib, &byte, 1) && b.tx == 1 && !a.tx && !c.tx);
    assert(netdev_poll(ic, &byte, 1, &length) == 1 && length == 1 && byte == 0xc3);
    struct net_proc_counters counts;
    netdev_counters(ib, &counts); assert(counts.tx_packets == 1 && !counts.rx_packets);
    netdev_counters(ic, &counts); assert(!counts.tx_packets && counts.rx_packets == 1);
    /* Carrier negotiation has no received packet to classify as an RX error. */
    c.link_down = 1;
    assert(netdev_poll(ic, &byte, 1, &length) == -19 && length == 0);
    netdev_counters(ic, &counts);
    assert(counts.rx_packets == 1 && !counts.rx_errors && !counts.rx_drops);
    c.link_down = 0;
    driver_manager_service_close(0); driver_manager_service_drain(0);
    driver_netdev_remove_owner(0);
    assert(detaches == 2 && detached[0] == ia && detached[1] == ib);
    assert(netdev_next(0) == ic && !netdev_is_ready(ia));
    struct e1000_info absent = {.present = 1}; netdev_get_info(ia, &absent);
    assert(!absent.present && netdev_send(ia, &byte, 1) < 0);
    assert(!netdev_send(ic, &byte, 1) && c.tx == 1);
    loading_slot = 0; assert(!driver_register_netdev(&ops, &a, &ia) && ia == 5);
    driver_manager_service_enable(0); driver_netdev_publish_owner(0);
    assert(netdev_next(ic) == ia);
    /* The core must not impose a fixed 16-NIC registry limit. */
    for (unsigned n=0; n<20; ++n) {
        uint32_t more;
        assert(!driver_register_netdev(&ops, &a, &more) && more == 6+n);
    }
    driver_netdev_publish_owner(0);
    assert(netdev_next(24) == 25 && netdev_next(25) == 0);
    driver_manager_service_close(0); driver_manager_service_drain(0);
    driver_netdev_remove_owner(0);
    driver_manager_service_close(1); driver_manager_service_drain(1);
    driver_netdev_remove_owner(1);
    assert(!netdev_next(0));
    puts("PASS netdev owner isolation, context dispatch, publication and stable ifindex");
    return 0;
}
