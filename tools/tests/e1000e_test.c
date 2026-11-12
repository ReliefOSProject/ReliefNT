/* SPDX-License-Identifier: Apache-2.0 */
/* Hardware boundary fixture: the driver is compiled as separate objects. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefos/driver.h>
#include <reliefnt/net_device.h>
#include <reliefnt/lock.h>

extern const struct reliefos_driver_module e1000e_driver_module __attribute__((weak));

struct descriptor {
    uint64_t address;
    uint16_t length;
    uint8_t byte10, byte11, status, byte13;
    uint16_t special;
};
static uint32_t registers[2][0x8000 / 4];
static unsigned active_registers;
#define REG(offset) registers[active_registers][(offset) / 4]
static uint16_t device_id = 0x10d3, command, command_second, phy[32];
static int multi_mode;
static const struct reliefnt_netdev_ops *networks[2];
static void *contexts[2];
static unsigned network_count;
static uint64_t bar = 0xc000000000ULL, physical_next = 0x140000000ULL;
static struct allocation { uint64_t address; unsigned pages; void *bytes; } memory[8];
static unsigned live, allocation_limit = 8, maps, unmaps, nvm_reads, mdio_accesses;
static int mapping_failure, reset_stuck, nvm_stuck, phy_stuck, registration_error, stop_error;
static const struct reliefnt_netdev_ops *network;
static void *network_context;
void *kernel_malloc(size_t size) { return calloc(1, size); }
void kernel_free(void *p) { free(p); }

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ *flags = 0; assert(!lock->state); lock->state = 1; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }

static uint64_t allocate(unsigned pages, uint64_t mask)
{
    if (live >= allocation_limit) return 0;
    for (unsigned i = 0; i < 8; ++i) if (!memory[i].address) {
        void *bytes = calloc(pages, 4096);
        assert(bytes);
        uint64_t address = physical_next;
        physical_next += (uint64_t)pages * 4096;
        assert(address + (uint64_t)pages * 4096 - 1 <= mask);
        memory[i] = (struct allocation){address, pages, bytes};
        ++live;
        return address;
    }
    abort();
}
static void release(uint64_t address, unsigned pages)
{
    assert(multi_mode || !(command & 4));
    for (unsigned i = 0; i < 8; ++i) if (memory[i].address == address) {
        assert(memory[i].pages == pages);
        free(memory[i].bytes);
        memory[i] = (struct allocation){0};
        --live;
        return;
    }
    abort();
}
static void *alias(uint64_t address)
{
    for (unsigned i = 0; i < 8; ++i)
        if (memory[i].address && address >= memory[i].address &&
            address - memory[i].address < (uint64_t)memory[i].pages * 4096)
            return (char *)memory[i].bytes + (address - memory[i].address);
    return NULL;
}
static int find(uint16_t vendor, uint16_t device, struct reliefos_driver_pci_device *out)
{
    if (vendor != 0x8086 || device != device_id) return -19;
    *out = (struct reliefos_driver_pci_device){.vendor_id=vendor, .device_id=device,
        .bus=2, .slot=3, .function=0, .class_code=2};
    return 0;
}
static int enumerate(uint32_t index, struct reliefos_driver_pci_device *out)
{
    if (index > (unsigned)multi_mode) return -19;
    int rc=find(0x8086, device_id, out);
    if (!rc) out->slot=(uint8_t)(3+index);
    return rc;
}
static uint16_t read16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{ assert(b==2 && (s==3 || (multi_mode && s==4)) && !f && offset==4); return s==3 ? command : command_second; }
static void write16(uint8_t b, uint8_t s, uint8_t f, uint8_t offset, uint16_t value)
{ assert(b==2 && (s==3 || (multi_mode && s==4)) && !f && offset==4); if(s==3) command=value; else command_second=value; }
static uint32_t read32(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{
    assert(b==2 && (s==3 || (multi_mode && s==4)) && !f);
    uint64_t address=bar+(s==4 ? 0x100000u : 0);
    return offset==0x10 ? (uint32_t)address | 4 : offset==0x14 ? (uint32_t)(address >> 32) : 0;
}
static void *map(uint64_t address, uint64_t bytes)
{
    assert((address==bar || (multi_mode && address==bar+0x100000u)) && bytes>=0x5b04 && bytes<=sizeof(registers[0]));
    assert(!((address==bar ? command : command_second) & 7));
    if (mapping_failure) return NULL;
    ++maps;
    return registers[address==bar ? 0 : 1];
}
static void unmap(void *pointer, uint64_t bytes)
{
    assert((pointer==registers[0] || (multi_mode && pointer==registers[1])) && bytes>=0x5b04 && bytes<=sizeof(registers[0]));
    assert(!((pointer==registers[0] ? command : command_second) & 4));
    assert(multi_mode || (!live && maps==unmaps+1));
    ++unmaps;
}
static void sleep_ms(uint64_t milliseconds)
{
    (void)milliseconds;
    for (active_registers=0; active_registers<(unsigned)(multi_mode+1); ++active_registers) {
    if ((REG(0) & (1u<<26)) && !reset_stuck) {
        REG(0) &= ~(1u<<26);
        REG(0x10) |= 1u<<9; /* EEPROM autoload complete. */
        REG(0x2800)=REG(0x2804)=REG(0x3800)=REG(0x3804)=0;
    }
    if ((REG(0x14) & 1) && !nvm_stuck) {
        unsigned word=(REG(0x14)>>2)&0x3fff;
        const uint16_t mac[3]={0x5452,0x1200,0x5634};
        assert(word<3);
        REG(0x14)=2 | (word<<2) | ((uint32_t)mac[word]<<16);
        ++nvm_reads;
    }
    uint32_t mdic=REG(0x20);
    if ((mdic & (3u<<26)) && !(mdic & (1u<<28)) && !phy_stuck) {
        unsigned reg=(mdic>>16)&31;
        assert(((mdic>>21)&31)==1);
        if (mdic & (1u<<27)) mdic=(mdic&~0xffffu)|phy[reg];
        else phy[reg]=(uint16_t)mdic;
        REG(0x20)=mdic|(1u<<28);
        ++mdio_accesses;
    }
    }
    active_registers=0;
}
int driver_register_netdev(const struct reliefnt_netdev_ops *ops, void *context, uint32_t *id)
{
    if (registration_error) return registration_error;
    network=ops; network_context=context; *id=2;
    if (multi_mode) { assert(network_count<2); networks[network_count]=ops; contexts[network_count++]=context; }
    return 0;
}
static void report_failure(int error) { stop_error=error; }
static void log_text(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
static const struct reliefos_driver_kernel_api api = {
    .abi_version=RELIEFOS_DRIVER_ABI_VERSION, .struct_size=sizeof(api),
    .pci_find=find, .pci_enumerate=enumerate, .pci_read16=read16, .pci_write16=write16, .pci_read32=read32,
    .map_mmio=map, .unmap_mmio=unmap, .phys_to_virt=alias,
    .alloc_dma=allocate, .free_dma=release, .sleep_ms=sleep_ms,
    .console_log=log_text,
    .report_teardown_failure=report_failure,
};
static void reset_fixture(void)
{
    assert(!live && maps==unmaps);
    multi_mode=0; network_count=0; active_registers=0;
    memset(registers,0,sizeof(registers));
    memset(phy,0,sizeof(phy));
    REG(0x8)=2;
    REG(0x10)=1u<<9;
    REG(0x5400)=0x12005452;
    REG(0x5404)=(1u<<31)|0x5634;
    phy[0]=(1u<<11); /* Wake a PHY left in power-down by firmware. */
    phy[2]=0x0141; phy[3]=0x0cb1;
    phy[4]=0x01e1; phy[9]=0x0300;
    command=7;
    device_id=0x10d3;
    network=NULL;
    allocation_limit=8;
    nvm_reads=mdio_accesses=0;
    mapping_failure=reset_stuck=nvm_stuck=phy_stuck=registration_error=stop_error=0;
}
static void finish(void)
{
    e1000e_driver_module.fini();
    assert(!stop_error && !live && maps==unmaps && command==7);
    network=NULL;
}
static struct descriptor *ring(unsigned base)
{
    uint64_t address=REG(base)|((uint64_t)REG(base+4)<<32);
    assert(address>0xffffffffULL);
    struct descriptor *result=alias(address);
    assert(result && (void *)result!=(void *)(uintptr_t)address);
    return result;
}
int main(void)
{
    /* This is the pre-implementation red case, not a replacement driver. */
    if (!&e1000e_driver_module) {
        fputs("FAIL e1000e: native PCIe network driver is missing\n",stderr);
        return 1;
    }
    assert(e1000e_driver_module.init && e1000e_driver_module.fini);
    assert(!strcmp(e1000e_driver_module.name,"e1000e"));
    assert(e1000e_driver_module.init(NULL)==-22);
    for (unsigned failure=0; failure<4; ++failure) {
        reset_fixture(); allocation_limit=failure;
        assert(e1000e_driver_module.init(&api)==-12);
        finish();
    }
    reset_fixture(); mapping_failure=1;
    assert(e1000e_driver_module.init(&api)<0); finish();
    reset_fixture(); device_id=0x15b8; /* I219 must not be falsely bound. */
    assert(e1000e_driver_module.init(&api)==-19); finish();
    reset_fixture(); phy_stuck=1;
    assert(e1000e_driver_module.init(&api)==-110); finish();
    reset_fixture(); REG(0x5404)=0; nvm_stuck=1;
    assert(e1000e_driver_module.init(&api)==-110); finish();
    reset_fixture(); registration_error=-16;
    assert(e1000e_driver_module.init(&api)==-16); finish();
    reset_fixture(); REG(0x8)=0; /* No cable is not a probe failure. */
    assert(!e1000e_driver_module.init(&api) && network && !network->is_ready(network_context)); finish();

    reset_fixture(); REG(0x5404)=0;
    assert(!e1000e_driver_module.init(&api) && network && network->is_ready(network_context));
    assert(nvm_reads==3 && mdio_accesses);
    assert(!memcmp(network->mac(network_context),"\x52\x54\x00\x12\x34\x56",6));
    assert(!(phy[0]&(1u<<11)) && (phy[0]&(1u<<12)));
    assert((REG(0x5b00)&(1u<<22)) && (REG(0x18)&(1u<<28)));
    struct e1000_info info={0}; network->get_info(network_context, &info);
    assert(info.present && info.active && info.device_id==0x10d3 && info.bus==2);
    struct descriptor *rx=ring(0x2800), *tx=ring(0x3800);
    unsigned rx_count=REG(0x2808)/16, tx_count=REG(0x3808)/16;
    assert(rx_count>=64 && tx_count>=16);
    unsigned char bytes[2048], output[2048];
    for(unsigned i=0;i<sizeof(bytes);++i) bytes[i]=(unsigned char)(i*37+11);
    assert(network->send(network_context, NULL,64)==-22);
    assert(network->send(network_context, bytes,0)==-22);
    assert(network->send(network_context, bytes,2049)==-90);
    for(unsigned cycle=0;cycle<3;++cycle) {
        unsigned start=REG(0x3818);
        for(unsigned n=0;n<tx_count-1;++n) assert(!network->send(network_context, bytes,1500));
        assert(network->send(network_context, bytes,1500)==-11); /* Full ring must never spin. */
        for(unsigned n=0;n<tx_count-1;++n) {
            struct descriptor *d=&tx[(start+n)%tx_count];
            assert(d->length==1500 && (d->byte11&0x0b)==0x0b && !(d->status&1));
            assert(!memcmp(alias(d->address),bytes,1500));
            d->status=1; /* Device completed DMA; descriptors become reusable. */
        }
    }
    for(unsigned n=0;n<rx_count*2+3;++n) {
        unsigned i=n%rx_count;
        memcpy(alias(rx[i].address),bytes,1500);
        rx[i].length=1500; rx[i].status=3;
        uint32_t length=99;
        assert(network->poll(network_context, output,sizeof(output),&length)==1 && length==1500);
        assert(!memcmp(output,bytes,1500) && !rx[i].status && REG(0x2818)==i);
    }
    unsigned i=(rx_count*2+3)%rx_count;
    rx[i].length=65535; rx[i].status=3;
    uint32_t length=99;
    assert(network->poll(network_context, output,sizeof(output),&length)==-90 && !length);
    i=(i+1)%rx_count; rx[i].length=64; rx[i].status=1; /* Fragment without EOP. */
    assert(network->poll(network_context, output,sizeof(output),&length)==-74 && !length);
    i=(i+1)%rx_count; rx[i].length=64; rx[i].status=3;
    /* The final fragment must not be returned as an independent frame. */
    assert(network->poll(network_context, output,sizeof(output),&length)==-74 && !length);
    i=(i+1)%rx_count; rx[i].length=64; rx[i].status=3; rx[i].byte13=1;
    assert(network->poll(network_context, output,sizeof(output),&length)==-74 && !length);
    REG(0x8)=0; assert(!network->is_ready(network_context));
    assert(network->send(network_context, bytes,64)==-100);
    reset_stuck=1;
    e1000e_driver_module.fini();
    assert(stop_error==-110 && live && maps==unmaps+1 && !(command&4));
    reset_stuck=stop_error=0;
    finish();
    reset_fixture(); device_id=0x150c;
    assert(!e1000e_driver_module.init(&api) && network);
    network->get_info(network_context, &info); assert(info.device_id==0x150c); finish();
    /* Identical models on distinct BDFs own distinct rings and callbacks. */
    reset_fixture(); multi_mode=1; command_second=7;
    active_registers=1;
    REG(0x8)=2; REG(0x10)=1u<<9;
    REG(0x5400)=0x44332202; REG(0x5404)=(1u<<31)|0x6655;
    active_registers=0;
    assert(!e1000e_driver_module.init(&api) && network_count==2 && live==8);
    assert(contexts[0]!=contexts[1]);
    struct e1000_info first_info, second_info;
    networks[0]->get_info(contexts[0],&first_info);
    networks[1]->get_info(contexts[1],&second_info);
    assert(first_info.slot==3 && second_info.slot==4 && memcmp(first_info.mac,second_info.mac,6));
    active_registers=0; struct descriptor *first_rx=ring(0x2800);
    active_registers=1; struct descriptor *second_rx=ring(0x2800);
    assert(first_rx!=second_rx);
    *(uint8_t *)alias(first_rx[0].address)=0xa1; first_rx[0].length=64; first_rx[0].status=3;
    *(uint8_t *)alias(second_rx[0].address)=0xb2; second_rx[0].length=64; second_rx[0].status=3;
    assert(networks[1]->poll(contexts[1],output,sizeof(output),&length)==1 && output[0]==0xb2);
    assert(networks[0]->poll(contexts[0],output,sizeof(output),&length)==1 && output[0]==0xa1);
    active_registers=0; finish(); assert(command_second==7);
    puts("PASS e1000e: PCI IDs, high BAR/DMA, MAC/NVM, PHY, bounded TX, RX wrap/errors, rollback and retained failed STOP");
    return 0;
}
