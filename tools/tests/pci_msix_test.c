/* SPDX-License-Identifier: Apache-2.0 */
/* Real IRQ allocation/dispatch against a PCI config and UC table boundary. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/pci.h>
#include <reliefnt/pci_irq.h>
#include <reliefnt/lock.h>

extern int pci_request_msix_irq(const struct reliefos_driver_pci_device *,
    void (*)(void *), void *, uint32_t *) __attribute__((weak));
static uint32_t config[64], table[1024];
static unsigned maps, fail_mapping, table_accesses, calls;
static bool apic_on = true;
uint16_t apic_bsp_id(void) { return 3; }
bool apic_enabled(void) { return apic_on; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state=1; *flags=0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state=0; }
void kernel_execution_lock_irqsave(uint64_t *flags) { *flags=0; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { (void)flags; }
uint32_t pci_config_read32(uint8_t b,uint8_t s,uint8_t f,uint8_t o)
{ (void)b;(void)s;(void)f; return config[o/4]; }
uint16_t pci_config_read16(uint8_t b,uint8_t s,uint8_t f,uint8_t o)
{ return (uint16_t)(pci_config_read32(b,s,f,o)>>((o&2)*8)); }
void pci_config_write32(uint8_t b,uint8_t s,uint8_t f,uint8_t o,uint32_t v)
{ (void)b;(void)s;(void)f; config[o/4]=v; }
void pci_config_write16(uint8_t b,uint8_t s,uint8_t f,uint8_t o,uint16_t v)
{ unsigned shift=(o&2)*8; uint32_t old=pci_config_read32(b,s,f,o); config[o/4]=(old&~(0xffffu<<shift))|((uint32_t)v<<shift); }
bool pci_device_bar_contains(const struct pci_device *dev,uint8_t bar,uint64_t pa,uint64_t bytes)
{ return dev->vendor_id==0x15ad && dev->device_id==0x07b0 && bar==2 &&
    pa>=0xd0000000 && pa<0xd0002000 && bytes && bytes<=0xd0002000-pa; }
void *pci_map_device_bar(const struct pci_device *dev,uint8_t bar,uint64_t pa,uint64_t bytes)
{
    ++table_accesses;
    assert(pci_device_bar_contains(dev,bar,pa,bytes));
    if (fail_mapping) return NULL;
    assert(pa==0xd0000000 && bytes==4096); ++maps; return table;
}
void pci_unmap_mmio(void *p,uint64_t bytes) { assert(p==table && bytes==4096 && maps); --maps; }
static void handler(void *context) { ++*(unsigned *)context; }
static void reset(void)
{
    assert(!maps); memset(config,0,sizeof(config)); memset(table,0,sizeof(table));
    config[0]=0x07b015ad; config[1]=1u<<20; config[0x34/4]=0x40;
    config[0x40/4]=0x00030011; /* Four entries, table BAR2+0x80, PBA BAR2+0x1000. */
    config[0x44/4]=0x82; config[0x48/4]=0x1002; config[0x18/4]=0xd0000000;
    fail_mapping=table_accesses=0;
}
int main(void)
{
    if (!pci_request_msix_irq) { fputs("FAIL: MSI-X IRQ allocation is unavailable\n",stderr); return 1; }
    struct reliefos_driver_pci_device dev={.vendor_id=0x15ad,.device_id=0x07b0};
    uint32_t h=0;
    reset(); apic_on=false;
    assert(pci_request_msix_irq(&dev,handler,&calls,&h)==-95 && !maps); apic_on=true;
    assert(pci_request_msix_irq(NULL,handler,&calls,&h)==-22);
    config[0x40/4]=0x4011; /* Cycle with no valid MSI-X capability length beyond end. */
    config[0x34/4]=0xfc; config[0xfc/4]=0x11;
    assert(pci_request_msix_irq(&dev,handler,&calls,&h)<0 && !table_accesses);
    reset(); config[0x44/4]=0x86; /* Reserved BIR. */
    assert(pci_request_msix_irq(&dev,handler,&calls,&h)<0 && !table_accesses);
    reset(); config[0x48/4]=0x82; /* Table and PBA overlap. */
    assert(pci_request_msix_irq(&dev,handler,&calls,&h)<0 && !table_accesses);
    reset(); config[0x40/4]=0x07ff0011; /* Table exceeds its BAR. */
    assert(pci_request_msix_irq(&dev,handler,&calls,&h)<0 && !table_accesses);
    reset(); fail_mapping=1;
    assert(pci_request_msix_irq(&dev,handler,&calls,&h)<0 && !maps && !(config[0x40/4]&0x80000000));
    reset();
    assert(!pci_request_msix_irq(&dev,handler,&calls,&h) && maps==1);
    assert((config[0x40/4]&0xc0000000)==0x80000000);
    assert(table[0x80/4]==0xfee03000 && table[0x84/4]==0);
    unsigned vector=table[0x88/4]; assert(vector>=0x50 && vector<=0x5f);
    assert(!(table[0x8c/4]&1));
    for (unsigned i=1;i<4;++i) assert(table[(0x8c+16*i)/4]&1);
    assert(pci_request_msix_irq(&dev,handler,&calls,&h)==-16 && maps==1);
    pci_irq_dispatch(vector); assert(calls==1);
    uint32_t old=h; pci_free_irq(h); assert(!maps);
    assert((config[0x40/4]&0xc0000000)==0x40000000 && (table[0x8c/4]&1));
    assert(!pci_request_msix_irq(&dev,handler,&calls,&h));
    assert(table[0x88/4]!=vector);
    pci_irq_dispatch(vector); assert(calls==1);
    pci_free_irq(old); assert(maps==1 && (config[0x40/4]&0x80000000));
    pci_irq_dispatch(table[0x88/4]); assert(calls==2);
    pci_free_irq(h); assert(!maps);
    puts("PASS MSI-X: bounds, overlap, rollback, masks, routing, dispatch and retired vectors");
    return 0;
}
