/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real driver against a VMXNET3 register/DMA boundary model. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefos/driver.h>
#include <reliefnt/net_device.h>
#include <reliefnt/lock.h>

extern const struct reliefos_driver_module vmxnet3_driver_module __attribute__((weak));

struct cell { uint64_t address; uint32_t a, b; };
struct model {
    uint32_t bar[2][1024];
    uint16_t command;
    unsigned version, upt, link, activated, fail_activate, fail_stop, fail_busmaster_disable;
    uint64_t shared;
    struct cell *tx, *txc, *rx, *rxc;
    unsigned tx_size, rx_size, rxc_size;
    const struct reliefnt_netdev_ops *ops;
    void *context;
    void (*irq)(void *);
    void *irq_context;
    unsigned irq_handle, irq_notifications, link_queries, inject_rearm, rx_doorbells, intr_config;
};
static struct model models[2];
static unsigned count=1, live, maps, allocation_limit=32, registered, teardown_error, allocation_owner;
static uint64_t next_pa=0x140000000ULL;
static int msix_result=-95, msi_result=-95;
static unsigned msix_attempts, msi_attempts;
static struct allocation { uint64_t pa; uint32_t pages; void *data; unsigned owner; } allocations[32];

/** @brief Allocate host storage for driver-owned metadata. @param size Bytes. @return Zeroed storage. */
void *kernel_malloc(size_t size) { return calloc(1,size); }
/** @brief Release driver-owned metadata. @param p Previously allocated storage. */
void kernel_free(void *p) { free(p); }
/** @brief Model an uncontended IRQ-safe lock. @param lock Lock state. @param flags Saved flags. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state=1; *flags=0; }
/** @brief Release the modeled lock. @param lock Lock state. @param flags Saved flags. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state=0; }

/** @brief Allocate a physical DMA lease. @param pages Number of pages. @param mask Address limit. @return Bus address or zero. */
static uint64_t allocate(uint32_t pages, uint64_t mask)
{
    if (live>=allocation_limit) return 0;
    for (unsigned i=0;i<32;++i) if (!allocations[i].pa) {
        void *p=calloc(pages,4096); assert(p);
        uint64_t pa=next_pa; next_pa+=(uint64_t)pages*4096;
        assert(next_pa-1<=mask);
        allocations[i]=(struct allocation){pa,pages,p,allocation_owner}; ++live; return pa;
    }
    abort();
}
/** @brief Translate a modeled bus address to its CPU alias. @param pa Bus address. @return Alias or NULL. */
static void *alias(uint64_t pa)
{
    for (unsigned i=0;i<32;++i) if (allocations[i].pa && pa>=allocations[i].pa &&
        pa-allocations[i].pa<(uint64_t)allocations[i].pages*4096)
        return (uint8_t *)allocations[i].data+(pa-allocations[i].pa);
    return NULL;
}
/** @brief Release a DMA lease after device quiescence. @param pa Original bus address. @param pages Original count. */
static void release(uint64_t pa,uint32_t pages)
{
    for (unsigned i=0;i<32;++i) if (allocations[i].pa==pa) {
        assert(!models[allocations[i].owner].activated);
        assert(allocations[i].pages==pages); free(allocations[i].data);
        allocations[i]=(struct allocation){0}; --live; return;
    }
    abort();
}
/** @brief Enumerate synthetic PCI functions. @param i Index. @param p Returned identity. @return Zero or -ENODEV. */
static int enumerate(uint32_t i,struct reliefos_driver_pci_device *p)
{
    if (i>=count) return -19;
    *p=(struct reliefos_driver_pci_device){.vendor_id=0x15ad,.device_id=0x07b0,
        .class_code=2,.bus=2,.slot=(uint8_t)(3+i)}; return 0;
}
/** @brief Read the PCI command register. @param b Bus. @param s Slot. @param f Function. @param off Offset. @return Register value. */
static uint16_t read16(uint8_t b,uint8_t s,uint8_t f,uint8_t off)
{ assert(b==2 && s>=3 && s<3+count && !f && off==4); return models[s-3].command; }
/** @brief Write the PCI command register. @param b Bus. @param s Slot. @param f Function. @param off Offset. @param v Value. */
static void write16(uint8_t b,uint8_t s,uint8_t f,uint8_t off,uint16_t v)
{
    assert(b==2 && s>=3 && s<3+count && !f && off==4);
    struct model *m=&models[s-3];
    if (m->fail_busmaster_disable && (m->command&4)) v|=4;
    m->command=v; allocation_owner=s-3;
}
/** @brief Read PCI BAR assignments. @param b Bus. @param s Slot. @param f Function. @param off Offset. @return BAR value. */
static uint32_t read32(uint8_t b,uint8_t s,uint8_t f,uint8_t off)
{ assert(b==2 && s>=3 && s<3+count && !f); return off==0x10 ? 0xd0000000+(s-3)*0x10000 : off==0x14 ? 0xd0001000+(s-3)*0x10000 : 0; }
/** @brief Map a modeled MMIO BAR. @param pa PCI bus address. @param bytes Mapping size. @return Register page or NULL. */
static void *map(uint64_t pa,uint64_t bytes)
{
    assert(bytes==4096);
    for (unsigned i=0;i<count;++i) for (unsigned j=0;j<2;++j)
        if (pa==0xd0000000+i*0x10000+j*0x1000) { ++maps; return models[i].bar[j]; }
    return NULL;
}
/** @brief Unmap a modeled MMIO BAR. @param p Original mapping. @param bytes Mapping size. */
static void unmap(void *p,uint64_t bytes)
{ (void)p; assert(bytes==4096 && maps); --maps; }
/** @brief Model a bounded sleep. @param ms Milliseconds. */
static void sleep_ms(uint64_t ms) { (void)ms; }
/** @brief Record a failed stop for later retry. @param error Negative errno. */
static void report_failure(int error) { teardown_error=(unsigned)-error; }
/** @brief Ignore log messages from modeled devices. @param level Log level. @param fmt Format. */
static void log_text(enum console_log_level level,const char *fmt,...) { (void)level; (void)fmt; }
/** @brief Capture one NIC registration. @param ops Driver operations. @param context Per-device state. @param index Returned index. @return Zero. */
int driver_register_netdev(const struct reliefnt_netdev_ops *ops,void *context,uint32_t *index)
{ assert(registered<count); models[registered].ops=ops; models[registered].context=context; *index=++registered; return 0; }
/** @brief Model only the PCI IRQ boundary, retaining a real driver's callback. */
int driver_request_pci_msix_irq(const struct reliefos_driver_pci_device *p,
    void (*handler)(void *),void *context,uint32_t *handle)
{
    ++msix_attempts;
    if (msix_result) return msix_result;
    struct model *m=&models[p->slot-3]; assert(!m->activated && !m->irq);
    m->irq=handler; m->irq_context=context; *handle=m->irq_handle=p->slot;
    return 0;
}
/** @brief Model the legacy MSI provider for fallback and explicit MSI preference. */
static int request_msi(const struct reliefos_driver_pci_device *p,
    void (*handler)(void *),void *context,uint32_t *handle)
{
    ++msi_attempts;
    if (msi_result) return msi_result;
    struct model *m=&models[p->slot-3]; assert(!m->activated && !m->irq);
    m->irq=handler; m->irq_context=context; *handle=m->irq_handle=p->slot;
    return 0;
}
/** @brief Synchronize and remove the retained interrupt callback. */
static void free_irq(uint32_t handle)
{
    struct model *m=&models[handle-3];
    assert(m->bar[0][0]==1); m->irq=NULL; m->irq_context=NULL; m->irq_handle=0;
}
/** @brief Record allocation-free notifications, including the init/publication handoff. */
void netdev_rx_notify(uint32_t id)
{ assert(id && id<=count); ++models[id-1].irq_notifications; }
static const struct reliefos_driver_kernel_api api={
    .abi_version=RELIEFOS_DRIVER_ABI_VERSION,.struct_size=sizeof(api),
    .pci_enumerate=enumerate,.pci_read16=read16,.pci_write16=write16,.pci_read32=read32,
    .map_mmio=map,.unmap_mmio=unmap,.alloc_dma=allocate,.free_dma=release,.phys_to_virt=alias,
    .sleep_ms=sleep_ms,.console_log=log_text,.report_teardown_failure=report_failure,
    .request_pci_irq=request_msi,.free_pci_irq=free_irq,
};

/** @brief Find the device associated with an MMIO mapping. @param p Register mapping. @param which Returned BAR index. @return Device model. */
static struct model *lookup(void *p,unsigned *which)
{
    for (unsigned i=0;i<count;++i) for (unsigned j=0;j<2;++j)
        if (p==models[i].bar[j]) { *which=j; return &models[i]; }
    abort();
}
/** @brief Read a 32-bit little-endian shared-memory word. @param p Base bytes. @param off Byte offset. @return Decoded value. */
static uint32_t word(const void *p,unsigned off) { uint32_t v; memcpy(&v,(const uint8_t *)p+off,4); return v; }
/** @brief Read a shared-memory bus pointer. @param p Base bytes. @param off Byte offset. @return Decoded address. */
static uint64_t quad(const void *p,unsigned off) { uint64_t v; memcpy(&v,(const uint8_t *)p+off,8); return v; }
/** @brief Read one modeled VMXNET3 register. @param p BAR mapping. @param off Register offset. @return Register value. */
uint32_t vmx_io_read(void *p,unsigned off)
{
    unsigned bar; struct model *m=lookup(p,&bar);
    if (bar==1 && off==0) return m->version;
    if (bar==1 && off==8) return m->upt;
    if (bar==1 && off==0x28) return 0x12005452;
    if (bar==1 && off==0x30) return 0x5634;
    if (bar==1 && off==0x20) {
        uint32_t cmd=m->bar[1][off/4];
        if (cmd==0xcafe0000) return m->activated ? 0 : 1;
        if (cmd==0xcafe0001 || cmd==0xcafe0002) return m->fail_stop ? 1 : 0;
        if (cmd==0xf00d0002) { ++m->link_queries; return m->link ? 0x03e80001 : 0; }
        if (cmd==0xf00d0008) return m->intr_config;
        return 0;
    }
    return m->bar[bar][off/4];
}
/** @brief Issue a modeled VMXNET3 register write. @param p BAR mapping. @param off Register offset. @param value Word. */
void vmx_io_write(void *p,unsigned off,uint32_t value)
{
    unsigned bar; struct model *m=lookup(p,&bar);
    if (!bar && off==0x800) ++m->rx_doorbells;
    if (bar==1 && off==0x40) { m->bar[bar][off/4]&=~value; return; }
    m->bar[bar][off/4]=value;
    if (!bar && !off && !value && m->inject_rearm) {
        --m->inject_rearm;
        m->rxc[0].address=3u<<14; m->rxc[0].a=64; m->rxc[0].b=0x80000000;
    }
    if (bar==1 && off==0x10) m->shared=(m->shared&0xffffffff00000000ULL)|value;
    if (bar==1 && off==0x18) m->shared=(m->shared&0xffffffffULL)|((uint64_t)value<<32);
    if (bar!=1 || off!=0x20) return;
    if (value==0xcafe0000 && !m->fail_activate) {
        const uint8_t *s=alias(m->shared); assert(s && word(s,0)==0xbabefee1);
        assert(word(s,56)==1500 && s[62]==1 && s[63]==1 && word(s,120)==0x0d);
        assert(!quad(s,24)); /* optional offloads disabled */
        const uint8_t *q=alias(quad(s,40)); assert(q && word(s,52)==512);
        m->tx=alias(quad(q,16)); m->txc=alias(quad(q,32));
        m->rx=alias(quad(q,256+16)); m->rxc=alias(quad(q,256+32));
        m->tx_size=word(q,56); m->rx_size=word(q,256+56); m->rxc_size=word(q,256+64);
        assert(m->tx && m->txc && m->rx && m->rxc && m->tx_size>=32 && m->rx_size>=32);
        m->activated=1;
    }
    if ((value==0xcafe0001 || value==0xcafe0002) && !m->fail_stop) m->activated=0;
}

/** @brief Reset empty fixtures before one probe. */
static void reset(void)
{
    assert(!live && !maps); memset(models,0,sizeof(models)); count=1; registered=0;
    teardown_error=0; allocation_limit=32; msix_result=msi_result=-95;
    msix_attempts=msi_attempts=0;
    for (unsigned i=0;i<2;++i) { models[i].version=models[i].upt=models[i].link=1; models[i].command=7; }
}
/** @brief Stop all controllers and verify resources were returned. */
static void finish(void)
{
    vmxnet3_driver_module.fini(); assert(!teardown_error && !live && !maps);
    for (unsigned i=0;i<count;++i) assert(models[i].command==7);
}
/** @brief Check masking, cached carrier, event handling and the empty/rearm race. */
static void interrupts(void)
{
    reset(); msix_result=0; assert(!vmxnet3_driver_module.init(&api));
    struct model *m=&models[0]; uint8_t out[2048]; uint32_t length;
    assert(m->irq && m->irq_notifications && !word(alias(m->shared),108));
    assert(((const uint8_t *)alias(m->shared))[80]==1);
    assert(m->rx_doorbells==1);
    assert(!m->ops->poll(m->context,out,sizeof(out),&length) && !m->bar[0][0]);
    unsigned queries=m->link_queries;
    for (unsigned i=0;i<100;++i) assert(m->ops->is_ready(m->context));
    assert(m->link_queries==queries); /* No GET_LINK for each packet. */
    unsigned notify=m->irq_notifications;
    m->irq(m->irq_context);
    assert(m->bar[0][0]==1 && m->irq_notifications==notify+1);
    m->inject_rearm=1;
    assert(!m->ops->poll(m->context,out,sizeof(out),&length));
    assert(m->bar[0][0]==1 && m->irq_notifications>notify+1);
    assert(m->ops->poll(m->context,out,sizeof(out),&length)==1 && length==64);
    assert(m->rx_doorbells==1); /* updateRxProd=false avoids per-packet MMIO. */
    ((uint8_t *)alias(m->shared))[1024+256]=1;
    m->rxc[1].address=1u|(3u<<14); m->rxc[1].a=64; m->rxc[1].b=0x80000000;
    assert(m->ops->poll(m->context,out,sizeof(out),&length)==1 && length==64);
    assert(m->rx_doorbells==2); /* Honor an explicit producer-update request. */
    assert(!m->ops->poll(m->context,out,sizeof(out),&length) && !m->bar[0][0]);
    m->link=0; m->bar[1][0x40/4]=4; m->irq(m->irq_context);
    assert(!m->ops->poll(m->context,out,sizeof(out),&length));
    assert(!m->ops->is_ready(m->context) && !m->bar[1][0x40/4]);
    m->link=1; m->bar[1][0x40/4]=4; m->irq(m->irq_context);
    assert(!m->ops->poll(m->context,out,sizeof(out),&length) && m->ops->is_ready(m->context));
    m->bar[1][0x40/4]=1; m->irq(m->irq_context);
    assert(m->ops->poll(m->context,out,sizeof(out),&length)<0);
    assert(!m->ops->is_ready(m->context) && m->bar[0][0]==1);
    finish(); assert(!m->irq);
    puts("PASS vmxnet3 IRQ mask, cached carrier, rearm race, link/queue events and teardown");
}
/** @brief Check negotiated MSI fallback, polling and IRQ lifecycle rollback. */
static void interrupt_fallbacks(void)
{
    reset(); msi_result=0;
    assert(!vmxnet3_driver_module.init(&api));
    assert(msix_attempts==1 && msi_attempts==1 && models[0].irq);
    finish(); assert(!models[0].irq);
    reset(); msix_result=msi_result=0; models[0].intr_config=2u|(1u<<2);
    assert(!vmxnet3_driver_module.init(&api));
    assert(!msix_attempts && msi_attempts==1 && models[0].irq);
    assert(!((uint8_t *)alias(models[0].shared))[80]); /* Active mask mode. */
    finish();
    reset(); msix_result=msi_result=0; models[0].intr_config=1;
    assert(!vmxnet3_driver_module.init(&api));
    assert(!msix_attempts && !msi_attempts && !models[0].irq);
    assert(word(alias(models[0].shared),108)==1); /* No speculative INTx. */
    finish();
    reset(); msix_result=0; models[0].fail_activate=1;
    assert(vmxnet3_driver_module.init(&api)<0); finish(); assert(!models[0].irq);
    reset(); msix_result=0;
    assert(!vmxnet3_driver_module.init(&api));
    models[0].fail_stop=1; vmxnet3_driver_module.fini();
    assert(teardown_error && live && maps && !models[0].irq);
    models[0].fail_stop=0; teardown_error=0; finish();
    puts("PASS vmxnet3 MSI preference/fallback, polling and IRQ failed-stop retention");
}
/** @brief Test probing, DMA ownership, packet rings and malformed completions. @return Zero on success. */
int main(void)
{
    if (!&vmxnet3_driver_module) { fputs("FAIL vmxnet3: native driver is missing\n",stderr); return 1; }
    assert(vmxnet3_driver_module.init(NULL)==-22);
    reset(); models[0].fail_stop=1;
    assert(vmxnet3_driver_module.init(&api)<0);
    assert(maps && !(models[0].command&4));
    models[0].fail_stop=0; teardown_error=0; finish();
    reset(); models[0].version=0; assert(vmxnet3_driver_module.init(&api)<0); finish();
    reset(); models[0].upt=0; assert(vmxnet3_driver_module.init(&api)<0); finish();
    reset(); models[0].fail_activate=1; assert(vmxnet3_driver_module.init(&api)<0); finish();
    for (unsigned i=0;i<4;++i) { reset(); allocation_limit=i; assert(vmxnet3_driver_module.init(&api)<0); finish(); }
    reset(); assert(!vmxnet3_driver_module.init(&api));
    struct model *m=&models[0]; assert(m->ops && m->ops->is_ready(m->context));
    assert(!memcmp(m->ops->mac(m->context),"\x52\x54\x00\x12\x34\x56",6));
    uint8_t packet[1500], out[2048]; memset(packet,0x69,sizeof(packet));
    for (unsigned cycle=0;cycle<3;++cycle) {
        for (unsigned i=0;i<m->tx_size-1;++i) assert(!m->ops->send(m->context,packet,sizeof(packet)));
        assert(m->ops->send(m->context,packet,sizeof(packet))==-11);
        for (unsigned i=0;i<m->tx_size-1;++i) {
            unsigned seq=cycle*(m->tx_size-1)+i, idx=seq%m->tx_size;
            assert((m->tx[idx].a&0x3fff)==sizeof(packet));
            assert(!memcmp(alias(m->tx[idx].address),packet,sizeof(packet)));
            struct cell *c=&m->txc[seq%m->tx_size];
            c->address=idx; c->a=0; c->b=(seq/m->tx_size%2 ? 0u : 0x80000000u);
        }
    }
    for (unsigned seq=0;seq<m->rx_size*3;++seq) {
        unsigned idx=seq%m->rx_size, ci=seq%m->rxc_size;
        assert(((m->rx[idx].a>>31)&1)==(1^(seq/m->rx_size&1)));
        memcpy(alias(m->rx[idx].address),packet,sizeof(packet));
        struct cell *c=&m->rxc[ci]; c->address=idx | (3u<<14);
        c->a=sizeof(packet);
        /* QEMU's rev-1 model emits type 0; other devices use RX type 3. */
        c->b=(seq%2 ? 3u<<24 : 0u)|(seq/m->rxc_size%2 ? 0u : 0x80000000u);
        uint32_t n=0; assert(m->ops->poll(m->context,out,sizeof(out),&n)==1 && n==sizeof(packet));
        assert(!memcmp(out,packet,sizeof(packet)));
    }
    models[0].link=0; assert(!m->ops->is_ready(m->context));
    assert(m->ops->send(m->context,packet,sizeof(packet))==-100);
    m->fail_stop=1; vmxnet3_driver_module.fini();
    assert(teardown_error && live && maps && !(m->command&4));
    m->fail_stop=0; teardown_error=0; finish();
    reset(); assert(!vmxnet3_driver_module.init(&api)); m=&models[0];
    assert(!m->ops->send(m->context,packet,sizeof(packet)));
    m->txc[0].address=9; m->txc[0].b=0x80000000u;
    assert(m->ops->send(m->context,packet,sizeof(packet))==-5);
    assert(!m->ops->is_ready(m->context)); finish();
    reset(); assert(!vmxnet3_driver_module.init(&api)); m=&models[0];
    m->rxc[0].address=0xfff|(3u<<14); m->rxc[0].a=1500;
    m->rxc[0].b=0x83000000u;
    uint32_t bad_length=42;
    assert(m->ops->poll(m->context,out,sizeof(out),&bad_length)==-5 && !bad_length);
    assert(!m->ops->is_ready(m->context)); finish();
    reset(); assert(!vmxnet3_driver_module.init(&api)); m=&models[0];
    m->rxc[0].address=3u<<14; m->rxc[0].a=16383; m->rxc[0].b=0x83000000u;
    assert(m->ops->poll(m->context,out,sizeof(out),&bad_length)==-90 && !bad_length);
    m->rxc[1].address=1u|(1u<<14); m->rxc[1].a=64; m->rxc[1].b=0x83000000u;
    assert(m->ops->poll(m->context,out,sizeof(out),&bad_length)==-74 && !bad_length);
    finish();
    interrupts();
    interrupt_fallbacks();
    reset(); assert(!vmxnet3_driver_module.init(&api)); m=&models[0];
    m->fail_busmaster_disable=1; vmxnet3_driver_module.fini();
    assert(teardown_error && live && maps);
    m->fail_busmaster_disable=0; teardown_error=0; finish();
    reset(); count=2; assert(!vmxnet3_driver_module.init(&api) && registered==2);
    assert(models[0].context!=models[1].context && models[0].tx!=models[1].tx);
    finish();
    puts("PASS vmxnet3: negotiation, DMA, TX/RX wrap, carrier, rollback and failed stop retention");
    return 0;
}
