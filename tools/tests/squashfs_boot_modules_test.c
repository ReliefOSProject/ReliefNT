/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/blkdev.h>
#include <reliefnt/boot_block.h>
#include <reliefnt/multiboot2.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t root_bytes[1024];
static uint8_t payload_bytes[513];
static struct block_device_registration registrations[2];
static unsigned registration_count;

bool paging_kernel_direct_map_range(uint64_t physical,uint64_t length)
{
    if(physical==0x100000u)return length<=sizeof(root_bytes);
    if(physical==0x200000u)return length<=sizeof(payload_bytes);
    return false;
}

void *paging_kernel_direct_map(uint64_t physical)
{
    if(physical==0x100000u)return root_bytes;
    if(physical==0x200000u)return payload_bytes;
    return NULL;
}

int register_block_device(const struct block_device_registration *registration)
{
    assert(registration && registration_count<2);
    registrations[registration_count++]=*registration;
    return 0;
}

int main(void)
{
    memcpy(root_bytes,"root",4);
    memcpy(payload_bytes,"squashfs",8);
    const struct boot_module modules[]={
        {0x100000u,0x100400u,"reliefos-installer-root"},
        {0x200000u,0x200201u,"reliefos-installer-payload"}
    };
    struct boot_info boot={.module_count=2};
    memcpy(boot.modules,modules,sizeof(modules));
    uint64_t root_device=0;
    boot.modules[1].end=boot.modules[1].start+1;
    assert(block_boot_image_register(&boot,&root_device)==-ENOEXEC);
    assert(root_device==0 && registration_count==0);
    boot.modules[1]=(struct boot_module){0x100000u,0x100201u,"reliefos-installer-payload"};
    assert(block_boot_image_register(&boot,&root_device)==-ENOEXEC);
    assert(root_device==0 && registration_count==0);
    boot.modules[1]=modules[1];
    assert(block_boot_image_register(&boot,&root_device)==2);
    assert(root_device==block_device_make_number(1,0));
    assert(registration_count==2);
    assert(registrations[0].info.device==block_device_make_number(1,0));
    assert(registrations[0].info.controller==0);
    assert(registrations[0].info.blocks==2);
    assert(!(registrations[0].info.flags&BLOCK_DEVICE_READ_ONLY));
    assert(registrations[1].info.device==block_device_make_number(1,1));
    assert(registrations[1].info.controller==1);
    assert(registrations[1].info.blocks==2);
    assert(registrations[1].info.flags&BLOCK_DEVICE_READ_ONLY);
    assert(registrations[1].operations->write==NULL);
    uint8_t readback[1024];
    assert(registrations[1].operations->read(registrations[1].context,0,2,readback)==0);
    assert(!memcmp(readback,"squashfs",8));
    for(size_t i=513;i<sizeof(readback);++i)assert(readback[i]==0);
    assert(registrations[1].operations->read(registrations[1].context,2,1,readback)==-EINVAL);
    memset(readback,0xa5,512);
    assert(registrations[0].operations->write(registrations[0].context,1,1,readback)==0);
    assert(!memcmp(root_bytes+512,readback,512));

    boot.module_count=1;
    assert(block_boot_image_register(&boot,&root_device)==1);
    assert(root_device==block_device_make_number(1,0) && registration_count==2);

    boot.module_count=2;
    boot.modules[1]=boot.modules[0];
    assert(block_boot_image_register(&boot,&root_device)==-EEXIST);
    assert(root_device==0 && registration_count==2);

    boot.module_count=1;
    boot.modules[0]=modules[1];
    assert(block_boot_image_register(&boot,&root_device)==1);
    assert(root_device==0 && registration_count==2);

    boot.module_count=0;
    assert(block_boot_image_register(&boot,&root_device)==0);
    assert(root_device==0 && registration_count==2);
    puts("PASS SquashFS boot module registration: ram0 root, read-only ram1 payload, padded tail, duplicate and malformed modules");
    return 0;
}
