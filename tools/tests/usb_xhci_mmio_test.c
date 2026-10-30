/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/usb/xhci/internal.h"

int main(void)
{
    uint32_t regs[4] __attribute__((aligned(8)))={0,0xaabbccdd,0,0};
    assert(!xhci_mmio_address((volatile uint8_t *)regs,0,0x12345000,false));
    assert(regs[0]==0x12345000 && regs[1]==0xaabbccdd);
    assert(xhci_mmio_address((volatile uint8_t *)regs,0,0x100000001ull,false)==-EINVAL);
    assert(regs[0]==0x12345000 && regs[1]==0xaabbccdd);
    assert(!xhci_mmio_address((volatile uint8_t *)regs,0,0x1122334455667788ull,true));
    assert(regs[0]==0x55667788 && regs[1]==0x11223344);
    assert(xhci_mmio_address((volatile uint8_t *)regs,4,0,true)==-EINVAL);
    uint32_t bytes=0;struct xhci_geometry geometry;
    assert(!xhci_geometry_init(0x40,0x08001040,0x2003,0x101f,0xc0000000,&geometry));
    assert(geometry.doorbells==0x2000 && geometry.runtime==0x1000);
    assert(!xhci_mmio_layout(&geometry,0x60,&bytes) && bytes==0x3000);
    assert(xhci_geometry_init(0x40,0x08001040,0x1000,0x1000,0xc0000000,&geometry)==-EINVAL);
    assert(xhci_geometry_init(0x40,0x08001040,0x2000,0x400,0xc0000000,&geometry)==-EINVAL);
    assert(xhci_geometry_init(0x40,0x08001040,0xfffffffcu,0x1000,0xc0000000,&geometry)==-EOPNOTSUPP);
    /* All 128 interrupter sets, not just the primary one, must be inside
     * the actual BAR checked after HALT. */
    assert(!xhci_geometry_init(0x40,0x08008040,0x600,0x1000,0xc0000000,&geometry));
    assert(geometry.runtime_end==0x2020 && geometry.limit==0x4000);
    assert(!xhci_mmio_layout(&geometry,0x20,&bytes) && bytes==0x3000);
    assert(xhci_mmio_layout(&geometry,0x4001,&bytes)==-EINVAL);
    puts("USB xHCI MMIO 32/64-bit address tests passed");
}
